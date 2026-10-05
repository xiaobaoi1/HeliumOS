#include <tty.h>
#include <screen.h>
#include <keyboard.h>
#include <serial.h>
#include <device.h>
#include <printf.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <display.h>
#include <display.h>

/* tty 的内部状态 */
static const struct device_ops *vga_ops = NULL;

static int cursor_x = 0;
static int cursor_y = 0;
static uint8_t current_attr = VGA_DEFAULT_ATTR;

static int saved_cursor_x = 0;
static int saved_cursor_y = 0;
static int cursor_saved    = 0;

static struct task *fg_task = NULL;

/* ---------- 内部辅助 ---------- */

static void put_cell(int x, int y, char c, uint8_t attr) {
    display_draw_char(x, y, c, attr);
}

static void update_hw_cursor(void) {
    display_set_cursor(cursor_x, cursor_y);
}

static void scroll_up(void) {
    display_scroll(current_attr);
    cursor_y = display_rows() - 1;
}

static void newline(void) {
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= display_rows()) {
        scroll_up();
    }
}

/* ---------- 生命周期 ---------- */

void tty_init(void) {
    display_init();

    cursor_x = 0;
    cursor_y = 0;
    current_attr = VGA_DEFAULT_ATTR;
    saved_cursor_x = 0;
    saved_cursor_y = 0;
    cursor_saved   = 0;

    kprintf("[TTY] Initialized\n");
}

void tty_try_upgrade(void) {
    const struct display_ops *new_ops = fb_char_get_ops();
    if (!new_ops) return;

    /* VBE 模式下 0xB8000 已失效，之前的 tty 输出无法迁移。
     * display_switch 内部已清屏。 */
    display_switch(new_ops);
}

void tty_set_foreground(struct task *t) {
    fg_task = t;
}

struct task *tty_get_foreground(void) {
    return fg_task;
}

/* ---------- 输出 ---------- */

int tty_write(const char *buf, uint32_t n) {
    if (!buf || n == 0) return EINVAL;
    for (uint32_t i = 0; i < n; i++) {
        char c = buf[i];
        switch (c) {
            case '\n':
                newline();
                break;
            case '\r':
                cursor_x = 0;
                break;
            case '\t': {
                int next = (cursor_x + 8) & ~7;
                while (cursor_x < next && cursor_x < display_cols()) {
                    put_cell(cursor_x, cursor_y, ' ', current_attr);
                    cursor_x++;
                }
                break;
            }
            case '\b':
                if (cursor_x > 0) {
                    cursor_x--;
                } else if (cursor_y > 0) {
                    cursor_y--;
                    cursor_x = display_cols() - 1;
                }
                put_cell(cursor_x, cursor_y, ' ', current_attr);
                break;
            default:
                if ((unsigned char)c < 0x20) break;
                put_cell(cursor_x, cursor_y, c, current_attr);
                cursor_x++;
                if (cursor_x >= display_cols()) {
                    newline();
                }
                break;
        }
    }
    update_hw_cursor();
    return (int)n;
}

void tty_clear(void) {
    display_clear(current_attr);
    cursor_x = 0;
    cursor_y = 0;
    update_hw_cursor();
}

void tty_set_color(uint8_t fg, uint8_t bg) {
    current_attr = ((bg & 0x0F) << 4) | (fg & 0x0F);
}

/* ---------- 输入（不变） ---------- */

int tty_read(char *buf, uint32_t n) {
    if (!buf || n == 0) return EINVAL;

    /* 键盘归属检查：有前台且不是自己 → 读不到 */
    struct task *cur = get_current_task();
    if (fg_task && cur != fg_task) {
        return 0;
    }

    uint32_t got = 0;
    while (got < n && keyboard_has_data()) {
        buf[got++] = keyboard_getchar();
    }
    return (int)got;
}

/* ---------- 光标 ---------- */

void tty_set_cursor(int x, int y) {
    if (x < 0 || x >= display_cols()) return;
    if (y < 0 || y >= display_rows()) return;
    cursor_x = x;
    cursor_y = y;
    update_hw_cursor();
}

void tty_get_size(int *cols, int *rows) {
    if (cols) *cols = display_cols();
    if (rows) *rows = display_rows();
}

void tty_get_cursor(int *x, int *y) {
    if (x) *x = cursor_x;
    if (y) *y = cursor_y;
}

void tty_save_cursor(void) {
    saved_cursor_x = cursor_x;
    saved_cursor_y = cursor_y;
    cursor_saved   = 1;
}

void tty_restore_cursor(void) {
    if (!cursor_saved) return;
    tty_set_cursor(saved_cursor_x, saved_cursor_y);
}

/* ---------- 调试（不变） ---------- */

int tty_debug_write(const char *buf, uint32_t n) {
    if (!buf || n == 0) return EINVAL;
    for (uint32_t i = 0; i < n; i++) {
        serial_write_char(buf[i]);
    }
    return (int)n;
}