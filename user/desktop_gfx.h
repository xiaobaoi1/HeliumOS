#ifndef DESKTOP_GFX_H
#define DESKTOP_GFX_H

#include "wm_font.h"
#include "unicode_font.h"
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
    const unsigned char *p = (const unsigned char*)s;
    while (*p) {
        uint32_t cp;
        int n = ufont_utf8_decode(p, &cp);
        const uint8_t *g = wm_font_glyph((int)cp);
        int gw = wm_font_width((int)cp);
        if (!g) {
            g = wm_font_glyph('?');
            gw = wm_font_width('?');
        }
        if (!gw) gw = 8;
        for (int r = 0; r < 16; r++) {
            for (int j = 0; j < gw; j++) {
                int x = px + j, y = py + r;
                if (x < 0 || x >= pw) continue;
                int bit;
                if (gw == 16) bit = g ? (g[r * 2 + (j >> 3)] & (0x80 >> (j & 7))) : 0;
                else          bit = g ? (g[r] & (0x80 >> j)) : 0;
                pixels[y * pw + x] = bit ? fg : bg;
            }
        }
        px += gw;
        p += n;
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