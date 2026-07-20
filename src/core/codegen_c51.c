#include "codegen_c51.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

static int g_c51_mem_model = 0;
void c51_set_mem_model(int model) { g_c51_mem_model = model; }

/* asm 检测（供 c51_emit_translation_unit 使用）*/
static bool g_detected_asm = false;
static void detect_asm_visitor(Ast *ast) {
    if (ast && ast->type == AST_ASM) g_detected_asm = true;
}

/* 已发射的 SFR/sfr16/sbit 名称（去重用）*/
static Dict *g_emitted_sfr = NULL;
/* 已发射的 typedef 名称（去重用）*/
static Dict *g_emitted_typedef = NULL;
/* 已发射的函数声明名称（去重用）*/
static Dict *g_emitted_funcdecl = NULL;

/* ── 中文→ASCII 标识符映射 ── */
/* key=ASCII别名, val=中文原名, 如 "_CN_1" → "阶乘"  */
static Dict *g_name_map = NULL;
static int g_name_counter = 0;
static bool g_map_names_enabled = true;

/* 若 map_names=true 且名字含非 ASCII 字符，返回 ASCII 别名；
 * 否则原样返回。 */
static const char *c51_map_name(const char *name) {
    if (!name || !g_map_names_enabled) return name;
    bool has_non_ascii = false;
    for (const unsigned char *p = (const unsigned char*)name; *p; p++) {
        if (*p >= 0x80) { has_non_ascii = true; break; }
    }
    if (!has_non_ascii) return name;

    /* 正查：已有别名则直接返回 */
    if (g_name_map) {
        /* dict_get 按 key 查询，这里需要 val→key 反查，所以遍历 */
        for (int i = 0; i < list_len(g_name_map->list); i++) {
            DictEntry *e = (DictEntry*)list_get(g_name_map->list, i);
            if (e->val && !strcmp((const char*)e->val, name))
                return e->key;
        }
    }
    /* 生成新别名 _CN_N */
    char alias[64];
    snprintf(alias, sizeof(alias), "_CN_%d", ++g_name_counter);
    if (!g_name_map) g_name_map = make_dict(NULL);
    dict_put(g_name_map, strdup(alias), strdup(name));
    /* 返回持久化的别名（已存入 dict，其 key 是持久化的） */
    for (int i = 0; i < list_len(g_name_map->list); i++) {
        DictEntry *e = (DictEntry*)list_get(g_name_map->list, i);
        if (!strcmp(e->key, alias)) return e->key;
    }
    return NULL; /* unreachable */
}

Dict *c51_get_name_map(void) { return g_name_map; }

static void c51_emit_decl(FILE *out, Ast *ast);
static void c51_emit_init(FILE *out, Ast *init);
static void c51_emit_stmt(FILE *out, Ast *ast, int indent);
static void c51_emit_expr(FILE *out, Ast *ast);
static void c51_emit_indent(FILE *out, int indent);
static void c51_emit_struct_def(FILE *out, Ctype *ctype);

static void c51_emit_indent(FILE *out, int indent) {
    for (int i = 0; i < indent; i++) fprintf(out, "    ");
}

static void c51_emit_memory_attr(FILE *out, CtypeAttr a) {
    if (a.ctype_static)   fprintf(out, "static ");
    if (a.ctype_extern)   fprintf(out, "extern ");
    if (a.ctype_c51_data)  fprintf(out, "data ");
    else if (a.ctype_c51_idata) fprintf(out, "idata ");
    else if (a.ctype_c51_xdata) fprintf(out, "xdata ");
    else if (a.ctype_c51_pdata) fprintf(out, "pdata ");
    else if (a.ctype_c51_code)  fprintf(out, "code ");
    else if (a.ctype_c51_bdata) fprintf(out, "bdata ");
    else if (a.ctype_c51_near)  fprintf(out, "near ");
    else if (a.ctype_c51_far)   fprintf(out, "far ");
    if (a.ctype_const)    fprintf(out, "const ");
    if (a.ctype_volatile) fprintf(out, "volatile ");
}

static const char *c51_base_type_name(Ctype *ctype) {
    if (!ctype) return "int";
    if (ctype->type < CTYPE_VOID || ctype->type > CTYPE_DOUBLE) return "int";
    CtypeAttr a = get_attr(ctype->attr);
    if (a.ctype_c51_sfr)   return "sfr";
    if (a.ctype_c51_sfr16) return "sfr16";
    if (a.ctype_c51_sbit)  return "sbit";
    if (a.ctype_c51_bit)   return "bit";
    switch (ctype->type) {
    case CTYPE_VOID:   return "void";
    case CTYPE_BOOL:   return "bit";
    case CTYPE_CHAR:   return a.ctype_unsigned ? "unsigned char" : "signed char";
    case CTYPE_INT:
        if (ctype->size <= 2) return a.ctype_unsigned ? "unsigned int" : "int";
        return a.ctype_unsigned ? "unsigned long" : "long";
    case CTYPE_LONG:   return a.ctype_unsigned ? "unsigned long" : "long";
    case CTYPE_FLOAT:  return "float";
    case CTYPE_DOUBLE: return "float";
    default: return "int";
    }
}

static void c51_emit_type_prefix(FILE *out, Ctype *ctype) {
    if (!ctype) return;
    /* 找到最内层非指针/非数组的基础类型 */
    Ctype *base = ctype;
    while (base && (base->type == CTYPE_PTR || base->type == CTYPE_ARRAY))
        base = base->ptr;
    if (!base) { fprintf(out, "int"); return; }
    /* 使用基类型的属性 */
    CtypeAttr a = get_attr(base->attr);
    c51_emit_memory_attr(out, a);
    /* struct/union 类型输出标签名（不展开定义）*/
    if (base->type == CTYPE_STRUCT) {
        fprintf(out, "%s %s", base->is_union ? "union" : "struct",
                base->tag ? base->tag : "?");
    } else {
        fprintf(out, "%s", c51_base_type_name(base));
    }
}

/* 输出指针 * 前缀（在变量名前），不包括 const/volatile 限定
 * 注意：parser 目前无法区分 const int *p 和 int *const p，
 * 若输出 const 会导致 C51 C183 unmodifiable lvalue 错误 */
static void c51_emit_ptr_prefix(FILE *out, Ctype *ctype) {
    if (!ctype) return;
    if (ctype->type == CTYPE_PTR) {
        fprintf(out, "*");
        c51_emit_ptr_prefix(out, ctype->ptr);
    }
}

/* 输出数组后缀 [N]（在变量名后）*/
static void c51_emit_array_suffix(FILE *out, Ctype *ctype) {
    if (!ctype) return;
    if (ctype->type == CTYPE_ARRAY) {
        if (ctype->len > 0) fprintf(out, "[%d]", ctype->len);
        else fprintf(out, "[]");
        c51_emit_array_suffix(out, ctype->ptr);
    }
    if (ctype->type == CTYPE_PTR)
        c51_emit_array_suffix(out, ctype->ptr);
}

/* 输出类型后缀（旧接口，兼容旧调用）*/
static void c51_emit_type_suffix(FILE *out, Ctype *ctype) {
    /* deprecated - use c51_emit_ptr_prefix + c51_emit_array_suffix instead */
    (void)out; (void)ctype;
}

static void c51_emit_struct_def(FILE *out, Ctype *ctype) {
    if (!ctype || ctype->type != CTYPE_STRUCT || !ctype->fields)
        { fprintf(out, "int"); return; }
    fprintf(out, ctype->is_union ? "union" : "struct");
    if (ctype->tag && ctype->tag[0]) fprintf(out, " %s", ctype->tag);
    fprintf(out, " {\n");
    List *keys = dict_keys(ctype->fields);
    List *vals = dict_values(ctype->fields);
    for (int i = 0; i < list_len(keys); i++) {
        char *fname = (char*)list_get(keys, i);
        Ctype *ft = (Ctype*)list_get(vals, i);
        fprintf(out, "    ");
        c51_emit_type_prefix(out, ft);
        fprintf(out, " ");
        c51_emit_ptr_prefix(out, ft);
        fprintf(out, "%s", c51_map_name(fname));
        if (ft->bit_size > 0) fprintf(out, " : %d", ft->bit_size);
        c51_emit_array_suffix(out, ft);
        fprintf(out, ";\n");
    }
    free(keys); free(vals);
    fprintf(out, "  }");
}

static void c51_emit_sfr_decl(FILE *out, Ast *ast) {
    Ctype *ct = ast->declvar->ctype;
    CtypeAttr a = get_attr(ct->attr);
    if (!ast->declinit) return;
    /* 去重：同一 SFR 名称只输出一次 */
    if (g_emitted_sfr && dict_get(g_emitted_sfr, ast->declvar->varname))
        return;
    if (g_emitted_sfr)
        dict_put(g_emitted_sfr, strdup(ast->declvar->varname), (void*)1);
    unsigned long addr = 0;
    if (ast->declinit->type == AST_LITERAL)
        addr = (unsigned long)ast->declinit->ival;
    if (a.ctype_c51_sfr)
        fprintf(out, "sfr  %s = 0x%02lX;\n", c51_map_name(ast->declvar->varname), addr);
    else if (a.ctype_c51_sfr16)
        fprintf(out, "sfr16 %s = 0x%04lX;\n", c51_map_name(ast->declvar->varname), addr);
    else if (a.ctype_c51_sbit) {
        if (ast->declinit->type == AST_LITERAL)
            fprintf(out, "sbit %s = 0x%02lX;\n", c51_map_name(ast->declvar->varname), addr);
        else {
            /* sbit 表达式（例如 P0^6）：直接输出表达式 */
            fprintf(out, "sbit %s = ", c51_map_name(ast->declvar->varname));
            c51_emit_expr(out, ast->declinit);
            fprintf(out, ";\n");
        }
    }
}

static void c51_emit_decl(FILE *out, Ast *ast) {
    if (!ast || !ast->declvar) return;
    Ctype *ct = ast->declvar->ctype;
    if (!ct) return;
    CtypeAttr a = get_attr(ct->attr);
    if (a.ctype_c51_sfr || a.ctype_c51_sfr16 || a.ctype_c51_sbit) {
        c51_emit_sfr_decl(out, ast); return;
    }
    if (a.ctype_c51_bit) {
        c51_emit_memory_attr(out, a);
        fprintf(out, "bit %s", c51_map_name(ast->declvar->varname));
        if (ast->declinit) { fprintf(out, " = "); c51_emit_expr(out, ast->declinit); }
        fprintf(out, ";\n"); return;
    }
    /* 输出完整声明：存储/内存属性 + 基础类型 + 指针* + 变量名 + 数组[] */
    if (ct->type == CTYPE_STRUCT && !ct->tag) {
        /* 匿名 struct：必须内联展开定义 */
        c51_emit_memory_attr(out, a);
        c51_emit_struct_def(out, ct);
        fprintf(out, " %s", c51_map_name(ast->declvar->varname));
    } else {
        c51_emit_type_prefix(out, ct);
        fprintf(out, " ");
        c51_emit_ptr_prefix(out, ct);
        fprintf(out, "%s", c51_map_name(ast->declvar->varname));
        c51_emit_array_suffix(out, ct);
    }
    if (ast->declinit) {
        fprintf(out, " = ");
        /* 当初始值是变量引用且目标为 struct/array 时，尝试展开内联 */
        Ast *init = ast->declinit;
        if ((ct->type == CTYPE_STRUCT || ct->type == CTYPE_ARRAY) &&
            (init->type == AST_GVAR || init->type == AST_LVAR)) {
            Ast *inline_init = NULL;
            if (init->type == AST_GVAR && init->ginit) {
                inline_init = init->ginit;
            }
            if (inline_init && (inline_init->type == AST_STRUCT_INIT || inline_init->type == AST_ARRAY_INIT)) {
                fprintf(out, "{");
                c51_emit_init(out, inline_init);
                fprintf(out, "}");
            } else {
                c51_emit_expr(out, init);
            }
        } else if (init->type == AST_ARRAY_INIT || init->type == AST_STRUCT_INIT) {
            /* 指针类型目标不能用初始化列表赋值 */
            if (ct->type == CTYPE_PTR) {
                c51_emit_expr(out, init);
            } else {
                fprintf(out, "{");
                c51_emit_init(out, init);
                fprintf(out, "}");
            }
        } else {
            c51_emit_expr(out, init);
        }
    }
    fprintf(out, ";\n");
}

static void c51_emit_init(FILE *out, Ast *init) {
    if (!init) { fprintf(out, "0"); return; }
    if (init->type == AST_ARRAY_INIT && init->arrayinit) {
        for (int i = 0; i < list_len(init->arrayinit); i++) {
            if (i > 0) fprintf(out, ", ");
            Ast *elem = (Ast*)list_get(init->arrayinit, i);
            if (!elem) { fprintf(out, "0"); continue; }
            if (elem->type == AST_ARRAY_INIT || elem->type == AST_STRUCT_INIT) {
                fprintf(out, "{"); c51_emit_init(out, elem); fprintf(out, "}");
            } else c51_emit_expr(out, elem);
        }
    } else if (init->type == AST_STRUCT_INIT && init->structinit) {
        for (int i = 0; i < list_len(init->structinit); i++) {
            if (i > 0) fprintf(out, ", ");
            Ast *elem = (Ast*)list_get(init->structinit, i);
            if (!elem) { fprintf(out, "0"); continue; }
            if (elem->type == AST_ARRAY_INIT || elem->type == AST_STRUCT_INIT) {
                fprintf(out, "{"); c51_emit_init(out, elem); fprintf(out, "}");
            } else c51_emit_expr(out, elem);
        }
    } else c51_emit_expr(out, init);
}

static void c51_emit_func_def(FILE *out, Ast *ast) {
    Ctype *ret = ast->ctype;
    if (ret) {
        CtypeAttr ra = get_attr(ret->attr);
        if (ret->type == CTYPE_STRUCT && ret->tag) {
            fprintf(out, "%s %s", ret->is_union ? "union" : "struct", ret->tag);
            fprintf(out, " ");
        } else if (ret->type == CTYPE_STRUCT) {
            c51_emit_struct_def(out, ret);
            fprintf(out, " ");
        } else {
            c51_emit_type_prefix(out, ret);
            fprintf(out, " ");
            c51_emit_ptr_prefix(out, ret);
        }
    } else {
        fprintf(out, "void ");
    }
    fprintf(out, "%s(", c51_map_name(ast->fname));
    if (ast->params) {
        for (int i = 0; i < list_len(ast->params); i++) {
            Ast *p = (Ast*)list_get(ast->params, i);
            if (i > 0) fprintf(out, ", ");
            if (p->ctype) {
                c51_emit_type_prefix(out, p->ctype);
                fprintf(out, " ");
                c51_emit_ptr_prefix(out, p->ctype);
                if (p->varname) {
                    fprintf(out, "%s", c51_map_name(p->varname));
                    c51_emit_array_suffix(out, p->ctype);
                }
            }
        }
    }
    fprintf(out, ")");
    if (ast->platform_info.mcs51.interrupt_no >= 0)
        fprintf(out, " interrupt %d", ast->platform_info.mcs51.interrupt_no);
    if (ast->platform_info.mcs51.using_no >= 0)
        fprintf(out, " using %d", ast->platform_info.mcs51.using_no);
    fprintf(out, "\n");
    c51_emit_stmt(out, ast->body, 0);
}

static void c51_emit_func_decl(FILE *out, Ast *ast) {
    if (ast->ctype) {
        c51_emit_type_prefix(out, ast->ctype);
        fprintf(out, " ");
        c51_emit_ptr_prefix(out, ast->ctype);
    }
    fprintf(out, "%s(", c51_map_name(ast->fname));
    if (ast->params) {
        for (int i = 0; i < list_len(ast->params); i++) {
            Ast *p = (Ast*)list_get(ast->params, i);
            if (i > 0) fprintf(out, ", ");
            if (p->ctype) {
                c51_emit_type_prefix(out, p->ctype);
                fprintf(out, " ");
                c51_emit_ptr_prefix(out, p->ctype);
                if (p->varname) {
                    fprintf(out, "%s", c51_map_name(p->varname));
                    c51_emit_array_suffix(out, p->ctype);
                }
            }
        }
    }
    fprintf(out, ")");
    if (ast->platform_info.mcs51.interrupt_no >= 0)
        fprintf(out, " interrupt %d", ast->platform_info.mcs51.interrupt_no);
    if (ast->platform_info.mcs51.using_no >= 0)
        fprintf(out, " using %d", ast->platform_info.mcs51.using_no);
    fprintf(out, ";\n");
}

static void c51_emit_stmt(FILE *out, Ast *ast, int indent) {
    if (!ast) { fprintf(out, ";\n"); return; }
    switch (ast->type) {
    case AST_COMPOUND_STMT:
        /* 多声明平铺：纯 decl 且非最外层函数体时，不额外加 {} */
        if (ast->stmts && list_len(ast->stmts) > 1 && indent > 0) {
            bool all_decls = true;
            for (int i = 0; i < list_len(ast->stmts); i++) {
                Ast *s = (Ast*)list_get(ast->stmts, i);
                if (s->type != AST_DECL) { all_decls = false; break; }
            }
            if (all_decls) {
                for (int i = 0; i < list_len(ast->stmts); i++) {
                    Ast *s = (Ast*)list_get(ast->stmts, i);
                    c51_emit_indent(out, indent);
                    c51_emit_stmt(out, s, indent);
                }
                break;
            }
        }
        fprintf(out, "{\n");
        if (ast->stmts) {
            for (int i = 0; i < list_len(ast->stmts); i++) {
                Ast *s = (Ast*)list_get(ast->stmts, i);
                c51_emit_indent(out, indent + 1);
                c51_emit_stmt(out, s, indent + 1);
            }
        }
        c51_emit_indent(out, indent);
        fprintf(out, "}\n");
        break;
    case AST_DECL:
        c51_emit_decl(out, ast);
        break;
    case AST_IF:
        fprintf(out, "if (");
        c51_emit_expr(out, ast->cond);
        fprintf(out, ")\n");
        c51_emit_indent(out, indent + 1);
        c51_emit_stmt(out, ast->then, indent + 1);
        if (ast->els) {
            c51_emit_indent(out, indent);
            fprintf(out, "else\n");
            c51_emit_indent(out, indent + 1);
            c51_emit_stmt(out, ast->els, indent + 1);
        }
        break;
    case AST_FOR:
        fprintf(out, "for (");
        if (ast->forinit) c51_emit_expr(out, ast->forinit);
        fprintf(out, "; ");
        if (ast->forcond) c51_emit_expr(out, ast->forcond);
        fprintf(out, "; ");
        if (ast->forstep) c51_emit_expr(out, ast->forstep);
        fprintf(out, ")\n");
        c51_emit_indent(out, indent + 1);
        c51_emit_stmt(out, ast->forbody, indent + 1);
        break;
    case AST_WHILE:
        fprintf(out, "while (");
        c51_emit_expr(out, ast->while_cond);
        fprintf(out, ")\n");
        c51_emit_indent(out, indent + 1);
        c51_emit_stmt(out, ast->while_body, indent + 1);
        break;
    case AST_DO_WHILE:
        fprintf(out, "do\n");
        c51_emit_indent(out, indent + 1);
        c51_emit_stmt(out, ast->while_body, indent + 1);
        c51_emit_indent(out, indent);
        fprintf(out, "while (");
        c51_emit_expr(out, ast->while_cond);
        fprintf(out, ");\n");
        break;
    case AST_SWITCH:
        fprintf(out, "switch (");
        c51_emit_expr(out, ast->ctrl);
        fprintf(out, ")\n");
        fprintf(out, "{\n");
        /* 输出 switch body — 展开各 case 的标签包装 */
        if (ast->switch_body && ast->switch_body->stmts) {
            for (int i = 0; i < list_len(ast->switch_body->stmts); i++) {
                Ast *s = (Ast*)list_get(ast->switch_body->stmts, i);
                /* case/default 被解析为 compound_stmt{label, stmt}，展开之 */
                if (s->type == AST_COMPOUND_STMT && s->stmts && list_len(s->stmts) > 0) {
                    Ast *first = (Ast*)list_get(s->stmts, 0);
                    if (first && first->type == AST_LABEL) {
                        /* 通过标签名匹配 case/default */
                        char *lbl = first->label;
                        bool found = false;
                        if (ast->default_label && !strcmp(ast->default_label, lbl)) {
                            fprintf(out, "default:\n");
                            found = true;
                        } else if (ast->cases) {
                            for (int ci = 0; ci < list_len(ast->cases); ci++) {
                                SwitchCase *sc = (SwitchCase*)list_get(ast->cases, ci);
                                if (sc->label && !strcmp(sc->label, lbl)) {
                                    if (sc->low == sc->high)
                                        fprintf(out, "case %ld:\n", sc->low);
                                    else
                                        fprintf(out, "case %ld ... %ld:\n", sc->low, sc->high);
                                    found = true;
                                    break;
                                }
                            }
                        }
                        /* 输出来自 case wrapper 中的语句（跳过 label 本身）*/
                        for (int j = 0; j < list_len(s->stmts); j++) {
                            Ast *inner = (Ast*)list_get(s->stmts, j);
                            if (inner->type == AST_LABEL) continue;
                            c51_emit_indent(out, indent + 1);
                            c51_emit_stmt(out, inner, indent + 1);
                        }
                        continue;
                    }
                }
                /* 常规语句（如 break 等直接输出） */
                c51_emit_indent(out, indent + 1);
                c51_emit_stmt(out, s, indent + 1);
            }
        }
        c51_emit_indent(out, indent);
        fprintf(out, "}\n");
        break;
    case AST_RETURN:
        fprintf(out, "return");
        if (ast->retval) { fprintf(out, " "); c51_emit_expr(out, ast->retval); }
        fprintf(out, ";\n");
        break;
    case AST_LABEL:
        fprintf(out, "%s:\n", c51_map_name(ast->label));
        break;
    case AST_GOTO:
        fprintf(out, "goto %s;\n", c51_map_name(ast->label));
        break;
    case AST_BREAK:
        fprintf(out, "break;\n");
        break;
    case AST_CONTINUE:
        fprintf(out, "continue;\n");
        break;
    case AST_ASM:
        fprintf(out, "#pragma asm\n");
        fprintf(out, "%s\n", ast->asm_body ? ast->asm_body : "");
        fprintf(out, "#pragma endasm\n");
        break;
    case AST_STATIC_ASSERT:
        /* _Static_assert 已在编译期求值，不输出任何内容 */
        break;
    default:
        c51_emit_expr(out, ast);
        fprintf(out, ";\n");
        break;
    }
}

/* 转义字符串中的特殊字符，输出到文件 */
static void c51_emit_escaped_string(FILE *out, const char *s) {
    if (!s) { fprintf(out, "\"\""); return; }
    fputc('"', out);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        case '\\': fputs("\\\\", out); break;
        case '"':  fputs("\\\"", out); break;
        default:
            if (c >= 0x20 && c < 0x7F)
                fputc(c, out);
            else
                fprintf(out, "\\x%02X", c);
            break;
        }
    }
    fputc('"', out);
}

static void c51_emit_expr(FILE *out, Ast *ast) {
    if (!ast) { fprintf(out, "0"); return; }
    switch (ast->type) {
    case AST_LITERAL:
        if (!ast->ctype) { fprintf(out, "%lld", ast->ival); break; }
        switch (ast->ctype->type) {
        case CTYPE_BOOL: fprintf(out, "%s", ast->ival ? "1" : "0"); break;
        case CTYPE_CHAR:
            if (ast->ival >= 0x20 && ast->ival < 0x7F && ast->ival != '\'' && ast->ival != '\\')
                fprintf(out, "'%c'", (char)ast->ival);
            else fprintf(out, "%d", (int)ast->ival);
            break;
        case CTYPE_FLOAT: case CTYPE_DOUBLE: fprintf(out, "%g", ast->fval); break;
        default: fprintf(out, "%lld", ast->ival); break;
        } break;
    case AST_STRING: c51_emit_escaped_string(out, ast->sval); break;
    case AST_LVAR: case AST_GVAR: fprintf(out, "%s", c51_map_name(ast->varname)); break;
    case AST_FUNC_DECL: case AST_FUNC_DEF: fprintf(out, "%s", c51_map_name(ast->fname)); break;
    case AST_FUNCALL:
        if (ast->fnexpr) {
            /* 函数指针调用需要括号包裹 */
            if (ast->fnexpr->type == AST_STRUCT_REF || ast->fnexpr->type == AST_DEREF ||
                ast->fnexpr->type == AST_LVAR || ast->fnexpr->type == AST_GVAR) {
                c51_emit_expr(out, ast->fnexpr);
            } else {
                fprintf(out, "("); c51_emit_expr(out, ast->fnexpr); fprintf(out, ")");
            }
        }
        else fprintf(out, "%s", c51_map_name(ast->fname));
        fprintf(out, "(");
        if (ast->args) for (int i = 0; i < list_len(ast->args); i++) {
            if (i > 0) fprintf(out, ", ");
            c51_emit_expr(out, (Ast*)list_get(ast->args, i));
        }
        fprintf(out, ")");
        break;
    case AST_ADDR: fprintf(out, "&"); c51_emit_expr(out, ast->operand); break;
    case AST_DEREF: {
        /* C51 不支持指针算术，将 *(ptr + idx) 转为 ptr[idx] */
        Ast *op = ast->operand;
        if (op && (op->type == '+' || op->type == PUNCT_ADD_ASSIGN) &&
            (op->left->type == AST_GVAR || op->left->type == AST_LVAR)) {
            /* 数组名 + 索引 → ptr[idx] */
            c51_emit_expr(out, op->left);
            fprintf(out, "[");
            c51_emit_expr(out, op->right);
            fprintf(out, "]");
        } else {
            fprintf(out, "(*");
            c51_emit_expr(out, op);
            fprintf(out, ")");
        }
        break;
    }
    case AST_CAST:
        fprintf(out, "(");
        if (ast->ctype) { c51_emit_type_prefix(out, ast->ctype); c51_emit_type_suffix(out, ast->ctype); }
        fprintf(out, ")");
        c51_emit_expr(out, ast->operand ? ast->operand : ast->cast_expr);
        break;
    case AST_POST_INC: c51_emit_expr(out, ast->operand); fprintf(out, "++"); break;
    case AST_POST_DEC: c51_emit_expr(out, ast->operand); fprintf(out, "--"); break;
    case AST_TERNARY:
        c51_emit_expr(out, ast->cond);
        fprintf(out, " ? "); c51_emit_expr(out, ast->then);
        fprintf(out, " : "); c51_emit_expr(out, ast->els);
        break;
    case AST_STRUCT_REF:
        if (ast->struc && ast->struc->type == AST_DEREF) {
            c51_emit_expr(out, ast->struc->operand);
            fprintf(out, "->%s", ast->field ? ast->field : "?");
        } else {
            c51_emit_expr(out, ast->struc);
            fprintf(out, ".%s", ast->field ? ast->field : "?");
        }
        break;
    case AST_BIT_REF:
        c51_emit_expr(out, ast->struc);
        if (ast->field) fprintf(out, ".%s", ast->field);
        else fprintf(out, ".%d", ast->bit_index);
        break;
    case PUNCT_INC: fprintf(out, "++"); c51_emit_expr(out, ast->operand); break;
    case PUNCT_DEC: fprintf(out, "--"); c51_emit_expr(out, ast->operand); break;
    case '!': fprintf(out, "!"); c51_emit_expr(out, ast->operand); break;
    case '~': fprintf(out, "~"); c51_emit_expr(out, ast->operand); break;
    case '%': fprintf(out, "("); c51_emit_expr(out, ast->left); fprintf(out, "%%"); c51_emit_expr(out, ast->right); fprintf(out, ")"); break;
    case PUNCT_MOD_ASSIGN: c51_emit_expr(out, ast->left); fprintf(out, "%%="); c51_emit_expr(out, ast->right); break;
    default:
        if (ast->type >= '+' && ast->type <= '/') {
            fprintf(out, "("); c51_emit_expr(out, ast->left);
            fprintf(out, "%c", ast->type);
            c51_emit_expr(out, ast->right); fprintf(out, ")");
        } else if (ast->type == '<' || ast->type == '>') {
            fprintf(out, "("); c51_emit_expr(out, ast->left);
            fprintf(out, "%c", ast->type);
            c51_emit_expr(out, ast->right); fprintf(out, ")");
        } else if (ast->type == '&' || ast->type == '|' || ast->type == '^') {
            fprintf(out, "("); c51_emit_expr(out, ast->left);
            fprintf(out, "%c", ast->type);
            c51_emit_expr(out, ast->right); fprintf(out, ")");
        } else if (ast->type == '=') {
            c51_emit_expr(out, ast->left); fprintf(out, "="); c51_emit_expr(out, ast->right);
        } else if (ast->type >= PUNCT_EQ && ast->type <= PUNCT_RSHIFT) {
            const char *op = "??";
            if (ast->type == PUNCT_EQ) op = "==";
            else if (ast->type == PUNCT_NE) op = "!=";
            else if (ast->type == PUNCT_LE) op = "<=";
            else if (ast->type == PUNCT_GE) op = ">=";
            else if (ast->type == PUNCT_LSHIFT) op = "<<";
            else if (ast->type == PUNCT_RSHIFT) op = ">>";
            else if (ast->type == PUNCT_LOGAND) op = "&&";
            else if (ast->type == PUNCT_LOGOR) op = "||";
            fprintf(out, "("); c51_emit_expr(out, ast->left);
            fprintf(out, "%s", op);
            c51_emit_expr(out, ast->right); fprintf(out, ")");
        } else if (ast->type >= PUNCT_SHL_ASSIGN && ast->type <= PUNCT_MOD_ASSIGN) {
            const char *op = "?=";
            if (ast->type == PUNCT_SHL_ASSIGN) op = "<<=";
            else if (ast->type == PUNCT_SHR_ASSIGN) op = ">>=";
            else if (ast->type == PUNCT_AND_ASSIGN) op = "&=";
            else if (ast->type == PUNCT_OR_ASSIGN) op = "|=";
            else if (ast->type == PUNCT_XOR_ASSIGN) op = "^=";
            else if (ast->type == PUNCT_ADD_ASSIGN) op = "+=";
            else if (ast->type == PUNCT_SUB_ASSIGN) op = "-=";
            else if (ast->type == PUNCT_MUL_ASSIGN) op = "*=";
            else if (ast->type == PUNCT_DIV_ASSIGN) op = "/=";
            c51_emit_expr(out, ast->left); fprintf(out, "%s", op); c51_emit_expr(out, ast->right);
        } else if (ast->type >= 256) {
            fprintf(out, "0");
        } else {
            c51_emit_expr(out, ast->left);
            fprintf(out, "%c", ast->type);
            c51_emit_expr(out, ast->right);
        }
        break;
    }
}

static void c51_emit_toplevel(FILE *out, Ast *ast) {
    if (!ast) return;
    switch (ast->type) {
    case AST_FUNC_DEF: c51_emit_func_def(out, ast); break;
    case AST_FUNC_DECL:
        /* 去重：同一函数名只声明一次 */
        if (ast->fname) {
            if (g_emitted_funcdecl && dict_get(g_emitted_funcdecl, ast->fname))
                break;
            if (g_emitted_funcdecl)
                dict_put(g_emitted_funcdecl, strdup(ast->fname), (void*)1);
        }
        c51_emit_func_decl(out, ast); break;
    case AST_DECL: c51_emit_decl(out, ast); break;
    case AST_STRUCT_DEF:
        if (ast->ctype && ast->ctype->type == CTYPE_STRUCT) {
            c51_emit_struct_def(out, ast->ctype); fprintf(out, ";\n");
        } break;
    case AST_TYPE_DEF:
        /* 去重：同一 typedef 名只定义一次 */
        {
            const char *tname = ast->typename ? ast->typename :
                                (ast->declvar && ast->declvar->varname ? ast->declvar->varname : NULL);
            if (tname) {
                if (g_emitted_typedef && dict_get(g_emitted_typedef, tname))
                    break;
                if (g_emitted_typedef)
                    dict_put(g_emitted_typedef, strdup(tname), (void*)1);
            }
        }
        if (ast->typename && ast->ctype) {
            fprintf(out, "typedef ");
            if (ast->ctype->type == CTYPE_ENUM && ast->ctype->fields) {
                /* typedef enum { ... } Name; 完整输出 */
                fprintf(out, "enum {\n");
                List *keys = dict_keys(ast->ctype->fields);
                for (int i = 0; i < list_len(keys); i++) {
                    fprintf(out, "    %s", (char*)list_get(keys, i));
                    if (i < list_len(keys) - 1) fprintf(out, ",");
                    fprintf(out, "\n");
                }
                free(keys);
                fprintf(out, "} %s;\n", c51_map_name(ast->typename));
            } else if (ast->ctype->type == CTYPE_STRUCT) {
                /* typedef struct { ... } Name; */
                c51_emit_struct_def(out, ast->ctype);
                fprintf(out, " %s;\n", c51_map_name(ast->typename));
            } else {
                c51_emit_type_prefix(out, ast->ctype);
                fprintf(out, " %s", c51_map_name(ast->typename));
                c51_emit_ptr_prefix(out, ast->ctype);
                c51_emit_array_suffix(out, ast->ctype);
                fprintf(out, ";\n");
            }
        } else if (ast->declvar && ast->declvar->varname && ast->declvar->ctype) {
            /* 兼容旧 AST 格式 */
            fprintf(out, "typedef ");
            c51_emit_type_prefix(out, ast->declvar->ctype);
            fprintf(out, " ");
            c51_emit_ptr_prefix(out, ast->declvar->ctype);
            fprintf(out, "%s", c51_map_name(ast->declvar->varname));
            c51_emit_array_suffix(out, ast->declvar->ctype);
            fprintf(out, ";\n");
        } break;
    case AST_ENUM_DEF:
        if (ast->ctype && ast->ctype->fields) {
            fprintf(out, "enum");
            if (ast->ctype->tag) fprintf(out, " %s", ast->ctype->tag);
            fprintf(out, " {\n");
            List *keys = dict_keys(ast->ctype->fields);
            for (int i = 0; i < list_len(keys); i++) {
                fprintf(out, "    %s", (char*)list_get(keys, i));
                if (i < list_len(keys) - 1) fprintf(out, ",");
                fprintf(out, "\n");
            }
            free(keys);
            fprintf(out, "};\n");
        } break;
    case AST_ASM:
        fprintf(out, "#pragma asm\n");
        fprintf(out, "%s\n", ast->asm_body ? ast->asm_body : "");
        fprintf(out, "#pragma endasm\n");
        break;
    case AST_STATIC_ASSERT:
        /* 编译期已求值，不输出 */
        break;
    default: break;
    }
}

void c51_emit_translation_unit(FILE *out, List *toplevels, int c51_model, bool map_names) {
    g_c51_mem_model = c51_model;
    g_map_names_enabled = map_names;
    /* 初始化去重表 */
    if (!g_emitted_sfr) g_emitted_sfr = make_dict(NULL);
    else dict_clear(g_emitted_sfr);
    if (!g_emitted_typedef) g_emitted_typedef = make_dict(NULL);
    else dict_clear(g_emitted_typedef);
    if (!g_emitted_funcdecl) g_emitted_funcdecl = make_dict(NULL);
    else dict_clear(g_emitted_funcdecl);
    fprintf(out, "/* Generated by ttcc (C11 to C51 translator) */\n");
    fprintf(out, "/* Memory model: %s */\n\n",
            c51_model == 0 ? "small" : c51_model == 1 ? "compact" : "large");
    /* 检测是否有 asm 语句，加上 #pragma SRC */
    bool has_asm = false;
    if (toplevels) {
        for (int i = 0; i < list_len(toplevels) && !has_asm; i++) {
            Ast *ast = (Ast*)list_get(toplevels, i);
            if (ast->type == AST_FUNC_DEF && ast->body) {
                /* 检查函数体中是否有 asm */
                lower_walk_ast(ast->body, &detect_asm_visitor);
                if (g_detected_asm) { has_asm = true; g_detected_asm = false; }
            }
        }
    }
    if (has_asm) {
        fprintf(out, "#pragma SRC\n\n");
    }
    if (!toplevels) return;
    for (int i = 0; i < list_len(toplevels); i++)
        c51_emit_toplevel(out, (Ast*)list_get(toplevels, i));

    /* 输出中文→ASCII 标识符映射（供 embed_toolchain 反向查错用） */
    if (g_name_map && list_len(g_name_map->list) > 0) {
        fprintf(out, "\n/* __TTCC_NAME_MAP__");
        for (int i = 0; i < list_len(g_name_map->list); i++) {
            DictEntry *e = (DictEntry*)list_get(g_name_map->list, i);
            fprintf(out, " %s=%s", e->key, (const char*)e->val);
        }
        fprintf(out, " */\n");
    }
}
