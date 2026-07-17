/* uart.c — 8051 UART 串口驱动实现 */

#include "uart.h"
#include "reg8051.h"

/* 波特率发生器重载值表（@12MHz，SMOD=1）*/
static unsigned int baud_reload_table[] = {
    0xFFFE, /* 1200   — 实际值取决于晶振 */
    0xFFFD, /* 2400   */
    0xFFFB, /* 4800   */
    0xFFF7, /* 9600   */
    0xFFEF, /* 19200  */
    0xFFDF, /* 38400  */
    0xFFBF, /* 57600  */
    0xFF7F, /* 115200 */
};

void uart_init(BaudRate baud)
{
    /* 设置定时器 2 为波特率发生器 */
    T2L_T2H = baud_reload_table[(int)baud];

    /* SCON = 0x50: 模式 1（8 位 UART），REN=1 使能接收 */
    SCON = 0x50;

    /* PCON SMOD=1（加倍波特率）*/
    PCON = 0x80;
}

void uart_send(unsigned char c)
{
    /* 等待发送缓冲区空 */
    while (!(SCON & 0x02)) {
        /* 等待 TI 位 */
    }
    /* 清除 TI 标志 */
    SCON = SCON & 0xFD;
    /* 写入数据 */
    SBUF = c;
}

unsigned char uart_recv(void)
{
    /* 等待接收完成 */
    while (!(SCON & 0x01)) {
        /* 等待 RI 位 */
    }
    /* 清除 RI 标志 */
    SCON = SCON & 0xFE;
    return SBUF;
}

void uart_send_string(const char *str)
{
    while (*str != '\0') {
        uart_send((unsigned char)(*str));
        str = str + 1;
    }
}

static const char hex_chars[] = "0123456789ABCDEF";

void uart_send_hex(unsigned int val)
{
    unsigned char i;
    unsigned char shift;
    uart_send_string("0x");
    for (i = 0; i < 4; i = i + 1) {
        shift = 12 - (i * 4);
        uart_send(hex_chars[(val >> shift) & 0x0F]);
    }
}
