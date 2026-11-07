#include <pci.h>
#include <io.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static struct pci_device g_devices[PCI_MAX_DEVICES];
static int g_count = 0;

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func << 8)
                  | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func,
                      uint8_t off, uint32_t val) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func << 8)
                  | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

static void scan_bus(uint8_t bus);

static void scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t vend_dev = pci_read_config(bus, slot, func, PCI_VENDOR_ID);
    uint16_t vendor = vend_dev & 0xFFFF;
    if (vendor == 0xFFFF) return;
    if (g_count >= PCI_MAX_DEVICES) return;

    struct pci_device *d = &g_devices[g_count];

    d->bus       = bus;
    d->slot      = slot;
    d->func      = func;
    d->vendor_id = vendor;
    d->device_id = (vend_dev >> 16) & 0xFFFF;

    uint32_t class_rev = pci_read_config(bus, slot, func, PCI_REVISION);
    d->revision   = class_rev & 0xFF;
    d->prog_if    = (class_rev >> 8) & 0xFF;
    d->subclass   = (class_rev >> 16) & 0xFF;
    d->class_code = (class_rev >> 24) & 0xFF;

    uint32_t ht = pci_read_config(bus, slot, func, PCI_HEADER_TYPE);
    d->header_type = (ht >> 16) & 0xFF;

    for (int i = 0; i < 6; i++) {
        d->bar[i] = pci_read_config(bus, slot, func, PCI_BAR0 + i * 4);
    }

    uint32_t irq = pci_read_config(bus, slot, func, PCI_INTERRUPT_LINE);
    d->interrupt_line = irq & 0xFF;
    d->interrupt_pin  = (irq >> 8) & 0xFF;

    KLOG_DBG("[PCI] %02x:%02x.%x vend=%04x dev=%04x class=%02x:%02x progif=%02x\n",
            bus, slot, func,
            d->vendor_id, d->device_id,
            d->class_code, d->subclass, d->prog_if);

    g_count++;

    /* PCI-to-PCI 桥：递归扫下级 bus */
    if ((d->header_type & 0x7F) == 0x01) {
        uint32_t bus_nums = pci_read_config(bus, slot, func, 0x18);
        uint8_t secondary = (bus_nums >> 8) & 0xFF;
        if (secondary != 0 && secondary != bus) {
            scan_bus(secondary);
        }
    }
}

static void scan_device(uint8_t bus, uint8_t slot) {
    uint32_t vend = pci_read_config(bus, slot, 0, PCI_VENDOR_ID);
    if ((vend & 0xFFFF) == 0xFFFF) return;

    scan_function(bus, slot, 0);

    /* 多功能设备：header_type bit 7 = 1 */
    uint32_t ht = pci_read_config(bus, slot, 0, PCI_HEADER_TYPE);
    if ((ht >> 16) & 0x80) {
        for (uint8_t f = 1; f < 8; f++) {
            uint32_t v = pci_read_config(bus, slot, f, PCI_VENDOR_ID);
            if ((v & 0xFFFF) != 0xFFFF) {
                scan_function(bus, slot, f);
            }
        }
    }
}

static void scan_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        scan_device(bus, slot);
    }
}

void pci_init(void) {
    g_count = 0;
    memset(g_devices, 0, sizeof(g_devices));

    kprintf("[PCI] Scanning bus 0...\n");
    scan_bus(0);
    kprintf("[PCI] Found %d device(s)\n", g_count);
}

int pci_device_count(void) { return g_count; }

const struct pci_device *pci_device_at(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_devices[idx];
}

const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < g_count; i++) {
        if (g_devices[i].class_code == class_code &&
            g_devices[i].subclass == subclass) {
            return &g_devices[i];
        }
    }
    return NULL;
}

const struct pci_device *pci_find(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < g_count; i++) {
        if (g_devices[i].vendor_id == vendor &&
            g_devices[i].device_id == device) {
            return &g_devices[i];
        }
    }
    return NULL;
}