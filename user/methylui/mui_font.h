#ifndef METHYLUI_FONT_H
#define METHYLUI_FONT_H

#include <stdint.h>

/* 加载 SYS:/UNICODE.HEX 的 ASCII 子集。可重复调用，已加载则直接返回。 */
void mui_font_load(void);

/* 返回 1 表示字体已加载，0 表示加载失败。 */
int mui_font_loaded(void);

/* 返回码点 cp 的字形指针（16 字节，8x16 位图）。
 * cp 超出 0..127 或未加载返回 NULL。 */
const uint8_t *mui_font_glyph(int cp);

#endif