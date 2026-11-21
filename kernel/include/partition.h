#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>
#include <ata.h>

#define MBR_MAX_PRIMARY 4

struct mbr_partition {
    uint8_t  bootable;
    uint8_t  type;           /* MBR 分区类型字节，0x0B/0x0C = FAT32 */
    uint32_t start_lba;
    uint32_t sector_count;
};

/* 读 LBA 0，解析 MBR。返回分区数（0 表示无 MBR 或无有效项）。 */
int mbr_parse(const struct ata_device *dev,
              struct mbr_partition *out, int max);

#endif