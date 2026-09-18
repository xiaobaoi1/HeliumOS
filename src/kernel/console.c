#include <console.h>
#include <screen.h>
#include <keyboard.h>
#include <serial.h>
#include <printf.h>
#include <errno.h>

/* 保存的光标位置（每控制台一个槽） */
static int saved_cursor_x = 0;
static int saved_cursor_y = 0;
static int cursor_saved    = 0;

void console_init(void) {
    saved_cursor_x = 0;
    saved_cursor_y = 0;
    cursor_saved   = 0;
    kprintf("[CONSOLE] Initialized\n");
}

/* ---------- 输出 ---------- */

int console_write(const char *buf, uint32_t n) {
    if (!buf || n == 0) return EINVAL;
    for (uint32_t i = 0; i < n; i++) {
        screen_write_char(buf[i]);
    }
    return (int)n;
}

void console_clear(void) {
    screen_clear();
}

void console_set_color(uint8_t fg, uint8_t bg) {
    screen_set_color(fg, bg);
}

/* ---------- 输入 ---------- */

int console_read(char *buf, uint32_t n) {
    if (!buf || n == 0) return EINVAL;
    uint32_t got = 0;
    while (got < n && keyboard_has_data()) {
        buf[got++] = keyboard_getchar();
    }
    return (int)got;   /* 非阻塞，可能返回 0 */
}

/* ---------- 光标 ---------- */

void console_set_cursor(int x, int y) {
    screen_set_cursor(x, y);
}

void console_get_cursor(int *x, int *y) {
    screen_get_cursor(x, y);
}

void console_save_cursor(void) {
    screen_get_cursor(&saved_cursor_x, &saved_cursor_y);
    cursor_saved = 1;
}

void console_restore_cursor(void) {
    if (!cursor_saved) return;
    screen_set_cursor(saved_cursor_x, saved_cursor_y);
}

/* ---------- 调试 ---------- */

int console_debug_write(const char *buf, uint32_t n) {
    if (!buf || n == 0) return EINVAL;
    for (uint32_t i = 0; i < n; i++) {
        serial_write_char(buf[i]);
    }
    return (int)n;
}