/* timer.h — 8051 定时器驱动头文件 */
#ifndef TIMER_H
#define TIMER_H

/* 定时器模式 */
#define TIMER_MODE0 0x00
#define TIMER_MODE1 0x01
#define TIMER_MODE2 0x10
#define TIMER_MODE3 0x11

/* 初始化定时器 0（16位模式） */
void timer0_init(unsigned int reload);

/* 启动/停止定时器 0 */
void timer0_start(void);
void timer0_stop(void);

/* 获取当前计数值 */
unsigned int timer0_get(void);

/* 忙等待微秒（近似） */
void delay_us(unsigned int us);
void delay_ms(unsigned int ms);

#endif
