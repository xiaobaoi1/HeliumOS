#ifndef METHYLUI_SURFACE_H
#define METHYLUI_SURFACE_H

#include "shm_layout.h"
#include <stdint.h>

#define MUI_FONT_W 8
#define MUI_FONT_H 16

struct mui_surface {
    unsigned int    *pixels;
    int              w, h;
    struct shm_hdr  *hdr;     /* 脏区标记用；NULL 表示不标 */

    /* 矩形裁剪区；clip_on == 0 表示整个 surface。见 mui_clip.h */
    int              clip_on;
    int              clip_x0, clip_y0, clip_x1, clip_y1;
};

/* 标记脏区 [x0, x1) × [y0, y1)。内部夹紧到 surface 边界。 */
void mui_mark_dirty(struct mui_surface *s,
                    int x0, int y0, int x1, int y1);

/* 填充矩形。像素 + 脏区。 */
void mui_fill(struct mui_surface *s, int x, int y, int w, int h,
              unsigned int rgb);

/* 1px 边框（空心矩形）。 */
void mui_border(struct mui_surface *s, int x, int y, int w, int h,
                unsigned int rgb);

/* 画一个字符。返回前进的像素宽度（总是 8）。 */
int mui_draw_char(struct mui_surface *s, int x, int y, unsigned int cp,
                  unsigned int fg, unsigned int bg);

/* 画字符串。返回总像素宽度。 */
int mui_text(struct mui_surface *s, int x, int y, const char *str,
             unsigned int fg, unsigned int bg);

/* 文本像素宽度（不绘制）。 */
int mui_text_width(const char *str);

#endif