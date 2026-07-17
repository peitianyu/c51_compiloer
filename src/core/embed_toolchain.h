#ifndef EMBED_TOOLCHAIN_H
#define EMBED_TOOLCHAIN_H

/*
 * embed_toolchain.h — Keil C51 工具链调用接口
 *
 * 在 Windows 上自动检测已安装的 Keil C51（C51.exe / BL51.EXE / OH51.EXE），
 * 依次调用编译 → 链接 → HEX 转换流水线。
 */

#include <stdio.h>

/* ── 工具链调用 ── */

/*
 * 调用 C51 → BL51 → OH51 流水线。
 * source_file: C51 源码路径
 * source_label: 原始源文件显示名（如 "demo/main.c"），用于警告输出
 * hex_out:     输出 HEX 文件路径（NULL 则生成到源文件同目录）
 * temp_dir:    临时工作目录
 * c51_model:   内存模型（0=small, 1=compact, 2=large）
 * 返回 0 成功，非 0 失败。
 */
int embed_run_toolchain(const char *source_file, const char *source_label,
                        const char *hex_out,
                        const char *temp_dir, int c51_model);

#endif /* EMBED_TOOLCHAIN_H */
