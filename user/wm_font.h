#ifndef USER_WM_FONT_H
#define USER_WM_FONT_H

#include <stdint.h>

void           wm_font_load(void);
int            wm_font_loaded(void);
const uint8_t *wm_font_glyph(int cp);

#endif