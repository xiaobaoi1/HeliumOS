/* term — 终端模拟器（支持完整 unifont.hex）
 *
 * 用法: TERM <shm_name> <client_w> <client_h> <event_pipe>
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
#include <stdint.h>

static int g_master = -1;

#define FONT_H   16
#define CELL_W   8
#define MAX_CP   0x10000
#define GLYPH_DATA_MAX (2 * 1024 * 1024)

/* ---------- 字体 ---------- */

static unsigned char **g_glyph_ptr  = NULL;
static unsigned char  *g_glyph_width = NULL;
static unsigned char  *g_glyph_data  = NULL;
static unsigned int    g_glyph_data_used = 0;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void load_font(void) {
    int fd = fs_open("SYS:/UNIFONT.HEX", FS_O_RDONLY);
    if (fd < 0) {
        dprintf(2, "term: font open failed %d\n", fd);
        return;
    }

    g_glyph_ptr   = (unsigned char**)malloc(MAX_CP * sizeof(unsigned char*));
    g_glyph_width = (unsigned char*)malloc(MAX_CP);
    g_glyph_data  = (unsigned char*)malloc(GLYPH_DATA_MAX);
    if (!g_glyph_ptr || !g_glyph_width || !g_glyph_data) {
        dprintf(2, "term: font alloc failed\n");
        fs_close(fd);
        return;
    }
    g_glyph_data_used = 0;

    static char buf[65536];   /* 64KB，跨越多个簇，触发合并读 */
    char line[128];
    int line_len = 0;
    int loaded = 0, skipped = 0;

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
                    cp = cp * 16 + v;
                    p++;
                }
                if (!ok || *p != ':') { line_len = 0; continue; }
                p++;

                int hl = 0;
                const char *q = p;
                while (hexval(q[0]) >= 0 && hexval(q[1]) >= 0) { hl += 2; q += 2; }

                int width;
                if (hl == 32)      width = 8;
                else if (hl == 64) width = 16;
                else { line_len = 0; skipped++; continue; }

                if (cp >= MAX_CP) { line_len = 0; skipped++; continue; }

                int data_size = hl / 2;
                if (g_glyph_data_used + data_size > GLYPH_DATA_MAX) {
                    line_len = 0; skipped++; continue;
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
                    g_glyph_ptr[cp]   = dst;
                    g_glyph_width[cp] = (unsigned char)width;
                    g_glyph_data_used += data_size;
                    loaded++;
                } else {
                    skipped++;
                }
                line_len = 0;
            } else if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }

    fs_close(fd);
    dprintf(2, "term: font loaded %d glyphs (%u bytes), skipped %d\n",
            loaded, g_glyph_data_used, skipped);
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

/* 滚回历史时，实时屏幕的绘制不可见，不标记 shm 脏区 */
static void mark_dirty(int x0, int y0, int x1, int y1) {
    if (g_scrolled) return;
    shm_mark_dirty(x0, y0, x1, y1);
}

/* 历史视图 / 滚动条本身的绘制需要强制标记 */
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
    int width = 8;
    unsigned char *g = NULL;

    if (g_glyph_ptr && g_glyph_width && cp < MAX_CP) {
        g = g_glyph_ptr[cp];
        if (g && g_glyph_width[cp]) width = g_glyph_width[cp];
    }

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

    /* 即将滚出的顶部一行进入历史 */
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
    if (cp == '\n') {
        g_pending_wrap = 0;
        newline();
        return;
    }
    if (cp == '\r') {
        g_cx = 0;
        g_pending_wrap = 0;
        return;
    }
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

    int width = 8;
    uint32_t to_draw = cp;
    if (g_glyph_ptr && g_glyph_width) {
        if (cp < MAX_CP && g_glyph_width[cp] != 0) {
            width = g_glyph_width[cp];
        } else if (cp != ' ') {
            to_draw = '?';
            if (g_glyph_width['?'] != 0) width = g_glyph_width['?'];
        }
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

static int  g_inbox_w = -1;
static char g_my_shm_name[16];
static char g_my_ev_name[24];
static int  g_resize_gen = 0;

/* 请求 WM 将自己的窗口 resize 到 (new_w, new_h) 客户区。
 * 自己创建新 shm（新名字），通知 WM 切换。
 *
 * 两个调用路径共用：
 *   1. 应用主动：term_request_resize(w, h)
 *   2. WM 主动（最大化）：收到 WM_EV_RESIZE 后调用
 */
static int term_resize(int new_w, int new_h) {
    if (!g_shm || g_inbox_w < 0) return -1;
    if (new_w < 80 || new_h < 48) return -1;
    if (new_w > 2048 || new_h > 1536) return -1;

    g_resize_gen++;
    char new_shm[16];
    snprintf(new_shm, sizeof(new_shm), "term-%d-r%d",
             (int)g_my_ev_name[8] - '0', g_resize_gen);

    size_t new_live_size = (size_t)new_w * (size_t)new_h * 4;
    unsigned int *new_live = (unsigned int*)malloc(new_live_size);
    if (new_live) memset(new_live, 0, new_live_size);

    /* 回滚缓冲是可选优化：分配失败不应阻止 canvas 跟随窗口 resize。 */
    size_t new_hist_size = (size_t)TERM_HIST_LINES * FONT_H *
                           (size_t)new_w * 4;
    unsigned int *new_hist = (unsigned int*)malloc(new_hist_size);
    if (new_hist) memset(new_hist, 0, new_hist_size);

    int new_shm_size = shm_total_size(new_w, new_h);
    int new_id = shm_open(new_shm, new_shm_size, 0);
    if (new_id < 0) { free(new_live); free(new_hist); return -1; }

    unsigned int new_addr = mmap(0, new_shm_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED, new_id, 0);
    if ((int)new_addr <= 0) { free(new_live); free(new_hist); return -1; }

    struct shm_hdr *new_hdr = (struct shm_hdr*)new_addr;
    unsigned int *new_pixels = (unsigned int*)(new_addr + SHM_HDR_SIZE);

    /* 保留旧内容：按 min 尺寸逐行拷贝，左上角对齐。
     * 滚回历史时实时屏幕在 g_live；否则在 shm 中（先擦掉光标）。 */
    if (!g_scrolled) cursor_restore();
    unsigned int *old_src = (g_scrolled && g_live) ? g_live : g_pixels;

    unsigned int *live_dst = new_live ? new_live : new_pixels;
    for (size_t i = 0; i < new_live_size / 4; i++)
        live_dst[i] = g_bg;

    if (old_src) {
        /* 只复制旧文本区，不要复制旧的滚动条列——
         * 否则 resize 后窗口中间会留下一条旧滚动条。 */
        int copy_w = (g_text_w < new_w) ? g_text_w : new_w;
        int copy_h = (g_ch < new_h) ? g_ch : new_h;
        for (int y = 0; y < copy_h; y++) {
            memcpy(live_dst + y * new_w,
                   old_src + y * g_cw,
                   (size_t)copy_w * 4);
        }
    }
    if (new_live)
        memcpy(new_pixels, new_live, new_live_size);

    new_hdr->counter = 0;
    new_hdr->cur_x0 = SHM_DIRTY_EMPTY; new_hdr->cur_y0 = 0;
    new_hdr->cur_x1 = 0; new_hdr->cur_y1 = 0;
    new_hdr->pub_x0 = SHM_DIRTY_EMPTY; new_hdr->pub_y0 = 0;
    new_hdr->pub_x1 = 0; new_hdr->pub_y1 = 0;

    /* 通知 WM 切换 shm */
    struct wm_resize_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC;
    req.type = WM_REQ_RESIZE;
    req.owner_pid = getpid();
    strncpy(req.old_shm, g_my_shm_name, sizeof(req.old_shm) - 1);
    strncpy(req.new_shm, new_shm, sizeof(req.new_shm) - 1);
    req.new_w = new_w;
    req.new_h = new_h;

    if (pipe_write(g_inbox_w, &req, sizeof(req)) != sizeof(req)) {
        free(new_live);
        free(new_hist);
        return -1;
    }

    /* 切换 shm */
    munmap((unsigned int)g_shm, shm_total_size(g_cw, g_ch));

    if (g_live) free(g_live);
    if (g_hist) free(g_hist);

    g_shm = new_hdr;
    g_pixels = new_pixels;
    g_live = new_live;
    g_hist = new_hist;
    g_hist_start = g_hist_count = 0;
    g_scrolled = 0;

    g_cw = new_w;
    g_ch = new_h;
    g_text_w = new_w - TERM_SCROLL_W;
    if (g_text_w < CELL_W) g_text_w = CELL_W;
    g_cols = g_text_w / CELL_W;
    g_rows = new_h / FONT_H;
    g_cursor_drawn = 0;
    if (g_cx >= g_cols) g_cx = g_cols - 1;
    if (g_cy >= g_rows) g_cy = g_rows - 1;

    term_scroll_init();
    term_scrollbar_paint();
    if (!g_scrolled) cursor_draw();

    /* 全屏脏区——首帧全绘 */
    g_shm->cur_x0 = 0;
    g_shm->cur_y0 = 0;
    g_shm->cur_x1 = g_cw;
    g_shm->cur_y1 = g_ch;

    strncpy(g_my_shm_name, new_shm, sizeof(g_my_shm_name) - 1);

    dprintf(2, "term: resize to cols=%d rows=%d ch=%d\n",
            g_cols, g_rows, g_ch);
    pty_set_winsize(g_master, g_cols, g_rows);

    /* 通知前台应用窗口尺寸已变化：kilo 识别 ESC W 后重新查询尺寸并重绘。
     * cooked 模式下这个序列会被行规程过滤掉，对 shell 无影响。 */
    if (g_master >= 0) {
        char note[2] = { 0x1B, 'W' };
        pty_write(g_master, note, 2);
    }

    return 0;
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

    /* pty + spawn shell */
    int fds[2];
    if (pty_open(fds) < 0) return 1;
    g_master = fds[0];
    int slave  = fds[1];

    /* 先设好窗口大小再起子进程：kilo 启动时立刻 ioctl(TIOCGWINSZ)，
     * 此时若还是 0x0 会退化成 DSR 查询（终端不响应）而启动失败。 */
    pty_set_winsize(g_master, g_cols, g_rows);

    struct spawn_params sp = {
        .size = sizeof(sp),
        .in_fd = SPAWN_FD_INHERIT,
        .out_fd = SPAWN_FD_INHERIT,
        .err_fd = SPAWN_FD_INHERIT,
        .in_pipe = -1, .out_pipe = -1, .err_pipe = -1,
        .in_pty = slave, .out_pty = slave, .err_pty = slave,
        .flags = 0, .envp = 0, .out_pid = 0,
    };
    /* 默认在 pty 上起交互 shell；若带第 6 个及以后的参数，
     * 则直接运行该命令 —— 桌面用 "TERM ... KILO SYS:\kmain.c"
     * 就能在终端里打开 kilo。 */
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

    for (;;) {
        while (pipe_avail(ev_rd) >= (int)sizeof(struct wm_event)) {
            struct wm_event ev;
            int n = pipe_read(ev_rd, &ev, sizeof(ev));
            if (n != sizeof(ev)) break;

            if (ev.type == WM_EV_KEY) {
                /* 滚回历史时按任意键回到实时屏幕（与常见终端一致） */
                if (g_scrolled) {
                    mui_scroll_end(&g_scroll);
                    term_view_update();
                }
                char c = (char)ev.code;
                pty_write(g_master, &c, 1);
            } else if (ev.type == WM_EV_RESIZE) {
                /* WM 主动 resize（最大化 / 还原）——重建 shm 并通知 WM 切换 */
                term_resize((int)ev.code, (int)ev.extra);
            } else if (ev.type == WM_EV_CLOSE_REQUEST) {
                /* 用户点了关闭按钮——主动销毁窗口，然后退出 */
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
                mui_scroll_by(&g_scroll, -(int)ev.code);
                term_view_update();
            }
            /* 鼠标 / 焦点事件忽略 */
        }

        if (pty_avail(g_master) > 0) {
            cursor_restore();

            char buf[256];
            int n = pty_read(g_master, buf, sizeof(buf));
            if (n > 0) {
                for (int i = 0; i < n; i++)
                    process_byte((unsigned char)buf[i]);
            }

            term_present();
            if (!g_scrolled) cursor_draw();
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