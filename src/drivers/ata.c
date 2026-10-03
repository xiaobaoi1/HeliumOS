#include <ata.h>
#include <io.h>
#include <printf.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static struct ata_device g_devices[ATA_MAX_DEVICES];
static int g_count = 0;

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

            kprintf("[ATA] %s %s detected (io=0x%x)\n",
                    channels[c].name, drive ? "slave" : "master",
                    channels[c].io_base);
            g_count++;
        }
    }

    if (g_count == 0) {
        kprintf("[ATA] No drives detected.\n");
    } else {
        kprintf("[ATA] %d drive(s) detected.\n", g_count);
    }
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
    if (!dev || !dev->present || !buffer) {
        if (buffer) memset(buffer, 0, 512);
        return -1;
    }
    uint16_t io = dev->io_base;
    uint8_t drive_sel = 0xE0 | (dev->drive << 4) | ((lba >> 24) & 0x0F);

    for (int retry = 0; retry < 3; retry++) {
        outb(io + ATA_REG_DRIVE, drive_sel);
        outb(io + ATA_REG_SECTOR_COUNT, 1);
        outb(io + ATA_REG_LBA_LOW,  (lba)       & 0xFF);
        outb(io + ATA_REG_LBA_MID,  (lba >> 8)  & 0xFF);
        outb(io + ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);
        outb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

        if (!ata_wait_bsy(io)) continue;
        if (!ata_wait_drq(io)) continue;

        for (int i = 0; i < 256; i++) {
            ((uint16_t*)buffer)[i] = inw(io + ATA_REG_DATA);
        }

        if (!ata_wait_bsy(io)) continue;

        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) continue;

        return 0;   /* 成功 */
    }

    kprintf("[ATA] Failed to read sector %d after retries.\n", lba);
    memset(buffer, 0, 512);
    return -1;
}

int ata_write_sector(const struct ata_device *dev, uint32_t lba, const uint8_t *buffer) {
    if (!dev || !dev->present || !buffer) return -1;
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