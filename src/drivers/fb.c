#include <fb.h>
#include <multiboot2.h>
#include <kmap.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>
#include <vmm.h>

static struct fb_info g_fb;

void fb_init(uint32_t mb_addr) {
    memset(&g_fb, 0, sizeof(g_fb));

    if (!mb_addr) return;

    struct multiboot2_info *info = (struct multiboot2_info*)mb_addr;
    uint8_t *ptr = (uint8_t*)mb_addr + sizeof(struct multiboot2_info);
    uint8_t *end = (uint8_t*)mb_addr + info->total_size;

    while (ptr < end) {
        struct multiboot2_tag *tag = (struct multiboot2_tag*)ptr;
        if (tag->type == MULTIBOOT2_TAG_TYPE_END) break;

        if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
            struct multiboot2_tag_framebuffer *fbt =
                (struct multiboot2_tag_framebuffer*)tag;

            g_fb.width  = fbt->framebuffer_width;
            g_fb.height = fbt->framebuffer_height;
            g_fb.pitch  = fbt->framebuffer_pitch;
            g_fb.bpp    = fbt->framebuffer_bpp;
            g_fb.phys   = fbt->framebuffer_addr;

            kprintf("[FB] phys=0x%x %ux%u pitch=%u bpp=%u type=%u\n",
                    (uint32_t)fbt->framebuffer_addr,
                    fbt->framebuffer_width, fbt->framebuffer_height,
                    fbt->framebuffer_pitch,
                    fbt->framebuffer_bpp, fbt->framebuffer_type);

            if (fbt->framebuffer_type != 1) {
                kprintf("[FB] not RGB, unsupported\n");
                return;
            }
            if (fbt->framebuffer_bpp != 32) {
                kprintf("[FB] only 32bpp supported, got %u\n",
                        fbt->framebuffer_bpp);
                return;
            }
            if (fbt->framebuffer_addr > 0xFFFFFFFFULL) {
                kprintf("[FB] framebuffer above 4GB, unsupported\n");
                return;
            }

            /* kmap framebuffer 物理地址 */
            uint32_t total = fbt->framebuffer_pitch * fbt->framebuffer_height;
            void *virt = kmap((uint32_t)fbt->framebuffer_addr, total);
            if (!virt) {
                kprintf("[FB] kmap failed (%u bytes)\n", total);
                return;
            }
            g_fb.virt = (uint32_t*)virt;
            g_fb.valid = 1;
            /* 分配后台缓冲——pmm 拿物理连续页，kmap 到内核虚拟 */
            uint32_t px = fbt->framebuffer_pitch / 4 * fbt->framebuffer_height;
            uint32_t back_bytes = px * 4;
            uint32_t back_pages = (back_bytes + 0xFFF) / 0x1000;
            uint32_t back_phys = pmm_alloc_pages(back_pages);
            if (!back_phys) {
                kprintf("[FB] cannot alloc back buffer\n");
                return;
            }
            g_fb.back = (uint32_t*)kmap(back_phys, back_bytes);
            if (!g_fb.back) {
                kprintf("[FB] cannot kmap back buffer\n");
                return;
            }
            for (uint32_t i = 0; i < px; i++) g_fb.back[i] = 0x000000;
            
            uint32_t screen_phys = pmm_alloc_pages(back_pages);
            if (!screen_phys) {
                kprintf("[FB] cannot alloc screen buffer\n");
                return;
            }
            g_fb.screen = (uint32_t*)kmap(screen_phys, back_bytes);
            if (!g_fb.screen) {
                kprintf("[FB] cannot kmap screen buffer\n");
                return;
            }
            for (uint32_t i = 0; i < px; i++) g_fb.screen[i] = 0x000000;


            kprintf("[FB] mapped to %p\n", virt);
            return;
        }

        ptr += tag->size;
        while ((uintptr_t)ptr % 8 != 0) ptr++;
    }

    kprintf("[FB] no framebuffer tag\n");
}

struct fb_info *fb_get(void) {
    return &g_fb;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
    if (!g_fb.valid) return;
    if (x >= g_fb.width || y >= g_fb.height) return;
    uint32_t off = (y * g_fb.pitch + x * 4) / 4;
    g_fb.back[off] = rgb;
}

void fb_clear(uint32_t rgb) {
    if (!g_fb.valid) return;
    uint32_t px = (g_fb.pitch / 4) * g_fb.height;
    for (uint32_t i = 0; i < px; i++) g_fb.back[i] = rgb;
}