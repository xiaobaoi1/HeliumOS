#ifndef METHYLUI_FONT_H
#define METHYLUI_FONT_H

#include <stdint.h>

void mui_font_load(void);
int mui_font_loaded(void);
const uint8_t *mui_font_glyph(int cp);
int mui_font_width(int cp);

#endif
