#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/* 通道端口基址 */
#define ATA_PRIMARY_IO_BASE    0x1F0
#define ATA_PRIMARY_CTRL_BASE  0x3F6
#define ATA_SECONDARY_IO_BASE  0x170
#define ATA_SECONDARY_CTRL_BASE 0x376

#define ATA_MAX_DEVICES 4

/* 寄存器偏移 */
#define ATA_REG_DATA         0
#define ATA_REG_ERROR        1
#define ATA_REG_FEATURES     1
#define ATA_REG_SECTOR_COUNT 2
#define ATA_REG_LBA_LOW      3
#define ATA_REG_LBA_MID      4
#define ATA_REG_LBA_HIGH     5
#define ATA_REG_DRIVE        6
#define ATA_REG_COMMAND      7
#define ATA_REG_STATUS       7

/* 命令 */
#define ATA_CMD_READ_PIO     0x20
#define ATA_CMD_WRITE_PIO    0x30
#define ATA_CMD_FLUSH        0xE7

/* 状态位 */
#define ATA_SR_BSY           0x80
#define ATA_SR_DRQ           0x08
#define ATA_SR_ERR           0x01
#define ATA_SR_DF            0x20

/* 一个 ATA 通道上的一个设备 */
struct ata_device {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t  drive;      /* 0 = master, 1 = slave */
    uint8_t  present;
};

/* 探测四个位置（主/从 × master/slave） */
void ata_init(void);

int                     ata_device_count(void);
const struct ata_device *ata_get_device(int idx);

/* LBA28 读写，每次一扇区 */
void ata_read_sector(const struct ata_device *dev, uint32_t lba, uint8_t *buffer);
void ata_write_sector(const struct ata_device *dev, uint32_t lba, const uint8_t *buffer);
void ata_flush(const struct ata_device *dev);

#endif