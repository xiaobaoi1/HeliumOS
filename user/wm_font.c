#include "wm_font.h"
#include "unicode_font.h"

static int g_ok;

void wm_font_load(void) {
    g_ok = (ufont_load() == 0);
}

int wm_font_loaded(void) { return g_ok; }

const uint8_t *wm_font_glyph(int cp) {
    return ufont_glyph((uint32_t)cp);
}

int wm_font_width(int cp) {
    return ufont_width((uint32_t)cp);
}
