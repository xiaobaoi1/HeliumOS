#include <ata.h>
#include <pci.h>
#include <io.h>
#include <pmm.h>
#include <printf.h>
#include <string.h>
#include <stdint.h>

#define BMIDE_CMD     0
#define BMIDE_STATUS  2
#define BMIDE_PRDT    4

#define BMIDE_CMD_START   (1u << 0)
#define BMIDE_CMD_READ    (1u << 3)   /* 1 = 读（设备→内存） */

#define BMIDE_ST_ACTIVE   (1u << 0)
#define BMIDE_ST_ERROR    (1u << 1)
#define BMIDE_ST_IRQ      (1u << 2)

#define BMIDE_PRDT_PAGES  1
#define BMIDE_DMA_PAGES   16          /* 64KB */
#define BMIDE_MAX_SECTORS (BMIDE_DMA_PAGES * 4096 / 512)   /* 128 */

static uint16_t g_bmide_io = 0;
static int      g_bmide_ok = 0;
static uint32_t g_prdt_phys[2] = {0, 0};
static uint32_t g_dma_phys[2]  = {0, 0};

int ata_bmide_init(void) {
    const struct pci_device *dev = pci_find_class(0x01, 0x01);
    if (!dev) {
        kprintf("[BMIDE] no IDE controller\n");
        return -1;
    }

    uint32_t bar4 = pci_read_config(dev->bus, dev->slot, dev->func, 0x20);
    if (!(bar4 & 0x1)) {
        kprintf("[BMIDE] BAR4 not I/O space (0x%x)\n", bar4);
        return -1;
    }
    uint16_t io = (uint16_t)(bar4 & ~0x3);
    if (io == 0 || io == 0xFFFF) {
        kprintf("[BMIDE] BAR4 invalid (0x%x)\n", io);
        return -1;
    }

    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= 0x5;
    pci_write_config(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);

    g_bmide_io = io;

    for (int ch = 0; ch < 2; ch++) {
        uint32_t prdt = pmm_alloc_page();
        uint32_t dma  = pmm_alloc_pages(BMIDE_DMA_PAGES);
        if (!prdt || !dma) {
            if (prdt) pmm_free_page(prdt);
            if (dma)  pmm_free_pages(dma, BMIDE_DMA_PAGES);
            kprintf("[BMIDE] ch%d alloc failed\n", ch);
            return -1;
        }
        memset((void*)prdt, 0, 4096);
        memset((void*)dma,  0, BMIDE_DMA_PAGES * 4096);
        g_prdt_phys[ch] = prdt;
        g_dma_phys[ch]  = dma;
    }

    outb(io + 0x00, 0);
    outb(io + 0x08, 0);
    inb(io + 0x02); inb(io + 0x0A);
    outb(io + 0x02, 0x06);
    outb(io + 0x0A, 0x06);

    g_bmide_ok = 1;
    kprintf("[BMIDE] ready: io=0x%x prog_if=0x%02x prdt=%p/%p dma=%p/%p\n",
            io,
            pci_read_config(dev->bus, dev->slot, dev->func, 0x09) & 0xFF,
            (void*)g_prdt_phys[0], (void*)g_prdt_phys[1],
            (void*)g_dma_phys[0],  (void*)g_dma_phys[1]);
    return 0;
}

int ata_bmide_ready(void) { return g_bmide_ok; }

static int bmide_channel_of(uint16_t io_base) {
    if (io_base == ATA_PRIMARY_IO_BASE)   return 0;
    if (io_base == ATA_SECONDARY_IO_BASE) return 1;
    return -1;
}

static void bmide_stop(int ch) {
    uint16_t cmd_port = g_bmide_io + (ch ? 0x08 : 0x00);
    uint16_t st_port  = g_bmide_io + (ch ? 0x0A : 0x02);
    outb(cmd_port, inb(cmd_port) & ~BMIDE_CMD_START);
    for (int i = 0; i < 1000000; i++) {
        if (!(inb(st_port) & BMIDE_ST_ACTIVE)) break;
    }
}

static int bmide_issue(const struct ata_device *dev, int write,
                       uint32_t lba, uint32_t count) {
    int ch = bmide_channel_of(dev->io_base);
    if (ch < 0) return -1;

    uint16_t cmd_port  = g_bmide_io + (ch ? 0x08 : 0x00);
    uint16_t st_port   = g_bmide_io + (ch ? 0x0A : 0x02);
    uint16_t prdt_port = g_bmide_io + (ch ? 0x0C : 0x04);

    bmide_stop(ch);
    outl(prdt_port, g_prdt_phys[ch]);

    uint16_t io = dev->io_base;
    uint8_t drive_sel = 0xE0 | (dev->drive << 4) | ((lba >> 24) & 0x0F);

    /* 等 BSY 清 */
    for (int i = 0; i < 1000000; i++) {
        if (!(inb(io + ATA_REG_STATUS) & ATA_SR_BSY)) break;
    }
    outb(io + ATA_REG_DRIVE, drive_sel);
    /* 等 DRDY 置位 */
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(io + ATA_REG_STATUS);
        if ((st & ATA_SR_DRDY) && !(st & ATA_SR_BSY)) break;
    }

    outb(io + ATA_REG_SECTOR_COUNT, (count == 256) ? 0 : (uint8_t)count);
    outb(io + ATA_REG_LBA_LOW,  (lba)       & 0xFF);
    outb(io + ATA_REG_LBA_MID,  (lba >> 8)  & 0xFF);
    outb(io + ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);

    /* 1. 设方向位，START=0 */
    uint8_t dir_cmd = 0;
    if (!write) dir_cmd |= BMIDE_CMD_READ;
    outb(cmd_port, dir_cmd);

    /* 2. 发 ATA DMA 命令 */
    outb(io + ATA_REG_COMMAND, write ? 0xCA : 0xC8);

    /* 3. START=1 */
    outb(cmd_port, dir_cmd | BMIDE_CMD_START);

    for (int i = 0; i < 100000000; i++) {
        uint8_t st = inb(st_port);
        if (st & BMIDE_ST_ERROR) {
            kprintf("[BMIDE] ch%d DMA error st=0x%x io=0x%x cmd=0x%x\n",
                    ch, st, io, write ? 0xCA : 0xC8);
            bmide_stop(ch);
            return -1;
        }
        if (!(st & BMIDE_ST_ACTIVE)) {
            outb(st_port, BMIDE_ST_IRQ);
            for (int t = 0; t < 1000000; t++) {
                if (!(inb(io + ATA_REG_STATUS) & ATA_SR_BSY)) break;
            }
            if (inb(io + ATA_REG_STATUS) & ATA_SR_ERR) {
                kprintf("[BMIDE] ch%d IDE error st=0x%x\n",
                        ch, inb(io + ATA_REG_STATUS));
                return -1;
            }
            return 0;
        }
    }
    kprintf("[BMIDE] ch%d DMA timeout\n", ch);
    bmide_stop(ch);
    return -1;
}

static int g_dma_fail_count[2] = {0, 0};   /* per channel */

int ata_bmide_read_sectors(const struct ata_device *dev, uint32_t lba,
                           uint32_t count, uint8_t *buffer) {
    if (!g_bmide_ok || !dev || !buffer || count == 0) return -1;
    int ch = bmide_channel_of(dev->io_base);
    if (ch < 0) return -1;
    if (g_dma_fail_count[ch] >= 3) return -1;   /* 已失败 3 次，让上层回退 PIO */

    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > BMIDE_MAX_SECTORS) chunk = BMIDE_MAX_SECTORS;

        struct ata_prd *prd = (struct ata_prd*)g_prdt_phys[ch];
        prd->phys  = g_dma_phys[ch] | 0x1;     /* EOT 在 bit 0 */
        prd->count = (uint16_t)(chunk * 512 - 1);
        prd->flags = 0;

        if (bmide_issue(dev, 0, lba + done, chunk) < 0) {
            g_dma_fail_count[ch]++;
            return -1;
        }
        memcpy(buffer + done * 512, (void*)g_dma_phys[ch], chunk * 512);
        done += chunk;
    }
    g_dma_fail_count[ch] = 0;
    return 0;
}

int ata_bmide_write_sectors(const struct ata_device *dev, uint32_t lba,
                            uint32_t count, const uint8_t *buffer) {
    if (!g_bmide_ok || !dev || !buffer || count == 0) return -1;
    int ch = bmide_channel_of(dev->io_base);
    if (ch < 0) return -1;

    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > BMIDE_MAX_SECTORS) chunk = BMIDE_MAX_SECTORS;

        memcpy((void*)g_dma_phys[ch], buffer + done * 512, chunk * 512);

        struct ata_prd *prd = (struct ata_prd*)g_prdt_phys[ch];
        prd->phys  = g_dma_phys[ch] | 0x1;     /* EOT 在 bit 0 */
        prd->count = (uint16_t)(chunk * 512 - 1);
        prd->flags = 0;                        /* reserved 必须 0 */

        if (bmide_issue(dev, 1, lba + done, chunk) < 0) return -1;
        done += chunk;
    }
    return 0;
}

int ata_bmide_setup_device(const struct ata_device *dev) {
    if (!g_bmide_ok || !dev) return -1;
    uint16_t io = dev->io_base;
    uint8_t  drive_sel = 0xE0 | (dev->drive << 4);

    for (int i = 0; i < 1000000; i++) {
        if (!(inb(io + ATA_REG_STATUS) & ATA_SR_BSY)) break;
    }

    outb(io + ATA_REG_DRIVE, drive_sel);
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(io + ATA_REG_STATUS);
        if ((st & ATA_SR_DRDY) && !(st & ATA_SR_BSY)) break;
    }

    outb(io + ATA_REG_FEATURES, 0x03);
    outb(io + ATA_REG_SECTOR_COUNT, 0x22);
    outb(io + ATA_REG_COMMAND, 0xEF);

    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(io + ATA_REG_STATUS);
        if (!(st & ATA_SR_BSY)) {
            if (st & ATA_SR_ERR) {
                kprintf("[BMIDE] drive %d io=0x%x: SET FEATURES 0x22 failed (st=0x%x)\n",
                        dev->drive, io, st);
                return -1;
            }
            kprintf("[BMIDE] drive %d io=0x%x: DMA mode 2 enabled\n",
                    dev->drive, io);
            return 0;
        }
    }
    kprintf("[BMIDE] drive %d: SET FEATURES timeout\n", dev->drive);
    return -1;
}