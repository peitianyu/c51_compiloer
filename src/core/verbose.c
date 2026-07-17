#include "cc.h"
#include <ctype.h>

/* 把 Ctype::attr 转成 "const volatile static …" 这种字符串 */
static const char *ctype_attr_string(int attr)
{
    /* 64 字节足够放下所有关键字 + 空格 + '\0' */
    static char buf[64];
    char *p = buf;
    CtypeAttr a = get_attr(attr);
    *p = '\0';

    /* 注意顺序：一般按 C 语法习惯把 "const/volatile" 放最前 */
    if (a.ctype_const)    { strcpy(p, "const ");     p += 6; }
    if (a.ctype_volatile) { strcpy(p, "volatile "); p += 9; }
    if (a.ctype_restrict) { strcpy(p, "restrict "); p += 9; }
    if (a.ctype_static)   { strcpy(p, "static ");   p += 7; }
    if (a.ctype_extern)   { strcpy(p, "extern ");   p += 7; }
    if (a.ctype_unsigned) { strcpy(p, "unsigned "); p += 9; }
    if (a.ctype_register) { strcpy(p, "register "); p += 9; }
    if (a.ctype_c51_data)  { strcpy(p, "data ");  p += 5; }
    if (a.ctype_c51_idata) { strcpy(p, "idata "); p += 6; }
    if (a.ctype_c51_xdata) { strcpy(p, "xdata "); p += 6; }
    if (a.ctype_c51_pdata) { strcpy(p, "pdata "); p += 6; }
    if (a.ctype_c51_code)  { strcpy(p, "code ");  p += 5; }
    if (a.ctype_c51_bdata) { strcpy(p, "bdata "); p += 6; }
    if (a.ctype_c51_near)  { strcpy(p, "near ");  p += 5; }
    if (a.ctype_c51_far)   { strcpy(p, "far ");   p += 4; }
    /* 函数限定符 */
    if (a.ctype_inline)   { strcpy(p, "inline ");   p += 7; }
    if (a.ctype_noreturn) { strcpy(p, "noreturn "); p += 10; }
    
    /* 末尾如果是空格，退一格覆盖掉 */
    if (p > buf && *(p-1) == ' ') *(p-1) = '\0';
    return buf;
}

/* 用于 ctype_to_string 防止 struct/union 互相递归打印 */
#define MAX_VISITED_CTYPES 64
static Ctype *visited_ctypes[MAX_VISITED_CTYPES];
static int visited_ctypes_depth = 0;

char *ctype_to_string(Ctype *ctype) {
    if (!ctype) return "(nil)";

    /* 检查是否已在访问栈中（防止 struct/union 互相递归无限打印） */
    if (ctype->type == CTYPE_STRUCT) {
        for (int vi = 0; vi < visited_ctypes_depth; vi++) {
            if (visited_ctypes[vi] == ctype) {
                /* 已访问过：只打印 tag 名或省略 */
                if (ctype->tag)
                    return ctype->is_union ? "(union ...)" : "(struct ...)";
                return ctype->is_union ? "(union ...)" : "(struct ...)";
            }
        }
    }

    String s = make_string();
    char *res;
    CtypeAttr at = get_attr(ctype->attr);
    const char *a = ctype_attr_string(ctype->attr);
    if (*a) string_appendf(&s, "%s ", a);
    if(ctype->bit_size) string_appendf(&s, "%d: ", ctype->bit_size);

    switch (ctype->type) {
    case CTYPE_VOID:  string_appendf(&s, "void"); break;
    case CTYPE_INT:
        if (at.ctype_c51_sfr16) { string_appendf(&s, "sfr16"); break; }
        /* MCS51 模式：size==2 → short（16bit），size==4 → int（32bit，标准） */
        if (ctype->size == 2)      string_appendf(&s, "short");
        else                       string_appendf(&s, "int");
        break;
    case CTYPE_LONG:
        /* MCS51 模式：size==4 → int（32bit），size==8 → long（64bit，标准） */
        if (ctype->size == 4)      string_appendf(&s, "int");
        else                       string_appendf(&s, "long");
        break;
    case CTYPE_BOOL:  string_appendf(&s, "bool"); break;
    case CTYPE_CHAR:
        if (at.ctype_c51_sfr)      string_appendf(&s, "sfr");
        else if (at.ctype_c51_sbit)string_appendf(&s, "sbit");
        else if (at.ctype_c51_bit) string_appendf(&s, "bit");
        else                       string_appendf(&s, "char");
        break;
    case CTYPE_FLOAT: string_appendf(&s, "float"); break;
    case CTYPE_DOUBLE:string_appendf(&s, "double"); break;
    case CTYPE_PTR: {
        string_appendf(&s, "(*%s)", ctype_to_string(ctype->ptr));
        return get_cstring(s);
    }
    case CTYPE_ARRAY: {
        string_appendf(&s, "[%d]%s", ctype->len, ctype_to_string(ctype->ptr));
        return get_cstring(s);
    }
    case CTYPE_STRUCT: {
        /* 压栈，防止递归 */
        if (visited_ctypes_depth < MAX_VISITED_CTYPES)
            visited_ctypes[visited_ctypes_depth++] = ctype;
        string_appendf(&s, ctype->is_union ? "(union" : "(struct");
        if (ctype->fields) {
            for (Iter i = list_iter(ctype->fields->list); !iter_end(i);) {
                DictEntry *e = iter_next(&i);
                string_appendf(&s, " (%s %s)", ctype_to_string(e->val), e->key);
            }
        }
        string_appendf(&s, ")");
        /* 弹栈 */
        if (visited_ctypes_depth > 0) visited_ctypes_depth--;
        return get_cstring(s);
    }
    case CTYPE_ENUM:
        string_appendf(&s, "(enum");
        if (ctype->fields) {
            for (Iter i = list_iter(ctype->fields->list); !iter_end(i);) {
                DictEntry *e = iter_next(&i);
                string_appendf(&s, " (%d %s)", *(int *)(e->val), e->key);
            }
        }
        string_appendf(&s, ")");
        return get_cstring(s);
    default: error("Unknown ctype: %d", ctype);
    }
    return get_cstring(s);
}

static void uop_to_string(String *buf, char *op, Ast *ast)
{
    string_appendf(buf, "(%s %s)", op, ast_to_string(ast->operand));
}

static void binop_to_string(String *buf, char *op, Ast *ast)
{
    string_appendf(buf, "(%s %s %s)", op, ast_to_string(ast->left),
                   ast_to_string(ast->right));
}

static void ast_to_string_int(String *buf, Ast *ast)
{
    if (!ast) {
        string_appendf(buf, "(nil)");
        return;
    }
    switch (ast->type) {
    case AST_LITERAL:
        switch (ast->ctype->type) {
        case CTYPE_BOOL:
            if (get_attr(ast->ctype->attr).ctype_register)
                string_appendf(buf, "0x%02X", (unsigned char)ast->ival);
            else
                string_appendf(buf, "%s", ast->ival ? "true" : "false");
            break;
        case CTYPE_CHAR:
            if (ast->ival == '\n')
                string_appendf(buf, "'\\n'");
            else if (ast->ival == '\r')
                string_appendf(buf, "'\\r'");
            else if (ast->ival == '\t')
                string_appendf(buf, "'\\t'");
            else if (ast->ival == '\\')
                string_appendf(buf, "'\\\\'");
            else if (ast->ival == '\'')
                string_appendf(buf, "'\\\''");
            else {
                unsigned char uc = (unsigned char)ast->ival;
                if (isprint(uc))
                    string_appendf(buf, "'%c'", uc);
                else
                    string_appendf(buf, "0x%02X", uc);
            }
            break;
        case CTYPE_INT:
            string_appendf(buf, "%d", ast->ival);
            break;
        case CTYPE_LONG:
            string_appendf(buf, "%lldL", ast->ival);
            break;
        case CTYPE_FLOAT:
        case CTYPE_DOUBLE:
            string_appendf(buf, "%f", ast->fval);
            break;
        case CTYPE_ENUM:
            string_appendf(buf, "%d", ast->ival);
            break;
        default:
            error("internal error");
        }
        break;
    case AST_STRING:
        string_appendf(buf, "\"%s\"", quote_cstring(ast->sval));
        break;
    case AST_LVAR:
    case AST_GVAR:
        string_appendf(buf, "%s", ast->varname);
        break;
    case AST_FUNCALL: {
        if (ast->fnexpr)
            string_appendf(buf, "(%s)(*%s)(", ctype_to_string(ast->ctype), ast_to_string(ast->fnexpr));
        else
            string_appendf(buf, "(%s)%s(", ctype_to_string(ast->ctype), ast->fname);
        for (Iter i = list_iter(ast->args); !iter_end(i);) {
            string_appendf(buf, "%s", ast_to_string(iter_next(&i)));
            if (!iter_end(i))
                string_appendf(buf, ",");
        }
        string_appendf(buf, ")");
        break;
    }
    case AST_FUNC_DECL:
        string_appendf(buf, "(funcdecl %s)%s(", ctype_to_string(ast->ctype), ast->fname);
        for (Iter i = list_iter(ast->params); !iter_end(i);) {
            Ast *param = iter_next(&i);
            string_appendf(buf, "%s %s", ctype_to_string(param->ctype), ast_to_string(param));
            if (!iter_end(i))
                string_appendf(buf, ",");
        }
        string_appendf(buf, ")");
        break;
    case AST_FUNC_DEF: {
        string_appendf(buf, "(%s)%s(", ctype_to_string(ast->ctype), ast->fname);
        for (Iter i = list_iter(ast->params); !iter_end(i);) {
            Ast *param = iter_next(&i);
            string_appendf(buf, "%s %s", ctype_to_string(param->ctype), ast_to_string(param));
            if (!iter_end(i))
                string_appendf(buf, ",");
        }
        string_appendf(buf, ")");
        ast_to_string_int(buf, ast->body);
        break;
    }
    /* AST_INTERRUPT_DEF removed: interrupts are represented as normal functions */
    case AST_DECL:
        string_appendf(buf, "(decl %s %s", ctype_to_string(ast->declvar->ctype),
                       ast->declvar->varname);
        if (ast->declinit)
            string_appendf(buf, " = %s)", ast_to_string(ast->declinit));
        else
            string_appendf(buf, ")");
        break;
    case AST_ARRAY_INIT:
        string_appendf(buf, "{");
        for (Iter i = list_iter(ast->arrayinit); !iter_end(i);) {
            ast_to_string_int(buf, iter_next(&i));
            if (!iter_end(i))
                string_appendf(buf, ",");
        }
        string_appendf(buf, "}");
        break;
    case AST_STRUCT_INIT:
        string_appendf(buf, "{");
        for (Iter i = list_iter(ast->arrayinit); !iter_end(i);) {
            ast_to_string_int(buf, iter_next(&i));
            if (!iter_end(i))
                string_appendf(buf, ",");
        }
        string_appendf(buf, "}");
        break;
    case AST_IF:
        string_appendf(buf, "(if %s %s", ast_to_string(ast->cond),
                       ast_to_string(ast->then));
        if (ast->els)
            string_appendf(buf, " %s", ast_to_string(ast->els));
        string_appendf(buf, ")");
        break;
    case AST_TERNARY:
        string_appendf(buf, "(? %s %s %s)", ast_to_string(ast->cond),
                       ast_to_string(ast->then), ast_to_string(ast->els));
        break;
    case AST_SWITCH: {
        string_appendf(buf, "(switch %s ", ast_to_string(ast->ctrl));
        for (Iter i = list_iter(ast->cases); !iter_end(i);) {
            SwitchCase *c = iter_next(&i);
            if (c->low == c->high)  string_appendf(buf, "(case %ld @%s) ", c->low, c->label ? c->label : "");
            else                    string_appendf(buf, "(case %ld ... %ld @%s) ", c->low, c->high, c->label ? c->label : "");
            
        }
        if (ast->default_label)
            string_appendf(buf, "(default @%s) ", ast->default_label);
        if (ast->switch_body)
            string_appendf(buf, "%s ", ast_to_string(ast->switch_body));
        string_appendf(buf, ")");
        break;
    }
    case AST_FOR:
        string_appendf(buf, "(for %s %s %s ", ast_to_string(ast->forinit),
                       ast_to_string(ast->forcond),
                       ast_to_string(ast->forstep));
        string_appendf(buf, "%s)", ast_to_string(ast->forbody));
        break;
    case AST_WHILE:
        string_appendf(buf, "(while (%s) %s)", ast_to_string(ast->while_cond), ast_to_string(ast->while_body));
        break;
    case AST_DO_WHILE:
        string_appendf(buf, "(do-while %s (%s))", ast_to_string(ast->while_body), ast_to_string(ast->while_cond));
        break;
    case AST_GOTO:
        string_appendf(buf, "(goto %s)", ast->label);
        break;
    case AST_CONTINUE:
        string_appendf(buf, "(continue)");
        break;
    case AST_BREAK:
        string_appendf(buf, "(break)");
        break;
    case AST_LABEL:
        string_appendf(buf, "(label %s)", ast->label);
        break;
    case AST_RETURN:
        string_appendf(buf, "(return %s)", ast_to_string(ast->retval));
        break;
    case AST_ASM:
        string_appendf(buf, "(asm \"%s\")", ast->asm_body ? ast->asm_body : "");
        break;
    case AST_STATIC_ASSERT:
        string_appendf(buf, "(static_assert \"%s\")", ast->els ? (char*)ast->els : "");
        break;
    case AST_COMPOUND_STMT: {
        string_appendf(buf, "{");
        for (Iter i = list_iter(ast->stmts); !iter_end(i);) {
            ast_to_string_int(buf, iter_next(&i));
            string_appendf(buf, ";");
        }
        string_appendf(buf, "}");
        break;
    }
    case AST_STRUCT_REF:
        ast_to_string_int(buf, ast->struc);
        string_appendf(buf, ".");
        string_appendf(buf, ast->field);
        break;
    case AST_BIT_REF:
        ast_to_string_int(buf, ast->struc);
        string_appendf(buf, ".%d", ast->bit_index);
        break;
    case AST_STRUCT_DEF:
        string_appendf(buf, "(def %s)", ctype_to_string(ast->ctype));
        break;
    case AST_ENUM_DEF:
        string_appendf(buf, "(def %s)", ctype_to_string(ast->ctype));
        break;
    case AST_TYPE_DEF:
        string_appendf(buf, "(typedef %s %s)", ctype_to_string(ast->ctype), ast->typename);
        break;
    case AST_CAST: {
        string_appendf(buf, "(%s)", ctype_to_string(ast->ctype));
        string_appendf(buf, "%s", ast_to_string(ast->cast_expr));
        break;
    }
    case AST_ADDR:
        uop_to_string(buf, "addr", ast);
        break;
    case AST_DEREF:
        uop_to_string(buf, "deref", ast);
        break;
    case PUNCT_INC:
        uop_to_string(buf, "++", ast);
        break;
    case PUNCT_DEC:
        uop_to_string(buf, "--", ast);
        break;
    case AST_POST_INC:
        uop_to_string(buf, "post++", ast);
        break;
    case AST_POST_DEC:
        uop_to_string(buf, "post--", ast);
        break;
    case PUNCT_LOGAND:
        binop_to_string(buf, "and", ast);
        break;
    case PUNCT_LOGOR:
        binop_to_string(buf, "or", ast);
        break;
    case '!':
        uop_to_string(buf, "!", ast);
        break;
    case '~':
        uop_to_string(buf, "~", ast);
        break;
    case '&':
        binop_to_string(buf, "&", ast);
        break;
    case '|':
        binop_to_string(buf, "|", ast);
        break;
    case PUNCT_LSHIFT:
        binop_to_string(buf, "<<", ast);
        break;
    case PUNCT_RSHIFT:
        binop_to_string(buf, ">>", ast);
        break;
    case PUNCT_SHL_ASSIGN:
        binop_to_string(buf, "<<=", ast);
        break;
    case PUNCT_SHR_ASSIGN:
        binop_to_string(buf, ">>=", ast);
        break;
    case PUNCT_AND_ASSIGN:
        binop_to_string(buf, "&=", ast);
        break;
    case PUNCT_OR_ASSIGN:
        binop_to_string(buf, "|=", ast);
        break;
    case PUNCT_XOR_ASSIGN:
        binop_to_string(buf, "^=", ast);
        break;
    case PUNCT_ADD_ASSIGN:
        binop_to_string(buf, "+=", ast);
        break;
    case PUNCT_SUB_ASSIGN:
        binop_to_string(buf, "-=", ast);
        break;
    case PUNCT_MUL_ASSIGN:
        binop_to_string(buf, "*=", ast);
        break;
    case PUNCT_DIV_ASSIGN:
        binop_to_string(buf, "/=", ast);
        break;
    case PUNCT_MOD_ASSIGN:
        binop_to_string(buf, "%=", ast);
        break;
    default: {
        char *left = ast_to_string(ast->left);
        char *right = ast_to_string(ast->right);
        if (ast->type == PUNCT_EQ)
            string_appendf(buf, "(== ");
        else if (ast->type == PUNCT_GE)
            string_appendf(buf, "(>= ");
        else if (ast->type == PUNCT_LE)
            string_appendf(buf, "(<= ");
        else if (ast->type == PUNCT_NE)
            string_appendf(buf, "(!= ");
        else
            string_appendf(buf, "(%c ", ast->type);
        string_appendf(buf, "%s %s)", left, right);
    }
    }
}

char *ast_to_string(Ast *ast)
{
    String s = make_string();
    ast_to_string_int(&s, ast);
    return get_cstring(s);
}

char *token_to_string(const Token tok)
{
    int ttype = get_ttype(tok);
    String s = make_string();
    if (ttype == TTYPE_NULL)
        return "(null)";
    switch (ttype) {
    case TTYPE_NULL:
        error("internal error: unknown token type: %d", get_ttype(tok));
    case TTYPE_IDENT:
        return get_ident(tok);
    case TTYPE_PUNCT:
        switch (get_punct(tok)) {
        case PUNCT_EQ:        string_appendf(&s, "=="); break;
        case PUNCT_GE:        string_appendf(&s, ">="); break;
        case PUNCT_LE:        string_appendf(&s, "<="); break;
        case PUNCT_NE:        string_appendf(&s, "!="); break;
        case PUNCT_ELLIPSIS:  string_appendf(&s, "..."); break;
        case PUNCT_INC:       string_appendf(&s, "++"); break;
        case PUNCT_DEC:       string_appendf(&s, "--"); break;
        case PUNCT_LOGAND:    string_appendf(&s, "&&"); break;
        case PUNCT_LOGOR:     string_appendf(&s, "||"); break;
        case PUNCT_ARROW:     string_appendf(&s, "->"); break;
        case PUNCT_LSHIFT:    string_appendf(&s, "<<"); break;
        case PUNCT_RSHIFT:    string_appendf(&s, ">>"); break;
        case PUNCT_SHL_ASSIGN:string_appendf(&s, "<<="); break;
        case PUNCT_SHR_ASSIGN:string_appendf(&s, ">>="); break;
        case PUNCT_AND_ASSIGN:string_appendf(&s, "&="); break;
        case PUNCT_OR_ASSIGN: string_appendf(&s, "|="); break;
        case PUNCT_XOR_ASSIGN:string_appendf(&s, "^="); break;
        case PUNCT_ADD_ASSIGN:string_appendf(&s, "+="); break;
        case PUNCT_SUB_ASSIGN:string_appendf(&s, "-="); break;
        case PUNCT_MUL_ASSIGN:string_appendf(&s, "*="); break;
        case PUNCT_DIV_ASSIGN:string_appendf(&s, "/="); break;
        case PUNCT_MOD_ASSIGN:string_appendf(&s, "%%="); break;
        default:
            if (get_punct(tok) < 256)
                string_appendf(&s, "%c", get_punct(tok));
            else
                string_appendf(&s, "(punct:%d)", get_punct(tok));
        }
        return get_cstring(s);
    case TTYPE_CHAR:
        string_append(&s, get_char(tok));
        return get_cstring(s);
    case TTYPE_NUMBER:
        return get_number(tok);
    case TTYPE_STRING:
        string_appendf(&s, "\"%s\"", get_strtok(tok));
        return get_cstring(s);
    }
    error("internal error: unknown token type: %d", get_ttype(tok));
    return NULL; /* non-reachable */
}