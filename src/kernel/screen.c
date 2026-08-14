#include <screen.h>
#include <stddef.h>

#define VGA_MEMORY (uint16_t*)0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static uint16_t *vga_buffer = VGA_MEMORY;
static int cursor_x = 0;
static int cursor_y = 0;

void screen_init(void) {
    // 清屏 (填黑色空格)
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = 0x0F00 | ' ';  // 白色前景, 黑色背景
    }
    cursor_x = 0;
    cursor_y = 0;
}

void screen_write_char(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else {
        int index = cursor_y * VGA_WIDTH + cursor_x;
        vga_buffer[index] = 0x0F00 | c;
        cursor_x++;
        if (cursor_x >= VGA_WIDTH) {
            cursor_x = 0;
            cursor_y++;
        }
    }
    // 滚动 (简单处理)
    if (cursor_y >= VGA_HEIGHT) {
        // 略, 后续完善
        cursor_y = VGA_HEIGHT - 1;
    }
}

void screen_write_string(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        screen_write_char(str[i]);
    }
}