#ifndef USER_GFX_H
#define USER_GFX_H

#include <stdint.h>
#include "syscall.h"

struct gfx_info {
    unsigned int width;
    unsigned int height;
    unsigned int bpp;
};

static inline int gfx_get_info(struct gfx_info *info) {
    return __syscall(SYS_GFX_GET_INFO, (int)info, 0, 0);
}

static inline int gfx_fill_rect(unsigned int x, unsigned int y,
                                unsigned int w, unsigned int h,
                                unsigned int rgb) {
    return __syscall5(SYS_GFX_FILL_RECT, (int)x, (int)y,
                      (int)w, (int)h, (int)rgb);
}

static inline int gfx_put_pixel(unsigned int x, unsigned int y,
                                unsigned int rgb) {
    return __syscall(SYS_GFX_PUT_PIXEL, (int)x, (int)y, (int)rgb);
}

static inline int gfx_blit(unsigned int x, unsigned int y,
                           unsigned int w, unsigned int h,
                           const void *buf) {
    return __syscall5(SYS_GFX_BLIT, (int)x, (int)y,
                      (int)w, (int)h, (int)buf);
}

#endif