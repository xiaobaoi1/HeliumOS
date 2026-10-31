#include "methylui/mui_clip.h"

void mui_clip_set_rect(struct mui_surface *s, int x0, int y0, int x1, int y1) {
    if (!s) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    s->clip_on = 1;
    s->clip_x0 = x0;
    s->clip_y0 = y0;
    s->clip_x1 = x1;
    s->clip_y1 = y1;
}

void mui_clip_set(struct mui_surface *s, int x, int y, int w, int h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    mui_clip_set_rect(s, x, y, x + w, y + h);
}

void mui_clip_reset(struct mui_surface *s) {
    if (!s) return;
    s->clip_on = 0;
    s->clip_x0 = 0;
    s->clip_y0 = 0;
    s->clip_x1 = s->w;
    s->clip_y1 = s->h;
}

int mui_clip_active(struct mui_surface *s) {
    return s && s->clip_on;
}

void mui_clip_get(struct mui_surface *s, struct mui_clip *out) {
    if (!out) return;
    if (!s) {
        out->x0 = out->y0 = out->x1 = out->y1 = 0;
        return;
    }
    if (s->clip_on) {
        out->x0 = s->clip_x0;
        out->y0 = s->clip_y0;
        out->x1 = s->clip_x1;
        out->y1 = s->clip_y1;
    } else {
        out->x0 = 0;
        out->y0 = 0;
        out->x1 = s->w;
        out->y1 = s->h;
    }
}
