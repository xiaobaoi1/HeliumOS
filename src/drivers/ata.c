#include <ata.h>
#include <io.h>
#include <printf.h>
#include <stdint.h>

/* 等待驱动器不忙 (BSY 位清除) */
static int ata_wait_bsy(void) {
    uint8_t status;
    int timeout = 1000000;
    do {
        status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        timeout--;
    } while ((status & ATA_SR_BSY) && timeout > 0);
    return (status & ATA_SR_BSY) ? 0 : 1;
}

/* 等待 DRQ 准备就绪（数据请求） */
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

/* 初始化：检测设备是否存在 */
void ata_init(void) {
    kprintf("[ATA] Initializing...\n");
    uint8_t status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
    if (status == 0xFF) {
        kprintf("[ATA] No drive detected.\n");
        return;
    }
    kprintf("[ATA] Drive detected (status 0x%x).\n", status);
}

/* 读取一个扇区（LBA28）*/
void ata_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!buffer) return;

    /* 选择驱动器（主盘，LBA 模式） */
    uint8_t drive_sel = 0xE0 | ((lba >> 24) & 0x0F);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_DRIVE, drive_sel);

    /* 发送参数：扇区数（1），LBA 低 24 位 */
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_SECTOR_COUNT, 1);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_LOW, (lba & 0xFF));
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);

    /* 发送读命令 */
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    /* 等待 BSY 清除 */
    if (!ata_wait_bsy()) {
        kprintf("[ATA] Read timeout (BSY).\n");
        return;
    }

    /* 等待 DRQ 置位 */
    if (!ata_wait_drq()) {
        kprintf("[ATA] Read timeout (DRQ).\n");
        return;
    }

    /* 读取 256 个 16 位字（512 字节） */
    uint16_t *data = (uint16_t*)buffer;
    for (int i = 0; i < 256; i++) {
        data[i] = inw(ATA_PRIMARY_IO_BASE + ATA_REG_DATA);
    }
}