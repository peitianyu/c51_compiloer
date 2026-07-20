#ifndef CODEGEN_C51_H
#define CODEGEN_C51_H

#include <stdio.h>
#include "cc.h"
#include "c11_lowering.h"

/*
 * codegen_c51.h — Keil C51 代码生成器头文件
 *
 * 将降级后的 AST 输出为 Keil C51 方言源码。
 *
 * 工具链调用（C51/BL51/OH51）由 embed_toolchain.h 提供，
 * 相关 EXE 二进制数据已编译在 ttcc 内部。
 */

/* ── 入口 ──
 * map_names: 若为 true，将中文标识符转为 ASCII 别名（供 Keil C51 工具链使用）；
 *            若为 false，保留原始名称（--no-build 源码输出）。
 */
void c51_emit_translation_unit(FILE *out, List *toplevels, int c51_model, bool map_names);

/* ── 仅输出来自指定源文件的顶层声明（用于 .c51 / .h51 分流输出）──
 * source_filter: 只输出来自该文件的声明，NULL 则输出全部。
 * out_includes:  非 NULL 时，将遇到的 .h 引用路径写入此 Dict（key=路径, val=略）。
 */
void c51_emit_filtered(FILE *out, List *toplevels,
                       const char *source_filter,
                       Dict *out_includes,
                       int c51_model, bool map_names);

/* ── 从 toplevels 收集所有被引用的 .h 文件路径（去重）──
 * 返回 Dict* (key=文件路径, val=(void*)1) */
Dict *c51_collect_headers(List *toplevels);

/* ── 中文→ASCII 名映射 ── */
/* 返回 Dict* (key=ASCII别名, val=中文原名) */
Dict *c51_get_name_map(void);

/* ── 内存模型配置 ── */
void c51_set_mem_model(int model);  /* 0=small, 1=compact, 2=large */

#endif /* CODEGEN_C51_H */
