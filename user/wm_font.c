#include "syscall.h"
#include "wm_font.h"
#include <stdint.h>

static unsigned char g_glyph[128][16];
static int g_ok = 0;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void wm_font_load(void) {
    int fd = fs_open("SYS:/UNIFONT.HEX", FS_O_RDONLY);
    if (fd < 0) return;

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

                if (cp >= 128) goto done;

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
                    }
                }
                line_len = 0;
            } else if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }
    done:
    fs_close(fd);
    g_ok = (loaded > 0);
}

int wm_font_loaded(void) { return g_ok; }

const uint8_t *wm_font_glyph(int cp) {
    if (!g_ok || cp < 0 || cp >= 128) return 0;
    return g_glyph[cp];
}