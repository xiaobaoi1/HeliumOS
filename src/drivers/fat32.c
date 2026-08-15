#include <fat32.h>
#include <ata.h>
#include <printf.h>
#include <string.h>

static struct fat32_bpb bpb;
static uint32_t partition_start_lba = 0;
static uint32_t fat_start_lba = 0;
static uint32_t data_start_lba = 0;
static uint32_t root_cluster = 0;

/* 读取 FAT 表项，处理跨扇区 */
static uint32_t get_next_cluster(uint32_t cluster) {
    if (cluster >= 0x0FFFFFF8) {
        return 0x0FFFFFFF;
    }
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / SECTOR_SIZE);
    uint32_t offset_in_sector = fat_offset % SECTOR_SIZE;

    uint8_t sector1[SECTOR_SIZE];
    uint8_t sector2[SECTOR_SIZE];
    ata_read_sector(fat_sector, sector1);

    uint32_t next = 0;
    if (offset_in_sector + 4 <= SECTOR_SIZE) {
        // 完全在同一扇区
        next = *(uint32_t*)(sector1 + offset_in_sector);
    } else {
        // 跨扇区
        ata_read_sector(fat_sector + 1, sector2);
        uint32_t part1_len = SECTOR_SIZE - offset_in_sector;
        memcpy((uint8_t*)&next, sector1 + offset_in_sector, part1_len);
        memcpy((uint8_t*)&next + part1_len, sector2, 4 - part1_len);
    }
    return next & 0x0FFFFFFF;
}

/* 读取指定簇的某个扇区到缓冲区（用于目录遍历和文件读取） */
static void read_sector_from_cluster(uint32_t cluster, uint32_t sector_in_cluster, uint8_t *buffer) {
    uint32_t lba = data_start_lba + (cluster - 2) * bpb.sectors_per_cluster + sector_in_cluster;
    ata_read_sector(lba, buffer);
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
    if (path[0] == '/') path++;

    uint32_t current_cluster = root_cluster;
    char token[64];

    while (*path) {
        int i = 0;
        while (path[i] && path[i] != '/' && i < 63) {
            token[i] = path[i];
            i++;
        }
        token[i] = '\0';
        if (path[i] == '/') path += i + 1;
        else path += i;

        int found = 0;
        uint32_t cluster = current_cluster;
        while (cluster < 0x0FFFFFF8) {
            int sectors_in_cluster = bpb.sectors_per_cluster;
            int entries_per_sector = bpb.bytes_per_sector / sizeof(struct fat32_dir_entry);

            for (int s = 0; s < sectors_in_cluster; s++) {
                uint8_t sector[SECTOR_SIZE];
                read_sector_from_cluster(cluster, s, sector);
                struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;

                for (int j = 0; j < entries_per_sector; j++) {
                    struct fat32_dir_entry *entry = &entries[j];
                    if (entry->name[0] == 0x00) break;
                    if (entry->name[0] == 0xE5) continue;

                    // 构建短文件名（8.3）
                    char name[13];
                    int k;
                    for (k = 0; k < 8 && entry->name[k] != ' '; k++) {
                        name[k] = entry->name[k];
                    }
                    if (entry->ext[0] != ' ') {
                        name[k++] = '.';
                        for (int l = 0; l < 3 && entry->ext[l] != ' '; l++) {
                            name[k++] = entry->ext[l];
                        }
                    }
                    name[k] = '\0';

                    // 转小写比较
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
                            // 目录
                            current_cluster = entry_cluster;
                        } else {
                            // 文件
                            file->first_cluster = entry_cluster;
                            file->current_cluster = entry_cluster;
                            file->current_offset = 0;
                            file->file_size = entry->file_size;
                            kprintf("[FAT32] File found: %s, size=%d, cluster=%d\n", token, file->file_size, file->first_cluster);
                            return 1;
                        }
                        break;
                    }
                }
                if (found) break;
            }
            if (found) break;
            cluster = get_next_cluster(cluster);
        }
        if (!found) {
            kprintf("[FAT32] Path component '%s' not found.\n", token);
            return 0;
        }
    }
    return 0;
}

/* 从文件读取数据（按 offset 和 size），成功返回读取字节数，失败返回 -1 */
int fat32_read_file(struct fat32_file *file, uint8_t *buffer, uint32_t offset, uint32_t size) {
    if (!buffer) return -1;
    if (offset + size > file->file_size) {
        kprintf("[FAT32] Read beyond file size.\n");
        return -1;
    }

    uint32_t cluster = file->first_cluster;
    uint32_t cluster_size = bpb.sectors_per_cluster * bpb.bytes_per_sector;
    uint32_t cluster_offset = offset / cluster_size;
    uint32_t skip_bytes = offset % cluster_size;

    // 跳到起始簇
    for (uint32_t i = 0; i < cluster_offset; i++) {
        cluster = get_next_cluster(cluster);
        if (cluster >= 0x0FFFFFF8) {
            kprintf("[FAT32] Cluster chain ended early.\n");
            return -1;
        }
    }

    uint32_t read_pos = 0;
    while (read_pos < size) {
        uint32_t first_sector = data_start_lba + (cluster - 2) * bpb.sectors_per_cluster;
        uint32_t sector_in_cluster = skip_bytes / bpb.bytes_per_sector;
        uint32_t sector_offset = skip_bytes % bpb.bytes_per_sector;

        uint32_t remaining_in_cluster = cluster_size - skip_bytes;
        uint32_t to_read = (size - read_pos) < remaining_in_cluster ? (size - read_pos) : remaining_in_cluster;

        while (to_read > 0) {
            uint8_t sector[SECTOR_SIZE];
            ata_read_sector(first_sector + sector_in_cluster, sector);
            uint32_t copy_len = (to_read < (SECTOR_SIZE - sector_offset)) ? to_read : (SECTOR_SIZE - sector_offset);
            memcpy(buffer + read_pos, sector + sector_offset, copy_len);
            read_pos += copy_len;
            to_read -= copy_len;
            sector_in_cluster++;
            sector_offset = 0;
        }

        if (read_pos < size) {
            cluster = get_next_cluster(cluster);
            if (cluster >= 0x0FFFFFF8) {
                kprintf("[FAT32] File truncated.\n");
                return read_pos; // 返回实际读取字节数（但调用者需检查是否等于 size）
            }
            skip_bytes = 0;
        }
    }
    return size;
}