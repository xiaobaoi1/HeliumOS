#ifndef DESKTOP_ICONS_H
#define DESKTOP_ICONS_H

#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include "desktop_gfx.h"
#include "methylui/mui_bmpload.h"
#include "methylui/mui_surface.h"

/* 图标位图。加载由 MUI 的 mui_bmp_load 完成，这里只保存结果。 */
static struct mui_bitmap g_img_folder = {0};
static struct mui_bitmap g_img_exec = {0};

/* 图标像素固定在 BSS，避免后续 mmap/munmap 把图标缓冲误解除映射。 */
static unsigned int g_folder_icon_px[256 * 256];
static unsigned int g_exec_icon_px[256 * 256];

static void icons_pin(struct mui_bitmap *img, unsigned int *dst) {
    if (!img->pixels) return;
    if (img->w <= 0 || img->h <= 0 || img->w * img->h > 256 * 256) return;
    memcpy(dst, img->pixels, (size_t)img->w * img->h * 4);
    img->pixels = dst;       /* 原 mmap 缓冲留给进程生命周期 */
}

static void icons_load(void) {
    mui_bmp_load(&g_img_folder, "SYS:\\folder.bmp");
    mui_bmp_load(&g_img_exec, "SYS:\\exec.bmp");
    icons_pin(&g_img_folder, g_folder_icon_px);
    icons_pin(&g_img_exec, g_exec_icon_px);
}

/* 壁纸/图标绘制是裸像素缓冲，这里临时包一个 surface 交给 mui_bmp_blit。 */
static void icon_blit(struct mui_bitmap *img, unsigned int *pixels, int pw,
                      int px, int py, int size) {
    if (!img->pixels || !pixels || pw <= 0 || size <= 0) return;

    struct mui_surface s;
    memset(&s, 0, sizeof(s));
    s.pixels = pixels;
    s.w = pw;
    s.h = 9999;
    s.hdr = NULL;
    mui_bmp_blit(img, &s, px, py, size, size);
}

/* ==================== 图标渲染 ==================== */

static void icon_draw_folder_large(unsigned int *pixels, int pw, int px, int py, int size) {
    if (g_img_folder.pixels) {
        icon_blit(&g_img_folder, pixels, pw, px, py, size);
        return;
    }
    int mid = size / 2;
    int q = size / 4;
    gfx_fill_rect(pixels, pw, 9999, px, py, mid, q, 0xD4A017);
    gfx_fill_rect(pixels, pw, 9999, px, py + q, size, size - q, 0xFFCA28);
    gfx_fill_rect(pixels, pw, 9999, px + size/4, py + size*2/3, size/2, size/3, 0x1E88E5);
    gfx_fill_rect(pixels, pw, 9999, px + size*1/3, py + size*5/6, size/3, 1, 0x0D47A1);
}

static void icon_draw_exec_large(unsigned int *pixels, int pw, int px, int py, int size) {
    if (g_img_exec.pixels) {
        icon_blit(&g_img_exec, pixels, pw, px, py, size);
        return;
    }
    gfx_fill_rect(pixels, pw, 9999, px, py, size, size, 0xE0E0E0);
    gfx_fill_rect(pixels, pw, 9999, px + 4, py + 4, size - 8, size - 8, 0x424242);
}

static void icon_draw_folder_small(unsigned int *pixels, int pw, int px, int py) {
    if (g_img_folder.pixels) {
        icon_blit(&g_img_folder, pixels, pw, px, py, 16);
        return;
    }
    gfx_fill_rect(pixels, pw, 9999, px, py, 8, 4, 0xD4A017);
    gfx_fill_rect(pixels, pw, 9999, px, py + 4, 16, 12, 0xFFCA28);
    gfx_fill_rect(pixels, pw, 9999, px + 4, py + 10, 8, 6, 0x1E88E5);
    gfx_fill_rect(pixels, pw, 9999, px + 6, py + 13, 4, 1, 0x0D47A1);
}

static void icon_draw_exec_small(unsigned int *pixels, int pw, int px, int py) {
    if (g_img_exec.pixels) {
        icon_blit(&g_img_exec, pixels, pw, px, py, 16);
        return;
    }
    gfx_fill_rect(pixels, pw, 9999, px + 2, py + 2, 12, 12, 0x424242);
}

static void icon_draw_file_small(unsigned int *pixels, int pw, int px, int py) {
    gfx_fill_rect(pixels, pw, 9999, px + 2, py, 12, 16, 0xFFFFFF);
    for (int y = 4; y < 14; y += 4)
        gfx_fill_rect(pixels, pw, 9999, px + 4, py + y, 8, 1, 0x9E9E9E);
}
#endif
