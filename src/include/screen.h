#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>

/* ---------- 颜色常量（前景 / 背景通用） ---------- */
#define VGA_BLACK        0
#define VGA_BLUE         1
#define VGA_GREEN        2
#define VGA_CYAN         3
#define VGA_RED          4
#define VGA_MAGENTA      5
#define VGA_BROWN        6
#define VGA_LIGHT_GRAY   7
#define VGA_DARK_GRAY    8
#define VGA_LIGHT_BLUE   9
#define VGA_LIGHT_GREEN  10
#define VGA_LIGHT_CYAN   11
#define VGA_LIGHT_RED    12
#define VGA_LIGHT_MAGENTA 13
#define VGA_YELLOW       14
#define VGA_WHITE        15

/* 默认配色 */
#define VGA_DEFAULT_ATTR ((VGA_BLACK << 4) | VGA_LIGHT_GRAY)

/* ---------- 基础输出 ---------- */
void screen_init(void);
void screen_clear(void);
void screen_write_char(char c);
void screen_write_string(const char *str);

/* ---------- 颜色控制 ---------- */
void screen_set_color(uint8_t fg, uint8_t bg);
uint8_t screen_get_fg(void);
uint8_t screen_get_bg(void);

/* 快捷打印：设置颜色 → 打印 → 恢复 */
void screen_write_string_colored(const char *str, uint8_t fg, uint8_t bg);
void screen_write_char_colored(const char c, uint8_t fg, uint8_t bg);

/* 光标 */
void screen_set_cursor(int x, int y);
void screen_get_cursor(int *x, int *y);

#endif