#include "embed_toolchain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <io.h>

/*
 * embed_toolchain.c — 调用 Keil C51 工具链流水线
 *
 * 检测已安装的 Keil C51（C51.exe / BL51.EXE / OH51.EXE），
 * 依次调用编译 → 链接 → HEX 转换流水线。
 */

/* ── 获取 embed_toolchain/bin 路径（相对于 ttcc.exe）── */
static const char *detect_keil_bin(void) {
    static char keil_bin_buf[4096];
    GetModuleFileNameA(NULL, keil_bin_buf, sizeof(keil_bin_buf));
    char *p = strrchr(keil_bin_buf, '\\');
    if (p) *p = '\0';
    size_t len = strlen(keil_bin_buf);
    snprintf(keil_bin_buf + len, sizeof(keil_bin_buf) - len, "\\embed_toolchain\\bin");
    return keil_bin_buf;
}

/* ── 提取基础文件名（不含目录和后缀）── */
static void extract_basename(const char *path, char *out, int out_sz) {
    const char *base = strrchr(path, '/');
    if (!base) base = strrchr(path, '\\');
    if (!base) base = path; else base++;
    const char *dot = strrchr(base, '.');
    int n = dot ? (int)(dot - base) : (int)strlen(base);
    if (n > out_sz - 1) n = out_sz - 1;
    strncpy(out, base, n); out[n] = '\0';
}

/* ── 运行工具链命令 ── */
static int run_step(const char *keil_bin, const char *exe, const char *args) {
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "cmd.exe /C \"\"%s\\%s\" %s\"",
             keil_bin, exe, args);
    fprintf(stdout, "note: running %s...\n", exe);
    int ret = system(cmd);
    if (ret && ret != 1)
        fprintf(stdout, "error: %s failed (exit %d)\n", exe, ret);
    return ret;
}

/* ── Windows 控制台颜色输出 ── */
enum { CLR_DEFAULT = 7, CLR_WARNING = 14, CLR_ERROR = 12, CLR_INFO = 7 };
static void set_color(int c) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    SetConsoleTextAttribute(h, (WORD)c);
}

/* ── 打印 C51 警告摘要，带颜色和中文描述 ── */
static void print_c51_warnings(const char *log_file, const char *source_label) {
    FILE *f = fopen(log_file, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "COMPILER") || strstr(line, "COPYRIGHT") ||
            strstr(line, "COMPILATION COMPLETE"))
            continue;
        bool is_err = (strstr(line, "ERROR") != NULL);
        if (!is_err && !strstr(line, "WARNING")) continue;
        char func[128] = "", desc[256] = "", cn[256] = "";
        char *p = strchr(line, '\'');
        if (p) {
            p++;
            int i = 0;
            while (*p && *p != '\'' && i < 127) func[i++] = *p++;
            func[i] = '\0';
            p += 2;
            if (*p) {
                i = 0;
                p[strcspn(p, "\r\n")] = 0;
                while (*p && i < 255) desc[i++] = *p++;
                desc[i] = '\0';
            }
        }
        if (strstr(desc, "recursive call to non-reentrant function"))
            snprintf(cn, sizeof(cn), "不可重入函数递归调用");
        else if (strstr(desc, "unmodifiable lvalue"))
            snprintf(cn, sizeof(cn), "左值不可修改（const）");
        else if (strstr(desc, "redefinition"))
            snprintf(cn, sizeof(cn), "重复定义");
        else if (strstr(desc, "asm/endasm requires src-control"))
            snprintf(cn, sizeof(cn), "内联汇编需要 #pragma SRC");
        else if (strstr(desc, "missing return-expression"))
            snprintf(cn, sizeof(cn), "缺少 return 语句");
        else if (strstr(desc, "undefined identifier"))
            snprintf(cn, sizeof(cn), "未定义的标识符");
        else if (strstr(desc, "UNCALLED SEGMENT"))
            snprintf(cn, sizeof(cn), "函数未被调用");
        else if (strstr(desc, "constant out of range"))
            snprintf(cn, sizeof(cn), "常量超出范围");
        else if (strstr(desc, "unreachable code"))
            snprintf(cn, sizeof(cn), "不可达代码");
        else
            snprintf(cn, sizeof(cn), "%s", desc);

        set_color(is_err ? CLR_ERROR : CLR_WARNING);
        fprintf(stdout, "  %s %s: '%s': %s\n",
                is_err ? "ERROR" : "WARNING", source_label, func, cn);
        set_color(CLR_DEFAULT);
    }
    fclose(f);
}

/* ── 从原文提取 Program Size（白色输出）── */
static const char *extract_program_size(const char *log_file) {
    static char buf[256];
    FILE *f = fopen(log_file, "r");
    if (!f) return NULL;
    while (fgets(buf, sizeof(buf), f))
        if (strstr(buf, "Program Size:")) { fclose(f); return buf; }
    fclose(f);
    return NULL;
}

/* ── 主流程：编译 → 链接 → 转换 HEX ── */
int embed_run_toolchain(const char *source_file, const char *source_label,
                        const char *hex_out,
                        const char *temp_dir, int c51_model) {
    (void)temp_dir;
    if (!source_file || !*source_file) return -1;

    const char *keil_bin = detect_keil_bin();
    if (!keil_bin) { fprintf(stdout, "error: Keil C51 not found\n"); return -1; }

    char base_name[256], obj_file[512], abs_file[512];
    extract_basename(source_file, base_name, sizeof(base_name));
    snprintf(obj_file, sizeof(obj_file), "%s.OBJ", base_name);
    snprintf(abs_file, sizeof(abs_file), "%s.ABS", base_name);

    const char *model_flag = "SMALL";
    if (c51_model == 1) model_flag = "COMPACT";
    else if (c51_model == 2) model_flag = "LARGE";

    char log[512];
    snprintf(log, sizeof(log), "%s\\ttcc_log.txt", temp_dir);

    /* C51: 编译（原始输出定向到日志，解析后只输出 WARNING/ERROR）*/
    {
        char args[8192];
        snprintf(args, sizeof(args), "\"%s\" OBJECT(%s) %s", source_file, obj_file, model_flag);
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "cmd.exe /C \"\"%s\\%s\" %s\" >\"%s\" 2>&1",
                 keil_bin, "C51.exe", args, log);
        int ret = system(cmd);
        if (ret && ret != 1) {
            fprintf(stdout, "  C51 failed (exit %d)\n", ret);
            _unlink(log);
            return -1;
        }
        print_c51_warnings(log, source_label ? source_label : source_file);
        _unlink(log);
    }
    /* BL51: 链接 */
    {
        char args[8192];
        snprintf(args, sizeof(args), "\"%s\" TO \"%s\"", obj_file, abs_file);
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "cmd.exe /C \"\"%s\\%s\" %s\" >\"%s\" 2>&1",
                 keil_bin, "BL51.EXE", args, log);
        int ret = system(cmd);
        if (ret && ret != 1) {
            fprintf(stdout, "  BL51 failed (exit %d)\n", ret);
            _unlink(log);
            return -1;
        }
        const char *size = extract_program_size(log);
        if (size) { set_color(CLR_DEFAULT); fprintf(stdout, "  %s", size); }
        _unlink(log);
    }
    /* OH51: 转换 HEX（静默）*/
    {
        char args[8192];
        snprintf(args, sizeof(args), "\"%s\"", abs_file);
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "cmd.exe /C \"\"%s\\%s\" %s\" >\"%s\" 2>&1",
                 keil_bin, "OH51.EXE", args, log);
        system(cmd);
        _unlink(log);
    }
    /* 复制 HEX 到目标路径 */
    {
        char src_hex[512]; snprintf(src_hex, sizeof(src_hex), "%s.HEX", base_name);
        FILE *fs = fopen(src_hex, "rb");
        if (!fs) { fprintf(stdout, "warning: HEX not found at %s\n", src_hex); return -1; }
        const char *dst = hex_out ? hex_out : src_hex;
        FILE *fd = fopen(dst, "wb");
        if (!fd) { fprintf(stdout, "error: cannot write %s\n", dst); fclose(fs); return -1; }
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fs)) > 0) fwrite(buf, 1, n, fd);
        fclose(fd); fclose(fs);
        printf("HEX file generated: %s\n", dst);
    }
    /* 清理临时文件 */
    {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s.OBJ", base_name); _unlink(tmp);
        snprintf(tmp, sizeof(tmp), "%s.ABS", base_name); _unlink(tmp);
        snprintf(tmp, sizeof(tmp), "%s.HEX", base_name); _unlink(tmp);
        snprintf(tmp, sizeof(tmp), "%s.M51", base_name); _unlink(tmp);
    }
    return 0;
}
