#include "methylui/mui_bmpload.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include <stdint.h>

static int bmp_le16(const uint8_t *p) {
    return (int)p[0] | ((int)p[1] << 8);
}

static int32_t bmp_le32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

void mui_bmp_free(struct mui_bitmap *img) {
    if (!img) return;
    if (img->pixels) free(img->pixels);
    img->pixels = NULL;
    img->w = 0;
    img->h = 0;
    img->trans = 0;
}

int mui_bmp_load(struct mui_bitmap *img, const char *path) {
    if (!img || !path) return 0;
    mui_bmp_free(img);

    int fd = fs_open(path, FS_O_RDONLY);
    if (fd < 0) return 0;

    uint8_t hdr[54];
    int got = 0;
    while (got < (int)sizeof(hdr)) {
        int n = fs_read(fd, hdr + got, (int)sizeof(hdr) - got);
        if (n <= 0) break;
        got += n;
    }
    if (got != (int)sizeof(hdr)) { fs_close(fd); return 0; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { fs_close(fd); return 0; }

    uint32_t offset = (uint32_t)bmp_le32(hdr + 10);
    int32_t  width  = bmp_le32(hdr + 18);
    int32_t  height = bmp_le32(hdr + 22);
    int      bpp    = bmp_le16(hdr + 28);
    uint32_t comp   = (uint32_t)bmp_le32(hdr + 30);

    if (width <= 0 || width > 4096 || height == 0 || height > 4096 ||
        height < -4096) {
        fs_close(fd);
        return 0;
    }
    /* 0 = BI_RGB；3 = BI_BITFIELDS（图标 BMP 使用标准 BGRA 掩码） */
    if ((bpp != 24 && bpp != 32) || (comp != 0 && comp != 3)) {
        fs_close(fd);
        return 0;
    }

    int abs_h = height < 0 ? -height : height;
    int row_size = ((int)width * bpp / 8 + 3) & ~3;

    uint8_t *row = (uint8_t*)malloc((size_t)row_size);
    if (!row) { fs_close(fd); return 0; }

    size_t px_count = (size_t)width * (size_t)abs_h;
    unsigned int *pixels = (unsigned int*)malloc(px_count * 4);
    if (!pixels) { free(row); fs_close(fd); return 0; }
    memset(pixels, 0, px_count * 4);

    if (fs_seek(fd, (int)offset) < 0) {
        free(row);
        free(pixels);
        fs_close(fd);
        return 0;
    }

    int any_alpha = 0;
    for (int y = 0; y < abs_h; y++) {
        got = 0;
        while (got < row_size) {
            int n = fs_read(fd, row + got, row_size - got);
            if (n <= 0) break;
            got += n;
        }
        if (got < row_size) break;

        int dy = (height > 0) ? (abs_h - 1 - y) : y;
        for (int x = 0; x < (int)width; x++) {
            uint8_t b = row[x * bpp / 8];
            uint8_t g = row[x * bpp / 8 + 1];
            uint8_t r = row[x * bpp / 8 + 2];
            uint8_t a = (bpp == 32) ? row[x * 4 + 3] : 0xFF;
            if (a) any_alpha = 1;
            pixels[(size_t)dy * (size_t)width + x] =
                ((unsigned int)a << 24) | ((unsigned int)r << 16) |
                ((unsigned int)g << 8) | b;
        }
    }

    fs_close(fd);
    free(row);

    if (bpp == 32 && !any_alpha) {
        for (size_t i = 0; i < px_count; i++) pixels[i] |= 0xFF000000u;
    }
    img->pixels = pixels;
    img->w = (int)width;
    img->h = abs_h;
    img->trans = pixels[0] & 0xFFFFFFu;
    return 1;
}

int mui_bmp_load_icon(struct mui_bitmap *img, const char *path) {
    if (!mui_bmp_load(img, path)) return 0;
    size_t n = (size_t)img->w * (size_t)img->h;
    for (size_t i = 0; i < n; i++) {
        unsigned int rgb = img->pixels[i] & 0xFFFFFFu;
        if (rgb == 0x000000u || rgb == 0xFFFFFFu)
            img->pixels[i] = 0;
    }
    return 1;
}

int mui_bmp_blit(struct mui_bitmap *img, struct mui_surface *s,
                 int x, int y, int w, int h) {
    if (!img || !img->pixels || !s || w <= 0 || h <= 0) return 0;
    if (img->w <= 0 || img->h <= 0) return 0;

    for (int dy = 0; dy < h; dy++) {
        int sy = dy * img->h / h;
        if (sy >= img->h) sy = img->h - 1;

        for (int dx = 0; dx < w; dx++) {
            int sx = dx * img->w / w;
            if (sx >= img->w) sx = img->w - 1;

            unsigned int c = img->pixels[(size_t)sy * (size_t)img->w + sx];
            unsigned int a = c >> 24;
            if (a == 0) continue;

            int px = x + dx;
            int py = y + dy;
            if (px < 0 || py < 0 || px >= s->w || py >= s->h) continue;
            if (s->clip_on &&
                (px < s->clip_x0 || py < s->clip_y0 ||
                 px >= s->clip_x1 || py >= s->clip_y1)) continue;

            if (a == 0xFF) {
                s->pixels[py * s->w + px] = c & 0xFFFFFFu;
            } else {
                unsigned int d = s->pixels[py * s->w + px];
                unsigned int sr = (c >> 16) & 0xFF, sg = (c >> 8) & 0xFF, sb = c & 0xFF;
                unsigned int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                unsigned int rr = (sr * a + dr * (255 - a)) / 255;
                unsigned int gg = (sg * a + dg * (255 - a)) / 255;
                unsigned int bb = (sb * a + db * (255 - a)) / 255;
                s->pixels[py * s->w + px] = (rr << 16) | (gg << 8) | bb;
            }
        }
    }

    mui_mark_dirty(s, x, y, x + w, y + h);
    return 1;
}
