#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/* 主 IDE 通道端口 */
#define ATA_PRIMARY_IO_BASE  0x1F0
#define ATA_PRIMARY_CTRL_BASE 0x3F6

/* 寄存器偏移（相对于 I/O Base） */
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

/* 状态位 */
#define ATA_SR_BSY           0x80
#define ATA_SR_DRQ           0x08
#define ATA_SR_ERR           0x01

/* 初始化 ATA 驱动（检测设备） */
void ata_init(void);

/* 读取一个扇区（LBA28，512 字节）到 buffer */
void ata_read_sector(uint32_t lba, uint8_t *buffer);

#endif