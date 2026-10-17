/* term — 终端模拟器（支持完整 unifont.hex）
 *
 * 用法: TERM <shm_name> <client_w> <client_h> <event_pipe>
 *
 * - shm 布局：[8B counter][w*h*4 pixels]
 * - stdin 是 WM 的事件 pipe，读键盘字节 → 写 pty master
 * - 从 pty master 读输出 → 渲染到 shm
 * - 每批渲染后 counter++
 */
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "wm_proto.h"
#include "shm_layout.h"
#include <stdint.h>

#define FONT_H   16         /* 所有字形高度 */
#define CELL_W   8          /* 每列像素宽度（字形可为 8 或 16 宽，占 1 或 2 列） */
#define MAX_CP   0x10000    /* BMP 码点范围 */

/* 字形数据池：unifont 约 1MB，留 2MB 富余 */
#define GLYPH_DATA_MAX (2 * 1024 * 1024)

/* ---------- 字体 ---------- */

static unsigned char **g_glyph_ptr  = NULL;   /* 索引 → 数据池内偏移 */
static unsigned char  *g_glyph_width = NULL;  /* 索引 → 8 或 16（0 表示未加载） */
static unsigned char  *g_glyph_data  = NULL;
static unsigned int    g_glyph_data_used = 0;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void load_font(void) {
    int fd = fs_open("SYS:/UNICODE.HEX", FS_O_RDONLY);
    if (fd < 0) {
        dprintf(2, "term: font open failed %d\n", fd);
        return;
    }

    /* 三个懒分配零页 mmap。指针表 256KB，宽度表 64KB，数据池 2MB。 */
    g_glyph_ptr   = (unsigned char**)malloc(MAX_CP * sizeof(unsigned char*));
    g_glyph_width = (unsigned char*)malloc(MAX_CP);
    g_glyph_data  = (unsigned char*)malloc(GLYPH_DATA_MAX);
    if (!g_glyph_ptr || !g_glyph_width || !g_glyph_data) {
        dprintf(2, "term: font alloc failed\n");
        fs_close(fd);
        return;
    }
    g_glyph_data_used = 0;

    char buf[4096];
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

                /* 解析 "<HEX_CODEPOINT>:<HEX_GLYPH>" */
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
                p++;  /* 跳过 ':' */

                int hl = 0;
                const char *q = p;
                while (hexval(q[0]) >= 0 && hexval(q[1]) >= 0) { hl += 2; q += 2; }

                int width;
                if (hl == 32)      width = 8;
                else if (hl == 64) width = 16;
                else { line_len = 0; skipped++; continue; }

                if (cp >= MAX_CP) { line_len = 0; skipped++; continue; }

                int data_size = hl / 2;   /* 16 或 32 */
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
static unsigned int *g_pixels;
static struct shm_hdr *g_shm = NULL;

/* ---------- ANSI 颜色 / 状态 ---------- */

static const uint32_t ansi_palette[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

static uint32_t g_fg = 0xCCCCCC;
static uint32_t g_bg = 0x000000;

static int ansi_state = 0;        /* 0=普通 1=ESC 2=CSI */
static int ansi_params[8];
static int ansi_np = 0;
static int ansi_cur = -1;         /* 当前累积参数，-1=空 */

static void put_pixel(int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || x >= g_cw || y >= g_ch) return;
    g_pixels[y * g_cw + x] = rgb;
}

static void fill_rect(int x, int y, int w, int h, unsigned int rgb) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put_pixel(x + i, y + j, rgb);
}

/* 在像素坐标处渲染单个码点。宽度从 g_glyph_width 查。 */
static void draw_char_at(int px, int py, uint32_t cp,
                         uint32_t fg, uint32_t bg) {
    int width = 8;
    unsigned char *g = NULL;

    if (g_glyph_ptr && g_glyph_width && cp < MAX_CP) {
        g = g_glyph_ptr[cp];
        if (g && g_glyph_width[cp]) width = g_glyph_width[cp];
    }

    /* 脏区标记——放在 fill_rect 之前，覆盖整个字符格 */
    if (g_shm) {
        uint32_t x0 = px;
        uint32_t y0 = py;
        uint32_t x1 = px + width;
        uint32_t y1 = py + FONT_H;
        if (g_shm->cur_x1 <= g_shm->cur_x0 || g_shm->cur_x0 == SHM_DIRTY_EMPTY) {
            g_shm->cur_x0 = x0;
            g_shm->cur_y0 = y0;
            g_shm->cur_x1 = x1;
            g_shm->cur_y1 = y1;
        } else {
            if (x0 < g_shm->cur_x0) g_shm->cur_x0 = x0;
            if (y0 < g_shm->cur_y0) g_shm->cur_y0 = y0;
            if (x1 > g_shm->cur_x1) g_shm->cur_x1 = x1;
            if (y1 > g_shm->cur_y1) g_shm->cur_y1 = y1;
        }
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

static void scroll(void) {
    for (int y = 0; y < g_ch - FONT_H; y++) {
        unsigned int *dst = g_pixels + y * g_cw;
        unsigned int *src = g_pixels + (y + FONT_H) * g_cw;
        memcpy(dst, src, g_cw * 4);
    }
    fill_rect(0, g_ch - FONT_H, g_cw, FONT_H, g_bg);

    /* 整屏变化 */
    if (g_shm) {
        g_shm->cur_x0 = 0;
        g_shm->cur_y0 = 0;
        g_shm->cur_x1 = g_cw;
        g_shm->cur_y1 = g_ch;
    }
}

static void newline(void) {
    g_cx = 0;
    g_cy++;
    while (g_cy >= g_rows) { scroll(); g_cy--; }
}

/* 渲染一个 Unicode 码点（含控制字符处理、宽度、自动换行、滚动） */
static void render_cp(uint32_t cp) {
    if (cp == '\n') { newline(); return; }
    if (cp == '\r') { g_cx = 0; return; }
    if (cp == '\b') {
        if (g_cx > 0) {
            g_cx--;
            draw_char_at(g_cx * CELL_W, g_cy * FONT_H, ' ', g_fg, g_bg);
        }
        return;
    }
    if (cp == '\t') {
        int target = (g_cx / 8 + 1) * 8;
        while (g_cx < target) {
            if (g_cx >= g_cols) newline();
            draw_char_at(g_cx * CELL_W, g_cy * FONT_H, ' ', g_fg, g_bg);
            g_cx++;
        }
        return;
    }
    if (cp < 0x20) return;

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
    if (g_cx >= g_cols) newline();
}

// ANSI

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
        else if (p == 1)                  { /* bold：暂忽略 */ }
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
        default: break;
    }
}

/* ---------- UTF-8 解码 ---------- */

static unsigned char g_utf8_buf[4];
static int g_utf8_len  = 0;
static int g_utf8_need = 0;

static void process_byte(unsigned char c) {
    /* ANSI 状态机优先 */
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
        if (c == '?') { return; }     /* 私有修饰符，忽略 */
        if (c >= 0x40 && c <= 0x7E) {
            ansi_handle_final((char)c);
            ansi_state = 0;
            return;
        }
        return;
    }
    if (c == 0x1B) { ansi_state = 1; return; }

    /* 普通 UTF-8 解码——原逻辑不变 */
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
 * 自己创建新 shm（新名字），通知 WM 切换。 */
static int term_resize(int new_w, int new_h) {
    if (!g_shm || g_inbox_w < 0) return -1;
    if (new_w < 80 || new_h < 48) return -1;
    if (new_w > 1600 || new_h > 1200) return -1;

    g_resize_gen++;
    char new_shm[16];
    snprintf(new_shm, sizeof(new_shm), "term-%d-r%d",
             (int)g_my_ev_name[8] - '0', g_resize_gen);

    int new_shm_size = shm_total_size(new_w, new_h);
    int new_id = shm_open(new_shm, new_shm_size, 0);
    if (new_id < 0) return -1;

    unsigned int new_addr = mmap(0, new_shm_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED, new_id);
    if ((int)new_addr <= 0) return -1;

    struct shm_hdr *new_hdr = (struct shm_hdr*)new_addr;
    unsigned int *new_pixels = (unsigned int*)(new_addr + SHM_HDR_SIZE);
    memset(new_pixels, 0, new_w * new_h * 4);

    /* 保留旧内容：按 min 尺寸逐行拷贝。
     * 旧像素左上角对齐新像素左上角——shell 提示符在左上，
     * 放大时原内容不动，新区域填背景色。 */
    if (g_pixels) {
        int copy_w = (g_cw < new_w) ? g_cw : new_w;
        int copy_h = (g_ch < new_h) ? g_ch : new_h;
        for (int y = 0; y < copy_h; y++) {
            memcpy(new_pixels + y * new_w,
                   g_pixels   + y * g_cw,
                   copy_w * 4);
        }
    }
    new_hdr->counter = 0;
    new_hdr->cur_x0 = SHM_DIRTY_EMPTY; new_hdr->cur_y0 = 0;
    new_hdr->cur_x1 = 0; new_hdr->cur_y1 = 0;
    new_hdr->pub_x0 = SHM_DIRTY_EMPTY; new_hdr->pub_y0 = 0;
    new_hdr->pub_x1 = 0; new_hdr->pub_y1 = 0;

    /* 发请求给 WM */
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
        return -1;
    }

    /* 切换到新 shm——旧的等 WM unlink 后自然销毁。
     * 这里不主动 munmap——WM 处理请求时 unlink 旧名，旧对象因 WM 侧
     * 引用计数下降 + term 侧引用计数下降 而销毁。
     * 但 term 侧 mmap 一直映射着——需要 munmap。 */
    munmap((unsigned int)g_shm, shm_total_size(g_cw, g_ch));

    g_shm = new_hdr;
    g_pixels = new_pixels;
    g_cw = new_w;
    g_ch = new_h;
    g_cols = new_w / CELL_W;
    g_rows = new_h / FONT_H;
    if (g_cx >= g_cols) g_cx = g_cols - 1;
    if (g_cy >= g_rows) g_cy = g_rows - 1;

    /* 全屏脏区——首帧全绘 */
    g_shm->cur_x0 = 0;
    g_shm->cur_y0 = 0;
    g_shm->cur_x1 = g_cw;
    g_shm->cur_y1 = g_ch;

    strncpy(g_my_shm_name, new_shm, sizeof(g_my_shm_name) - 1);

    return 0;
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    if (argc < 5) {
        dprintf(2, "usage: TERM <shm_name> <w> <h> <event_pipe>\n");
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
                             PROT_READ | PROT_WRITE, MAP_SHARED, shm_id);
    if ((int)addr <= 0) { dprintf(2, "term: mmap %d\n", (int)addr); return 1; }

    g_shm = (struct shm_hdr*)addr;
    g_pixels = (unsigned int*)(addr + SHM_HDR_SIZE);

    memset(g_pixels, 0, g_cw * g_ch * 4);

    /* 初始：pub 空，cur 空，counter=0 */
    g_shm->counter = 0;
    g_shm->cur_x0 = SHM_DIRTY_EMPTY; g_shm->cur_y0 = 0;
    g_shm->cur_x1 = 0; g_shm->cur_y1 = 0;
    g_shm->pub_x0 = SHM_DIRTY_EMPTY; g_shm->pub_y0 = 0;
    g_shm->pub_x1 = 0; g_shm->pub_y1 = 0;

    g_cols = g_cw / CELL_W;
    g_rows = g_ch / FONT_H;

    g_inbox_w = pipe_open(WM_INBOX_NAME, 1);
    if (g_inbox_w < 0) {
        dprintf(2, "term: wm-inbox open failed\n");
        return 1;
    }

    load_font();

    /* pty + spawn shell */
    int fds[2];
    if (pty_open(fds) < 0) return 1;
    int master = fds[0];
    int slave  = fds[1];

    struct spawn_params sp = {
        .size = sizeof(sp),
        .in_fd = SPAWN_FD_INHERIT,
        .out_fd = SPAWN_FD_INHERIT,
        .err_fd = SPAWN_FD_INHERIT,
        .in_pipe = -1, .out_pipe = -1, .err_pipe = -1,
        .in_pty = slave, .out_pty = slave, .err_pty = slave,
        .flags = 0, .envp = 0, .out_pid = 0,
    };
    int sh = spawn("SHELL", NULL, &sp);
    if (sh < 0) { dprintf(2, "term: spawn SHELL %d\n", sh); return 1; }
    pty_set_fg(master, sp.out_pid);

    for (;;) {
        if (pipe_avail(ev_rd) > 0) {
            char kbuf[64];
            int kn = pipe_read(ev_rd, kbuf, sizeof(kbuf));
            if (kn > 0) pty_write(master, kbuf, kn);
        }

        if (pty_avail(master) > 0) {
            char buf[256];
            int n = pty_read(master, buf, sizeof(buf));
            if (n > 0) {
                for (int i = 0; i < n; i++)
                    process_byte((unsigned char)buf[i]);
            }
        }

        /* 每轮都检查：pub 空 + cur 非空 → 发布。
         * 不依赖 had_output——避免"上一轮 cur 累积但 pub 被占用，
         * 这一轮 pty 空，永远不发布"的尾数据丢失。 */
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