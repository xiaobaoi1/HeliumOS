#include <font.h>
#include <fat32.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>

static struct font g_font;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void font_init(struct fat32_volume *vol) {
    memset(&g_font, 0, sizeof(g_font));

    if (!vol) { kprintf("[FONT] no volume\n"); return; }

    struct fat32_file file;
    if (fat32_open_file(vol, "/UNICODE.HEX", &file) != OK) {
        kprintf("[FONT] cannot open /UNICODE.HEX\n");
        return;
    }

    uint32_t fsize = file.file_size;
    if (fsize == 0 || fsize > 64 * 1024) {
        kprintf("[FONT] bad size %u\n", fsize);
        return;
    }

    uint8_t buf[512];
    char line[128];
    int line_len = 0;
    int loaded = 0;
    uint32_t off = 0;

    while (off < fsize) {
        uint32_t chunk = fsize - off;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        int r = fat32_read_file(vol, &file, buf, off, chunk);
        if (r <= 0) break;
        off += r;

        for (int i = 0; i < r; i++) {
            char c = (char)buf[i];
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

                    /* 探测 hex 长度 */
                    int hexlen = 0;
                    const char *q = p;
                    while (hexval(q[0]) >= 0 && hexval(q[1]) >= 0) {
                        hexlen += 2;
                        q += 2;
                    }

                    int nbytes = hexlen / 2;
                    if (nbytes != 16 && nbytes != 32) {
                        line_len = 0;
                        continue;   /* 不支持的长度 */
                    }

                    uint8_t raw[32];
                    int ok = 1;
                    for (int i = 0; i < nbytes; i++) {
                        int hi = hexval(p[0]);
                        int lo = hexval(p[1]);
                        if (hi < 0 || lo < 0) { ok = 0; break; }
                        raw[i] = (uint8_t)((hi << 4) | lo);
                        p += 2;
                    }

                    if (ok) {
                        g_font.width[cp]  = (nbytes == 16) ? 8 : 16;
                        g_font.height[cp] = 16;
                        memcpy(g_font.glyph[cp], raw, nbytes);
                        loaded++;
                    }
                }
                line_len = 0;
            } else if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }

    g_font.valid = (loaded > 0) ? 1 : 0;
    kprintf("[FONT] loaded %d glyphs\n", loaded);
}

const struct font *font_get(void) { return &g_font; }

const uint8_t *font_glyph(uint8_t c, int *out_w) {
    if (!g_font.valid) return NULL;
    if (c >= 128 || g_font.width[c] == 0) {
        /* fallback：'?' */
        c = '?';
        if (g_font.width[c] == 0) return NULL;
    }
    if (out_w) *out_w = g_font.width[c];
    return g_font.glyph[c];
}