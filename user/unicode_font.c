#include "unicode_font.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include <stdint.h>

#define UFONT_MAX_CP   0x10000
#define UFONT_DATA_MAX (2 * 1024 * 1024)

static uint8_t **g_ptr;
static uint8_t  *g_width;
static uint8_t  *g_data;
static uint32_t  g_used;
static int       g_ok;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ufont_load(void) {
    if (g_ok) return 0;
    int fd = fs_open("SYS:/UNIFONT-17.HEX", FS_O_RDONLY);
    if (fd < 0) return -1;
    g_ptr = (uint8_t**)malloc(UFONT_MAX_CP * sizeof(uint8_t*));
    g_width = (uint8_t*)malloc(UFONT_MAX_CP);
    g_data = (uint8_t*)malloc(UFONT_DATA_MAX);
    if (!g_ptr || !g_width || !g_data) {
        if (g_ptr) free(g_ptr);
        if (g_width) free(g_width);
        if (g_data) free(g_data);
        fs_close(fd);
        return -1;
    }
    memset(g_ptr, 0, UFONT_MAX_CP * sizeof(uint8_t*));
    memset(g_width, 0, UFONT_MAX_CP);
    g_used = 0;

    static char buf[8192];
    char line[160];
    int line_len = 0, loaded = 0;
    for (;;) {
        int n = fs_read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c != '\n' && c != '\r') {
                if (line_len < (int)sizeof(line) - 1) line[line_len++] = c;
                continue;
            }
            if (!line_len) continue;
            line[line_len] = '\0';
            const char *p = line;
            uint32_t cp = 0;
            int ok = 1;
            while (*p && *p != ':') {
                int v = hexval(*p++);
                if (v < 0) { ok = 0; break; }
                cp = cp * 16 + (uint32_t)v;
            }
            if (!ok || *p != ':' || cp >= UFONT_MAX_CP) { line_len = 0; continue; }
            p++;
            int hl = 0;
            const char *q = p;
            while (hexval(q[0]) >= 0 && hexval(q[1]) >= 0) { hl += 2; q += 2; }
            int width = (hl == 32) ? 8 : (hl == 64) ? 16 : 0;
            int bytes = hl / 2;
            if (!width || g_used + (uint32_t)bytes > UFONT_DATA_MAX) {
                line_len = 0; continue;
            }
            uint8_t *dst = g_data + g_used;
            int decode_ok = 1;
            for (int k = 0; k < bytes; k++) {
                int hi = hexval(p[0]), lo = hexval(p[1]);
                if (hi < 0 || lo < 0) { decode_ok = 0; break; }
                dst[k] = (uint8_t)((hi << 4) | lo);
                p += 2;
            }
            if (decode_ok) {
                g_ptr[cp] = dst;
                g_width[cp] = (uint8_t)width;
                g_used += (uint32_t)bytes;
                loaded++;
            }
            line_len = 0;
        }
    }
    fs_close(fd);
    g_ok = loaded > 0;
    return g_ok ? 0 : -1;
}

int ufont_loaded(void) { return g_ok; }

const uint8_t *ufont_glyph(uint32_t cp) {
    if (!g_ok || cp >= UFONT_MAX_CP) return NULL;
    return g_ptr[cp];
}

int ufont_width(uint32_t cp) {
    if (!g_ok || cp >= UFONT_MAX_CP) return 0;
    return g_width[cp];
}

int ufont_utf8_decode(const unsigned char *s, uint32_t *cp) {
    if (!s || !*s) { if (cp) *cp = 0; return 0; }
    uint8_t c = s[0];
    if (c < 0x80) { if (cp) *cp = c; return 1; }
    int n = (c & 0xE0) == 0xC0 ? 2 :
            (c & 0xF0) == 0xE0 ? 3 :
            (c & 0xF8) == 0xF0 ? 4 : 1;
    uint32_t v = n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { if (cp) *cp = '?'; return 1; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    if (cp) *cp = v;
    return n;
}
