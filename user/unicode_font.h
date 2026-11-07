#ifndef USER_UNICODE_FONT_H
#define USER_UNICODE_FONT_H

#include <stdint.h>

int ufont_load(void);
int ufont_loaded(void);
const uint8_t *ufont_glyph(uint32_t cp);
int ufont_width(uint32_t cp);

/* Decode one UTF-8 sequence. Returns byte count (>=1) and stores codepoint. */
int ufont_utf8_decode(const unsigned char *s, uint32_t *cp);

#endif
