#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <io.h>
#include <process.h>
#include "core/cc.h"
#include "core/c11_lowering.h"
#include "core/codegen_c51.h"
#include "core/embed_toolchain.h"

/* ─── 控制台 UTF-8 编码 ─── */
static void init_console(void) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

/* ─── 枚举常量 ─── */
enum { TGT_HOST = 0, TGT_MCS51 };
enum { MODEL_SMALL = 0, MODEL_COMPACT, MODEL_LARGE };

/* ─── CLI 解析结果 ─── */
typedef struct {
    const char *outfile;
    List *inputs;          /* 输入文件路径列表 */
    List *include_dirs;    /* -I 目录列表 */
    int target;
    int c51_model;
    bool no_build;
    const char *first_input;
} CliOptions;

/* ─── 辅助函数 ─── */
static void die(const char *msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <input.c>... | @list.txt\n", prog ? prog : "ttcc");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o <file>           output HEX file (default: <input>.HEX)\n");
    fprintf(stderr, "  -I<dir>             add include search path\n");
    fprintf(stderr, "  -D<name>[=<val>]    predefine macro\n");
    fprintf(stderr, "  --target <name>     target: mcs51, c51, 8051 (default: host)\n");
    fprintf(stderr, "  --model <name>      C51 memory model: small, compact, large\n");
    fprintf(stderr, "  --no-build          output C51 source to stdout instead of HEX\n");
    fprintf(stderr, "  @<file>             read arguments (options + inputs) from <file>\n");
}

/* ─── 从 @file 展开参数到 token 列表（自动跳过注释行，按空白拆分） ─── */
static void expand_argfile(const char *path, List *tokens) {
    FILE *lf = fopen(path, "r");
    if (!lf) { fprintf(stderr, "error: cannot open '%s'\n", path); exit(1); }
    char line[4096];
    while (fgets(line, sizeof(line), lf)) {
        /* 去掉尾部空白 */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' ' || line[len-1] == '\t'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;
        /* 按空白拆分 token */
        char *p = line;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char *start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            char saved = *p;
            *p = '\0';
            list_push(tokens, strdup(start));
            *p = saved;
        }
    }
    fclose(lf);
}

/* ─── 命令行参数解析 ─── */
static void parse_args(CliOptions *opts, const char *prog, int argc, char **argv) {
    memset(opts, 0, sizeof(*opts));
    opts->inputs = make_list();
    opts->include_dirs = make_list();
    opts->c51_model = MODEL_SMALL;

    /* 阶段 1: 将 argv[1..] + @file 展开为扁平 token 列表 */
    List *tokens = make_list();
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '@')
            expand_argfile(argv[i] + 1, tokens);
        else
            list_push(tokens, strdup(argv[i]));
    }

    /* 阶段 2: 解析扁平 token 列表 */
    int nt = list_len(tokens);
    for (int i = 0; i < nt; i++) {
        const char *a = list_get(tokens, i);
        if (!strcmp(a, "-o")) {
            if (++i >= nt) die("-o needs arg");
            opts->outfile = list_get(tokens, i);
        } else if (!strncmp(a, "-I", 2)) {
            const char *p = a[2] ? a + 2 : (++i < nt ? list_get(tokens, i) : NULL);
            if (!p) die("-I needs arg");
            pp_global_add_include_path(p);
            list_push(opts->include_dirs, strdup(p));
        } else if (!strncmp(a, "-D", 2)) {
            const char *def = a[2] ? a + 2 : (++i < nt ? list_get(tokens, i) : NULL);
            if (!def) die("-D needs arg");
            char name[256]; const char *val = "1";
            const char *eq = strchr(def, '=');
            if (eq) {
                int n = (int)(eq - def); if (n >= 256) n = 255;
                strncpy(name, def, n); name[n] = '\0'; val = eq + 1;
            } else { strncpy(name, def, 255); name[255] = '\0'; }
            pp_global_define(name, val);
        } else if (!strcmp(a, "--target")) {
            if (++i >= nt) die("--target needs arg");
            const char *t = list_get(tokens, i);
            if (!strcmp(t, "mcs51") || !strcmp(t, "c51") || !strcmp(t, "8051") || !strcmp(t, "MCS51"))
                opts->target = TGT_MCS51;
            else fprintf(stderr, "warning: unknown target '%s'\n", t);
        } else if (!strcmp(a, "--model")) {
            if (++i >= nt) die("--model needs arg");
            if (!strcmp(list_get(tokens, i), "small")) opts->c51_model = MODEL_SMALL;
            else if (!strcmp(list_get(tokens, i), "compact")) opts->c51_model = MODEL_COMPACT;
            else if (!strcmp(list_get(tokens, i), "large")) opts->c51_model = MODEL_LARGE;
            else die("--model must be small, compact, or large");
        } else if (!strcmp(a, "--no-build")) {
            opts->no_build = true;
        } else if (a[0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            usage(prog); exit(1);
        } else {
            list_push(opts->inputs, strdup(a));
            if (!opts->first_input) opts->first_input = a;
        }
    }

    /* 清理临时 token 列表（字符串本身由后续清理代码负责） */
    free(tokens);
}

/* ─── 配置 MCS51 预处理宏 ─── */
static void setup_mcs51_defines(int c51_model) {
    pp_global_define("__MCS51__", "1");
    pp_global_define("__C51__",   "1");
    pp_global_define("__INT_MAX__", "32767");
    pp_global_define("__UINT_MAX__", "65535");
    pp_global_define("__LONG_MAX__", "2147483647");
    if (c51_model == MODEL_SMALL)   pp_global_define("__SMALL__", "1");
    if (c51_model == MODEL_COMPACT) pp_global_define("__COMPACT__","1");
    if (c51_model == MODEL_LARGE)   pp_global_define("__LARGE__", "1");
}

/* ─── extract_basename: 提取基础文件名 ─── */
static void extract_basename(const char *path, char *out, int out_sz) {
    const char *base = strrchr(path, '/');
    if (!base) base = strrchr(path, '\\');
    if (!base) base = path; else base++;
    const char *dot = strrchr(base, '.');
    int n = dot ? (int)(dot - base) : (int)strlen(base);
    if (n > out_sz - 1) n = out_sz - 1;
    strncpy(out, base, n); out[n] = '\0';
}

/* 全局 PP 初始化状态，通过 hook 重建 */
static int g_setup_target = 0;
static int g_setup_c51_model = 0;
static List *g_setup_include_dirs = NULL;

static void pp_setup_hook(void) {
    if (g_setup_target == TGT_MCS51) setup_mcs51_defines(g_setup_c51_model);
    if (g_setup_include_dirs) {
        for (Iter it = list_iter(g_setup_include_dirs); !iter_end(it); )
            pp_global_add_include_path(iter_next(&it));
    }
}

/* ─── 编译单个输入文件 → AST ─── */
static List *compile_single_file(const char *path, List *include_dirs, int target, int c51_model) {
    g_setup_target = target;
    g_setup_c51_model = c51_model;
    g_setup_include_dirs = include_dirs;
    pp_global_on_init(pp_setup_hook);
    if (!pp_preprocess_to_stdin(path)) die("preprocess failed");
    set_current_filename(path);
    lexer_reset();
    parser_reset();
    List *tops = read_toplevels();
    if (!tops) die("parse failed");
    pp_cleanup_temp();
    return tops;
}

/* ─── 旧版：从单个 AST 生成 C51 源码到临时文件（--no-build 模式用）─── */
static char *generate_c51_file_old(List *toplevels, int c51_model, bool map_names) {
    char c51_dir[4096];
    DWORD ret = GetTempPathA(sizeof(c51_dir), c51_dir);
    if (ret == 0 || ret > sizeof(c51_dir))
        strcpy(c51_dir, "C:\\TEMP\\");
    char *c51_path = malloc(8192);
    snprintf(c51_path, 8192, "%sttcc_%d_%d.c", c51_dir, _getpid(), rand());
    FILE *c51_out = fopen(c51_path, "w");
    if (!c51_out) die("cannot create temp file");
    List *lowered = lower_program(toplevels, c51_model);
    c51_emit_translation_unit(c51_out, lowered, c51_model, map_names);
    fclose(c51_out);
    return c51_path;
}

/* ─── 旧版：为所有输入文件生成独立 C51 文件（--no-build 模式用）─── */
static List *generate_all_c51_files(List *inputs, List *include_dirs, int target, int c51_model, bool map_names) {
    List *c51_files = make_list();
    for (Iter it = list_iter(inputs); !iter_end(it); ) {
        char *path = iter_next(&it);
        List *tops = compile_single_file(path, include_dirs, target, c51_model);
        char *c51_path = generate_c51_file_old(tops, c51_model, map_names);
        list_push(c51_files, c51_path);
    }
    return c51_files;
}

/* ─── --no-build 模式：输出 C51 源码 ─── */
static void output_source_mode_multi(List *c51_paths, List *inputs, const char *outfile) {
    if (outfile && list_len(c51_paths) == 1) {
        rename((const char*)list_get(c51_paths, 0), outfile);
        fprintf(stderr, "note: C51 source -> %s\n", outfile);
    } else if (outfile && list_len(c51_paths) > 1) {
        char dir[4096];
        strncpy(dir, outfile, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        CreateDirectoryA(dir, NULL);
        for (int i = 0; i < list_len(c51_paths); i++) {
            const char *src = (const char*)list_get(c51_paths, i);
            const char *in = (const char*)list_get(inputs, i);
            char bn[256]; char dst[8192];
            extract_basename(in, bn, sizeof(bn));
            snprintf(dst, sizeof(dst), "%s\\%s.c51", dir, bn);
            rename(src, dst);
            fprintf(stderr, "note: C51 source -> %s\n", dst);
        }
    } else {
        for (int i = 0; i < list_len(c51_paths); i++) {
            const char *path = (const char*)list_get(c51_paths, i);
            FILE *f = fopen(path, "r");
            if (f) {
                char buf[4096]; size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
                fclose(f);
            }
            _unlink(path);
        }
    }
}

/* ─── 为源路径加上 .51 后缀（main.c → main.51, timer.h → timer.h51）─── */
static void append_51_ext(const char *src_path, char *out, int out_sz) {
    char base[256];
    extract_basename(src_path, base, sizeof(base));
    snprintf(out, out_sz, "%s.51", base);
}

/* ─── 获取目录名 ─── */
static void get_dirname(const char *path, char *out, int out_sz) {
    const char *sep = strrchr(path, '/');
    if (!sep) sep = strrchr(path, '\\');
    if (sep) {
        int n = (int)(sep - path);
        snprintf(out, out_sz, "%.*s", n, path);
    } else {
        snprintf(out, out_sz, ".");
    }
}

/* ─── 流水线：生成 .51 到 build/ 镜像目录，然后编译 ─── */
static void build_with_includes(List *inputs, List *include_dirs,
                                const char *outfile,
                                const char *first_input, int target,
                                int c51_model) {
    const char *first = first_input ? first_input : (const char*)list_get(inputs, 0);

    /* 确定 build 目录 */
    char build_root[8192] = "build";
    CreateDirectoryA("build", NULL);
    {
        /* 在 build 下镜像第一层目录：stc15w4k48s4/ → build/stc15w4k48s4/ */
        char dir[4096];
        get_dirname(first, dir, sizeof(dir));
        if (strcmp(dir, ".") != 0) {
            snprintf(build_root, sizeof(build_root), "build\\%s", dir);
            for (char *p = build_root + 6; *p; p++) {
                if (*p == '\\' || *p == '/') {
                    char saved = *p; *p = '\0';
                    CreateDirectoryA(build_root, NULL);
                    *p = saved;
                }
            }
            CreateDirectoryA(build_root, NULL);
        }
    }
    bool map_names = true;
    /* ── 阶段1: 编译所有输入文件，合并 AST ── */
    List *all_tops = make_list();
    for (int i = 0; i < list_len(inputs); i++) {
        const char *in_path = (const char*)list_get(inputs, i);
        List *tops = compile_single_file(in_path, include_dirs, target, c51_model);
        for (int j = 0; j < list_len(tops); j++)
            list_push(all_tops, list_get(tops, j));
    }
    /* ── 阶段2: 一次降级 + 一次代码生成 ── */
    List *lowered = lower_program(all_tops, c51_model);
    /* 输出文件名取第一个输入 */
    char out_name[4096];
    append_51_ext(first, out_name, sizeof(out_name));
    char full_path[12288];
    snprintf(full_path, sizeof(full_path), "%s\\%s", build_root, out_name);
    char dir[4096];
    get_dirname(full_path, dir, sizeof(dir));
    CreateDirectoryA(dir, NULL);
    FILE *f = fopen(full_path, "w");
    if (!f) { fprintf(stderr, "error: cannot write %s\n", full_path); exit(1); }
    c51_emit_translation_unit(f, lowered, c51_model, map_names);
    fclose(f);
    fprintf(stdout, "  C51: %s\n", full_path);
    List *c51_paths = make_list();
    list_push(c51_paths, strdup(full_path));
    char hex_path[4096];
    const char *hex_out = outfile;
    if (!hex_out) {
        char bn[256];
        extract_basename(first, bn, sizeof(bn));
        snprintf(hex_path, sizeof(hex_path), "%s.HEX", bn);
        hex_out = hex_path;
    }
    char tmp_dir[4096];
    DWORD r2 = GetTempPathA(sizeof(tmp_dir), tmp_dir);
    if (r2 == 0 || r2 > sizeof(tmp_dir))
        strcpy(tmp_dir, "C:\\TEMP\\");
    List *c51_labels = make_list();
    list_push(c51_labels, strdup(first));
    int ret = embed_run_toolchain(c51_paths, c51_labels, hex_out, tmp_dir, c51_model, include_dirs);
    if (ret) { fprintf(stderr, "error: toolchain failed\n"); exit(1); }
}

/* ─── 入口 ─── */
int main(int argc, char **argv) {
    init_console();
    CliOptions opts;
    parse_args(&opts, argv[0], argc, argv);
    if (list_empty(opts.inputs)) { usage(argv[0]); return 1; }

    if (opts.target == TGT_MCS51)
        parser_set_target_mcs51();

    /* 添加 include 路径到预处理器 */
    for (Iter it = list_iter(opts.include_dirs); !iter_end(it); )
        pp_global_add_include_path(iter_next(&it));

    /* 输出 */
    if (opts.no_build) {
        /* --no-build 模式：用旧方式生成到临时目录 */
        bool map_names = false;
        List *c51_paths = generate_all_c51_files(opts.inputs, opts.include_dirs, opts.target, opts.c51_model, map_names);
        output_source_mode_multi(c51_paths, opts.inputs, opts.outfile);
    } else {
        build_with_includes(opts.inputs, opts.include_dirs, opts.outfile, opts.first_input,
                             opts.target, opts.c51_model);
    }

    /* 清理 */
    for (Iter it = list_iter(opts.inputs); !iter_end(it); )
        free(iter_next(&it));
    free(opts.inputs);
    return 0;
}

// DEBUG: entry point
