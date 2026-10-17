#include <tty.h>
#include <display.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <screen.h>

static int cursor_x = 0;
static int cursor_y = 0;
static uint8_t current_attr = VGA_DEFAULT_ATTR;

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

void tty_init(void) {
    display_init();
    cursor_x = 0;
    cursor_y = 0;
    current_attr = VGA_DEFAULT_ATTR;
    kprintf("[TTY] Initialized\n");
}

void tty_try_upgrade(void) {
    const struct display_ops *new_ops = fb_char_get_ops();
    if (!new_ops) return;
    display_switch(new_ops);
}

int tty_write(const char *buf, uint32_t n) {
    if (!buf || n == 0) return EINVAL;
    for (uint32_t i = 0; i < n; i++) {
        char c = buf[i];
        switch (c) {
            case '\n': newline(); break;
            case '\r': cursor_x = 0; break;
            case '\t': {
                int next = (cursor_x + 8) & ~7;
                while (cursor_x < next && cursor_x < display_cols()) {
                    put_cell(cursor_x, cursor_y, ' ', current_attr);
                    cursor_x++;
                }
                break;
            }
            case '\b':
                if (cursor_x > 0) cursor_x--;
                else if (cursor_y > 0) {
                    cursor_y--;
                    cursor_x = display_cols() - 1;
                }
                put_cell(cursor_x, cursor_y, ' ', current_attr);
                break;
            default:
                if ((unsigned char)c < 0x20) break;
                put_cell(cursor_x, cursor_y, c, current_attr);
                cursor_x++;
                if (cursor_x >= display_cols()) newline();
                break;
        }
    }
    update_hw_cursor();
    return (int)n;
}

void tty_write_char(char c) {
    tty_write(&c, 1);
    display_flush();
}