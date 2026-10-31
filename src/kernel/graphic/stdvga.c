#include "stdvga.h"

#include <pci.h>
#include <fb.h>
#include <io.h>
#include <printf.h>
#include <stdint.h>

/* Bochs/QEMU VBE DISPI 寄存器（stdvga 的兼容接口） */
#define VBE_DISPI_IOPORT_INDEX 0x1CE
#define VBE_DISPI_IOPORT_DATA  0x1CF
#define VBE_DISPI_ID0          0xB0C0
#define VBE_DISPI_INDEX_ID     0x0

static int g_loaded;
static const char *g_error = "not probed";

static uint16_t vbe_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

int stdvga_init(void) {
    g_loaded = 0;
    g_error = "unknown error";

    const struct pci_device *pci = pci_find(STDVGA_PCI_VENDOR,
                                            STDVGA_PCI_DEVICE);
    uint16_t vbe_id = vbe_read(VBE_DISPI_INDEX_ID);
    int vbe_ok = (vbe_id >= VBE_DISPI_ID0 && vbe_id <= VBE_DISPI_ID0 + 0x0F);
    struct fb_info *fb = fb_get();

    kprintf("[STDVGA] pci=%s vbe_id=0x%04x fb_valid=%d\n",
            pci ? "yes" : "no", vbe_id, (fb && fb->valid) ? 1 : 0);

    if (!pci) {
        g_error = "QEMU/Bochs stdvga PCI device (1234:1111) not found";
        return -1;
    }
    if (!fb || !fb->valid) {
        g_error = "no boot framebuffer available";
        return -1;
    }

    /* VBE 只用于确认 DISPI 接口存在；实际分辨率以 bootloader
     * 建立的 framebuffer 为准（VBE 模式寄存器在不同环境下可能不可读）。 */
    if (vbe_ok) {
        kprintf("[STDVGA] VBE DISPI id=0x%04x; framebuffer %ux%u %ubpp pitch=%u\n",
                vbe_id, fb->width, fb->height, fb->bpp, fb->pitch);
    } else {
        kprintf("[STDVGA] VBE registers not readable; using boot framebuffer\n");
    }

    /* 只绑定 bootloader 已经建立的线性 framebuffer，不重设模式。 */
    g_loaded = 1;
    g_error = "";
    return 0;
}

int stdvga_loaded(void) {
    return g_loaded;
}

const char *stdvga_error(void) {
    return g_error;
}
