#include <fat32.h>
#include <ata.h>
#include <printf.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <path.h>

#define FAT32_MAX_VOLUMES 4

static struct fat32_volume fat32_volumes[FAT32_MAX_VOLUMES];
static int fat32_volume_count = 0;
static struct fat32_volume *default_volume = NULL;

/* FAT 扇区单槽缓存（FAT 表访问有强局部性，单槽足够） */
static uint32_t fat_cache_lba = 0xFFFFFFFF;
static uint8_t  fat_cache[SECTOR_SIZE];
static int      fat_cache_valid = 0;

static void fat_read_cached(uint32_t lba, uint8_t *out) {
    if (fat_cache_valid && fat_cache_lba == lba) {
        memcpy(out, fat_cache, SECTOR_SIZE);
        return;
    }
    ata_read_sector(lba, fat_cache);
    fat_cache_lba = lba;
    fat_cache_valid = 1;
    memcpy(out, fat_cache, SECTOR_SIZE);
}

static void fat_write_cached(uint32_t lba, const uint8_t *in) {
    memcpy(fat_cache, in, SECTOR_SIZE);
    fat_cache_lba = lba;
    fat_cache_valid = 1;
    ata_write_sector(lba, in);
}

/* ---------- 内部辅助 ---------- */

static uint32_t get_next_cluster(struct fat32_volume *vol, uint32_t cluster) {
    if (cluster >= 0x0FFFFFF8) return 0x0FFFFFFF;

    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = vol->fat_start_lba + (fat_offset / SECTOR_SIZE);
    uint32_t offset_in_sector = fat_offset % SECTOR_SIZE;

    uint8_t sector1[SECTOR_SIZE];
    uint8_t sector2[SECTOR_SIZE];
    fat_read_cached(fat_sector, sector1);

    uint32_t next = 0;
    if (offset_in_sector + 4 <= SECTOR_SIZE) {
        next = *(uint32_t*)(sector1 + offset_in_sector);
    } else {
        fat_read_cached(fat_sector + 1, sector2);
        uint32_t part1_len = SECTOR_SIZE - offset_in_sector;
        memcpy((uint8_t*)&next, sector1 + offset_in_sector, part1_len);
        memcpy((uint8_t*)&next + part1_len, sector2, 4 - part1_len);
    }
    return next & 0x0FFFFFFF;
}

/* 写 FAT 表项（所有副本） */
static int set_fat_entry(struct fat32_volume *vol, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t offset_in_sector = fat_offset % SECTOR_SIZE;
    uint32_t sector_offset = fat_offset / SECTOR_SIZE;

    for (int copy = 0; copy < vol->bpb.fat_count; copy++) {
        uint32_t fat_sector = vol->fat_start_lba +
                              copy * vol->bpb.fat_size_32 +
                              sector_offset;
        uint8_t sector[SECTOR_SIZE];
        fat_read_cached(fat_sector, sector);

        uint32_t old = *(uint32_t*)(sector + offset_in_sector);
        uint32_t new_val = (old & 0xF0000000) | (value & 0x0FFFFFFF);
        *(uint32_t*)(sector + offset_in_sector) = new_val;

        fat_write_cached(fat_sector, sector);
    }
    return OK;
}

/* 数据区最大簇号 */
static uint32_t fat32_max_cluster(struct fat32_volume *vol) {
    uint32_t total_sectors = vol->bpb.total_sectors_32;
    if (total_sectors == 0) total_sectors = vol->bpb.total_sectors_16;
    uint32_t data_sectors = total_sectors -
                            (vol->data_start_lba - vol->partition_start_lba);
    return data_sectors / vol->bpb.sectors_per_cluster + 2;
}

/* 分配一个空闲簇，标记为 EOC，返回簇号。失败返回 0。 */
static uint32_t alloc_cluster(struct fat32_volume *vol) {
    uint32_t max_c = fat32_max_cluster(vol);
    uint32_t start = vol->last_alloc_hint;
    if (start < 2 || start >= max_c) start = 2;

    /* 从 hint 往后扫 */
    for (uint32_t c = start; c < max_c; c++) {
        if (get_next_cluster(vol, c) == 0) {
            set_fat_entry(vol, c, 0x0FFFFFFF);
            vol->last_alloc_hint = c + 1;
            return c;
        }
    }
    /* 从 2 扫到 hint（回卷） */
    for (uint32_t c = 2; c < start; c++) {
        if (get_next_cluster(vol, c) == 0) {
            set_fat_entry(vol, c, 0x0FFFFFFF);
            vol->last_alloc_hint = c + 1;
            return c;
        }
    }
    return 0;
}

/* 释放簇链（从 start 开始） */
static void free_cluster_chain(struct fat32_volume *vol, uint32_t start) {
    uint32_t c = start;
    uint32_t guard = 0x100000;
    while (c >= 2 && c < 0x0FFFFFF8 && guard-- > 0) {
        uint32_t next = get_next_cluster(vol, c);
        set_fat_entry(vol, c, 0);
        c = next;
    }
}

static void read_sector_from_cluster(struct fat32_volume *vol, uint32_t cluster,
                                     uint32_t sector_in_cluster, uint8_t *buffer) {
    uint32_t lba = vol->data_start_lba +
                   (cluster - 2) * vol->bpb.sectors_per_cluster +
                   sector_in_cluster;
    ata_read_sector(lba, buffer);
}

static void write_sector_from_cluster(struct fat32_volume *vol, uint32_t cluster,
                                      uint32_t sector_in_cluster,
                                      const uint8_t *buffer) {
    uint32_t lba = vol->data_start_lba +
                   (cluster - 2) * vol->bpb.sectors_per_cluster +
                   sector_in_cluster;
    ata_write_sector(lba, buffer);
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
    vol->last_alloc_hint = 2;

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
        uint32_t guard = 1024;
        while (cluster < 0x0FFFFFF8 && guard-- > 0) {
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
                    if ((entry->attributes & 0x0F) == 0x0F) continue; /* LFN */

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
                            /* 记录目录项位置，供写回时用 */
                            file->dirent_sector_lba = vol->data_start_lba +
                                (cluster - 2) * vol->bpb.sectors_per_cluster + s;
                            file->dirent_entry_idx = j;
                            file->dirent_valid = 1;
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

/* ---------- 判断是否是目录 ---------- */
int fat32_is_dir(struct fat32_volume *vol, const char *path) {
    if (!vol || !vol->valid) return 0;

    /* 根目录视为目录 */
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        return 1;
    }

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

        int is_last = (*path == '\0');
        int found = 0;
        uint32_t cluster = current_cluster;
        uint32_t guard = 1024;

        while (cluster < 0x0FFFFFF8 && !found && guard-- > 0) {
            int sectors = vol->bpb.sectors_per_cluster;
            int eps = vol->bpb.bytes_per_sector / sizeof(struct fat32_dir_entry);

            for (int s = 0; s < sectors && !found; s++) {
                uint8_t sector[SECTOR_SIZE];
                read_sector_from_cluster(vol, cluster, s, sector);
                struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;

                for (int j = 0; j < eps; j++) {
                    struct fat32_dir_entry *e = &entries[j];
                    if (e->name[0] == 0x00) break;
                    if (e->name[0] == 0xE5) continue;
                    if ((e->attributes & 0x0F) == 0x0F) continue; /* LFN */

                    char name[13];
                    int k = 0;
                    for (int m = 0; m < 8 && e->name[m] != ' '; m++) name[k++] = e->name[m];
                    name[k] = '\0';
                    for (int m = 0; name[m]; m++)
                        if (name[m] >= 'A' && name[m] <= 'Z') name[m] += 32;

                    char tl[64];
                    for (int m = 0; token[m]; m++)
                        tl[m] = (token[m] >= 'A' && token[m] <= 'Z') ? token[m] + 32 : token[m];
                    tl[strlen(token)] = '\0';

                    if (strcmp(name, tl) == 0) {
                        found = 1;
                        if (is_last) {
                            return (e->attributes & 0x10) ? 1 : 0;
                        }
                        if (!(e->attributes & 0x10)) return 0;
                        current_cluster = (e->cluster_high << 16) | e->cluster_low;
                        break;
                    }
                }
            }
            if (!found) cluster = get_next_cluster(vol, cluster);
        }
        if (!found) return 0;
    }
    return 0;
}

/* ---------- 目录迭代 ---------- */
int fat32_opendir(struct fat32_volume *vol, const char *path,
                  struct fat32_dir *dir) {
    if (!vol || !vol->valid || !dir) return EINVAL;

    uint32_t cluster;

    /* 根目录 */
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        cluster = vol->root_cluster;
    } else {
        /* 逐级查找目录 */
        if (path[0] == '/') path++;
        uint32_t cur = vol->root_cluster;
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
            uint32_t c = cur;
            uint32_t guard = 1024;
            while (c < 0x0FFFFFF8 && !found && guard-- > 0) {
                int sectors = vol->bpb.sectors_per_cluster;
                int eps = vol->bpb.bytes_per_sector / sizeof(struct fat32_dir_entry);
                for (int s = 0; s < sectors && !found; s++) {
                    uint8_t sector[SECTOR_SIZE];
                    read_sector_from_cluster(vol, c, s, sector);
                    struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;
                    for (int j = 0; j < eps; j++) {
                        struct fat32_dir_entry *e = &entries[j];
                        if (e->name[0] == 0x00) break;
                        if (e->name[0] == 0xE5) continue;
                        if (!(e->attributes & 0x10)) continue;
                        if ((e->attributes & 0x0F) == 0x0F) continue; /* LFN */

                        char name[13];
                        int k = 0;
                        for (int m = 0; m < 8 && e->name[m] != ' '; m++) name[k++] = e->name[m];
                        name[k] = '\0';
                        for (int m = 0; name[m]; m++)
                            if (name[m] >= 'A' && name[m] <= 'Z') name[m] += 32;
                        char tl[64];
                        for (int m = 0; token[m]; m++)
                            tl[m] = (token[m] >= 'A' && token[m] <= 'Z') ? token[m]+32 : token[m];
                        tl[strlen(token)] = '\0';
                        if (strcmp(name, tl) == 0) {
                            found = 1;
                            cur = (e->cluster_high << 16) | e->cluster_low;
                            break;
                        }
                    }
                }
                if (!found) c = get_next_cluster(vol, c);
            }
            if (!found) return ENOENT;
        }
        cluster = cur;
    }

    dir->start_cluster = cluster;
    dir->cur_cluster = cluster;
    dir->sector = 0;
    dir->entry = 0;
    dir->valid = 1;
    return OK;
}

int fat32_readdir(struct fat32_volume *vol, struct fat32_dir *dir,
                  struct dirent *out) {
    if (!vol || !vol->valid || !dir || !out || !dir->valid) return EINVAL;
    
    uint32_t guard = 1024;
    while (dir->cur_cluster < 0x0FFFFFF8 && guard-- > 0) {
        int sectors = vol->bpb.sectors_per_cluster;
        int eps = vol->bpb.bytes_per_sector / sizeof(struct fat32_dir_entry);

        while (dir->sector < (uint32_t)sectors) {
            uint8_t sector[SECTOR_SIZE];
            read_sector_from_cluster(vol, dir->cur_cluster, dir->sector, sector);
            struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;

            while (dir->entry < (uint32_t)eps) {
                struct fat32_dir_entry *e = &entries[dir->entry++];

                if (e->name[0] == 0x00) return 0;             /* 目录结束 */
                if (e->name[0] == 0xE5) continue;             /* 已删除 */
                if ((e->attributes & 0x0F) == 0x0F) continue; /* LFN */
                if (e->name[0] == '.') continue;              /* . / .. */

                int k = 0;
                for (int m = 0; m < 8 && e->name[m] != ' ' && k < DIRENT_NAME_MAX - 2; m++)
                    out->name[k++] = e->name[m];
                if (e->ext[0] != ' ' && k < DIRENT_NAME_MAX - 2) {
                    out->name[k++] = '.';
                    for (int m = 0; m < 3 && e->ext[m] != ' ' && k < DIRENT_NAME_MAX - 1; m++)
                        out->name[k++] = e->ext[m];
                }
                out->name[k] = '\0';
                out->attributes = e->attributes;
                out->size = e->file_size;
                return 1;
            }
            dir->sector++;
            dir->entry = 0;
        }
        dir->cur_cluster = get_next_cluster(vol, dir->cur_cluster);
        dir->sector = 0;
        dir->entry = 0;
    }
    return 0;
}

void fat32_closedir(struct fat32_volume *vol, struct fat32_dir *dir) {
    (void)vol;
    if (dir) dir->valid = 0;
}

/* "out.txt" → out8="OUT     " out3="TXT" */
static void name_to_83(const char *name, uint8_t *out8, uint8_t *out3) {
    memset(out8, ' ', 8);
    memset(out3, ' ', 3);

    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        out8[j++] = c;
    }
    if (name[i] == '.') {
        i++;
        j = 0;
        while (name[i] && j < 3) {
            char c = name[i++];
            if (c >= 'a' && c <= 'z') c -= 32;
            out3[j++] = c;
        }
    }
}

/* 8.3 目录项 → 名字字符串 */
static void dir_entry_to_name(const struct fat32_dir_entry *e, char *out) {
    int k = 0;
    for (int i = 0; i < 8 && e->name[i] != ' '; i++) out[k++] = e->name[i];
    if (e->ext[0] != ' ') {
        out[k++] = '.';
        for (int i = 0; i < 3 && e->ext[i] != ' '; i++) out[k++] = e->ext[i];
    }
    out[k] = '\0';
}

/* 大小写不敏感比较 */
static int name_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* 在目录里找名字。
 * 找到返回 1，填 *out_sector_lba / *out_entry_idx / *out_entry。
 * 未找到返回 0。 */
static int find_dirent(struct fat32_volume *vol, uint32_t dir_cluster,
                       const char *name,
                       uint32_t *out_sector_lba, uint32_t *out_entry_idx,
                       struct fat32_dir_entry *out_entry) {
    uint32_t cluster = dir_cluster;
    uint32_t guard = 1024;
    int sectors = vol->bpb.sectors_per_cluster;
    int eps = vol->bpb.bytes_per_sector / sizeof(struct fat32_dir_entry);

    while (cluster < 0x0FFFFFF8 && guard-- > 0) {
        for (int s = 0; s < sectors; s++) {
            uint8_t sector[SECTOR_SIZE];
            uint32_t lba = vol->data_start_lba +
                           (cluster - 2) * vol->bpb.sectors_per_cluster + s;
            ata_read_sector(lba, sector);
            struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;

            for (int j = 0; j < eps; j++) {
                struct fat32_dir_entry *e = &entries[j];
                if (e->name[0] == 0x00) return 0;
                if (e->name[0] == 0xE5) continue;
                if ((e->attributes & 0x0F) == 0x0F) continue;  /* LFN */

                char ename[13];
                dir_entry_to_name(e, ename);
                if (name_eq(ename, name)) {
                    if (out_sector_lba) *out_sector_lba = lba;
                    if (out_entry_idx)  *out_entry_idx = j;
                    if (out_entry)      *out_entry = *e;
                    return 1;
                }
            }
        }
        cluster = get_next_cluster(vol, cluster);
    }
    return 0;
}

/* 找空闲目录项（0x00 或 0xE5）。*/
static int find_free_dirent(struct fat32_volume *vol, uint32_t dir_cluster,
                            uint32_t *out_sector_lba, uint32_t *out_entry_idx,
                            uint32_t *out_last_cluster) {
    uint32_t cluster = dir_cluster;
    uint32_t guard = 1024;
    int sectors = vol->bpb.sectors_per_cluster;
    int eps = vol->bpb.bytes_per_sector / sizeof(struct fat32_dir_entry);

    while (cluster < 0x0FFFFFF8 && guard-- > 0) {
        if (out_last_cluster) *out_last_cluster = cluster;

        for (int s = 0; s < sectors; s++) {
            uint8_t sector[SECTOR_SIZE];
            uint32_t lba = vol->data_start_lba +
                           (cluster - 2) * vol->bpb.sectors_per_cluster + s;
            ata_read_sector(lba, sector);
            struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;

            for (int j = 0; j < eps; j++) {
                struct fat32_dir_entry *e = &entries[j];
                if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
                    if (out_sector_lba) *out_sector_lba = lba;
                    if (out_entry_idx)  *out_entry_idx = j;
                    return 1;
                }
            }
        }
        cluster = get_next_cluster(vol, cluster);
    }
    return 0;
}

/* 写目录项 */
static void write_dirent(struct fat32_volume *vol, uint32_t sector_lba,
                         uint32_t entry_idx, const char *name,
                         uint8_t attr, uint32_t first_cluster, uint32_t size) {
    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(sector_lba, sector);
    struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;
    struct fat32_dir_entry *e = &entries[entry_idx];
    memset(e, 0, sizeof(*e));

    uint8_t name8[8], ext3[3];
    name_to_83(name, name8, ext3);
    memcpy(e->name, name8, 8);
    memcpy(e->ext, ext3, 3);
    e->attributes   = attr;
    e->cluster_high = (first_cluster >> 16) & 0xFFFF;
    e->cluster_low  = first_cluster & 0xFFFF;
    e->file_size    = size;

    ata_write_sector(sector_lba, sector);
}

/* 更新目录项的 first_cluster / file_size */
static void update_dirent(struct fat32_volume *vol, uint32_t sector_lba,
                          uint32_t entry_idx, uint32_t first_cluster,
                          uint32_t size) {
    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(sector_lba, sector);
    struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;
    struct fat32_dir_entry *e = &entries[entry_idx];

    e->cluster_high = (first_cluster >> 16) & 0xFFFF;
    e->cluster_low  = first_cluster & 0xFFFF;
    e->file_size    = size;

    ata_write_sector(sector_lba, sector);
}

/* ---------- 创建文件（存在则打开） ---------- */

int fat32_create_file(struct fat32_volume *vol, const char *path,
                      struct fat32_file *file) {
    if (!vol || !vol->valid || !path || !file) return EINVAL;

    /* 先尝试打开，存在就直接返回 */
    if (fat32_open_file(vol, path, file) == OK) {
        return OK;
    }

    /* 拆分 path 成 父目录 + 文件名 */
    char dir_path[PATH_MAX_LEN];
    char name[64];
    const char *p = path;
    if (p[0] == '/') p++;

    const char *last_slash = NULL;
    for (const char *q = p; *q; q++) {
        if (*q == '/') last_slash = q;
    }

    if (last_slash == NULL) {
        dir_path[0] = '/';
        dir_path[1] = '\0';
        int k = 0;
        while (p[k] && k < (int)sizeof(name) - 1) { name[k] = p[k]; k++; }
        name[k] = '\0';
    } else {
        int dlen = last_slash - p;
        if (dlen >= PATH_MAX_LEN - 1) return EINVAL;
        dir_path[0] = '/';
        memcpy(dir_path + 1, p, dlen);
        dir_path[dlen + 1] = '\0';

        int k = 0;
        const char *np = last_slash + 1;
        while (np[k] && k < (int)sizeof(name) - 1) { name[k] = np[k]; k++; }
        name[k] = '\0';
    }

    if (name[0] == '\0') return EINVAL;

    /* 打开父目录 */
    struct fat32_dir dir;
    if (fat32_opendir(vol, dir_path, &dir) != OK) return ENOENT;

    /* 再检查一次（防止 open_file 因 LFN 未识别但确实存在） */
    uint32_t sect_lba, idx;
    struct fat32_dir_entry found;
    if (find_dirent(vol, dir.cur_cluster, name, &sect_lba, &idx, &found)) {
        /* 已有同名的目录项。若是文件则直接打开，若是目录返回错误。 */
        if (found.attributes & 0x10) return EISDIR;
        memset(file, 0, sizeof(*file));
        file->first_cluster = (found.cluster_high << 16) | found.cluster_low;
        file->current_cluster = file->first_cluster;
        file->current_offset = 0;
        file->file_size = found.file_size;
        file->dirent_sector_lba = sect_lba;
        file->dirent_entry_idx = idx;
        file->dirent_valid = 1;
        return OK;
    }

    /* 找空闲槽；找不到则扩展目录 */
    uint32_t last_cluster = 0;
    if (!find_free_dirent(vol, dir.cur_cluster, &sect_lba, &idx, &last_cluster)) {
        /* 目录满：分配新簇挂到链尾 */
        uint32_t new_cluster = alloc_cluster(vol);
        if (new_cluster == 0) return ENOSPC;

        set_fat_entry(vol, last_cluster, new_cluster);

        /* 清零新簇 */
        uint8_t zero[SECTOR_SIZE];
        memset(zero, 0, SECTOR_SIZE);
        for (int s = 0; s < vol->bpb.sectors_per_cluster; s++) {
            write_sector_from_cluster(vol, new_cluster, s, zero);
        }

        sect_lba = vol->data_start_lba +
                   (new_cluster - 2) * vol->bpb.sectors_per_cluster;
        idx = 0;
    }

    
    /* 填 file 结构 */
    memset(file, 0, sizeof(*file));
    file->first_cluster = 0;
    file->current_cluster = 0;
    file->current_offset = 0;
    file->file_size = 0;
    file->dirent_sector_lba = sect_lba;
    file->dirent_entry_idx = idx;
    file->dirent_valid = 1;

    /* 写空目录项：first_cluster = 0，size = 0 */
    write_dirent(vol, sect_lba, idx, name, 0x20, 0, 0);
    ata_flush();

    return OK;
}

/* ---------- 写文件 ---------- */

int fat32_write_file(struct fat32_volume *vol, struct fat32_file *file,
                     const uint8_t *buffer, uint32_t offset, uint32_t size) {
    if (!vol || !vol->valid || !file || !buffer) return EINVAL;
    if (size == 0) return 0;

    uint32_t cluster_size = vol->bpb.sectors_per_cluster * vol->bpb.bytes_per_sector;
    uint32_t end_offset = offset + size;

    /* 1. 扩展：需要写到的位置超过当前 file_size */
    if (end_offset > file->file_size) {
        uint32_t needed_clusters = (end_offset + cluster_size - 1) / cluster_size;

        /* 计算当前簇数 */
        uint32_t current_clusters = 0;
        uint32_t last_c = 0;
        uint32_t c = file->first_cluster;
        uint32_t guard = 0x100000;
        while (c >= 2 && c < 0x0FFFFFF8 && guard-- > 0) {
            current_clusters++;
            last_c = c;
            c = get_next_cluster(vol, c);
        }

        if (file->first_cluster < 2) {
            /* 空文件：分配第一簇 */
            uint32_t nc = alloc_cluster(vol);
            if (nc == 0) return ENOSPC;
            file->first_cluster = nc;
            file->current_cluster = nc;
            current_clusters = 1;
            last_c = nc;
        }

        /* 补足 */
        while (current_clusters < needed_clusters) {
            uint32_t nc = alloc_cluster(vol);
            if (nc == 0) return ENOSPC;
            set_fat_entry(vol, last_c, nc);
            last_c = nc;
            current_clusters++;
        }
    }

    /* 2. 定位到 offset 所在簇 */
    uint32_t cluster = file->first_cluster;
    uint32_t cluster_offset = offset / cluster_size;
    uint32_t skip_bytes = offset % cluster_size;
    for (uint32_t i = 0; i < cluster_offset; i++) {
        cluster = get_next_cluster(vol, cluster);
        if (cluster >= 0x0FFFFFF8) return EIO;
    }

    /* 3. 逐扇区写 */
    uint32_t written = 0;
    while (written < size) {
        uint32_t first_sector = vol->data_start_lba +
                                (cluster - 2) * vol->bpb.sectors_per_cluster;
        uint32_t sector_in_cluster = skip_bytes / vol->bpb.bytes_per_sector;
        uint32_t sector_offset = skip_bytes % vol->bpb.bytes_per_sector;

        uint32_t remaining_in_cluster = cluster_size - skip_bytes;
        uint32_t to_write = (size - written) < remaining_in_cluster
                          ? (size - written) : remaining_in_cluster;

        while (to_write > 0) {
            uint8_t sector[SECTOR_SIZE];
            uint32_t lba = first_sector + sector_in_cluster;

            if (sector_offset != 0 || to_write < SECTOR_SIZE) {
                /* 部分写：先读出该扇区 */
                ata_read_sector(lba, sector);
            } else {
                memset(sector, 0, SECTOR_SIZE);
            }

            uint32_t copy_len = (to_write < (SECTOR_SIZE - sector_offset))
                              ? to_write : (SECTOR_SIZE - sector_offset);
            memcpy(sector + sector_offset, buffer + written, copy_len);

            ata_write_sector(lba, sector);

            written += copy_len;
            to_write -= copy_len;
            sector_in_cluster++;
            sector_offset = 0;
        }

        if (written < size) {
            cluster = get_next_cluster(vol, cluster);
            if (cluster >= 0x0FFFFFF8) return EIO;
            skip_bytes = 0;
        }
    }

    /* 4. 更新 file_size */
    if (end_offset > file->file_size) {
        file->file_size = end_offset;
    }

    /* 5. 更新目录项 */
    if (file->dirent_valid) {
        update_dirent(vol, file->dirent_sector_lba, file->dirent_entry_idx,
                      file->first_cluster, file->file_size);
    }

    /* 一次 flush，覆盖本次写入的所有扇区 */
    ata_flush();

    return (int)size;
}