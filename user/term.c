/* term — 终端模拟器（支持完整 unifont.hex + 输入法）
 *
 * 用法: TERM <shm_name> <client_w> <client_h> <event_pipe> [prog [args...]]
 */
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "wm_proto.h"
#include "shm_layout.h"
#include "methylui/mui_surface.h"
#include "methylui/mui_widget.h"
#include "methylui/mui_scroll.h"
#include "ime.h"
#include "unicode_font.h"
#include <stdint.h>

static int g_master = -1;

#define FONT_H   16
#define CELL_W   8
#define MAX_CP   0x10000
#define GLYPH_DATA_MAX (2 * 1024 * 1024)

/* ---------- 字体 ---------- */

static unsigned char **g_glyph_ptr   = NULL;
static unsigned char  *g_glyph_width = NULL;
static unsigned char  *g_glyph_data  = NULL;
static unsigned int    g_glyph_data_used = 0;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int load_unifont(void) {
    int fd = fs_open("SYS:/UNIFONT-17.HEX", FS_O_RDONLY);
    if (fd < 0) return -1;

    static char buf[65536];
    char line[128];
    int line_len = 0;
    int loaded = 0;

    for (;;) {
        int n = fs_read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (line_len == 0) continue;
                line[line_len] = '\0';

                const char *p = line;
                uint32_t cp = 0;
                int ok = 1;
                while (*p && *p != ':') {
                    int v = hexval(*p);
                    if (v < 0) { ok = 0; break; }
                    cp = cp * 16 + (uint32_t)v;
                    p++;
                }
                if (!ok || *p != ':') { line_len = 0; continue; }
                p++;
                int hl = 0;
                const char *q = p;
                while (hexval(q[0]) >= 0 && hexval(q[1]) >= 0) { hl += 2; q += 2; }
                int width = (hl == 32) ? 8 : (hl == 64) ? 16 : 0;
                if (!width || cp >= MAX_CP) { line_len = 0; continue; }
                int data_size = hl / 2;
                if (g_glyph_data_used + (unsigned)data_size > GLYPH_DATA_MAX) {
                    line_len = 0; continue;
                }
                unsigned char *dst = g_glyph_data + g_glyph_data_used;
                int decode_ok = 1;
                for (int k = 0; k < data_size; k++) {
                    int hi = hexval(p[0]);
                    int lo = hexval(p[1]);
                    if (hi < 0 || lo < 0) { decode_ok = 0; break; }
                    dst[k] = (unsigned char)((hi << 4) | lo);
                    p += 2;
                }
                if (decode_ok) {
                    g_glyph_ptr[cp] = dst;
                    g_glyph_width[cp] = (unsigned char)width;
                    g_glyph_data_used += (unsigned)data_size;
                    loaded++;
                }
                line_len = 0;
            } else if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }
    fs_close(fd);
    return loaded;
}

static void load_font(void) {
    int loaded = ufont_load();
    dprintf(2, "term: unifont=%d\n", loaded == 0 ? 57086 : 0);
}

/* ---------- 渲染 ---------- */

#define COL_BG 0x000000
#define COL_FG 0xCCCCCC

static int g_cw, g_ch;
static int g_cols, g_rows;
static int g_cx = 0, g_cy = 0;
static int g_pending_wrap = 0;
static int g_cursor_visible = 1;
static int g_cursor_drawn = 0;
static int g_cursor_drawn_x = 0, g_cursor_drawn_y = 0;
static uint32_t g_cursor_save[CELL_W * FONT_H];
static unsigned int *g_pixels;        /* 当前显示（shm） */
static struct shm_hdr *g_shm = NULL;

/* ---------- 滚动条 + 回滚 ---------- */
#define TERM_SCROLL_W   8
#define TERM_HIST_LINES 32            /* 回滚 32 行（每行 FONT_H 像素行） */
static int g_text_w;                  /* 终端文本区宽度（不含滚动条） */
static unsigned int *g_live;          /* 滚回时保存的实时屏幕 */
static unsigned int *g_hist;          /* 历史行环形缓冲：LINES * FONT_H * g_cw */
static int g_hist_start, g_hist_count;
static struct mui_scroll g_scroll;
static int g_scrolled;                /* 当前显示历史视图 */
static int g_view_dirty;

static const uint32_t ansi_palette[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

static uint32_t g_fg = 0xCCCCCC;
static uint32_t g_bg = 0x000000;

static int ansi_state = 0;
static int ansi_params[8];
static int ansi_np = 0;
static int ansi_cur = -1;

static void shm_mark_dirty(int x0, int y0, int x1, int y1) {
    if (!g_shm) return;
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_cw) x1 = g_cw;
    if (y1 > g_ch) y1 = g_ch;
    if (x1 <= x0 || y1 <= y0) return;

    uint32_t ux0 = (uint32_t)x0, uy0 = (uint32_t)y0;
    uint32_t ux1 = (uint32_t)x1, uy1 = (uint32_t)y1;

    if (g_shm->cur_x1 <= g_shm->cur_x0 || g_shm->cur_x0 == SHM_DIRTY_EMPTY) {
        g_shm->cur_x0 = ux0; g_shm->cur_y0 = uy0;
        g_shm->cur_x1 = ux1; g_shm->cur_y1 = uy1;
    } else {
        if (ux0 < g_shm->cur_x0) g_shm->cur_x0 = ux0;
        if (uy0 < g_shm->cur_y0) g_shm->cur_y0 = uy0;
        if (ux1 > g_shm->cur_x1) g_shm->cur_x1 = ux1;
        if (uy1 > g_shm->cur_y1) g_shm->cur_y1 = uy1;
    }
}

static void mark_dirty(int x0, int y0, int x1, int y1) {
    if (g_scrolled) return;
    shm_mark_dirty(x0, y0, x1, y1);
}

static void view_mark_dirty(int x0, int y0, int x1, int y1) {
    shm_mark_dirty(x0, y0, x1, y1);
}

static void put_pixel(int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || x >= g_cw || y >= g_ch) return;
    unsigned int *dst = (g_scrolled && g_live) ? g_live : g_pixels;
    dst[y * g_cw + x] = rgb;
}

static void fill_rect(int x, int y, int w, int h, unsigned int rgb) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put_pixel(x + i, y + j, rgb);
    mark_dirty(x, y, x + w, y + h);
}

static void draw_char_at(int px, int py, uint32_t cp,
                         uint32_t fg, uint32_t bg) {
    int width = ufont_width(cp);
    const unsigned char *g = ufont_glyph(cp);
    if (!width) width = CELL_W;

    fill_rect(px, py, width, FONT_H, bg);

    if (!g) return;

    if (width == 8) {
        for (int r = 0; r < FONT_H; r++) {
            unsigned char bits = g[r];
            for (int j = 0; j < 8; j++) {
                if (bits & (0x80 >> j)) put_pixel(px + j, py + r, fg);
            }
        }
    } else {
        for (int r = 0; r < FONT_H; r++) {
            unsigned char hi = g[r * 2];
            unsigned char lo = g[r * 2 + 1];
            for (int j = 0; j < 8; j++) {
                if (hi & (0x80 >> j)) put_pixel(px + j,     py + r, fg);
                if (lo & (0x80 >> j)) put_pixel(px + 8 + j, py + r, fg);
            }
        }
    }
}

static unsigned int *hist_line(int i) {
    int slot = (g_hist_start + i) % TERM_HIST_LINES;
    return g_hist + (size_t)slot * FONT_H * g_cw;
}

static void hist_push_line(unsigned int *src) {
    if (!g_hist || !src) return;
    if (g_hist_count >= TERM_HIST_LINES) {
        g_hist_start = (g_hist_start + 1) % TERM_HIST_LINES;
        g_hist_count = TERM_HIST_LINES - 1;
    }
    int slot = (g_hist_start + g_hist_count) % TERM_HIST_LINES;
    memcpy(g_hist + (size_t)slot * FONT_H * g_cw, src,
           (size_t)FONT_H * g_cw * 4);
    g_hist_count++;
}

static void scroll(void) {
    g_cursor_drawn = 0;

    unsigned int *src = (g_scrolled && g_live) ? g_live : g_pixels;
    int at_bottom = (g_scroll.value >= g_hist_count);

    hist_push_line(src);

    for (int y = 0; y < g_ch - FONT_H; y++) {
        unsigned int *dst = src + y * g_cw;
        unsigned int *s = src + (y + FONT_H) * g_cw;
        memcpy(dst, s, g_cw * 4);
    }
    for (int y = g_ch - FONT_H; y < g_ch; y++) {
        unsigned int *dst = src + y * g_cw;
        for (int x = 0; x < g_cw; x++) dst[x] = g_bg;
    }

    mui_scroll_set_range(&g_scroll, g_hist_count + g_rows, g_rows);
    if (at_bottom)
        g_scroll.value = g_hist_count;
    else if (g_scroll.value > g_hist_count)
        g_scroll.value = g_hist_count;
    g_scroll.base.dirty = 1;

    if (!g_scrolled) mark_dirty(0, 0, g_cw, g_ch);
}

static void newline(void) {
    g_pending_wrap = 0;
    g_cx = 0;
    g_cy++;
    while (g_cy >= g_rows) { scroll(); g_cy--; }
}

static void render_cp(uint32_t cp) {
    if (cp == '\n') { g_pending_wrap = 0; newline(); return; }
    if (cp == '\r') { g_cx = 0; g_pending_wrap = 0; return; }
    if (cp == '\b') {
        g_pending_wrap = 0;
        if (g_cx > 0) {
            g_cx--;
            draw_char_at(g_cx * CELL_W, g_cy * FONT_H, ' ', g_fg, g_bg);
        }
        return;
    }
    if (cp == '\t') {
        g_pending_wrap = 0;
        int target = (g_cx / 8 + 1) * 8;
        while (g_cx < target) {
            if (g_cx >= g_cols) newline();
            draw_char_at(g_cx * CELL_W, g_cy * FONT_H, ' ', g_fg, g_bg);
            g_cx++;
        }
        return;
    }
    if (cp < 0x20) return;

    if (g_pending_wrap) {
        newline();
        g_pending_wrap = 0;
    }

    int width = ufont_width(cp);
    uint32_t to_draw = cp;
    if (!width) {
        if (cp != ' ') {
            to_draw = '?';
            width = ufont_width('?');
        }
        if (!width) width = CELL_W;
    }

    int cells = (width + CELL_W - 1) / CELL_W;
    if (g_cx + cells > g_cols) newline();

    draw_char_at(g_cx * CELL_W, g_cy * FONT_H, to_draw, g_fg, g_bg);
    g_cx += cells;
    if (g_cx >= g_cols) {
        g_cx = g_cols - 1;
        g_pending_wrap = 1;
    }
}

static void cursor_restore(void) {
    if (g_scrolled) { g_cursor_drawn = 0; return; }
    if (!g_cursor_drawn || !g_shm) return;
    int px = g_cursor_drawn_x * CELL_W;
    int py = g_cursor_drawn_y * FONT_H;
    if (py + FONT_H > g_ch) { g_cursor_drawn = 0; return; }

    for (int r = 0; r < FONT_H; r++) {
        for (int c = 0; c < CELL_W; c++) {
            int x = px + c;
            int y = py + r;
            if (x < g_cw && y < g_ch)
                g_pixels[y * g_cw + x] = g_cursor_save[r * CELL_W + c];
        }
    }

    mark_dirty(px, py, px + CELL_W, py + FONT_H);
    g_cursor_drawn = 0;
}

static void cursor_draw(void) {
    if (g_scrolled) return;
    if (!g_cursor_visible || !g_shm) return;

    int px = g_cx * CELL_W;
    int py = g_cy * FONT_H;
    if (py + FONT_H > g_ch) return;

    for (int r = 0; r < FONT_H; r++) {
        for (int c = 0; c < CELL_W; c++) {
            int x = px + c;
            int y = py + r;
            if (x < g_cw && y < g_ch)
                g_cursor_save[r * CELL_W + c] = g_pixels[y * g_cw + x];
        }
    }
    g_cursor_drawn_x = g_cx;
    g_cursor_drawn_y = g_cy;
    g_cursor_drawn = 1;

    for (int i = 0; i < CELL_W; i++) {
        g_pixels[(py + FONT_H - 1) * g_cw + px + i] = 0xFFFFFF;
        g_pixels[(py + FONT_H - 2) * g_cw + px + i] = 0xFFFFFF;
    }

    mark_dirty(px, py + FONT_H - 2, px + CELL_W, py + FONT_H);
}

/* ---------- 滚动条 / 回滚视图 ---------- */

static void term_scroll_on_change(struct mui_scroll *s) {
    (void)s;
    g_view_dirty = 1;
}

static void term_scrollbar_paint(void) {
    if (!g_pixels || !g_shm) return;

    g_scroll.base.x = g_cw - TERM_SCROLL_W;
    g_scroll.base.y = 0;
    g_scroll.base.w = TERM_SCROLL_W;
    g_scroll.base.h = g_ch;
    if (g_scroll.base.w < 4) g_scroll.base.w = 4;
    if (g_scroll.base.h < 4) g_scroll.base.h = 4;
    g_scroll.base.dirty = 1;

    struct mui_surface s;
    memset(&s, 0, sizeof(s));
    s.pixels = g_pixels;
    s.w = g_cw;
    s.h = g_ch;
    s.hdr = g_shm;

    mui_widget_paint(&g_scroll.base, &s, g_scroll.base.x, 0);
    view_mark_dirty(g_scroll.base.x, 0, g_cw, g_ch);
}

static void term_scroll_init(void) {
    mui_scroll_init(&g_scroll, g_cw - TERM_SCROLL_W, 0,
                    TERM_SCROLL_W, g_ch);
    g_scroll.on_change = term_scroll_on_change;
    mui_scroll_set_range(&g_scroll, g_hist_count + g_rows, g_rows);
    g_scroll.value = g_hist_count;
    g_scroll.base.dirty = 1;
}

static void term_compose_view(int v) {
    if (!g_live || !g_pixels) return;

    int hist = g_hist_count;
    if (v < 0) v = 0;
    if (v > hist) v = hist;

    for (int sy = 0; sy < g_rows; sy++) {
        int idx = v + sy;
        unsigned int *src = NULL;
        if (idx < hist) {
            src = hist_line(idx);
        } else if (idx < hist + g_rows) {
            src = g_live + (size_t)(idx - hist) * FONT_H * g_cw;
        }

        unsigned int *dst = g_pixels + (size_t)sy * FONT_H * g_cw;
        if (src) {
            for (int r = 0; r < FONT_H; r++) {
                memcpy(dst + r * g_cw, src + r * g_cw,
                       (size_t)g_text_w * 4);
                for (int x = g_text_w; x < g_cw; x++)
                    dst[r * g_cw + x] = g_bg;
            }
        } else {
            for (int r = 0; r < FONT_H; r++) {
                for (int x = 0; x < g_cw; x++)
                    dst[r * g_cw + x] = g_bg;
            }
        }
    }

    for (int y = g_rows * FONT_H; y < g_ch; y++) {
        unsigned int *dst = g_pixels + y * g_cw;
        for (int x = 0; x < g_cw; x++) dst[x] = g_bg;
    }

    view_mark_dirty(0, 0, g_cw, g_ch);
}

static void term_view_update(void) {
    int hist = g_hist_count;
    int v = mui_scroll_value(&g_scroll);
    if (v < 0) v = 0;
    if (v > hist) v = hist;
    int want = (v < hist);

    if (want && !g_scrolled) {
        if (g_live) {
            cursor_restore();
            memcpy(g_live, g_pixels, (size_t)g_cw * g_ch * 4);
            g_scrolled = 1;
        }
    } else if (!want && g_scrolled) {
        if (g_live)
            memcpy(g_pixels, g_live, (size_t)g_cw * g_ch * 4);
        g_scrolled = 0;
        g_cursor_drawn = 0;
        cursor_draw();
        view_mark_dirty(0, 0, g_cw, g_ch);
    }

    if (g_scrolled) {
        term_compose_view(v);
    } else if (!g_cursor_drawn) {
        cursor_draw();
    }

    term_scrollbar_paint();
    g_view_dirty = 0;
}

static void term_present(void) {
    if (g_scrolled)
        term_view_update();
    else
        term_scrollbar_paint();
}

/* ---------- ANSI ---------- */

static void ansi_reset_params(void) {
    ansi_np = 0;
    ansi_cur = -1;
}

static void ansi_push_param(void) {
    if (ansi_np < 8) {
        ansi_params[ansi_np++] = (ansi_cur >= 0) ? ansi_cur : 0;
    }
    ansi_cur = -1;
}

static void ansi_sgr(void) {
    if (ansi_np == 0) { g_fg = 0xCCCCCC; g_bg = 0x000000; return; }
    for (int i = 0; i < ansi_np; i++) {
        int p = ansi_params[i];
        if (p == 0)                       { g_fg = 0xCCCCCC; g_bg = 0x000000; }
        else if (p == 39)                 { g_fg = 0xCCCCCC; }
        else if (p == 49)                 { g_bg = 0x000000; }
        else if (p == 1)                  { /* bold */ }
        else if (p == 7)                  { uint32_t t = g_fg; g_fg = g_bg; g_bg = t; }
        else if (p >= 30 && p <= 37)      g_fg = ansi_palette[p - 30];
        else if (p >= 40 && p <= 47)      g_bg = ansi_palette[p - 40];
        else if (p >= 90 && p <= 97)      g_fg = ansi_palette[p - 90 + 8];
        else if (p >= 100 && p <= 107)    g_bg = ansi_palette[p - 100 + 8];
    }
}

static void ansi_clear_screen(void) {
    fill_rect(0, 0, g_cw, g_ch, g_bg);
}

static void ansi_erase_line(int mode) {
    int py = g_cy * FONT_H;
    if (py >= g_ch) return;
    if (mode == 0) {
        int px = g_cx * CELL_W;
        fill_rect(px, py, g_cw - px, FONT_H, g_bg);
    } else if (mode == 1) {
        int px = g_cx * CELL_W + CELL_W;
        fill_rect(0, py, px, FONT_H, g_bg);
    } else if (mode == 2) {
        fill_rect(0, py, g_cw, FONT_H, g_bg);
    }
}

static void ansi_set_cursor(int row, int col) {
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    if (row > g_rows) row = g_rows;
    if (col > g_cols) col = g_cols;
    g_cy = row - 1;
    g_cx = col - 1;
    g_pending_wrap = 0;
}

static void ansi_handle_final(char final) {
    if (ansi_cur >= 0) ansi_push_param();

    switch (final) {
        case 'm': ansi_sgr(); break;
        case 'J': {
            int p = (ansi_np > 0) ? ansi_params[0] : 0;
            if (p == 2 || p == 3) ansi_clear_screen();
            break;
        }
        case 'H':
        case 'f': {
            int r = (ansi_np > 0) ? ansi_params[0] : 1;
            int c = (ansi_np > 1) ? ansi_params[1] : 1;
            ansi_set_cursor(r, c);
            break;
        }
        case 'K': {
            int p = (ansi_np > 0) ? ansi_params[0] : 0;
            ansi_erase_line(p);
            break;
        }
        case 'A': {
            int n = (ansi_np > 0) ? ansi_params[0] : 1;
            if (n < 1) n = 1;
            g_cy -= n; if (g_cy < 0) g_cy = 0;
            break;
        }
        case 'B': {
            int n = (ansi_np > 0) ? ansi_params[0] : 1;
            if (n < 1) n = 1;
            g_cy += n; if (g_cy >= g_rows) g_cy = g_rows - 1;
            break;
        }
        case 'C': {
            int n = (ansi_np > 0) ? ansi_params[0] : 1;
            if (n < 1) n = 1;
            g_cx += n; if (g_cx >= g_cols) g_cx = g_cols - 1;
            break;
        }
        case 'D': {
            int n = (ansi_np > 0) ? ansi_params[0] : 1;
            if (n < 1) n = 1;
            g_cx -= n; if (g_cx < 0) g_cx = 0;
            break;
        }
        case 'h':
            if (ansi_np > 0 && ansi_params[0] == 25) g_cursor_visible = 1;
            break;
        case 'l':
            if (ansi_np > 0 && ansi_params[0] == 25) {
                if (g_cursor_drawn) cursor_restore();
                g_cursor_visible = 0;
            }
            break;
        default: break;
    }
}

/* ---------- UTF-8 ---------- */

static unsigned char g_utf8_buf[4];
static int g_utf8_len  = 0;
static int g_utf8_need = 0;

static void process_byte(unsigned char c) {
    if (ansi_state == 1) {
        if (c == '[') { ansi_state = 2; ansi_reset_params(); return; }
        ansi_state = 0;
        return;
    }
    if (ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            if (ansi_cur < 0) ansi_cur = 0;
            ansi_cur = ansi_cur * 10 + (c - '0');
            return;
        }
        if (c == ';') { ansi_push_param(); return; }
        if (c == '?') { return; }
        if (c >= 0x40 && c <= 0x7E) {
            ansi_handle_final((char)c);
            ansi_state = 0;
            return;
        }
        return;
    }
    if (c == 0x1B) { ansi_state = 1; return; }

    if (g_utf8_need > 0) {
        if ((c & 0xC0) != 0x80) {
            render_cp('?');
            g_utf8_need = 0;
            g_utf8_len  = 0;
            process_byte(c);
            return;
        }
        g_utf8_buf[g_utf8_len++] = c;
        if (g_utf8_len < g_utf8_need) return;

        uint32_t cp;
        if      (g_utf8_need == 2) cp = g_utf8_buf[0] & 0x1F;
        else if (g_utf8_need == 3) cp = g_utf8_buf[0] & 0x0F;
        else                       cp = g_utf8_buf[0] & 0x07;
        for (int k = 1; k < g_utf8_need; k++)
            cp = (cp << 6) | (g_utf8_buf[k] & 0x3F);

        render_cp(cp);
        g_utf8_need = 0;
        g_utf8_len  = 0;
        return;
    }

    if (c < 0x80)                       { render_cp(c); return; }
    if ((c & 0xE0) == 0xC0) { g_utf8_need = 2; g_utf8_buf[0] = c; g_utf8_len = 1; }
    else if ((c & 0xF0) == 0xE0) { g_utf8_need = 3; g_utf8_buf[0] = c; g_utf8_len = 1; }
    else if ((c & 0xF8) == 0xF0) { g_utf8_need = 4; g_utf8_buf[0] = c; g_utf8_len = 1; }
    else                         { render_cp('?'); }
}
#define IME_BAR_W 480

static struct ime_ctx g_term_ime;
static uint32_t g_ime_save[IME_BAR_W * FONT_H];
static int g_ime_saved_w;
static int g_ime_visible;
static void ime_restore(void);
static void ime_draw(void);
static int  g_inbox_w = -1;
static char g_my_shm_name[16];
static char g_my_ev_name[24];
static int  g_resize_gen = 0;

static int term_resize(int new_w, int new_h) {
    if (!g_shm || g_inbox_w < 0) return -1;
    if (new_w < 80 || new_h < 48) return -1;
    if (new_w > 2048 || new_h > 1536) return -1;
    g_ime_visible = 0;
    g_ime_saved_w = 0;
    g_resize_gen++;
    char new_shm[16];
    snprintf(new_shm, sizeof(new_shm), "term-%d-r%d",
             (int)g_my_ev_name[8] - '0', g_resize_gen);

    /* 先保存旧尺寸——复制完才更新 g_*，hist_line 依赖 g_cw */
    int old_cw     = g_cw;
    int old_ch     = g_ch;
    int old_text_w = g_text_w;

    int new_text_w = new_w - TERM_SCROLL_W;
    if (new_text_w < CELL_W) new_text_w = CELL_W;

    int copy_w = (old_text_w < new_text_w) ? old_text_w : new_text_w;
    int copy_h = (old_ch < new_h) ? old_ch : new_h;

    size_t new_live_size = (size_t)new_w * (size_t)new_h * 4;
    unsigned int *new_live = (unsigned int*)malloc(new_live_size);
    if (!new_live) return -1;
    for (size_t i = 0; i < new_live_size / 4; i++) new_live[i] = g_bg;

    size_t new_hist_size = (size_t)TERM_HIST_LINES * FONT_H *
                           (size_t)new_w * 4;
    unsigned int *new_hist = (unsigned int*)malloc(new_hist_size);
    if (!new_hist) { free(new_live); return -1; }
    for (size_t i = 0; i < new_hist_size / 4; i++) new_hist[i] = g_bg;

    int new_shm_size = shm_total_size(new_w, new_h);
    int new_id = shm_open(new_shm, new_shm_size, 0);
    if (new_id < 0) { free(new_live); free(new_hist); return -1; }

    unsigned int new_addr = mmap(0, new_shm_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED, new_id, 0);
    if ((int)new_addr <= 0) { free(new_live); free(new_hist); return -1; }

    struct shm_hdr *new_hdr    = (struct shm_hdr*)new_addr;
    unsigned int   *new_pixels = (unsigned int*)(new_addr + SHM_HDR_SIZE);

    if (!g_scrolled) cursor_restore();
    unsigned int *old_src = (g_scrolled && g_live) ? g_live : g_pixels;
    if (old_src) {
        for (int y = 0; y < copy_h; y++) {
            memcpy(new_live + (size_t)y * new_w,
                   old_src  + (size_t)y * old_cw,
                   (size_t)copy_w * 4);
        }
    }

    int old_hist_count = g_hist_count;
    if (g_hist && old_hist_count > 0) {
        for (int i = 0; i < old_hist_count; i++) {
            unsigned int *src = hist_line(i);
            unsigned int *dst = new_hist + (size_t)i * FONT_H * new_w;
            for (int r = 0; r < FONT_H; r++) {
                memcpy(dst + (size_t)r * new_w,
                       src + (size_t)r * old_cw,
                       (size_t)copy_w * 4);
            }
        }
    }

    memcpy(new_pixels, new_live, new_live_size);

    new_hdr->counter = 0;
    new_hdr->cur_x0 = SHM_DIRTY_EMPTY; new_hdr->cur_y0 = 0;
    new_hdr->cur_x1 = 0; new_hdr->cur_y1 = 0;
    new_hdr->pub_x0 = SHM_DIRTY_EMPTY; new_hdr->pub_y0 = 0;
    new_hdr->pub_x1 = 0; new_hdr->pub_y1 = 0;

    struct wm_resize_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC;
    req.type  = WM_REQ_RESIZE;
    req.owner_pid = getpid();
    strncpy(req.old_shm, g_my_shm_name, sizeof(req.old_shm) - 1);
    strncpy(req.new_shm, new_shm,      sizeof(req.new_shm) - 1);
    req.new_w = new_w;
    req.new_h = new_h;

    if (pipe_write(g_inbox_w, &req, sizeof(req)) != sizeof(req)) {
        free(new_live); free(new_hist);
        munmap(new_addr, new_shm_size);
        shm_unlink(new_shm);
        return -1;
    }

    munmap((unsigned int)g_shm, shm_total_size(old_cw, old_ch));
    if (g_live) free(g_live);
    if (g_hist) free(g_hist);

    g_shm        = new_hdr;
    g_pixels     = new_pixels;
    g_live       = new_live;
    g_hist       = new_hist;
    g_hist_start = 0;
    g_hist_count = old_hist_count;
    g_scrolled   = 0;

    g_cw     = new_w;
    g_ch     = new_h;
    g_text_w = new_text_w;
    g_cols   = g_text_w / CELL_W;
    g_rows   = new_h / FONT_H;

    g_cursor_drawn = 0;
    if (g_cx >= g_cols) g_cx = g_cols - 1;
    if (g_cy >= g_rows) g_cy = g_rows - 1;

    term_scroll_init();
    term_scrollbar_paint();
    if (!g_scrolled) cursor_draw();

    g_shm->cur_x0 = 0;
    g_shm->cur_y0 = 0;
    g_shm->cur_x1 = g_cw;
    g_shm->cur_y1 = g_ch;

    strncpy(g_my_shm_name, new_shm, sizeof(g_my_shm_name) - 1);

    pty_set_winsize(g_master, g_cols, g_rows);

    if (g_master >= 0) {
        char note[2] = { 0x1B, 'W' };
        pty_write(g_master, note, 2);
    }

    return 0;
}

static void ime_restore(void) {
    if (!g_ime_visible || !g_ime_saved_w || !g_pixels) return;
    int y = g_ch - FONT_H;
    for (int r = 0; r < FONT_H; r++) {
        unsigned int *dst = g_pixels + (y + r) * g_cw;
        unsigned int *src = g_ime_save + r * g_ime_saved_w;
        memcpy(dst, src, g_ime_saved_w * 4);
    }
    mark_dirty(0, y, g_ime_saved_w, y + FONT_H);
    g_ime_visible = 0;
}

static void ime_draw_text(const char *text, int x, int y, uint32_t fg, uint32_t bg) {
    const unsigned char *p = (const unsigned char*)text;
    int limit = x + g_ime_saved_w - 4;
    while (*p) {
        uint32_t cp; int n = 1;
        if (*p < 0x80) cp = *p;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); n = 4; }
        else { cp = '?'; n = 1; }
        int w = ufont_width(cp);
        if (!w) w = CELL_W;
        if (x + w > limit) break;
        draw_char_at(x, y, cp, fg, bg);
        x += w; p += n;
    }
}

static void ime_draw(void) {
    ime_restore();
    if (!g_term_ime.active || g_scrolled || !g_pixels) return;
    if (!g_term_ime.pinyin_len && !g_term_ime.show_hint) return;

    int w = g_text_w < IME_BAR_W ? g_text_w : IME_BAR_W;
    if (w < 80) return;
    int y = g_ch - FONT_H;
    if (y < 0) return;
    g_ime_saved_w = w;
    for (int r = 0; r < FONT_H; r++)
        memcpy(g_ime_save + r * w, g_pixels + (y + r) * g_cw, w * 4);
    fill_rect(0, y, w, FONT_H, 0x2A2A3E);
    
    const char *hint = ime_get_hint(&g_term_ime);
    if (hint[0]) {
        ime_draw_text(hint, 4, y, 0xFFFFFF, 0x2A2A3E);
    } else {
        ime_draw_text("IME: 拼音输入", 4, y, 0xFFFFFF, 0x2A2A3E);
    }
    
    g_ime_visible = 1;
    mark_dirty(0, y, w, y + FONT_H);
}

/* 把上屏结果通过 PTY 传给终端子进程 */
static void term_ime_commit_cb(const char *utf8, void *userdata) {
    (void)userdata;
    pty_write(g_master, utf8, (unsigned int)strlen(utf8));
}

static int ime_key(int key, unsigned int mods) {
    int handled = ime_process_key(&g_term_ime, key, mods, NULL, term_ime_commit_cb, NULL);
    ime_draw();
    return handled;
}

static int g_tab_gen = 0;

static void term_spawn_tab(void) {
    char shm[16], ev[16], wbuf[8], hbuf[8];
    int gen = ++g_tab_gen;
    snprintf(shm, sizeof(shm), "tt-%d-%d", getpid() & 0xFFFF, gen);
    snprintf(ev, sizeof(ev), "te-%d-%d", getpid() & 0xFFFF, gen);
    int size = shm_total_size(g_cw, g_ch);
    if (shm_open(shm, size, 0) < 0) return;
    pipe_unlink(ev);
    int fds[2];
    if (pipe_named(ev, fds) < 0) return;
    ipc_close(fds[0]); ipc_close(fds[1]);

    struct wm_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC; req.type = WM_REQ_CREATE; req.kind = WM_KIND_NORMAL;
    req.x = 200; req.y = 100; req.w = g_cw; req.h = g_ch;
    req.owner_pid = getpid();
    strncpy(req.shm_name, shm, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name, ev, sizeof(req.ev_name) - 1);
    strncpy(req.title, "Terminal", sizeof(req.title) - 1);
    if (pipe_write(g_inbox_w, &req, sizeof(req)) != sizeof(req)) return;

    snprintf(wbuf, sizeof(wbuf), "%d", g_cw);
    snprintf(hbuf, sizeof(hbuf), "%d", g_ch);
    char *argv[] = { "TERM", shm, wbuf, hbuf, ev, NULL };
    struct spawn_params sp = {
        .size = sizeof(sp), .in_fd = SPAWN_FD_INHERIT,
        .out_fd = SPAWN_FD_INHERIT, .err_fd = SPAWN_FD_INHERIT,
        .in_pipe = -1, .out_pipe = -1, .err_pipe = -1,
        .in_pty = -1, .out_pty = -1, .err_pty = -1,
        .flags = 0, .envp = 0, .out_pid = 0,
    };
    spawn("TERM", argv, &sp);
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    if (argc < 5) {
        dprintf(2, "usage: TERM <shm_name> <w> <h> <event_pipe> [prog [args...]]\n");
        return 1;
    }

    const char *shm_name = argv[1];
    g_cw = atoi(argv[2]);
    g_ch = atoi(argv[3]);
    const char *ev_name = argv[4];
    if (g_cw <= 0 || g_ch <= 0) return 1;

    strncpy(g_my_shm_name, shm_name, sizeof(g_my_shm_name) - 1);
    strncpy(g_my_ev_name, ev_name, sizeof(g_my_ev_name) - 1);

    int ev_rd = pipe_open(ev_name, /*wr=*/0);
    if (ev_rd < 0) {
        dprintf(2, "term: pipe_open('%s') = %d\n", ev_name, ev_rd);
        return 1;
    }

    int shm_size = shm_total_size(g_cw, g_ch);
    int shm_id = shm_open(shm_name, shm_size, 0);
    if (shm_id < 0) { dprintf(2, "term: shm_open %d\n", shm_id); return 1; }

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE, MAP_SHARED, shm_id, 0);
    if ((int)addr <= 0) { dprintf(2, "term: mmap %d\n", (int)addr); return 1; }

    g_shm = (struct shm_hdr*)addr;
    g_pixels = (unsigned int*)(addr + SHM_HDR_SIZE);

    memset(g_pixels, 0, g_cw * g_ch * 4);

    g_shm->counter = 0;
    g_shm->cur_x0 = SHM_DIRTY_EMPTY; g_shm->cur_y0 = 0;
    g_shm->cur_x1 = 0; g_shm->cur_y1 = 0;
    g_shm->pub_x0 = SHM_DIRTY_EMPTY; g_shm->pub_y0 = 0;
    g_shm->pub_x1 = 0; g_shm->pub_y1 = 0;

    g_text_w = g_cw - TERM_SCROLL_W;
    if (g_text_w < CELL_W) g_text_w = CELL_W;
    g_cols = g_text_w / CELL_W;
    g_rows = g_ch / FONT_H;

    size_t live_size = (size_t)g_cw * (size_t)g_ch * 4;
    g_live = (unsigned int*)malloc(live_size);
    if (g_live) memset(g_live, 0, live_size);

    size_t hist_size = (size_t)TERM_HIST_LINES * FONT_H * (size_t)g_cw * 4;
    g_hist = (unsigned int*)malloc(hist_size);
    if (g_hist) memset(g_hist, 0, hist_size);
    g_hist_start = g_hist_count = 0;
    g_scrolled = 0;

    term_scroll_init();
    term_scrollbar_paint();

    g_inbox_w = pipe_open(WM_INBOX_NAME, 1);
    if (g_inbox_w < 0) {
        dprintf(2, "term: wm-inbox open failed\n");
        return 1;
    }

    load_font();
    ime_init(&g_term_ime);
    int fds[2];
    if (pty_open(fds) < 0) return 1;
    g_master = fds[0];
    int slave  = fds[1];

    pty_set_winsize(g_master, g_cols, g_rows);

    static char *shell_envp[] = {
        "PATH=SYS:/",
        "HOME=SYS:/",
        "TERM=helium",
        NULL,
    };

    struct spawn_params sp = {
        .size = sizeof(sp),
        .in_fd = SPAWN_FD_INHERIT,
        .out_fd = SPAWN_FD_INHERIT,
        .err_fd = SPAWN_FD_INHERIT,
        .in_pipe = -1, .out_pipe = -1, .err_pipe = -1,
        .in_pty = slave, .out_pty = slave, .err_pty = slave,
        .flags = 0, .envp = (uint32_t)shell_envp, .out_pid = 0,
    };
    int sh;
    if (argc >= 6) {
        char *cmd_argv[8];
        int cmd_argc = 0;
        for (int i = 5; i < argc && cmd_argc < 7; i++)
            cmd_argv[cmd_argc++] = argv[i];
        cmd_argv[cmd_argc] = NULL;
        sh = spawn(argv[5], cmd_argv, &sp);
    } else {
        sh = spawn("SHELL", NULL, &sp);
    }
    dprintf(2, "term: cols=%d rows=%d ch=%d\n", g_cols, g_rows, g_ch);
    if (sh < 0) { dprintf(2, "term: spawn %s %d\n", argc >= 6 ? argv[5] : "SHELL", sh); return 1; }
    pty_set_fg(g_master, sp.out_pid);

    pty_close(slave);

    for (;;) {
        while (pipe_avail(ev_rd) >= (int)sizeof(struct wm_event)) {
            struct wm_event ev;
            int n = pipe_read(ev_rd, &ev, sizeof(ev));
            if (n != sizeof(ev)) break;

            if (ev.type == WM_EV_KEY) {
                if (g_scrolled) {
                    mui_scroll_end(&g_scroll);
                    term_view_update();
                }
                if (!ime_key((int)ev.code, ev.extra)) {
                    char c = (char)ev.code;
                    pty_write(g_master, &c, 1);
                }
            } else if (ev.type == WM_EV_RESIZE) {
                ime_restore();
                term_resize((int)ev.code, (int)ev.extra);
            } else if (ev.type == WM_EV_NEW_TAB) {
                term_spawn_tab();
            } else if (ev.type == WM_EV_CLOSE_REQUEST) {
                struct wm_destroy_request dr;
                dr.magic = WM_MAGIC;
                dr.type = WM_REQ_DESTROY;
                dr.owner_pid = getpid();
                strncpy(dr.shm_name, g_my_shm_name, sizeof(dr.shm_name) - 1);
                pipe_write(g_inbox_w, &dr, sizeof(dr));
                return 0;
            } else if (ev.type == WM_EV_MOUSE_MOVE) {
                int over = (ev.x >= g_cw - TERM_SCROLL_W);
                if (over || g_scroll.dragging) {
                    if (over && !g_scroll.base.hover) {
                        g_scroll.base.hover = 1;
                        term_view_update();
                    }
                    if (g_scroll.base.on_mouse_move)
                        g_scroll.base.on_mouse_move(&g_scroll.base,
                                                    ev.x - g_scroll.base.x,
                                                    ev.y - g_scroll.base.y);
                    if (g_view_dirty)
                        term_view_update();
                } else if (g_scroll.base.hover) {
                    g_scroll.base.hover = 0;
                    term_view_update();
                }
            } else if (ev.type == WM_EV_MOUSE_LEAVE) {
                if (g_scroll.base.hover && !g_scroll.dragging) {
                    g_scroll.base.hover = 0;
                    term_view_update();
                }
            } else if (ev.type == WM_EV_MOUSE_DOWN &&
                       (ev.code & WM_BTN_LEFT) &&
                       ev.x >= g_cw - TERM_SCROLL_W) {
                g_scroll.base.hover = 1;
                if (g_scroll.base.on_mouse_down)
                    g_scroll.base.on_mouse_down(&g_scroll.base,
                                                ev.x - g_scroll.base.x,
                                                ev.y - g_scroll.base.y, 1);
                term_view_update();
            } else if (ev.type == WM_EV_MOUSE_UP) {
                if (g_scroll.dragging) {
                    if (g_scroll.base.on_mouse_up)
                        g_scroll.base.on_mouse_up(&g_scroll.base,
                                                  ev.x - g_scroll.base.x,
                                                  ev.y - g_scroll.base.y, 1);
                    term_view_update();
                }
            } else if (ev.type == WM_EV_WHEEL) {
                mui_scroll_by(&g_scroll, (int)ev.code);
                term_view_update();
            }
        }

        int av = pty_avail(g_master);
        if (av < 0) {
            while ((av = pty_avail(g_master)) > 0) {
                char buf[256];
                int n = pty_read(g_master, buf, sizeof(buf));
                if (n <= 0) break;
                for (int i = 0; i < n; i++)
                    process_byte((unsigned char)buf[i]);
            }

            ime_restore();
            cursor_restore();
            if (!g_scrolled) cursor_draw();

            if (g_shm &&
                (g_shm->pub_x0 == SHM_DIRTY_EMPTY ||
                 g_shm->pub_x1 <= g_shm->pub_x0) &&
                g_shm->cur_x0 != SHM_DIRTY_EMPTY &&
                g_shm->cur_x1 > g_shm->cur_x0) {
                g_shm->pub_x0 = g_shm->cur_x0;
                g_shm->pub_y0 = g_shm->cur_y0;
                g_shm->pub_x1 = g_shm->cur_x1;
                g_shm->pub_y1 = g_shm->cur_y1;
                g_shm->counter++;
            }

            struct wm_destroy_request dr;
            dr.magic = WM_MAGIC;
            dr.type = WM_REQ_DESTROY;
            dr.owner_pid = getpid();
            strncpy(dr.shm_name, g_my_shm_name, sizeof(dr.shm_name) - 1);
            pipe_write(g_inbox_w, &dr, sizeof(dr));

            return 0;
        }

        if (av > 0) {
            ime_restore();
            cursor_restore();

            char buf[256];
            int n = pty_read(g_master, buf, sizeof(buf));
            if (n > 0) {
                for (int i = 0; i < n; i++)
                    process_byte((unsigned char)buf[i]);
            }

            term_present();
            if (!g_scrolled) cursor_draw();
            ime_draw();
        }

        if (g_shm && (g_shm->pub_x0 == SHM_DIRTY_EMPTY ||
                      g_shm->pub_x1 <= g_shm->pub_x0)) {
            if (g_shm->cur_x0 != SHM_DIRTY_EMPTY &&
                g_shm->cur_x1 > g_shm->cur_x0) {
                g_shm->pub_x0 = g_shm->cur_x0;
                g_shm->pub_y0 = g_shm->cur_y0;
                g_shm->pub_x1 = g_shm->cur_x1;
                g_shm->pub_y1 = g_shm->cur_y1;
                g_shm->counter++;
                g_shm->cur_x0 = SHM_DIRTY_EMPTY;
                g_shm->cur_y0 = 0;
                g_shm->cur_x1 = 0;
                g_shm->cur_y1 = 0;
            }
        }

        sleep_ms(1);
    }

    return 0;
}