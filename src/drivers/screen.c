#include <screen.h>
#include <io.h>
#include <stddef.h>

#define VGA_MEMORY   ((uint16_t*)0xB8000)
#define VGA_WIDTH    80
#define VGA_HEIGHT   25

/* VGA I/O 端口（用于移动硬件光标） */
#define VGA_CTRL_REG   0x3D4
#define VGA_DATA_REG   0x3D5
#define VGA_CURSOR_HI  0x0E
#define VGA_CURSOR_LO  0x0F

static uint16_t *vga_buffer = VGA_MEMORY;
static int cursor_x = 0;
static int cursor_y = 0;

/* 当前属性字节：高 4 位背景，低 4 位前景 */
static uint8_t current_attr = VGA_DEFAULT_ATTR;

/* ---------- 内部辅助 ---------- */

static inline uint16_t make_cell(char c) {
    return ((uint16_t)current_attr << 8) | (uint8_t)c;
}
static inline uint16_t make_attr_cell(char c, uint8_t attr) {
    return ((uint16_t)attr << 8) | (uint8_t)c;
}

static void update_hardware_cursor(void) {
    uint16_t pos = cursor_y * VGA_WIDTH + cursor_x;
    outb(VGA_CTRL_REG, VGA_CURSOR_HI);
    outb(VGA_DATA_REG, (pos >> 8) & 0xFF);
    outb(VGA_CTRL_REG, VGA_CURSOR_LO);
    outb(VGA_DATA_REG, pos & 0xFF);
}

static void clear_line(int y) {
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_buffer[y * VGA_WIDTH + x] = make_attr_cell(' ', VGA_DEFAULT_ATTR);
    }
}

static void scroll_up(void) {
    for (int y = 1; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[(y - 1) * VGA_WIDTH + x] = vga_buffer[y * VGA_WIDTH + x];
        }
    }
    clear_line(VGA_HEIGHT - 1);
    cursor_y = VGA_HEIGHT - 1;
}

static void newline(void) {
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= VGA_HEIGHT) {
        scroll_up();
    }
}

/* ---------- 对外接口 ---------- */

void screen_init(void) {
    current_attr = VGA_DEFAULT_ATTR;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = make_cell(' ');
    }
    cursor_x = 0;
    cursor_y = 0;
    update_hardware_cursor();
}

void screen_clear(void) {
    screen_init();
}

void screen_set_color(uint8_t fg, uint8_t bg) {
    current_attr = ((bg & 0x0F) << 4) | (fg & 0x0F);
}

uint8_t screen_get_fg(void) { return current_attr & 0x0F; }
uint8_t screen_get_bg(void) { return (current_attr >> 4) & 0x0F; }

void screen_write_char(char c) {
    switch (c) {
        case '\n':
            newline();
            break;

        case '\r':
            cursor_x = 0;
            break;

        case '\t': {
            int next = (cursor_x + 8) & ~7;
            while (cursor_x < next && cursor_x < VGA_WIDTH) {
                screen_write_char(' ');
            }
            break;
        }

        case '\b': {
            if (cursor_x > 0) {
                cursor_x--;
            } else if (cursor_y > 0) {
                cursor_y--;
                cursor_x = VGA_WIDTH - 1;
            }
            int idx = cursor_y * VGA_WIDTH + cursor_x;
            vga_buffer[idx] = make_cell(' ');
            break;
        }

        default: {
            if ((unsigned char)c < 0x20) break;
            int idx = cursor_y * VGA_WIDTH + cursor_x;
            vga_buffer[idx] = make_cell(c);
            cursor_x++;
            if (cursor_x >= VGA_WIDTH) {
                newline();
            }
            break;
        }
    }
    update_hardware_cursor();
}

void screen_write_string(const char *str) {
    if (!str) return;
    for (int i = 0; str[i] != '\0'; i++) {
        screen_write_char(str[i]);
    }
}

void screen_write_char_colored(const char c, uint8_t fg, uint8_t bg) {
    uint8_t saved_fg = screen_get_fg();
    uint8_t saved_bg = screen_get_bg();
    screen_set_color(fg, bg);
    screen_write_char(c);
    screen_set_color(saved_fg, saved_bg);
}

void screen_write_string_colored(const char *str, uint8_t fg, uint8_t bg) {
    uint8_t saved_fg = screen_get_fg();
    uint8_t saved_bg = screen_get_bg();
    screen_set_color(fg, bg);
    screen_write_string(str);
    screen_set_color(saved_fg, saved_bg);
}

void screen_set_cursor(int x, int y) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return;
    cursor_x = x;
    cursor_y = y;
    update_hardware_cursor();
}

void screen_get_cursor(int *x, int *y) {
    if (x) *x = cursor_x;
    if (y) *y = cursor_y;
}