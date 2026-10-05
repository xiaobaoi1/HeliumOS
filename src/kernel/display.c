#include <display.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>

static const struct display_ops *g_display = NULL;
static uint16_t cell_buf[DISP_MAX_ROWS][DISP_MAX_COLS];
static int buf_cols = 0;
static int buf_rows = 0;
static int cur_x = -1;
static int cur_y = -1;

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

    if (g_display && g_display->draw_char) {
        g_display->draw_char(x, y, c, attr);
    }
}

void display_clear(uint8_t attr) {
    uint16_t cell = ((uint16_t)attr << 8) | ' ';
    for (int y = 0; y < buf_rows; y++)
        for (int x = 0; x < buf_cols; x++)
            cell_buf[y][x] = cell;

    if (g_display && g_display->clear) {
        g_display->clear(attr);
    }

    cur_x = -1;
    cur_y = -1;
}

void display_scroll(uint8_t attr) {
    if (buf_rows < 2) return;

    for (int y = 1; y < buf_rows; y++)
        for (int x = 0; x < buf_cols; x++)
            cell_buf[y-1][x] = cell_buf[y][x];

    uint16_t cell = ((uint16_t)attr << 8) | ' ';
    for (int x = 0; x < buf_cols; x++)
        cell_buf[buf_rows-1][x] = cell;

    if (g_display && g_display->scroll) {
        g_display->scroll(attr);
    }
}

void display_set_cursor(int x, int y) {
    if (x == cur_x && y == cur_y) return;

    /* 擦旧：从缓冲重绘该格（字符覆盖光标像素） */
    if (cur_x >= 0 && cur_y >= 0 &&
        cur_x < buf_cols && cur_y < buf_rows &&
        g_display && g_display->draw_char) {
        uint16_t cell = cell_buf[cur_y][cur_x];
        char c = cell & 0xFF;
        uint8_t attr = (cell >> 8) & 0xFF;
        if (c == 0) c = ' ';
        g_display->draw_char(cur_x, cur_y, c, attr);
    }

    cur_x = x;
    cur_y = y;

    if (g_display && g_display->draw_cursor) {
        g_display->draw_cursor(x, y);
    }
}

void display_init(void) {
    vga_text_register();
}