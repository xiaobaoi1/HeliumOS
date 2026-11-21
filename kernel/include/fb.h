#ifndef FB_H
#define FB_H

#include <stdint.h>

struct fb_info {
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;
    uint8_t   bpp;
    uint8_t   valid;
    uint16_t  reserved;
    uint64_t  phys;
    uint32_t *virt;      /* framebuffer 内核虚拟地址 */
    uint32_t *back;      /* 后台缓冲（内核虚拟地址） */
    uint32_t *screen;    /* 合成结果（tty + 窗口 + 鼠标） */
};

struct fb_user_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t addr;    /* 用户虚拟地址 */
};

/* 从 Multiboot2 info 解析 framebuffer tag。
 * 找不到 framebuffer 时 fb->valid = 0，不 panic。 */
void fb_init(uint32_t mb_addr);

/* 拿到全局 fb。valid = 0 表示不可用。 */
struct fb_info *fb_get(void);

/* 基础操作——P1 只提供这两个，P4 再加。 */
void fb_clear(uint32_t rgb);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb);

#endif