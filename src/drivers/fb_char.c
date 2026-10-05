#include <display.h>
#include <fb.h>
#include <font.h>
#include <screen.h>
#include <string.h>
#include <stdint.h>

#define CELL_W 8
#define CELL_H 16

static const uint32_t vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static inline void put_pixel(struct fb_info *fb, int x, int y, uint32_t rgb) {
    if ((uint32_t)x >= fb->width || (uint32_t)y >= fb->height) return;
    uint32_t off = (y * fb->pitch + x * 4) / 4;
    fb->back[off] = rgb;
}

static void fill_rect(struct fb_info *fb, int x, int y, int w, int h, uint32_t rgb) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            put_pixel(fb, x + i, y + j, rgb);
        }
    }
}

static void fbc_draw_char(int x, int y, char ch, uint8_t attr) {
    struct fb_info *fb = fb_get();
    if (!fb || !fb->valid) return;

    uint32_t fg = vga_palette[attr & 0x0F];
    uint32_t bg = vga_palette[(attr >> 4) & 0x0F];

    int px = x * CELL_W;
    int py = y * CELL_H;

    fill_rect(fb, px, py, CELL_W, CELL_H, bg);

    int w = 8;
    const uint8_t *g = font_glyph((uint8_t)ch, &w);
    if (!g) return;

    if (w == 8) {
        for (int r = 0; r < 16; r++) {
            uint8_t row = g[r];
            for (int col = 0; col < 8; col++) {
                if (row & (0x80 >> col)) {
                    put_pixel(fb, px + col, py + r, fg);
                }
            }
        }
    } else if (w == 16) {
        /* 双宽字符只画左半——当前 tty 单列模型。ASCII 碰不到。 */
        for (int r = 0; r < 16; r++) {
            uint8_t hi = g[r * 2];
            for (int col = 0; col < 8; col++) {
                if (hi & (0x80 >> col)) {
                    put_pixel(fb, px + col, py + r, fg);
                }
            }
        }
    }
}

static void fbc_clear(uint8_t attr) {
    struct fb_info *fb = fb_get();
    if (!fb || !fb->valid) return;
    uint32_t bg = vga_palette[(attr >> 4) & 0x0F];
    uint32_t n = (fb->pitch / 4) * fb->height;
    for (uint32_t i = 0; i < n; i++) fb->back[i] = bg;
}

static void fbc_scroll(uint8_t attr) {
    struct fb_info *fb = fb_get();
    if (!fb || !fb->valid) return;

    uint32_t shift = CELL_H * fb->pitch;
    uint32_t total = fb->pitch * fb->height;
    if (shift >= total) return;

    memcpy((uint8_t*)fb->back, (uint8_t*)fb->back + shift, total - shift);

    uint32_t bg = vga_palette[(attr >> 4) & 0x0F];
    uint32_t *last = (uint32_t*)((uint8_t*)fb->back + total - shift);
    for (uint32_t i = 0; i < shift / 4; i++) last[i] = bg;
}

static int cur_x = -1;
static int cur_y = -1;

static void fbc_draw_cursor(int x, int y) {
    struct fb_info *fb = fb_get();
    if (!fb || !fb->valid) return;

    int px = x * CELL_W;
    int py = y * CELL_H + 14;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 8; i++)
            put_pixel(fb, px + i, py + j, 0xFFFFFF);
}

static const struct display_ops fb_char_ops_template = {
    .name = "fb_char",
    .cols = 0,
    .rows = 0,
    .draw_char = fbc_draw_char,
    .clear = fbc_clear,
    .scroll = fbc_scroll,
    .draw_cursor = fbc_draw_cursor,
};

const struct display_ops *fb_char_get_ops(void) {
    struct fb_info *fb = fb_get();
    const struct font *f = font_get();
    if (!fb || !fb->valid) return NULL;
    if (!f || !f->valid) return NULL;

    static struct display_ops ops;
    ops = fb_char_ops_template;
    ops.cols = fb->width / CELL_W;
    ops.rows = fb->height / CELL_H;
    return &ops;
}