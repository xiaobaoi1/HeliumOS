#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>

/* ---------- 颜色常量 ---------- */
#define VGA_BLACK        0
#define VGA_BLUE         1
#define VGA_GREEN        2
#define VGA_CYAN         3
#define VGA_RED          4
#define VGA_MAGENTA      5
#define VGA_BROWN        6
#define VGA_LIGHT_GRAY   7
#define VGA_DARK_GRAY    8
#define VGA_LIGHT_BLUE   9
#define VGA_LIGHT_GREEN  10
#define VGA_LIGHT_CYAN   11
#define VGA_LIGHT_RED    12
#define VGA_LIGHT_MAGENTA 13
#define VGA_YELLOW       14
#define VGA_WHITE        15

#define VGA_DEFAULT_ATTR ((VGA_BLACK << 4) | VGA_LIGHT_GRAY)

/* VGA 文本模式尺寸 */
#define VGA_WIDTH   80
#define VGA_HEIGHT  25

/* VGA 的一个 cell：位置 + 属性 + 字符
 * vga dev 的 write 接受 vga_cell 序列 */
struct vga_cell {
    uint16_t x;
    uint16_t y;
    uint8_t  attr;
    char     c;
} __attribute__((packed));

/* VGA dev 的 ioctl 命令 */
#define VGA_IOCTL_CLEAR          1   /* arg = attr（uint8_t），整屏填属性空格 */
#define VGA_IOCTL_SET_HW_CURSOR  2   /* arg = (x << 16) | y，设置硬件光标 */
#define VGA_IOCTL_GET_HW_CURSOR  3   /* arg = int[2]，返回 (x, y) */
#define VGA_IOCTL_SCROLL         4   /* arg = attr，向上滚一行，最后一行清空 */

/* 初始化 VGA 并注册为 dev */
void screen_init(void);

#endif