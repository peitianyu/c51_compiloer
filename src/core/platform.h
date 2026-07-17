#ifndef PLATFORM_H
#define PLATFORM_H

/*
 * platform.h - 编译目标平台抽象层
 *
 * 所有平台相关的类型属性标签、AST 扩展字段说明、以及
 * 平台 sexpr 扩展关键字均集中于此，便于后续扩展到新平台。
 *
 * 当前支持平台：
 *   PLAT_HOST   - 宿主机 (x86/x86_64)，默认模式
 *   PLAT_MCS51  - Intel MCS-51 / Keil C51
 *
 * 使用方式：
 *   - parser.c / sexpr.c 通过 g_target_platform 判断当前目标
 *   - CtypeAttr 中的 ctype_c51_* 位字段由本平台定义保护
 *   - 平台相关的 sexpr 关键字（interrupt/using）在此处统一声明
 */

/* ================================================================
 * 目标平台枚举
 * ================================================================ */
typedef enum {
    PLAT_HOST  = 0,   /* 宿主机（x86/x86_64），默认 */
    PLAT_MCS51 = 1,   /* Intel MCS-51 / Keil C51 */
} TargetPlatform;

/* 全局当前目标平台（由 parser.c 维护，main.c 通过 parser_set_target_* 写入） */
extern TargetPlatform g_target_platform;

/* ================================================================
 * MCS-51 / C51 平台专属 CtypeAttr 位字段说明
 *
 *   这些字段定义于 cc.h CtypeAttr 结构体中；在此处集中注释，
 *   说明每个字段的含义和适用平台，便于维护。
 *
 *   ctype_c51_sfr    : sfr 关键字（8 位特殊功能寄存器）
 *   ctype_c51_sfr16  : sfr16 关键字（16 位特殊功能寄存器）
 *   ctype_c51_sbit   : sbit 关键字（位寻址变量）
 *   ctype_c51_bit    : bit 关键字（位变量）
 *   ctype_c51_data   : data 内存区限定符（内部低 128B）
 *   ctype_c51_idata  : idata 内存区限定符（内部全 256B，间接访问）
 *   ctype_c51_xdata  : xdata 内存区限定符（外部 64KB）
 *   ctype_c51_pdata  : pdata 内存区限定符（外部分页 256B）
 *   ctype_c51_code   : code 内存区限定符（程序存储器）
 *   ctype_c51_bdata  : bdata 内存区限定符（位寻址区）
 *   ctype_c51_near   : near 内存区限定符（等价 data）
 *   ctype_c51_far    : far 内存区限定符（等价 xdata）
 * ================================================================ */

/* MCS-51 函数修饰（存储在 Ast.platform_info.mcs51 中） */
typedef struct {
    int interrupt_no; /* interrupt N（-1 = 未指定）*/
    int using_no;     /* using N（-1 = 未指定）*/
} PlatInfoMCS51;

/* 平台相关函数信息联合体（存储在 Ast 中） */
typedef union {
    PlatInfoMCS51 mcs51; /* PLAT_MCS51 */
    /* 为未来平台预留槽位 */
} PlatInfo;

/* ================================================================
 * MCS-51 C51 声明类型枚举（parser.c 内部使用，暴露给 platform.h 便于引用）
 * ================================================================ */
typedef enum {
    C51_DECL_NONE  = 0,
    C51_DECL_SFR   = 1,
    C51_DECL_SFR16 = 2,
    C51_DECL_SBIT  = 3,
    C51_DECL_BIT   = 4,
} C51DeclKind;

/* ================================================================
 * MCS-51 sexpr 扩展关键字（由 sexpr.c 输出，由 ssa_sexpr.c 解析）
 *
 *   (interrupt N)   - 中断号（函数修饰）
 *   (using N)       - 寄存器组号（函数修饰）
 *
 * 未来平台可在此处添加新的关键字常量，实现统一管理。
 * ================================================================ */
#define PLAT_SEXPR_INTERRUPT  "interrupt"
#define PLAT_SEXPR_USING      "using"

/* ================================================================
 * 平台 API（由 parser.c 实现）
 * ================================================================ */

/* 将解析器切换到 MCS-51 模式（调整整型宽度、指针大小等） */
extern void parser_set_target_mcs51(void);

/* 查询当前是否处于 MCS-51 目标模式 */
static inline int platform_is_mcs51(void) {
    return g_target_platform == PLAT_MCS51;
}

#endif /* PLATFORM_H */
