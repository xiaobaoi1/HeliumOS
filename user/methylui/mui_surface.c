#include "methylui/mui_surface.h"
#include "methylui/mui_font.h"

static void put_pixel(struct mui_surface *s, int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    s->pixels[y * s->w + x] = rgb;
}

void mui_mark_dirty(struct mui_surface *s,
                    int x0, int y0, int x1, int y1) {
    if (!s || !s->hdr) return;
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x1 <= x0 || y1 <= y0) return;

    uint32_t ux0 = (uint32_t)x0, uy0 = (uint32_t)y0;
    uint32_t ux1 = (uint32_t)x1, uy1 = (uint32_t)y1;

    if (s->hdr->cur_x1 <= s->hdr->cur_x0 ||
        s->hdr->cur_x0 == SHM_DIRTY_EMPTY) {
        s->hdr->cur_x0 = ux0; s->hdr->cur_y0 = uy0;
        s->hdr->cur_x1 = ux1; s->hdr->cur_y1 = uy1;
    } else {
        if (ux0 < s->hdr->cur_x0) s->hdr->cur_x0 = ux0;
        if (uy0 < s->hdr->cur_y0) s->hdr->cur_y0 = uy0;
        if (ux1 > s->hdr->cur_x1) s->hdr->cur_x1 = ux1;
        if (uy1 > s->hdr->cur_y1) s->hdr->cur_y1 = uy1;
    }
}

void mui_fill(struct mui_surface *s, int x, int y, int w, int h,
              unsigned int rgb) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put_pixel(s, x + i, y + j, rgb);
    mui_mark_dirty(s, x, y, x + w, y + h);
}

void mui_border(struct mui_surface *s, int x, int y, int w, int h,
                unsigned int rgb) {
    if (w <= 0 || h <= 0) return;
    mui_fill(s, x, y, w, 1, rgb);
    mui_fill(s, x, y + h - 1, w, 1, rgb);
    mui_fill(s, x, y, 1, h, rgb);
    mui_fill(s, x + w - 1, y, 1, h, rgb);
}

int mui_draw_char(struct mui_surface *s, int x, int y, unsigned int cp,
                  unsigned int fg, unsigned int bg) {
    if (cp >= 128) cp = '?';
    const uint8_t *g = mui_font_glyph((int)cp);

    mui_fill(s, x, y, MUI_FONT_W, MUI_FONT_H, bg);
    if (!g) return MUI_FONT_W;

    for (int r = 0; r < MUI_FONT_H; r++) {
        unsigned char bits = g[r];
        for (int j = 0; j < 8; j++) {
            if (bits & (0x80 >> j))
                put_pixel(s, x + j, y + r, fg);
        }
    }
    return MUI_FONT_W;
}

int mui_text(struct mui_surface *s, int x, int y, const char *str,
             unsigned int fg, unsigned int bg) {
    int start_x = x;
    for (const char *p = str; *p; p++) {
        x += mui_draw_char(s, x, y, (unsigned char)*p, fg, bg);
    }
    return x - start_x;
}

int mui_text_width(const char *str) {
    int w = 0;
    for (const char *p = str; *p; p++) w += MUI_FONT_W;
    return w;
}