#include <fat32.h>
#include <ata.h>
#include <printf.h>
#include <string.h>

static struct fat32_bpb bpb;
static uint32_t partition_start_lba = 0;
static uint32_t fat_start_lba = 0;
static uint32_t data_start_lba = 0;
static uint32_t root_cluster = 0;

/* 读取一个簇（可能跨扇区） */
static void read_cluster(uint32_t cluster, uint8_t *buffer) {
    if (cluster < 2) {
        kprintf("[FAT32] Invalid cluster %d\n", cluster);
        return;
    }
    uint32_t first_sector = data_start_lba + (cluster - 2) * bpb.sectors_per_cluster;
    for (int i = 0; i < bpb.sectors_per_cluster; i++) {
        ata_read_sector(first_sector + i, buffer + i * SECTOR_SIZE);
    }
}

/* 读取 FAT 表，获取下一个簇号 */
static uint32_t get_next_cluster(uint32_t cluster) {
    if (cluster >= 0x0FFFFFF8) {
        return 0x0FFFFFFF; // 结束标记
    }
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / SECTOR_SIZE);
    uint32_t fat_offset_in_sector = fat_offset % SECTOR_SIZE;

    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(fat_sector, sector);
    uint32_t next = *(uint32_t*)(sector + fat_offset_in_sector);
    next &= 0x0FFFFFFF;  // 只保留 28 位
    return next;
}

/* 初始化 FAT32 */
void fat32_init(uint32_t partition_lba) {
    partition_start_lba = partition_lba;
    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(partition_lba, sector);

    struct fat32_bpb *bpb_ptr = (struct fat32_bpb*)sector;
    if (bpb_ptr->signature != FAT32_SIGNATURE) {
        kprintf("[FAT32] Invalid boot sector (signature=0x%x)\n", bpb_ptr->signature);
        return;
    }

    // 拷贝 BPB
    bpb = *bpb_ptr;
    kprintf("[FAT32] BPB: bytes_per_sector=%d, sectors_per_cluster=%d, fat_size=%d, root_cluster=%d\n",
            bpb.bytes_per_sector, bpb.sectors_per_cluster, bpb.fat_size_32, bpb.root_cluster);

    fat_start_lba = partition_lba + bpb.reserved_sector_count;
    data_start_lba = fat_start_lba + bpb.fat_count * bpb.fat_size_32;
    root_cluster = bpb.root_cluster;

    kprintf("[FAT32] Initialized: FAT starts at LBA %d, data at LBA %d\n", fat_start_lba, data_start_lba);
}

/* 遍历路径，打开文件 */
int fat32_open_file(const char *path, struct fat32_file *file) {
    // 跳过开头的 '/'
    if (path[0] == '/') path++;

    uint32_t current_cluster = root_cluster;
    uint8_t cluster_buffer[SECTOR_SIZE * 16]; // 临时存放整个簇（最大 16 扇区）
    char token[64];

    while (*path) {
        // 提取路径中的下一个名称（直到 '/' 或结束）
        int i = 0;
        while (path[i] && path[i] != '/' && i < 63) {
            token[i] = path[i];
            i++;
        }
        token[i] = '\0';

        if (path[i] == '/') path += i + 1;
        else path += i;

        // 查找当前目录项
        int found = 0;
        uint32_t cluster = current_cluster;
        while (cluster < 0x0FFFFFF8) {
            read_cluster(cluster, cluster_buffer);
            int entries_per_cluster = (bpb.bytes_per_sector * bpb.sectors_per_cluster) / sizeof(struct fat32_dir_entry);
            struct fat32_dir_entry *entries = (struct fat32_dir_entry*)cluster_buffer;

            for (int j = 0; j < entries_per_cluster; j++) {
                struct fat32_dir_entry *entry = &entries[j];
                if (entry->name[0] == 0x00) break;  // 目录项结束
                if (entry->name[0] == 0xE5) continue; // 已删除项

                // 检查名称（8.3 格式）
                char name[13];
                int k;
                for (k = 0; k < 8 && entry->name[k] != ' '; k++) {
                    name[k] = entry->name[k];
                }
                // 如果有扩展名
                if (entry->ext[0] != ' ') {
                    name[k++] = '.';
                    for (int l = 0; l < 3 && entry->ext[l] != ' '; l++) {
                        name[k++] = entry->ext[l];
                    }
                }
                name[k] = '\0';

                // 转换为小写比较（FAT32 存储大写）
                for (int m = 0; name[m]; m++) {
                    if (name[m] >= 'A' && name[m] <= 'Z') name[m] += 32;
                }
                char token_lower[64];
                for (int m = 0; token[m]; m++) {
                    token_lower[m] = (token[m] >= 'A' && token[m] <= 'Z') ? token[m] + 32 : token[m];
                }
                token_lower[strlen(token)] = '\0';

                if (strcmp(name, token_lower) == 0) {
                    found = 1;
                    uint32_t entry_cluster = (entry->cluster_high << 16) | entry->cluster_low;
                    if (entry->attributes & 0x10) {
                        // 这是一个目录
                        current_cluster = entry_cluster;
                        break;
                    } else {
                        // 这是一个文件
                        file->first_cluster = entry_cluster;
                        file->current_cluster = entry_cluster;
                        file->current_offset = 0;
                        file->file_size = entry->file_size;
                        kprintf("[FAT32] File found: %s, size=%d, cluster=%d\n", token, file->file_size, file->first_cluster);
                        return 1;
                    }
                }
            }
            if (found && !(entries[0].attributes & 0x10)) return 1; // 文件已找到
            if (found && (entries[0].attributes & 0x10)) break;   // 进入子目录
            cluster = get_next_cluster(cluster);
        }
        if (!found) {
            kprintf("[FAT32] Path component '%s' not found.\n", token);
            return 0;
        }
    }
    return 0;
}

/* 从文件读取数据（按 offset 和 size）*/
int fat32_read_file(struct fat32_file *file, uint8_t *buffer, uint32_t offset, uint32_t size) {
    if (offset + size > file->file_size) {
        kprintf("[FAT32] Read beyond file size.\n");
        return 0;
    }

    uint32_t cluster = file->first_cluster;
    uint32_t cluster_size = bpb.sectors_per_cluster * bpb.bytes_per_sector;
    uint32_t cluster_offset = offset / cluster_size;
    uint32_t skip_bytes = offset % cluster_size;

    // 跳转到目标簇
    for (int i = 0; i < cluster_offset; i++) {
        cluster = get_next_cluster(cluster);
        if (cluster >= 0x0FFFFFF8) {
            kprintf("[FAT32] Cluster chain ended early.\n");
            return 0;
        }
    }

    uint8_t cluster_buffer[SECTOR_SIZE * 16];
    uint32_t read_pos = 0;
    while (read_pos < size) {
        read_cluster(cluster, cluster_buffer);
        uint32_t copy_size = (size - read_pos) > (cluster_size - skip_bytes) ? (cluster_size - skip_bytes) : (size - read_pos);
        memcpy(buffer + read_pos, cluster_buffer + skip_bytes, copy_size);
        read_pos += copy_size;
        cluster = get_next_cluster(cluster);
        skip_bytes = 0;
        if (cluster >= 0x0FFFFFF8 && read_pos < size) {
            kprintf("[FAT32] File truncated.\n");
            return read_pos;
        }
    }
    return size;
}