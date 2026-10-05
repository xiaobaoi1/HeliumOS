#ifndef TTY_H
#define TTY_H

#include <stdint.h>

/* tty 生命周期 */
void tty_init(void);

/* ---------- 输出 ---------- */
int  tty_write(const char *buf, uint32_t n);
void tty_clear(void);
void tty_set_color(uint8_t fg, uint8_t bg);

/* ---------- 输入 ---------- */
int  tty_read(char *buf, uint32_t n);

/* ---------- 光标 ---------- */
void tty_set_cursor(int x, int y);
void tty_get_size(int *cols, int *rows);
void tty_get_cursor(int *x, int *y);
void tty_save_cursor(void);
void tty_restore_cursor(void);

/* ---------- 调试 ---------- */
int  tty_debug_write(const char *buf, uint32_t n);

/* 前台进程（键盘归属）。
 * NULL 表示"无前台"，所有进程都能读键盘。
 * 非 NULL 时只有该进程能读到键盘输入。 */
struct task;
void tty_set_foreground(struct task *t);
struct task *tty_get_foreground(void);

void tty_try_upgrade(void);

#endif