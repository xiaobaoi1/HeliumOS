#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <volume.h>
#include <ata.h>

#define SECTOR_SIZE 512
#define FAT32_SIGNATURE 0xAA55

/* BPB（不变） */
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

/* 目录项（不变） */
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

/* 文件句柄（不变） */
struct fat32_file {
    uint32_t first_cluster;
    uint32_t current_cluster;
    uint32_t current_offset;
    uint32_t file_size;
    uint32_t dirent_sector_lba;   /* 目录项所在扇区 LBA */
    uint32_t dirent_entry_idx;    /* 扇区内的项索引 */
    uint8_t  dirent_valid;        /* 是否已记录 */
};

/* ★ 新增：FAT32 卷上下文。每个挂载点一个 */
struct fat32_volume {
    struct fat32_bpb bpb;             /* 卷的 BPB */
    uint32_t partition_start_lba;     /* 分区起始 LBA */
    uint32_t fat_start_lba;           /* FAT 表起始 LBA */
    uint32_t data_start_lba;          /* 数据区起始 LBA */
    uint32_t root_cluster;            /* 根目录簇号 */
    uint8_t  valid;                   /* 挂载成功标志 */
    uint32_t last_alloc_hint;    /* 上次分配的簇号 + 1，避免每次从头扫 */
    const struct ata_device *dev;   /* 新增：所属 ATA 设备 */
};

/* 目录迭代句柄 */
struct fat32_dir {
    uint32_t start_cluster;
    uint32_t cur_cluster;
    uint32_t sector;         /* 簇内扇区号 */
    uint32_t entry;          /* 扇区内目录项索引 */
    uint8_t  valid;
};

/* ★ 挂载一个 FAT32 分区 */
struct fat32_volume *fat32_mount(const struct ata_device *dev,
                                 uint32_t partition_lba);

/* ★ 所有操作都带 vol 参数 */
int fat32_open_file(struct fat32_volume *vol, const char *path,
                    struct fat32_file *file);

int fat32_read_file(struct fat32_volume *vol, struct fat32_file *file,
                    uint8_t *buffer, uint32_t offset, uint32_t size);
/* 打开或创建文件（存在则打开，不存在则创建） */
int fat32_create_file(struct fat32_volume *vol, const char *path,
                      struct fat32_file *file);

/* 向文件写入。offset 是文件内偏移，size 是字节数。
 * 若 offset+size > file_size 会自动扩展簇链并更新目录项。
 * 返回写入字节数（成功）或负错误码。 */
int fat32_write_file(struct fat32_volume *vol, struct fat32_file *file,
                     const uint8_t *buffer, uint32_t offset, uint32_t size);

/* 截断文件为 0：释放簇链，清 first_cluster / file_size，更新目录项。 */
int fat32_truncate(struct fat32_volume *vol, struct fat32_file *file);
                     
/* 目录操作 */
int  fat32_opendir(struct fat32_volume *vol, const char *path,
                   struct fat32_dir *dir);
int  fat32_readdir(struct fat32_volume *vol, struct fat32_dir *dir,
                   struct dirent *out);
void fat32_closedir(struct fat32_volume *vol, struct fat32_dir *dir);

/* 判断某路径是否是目录 */
int  fat32_is_dir(struct fat32_volume *vol, const char *path);

/* 删除文件（不处理目录）。失败：ENOENT / EISDIR / EIO */
int fat32_unlink(struct fat32_volume *vol, const char *path);

/* 创建目录。失败：EEXIST / ENOSPC / ENAMETOOLONG / ENOENT（父目录不存在） */
int fat32_mkdir(struct fat32_volume *vol, const char *path);

/* 删除空目录。失败：ENOENT / ENOTDIR / ENOTEMPTY / EINVAL（根） */
int fat32_rmdir(struct fat32_volume *vol, const char *path);

/* 改名/移动。目标已存在返回 EEXIST。*/
int fat32_rename(struct fat32_volume *vol, const char *old_path,
                 const char *new_path);

#endif