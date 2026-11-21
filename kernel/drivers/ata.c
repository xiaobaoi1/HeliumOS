#include <ata.h>
#include <io.h>
#include <printf.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ahci.h>


static struct ata_device g_devices[ATA_MAX_DEVICES];
static int g_count = 0;

static int g_ata_mode = ATA_MODE_PIO;

int ata_get_mode(void) { return g_ata_mode; }

static void ata_delay_400ns(uint16_t ctrl_base) {
    for (int i = 0; i < 4; i++) inb(ctrl_base);
}

static int ata_probe(uint16_t io_base, uint16_t ctrl_base, uint8_t drive) {
    outb(io_base + ATA_REG_DRIVE, 0xA0 | (drive << 4));
    ata_delay_400ns(ctrl_base);

    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if (status == 0xFF || status == 0x00) return 0;

    /* 用 IDENTIFY DEVICE 确认设备真的存在。
     * 空端口的 status 可能偶然非零，IDENTIFY 能过滤掉。 */
    outb(io_base + ATA_REG_SECTOR_COUNT, 0);
    outb(io_base + ATA_REG_LBA_LOW, 0);
    outb(io_base + ATA_REG_LBA_MID, 0);
    outb(io_base + ATA_REG_LBA_HIGH, 0);
    outb(io_base + ATA_REG_COMMAND, 0xEC);   /* IDENTIFY DEVICE */

    status = inb(io_base + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) return 0;

    /* 等 BSY 清零 */
    int i;
    for (i = 0; i < 100000; i++) {
        status = inb(io_base + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) break;
    }
    if (i == 100000) return 0;

    /* LBA_MID / LBA_HIGH 非 0 = ATAPI 或 SATA，不是纯 ATA */
    if (inb(io_base + ATA_REG_LBA_MID) != 0 ||
        inb(io_base + ATA_REG_LBA_HIGH) != 0) {
        return 0;
    }

    /* 等 DRQ 或 ERR */
    for (i = 0; i < 100000; i++) {
        status = inb(io_base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return 0;
        if (status & ATA_SR_DF)  return 0;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            /* 读走 256 word 的 identify 数据 */
            for (int k = 0; k < 256; k++) inw(io_base + ATA_REG_DATA);
            return 1;
        }
    }
    return 0;
}

void ata_init(void) {
    kprintf("[ATA] Initializing...\n");
    g_count = 0;

    /* 1. AHCI 优先。控制器存在且至少 1 个设备才算成功。 */
    if (ahci_init() == 0 && ahci_device_count() > 0) {
        g_ata_mode = ATA_MODE_AHCI;
        kprintf("[ATA] Mode: AHCI\n");
        for (int i = 0; i < ahci_device_count() && g_count < ATA_MAX_DEVICES; i++) {
            int port = ahci_get_port(i);
            if (port < 0) continue;
            struct ata_device *d = &g_devices[g_count];
            memset(d, 0, sizeof(*d));
            d->present   = 1;
            d->ahci_port = (uint8_t)port;
            g_count++;
        }
        kprintf("[ATA] %d drive(s) detected, mode=AHCI\n", g_count);
        return;
    }

    /* 2. PIO 探测（扫 IDE 命令块 0x1F0 / 0x170）。会填 g_devices。 */
    static const struct {
        uint16_t io_base;
        uint16_t ctrl_base;
        const char *name;
    } channels[2] = {
        { ATA_PRIMARY_IO_BASE,   ATA_PRIMARY_CTRL_BASE,   "primary" },
        { ATA_SECONDARY_IO_BASE, ATA_SECONDARY_CTRL_BASE, "secondary" },
    };
    for (int c = 0; c < 2; c++) {
        for (uint8_t drive = 0; drive < 2; drive++) {
            if (!ata_probe(channels[c].io_base, channels[c].ctrl_base, drive))
                continue;
            if (g_count >= ATA_MAX_DEVICES) break;
            struct ata_device *d = &g_devices[g_count];
            d->io_base   = channels[c].io_base;
            d->ctrl_base = channels[c].ctrl_base;
            d->drive     = drive;
            d->present   = 1;
            d->ahci_port = 0xFF;
            g_count++;
        }
    }

    /* 3. PIO 探测到盘，才尝试升级到 BMIDE（避免 IDE 空盘时误触发） */
    if (g_count > 0 && ata_bmide_init() == 0) {
        g_ata_mode = ATA_MODE_BMIDE;
        for (int i = 0; i < g_count; i++) {
            ata_bmide_setup_device(&g_devices[i]);
        }
        kprintf("[ATA] Mode: BMIDE\n");
    } else {
        g_ata_mode = ATA_MODE_PIO;
        kprintf("[ATA] Mode: PIO\n");
    }

    kprintf("[ATA] %d drive(s) detected, mode=%s\n",
            g_count,
            g_ata_mode == ATA_MODE_BMIDE ? "BMIDE" : "PIO");
}

int ata_device_count(void) { return g_count; }

const struct ata_device *ata_get_device(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_devices[idx];
}

static int ata_wait_bsy(uint16_t io_base) {
    uint8_t status;
    int timeout = 1000000;
    while (timeout-- > 0) {
        status = inb(io_base + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) return 1;
    }
    return 0;
}

static int ata_wait_drq(uint16_t io_base) {
    uint8_t status;
    int timeout = 1000000;
    while (timeout-- > 0) {
        status = inb(io_base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return 0;
        if (status & ATA_SR_DF)  return 0;
        if (status == 0xFF)      return 0;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 1;
    }
    return 0;
}

int ata_read_sector(const struct ata_device *dev, uint32_t lba, uint8_t *buffer) {
    return ata_read_sectors(dev, lba, 1, buffer);
}

int ata_read_sectors(const struct ata_device *dev, uint32_t lba,
                     uint32_t count, uint8_t *buffer) {
    if (!dev || !dev->present || !buffer || count == 0) {
        if (buffer && count > 0) memset(buffer, 0, count * 512);
        return -1;
    }

    if (dev->ahci_port != 0xFF) {
        uint32_t done = 0;
        uint32_t max_per = AHCI_DMA_BUF_SIZE / 512;   /* 128 */
        while (done < count) {
            uint32_t chunk = count - done;
            if (chunk > max_per) chunk = max_per;
            if (ahci_read_sectors(dev->ahci_port, lba + done, chunk,
                                  buffer + done * 512) < 0) {
                memset(buffer, 0, count * 512);
                return -1;
            }
            done += chunk;
        }
        return 0;
    }

    if (g_ata_mode == ATA_MODE_BMIDE) {
        if (ata_bmide_read_sectors(dev, lba, count, buffer) == 0) {
            return 0;
        }
        /* DMA 失败，fall through 到 PIO */
    }

    /* 原 PIO 逻辑 */
    if (count > 256) { memset(buffer, 0, count*512); return -1; }
    uint16_t io = dev->io_base;
    uint8_t drive_sel = 0xE0 | (dev->drive << 4) | ((lba >> 24) & 0x0F);

    for (int retry = 0; retry < 3; retry++) {
        outb(io + ATA_REG_DRIVE, drive_sel);
        outb(io + ATA_REG_SECTOR_COUNT, (count == 256) ? 0 : (uint8_t)count);
        outb(io + ATA_REG_LBA_LOW,  (lba)       & 0xFF);
        outb(io + ATA_REG_LBA_MID,  (lba >> 8)  & 0xFF);
        outb(io + ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);
        outb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

        int ok = 1;
        for (uint32_t s = 0; s < count; s++) {
            if (!ata_wait_bsy(io)) { ok = 0; break; }
            if (!ata_wait_drq(io)) { ok = 0; break; }
            uint16_t *p = (uint16_t*)(buffer + s * 512);
            for (int i = 0; i < 256; i++)
                p[i] = inw(io + ATA_REG_DATA);
        }
        if (!ok) continue;
        if (!ata_wait_bsy(io)) continue;
        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) continue;
        return 0;
    }

    memset(buffer, 0, count * 512);
    return -1;
}

int ata_write_sector(const struct ata_device *dev, uint32_t lba,
                     const uint8_t *buffer) {
    if (!dev || !dev->present || !buffer) return -1;
    if (dev->ahci_port != 0xFF) {
        return ahci_write_sectors(dev->ahci_port, lba, 1, buffer);
    }
    if (g_ata_mode == ATA_MODE_BMIDE) {
        return ata_bmide_write_sectors(dev, lba, 1, buffer);
    }

    uint16_t io = dev->io_base;
    uint8_t drive_sel = 0xE0 | (dev->drive << 4) | ((lba >> 24) & 0x0F);

    for (int retry = 0; retry < 3; retry++) {
        if (!ata_wait_bsy(io)) continue;

        outb(io + ATA_REG_DRIVE, drive_sel);
        outb(io + ATA_REG_SECTOR_COUNT, 1);
        outb(io + ATA_REG_LBA_LOW,  (lba)       & 0xFF);
        outb(io + ATA_REG_LBA_MID,  (lba >> 8)  & 0xFF);
        outb(io + ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);
        outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

        if (!ata_wait_bsy(io)) continue;
        if (!ata_wait_drq(io)) continue;

        const uint16_t *p = (const uint16_t*)buffer;
        for (int i = 0; i < 256; i++) {
            outw(io + ATA_REG_DATA, p[i]);
        }

        if (!ata_wait_bsy(io)) continue;

        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) continue;

        return 0;
    }

    kprintf("[ATA] Failed to write sector %d after retries.\n", lba);
    return -1;
}

void ata_flush(const struct ata_device *dev) {
    if (!dev || !dev->present) return;
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_FLUSH);
    ata_wait_bsy(dev->io_base);
}