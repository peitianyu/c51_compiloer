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

/* ── 获取 embed_toolchain 根目录（bin/ 的父目录）── */
static const char *detect_keil_root(void) {
    static char root_buf[4096];
    strncpy(root_buf, detect_keil_bin(), sizeof(root_buf) - 1);
    char *p = strrchr(root_buf, '\\');
    if (p) *p = '\0';
    return root_buf;
}

/* ── 获取当前工作目录（用于 OBJ/ABS 生成位置）── */
static const char *get_cwd(void) {
    static char cwd_buf[4096];
    GetCurrentDirectoryA(sizeof(cwd_buf), cwd_buf);
    return cwd_buf;
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

/* ── 运行工具链命令（静默模式，输出定向到日志）── */
static int run_step_silent(const char *keil_bin, const char *exe, const char *args, const char *log) {
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "cmd.exe /C \"\"%s\\%s\" %s\" >\"%s\" 2>&1",
             keil_bin, exe, args, log);
    return system(cmd);
}

/* ── 用 A51.EXE 汇编 .A51 文件 ── */
static int assemble_a51(const char *keil_bin, const char *src_path, const char *obj_file, const char *log) {
    char args[8192];
    snprintf(args, sizeof(args), "\"%s\" OBJECT(\"%s\")", src_path, obj_file);
    return run_step_silent(keil_bin, "A51.EXE", args, log);
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
        else if (strstr(desc, "UNRESOLVED EXTERNAL SYMBOL"))
            snprintf(cn, sizeof(cn), "未解析的外部符号");
        else if (strstr(desc, "REFERENCE MADE TO UNRESOLVED EXTERNAL"))
            snprintf(cn, sizeof(cn), "引用了未解析的外部符号");
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
            snprintf(cn, sizeof(cn), "函数未被调用，已丢弃");
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

/* ── 打印链接器警告/错误（带中文翻译）── */
static void print_linker_warnings(const char *log_file) {
    FILE *f = fopen(log_file, "r");
    if (!f) return;
    char line[1024], next[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "COMPILER") || strstr(line, "COPYRIGHT") ||
            strstr(line, "LINK/LOCATE RUN COMPLETE") ||
            strstr(line, "RESTRICTED VERSION") ||
            strstr(line, "Program Size:"))
            continue;
        bool is_err = (strstr(line, "ERROR") != NULL);
        if (!is_err && !strstr(line, "WARNING")) continue;

        /* 提取描述 */
        const char *desc = "";
        if (strstr(line, "UNRESOLVED EXTERNAL SYMBOL"))
            desc = "未解析的外部符号";
        else if (strstr(line, "REFERENCE MADE TO UNRESOLVED EXTERNAL"))
            desc = "引用了未解析的外部符号";
        else if (strstr(line, "UNCALLED SEGMENT"))
            desc = "段未被调用，已丢弃";
        else
            continue; /* 不认识的警告跳过 */

        /* 尝试从后续缩进行提取符号名 */
        char sym[128] = "";
        long pos = ftell(f);
        if (fgets(next, sizeof(next), f) && next[0] == ' ') {
            char *p = strstr(next, "SYMBOL:");
            int skip = 7;
            if (!p) { p = strstr(next, "SEGMENT:"); skip = 8; }
            if (p) {
                p += skip;
                while (*p == ' ') p++;
                int i = 0;
                while (*p && *p != '\r' && *p != '\n' && *p != ' ' && i < 127) sym[i++] = *p++;
                sym[i] = '\0';
                /* 段名 ?PR?FUNC?MODULE → 只取函数名 */
                if (strncmp(sym, "?PR?", 4) == 0) {
                    memmove(sym, sym + 4, strlen(sym + 4) + 1);
                    char *q = strrchr(sym, '?');
                    if (q) *q = '\0';
                }
            }
        } else {
            fseek(f, pos, SEEK_SET);
        }

        set_color(is_err ? CLR_ERROR : CLR_WARNING);
        if (sym[0])
            fprintf(stdout, "  %s: '%s': %s\n",
                    is_err ? "ERROR" : "WARNING", sym, desc);
        else
            fprintf(stdout, "  %s: %s\n",
                    is_err ? "ERROR" : "WARNING", desc);
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

    /* 用绝对路径指向 OBJ 文件（BL51 需要）*/
    char obj_abs[512], abs_abs[512];
    const char *cwd = get_cwd();
    snprintf(obj_abs, sizeof(obj_abs), "%s\\%s", cwd, obj_file);
    snprintf(abs_abs, sizeof(abs_abs), "%s\\%s", cwd, abs_file);

    const char *model_flag = "SMALL";
    if (c51_model == 1) model_flag = "COMPACT";
    else if (c51_model == 2) model_flag = "LARGE";

    char log[512];
    snprintf(log, sizeof(log), "%s\\ttcc_log.txt", temp_dir);

    /* C51: 编译（OBJECT 必须带 .OBJ 后缀）*/
    {
        char obj_with_ext[512];
        snprintf(obj_with_ext, sizeof(obj_with_ext), "%s.OBJ", base_name);
        char args[8192];
        snprintf(args, sizeof(args), "\"%s\" OBJECT(%s) %s", source_file, obj_with_ext, model_flag);
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "cmd.exe /C \"\"%s\\%s\" %s\" >\"%s\" 2>&1",
                 keil_bin, "C51.exe", args, log);
        int ret = system(cmd);
        if (ret && ret != 1) {
            fprintf(stdout, "FAILED (exit %d)\n", ret);
            _unlink(log);
            return -1;
        }
        /* 统计并输出警告/错误摘要 */
        int nw = 0, ne = 0;
        {
            FILE *f = fopen(log, "r");
            if (f) {
                char line[1024];
                while (fgets(line, sizeof(line), f)) {
                    if (strstr(line, "COMPILATION COMPLETE")) continue;
                    if (strstr(line, "COPYRIGHT")) continue;
                    if (strstr(line, "COMPILER")) continue;
                    if (strstr(line, "WARNING")) nw++;
                    else if (strstr(line, "ERROR")) ne++;
                }
                fclose(f);
            }
        }
        if (ne || nw) {
            set_color(ne ? CLR_ERROR : CLR_WARNING);
            fprintf(stdout, "  %d warning(s), %d error(s)\n", nw, ne);
            set_color(CLR_DEFAULT);
            print_c51_warnings(log, source_label ? source_label : source_file);
        }
        _unlink(log);
    }
    /* A51: 汇编 STARTUP.A51 和 INIT.A51 */
    {
        const char *keil_root = detect_keil_root();
        char startup_a51[512], init_a51[512];
        char startup_obj[512], init_obj[512];
        char a51_log[512];
        snprintf(a51_log, sizeof(a51_log), "%s\\a51_log.txt", temp_dir);
        snprintf(startup_a51, sizeof(startup_a51), "%s\\LIB\\STARTUP.A51", keil_root);
        snprintf(init_a51, sizeof(init_a51), "%s\\LIB\\INIT.A51", keil_root);
        snprintf(startup_obj, sizeof(startup_obj), "%s\\STARTUP.OBJ", cwd);
        snprintf(init_obj, sizeof(init_obj), "%s\\INIT.OBJ", cwd);

        if (assemble_a51(keil_bin, startup_a51, startup_obj, a51_log)) return -1;
        if (assemble_a51(keil_bin, init_a51, init_obj, a51_log)) return -1;
        _unlink(a51_log);
    }
    /* BL51: 链接（含浮点运算库）*/
    {
        const char *model_lib = "C51S.LIB";
        const char *model_fplib = "C51FPS.LIB";
        if (c51_model == 1) { model_lib = "C51C.LIB"; model_fplib = "C51FPC.LIB"; }
        else if (c51_model == 2) { model_lib = "C51L.LIB"; model_fplib = "C51FPL.LIB"; }
        const char *keil_root = detect_keil_root();
        char lib_path[512], fplib_path[512];
        snprintf(lib_path, sizeof(lib_path), "%s\\LIB\\%s", keil_root, model_lib);
        snprintf(fplib_path, sizeof(fplib_path), "%s\\LIB\\%s", keil_root, model_fplib);

        char args[8192];
        snprintf(args, sizeof(args), "\"%s\",\"%s\\STARTUP.OBJ\",\"%s\\INIT.OBJ\",\"%s\",\"%s\" TO \"%s\"",
                 obj_abs, cwd, cwd, lib_path, fplib_path, abs_abs);
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "cmd.exe /C \"\"%s\\%s\" %s\" >\"%s\" 2>&1",
                 keil_bin, "BL51.EXE", args, log);
        int ret = system(cmd);
        if (ret && ret != 1) {
            fprintf(stdout, "error: BL51 failed (exit %d)\n", ret);
            _unlink(log);
            return -1;
        }
        print_linker_warnings(log);
        const char *size = extract_program_size(log);
        if (size) {
            char size_trim[256];
            strncpy(size_trim, size, sizeof(size_trim) - 1);
            size_trim[strcspn(size_trim, "\r\n")] = '\0';
            fprintf(stdout, "  %s\n", size_trim);
        }
        _unlink(log);
    }
    /* OH51: 转换 HEX */
    {
        char args[8192];
        snprintf(args, sizeof(args), "\"%s\"", abs_abs);
        if (run_step_silent(keil_bin, "OH51.EXE", args, log)) return -1;
        _unlink(log);
    }
    /* 复制 HEX 到目标路径 */
    {
        char src_hex[512]; snprintf(src_hex, sizeof(src_hex), "%s\\%s.HEX", cwd, base_name);
        FILE *fs = fopen(src_hex, "rb");
        if (!fs) { fprintf(stdout, "warning: HEX not found at %s\n", src_hex); return -1; }
        const char *dst = hex_out ? hex_out : src_hex;
        FILE *fd = fopen(dst, "wb");
        if (!fd) { fprintf(stdout, "error: cannot write %s\n", dst); fclose(fs); return -1; }
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fs)) > 0) fwrite(buf, 1, n, fd);
        fclose(fd); fclose(fs);
        set_color(CLR_DEFAULT);
        fprintf(stdout, "HEX file generated: %s\n", dst);
    }
    /* 清理临时文件 */
    {
        _unlink(obj_abs);
        _unlink(abs_abs);
        char hex_abs[512]; snprintf(hex_abs, sizeof(hex_abs), "%s\\%s.HEX", cwd, base_name); _unlink(hex_abs);
        char m51_abs[512]; snprintf(m51_abs, sizeof(m51_abs), "%s\\%s.M51", cwd, base_name); _unlink(m51_abs);
        char startup_obj[512]; snprintf(startup_obj, sizeof(startup_obj), "%s\\STARTUP.OBJ", cwd); _unlink(startup_obj);
        char init_obj[512]; snprintf(init_obj, sizeof(init_obj), "%s\\INIT.OBJ", cwd); _unlink(init_obj);
    }
    return 0;
}
