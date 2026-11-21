#include <audio.h>
#include <pci.h>
#include <io.h>
#include <printf.h>
#include <stdint.h>

#define AC97_VENDOR          0x8086
#define AC97_DEVICE          0x2415
#define AC97_REG_RESET       0x00
#define AC97_REG_MASTER_VOL  0x02
#define AC97_REG_PCM_VOL     0x18

static uint16_t ac97_read(uint16_t base, uint8_t reg) {
    return inw((uint16_t)(base + reg));
}

static void ac97_write(uint16_t base, uint8_t reg, uint16_t val) {
    outw((uint16_t)(base + reg), val);
    (void)inb(0x80);
}

int audio_init(void) {
    const struct pci_device *dev = pci_find(AC97_VENDOR, AC97_DEVICE);
    if (!dev) {
        const struct pci_device *hda = pci_find_class(0x04, 0x03);
        if (hda) {
            uint32_t bar = hda->bar[0];
            uint32_t phys = PCI_BAR_MEM_ADDR(bar);
            if (!(bar & 1) && pci_bar_accessible(phys)) {
                volatile uint32_t *mmio = (volatile uint32_t*)phys;
                uint16_t gcap = (uint16_t)(mmio[0] & 0xFFFF);
                uint16_t states = (uint16_t)(mmio[2] & 0xFFFF);
                kprintf("[AUDIO] HDA loaded: vendor=%04x device=%04x "
                        "gcap=%04x codecs=%04x\n",
                        hda->vendor_id, hda->device_id, gcap, states);
                return 0;
            }
            kprintf("[AUDIO] HDA found but MMIO unavailable\n");
            return -1;
        }
        kprintf("[AUDIO] no supported sound card found\n");
        return -1;
    }

    uint32_t bar0 = dev->bar[0];
    if (!(bar0 & 1)) {
        kprintf("[AUDIO] AC97 BAR0 is not I/O space\n");
        return -1;
    }
    uint16_t io = (uint16_t)PCI_BAR_IO_ADDR(bar0);

    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func,
                                   PCI_COMMAND);
    cmd |= 0x5;
    pci_write_config(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);

    ac97_write(io, AC97_REG_RESET, 1);
    for (volatile int i = 0; i < 100000; i++) { }
    uint16_t id = ac97_read(io, 0x28);
    uint16_t vol = ac97_read(io, AC97_REG_MASTER_VOL);
    if (id == 0xFFFF && vol == 0xFFFF) {
        kprintf("[AUDIO] AC97 at I/O 0x%x probe failed\n", io);
        return -1;
    }

    ac97_write(io, AC97_REG_MASTER_VOL, 0x0000);
    ac97_write(io, AC97_REG_PCM_VOL, 0x0000);
    kprintf("[AUDIO] AC97 loaded successfully: vendor=%04x device=%04x "
            "io=0x%x ext_id=%04x\n",
            dev->vendor_id, dev->device_id, io, id);
    return 0;
}
