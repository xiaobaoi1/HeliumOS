#ifndef METHYLUI_CLIP_H
#define METHYLUI_CLIP_H

#include "methylui/mui_surface.h"

/* 矩形裁剪。
 * mui_clip_set / mui_clip_set_rect 之后，该 surface 上所有 mui_fill /
 * mui_border / mui_draw_char / mui_text 的像素写入和脏区标记都会被限制在
 * 裁剪区内；mui_clip_reset 恢复为整个 surface。
 *
 * 用法（组件绘制）：
 *   mui_clip_set(s, x, y, w, h);
 *   ... 绘制组件 ...
 *   mui_clip_reset(s);
 */
struct mui_clip {
    int x0, y0, x1, y1;
};

void mui_clip_set(struct mui_surface *s, int x, int y, int w, int h);
void mui_clip_set_rect(struct mui_surface *s, int x0, int y0, int x1, int y1);
void mui_clip_reset(struct mui_surface *s);
int  mui_clip_active(struct mui_surface *s);
void mui_clip_get(struct mui_surface *s, struct mui_clip *out);

#endif
