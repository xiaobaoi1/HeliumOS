#include "syscall.h"
#include <gfx.h>
#include <stdint.h>
#include "stdio.h"
#include "string.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct gfx_info info;
    if (gfx_get_info(&info) < 0) {
        printf("gfx not available\n");
        return 1;
    }
    printf("gfx: %ux%u bpp=%u\n", info.width, info.height, info.bpp);

    /* 一个红色矩形 */
    gfx_fill_rect(100, 100, 200, 100, 0xFF0000);
    /* 绿色 */
    gfx_fill_rect(350, 100, 200, 100, 0x00FF00);
    /* 蓝色 */
    gfx_fill_rect(600, 100, 200, 100, 0x0000FF);

    /* blit 测试：40x30 缓冲区，棋盘格 */
    uint32_t buf[40 * 30];
    for (int j = 0; j < 30; j++) {
        for (int i = 0; i < 40; i++) {
            buf[j * 40 + i] = ((i ^ j) & 1) ? 0xFFFFFF : 0x000000;
        }
    }
    gfx_blit(100, 300, 40, 30, buf);

    printf("drawn\n");
    return 0;
}