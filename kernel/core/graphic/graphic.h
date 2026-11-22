#ifndef HELIUM_GRAPHIC_H
#define HELIUM_GRAPHIC_H

#include <stdint.h>

/* 图形设备抽象：目前只有 stdvga（QEMU/Bochs 标准虚拟显卡）。
 * 驱动只“加载/绑定”到 bootloader 已建立的线性 framebuffer，
 * 不重新设置显示模式，避免破坏 GRUB 已经配置好的分辨率。 */
struct graphic_device {
    const char *name;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  loaded;
};

/* 在 fb_init + pci_init 之后调用。会通过 kprintf 把结果打到串口。 */
void graphic_init(void);
struct graphic_device *graphic_get(void);

#endif
