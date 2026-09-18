#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/* 初始化键盘驱动（如果有需要，比如设置 LED） */
void keyboard_init(void);

/* 中断处理入口，由 isr.c 调用 */
void keyboard_handle_irq(void);

/* 用户/内核读取一个字符，返回 0 表示无数据 */
char keyboard_getchar(void);

/* 缓冲区是否有数据 */
int keyboard_has_data(void);

#endif