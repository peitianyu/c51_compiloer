#include "c11_lowering.h"
#include <stdio.h>
#include <string.h>

static void lower_walk_ast_internal(Ast *ast, void (*visitor)(Ast *), int depth);

void lower_walk_ast(Ast *ast, void (*visitor)(Ast *)) {
    lower_walk_ast_internal(ast, visitor, 0);
}

static void lower_walk_ast_internal(Ast *ast, void (*visitor)(Ast *), int depth) {
    if (!ast) return;
    switch (ast->type) {
    case AST_LITERAL: case AST_STRING: case AST_LVAR: case AST_GVAR:
    case AST_FUNC_DECL: case AST_FUNC_DEF: case AST_ASM: case AST_STATIC_ASSERT:
    case AST_ENUM_DEF: case AST_STRUCT_DEF:
    case AST_TYPE_DEF: break;
    case AST_FUNCALL:
        lower_walk_ast_internal(ast->fnexpr, visitor, depth);
        if (ast->args) for (int i = 0; i < list_len(ast->args); i++)
            lower_walk_ast_internal(list_get(ast->args, i), visitor, depth);
        break;
    case AST_DECL:
        lower_walk_ast_internal(ast->declvar, visitor, depth);
        lower_walk_ast_internal(ast->declinit, visitor, depth);
        break;
    case AST_ARRAY_INIT:
        if (ast->arrayinit) for (int i = 0; i < list_len(ast->arrayinit); i++)
            lower_walk_ast_internal(list_get(ast->arrayinit, i), visitor, depth);
        break;
    case AST_STRUCT_INIT:
        if (ast->structinit) for (int i = 0; i < list_len(ast->structinit); i++)
            lower_walk_ast_internal(list_get(ast->structinit, i), visitor, depth);
        break;
    case AST_ADDR: case AST_DEREF: case AST_CAST:
    case AST_POST_INC: case AST_POST_DEC:
    case '!': case '~': case PUNCT_INC: case PUNCT_DEC:
        lower_walk_ast_internal(ast->operand, visitor, depth);
        break;
    case AST_TERNARY:
        lower_walk_ast_internal(ast->cond, visitor, depth);
        lower_walk_ast_internal(ast->then, visitor, depth);
        lower_walk_ast_internal(ast->els, visitor, depth);
        break;
    case AST_IF:
        lower_walk_ast_internal(ast->cond, visitor, depth);
        lower_walk_ast_internal(ast->then, visitor, depth);
        lower_walk_ast_internal(ast->els, visitor, depth);
        break;
    case AST_FOR:
        lower_walk_ast_internal(ast->forinit, visitor, depth);
        lower_walk_ast_internal(ast->forcond, visitor, depth);
        lower_walk_ast_internal(ast->forstep, visitor, depth);
        lower_walk_ast_internal(ast->forbody, visitor, depth);
        break;
    case AST_WHILE: case AST_DO_WHILE:
        lower_walk_ast_internal(ast->while_cond, visitor, depth);
        lower_walk_ast_internal(ast->while_body, visitor, depth);
        break;
    case AST_SWITCH:
        lower_walk_ast_internal(ast->ctrl, visitor, depth);
        lower_walk_ast_internal(ast->switch_body, visitor, depth);
        break;
    case AST_LABEL: case AST_GOTO: case AST_BREAK:
    case AST_CONTINUE: break;
    case AST_RETURN:
        lower_walk_ast_internal(ast->retval, visitor, depth);
        break;
    case AST_COMPOUND_STMT:
        if (ast->stmts) for (int i = 0; i < list_len(ast->stmts); i++)
            lower_walk_ast_internal(list_get(ast->stmts, i), visitor, depth);
        break;
    case AST_STRUCT_REF: case AST_BIT_REF:
        lower_walk_ast_internal(ast->struc, visitor, depth);
        break;
    default:
        if (ast->type >= '!' && ast->type <= '~') break;
        if (ast->type >= AST_POST_INC) break;
        lower_walk_ast_internal(ast->left, visitor, depth);
        lower_walk_ast_internal(ast->right, visitor, depth);
        break;
    }
    visitor(ast);
}

C51MemSpace c51_default_memspace(Ctype *ctype, bool is_local, int mem_model) {
    CtypeAttr a = get_attr(ctype->attr);
    if (ctype->type == CTYPE_CHAR && a.ctype_c51_bit) return C51_MS_BIT;
    if (a.ctype_c51_data)  return C51_MS_DATA;
    if (a.ctype_c51_xdata) return C51_MS_XDATA;
    if (a.ctype_c51_idata) return C51_MS_IDATA;
    if (a.ctype_c51_code)  return C51_MS_CODE;
    if (a.ctype_c51_bdata) return C51_MS_BDATA;
    if (a.ctype_c51_pdata) return C51_MS_PDATA;
    if (mem_model == 2) return C51_MS_XDATA;
    else if (mem_model == 1) return is_local ? C51_MS_DATA : C51_MS_PDATA;
    else { if (is_local) return C51_MS_DATA; return a.ctype_const ? C51_MS_CODE : C51_MS_XDATA; }
}

void lower_hoist_decls(Ast *ast) {
    if (!ast || ast->type != AST_COMPOUND_STMT || !ast->stmts) return;
    for (int i = 0; i < list_len(ast->stmts); i++) {
        Ast *child = list_get(ast->stmts, i);
        if (child && child->type == AST_COMPOUND_STMT) lower_hoist_decls(child);
    }
    List *decls = make_list();
    List *non_decls = make_list();
    for (int i = 0; i < list_len(ast->stmts); i++) {
        Ast *s = list_get(ast->stmts, i);
        if (s->type == AST_DECL) list_push(decls, s);
        else list_push(non_decls, s);
    }
    if (list_len(decls) == 0) { free(decls); free(non_decls); return; }
    ast->stmts = make_list();
    for (int i = 0; i < list_len(decls); i++) list_push(ast->stmts, list_get(decls, i));
    for (int i = 0; i < list_len(non_decls); i++) list_push(ast->stmts, list_get(non_decls, i));
    free(decls); free(non_decls);
}

static void lower_split_for_init_internal(Ast *ast);

void lower_split_for_init(Ast *ast) {
    lower_split_for_init_internal(ast);
}

static void lower_split_for_init_internal(Ast *ast) {
    if (!ast) return;
    if (ast->type == AST_COMPOUND_STMT && ast->stmts) {
        List *new_stmts = make_list();
        for (int i = 0; i < list_len(ast->stmts); i++) {
            Ast *s = list_get(ast->stmts, i);
            lower_split_for_init_internal(s);
            list_push(new_stmts, s);
        }
        ast->stmts = new_stmts;
        return;
    }
    if (ast->type != AST_FOR) {
        if (ast->type == AST_IF) {
            lower_split_for_init_internal(ast->then);
            lower_split_for_init_internal(ast->els);
        } else if (ast->type == AST_WHILE || ast->type == AST_DO_WHILE) {
            lower_split_for_init_internal(ast->while_body);
        } else if (ast->type == AST_SWITCH) {
            lower_split_for_init_internal(ast->switch_body);
        }
        return;
    }
    lower_split_for_init_internal(ast->forbody);
    if (ast->forinit && ast->forinit->type == AST_COMPOUND_STMT)
        lower_split_for_init_internal(ast->forinit);
    if (!ast->forinit || ast->forinit->type != AST_DECL) return;

    Ast *decl = ast->forinit;
    Ast *block = malloc(sizeof(Ast));
    memset(block, 0, sizeof(Ast));
    block->type = AST_COMPOUND_STMT;
    /* 创建赋值表达式：var = init_expr */
    Ast *init_expr = decl->declinit;
    Ast *var_ref = decl->declvar;
    Ast *new_for = malloc(sizeof(Ast));
    memcpy(new_for, ast, sizeof(Ast));
    if (init_expr && var_ref) {
        /* 构建 var = init_expr 赋值节点 */
        Ast *assign = malloc(sizeof(Ast));
        memset(assign, 0, sizeof(Ast));
        assign->type = '=';
        assign->left = var_ref;
        assign->right = init_expr;
        assign->ctype = var_ref->ctype;
        new_for->forinit = assign;
    } else {
        new_for->forinit = NULL;
    }
    block->stmts = make_list();
    list_push(block->stmts, decl);
    list_push(block->stmts, new_for);
    memcpy(ast, block, sizeof(Ast));
    free(block);
}

void lower_expand_compound_literal(Ast *ast) {
    if (!ast) return;
    switch (ast->type) {
    case AST_DECL:
        lower_expand_compound_literal(ast->declvar);
        lower_expand_compound_literal(ast->declinit);
        return;
    case AST_FUNCALL:
        lower_expand_compound_literal(ast->fnexpr);
        if (ast->args) for (int i = 0; i < list_len(ast->args); i++) lower_expand_compound_literal(list_get(ast->args, i));
        return;
    case AST_RETURN: lower_expand_compound_literal(ast->retval); return;
    case AST_COMPOUND_STMT:
        if (ast->stmts) for (int i = 0; i < list_len(ast->stmts); i++) lower_expand_compound_literal(list_get(ast->stmts, i));
        return;
    case AST_FOR:
        lower_expand_compound_literal(ast->forinit); lower_expand_compound_literal(ast->forcond);
        lower_expand_compound_literal(ast->forstep); lower_expand_compound_literal(ast->forbody);
        return;
    case AST_IF: lower_expand_compound_literal(ast->cond); lower_expand_compound_literal(ast->then); lower_expand_compound_literal(ast->els); return;
    case AST_WHILE: case AST_DO_WHILE: lower_expand_compound_literal(ast->while_cond); lower_expand_compound_literal(ast->while_body); return;
    case AST_ADDR: case AST_DEREF: case AST_CAST: lower_expand_compound_literal(ast->operand); return;
    case AST_STRUCT_INIT:
        if (ast->structinit) for (int i = 0; i < list_len(ast->structinit); i++) lower_expand_compound_literal(list_get(ast->structinit, i));
        return;
    case AST_ARRAY_INIT:
        if (ast->arrayinit) for (int i = 0; i < list_len(ast->arrayinit); i++) lower_expand_compound_literal(list_get(ast->arrayinit, i));
        return;
    default:
        if (ast->type == AST_LITERAL || ast->type == AST_STRING || ast->type == AST_LVAR || ast->type == AST_GVAR || ast->type == AST_FUNC_DEF || ast->type == AST_FUNC_DECL || ast->type == AST_ASM || ast->type == AST_STATIC_ASSERT) return;
        if (ast->left) lower_expand_compound_literal(ast->left);
        if (ast->right) lower_expand_compound_literal(ast->right);
        return;
    }
}

Ast *lower_flatten_designated_init(Ast *ast) { (void)ast; return ast; }
void lower_expand_generic(Ast *ast) { (void)ast; }

/* 编译期常量整数表达式求值（在 lower pass 中自包含，不依赖 parser.c 的 eval_intexpr） */
static long long lower_eval_const_expr(Ast *ast) {
    if (!ast) return 0;
    switch (ast->type) {
    case AST_LITERAL:
        if (ast->ctype && (ast->ctype->type >= CTYPE_BOOL && ast->ctype->type <= CTYPE_LONG))
            return ast->ival;
        return 0;
    case AST_STRING: return 0;
    case AST_CAST:
        if (ast->cast_expr) return lower_eval_const_expr(ast->cast_expr);
        if (ast->operand) return lower_eval_const_expr(ast->operand);
        return 0;
    case '+': return lower_eval_const_expr(ast->left) + lower_eval_const_expr(ast->right);
    case '-': return lower_eval_const_expr(ast->left) - lower_eval_const_expr(ast->right);
    case '*': return lower_eval_const_expr(ast->left) * lower_eval_const_expr(ast->right);
    case '/': return lower_eval_const_expr(ast->left) / lower_eval_const_expr(ast->right);
    case '%': return lower_eval_const_expr(ast->left) % lower_eval_const_expr(ast->right);
    case '&': return lower_eval_const_expr(ast->left) & lower_eval_const_expr(ast->right);
    case '|': return lower_eval_const_expr(ast->left) | lower_eval_const_expr(ast->right);
    case '^': return lower_eval_const_expr(ast->left) ^ lower_eval_const_expr(ast->right);
    case '>': return lower_eval_const_expr(ast->left) > lower_eval_const_expr(ast->right);
    case '<': return lower_eval_const_expr(ast->left) < lower_eval_const_expr(ast->right);
    case PUNCT_EQ:  return lower_eval_const_expr(ast->left) == lower_eval_const_expr(ast->right);
    case PUNCT_NE:  return lower_eval_const_expr(ast->left) != lower_eval_const_expr(ast->right);
    case PUNCT_LE:  return lower_eval_const_expr(ast->left) <= lower_eval_const_expr(ast->right);
    case PUNCT_GE:  return lower_eval_const_expr(ast->left) >= lower_eval_const_expr(ast->right);
    case PUNCT_LSHIFT: return lower_eval_const_expr(ast->left) << lower_eval_const_expr(ast->right);
    case PUNCT_RSHIFT: return lower_eval_const_expr(ast->right);
    case PUNCT_LOGAND: return lower_eval_const_expr(ast->left) && lower_eval_const_expr(ast->right);
    case PUNCT_LOGOR:  return lower_eval_const_expr(ast->left) || lower_eval_const_expr(ast->right);
    case '!': return !lower_eval_const_expr(ast->operand);
    case '~': return ~lower_eval_const_expr(ast->operand);
    case PUNCT_INC: return lower_eval_const_expr(ast->operand) + 1;
    case PUNCT_DEC: return lower_eval_const_expr(ast->operand) - 1;
    case AST_GVAR:
        if (ast->ginit) return lower_eval_const_expr(ast->ginit);
        return 0;
    case AST_LVAR: return 0;
    case AST_TERNARY:
        return lower_eval_const_expr(ast->cond) ? lower_eval_const_expr(ast->then) : lower_eval_const_expr(ast->els);
    default:
        /* sizeof 等操作：尝试简单处理 */
        return 0;
    }
}

/* 常量条件折叠：遍历 stmts，对 if(常量表达式) 编译期求值并折叠 */
static void lower_fold_if_const_in_stmts(List *stmts) {
    if (!stmts) return;
    for (int i = 0; i < list_len(stmts); i++) {
        Ast *s = (Ast*)list_get(stmts, i);
        if (!s) continue;
        if (s->type == AST_IF && s->cond && is_inttype(s->cond->ctype)) {
            long long val = lower_eval_const_expr(s->cond);
            if (val != 0 && s->then) {
                list_set(stmts, i, s->then);
            } else if (val == 0 && s->els) {
                list_set(stmts, i, s->els);
            }
        }
        /* 递归：只在 AST_COMPOUND_STMT 节点访问 stmts 字段（union 安全） */
        Ast *next = (Ast*)list_get(stmts, i);
        if (!next) continue;
        if (next->type == AST_COMPOUND_STMT && next->stmts) {
            lower_fold_if_const_in_stmts(next->stmts);
        } else if (next->type == AST_FOR) {
            if (next->forbody && next->forbody->type == AST_COMPOUND_STMT && next->forbody->stmts)
                lower_fold_if_const_in_stmts(next->forbody->stmts);
        } else if (next->type == AST_WHILE || next->type == AST_DO_WHILE) {
            if (next->while_body && next->while_body->type == AST_COMPOUND_STMT && next->while_body->stmts)
                lower_fold_if_const_in_stmts(next->while_body->stmts);
        } else if (next->type == AST_IF) {
            if (next->then && next->then->type == AST_COMPOUND_STMT && next->then->stmts)
                lower_fold_if_const_in_stmts(next->then->stmts);
            if (next->els && next->els->type == AST_COMPOUND_STMT && next->els->stmts)
                lower_fold_if_const_in_stmts(next->els->stmts);
        }
    }
}

/* 在 stmts 列表中查找并处理 _Static_assert：移除通过检查的节点，失败则终止 */
static void lower_process_static_assert_in_stmts(List *stmts, const char *context) {
    if (!stmts) return;
    /* 第一遍：检查所有 _Static_assert，失败则终止 */
    for (int i = 0; i < list_len(stmts); i++) {
        Ast *s = (Ast*)list_get(stmts, i);
        if (!s) continue;
        if (s->type == AST_STATIC_ASSERT) {
            long long val = lower_eval_const_expr(s->cond);
            if (val == 0) {
                const char *msg = s->els ? (const char*)s->els : "static_assert failed";
                error("_Static_assert failed%s%s: %s",
                      context ? " in " : "", context ? context : "", msg);
            }
        }
    }
    /* 第二遍：重建列表，移除所有 _Static_assert 节点 */
    List *new_list = make_list();
    for (int i = 0; i < list_len(stmts); i++) {
        Ast *s = (Ast*)list_get(stmts, i);
        if (!s) continue;
        if (s->type == AST_STATIC_ASSERT) continue; /* 跳过已通过的 _Static_assert */
        if (s->type == AST_COMPOUND_STMT && s->stmts)
            lower_process_static_assert_in_stmts(s->stmts, context);
        list_push(new_list, s);
    }
    /* 替换原列表 */
    stmts->head = new_list->head;
    stmts->tail = new_list->tail;
    stmts->len = new_list->len;
    new_list->head = NULL;
    new_list->tail = NULL;
    new_list->len = 0;
    free(new_list);
}

List *lower_expand_static_assert(List *toplevels) {
    if (!toplevels) return toplevels;
    /* 处理顶层 _Static_assert */
    lower_process_static_assert_in_stmts(toplevels, NULL);
    /* 处理函数体内的 */
    for (int i = 0; i < list_len(toplevels); i++) {
        Ast *node = (Ast*)list_get(toplevels, i);
        if (node && node->type == AST_FUNC_DEF && node->body && node->body->stmts)
            lower_process_static_assert_in_stmts(node->body->stmts, node->fname);
    }
    return toplevels;
}

static void strip_c11_attrs_visitor(Ast *ast) {
    if (!ast || !ast->ctype) return;
    CtypeAttr a = get_attr(ast->ctype->attr);
    int changed = 0;
    if (a.ctype_restrict) { a.ctype_restrict = 0; changed = 1; }
    if (a.ctype_noreturn) { a.ctype_noreturn = 0; changed = 1; }
    if (changed) {
        int mask = ~0;
        mask &= ~(1 << 3); mask &= ~(1 << 9);
        ast->ctype->attr = ast->ctype->attr & mask;
    }
}

void lower_strip_c11_attrs(Ast *ast) { lower_walk_ast(ast, strip_c11_attrs_visitor); }

static int anon_seq = 0;
static void handle_anonymous_visitor(Ast *ast) {
    if (!ast) return;
    Ctype *ct = NULL;
    char *typedef_name = NULL;
    if (ast->type == AST_STRUCT_DEF) {
        ct = ast->ctype;
    } else if (ast->type == AST_DECL) {
        if (ast->declvar && ast->declvar->ctype) ct = ast->declvar->ctype;
    } else if (ast->type == AST_TYPE_DEF) {
        if (ast->ctype) ct = ast->ctype;
        typedef_name = ast->typename;
    }
    if (ct && ct->type == CTYPE_STRUCT && !ct->tag) {
        if (typedef_name) {
            /* typedef struct { ... } Name; → 把 Name 赋给 struct 作为 tag */
            ct->tag = strdup(typedef_name);
        } else {
            char anon_name[64];
            snprintf(anon_name, sizeof(anon_name), "__anon_%d", anon_seq++);
            ct->tag = strdup(anon_name);
        }
    }
    if (ct && ct->type == CTYPE_STRUCT && ct->fields) {
            List *keys = dict_keys(ct->fields);
            List *vals = dict_values(ct->fields);
            for (int i = 0; i < list_len(keys); i++) {
                Ctype *ft = (Ctype*)list_get(vals, i);
                if (ft && ft->type == CTYPE_STRUCT && ft->tag == NULL) {
                    char anon_name[64];
                    snprintf(anon_name, sizeof(anon_name), "__anon_%d", anon_seq++);
                    ft->tag = strdup(anon_name);
                    char *old_key = (char*)list_get(keys, i);
                    if (old_key && old_key[0] == '\0') {
                        for (ListNode *n = ct->fields->list->head; n; n = n->next) {
                            DictEntry *e = (DictEntry *)n->elem;
                            if (e->key == old_key) { e->key = strdup(anon_name); break; }
                        }
                    }
                }
            }
            free(keys); free(vals);
        }
}
void lower_handle_anonymous(Ast *ast) { lower_walk_ast(ast, handle_anonymous_visitor); }

static void check_vla_visitor(Ast *ast) {
    if (!ast || !ast->ctype) return;
    if (ast->ctype->type == CTYPE_ARRAY && ast->ctype->len < 0)
        error("Variable-length arrays (VLA) are not supported on MCS-51 target");
}
void lower_check_vla(Ast *ast) { lower_walk_ast(ast, check_vla_visitor); }

List *lower_program(List *toplevels, int c51_model) {
    (void)c51_model;
    for (int i = 0; i < list_len(toplevels); i++) {
        Ast *node = (Ast*)list_get(toplevels, i);
        /* VLA 检查：遍历整个 AST 树（含函数体内部）*/
        lower_check_vla(node);
        if (node->type == AST_FUNC_DEF && node->body) {
            /* 深入函数体检查 VLA */
            lower_check_vla(node->body);
            if (node->body->stmts) for (int j = 0; j < list_len(node->body->stmts); j++) {
                Ast *s = list_get(node->body->stmts, j);
                lower_check_vla(s);
            }
        }
        lower_handle_anonymous(node);
        lower_strip_c11_attrs(node);
        if (node->type == AST_FUNC_DEF) {
            if (node->body) lower_hoist_decls(node->body);
            if (node->body) lower_split_for_init(node->body);
            lower_expand_compound_literal(node);
            if (node->body) {
                lower_fold_if_const_in_stmts(node->body->stmts);
                /* 再折叠一遍处理 do{}while(0) 内部 */
                lower_fold_if_const_in_stmts(node->body->stmts);
            }
        }
        if (node->type == AST_DECL) lower_expand_compound_literal(node);
    }
    lower_expand_static_assert(toplevels);
    return toplevels;
}
