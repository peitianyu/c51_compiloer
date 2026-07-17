/* gpio.c — 8051 GPIO 控制实现 */

#include "gpio.h"
#include "reg8051.h"

void gpio_init_pin(PinNumber pin, PinDirection dir)
{
    if (dir == PIN_OUTPUT) {
        P1 = P1 & ~(1 << pin);
    } else {
        P1 = P1 | (1 << pin);
    }
}

void gpio_set_pin(PinNumber pin, unsigned char val)
{
    if (val) {
        P1 = P1 | (1 << pin);
    } else {
        P1 = P1 & ~(1 << pin);
    }
}

unsigned char gpio_read_pin(PinNumber pin)
{
    return (P1 >> pin) & 1;
}

void gpio_toggle_pin(PinNumber pin)
{
    P1 = P1 ^ (1 << pin);
}
