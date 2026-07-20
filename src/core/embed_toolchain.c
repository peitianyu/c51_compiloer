#include "embed_toolchain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <io.h>
#include "list.h"
#include "dict.h"

/* ── 从 C51 源码中解析中文→ASCII 名映射 ── */
/* 映射格式（在 C51 文件末尾的注释中）：
 *   " __TTCC_NAME_MAP__ _cn_1=阶乘 _cn_2=初始化硬件 "
 * 返回已分配的 Dict* (key=ASCII别名, val=中文原名) */
static Dict *parse_name_map(const char *c51_source) {
    Dict *map = make_dict(NULL);
    FILE *f = fopen(c51_source, "r");
    if (!f) return map;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "__TTCC_NAME_MAP__")) {
            char *p = strstr(line, "__TTCC_NAME_MAP__");
            p += 18; /* skip past marker */
            while (*p) {
                while (*p == ' ' || *p == '\t' || *p == '*' || *p == '/') p++;
                if (!*p || *p == '\n' || *p == '\r') break;
                char alias[128], chinese[256];
                if (sscanf(p, "%127[^=]=%255s", alias, chinese) == 2) {
                    dict_put(map, strdup(alias), strdup(chinese));
                    p += strlen(alias) + 1 + strlen(chinese);
                } else break;
            }
            break;
        }
    }
    fclose(f);
    return map;
}

/* 根据别名查找中文原名
 * C51/BL51 会对标识符做大写处理，且可能加/减下划线前缀，
 * 所以需要多种尝试 */
static const char *lookup_chinese(Dict *map, const char *alias) {
    if (!map || !alias) return NULL;
    /* 1) 精确匹配 */
    {
        const char *r = (const char*)dict_get(map, alias);
        if (r) return r;
    }
    /* 2) 大小写不敏感匹配 */
    for (int i = 0; i < list_len(map->list); i++) {
        DictEntry *e = (DictEntry*)list_get(map->list, i);
        if (e->key && _stricmp(e->key, alias) == 0)
            return (const char*)e->val;
    }
    /* 3) 逐级去掉前导 _ 再试（如 __CN_3 → _CN_3 → CN_3）*/
    const char *p = alias;
    while (*p == '_') {
        p++;
        /* 精确匹配 */
        { const char *r = (const char*)dict_get(map, p); if (r) return r; }
        /* 大小写不敏感 */
        for (int i = 0; i < list_len(map->list); i++) {
            DictEntry *e = (DictEntry*)list_get(map->list, i);
            if (e->key && _stricmp(e->key, p) == 0)
                return (const char*)e->val;
        }
    }
    return NULL;
}

static const char *detect_keil_bin(void) {
    {
        static char keil_bin_buf[4096];
        GetModuleFileNameA(NULL, keil_bin_buf, sizeof(keil_bin_buf));
        char *p = strrchr(keil_bin_buf, '\\');
        if (p) *p = '\0';
        size_t len = strlen(keil_bin_buf);
        snprintf(keil_bin_buf + len, sizeof(keil_bin_buf) - len, "\\Keil_v5\\C51\\BIN");
        char c51_check[4096];
        snprintf(c51_check, sizeof(c51_check), "%s\\C51.exe", keil_bin_buf);
        if (_access(c51_check, 0) == 0)
            return keil_bin_buf;
    }
    return NULL;
}

/* ── 获取 embed_toolchain/C51 根目录 ── */
static const char *detect_keil_root(void) {
    const char *bin = detect_keil_bin();
    if (bin) {
        static char root_buf[4096];
        strncpy(root_buf, bin, sizeof(root_buf) - 1);
        root_buf[sizeof(root_buf) - 1] = '\0';
        char *p = strrchr(root_buf, '\\');
        if (p) *p = '\0';
        return root_buf;
    }
    return NULL;
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
    snprintf(cmd, sizeof(cmd), "%s\\%s %s >%s 2>&1",
             keil_bin, exe, args, log);
    return system(cmd);
}

/* ── 用 A51.EXE 汇编 .A51 文件 ── */
static int assemble_a51(const char *keil_bin, const char *src_path, const char *obj_file, const char *log) {
    char args[8192];
    snprintf(args, sizeof(args), "%s OBJECT(%s)", src_path, obj_file);
    return run_step_silent(keil_bin, "A51.EXE", args, log);
}

/* ── Windows 控制台颜色输出 ── */
enum { CLR_DEFAULT = 7, CLR_WARNING = 14, CLR_ERROR = 12, CLR_INFO = 7 };
static void set_color(int c) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    SetConsoleTextAttribute(h, (WORD)c);
}

/* ── 打印 C51 警告摘要，带颜色和中文描述 ── */
static void print_c51_warnings(const char *log_file, const char *source_label, Dict *name_map) {
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
            /* 格式: *** WARNING file.c(123): 'function_name': description */
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
        } else {
            /* 无单引号格式: *** WARNING file.c(123): description */
            /* 跳过 "WARNING" 前缀，提取冒号后的内容 */
            p = strstr(line, "WARNING");
            if (!p) p = strstr(line, "ERROR");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    p++;
                    while (*p == ' ') p++;
                    p[strcspn(p, "\r\n")] = 0;
                    strncpy(desc, p, sizeof(desc) - 1);
                    desc[sizeof(desc) - 1] = '\0';
                }
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
        else if (strstr(desc, "unreachable code")) {
            snprintf(cn, sizeof(cn), "不可达代码（常量条件分支）");
        }
        else {
            /* 提取原始行中的文件名和行号: "IN LINE 346 OF build\file.51: desc" */
            char file_buf[256] = "", line_str[32] = "";
            char *in_line = strstr(line, "IN LINE ");
            if (in_line) {
                in_line += 8; /* 跳过 "IN LINE " */
                char *p = in_line;
                while (*p && isdigit((unsigned char)*p) && (p - in_line) < 31)
                    p++;
                if (p > in_line) {
                    int ln = (int)(p - in_line);
                    strncpy(line_str, in_line, ln);
                    line_str[ln] = '\0';
                }
                char *of = strstr(p, " OF ");
                if (of) {
                    of += 4;
                    char *colon = strchr(of, ':');
                    if (colon) {
                        int fn = (int)(colon - of);
                        if (fn > 255) fn = 255;
                        strncpy(file_buf, of, fn);
                        file_buf[fn] = '\0';
                    }
                }
            }
            if (file_buf[0] && line_str[0]) {
                fprintf(stdout, "  C51 WARNING: %s:%s: %s\n", file_buf, line_str, desc);
            } else {
                /* fallback: 打印原始行 */
                char dbg[1024]; strncpy(dbg, line, 1023); dbg[1023]=0;
                int dlen = strlen(dbg); while(dlen>0&&(dbg[dlen-1]=='\n'||dbg[dlen-1]=='\r')) dbg[--dlen]=0;
                fprintf(stdout, "  C51 WARNING: %s\n", dbg);
            }
            continue;
        }

        set_color(is_err ? CLR_ERROR : CLR_WARNING);
        if (func[0]) {
            /* 尝试反向查找中文原名 */
            const char *cn_func = name_map ? lookup_chinese(name_map, func) : NULL;
            if (cn_func)
                fprintf(stdout, "  %s %s: '%s'(%s): %s\n",
                        is_err ? "ERROR" : "WARNING", source_label, func, cn_func, cn);
            else
                fprintf(stdout, "  %s %s: '%s': %s\n",
                        is_err ? "ERROR" : "WARNING", source_label, func, cn);
        }
        else
            fprintf(stdout, "  %s %s: %s\n",
                    is_err ? "ERROR" : "WARNING", source_label, cn);
        set_color(CLR_DEFAULT);
    }
    fclose(f);
}

/* ── 打印链接器警告/错误（带中文翻译 + 中文名反向查找）── */
static void print_linker_warnings(const char *log_file, Dict *name_map) {
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
        if (sym[0]) {
            const char *cn_sym = name_map ? lookup_chinese(name_map, sym) : NULL;
            if (cn_sym)
                fprintf(stdout, "  %s: '%s'(%s): %s\n",
                        is_err ? "ERROR" : "WARNING", sym, cn_sym, desc);
            else
                fprintf(stdout, "  %s: '%s': %s\n",
                        is_err ? "ERROR" : "WARNING", sym, desc);
        }
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

/* ── 合并多个 Dict 中的条目 ── */
static void merge_name_maps(Dict *dest, Dict *src) {
    if (!dest || !src) return;
    for (int i = 0; i < list_len(src->list); i++) {
        DictEntry *e = (DictEntry*)list_get(src->list, i);
        if (!dict_get(dest, e->key))
            dict_put(dest, strdup(e->key), strdup((const char*)e->val));
    }
}

/* ── 检测 C51 源码中是否有 #pragma SRC ── */
static bool has_pragma_src(const char *c51_source) {
    FILE *f = fopen(c51_source, "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "#pragma SRC") || strstr(line, "#pragma\tsRC") ||
            strstr(line, "#pragma\tSRC") || strstr(line, "#pragma  SRC")) {
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

/* ── 编译单个 C51 文件（自动检测 #pragma SRC 并走 SRC→A51→OBJ 路径）── */
static int compile_c51_file(const char *keil_bin, const char *keil_root,
                            const char *source_file,
                            const char *source_label, const char *base_name,
                            const char *model_flag, const char *log,
                            bool is_src_mode,
                            List *include_dirs) {
    (void)keil_root; (void)source_label; (void)include_dirs;
    if (is_src_mode) {
        /* #pragma SRC 模式：
         * C51.exe 不生成 .OBJ，只生成 .SRC 到源文件同目录。 */
        char src_dir[1024], src_file_only[256];
        {
            const char *p = strrchr(source_file, '\\');
            if (p) {
                int n = (int)(p - source_file);
                memcpy(src_dir, source_file, n); src_dir[n] = '\0';
                strncpy(src_file_only, p + 1, sizeof(src_file_only) - 1);
                src_file_only[sizeof(src_file_only) - 1] = '\0';
            } else {
                snprintf(src_dir, sizeof(src_dir), ".");
                strncpy(src_file_only, source_file, sizeof(src_file_only) - 1);
                src_file_only[sizeof(src_file_only) - 1] = '\0';
            }
        }
        {
            char args[8192], cmd[8192];
            snprintf(args, sizeof(args), "%s %s",
                     source_file, model_flag);
            snprintf(cmd, sizeof(cmd), "%s\\C51.exe %s >%s 2>&1",
                     keil_bin, args, log);
            int ret = system(cmd);
            if (ret && ret != 1) return ret;
        }
        /* A51: 汇编 .SRC → .OBJ */
        {
            char src_path[1024], obj_path[1024], a51_log[1024];
            snprintf(src_path, sizeof(src_path), "%s\\%s.SRC", src_dir, base_name);
            snprintf(obj_path, sizeof(obj_path), "%s\\%s.OBJ", get_cwd(), base_name);
            snprintf(a51_log, sizeof(a51_log), "%s_a51_%s.log", log, base_name);
            fprintf(stdout, "  A51: %s.SRC\n", base_name);
            int ret = assemble_a51(keil_bin, src_path, obj_path, a51_log);
            _unlink(a51_log);
            return ret;
        }
    } else {
        /* 普通模式：C51 → .OBJ */
        char obj_with_ext[512];
        snprintf(obj_with_ext, sizeof(obj_with_ext), "%s.OBJ", base_name);
        char args[8192];
        snprintf(args, sizeof(args), "%s OBJECT(%s) %s",
                 source_file, obj_with_ext, model_flag);
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "%s\\C51.exe %s >%s 2>&1",
                 keil_bin, args, log);
        return system(cmd);
    }
}

/* ── 主流程：多文件编译 → 链接 → HEX ── */
int embed_run_toolchain(List *source_files, List *source_labels,
                        const char *hex_out,
                        const char *temp_dir, int c51_model,
                        List *include_dirs) {
    (void)temp_dir;
    if (list_empty(source_files)) return -1;

    const char *keil_bin = detect_keil_bin();
    if (!keil_bin) { fprintf(stdout, "error: Keil C51 not found\n"); return -1; }
    const char *cwd = get_cwd();

    const char *model_flag = "SMALL";
    if (c51_model == 1) model_flag = "COMPACT";
    else if (c51_model == 2) model_flag = "LARGE";

    char log[512];
    snprintf(log, sizeof(log), "%s\\ttcc_log.txt", temp_dir);

    /* 合并所有文件的 name_map */
    Dict *all_name_maps = make_dict(NULL);
    /* 收集所有 OBJ 路径 */
    List *obj_list = make_list();
    /* 收集需要清理的临时文件 */
    List *cleanup_files = make_list();
    int n_total_warnings = 0, n_total_errors = 0;

    /* ── 阶段1: 逐个编译 C51 文件 ── */
    {
        for (int fi = 0; fi < list_len(source_files); fi++) {
            const char *source_file = (const char*)list_get(source_files, fi);
            const char *source_label = (fi < list_len(source_labels))
                ? (const char*)list_get(source_labels, fi) : source_file;

            char base_name[256];
            extract_basename(source_file, base_name, sizeof(base_name));

            /* 解析该文件的中文→ASCII 名映射并合并 */
            Dict *file_map = parse_name_map(source_file);
            merge_name_maps(all_name_maps, file_map);
            /* free file_map entries but keep strings (already strdup'd into all_name_maps) */
            {
                for (int i = 0; i < list_len(file_map->list); i++) {
                    DictEntry *e = (DictEntry*)list_get(file_map->list, i);
                    free(e->key); free(e->val); free(e);
                }
                free(file_map);
            }

            char obj_path[512];
            snprintf(obj_path, sizeof(obj_path), "%s\\%s.OBJ", cwd, base_name);
            list_push(obj_list, strdup(obj_path));
            list_push(cleanup_files, strdup(obj_path));

            /* 检测是否有 #pragma SRC */
            bool src_mode = has_pragma_src(source_file);
            if (src_mode) {
                /* SRC 模式也会产生 .SRC 临时文件 */
                char src_path[512];
                snprintf(src_path, sizeof(src_path), "%s\\%s.SRC", cwd, base_name);
                list_push(cleanup_files, strdup(src_path));
            }

            /* C51 编译 */
            fprintf(stdout, "  C51: %s%s\n", source_label, src_mode ? " (SRC mode)" : "");
            const char *keil_root = detect_keil_root();
            int ret = compile_c51_file(keil_bin, keil_root, source_file, source_label,
                                       base_name, model_flag, log, src_mode,
                                       include_dirs);
            if (ret && ret != 1) {
                fprintf(stdout, "FAILED (exit %d)\n", ret);
                return -1;
            }
            /* 统计警告/错误 */
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
            n_total_warnings += nw;
            n_total_errors += ne;
            if (ne || nw)
                print_c51_warnings(log, source_label, all_name_maps);
        }
        if (n_total_errors || n_total_warnings) {
            set_color(n_total_errors ? CLR_ERROR : CLR_WARNING);
            fprintf(stdout, "  Total: %d warning(s), %d error(s)\n",
                    n_total_warnings, n_total_errors);
            set_color(CLR_DEFAULT);
        }
        _unlink(log);
    }

    /* ── 阶段2: A51 汇编 STARTUP.A51 和 INIT.A51 ── */
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
        list_push(cleanup_files, strdup(startup_obj));
        list_push(cleanup_files, strdup(init_obj));

        fprintf(stdout, "  A51: STARTUP.A51\n");
        if (assemble_a51(keil_bin, startup_a51, startup_obj, a51_log)) return -1;
        fprintf(stdout, "  A51: INIT.A51\n");
        if (assemble_a51(keil_bin, init_a51, init_obj, a51_log)) return -1;
        _unlink(a51_log);
    }

    /* ── 阶段3: BL51 链接所有 OBJ + LIB → .ABS，再 OH51 → Intel HEX ── */
    {
        const char *model_lib = "C51S.LIB";
        const char *model_fplib = "C51FPS.LIB";
        if (c51_model == 1) { model_lib = "C51C.LIB"; model_fplib = "C51FPC.LIB"; }
        else if (c51_model == 2) { model_lib = "C51L.LIB"; model_fplib = "C51FPL.LIB"; }
        const char *keil_root = detect_keil_root();
        char lib_path[512], fplib_path[512];
        snprintf(lib_path, sizeof(lib_path), "%s\\LIB\\%s", keil_root, model_lib);
        snprintf(fplib_path, sizeof(fplib_path), "%s\\LIB\\%s", keil_root, model_fplib);

        /* 构建 BL51 输入: obj1.OBJ,...,objN.OBJ,STARTUP.OBJ,INIT.OBJ,LIB,FPLIB TO hex */
        char args[16384];
        int pos = 0;
        for (int i = 0; i < list_len(obj_list); i++) {
            const char *obj = (const char*)list_get(obj_list, i);
            if (i > 0) args[pos++] = ',';
            args[pos++] = '"';
            int len = (int)strlen(obj);
            memcpy(args + pos, obj, len); pos += len;
            args[pos++] = '"';
        }
        {
            char startup_obj[512], init_obj[512];
            snprintf(startup_obj, sizeof(startup_obj), "%s\\STARTUP.OBJ", cwd);
            snprintf(init_obj, sizeof(init_obj), "%s\\INIT.OBJ", cwd);
            snprintf(args + pos, sizeof(args) - pos, ",\"%s\",\"%s\"",
                     startup_obj, init_obj);
            pos = (int)strlen(args);
        }

        /* 计算 ABS 中间文件路径 */
        const char *first = (const char*)list_get(source_files, 0);
        char bn[256];
        extract_basename(first, bn, sizeof(bn));
        char abs_path[512];
        snprintf(abs_path, sizeof(abs_path), "%s\\%s.ABS", cwd, bn);

        /* 计算最终 HEX 输出路径 */
        char hex_abs[512];
        if (hex_out) {
            strncpy(hex_abs, hex_out, sizeof(hex_abs) - 1);
            hex_abs[sizeof(hex_abs) - 1] = '\0';
        } else {
            snprintf(hex_abs, sizeof(hex_abs), "%s\\a.HEX", cwd);
        }

        /* 追加 LIB, FPLIB, TO（BL51 只输出 .ABS 格式，之后 OH51 转 Intel HEX） */
        snprintf(args + pos, sizeof(args) - pos, ",\"%s\",\"%s\" TO \"%s\"",
                 lib_path, fplib_path, abs_path);

        /* 运行 BL51 */
        char bl51_path[512];
        snprintf(bl51_path, sizeof(bl51_path), "%s\\BL51.EXE", keil_bin);
        char cmd[16384];
        snprintf(cmd, sizeof(cmd), "%s %s >%s 2>&1",
                 bl51_path, args, log);
        fprintf(stdout, "  BL51: linking %d object(s)\n", list_len(obj_list));
        int ret = system(cmd);
        /* 若失败尝试 C:\Keil_v5 */
        if (ret && ret != 1 && _access("C:\\Keil_v5\\C51\\BIN\\BL51.EXE", 0) == 0) {
            snprintf(bl51_path, sizeof(bl51_path), "C:\\Keil_v5\\C51\\BIN\\BL51.EXE");
            snprintf(cmd, sizeof(cmd), "%s %s >%s 2>&1",
                     bl51_path, args, log);
        }
        if (ret && ret != 1) {
            fprintf(stdout, "error: BL51 failed (exit %d)\n", ret);
            _unlink(log);
            return -1;
        }
        print_linker_warnings(log, all_name_maps);
        const char *size = extract_program_size(log);
        if (size) {
            char size_trim[256];
            strncpy(size_trim, size, sizeof(size_trim) - 1);
            size_trim[strcspn(size_trim, "\r\n")] = '\0';
            fprintf(stdout, "  %s\n", size_trim);
        }
        _unlink(log);

        /* ── OH51: .ABS → Intel HEX ── */
        {
            char oh51_log[512];
            snprintf(oh51_log, sizeof(oh51_log), "%s\\oh51_log.txt", temp_dir);
            /* OH51 输出 <basename>.hex 到 CWD。先将 .ABS 复制到 CWD 下短名 */
            char abs_short[512];
            snprintf(abs_short, sizeof(abs_short), "%s\\ttcc_out.ABS", cwd);
            _unlink(abs_short);
            {
                FILE *fs = fopen(abs_path, "rb");
                if (fs) {
                    FILE *fd = fopen(abs_short, "wb");
                    if (fd) {
                        char bf[4096]; size_t n;
                        while ((n = fread(bf, 1, sizeof(bf), fs)) > 0) fwrite(bf, 1, n, fd);
                        fclose(fd);
                    }
                    fclose(fs);
                }
            }
            fprintf(stdout, "  OH51: %s → %s\n", abs_path, hex_abs);
            char oh51_cmd[16384];
            snprintf(oh51_cmd, sizeof(oh51_cmd),
                     "%s\\OH51.EXE %s >%s 2>&1",
                     keil_bin, abs_short, oh51_log);
            int oh_ret = system(oh51_cmd);
            if (oh_ret && oh_ret != 1) {
                fprintf(stdout, "error: OH51 failed (exit %d)\n", oh_ret);
                _unlink(oh51_log);
                return -1;
            }
            _unlink(oh51_log);
            /* OH51 输出 ttcc_out.hex 到 CWD，复制到最终目标 */
            {
                char oh51_out[512];
                snprintf(oh51_out, sizeof(oh51_out), "%s\\ttcc_out.hex", cwd);
                _unlink(hex_abs);
                if (rename(oh51_out, hex_abs) != 0) {
                    FILE *fs = fopen(oh51_out, "rb");
                    if (fs) {
                        FILE *fd = fopen(hex_abs, "wb");
                        if (fd) {
                            char bf[4096]; size_t n;
                            while ((n = fread(bf, 1, sizeof(bf), fs)) > 0) fwrite(bf, 1, n, fd);
                            fclose(fd);
                        }
                        fclose(fs);
                    }
                }
                _unlink(oh51_out);
            }
            _unlink(abs_short);
        }

        /* 如果 hex_out 不同于 hex_abs，才复制 */
        if (hex_out && strcmp(hex_abs, hex_out) != 0) {
            FILE *fs = fopen(hex_abs, "rb");
            if (fs) {
                FILE *fd = fopen(hex_out, "wb");
                if (fd) {
                    char buf[4096]; size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), fs)) > 0) fwrite(buf, 1, n, fd);
                    fclose(fd);
                }
                fclose(fs);
            }
        }
        fprintf(stdout, "HEX file: %s\n", hex_out ? hex_out : hex_abs);
    }

    /* ── 清理临时文件 ── */
    for (int i = 0; i < list_len(cleanup_files); i++) {
        _unlink((const char*)list_get(cleanup_files, i));
    }
    {
        const char *first = (const char*)list_get(source_files, 0);
        char bn[256];
        extract_basename(first, bn, sizeof(bn));
        char m51_abs[512], abs_abs[512];
        snprintf(m51_abs, sizeof(m51_abs), "%s\\%s.M51", cwd, bn);
        snprintf(abs_abs, sizeof(abs_abs), "%s\\%s.ABS", cwd, bn);
        _unlink(m51_abs);
        _unlink(abs_abs);
    }
    return 0;
}

int g_embed_debug_flag = 0;

