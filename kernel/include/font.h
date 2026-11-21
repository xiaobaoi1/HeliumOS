#ifndef FONT_H
#define FONT_H

struct fat32_volume;

#include <stdint.h>

#define FONT_MAX_W 16
#define FONT_MAX_H 16
#define FONT_MAX_BYTES ((FONT_MAX_W / 8) * FONT_MAX_H)   /* 32 */

struct font {
    uint8_t valid;
    uint8_t width[128];                    /* 8 或 16；0 = 未加载 */
    uint8_t glyph[128][FONT_MAX_BYTES];    /* 最大 16x16 = 32 字节 */
    uint8_t height[128];                   /* 都是 16 */
};

void font_init(struct fat32_volume *vol);
const struct font *font_get(void);
/* 返回字形指针；*out_w 填宽度；未加载返回 NULL */
const uint8_t *font_glyph(uint8_t c, int *out_w);

#endif