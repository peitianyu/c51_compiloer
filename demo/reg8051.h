/* reg8051.h — 8051/MCS51 标准特殊功能寄存器定义
 *
 * 用于 ttcc C11→C51 编译器，定义所有标准 8051 SFR 寄存器。
 * 使用 C51 兼容的 sfr/sfr16/sbit 语法。
 */

#ifndef REG8051_H
#define REG8051_H

/* ── SFR 寄存器 (8-bit 特殊功能寄存器) ── */

/* 中断控制 */
sfr IE   = 0xA8;   /* 中断使能 */
sfr IP   = 0xB8;   /* 中断优先级 */

/* 定时器/计数器 */
sfr TMOD = 0x89;   /* 定时器模式 */
sfr TCON = 0x88;   /* 定时器控制 */
sfr TL0  = 0x8A;   /* 定时器 0 低字节 */
sfr TH0  = 0x8C;   /* 定时器 0 高字节 */
sfr TL1  = 0x8B;   /* 定时器 1 低字节 */
sfr TH1  = 0x8D;   /* 定时器 1 高字节 */

/* 串口 */
sfr SCON = 0x98;   /* 串口控制 */
sfr SBUF = 0x99;   /* 串口数据缓冲 */

/* 电源 */
sfr PCON = 0x87;   /* 电源控制 */

/* I/O 端口 */
sfr P0   = 0x80;   /* 端口 0 */
sfr P1   = 0x90;   /* 端口 1 */
sfr P2   = 0xA0;   /* 端口 2 */
sfr P3   = 0xB0;   /* 端口 3 */

/* 辅助功能 */
sfr AUXR = 0x8E;   /* 辅助寄存器 */

/* ── sfr16 寄存器 (16-bit) ── */

/* 定时器 2（用于波特率生成） */
sfr16 T2L_T2H = 0xCC;  /* 定时器 2 低/高字节（连续） */
sfr16 RCAP2L_RCAP2H = 0xCA;  /* 定时器 2 捕获/重载 */

/* ── sbit 位定义 ── */

/* TCON 位 */
sbit IT0  = 0x88;   /* TCON.0 — 外部中断 0 触发方式 */
sbit IE0  = 0x89;   /* TCON.1 — 外部中断 0 标志 */
sbit IT1  = 0x8A;   /* TCON.2 — 外部中断 1 触发方式 */
sbit IE1  = 0x8B;   /* TCON.3 — 外部中断 1 标志 */
sbit TR0  = 0x8C;   /* TCON.4 — 定时器 0 运行控制 */
sbit TF0  = 0x8D;   /* TCON.5 — 定时器 0 溢出标志 */
sbit TR1  = 0x8E;   /* TCON.6 — 定时器 1 运行控制 */
sbit TF1  = 0x8F;   /* TCON.7 — 定时器 1 溢出标志 */

/* SCON 位 */
sbit RI   = 0x98;   /* SCON.0 — 接收中断标志 */
sbit TI   = 0x99;   /* SCON.1 — 发送中断标志 */
sbit RB8  = 0x9A;   /* SCON.2 — 接收第 9 位 */
sbit TB8  = 0x9B;   /* SCON.3 — 发送第 9 位 */
sbit REN  = 0x9C;   /* SCON.4 — 接收使能 */
sbit SM2  = 0x9D;   /* SCON.5 — 多机通信控制 */
sbit SM1  = 0x9E;   /* SCON.6 — 串口模式位 1 */
sbit SM0  = 0x9F;   /* SCON.7 — 串口模式位 0 */

/* IE 位 */
sbit EX0  = 0xA8;   /* IE.0 — 外部中断 0 使能 */
sbit ET0  = 0xA9;   /* IE.1 — 定时器 0 中断使能 */
sbit EX1  = 0xAA;   /* IE.2 — 外部中断 1 使能 */
sbit ET1  = 0xAB;   /* IE.3 — 定时器 1 中断使能 */
sbit ES   = 0xAC;   /* IE.4 — 串口中断使能 */
sbit ET2  = 0xAD;   /* IE.5 — 定时器 2 中断使能 */
sbit EA   = 0xAF;   /* IE.7 — 全局中断使能 */

/*
 * 常用宏定义
 */

/* TMOD 定时器模式 */
#define T0_MODE0  0x00   /* 13 位定时器 */
#define T0_MODE1  0x01   /* 16 位定时器 */
#define T0_MODE2  0x02   /* 8 位自动重载 */
#define T0_MODE3  0x03   /* 两个 8 位定时器 */
#define T0_CT     0x04   /* 计数器模式 */
#define T0_GATE   0x08   /* 门控模式 */

#define T1_MODE0  0x00   /* 13 位定时器 */
#define T1_MODE1  0x10   /* 16 位定时器 */
#define T1_MODE2  0x20   /* 8 位自动重载 */
#define T1_MODE3  0x30   /* 禁用 */
#define T1_CT     0x40   /* 计数器模式 */
#define T1_GATE   0x80   /* 门控模式 */

/* 中断号 */
#define INT0_VEC  0   /* 外部中断 0 */
#define TMR0_VEC  1   /* 定时器 0 */
#define INT1_VEC  2   /* 外部中断 1 */
#define TMR1_VEC  3   /* 定时器 1 */
#define UART_VEC  4   /* 串口 */
#define TMR2_VEC  5   /* 定时器 2 */

#endif /* REG8051_H */
