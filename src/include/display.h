#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

#define DISP_MAX_COLS 200
#define DISP_MAX_ROWS 64

struct display_ops {
    const char *name;
    int cols;
    int rows;
    /* 只画字形，不处理光标 */
    void (*draw_char)(int x, int y, char c, uint8_t attr);
    void (*clear)(uint8_t attr);
    void (*scroll)(uint8_t attr);
    /* 画光标——不改字符缓冲。擦旧由 display_set_cursor 重绘字符完成。 */
    void (*draw_cursor)(int x, int y);
};

void display_init(void);
const struct display_ops *display_get(void);

int display_cols(void);
int display_rows(void);

void display_register(const struct display_ops *ops);
void display_switch(const struct display_ops *ops);

/* 带影子缓冲的高级 API */
void display_draw_char(int x, int y, char c, uint8_t attr);
void display_clear(uint8_t attr);
void display_scroll(uint8_t attr);
void display_set_cursor(int x, int y);

/* 后端注册 */
void vga_text_register(void);
const struct display_ops *fb_char_get_ops(void);

#endif