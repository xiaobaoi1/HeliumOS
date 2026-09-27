#include <ata.h>
#include <io.h>
#include <printf.h>
#include <stdint.h>

static int ata_wait_bsy(void) {
    uint8_t status;
    int timeout = 1000000;
    while (timeout-- > 0) {
        status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) return 1;
    }
    return 0;
}

static int ata_wait_drq(void) {
    uint8_t status;
    int timeout = 1000000;
    while (timeout-- > 0) {
        status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return 0;
        if (status & ATA_SR_DF)  return 0;   /* Device Fault */
        if (status == 0xFF)      return 0;   /* 设备离线 */
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 1;
    }
    return 0;
}

void ata_init(void) {
    kprintf("[ATA] Initializing...\n");
    uint8_t status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
    if (status == 0xFF) {
        kprintf("[ATA] No drive detected.\n");
        return;
    }
    kprintf("[ATA] Drive detected (status 0x%x).\n", status);
}

void ata_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!buffer) return;

    for (int retry = 0; retry < 3; retry++) {
        // 选择驱动器
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_SECTOR_COUNT, 1);
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_LOW, (lba & 0xFF));
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);

        outb(ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

        // 等待 BSY 清除
        if (!ata_wait_bsy()) {
            kprintf("[ATA] BSY timeout, retry %d\n", retry);
            continue;
        }
        if (!ata_wait_drq()) {
            kprintf("[ATA] DRQ timeout, retry %d\n", retry);
            continue;
        }

        // 读取数据
        for (int i = 0; i < 256; i++) {
            ((uint16_t*)buffer)[i] = inw(ATA_PRIMARY_IO_BASE + ATA_REG_DATA);
        }

         /* 等设备完成（BSY=0） */
        if (!ata_wait_bsy()) {
            kprintf("[ATA] read BSY timeout, retry %d\n", retry);
            continue;
        }

        // 检查错误位
        uint8_t status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            kprintf("[ATA] Error during read, retry %d\n", retry);
            continue;
        }
        return; // 成功
    }
    kprintf("[ATA] Failed to read sector %d after retries.\n", lba);
}
void ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (!buffer) return;

    for (int retry = 0; retry < 3; retry++) {
        /* 1. 等设备空闲，再写寄存器 */
        if (!ata_wait_bsy()) {
            kprintf("[ATA] write pre-BSY timeout, retry %d\n", retry);
            continue;
        }

        /* 2. 设参数 */
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_SECTOR_COUNT, 1);
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_LOW,  (lba)       & 0xFF);
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_MID,  (lba >> 8)  & 0xFF);
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);

        outb(ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

        /* 3. 等 BSY 清零 */
        if (!ata_wait_bsy()) {
            kprintf("[ATA] write BSY timeout, retry %d\n", retry);
            continue;
        }

        /* 4. 等 DRQ 置位 */
        if (!ata_wait_drq()) {
            kprintf("[ATA] write DRQ timeout, retry %d\n", retry);
            continue;
        }

        /* 5. 写 256 word */
        const uint16_t *p = (const uint16_t*)buffer;
        for (int i = 0; i < 256; i++) {
            outw(ATA_PRIMARY_IO_BASE + ATA_REG_DATA, p[i]);
        }

        /* 6. 等设备完成写盘（BSY=0） */
        if (!ata_wait_bsy()) {
            kprintf("[ATA] write post-BSY timeout, retry %d\n", retry);
            continue;
        }

        /* 7. 检查错误 */
        uint8_t status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            kprintf("[ATA] write ERR, retry %d\n", retry);
            continue;
        }
        return;
    }
    kprintf("[ATA] Failed to write sector %d after retries.\n", lba);
}

void ata_flush(void) {
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND, ATA_CMD_FLUSH);
    ata_wait_bsy();
}