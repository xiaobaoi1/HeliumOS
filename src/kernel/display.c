#include <display.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>
#include <fb.h>
#include <mouse.h>

static const struct display_ops *g_display = NULL;
static uint16_t cell_buf[DISP_MAX_ROWS][DISP_MAX_COLS];
static int buf_cols = 0;
static int buf_rows = 0;
static int cur_x = -1;
static int cur_y = -1;

static uint32_t g_fb_owner = 0;

void display_set_owner(uint32_t pid) {
    uint32_t old = g_fb_owner;
    g_fb_owner = pid;

    /* 用户 WM 退出——内核恢复渲染，整屏重绘。
     * 此时 g_fb_owner 已清 0，display_flush 会真正执行。 */
    if (old != 0 && pid == 0) {
        display_dirty(0, 0, 10000, 10000);
        display_flush();
    }
}
uint32_t display_get_owner(void) { return g_fb_owner; }

void display_register(const struct display_ops *ops) {
    if (!ops) return;
    g_display = ops;
    buf_cols = ops->cols > DISP_MAX_COLS ? DISP_MAX_COLS : ops->cols;
    buf_rows = ops->rows > DISP_MAX_ROWS ? DISP_MAX_ROWS : ops->rows;
    kprintf("[DISP] Registered backend: %s (%dx%d)\n",
            ops->name, ops->cols, ops->rows);
}

void display_switch(const struct display_ops *ops) {
    if (!ops) return;

    /* 清空影子缓冲 */
    uint16_t blank = ((uint16_t)0x07 << 8) | ' ';
    for (int y = 0; y < DISP_MAX_ROWS; y++)
        for (int x = 0; x < DISP_MAX_COLS; x++)
            cell_buf[y][x] = blank;

    cur_x = -1;
    cur_y = -1;

    g_display = ops;
    buf_cols = ops->cols > DISP_MAX_COLS ? DISP_MAX_COLS : ops->cols;
    buf_rows = ops->rows > DISP_MAX_ROWS ? DISP_MAX_ROWS : ops->rows;

    /* 清物理屏 */
    if (ops->clear) ops->clear(0x07);

    kprintf("[DISP] Switched to: %s (%dx%d)\n",
            ops->name, ops->cols, ops->rows);
}

const struct display_ops *display_get(void) { return g_display; }
int display_cols(void) { return g_display ? g_display->cols : 80; }
int display_rows(void) { return g_display ? g_display->rows : 25; }

void display_draw_char(int x, int y, char c, uint8_t attr) {
    if (x < 0 || x >= buf_cols || y < 0 || y >= buf_rows) return;

    cell_buf[y][x] = ((uint16_t)attr << 8) | (uint8_t)c;
    display_dirty(x * 8, y * 16, (x + 1) * 8, (y + 1) * 16);

    if (g_display && g_display->draw_char) {
        g_display->draw_char(x, y, c, attr);
    }
}

void display_clear(uint8_t attr) {
    uint16_t cell = ((uint16_t)attr << 8) | ' ';
    for (int y = 0; y < buf_rows; y++)
        for (int x = 0; x < buf_cols; x++)
            cell_buf[y][x] = cell;
    
    display_dirty(0, 0, 10000, 10000);   /* 大值，被 flush 里 clamp */

    if (g_display && g_display->clear) {
        g_display->clear(attr);
    }

    cur_x = -1;
    cur_y = -1;
}

void display_scroll(uint8_t attr) {
    if (buf_rows < 2) return;

    /* 先擦旧光标——必须在 cell_buf 滚动之前。
     * 滚动后 cell_buf[cur_y][cur_x] 已经是别的字符。 */
    if (cur_x >= 0 && cur_y >= 0 &&
        cur_x < buf_cols && cur_y < buf_rows &&
        g_display && g_display->draw_char) {
        uint16_t cell = cell_buf[cur_y][cur_x];
        char c = cell & 0xFF;
        uint8_t a = (cell >> 8) & 0xFF;
        if (c == 0) c = ' ';
        g_display->draw_char(cur_x, cur_y, c, a);
    }

    /* 光标位置失效——下次 set_cursor 会重画 */
    cur_x = -1;
    cur_y = -1;

    /* 移动 cell_buf */
    for (int y = 1; y < buf_rows; y++)
        for (int x = 0; x < buf_cols; x++)
            cell_buf[y-1][x] = cell_buf[y][x];

    uint16_t cell = ((uint16_t)attr << 8) | ' ';
    for (int x = 0; x < buf_cols; x++)
        cell_buf[buf_rows-1][x] = cell;
    
    display_dirty(0, 0, 10000, 10000);

    /* 滚物理屏 */
    if (g_display && g_display->scroll) {
        g_display->scroll(attr);
    }
}

void display_set_cursor(int x, int y) {
    if (x == cur_x && y == cur_y) return;

    int old_x = cur_x;
    int old_y = cur_y;

    /* 擦旧：从缓冲重绘该格 */
    if (old_x >= 0 && old_y >= 0 &&
        old_x < buf_cols && old_y < buf_rows &&
        g_display && g_display->draw_char) {
        uint16_t cell = cell_buf[old_y][old_x];
        char c = cell & 0xFF;
        uint8_t attr = (cell >> 8) & 0xFF;
        if (c == 0) c = ' ';
        g_display->draw_char(old_x, old_y, c, attr);
    }

    cur_x = x;
    cur_y = y;

    /* 旧位置和新位置都要标脏 */
    if (old_x >= 0 && old_y >= 0) {
        display_dirty(old_x * 8, old_y * 16, (old_x + 1) * 8, (old_y + 1) * 16);
    }
    display_dirty(x * 8, y * 16, (x + 1) * 8, (y + 1) * 16);

    if (g_display && g_display->draw_cursor) {
        g_display->draw_cursor(x, y);
    }
}

void display_init(void) {
    vga_text_register();
}

/* ---------- 脏区 ---------- */

static int dirty_x0 = 0, dirty_y0 = 0;
static int dirty_x1 = -1, dirty_y1 = -1;   /* x1 < x0 表示空 */

void display_dirty(int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) return;

    if (dirty_x1 < dirty_x0) {
        /* 空脏区 */
        dirty_x0 = x0; dirty_y0 = y0;
        dirty_x1 = x1; dirty_y1 = y1;
    } else {
        if (x0 < dirty_x0) dirty_x0 = x0;
        if (y0 < dirty_y0) dirty_y0 = y0;
        if (x1 > dirty_x1) dirty_x1 = x1;
        if (y1 > dirty_y1) dirty_y1 = y1;
    }
}

/* ---------- 鼠标合成 ---------- */

#define MOUSE_W 8
#define MOUSE_H 12

static int      mouse_prev_x = -1;
static int      mouse_prev_y = -1;
static uint32_t mouse_save[MOUSE_W * MOUSE_H];   /* 鼠标下像素的备份 */

/* 箭头图案——8x12，1 = 白，0 = 透明 */
static const uint8_t mouse_cursor[MOUSE_H] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC,
    0xFE, 0xFF, 0xF8, 0xD8, 0x88, 0x00
};

/* 从 back 恢复鼠标旧位置的像素到 framebuffer */
static void mouse_restore(void) {
    if (mouse_prev_x < 0) return;
    struct fb_info *fb = fb_get();
    if (!fb || !fb->valid) return;

    for (int y = 0; y < MOUSE_H; y++) {
        for (int x = 0; x < MOUSE_W; x++) {
            int px = mouse_prev_x + x;
            int py = mouse_prev_y + y;
            if (px < 0 || py < 0 ||
                px >= (int)fb->width || py >= (int)fb->height) continue;
            uint32_t off = (py * fb->pitch + px * 4) / 4;
            fb->virt[off] = mouse_save[y * MOUSE_W + x];
        }
    }
    mouse_prev_x = -1;
    mouse_prev_y = -1;
}

/* 把鼠标位置的 framebuffer 像素存到 mouse_save，再画箭头 */
static void mouse_draw(int mx, int my) {
    struct fb_info *fb = fb_get();
    if (!fb || !fb->valid) return;

    for (int y = 0; y < MOUSE_H; y++) {
        for (int x = 0; x < MOUSE_W; x++) {
            int px = mx + x;
            int py = my + y;
            if (px < 0 || py < 0 ||
                px >= (int)fb->width || py >= (int)fb->height) {
                mouse_save[y * MOUSE_W + x] = 0;
                continue;
            }
            uint32_t off = (py * fb->pitch + px * 4) / 4;
            mouse_save[y * MOUSE_W + x] = fb->screen[off];
        }
    }

    for (int y = 0; y < MOUSE_H; y++) {
        uint8_t row = mouse_cursor[y];
        for (int x = 0; x < MOUSE_W; x++) {
            if (row & (0x80 >> x)) {
                int px = mx + x;
                int py = my + y;
                if (px < 0 || py < 0 ||
                    px >= (int)fb->width || py >= (int)fb->height) continue;
                uint32_t off = (py * fb->pitch + px * 4) / 4;
                fb->virt[off] = 0xFFFFFF;
            }
        }
    }

    mouse_prev_x = mx;
    mouse_prev_y = my;
}

/* ---------- flush ---------- */

void display_flush(void) {
    if (g_fb_owner != 0) return;
    struct fb_info *fb = fb_get();
    if (!fb || !fb->valid) return;

    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));

    mouse_restore();

    int has_dirty = (dirty_x1 > dirty_x0);
    if (has_dirty) {
        int x0 = dirty_x0, y0 = dirty_y0;
        int x1 = dirty_x1, y1 = dirty_y1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int)fb->width)  x1 = (int)fb->width;
        if (y1 > (int)fb->height) y1 = (int)fb->height;

        if (x1 > x0 && y1 > y0) {
            /* 1. back(tty) 脏区 -> screen */
            for (int y = y0; y < y1; y++) {
                uint32_t *src = fb->back + (y * fb->pitch) / 4 + x0;
                uint32_t *dst = fb->screen + (y * fb->pitch) / 4 + x0;
                memcpy(dst, src, (x1 - x0) * 4);
            }

            /* 3. screen 脏区 -> virt */
            for (int y = y0; y < y1; y++) {
                uint32_t *src = fb->screen + (y * fb->pitch) / 4 + x0;
                uint32_t *dst = fb->virt + (y * fb->pitch) / 4 + x0;
                memcpy(dst, src, (x1 - x0) * 4);
            }
        }
        dirty_x1 = -1;
    }

    struct mouse_state *m = mouse_get();
    if (m) mouse_draw(m->x, m->y);

    __asm__ volatile("push %0; popf" :: "r"(flags));
}