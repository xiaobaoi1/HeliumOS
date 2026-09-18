#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

/* 控制台生命周期 */
void console_init(void);

/* ---------- 输出 ---------- */
/* 向屏幕写 n 字节，返回写入的字节数（负数 = 错误码） */
int  console_write(const char *buf, uint32_t n);

/* 清屏（不重置颜色） */
void console_clear(void);

/* 设置当前颜色 */
void console_set_color(uint8_t fg, uint8_t bg);

/* ---------- 输入 ---------- */
/* 从键盘读 n 字节（非阻塞），返回实际读到的字节数（可能为 0） */
int  console_read(char *buf, uint32_t n);

/* ---------- 光标 ---------- */
void console_set_cursor(int x, int y);
void console_get_cursor(int *x, int *y);
void console_save_cursor(void);
void console_restore_cursor(void);

/* ---------- 调试 ---------- */
/* 向串口写 n 字节，不污染屏幕 */
int  console_debug_write(const char *buf, uint32_t n);

#endif