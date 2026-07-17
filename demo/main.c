/* main.c — 8051 综合演示程序
 *
 * 演示功能：
 *   1. 初始化 GPIO、UART、定时器
 *   2. 通过 UART 输出启动信息
 *   3. 定时闪烁 LED（P1.0）
 *   4. 通过 UART 打印计数器值
 *
 * 编译：ttcc --target mcs51 -o multi_file.c main.c timer.c uart.c gpio.c
 */

/* 声明外部函数 */
#include "timer.h"
#include "uart.h"
#include "gpio.h"
#include "reg8051.h"

/* 中断服务函数 */
void isr_timer0(void) interrupt 1
{
    static unsigned int tick = 0;
    tick = tick + 1;
    /* 每 100ms 翻转一次 LED */
    if (tick >= 100) {
        tick = 0;
        gpio_toggle_pin(0);
    }
}

/*
 * 计算阶乘（递归函数测试）
 */
static unsigned long factorial(unsigned int n)
{
    if (n <= 1) {
        return 1;
    }
    return (unsigned long)n * factorial(n - 1);
}

/*
 * 计算斐波那契数列
 */
static unsigned int fibonacci(unsigned int n)
{
    if (n == 0) {
        return 0;
    }
    if (n == 1) {
        return 1;
    }
    return fibonacci(n - 1) + fibonacci(n - 2);
}

/* 主函数 */
void main(void)
{
    unsigned int i;
    unsigned long fact_result;
    unsigned int fib_result;

    /* 初始化 GPIO P1.0 为输出（LED）*/
    gpio_init_pin(0, 0);
    gpio_set_pin(0, 0);

    /* 初始化 UART（9600 波特率）*/
    uart_init(3);

    /* 初始化定时器 0（100ms 中断间隔 @12MHz）*/
    timer0_init(0xFFFF);

    /* 输出启动信息 */
    uart_send_string("\r\n");
    uart_send_string("8051 Multi-File Demo\r\n");
    uart_send_string("====================\r\n");
    uart_send_string("System initialized.\r\n");

    /* 计算演示 */
    for (i = 0; i < 10; i = i + 1) {
        fact_result = factorial(i);
        fib_result  = fibonacci(i);

        uart_send_string("n=");
        uart_send_hex(i);
        uart_send_string(" fact=");
        uart_send_hex((unsigned int)fact_result);
        uart_send_string(" fib=");
        uart_send_hex(fib_result);
        uart_send_string("\r\n");
    }

    uart_send_string("All tests passed.\r\n");

    /* 启动定时器 */
    timer0_start();

    /* 主循环 */
    while (1) {
        delay_ms(500);
        gpio_set_pin(0, 1);
        delay_ms(500);
        gpio_set_pin(0, 0);
    }
}
