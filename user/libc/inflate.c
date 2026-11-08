/* DEFLATE 解压 + gzip/zlib 包装。
 * 从 image.c 提取，供 PNG 和 HTTP 共用。 */

#include "inflate.h"
#include "string.h"
#include <stdint.h>

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

int inflate_raw(const uint8_t *in, int in_len,
                uint8_t *out, int out_max, int *out_len) {
    if (!in || !out || in_len <= 0 || out_max <= 0) return -1;

    struct bits b;
    br_init(&b, in, in_len);

    int pos = 0, final = 0;
    while (!final) {
        final = br_bits(&b, 1);
        int type = br_bits(&b, 2);
        if (final < 0 || type < 0) return -1;
        if (type == 0) {
            br_align(&b);
            int len = br_bits(&b, 16), nlen = br_bits(&b, 16);
            if (len < 0 || nlen < 0 || (len ^ 0xFFFF) != nlen) return -1;
            if (pos + len > out_max) return -1;
            for (int i = 0; i < len; i++) {
                int v = br_bits(&b, 8);
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
        } else {
            int hlit = br_bits(&b, 5), hdist = br_bits(&b, 5), hclen = br_bits(&b, 4);
            if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
            hlit += 257; hdist += 1; hclen += 4;
            static const int order[19] = {
                16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
            };
            uint8_t cl[19];
            memset(cl, 0, sizeof(cl));
            for (int i = 0; i < hclen; i++) {
                int v = br_bits(&b, 3);
                if (v < 0) return -1;
                cl[order[i]] = (uint8_t)v;
            }
            struct huff clh;
            huff_build(&clh, cl, 19);
            uint8_t lens[320];
            int total = hlit + hdist, i = 0;
            while (i < total) {
                int s = huff_decode(&b, &clh);
                if (s < 0) return -1;
                if (s <= 15) lens[i++] = (uint8_t)s;
                else if (s == 16) {
                    if (i == 0) return -1;
                    int rep = br_bits(&b, 2);
                    if (rep < 0) return -1;
                    rep += 3;
                    uint8_t prev = lens[i - 1];
                    while (rep-- && i < total) lens[i++] = prev;
                } else if (s == 17) {
                    int rep = br_bits(&b, 3);
                    if (rep < 0) return -1;
                    rep += 3;
                    while (rep-- && i < total) lens[i++] = 0;
                } else if (s == 18) {
                    int rep = br_bits(&b, 7);
                    if (rep < 0) return -1;
                    rep += 11;
                    while (rep-- && i < total) lens[i++] = 0;
                } else return -1;
            }
            huff_build(&lit, lens, hlit);
            huff_build(&dist, lens + hlit, hdist);
        }

        for (;;) {
            int sym = huff_decode(&b, &lit);
            if (sym < 0) return -1;
            if (sym < 256) {
                if (pos >= out_max) return -1;
                out[pos++] = (uint8_t)sym;
            } else if (sym == 256) break;
            else {
                sym -= 257;
                if (sym < 0 || sym >= 29) return -1;
                int le = br_bits(&b, len_extra[sym]);
                int dsym = huff_decode(&b, &dist);
                if (le < 0 || dsym < 0 || dsym >= 30) return -1;
                int de = br_bits(&b, dist_extra[dsym]);
                if (de < 0) return -1;
                int length = len_base[sym] + le;
                int distance = dist_base[dsym] + de;
                if (distance <= 0 || distance > pos || pos + length > out_max)
                    return -1;
                for (int k = 0; k < length; k++) {
                    out[pos] = out[pos - distance];
                    pos++;
                }
            }
        }
    }
    if (out_len) *out_len = pos;
    return 0;
}

int gzip_decompress(const uint8_t *in, int in_len,
                    uint8_t *out, int out_max) {
    if (!in || !out || in_len < 18) return -1;
    if (in[0] != 0x1f || in[1] != 0x8b || in[2] != 0x08) return -1;
    uint8_t flags = in[3];
    int pos = 10;
    if (flags & 0x04) {
        if (pos + 2 > in_len) return -1;
        int xlen = in[pos] | (in[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08) {
        while (pos < in_len && in[pos]) pos++;
        pos++;
    }
    if (flags & 0x10) {
        while (pos < in_len && in[pos]) pos++;
        pos++;
    }
    if (flags & 0x02) pos += 2;
    if (pos >= in_len - 8) return -1;
    int out_len = 0;
    int r = inflate_raw(in + pos, in_len - pos - 8, out, out_max, &out_len);
    return r == 0 ? out_len : -1;
}

int zlib_decompress(const uint8_t *in, int in_len,
                    uint8_t *out, int out_max) {
    if (!in || !out || in_len < 6) return -1;
    uint8_t cmf = in[0], flg = in[1];
    if ((cmf & 0x0f) != 8) return -1;
    if (((cmf << 8) | flg) % 31 != 0) return -1;
    int pos = 2;
    if (flg & 0x20) pos += 4;
    if (pos >= in_len - 4) return -1;
    int out_len = 0;
    int r = inflate_raw(in + pos, in_len - pos - 4, out, out_max, &out_len);
    return r == 0 ? out_len : -1;
}