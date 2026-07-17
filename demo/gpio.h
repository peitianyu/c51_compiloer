/* gpio.h — 8051 GPIO 控制头文件 */

#ifndef GPIO_H
#define GPIO_H

/* 引脚号枚举（P1 端口）*/
typedef enum {
    PIN_0 = 0,
    PIN_1 = 1,
    PIN_2 = 2,
    PIN_3 = 3,
    PIN_4 = 4,
    PIN_5 = 5,
    PIN_6 = 6,
    PIN_7 = 7,
} PinNumber;

/* 引脚方向 */
typedef enum {
    PIN_OUTPUT = 0,
    PIN_INPUT  = 1,
} PinDirection;

/* 初始化 P1 端口引脚 */
void gpio_init_pin(PinNumber pin, PinDirection dir);

/* 设置 P1 引脚输出高/低 */
void gpio_set_pin(PinNumber pin, unsigned char val);

/* 读取 P1 引脚 */
unsigned char gpio_read_pin(PinNumber pin);

/* 翻转 P1 引脚 */
void gpio_toggle_pin(PinNumber pin);

#endif
