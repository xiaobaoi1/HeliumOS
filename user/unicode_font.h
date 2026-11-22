#ifndef USER_UNICODE_FONT_H
#define USER_UNICODE_FONT_H

#include <stdint.h>

int ufont_load(void);
int ufont_loaded(void);
/* 返回 cp 的位图；未加载、超出范围或没有字形时返回 NULL。 */

const uint8_t *ufont_glyph(uint32_t cp);
/* 返回 cp 的像素宽度；0 表示未加载、超范围或未知宽度，
 * 调用方必须自行 fallback（通常 8px 或 '?' 的宽度）。 */

int ufont_width(uint32_t cp);

/* Decode one UTF-8 sequence. Returns byte count (>=1) and stores codepoint. */
int ufont_utf8_decode(const unsigned char *s, uint32_t *cp);

#endif
