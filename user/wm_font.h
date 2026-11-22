#ifndef USER_WM_FONT_H
#define USER_WM_FONT_H

#include <stdint.h>

#define FONT_W 8
#define FONT_H 16

void wm_font_load(void);
int wm_font_loaded(void);
const uint8_t *wm_font_glyph(int cp);
int wm_font_width(int cp);

#endif
