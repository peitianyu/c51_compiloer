/* timer.c — 8051 定时器驱动实现 */

#include "timer.h"
#include "reg8051.h"

void timer0_init(unsigned int reload)
{
    /* 设置定时器 0 为 16 位定时模式 (MODE1) */
    TMOD = (TMOD & 0xF0) | T0_MODE1;
    /* 设置重载值 */
    TL0 = (unsigned char)(reload & 0xFF);
    TH0 = (unsigned char)((reload >> 8) & 0xFF);
}

void timer0_start(void)
{
    TR0 = 1;
}

void timer0_stop(void)
{
    TR0 = 0;
}

unsigned int timer0_get(void)
{
    unsigned int val;
    val = (unsigned int)TH0;
    val = (val << 8) | (unsigned int)TL0;
    return val;
}

void delay_us(unsigned int us)
{
    unsigned int i;
    for (i = 0; i < us; i = i + 1) {
        unsigned char j;
        for (j = 0; j < 3; j = j + 1) {
            /* 空转 */
        }
    }
}

void delay_ms(unsigned int ms)
{
    unsigned int i;
    for (i = 0; i < ms; i = i + 1) {
        delay_us(1000);
    }
}
