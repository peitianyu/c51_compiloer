#ifndef C11_LOWERING_H
#define C11_LOWERING_H

#include "cc.h"

/*
 * c11_lowering.h - C11 → C89 AST 降级 Pass 头文件
 *
 * 在 AST 层面将 C11 特性变换为 C89 等效形式，
 * 使得 codegen_c51.c 能以统一方式遍历输出。
 *
 * 入口函数：
 *   List* lower_program(List *toplevels)
 *     对顶层声明列表依次执行全部降级 Pass，返回降级后的 AST 列表。
 *     注意：降级是原地变换（修改传入的 AST 节点），返回的列表与传入列表相同。
 */

/* ── 入口 ── */
List *lower_program(List *toplevels, int c51_model);

/* ── 各 Pass（可单独调用，也可通过 lower_program 统一执行）── */

/* Pass 1: 声明提升 — 将块内声明移到块开头 */
void lower_hoist_decls(Ast *ast);

/* Pass 2: for-init 拆分 — 将 for (int i = 0; ...) 拆为 { int i; for (i = 0; ...) } */
void lower_split_for_init(Ast *ast);

/* Pass 3: 复合字面量展开 — 将 (T){...} 展开为临时变量 */
void lower_expand_compound_literal(Ast *ast);

/* Pass 4: 指定初始化器展平 — 将 .field = val 按字段顺序重排 */
Ast *lower_flatten_designated_init(Ast *ast);

/* Pass 5: _Generic 展开 — 编译期确定类型并替换为匹配分支 */
void lower_expand_generic(Ast *ast);

/* Pass 6: _Static_assert 求值 — true 移除节点，false 终止编译 */
List *lower_expand_static_assert(List *toplevels);

/* Pass 7: 清理 C11 属性 — 移除 restrict、_Alignas、_Noreturn 等 */
void lower_strip_c11_attrs(Ast *ast);

/* Pass 8: 匿名 struct/union 命名 */
void lower_handle_anonymous(Ast *ast);

/* Pass 9: VLA 检查 */
void lower_check_vla(Ast *ast);

/* ── 辅助函数 ── */

/* 计算 C51 默认存储区 */
typedef enum {
    C51_MS_DATA = 0,
    C51_MS_IDATA,
    C51_MS_XDATA,
    C51_MS_PDATA,
    C51_MS_CODE,
    C51_MS_BDATA,
    C51_MS_BIT,
} C51MemSpace;

C51MemSpace c51_default_memspace(Ctype *ctype, bool is_local, int mem_model);

/* 递归 AST 遍历 */
void lower_walk_ast(Ast *ast, void (*visitor)(Ast *));

#endif /* C11_LOWERING_H */
