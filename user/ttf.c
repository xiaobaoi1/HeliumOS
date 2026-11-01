#include "ttf.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include <stdint.h>

#define TTF_MAX_BYTES       (512u * 1024u)
#define TTF_MAX_CONTOURS    64
#define TTF_MAX_POINTS      1024
#define TTF_MAX_EXPANDED    2048
#define TTF_MAX_EDGES       8192
#define TTF_CURVE_STEPS     6

#define TAG(a,b,c,d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | \
                      (uint32_t)(c) << 8 | (uint32_t)(d))

struct ttf_font {
    uint8_t *data;
    uint32_t size;
    const uint8_t *cmap4;
    uint32_t cmap4_len;
    const uint8_t *head;
    const uint8_t *hhea;
    const uint8_t *maxp;
    const uint8_t *hmtx;
    uint32_t hmtx_len;
    const uint8_t *loca;
    uint32_t loca_len;
    const uint8_t *glyf;
    uint32_t glyf_len;
    uint16_t num_glyphs;
    uint16_t num_hmetrics;
    int16_t loca_format;
    int16_t ascent;
    int16_t descent;
};

static struct ttf_font g_font;

struct ttf_edge {
    float x0, y0, x1, y1;
    int dir;
};

static struct ttf_edge g_edges[TTF_MAX_EDGES];
static int g_edge_count;
static float g_scale, g_baseline, g_xoff;

static int16_t g_end_pts[TTF_MAX_CONTOURS];
static uint8_t g_flags[TTF_MAX_POINTS];
static int32_t g_xs[TTF_MAX_POINTS], g_ys[TTF_MAX_POINTS];
static int32_t g_ex[TTF_MAX_EXPANDED], g_ey[TTF_MAX_EXPANDED];
static uint8_t g_eon[TTF_MAX_EXPANDED];
static float g_hits[TTF_MAX_EDGES];
static int g_hit_dirs[TTF_MAX_EDGES];

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}
static int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static const uint8_t *find_table(const uint8_t *d, uint16_t n,
                                 uint32_t tag, uint32_t *out_len) {
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *e = d + 12 + i * 16;
        if (rd32(e) != tag) continue;
        uint32_t off = rd32(e + 8), len = rd32(e + 12);
        if (off > g_font.size || len > g_font.size - off) return NULL;
        *out_len = len;
        return d + off;
    }
    return NULL;
}

static const uint8_t *find_cmap4(const uint8_t *cmap, uint32_t len) {
    if (len < 4) return NULL;
    uint16_t n = rd16(cmap + 2);
    const uint8_t *best = NULL;
    uint32_t best_len = 0;
    int best_score = -1;
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *r = cmap + 4 + i * 8;
        if ((uint32_t)(r - cmap) + 8 > len) break;
        uint16_t platform = rd16(r), enc = rd16(r + 2);
        uint32_t off = rd32(r + 4);
        if (off + 4 > len) continue;
        const uint8_t *sub = cmap + off;
        if (rd16(sub) != 4) continue;
        uint32_t slen = rd16(sub + 2);
        if (slen < 16 || off + slen > len) continue;
        int score = (platform == 3 && enc == 1) ? 3 :
                    (platform == 0) ? 2 : 1;
        if (score > best_score) {
            best_score = score; best = sub; best_len = slen;
        }
    }
    if (best) g_font.cmap4_len = best_len;
    return best;
}

int ttf_load(const char *path, int pixel_height) {
    (void)pixel_height;
    memset(&g_font, 0, sizeof(g_font));
    int fd = fs_open(path, FS_O_RDONLY);
    if (fd < 0) return -1;

    uint8_t *data = (uint8_t*)malloc(TTF_MAX_BYTES);
    if (!data) { fs_close(fd); return -1; }
    uint32_t size = 0;
    for (;;) {
        if (size >= TTF_MAX_BYTES) { fs_close(fd); free(data); return -1; }
        int n = fs_read(fd, data + size, TTF_MAX_BYTES - size);
        if (n < 0) { fs_close(fd); free(data); return -1; }
        if (n == 0) break;
        size += (uint32_t)n;
    }
    fs_close(fd);
    if (size < 12) { free(data); return -1; }

    uint32_t version = rd32(data);
    if (version != 0x00010000u && version != TAG('t','r','u','e')) {
        free(data); return -1;
    }
    uint16_t ntab = rd16(data + 4);
    if (ntab == 0 || 12u + (uint32_t)ntab * 16u > size) {
        free(data); return -1;
    }
    g_font.data = data;
    g_font.size = size;

    uint32_t len;
    g_font.head = find_table(data, ntab, TAG('h','e','a','d'), &len);
    if (!g_font.head || len < 54) goto fail;
    g_font.hhea = find_table(data, ntab, TAG('h','h','e','a'), &len);
    if (!g_font.hhea || len < 36) goto fail;
    g_font.maxp = find_table(data, ntab, TAG('m','a','x','p'), &len);
    if (!g_font.maxp || len < 6) goto fail;
    const uint8_t *cmap = find_table(data, ntab, TAG('c','m','a','p'), &len);
    if (!cmap) goto fail;
    g_font.cmap4 = find_cmap4(cmap, len);
    if (!g_font.cmap4) goto fail;
    g_font.hmtx = find_table(data, ntab, TAG('h','m','t','x'), &g_font.hmtx_len);
    if (!g_font.hmtx) goto fail;
    g_font.loca = find_table(data, ntab, TAG('l','o','c','a'), &g_font.loca_len);
    if (!g_font.loca) goto fail;
    g_font.glyf = find_table(data, ntab, TAG('g','l','y','f'), &g_font.glyf_len);
    if (!g_font.glyf) goto fail;

    g_font.num_glyphs = rd16(g_font.maxp + 4);
    g_font.num_hmetrics = rd16(g_font.hhea + 34);
    g_font.loca_format = rds16(g_font.head + 50);
    g_font.ascent = rds16(g_font.hhea + 4);
    g_font.descent = rds16(g_font.hhea + 6);
    if (!g_font.num_glyphs || !g_font.num_hmetrics ||
        g_font.ascent <= g_font.descent) goto fail;
    return 0;

fail:
    free(data);
    memset(&g_font, 0, sizeof(g_font));
    return -1;
}

static uint16_t ttf_gid(uint32_t cp) {
    const uint8_t *c = g_font.cmap4;
    if (!c || cp > 0xFFFF) return 0;
    uint16_t seg2 = rd16(c + 6);
    uint16_t segs = seg2 / 2;
    if (!segs || 16u + (uint32_t)seg2 * 4u > g_font.cmap4_len) return 0;
    const uint8_t *endp = c + 14;
    const uint8_t *startp = endp + seg2 + 2;
    const uint8_t *deltap = startp + seg2;
    const uint8_t *rop = deltap + seg2;
    for (uint16_t i = 0; i < segs; i++) {
        uint16_t end = rd16(endp + i * 2);
        uint16_t start = rd16(startp + i * 2);
        if (cp < start || cp > end) continue;
        int16_t delta = rds16(deltap + i * 2);
        uint16_t ro = rd16(rop + i * 2);
        if (ro == 0) return (uint16_t)(cp + delta);
        const uint8_t *p = rop + i * 2 + ro + (cp - start) * 2;
        if (p < c || p + 2 > c + g_font.cmap4_len) return 0;
        uint16_t g = rd16(p);
        return g ? (uint16_t)(g + delta) : 0;
    }
    return 0;
}

static int ttf_glyph_range(uint16_t gid, uint32_t *start, uint32_t *end) {
    if (gid >= g_font.num_glyphs) return 0;
    uint32_t a, b;
    if (g_font.loca_format == 0) {
        if ((uint32_t)(gid * 2 + 4) > g_font.loca_len) return 0;
        a = rd16(g_font.loca + gid * 2) * 2u;
        b = rd16(g_font.loca + gid * 2 + 2) * 2u;
    } else {
        if ((uint32_t)(gid * 4 + 8) > g_font.loca_len) return 0;
        a = rd32(g_font.loca + gid * 4);
        b = rd32(g_font.loca + gid * 4 + 4);
    }
    if (a > b || b > g_font.glyf_len) return 0;
    *start = a; *end = b;
    return 1;
}

static void add_edge(float x0, float y0, float x1, float y1) {
    if (g_edge_count >= TTF_MAX_EDGES || y0 == y1) return;
    int dir = 1;
    if (y0 > y1) {
        float tx = x0, ty = y0;
        x0 = x1; y0 = y1; x1 = tx; y1 = ty; dir = -1;
    }
    struct ttf_edge *e = &g_edges[g_edge_count++];
    e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1; e->dir = dir;
}

static void emit_line(float fx0, float fy0, float fx1, float fy1) {
    add_edge(g_xoff + fx0 * g_scale, g_baseline - fy0 * g_scale,
             g_xoff + fx1 * g_scale, g_baseline - fy1 * g_scale);
}

static void emit_quad(float x0, float y0, float x1, float y1,
                      float x2, float y2) {
    float px = x0, py = y0;
    for (int i = 1; i <= TTF_CURVE_STEPS; i++) {
        float t = (float)i / TTF_CURVE_STEPS;
        float u = 1.0f - t;
        float nx = u*u*x0 + 2.0f*u*t*x1 + t*t*x2;
        float ny = u*u*y0 + 2.0f*u*t*y1 + t*t*y2;
        emit_line(px, py, nx, ny);
        px = nx; py = ny;
    }
}

static int inside_at(float sx, float sy) {
    int n = 0;
    for (int i = 0; i < g_edge_count; i++) {
        struct ttf_edge *e = &g_edges[i];
        if (sy < e->y0 || sy >= e->y1) continue;
        float x = e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
        if (n < TTF_MAX_EDGES) {
            g_hits[n] = x;
            g_hit_dirs[n] = e->dir;
            n++;
        }
    }
    for (int i = 1; i < n; i++) {
        float v = g_hits[i];
        int d = g_hit_dirs[i];
        int j = i - 1;
        while (j >= 0 && g_hits[j] > v) {
            g_hits[j + 1] = g_hits[j];
            g_hit_dirs[j + 1] = g_hit_dirs[j];
            j--;
        }
        g_hits[j + 1] = v;
        g_hit_dirs[j + 1] = d;
    }
    int wind = 0;
    float start = 0.0f;
    for (int i = 0; i < n; i++) {
        float x = g_hits[i];
        if (wind == 0) start = x;
        wind += g_hit_dirs[i];
        if (wind == 0 && sx >= start && sx < x) return 1;
    }
    return wind != 0 && sx >= start;
}

static int render_outline(const uint8_t *g, uint32_t len, int ncontours) {
    if (ncontours > TTF_MAX_CONTOURS) return 0;
    if (10u + (uint32_t)ncontours * 2u > len) return 0;
    uint32_t total = 0;
    for (int c = 0; c < ncontours; c++) {
        uint16_t e = rd16(g + 10 + c * 2);
        if (e + 1 > TTF_MAX_POINTS) return 0;
        if (c && e < g_end_pts[c - 1]) return 0;
        g_end_pts[c] = e;
        total = (uint32_t)e + 1;
    }
    if (total == 0) return 0;
    uint32_t p = 10u + (uint32_t)ncontours * 2u;
    if (p + 2 > len) return 0;
    uint16_t ilen = rd16(g + p);
    p += 2u + ilen;
    if (p > len) return 0;

    uint32_t i = 0;
    while (i < total) {
        if (p >= len) return 0;
        uint8_t f = g[p++];
        g_flags[i++] = f;
        if (f & 8) {
            if (p >= len) return 0;
            uint8_t rep = g[p++];
            while (rep-- && i < total) g_flags[i++] = f;
        }
    }
    int32_t x = 0, y = 0;
    for (i = 0; i < total; i++) {
        uint8_t f = g_flags[i];
        int32_t d;
        if (f & 2) {
            if (p >= len) return 0;
            d = g[p++];
            if (!(f & 16)) d = -d;
        } else {
            if (f & 16) d = 0;
            else {
                if (p + 2 > len) return 0;
                d = rds16(g + p); p += 2;
            }
        }
        x += d; g_xs[i] = x;
    }
    for (i = 0; i < total; i++) {
        uint8_t f = g_flags[i];
        int32_t d;
        if (f & 4) {
            if (p >= len) return 0;
            d = g[p++];
            if (!(f & 32)) d = -d;
        } else {
            if (f & 32) d = 0;
            else {
                if (p + 2 > len) return 0;
                d = rds16(g + p); p += 2;
            }
        }
        y += d; g_ys[i] = y;
    }

    g_edge_count = 0;
    uint32_t first = 0;
    for (int c = 0; c < ncontours; c++) {
        uint32_t end = g_end_pts[c];
        if (end < first) return 0;
        uint32_t n = end - first + 1;
        uint32_t ex = 0;
        if (n * 2 > TTF_MAX_EXPANDED) return 0;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t a = first + k;
            uint32_t b = first + (k + 1) % n;
            g_ex[ex] = g_xs[a]; g_ey[ex] = g_ys[a];
            g_eon[ex++] = g_flags[a] & 1;
            if (!(g_flags[a] & 1) && !(g_flags[b] & 1)) {
                g_ex[ex] = (g_xs[a] + g_xs[b]) / 2;
                g_ey[ex] = (g_ys[a] + g_ys[b]) / 2;
                g_eon[ex++] = 1;
            }
        }
        int st = -1;
        for (uint32_t k = 0; k < ex; k++) if (g_eon[k]) { st = (int)k; break; }
        if (st < 0) { first = end + 1; continue; }
        uint32_t idx = (uint32_t)st;
        uint32_t guard = 0;
        do {
            uint32_t n1 = (idx + 1) % ex;
            if (g_eon[idx] && g_eon[n1]) {
                emit_line((float)g_ex[idx], (float)g_ey[idx],
                          (float)g_ex[n1], (float)g_ey[n1]);
                idx = n1;
            } else if (g_eon[idx] && !g_eon[n1]) {
                uint32_t n2 = (n1 + 1) % ex;
                if (!g_eon[n2]) break;
                emit_quad((float)g_ex[idx], (float)g_ey[idx],
                          (float)g_ex[n1], (float)g_ey[n1],
                          (float)g_ex[n2], (float)g_ey[n2]);
                idx = n2;
            } else {
                break;
            }
            if (++guard > ex + 2) break;
        } while (idx != (uint32_t)st);
        first = end + 1;
    }
    return g_edge_count > 0;
}

int ttf_render_glyph(uint32_t cp, uint8_t *mask, int cell_w, int cell_h) {
    if (!g_font.data || !mask || cell_w <= 0 || cell_h <= 0 || cell_w > 8)
        return 0;
    memset(mask, 0, (uint32_t)cell_h);
    uint16_t gid = ttf_gid(cp);
    if (!gid) return 0;
    if (gid >= g_font.num_glyphs) return 0;

    uint32_t hoff = (uint32_t)(gid < g_font.num_hmetrics ?
                               gid : g_font.num_hmetrics - 1) * 4u;
    if (hoff + 2 > g_font.hmtx_len) return 0;
    uint16_t advance = rd16(g_font.hmtx + hoff);
    if (!advance) return 0;

    uint32_t a, b;
    if (!ttf_glyph_range(gid, &a, &b)) return 0;
    if (a == b) return 1;

    const uint8_t *g = g_font.glyf + a;
    uint32_t len = b - a;
    if (len < 10) return 0;
    int ncontours = rds16(g);
    if (ncontours < 0) return 0;
    if (!ncontours) return 1;

    float inset = 1.0f;
    float sx = ((float)cell_w - inset) / (float)advance;
    float sy = ((float)cell_h - inset) /
               (float)(g_font.ascent - g_font.descent);
    g_scale = sx < sy ? sx : sy;
    float total_h = (float)(g_font.ascent - g_font.descent) * g_scale;
    g_baseline = (float)cell_h * 0.5f +
                 ((float)g_font.ascent + (float)g_font.descent) * g_scale * 0.5f;
    if (total_h > (float)cell_h) g_baseline = (float)cell_h - 1.0f;
    g_xoff = ((float)cell_w - (float)advance * g_scale) * 0.5f;

    if (!render_outline(g, len, ncontours)) return 1;

    for (int py = 0; py < cell_h; py++) {
        for (int px = 0; px < cell_w; px++) {
            int hits = 0;
            for (int syi = 0; syi < 4; syi++) {
                for (int sxi = 0; sxi < 4; sxi++) {
                    float fx = (float)px + ((float)sxi + 0.5f) * 0.25f;
                    float fy = (float)py + ((float)syi + 0.5f) * 0.25f;
                    if (inside_at(fx, fy)) hits++;
                }
            }
            if (hits >= 6) mask[py] |= (uint8_t)(0x80u >> px);
        }
    }
    return 1;
}
