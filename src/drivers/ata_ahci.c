#include <ata.h>
#include <pci.h>
#include <kmap.h>
#include <printf.h>
#include <stdint.h>

static uint32_t g_ahci_bar = 0;
static int      g_ahci_ok  = 0;

int ata_ahci_init(void) {
    const struct pci_device *dev = pci_find_class(0x01, 0x06);
    if (!dev) return -1;

    uint32_t bar5 = pci_read_config(dev->bus, dev->slot, dev->func, 0x24);
    if (bar5 & 0x1) {
        kprintf("[AHCI] BAR5 is I/O space, unsupported\n");
        return -1;
    }
    uint32_t mmio = bar5 & ~0xF;
    if (mmio == 0 || mmio >= 0x40000000) {
        kprintf("[AHCI] BAR5 invalid (0x%x)\n", mmio);
        return -1;
    }

    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= 0x5;
    pci_write_config(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);

    g_ahci_bar = mmio;
    g_ahci_ok  = 1;
    kprintf("[AHCI] found: abar=0x%x\n", mmio);
    return 0;
}

int ata_ahci_ready(void) { return g_ahci_ok; }