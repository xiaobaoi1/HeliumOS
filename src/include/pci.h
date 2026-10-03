#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES 64

/* 配置空间标准偏移 */
#define PCI_VENDOR_ID      0x00
#define PCI_DEVICE_ID      0x02
#define PCI_COMMAND        0x04
#define PCI_STATUS         0x06
#define PCI_REVISION       0x08
#define PCI_PROG_IF        0x09
#define PCI_SUBCLASS       0x0A
#define PCI_CLASS          0x0B
#define PCI_HEADER_TYPE    0x0E
#define PCI_BAR0           0x10
#define PCI_SECONDARY_BUS  0x19
#define PCI_INTERRUPT_LINE 0x3C
#define PCI_INTERRUPT_PIN  0x3D

/* class code */
#define PCI_CLASS_STORAGE  0x01
#define PCI_SUBCLASS_IDE   0x01

struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint8_t  revision;
    uint8_t  prog_if;
    uint8_t  subclass;
    uint8_t  class_code;
    uint8_t  header_type;
    uint8_t  interrupt_line;
    uint8_t  interrupt_pin;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar[6];
};

void pci_init(void);

int pci_device_count(void);
const struct pci_device *pci_device_at(int idx);

/* 查找第一个匹配的设备 */
const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass);
const struct pci_device *pci_find(uint16_t vendor, uint16_t device);

/* 配置空间读写（off 会按 dword 对齐） */
uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_write_config(uint8_t bus, uint8_t slot, uint8_t func,
                          uint8_t off, uint32_t val);

#endif