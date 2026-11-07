#include "image.h"
#ifdef IMAGE_HOST_TEST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define IMG_DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include <stdint.h>
#define IMG_DBG(...) do { } while (0)
#endif

struct bits {
    const uint8_t *p, *end;
    uint32_t buf;
    int cnt;
};

struct huff {
    int count[17];
    int mincode[17];
    int maxcode[17];
    int valptr[17];
    uint16_t sym[288];
};

static void image_zero(struct image *img) {
    if (!img) return;
    img->w = img->h = 0;
    img->pixels = NULL;
}

void image_free(struct image *img) {
    if (!img) return;
    if (img->pixels) free(img->pixels);
    image_zero(img);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int br_init(struct bits *b, const uint8_t *p, int len) {
    b->p = p; b->end = p + len; b->buf = 0; b->cnt = 0;
    return 0;
}

static int br_bits(struct bits *b, int n) {
    while (b->cnt < n) {
        if (b->p >= b->end) return -1;
        b->buf |= (uint32_t)(*b->p++) << b->cnt;
        b->cnt += 8;
    }
    int v = (int)(b->buf & ((1u << n) - 1u));
    b->buf >>= n;
    b->cnt -= n;
    return v;
}

static void br_align(struct bits *b) {
    b->buf = 0;
    b->cnt = 0;
}

static void huff_build(struct huff *h, const uint8_t *lens, int n) {
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->valptr[len] = k;
        h->mincode[len] = code;
        code += h->count[len];
        h->maxcode[len] = h->count[len] ? code - 1 : -1;
        code <<= 1;
        k += h->count[len];
    }
    k = 0;
    for (int len = 1; len <= 16; len++)
        for (int i = 0; i < n; i++)
            if (lens[i] == len) h->sym[k++] = (uint16_t)i;
}

static int huff_decode(struct bits *b, const struct huff *h) {
    int code = 0;
    for (int len = 1; len <= 15; len++) {
        int bit = br_bits(b, 1);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        if (h->count[len] && code >= h->mincode[len] && code <= h->maxcode[len])
            return h->sym[h->valptr[len] + code - h->mincode[len]];
    }
    return -1;
}

static const int len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const int len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const int dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const int dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static int inflate_blocks(struct bits *b, uint8_t *out, int out_max, int *out_len) {
    int pos = 0, final = 0;
    while (!final) {
        final = br_bits(b, 1);
        int type = br_bits(b, 2);
        IMG_DBG("inflate block final=%d type=%d pos=%d\n", final, type, pos);
        if (final < 0 || type < 0) return -1;
        if (type == 0) {
            br_align(b);
            int len = br_bits(b, 16), nlen = br_bits(b, 16);
            if (len < 0 || nlen < 0 || (len ^ 0xFFFF) != nlen) return -1;
            if (pos + len > out_max) return -1;
            for (int i = 0; i < len; i++) {
                int v = br_bits(b, 8);
                if (v < 0) return -1;
                out[pos++] = (uint8_t)v;
            }
            continue;
        }
        if (type == 3) return -1;
        struct huff lit, dist;
        if (type == 1) {
            uint8_t ll[288], dl[32];
            for (int i = 0; i < 144; i++) ll[i] = 8;
            for (int i = 144; i < 256; i++) ll[i] = 9;
            for (int i = 256; i < 280; i++) ll[i] = 7;
            for (int i = 280; i < 288; i++) ll[i] = 8;
            for (int i = 0; i < 32; i++) dl[i] = 5;
            huff_build(&lit, ll, 288);
            huff_build(&dist, dl, 32);
            IMG_DBG("fixed len7 c=%d min=%d max=%d vp=%d sym0=%d sym12=%d\n",
                    lit.count[7], lit.mincode[7], lit.maxcode[7], lit.valptr[7],
                    lit.sym[0], lit.sym[12]);
        } else {
            int hlit = br_bits(b, 5), hdist = br_bits(b, 5), hclen = br_bits(b, 4);
            if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
            hlit += 257; hdist += 1; hclen += 4;
            IMG_DBG("dyn hlit=%d hdist=%d hclen=%d\n", hlit, hdist, hclen);
            static const int order[19] = {
                16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
            };
            uint8_t cl[19];
            memset(cl, 0, sizeof(cl));
            for (int i = 0; i < hclen; i++) {
                int v = br_bits(b, 3);
                if (v < 0) return -1;
                cl[order[i]] = (uint8_t)v;
            }
            struct huff clh;
            huff_build(&clh, cl, 19);
            uint8_t lens[320];
            int total = hlit + hdist, i = 0;
            while (i < total) {
                int s = huff_decode(b, &clh);
                if (s < 0) { IMG_DBG("cl decode fail i=%d\n", i); return -1; }
                if (s <= 15) lens[i++] = (uint8_t)s;
                else if (s == 16) {
                    if (i == 0) return -1;
                    int rep = br_bits(b, 2);
                    if (rep < 0) return -1;
                    rep += 3;
                    uint8_t prev = lens[i - 1];
                    while (rep-- && i < total) lens[i++] = prev;
                } else if (s == 17) {
                    int rep = br_bits(b, 3);
                    if (rep < 0) return -1;
                    rep += 3;
                    while (rep-- && i < total) lens[i++] = 0;
                } else if (s == 18) {
                    int rep = br_bits(b, 7);
                    if (rep < 0) return -1;
                    rep += 11;
                    while (rep-- && i < total) lens[i++] = 0;
                } else return -1;
            }
            huff_build(&lit, lens, hlit);
            huff_build(&dist, lens + hlit, hdist);
            IMG_DBG("trees built lens0=%d lens256=%d\n", lit.count[0], lit.count[7]);
        }
        for (;;) {
            int sym = huff_decode(b, &lit);
            if (pos < 30) IMG_DBG("lit sym=%d pos=%d cnt=%d buf=%x\n", sym, pos, b->cnt, b->buf);
            if (sym < 0) { IMG_DBG("lit decode fail pos=%d\n", pos); return -1; }
            if (sym < 256) {
                if (pos >= out_max) return -1;
                out[pos++] = (uint8_t)sym;
            } else if (sym == 256) break;
            else {
                sym -= 257;
                if (sym < 0 || sym >= 29) return -1;
                int le = br_bits(b, len_extra[sym]);
                int de = 0, dsym = huff_decode(b, &dist);
                if (le < 0 || dsym < 0 || dsym >= 30) return -1;
                de = br_bits(b, dist_extra[dsym]);
                if (de < 0) return -1;
                int length = len_base[sym] + le;
                int distance = dist_base[dsym] + de;
                if (pos < 30) IMG_DBG("match len=%d dist=%d dsym=%d le=%d de=%d pos=%d\n", length,distance,dsym,le,de,pos);
                if (distance <= 0 || distance > pos || pos + length > out_max) {
                    IMG_DBG("copy fail pos=%d dist=%d len=%d\n", pos, distance, length);
                    return -1;
                }
                for (int k = 0; k < length; k++) {
                    out[pos] = out[pos - distance];
                    pos++;
                }
            }
        }
    }
    *out_len = pos;
    return 0;
}

static int png_unfilter(uint8_t *raw, int w, int h, int ch) {
    int rb = w * ch;
    int bpp = ch;
    for (int y = 0; y < h; y++) {
        uint8_t *cur = raw + (size_t)y * (rb + 1) + 1;
        uint8_t *prev = y ? raw + (size_t)(y - 1) * (rb + 1) + 1 : NULL;
        int f = raw[(size_t)y * (rb + 1)];
        for (int i = 0; i < rb; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int v = cur[i];
            if (f == 1) v += a;
            else if (f == 2) v += b;
            else if (f == 3) v += (a + b) >> 1;
            else if (f == 4) {
                int p = a + b - c;
                int pa = p > a ? p - a : a - p;
                int pb = p > b ? p - b : b - p;
                int pc = p > c ? p - c : c - p;
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            } else if (f != 0) return -1;
            cur[i] = (uint8_t)v;
        }
    }
    return 0;
}

static int png_decode(const uint8_t *data, int len, struct image *img) {
    if (len < 33 || data[0] != 0x89 || data[1] != 'P' ||
        data[2] != 'N' || data[3] != 'G') return 0;
    int pos = 8;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    int idat_total = 0;
    uint8_t palette[256 * 3], pal_alpha[256];
    memset(pal_alpha, 255, sizeof(pal_alpha));
    for (int i = 0; i < 256; i++) palette[i * 3 + 0] = palette[i * 3 + 1] = palette[i * 3 + 2] = 0;
    while (pos + 12 <= len) {
        uint32_t n = be32(data + pos);
        const uint8_t *type = data + pos + 4;
        const uint8_t *pay = data + pos + 8;
        if (pos + 12 + (int)n > len) return 0;
        if (memcmp(type, "IHDR", 4) == 0 && n >= 13) {
            w = (int)be32(pay); h = (int)be32(pay + 4);
            depth = pay[8]; ctype = pay[9]; interlace = pay[12];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            int pc = (int)(n / 3);
            if (pc > 256) pc = 256;
            for (int i = 0; i < pc; i++) {
                palette[i * 3 + 0] = pay[i * 3 + 0];
                palette[i * 3 + 1] = pay[i * 3 + 1];
                palette[i * 3 + 2] = pay[i * 3 + 2];
            }
        } else if (memcmp(type, "tRNS", 4) == 0 && ctype == 3) {
            int pc = (int)n; if (pc > 256) pc = 256;
            for (int i = 0; i < pc; i++) pal_alpha[i] = pay[i];
        } else if (memcmp(type, "IDAT", 4) == 0) {
            idat_total += (int)n;
        } else if (memcmp(type, "IEND", 4) == 0) break;
        pos += 12 + (int)n;
    }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || depth != 8 || interlace != 0)
        return 0;
    int ch = ctype == 2 ? 3 : ctype == 6 ? 4 : ctype == 0 ? 1 :
             ctype == 4 ? 2 : ctype == 3 ? 1 : 0;
    IMG_DBG("PNG %dx%d depth=%d type=%d ch=%d idat=%d\n", w,h,depth,ctype,ch,idat_total);
    if (!ch || !idat_total) return 0;
    uint8_t *idat = (uint8_t*)malloc((size_t)idat_total);
    if (!idat) return 0;
    pos = 8;
    int at = 0;
    while (pos + 12 <= len) {
        uint32_t n = be32(data + pos);
        const uint8_t *type = data + pos + 4;
        const uint8_t *pay = data + pos + 8;
        if (memcmp(type, "IDAT", 4) == 0) {
            memcpy(idat + at, pay, n);
            at += (int)n;
        }
        pos += 12 + (int)n;
    }
    int row_bytes = w * ch;
    int raw_size = (row_bytes + 1) * h;
    uint8_t *raw = (uint8_t*)malloc((size_t)raw_size);
    if (!raw || at < 6) { free(idat); free(raw); return 0; }
    struct bits b;
    br_init(&b, idat + 2, at - 6);
    int out_len = 0;
    int inf = inflate_blocks(&b, raw, raw_size, &out_len);
    IMG_DBG("PNG inflate=%d got=%d want=%d\n", inf, out_len, raw_size);
    int ok = inf == 0 && out_len == raw_size;
    free(idat);
    if (!ok || png_unfilter(raw, w, h, ch) < 0) { free(raw); return 0; }
    uint32_t *px = (uint32_t*)malloc((size_t)w * h * 4);
    if (!px) { free(raw); return 0; }
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (row_bytes + 1) + 1;
        for (int x = 0; x < w; x++) {
            uint8_t r, g, bl, a = 255;
            if (ctype == 0) { r = g = bl = row[x]; }
            else if (ctype == 4) { r = g = bl = row[x * 2]; a = row[x * 2 + 1]; }
            else if (ctype == 2) { r = row[x * 3]; g = row[x * 3 + 1]; bl = row[x * 3 + 2]; }
            else if (ctype == 6) { r = row[x * 4]; g = row[x * 4 + 1]; bl = row[x * 4 + 2]; a = row[x * 4 + 3]; }
            else { int idx = row[x]; r = palette[idx * 3]; g = palette[idx * 3 + 1]; bl = palette[idx * 3 + 2]; a = pal_alpha[idx]; }
            px[(size_t)y * w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                     ((uint32_t)g << 8) | bl;
        }
    }
    free(raw);
    img->w = w; img->h = h; img->pixels = px;
    return 1;
}

/* ---------- JPEG baseline decoder ---------- */

struct jcomp {
    int id, h, v, tq, td, ta, pred;
    int pw, ph, bw, bh, stride;
    uint8_t *plane;
};

struct jpeg {
    int width, height, ncomp, maxh, maxv;
    struct jcomp comp[4];
    uint8_t qt[4][64];
    struct huff dc[4], ac[4];
    int dc_pred[4];
    int restart_interval;
    struct bits br;
};

static const int zigzag[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

static const int16_t idct_cos[8][8] = {
    {4096, 4017, 3784, 3406, 2896, 2276, 1567, 799},
    {4096, 3406, 1567, -799, -2896, -4017, -3784, -2276},
    {4096, 2276, -1567, -4017, -2896, 799, 3784, 3406},
    {4096, 799, -3784, -2276, 2896, 3406, -1567, -4017},
    {4096, -799, -3784, 2276, 2896, -3406, -1567, 4017},
    {4096, -2276, -1567, 4017, -2896, -799, 3784, -3406},
    {4096, -3406, 1567, 799, -2896, 4017, -3784, 2276},
    {4096, -4017, 3784, -3406, 2896, -2276, 1567, -799}
};

static int jpeg_bit(struct bits *b) {
    if (b->cnt == 0) {
        if (b->p >= b->end) return -1;
        uint8_t v = *b->p++;
        if (v == 0xFF) {
            if (b->p >= b->end) return -1;
            uint8_t n = *b->p;
            if (n == 0x00) b->p++;
            else return -1;
        }
        b->buf = v;
        b->cnt = 8;
    }
    int bit = (b->buf >> 7) & 1;
    b->buf = (b->buf << 1) & 0xFF;
    b->cnt--;
    return bit;
}

static int jpeg_get(struct bits *b, int n) {
    int v = 0;
    while (n-- > 0) {
        int bit = jpeg_bit(b);
        if (bit < 0) return -1;
        v = (v << 1) | bit;
    }
    return v;
}

static void jpeg_align(struct bits *b) {
    b->cnt = 0;
    b->buf = 0;
}

static int jpeg_huff(struct bits *b, const struct huff *h) {
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        int bit = jpeg_bit(b);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        if (h->count[len] && code >= h->mincode[len] && code <= h->maxcode[len])
            return h->sym[h->valptr[len] + code - h->mincode[len]];
    }
    return -1;
}

static int jpeg_extend(int v, int n) {
    if (n == 0) return 0;
    int vt = 1 << (n - 1);
    return v < vt ? v - (1 << n) + 1 : v;
}

static void jpeg_idct(const int32_t *coef, uint8_t *dst, int stride) {
    int64_t tmp[8][8];
    for (int y = 0; y < 8; y++) {
        for (int u = 0; u < 8; u++) {
            int64_t s = 0;
            for (int v = 0; v < 8; v++) {
                int64_t a = v ? 4096 : 2896;
                s += (int64_t)coef[v * 8 + u] * a * idct_cos[y][v];
            }
            tmp[y][u] = s;
        }
    }
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int64_t s = 0;
            for (int u = 0; u < 8; u++) {
                int64_t a = u ? 4096 : 2896;
                s += tmp[y][u] * a * idct_cos[x][u];
            }
            int v = (int)(s / (1LL << 50)) + 128;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            dst[y * stride + x] = (uint8_t)v;
        }
    }
}

static int jpeg_block(struct jpeg *j, struct bits *b, struct jcomp *cp,
                      uint8_t *dst) {
    int32_t coef[64];
    memset(coef, 0, sizeof(coef));
    int t = jpeg_huff(b, &j->dc[cp->td]);
    if (t < 0 || t > 11) return -1;
    int diff = t ? jpeg_get(b, t) : 0;
    if (diff < 0) return -1;
    diff = jpeg_extend(diff, t);
    cp->pred += diff;
    coef[0] = cp->pred * j->qt[cp->tq][0];

    for (int k = 1; k < 64; k++) {
        int rs = jpeg_huff(b, &j->ac[cp->ta]);
        if (rs < 0) return -1;
        int run = rs >> 4, size = rs & 15;
        if (size == 0) {
            if (run == 15) { k += 15; continue; }
            break;
        }
        k += run;
        if (k > 63) return -1;
        int v = jpeg_get(b, size);
        if (v < 0) return -1;
        v = jpeg_extend(v, size);
        coef[zigzag[k]] = v * j->qt[cp->tq][k];
    }
    jpeg_idct(coef, dst, cp->stride);
    return 0;
}

static int jpeg_skip_restart(struct bits *b) {
    jpeg_align(b);
    while (b->p < b->end && *b->p != 0xFF) b->p++;
    while (b->p < b->end && *b->p == 0xFF) b->p++;
    if (b->p >= b->end) return -1;
    uint8_t m = *b->p++;
    return (m >= 0xD0 && m <= 0xD7) ? 0 : -1;
}

static int jpeg_scan(struct jpeg *j, const uint8_t *data, int len) {
    jpeg_align(&j->br);
    j->br.p = data;
    j->br.end = data + len;
    int mcu_cols = (j->width + 8 * j->maxh - 1) / (8 * j->maxh);
    int mcu_rows = (j->height + 8 * j->maxv - 1) / (8 * j->maxv);
    int total = mcu_cols * mcu_rows;
    int count = 0;
    for (int my = 0; my < mcu_rows; my++) {
        for (int mx = 0; mx < mcu_cols; mx++) {
            if (j->restart_interval && count && count % j->restart_interval == 0) {
                if (jpeg_skip_restart(&j->br) < 0) return 0;
                for (int i = 0; i < j->ncomp; i++) j->comp[i].pred = 0;
            }
            for (int ci = 0; ci < j->ncomp; ci++) {
                struct jcomp *cp = &j->comp[ci];
                for (int by = 0; by < cp->v; by++) {
                    for (int bx = 0; bx < cp->h; bx++) {
                        int bcol = mx * cp->h + bx;
                        int brow = my * cp->v + by;
                        uint8_t *dst = cp->plane + (size_t)brow * 8 * cp->stride + bcol * 8;
                        if (bcol >= cp->bw || brow >= cp->bh) continue;
                        if (jpeg_block(j, &j->br, cp, dst) < 0) {
                            IMG_DBG("jpeg block fail mcu=%d,%d comp=%d block=%d,%d\n", mx,my,ci,bx,by);
                            return 0;
                        }
                    }
                }
            }
            count++;
        }
    }
    return count == total;
}

static int jpeg_decode(const uint8_t *data, int len, struct image *img) {
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return 0;
    struct jpeg j;
    memset(&j, 0, sizeof(j));
    int pos = 2, scan_pos = -1, scan_len = 0, ok = 0;
    while (pos + 1 < len) {
        while (pos < len && data[pos] != 0xFF) pos++;
        while (pos < len && data[pos] == 0xFF) pos++;
        if (pos >= len) break;
        int m = data[pos++];
        if (m == 0xD9) break;
        if (m == 0xDA) {
            if (pos + 1 >= len) break;
            int sl = ((int)data[pos] << 8) | data[pos + 1];
            if (pos + sl > len) break;
            int ns = data[pos + 2];
            for (int i = 0; i < ns; i++) {
                int cid = data[pos + 3 + i * 2];
                int tt = data[pos + 4 + i * 2];
                for (int c = 0; c < j.ncomp; c++) {
                    if (j.comp[c].id == cid) {
                        j.comp[c].td = tt >> 4;
                        j.comp[c].ta = tt & 15;
                    }
                }
            }
            scan_pos = pos + sl;
            scan_len = len - scan_pos;
            break;
        }
        if (pos + 2 > len) break;
        int seglen = ((int)data[pos] << 8) | data[pos + 1];
        if (seglen < 2 || pos + seglen > len) break;
        const uint8_t *p = data + pos + 2;
        const uint8_t *end = data + pos + seglen;
        if (m == 0xDB) {
            while (p < end) {
                int pq = p[0] >> 4, tq = p[0] & 15; p++;
                if (tq > 3) break;
                if (pq == 0 && p + 64 <= end) {
                    for (int i = 0; i < 64; i++) j.qt[tq][i] = p[i];
                    p += 64;
                } else if (pq == 1 && p + 128 <= end) {
                    for (int i = 0; i < 64; i++) j.qt[tq][i] = p[i * 2];
                    p += 128;
                } else break;
            }
        } else if (m == 0xC4) {
            while (p + 17 <= end) {
                int tc = p[0] >> 4, th = p[0] & 15; p++;
                if (tc > 1 || th > 3) break;
                uint8_t lens[256];
                memset(lens, 0, sizeof(lens));
                int total = 0;
                for (int i = 1; i <= 16; i++) {
                    int c = p[i - 1];
                    for (int k = 0; k < c; k++) lens[total++] = (uint8_t)i;
                }
                p += 16;
                if (p + total > end) break;
                struct huff *h = tc ? &j.ac[th] : &j.dc[th];
                huff_build(h, lens, total);
                int k = 0;
                for (int i = 1; i <= 16; i++)
                    for (int n = 0; n < h->count[i]; n++) h->sym[k++] = *p++;
            }
        } else if (m == 0xC0 || m == 0xC1) {
            if (p + 6 > end) break;
            j.height = (p[1] << 8) | p[2];
            j.width = (p[3] << 8) | p[4];
            j.ncomp = p[5];
            if (j.ncomp < 1 || j.ncomp > 3) break;
            const uint8_t *q = p + 6;
            j.maxh = j.maxv = 1;
            for (int i = 0; i < j.ncomp; i++) {
                if (q + 3 > end) break;
                j.comp[i].id = q[0];
                j.comp[i].h = q[1] >> 4;
                j.comp[i].v = q[1] & 15;
                j.comp[i].tq = q[2];
                if (!j.comp[i].h) j.comp[i].h = 1;
                if (!j.comp[i].v) j.comp[i].v = 1;
                if (j.comp[i].h > j.maxh) j.maxh = j.comp[i].h;
                if (j.comp[i].v > j.maxv) j.maxv = j.comp[i].v;
                q += 3;
            }
            IMG_DBG("JPEG SOF %dx%d n=%d max=%dx%d\n", j.width,j.height,j.ncomp,j.maxh,j.maxv);
            if (j.width <= 0 || j.height <= 0 || j.width > 4096 || j.height > 4096)
                break;
            for (int i = 0; i < j.ncomp; i++) {
                struct jcomp *cp = &j.comp[i];
                cp->pw = (j.width * cp->h + j.maxh - 1) / j.maxh;
                cp->ph = (j.height * cp->v + j.maxv - 1) / j.maxv;
                cp->bw = (cp->pw + 7) / 8;
                cp->bh = (cp->ph + 7) / 8;
                cp->stride = cp->bw * 8;
                cp->plane = (uint8_t*)malloc((size_t)cp->stride * cp->bh * 8);
                if (!cp->plane) break;
                memset(cp->plane, 0, (size_t)cp->stride * cp->bh * 8);
            }
        } else if (m == 0xC2) {
            break; /* progressive unsupported */
        } else if (m == 0xDD) {
            if (p + 2 <= end) j.restart_interval = (p[0] << 8) | p[1];
        }
        pos += seglen;
    }

    if (scan_pos >= 0 && j.width > 0 && j.ncomp > 0) {
        for (int i = 0; i < j.ncomp; i++)
            if (!j.comp[i].plane) goto done;
        IMG_DBG("JPEG scan start len=%d\n", scan_len);
        ok = jpeg_scan(&j, data + scan_pos, scan_len);
        IMG_DBG("JPEG scan ok=%d\n", ok);
    }
    if (ok) {
        if (j.ncomp >= 3) IMG_DBG("jpeg samples %d %d %d\n",
            j.comp[0].plane[0], j.comp[1].plane[0], j.comp[2].plane[0]);
        uint32_t *px = (uint32_t*)malloc((size_t)j.width * j.height * 4);
        if (px) {
            for (int y = 0; y < j.height; y++) {
                for (int x = 0; x < j.width; x++) {
                    int r, g, bl;
                    if (j.ncomp == 1) {
                        struct jcomp *cp = &j.comp[0];
                        int sx = x * cp->h / j.maxh;
                        int sy = y * cp->v / j.maxv;
                        if (sx >= cp->pw) sx = cp->pw - 1;
                        if (sy >= cp->ph) sy = cp->ph - 1;
                        r = g = bl = cp->plane[sy * cp->stride + sx];
                    } else {
                        struct jcomp *Y = &j.comp[0], *Cb = &j.comp[1], *Cr = &j.comp[2];
                        int yy = y * Y->v / j.maxv, yx = x * Y->h / j.maxh;
                        int by = y * Cb->v / j.maxv, bx = x * Cb->h / j.maxh;
                        int ry = y * Cr->v / j.maxv, rx = x * Cr->h / j.maxh;
                        if (yx >= Y->pw) yx = Y->pw - 1;
                        if (yy >= Y->ph) yy = Y->ph - 1;
                        if (bx >= Cb->pw) bx = Cb->pw - 1;
                        if (by >= Cb->ph) by = Cb->ph - 1;
                        if (rx >= Cr->pw) rx = Cr->pw - 1;
                        if (ry >= Cr->ph) ry = Cr->ph - 1;
                        int yv = Y->plane[yy * Y->stride + yx];
                        int cb = Cb->plane[by * Cb->stride + bx] - 128;
                        int cr = Cr->plane[ry * Cr->stride + rx] - 128;
                        r  = yv + ((91881 * cr) >> 16);
                        g  = yv - ((22554 * cb + 46802 * cr) >> 16);
                        bl = yv + ((116130 * cb) >> 16);
                        if (r < 0) r = 0; if (r > 255) r = 255;
                        if (g < 0) g = 0; if (g > 255) g = 255;
                        if (bl < 0) bl = 0; if (bl > 255) bl = 255;
                    }
                    px[(size_t)y * j.width + x] = 0xFF000000u |
                        ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
                }
            }
            img->w = j.width; img->h = j.height; img->pixels = px;
        }
    }
done:
    for (int i = 0; i < 4; i++) if (j.comp[i].plane) free(j.comp[i].plane);
    return ok && img->pixels != NULL;
}

int image_load(struct image *img, const char *path) {
    if (!img || !path) return 0;
    image_free(img);
    int cap = 1 << 20, len = 0;
    uint8_t *data = (uint8_t*)malloc(cap);
    if (!data) return 0;
#ifdef IMAGE_HOST_TEST
    FILE *fp = fopen(path, "rb");
    if (!fp) { free(data); return 0; }
    for (;;) {
        if (len == cap) {
            int ncap = cap * 2;
            uint8_t *nd = (uint8_t*)realloc(data, ncap);
            if (!nd) break;
            data = nd; cap = ncap;
        }
        size_t n = fread(data + len, 1, cap - len, fp);
        if (!n) break;
        len += (int)n;
    }
    fclose(fp);
#else
    int fd = fs_open(path, FS_O_RDONLY);
    if (fd < 0) { free(data); return 0; }
    for (;;) {
        if (len == cap) {
            int ncap = cap * 2;
            uint8_t *nd = (uint8_t*)realloc(data, ncap);
            if (!nd) break;
            data = nd; cap = ncap;
        }
        int n = fs_read(fd, data + len, cap - len);
        if (n <= 0) break;
        len += n;
    }
    fs_close(fd);
#endif
    IMG_DBG("image: %s bytes=%d magic=%02x%02x\n", path, len, data[0], data[1]);
    int ok = 0;
    if (len > 8 && data[0] == 0x89 && data[1] == 'P' &&
        data[2] == 'N' && data[3] == 'G')
        ok = png_decode(data, len, img);
    else if (len > 2 && data[0] == 0xFF && data[1] == 0xD8)
        ok = jpeg_decode(data, len, img);
    free(data);
    if (!ok) image_free(img);
    return ok;
}
