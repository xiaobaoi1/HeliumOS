#include <display.h>
#include <screen.h>
#include <io.h>
#include <stdint.h>

#define VGA_MEM        ((uint16_t*)0xB8000)
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5
#define VGA_CURSOR_HI  0x0E
#define VGA_CURSOR_LO  0x0F

static void vga_draw_char(int x, int y, char c, uint8_t attr) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return;
    VGA_MEM[y * VGA_WIDTH + x] = ((uint16_t)attr << 8) | (uint8_t)c;
}

static void vga_clear(uint8_t attr) {
    uint16_t cell = ((uint16_t)attr << 8) | ' ';
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_MEM[i] = cell;
    }
}

static void vga_scroll(uint8_t attr) {
    for (int y = 1; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            VGA_MEM[(y - 1) * VGA_WIDTH + x] = VGA_MEM[y * VGA_WIDTH + x];
        }
    }
    uint16_t cell = ((uint16_t)attr << 8) | ' ';
    for (int x = 0; x < VGA_WIDTH; x++) {
        VGA_MEM[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = cell;
    }
}

static void vga_set_cursor(int x, int y) {
    uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);
    outb(VGA_CRTC_INDEX, VGA_CURSOR_HI);
    outb(VGA_CRTC_DATA, (pos >> 8) & 0xFF);
    outb(VGA_CRTC_INDEX, VGA_CURSOR_LO);
    outb(VGA_CRTC_DATA, pos & 0xFF);
}

static const struct display_ops vga_text_ops = {
    .name = "vga_text",
    .cols = VGA_WIDTH,
    .rows = VGA_HEIGHT,
    .draw_char = vga_draw_char,
    .clear = vga_clear,
    .scroll = vga_scroll,
    .draw_cursor = vga_set_cursor,
};

void vga_text_register(void) {
    display_register(&vga_text_ops);
}