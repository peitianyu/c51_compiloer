#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <windows.h>
#include <io.h>    /* _dup2, _fileno */

#define PP_PATH_SEP '/'
#define PP_PATH_SEP_STR "/"

#include "cc.h"

/* 跨平台临时文件辅助 */
static FILE *pp_tmpfile(void)
{
    return tmpfile();
}

#define PP_LINE_SIZE 4096

typedef enum {
    MACRO_OBJ,      /* 对象式宏 */
    MACRO_FUNC      /* 函数式宏 */
} MacroType;

typedef struct Macro {
    char *name;
    MacroType type;
    char *body;
    List *params;   /* 函数式宏的参数列表 */
    bool is_variadic; /* 是否为变参宏（最后参数为 ...） */
} Macro;

typedef struct InputFile {
    FILE *fp;
    char *filename;
    int line;
    char *buf;
    int buf_len;
    struct InputFile *next;
} InputFile;

typedef struct CondState {
    bool active;        /* 当前条件块是否生效 */
    bool taken;         /* 是否有分支已被执行 */
    struct CondState *next;
} CondState;

typedef struct PPContext {
    Dict *macros;           /* 宏定义表 */
    List *include_paths;    /* 包含路径 */
    InputFile *input;       /* 当前输入文件栈 */
    CondState *cond_stack;  /* 条件编译栈 */
    char *line;             /* 当前行缓冲区 */
    char *last_line;        /* 上次返回的行（用于释放） */
    bool in_block_comment;  /* 是否处于跨行块注释中 */

    /* 预定义宏：每次预处理运行固定的一组日期/时间 */
    char *pp_date;
    char *pp_time;

    /* #pragma once：记录已标记 once 的文件（key=fullpath, val=(void*)1） */
    Dict *pragma_once;
} PPContext;

static bool is_pp_macro(PPContext *ctx, const char *name);
static Macro *get_pp_macro(PPContext *ctx, const char *name);
void pp_undef(PPContext *ctx, const char *name);
static bool handle_ifdef(PPContext *ctx, const char *args, bool is_ifndef);
static bool handle_if(PPContext *ctx, const char *args);
static bool handle_elif(PPContext *ctx, const char *args);
static bool handle_else(PPContext *ctx, const char *args);
static bool handle_endif(PPContext *ctx, const char *args);
static char *expand_macro_simple(PPContext *ctx, const char *line);
static bool should_skip(PPContext *ctx);

static bool handle_error_warning(PPContext *ctx, const char *args, bool is_error);
static bool handle_pragma(PPContext *ctx, const char *args);

static bool pp_remove_macro_entry(PPContext *ctx, const char *name, Macro **out_macro);

static inline bool is_ident_start(char c);
static inline bool is_ident_char(char c);
static inline char *string_steal(String *s);
static bool list_contains_cstr(List *list, const char *s);

/* pp.c 对外接口（本文件内会提前使用） */
const char *pp_current_file(PPContext *ctx);
int pp_current_line(PPContext *ctx);

static bool is_predefined_macro(const char *name);
static char *pp_predefined_macro_expansion(PPContext *ctx, const char *name);
static char *pp_make_c_string_literal(const char *raw);
static void pp_init_date_time(PPContext *ctx);

static void string_rtrim_ws(String *s)
{
    while (s->len > 0) {
        char c = s->body[s->len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v')
            s->body[--s->len] = '\0';
        else
            break;
    }
}

static const char *skip_space(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static bool body_param_index(Macro *m, const char *name, int *out_idx)
{
    if (out_idx) *out_idx = -1;
    if (!m || !m->params) return false;
    int idx = 0;
    for (Iter it = list_iter(m->params); !iter_end(it); idx++) {
        char *param = iter_next(&it);
        if (param && strcmp(param, name) == 0) {
            if (out_idx) *out_idx = idx;
            return true;
        }
    }
    return false;
}

static char *pp_stringify_text(const char *arg)
{
    /* 规范化空白并转义，返回带引号的 C 字符串字面量 */
    const char *p = arg ? arg : "";
    while (*p && isspace((unsigned char)*p)) p++;
    const char *end = p + strlen(p);
    while (end > p && isspace((unsigned char)*(end - 1))) end--;

    String out = make_string();
    string_append(&out, '"');

    bool in_ws = false;
    for (const char *q = p; q < end; q++) {
        unsigned char c = (unsigned char)*q;
        if (isspace(c)) {
            in_ws = true;
            continue;
        }
        if (in_ws) {
            string_append(&out, ' ');
            in_ws = false;
        }
        if (c == '\\' || c == '"') {
            string_append(&out, '\\');
            string_append(&out, (char)c);
        } else {
            string_append(&out, (char)c);
        }
    }

    string_append(&out, '"');
    return string_steal(&out);
}

/* name 会被 DictEntry 引用，Macro 内部不单独复制 name */
static Macro *macro_create(const char *name, MacroType type, const char *body)
{
    Macro *m = malloc(sizeof(Macro));
    m->name = (char *)name;
    m->type = type;
    m->body = strdup(body);
    m->params = NULL;
    m->is_variadic = false;
    return m;
}

static void macro_free(Macro *m)
{
    if (!m) return;
    free(m->body);
    if (m->params) {
        list_free(m->params);
        free(m->params);
    }
    free(m);
}

PPContext *pp_init(void)
{
    PPContext *ctx = calloc(1, sizeof(PPContext));
    ctx->macros = make_dict(NULL);
    ctx->pragma_once = make_dict(NULL);
    ctx->include_paths = make_list();
    ctx->line = malloc(PP_LINE_SIZE);
    ctx->last_line = NULL;

    pp_init_date_time(ctx);
    
    list_push(ctx->include_paths, strdup("."));
    list_push(ctx->include_paths, strdup("/usr/include"));
    list_push(ctx->include_paths, strdup("/usr/local/include"));
        
    return ctx;
}

void pp_free(PPContext *ctx)
{
    if (!ctx) return;
    
    ListNode *node, *tmp;
    list_for_each_safe(node, tmp, ctx->macros->list) {
        DictEntry *e = (DictEntry *)node->elem;
        Macro *m = e->val;
        /* e->key 与 m->name 同源，只释放一次 */
        free(e->key);
        m->name = NULL;
        macro_free(m);
        free(e);
        free(node);
    }
    free(ctx->macros->list);
    free(ctx->macros);
    
    list_free(ctx->include_paths);
    free(ctx->include_paths);
    
    while (ctx->input) {
        InputFile *f = ctx->input;
        ctx->input = f->next;
        if (f->fp) fclose(f->fp);
        free(f->filename);
        free(f->buf);
        free(f);
    }
    
    while (ctx->cond_stack) {
        CondState *s = ctx->cond_stack;
        ctx->cond_stack = s->next;
        free(s);
    }
    
    free(ctx->line);
    free(ctx->last_line);

    free(ctx->pp_date);
    free(ctx->pp_time);

    if (ctx->pragma_once && ctx->pragma_once->list) {
        ListNode *node, *tmp;
        list_for_each_safe(node, tmp, ctx->pragma_once->list) {
            DictEntry *e = (DictEntry *)node->elem;
            if (e) {
                free(e->key);
                free(e);
            }
            free(node);
        }
        free(ctx->pragma_once->list);
        free(ctx->pragma_once);
    }
    free(ctx);
}

static char *pp_get_ident(const char *p, int *len)
{
    const char *start = p;
    if (!is_ident_start(*p)) return NULL;
    p++;
    while (is_ident_char(*p)) p++;
    *len = p - start;
    if (*len == 0) return NULL;
    char *s = malloc(*len + 1);
    strncpy(s, start, *len);
    s[*len] = '\0';
    return s;
}

static bool is_pp_macro(PPContext *ctx, const char *name)
{
    if (is_predefined_macro(name)) return true;
    return get_pp_macro(ctx, name) != NULL;
}

static bool is_predefined_macro(const char *name)
{
    if (!name) return false;
    return strcmp(name, "__FILE__") == 0 ||
           strcmp(name, "__LINE__") == 0 ||
           strcmp(name, "__DATE__") == 0 ||
           strcmp(name, "__TIME__") == 0 ||
           strcmp(name, "__STDC__") == 0 ||
           strcmp(name, "__STDC_VERSION__") == 0 ||
           strcmp(name, "__STDC_HOSTED__") == 0;
}

static void pp_init_date_time(PPContext *ctx)
{
    if (!ctx) return;

    time_t t = time(NULL);
    struct tm tmv;
    struct tm *ptm = NULL;

    /* localtime_r 在部分平台可用；这里用 localtime 兼容即可 */
    ptm = localtime(&t);
    if (!ptm) {
        ctx->pp_date = strdup("Jan  1 1970");
        ctx->pp_time = strdup("00:00:00");
        return;
    }
    tmv = *ptm;

    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    const char *mon = (tmv.tm_mon >= 0 && tmv.tm_mon < 12) ? months[tmv.tm_mon] : "Jan";

    char datebuf[16];
    int day = tmv.tm_mday;
    int year = tmv.tm_year + 1900;
    /* 标准样式："Mmm dd yyyy"，dd 为两字符，前导空格 */
    snprintf(datebuf, sizeof(datebuf), "%s %2d %4d", mon, day, year);
    ctx->pp_date = strdup(datebuf);

    char timebuf[16];
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    ctx->pp_time = strdup(timebuf);
}

static char *pp_make_c_string_literal(const char *raw)
{
    const char *p = raw ? raw : "";
    String out = make_string();
    string_append(&out, '"');
    while (*p) {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\' || c == '"') {
            string_append(&out, '\\');
            string_append(&out, (char)c);
        } else if (c == '\n') {
            string_appendf(&out, "\\n");
        } else if (c == '\r') {
            string_appendf(&out, "\\r");
        } else if (c == '\t') {
            string_appendf(&out, "\\t");
        } else {
            string_append(&out, (char)c);
        }
    }
    string_append(&out, '"');
    return string_steal(&out);
}

static char *pp_predefined_macro_expansion(PPContext *ctx, const char *name)
{
    if (!name) return NULL;

    if (strcmp(name, "__LINE__") == 0) {
        int line = pp_current_line(ctx);
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", line);
        return strdup(buf);
    }
    if (strcmp(name, "__FILE__") == 0) {
        const char *file = pp_current_file(ctx);
        return pp_make_c_string_literal(file ? file : "");
    }
    if (strcmp(name, "__DATE__") == 0) {
        return pp_make_c_string_literal(ctx && ctx->pp_date ? ctx->pp_date : "Jan  1 1970");
    }
    if (strcmp(name, "__TIME__") == 0) {
        return pp_make_c_string_literal(ctx && ctx->pp_time ? ctx->pp_time : "00:00:00");
    }
    if (strcmp(name, "__STDC__") == 0) {
        return strdup("1");
    }
    if (strcmp(name, "__STDC_VERSION__") == 0) {
        return strdup("199901");
    }
    if (strcmp(name, "__STDC_HOSTED__") == 0) {
        return strdup("0");
    }

    return NULL;
}

static Macro *get_pp_macro(PPContext *ctx, const char *name)
{
    if (!ctx || !ctx->macros || !ctx->macros->list) return NULL;
    for (ListNode *node = ctx->macros->list->tail; node; node = node->prev) {
        DictEntry *e = (DictEntry *)node->elem;
        if (e && e->key && strcmp(e->key, name) == 0) {
            return (Macro *)e->val;
        }
    }
    return NULL;
}

void pp_define(PPContext *ctx, const char *name, const char *body)
{
    pp_undef(ctx, name);
    char *name_copy = strdup(name);
    Macro *m = macro_create(name_copy, MACRO_OBJ, body);
    dict_put(ctx->macros, name_copy, m);
}

void pp_define_func(PPContext *ctx, const char *name, List *params, bool is_variadic, const char *body)
{
    pp_undef(ctx, name);
    char *name_copy = strdup(name);
    Macro *m = macro_create(name_copy, MACRO_FUNC, body);
    m->params = params;
    m->is_variadic = is_variadic;
    dict_put(ctx->macros, name_copy, m);
}

static char *join_args_range(List *args, int start)
{
    if (!args || start >= (int)args->len) return strdup("");
    String s = make_string();
    for (int i = start; i < (int)args->len; i++) {
        char *a = (char *)list_get(args, i);
        if (!a) a = "";
        if (i != start) string_appendf(&s, ", %s", a);
        else string_appendf(&s, "%s", a);
    }
    return string_steal(&s);
}

static bool pp_remove_macro_entry(PPContext *ctx, const char *name, Macro **out_macro)
{
    if (out_macro) *out_macro = NULL;
    if (!ctx || !ctx->macros || !ctx->macros->list) return false;

    ListNode *node = ctx->macros->list->tail;
    while (node) {
        DictEntry *entry = (DictEntry *)node->elem;
        if (entry && entry->key && strcmp(entry->key, name) == 0) {
            ListNode *prev = node->prev;
            ListNode *next = node->next;
            if (prev) prev->next = next;
            else ctx->macros->list->head = next;
            if (next) next->prev = prev;
            else ctx->macros->list->tail = prev;

            ctx->macros->list->len--;

            if (out_macro) *out_macro = (Macro *)entry->val;
            free(entry->key);
            free(entry);
            free(node);
            return true;
        }
        node = node->prev;
    }
    return false;
}

void pp_undef(PPContext *ctx, const char *name)
{
    Macro *m = NULL;
    if (pp_remove_macro_entry(ctx, name, &m) && m) {
        m->name = NULL;
        macro_free(m);
    }
}

static bool should_skip(PPContext *ctx)
{
    for (CondState *s = ctx->cond_stack; s; s = s->next) {
        if (!s->active) return true;
    }
    return false;
}

static bool should_skip_parent(PPContext *ctx)
{
    if (!ctx || !ctx->cond_stack) return false;
    for (CondState *s = ctx->cond_stack->next; s; s = s->next) {
        if (!s->active) return true;
    }
    return false;
}

static void cond_push(PPContext *ctx, bool active)
{
    CondState *s = malloc(sizeof(CondState));
    s->active = active;
    s->taken = active;
    s->next = ctx->cond_stack;
    ctx->cond_stack = s;
}

static void cond_pop(PPContext *ctx)
{
    CondState *s = ctx->cond_stack;
    if (!s) return;
    ctx->cond_stack = s->next;
    free(s);
}

static void cond_else(PPContext *ctx)
{
    CondState *s = ctx->cond_stack;
    if (!s) return;
    s->active = !s->taken;
    s->taken = true;
}

/* #if/#elif 常量表达式求值 */
typedef enum {
    PP_TOK_END,
    PP_TOK_NUM,
    PP_TOK_IDENT,
    PP_TOK_OP,
} PPExprTokType;

typedef struct {
    PPExprTokType type;
    long num;
    char ident[128];
    char op[3];
} PPExprTok;

typedef struct {
    const char *p;
    PPExprTok cur;
    PPContext *ctx;
    int suppress;  /* 短路求值：非0时跳过副作用（除零等） */
} PPExprLexer;

static void pp_expr_next(PPExprLexer *lx)
{
    const char *p = lx->p;
    while (*p && isspace((unsigned char)*p)) p++;

    lx->cur.type = PP_TOK_END;
    lx->cur.num = 0;
    lx->cur.ident[0] = '\0';
    lx->cur.op[0] = '\0';

    if (!*p) {
        lx->p = p;
        lx->cur.type = PP_TOK_END;
        return;
    }

    if (isdigit((unsigned char)*p)) {
        char *endp = NULL;
        long v = strtol(p, &endp, 0);
        lx->cur.type = PP_TOK_NUM;
        lx->cur.num = v;
        /* 跳过整数后缀 L, LL, U, UL, ULL, LU 等（大小写均可） */
        while (*endp && (toupper((unsigned char)*endp) == 'L' || toupper((unsigned char)*endp) == 'U'))
            endp++;
        lx->p = endp;
        return;
    }

    if (is_ident_start(*p)) {
        int i = 0;
        while (*p && is_ident_char(*p) && i < (int)sizeof(lx->cur.ident) - 1) {
            lx->cur.ident[i++] = *p++;
        }
        lx->cur.ident[i] = '\0';
        lx->cur.type = PP_TOK_IDENT;
        lx->p = p;
        return;
    }

    if ((p[0] == '|' && p[1] == '|') || (p[0] == '&' && p[1] == '&') ||
        (p[0] == '<' && p[1] == '<') || (p[0] == '>' && p[1] == '>') ||
        (p[0] == '<' && p[1] == '=') || (p[0] == '>' && p[1] == '=') ||
        (p[0] == '=' && p[1] == '=') || (p[0] == '!' && p[1] == '=')) {
        lx->cur.type = PP_TOK_OP;
        lx->cur.op[0] = p[0];
        lx->cur.op[1] = p[1];
        lx->cur.op[2] = '\0';
        lx->p = p + 2;
        return;
    }

    lx->cur.type = PP_TOK_OP;
    lx->cur.op[0] = *p;
    lx->cur.op[1] = '\0';
    lx->p = p + 1;
}

static bool pp_tok_is_op(PPExprLexer *lx, const char *op)
{
    return lx->cur.type == PP_TOK_OP && strcmp(lx->cur.op, op) == 0;
}

static long pp_parse_expr(PPExprLexer *lx);

static long pp_parse_primary(PPExprLexer *lx)
{
    if (lx->cur.type == PP_TOK_NUM) {
        long v = lx->cur.num;
        pp_expr_next(lx);
        return v;
    }

    if (lx->cur.type == PP_TOK_IDENT && strcmp(lx->cur.ident, "defined") == 0) {
        pp_expr_next(lx);
        bool paren = false;
        if (pp_tok_is_op(lx, "(")) {
            paren = true;
            pp_expr_next(lx);
        }
        if (lx->cur.type != PP_TOK_IDENT) {
            error("Expected identifier after defined");
        }
        bool v = is_pp_macro(lx->ctx, lx->cur.ident);
        pp_expr_next(lx);
        if (paren) {
            if (!pp_tok_is_op(lx, ")")) error("Expected ')' after defined(...)");
            pp_expr_next(lx);
        }
        return v ? 1 : 0;
    }

    if (lx->cur.type == PP_TOK_IDENT) {
        /* 未定义标识符按 0 处理（宏在预展开阶段会被替换掉） */
        pp_expr_next(lx);
        return 0;
    }

    if (pp_tok_is_op(lx, "(")) {
        pp_expr_next(lx);
        long v = pp_parse_expr(lx);
        if (!pp_tok_is_op(lx, ")")) error("Expected ')' in #if expression");
        pp_expr_next(lx);
        return v;
    }

    error("Unexpected token in #if expression");
    return 0;
}

static long pp_parse_unary(PPExprLexer *lx)
{
    if (pp_tok_is_op(lx, "+")) { pp_expr_next(lx); return +pp_parse_unary(lx); }
    if (pp_tok_is_op(lx, "-")) { pp_expr_next(lx); return -pp_parse_unary(lx); }
    if (pp_tok_is_op(lx, "!")) { pp_expr_next(lx); return !pp_parse_unary(lx); }
    if (pp_tok_is_op(lx, "~")) { pp_expr_next(lx); return ~pp_parse_unary(lx); }
    return pp_parse_primary(lx);
}

static long pp_parse_mul(PPExprLexer *lx)
{
    long v = pp_parse_unary(lx);
    while (pp_tok_is_op(lx, "*") || pp_tok_is_op(lx, "/") || pp_tok_is_op(lx, "%")) {
        char op0 = lx->cur.op[0];
        pp_expr_next(lx);
        long r = pp_parse_unary(lx);
        if (op0 == '*') v = v * r;
        else if (op0 == '/') {
            if (r == 0) { if (!lx->suppress) error("Division by zero in #if expression"); v = 0; }
            else v = v / r;
        } else {
            if (r == 0) { if (!lx->suppress) error("Modulo by zero in #if expression"); v = 0; }
            else v = v % r;
        }
    }
    return v;
}

static long pp_parse_add(PPExprLexer *lx)
{
    long v = pp_parse_mul(lx);
    while (pp_tok_is_op(lx, "+") || pp_tok_is_op(lx, "-")) {
        char op0 = lx->cur.op[0];
        pp_expr_next(lx);
        long r = pp_parse_mul(lx);
        v = (op0 == '+') ? (v + r) : (v - r);
    }
    return v;
}

static long pp_parse_shift(PPExprLexer *lx)
{
    long v = pp_parse_add(lx);
    while (pp_tok_is_op(lx, "<<") || pp_tok_is_op(lx, ">>")) {
        bool is_shl = (lx->cur.op[0] == '<');
        pp_expr_next(lx);
        long r = pp_parse_add(lx);
        v = is_shl ? (v << r) : (v >> r);
    }
    return v;
}

static long pp_parse_rel(PPExprLexer *lx)
{
    long v = pp_parse_shift(lx);
    while (pp_tok_is_op(lx, "<") || pp_tok_is_op(lx, ">") || pp_tok_is_op(lx, "<=") || pp_tok_is_op(lx, ">=")) {
        char op1 = lx->cur.op[0];
        bool eq = (lx->cur.op[1] == '=');
        pp_expr_next(lx);
        long r = pp_parse_shift(lx);
        if (op1 == '<') v = eq ? (v <= r) : (v < r);
        else v = eq ? (v >= r) : (v > r);
        v = v ? 1 : 0;
    }
    return v;
}

static long pp_parse_eq(PPExprLexer *lx)
{
    long v = pp_parse_rel(lx);
    while (pp_tok_is_op(lx, "==") || pp_tok_is_op(lx, "!=")) {
        bool neq = (lx->cur.op[0] == '!');
        pp_expr_next(lx);
        long r = pp_parse_rel(lx);
        v = neq ? (v != r) : (v == r);
        v = v ? 1 : 0;
    }
    return v;
}

static long pp_parse_bitand(PPExprLexer *lx)
{
    long v = pp_parse_eq(lx);
    while (pp_tok_is_op(lx, "&")) {
        pp_expr_next(lx);
        long r = pp_parse_eq(lx);
        v = v & r;
    }
    return v;
}

static long pp_parse_bitxor(PPExprLexer *lx)
{
    long v = pp_parse_bitand(lx);
    while (pp_tok_is_op(lx, "^")) {
        pp_expr_next(lx);
        long r = pp_parse_bitand(lx);
        v = v ^ r;
    }
    return v;
}

static long pp_parse_bitor(PPExprLexer *lx)
{
    long v = pp_parse_bitxor(lx);
    while (pp_tok_is_op(lx, "|")) {
        pp_expr_next(lx);
        long r = pp_parse_bitxor(lx);
        v = v | r;
    }
    return v;
}

static long pp_parse_expr(PPExprLexer *lx);

static long pp_parse_logand(PPExprLexer *lx)
{
    long v = pp_parse_bitor(lx);
    while (pp_tok_is_op(lx, "&&")) {
        pp_expr_next(lx);
        if (!v) {
            /* 短路：左边为0，不求值右边，&& 结果是 0 */
            lx->suppress++;
            pp_parse_bitor(lx);
            lx->suppress--;
            v = 0;
        } else {
            long r = pp_parse_bitor(lx);
            v = r ? 1 : 0;
        }
    }
    return v;
}

static long pp_parse_logor(PPExprLexer *lx)
{
    long v = pp_parse_logand(lx);
    while (pp_tok_is_op(lx, "||")) {
        pp_expr_next(lx);
        if (v) {
            /* 短路：左边非0，不求值右边，但 || 结果是 1 */
            lx->suppress++;
            pp_parse_logand(lx);
            lx->suppress--;
            v = 1;
        } else {
            long r = pp_parse_logand(lx);
            v = r ? 1 : 0;
        }
    }
    return v;
}

static long pp_parse_ternary(PPExprLexer *lx)
{
    long cond = pp_parse_logor(lx);
    if (!pp_tok_is_op(lx, "?")) return cond;
    pp_expr_next(lx);  /* skip '?' */
    long then_val;
    long else_val;
    if (cond) {
        then_val = pp_parse_expr(lx);
        if (!pp_tok_is_op(lx, ":")) return then_val;
        pp_expr_next(lx);  /* skip ':' */
        lx->suppress++;
        else_val = pp_parse_expr(lx);
        lx->suppress--;
    } else {
        lx->suppress++;
        then_val = pp_parse_expr(lx);
        lx->suppress--;
        if (!pp_tok_is_op(lx, ":")) return 0;
        pp_expr_next(lx);  /* skip ':' */
        else_val = pp_parse_expr(lx);
    }
    return cond ? then_val : else_val;
}

static long pp_parse_expr(PPExprLexer *lx)
{
    return pp_parse_ternary(lx);
}

static List *clone_expanding_with(List *expanding, const char *add)
{
    List *r = make_list();
    for (Iter it = list_iter(expanding); !iter_end(it);) {
        char *v = iter_next(&it);
        list_push(r, strdup(v));
    }
    if (add) list_push(r, strdup(add));
    return r;
}

static char *pp_expand_macros_for_if(PPContext *ctx, const char *args, List *expanding, int depth)
{
    if (!args) return strdup("");
    if (depth > 32) return strdup(args);

    String out = make_string();
    const char *p = args;
    bool expecting_defined_operand = false;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        if (is_ident_start(*p)) {
            int len = 0;
            char *ident = pp_get_ident(p, &len);
            if (!ident) break;

            if (strcmp(ident, "defined") == 0) {
                string_appendf(&out, "defined ");
                expecting_defined_operand = true;
                free(ident);
                p += len;
                continue;
            }

            if (expecting_defined_operand) {
                string_appendf(&out, "%s ", ident);
                expecting_defined_operand = false;
                free(ident);
                p += len;
                continue;
            }

            if (is_predefined_macro(ident)) {
                char *exp = pp_predefined_macro_expansion(ctx, ident);
                if (exp) {
                    string_appendf(&out, "%s ", exp);
                    free(exp);
                    free(ident);
                    p += len;
                    continue;
                }
            }

            if (list_contains_cstr(expanding, ident)) {
                string_appendf(&out, "%s ", ident);
                free(ident);
                p += len;
                continue;
            }

            Macro *m = get_pp_macro(ctx, ident);
            if (m && m->type == MACRO_OBJ) {
                List *next_expanding = clone_expanding_with(expanding, ident);

                const char *body = m->body ? m->body : "";
                const char *b = skip_space(body);
                if (!*b) {
                    string_appendf(&out, "0 ");
                } else {
                    char *expanded_body = pp_expand_macros_for_if(ctx, body, next_expanding, depth + 1);
                    string_appendf(&out, "( %s ) ", expanded_body);
                    free(expanded_body);
                }

                list_free(next_expanding);
                free(next_expanding);
            } else {
                string_appendf(&out, "%s ", ident);
            }

            free(ident);
            p += len;
            continue;
        }

        if (p[0] == '(') {
            string_appendf(&out, "( ");
            if (expecting_defined_operand) {
                /* defined( ... ) 仍在等待 ident */
            }
            p++;
            continue;
        }
        if (p[0] == ')') {
            string_appendf(&out, ") ");
            p++;
            continue;
        }

        if ((p[0] == '|' && p[1] == '|') || (p[0] == '&' && p[1] == '&') ||
            (p[0] == '<' && p[1] == '<') || (p[0] == '>' && p[1] == '>') ||
            (p[0] == '<' && p[1] == '=') || (p[0] == '>' && p[1] == '=') ||
            (p[0] == '=' && p[1] == '=') || (p[0] == '!' && p[1] == '=')) {
            string_appendf(&out, "%c%c ", p[0], p[1]);
            p += 2;
            continue;
        }

        if (isdigit((unsigned char)*p)) {
            char *endp = NULL;
            long v = strtol(p, &endp, 0);
            /* 跳过整数后缀 L, LL, U, UL, ULL 等 */
            while (*endp && (toupper((unsigned char)*endp) == 'L' || toupper((unsigned char)*endp) == 'U'))
                endp++;
            string_appendf(&out, "%ld ", v);
            p = endp;
            continue;
        }

        string_appendf(&out, "%c ", *p);
        p++;
    }

    return string_steal(&out);
}

static long pp_eval_if_expr(PPContext *ctx, const char *args)
{
    List *expanding = make_list();
    char *expanded = pp_expand_macros_for_if(ctx, args, expanding, 0);
    list_free(expanding);
    free(expanding);

    PPExprLexer lx = {.p = expanded, .ctx = ctx};
    pp_expr_next(&lx);
    long v = pp_parse_expr(&lx);
    if (lx.cur.type != PP_TOK_END) {
        error("Trailing tokens in #if expression: '%s'", expanded);
    }
    free(expanded);
    return v;
}

static FILE *open_include_file(PPContext *ctx, const char *filename, char **fullpath)
{
    FILE *fp = NULL;
    char *resolved = NULL;
    
    fp = TTCC_fopen(filename, "r");
    if (fp) {
        resolved = TTCC_resolve_path(filename);
        *fullpath = resolved ? resolved : strdup(filename);
        return fp;
    }
    
    for (Iter i = list_iter(ctx->include_paths); !iter_end(i);) {
        char *path = iter_next(&i);
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s/%s", path, filename);
        fp = TTCC_fopen(buf, "r");
        if (fp) {
            resolved = TTCC_resolve_path(buf);
            *fullpath = resolved ? resolved : strdup(buf);
            return fp;
        }
    }
    
    return NULL;
}

bool pp_push_file(PPContext *ctx, const char *filename)
{
    char *fullpath = NULL;
    FILE *fp = open_include_file(ctx, filename, &fullpath);
    if (!fp) {
        return false;
    }

    /* #pragma once：如果该文件已标记 once，则后续 include 直接跳过 */
    if (ctx && ctx->pragma_once && fullpath && dict_get(ctx->pragma_once, fullpath)) {
        fclose(fp);
        free(fullpath);
        return true;
    }
    
    InputFile *f = malloc(sizeof(InputFile));
    f->fp = fp;
    f->filename = fullpath;
    f->line = 0;
    f->buf = malloc(PP_LINE_SIZE);
    f->buf_len = 0;
    f->next = ctx->input;
    ctx->input = f;
    
    return true;
}

void pp_pop_file(PPContext *ctx)
{
    if (!ctx->input) return;
    InputFile *f = ctx->input;
    ctx->input = f->next;
    if (f->fp) fclose(f->fp);
    free(f->filename);
    free(f->buf);
    free(f);
}

static char *read_physical_line(InputFile *f)
{
    if (!f || !f->fp) return NULL;
    if (fgets(f->buf, PP_LINE_SIZE, f->fp)) {
        f->line++;
        int len = strlen(f->buf);
        if (len > 0 && f->buf[len - 1] == '\n') {
            f->buf[len - 1] = '\0';
            len--;
        }
        // 处理 Windows 换行符 \r\n
        if (len > 0 && f->buf[len - 1] == '\r') {
            f->buf[len - 1] = '\0';
        }
        return f->buf;
    }
    return NULL;
}

/* 读取逻辑行：处理反斜杠续行（\\\n） */
static char *read_logical_line(PPContext *ctx, InputFile *f)
{
    if (!ctx || !f) return NULL;

    char *line = read_physical_line(f);
    if (!line) return NULL;

    ctx->line[0] = '\0';
    size_t cap = PP_LINE_SIZE;
    size_t used = 0;

    while (1) {
        size_t n = strlen(line);
        if (used + n + 1 > cap) {
            error("PP line too long (>%d)", PP_LINE_SIZE);
            return NULL;
        }
        memcpy(ctx->line + used, line, n);
        used += n;
        ctx->line[used] = '\0';

        if (used > 0 && ctx->line[used - 1] == '\\') {
            ctx->line[--used] = '\0';
            line = read_physical_line(f);
            if (!line) break;
            continue;
        }
        break;
    }

    return ctx->line;
}

static inline bool is_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static inline bool is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static inline char *string_steal(String *s)
{
    char *r = s->body;
    s->body = NULL;
    s->nalloc = s->len = 0;
    return r;
}

static bool list_contains_cstr(List *list, const char *s)
{
    if (!list) return false;
    for (Iter i = list_iter(list); !iter_end(i);) {
        char *v = iter_next(&i);
        if (v && strcmp(v, s) == 0) return true;
    }
    return false;
}

static char *trim_copy_range(const char *start, const char *end)
{
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    size_t n = (size_t)(end - start);
    char *r = malloc(n + 1);
    memcpy(r, start, n);
    r[n] = '\0';
    return r;
}

static bool parse_macro_call_args(const char *p, const char **out_after, List **out_args)
{
    if (*p != '(') return false;
    p++;

    List *args = make_list();
    const char *arg_start = p;
    int depth = 0;
    bool in_str = false, in_chr = false;

    while (*p) {
        char c = *p;

        if (in_str) {
            if (c == '\\' && p[1]) { p += 2; continue; }
            if (c == '"') in_str = false;
            p++;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && p[1]) { p += 2; continue; }
            if (c == '\'') in_chr = false;
            p++;
            continue;
        }

        if (c == '"') { in_str = true; p++; continue; }
        if (c == '\'') { in_chr = true; p++; continue; }

        if (c == '(') { depth++; p++; continue; }
        if (c == ')') {
            if (depth == 0) {
                char *arg = trim_copy_range(arg_start, p);
                if (!(args->len == 0 && arg[0] == '\0')) {
                    list_push(args, arg);
                } else {
                    free(arg);
                }
                p++;
                if (out_after) *out_after = p;
                if (out_args) *out_args = args;
                return true;
            }
            depth--;
            p++;
            continue;
        }
        if (c == ',' && depth == 0) {
            char *arg = trim_copy_range(arg_start, p);
            list_push(args, arg);
            p++;
            arg_start = p;
            continue;
        }

        p++;
    }

    list_free(args);
    free(args);
    return false;
}

static char *expand_macro_text(PPContext *ctx, const char *text, List *expanding, int depth);

static char *substitute_macro_params(PPContext *ctx, Macro *m, List *args, List *expanding, int depth)
{
    if (!m->params) {
        return strdup(m->body);
    }
    String out = make_string();
    const char *p = m->body;
    bool paste_next = false;
    char *va_join = NULL;
    int nfixed = 0;
    if (m->is_variadic && m->params && m->params->len > 0) {
        nfixed = (int)m->params->len - 1;
        va_join = join_args_range(args, nfixed);
    }

    while (*p) {
        if (isspace((unsigned char)*p)) {
            if (paste_next) {
                /* ##后跳过空白，继续让下一个 token 在各自分支中被正确处理 */
                while (*p && isspace((unsigned char)*p)) p++;
                string_rtrim_ws(&out);
                continue;
            }
            string_append(&out, *p);
            p++;
            continue;
        }

        if (*p == '"') {
            string_append(&out, *p++);
            while (*p) {
                char c = *p;
                string_append(&out, c);
                p++;
                if (c == '\\' && *p) {
                    string_append(&out, *p++);
                    continue;
                }
                if (c == '"') break;
            }
            continue;
        }
        if (*p == '\'') {
            string_append(&out, *p++);
            while (*p) {
                char c = *p;
                string_append(&out, c);
                p++;
                if (c == '\\' && *p) {
                    string_append(&out, *p++);
                    continue;
                }
                if (c == '\'') break;
            }
            continue;
        }

        /* token pasting */
        if (p[0] == '#' && p[1] == '#') {
            paste_next = true;
            p += 2;
            string_rtrim_ws(&out);
            continue;
        }

        /* stringification: #param */
        if (p[0] == '#') {
            const char *q = skip_space(p + 1);
            if (is_ident_start(*q)) {
                int nlen = 0;
                char *ident = pp_get_ident(q, &nlen);
                if (ident) {
                    int idx = -1;
                    if (body_param_index(m, ident, &idx)) {
                        char *raw = (char *)list_get(args, idx);
                        char *strlit = pp_stringify_text(raw);
                        if (paste_next) {
                            string_rtrim_ws(&out);
                        }
                        string_appendf(&out, "%s", strlit);
                        free(strlit);
                        free(ident);
                        p = q + nlen;
                        paste_next = false;
                        continue;
                    }
                    free(ident);
                }
            }
            if (paste_next) string_rtrim_ws(&out);
            string_append(&out, *p++);
            paste_next = false;
            continue;
        }

        if (is_ident_start(*p)) {
            int len = 0;
            char *ident = pp_get_ident(p, &len);
            if (ident) {
                int idx = -1;
                bool is_param = body_param_index(m, ident, &idx);

                const char *after_ident = p + len;
                const char *peek = skip_space(after_ident);
                bool next_is_paste = (peek[0] == '#' && peek[1] == '#');
                bool is_paste_operand = paste_next || next_is_paste;

                if (is_param) {
                    char *arg = NULL;
                    bool is_va = (m->is_variadic && strcmp(ident, "__VA_ARGS__") == 0);
                    if (is_va) arg = va_join;
                    else arg = (char *)list_get(args, idx);
                    if (!arg) arg = "";
                    if (is_paste_operand) {
                        const char *as = arg;
                        while (*as && isspace((unsigned char)*as)) as++;
                        const char *ae = as + strlen(as);
                        while (ae > as && isspace((unsigned char)*(ae - 1))) ae--;
                        if (paste_next) string_rtrim_ws(&out);

                        /* GNU 扩展：`, ##__VA_ARGS__` 在 __VA_ARGS__ 为空时消隐逗号 */
                        if (is_va && (ae == as)) {
                            string_rtrim_ws(&out);
                            if (out.len > 0 && out.body[out.len - 1] == ',') {
                                out.body[--out.len] = '\0';
                                string_rtrim_ws(&out);
                            }
                        }
                        string_appendf(&out, "%.*s", (int)(ae - as), as);
                        /* paste 右侧处理完毕，加空格以分隔后续 token */
                        if (paste_next) string_append(&out, ' ');
                    } else {
                        char *expanded_arg = expand_macro_text(ctx, arg, expanding, depth + 1);
                        if (paste_next) string_rtrim_ws(&out);
                        string_appendf(&out, "%s", expanded_arg);
                        free(expanded_arg);
                    }
                } else {
                    if (paste_next) string_rtrim_ws(&out);
                    string_appendf(&out, "%s", ident);
                    /* paste 右侧非参数（字面量），加空格分隔后续 token */
                    if (paste_next) string_append(&out, ' ');
                }
                free(ident);
                p += len;
                paste_next = false;
                continue;
            }
        }

        if (paste_next) {
            string_rtrim_ws(&out);
        }
        string_append(&out, *p);
        p++;
        /* paste 右侧处理完毕，加空格以分隔后续 token */
        if (paste_next) string_append(&out, ' ');
        paste_next = false;
    }

    if (va_join) free(va_join);
    return string_steal(&out);
}

static char *expand_macro_text(PPContext *ctx, const char *text, List *expanding, int depth)
{
    if (!text) return strdup("");
    if (depth > 64) return strdup(text);

    String out = make_string();
    const char *p = text;

    while (*p) {
        if (ctx->in_block_comment) {
            const char *q = strstr(p, "*/");
            if (!q) {
                string_appendf(&out, "%s", p);
                return string_steal(&out);
            }
            while (p < q + 2) string_append(&out, *p++);
            ctx->in_block_comment = false;
            continue;
        }

        if (p[0] == '/' && p[1] == '/') {
            string_appendf(&out, "%s", p);
            return string_steal(&out);
        }
        if (p[0] == '/' && p[1] == '*') {
            const char *q = strstr(p + 2, "*/");
            if (!q) {
                ctx->in_block_comment = true;
                string_appendf(&out, "%s", p);
                return string_steal(&out);
            }
            while (p < q + 2) string_append(&out, *p++);
            continue;
        }

        /* 字符串/字符常量：不展开 */
        if (*p == '"') {
            string_append(&out, *p++);
            while (*p) {
                char c = *p;
                string_append(&out, c);
                p++;
                if (c == '\\' && *p) {
                    string_append(&out, *p++);
                    continue;
                }
                if (c == '"') break;
            }
            continue;
        }
        if (*p == '\'') {
            string_append(&out, *p++);
            while (*p) {
                char c = *p;
                string_append(&out, c);
                p++;
                if (c == '\\' && *p) {
                    string_append(&out, *p++);
                    continue;
                }
                if (c == '\'') break;
            }
            continue;
        }

        if (is_ident_start(*p)) {
            int len = 0;
            char *ident = pp_get_ident(p, &len);
            if (ident) {
                if (is_predefined_macro(ident)) {
                    char *exp = pp_predefined_macro_expansion(ctx, ident);
                    if (exp) {
                        string_appendf(&out, "%s", exp);
                        free(exp);
                        free(ident);
                        p += len;
                        continue;
                    }
                }
                Macro *m = get_pp_macro(ctx, ident);
                if (!m || list_contains_cstr(expanding, ident)) {
                    string_appendf(&out, "%s", ident);
                    free(ident);
                    p += len;
                    continue;
                }

                if (m->type == MACRO_OBJ) {
                    List *next_expanding = clone_expanding_with(expanding, ident);

                    char *expanded_body = expand_macro_text(ctx, m->body, next_expanding, depth + 1);
                    string_appendf(&out, "%s", expanded_body);

                    free(expanded_body);
                    list_free(next_expanding);
                    free(next_expanding);
                    free(ident);
                    p += len;
                    continue;
                }

                const char *q = p + len;
                while (*q && isspace((unsigned char)*q)) q++;
                if (*q != '(') {
                    string_appendf(&out, "%s", ident);
                    free(ident);
                    p += len;
                    continue;
                }

                const char *after = NULL;
                List *args = NULL;
                if (!parse_macro_call_args(q, &after, &args)) {
                    string_appendf(&out, "%s", ident);
                    free(ident);
                    p += len;
                    continue;
                }

                int nparams = m->params ? m->params->len : 0;
                int nargs = args ? args->len : 0;
                if (!m->is_variadic) {
                    if (nparams != nargs) {
                        while (p < after) string_append(&out, *p++);
                        free(ident);
                        list_free(args);
                        free(args);
                        continue;
                    }
                } else {
                    int nfixed = nparams > 0 ? (nparams - 1) : 0;
                    if (nargs < nfixed) {
                        while (p < after) string_append(&out, *p++);
                        free(ident);
                        list_free(args);
                        free(args);
                        continue;
                    }
                }

                List *next_expanding = clone_expanding_with(expanding, ident);

                char *substed = substitute_macro_params(ctx, m, args, next_expanding, depth + 1);
                char *expanded = expand_macro_text(ctx, substed, next_expanding, depth + 1);
                
                /* rescan：如果 expanded 以函数宏名结尾，且 after 以 '(' 开头，
                 * 则将 expanded 末尾的宏名与 after 一起作为函数宏调用处理。
                 * 这支持 CAT(A,B)(x) → AB(x) 这类 paste 后继续调用的情形。 */
                const char *exp_end = expanded + strlen(expanded);
                /* 去掉 expanded 末尾空白，找到末尾 ident */
                const char *eid = exp_end;
                while (eid > expanded && isspace((unsigned char)*(eid - 1))) eid--;
                const char *eid_start = eid;
                while (eid_start > expanded && is_ident_char(*(eid_start - 1))) eid_start--;
                if (eid_start < eid && is_ident_start(*eid_start)) {
                    /* expanded 以 ident 结尾 */
                    int tail_len = (int)(eid - eid_start);
                    char *tail_name = malloc(tail_len + 1);
                    memcpy(tail_name, eid_start, tail_len);
                    tail_name[tail_len] = '\0';
                    const char *aafter = after;
                    while (*aafter && isspace((unsigned char)*aafter)) aafter++;
                    Macro *tail_m = get_pp_macro(ctx, tail_name);
                    if (tail_m && tail_m->type == MACRO_FUNC && *aafter == '(' &&
                        !list_contains_cstr(expanding, tail_name)) {
                        /* 把 expanded 前缀（不含末尾宏名）追加，然后处理 tail_name(args) */
                        string_appendf(&out, "%.*s", (int)(eid_start - expanded), expanded);
                        /* 将 tail_name 和 after 合并后一起展开；
                         * rescan 时使用空 expanding，允许所有宏展开（含 CAT 等），
                         * 通过 depth 限制防止无限递归 */
                        size_t tnlen = tail_len;
                        size_t arlen = strlen(after);
                        char *tail_combined = malloc(tnlen + arlen + 1);
                        memcpy(tail_combined, tail_name, tnlen);
                        memcpy(tail_combined + tnlen, after, arlen + 1);
                        List *rescan_exp = make_list();
                        char *tail_expanded = expand_macro_text(ctx, tail_combined, rescan_exp, depth + 1);
                        free(tail_combined);
                        list_free(rescan_exp); free(rescan_exp);
                        string_appendf(&out, "%s", tail_expanded);
                        free(tail_expanded);
                        free(tail_name);
                        free(expanded); free(substed);
                        list_free(next_expanding); free(next_expanding);
                        list_free(args); free(args);
                        free(ident);
                        p = after + arlen;
                        continue;
                    }
                    free(tail_name);
                }
                string_appendf(&out, "%s", expanded);

                free(expanded);
                free(substed);
                list_free(next_expanding);
                free(next_expanding);
                list_free(args);
                free(args);
                free(ident);

                p = after;
                continue;
            }
        }

        string_append(&out, *p);
        p++;
    }

    return string_steal(&out);
}

static char *expand_macro_simple(PPContext *ctx, const char *line)
{
    List *expanding = make_list();
    char *r = expand_macro_text(ctx, line, expanding, 0);
    list_free(expanding);
    free(expanding);
    return r;
}

static void get_dirname(const char *path, char *dir, size_t dir_size)
{
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        strncpy(dir, ".", dir_size);
        dir[dir_size - 1] = '\0';
        return;
    }
    
    size_t len = last_slash - path;
    if (len >= dir_size) len = dir_size - 1;
    strncpy(dir, path, len);
    dir[len] = '\0';
}

static FILE *open_include_file_with_dir(PPContext *ctx, const char *filename,
                                        const char *extra_dir, char **fullpath)
{
    if (!ctx) return NULL;

    FILE *fp = fopen(filename, "r");
    if (fp) {
        *fullpath = strdup(filename);
        return fp;
    }

    if (extra_dir && extra_dir[0]) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s/%s", extra_dir, filename);
        fp = fopen(buf, "r");
        if (fp) {
            *fullpath = strdup(buf);
            return fp;
        }
    }

    return open_include_file(ctx, filename, fullpath);
}

static bool handle_include(PPContext *ctx, const char *args)
{
    const char *p = skip_space(args);
    char filename[512];
    char current_dir[512] = "";
    
    if (ctx->input && ctx->input->filename) {
        get_dirname(ctx->input->filename, current_dir, sizeof(current_dir));
    }
    
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < sizeof(filename) - 1) {
            filename[i++] = *p++;
        }
        filename[i] = '\0';
        
        char *fullpath = NULL;
        FILE *fp = open_include_file_with_dir(ctx, filename, current_dir, &fullpath);
        if (!fp) {
            error("Cannot open include file: %s", filename);
            return false;
        }

        /* #pragma once：如果该文件已标记 once，则后续 include 直接跳过 */
        if (ctx && ctx->pragma_once && fullpath && dict_get(ctx->pragma_once, fullpath)) {
            fclose(fp);
            free(fullpath);
            return true;
        }
        
        InputFile *f = malloc(sizeof(InputFile));
        f->fp = fp;
        f->filename = fullpath;
        f->line = 0;
        f->buf = malloc(PP_LINE_SIZE);
        f->buf_len = 0;
        f->next = ctx->input;
        ctx->input = f;
        
    } else if (*p == '<') {
        p++;
        int i = 0;
        while (*p && *p != '>' && i < sizeof(filename) - 1) {
            filename[i++] = *p++;
        }
        filename[i] = '\0';
        
        if (!pp_push_file(ctx, filename)) {
            error("Cannot open include file: <%s>", filename);
            return false;
        }
    } else {
        error("Invalid #include syntax");
        return false;
    }
    
    return true;
}

static char *pp_clean_define_body(const char *body)
{
    if (!body) return strdup("");
    String out = make_string();
    bool in_str = false;
    bool in_chr = false;
    bool esc = false;
    for (const char *p = body; *p; ++p) {
        char c = *p;
        char n = p[1];
        if (!in_str && !in_chr) {
            if (c == '/' && n == '/') break;
            if (c == '/' && n == '*') {
                p += 2;
                while (*p && !(p[0] == '*' && p[1] == '/')) p++;
                if (!*p) break;
                p++;
                continue;
            }
            if (c == '"') in_str = true;
            else if (c == '\'') in_chr = true;
        } else {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (in_str && c == '"') {
                in_str = false;
            } else if (in_chr && c == '\'') {
                in_chr = false;
            }
        }
        string_appendf(&out, "%c", c);
    }
    {
        char *s = string_steal(&out);
        int len = (int)strlen(s);
        while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
        return s;
    }
}

static bool handle_define(PPContext *ctx, const char *args)
{
    const char *p = skip_space(args);
    int len;
    char *name = pp_get_ident(p, &len);
    if (!name) {
        error("Expected identifier in #define");
        return false;
    }

    if (is_predefined_macro(name)) {
        error("Cannot redefine predefined macro: %s", name);
        free(name);
        return false;
    }
    p += len;
    
    if (*p == '(') {
        List *params = make_list();
        bool is_variadic = false;
        p++;
        while (1) {
            p = skip_space(p);
            if (*p == ')') {
                p++;
                break;
            }

            if (p[0] == '.' && p[1] == '.' && p[2] == '.') {
                /* C99 variadic macro: ... 作为最后一个形参 */
                is_variadic = true;
                list_push(params, strdup("__VA_ARGS__"));
                p += 3;
                p = skip_space(p);
                if (*p != ')') {
                    error("'...' must be the last macro parameter");
                    return false;
                }
                p++;
                break;
            }

            char *param = pp_get_ident(p, &len);
            if (!param) {
                error("Expected parameter name");
                return false;
            }
            list_push(params, param);
            p += len;
            p = skip_space(p);
            if (*p == ',') {
                p++;
            } else if (*p != ')') {
                error("Expected ',' or ')'");
                return false;
            }
        }
        const char *body = skip_space(p);
        char *clean_body = pp_clean_define_body(body);
        pp_define_func(ctx, name, params, is_variadic, clean_body);
        free(clean_body);
    } else {
        const char *body = skip_space(p);
        char *clean_body = pp_clean_define_body(body);
        pp_define(ctx, name, clean_body);
        free(clean_body);
    }
    
    free(name);
    return true;
}

static bool handle_undef(PPContext *ctx, const char *args)
{
    const char *p = skip_space(args);
    int len;
    char *name = pp_get_ident(p, &len);
    if (!name) {
        error("Expected identifier in #undef");
        return false;
    }

    if (is_predefined_macro(name)) {
        error("Cannot undefine predefined macro: %s", name);
        free(name);
        return false;
    }
    pp_undef(ctx, name);
    free(name);
    return true;
}

static bool handle_error_warning(PPContext *ctx, const char *args, bool is_error)
{
    const char *msg = skip_space(args);
    const char *file = pp_current_file(ctx);
    int line = pp_current_line(ctx);

    if (!msg || msg[0] == '\0') msg = is_error ? "#error" : "#warning";
    fprintf(stderr, "%s:%d: %s: %s\n", file ? file : "(null)", line, is_error ? "error" : "warning", msg);
    if (is_error) exit(1);
    return true;
}

static bool handle_pragma(PPContext *ctx, const char *args)
{
    const char *p = skip_space(args);
    if (!*p) return true;

    int len = 0;
    char *kind = pp_get_ident(p, &len);
    if (!kind) {
        /* 不是标识符的 pragma：忽略 */
        return true;
    }
    p += len;
    p = skip_space(p);

    if (strcmp(kind, "once") == 0) {
        const char *cur = pp_current_file(ctx);
        if (cur && ctx && ctx->pragma_once && !dict_get(ctx->pragma_once, (char *)cur)) {
            dict_put(ctx->pragma_once, strdup(cur), (void *)1);
        }
        free(kind);
        return true;
    }

    if (strcmp(kind, "message") == 0) {
        const char *msg = p;
        msg = skip_space(msg);
        /* 兼容 #pragma message("...") */
        if (msg[0] == '(') {
            msg++;
            msg = skip_space(msg);
            const char *end = msg + strlen(msg);
            while (end > msg && isspace((unsigned char)*(end - 1))) end--;
            if (end > msg && *(end - 1) == ')') end--;

            char *tmp = trim_copy_range(msg, end);
            const char *file = pp_current_file(ctx);
            int line = pp_current_line(ctx);
            fprintf(stderr, "%s:%d: note: %s\n", file ? file : "(null)", line, tmp);
            free(tmp);
        } else {
            const char *file = pp_current_file(ctx);
            int line = pp_current_line(ctx);
            fprintf(stderr, "%s:%d: note: %s\n", file ? file : "(null)", line, msg && *msg ? msg : "#pragma message");
        }

        free(kind);
        return true;
    }

    /* 其他 pragma 暂时忽略（后续可扩展） */
    free(kind);
    return true;
}

static bool handle_ifdef(PPContext *ctx, const char *args, bool is_ifndef)
{
    const char *p = skip_space(args);
    int len;
    char *name = pp_get_ident(p, &len);
    if (!name) {
        error("Expected identifier in #ifdef/#ifndef");
        return false;
    }
    bool defined = is_pp_macro(ctx, name);
    bool active = is_ifndef ? !defined : defined;
    cond_push(ctx, active && !should_skip(ctx));
    free(name);
    return true;
}

static bool handle_if(PPContext *ctx, const char *args)
{
    long v = pp_eval_if_expr(ctx, args);
    bool active = (v != 0);
    cond_push(ctx, active && !should_skip(ctx));
    return true;
}

static bool handle_elif(PPContext *ctx, const char *args)
{
    if (!ctx->cond_stack) {
        error("#elif without #if");
        return false;
    }

    CondState *s = ctx->cond_stack;
    if (s->taken) {
        s->active = false;
        return true;
    }

    if (should_skip_parent(ctx)) {
        s->active = false;
        return true;
    }

    long v = pp_eval_if_expr(ctx, args);
    s->active = (v != 0);
    if (s->active) s->taken = true;
    return true;
}

static bool handle_else(PPContext *ctx, const char *args)
{
    if (!ctx->cond_stack) {
        error("#else without #if");
        return false;
    }
    cond_else(ctx);
    /* 外层不激活时，#else 也不能激活 */
    if (should_skip_parent(ctx)) {
        ctx->cond_stack->active = false;
    }
    return true;
}

static bool handle_endif(PPContext *ctx, const char *args)
{
    if (!ctx->cond_stack) {
        error("#endif without #if");
        return false;
    }
    cond_pop(ctx);
    return true;
}

static bool handle_directive(PPContext *ctx, const char *line)
{
    const char *p = skip_space(line);
    if (*p != '#') return false;
    p++;
    p = skip_space(p);
    
    int len;
    char *directive = pp_get_ident(p, &len);
    if (!directive) return false;
    p += len;
    const char *args = skip_space(p);
    
    if (strcmp(directive, "include") == 0) {
        if (!should_skip(ctx)) {
            handle_include(ctx, args);
        }
    } else if (strcmp(directive, "define") == 0) {
        if (!should_skip(ctx)) {
            handle_define(ctx, args);
        }
    } else if (strcmp(directive, "undef") == 0) {
        if (!should_skip(ctx)) {
            handle_undef(ctx, args);
        }
    } else if (strcmp(directive, "error") == 0) {
        if (!should_skip(ctx)) {
            handle_error_warning(ctx, args, true);
        }
    } else if (strcmp(directive, "warning") == 0) {
        if (!should_skip(ctx)) {
            handle_error_warning(ctx, args, false);
        }
    } else if (strcmp(directive, "pragma") == 0) {
        if (!should_skip(ctx)) {
            handle_pragma(ctx, args);
        }
    } else if (strcmp(directive, "if") == 0) {
        handle_if(ctx, args);
    } else if (strcmp(directive, "elif") == 0) {
        handle_elif(ctx, args);
    } else if (strcmp(directive, "ifdef") == 0) {
        handle_ifdef(ctx, args, false);
    } else if (strcmp(directive, "ifndef") == 0) {
        handle_ifdef(ctx, args, true);
    } else if (strcmp(directive, "else") == 0) {
        handle_else(ctx, args);
    } else if (strcmp(directive, "endif") == 0) {
        handle_endif(ctx, args);
    } else if (strcmp(directive, "line") == 0) {
        if (!should_skip(ctx)) {
            /* #line 指令：先展开宏，再解析行号（和可选文件名） */
            char *expanded = expand_macro_simple(ctx, args);
            const char *ep = skip_space(expanded);
            char *endp;
            long lineno = strtol(ep, &endp, 10);
            if (endp > ep && lineno > 0) {
                /* #line N 表示接下来的一行是第 N 行，
                 * 由于 read_physical_line 每次读行后 line++，
                 * 设置 line = N-1，这样读下一行后 line = N */
                if (ctx->input) ctx->input->line = (int)lineno - 1;
                ep = skip_space(endp);
                if (*ep == '"') {
                    /* 可选文件名 */
                    ep++;
                    const char *fe = strchr(ep, '"');
                    if (fe) {
                        char *fname = malloc(fe - ep + 1);
                        memcpy(fname, ep, fe - ep);
                        fname[fe - ep] = '\0';
                        if (ctx->input) ctx->input->filename = fname;
                        else free(fname);
                    }
                }
            }
            free(expanded);
        }
    } else {
    }
    
    free(directive);
    return true;
}

char *pp_read_line(PPContext *ctx)
{
    if (ctx->last_line) {
        free(ctx->last_line);
        ctx->last_line = NULL;
    }

    while (ctx->input) {
        char *line = read_logical_line(ctx, ctx->input);
        if (!line) {
            pp_pop_file(ctx);
            continue;
        }
        
        const char *p = skip_space(line);
        if (*p == '#') {
            handle_directive(ctx, line);
            ctx->last_line = strdup("");
            return ctx->last_line;
        }
        
        if (should_skip(ctx)) {
            ctx->last_line = strdup("");
            return ctx->last_line;
        }
        
        ctx->last_line = expand_macro_simple(ctx, line);
        return ctx->last_line;
    }
    
    return NULL;
}

bool pp_preprocess_file(PPContext *ctx, const char *filename)
{
    if (!pp_push_file(ctx, filename)) {
        fprintf(stderr, "Cannot open file: %s\n", filename);
        return false;
    }
    
    char *line;
    while ((line = pp_read_line(ctx)) != NULL) {
        if (line[0] != '\0') {
            printf("%s\n", line);
        }
    }
    
    return true;
}

const char *pp_current_file(PPContext *ctx)
{
    if (ctx->input) return ctx->input->filename;
    return NULL;
}

int pp_current_line(PPContext *ctx)
{
    if (ctx->input) return ctx->input->line;
    return 0;
}

static PPContext *g_pp = NULL;

void pp_global_init(void)
{
    g_pp = pp_init();
}

void pp_global_free(void)
{
    if (g_pp) {
        pp_free(g_pp);
        g_pp = NULL;
    }
}

bool pp_global_push_file(const char *filename)
{
    if (!g_pp) pp_global_init();
    return pp_push_file(g_pp, filename);
}

void pp_global_add_include_path(const char *path)
{
    if (!g_pp) pp_global_init();
    list_push(g_pp->include_paths, strdup(path));
}

void pp_global_define(const char *name, const char *body)
{
    if (!g_pp) pp_global_init();
    pp_define(g_pp, name, body ? body : "1");
}

char *pp_global_read_line(void)
{
    if (!g_pp) return NULL;
    return pp_read_line(g_pp);
}

const char *pp_global_current_file(void)
{
    if (!g_pp) return NULL;
    return pp_current_file(g_pp);
}

int pp_global_current_line(void)
{
    if (!g_pp) return 0;
    return pp_current_line(g_pp);
}

/* 高级接口：预处理文件并重定向到stdin */
bool pp_preprocess_to_stdin(const char *filename)
{
    FILE *tmp = pp_tmpfile();
    if (!tmp) return false;
    
    if (!g_pp) pp_global_init();
    if (!pp_global_push_file(filename)) {
        fclose(tmp);
        pp_global_free();
        return false;
    }
    
    char *line;
    char *last_file = NULL;
    int last_line = 0;
    while ((line = pp_global_read_line()) != NULL) {
        const char *cur_file = pp_global_current_file();
        int cur_line = pp_global_current_line();
        if (cur_file) {
            if (!last_file || strcmp(cur_file, last_file) != 0 || cur_line != last_line + 1) {
                fprintf(tmp, "#line %d \"%s\"\n", cur_line, cur_file);
            }
            if (last_file) free(last_file);
            last_file = strdup(cur_file);
            last_line = cur_line;
        }
        fprintf(tmp, "%s\n", line);
    }
    if (last_file) free(last_file);
    
    fflush(tmp);
    rewind(tmp);

    /* 释放当前文件的预处理上下文，避免宏/头文件守卫泄漏到下一个文件 */
    pp_global_free();

    return _dup2(_fileno(tmp), _fileno(stdin)) == 0;
}

/* 清理预处理临时文件（预留接口） */
void pp_cleanup_temp(void)
{
    (void)0;
}

