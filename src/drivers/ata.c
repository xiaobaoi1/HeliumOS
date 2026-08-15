#include <ata.h>
#include <io.h>
#include <printf.h>
#include <stdint.h>

static int ata_wait_bsy(void) {
    uint8_t status;
    int timeout = 1000000;
    do {
        status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        timeout--;
    } while ((status & ATA_SR_BSY) && timeout > 0);
    return (status & ATA_SR_BSY) ? 0 : 1;
}

static int ata_wait_drq(void) {
    uint8_t status;
    int timeout = 1000000;
    do {
        status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        timeout--;
    } while (!(status & ATA_SR_DRQ) && (status & ATA_SR_BSY) == 0 && timeout > 0);
    if (timeout == 0 || (status & ATA_SR_ERR)) {
        return 0;
    }
    return 1;
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