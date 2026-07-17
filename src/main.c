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

/* ─── 编译所有输入文件 → AST 集合 ─── */
static List *compile_all(List *inputs, List *include_dirs, int target, int c51_model) {
    List *all_toplevels = make_list();
    for (Iter it = list_iter(inputs); !iter_end(it); ) {
        char *path = iter_next(&it);
        if (target == TGT_MCS51) setup_mcs51_defines(c51_model);
        for (Iter di = list_iter(include_dirs); !iter_end(di); )
            pp_global_add_include_path(iter_next(&di));
        if (!pp_preprocess_to_stdin(path)) die("preprocess failed");
        set_current_filename(path);
        parser_reset();
        List *tops = read_toplevels();
        if (!tops) die("parse failed");
        for (int i = 0; i < list_len(tops); i++)
            list_push(all_toplevels, list_get(tops, i));
    }
    pp_cleanup_temp();
    return all_toplevels;
}

/* ─── 生成 C51 源码到临时文件 ─── */
static char *generate_c51_source(List *all_toplevels, int c51_model, bool map_names) {
    char c51_dir[4096];
    DWORD ret = GetTempPathA(sizeof(c51_dir), c51_dir);
    if (ret == 0 || ret > sizeof(c51_dir))
        strcpy(c51_dir, "C:\\TEMP\\");
    char *c51_path = malloc(4096);
    snprintf(c51_path, 4096, "%sttcc_%d.c", c51_dir, _getpid());
    FILE *c51_out = fopen(c51_path, "w");
    if (!c51_out) die("cannot create temp file");
    List *saved_strings = strings;
    List *saved_ctypes = ctypes;
    List *lowered = lower_program(all_toplevels, c51_model);
    c51_emit_translation_unit(c51_out, lowered, c51_model, map_names);
    fclose(c51_out);
    list_free(saved_strings);
    list_free(saved_ctypes);
    return c51_path;
}

/* ─── --no-build 模式：输出 C51 源码 ─── */
static void output_source_mode(const char *c51_path, const char *outfile) {
    if (outfile) {
        rename(c51_path, outfile);
        fprintf(stderr, "note: C51 source -> %s\n", outfile);
    } else {
        FILE *f = fopen(c51_path, "r");
        if (f) { char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
            fclose(f);
        }
        _unlink(c51_path);
    }
}

/* ─── 默认模式：调用 Keil 工具链生成 HEX ─── */
static void build_hex_mode(const char *c51_path, const char *outfile,
                           const char *first_input, int c51_model,
                           List *include_dirs) {
    char hex_path[4096];
    const char *hex_out = outfile;
    if (!hex_out) {
        const char *base = first_input ? first_input : "a";
        char bn[256];
        const char *dot = strrchr(base, '.');
        int n = dot ? (int)(dot - base) : (int)strlen(base);
        if (n > 255) n = 255;
        strncpy(bn, base, n); bn[n] = '\0';
        snprintf(hex_path, sizeof(hex_path), "%s.HEX", bn);
        hex_out = hex_path;
    }
    char tmp_dir[4096];
    DWORD r2 = GetTempPathA(sizeof(tmp_dir), tmp_dir);
    if (r2 == 0 || r2 > sizeof(tmp_dir))
        strcpy(tmp_dir, "C:\\TEMP\\");
    int ret = embed_run_toolchain(c51_path, first_input, hex_out, tmp_dir, c51_model, include_dirs);
    _unlink(c51_path);
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

    /* 编译 → C51 源码（--no-build 时保留中文名，HEX 构建时映射为 ASCII）*/
    List *all_toplevels = compile_all(opts.inputs, opts.include_dirs, opts.target, opts.c51_model);
    bool map_names = !opts.no_build;
    char *c51_path = generate_c51_source(all_toplevels, opts.c51_model, map_names);

    /* 输出 */
    if (opts.no_build) {
        output_source_mode(c51_path, opts.outfile);
    } else {
        build_hex_mode(c51_path, opts.outfile, opts.first_input, opts.c51_model, opts.include_dirs);
    }

    /* 清理 */
    for (Iter it = list_iter(opts.inputs); !iter_end(it); )
        free(iter_next(&it));
    free(opts.inputs);
    return 0;
}
