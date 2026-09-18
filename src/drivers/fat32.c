#include <fat32.h>
#include <ata.h>
#include <printf.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>

#define FAT32_MAX_VOLUMES 4

static struct fat32_volume fat32_volumes[FAT32_MAX_VOLUMES];
static int fat32_volume_count = 0;
static struct fat32_volume *default_volume = NULL;

/* ---------- 内部辅助 ---------- */

static uint32_t get_next_cluster(struct fat32_volume *vol, uint32_t cluster) {
    if (cluster >= 0x0FFFFFF8) {
        return 0x0FFFFFFF;
    }
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = vol->fat_start_lba + (fat_offset / SECTOR_SIZE);
    uint32_t offset_in_sector = fat_offset % SECTOR_SIZE;

    uint8_t sector1[SECTOR_SIZE];
    uint8_t sector2[SECTOR_SIZE];
    ata_read_sector(fat_sector, sector1);

    uint32_t next = 0;
    if (offset_in_sector + 4 <= SECTOR_SIZE) {
        next = *(uint32_t*)(sector1 + offset_in_sector);
    } else {
        ata_read_sector(fat_sector + 1, sector2);
        uint32_t part1_len = SECTOR_SIZE - offset_in_sector;
        memcpy((uint8_t*)&next, sector1 + offset_in_sector, part1_len);
        memcpy((uint8_t*)&next + part1_len, sector2, 4 - part1_len);
    }
    return next & 0x0FFFFFFF;
}

static void read_sector_from_cluster(struct fat32_volume *vol, uint32_t cluster,
                                     uint32_t sector_in_cluster, uint8_t *buffer) {
    uint32_t lba = vol->data_start_lba +
                   (cluster - 2) * vol->bpb.sectors_per_cluster +
                   sector_in_cluster;
    ata_read_sector(lba, buffer);
}

/* ---------- 挂载 ---------- */

struct fat32_volume *fat32_mount(uint32_t partition_lba) {
    if (fat32_volume_count >= FAT32_MAX_VOLUMES) {
        kprintf("[FAT32] ERROR: too many volumes\n");
        return NULL;
    }

    struct fat32_volume *vol = &fat32_volumes[fat32_volume_count];
    memset(vol, 0, sizeof(*vol));

    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(partition_lba, sector);

    struct fat32_bpb *bpb = (struct fat32_bpb*)sector;
    if (bpb->signature != FAT32_SIGNATURE) {
        kprintf("[FAT32] Invalid boot sector (signature=0x%x)\n", bpb->signature);
        return NULL;
    }

    vol->bpb = *bpb;
    vol->partition_start_lba = partition_lba;
    vol->fat_start_lba = partition_lba + bpb->reserved_sector_count;
    vol->data_start_lba = vol->fat_start_lba + bpb->fat_count * bpb->fat_size_32;
    vol->root_cluster = bpb->root_cluster;
    vol->valid = 1;

    fat32_volume_count++;
    if (!default_volume) default_volume = vol;

    kprintf("[FAT32] Mounted at LBA %d: bps=%d spc=%d fat_size=%d root=%d\n",
            partition_lba,
            vol->bpb.bytes_per_sector,
            vol->bpb.sectors_per_cluster,
            vol->bpb.fat_size_32,
            vol->root_cluster);

    return vol;
}

struct fat32_volume *fat32_get_default(void) {
    return default_volume;
}

/* ---------- 打开文件 ---------- */

int fat32_open_file(struct fat32_volume *vol, const char *path,
                    struct fat32_file *file) {
    if (!vol || !vol->valid || !path || !file) return EINVAL;

    if (path[0] == '/') path++;
    uint32_t current_cluster = vol->root_cluster;
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
            int sectors_in_cluster = vol->bpb.sectors_per_cluster;
            int entries_per_sector = vol->bpb.bytes_per_sector /
                                     sizeof(struct fat32_dir_entry);

            for (int s = 0; s < sectors_in_cluster; s++) {
                uint8_t sector[SECTOR_SIZE];
                read_sector_from_cluster(vol, cluster, s, sector);
                struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;

                for (int j = 0; j < entries_per_sector; j++) {
                    struct fat32_dir_entry *entry = &entries[j];
                    if (entry->name[0] == 0x00) break;
                    if (entry->name[0] == 0xE5) continue;

                    /* 8.3 名字构建 */
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

                    /* 统一小写比较 */
                    for (int m = 0; name[m]; m++) {
                        if (name[m] >= 'A' && name[m] <= 'Z') name[m] += 32;
                    }
                    char token_lower[64];
                    for (int m = 0; token[m]; m++) {
                        token_lower[m] = (token[m] >= 'A' && token[m] <= 'Z')
                                       ? token[m] + 32 : token[m];
                    }
                    token_lower[strlen(token)] = '\0';

                    if (strcmp(name, token_lower) == 0) {
                        found = 1;
                        uint32_t entry_cluster =
                            (entry->cluster_high << 16) | entry->cluster_low;
                        if (entry->attributes & 0x10) {
                            /* 目录：继续深入 */
                            current_cluster = entry_cluster;
                        } else {
                            /* 文件：填充句柄 */
                            file->first_cluster = entry_cluster;
                            file->current_cluster = entry_cluster;
                            file->current_offset = 0;
                            file->file_size = entry->file_size;
                            return OK;
                        }
                        break;
                    }
                }
                if (found) break;
            }
            if (found) break;
            cluster = get_next_cluster(vol, cluster);
        }
        if (!found) return ENOENT;
    }
    return ENOENT;
}

/* ---------- 读文件 ---------- */

int fat32_read_file(struct fat32_volume *vol, struct fat32_file *file,
                    uint8_t *buffer, uint32_t offset, uint32_t size) {
    if (!vol || !vol->valid || !file || !buffer) return EINVAL;
    if (offset + size > file->file_size) return EINVAL;

    uint32_t cluster = file->first_cluster;
    uint32_t cluster_size = vol->bpb.sectors_per_cluster * vol->bpb.bytes_per_sector;
    uint32_t cluster_offset = offset / cluster_size;
    uint32_t skip_bytes = offset % cluster_size;

    for (uint32_t i = 0; i < cluster_offset; i++) {
        cluster = get_next_cluster(vol, cluster);
        if (cluster >= 0x0FFFFFF8) return EIO;
    }

    uint32_t read_pos = 0;
    while (read_pos < size) {
        uint32_t first_sector = vol->data_start_lba +
                                (cluster - 2) * vol->bpb.sectors_per_cluster;
        uint32_t sector_in_cluster = skip_bytes / vol->bpb.bytes_per_sector;
        uint32_t sector_offset = skip_bytes % vol->bpb.bytes_per_sector;

        uint32_t remaining_in_cluster = cluster_size - skip_bytes;
        uint32_t to_read = (size - read_pos) < remaining_in_cluster
                         ? (size - read_pos) : remaining_in_cluster;

        while (to_read > 0) {
            uint8_t sector[SECTOR_SIZE];
            ata_read_sector(first_sector + sector_in_cluster, sector);
            uint32_t copy_len = (to_read < (SECTOR_SIZE - sector_offset))
                              ? to_read : (SECTOR_SIZE - sector_offset);
            memcpy(buffer + read_pos, sector + sector_offset, copy_len);
            read_pos += copy_len;
            to_read -= copy_len;
            sector_in_cluster++;
            sector_offset = 0;
        }

        if (read_pos < size) {
            cluster = get_next_cluster(vol, cluster);
            if (cluster >= 0x0FFFFFF8) return read_pos;
            skip_bytes = 0;
        }
    }
    return size;
}