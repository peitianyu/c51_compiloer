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

/* ── 中文→ASCII 名映射 ── */
/* 返回 Dict* (key=ASCII别名, val=中文原名) */
Dict *c51_get_name_map(void);

/* ── 内存模型配置 ── */
void c51_set_mem_model(int model);  /* 0=small, 1=compact, 2=large */

#endif /* CODEGEN_C51_H */
