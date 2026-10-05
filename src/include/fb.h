#ifndef FB_H
#define FB_H

#include <stdint.h>

struct fb_info {
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;      /* 每行字节数 */
    uint8_t   bpp;        /* 每像素位数，当前只支持 32 */
    uint8_t   valid;
    uint16_t  reserved;
    uint64_t  phys;       /* framebuffer 物理地址 */
    uint32_t *virt;       /* 内核虚拟地址（kmap） */
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