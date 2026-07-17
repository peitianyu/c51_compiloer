/* uart.h — 8051 UART 串口驱动头文件 */

#ifndef UART_H
#define UART_H

/* 波特率枚举 */
typedef enum {
    BAUD_1200  = 0,
    BAUD_2400  = 1,
    BAUD_4800  = 2,
    BAUD_9600  = 3,
    BAUD_19200 = 4,
    BAUD_38400 = 5,
    BAUD_57600 = 6,
    BAUD_115200 = 7,
} BaudRate;

/* 初始化 UART（模式 1，8 位可变波特率）*/
void uart_init(BaudRate baud);

/* 发送一个字节 */
void uart_send(unsigned char c);

/* 接收一个字节（阻塞）*/
unsigned char uart_recv(void);

/* 发送字符串 */
void uart_send_string(const char *str);

/* 发送十六进制值 */
void uart_send_hex(unsigned int val);

#endif
