#ifndef DESKTOP_GFX_H
#define DESKTOP_GFX_H

#include "wm_font.h"
#include <stdint.h>

static unsigned int gfx_lerp(unsigned int a, unsigned int b, int t, int max) {
    if (max <= 0) return a;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * t / max;
    int g = ag + (bg - ag) * t / max;
    int bl = ab + (bb - ab) * t / max;
    return (r << 16) | (g << 8) | bl;
}

static void gfx_draw_text(unsigned int *pixels, int pw, int px, int py,
                          const char *s, unsigned int fg, unsigned int bg) {
    for (const char *p = s; *p; p++) {
        int cp = (unsigned char)*p;
        if (cp >= 128) cp = '?';
        const uint8_t *g = wm_font_glyph(cp);
        for (int r = 0; r < 16; r++) {
            unsigned char bits = g ? g[r] : 0;
            for (int j = 0; j < 8; j++) {
                int x = px + j;
                int y = py + r;
                if (x < 0 || x >= pw) continue;
                unsigned int c = (bits & (0x80 >> j)) ? fg : bg;
                pixels[y * pw + x] = c;
            }
        }
        px += 8;
    }
}

static void gfx_fill_rect(unsigned int *pixels, int pw, int ph,
                          int x, int y, int w, int h, unsigned int c) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= ph) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= pw) continue;
            pixels[py * pw + px] = c;
        }
    }
}
#endif