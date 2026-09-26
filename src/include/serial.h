#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

// COM1 端口基址
#define SERIAL_COM1_BASE 0x3F8

// 串口寄存器偏移（相对于基址）
#define SERIAL_DATA        0  // 数据寄存器（读/写）
#define SERIAL_INTERRUPT   1  // 中断使能寄存器
#define SERIAL_FIFO        2  // FIFO 控制寄存器（只写）
#define SERIAL_LINE_CTRL   3  // 线路控制寄存器
#define SERIAL_MODEM_CTRL  4  // 调制解调器控制寄存器
#define SERIAL_LINE_STATUS 5  // 线路状态寄存器（只读）

// 初始化串口（波特率 115200, 8N1）
void serial_init(void);

/* 注册为 DEV_TYPE_SERIAL。需要在 dev_init 之后调用。 */
void serial_register_dev(void);

// 发送单个字符
void serial_write_char(char c);

// 发送字符串
void serial_write_string(const char *str);

// 检查发送缓冲区是否为空（非阻塞发送用）
int serial_is_transmit_empty(void);

#endif