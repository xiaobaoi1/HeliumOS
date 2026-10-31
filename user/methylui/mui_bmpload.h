#ifndef METHYLUI_BMPLOAD_H
#define METHYLUI_BMPLOAD_H

#include "methylui/mui_surface.h"

/* BMP 位图（24/32bpp，未压缩）。
 * 像素格式 0x00RRGGBB；trans 为透明色（默认取左上角像素）。 */
struct mui_bitmap {
    unsigned int *pixels;
    int w, h;
    unsigned int trans;
};

/* 从文件加载 BMP。返回 1 成功，0 失败。重复调用会先释放旧像素。 */
int  mui_bmp_load(struct mui_bitmap *img, const char *path);

void mui_bmp_free(struct mui_bitmap *img);

/* 把位图缩放到 (x,y,w,h) 画到 surface。trans 色跳过。
 * 自动遵守 surface 的裁剪区，并标记目的地脏区。 */
int  mui_bmp_blit(struct mui_bitmap *img, struct mui_surface *s,
                  int x, int y, int w, int h);

#endif
