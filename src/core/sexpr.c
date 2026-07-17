#include "cc.h"
#include <ctype.h>

static void sexpr_int(String *buf, Ast *ast);   /* forward */

/* 计算 struct 字段的逻辑 slot 偏移（union 字段共享同一 slot，嵌套 struct 展开多个 slot）
 *
 * 逻辑 slot = init_vals 数组中该字段第一个元素的索引
 * 规则：
 *   - 每个唯一字节偏移对应一个 slot 组
 *   - 若某字段的字节偏移等于前一个字段（union 共享），则 slot 不增加
 *   - 嵌套 struct 字段占用 field_logical_slots(nested) 个 slot
 */
static int field_logical_slots(Ctype *ft); /* forward */

static int field_logical_slots(Ctype *ft) {
    if (!ft) return 1;
    if (ft->type == CTYPE_STRUCT && !ft->is_union && ft->fields) {
        /* 嵌套 struct：统计 slot 数（按唯一 byte offset 分组） */
        int slots = 0;
        int prev_off = -1;
        List *vals = dict_values(ft->fields);
        for (Iter vi = list_iter(vals); !iter_end(vi);) {
            Ctype *nft = (Ctype *)iter_next(&vi);
            if (nft->offset != prev_off) {
                slots += field_logical_slots(nft); /* 递归 */
                prev_off = nft->offset;
            }
        }
        return slots > 0 ? slots : 1;
    }
    if (ft->type == CTYPE_ARRAY && ft->ptr) {
        return ft->len * field_logical_slots(ft->ptr);
    }
    return 1;
}

static int struct_field_logical_slot(Ctype *struct_ctype, const char *fieldname) {
    if (!struct_ctype || !struct_ctype->fields || !fieldname) return 0;
    int slot = 0;
    int prev_off = -1;
    int prev_slots = 0; /* slots used by prev group */
    List *keys   = dict_keys(struct_ctype->fields);
    List *values = dict_values(struct_ctype->fields);
    Iter ki = list_iter(keys);
    Iter vi = list_iter(values);
    for (; !iter_end(ki) && !iter_end(vi);) {
        char  *fname = (char  *)iter_next(&ki);
        Ctype *ft    = (Ctype *)iter_next(&vi);
        if (ft->offset != prev_off) {
            /* 新的 byte offset：推进 slot（前一组已用 prev_slots 个 slot） */
            if (prev_off >= 0) slot += prev_slots;
            prev_off = ft->offset;
            prev_slots = field_logical_slots(ft);
        }
        /* 当前字段是这个 slot 组的一个成员 */
        if (strcmp(fname, fieldname) == 0)
            return slot;
    }
    return 0;
}

static void sexpr_uop(String *buf, const char *op, Ast *ast)
{
    string_appendf(buf, "(%s ", op);
    sexpr_int(buf, ast->operand);
    string_appendf(buf, ")");
}

static void sexpr_binop(String *buf, const char *op, Ast *ast)
{
    string_appendf(buf, "(%s ", op);
    sexpr_int(buf, ast->left);
    string_appendf(buf, " ");
    sexpr_int(buf, ast->right);
    string_appendf(buf, ")");
}

static void sexpr_int(String *buf, Ast *ast)
{
    if (!ast) {
        string_appendf(buf, "nil");
        return;
    }
    switch (ast->type) {

    /* ── literals ────────────────────────────────── */
    case AST_LITERAL:
        switch (ast->ctype->type) {
        case CTYPE_BOOL:
            if (get_attr(ast->ctype->attr).ctype_register)
                string_appendf(buf, "(literal char 0x%02X)", (unsigned char)ast->ival);
            else
                string_appendf(buf, "(literal bool %s)", ast->ival ? "true" : "false");
            break;
        case CTYPE_CHAR: {
            unsigned char uc = (unsigned char)ast->ival;
            if (ast->ival == '\n')      string_appendf(buf, "(literal char '\\n')");
            else if (ast->ival == '\r') string_appendf(buf, "(literal char '\\r')");
            else if (ast->ival == '\t') string_appendf(buf, "(literal char '\\t')");
            else if (ast->ival == '\\') string_appendf(buf, "(literal char '\\\\')");
            else if (ast->ival == '\'') string_appendf(buf, "(literal char '\\'')");
            else if (isprint(uc))       string_appendf(buf, "(literal char '%c')", uc);
            else                        string_appendf(buf, "(literal char 0x%02X)", uc);
            break;
        }
        case CTYPE_INT:
            /* MCS51 模式下 int size==2，输出 "short" 让 SSA 解析为 i16 */
            if (ast->ctype->size == 2)
                string_appendf(buf, "(literal short %d)", (int)ast->ival);
            else
                string_appendf(buf, "(literal int %d)", (int)ast->ival);
            break;
        case CTYPE_LONG:
            /* MCS51 模式下 long size==4，输出 "int" 让 SSA 解析为 i32 */
            if (ast->ctype->size == 4)
                string_appendf(buf, "(literal int %lld)", ast->ival);
            else
                string_appendf(buf, "(literal long %lld)", ast->ival);
            break;
        case CTYPE_FLOAT:
        case CTYPE_DOUBLE:
            string_appendf(buf, "(literal %s %f)",
                           ast->ctype->type == CTYPE_FLOAT ? "float" : "double",
                           ast->fval);
            break;
        case CTYPE_ENUM:
            string_appendf(buf, "(literal enum %d)", ast->ival);
            break;
        default:
            string_appendf(buf, "(literal ? %lld)", ast->ival);
        }
        break;

    case AST_STRING:
        string_appendf(buf, "(string \"%s\")", quote_cstring(ast->sval));
        break;

    /* ── variables ───────────────────────────────── */
    case AST_LVAR:
        string_appendf(buf, "%s", ast->varname);
        break;
    case AST_GVAR:
        string_appendf(buf, "%s", ast->varname);
        break;

    /* ── function call ───────────────────────────── */
    case AST_FUNCALL: {
        string_appendf(buf, "(funcall %s", ctype_to_string(ast->ctype));
        if (ast->fnexpr) {
            string_appendf(buf, " (indirect ");
            sexpr_int(buf, ast->fnexpr);
            string_appendf(buf, ")");
        } else {
            string_appendf(buf, " %s", ast->fname);
        }
        for (Iter i = list_iter(ast->args); !iter_end(i);) {
            string_appendf(buf, " ");
            sexpr_int(buf, iter_next(&i));
        }
        string_appendf(buf, ")");
        break;
    }

    /* ── function declaration ────────────────────── */
    case AST_FUNC_DECL: {
        string_appendf(buf, "(func-decl %s %s", ctype_to_string(ast->ctype), ast->fname);
        if (ast->platform_info.mcs51.interrupt_no >= 0)
            string_appendf(buf, " (" PLAT_SEXPR_INTERRUPT " %d)", ast->platform_info.mcs51.interrupt_no);
        if (ast->platform_info.mcs51.using_no >= 0)
            string_appendf(buf, " (" PLAT_SEXPR_USING " %d)", ast->platform_info.mcs51.using_no);
        for (Iter i = list_iter(ast->params); !iter_end(i);) {
            Ast *p = iter_next(&i);
            string_appendf(buf, " (param %s %s)", ctype_to_string(p->ctype), p->varname ? p->varname : "?");
        }
        string_appendf(buf, ")");
        break;
    }

    /* ── function definition ─────────────────────── */
    case AST_FUNC_DEF: {
        string_appendf(buf, "(func-def %s %s", ctype_to_string(ast->ctype), ast->fname);
        if (ast->platform_info.mcs51.interrupt_no >= 0)
            string_appendf(buf, " (" PLAT_SEXPR_INTERRUPT " %d)", ast->platform_info.mcs51.interrupt_no);
        if (ast->platform_info.mcs51.using_no >= 0)
            string_appendf(buf, " (" PLAT_SEXPR_USING " %d)", ast->platform_info.mcs51.using_no);
        string_appendf(buf, " (params");
        for (Iter i = list_iter(ast->params); !iter_end(i);) {
            Ast *p = iter_next(&i);
            string_appendf(buf, " (param %s %s)", ctype_to_string(p->ctype), p->varname ? p->varname : "?");
        }
        string_appendf(buf, ")");
        string_appendf(buf, " ");
        sexpr_int(buf, ast->body);
        string_appendf(buf, ")");
        break;
    }

    /* ── declaration ─────────────────────────────── */
    case AST_DECL:
        string_appendf(buf, "(decl %s %s", ctype_to_string(ast->declvar->ctype), ast->declvar->varname);
        if (ast->declinit) {
            string_appendf(buf, " ");
            sexpr_int(buf, ast->declinit);
        }
        string_appendf(buf, ")");
        break;

    /* ── array / struct initializer ──────────────── */
    case AST_ARRAY_INIT:
        string_appendf(buf, "(array-init");
        for (Iter i = list_iter(ast->arrayinit); !iter_end(i);) {
            string_appendf(buf, " ");
            sexpr_int(buf, iter_next(&i));
        }
        string_appendf(buf, ")");
        break;
    case AST_STRUCT_INIT:
        string_appendf(buf, "(struct-init");
        for (Iter i = list_iter(ast->structinit); !iter_end(i);) {
            string_appendf(buf, " ");
            sexpr_int(buf, iter_next(&i));
        }
        string_appendf(buf, ")");
        break;

    /* ── control flow ────────────────────────────── */
    case AST_IF:
        string_appendf(buf, "(if ");
        sexpr_int(buf, ast->cond);
        string_appendf(buf, " ");
        sexpr_int(buf, ast->then);
        if (ast->els) {
            string_appendf(buf, " ");
            sexpr_int(buf, ast->els);
        }
        string_appendf(buf, ")");
        break;

    case AST_TERNARY:
        string_appendf(buf, "(ternary ");
        sexpr_int(buf, ast->cond);
        string_appendf(buf, " ");
        sexpr_int(buf, ast->then);
        string_appendf(buf, " ");
        sexpr_int(buf, ast->els);
        string_appendf(buf, ")");
        break;

    case AST_SWITCH: {
        string_appendf(buf, "(switch ");
        sexpr_int(buf, ast->ctrl);
        for (Iter i = list_iter(ast->cases); !iter_end(i);) {
            SwitchCase *c = iter_next(&i);
            if (c->low == c->high)
                string_appendf(buf, " (case %ld)", c->low);
            else
                string_appendf(buf, " (case %ld %ld)", c->low, c->high);
        }
        if (ast->default_label)
            string_appendf(buf, " (default)");
        if (ast->switch_body) {
            string_appendf(buf, " ");
            sexpr_int(buf, ast->switch_body);
        }
        string_appendf(buf, ")");
        break;
    }

    case AST_FOR:
        string_appendf(buf, "(for ");
        sexpr_int(buf, ast->forinit);
        string_appendf(buf, " ");
        sexpr_int(buf, ast->forcond);
        string_appendf(buf, " ");
        sexpr_int(buf, ast->forstep);
        string_appendf(buf, " ");
        sexpr_int(buf, ast->forbody);
        string_appendf(buf, ")");
        break;

    case AST_WHILE:
        string_appendf(buf, "(while ");
        sexpr_int(buf, ast->while_cond);
        string_appendf(buf, " ");
        sexpr_int(buf, ast->while_body);
        string_appendf(buf, ")");
        break;

    case AST_DO_WHILE:
        string_appendf(buf, "(do-while ");
        sexpr_int(buf, ast->while_body);
        string_appendf(buf, " ");
        sexpr_int(buf, ast->while_cond);
        string_appendf(buf, ")");
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
        string_appendf(buf, "(return ");
        sexpr_int(buf, ast->retval);
        string_appendf(buf, ")");
        break;
    case AST_ASM:
        string_appendf(buf, "(asm \"%s\")", ast->asm_body ? ast->asm_body : "");
        break;
    case AST_STATIC_ASSERT:
        string_appendf(buf, "(static_assert \"%s\")", ast->els ? (char*)ast->els : "");
        break;

    /* ── compound statement ──────────────────────── */
    case AST_COMPOUND_STMT: {
        string_appendf(buf, "(compound");
        for (Iter i = list_iter(ast->stmts); !iter_end(i);) {
            string_appendf(buf, " ");
            sexpr_int(buf, iter_next(&i));
        }
        string_appendf(buf, ")");
        break;
    }

    /* ── struct / bit reference ──────────────────── */
    case AST_STRUCT_REF: {
        /* 输出字段的逻辑 slot 偏移，供 SSA 后端直接使用
         * 逻辑 slot = init_vals 数组中该字段的索引（union 字段共享 slot） */
        Ctype *base_ctype = ast->struc ? ast->struc->ctype : NULL;
        /* 如果 base 是指针（->），解引用 */
        if (base_ctype && base_ctype->type == CTYPE_PTR) base_ctype = base_ctype->ptr;
        int slot_off = struct_field_logical_slot(base_ctype, ast->field);
        string_appendf(buf, "(struct-ref ");
        sexpr_int(buf, ast->struc);
        string_appendf(buf, " %s %d)", ast->field, slot_off);
        break;
    }
    case AST_BIT_REF:
        string_appendf(buf, "(bit-ref ");
        sexpr_int(buf, ast->struc);
        string_appendf(buf, " %d)", ast->bit_index);
        break;

    /* ── type definitions ────────────────────────── */
    case AST_STRUCT_DEF:
        string_appendf(buf, "(struct-def %s)", ctype_to_string(ast->ctype));
        break;
    case AST_ENUM_DEF:
        string_appendf(buf, "(enum-def %s)", ctype_to_string(ast->ctype));
        break;
    case AST_TYPE_DEF:
        string_appendf(buf, "(typedef %s %s)", ast->typename, ctype_to_string(ast->ctype));
        break;

    /* ── cast ────────────────────────────────────── */
    case AST_CAST:
        string_appendf(buf, "(cast %s ", ctype_to_string(ast->ctype));
        sexpr_int(buf, ast->cast_expr);
        string_appendf(buf, ")");
        break;

    /* ── unary operators ─────────────────────────── */
    case AST_ADDR:     sexpr_uop(buf, "addr",     ast); break;
    case AST_DEREF:
        /* 若解引用结果类型为数组（如 arr[i] 的类型是 char[N]），
         * 则不需要 deref，直接输出内部地址表达式即可
         * （在 bril_sim 平坦内存模型中，行地址即为算术偏移） */
        if (ast->ctype && ast->ctype->type == CTYPE_ARRAY) {
            sexpr_int(buf, ast->operand);
        } else {
            sexpr_uop(buf, "deref", ast);
        }
        break;
    case PUNCT_INC:    sexpr_uop(buf, "pre-inc",  ast); break;
    case PUNCT_DEC:    sexpr_uop(buf, "pre-dec",  ast); break;
    case AST_POST_INC: sexpr_uop(buf, "post-inc", ast); break;
    case AST_POST_DEC: sexpr_uop(buf, "post-dec", ast); break;
    case '!':          sexpr_uop(buf, "not",      ast); break;
    case '~':          sexpr_uop(buf, "bitnot",   ast); break;

    /* ── binary / logic ──────────────────────────── */
    case PUNCT_LOGAND: sexpr_binop(buf, "and", ast); break;
    case PUNCT_LOGOR:  sexpr_binop(buf, "or",  ast); break;
    case '&':          sexpr_binop(buf, "bitand", ast); break;
    case '|':          sexpr_binop(buf, "bitor",  ast); break;
    case '^':          sexpr_binop(buf, "bitxor", ast); break;
    case PUNCT_LSHIFT: sexpr_binop(buf, "shl", ast); break;
    case PUNCT_RSHIFT: sexpr_binop(buf, "shr", ast); break;

    /* compound assignment */
    case PUNCT_SHL_ASSIGN: sexpr_binop(buf, "shl-assign", ast); break;
    case PUNCT_SHR_ASSIGN: sexpr_binop(buf, "shr-assign", ast); break;
    case PUNCT_AND_ASSIGN: sexpr_binop(buf, "and-assign", ast); break;
    case PUNCT_OR_ASSIGN:  sexpr_binop(buf, "or-assign",  ast); break;
    case PUNCT_XOR_ASSIGN: sexpr_binop(buf, "xor-assign", ast); break;
    case PUNCT_ADD_ASSIGN: sexpr_binop(buf, "add-assign", ast); break;
    case PUNCT_SUB_ASSIGN: sexpr_binop(buf, "sub-assign", ast); break;
    case PUNCT_MUL_ASSIGN: sexpr_binop(buf, "mul-assign", ast); break;
    case PUNCT_DIV_ASSIGN: sexpr_binop(buf, "div-assign", ast); break;
    case PUNCT_MOD_ASSIGN: sexpr_binop(buf, "mod-assign", ast); break;

    /* ── remaining binary ops (default) ──────────── */
    default: {
        const char *op;
        switch (ast->type) {
        case '+': op = "add"; break;
        case '-': op = "sub"; break;
        case '*': op = "mul"; break;
        case '/': op = "div"; break;
        case '%': op = "mod"; break;
        case '<': op = "lt";  break;
        case '>': op = "gt";  break;
        case '=': op = "assign"; break;
        case ',': op = "comma";  break;
        case PUNCT_EQ: op = "eq"; break;
        case PUNCT_GE: op = "ge"; break;
        case PUNCT_LE: op = "le"; break;
        case PUNCT_NE: op = "ne"; break;
        default: {
            /* fallback: print type code */
            static char tmp[16];
            if (ast->type > 32 && ast->type < 127)

                snprintf(tmp, sizeof(tmp), "%c", ast->type);
            else
                snprintf(tmp, sizeof(tmp), "op%d", ast->type);
            op = tmp;
            break;
        }
        }
        /* 指针算术步长处理：若左/右操作数是指针（或数组退化为指针）且指向类型占多个 slot，
         * 需在 sexpr 中把整数偏移乘以步长（bril_sim 内部 ptr+int 默认 ×1 slot） */
        if ((ast->type == '+' || ast->type == '-') && ast->left && ast->right) {
            /* 确定哪侧是指针/数组，哪侧是整数索引 */
            Ctype *lct = ast->left->ctype;
            Ctype *rct = ast->right->ctype;
            /* 获取指向的元素类型（数组退化时用 arr->ptr，指针类型用 ptr->ptr） */
            Ctype *elem_ctype = NULL;
            Ast   *idx_ast    = NULL;
            int    ptr_is_left = 1;
            if (lct && (lct->type == CTYPE_ARRAY || lct->type == CTYPE_PTR) && lct->ptr &&
                rct && rct->type != CTYPE_PTR && rct->type != CTYPE_ARRAY) {
                elem_ctype  = lct->ptr;  /* 左是 ptr/array，右是索引 */
                idx_ast     = ast->right;
                ptr_is_left = 1;
            } else if (rct && (rct->type == CTYPE_PTR) && rct->ptr &&
                       lct && lct->type != CTYPE_PTR && lct->type != CTYPE_ARRAY) {
                elem_ctype  = rct->ptr;  /* 右是 ptr，左是索引（int+ptr） */
                idx_ast     = ast->left;
                ptr_is_left = 0;
            }
            if (elem_ctype && idx_ast) {
                int stride = field_logical_slots(elem_ctype);
                if (stride > 1) {
                    /* 生成 (add/sub ptr (mul idx stride)) */
                    string_appendf(buf, "(%s ", op);
                    if (ptr_is_left) {
                        sexpr_int(buf, ast->left);   /* ptr/array side */
                        string_appendf(buf, " (mul ");
                        sexpr_int(buf, idx_ast);
                        string_appendf(buf, " (literal int %d))", stride);
                    } else {
                        sexpr_int(buf, ast->right);   /* ptr side（int + ptr 情况） */
                        string_appendf(buf, " (mul ");
                        sexpr_int(buf, idx_ast);
                        string_appendf(buf, " (literal int %d))", stride);
                    }
                    string_appendf(buf, ")");
                    break;
                }
            }
        }
        sexpr_binop(buf, op, ast);
        break;
    }
    }
}

/* ── public API ───────────────────────────────────────────── */

char *ast_to_sexpr(Ast *ast)
{
    String s = make_string();
    sexpr_int(&s, ast);
    return get_cstring(s);
}
