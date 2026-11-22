#ifndef DESKTOP_ICONS_H
#define DESKTOP_ICONS_H

#include "syscall.h"
#include "stdlib.h"
#include "stdio.h"
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
    img->pixels = dst;       /* 原加载缓冲不在这里释放，数据已复制到 BSS */
}

static void icons_load(void) {
    mui_bmp_load_icon(&g_img_folder, "SYS:\\folder.bmp");
    mui_bmp_load_icon(&g_img_exec, "SYS:\\exec.bmp");
    icons_pin(&g_img_folder, g_folder_icon_px);
    icons_pin(&g_img_exec, g_exec_icon_px);
}

/* 壁纸/图标绘制是裸像素缓冲，这里临时包一个 surface 交给 mui_bmp_blit。 */
static void icon_blit(struct mui_bitmap *img, unsigned int *pixels, int pw,
                      int ph, int px, int py, int size) {
    if (!img->pixels || !pixels || pw <= 0 || ph <= 0 || size <= 0) return;

    struct mui_surface s;
    memset(&s, 0, sizeof(s));
    s.pixels = pixels;
    s.w = pw;
    s.h = ph;
    s.hdr = NULL;
    mui_bmp_blit(img, &s, px, py, size, size);
}

/* ==================== 图标渲染 ==================== */

static void icon_draw_folder_large(unsigned int *pixels, int pw, int ph,
                                   int px, int py, int size) {
    if (g_img_folder.pixels) {
        icon_blit(&g_img_folder, pixels, pw, ph, px, py, size);
        return;
    }
    int mid = size / 2;
    int q = size / 4;
    gfx_fill_rect(pixels, pw, ph, px, py, mid, q, 0xD4A017);
    gfx_fill_rect(pixels, pw, ph, px, py + q, size, size - q, 0xFFCA28);
    gfx_fill_rect(pixels, pw, ph, px + size/4, py + size*2/3, size/2, size/3, 0x1E88E5);
    gfx_fill_rect(pixels, pw, ph, px + size*1/3, py + size*5/6, size/3, 1, 0x0D47A1);
}

static void icon_draw_exec_large(unsigned int *pixels, int pw, int ph,
                                 int px, int py, int size) {
    if (g_img_exec.pixels) {
        icon_blit(&g_img_exec, pixels, pw, ph, px, py, size);
        return;
    }
    gfx_fill_rect(pixels, pw, ph, px, py, size, size, 0xE0E0E0);
    gfx_fill_rect(pixels, pw, ph, px + 4, py + 4, size - 8, size - 8, 0x424242);
}

static void icon_draw_folder_small(unsigned int *pixels, int pw, int ph,
                                   int px, int py) {
    if (g_img_folder.pixels) {
        icon_blit(&g_img_folder, pixels, pw, ph, px, py, 16);
        return;
    }
    gfx_fill_rect(pixels, pw, ph, px, py, 8, 4, 0xD4A017);
    gfx_fill_rect(pixels, pw, ph, px, py + 4, 16, 12, 0xFFCA28);
    gfx_fill_rect(pixels, pw, ph, px + 4, py + 10, 8, 6, 0x1E88E5);
    gfx_fill_rect(pixels, pw, ph, px + 6, py + 13, 4, 1, 0x0D47A1);
}

static void icon_draw_exec_small(unsigned int *pixels, int pw, int ph,
                                 int px, int py) {
    if (g_img_exec.pixels) {
        icon_blit(&g_img_exec, pixels, pw, ph, px, py, 16);
        return;
    }
    gfx_fill_rect(pixels, pw, ph, px + 2, py + 2, 12, 12, 0x424242);
}

static void icon_draw_file_small(unsigned int *pixels, int pw, int ph,
                                 int px, int py) {
    gfx_fill_rect(pixels, pw, ph, px + 2, py, 12, 16, 0xFFFFFF);
    for (int y = 4; y < 14; y += 4)
        gfx_fill_rect(pixels, pw, ph, px + 4, py + y, 8, 1, 0x9E9E9E);
}

/* ============================================================
 *  自定义程序/文件图标
 *
 *  配置保存在 SYS:\ICONS.CFG，每行 "完整路径|BMP路径"。
 *  图标像素解码后缩放到 64x64 并固定在 BSS，避免后续
 *  mmap/munmap 干扰；绘制时再缩放到 48/16 像素。
 * ============================================================ */

#define CI_MAX          12
#define CI_DIM          64
#define CI_ICON_BASE    100
#define CI_MAP_MAX      64
#define CI_CFG_PATH     "SYS:\\ICONS.CFG"

struct ci_slot {
    int used;
    unsigned int trans;
    char key[128];
    unsigned int px[CI_DIM * CI_DIM];
};

struct ci_map {
    char key[128];
    char path[128];
};

static struct ci_slot g_ci_slots[CI_MAX];
static struct ci_map  g_ci_map[CI_MAP_MAX];
static int g_ci_map_count = 0;
static int g_ci_rr = 0;

static int ci_key_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int ci_slot_find(const char *key) {
    for (int i = 0; i < CI_MAX; i++)
        if (g_ci_slots[i].used && ci_key_eq(g_ci_slots[i].key, key)) return i;
    return -1;
}

static int ci_map_find(const char *key) {
    for (int i = 0; i < g_ci_map_count; i++)
        if (ci_key_eq(g_ci_map[i].key, key)) return i;
    return -1;
}

static const char *ci_map_path(const char *key) {
    int i = ci_map_find(key);
    return i >= 0 ? g_ci_map[i].path : NULL;
}

static int ci_slot_load(int slot, const char *key, const char *path) {
    struct mui_bitmap img;
    memset(&img, 0, sizeof(img));
    if (!mui_bmp_load_icon(&img, path) || !img.pixels ||
        img.w <= 0 || img.h <= 0) {
        g_ci_slots[slot].used = 0;
        return -1;
    }

    g_ci_slots[slot].trans = img.trans;
    for (int y = 0; y < CI_DIM; y++) {
        int sy = y * img.h / CI_DIM;
        if (sy >= img.h) sy = img.h - 1;
        for (int x = 0; x < CI_DIM; x++) {
            int sx = x * img.w / CI_DIM;
            if (sx >= img.w) sx = img.w - 1;
            g_ci_slots[slot].px[y * CI_DIM + x] =
                img.pixels[sy * img.w + sx];
        }
    }
    strncpy(g_ci_slots[slot].key, key, sizeof(g_ci_slots[slot].key) - 1);
    g_ci_slots[slot].key[sizeof(g_ci_slots[slot].key) - 1] = '\0';
    g_ci_slots[slot].used = 1;
    mui_bmp_free(&img);
    return 0;
}

static int ci_assign(const char *key, const char *path) {
    int slot = ci_slot_find(key);
    if (slot < 0) {
        for (int i = 0; i < CI_MAX; i++) {
            if (!g_ci_slots[i].used) { slot = i; break; }
        }
    }
    if (slot < 0) {
        slot = g_ci_rr % CI_MAX;
        g_ci_rr++;
    }
    return ci_slot_load(slot, key, path);
}

/* 返回图标槽编号；没有自定义图标返回 -1。 */
static int custom_icon_lookup(const char *key) {
    if (!key || !key[0]) return -1;
    if (!ci_map_path(key)) return -1;
    int slot = ci_slot_find(key);
    if (slot < 0) {
        const char *p = ci_map_path(key);
        if (!p) return -1;
        ci_assign(key, p);
        slot = ci_slot_find(key);
    }
    return slot;
}

static void custom_icon_draw(int slot, unsigned int *pixels, int pw, int ph,
                             int px, int py, int size) {
    if (slot < 0 || slot >= CI_MAX || !g_ci_slots[slot].used) return;
    struct mui_bitmap img;
    memset(&img, 0, sizeof(img));
    img.pixels = g_ci_slots[slot].px;
    img.w = CI_DIM;
    img.h = CI_DIM;
    img.trans = g_ci_slots[slot].trans;
    icon_blit(&img, pixels, pw, ph, px, py, size);
}

static void ci_map_set(const char *key, const char *path) {
    int i = ci_map_find(key);
    if (i < 0) {
        if (g_ci_map_count >= CI_MAP_MAX) return;
        i = g_ci_map_count++;
    }
    strncpy(g_ci_map[i].key, key, sizeof(g_ci_map[i].key) - 1);
    g_ci_map[i].key[sizeof(g_ci_map[i].key) - 1] = '\0';
    strncpy(g_ci_map[i].path, path, sizeof(g_ci_map[i].path) - 1);
    g_ci_map[i].path[sizeof(g_ci_map[i].path) - 1] = '\0';
}

static void icons_config_load(void) {
    int fd = fs_open(CI_CFG_PATH, FS_O_RDONLY);
    if (fd < 0) return;

    static char buf[8192];
    int n = fs_read(fd, buf, sizeof(buf) - 1);
    fs_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (i < n && buf[i] != '\n' && buf[i] != '\r') continue;
        buf[i] = '\0';
        if (i > start) {
            char *line = buf + start;
            char *bar = strchr(line, '|');
            if (bar) {
                *bar = '\0';
                char *key = line;
                char *path = bar + 1;
                if (key[0] && path[0]) {
                    ci_map_set(key, path);
                    ci_assign(key, path);
                }
            }
        }
        start = i + 1;
    }
}

static void icons_config_save(void) {
    int fd = fs_open(CI_CFG_PATH, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC);
    if (fd < 0) return;
    for (int i = 0; i < g_ci_map_count; i++) {
        char line[280];
        int len = snprintf(line, sizeof(line), "%s|%s\n",
                           g_ci_map[i].key, g_ci_map[i].path);
        if (len > 0) fs_write(fd, line, (unsigned int)len);
    }
    fs_close(fd);
}

/* 设置某个文件的自定义图标；失败返回 -1。 */
static int icons_set_custom(const char *key, const char *icon_path) {
    if (!key || !key[0] || !icon_path || !icon_path[0]) return -1;
    int fd = fs_open(icon_path, FS_O_RDONLY);
    if (fd < 0) return -1;
    fs_close(fd);

    ci_map_set(key, icon_path);
    if (ci_assign(key, icon_path) < 0) return -1;
    icons_config_save();
    return 0;
}
#endif
