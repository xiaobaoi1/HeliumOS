/* term — 终端模拟器
 *
 * 用法: TERM <shm_name> <client_w> <client_h>
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
#include <stdint.h>

#define FONT_W 8
#define FONT_H 16

/* ---------- 字体 ---------- */

static unsigned char g_glyph[128][16];
static int g_font_ok = 0;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void load_font(void) {
    int fd = fs_open("SYS:/UNICODE.HEX", FS_O_RDONLY);
    if (fd < 0) { dprintf(2, "term: font open failed %d\n", fd); return; }

    char buf[512], line[128];
    int line_len = 0, loaded = 0;

    for (;;) {
        int n = fs_read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (line_len == 0) continue;
                line[line_len] = '\0';
                const char *p = line;
                int cp = 0;
                while (*p && *p != ':') {
                    int v = hexval(*p);
                    if (v < 0) break;
                    cp = cp * 16 + v;
                    p++;
                }
                if (*p == ':' && cp >= 0 && cp < 128) {
                    p++;
                    int hl = 0;
                    const char *q = p;
                    while (hexval(q[0]) >= 0 && hexval(q[1]) >= 0) { hl += 2; q += 2; }
                    if (hl == 32) {
                        for (int k = 0; k < 16; k++) {
                            g_glyph[cp][k] = (hexval(p[0]) << 4) | hexval(p[1]);
                            p += 2;
                        }
                        loaded++;
                    } else if (hl == 64) {
                        for (int k = 0; k < 16; k++) {
                            g_glyph[cp][k] = hexval(p[0]) << 4;
                            p += 4;
                        }
                        loaded++;
                    }
                }
                line_len = 0;
            } else if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }
    fs_close(fd);
    g_font_ok = (loaded > 0);
    dprintf(2, "term: font %d glyphs\n", loaded);
}

/* ---------- 渲染 ---------- */

#define COL_BG 0x000000
#define COL_FG 0xCCCCCC

static int g_cw, g_ch;
static unsigned int *g_pixels;

static void put_pixel(int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || x >= g_cw || y >= g_ch) return;
    g_pixels[y * g_cw + x] = rgb;
}

static void fill_rect(int x, int y, int w, int h, unsigned int rgb) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put_pixel(x + i, y + j, rgb);
}

static void draw_char(int col, int row, char c) {
    int px = col * FONT_W;
    int py = row * FONT_H;

    fill_rect(px, py, FONT_W, FONT_H, COL_BG);

    if (!g_font_ok) return;
    if ((unsigned char)c >= 128) c = '?';

    const unsigned char *g = g_glyph[(unsigned char)c];
    for (int r = 0; r < FONT_H; r++) {
        unsigned char bits = g[r];
        for (int j = 0; j < FONT_W; j++) {
            if (bits & (0x80 >> j)) put_pixel(px + j, py + r, COL_FG);
        }
    }
}

static void scroll(void) {
    for (int y = 0; y < g_ch - FONT_H; y++) {
        unsigned int *dst = g_pixels + y * g_cw;
        unsigned int *src = g_pixels + (y + FONT_H) * g_cw;
        memcpy(dst, src, g_cw * 4);
    }
    fill_rect(0, g_ch - FONT_H, g_cw, FONT_H, COL_BG);
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    if (argc < 4) {
        dprintf(2, "usage: TERM <shm_name> <w> <h>\n");
        return 1;
    }

    const char *shm_name = argv[1];
    g_cw = atoi(argv[2]);
    g_ch = atoi(argv[3]);
    if (g_cw <= 0 || g_ch <= 0) return 1;

    /* mmap shm */
    int shm_size = 8 + g_cw * g_ch * 4;
    int shm_id = shm_open(shm_name, shm_size, 0);
    if (shm_id < 0) { dprintf(2, "term: shm_open %d\n", shm_id); return 1; }

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id);
    if ((int)addr <= 0) { dprintf(2, "term: mmap %d\n", (int)addr); return 1; }

    uint64_t *counter = (uint64_t*)addr;
    g_pixels = (unsigned int*)(addr + 8);

    /* 初始化 shm */
    memset(g_pixels, 0, g_cw * g_ch * 4);
    *counter = 1;

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

    /* VT 状态 */
    int cols = g_cw / FONT_W;
    int rows = g_ch / FONT_H;
    int cx = 0, cy = 0;

        for (;;) {
        int dirty = 0;

        /* 1. 只在 stdin 有数据时读——避免阻塞 */
        if (fd_avail(0) > 0) {
            char kbuf[64];
            int kn = read(0, kbuf, sizeof(kbuf));
            if (kn > 0) pty_write(master, kbuf, kn);
        }

        /* 2. pty master → 渲染（每轮最多读一次）*/
        if (pty_avail(master) > 0) {
            char buf[256];
            int n = pty_read(master, buf, sizeof(buf));
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    char c = buf[i];
                    if (c == '\n') { cx = 0; cy++; }
                    else if (c == '\r') { cx = 0; }
                    else if (c == '\b') {
                        if (cx > 0) { cx--; draw_char(cx, cy, ' '); }
                    } else if ((unsigned char)c >= 0x20) {
                        if (cx < cols) draw_char(cx, cy, c);
                        cx++;
                        if (cx >= cols) { cx = 0; cy++; }
                    }
                    while (cy >= rows) { scroll(); cy--; }
                }
                dirty = 1;
            }
        }

        if (dirty) (*counter)++;
        sleep_ms(1);
    }

    return 0;
}