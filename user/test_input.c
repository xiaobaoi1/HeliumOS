#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include <stdint.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct fb_user_info fb;
    int r = fb_map(&fb);
    if (r < 0) { printf("fb_map: %d\n", r); return 1; }

    printf("fb: %ux%u pitch=%u bpp=%u addr=0x%x\n",
           fb.width, fb.height, fb.pitch, fb.bpp, fb.addr);

    if (fb_claim() < 0) { printf("fb_claim failed\n"); return 1; }
    printf("claimed. drawing 5 seconds...\n");

    uint32_t *screen = (uint32_t*)fb.addr;
    uint32_t pitch_px = fb.pitch / 4;

    for (int i = 0; i < 500; i++) {
        /* 清屏为黑 */
        for (uint32_t j = 0; j < fb.height; j++) {
            for (uint32_t k = 0; k < fb.width; k++) {
                screen[j * pitch_px + k] = 0x000000;
            }
        }

        /* 画三个移动的色块 */
        int ox = (i * 3) % (int)(fb.width - 200);
        int oy = (i * 2) % (int)(fb.height - 200);

        for (int j = 0; j < 100; j++) {
            for (int k = 0; k < 100; k++) {
                screen[(oy + j) * pitch_px + (ox + k)] = 0xFF0000;
                screen[(oy + j + 100) * pitch_px + (ox + k + 100)] = 0x00FF00;
            }
        }
        sleep_ms(10);
    }

    printf("done\n");
    return 0;
}