#include <ctype.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cc.h"

/* 全局目标平台（platform.h 中声明，此处定义） */
TargetPlatform g_target_platform = PLAT_HOST;

#define MAX_ARGS 16
#define MAX_OP_PRIO 16
#define MAX_ALIGN 16

List *ctypes = &EMPTY_LIST;
List *strings = &EMPTY_LIST;
List *flonums = &EMPTY_LIST;

static Dict *globalenv = &EMPTY_DICT;
static Dict *localenv = NULL;
static Dict *struct_defs = &EMPTY_DICT;
static Dict *union_defs = &EMPTY_DICT;
static Dict *enum_defs = &EMPTY_DICT;
static Dict *functionenv = &EMPTY_DICT;
static Dict *typedefenv = &EMPTY_DICT;
static List *localvars = NULL;
static List *labels = NULL;
static bool in_cond_expr = false;  
static Dict *parser_empty_dict = &EMPTY_DICT;

typedef struct SwitchParseCtx {
    Dict *seen;
    List *cases;
    char *default_label;
    struct SwitchParseCtx *parent;
} SwitchParseCtx;

static SwitchParseCtx *switch_ctx = NULL;

/* 全局 compound literal 的 decl 节点：在表达式解析阶段创建，但需要在顶层输出 */
static List *pending_toplevel_decls = NULL;

/* 局部 compound literal 的 decl 节点：在表达式解析阶段创建，需要在最近的外层复合语句注入 */
static List *pending_local_decls = NULL;

static Ctype *ctype_void = &(Ctype){0, CTYPE_VOID, 0, NULL};
static Ctype *ctype_int = &(Ctype){0, CTYPE_INT, 4, NULL};
static Ctype *ctype_short = &(Ctype){0, CTYPE_INT, 2, NULL}; 
static Ctype *ctype_long = &(Ctype){0, CTYPE_LONG, 8, NULL};
static Ctype *ctype_bool = &(Ctype){0, CTYPE_BOOL, 1, NULL};
static Ctype *ctype_char = &(Ctype){0, CTYPE_CHAR, 1, NULL};
static Ctype *ctype_float = &(Ctype){0, CTYPE_FLOAT, 4, NULL};
static Ctype *ctype_double = &(Ctype){0, CTYPE_DOUBLE, 8, NULL};

/* MCS51 目标模式：按照 Keil C51 规范设置整型宽度 */
static int g_ptr_size = 4; /* 默认宿主机 32/64bit 指针 */

/* C51 内部声明类型跟踪（C51DeclKind 定义于 platform.h） */
static C51DeclKind g_last_c51_decl_kind = C51_DECL_NONE;

void parser_set_target_mcs51(void) {
    g_target_platform     = PLAT_MCS51;
    ctype_int->size   = 2; /* int  = 16bit（Keil C51 默认）*/
    ctype_short->size = 2; /* short = 16bit（同 int）*/
    ctype_long->size  = 4; /* long = 32bit */
    g_ptr_size        = 2; /* C51 指针 = 16bit（64KB 地址空间）*/
}

static int labelseq = 0;

static Ast *read_expr(void);
static Ast *read_comma_expr(void);
static Ast *read_expr_int(int prec);
static Ctype *make_ptr_type(Ctype *ctype);
static Ctype *make_array_type(Ctype *ctype, int size);
static Ast *read_compound_stmt(void);
static Ast *read_decl_or_stmt(void);
static Ctype *result_type(int op, Ctype *a, Ctype *b);
static Ctype *convert_array(Ctype *ctype);
static Ast *read_stmt(void);
static Ast *read_postfix_expr_tail(Ast *ast);
static Ctype *clone_ctype_with_attr(Ctype *ctype, int attr);
static void expect(char punct);
static Ctype *read_decl_int(Token *name);
static int read_decl_ctype_attr(Token tok, int *attr_out);
static bool have_redefine_var(char* var_name);
static bool get_enum_val(char* key, int* val);
static bool is_type_keyword(const Token tok);
static Ctype *read_decl_spec(void);
static Ctype *read_array_dimensions(Ctype *basetype);
static void read_func_ptr_params(void);
static Ast *read_switch_case_stmt(void);
static Ast *read_switch_default_stmt(void);

void parser_reset(void)
{
    globalenv = make_dict(parser_empty_dict);
    struct_defs = make_dict(parser_empty_dict);
    union_defs = make_dict(parser_empty_dict);
    enum_defs = make_dict(parser_empty_dict);
    functionenv = make_dict(parser_empty_dict);
    typedefenv = make_dict(parser_empty_dict);

    localenv = NULL;
    localvars = NULL;
    labels = NULL;
    switch_ctx = NULL;
    pending_toplevel_decls = make_list();
    pending_local_decls = make_list();

    ctypes = make_list();
    strings = make_list();
    flonums = make_list();

    labelseq = 0;
    in_cond_expr = false;
}

static Ast *ast_uop(int type, Ctype *ctype, Ast *operand)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = type;
    r->ctype = ctype;
    r->operand = operand;
    return r;
}

static Ast *ast_binop(int type, Ast *left, Ast *right)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = type;
    r->ctype = result_type(type, left->ctype, right->ctype);
    if (type != '=' && convert_array(left->ctype)->type != CTYPE_PTR &&
        convert_array(right->ctype)->type == CTYPE_PTR) {
        r->left = right;
        r->right = left;
    } else {
        r->left = left;
        r->right = right;
    }
    return r;
}

static Ast *ast_inttype(Ctype *ctype, long long val)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_LITERAL;
    r->ctype = ctype;
    r->ival = val;
    return r;
}

static Ast *ast_double(double val)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_LITERAL;
    r->ctype = ctype_double;
    r->fval = val;
    list_push(flonums, r);
    return r;
}

char *make_label(void)
{
    String s = make_string();
    string_appendf(&s, "L%d", labelseq++);
    return get_cstring(s);
}

static bool dict_has_current(Dict *dict, char *key)
{
    if (!dict || !dict->list) return false;
    for (Iter i = list_iter(dict->list); !iter_end(i);) {
        DictEntry *e = iter_next(&i);
        if (e && e->key && !strcmp(key, e->key))
            return true;
    }
    return false;
}

static Ast *ast_lvar(Ctype *ctype, char *name)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_LVAR;
    r->ctype = ctype;
    r->varname = name;
    dict_put(localenv, name, r);
    if (localvars)
        list_push(localvars, r);
    return r;
}

static Ast *ast_gvar(Ctype *ctype, char *name, bool filelocal)
{
    // 支持 tentative definition：允许同一全局变量多次声明，最后一次带初始值的生效
    if (globalenv) {
        Ast *existing = dict_get(globalenv, name);
        if (existing) {
            // 允许重复声明（tentative definition）：返回已有节点
            // 调用方若有初始值会通过 ginit 字段更新
            return existing;
        }
    }
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_GVAR;
    r->ctype = ctype;
    r->varname = name;
    r->glabel = filelocal ? make_label() : name;
    dict_put(globalenv, name, r);
    return r;
}

static Ast *ast_string(char *str)
{
    Ast *r = malloc(sizeof(Ast));
    union { CtypeAttr c; int i; } a; a.i = 0;
    a.c.ctype_const = 1;
    Ctype *code_char = clone_ctype_with_attr(ctype_char, a.i);
    r->type = AST_STRING;
    r->ctype = make_array_type(code_char, strlen(str) + 1);
    r->sval = str;
    r->slabel = make_label();
    return r;
}

static Ast *ast_funcall(Ctype *ctype, char *fname, List *args, Ast *fnexpr)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_FUNCALL;
    r->ctype = ctype;
    r->fname = fname;
    r->args = args;
    r->fnexpr = fnexpr;
    return r;
}

static Ast *ast_typedef(Ctype *ctype, char *typename) 
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_TYPE_DEF;
    r->ctype = ctype;
    r->typename = typename;
    return r;
}

static Ast *ast_func_def(Ctype *rettype,
                     char *fname,
                     List *params,
                     Ast *body,
                     List *localvars, 
                     List *labels)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_FUNC_DEF;
    r->ctype = rettype;
    r->fname = fname;
    r->params = params;
    r->localvars = localvars;
    r->labels = labels;
    r->body = body;
    r->platform_info.mcs51.interrupt_no = -1;
    r->platform_info.mcs51.using_no = -1;
    return r;
}

static Ast *ast_func_decl(Ctype *rettype, char *fname, List *params)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_FUNC_DECL;
    r->ctype = rettype;
    r->fname = fname;
    r->params = params;
    r->localvars = NULL;
    r->labels = NULL;
    r->body = NULL;
    r->platform_info.mcs51.interrupt_no = -1;
    r->platform_info.mcs51.using_no = -1;
    return r;
}
                    

static bool valid_init_var(Ast *var, Ast *init)
{
    // NOTE: 未初始化, 则不判断
    if(!init) return true;
    /* 允许外层是 cast 的初始化（如 (int)&a 或 (void*)(int)0x4000） */
    if (init->type == AST_CAST) {
        if (is_inttype(var->ctype) && is_inttype(init->ctype)) return true;
        if (var->ctype->type == CTYPE_PTR) {
            if (init->ctype && (init->ctype->type == CTYPE_PTR || init->ctype->type == CTYPE_ARRAY)) return true;
            if (init->cast_expr && init->cast_expr->type == AST_ADDR) return true;
        }
    }
    if(init->type == AST_CAST) init = init->cast_expr;
    switch(var->ctype->type) {
        case CTYPE_BOOL ... CTYPE_DOUBLE:
            return (init->ctype->type <= CTYPE_DOUBLE);
        case CTYPE_ARRAY:
            // 支持数组初始化列表和字符串初始化字符数组
            if (init->type == AST_ARRAY_INIT) return true;
            if (init->type == AST_STRING && var->ctype->ptr->type == CTYPE_CHAR) return true;
            return false;
        case CTYPE_PTR:
            // 函数指针可以接受：其他指针、数组、函数声明、整数字面量、取地址表达式
            if (init->ctype->type == CTYPE_PTR || init->ctype->type == CTYPE_ARRAY ||
                init->ctype->type == CTYPE_INT || init->type == AST_ADDR)
                return true;
            // 函数名赋值给函数指针（AST_FUNC_DECL/AST_FUNC_DEF的ctype是返回类型）
            if (init->type == AST_FUNC_DECL || init->type == AST_FUNC_DEF)
                return true;
            return false;
        case CTYPE_STRUCT:
            return (init->ctype->type == CTYPE_STRUCT);
        case CTYPE_ENUM:
              return (init->ctype->type == CTYPE_ENUM || init->ctype->type == CTYPE_INT);
        default: return false;
    }
}

static Ast *ast_decl(Ast *var, Ast *init)
{
    if(!valid_init_var(var, init))
        error("Invalid var init: (%s) -> (%s)\n", ctype_to_string(init->ctype), ctype_to_string(var->ctype));

    Ast *r = malloc(sizeof(Ast));
    r->type = AST_DECL;
    r->ctype = NULL;
    r->declvar = var;
    r->declinit = init;
    return r;
}

static Ast *ast_array_init(List *arrayinit)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_ARRAY_INIT;
    r->ctype = NULL;
    r->arrayinit = arrayinit;
    return r;
}

static Ast *ast_struct_init(Ctype *ctype, List *structinit) {
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_STRUCT_INIT;
    r->ctype = ctype;
    r->structinit = structinit;
    return r;
}

static Ast *ast_if(Ast *cond, Ast *then, Ast *els)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_IF;
    r->ctype = NULL;
    r->cond = cond;
    r->then = then;
    r->els = els;
    return r;
}

static Ast *ast_ternary(Ctype *ctype, Ast *cond, Ast *then, Ast *els)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_TERNARY;
    r->ctype = ctype;
    r->cond = cond;
    r->then = then;
    r->els = els;
    return r;
}

static Ast *ast_switch(Ast *ctrl, List *cases, Ast *body, char *default_label)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_SWITCH;
    r->ctype = NULL;
    r->ctrl = ctrl;
    r->cases = cases;
    r->default_stmt = NULL;
    r->switch_body = body;
    r->default_label = default_label;
    return r;
}

static SwitchCase *make_switch_case(long low, long high, Ast *stmt, char *label)
{
    SwitchCase *c = malloc(sizeof(SwitchCase));
    c->low  = low;
    c->high  = high;
    c->stmt = stmt;
    c->label = label;
    return c;
}

static Ast *ast_for(Ast *init, Ast *cond, Ast *step, Ast *body)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_FOR;
    r->ctype = NULL;
    r->forinit = init;
    r->forcond = cond;
    r->forstep = step;
    r->forbody = body;
    return r;
}

static Ast *ast_while(Ast *cond, Ast *body)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_WHILE;
    r->ctype = NULL;
    r->while_cond = cond;
    r->while_body = body;
    return r;
}

static Ast *ast_dowhile(Ast *cond, Ast *body)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_DO_WHILE;
    r->ctype = NULL;
    r->while_cond = cond;
    r->while_body = body;
    return r;
}

static Ast *ast_goto(char* label)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_GOTO;
    r->ctype = NULL;
    r->label = label;
    return r;
}

static Ast *ast_label(char* label)
{
    for (Iter i = list_iter(labels); !iter_end(i);) {
        char* v = iter_next(&i);
        if(!strcmp(v, label)) error("duplicate label: %s", label);
    }

    Ast *r = malloc(sizeof(Ast));
    r->type = AST_LABEL;
    r->ctype = NULL;
    r->label = strdup(label);
    list_push(labels, r->label);
    return r;
}

Ast *ast_break(void)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_BREAK;
    r->ctype = NULL;
    return r;
}

Ast *ast_continue(void)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_CONTINUE;
    r->ctype = NULL;
    return r;
}

static Ast *ast_return(Ast *retval)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_RETURN;
    r->ctype = NULL;
    r->retval = retval;
    return r;
}

static Ast *ast_asm(char *asm_body)
{
    Ast *r = malloc(sizeof(Ast));
    memset(r, 0, sizeof(Ast));
    r->type = AST_ASM;
    r->asm_body = asm_body;
    return r;
}

static Ast *ast_static_assert(Ast *cond, char *msg)
{
    Ast *r = malloc(sizeof(Ast));
    memset(r, 0, sizeof(Ast));
    r->type = AST_STATIC_ASSERT;
    r->cond = cond;
    /* 注意：msg 不能存 sval（与 cond 在 union 中重叠），改用 then 字段（offset 8）存储 */
    r->els = (Ast*)msg;
    return r;
}

static Ast *ast_compound_stmt(List *stmts)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_COMPOUND_STMT;
    r->ctype = NULL;
    r->stmts = stmts;
    return r;
}

static Ast *ast_struct_ref(Ctype *ctype, Ast *struc, char *name)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_STRUCT_REF;
    r->ctype = ctype;
    r->struc = struc;
    r->field = name;
    return r;
}

static Ast *ast_struct_def(Ctype *ctype) {
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_STRUCT_DEF;
    r->ctype = ctype;
    return r;
};

static Ast *ast_enum_def(Ctype *ctype) {
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_ENUM_DEF;
    r->ctype = ctype;
    return r;
};

static Ast *ast_cast(Ctype *target, Ast *expr)
{
    Ast *r = malloc(sizeof(Ast));
    r->type = AST_CAST;
    r->ctype = target;
    r->cast_expr = expr;
    return r;
}

static Ctype *make_ptr_type(Ctype *ctype)
{
    Ctype *r = malloc(sizeof(Ctype));
    r->type = CTYPE_PTR;
    r->ptr = ctype;
    r->size = g_ptr_size; /* 指针大小由目标平台决定（默认4，MCS51为2）*/
    r->attr = ctype->attr;
    r->bit_offset = 0;
    r->bit_size = 0;
    list_push(ctypes, r);
    return r;
}

static Ctype *clone_ctype_with_attr(Ctype *ctype, int attr)
{
    Ctype *r = malloc(sizeof(Ctype));
    memcpy(r, ctype, sizeof(Ctype));
    /* 继承原始类型中的 unsigned 属性（通过 typedef 传递），但不继承 typedef/static/extern 等限定 */
    union { CtypeAttr c_attr; int i_attr; } old_a = {0};
    old_a.i_attr = ctype->attr;
    union { CtypeAttr c_attr; int i_attr; } new_a = {0};
    new_a.i_attr = attr;
    if (old_a.c_attr.ctype_unsigned) new_a.c_attr.ctype_unsigned = 1;
    r->attr = new_a.i_attr;
    list_push(ctypes, r);
    return r;
}

static Ctype *make_array_type(Ctype *ctype, int len)
{
    Ctype *r = malloc(sizeof(Ctype));
    r->type = CTYPE_ARRAY;
    r->ptr = ctype;
    r->attr = ctype->attr;
    r->bit_offset = 0;
    r->bit_size = 0;
    r->size = (len < 0) ? -1 : ctype->size * len;
    r->len = len;
    list_push(ctypes, r);
    return r;
}

static Ctype *make_struct_field_type(Ctype *ctype, int offset, int bit_offset, int bit_size)
{
    Ctype *r = malloc(sizeof(Ctype));
    memcpy(r, ctype, sizeof(Ctype));
    r->offset = offset;
    r->bit_offset = bit_offset;
    r->bit_size = bit_size;
    list_push(ctypes, r);
    return r;
}

static Ctype *make_struct_type(Dict *fields, int size, bool is_union)
{
    Ctype *r = malloc(sizeof(Ctype));
    r->type = CTYPE_STRUCT;
    r->fields = fields;
    r->size = size;
    r->bit_offset = 0;
    r->bit_size = 0;
    r->is_union = is_union;
    r->tag = NULL;
    r->attr = 0;
    r->ptr = NULL;
    r->len = 0;
    r->offset = 0;
    list_push(ctypes, r);
    return r;
}

static Ctype *make_enum_type(Dict *fields)
{
    Ctype *r = malloc(sizeof(Ctype));
    r->type = CTYPE_ENUM;
    r->fields = fields;
    r->tag = NULL;
    r->attr = 0;
    r->ptr = NULL;
    r->size = 0;
    r->len = 0;
    r->offset = 0;
    r->bit_offset = 0;
    r->bit_size = 0;
    r->is_union = false;
    list_push(ctypes, r);
    return r;
}

bool is_inttype(Ctype *ctype)
{
    return ctype->type == CTYPE_BOOL || ctype->type == CTYPE_CHAR || ctype->type == CTYPE_INT ||
           ctype->type == CTYPE_LONG || ctype->type == CTYPE_ENUM;
}

bool is_flotype(Ctype *ctype)
{
    return ctype->type == CTYPE_FLOAT || ctype->type == CTYPE_DOUBLE;
}

static void ensure_lvalue(Ast *ast)
{
    switch (ast->type) {
    case AST_LVAR:
    case AST_GVAR:
    case AST_DEREF:
    case AST_STRUCT_REF:
        return;
    default:
        error("lvalue expected, but got %s", ast_to_string(ast));
    }
}

static void expect(char punct)
{
    Token tok = read_token();
    if (!is_punct(tok, punct))
        error("'%c' expected, but got %s", punct, token_to_string(tok));
}

static bool is_ident(const Token tok, char *s)
{
    return get_ttype(tok) == TTYPE_IDENT && !strcmp(get_ident(tok), s);
}

static SwitchParseCtx *current_switch_ctx(void)
{
    return switch_ctx;
}

static void switch_ctx_record_case(SwitchParseCtx *ctx, long low, long high)
{
    if (!ctx) return;
    for (long v = low; v <= high; ++v) {
        String buf = make_string();
        string_appendf(&buf, "%ld", v);
        char *key = get_cstring(buf);
        if (dict_get(ctx->seen, key))
            error("duplicate case value %ld in range", v);
        dict_put(ctx->seen, key, (void *)1);
    }
}

static bool is_right_assoc(const Token tok)
{
    int p = get_punct(tok);
    return p == '=' || p == PUNCT_SHL_ASSIGN || p == PUNCT_SHR_ASSIGN ||
           p == PUNCT_AND_ASSIGN || p == PUNCT_OR_ASSIGN  || p == PUNCT_XOR_ASSIGN ||
           p == PUNCT_ADD_ASSIGN || p == PUNCT_SUB_ASSIGN || p == PUNCT_MUL_ASSIGN ||
           p == PUNCT_DIV_ASSIGN || p == PUNCT_MOD_ASSIGN;
}

static bool is_assign_punct(int p)
{
    return p == '=' || p == PUNCT_SHL_ASSIGN || p == PUNCT_SHR_ASSIGN ||
           p == PUNCT_AND_ASSIGN || p == PUNCT_OR_ASSIGN  || p == PUNCT_XOR_ASSIGN ||
           p == PUNCT_ADD_ASSIGN || p == PUNCT_SUB_ASSIGN || p == PUNCT_MUL_ASSIGN ||
           p == PUNCT_DIV_ASSIGN || p == PUNCT_MOD_ASSIGN;
}

static bool is_compound_assign_punct(int p)
{
    return p == PUNCT_SHL_ASSIGN || p == PUNCT_SHR_ASSIGN ||
           p == PUNCT_AND_ASSIGN || p == PUNCT_OR_ASSIGN  || p == PUNCT_XOR_ASSIGN ||
           p == PUNCT_ADD_ASSIGN || p == PUNCT_SUB_ASSIGN || p == PUNCT_MUL_ASSIGN ||
           p == PUNCT_DIV_ASSIGN || p == PUNCT_MOD_ASSIGN;
}

static int compound_base_op(int p)
{
    switch (p) {
    case PUNCT_SHL_ASSIGN: return PUNCT_LSHIFT;
    case PUNCT_SHR_ASSIGN: return PUNCT_RSHIFT;
    case PUNCT_AND_ASSIGN: return '&';
    case PUNCT_OR_ASSIGN:  return '|';
    case PUNCT_XOR_ASSIGN: return '^';
    case PUNCT_ADD_ASSIGN: return '+';
    case PUNCT_SUB_ASSIGN: return '-';
    case PUNCT_MUL_ASSIGN: return '*';
    case PUNCT_DIV_ASSIGN: return '/';
    case PUNCT_MOD_ASSIGN: return '%';
    default: return p;
    }
}

static int eval_intexpr(Ast *ast)
{
    switch (ast->type) {
    case AST_CAST:
        /* 支持类似 (void*)(int)0x4000 的写法：如果内层表达式是整型常量，仍然可以求值 */
        if (is_inttype(ast->ctype)) return eval_intexpr(ast->cast_expr);
        if (ast->cast_expr && is_inttype(ast->cast_expr->ctype))
            return eval_intexpr(ast->cast_expr);
        error("Integer expression expected, but got %s", ast_to_string(ast));
    
    case AST_LITERAL:
        if (is_inttype(ast->ctype)) return ast->ival;
        error("Integer expression expected, but got %s", ast_to_string(ast));
    case AST_GVAR:
        if (ast->ginit) return eval_intexpr(ast->ginit);
        {
            Ast *g = dict_get(globalenv, ast->varname);
            if (g && g->ginit) return eval_intexpr(g->ginit);
        }
        error("Integer expression expected, but got %s", ast_to_string(ast));
    case AST_BIT_REF:
        {
            int idx = ast->bit_index;
            Ast *base = ast->struc;
            if (base->type == AST_LITERAL) {
                return (base->ival >> idx) & 1;
            }
            if (base->type == AST_GVAR) {
                Ast *g = base->ginit;
                if (!g) {
                    g = dict_get(globalenv, base->varname);
                    if (g) g = g->ginit;
                }
                if (g) {
                    if (g->type == AST_LITERAL) return (g->ival >> idx) & 1;
                    return (eval_intexpr(g) >> idx) & 1;
                }
                error("Integer expression expected, but got %s", ast_to_string(base));
            }
            int b = eval_intexpr(base);
            return (b >> idx) & 1;
        }
    case '+': return eval_intexpr(ast->left) + eval_intexpr(ast->right);
    case '-': return eval_intexpr(ast->left) - eval_intexpr(ast->right);
    case '*': return eval_intexpr(ast->left) * eval_intexpr(ast->right);
    case '/': return eval_intexpr(ast->left) / eval_intexpr(ast->right);
    case '%': return eval_intexpr(ast->left) % eval_intexpr(ast->right);
    case '^': return eval_intexpr(ast->left) ^ eval_intexpr(ast->right);
    case '|': return eval_intexpr(ast->left) | eval_intexpr(ast->right);
    case '&': return eval_intexpr(ast->left) & eval_intexpr(ast->right);
    case '>': return eval_intexpr(ast->left) > eval_intexpr(ast->right);
    case '<': return eval_intexpr(ast->left) < eval_intexpr(ast->right);
    case '!': return !eval_intexpr(ast->left);
    case '~': return ~eval_intexpr(ast->left);
    case PUNCT_LSHIFT: return eval_intexpr(ast->left) << eval_intexpr(ast->right);
    case PUNCT_RSHIFT: return eval_intexpr(ast->left) >> eval_intexpr(ast->right);
    case PUNCT_LOGAND: return eval_intexpr(ast->left) && eval_intexpr(ast->right);
    case PUNCT_LOGOR:  return eval_intexpr(ast->left) || eval_intexpr(ast->right);
    case PUNCT_EQ:     return eval_intexpr(ast->left) == eval_intexpr(ast->right);
    case PUNCT_GE:     return eval_intexpr(ast->left) >= eval_intexpr(ast->right);
    case PUNCT_LE:     return eval_intexpr(ast->left) <= eval_intexpr(ast->right);
    case PUNCT_NE:     return eval_intexpr(ast->left) != eval_intexpr(ast->right);
    default:
        error("Integer expression expected, but got %s", ast_to_string(ast));
        return 0; /* non-reachable */
    }
}

static float eval_floatexpr(Ast *ast) 
{
    switch (ast->type) {
    case AST_CAST:
        /* 支持将内层整/浮点常量通过 cast 后用于浮点常量求值 */
        if (is_flotype(ast->ctype) || is_inttype(ast->ctype))
            return eval_floatexpr(ast->cast_expr);
        if (ast->cast_expr && (is_flotype(ast->cast_expr->ctype) || is_inttype(ast->cast_expr->ctype)))
            return eval_floatexpr(ast->cast_expr);
        error("Float expression expected, but got %s", ast_to_string(ast));

    case AST_LITERAL:
        if (is_flotype(ast->ctype))
            return ast->fval;
        else if (is_inttype(ast->ctype))
            return ast->ival;
        error("Float expression expected, but got %s", ast_to_string(ast));
    case '+':
        return eval_floatexpr(ast->left) + eval_floatexpr(ast->right);
    case '-':
        return eval_floatexpr(ast->left) - eval_floatexpr(ast->right);
    case '*':
        return eval_floatexpr(ast->left) * eval_floatexpr(ast->right);
    case '/':
        return eval_floatexpr(ast->left) / eval_floatexpr(ast->right);
    default:
        error("Float expression expected, but got %s", ast_to_string(ast));
        return 0; /* non-reachable */
    }
}

static int priority(const Token tok)
{
    switch (get_punct(tok)) {
    case '[':
    case '.':
    case PUNCT_ARROW:
        return 1;
    case PUNCT_INC:
    case PUNCT_DEC:
        return 2;
    case '*':
    case '/':
    case '%':
        return 3;
    case '+':
    case '-':
        return 4;
    case PUNCT_LSHIFT:
    case PUNCT_RSHIFT:
        return 5;
    case '<':
    case '>':
        return 6;
    case '&':
        return 8;
    case '^':
        return 9;
    case '|':
        return 10;
    case PUNCT_EQ:
    case PUNCT_GE:
    case PUNCT_LE:
    case PUNCT_NE:
        return 7;
    case PUNCT_LOGAND:
        return 11;
    case PUNCT_LOGOR:
        return 12;
    case '?':
        return 13;
    case '=':
    case PUNCT_SHL_ASSIGN:
    case PUNCT_SHR_ASSIGN:
    case PUNCT_AND_ASSIGN:
    case PUNCT_OR_ASSIGN:
    case PUNCT_XOR_ASSIGN:
    case PUNCT_ADD_ASSIGN:
    case PUNCT_SUB_ASSIGN:
    case PUNCT_MUL_ASSIGN:
    case PUNCT_DIV_ASSIGN:
    case PUNCT_MOD_ASSIGN:
        return 14;
    default:
        return -1;
    }
}

static bool have_redefine_var(char* var_name) 
{
    if(localenv && dict_has_current(localenv, var_name)) return true;
    return false;
}

static bool have_redefine_func(char* func_name) 
{
    Ast *func = dict_get(functionenv, func_name);
    return func && func->type == AST_FUNC_DEF;
}

static Ast *read_func_args(char *fname, Ast *func_ptr)
{
    List *args = make_list();
    while (1) {
        Token tok = read_token();
        if (is_punct(tok, ')'))
            break;
        unget_token(tok);
        list_push(args, read_expr());
        tok = read_token();
        if (is_punct(tok, ')'))
            break;
        if (!is_punct(tok, ','))
            error("Unexpected token: '%s'", token_to_string(tok));
    }
    if (MAX_ARGS < list_len(args))
        error("Too many arguments: %s", fname);

    // 如果提供了函数指针，使用函数指针的类型
    if (func_ptr) {
        return ast_funcall(func_ptr->ctype->ptr, fname, args, func_ptr);
    }
    
    Ast *func = dict_get(functionenv, fname);
    if(!func) error("Undecl function: %s", fname);
    
    return ast_funcall(func->ctype, fname, args, NULL);
}

static Ast *read_ident_or_func(char *name)
{
    Token tok = read_token();
    if (is_punct(tok, '(')) {
        /* GCC 内建函数处理：__builtin_expect(expr, val) → 返回 expr */
        if (!strcmp(name, "__builtin_expect")) {
            Ast *expr = read_expr();
            expect(',');
            /* 消耗第二个参数（常量提示） */
            read_expr();
            expect(')');
            return expr;
        }
        /* __builtin_unreachable() → 返回 0 */
        if (!strcmp(name, "__builtin_unreachable") ||
            !strcmp(name, "__builtin_trap")) {
            expect(')');
            return ast_inttype(ctype_int, 0);
        }
        /* C11 _Generic(expr, type: val, ..., default: val) */
        if (!strcmp(name, "_Generic")) {
            Ast *ctrl = read_expr_int(14);
            Ctype *ctrl_type = ctrl->ctype;
            expect(',');
            Ast *default_val = NULL;
            Ast *matched_val = NULL;
            while (1) {
                Token t = peek_token();
                if (is_ident(t, "default")) {
                    read_token();
                    expect(':');
                    default_val = read_expr_int(14);
                } else {
                    /* 读取类型（支持 int[4]、int *、const int * 等） */
                    Ctype *assoc_type = read_decl_spec();
                    /* 处理可能的数组维度: int[4] */
                    if (is_punct(peek_token(), '[')) {
                        read_token(); /* consume '[' */
                        int dim = -1;
                        if (!is_punct(peek_token(), ']')) {
                            Ast *sz = read_expr_int(14);
                            if (sz->type == AST_LITERAL) dim = (int)sz->ival;
                        }
                        expect(']');
                        assoc_type = make_array_type(assoc_type, dim);
                    }
                    expect(':');
                    Ast *assoc_val = read_expr_int(14);
                    /* 简单类型匹配：比较 type->type */
                    if (!matched_val && ctrl_type && assoc_type &&
                        ctrl_type->type == assoc_type->type) {
                        matched_val = assoc_val;
                    }
                }
                t = read_token();
                if (is_punct(t, ')')) break;
                if (!is_punct(t, ','))
                    error("',' expected in _Generic, but got %s", token_to_string(t));
            }
            Ast *result = matched_val ? matched_val : (default_val ? default_val : ast_inttype(ctype_int, 0));
            return read_postfix_expr_tail(result);
        }
        // 检查是否是函数指针变量调用
        Ast *v = dict_get(localenv, name);
        if (!v) v = dict_get(globalenv, name);
        if (v && v->ctype && v->ctype->type == CTYPE_PTR) {
            // 可能是函数指针调用，检查返回类型是否有效
            return read_func_args(name, v);
        }
        return read_func_args(name, NULL);
    }
    
    // 只有在不在?:的then分支中，且下一个token是:时，才认为是标签
    if (!in_cond_expr && is_punct(tok, ':'))
        return ast_label(name);

    unget_token(tok);

    Ast *v = dict_get(localenv, name);
    if (!v) {
        v = dict_get(globalenv, name);
        if(!v) {
            // 检查是否是函数名（用于函数指针初始化）
            Ast *func = dict_get(functionenv, name);
            if(func) {
                // 返回函数声明/定义的AST，其ctype是返回类型
                return func;
            }
            error("Undefined varaible: %s", name);
        }
    }
        
    return v;
}

static bool is_long_token(char *p)
{
    for (; *p; p++) {
        if (!isdigit(*p))
            return (*p == 'L' || *p == 'l') && p[1] == '\0';
    }
    return false;
}

static bool is_int_token(char *p)
{
    /* 允许纯十进制数字（包括负号） */
    if (*p == '-') p++;
    for (; *p; p++)
        if (!isdigit(*p))
            return false;
    return true;
}

static bool is_float_token(char *p)
{
    for (; *p; p++)
        if (!isdigit(*p))
            break;
    if (*p++ != '.')
        return false;
    for (; *p; p++)
        if (!isdigit(*p))
            return false;
    return true;
}

static Ast *read_prim(void)
{
    Token tok = read_token();
    switch (get_ttype(tok)) {
    case TTYPE_NULL:
        return NULL;
    case TTYPE_IDENT: {
        int enum_val = 0;
        if(get_enum_val(get_ident(tok), &enum_val)) return ast_inttype(ctype_int, enum_val);
        else                                        return read_ident_or_func(get_ident(tok));
    }
    case TTYPE_NUMBER: {
        char *number = get_number(tok);
        if (is_long_token(number))
            return ast_inttype(ctype_long, atoll(number));
        if (is_int_token(number)) {
            long long val = atoll(number);
            /* 判断是否超出 int 范围 (含负数) */
            if (val > (long long)INT_MAX || val < (long long)INT_MIN)
                return ast_inttype(ctype_long, val);
            return ast_inttype(ctype_int, (int)val);
        }
        if (is_float_token(number))
            return ast_double(atof(number));
        error("Malformed number: %s", token_to_string(tok));
    }
    case TTYPE_CHAR:
        return ast_inttype(ctype_char, get_char(tok));
    case TTYPE_STRING: {
        /* C 标准：相邻字符串字面量自动拼接 */
        const char *s = get_strtok(tok);
        String combined = make_string();
        int slen = strlen(s);
        for (int i = 0; i < slen; i++) string_append(&combined, s[i]);
        for (;;) {
            Token next = read_token();
            if (get_ttype(next) == TTYPE_STRING) {
                const char *s2 = get_strtok(next);
                int s2len = strlen(s2);
                for (int i = 0; i < s2len; i++) string_append(&combined, s2[i]);
            } else {
                unget_token(next);
                break;
            }
        }
        Ast *r = ast_string(get_cstring(combined));
        list_push(strings, r);
        return r;
    }
    case TTYPE_PUNCT:
        unget_token(tok);
        return NULL;
    default:
        error("internal error: unknown token type: %d", get_ttype(tok));
        return NULL; /* non-reachable */
    }
}

#define swap(a, b)         \
    {                      \
        typeof(a) tmp = b; \
        b = a;             \
        a = tmp;           \
    }


static Ctype *arith_bin_type[CTYPE_DOUBLE+1][CTYPE_DOUBLE+1];
static void init_arith_table(void)
{
    #define T(a,b,res) arith_bin_type[a][b] = arith_bin_type[b][a] = (res)
    T(CTYPE_BOOL,   CTYPE_BOOL,   ctype_bool);
    T(CTYPE_BOOL,   CTYPE_CHAR,   ctype_char);
    T(CTYPE_BOOL,   CTYPE_INT,    ctype_int);
    T(CTYPE_BOOL,   CTYPE_LONG,   ctype_long);
    T(CTYPE_BOOL,   CTYPE_FLOAT,  ctype_float);
    T(CTYPE_BOOL,   CTYPE_DOUBLE, ctype_double);

    T(CTYPE_CHAR,   CTYPE_CHAR,   ctype_int);   /* C integer promotion: char+char→int */
    T(CTYPE_CHAR,   CTYPE_INT,    ctype_int);
    T(CTYPE_CHAR,   CTYPE_LONG,   ctype_long);
    T(CTYPE_CHAR,   CTYPE_FLOAT,  ctype_float);
    T(CTYPE_CHAR,   CTYPE_DOUBLE, ctype_double);

    T(CTYPE_INT,    CTYPE_INT,    ctype_int);
    T(CTYPE_INT,    CTYPE_LONG,   ctype_long);
    T(CTYPE_INT,    CTYPE_FLOAT,  ctype_float);
    T(CTYPE_INT,    CTYPE_DOUBLE, ctype_double);

    T(CTYPE_LONG,   CTYPE_LONG,   ctype_long);
    T(CTYPE_LONG,   CTYPE_FLOAT,  ctype_float);
    T(CTYPE_LONG,   CTYPE_DOUBLE, ctype_double);

    T(CTYPE_FLOAT,  CTYPE_FLOAT,  ctype_float);
    T(CTYPE_FLOAT,  CTYPE_DOUBLE, ctype_double);

    T(CTYPE_DOUBLE, CTYPE_DOUBLE, ctype_double);
    #undef T
}

static Ctype *result_type_int(jmp_buf *jmp, int op, Ctype *a, Ctype *b)
{
    static int arith_table_inited = 0;
    if (!arith_table_inited) {
        init_arith_table();
        arith_table_inited = 1;
    }

    if (a->type == CTYPE_PTR || b->type == CTYPE_PTR) {
        if (op == '=') return a->type == CTYPE_PTR ? a : b;
        
        /* 三元运算符 ?: 中，void* 与任意指针兼容，整数 0 当作 NULL */
        if (op == ':') {
            if (a->type == CTYPE_PTR && b->type == CTYPE_PTR) {
                /* void* 与任意指针，返回非 void 的那个 */
                if (a->ptr && a->ptr->type == CTYPE_VOID) return b;
                return a;
            }
            if (a->type == CTYPE_PTR) return a;  /* b 是整数(NULL) */
            return b;  /* a 是整数(NULL) */
        }

        if (op == PUNCT_EQ || op == PUNCT_NE || op == '<' || op == '>' ||
            op == PUNCT_LE || op == PUNCT_GE) {
            if (a->type == CTYPE_PTR && b->type == CTYPE_PTR)
                return ctype_int;
            goto err;
        }

        if (op == '-') {
            if (a->type == CTYPE_PTR && b->type == CTYPE_PTR)
                return ctype_int;
            if (a->type == CTYPE_PTR)
                return a;
            goto err;
        }

        if (op == '+') {
            if (a->type == CTYPE_PTR && b->type != CTYPE_PTR)
                return a;
            if (b->type == CTYPE_PTR && a->type != CTYPE_PTR)
                return b;
            goto err;
        }

        goto err;
    }
    if (a->type == CTYPE_ARRAY || b->type == CTYPE_ARRAY) goto err;

    int ai = (a->type==CTYPE_ENUM) ? CTYPE_INT : a->type;
    int bi = (b->type==CTYPE_ENUM) ? CTYPE_INT : b->type;
    if ((unsigned)ai >= CTYPE_DOUBLE+1 || (unsigned)bi >= CTYPE_DOUBLE+1) goto err;

    Ctype *t = arith_bin_type[ai][bi];

    if (!t) goto err;
    return t;

err:
    longjmp(*jmp, 1);
    return ctype_void;
}

static Ast *read_subscript_expr(Ast *ast)
{
    Ast *sub = read_expr();
    expect(']');
    Ast *t = ast_binop('+', ast, sub);
    return ast_uop(AST_DEREF, t->ctype->ptr, t);
}

static Ctype *convert_array(Ctype *ctype)
{
    if (ctype->type != CTYPE_ARRAY)
        return ctype;
    return make_ptr_type(ctype->ptr);
}

static Ctype *result_type(int op, Ctype *a, Ctype *b)
{
    if (is_compound_assign_punct(op)) {
        // 复合赋值：先用底层二元运算检查合法性，结果类型按C语义取左值类型
        int base_op = compound_base_op(op);
        // 特殊：shift-assign 只允许整数
        if ((base_op == PUNCT_LSHIFT || base_op == PUNCT_RSHIFT) &&
            (!is_inttype(a) || !is_inttype(b))) {
            error("invalid operand to shift");
        }
        jmp_buf jmpbuf;
        if (setjmp(jmpbuf) == 0) {
            (void)result_type_int(&jmpbuf, base_op, convert_array(a), convert_array(b));
            return a;
        }
        error("incompatible operands in compound assignment");
        return NULL; /* non-reachable */
    }

    // 特殊处理：函数指针赋值
    if (op == '=') {
        // 允许将函数名赋值给函数指针
        // 或者允许指针之间的赋值
        if (a->type == CTYPE_PTR && b->type == CTYPE_PTR)
            return a;
        // 允许 int 赋值给指针（地址值）
        if (a->type == CTYPE_PTR && b->type <= CTYPE_LONG)
            return a;
    }
    
    jmp_buf jmpbuf;
    if (setjmp(jmpbuf) == 0)
        return result_type_int(&jmpbuf, op, convert_array(a), convert_array(b));
    error("incompatible operands: %d: <%s> and <%s>", op, ctype_to_string(a),
          ctype_to_string(b));
    return NULL; /* non-reachable */
}

/* 前向声明，供复合字面量代码使用 */
static Ast *read_decl_struct_init(Ctype *ctype);
static Ast *read_decl_array_init_recurse(Ctype *ctype);

/* 为给定类型创建零初始化 AST 节点 */
static Ast *make_zero_init(Ctype *ctype)
{
    if (!ctype) return ast_inttype(ctype_int, 0);
    switch (ctype->type) {
    case CTYPE_BOOL ... CTYPE_LONG:
        return ast_inttype(ctype, 0);
    case CTYPE_FLOAT ... CTYPE_DOUBLE:
        return ast_double(0.0);
    case CTYPE_PTR:
        return ast_inttype(ctype_long, 0);
    case CTYPE_ARRAY: {
        /* 递归创建零数组 */
        List *lst = make_list();
        int n = (ctype->len > 0) ? ctype->len : 0;
        for (int i = 0; i < n; i++)
            list_push(lst, make_zero_init(ctype->ptr));
        return ast_array_init(lst);
    }
    case CTYPE_STRUCT: {
        List *lst = make_list();
        if (ctype->fields) {
            for (Iter it = list_iter(ctype->fields->list); !iter_end(it);) {
                DictEntry *e = iter_next(&it);
                Ctype *ft = dict_get(ctype->fields, e->key);
                list_push(lst, make_zero_init(ft));
            }
        }
        return ast_struct_init(ctype, lst);
    }
    default:
        return ast_inttype(ctype_int, 0);
    }
}
static void skip_attribute(void);

static Ast *read_unary_expr(void)
{
    Token tok = read_token();
    if (get_ttype(tok) != TTYPE_PUNCT && get_ttype(tok) != TTYPE_IDENT) {
        unget_token(tok);
        return read_prim();
    }

    if (is_punct(tok, '(') && is_type_keyword(peek_token())) {
        Ctype *target = read_decl_spec();
        /* 处理函数指针类型转换: (void (*)(void)) 或 (int (*)(int, int)) */
        if (is_punct(peek_token(), '(')) {
            Token paren = read_token(); /* consume '(' */
            Token star = read_token();
            if (is_punct(star, '*')) {
                /* 跳过 (*) 中间的名字（如果有）*/
                Token inner = read_token();
                if (!is_punct(inner, ')'))
                    ; /* 有名字，已消耗 */
                /* 消耗参数列表 */
                expect('(');
                read_func_ptr_params();
                target = make_ptr_type(target);
            } else {
                unget_token(star);
                unget_token(paren);
            }
        }
        expect(')');
        /* 复合字面量: (struct S){1, 2} 或 (int[]){1,2,3} */
        if (is_punct(peek_token(), '{')) {
            char namebuf[32];
            static int compound_seq = 0;
            snprintf(namebuf, sizeof(namebuf), "__compound_%d", compound_seq++);
            Ast *var = localenv
                ? ast_lvar(target, strdup(namebuf))
                : ast_gvar(target, strdup(namebuf), false);
            Ast *init;
            if (target->type == CTYPE_STRUCT)
                init = read_decl_struct_init(target);
            else if (target->type == CTYPE_ARRAY)
                init = read_decl_array_init_recurse(target);
            else {
                expect('{');
                init = read_expr();
                expect('}');
            }
            Ast *decl = ast_decl(var, init);
            /* 全局 compound literal：把 decl 推入顶层输出队列 */
            if (!localenv) {
                if (!pending_toplevel_decls) pending_toplevel_decls = make_list();
                list_push(pending_toplevel_decls, decl);
                /* 同时在 gvar 上记录 ginit，便于 codegen 内联展开 */
                if (var->type == AST_GVAR) var->ginit = init;
            } else {
                /* 局部 compound literal：推入待注入队列 */
                if (!pending_local_decls) pending_local_decls = make_list();
                list_push(pending_local_decls, decl);
            }
            return read_postfix_expr_tail(var);
        }
        Ast *expr = read_unary_expr();
        Ast *cast = ast_cast(target, expr);
        return read_postfix_expr_tail(cast);
    }

    if (is_ident(tok, "sizeof")) {
        Token t = read_token();
        if (is_punct(t, '(')) {
            if (is_type_keyword(peek_token())) {
                Ctype *target = read_decl_spec();
                expect(')');
                return ast_inttype(ctype_int, target->size);
            }
            Ast *e = read_expr();
            expect(')');
            return ast_inttype(ctype_int, e->ctype->size);
        }
        unget_token(t);
        Ast *e = read_unary_expr();
        return ast_inttype(ctype_int, e->ctype->size);
    }

    if (is_punct(tok, '(')) {
        /* GNU 扩展：语句表达式 ({...}) */
        if (is_punct(peek_token(), '{')) {
            read_token(); /* consume '{' */
            Ast *compound = read_compound_stmt();
            expect(')');
            /* 语句表达式的值是最后一条表达式语句的值；
             * 为简单实现，取最后一条语句，若无则返回 0。 */
            List *stmts = compound->stmts;
            if (stmts && stmts->len > 0) {
                Ast *last = (Ast *)list_get(stmts, stmts->len - 1);
                /* 只有表达式类型的语句才有 ctype，其他类型返回 int 0 */
                if (last && last->ctype)
                    return read_postfix_expr_tail(last);
            }
            return read_postfix_expr_tail(ast_inttype(ctype_int, 0));
        }
        Ast *r = read_comma_expr();
        expect(')');
        return read_postfix_expr_tail(r);
    }
    if (is_punct(tok, '&')) {
        Ast *operand = read_unary_expr();
        /* 函数名（包括函数定义和函数声明）可以取地址，不需要 lvalue */
        if (operand->type == AST_FUNC_DEF || operand->type == AST_FUNC_DECL) {
            /* &funcname：返回函数指针 */
            Ctype *fptr = make_ptr_type(operand->ctype);
            return ast_uop(AST_ADDR, fptr, operand);
        }
        /* 数组名退化 */
        if (operand->ctype->type == CTYPE_ARRAY) {
            Ctype *ptr = make_ptr_type(operand->ctype->ptr);
            return ast_uop(AST_ADDR, ptr, operand);
        }
        ensure_lvalue(operand);
        return ast_uop(AST_ADDR, make_ptr_type(operand->ctype), operand);
    }
    if (is_punct(tok, '!')) {
        Ast *operand = read_unary_expr();
        return ast_uop('!', ctype_int, operand);
    }
    if (is_punct(tok, '~')) {
        Ast *operand = read_unary_expr();
        return ast_uop('~', ctype_int, operand);
    }
    if (is_punct(tok, '*')) {
        Ast *operand = read_unary_expr();
        Ctype *ctype = convert_array(operand->ctype);
        if (ctype->type != CTYPE_PTR) {
            /* 函数指针解引用：*funcptr 在 C 中等价于 funcptr 本身，
             * 类型系统不完整时（如函数指针被简化为 PTR-to-int），
             * 直接返回 operand 并继续处理后缀（如函数调用）。*/
            return read_postfix_expr_tail(operand);
        }
        if (ctype->ptr == ctype_void)
            error("pointer to void can not be dereferenced, but got %s",
                  ast_to_string(operand));
        return ast_uop(AST_DEREF, operand->ctype->ptr, operand);
    }
    // 一元正号: +a 等同于 a
    if (is_punct(tok, '+')) {
        Ast *operand = read_unary_expr();
        return operand;
    }
    // 一元负号: -a 等同于 0 - a
    if (is_punct(tok, '-')) {
        Ast *operand = read_unary_expr();
        return ast_binop('-', ast_inttype(ctype_int, 0), operand);
    }
    // 前置自增: ++a
    if (is_punct(tok, PUNCT_INC)) {
        Ast *operand = read_unary_expr();
        ensure_lvalue(operand);
        return ast_uop(PUNCT_INC, operand->ctype, operand);
    }
    // 前置自减: --a
    if (is_punct(tok, PUNCT_DEC)) {
        Ast *operand = read_unary_expr();
        ensure_lvalue(operand);
        return ast_uop(PUNCT_DEC, operand->ctype, operand);
    }
    unget_token(tok);
    return read_postfix_expr_tail(read_prim());
}

static Ast *read_cond_expr(Ast *cond)
{
    // 三元运算符是右结合的
    // then 分支使用优先级14（高于?:的13），这样遇到:时会停止
    // 设置标志，防止read_ident_or_func将identifier:当作标签
    in_cond_expr = true;
    Ast *then = read_expr_int(14);
    in_cond_expr = false;
    expect(':');
    // else 分支使用优先级13（?:的优先级），支持右结合
    Ast *els = read_expr_int(13);
    // 三元运算符的类型是 then 和 else 分支的公共类型
    Ctype *ctype = result_type(':', then->ctype, els->ctype);
    return ast_ternary(ctype, cond, then, els);
}

static Ast *read_struct_field(Ast *struc)
{
    if (struc->ctype->type != CTYPE_STRUCT)
        error("struct expected, but got %s", ast_to_string(struc));
    Token name = read_token();
    if (get_ttype(name) != TTYPE_IDENT)
        error("field name expected, but got %s", token_to_string(name));
    char *ident = get_ident(name);
    if (!struc->ctype->fields)
        error("struct/union has no fields (incomplete type): %s", ctype_to_string(struc->ctype));
    Ctype *field = dict_get(struc->ctype->fields, ident);
    if (!field)
        error("no field '%s' in %s", ident, ctype_to_string(struc->ctype));
    return ast_struct_ref(field, struc, ident);
}
static Ast *read_postfix_expr_tail(Ast *ast)
{
    if (!ast)
        return NULL;
    while (1) {
        Token tok = read_token();
        if (get_ttype(tok) != TTYPE_PUNCT) {
            unget_token(tok);
            return ast;
        }
        if (is_punct(tok, '.')) {
            Token next = peek_token();
            if (get_ttype(next) == TTYPE_NUMBER) {
                Token numtok = read_token();
                char *num = get_number(numtok);
                if (!is_int_token(num)) error("Invalid bit index: %s", token_to_string(numtok));
                int idx = atoi(num);
                Ast *r = malloc(sizeof(Ast));
                r->type = AST_BIT_REF;
                r->ctype = ctype_bool;
                r->struc = ast;
                r->bit_index = idx;
                ast = r;
                continue;
            }
            ast = read_struct_field(ast);
            continue;
        }
        if (is_punct(tok, PUNCT_ARROW)) {
            /* 数组可以退化为指针（array-to-pointer conversion） */
            Ctype *arrow_ctype = convert_array(ast->ctype);
            if (arrow_ctype->type != CTYPE_PTR)
                error("pointer type expected, but got %s %s",
                      ctype_to_string(ast->ctype), ast_to_string(ast));
            if (ast->ctype->type == CTYPE_ARRAY)
                ast = ast_uop(AST_ADDR, make_ptr_type(ast->ctype->ptr), ast);
            ast = ast_uop(AST_DEREF, arrow_ctype->ptr, ast);
            ast = read_struct_field(ast);
            continue;
        }
        if (is_punct(tok, '[')) {
            ast = read_subscript_expr(ast);
            continue;
        }
        if (is_punct(tok, '(')) {
            /* 函数指针调用：支持 (*fptr)()、fptr()、v.fptr()、p->fptr() 等多种形式 */
            if (ast->type == AST_DEREF && ast->operand &&
                ast->operand->ctype && ast->operand->ctype->type == CTYPE_PTR) {
                Ast *func_ptr = ast->operand;
                if (func_ptr->type == AST_LVAR || func_ptr->type == AST_GVAR) {
                    ast = read_func_args(func_ptr->varname, func_ptr);
                    continue;
                }
                /* (*expr[i])(args) 等任意函数指针表达式调用 */
                ast = read_func_args("(funcptr)", func_ptr);
                continue;
            }
            /* 结构体/联合体成员是函数指针：v.fptr() 或 p->fptr() */
            if (ast->type == AST_STRUCT_REF && ast->ctype && ast->ctype->type == CTYPE_PTR) {
                ast = read_func_args(ast->field, ast);
                continue;
            }
            /* 普通函数指针变量：fptr() */
            if ((ast->type == AST_LVAR || ast->type == AST_GVAR) &&
                ast->ctype && ast->ctype->type == CTYPE_PTR) {
                ast = read_func_args(ast->varname, ast);
                continue;
            }
            /* 函数变量（非指针，如从 _Generic 得到的函数引用）：a_f() */
            if ((ast->type == AST_LVAR || ast->type == AST_GVAR) && ast->varname) {
                ast = read_func_args(ast->varname, NULL);
                continue;
            }
            /* 函数定义/声明作为值引用后调用：_Generic(...)() */
            if ((ast->type == AST_FUNC_DEF || ast->type == AST_FUNC_DECL) && ast->fname) {
                ast = read_func_args(ast->fname, NULL);
                continue;
            }
            /* 函数调用返回值是函数指针：go()() */
            if (ast->ctype && ast->ctype->type == CTYPE_PTR) {
                /* 使用临时名字进行函数指针调用 */
                ast = read_func_args("(funcptr)", ast);
                continue;
            }
            /* 函数调用/解引用结果再次调用：(*fp)(a)(b) 或 f()(a) */
            if (ast->type == AST_FUNCALL || ast->type == AST_DEREF) {
                ast = read_func_args("(funcptr)", ast);
                continue;
            }
            unget_token(tok);
            return ast;
        }
        if (is_punct(tok, PUNCT_INC) || is_punct(tok, PUNCT_DEC)) {
            ensure_lvalue(ast);
            int post_type = is_punct(tok, PUNCT_INC) ? AST_POST_INC : AST_POST_DEC;
            ast = ast_uop(post_type, ast->ctype, ast);
            continue;
        }
        unget_token(tok);
        return ast;
    }
}

static Ast *read_expr_int(int prec)
{
    Ast *ast = read_unary_expr();
    if (!ast)
        return NULL;
    while (1) {
        Token tok = read_token();
        if (get_ttype(tok) != TTYPE_PUNCT) {
            unget_token(tok);
            return ast;
        }
        int prec2 = priority(tok);
        if (prec2 < 0 || prec <= prec2) {
            unget_token(tok);
            return ast;
        }
        if (is_punct(tok, '?')) {
            ast = read_cond_expr(ast);
            continue;
        }
        if (is_punct(tok, '.')) {
            Token next = peek_token();
            if (get_ttype(next) == TTYPE_NUMBER) {
                Token numtok = read_token();
                char *num = get_number(numtok);
                if (!is_int_token(num)) error("Invalid bit index: %s", token_to_string(numtok));
                int idx = atoi(num);
                Ast *r = malloc(sizeof(Ast));
                r->type = AST_BIT_REF;
                r->ctype = ctype_bool;
                r->struc = ast;
                r->bit_index = idx;
                ast = r;
                continue;
            } else {
                ast = read_struct_field(ast);
                continue;
            }
        }
        if (is_punct(tok, PUNCT_ARROW)) {
            Ctype *arrow_ctype = convert_array(ast->ctype);
            if (arrow_ctype->type != CTYPE_PTR)
                error("pointer type expected, but got %s %s",
                      ctype_to_string(ast->ctype), ast_to_string(ast));
            if (ast->ctype->type == CTYPE_ARRAY)
                ast = ast_uop(AST_ADDR, make_ptr_type(ast->ctype->ptr), ast);
            ast = ast_uop(AST_DEREF, arrow_ctype->ptr, ast);
            ast = read_struct_field(ast);
            continue;
        }
        if (is_punct(tok, '[')) {
            ast = read_subscript_expr(ast);
            continue;
        }
        // 支持 (*fp)(args) 形式的函数指针调用
        if (is_punct(tok, '(')) {
            // 检查左侧是否是解引用表达式（函数指针调用）
            if (ast->type == AST_DEREF && ast->operand &&
                ast->operand->ctype && ast->operand->ctype->type == CTYPE_PTR) {
                // (*fp)(args) 形式 - 获取函数指针变量名
                Ast *func_ptr = ast->operand;
                if (func_ptr->type == AST_LVAR || func_ptr->type == AST_GVAR) {
                    ast = read_func_args(func_ptr->varname, func_ptr);
                    continue;
                }
                /* (*expr[i])(args) 等任意函数指针表达式调用 */
                ast = read_func_args("(funcptr)", func_ptr);
                continue;
            }
            // 普通函数调用已在 read_ident_or_func 处理
            unget_token(tok);
            return ast;
        }
        if (is_punct(tok, PUNCT_INC) || is_punct(tok, PUNCT_DEC)) {
            ensure_lvalue(ast);
            int post_type = is_punct(tok, PUNCT_INC) ? AST_POST_INC : AST_POST_DEC;
            ast = ast_uop(post_type, ast->ctype, ast);
            continue;
        }
        if (is_assign_punct(get_punct(tok)))
            ensure_lvalue(ast);
        
        Ast *rest = read_expr_int(prec2 + (is_right_assoc(tok) ? 1 : 0));
        if (!rest)
            error("second operand missing");
        if (is_punct(tok, PUNCT_LSHIFT) || is_punct(tok, PUNCT_RSHIFT) ||
            is_punct(tok, PUNCT_SHL_ASSIGN) || is_punct(tok, PUNCT_SHR_ASSIGN)) {
            if (!is_inttype(ast->ctype) || !is_inttype(rest->ctype))
                error("invalid operand to shift");
        }
        ast = ast_binop(get_punct(tok), ast, rest);
    }
}

static Ast *read_expr_float(int prec)
{
    return NULL;
} 

static Ast *read_expr()
{
    return read_expr_int(MAX_OP_PRIO);
}

/* 逗号表达式：expr, expr, ... 按顺序求值，返回最后一个 */
static Ast *read_comma_expr(void)
{
    Ast *e = read_expr();
    while (is_punct(peek_token(), ',')) {
        read_token(); /* consume ',' */
        e = read_expr();
    }
    return e;
}

static Ctype *get_ctype(const Token tok)
{
    if (get_ttype(tok) != TTYPE_IDENT) return NULL;

    char *ident = get_ident(tok);
    if (!strcmp(ident, "void"))     return ctype_void;
    if (!strcmp(ident, "int"))      return ctype_int;
    if (!strcmp(ident, "short"))    return ctype_short;
    if (!strcmp(ident, "long"))     return ctype_long;
    if (!strcmp(ident, "bool"))     return ctype_bool;
    if (!strcmp(ident, "_Bool"))    return ctype_bool;
    if (!strcmp(ident, "char"))     return ctype_char;
    if (!strcmp(ident, "float"))    return ctype_float;
    if (!strcmp(ident, "double"))   return ctype_double;
    /* MCS51/C51 扩展类型：sfr=volatile unsigned char，sfr16=volatile unsigned short，sbit=volatile unsigned char */
    if (!strcmp(ident, "sfr") || !strcmp(ident, "__sfr"))        return ctype_char;   /* 8位 SFR，视为 unsigned char */
    if (!strcmp(ident, "sfr16") || !strcmp(ident, "__sfr16"))    return ctype_short;  /* 16位 SFR，视为 unsigned short */
    if (!strcmp(ident, "sbit") || !strcmp(ident, "__sbit"))      return ctype_char;   /* 位变量，视为 unsigned char */
    if (!strcmp(ident, "bit") || !strcmp(ident, "__bit"))        return ctype_char;   /* 位变量，视为 unsigned char */
    return NULL;
}

static bool is_type_keyword(const Token tok)
{
    bool is_keyword = get_ctype(tok) || is_ident(tok, "struct") || is_ident(tok, "union") || is_ident(tok, "enum") || 
        is_ident(tok, "const") || is_ident(tok, "volatile") || is_ident(tok, "restrict") ||
        is_ident(tok, "static") || is_ident(tok, "extern") || is_ident(tok, "unsigned") || is_ident(tok, "signed") ||
        is_ident(tok, "register") || is_ident(tok, "typedef") || is_ident(tok, "inline") || 
        is_ident(tok, "noreturn") || is_ident(tok, "short") ||
        /* MCS51/C51 特殊类型关键字（实际类型，不是存储修饰符）*/
        is_ident(tok, "sfr")   || is_ident(tok, "sfr16")  || is_ident(tok, "sbit")  ||
        is_ident(tok, "bit");
    /* 注：C51 存储修饰符 data/xdata/idata/code 等故意不在此处列出，
     * 它们是存储限定符，不是独立类型，放在此处会与同名变量冲突 */

    if(is_keyword) 
        return true;            
    
    if(get_ttype(tok) != TTYPE_IDENT) 
        return false;
    
    return dict_get(typedefenv, get_ident(tok));
}

#define IS_C51_KW(tok, kw) (is_ident((tok), (kw)) || is_ident((tok), "__" kw))

static bool is_c51_decl_modifier(Token tok)
{
    return get_ttype(tok) == TTYPE_IDENT &&
           (IS_C51_KW(tok, "data")  || IS_C51_KW(tok, "idata") ||
            IS_C51_KW(tok, "xdata") || IS_C51_KW(tok, "code")  ||
            IS_C51_KW(tok, "bdata") || IS_C51_KW(tok, "pdata") ||
            IS_C51_KW(tok, "far")   || IS_C51_KW(tok, "near")  ||
            IS_C51_KW(tok, "reentrant"));
}

static int skip_c51_decl_modifiers(void)
{
    int attr = 0;
    while (1) {
        Token tok = peek_token();
        if (!is_c51_decl_modifier(tok))
            return attr;
        /* 前瞻：若该修饰词后面紧跟 '=' ';' ',' '[' ')' 则它是变量名，不消耗 */
        tok = read_token();
        Token nxt = peek_token();
        if (is_punct(nxt, '=') || is_punct(nxt, ';') ||
            is_punct(nxt, ',') || is_punct(nxt, '[') ||
            is_punct(nxt, ')') || is_punct(nxt, '{')) {
            unget_token(tok);
            return attr;
        }
        (void)read_decl_ctype_attr(tok, &attr);
    }
}


static int array_n_elts(Ctype *ctype)
{
    if (ctype->type != CTYPE_ARRAY) return -1;
    return ctype->len;          /* -1 表示“未知” */
}

static Ast *read_decl_struct_init(Ctype *ctype);  /* forward declaration */
static Ast *read_decl_struct_init_elide(Ctype *ctype);  /* forward declaration */

/* brace_has_comma: 在不消耗 token 的前提下，判断 '{...}' 内部（depth=1 层）是否有 ','。
 * 假设 peek_token() == '{'。该函数读取 '{' 到匹配的 '}' 之间的所有 tokens，
 * 检查 depth=1 时是否有 ','，然后将所有读过的 tokens 放回 unget buffer（逆序）。
 * 返回 true 表示多值（不是简单的 {scalar_expr}），false 表示单值或空。
 * 注意：unget buffer 必须足够大（>=16），以容纳扫描的 tokens。 */
static bool brace_has_comma(void)
{
    /* 读取 '{' 直到匹配的 '}' 或发现 depth=1 的 ',' */
    Token buf[16];
    int count = 0;
    bool result = false;

    /* 读 '{' */
    buf[count++] = read_token(); /* 应该是 '{' */

    int depth = 1;
    while (depth > 0 && count < 15) {
        Token t = read_token();
        buf[count++] = t;
        if (is_punct(t, '{')) {
            depth++;
        } else if (is_punct(t, '}')) {
            depth--;
        } else if (is_punct(t, ',') && depth == 1) {
            result = true;
            /* 继续读完整个 brace 块，以便能放回所有 tokens */
            /* 实际上找到 comma 就可以停了，但需要把剩余 tokens 都放回 */
            /* 继续读直到 depth == 0 */
        } else if (get_ttype(t) == TTYPE_NULL) {
            break;
        }
    }

    /* 如果没读完（count==15），result 不可靠，但我们仍然把已读的放回 */
    /* 将读过的 tokens 逆序放回 unget buffer */
    for (int i = count - 1; i >= 0; i--) {
        unget_token(buf[i]);
    }

    return result;
}

static Ast *read_decl_array_init_recurse(Ctype *ctype)
{
    Token tok = read_token();

    /* wchar_t 数组可以用字符串字面量（L"..."）初始化：UTF-8 解码为 Unicode codepoints */
    if ((ctype->ptr->type == CTYPE_INT || ctype->ptr->type == CTYPE_LONG) &&
        get_ttype(tok) == TTYPE_STRING) {
        const char *s = get_strtok(tok);
        List *lst = make_list();
        int i = 0;
        while (s[i]) {
            unsigned char c0 = (unsigned char)s[i];
            unsigned int cp;
            if (c0 < 0x80) {
                cp = c0; i++;
            } else if ((c0 & 0xE0) == 0xC0) {
                cp = (c0 & 0x1F) << 6 | ((unsigned char)s[i+1] & 0x3F); i += 2;
            } else if ((c0 & 0xF0) == 0xE0) {
                cp = (c0 & 0x0F) << 12 | ((unsigned char)s[i+1] & 0x3F) << 6 | ((unsigned char)s[i+2] & 0x3F); i += 3;
            } else if ((c0 & 0xF8) == 0xF0) {
                cp = (c0 & 0x07) << 18 | ((unsigned char)s[i+1] & 0x3F) << 12 | ((unsigned char)s[i+2] & 0x3F) << 6 | ((unsigned char)s[i+3] & 0x3F); i += 4;
            } else {
                cp = c0; i++;
            }
            list_push(lst, ast_inttype(ctype->ptr, (long)cp));
        }
        list_push(lst, ast_inttype(ctype->ptr, 0)); /* NUL 终止 */
        return ast_array_init(lst);
    }

    /* char 数组可以用字符串字面量初始化（支持相邻字符串拼接） */
    if (ctype->ptr->type == CTYPE_CHAR && get_ttype(tok) == TTYPE_STRING) {
        const char *s = get_strtok(tok);
        String combined = make_string();
        int slen = strlen(s);
        for (int i = 0; i < slen; i++) string_append(&combined, s[i]);
        for (;;) {
            Token next = read_token();
            if (get_ttype(next) == TTYPE_STRING) {
                const char *s2 = get_strtok(next);
                int s2len = strlen(s2);
                for (int i = 0; i < s2len; i++) string_append(&combined, s2[i]);
            } else {
                unget_token(next);
                break;
            }
        }
        return ast_string(get_cstring(combined));
    }

    if (!is_punct(tok, '{'))
        error("Expected '{' for array initializer of %s, got %s",
              ctype_to_string(ctype), token_to_string(tok));

    List *row_list = make_list();   
    int expect_rows = array_n_elts(ctype);
    int actual_rows = 0;
    /* 指定初始化器支持：维护当前插入索引 */
    int cur_idx = 0;

    while (1) {
        if (is_punct(peek_token(), '}'))  
            break;

        /* 检查是否为指定初始化器 [N] = val 或 [lo ... hi] = val（GCC 范围扩展） */
        if (is_punct(peek_token(), '[')) {
            read_token();  /* 消耗 '[' */
            Token idx_tok = read_token();
            int des_lo = 0, des_hi = -1;
            if (get_ttype(idx_tok) == TTYPE_NUMBER)
                des_lo = atoi(get_number(idx_tok));
            /* 检查是否为范围 [lo ... hi] */
            Token maybe_dots = read_token();
            if (is_punct(maybe_dots, PUNCT_ELLIPSIS)) {
                /* 是 '...' → 读取上界 */
                Token hi_tok = read_token();
                if (get_ttype(hi_tok) == TTYPE_NUMBER)
                    des_hi = atoi(get_number(hi_tok));
                expect(']');
            } else {
                /* 不是范围，放回 */
                unget_token(maybe_dots);
                expect(']');
                des_hi = -1;
            }
            expect('=');
            /* 先读取值（对范围赋值需要重用同一个值） */
            /* 注意：值在后面由正常的 one_row 读取路径处理，
             * 这里只设置 cur_idx，范围上界由 range_hi 记录 */
            cur_idx = des_lo;
            /* 对于范围 [lo...hi]，读取一次值，然后复制到 lo..hi */
            if (des_hi >= 0) {
                /* 扩展 row_list 到 des_hi */
                while (list_len(row_list) <= des_hi)
                    list_push(row_list, make_zero_init(ctype->ptr));
                /* 读取值 */
                Ast *range_val = NULL;
                if (ctype->ptr->type == CTYPE_ARRAY)
                    range_val = read_decl_array_init_recurse(ctype->ptr);
                else if (ctype->ptr->type == CTYPE_STRUCT) {
                    if (is_punct(peek_token(), '{'))
                        range_val = read_decl_struct_init(ctype->ptr);
                    else
                        range_val = read_decl_struct_init_elide(ctype->ptr);
                } else {
                    range_val = read_expr();
                }
                /* 填充 lo..hi */
                for (int ri = des_lo; ri <= des_hi; ri++)
                    list_set(row_list, ri, range_val);
                cur_idx = des_hi + 1;
                actual_rows = list_len(row_list);
                /* 消耗可能的逗号，继续下一个初始化器 */
                if (is_punct(peek_token(), ',')) {
                    read_token();
                    continue;
                }
                break;
            }
            /* 扩展 row_list 到 cur_idx */
            while (list_len(row_list) <= cur_idx)
                list_push(row_list, make_zero_init(ctype->ptr));
        }

        Ast *one_row;
        if (ctype->ptr->type == CTYPE_ARRAY) {
            /* 支持 brace elision（花括号省略）：如果内层数组没有 '{'，则读取标量值填充 */
            if (is_punct(peek_token(), '{')) {
                one_row = read_decl_array_init_recurse(ctype->ptr);
            } else {
                /* Brace elision: 读取 inner_n 个标量值构造内层数组 */
                int inner_n = array_n_elts(ctype->ptr);
                List *inner_list = make_list();
                for (int ii = 0; ii < inner_n; ii++) {
                    if (is_punct(peek_token(), '}'))
                        break;
                    Ast *val = read_expr();
                    list_push(inner_list, val);
                    if (ii + 1 < inner_n && is_punct(peek_token(), ','))
                        read_token(); /* 消耗内层逗号 */
                }
                /* 用零填充剩余元素 */
                while (list_len(inner_list) < inner_n)
                    list_push(inner_list, make_zero_init(ctype->ptr->ptr));
                one_row = ast_array_init(inner_list);
            }
        }
        else if (ctype->ptr->type == CTYPE_STRUCT) {
            if (is_punct(peek_token(), '{'))
                one_row = read_decl_struct_init(ctype->ptr);
            else {
                /* 可能是 struct 表达式（复合字面量、变量等）或 brace elision */
                Token peek = peek_token();
                /* 若下一 token 是 ident 且不是类型名，或是 '('，则尝试 read_expr；
                 * 对于纯标量列表（brace elision），用 read_decl_struct_init_elide */
                bool is_expr = false;
                if (is_punct(peek, '(')) {
                    /* lookahead: check if it looks like a compound literal / cast */
                    Token t1 = read_token(); /* '(' */
                    Token t2 = read_token(); /* '(' or typename or ident */
                    unget_token(t2);
                    unget_token(t1);
                    /* compound literal starts with (type) or ((type)) etc */
                    is_expr = true; /* always use read_expr for '(' prefix */
                } else if (get_ttype(peek) == TTYPE_IDENT) {
                    /* identifier: could be a variable of struct type */
                    is_expr = true;
                }
                if (is_expr)
                    one_row = read_expr();
                else
                    one_row = read_decl_struct_init_elide(ctype->ptr);
            }
        }
        else {
            /* 标量元素：支持 {expr} 单值初始化（C99），但多值 {expr,...} 则只取第一个值 */
            if (is_punct(peek_token(), '{') && !brace_has_comma()) {
                read_token(); /* consume '{' */
                one_row = read_expr();
                expect('}');
            } else {
                Token t = read_token();
                unget_token(t);
                one_row = read_expr();
            }
            if (one_row && ctype->ptr->type != CTYPE_PTR)
                result_type('=', one_row->ctype, ctype->ptr);
        }

        /* 放入指定位置或追加 */
        if (cur_idx < list_len(row_list))
            list_set(row_list, cur_idx, one_row);
        else {
            while (list_len(row_list) < cur_idx)
                list_push(row_list, make_zero_init(ctype->ptr));
            list_push(row_list, one_row);
        }
        cur_idx++;
        actual_rows = list_len(row_list);

        if (is_punct(peek_token(), ',')) {
            read_token();
            continue;
        }
        break;
    }

    expect('}');

    actual_rows = list_len(row_list);
    if (expect_rows != -1 && actual_rows > expect_rows)
        error("Array row count mismatch: expect %d, got %d",
              expect_rows, actual_rows);
    /* 如果 actual_rows < expect_rows，用零填充 */
    if (expect_rows != -1 && actual_rows < expect_rows) {
        while (list_len(row_list) < expect_rows)
            list_push(row_list, make_zero_init(ctype->ptr));
    }

    return ast_array_init(row_list);
}

static List *init_empty_struct_init(Ctype *ctype) {
    List *initlist = make_list();
    for (Iter it = list_iter(ctype->fields->list); !iter_end(it);) {
        DictEntry *e = iter_next(&it);
        Ctype *type = dict_get(ctype->fields, e->key);
        switch(type->type) {
            case CTYPE_BOOL ... CTYPE_LONG:
                list_push(initlist, ast_inttype(type, 0));
                break;
            case CTYPE_FLOAT ... CTYPE_DOUBLE:
                list_push(initlist, ast_double(0.0));
                break;
            case CTYPE_ARRAY:
                list_push(initlist, ast_array_init(make_list()));
                break;
            case CTYPE_PTR:
                list_push(initlist, make_ptr_type(type));
                break;
            case CTYPE_STRUCT:
                list_push(initlist, ast_struct_init(ctype, make_list()));
                break;
            default:
                error("internal error: unknown field type %d", type->type);
        };
    }
    return initlist;
}

static int struct_field_index(Ctype *ctype, const char *name)
{
    int idx = 0;
    for (Iter it = list_iter(ctype->fields->list); !iter_end(it); idx++) {
        DictEntry *e = iter_next(&it);
        if (!strcmp(e->key, name))
            return idx;
    }
    return -1;
}

static Ast *read_decl_struct_init(Ctype *ctype)
{
    Token tok = read_token();
    if (!is_punct(tok, '{'))
        error("Expected an initializer struct for %s, but got %s",
              ctype_to_string(ctype), token_to_string(tok));
    List *initlist = init_empty_struct_init(ctype);
    /* 空 struct/union：允许 {} 初始化（直接跳过大括号内容） */
    if (list_len(initlist) == 0) {
        /* 读到匹配的 }，忽略其中内容 */
        int depth = 1;
        while (depth > 0) {
            Token t = read_token();
            if (get_ttype(t) == TTYPE_NULL) break;
            if (is_punct(t, '{')) depth++;
            else if (is_punct(t, '}')) depth--;
        }
        return ast_struct_init(ctype, initlist);
    }
    Iter it = list_iter(initlist);
    int idx = 0;
    while (1) {
        Token tok = read_token();
        if (is_punct(tok, '}')) break;
        unget_token(tok);

        // 检查是否为指定初始化器 .field = value（支持 .a.b = val 嵌套）
        if(is_punct(tok, '.')) {
            read_token(); // 消费 '.'
            tok = read_token();
            if(get_ttype(tok) != TTYPE_IDENT)
                error("Expected identifier for struct: %s, but got: %s", ctype_to_string(ctype), token_to_string(tok));
            idx = struct_field_index(ctype, get_ident(tok));
            // 获取该字段的类型
            Iter field_it = list_iter(ctype->fields->list);
            Ctype *field_type = NULL;
            for (int i = 0; i <= idx && !iter_end(field_it); i++) {
                DictEntry *e = iter_next(&field_it);
                if (i == idx) field_type = dict_get(ctype->fields, e->key);
            }
            // 根据字段类型选择合适的初始化读取方式
            Ast *var;
            if (is_punct(peek_token(), '.') && field_type && field_type->type == CTYPE_STRUCT) {
                /* 嵌套 designated initializer: .a.b = val
                 * 把剩余的 .b = val 包装成 { .b = val } 喂给递归 */
                /* 通过 put-back 技巧构造 { .b = val } 伪 token 流 */
                /* 先收集直到 ',' 或 '}' 的所有 token，然后包一层 {} 放回 */
                /* 简单实现：记录子字段 idx，递归调用 */
                /* 构造一个新的 struct init list */
                List *sub_initlist = init_empty_struct_init(field_type);
                /* 解析子字段链 */
                Ctype *cur_type = field_type;
                List *cur_list = sub_initlist;
                int sub_idx = -1;
                while (is_punct(peek_token(), '.')) {
                    read_token(); /* consume '.' */
                    Token sub_field_tok = read_token();
                    if (get_ttype(sub_field_tok) != TTYPE_IDENT)
                        error("Expected field name in designator");
                    sub_idx = struct_field_index(cur_type, get_ident(sub_field_tok));
                    /* 如果还有更多 '.'，继续深入 */
                    if (is_punct(peek_token(), '.')) {
                        /* 取子字段类型继续 */
                        Iter si = list_iter(cur_type->fields->list);
                        Ctype *sub_ft = NULL;
                        for (int i = 0; i <= sub_idx && !iter_end(si); i++) {
                            DictEntry *e2 = iter_next(&si);
                            if (i == sub_idx) sub_ft = dict_get(cur_type->fields, e2->key);
                        }
                        if (!sub_ft || sub_ft->type != CTYPE_STRUCT)
                            error("Cannot descend into non-struct field");
                        /* 创建子字段的 initlist */
                        List *new_sub = init_empty_struct_init(sub_ft);
                        list_set(cur_list, sub_idx, ast_struct_init(sub_ft, new_sub));
                        cur_type = sub_ft;
                        cur_list = new_sub;
                    }
                }
                expect('=');
                /* 读取最终值 */
                Ast *leaf_val = read_expr();
                if (sub_idx >= 0)
                    list_set(cur_list, sub_idx, leaf_val);
                var = ast_struct_init(field_type, sub_initlist);
            } else {
                expect('=');
                if (field_type && field_type->type == CTYPE_STRUCT) {
                    /* designated init: 可以是 {...} 也可以是结构体表达式 */
                    if (is_punct(peek_token(), '{')) {
                        var = read_decl_struct_init(field_type);
                    } else {
                        var = read_expr();
                    }
                } else if (field_type && field_type->type == CTYPE_ARRAY) {
                    if (is_punct(peek_token(), '{'))
                        var = read_decl_array_init_recurse(field_type);
                    else
                        var = read_expr();
                } else {
                    var = read_expr();
                }
            }
            list_set(initlist, idx, var);
            if (ctype->is_union) {
                idx = list_len(initlist);
                it = list_iter(initlist);
                while (!iter_end(it)) iter_next(&it);
            } else {
                idx++;
                it = list_iter(initlist);
                for (int i = 0; i < idx && !iter_end(it); i++)
                    iter_next(&it);
            }
            tok = read_token();
        } else {
            // 顺序初始化
            if(iter_end(it))
                error("Expected value for struct: %s, out of range", ctype_to_string(ctype));
            // 获取当前字段的类型
            Iter field_it = list_iter(ctype->fields->list);
            Ctype *field_type = NULL;
            int current_idx = 0;
            for (; !iter_end(field_it) && current_idx <= idx; current_idx++) {
                DictEntry *e = iter_next(&field_it);
                if (current_idx == idx) field_type = dict_get(ctype->fields, e->key);
            }
            // 根据字段类型选择合适的初始化读取方式
            Ast *v;
            if (field_type && field_type->type == CTYPE_STRUCT) {
                /* 允许标量值直接初始化 struct（brace elision），也允许 {…} */
                if (is_punct(peek_token(), '{')) {
                    v = read_decl_struct_init(field_type);
                } else if (is_punct(peek_token(), '(')) {
                    /* 可能是复合字面量 (type){...} 或强制转换表达式，用 read_expr */
                    Token t1 = read_token(); /* '(' */
                    Token t2 = read_token(); /* typename or expr */
                    unget_token(t2);
                    unget_token(t1);
                    if (is_type_keyword(t2))
                        v = read_expr(); /* 复合字面量或 cast */
                    else
                        v = read_decl_struct_init_elide(field_type);
                } else {
                    v = read_decl_struct_init_elide(field_type);
                }
            } else if (field_type && field_type->type == CTYPE_ARRAY) {
                /* 允许标量值 brace-elide 填充 array 字段，也允许 {…} */
                if (is_punct(peek_token(), '{'))
                    v = read_decl_array_init_recurse(field_type);
                else {
                    /* brace elision: 从当前 token 流读取值填充数组，遇到 '}' 停止 */
                    List *alist = make_list();
                    int n = array_n_elts(field_type);
                    while (list_len(alist) < n && !is_punct(peek_token(), '}')) {
                        list_push(alist, read_expr());
                        if (is_punct(peek_token(), ','))
                            read_token();
                        else
                            break;
                    }
                    while (list_len(alist) < n)
                        list_push(alist, make_zero_init(field_type->ptr));
                    v = ast_array_init(alist);
                }
            } else {
                /* 标量字段也可能有 {expr} 初始化（C99 允许）
                 * 但若 {…} 内含多个值（多值 brace），说明这是后续聚合字段的初始化器，
                 * 当前标量字段应用 0 初始化，不消耗 {…} */
                if (is_punct(peek_token(), '{') && !brace_has_comma()) {
                    read_token(); /* consume '{' */
                    v = read_expr();
                    expect('}');
                } else {
                    v = read_expr();
                }
            }
            list_set(initlist, idx, v);
            iter_next(&it);
            idx++;
            tok = read_token();
            if(iter_end(it) && !is_punct(tok, '}') && !is_punct(tok, ','))
                error("Expected value for struct: %s, out of range", ctype_to_string(ctype));
        }

        if (!is_punct(tok, ',')) unget_token(tok);
    }

    return ast_struct_init(ctype, initlist);
}

/* read_decl_struct_init_elide: brace elision 版本，
 * 不消耗 {}，从外层 token 流顺序读取值填充 struct 字段（C99 brace elision）。
 * 只读取恰好能填满 struct 各字段的 token，字段间的 ',' 会被消耗。
 * 停止时 token 流停在下一个 ',' 或 '}' 的**前面**（不消耗它）。 */
static Ast *read_decl_struct_init_elide(Ctype *ctype)
{
    List *initlist = init_empty_struct_init(ctype);
    if (list_len(initlist) == 0)
        return ast_struct_init(ctype, initlist);
    Iter it = list_iter(ctype->fields->list);
    int idx = 0;
    /* union 只初始化第一个字段 */
    int field_count = ctype->is_union ? 1 : list_len(ctype->fields->list);
    while (idx < field_count) {
        Token peek = peek_token();
        /* 遇到外层 '}' 说明初始化值用完了，停止 */
        if (is_punct(peek, '}'))
            break;
        DictEntry *e = iter_next(&it);
        Ctype *field_type = dict_get(ctype->fields, e->key);
        Ast *v;
        if (field_type->type == CTYPE_STRUCT) {
            if (is_punct(peek_token(), '{'))
                v = read_decl_struct_init(field_type);
            else
                v = read_decl_struct_init_elide(field_type);
        } else if (field_type->type == CTYPE_ARRAY) {
            if (is_punct(peek_token(), '{'))
                v = read_decl_array_init_recurse(field_type);
            else {
                /* brace elision: 从 token 流读取 n 个元素（遇到 '}' 停止）*/
                List *alist = make_list();
                int n = array_n_elts(field_type);
                while (list_len(alist) < n && !is_punct(peek_token(), '}')) {
                    list_push(alist, read_expr());
                    if ((int)list_len(alist) < n) {
                        /* 还有更多元素要读，消耗 ',' */
                        if (is_punct(peek_token(), ','))
                            read_token();
                        else
                            break;
                    }
                }
                while (list_len(alist) < n)
                    list_push(alist, make_zero_init(field_type->ptr));
                v = ast_array_init(alist);
            }
        } else {
            if (is_punct(peek_token(), '{') && !brace_has_comma()) {
                read_token(); /* consume '{' */
                v = read_expr();
                expect('}');
            } else {
                v = read_expr();
            }
        }
        list_set(initlist, idx, v);
        idx++;
        /* 如果还有更多字段，消耗字段间的 ',' */
        if (idx < field_count) {
            if (is_punct(peek_token(), ','))
                read_token();
            else
                break; /* 没有更多值，停止 */
        }
    }
    return ast_struct_init(ctype, initlist);
}

static char *read_struct_union_enum_tag(void)
{
    /* 先跳过可能的 __attribute__((xxx)) */
    while (1) {
        Token t = read_token();
        if (get_ttype(t) == TTYPE_IDENT && is_ident(t, "__attribute__")) {
            skip_attribute();
        } else {
            unget_token(t);
            break;
        }
    }
    Token tok = read_token();
    if (get_ttype(tok) == TTYPE_IDENT && !is_ident(tok, "__attribute__"))
        return get_ident(tok);
    unget_token(tok);
    return NULL;
}

static Dict *read_struct_union_fields(bool is_struct_type)
{
    Dict *r = make_dict(NULL);
    expect('{');
    int bit_offset = 0;
    while (1) {
        if (!is_type_keyword(peek_token()))
            break;
        
        // 读取类型说明符
        Ctype *base_ctype = read_decl_spec();

        /* 匿名 struct/union: 紧跟 ';' 表示匿名内嵌，把其字段提升到当前 dict */
        if (is_punct(peek_token(), ';')) {
            read_token(); /* consume ';' */
            if (base_ctype->type == CTYPE_STRUCT) {
                /* 把匿名结构体/联合体的字段提升 */
                if (base_ctype->fields) {
                    for (Iter it = list_iter(dict_keys(base_ctype->fields)); !iter_end(it);) {
                        char *fname = iter_next(&it);
                        Ctype *ftype = dict_get(base_ctype->fields, fname);
                        if (base_ctype->is_union) {
                            /* 匿名 union 字段提升：克隆并用 offset=-1 标记为 union 组成员
                             * 供 read_struct_def 分配共同的 offset */
                            Ctype *copy = make_struct_field_type(ftype, -1, 0, 0);
                            dict_put(r, fname, copy);
                        } else {
                            dict_put(r, fname, ftype);
                        }
                    }
                }
            }
            continue;
        }

        /* 支持多变量声明: int i, j, k; */
        do {
            Ctype *ctype = base_ctype;
            /* 消耗可能的逗号（第二次以后） */
            Token name = read_token();

            // 检查是否是函数指针语法: type (*name)(params)
            if (is_punct(name, '(')) {
                Token next_tok = read_token();
                if (is_punct(next_tok, '*')) {
                    // 函数指针: type (*name)(params)
                    name = read_token();
                    if (get_ttype(name) != TTYPE_IDENT)
                        error("Identifier expected in function pointer, but got %s", token_to_string(name));
                    expect(')');
                    expect('(');
                    read_func_ptr_params();
                    ctype = make_ptr_type(ctype);
                } else {
                    unget_token(next_tok);
                    error("Identifier expected, but got %s", token_to_string(name));
                }
            } else if (is_punct(name, '*')) {
                /* 指针字段: int *p; */
                ctype = make_ptr_type(ctype);
                name = read_token();
                if (get_ttype(name) != TTYPE_IDENT)
                    error("Identifier expected, but got %s", token_to_string(name));
            } else if (get_ttype(name) != TTYPE_IDENT) {
                error("Identifier expected, but got %s", token_to_string(name));
            }

            // 读取数组维度
            ctype = read_array_dimensions(ctype);

            int bit_size = 0;
            Token tok = peek_token();
            if (is_struct_type && is_punct(tok, ':')) {
                read_token();
                Ast *bit_ast = read_expr();
                if (!is_inttype(bit_ast->ctype))
                    error("Bit field need int type, but got %s", ctype_to_string(bit_ast->ctype));
                bit_size = eval_intexpr(bit_ast);
            }

            dict_put(r, get_ident(name), make_struct_field_type(ctype, 0, bit_offset, bit_size));
            bit_offset += bit_size;

            tok = read_token();
            if (is_punct(tok, ';'))
                break;
            if (!is_punct(tok, ','))
                error("',' or ';' expected in struct field, but got %s", token_to_string(tok));
        } while (1);
    }
    expect('}');
    return r;
}

static Ctype *read_union_def(void)
{
    char *tag = read_struct_union_enum_tag();
    Ctype *ctype = tag ? dict_get(union_defs, tag) : NULL;
    if (ctype) {
        /* 如果下面没有 '{'，这只是对已知 union 的引用 */
        if (!is_punct(peek_token(), '{'))
            return ctype;
        /* 否则是重新定义（完善不完整类型） */
    }

    /* 如果没有 '{'，则是前向声明 */
    if (!is_punct(peek_token(), '{')) {
        if (tag && !ctype) {
            Ctype *fwd = malloc(sizeof(Ctype));
            memset(fwd, 0, sizeof(Ctype));
            fwd->type = CTYPE_STRUCT;
            fwd->fields = make_dict(NULL);
            fwd->size = 0;
            fwd->is_union = true;
            fwd->tag = tag;
            dict_put(union_defs, tag, fwd);
            ctype = fwd;
        }
        if (!ctype)
            error("Type expected, but got %s", token_to_string(peek_token()));
        return ctype;
    }

    /* 在解析字段前，先注册占位符以支持自引用 */
    Ctype *placeholder = NULL;
    if (tag) {
        if (!ctype) {
            placeholder = malloc(sizeof(Ctype));
            memset(placeholder, 0, sizeof(Ctype));
            placeholder->type = CTYPE_STRUCT;
            placeholder->is_union = true;
            placeholder->fields = make_dict(NULL);
            placeholder->size = 0;
            placeholder->tag = tag;
            dict_put(union_defs, tag, placeholder);
        } else {
            placeholder = ctype; /* 可能是前向声明的占位符 */
            if (!placeholder->tag) placeholder->tag = tag;
        }
    }

    Dict *fields = read_struct_union_fields(false);
    int maxsize = 0;
    for (Iter i = list_iter(dict_values(fields)); !iter_end(i);) {
        Ctype *fieldtype = iter_next(&i);
        maxsize = (maxsize < fieldtype->size) ? fieldtype->size : maxsize;
    }

    if (placeholder) {
        /* 原地更新占位符，使所有持有该指针的引用（如 typedef 的 ptr）都能正确看到字段 */
        placeholder->fields = fields;
        placeholder->size = maxsize;
        return placeholder;
    }

    Ctype *r = make_struct_type(fields, maxsize, true);
    return r;
}

static Ctype *read_struct_def(void)
{
    char *tag = read_struct_union_enum_tag();
    Ctype *ctype = tag ? dict_get(struct_defs, tag) : NULL;
    if (ctype) {
        /* 如果已注册的类型已有字段（完整类型），直接返回 */
        /* 如果是前向声明（没有字段），也直接返回占位符（字段后续填充） */
        Token next = peek_token();
        if (!is_punct(next, '{'))
            return ctype; /* 纯引用：struct S x; 或 struct S *p; */
        /* 否则是重新定义，继续解析字段（不报错，允许完成不完整类型） */
    }

    /* 如果没有 '{' 则是前向声明 struct T; 注册占位符 */
    Token next = peek_token();
    if (!is_punct(next, '{')) {
        /* 前向声明：创建不完整类型占位符 */
        if (tag && !ctype) {
            Ctype *fwd = malloc(sizeof(Ctype));
            memset(fwd, 0, sizeof(Ctype));
            fwd->type = CTYPE_STRUCT;
            fwd->fields = make_dict(NULL);
            fwd->size = 0;
            fwd->tag = tag;
            dict_put(struct_defs, tag, fwd);
            ctype = fwd;
        }
        if (!ctype)
            error("Type expected, but got %s", token_to_string(peek_token()));
        return ctype;
    }

    /* 在解析字段前，先注册占位符以支持自引用 */
    Ctype *placeholder = NULL;
    if (tag) {
        if (!ctype) {
            placeholder = malloc(sizeof(Ctype));
            memset(placeholder, 0, sizeof(Ctype));
            placeholder->type = CTYPE_STRUCT;
            placeholder->fields = make_dict(NULL);
            placeholder->size = 0;
            placeholder->tag = tag;
            dict_put(struct_defs, tag, placeholder);
        } else {
            placeholder = ctype; /* 可能是前向声明的占位符 */
            if (!placeholder->tag) placeholder->tag = tag;
        }
    }

    Dict *fields = read_struct_union_fields(true);
    int offset = 0;
    Iter i = list_iter(dict_values(fields));
    Ctype *fieldtype = NULL;
    /* union 组追踪：offset=-1 的字段表示来自匿名 union 的提升字段 */
    int union_group_base = -1;   /* 当前 union 组的 struct 偏移 */
    int union_group_maxsz = 0;   /* 当前 union 组的最大字段 size */
    for (; !iter_end(i);) {
        fieldtype = iter_next(&i);
        if (fieldtype->offset == -1) {
            /* 匿名 union 提升字段：和同组字段共享 base offset */
            if (union_group_base < 0) {
                /* 第一次遇到该 union 组：分配 base offset（以 size 对齐） */
                int size = (fieldtype->size < MAX_ALIGN) ? fieldtype->size : MAX_ALIGN;
                if (size > 0 && offset % size != 0)
                    offset += size - offset % size;
                union_group_base = offset;
                union_group_maxsz = 0;
            }
            fieldtype->offset = union_group_base;
            if (fieldtype->size > union_group_maxsz) union_group_maxsz = fieldtype->size;
        } else {
            /* 正常字段：如果有 pending union 组，先推进 offset */
            if (union_group_base >= 0) {
                offset = union_group_base + union_group_maxsz;
                union_group_base = -1;
                union_group_maxsz = 0;
            }
            int size = (fieldtype->size < MAX_ALIGN) ? fieldtype->size : MAX_ALIGN;
            if (size > 0 && offset % size != 0)
                offset += size - offset % size;
            fieldtype->offset = offset;
            offset += fieldtype->size;
        }
    }
    /* 结束时若还有 pending union 组 */
    if (union_group_base >= 0)
        offset = union_group_base + union_group_maxsz;
    if (fieldtype && fieldtype->bit_size) {
        offset = (fieldtype->bit_offset+fieldtype->bit_size)/8;
        if((fieldtype->bit_offset+fieldtype->bit_size)%8) offset++;
    }

    if (placeholder) {
        /* 原地更新占位符，使所有持有该指针的自引用都能正确看到字段 */
        placeholder->fields = fields;
        placeholder->size = offset;
        return placeholder;
    }

    Ctype *r = make_struct_type(fields, offset, false);
    return r;
}

static bool get_enum_val(char* key, int* val) {
    for (Iter i = list_iter(enum_defs->list); !iter_end(i);) {
        DictEntry *e = iter_next(&i);
        Ctype *type = e->val;
        int *v = dict_get(type->fields, key);
        if (v) {
            *val = *v;  
            return true;
        }
    }
    return false;
}

static Dict *read_enum_fields(void) 
{
    Dict *r = make_dict(NULL);
    expect('{');
    int cnt = 0;
    while (1) {
        Token name = read_token();
        if(is_punct(name, '}')) {
            unget_token(name);
            break;
        }

        if(get_ttype(name) != TTYPE_IDENT) 
            error("Enum need identify, but got %s", token_to_string(name));
        
        Token tok = read_token(); 
        if(is_punct(tok, '=')) {
            /* 枚举初始化表达式：可以是整数字面量、其他枚举常量或整数表达式 */
            Ast *val_ast = read_expr();
            int v = eval_intexpr(val_ast);
            cnt = v;
        }else {
            unget_token(tok);
        }

        int *enum_val = malloc(sizeof(int));
        *enum_val = cnt;
        dict_put(r, get_ident(name), enum_val);
        
        cnt++;
        tok = peek_token(); 
        if(is_punct(tok, '}')) break;

        expect(',');
    }
    expect('}');
    return r;
}

static Ctype *read_enum_def(void)
{
    char *tag = read_struct_union_enum_tag();
    Ctype *ctype = tag ? dict_get(enum_defs, tag) : NULL;
    if (ctype) {
        /* 如果下面没有 '{'，这只是对已知枚举的引用 */
        if (!is_punct(peek_token(), '{'))
            return ctype;
    }

    /* 如果下面没有 '{'，说明是前向声明（枚举类型不完整），返回 int 作为占位 */
    if (!is_punct(peek_token(), '{')) {
        /* 前向声明枚举，允许作为指针基类型 */
        Ctype *r = make_enum_type(make_dict(NULL));
        if (tag) dict_put(enum_defs, tag, r);
        return r;
    }

    Dict *fields = read_enum_fields();
    Ctype *r = make_enum_type(fields);
    if (tag) {
        r->tag = strdup(tag);
        dict_put(enum_defs, tag, r);
    } else {
        /* 匿名枚举：也需要存入 enum_defs 以便 get_enum_val 查找枚举常量 */
        char anon_tag[32];
        snprintf(anon_tag, sizeof(anon_tag), "__anon_enum_%d", labelseq++);
        dict_put(enum_defs, strdup(anon_tag), r);
    }
    return r;
}

static int read_decl_ctype_attr(Token tok, int *attr_out) {
    if(get_ttype(tok) != TTYPE_IDENT) return 0;

    union { CtypeAttr c_attr; int i_attr; }attr = {0};
    bool is_signed_kw = false;
    
    if(is_ident(tok, "const"))          { attr.c_attr.ctype_const = 1; }
    else if(is_ident(tok, "volatile"))  { attr.c_attr.ctype_volatile = 1; }
    else if (is_ident(tok, "restrict")) { attr.c_attr.ctype_restrict = 1; }
    else if (is_ident(tok, "static"))   { attr.c_attr.ctype_static = 1; }
    else if (is_ident(tok, "extern"))   { attr.c_attr.ctype_extern = 1; }
    else if (is_ident(tok, "unsigned")) { attr.c_attr.ctype_unsigned = 1; }
    else if (is_ident(tok, "signed"))   { is_signed_kw = true; /* signed 是默认，无需设置属性位，但要被识别 */ }
    else if (is_ident(tok, "register")) { attr.c_attr.ctype_register = 1; }
    else if (is_ident(tok, "typedef"))  { attr.c_attr.ctype_typedef = 1; }
    else if (is_ident(tok, "inline"))   { attr.c_attr.ctype_inline = 1; }
    else if (is_ident(tok, "noreturn")) { attr.c_attr.ctype_noreturn = 1; }
    else if (IS_C51_KW(tok, "data"))  { attr.c_attr.ctype_c51_data = 1; }
    else if (IS_C51_KW(tok, "idata")) { attr.c_attr.ctype_c51_idata = 1; }
    else if (IS_C51_KW(tok, "xdata")) { attr.c_attr.ctype_c51_xdata = 1; }
    else if (IS_C51_KW(tok, "code"))  { attr.c_attr.ctype_c51_code = 1; }
    else if (IS_C51_KW(tok, "bdata")) { attr.c_attr.ctype_c51_bdata = 1; }
    else if (IS_C51_KW(tok, "pdata")) { attr.c_attr.ctype_c51_pdata = 1; }
    else if (IS_C51_KW(tok, "far"))   { attr.c_attr.ctype_c51_far = 1; }
    else if (IS_C51_KW(tok, "near"))  { attr.c_attr.ctype_c51_near = 1; }
    else if (IS_C51_KW(tok, "reentrant") || IS_C51_KW(tok, "interrupt") ||
             IS_C51_KW(tok, "using")) {
        *attr_out |= 0;
        return -1;
    }
    else                               { return 0; /* 不是属性关键字 */ }

    *attr_out |= attr.i_attr;
    return is_signed_kw ? -1 : attr.i_attr ? attr.i_attr : -1;
}

/* __attribute__ 解析的全局中间结果 */
static int g_attribute_extra_attr = 0;
static int g_attribute_c51_interrupt = -1;
static int g_attribute_c51_using = -1;

/* 跳过 __attribute__((xxx))；同时识别 c51_interrupt(N) 和 c51_using(M) */
static void skip_attribute(void)
{
    /* 期望 __attribute__ 后面跟着 ((...)) */
    Token next = read_token();
    if (!is_punct(next, '(')) { unget_token(next); return; }

    int depth = 1;
    /* 标记是否是 c51_interrupt(N) 或 c51_using(N) 的上下文 */
    while (depth > 0) {
        Token t = read_token();
        if (get_ttype(t) == TTYPE_NULL) break;
        if (is_punct(t, '(')) {
            depth++;
        } else if (is_punct(t, ')')) {
            depth--;
        } else if (get_ttype(t) == TTYPE_IDENT) {
            char *id = get_ident(t);
            if (!strcmp(id, "c51_interrupt") || !strcmp(id, "__c51_interrupt")) {
                /* c51_interrupt(N) — 消耗 '(' N ')' */
                Token paren = read_token();
                if (is_punct(paren, '(')) {
                    Token num = read_token();
                    if (get_ttype(num) == TTYPE_NUMBER) {
                        g_attribute_c51_interrupt = atoi(get_number(num));
                    }
                    expect(')');
                }
            } else if (!strcmp(id, "c51_using") || !strcmp(id, "__c51_using")) {
                /* c51_using(N) */
                Token paren = read_token();
                if (is_punct(paren, '(')) {
                    Token num = read_token();
                    if (get_ttype(num) == TTYPE_NUMBER) {
                        g_attribute_c51_using = atoi(get_number(num));
                    }
                    expect(')');
                }
            }
        }
    }
}

static Ctype *read_decl_spec(void)
{
    Token tok = read_token();
    int attr = 0;
    g_last_c51_decl_kind = C51_DECL_NONE;

    /* 循环消耗：属性关键字 + __attribute__ */
    while (1) {
        if (get_ttype(tok) == TTYPE_IDENT && is_ident(tok, "__attribute__")) {
            skip_attribute();
            tok = read_token();
            continue;
        }
        /* MCS51/C51: interrupt N 和 using N 跟随一个数字，需要消耗该数字 */
        if (get_ttype(tok) == TTYPE_IDENT &&
            (IS_C51_KW(tok, "interrupt") || IS_C51_KW(tok, "using"))) {
            Token num = read_token();
            if (get_ttype(num) != TTYPE_NUMBER) unget_token(num);
            tok = read_token();
            continue;
        }
        if (!read_decl_ctype_attr(tok, &attr)) break;
        tok = read_token();
    }

    /* 处理 long long → long, long double → double */
    if (get_ttype(tok) == TTYPE_IDENT && is_ident(tok, "long")) {
        Token tok2 = read_token();
        if (get_ttype(tok2) == TTYPE_IDENT && is_ident(tok2, "long")) {
            /* MCS51: long long 不支持，直接报错 */
            if (g_target_platform == PLAT_MCS51)
                error("long long is not supported on MCS-51 target");
            /* long long → 等价 long，再跳过可能的 int */
            Token tok3 = peek_token();
            if (get_ttype(tok3) == TTYPE_IDENT && is_ident(tok3, "int"))
                read_token(); /* consume 'int' */
        } else if (get_ttype(tok2) == TTYPE_IDENT && is_ident(tok2, "double")) {
            /* long double → 等价 double */
            tok = tok2; /* tok 变为 "double"，get_ctype(double) 返回 ctype_double */
        } else if (get_ttype(tok2) == TTYPE_IDENT && is_ident(tok2, "int")) {
            /* long int → long，跳过 int */
        } else {
            unget_token(tok2);
        }
    }
       
    Ctype *ctype = NULL;
    if (is_ident(tok, "struct")) {
        ctype = read_struct_def();
    } else if (is_ident(tok, "union")) {
        ctype = read_union_def();
    } else if (is_ident(tok, "enum")) {
        ctype = read_enum_def();
    } else if (IS_C51_KW(tok, "sfr")) {
        g_last_c51_decl_kind = C51_DECL_SFR;
        { union { CtypeAttr c_attr; int i_attr; }a = {0}; a.c_attr.ctype_c51_sfr = 1; attr |= a.i_attr; }
        ctype = ctype_char;
    } else if (IS_C51_KW(tok, "sfr16")) {
        g_last_c51_decl_kind = C51_DECL_SFR16;
        { union { CtypeAttr c_attr; int i_attr; }a = {0}; a.c_attr.ctype_c51_sfr16 = 1; attr |= a.i_attr; }
        ctype = ctype_short;
    } else if (IS_C51_KW(tok, "sbit")) {
        g_last_c51_decl_kind = C51_DECL_SBIT;
        { union { CtypeAttr c_attr; int i_attr; }a = {0}; a.c_attr.ctype_c51_sbit = 1; attr |= a.i_attr; }
        ctype = ctype_char;
    } else if (IS_C51_KW(tok, "bit")) {
        g_last_c51_decl_kind = C51_DECL_BIT;
        { union { CtypeAttr c_attr; int i_attr; }a = {0}; a.c_attr.ctype_c51_bit = 1; attr |= a.i_attr; }
        ctype = ctype_char;
    } else {
        ctype = get_ctype(tok);
    }

    if (!ctype && get_ttype(tok) == TTYPE_IDENT && !get_attr(attr).ctype_typedef) 
        ctype = dict_get(typedefenv, get_ident(tok));

    /* 处理 noreturn 属性关键字 */
    if (!ctype) {
        if (get_ttype(tok) == TTYPE_IDENT && is_ident(tok, "__attribute__")) {
            /* 跳过，已在前面循环处理 */
        }
    }
    if (!ctype) {
        CtypeAttr a = get_attr(attr);
        if (a.ctype_unsigned) {
            /* unsigned 单独使用 → unsigned int，回退当前 token */
            unget_token(tok);
            ctype = ctype_int;
        }
    }

    /* MCS51 不支持的 C11 关键字显式报错 */
    if (!ctype && get_ttype(tok) == TTYPE_IDENT) {
        char *kw = get_ident(tok);
        if (!strcmp(kw, "_Complex") || !strcmp(kw, "_Imaginary"))
            error("_Complex and _Imaginary types are not supported on MCS-51 target");
        if (!strcmp(kw, "_Atomic"))
            error("_Atomic is not supported on MCS-51 target (no threading)");
        if (!strcmp(kw, "_Thread_local"))
            error("_Thread_local is not supported on MCS-51 target (no threading)");
    }

    if (!ctype) {
        /* 如果标识符未识别为类型，检查是否可能是隐式 int（如 "main()" 是 K&R 风格） */
        if (get_ttype(tok) == TTYPE_IDENT) {
            Token nxt = peek_token();
            if (is_punct(nxt, '(')) {
                /* 例如 main(...) — 隐式 int */
                unget_token(tok);
                ctype = ctype_int;
                attr = 0; /* 无属性 */
            }
        }
    }

    if (!ctype) 
        error("Type expected, but got %s", token_to_string(tok));
        
    while (1) {
        tok = read_token();
        if (!is_punct(tok, '*')) {
            /* 消耗指针后的修饰符/属性 */
            while (1) {
                if (get_ttype(tok) == TTYPE_IDENT && is_ident(tok, "__attribute__")) {
                    skip_attribute();
                    tok = read_token();
                    continue;
                }
                /* C51 内存修饰词（data/xdata/idata/code 等）与变量名冲突时，
                 * 用前瞻决定：若后面紧跟 '=' ',' ';' '[' ')' 则是变量名，停止消耗 */
                if (get_ttype(tok) == TTYPE_IDENT && is_c51_decl_modifier(tok)) {
                    Token nxt = peek_token();
                    if (is_punct(nxt, '=') || is_punct(nxt, ';') ||
                        is_punct(nxt, ',') || is_punct(nxt, '[') ||
                        is_punct(nxt, ')') || is_punct(nxt, '{')) {
                        break; /* tok 是变量名，不作修饰词消耗 */
                    }
                }
                if (!read_decl_ctype_attr(tok, &attr)) break;
                tok = read_token();
            }
            unget_token(tok);
            return clone_ctype_with_attr(ctype, attr);
        }
        /* 将前面的类型限定词（const/volatile 等）应用到当前类型再包装指针，
         * 确保 const int *p 被解析为「指向 const int 的指针」而非「const 指针指向 int」 */
        if (attr) {
            ctype = clone_ctype_with_attr(ctype, attr);
            attr = 0;
        }
        ctype = make_ptr_type(ctype);
    }
}

static Ast *read_decl_init_val(Ast *var, bool consume_semicolon)
{
    if (var->ctype->type == CTYPE_ARRAY) {
        Ast *init = read_decl_array_init_recurse(var->ctype);
        int len = (init->type == AST_STRING) ? strlen(init->sval) + 1
                                             : list_len(init->arrayinit);
        if (var->ctype->len == -1) {
            var->ctype->len = len;
            var->ctype->size = len * var->ctype->ptr->size;
        } else if (var->ctype->len < len) {
            /* 只有当初始化值比数组长时才报错（字符串可以比数组短，用0填充） */
            error("Invalid array initializer: expected %d items but got %d",
                  var->ctype->len, len);
        }
        if (consume_semicolon) expect(';');
        if (var->type == AST_GVAR) var->ginit = init;
        return ast_decl(var, init);
    } else if(var->ctype->type == CTYPE_STRUCT) {
        Ast *init;
        if (is_punct(peek_token(), '{')) {
            init = read_decl_struct_init(var->ctype);
        } else {
            /* struct s t = func() 形式：函数调用或表达式赋值 */
            init = read_expr();
        }
        if (consume_semicolon) expect(';');
        if (var->type == AST_GVAR) var->ginit = init;
        return ast_decl(var, init);
    } else if(var->ctype->type == CTYPE_PTR) {
        Ast *init = read_expr();
        if (consume_semicolon) expect(';');
        if(init->type == AST_ADDR) return ast_decl(var, init);
        
        // 允许数组名直接赋值给指针（数组退化为指向首元素的指针）
        if(init->ctype->type == CTYPE_ARRAY) {
            // 创建新的指针类型用于类型检查，不修改原始数组变量的 ctype
            // 使用返回的 ast_decl 时 init->ctype 仍保持原数组类型
            return ast_decl(var, init);
        }
        
        // 允许函数名赋值给函数指针
        if(init->type == AST_FUNC_DECL || init->type == AST_FUNC_DEF) {
            // 函数名作为指针，使用函数标签作为地址
            return ast_decl(var, init);
        }

        // 允许指针赋值给指针（局部变量无需编译时求值）
        if (init->ctype->type == CTYPE_PTR) {
            if (var->type == AST_GVAR) var->ginit = init;
            return ast_decl(var, init);
        }

        // 全局指针可能需要编译时常量；对于字面量 0/NULL 走 eval_intexpr
        if (var->type == AST_GVAR && is_inttype(init->ctype)) {
            /* NULL / 0 赋给全局指针 */
            if (var->type == AST_GVAR) var->ginit = init;
        }
        return ast_decl(var, init);
    }

    Ast *init = read_expr();
    if (consume_semicolon) expect(';');

    if (var->type == AST_GVAR) {
        if (is_inttype(var->ctype)) {
            CtypeAttr a = get_attr(var->ctype->attr);
            if (!(var->ctype->type == CTYPE_BOOL && a.ctype_register)) {
                init = ast_inttype(ctype_int, eval_intexpr(init));
            }
        } else {
            init = ast_double(eval_floatexpr(init));
        }
        var->ginit = init;
    }

    if (init->type == AST_LITERAL && is_inttype(init->ctype) && is_inttype(var->ctype))
        init->ctype = var->ctype;

    return ast_decl(var, init);
}

// 读取函数指针参数列表并跳过
static void read_func_ptr_params(void)
{
    // 读取参数列表，只处理到匹配的 ')'
    int depth = 1;
    while (depth > 0) {
        Token t = read_token();
        if (is_punct(t, '('))
            depth++;
        else if (is_punct(t, ')'))
            depth--;
        else if (get_ttype(t) == TTYPE_NULL)
            error("Unexpected end of input in function pointer declaration");
    }
}

static Ctype *read_array_dimensions_int(Ctype *basetype)
{
    Token tok = read_token();
    if (!is_punct(tok, '[')) {
        unget_token(tok);
        return NULL;
    }
    int dim = -1;
    /* 跳过数组参数中的类型修饰符: const, static, volatile, restrict */
    while (1) {
        Token q = peek_token();
        if (get_ttype(q) == TTYPE_IDENT &&
            (is_ident(q, "const") || is_ident(q, "static") ||
             is_ident(q, "volatile") || is_ident(q, "restrict"))) {
            read_token();
            continue;
        }
        break;
    }
    if (!is_punct(peek_token(), ']') && !is_punct(peek_token(), '*')) {
        Ast *size = read_expr();
        /* 尝试常量求值；VLA（变量长度数组）时 eval_intexpr 可能失败，
         * 在这种情况下用 -1 标记 VLA */
        if (size->type == AST_LITERAL && is_inttype(size->ctype)) {
            dim = size->ival;
        } else {
            /* 尝试常量折叠，失败则作为 VLA 处理 */
            int ok = 0;
            /* 先检查是否是可求值的常量表达式 */
            switch (size->type) {
            case AST_LITERAL: case '+': case '-': case '*': case '/':
            case '&': case '|': case '^': case PUNCT_LSHIFT: case PUNCT_RSHIFT:
            case PUNCT_LOGAND: case PUNCT_LOGOR: case '!': case '~':
            case PUNCT_EQ: case PUNCT_NE: case PUNCT_GE: case PUNCT_LE:
            case '<': case '>': case AST_CAST:
                ok = 1; break;
            default: break;
            }
            if (ok) dim = eval_intexpr(size);
            else dim = -1; /* VLA */
        }
    } else if (is_punct(peek_token(), '*')) {
        /* VLA 变长数组标记 [*] */
        read_token();
    }
    expect(']');
    Ctype *sub = read_array_dimensions_int(basetype);
    if (sub) {
        if (sub->len == -1 && dim == -1)
            error("Array size is not specified");
        return make_array_type(sub, dim);
    }
    return make_array_type(basetype, dim);
}

static Ctype *read_array_dimensions(Ctype *basetype)
{
    Ctype *ctype = read_array_dimensions_int(basetype);
    return ctype ? ctype : basetype;
}

static Ast *read_decl_init(Ast *var)
{
    Token tok = read_token();
    if (is_punct(tok, '='))
        return read_decl_init_val(var, true);
    if (var->ctype->len == -1 && var->ctype->type == CTYPE_ARRAY) {
        /* VLA（变长数组）或 extern 声明不需要初始化 */
        unget_token(tok);
        expect(';');
        return ast_decl(var, NULL);
    }
    if (var->ctype->len == -1)
        error("Missing array initializer");
    unget_token(tok);
    expect(';');
    return ast_decl(var, NULL);
}


static Ctype *read_decl_int(Token *name)
{
    Ctype *ctype = read_decl_spec();
    {
        int extra_attr = skip_c51_decl_modifiers();
        if (extra_attr) ctype = clone_ctype_with_attr(ctype, ctype->attr | extra_attr);
    }
    *name = read_token();
    
    // 检查是否是函数指针语法: type (*name)(params)
    if (is_punct(*name, '(')) {
        Token next_tok = read_token();
        if (is_punct(next_tok, '*')) {
            // 函数指针: type (*name)(params) 或函数指针数组: type (*name[N])(params)
            *name = read_token();
            /* 处理嵌套函数指针：(* (*p)(...))(...) 形式，* 后面又是 ( */
            if (is_punct(*name, '(')) {
                /* 内层是 (*p) 形式：读 * 和 p */
                next_tok = read_token(); /* 复用 next_tok */
                if (is_punct(next_tok, '*')) {
                    *name = read_token(); /* p */
                    if (get_ttype((*name)) != TTYPE_IDENT)
                        error("Identifier expected in nested function pointer, but got %s", token_to_string(*name));
                    expect(')'); /* close (*p) */
                } else {
                    unget_token(next_tok);
                    read_func_ptr_params();
                }
            }
            if (get_ttype((*name)) != TTYPE_IDENT)
                error("Identifier expected in function pointer, but got %s", token_to_string((*name)));
            /* 处理可能的数组维度 (*name[N]) */
            if (is_punct(peek_token(), '[')) {
                read_token(); /* consume '[' */
                if (!is_punct(peek_token(), ']')) {
                    Ast *dim_expr = read_expr();
                    (void)dim_expr;
                }
                expect(']');
            }
            /* 处理返回函数指针的函数：(*f1(params))(ret_params)
             * 此时 name = f1，peek 是 ( —— f1 的参数列表 */
            if (is_punct(peek_token(), '(')) {
                /* 跳过 f1 的参数列表 */
                read_token(); /* consume '(' */
                read_func_ptr_params();
                expect(')'); /* consume ')' of (*f1(params)) */
                /* 跳过返回类型的参数列表 (ret_params) */
                expect('(');
                read_func_ptr_params();
                /* 这是一个返回函数指针的函数，ctype 是最终返回值的类型 */
                return make_ptr_type(ctype);
            }
            expect(')');
            expect('(');
            // 读取参数列表
            read_func_ptr_params();
            // 创建函数指针类型：作为指向函数的指针
            // 这里我们使用普通指针类型，运行时通过名称调用
            return make_ptr_type(ctype);
        } else {
            // 不是函数指针，回退token
            unget_token(next_tok);
            // 继续正常处理，但此时*name是'('
            error("Identifier expected, but got %s", token_to_string(*name));
        }
    }
    
    if (get_ttype((*name)) != TTYPE_IDENT)
        error("Identifier expected, but got %s", token_to_string(*name));
    return read_array_dimensions(ctype);
}

// 读取单个变量的初始化（不处理分号，由上层统一处理）
static Ast *read_decl_init_single(Ast *var)
{
    Token tok = read_token();
    if (is_punct(tok, '='))
        return read_decl_init_val(var, false);
    if (var->ctype->len == -1 && var->ctype->type == CTYPE_ARRAY) {
        /* VLA 或 extern 声明，不需要初始化 */
        unget_token(tok);
        return ast_decl(var, NULL);
    }
    if (var->ctype->len == -1)
        error("Missing array initializer");
    unget_token(tok);
    return ast_decl(var, NULL);
}

static Ast *read_global_decl_multi(Ctype *ctype, char *first_ident, Ctype *first_ctype_with_dims)
{
    List *decls = make_list();
    Ctype *base_ctype = ctype;
    char *ident = first_ident;
    Ctype *ident_ctype = first_ctype_with_dims ? first_ctype_with_dims : base_ctype;
    bool first = true;

    while (1) {
        Ctype *var_ctype;
        if (first && first_ctype_with_dims) {
            /* 第一个变量的数组维度已由上层消费，直接使用 */
            var_ctype = first_ctype_with_dims;
            first = false;
        } else {
            var_ctype = read_array_dimensions(ident_ctype);
        }
        Ast *var = ast_gvar(var_ctype, ident, false);
        Ast *decl = read_decl_init_single(var);
        list_push(decls, decl);

        Token tok = read_token();
        if (is_punct(tok, ';'))
            break;
        if (!is_punct(tok, ','))
            error("Comma expected, but got %s", token_to_string(tok));

        /* 下一个变量名前可能有 * (指针修饰符) */
        Ctype *next_ctype = base_ctype;
        Token next = read_token();
        while (is_punct(next, '*')) {
            next_ctype = make_ptr_type(next_ctype);
            next = read_token();
        }
        if (get_ttype(next) != TTYPE_IDENT)
            error("Identifier expected, but got %s", token_to_string(next));
        ident_ctype = next_ctype;
        ident = get_ident(next);
    }

    if (list_len(decls) == 1)
        return list_get(decls, 0);
    return ast_compound_stmt(decls);
}

// 读取逗号分隔的多个变量声明
static Ast *read_decl_multi(Ctype *ctype, Token first_name)
{
    List *decls = make_list();
    Ctype *base_ctype = ctype;
    Token varname = first_name;
    Ctype *var_base = base_ctype;

    /* Detect static local variable: ctype_static==1 inside a function scope */
    bool is_static_local = localenv && get_attr(ctype->attr).ctype_static;
    
    while (1) {
        if (ctype->type == CTYPE_VOID)
            error("Storage size of '%s' is not known", token_to_string(varname));
        if (have_redefine_var(get_ident(varname)))
            error("Fuction redefine local val: %s", token_to_string(varname));
        
        Ctype *var_ctype = read_array_dimensions(var_base);

        Ast *var;
        if (is_static_local) {
            /* Static local variable: allocate as a global (persistent storage),
             * but register under original name in localenv for local scope lookup.
             * Use a unique internal name to avoid collisions across functions. */
            char namebuf[128];
            snprintf(namebuf, sizeof(namebuf), "__sloc_%d", labelseq++);
            char *gname = strdup(namebuf);
            var = ast_gvar(var_ctype, gname, false);
            var->varname = gname;  /* use unique name for global storage */
            /* Also register under original name in localenv */
            dict_put(localenv, get_ident(varname), var);
        } else {
            var = ast_lvar(var_ctype, get_ident(varname));
        }

        Ast *decl = read_decl_init_single(var);
        list_push(decls, decl);
        
        Token tok = read_token();
        if (!is_punct(tok, ',')) {
            unget_token(tok);
            break;
        }
        
        /* 下一个变量名前可能有 * (指针修饰符) 或 (*name) 形式 */
        Ctype *next_ctype = base_ctype;
        varname = read_token();
        while (is_punct(varname, '*')) {
            next_ctype = make_ptr_type(next_ctype);
            varname = read_token();
        }
        /* (*p)[4] 形式：指向数组的指针 */
        if (is_punct(varname, '(')) {
            Token inner = read_token();
            if (is_punct(inner, '*')) {
                varname = read_token();
                if (get_ttype(varname) != TTYPE_IDENT)
                    error("Identifier expected in (*name), but got %s", token_to_string(varname));
                expect(')');
                /* 读取后续数组维度：(*p)[4] → p 是 ptr to array[4] of base_ctype
                 * 先读 [4] 包装 base_ctype，再取指针 */
                Ctype *arr_type = read_array_dimensions(next_ctype);
                next_ctype = make_ptr_type(arr_type);
            } else {
                unget_token(inner);
                error("Identifier expected, but got %s", token_to_string(varname));
            }
        }
        var_base = next_ctype;
        if (get_ttype(varname) != TTYPE_IDENT)
            error("Identifier expected, but got %s", token_to_string(varname));
    }
    
    expect(';');
    
    if (list_len(decls) == 1)
        return list_get(decls, 0);
    
    // 返回复合声明语句
    return ast_compound_stmt(decls);
}

static Ast *read_decl(void)
{
    Ctype *ctype = read_decl_spec();
    {
        int extra_attr = skip_c51_decl_modifiers();
        if (extra_attr) ctype = clone_ctype_with_attr(ctype, ctype->attr | extra_attr);
    }
    Token varname = read_token();

    /* 仅类型声明（如 struct T; / enum E;） */
    if (is_punct(varname, ';')) {
        return ast_compound_stmt(make_list());
    }
    
    // 检查是否是函数指针语法: type (*name)(params)
    if (is_punct(varname, '(')) {
        Token next_tok = read_token();
        if (is_punct(next_tok, '*')) {
            // 函数指针: type (*name)(params)
            varname = read_token();
            /* 处理嵌套函数指针：(* (*p)(...))(...) */
            if (is_punct(varname, '(')) {
                next_tok = read_token(); /* 复用 next_tok */
                if (is_punct(next_tok, '*')) {
                    varname = read_token(); /* p */
                    if (get_ttype(varname) != TTYPE_IDENT)
                        error("Identifier expected in nested function pointer, but got %s", token_to_string(varname));
                    expect(')'); /* close (*p) */
                } else {
                    unget_token(next_tok);
                    read_func_ptr_params();
                }
            }
            if (get_ttype(varname) != TTYPE_IDENT)
                error("Identifier expected in function pointer, but got %s", token_to_string(varname));
            /* 跳过可能的参数列表 (*p)(int a, int b) */
            if (is_punct(peek_token(), '(')) {
                expect('(');
                read_func_ptr_params();
            }
            expect(')');
            expect('(');
            // 读取参数列表
            read_func_ptr_params();
            // 创建函数指针类型
            ctype = make_ptr_type(ctype);
            return read_decl_multi(ctype, varname);
        } else {
            unget_token(next_tok);
            error("Identifier expected, but got %s", token_to_string(varname));
        }
    }
    
    if (get_ttype(varname) != TTYPE_IDENT)
        error("Identifier expected, but got %s", token_to_string(varname));

    /* 检测局部函数原型声明: int f(params); */
    if (is_punct(peek_token(), '(')) {
        char *fname = get_ident(varname);
        expect('(');
        read_func_ptr_params(); /* 消耗参数列表 */
        expect(';');
        /* 注册函数名到 functionenv（允许后续调用），除非已有定义 */
        if (!dict_get(functionenv, fname)) {
            Ast *fdecl = ast_func_decl(ctype, fname, make_list());
            dict_put(functionenv, fname, fdecl);
        }
        return ast_compound_stmt(make_list());
    }

    return read_decl_multi(ctype, varname);
}

static Ast *read_if_stmt(void)
{
    expect('(');
    Ast *cond = read_expr();
    expect(')');
    Ast *then = read_stmt();

    Token tok = read_token();
    if (get_ttype(tok) != TTYPE_IDENT || strcmp(get_ident(tok), "else")) {
        unget_token(tok);
        return ast_if(cond, then, NULL);
    }

    tok = read_token();
    if (get_ttype(tok) == TTYPE_IDENT && strcmp(get_ident(tok), "if") == 0) {
        Ast *els = read_if_stmt();
        return ast_if(cond, then, els);
    } else {
        unget_token(tok);
        Ast *els = read_stmt();
        return ast_if(cond, then, els);
    }
}

static Ast *read_switch_stmt(void)
{
    expect('(');
    Ast *ctrl = read_expr();
    expect(')');

    SwitchParseCtx ctx = {0};
    ctx.seen = make_dict(NULL);
    ctx.cases = make_list();
    ctx.default_label = NULL;
    ctx.parent = switch_ctx;
    switch_ctx = &ctx;

    Ast *body = read_stmt();

    switch_ctx = ctx.parent;
    return ast_switch(ctrl, ctx.cases, body, ctx.default_label);
}

static Ast *read_switch_case_stmt(void)
{
    SwitchParseCtx *ctx = current_switch_ctx();
    if (!ctx) error("case label not within switch");

    long low = eval_intexpr(read_expr());
    long high = low;

    Token tok = peek_token();
    if (is_punct(tok, PUNCT_ELLIPSIS)) {
        read_token();
        high = eval_intexpr(read_expr());
        if (high < low)
            error("case range end (%ld) < start (%ld)", high, low);
    }

    switch_ctx_record_case(ctx, low, high);
    expect(':');

    char *label = make_label();
    list_push(ctx->cases, make_switch_case(low, high, NULL, label));

    List *stmts = make_list();
    list_push(stmts, ast_label(label));
    Ast *stmt = read_stmt();
    if (stmt)
        list_push(stmts, stmt);
    return ast_compound_stmt(stmts);
}

static Ast *read_switch_default_stmt(void)
{
    SwitchParseCtx *ctx = current_switch_ctx();
    if (!ctx) error("default label not within switch");
    if (ctx->default_label) error("multiple default labels");

    expect(':');
    ctx->default_label = make_label();

    List *stmts = make_list();
    list_push(stmts, ast_label(ctx->default_label));
    Ast *stmt = read_stmt();
    if (stmt)
        list_push(stmts, stmt);
    return ast_compound_stmt(stmts);
}


static Ast *read_opt_decl_or_stmt(void)
{
    Token tok = read_token();
    if (is_punct(tok, ';'))
        return NULL;
    unget_token(tok);
    return read_decl_or_stmt();
}

static Ast *read_opt_expr(void)
{
    Token tok = read_token();
    if (is_punct(tok, ';'))
        return NULL;
    unget_token(tok);
    Ast *r = read_expr();
    expect(';');
    return r;
}

static Ast *read_for_stmt(void)
{
    expect('(');
    localenv = make_dict(localenv);
    Ast *init = read_opt_decl_or_stmt();
    Ast *cond = read_opt_expr();
    Ast *step = is_punct(peek_token(), ')') ? NULL : read_comma_expr();
    expect(')');
    Ast *body = read_stmt();
    localenv = dict_parent(localenv);
    return ast_for(init, cond, step, body);
}

static Ast *read_while_stmt(void)
{
    expect('(');
    Ast *cond = read_expr();          
    expect(')');                      
    Ast *body = read_stmt();          
    return ast_while(cond, body);     
}

static Ast *read_dowhile_stmt(void)
{
    Ast *body = read_stmt(); 
    Token tok = read_token();
    if(!is_ident(tok, "while"))
        error("Do while need while, but got %s", token_to_string(tok));
    expect('(');
    Ast *cond = read_expr();   
    expect(')');
    expect(';');             
    return ast_dowhile(cond, body);
}

static Ast *read_goto_stmt(void) 
{
    Token tok = read_token();
    if(get_ttype(tok) != TTYPE_IDENT)
        error("Goto need a identify, but got %s", token_to_string(tok));

    // FIXME: 应该需要检查一下是否存在标签
    expect(';'); 
    return ast_goto(get_ident(tok));
}

static Ast *read_break_stmt(void)
{
    expect(';');
    return ast_break();
}

static Ast *read_continue_stmt(void)
{
    expect(';');
    return ast_continue();
}

static Ast *read_return_stmt(void)
{
    Ast *retval = read_expr();
    expect(';');
    return ast_return(retval);
}

static Ast *read_static_assert_stmt(void)
{
    /* _Static_assert(constant-expression, string-literal); */
    expect('(');
    Ast *cond = read_expr();
    expect(',');
    Token tok = read_token();
    char *msg = NULL;
    if (get_ttype(tok) == TTYPE_STRING) {
        msg = strdup(get_strtok(tok));
    } else {
        error("Expected string literal in _Static_assert, got %s", token_to_string(tok));
    }
    expect(')');
    expect(';');
    return ast_static_assert(cond, msg);
}

static Ast *read_asm_stmt(void)
{
    /* __asm("...") 或 __asm { ... } */
    Token tok = read_token();
    char *body = NULL;

    if (is_punct(tok, '(')) {
        /* __asm("instructions") */
        tok = read_token();
        if (get_ttype(tok) == TTYPE_STRING) {
            body = strdup(get_strtok(tok));
        } else {
            error("Expected string literal in __asm(), got %s", token_to_string(tok));
        }
        expect(')');
        expect(';');  /* __asm("...") 后面需要分号 */
    } else if (is_punct(tok, '{')) {
        /* __asm { instructions } */
        String sb = make_string();
        int depth = 1;
        while (depth > 0) {
            Token t = read_token();
            if (get_ttype(t) == TTYPE_NULL) break;
            if (is_punct(t, '{')) depth++;
            else if (is_punct(t, '}')) depth--;
            else if (get_ttype(t) == TTYPE_STRING) {
                string_appendf(&sb, "%s", get_strtok(t));
            } else {
                string_appendf(&sb, "%s ", token_to_string(t));
            }
        }
        body = get_cstring(sb);
    } else {
        error("Expected '(' or '{' after __asm, got %s", token_to_string(tok));
    }

    /* GCC 扩展内联汇编检测：%0, %1 或 "r"/"m" 约束 */
    if (body && (strstr(body, "%0") || strstr(body, "%1") ||
                 strstr(body, "\"r\"") || strstr(body, "\"m\""))) {
        error("GCC extended inline asm is not supported on C51 target. "
              "Rewrite as pure C51 __asm block or a separate assembly file.");
    }
    return ast_asm(body);
}

static bool is_first = true;
static Ast *read_stmt(void)
{
    Token tok = peek_token();  
    if (current_switch_ctx()) {
        if (is_ident(tok, "case")) { read_token(); return read_switch_case_stmt(); }
        if (is_ident(tok, "default")) { read_token(); return read_switch_default_stmt(); }
    }
    if(!is_first) {
        if (is_ident(tok, "continue"))  { read_token(); return read_continue_stmt(); } 
        if (is_ident(tok, "break"))     { read_token(); return read_break_stmt(); }
    }
    is_first = false;

    // null statement: ';'
    if (is_punct(tok, ';')) {
        read_token();
        return ast_compound_stmt(make_list());
    }

    if (is_ident(tok, "if"))     { read_token(); return read_if_stmt(); }
    if (is_ident(tok, "switch")) { read_token(); return read_switch_stmt(); }
    if (is_ident(tok, "for"))    { read_token(); return read_for_stmt(); }
    if (is_ident(tok, "while"))  { read_token(); return read_while_stmt(); }
    if (is_ident(tok, "do"))     { read_token(); return read_dowhile_stmt(); }
    if (is_ident(tok, "return")) { read_token(); return read_return_stmt(); }
    if (is_ident(tok, "goto"))   { read_token(); return read_goto_stmt(); }
    if (is_ident(tok, "__asm"))  { read_token(); return read_asm_stmt(); }
    if (is_ident(tok, "_Static_assert"))  { read_token(); return read_static_assert_stmt(); }
    if (is_punct(tok, '{'))      { read_token(); return read_compound_stmt(); }

    Ast *r = read_comma_expr();
    if(r->type != AST_LABEL) expect(';');
    return r;
}

static Ast *read_decl_or_stmt(void)
{
    Token tok = peek_token();
    if (get_ttype(tok) == TTYPE_NULL)
        return NULL;

    if (is_type_keyword(tok)) {
        /* 特殊情况：typedef 名后面跟 ':' 是 goto 标签，不是声明
         * 由于 unget buffer 只有1个槽，无法 peek 两个 token，
         * 改为：读掉 tok，读下一个，若是 ':' 则直接生成标签，否则 unget 两者 */
        if (get_ttype(tok) == TTYPE_IDENT) {
            Token t2;
            read_token(); /* 消耗 tok */
            t2 = read_token(); /* 读下一个 token */
            if (is_punct(t2, ':')) {
                /* goto 标签：tok 是标签名 */
                Ast *lbl = ast_label(get_ident(tok));
                Ast *body = read_stmt();
                List *lst = make_list();
                list_push(lst, lbl);
                list_push(lst, body);
                return ast_compound_stmt(lst);
            }
            /* 不是标签：需要放回两个 token，但 buffer 只有1个
             * 先 unget t2（下一个），然后把 tok 的内容放到特殊槽 */
            unget_token(t2);
            unget_token(tok);
        }
        return read_decl();
    }
    return read_stmt();
}

static Ast *read_compound_stmt(void)
{
    localenv = make_dict(localenv);
    struct_defs = make_dict(struct_defs);
    union_defs = make_dict(union_defs);
    List *list = make_list();
    while (1) {
        Token tok = read_token();
        if (is_punct(tok, '}'))
            break;
        unget_token(tok);

        Ast *stmt = read_decl_or_stmt();

        if (stmt)
            list_push(list, stmt);
        if (!stmt)
            break;
    }
    localenv = dict_parent(localenv);
    struct_defs = dict_parent(struct_defs);
    union_defs = dict_parent(union_defs);

    /* 注入当前作用域内的局部 compound literal decl（插入到列表开头）
     * 这样在使用变量之前，临时变量就已经声明并初始化了 */
    if (pending_local_decls && !list_empty(pending_local_decls)) {
        while (!list_empty(pending_local_decls)) {
            Ast *pd = list_shift(pending_local_decls);
            if (pd) list_unshift(list, pd);
        }
    }

    return ast_compound_stmt(list);
}

static List *read_params(void)
{
    List *params = make_list();
    static int anon_param_seq = 0;
    Token tok = read_token();
    if (is_punct(tok, ')'))  
        return params;
    unget_token(tok);
    while (1) {
        /* 处理 ... 可变参数 */
        tok = peek_token();
        if (is_punct(tok, PUNCT_ELLIPSIS)) {
            read_token(); /* consume ... */
            tok = read_token();
            if (!is_punct(tok, ')')) 
                error("')' expected after '...', but got %s", token_to_string(tok));
            return params;
        }
        Ctype *ctype = read_decl_spec();
        tok = read_token();
        
        // 检查是否是函数指针语法: type (*name)(params)
        if (is_punct(tok, '(')) {
            // 可能是函数指针，检查下一个token是否是 *
            Token next_tok = read_token();
            if (is_punct(next_tok, '*')) {
                // 跳过 * 后面的 const/volatile/restrict 修饰符
                while (1) {
                    Token q = peek_token();
                    if (get_ttype(q) == TTYPE_IDENT &&
                        (is_ident(q, "const") || is_ident(q, "volatile") || is_ident(q, "restrict"))) {
                        read_token();
                    } else break;
                }
                // 读取函数指针名称（可能是无名参数，直接是 ')'，或者数组 [N]）
                Token name_tok = read_token();
                char *fn_name;
                if (is_punct(name_tok, '[')) {
                    /* 无名函数指针数组参数: int (*[N])(params) */
                    /* 消耗 [N] */
                    if (!is_punct(peek_token(), ']')) {
                        Ast *dim = read_expr();
                        (void)dim;
                    }
                    expect(']');
                    expect(')');
                    char anon_name[32];
                    snprintf(anon_name, sizeof(anon_name), "__anon_param_%d", anon_param_seq++);
                    fn_name = strdup(anon_name);
                    expect('(');
                    int depth2 = 1;
                    while (depth2 > 0) {
                        Token t = read_token();
                        if (is_punct(t, '(')) depth2++;
                        else if (is_punct(t, ')')) depth2--;
                        else if (get_ttype(t) == TTYPE_NULL)
                            error("Unexpected end of input in function pointer declaration");
                    }
                    Ctype *fn_ptr_type = make_ptr_type(ctype);
                    list_push(params, ast_lvar(fn_ptr_type, fn_name));
                    tok = read_token();
                    if (is_punct(tok, ')'))
                        return params;
                    if (!is_punct(tok, ','))
                        error("Comma expected, but got %s", token_to_string(tok));
                    continue;
                }
                if (is_punct(name_tok, ')')) {
                    /* 无名函数指针参数: int (*)(params) */
                    char anon_name[32];
                    snprintf(anon_name, sizeof(anon_name), "__anon_param_%d", anon_param_seq++);
                    fn_name = strdup(anon_name);
                    /* 不需要 expect(')')，已经消耗了 */
                    expect('(');
                    int depth = 1;
                    while (depth > 0) {
                        Token t = read_token();
                        if (is_punct(t, '(')) depth++;
                        else if (is_punct(t, ')')) depth--;
                        else if (get_ttype(t) == TTYPE_NULL)
                            error("Unexpected end of input in function pointer declaration");
                    }
                    Ctype *fn_ptr_type = make_ptr_type(ctype);
                    list_push(params, ast_lvar(fn_ptr_type, fn_name));
                    tok = read_token();
                    if (is_punct(tok, ')'))
                        return params;
                    if (!is_punct(tok, ','))
                        error("Comma expected, but got %s", token_to_string(tok));
                    continue;
                }
                if (get_ttype(name_tok) != TTYPE_IDENT)
                    error("Identifier expected in function pointer, but got %s", token_to_string(name_tok));
                fn_name = get_ident(name_tok);
                expect(')');
                /* 检查是否跟着 '('（函数指针参数列表），还是普通 const 指针 */
                Token after_paren = peek_token();
                if (!is_punct(after_paren, '(')) {
                    /* int (* const x) — 普通 const 指针参数，不是函数指针 */
                    Ctype *fn_ptr_type = make_ptr_type(ctype);
                    if(have_redefine_var(fn_name))
                        error("Function have redefined param: %s", fn_name);
                    list_push(params, ast_lvar(fn_ptr_type, fn_name));
                    tok = read_token();
                    if (is_punct(tok, ')'))
                        return params;
                    if (!is_punct(tok, ','))
                        error("Comma expected, but got %s", token_to_string(tok));
                    continue;
                }
                expect('(');
                // 读取函数指针的参数列表（忽略具体参数，只处理到右括号）
                int depth = 1;
                while (depth > 0) {
                    Token t = read_token();
                    if (is_punct(t, '('))
                        depth++;
                    else if (is_punct(t, ')'))
                        depth--;
                    else if (get_ttype(t) == TTYPE_NULL)
                        error("Unexpected end of input in function pointer declaration");
                }
                // 创建函数指针类型（作为普通指针处理）
                Ctype *fn_ptr_type = make_ptr_type(ctype);
                if(have_redefine_var(fn_name))
                    error("Function have redefined param: %s", fn_name);
                list_push(params, ast_lvar(fn_ptr_type, fn_name));
                
                tok = read_token();
                if (is_punct(tok, ')'))
                    return params;
                if (!is_punct(tok, ','))
                    error("Comma expected, but got %s", token_to_string(tok));
                continue;
            } else {
                /* 不是 (*name) 形式，可能是 int (int x) 函数类型参数
                 * 或 int () 无参函数类型参数，统一当作函数指针处理。
                 * next_tok 已读出（不是 '*'），把它和 tok('(') 当作
                 * 已消耗，继续把 next_tok unget 然后读完括号内容 */
                unget_token(next_tok);
                /* 消耗括号内的参数列表 */
                int depth = 1;
                while (depth > 0) {
                    Token t = read_token();
                    if (is_punct(t, '(')) depth++;
                    else if (is_punct(t, ')')) depth--;
                    else if (get_ttype(t) == TTYPE_NULL)
                        error("Unexpected end of input in parameter list");
                }
                /* 检查是否跟着 '(' — 即 int (int)(params) 形式 */
                if (is_punct(peek_token(), '(')) {
                    read_token();
                    int d2 = 1;
                    while (d2 > 0) {
                        Token t = read_token();
                        if (is_punct(t, '(')) d2++;
                        else if (is_punct(t, ')')) d2--;
                        else if (get_ttype(t) == TTYPE_NULL)
                            error("Unexpected end of input in parameter list");
                    }
                }
                /* 当作函数指针类型（匿名参数） */
                Ctype *fn_ptr_type = make_ptr_type(ctype);
                char anon_name[32];
                snprintf(anon_name, sizeof(anon_name), "__anon_param_%d", anon_param_seq++);
                list_push(params, ast_lvar(fn_ptr_type, strdup(anon_name)));
                tok = read_token();
                if (is_punct(tok, ')'))
                    return params;
                if (!is_punct(tok, ','))
                    error("Comma expected, but got %s", token_to_string(tok));
                continue;
            }
        }
        
        if (get_ttype(tok) != TTYPE_IDENT) {
            if(ctype->type == CTYPE_VOID && is_punct(tok, ')') && params->len == 0) {
                return params;
            } else if (is_punct(tok, '[')) {
                /* 无名数组参数: int [5], int [const 5] 等 */
                unget_token(tok);
                ctype = read_array_dimensions(ctype);
                /* 数组参数退化为指针 */
                if (ctype->type == CTYPE_ARRAY)
                    ctype = make_ptr_type(ctype->ptr);
                char anon_name[32];
                snprintf(anon_name, sizeof(anon_name), "__anon_param_%d", anon_param_seq++);
                list_push(params, ast_lvar(ctype, strdup(anon_name)));
                tok = read_token();
                if (is_punct(tok, ')'))
                    return params;
                if (!is_punct(tok, ','))
                    error("Comma expected, but got %s", token_to_string(tok));
                continue;
            } else if (is_punct(tok, ',') || is_punct(tok, ')')) {
                char anon_name[32];
                snprintf(anon_name, sizeof(anon_name), "__anon_param_%d", anon_param_seq++);
                list_push(params, ast_lvar(ctype, strdup(anon_name)));
                if (is_punct(tok, ')'))
                    return params;
                continue;
            } else  {
                error("Identifier expected, but got %s", token_to_string(tok));
            }
        }
            
        ctype = read_array_dimensions(ctype);
        if (ctype->type == CTYPE_ARRAY)
            ctype = make_ptr_type(ctype->ptr);
        if(have_redefine_var(get_ident(tok)))
            error("Function have redefined param: %s", token_to_string(tok));
        list_push(params, ast_lvar(ctype, get_ident(tok)));
        tok = read_token();
        if (is_punct(tok, ')'))
            return params;
        if (!is_punct(tok, ','))
            error("Comma expected, but got %s", token_to_string(tok));
    }
}

static Ast *read_func_def(Ctype *rettype, char *fname)
{    
    if(have_redefine_func(fname))
        error("Redeclaration function: %s", fname);
    
    expect('(');
    localenv = make_dict(globalenv);
    List *params = read_params();

    /* 平台相关：跳过 interrupt N 和/或 using N 修饰符（出现在 ')' 和 '{' 之间） */
    PlatInfoMCS51 c51_info = { -1, -1 };
    Token tok = read_token();
    while (get_ttype(tok) == TTYPE_IDENT &&
           (IS_C51_KW(tok, "interrupt") || IS_C51_KW(tok, "using") ||
            IS_C51_KW(tok, "reentrant") || IS_C51_KW(tok, "small")  ||
            IS_C51_KW(tok, "large")     || IS_C51_KW(tok, "compact"))) {
        /* interrupt/using 后面跟一个数字，reentrant/small/large/compact 不跟数字 */
        if (IS_C51_KW(tok, "interrupt") || IS_C51_KW(tok, "using")) {
            Token num = read_token();
            if (get_ttype(num) == TTYPE_NUMBER) {
                int n = atoi(get_number(num));
                if (IS_C51_KW(tok, "interrupt")) c51_info.interrupt_no = n;
                else c51_info.using_no = n;
            } else {
                unget_token(num);
            }
        }
        tok = read_token();
    }

    /* 合并 __attribute__((c51_interrupt(N), c51_using(M))) 指定的值 */
    if (g_attribute_c51_interrupt >= 0) {
        if (c51_info.interrupt_no < 0) c51_info.interrupt_no = g_attribute_c51_interrupt;
        g_attribute_c51_interrupt = -1;
    }
    if (g_attribute_c51_using >= 0) {
        if (c51_info.using_no < 0) c51_info.using_no = g_attribute_c51_using;
        g_attribute_c51_using = -1;
    }

    if(is_punct(tok, '{')) {
        is_first = true;
        // 先创建函数声明并添加到符号表，支持递归调用
        Ast *func_decl = ast_func_decl(rettype, fname, params);
        func_decl->platform_info.mcs51 = c51_info;
        dict_put(functionenv, fname, func_decl);
        
        localenv = make_dict(localenv);
        localvars = make_list();
        labels = make_list();
        Ast *body = read_compound_stmt();
        Ast *r = ast_func_def(rettype, fname, params, body, localvars, labels);
        r->platform_info.mcs51 = c51_info;
        localenv = dict_parent(localenv);
        localvars = NULL;
        labels = NULL;

        // 更新符号表中的函数定义
        dict_put(functionenv, fname, r);
        return r;
    } else if (is_punct(tok, ';') || is_punct(tok, ',')) {
        /* ';' 或 ',' → 函数前向声明（prototype）；',' 表示多声明符，回退让调用方处理 */
        if (is_punct(tok, ',')) unget_token(tok);
        Ast *r = ast_func_decl(rettype, fname, params);
        r->platform_info.mcs51 = c51_info;
        dict_put(functionenv, fname, r);
        return r;
    }

    return NULL;
}

static void skip_to_semicolon(void)
{
    int depth_paren = 0;
    int depth_bracket = 0;
    int depth_brace = 0;

    while (1) {
        Token t = read_token();
        if (get_ttype(t) == TTYPE_NULL)
            return;
        if (is_punct(t, '(')) depth_paren++;
        else if (is_punct(t, ')') && depth_paren > 0) depth_paren--;
        else if (is_punct(t, '[')) depth_bracket++;
        else if (is_punct(t, ']') && depth_bracket > 0) depth_bracket--;
        else if (is_punct(t, '{')) depth_brace++;
        else if (is_punct(t, '}') && depth_brace > 0) depth_brace--;
        else if (is_punct(t, ';') && depth_paren == 0 && depth_bracket == 0 && depth_brace == 0)
            return;
    }
}

static Ast *read_decl_or_func_def(void)
{
    Token tok = peek_token();
    if (get_ttype(tok) == TTYPE_NULL)
        return NULL;

    /* 顶层 _Static_assert */
    if (is_ident(tok, "_Static_assert")) {
        read_token();
        return read_static_assert_stmt();
    }

    Ctype *ctype = read_decl_spec();
    {
        int extra_attr = skip_c51_decl_modifiers();
        if (extra_attr) ctype = clone_ctype_with_attr(ctype, ctype->attr | extra_attr);
    }
    Token tok1 = read_token();
    char *ident;
    
    // 检查是否是函数指针: type (*name)(params) 或 typedef type (*name)(params);
    if (is_punct(tok1, '(')) {
        Token next_tok = read_token();
        if (is_punct(next_tok, '*')) {
            // 函数指针: type (*name)(params) 或函数指针数组: type (*name[N])(params)
            Token name_tok = read_token();
            if (get_ttype(name_tok) != TTYPE_IDENT)
                error("Identifier expected in function pointer, but got %s", token_to_string(name_tok));
            ident = get_ident(name_tok);
            /* 处理可能的数组维度 (*name[N]) */
            if (is_punct(peek_token(), '[')) {
                read_token(); /* consume '[' */
                if (!is_punct(peek_token(), ']')) {
                    Ast *dim_expr = read_expr();
                    (void)dim_expr;
                }
                expect(']');
            }
            /* 处理返回函数指针的函数：int (* f1(int a, int b))(int c, int b)
             * 读完 f1 后，peek 是 ( —— f1 的参数列表 */
            if (is_punct(peek_token(), '(')) {
                /* 用 read_params() 正确读取 f1 的参数列表 */
                read_token(); /* consume '(' */
                localenv = make_dict(globalenv);
                List *params = read_params(); /* 读参数并消耗最后的 ')' */
                expect(')'); /* consume ')' of (*f1(params)) */
                /* 跳过返回类型参数列表 (ret_params) */
                expect('(');
                read_func_ptr_params(); /* 消耗返回类型参数列表并消耗最后的 ')' */
                ctype = make_ptr_type(ctype); /* 返回类型是函数指针 */
                /* 现在 ident = f1，下一个是 '{' 或 ';' */
                Token after = peek_token();
                if (is_punct(after, '{')) {
                    /* 函数定义：直接读函数体 */
                    if (have_redefine_func(ident))
                        error("Redeclaration function: %s", ident);
                    read_token(); /* consume '{' */
                    Ast *func_decl = ast_func_decl(ctype, ident, params);
                    dict_put(functionenv, ident, func_decl);
                    localenv = make_dict(localenv);
                    localvars = make_list();
                    labels = make_list();
                    is_first = true;
                    Ast *body = read_compound_stmt();
                    Ast *r = ast_func_def(ctype, ident, params, body, localvars, labels);
                    localenv = dict_parent(localenv);
                    localvars = NULL; labels = NULL;
                    dict_put(functionenv, ident, r);
                    return r;
                }
                if (is_punct(after, ';')) {
                    read_token();
                    Ast *var = ast_gvar(ctype, ident, false);
                    return ast_decl(var, NULL);
                }
                error("Expected '{' or ';' after function-returning-pointer decl, got %s", token_to_string(after));
            }
            expect(')');
            expect('(');
            read_func_ptr_params();
            // 创建函数指针类型
            ctype = make_ptr_type(ctype);
            Token after = peek_token();
            if (get_attr(ctype->attr).ctype_typedef || is_punct(after, ';')) {
                /* typedef 或无初始化：int (*fptr)(); */
                if (get_attr(ctype->attr).ctype_typedef) {
                    expect(';');
                    dict_put(typedefenv, ident, ctype);
                    return ast_typedef(ctype, ident);
                }
                read_token(); /* consume ';' */
                Ast *var = ast_gvar(ctype, ident, false);
                return ast_decl(var, NULL);
            }
            /* 带初始化：int (*fptr)() = 0; 或 int (*f)(int) = &fred; */
            Ast *var = ast_gvar(ctype, ident, false);
            return read_decl_init(var);
        } else {
            unget_token(next_tok);
            unget_token(tok1);
            tok1 = read_token();
        }
    }
    
    /* 支持指针修饰符 * (如 enum E *e; 或 struct S *p;) */
    while (is_punct(tok1, '*')) {
        ctype = make_ptr_type(ctype);
        /* const 修饰符可能跟在 * 后面，如 const struct S *const p */
        Token maybe_const = peek_token();
        if (get_ttype(maybe_const) == TTYPE_IDENT && 
            (is_ident(maybe_const, "const") || is_ident(maybe_const, "volatile") || is_ident(maybe_const, "restrict"))) {
            read_token();  /* 消耗 const/volatile/restrict */
        }
        tok1 = read_token();
    }
    
    if (get_ttype(tok1) != TTYPE_IDENT) {
        if(is_punct(tok1, ';')) {
            if(ctype->type == CTYPE_STRUCT) return ast_struct_def(ctype);
            else if(ctype->type == CTYPE_ENUM) return ast_enum_def(ctype);
        }
        error("Identifier expected, but got %s", token_to_string(tok1));
    }
    ident = get_ident(tok1);

    if ((g_last_c51_decl_kind == C51_DECL_SFR || g_last_c51_decl_kind == C51_DECL_SFR16 ||
         g_last_c51_decl_kind == C51_DECL_SBIT) && is_punct(peek_token(), '=')) {
        Ast *var = ast_gvar(ctype, ident, false);
        if (g_last_c51_decl_kind == C51_DECL_SBIT) {
            /* sbit 的 '^' 是位位置运算符，不是异或。
             * 直接读取表达式，不进行编译时求值。 */
            read_token(); /* consume '=' */
            Ast *init = read_expr();
            expect(';');
            if (var->type == AST_GVAR) var->ginit = init;
            return ast_decl(var, init);
        }
        return read_decl_init(var);
    }

    tok = peek_token();
    if (is_punct(tok, '(')) {
        Ast *fdecl = read_func_def(ctype, ident);
        /* 处理逗号分隔的多声明：int f(int a), g(int a), x; */
        while (is_punct(peek_token(), ',')) {
            read_token(); /* 消耗 ',' */
            Ctype *next_ctype = ctype; /* 使用相同基类型 */
            /* 处理可能的 * */
            while (is_punct(peek_token(), '*')) {
                read_token();
                next_ctype = make_ptr_type(next_ctype);
            }
            Token next_name = read_token();
            if (get_ttype(next_name) != TTYPE_IDENT) { unget_token(next_name); break; }
            char *next_ident = get_ident(next_name);
            Ast *next_decl = NULL;
            if (is_punct(peek_token(), '(')) {
                /* 另一个函数声明 */
                next_decl = read_func_def(next_ctype, next_ident);
            } else {
                /* 变量声明 */
                Ast *var = ast_gvar(next_ctype, next_ident, false);
                next_decl = read_decl_init_single(var);
            }
            if (next_decl) {
                if (!pending_toplevel_decls) pending_toplevel_decls = make_list();
                list_push(pending_toplevel_decls, next_decl);
            }
        }
        /* 消耗结尾分号（read_func_def 处理 ',' 时未消耗 ';'）*/
        if (is_punct(peek_token(), ';')) read_token();
        return fdecl;
    }
    if (ctype->type == CTYPE_VOID)
        error("Storage size of '%s' is not known", token_to_string(tok1));
    Ctype *ctype_with_dims = read_array_dimensions(ctype);
    tok = peek_token();  /* re-peek after consuming array dimensions */
    if (is_punct(tok, '=') || is_punct(tok, ';') || is_punct(tok, ',')) {
        if (is_punct(tok, ',') || is_punct(tok, '=')) {
            /* 多变量声明（含初始化）以及带初始化的单变量：
             * 传给 read_global_decl_multi 基础类型（不含已消耗的数组维度），
             * 由它统一处理每个变量的数组维度和初始化 */
            return read_global_decl_multi(ctype, ident, ctype_with_dims);
        }
        if (is_punct(tok, ';')) {
            if (get_attr(ctype_with_dims->attr).ctype_typedef) {
                dict_put(typedefenv, ident, ctype_with_dims);
                read_token();
                return ast_typedef(ctype_with_dims, ident);
            }
            read_token();
            Ast *var = ast_gvar(ctype_with_dims, ident, false);
            return ast_decl(var, NULL);
        }
        /* '=' 初始化 */
        Ast *var = ast_gvar(ctype_with_dims, ident, false);
        return read_decl_init(var);
    }
    if (ctype_with_dims->type == CTYPE_ARRAY) {
        Ast *var = ast_gvar(ctype_with_dims, ident, false);
        return read_decl_init(var);
    }
    error("Don't know how to handle %s", token_to_string(tok));
    return NULL; /* non-reachable */
}

List *read_toplevels(void)
{
    List *r = make_list();
    while (1) {
        Token tok = peek_token();
        if (get_ttype(tok) == TTYPE_NULL)
            return r;

        Ast *ast = read_decl_or_func_def();
        if (!ast)
            return r;
        while (pending_toplevel_decls && !list_empty(pending_toplevel_decls)) {
            Ast *pending = list_shift(pending_toplevel_decls);
            if (pending) list_push(r, pending);
        }
        if (ast->type == AST_COMPOUND_STMT && ast->stmts) {
            for (Iter it = list_iter(ast->stmts); !iter_end(it);) {
                Ast *item = iter_next(&it);
                if (item) list_push(r, item);
            }
        } else {
            list_push(r, ast);
        }
    }
    list_free(globalenv->list);
    return r;
}

CtypeAttr get_attr(int in_attr) 
{
    union { CtypeAttr c_attr; int i_attr; }attr = {0};
    attr.i_attr = in_attr;
    return attr.c_attr;
}