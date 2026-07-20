#ifndef EMBED_TOOLCHAIN_H
#define EMBED_TOOLCHAIN_H

/*
 * embed_toolchain.h — Keil C51 工具链调用接口
 *
 * 在 Windows 上自动检测已安装的 Keil C51（C51.exe / BL51.EXE），
 * 依次调用编译 → 链接流水线。
 *
 * 支持多文件编译：每个 .c 文件独立编译为 .OBJ，
 * 然后 BL51 链接所有 .OBJ 文件直接输出 .HEX。
 */

#include <stdio.h>
#include "list.h"

/* ── 工具链调用 ── */

/*
 * 调用 C51 → BL51 流水线（多个 C51 源码分别编译，再链接为单个 HEX）。
 * source_files: C51 源码路径列表（每个文件单独编译为 .OBJ）
 * source_labels: 对应的原始源文件显示名列表（如 "stc15w4k48s4/main.c"），用于警告输出
 * hex_out:      输出 HEX 文件路径（NULL 则生成到第一个源文件同目录）
 * temp_dir:     临时工作目录
 * c51_model:    内存模型（0=small, 1=compact, 2=large）
 * 返回 0 成功，非 0 失败。
 */
int embed_run_toolchain(List *source_files, List *source_labels,
                        const char *hex_out,
                        const char *temp_dir, int c51_model,
                        List *include_dirs);

#endif /* EMBED_TOOLCHAIN_H */
