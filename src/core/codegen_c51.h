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

/* ── 入口 ── */
void c51_emit_translation_unit(FILE *out, List *toplevels, int c51_model);

/* ── 内存模型配置 ── */
void c51_set_mem_model(int model);  /* 0=small, 1=compact, 2=large */

#endif /* CODEGEN_C51_H */
