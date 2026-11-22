#ifndef HELIUM_STDVGA_H
#define HELIUM_STDVGA_H

/* QEMU/Bochs 标准 VGA（-vga std）PCI ID */
#define STDVGA_PCI_VENDOR 0x1234
#define STDVGA_PCI_DEVICE 0x1111

/* 探测并“加载”stdvga：绑定到已有 framebuffer。
 * 返回 0 成功，-1 失败；失败原因由 stdvga_error() 给出。 */
int stdvga_init(void);
int stdvga_loaded(void);
const char *stdvga_error(void);

#endif
