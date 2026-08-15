#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

#define SECTOR_SIZE 512
#define FAT32_SIGNATURE 0xAA55

/* FAT32 BPB (BIOS Parameter Block) - 位于 LBA 2048 的引导扇区 */
struct fat32_bpb {
    uint8_t  jump_code[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    // FAT32 扩展部分
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
    uint8_t  boot_code[420];
    uint16_t signature;
} __attribute__((packed));

/* FAT32 目录项 (短文件名) */
struct fat32_dir_entry {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attributes;
    uint8_t  reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t mod_time;
    uint16_t mod_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__((packed));

/* 文件句柄，用于跟踪打开的文件 */
struct fat32_file {
    uint32_t first_cluster;
    uint32_t current_cluster;
    uint32_t current_offset;
    uint32_t file_size;
    uint8_t  buffer[SECTOR_SIZE];
};

void fat32_init(uint32_t partition_lba);
int fat32_open_file(const char *path, struct fat32_file *file);
int fat32_read_file(struct fat32_file *file, uint8_t *buffer, uint32_t offset, uint32_t size);

#endif