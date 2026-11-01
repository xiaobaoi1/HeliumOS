#include "graphic.h"
#include "stdvga.h"

#include <fb.h>
#include <printf.h>
#include <string.h>
#include <driver.h>

static struct graphic_device g_graphic;

int graphic_init(void) {
    memset(&g_graphic, 0, sizeof(g_graphic));
    g_graphic.name = "stdvga";

    if (stdvga_init() == 0) {
        struct fb_info *fb = fb_get();
        g_graphic.width  = fb ? fb->width : 0;
        g_graphic.height = fb ? fb->height : 0;
        g_graphic.bpp    = fb ? fb->bpp : 0;
        g_graphic.loaded = 1;
        kprintf("[GRAPHIC] stdvga loaded successfully: %ux%u %ubpp\n",
                g_graphic.width, g_graphic.height, g_graphic.bpp);
        return 0;
    } else {
        g_graphic.loaded = 0;
        kprintf("[GRAPHIC] stdvga load failed: %s\n", stdvga_error());
        return -1;
    }
}
DRIVER_INIT(graphic_init);

struct graphic_device *graphic_get(void) {
    return &g_graphic;
}
