#ifndef USER_TTF_H
#define USER_TTF_H

#include <stdint.h>

/* Load a TrueType font from the filesystem and rasterise glyphs into
 * a 1-bit underline mask (one byte per pixel row, MSB = leftmost pixel).
 * fg/bg colours remain the terminal renderer's responsibility. */
int ttf_load(const char *path, int pixel_height);
int ttf_render_glyph(uint32_t codepoint, uint8_t *mask,
                     int cell_w, int cell_h);

#endif
