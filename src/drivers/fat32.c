#include <fat32.h>
#include <ata.h>
#include <printf.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <path.h>
#include <rtc.h>

#define FAT32_MAX_VOLUMES 4

static struct fat32_volume fat32_volumes[FAT32_MAX_VOLUMES];
static int fat32_volume_count = 0;

/* FAT 扇区单槽缓存（FAT 表访问有强局部性，单槽足够） */
static uint32_t fat_cache_lba = 0xFFFFFFFF;
static uint8_t  fat_cache[SECTOR_SIZE];
static int      fat_cache_valid = 0;
static const struct ata_device *fat_cache_dev = NULL;

static void fat_read_cached(const struct ata_device *dev, uint32_t lba, uint8_t *out) {
    if (fat_cache_valid && fat_cache_lba == lba && fat_cache_dev == dev) {
        memcpy(out, fat_cache, SECTOR_SIZE);
        return;
    }
    ata_read_sector(dev, lba, fat_cache);
    fat_cache_lba = lba;
    fat_cache_dev = dev;
    fat_cache_valid = 1;
    memcpy(out, fat_cache, SECTOR_SIZE);
}

static void fat_write_cached(const struct ata_device *dev, uint32_t lba, const uint8_t *in) {
    memcpy(fat_cache, in, SECTOR_SIZE);
    fat_cache_lba = lba;
    fat_cache_dev = dev;
    fat_cache_valid = 1;
    ata_write_sector(dev, lba, in);
}

/* 判断是否 LFN 项 */
static int is_lfn_entry(const struct fat32_dir_entry *e) {
    return (e->attributes & 0x3F) == 0x0F;
}

/* 把 LFN 项的 13 个字符写到 buf + offset 处。
 * 遇到 0x0000 视为字符串结束。 */
static void lfn_write_at(const struct fat32_dir_entry *e, char *buf,
                         int max, int offset) {
    const uint8_t *raw = (const uint8_t*)e;
    /* 3 段 UCN 的字节偏移（FAT 规范） */
    static const int offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

    for (int i = 0; i < 13; i++) {
        if (offset + i >= max - 1) return;
        uint16_t w = *(uint16_t*)(raw + offs[i]);
        if (w == 0x0000) {
            buf[offset + i] = '\0';
            return;
        }
        if (w == 0xFFFF) {
            /* 填充字符，视为结束 */
            buf[offset + i] = '\0';
            return;
        }
        /* UTF-16 → ASCII 简化：只处理 < 0x80 */
        buf[offset + i] = (w < 0x80) ? (char)w : '?';
    }
}

/* 处理一个 LFN 项：把 13 个字符写进 buf，更新 max_seq。 */
static void lfn_collect(const struct fat32_dir_entry *e,
                        char *buf, int buf_size, int *max_seq) {
    int seq = e->name[0] & 0x1F;
    if (seq < 1 || seq > 20) return;
    lfn_write_at(e, buf, buf_size, (seq - 1) * 13);
    if (seq > *max_seq) *max_seq = seq;
}

/* 构造项名字：优先 LFN，否则 8.3。 */
static void build_name(const struct fat32_dir_entry *e,
                       const char *lfn_buf, int lfn_max_seq,
                       char *out, int out_size) {
    if (lfn_max_seq > 0) {
        int len = lfn_max_seq * 13;
        if (len >= out_size) len = out_size - 1;
        memcpy(out, lfn_buf, len);
        out[len] = '\0';
        return;
    }
    int k = 0;
    for (int i = 0; i < 8 && e->name[i] != ' ' && k < out_size - 2; i++)
        out[k++] = e->name[i];
    if (e->ext[0] != ' ' && k < out_size - 2) {
        out[k++] = '.';
        for (int i = 0; i < 3 && e->ext[i] != ' ' && k < out_size - 1; i++)
            out[k++] = e->ext[i];
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
/* ---------- 内部辅助 ---------- */

static uint32_t get_next_cluster(struct fat32_volume *vol, uint32_t cluster) {
    if (cluster >= 0x0FFFFFF8) return 0x0FFFFFFF;

    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = vol->fat_start_lba + (fat_offset / SECTOR_SIZE);
    uint32_t offset_in_sector = fat_offset % SECTOR_SIZE;

    uint8_t sector1[SECTOR_SIZE];
    uint8_t sector2[SECTOR_SIZE];
    fat_read_cached(vol->dev, fat_sector, sector1);

    uint32_t next = 0;
    if (offset_in_sector + 4 <= SECTOR_SIZE) {
        next = *(uint32_t*)(sector1 + offset_in_sector);
    } else {
        fat_read_cached(vol->dev, fat_sector + 1, sector2);
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
        fat_read_cached(vol->dev, fat_sector, sector);

        uint32_t old = *(uint32_t*)(sector + offset_in_sector);
        uint32_t new_val = (old & 0xF0000000) | (value & 0x0FFFFFFF);
        *(uint32_t*)(sector + offset_in_sector) = new_val;

        fat_write_cached(vol->dev, fat_sector, sector);
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
    ata_read_sector(vol->dev, lba, buffer);
}

static void write_sector_from_cluster(struct fat32_volume *vol, uint32_t cluster,
                                      uint32_t sector_in_cluster,
                                      const uint8_t *buffer) {
    uint32_t lba = vol->data_start_lba +
                   (cluster - 2) * vol->bpb.sectors_per_cluster +
                   sector_in_cluster;
    ata_write_sector(vol->dev, lba, buffer);
}

/* ---------- 挂载 ---------- */

struct fat32_volume *fat32_mount(const struct ata_device *dev,
                                 uint32_t partition_lba) {
    if (!dev || !dev->present) return NULL;
    if (fat32_volume_count >= FAT32_MAX_VOLUMES) {
        kprintf("[FAT32] ERROR: too many volumes\n");
        return NULL;
    }

    struct fat32_volume *vol = &fat32_volumes[fat32_volume_count];
    memset(vol, 0, sizeof(*vol));

    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(dev, partition_lba, sector);

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
    vol->dev = dev;

    fat32_volume_count++;

    kprintf("[FAT32] Mounted at LBA %d: bps=%d spc=%d fat_size=%d root=%d\n",
            partition_lba,
            vol->bpb.bytes_per_sector,
            vol->bpb.sectors_per_cluster,
            vol->bpb.fat_size_32,
            vol->root_cluster);

    return vol;
}

/* 在目录里找一项。
 *   start_cluster  目录起始簇
 *   name           要匹配的名字（大小写不敏感）
 *   want_dir       -1 = 不限；0 = 只要文件；1 = 只要目录
 * 找到返回 1，填 out_e（短名项）、out_sector_lba、out_entry_idx。
 * 未找到返回 0。 */
static int find_entry_in_dir(struct fat32_volume *vol, uint32_t start_cluster,
                             const char *name, int want_dir,
                             struct fat32_dir_entry *out_e,
                             uint32_t *out_sector_lba,
                             uint32_t *out_entry_idx,
                             int *out_lfn_count) {
    uint32_t cluster = start_cluster;
    uint32_t guard = 1024;
    char lfn_buf[260];
    int  lfn_max_seq = 0;

    while (cluster < 0x0FFFFFF8 && guard-- > 0) {
        int sectors = vol->bpb.sectors_per_cluster;
        int eps = vol->bpb.bytes_per_sector / sizeof(struct fat32_dir_entry);

        for (int s = 0; s < sectors; s++) {
            uint8_t sector[SECTOR_SIZE];
            uint32_t lba = vol->data_start_lba +
                           (cluster - 2) * vol->bpb.sectors_per_cluster + s;
            ata_read_sector(vol->dev, lba, sector);
            struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;

            for (int j = 0; j < eps; j++) {
                struct fat32_dir_entry *e = &entries[j];

                if (e->name[0] == 0x00) {
                    /* 跳过本簇剩余部分，继续下一簇 */
                    s = sectors;   /* 让 s 循环结束 */
                    break;         /* 跳出 j 循环 */
                }
                if (e->name[0] == 0xE5) {
                    lfn_max_seq = 0;
                    continue;
                }
                if (is_lfn_entry(e)) {
                    lfn_collect(e, lfn_buf, sizeof(lfn_buf), &lfn_max_seq);
                    continue;
                }

                /* 跳过 . / ..（无 LFN 时的短名） */
                if (lfn_max_seq == 0 &&
                    e->name[0] == '.' &&
                    (e->name[1] == ' ' ||
                     (e->name[1] == '.' && e->name[2] == ' '))) {
                    continue;
                }

                char ename[260];
                build_name(e, lfn_buf, lfn_max_seq, ename, sizeof(ename));
                lfn_max_seq = 0;

                if (!name_eq(ename, name)) continue;

                int is_dir = (e->attributes & 0x10) ? 1 : 0;
                if (want_dir == 1 && !is_dir) continue;
                if (want_dir == 0 && is_dir)  continue;

                if (out_lfn_count) *out_lfn_count = lfn_max_seq;
                if (out_e) *out_e = *e;
                if (out_sector_lba) *out_sector_lba = lba;
                if (out_entry_idx) *out_entry_idx = j;
                return 1;
            }
        }
        cluster = get_next_cluster(vol, cluster);
    }
    return 0;
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

        struct fat32_dir_entry e;
        uint32_t lba, idx;
        if (!find_entry_in_dir(vol, current_cluster, token, -1,
                               &e, &lba, &idx, NULL)) {
            return ENOENT;
        }

        uint32_t entry_cluster = (e.cluster_high << 16) | e.cluster_low;

        if (e.attributes & 0x10) {
            /* 目录：如果这是路径最后一段，说明在打开目录 → EISDIR */
            if (*path == '\0') return EISDIR;
            current_cluster = entry_cluster;
        } else {
            /* 文件：填句柄 */
            file->first_cluster = entry_cluster;
            file->current_cluster = entry_cluster;
            file->current_offset = 0;
            file->file_size = e.file_size;
            file->dirent_sector_lba = lba;
            file->dirent_entry_idx = idx;
            file->dirent_valid = 1;
            file->create_time = e.create_time;
            file->create_date = e.create_date;
            file->mod_time    = e.mod_time;
            file->mod_date    = e.mod_date;
            return OK;
        }
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
            ata_read_sector(vol->dev, first_sector + sector_in_cluster, sector);
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

        struct fat32_dir_entry e;
        if (!find_entry_in_dir(vol, current_cluster, token, -1,
                               &e, NULL, NULL, NULL)) {
            return 0;
        }

        if (is_last) {
            return (e.attributes & 0x10) ? 1 : 0;
        }
        if (!(e.attributes & 0x10)) return 0;
        current_cluster = (e.cluster_high << 16) | e.cluster_low;
    }
    return 0;
}

/* ---------- 目录迭代 ---------- */
int fat32_opendir(struct fat32_volume *vol, const char *path,
                  struct fat32_dir *dir) {
    if (!vol || !vol->valid || !dir) return EINVAL;

    uint32_t cluster;

    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        cluster = vol->root_cluster;
    } else {
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

            struct fat32_dir_entry e;
            if (!find_entry_in_dir(vol, cur, token, 1, &e, NULL, NULL, NULL)) {
                return ENOENT;
            }
            cur = (e.cluster_high << 16) | e.cluster_low;
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

    char lfn_buf[260];
    int  lfn_max_seq = 0;

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

                if (e->name[0] == 0x00) {
                    /* 本簇剩余部分视为空，跳到下一簇 */
                    dir->sector = (uint32_t)sectors;
                    dir->entry = 0;
                    break;
                }
                if (e->name[0] == 0xE5) {
                    lfn_max_seq = 0;
                    continue;
                }

                if (is_lfn_entry(e)) {
                    int seq = e->name[0] & 0x1F;
                    if (seq >= 1 && seq <= 20) {
                        lfn_write_at(e, lfn_buf, sizeof(lfn_buf),
                                     (seq - 1) * 13);
                        if (seq > lfn_max_seq) lfn_max_seq = seq;
                    }
                    continue;
                }

                /* 精确判断 . / ..：无 LFN 且短名是 "." 或 ".." */
                if (lfn_max_seq == 0 &&
                    e->name[0] == '.' &&
                    (e->name[1] == ' ' ||
                     (e->name[1] == '.' && e->name[2] == ' '))) {
                    continue;
                }

                int k = 0;
                if (lfn_max_seq > 0) {
                    lfn_buf[lfn_max_seq * 13] = '\0';
                    strncpy(out->name, lfn_buf, DIRENT_NAME_MAX - 1);
                    out->name[DIRENT_NAME_MAX - 1] = '\0';
                    lfn_max_seq = 0;
                } else {
                    for (int m = 0; m < 8 && e->name[m] != ' ' &&
                                    k < DIRENT_NAME_MAX - 2; m++)
                        out->name[k++] = e->name[m];
                    if (e->ext[0] != ' ' && k < DIRENT_NAME_MAX - 2) {
                        out->name[k++] = '.';
                        for (int m = 0; m < 3 && e->ext[m] != ' ' &&
                                        k < DIRENT_NAME_MAX - 1; m++)
                            out->name[k++] = e->ext[m];
                    }
                    out->name[k] = '\0';
                }
                out->attributes = e->attributes;
                out->size = e->file_size;
                out->mod_date = e->mod_date;
                out->mod_time = e->mod_time;
                out->pad = 0;
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

/* 当前 RTC 时间 → FAT32 目录项的 time/date 格式。
 * time: hour(5)<<11 | minute(6)<<5 | (second/2)(5)
 * date: (year-1980)(7)<<9 | month(4)<<5 | day(5)
 * RTC 读失败时填 0（目录项显示 1980-00-00，可接受）。 */
static void fat32_now(uint16_t *out_time, uint16_t *out_date) {
    struct rtc_time t;
    if (rtc_read(&t) < 0) {
        *out_time = 0;
        *out_date = 0;
        return;
    }
    uint16_t year = (t.year >= 1980) ? (uint16_t)(t.year - 1980) : 0;
    if (year > 127) year = 127;
    *out_time = (uint16_t)(((t.hour & 0x1F) << 11) |
                           ((t.minute & 0x3F) << 5) |
                           ((t.second / 2) & 0x1F));
    *out_date = (uint16_t)((year << 9) |
                           ((t.month & 0x0F) << 5) |
                           (t.day & 0x1F));
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

/* path → dir_path + name。dir_path 至少 PATH_MAX_LEN 字节，
 * name 至少 64 字节。成功返回 0，失败返回 -1。 */
static int split_parent_name(const char *path, char *dir_path, char *name) {
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
        while (p[k] && k < 63) { name[k] = p[k]; k++; }
        name[k] = '\0';
    } else {
        int dlen = last_slash - p;
        if (dlen >= PATH_MAX_LEN - 1) return -1;
        dir_path[0] = '/';
        memcpy(dir_path + 1, p, dlen);
        dir_path[dlen + 1] = '\0';

        int k = 0;
        const char *np = last_slash + 1;
        while (np[k] && k < 63) { name[k] = np[k]; k++; }
        name[k] = '\0';
    }
    return (name[0] == '\0') ? -1 : 0;
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
            ata_read_sector(vol->dev, lba, sector);
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

/* 名字是否 8.3 兼容 */
static int is_83_compatible(const char *name) {
    int main_len = 0, ext_len = 0;
    int in_ext = 0;

    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (c == '.') {
            if (in_ext) return 0;
            in_ext = 1;
            continue;
        }
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return 0;
        }
        if (in_ext) {
            if (++ext_len > 3) return 0;
        } else {
            if (++main_len > 8) return 0;
        }
    }
    return main_len > 0;
}

/* 短名冲突时的最大尝试次数 */
#define SHORT_NAME_MAX_TRIES  1000000

/* 生成 8.3 短名。8.3 兼容且不冲突则直接用；否则生成 ~N 形式。
 * 返回 0 成功，-1 失败。 */
static int make_short_name(struct fat32_volume *vol, uint32_t dir_cluster,
                           const char *name, uint8_t *out8, uint8_t *out3) {
    /* 拆分主名/扩展名并大写，过滤非法字符 */
    char clean_main[16] = {0};
    char clean_ext[8] = {0};
    int cm = 0, ce = 0;
    int in_ext = 0;

    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (c == '.') { in_ext = 1; continue; }
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            continue;
        }
        if (in_ext) {
            if (ce < 3) clean_ext[ce++] = c;
        } else {
            if (cm < 8) clean_main[cm++] = c;
        }
    }
    clean_main[cm] = '\0';
    clean_ext[ce] = '\0';

    if (cm == 0) {
        clean_main[0] = '_';
        clean_main[1] = '\0';
        cm = 1;
    }

    /* 尝试直接用（如果不冲突） */
    if (is_83_compatible(name) && cm <= 8 && ce <= 3) {
        char test[13];
        int k = 0;
        for (int i = 0; i < cm; i++) test[k++] = clean_main[i];
        if (ce > 0) {
            test[k++] = '.';
            for (int i = 0; i < ce; i++) test[k++] = clean_ext[i];
        }
        test[k] = '\0';

        if (!find_entry_in_dir(vol, dir_cluster, test, -1,
                               NULL, NULL, NULL, NULL)) {
            memset(out8, ' ', 8);
            memset(out3, ' ', 3);
            for (int i = 0; i < cm; i++) out8[i] = clean_main[i];
            for (int i = 0; i < ce; i++) out3[i] = clean_ext[i];
            return 0;
        }
    }

    /* 生成 ~N 后缀 */
    for (int n = 1; n < SHORT_NAME_MAX_TRIES; n++) {
        char digits[8];
        int dn = 0;
        int t = n;
        while (t > 0) { digits[dn++] = '0' + (t % 10); t /= 10; }
        if (dn == 0) digits[dn++] = '0';

        int suffix_len = 1 + dn;    /* '~' + digits */
        int prefix_len = 8 - suffix_len;
        if (prefix_len < 1) prefix_len = 1;

        memset(out8, ' ', 8);
        memset(out3, ' ', 3);

        int copy = cm < prefix_len ? cm : prefix_len;
        for (int i = 0; i < copy; i++) out8[i] = clean_main[i];
        out8[copy] = '~';
        for (int i = 0; i < dn; i++) out8[copy + 1 + i] = digits[dn - 1 - i];
        for (int i = 0; i < ce; i++) out3[i] = clean_ext[i];

        /* 检查冲突 */
        char test[13];
        int k = 0;
        for (int i = 0; i < 8 && out8[i] != ' '; i++) test[k++] = out8[i];
        if (out3[0] != ' ') {
            test[k++] = '.';
            for (int i = 0; i < 3 && out3[i] != ' '; i++) test[k++] = out3[i];
        }
        test[k] = '\0';

        if (!find_entry_in_dir(vol, dir_cluster, test, -1,
                               NULL, NULL, NULL, NULL)) {
            return 0;
        }
    }
    return -1;
}

/* 在目录里找 n 个连续空闲目录项（不跨扇区）。
 * 找到返回 1，填 out_sector_lba / out_entry_idx。
 * 找不到返回 0。 */
static int find_n_free_dirents(struct fat32_volume *vol, uint32_t dir_cluster,
                               int n,
                               uint32_t *out_sector_lba, uint32_t *out_entry_idx,
                               uint32_t *out_last_cluster) {
    int sectors = vol->bpb.sectors_per_cluster;
    int eps = vol->bpb.bytes_per_sector / sizeof(struct fat32_dir_entry);
    if (n < 1 || n > eps) return 0;

    uint32_t cluster = dir_cluster;
    uint32_t guard = 1024;
    while (cluster < 0x0FFFFFF8 && guard-- > 0) {
        if (out_last_cluster) *out_last_cluster = cluster;

        for (int s = 0; s < sectors; s++) {
            uint8_t sector[SECTOR_SIZE];
            uint32_t lba = vol->data_start_lba +
                           (cluster - 2) * vol->bpb.sectors_per_cluster + s;
            ata_read_sector(vol->dev, lba, sector);
            struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;

            int run = 0, run_start = 0;
            for (int j = 0; j < eps; j++) {
                struct fat32_dir_entry *e = &entries[j];
                if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
                    if (run == 0) run_start = j;
                    if (++run >= n) {
                        if (out_sector_lba) *out_sector_lba = lba;
                        if (out_entry_idx)  *out_entry_idx = run_start;
                        return 1;
                    }
                } else {
                    run = 0;
                }
            }
        }
        cluster = get_next_cluster(vol, cluster);
    }
    return 0;
}

/* 填一个 LFN 项：包含 name 从 char_start 起的 13 个字符。 */
static void fill_lfn_entry(struct fat32_dir_entry *e, const char *name,
                           int name_len, int char_start) {
    static const int offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    uint8_t *raw = (uint8_t*)e;
    memset(e, 0, sizeof(*e));

    int ended = 0;
    for (int i = 0; i < 13; i++) {
        int ci = char_start + i;
        uint16_t w;
        if (ended) {
            w = 0xFFFF;
        } else if (ci >= name_len) {
            w = 0x0000;
            ended = 1;
        } else {
            w = (uint8_t)name[ci];
        }
        *(uint16_t*)(raw + offs[i]) = w;
    }
}

/* 写一个长名文件的完整目录项序列（LFN 项 + 短名项）。
 * 起始位置 (sect_lba, entry_idx)，前 (n_lfn + 1) 项必须都在同一扇区。 */
static void write_lfn_sequence(struct fat32_volume *vol,
                               uint32_t sect_lba, uint32_t entry_idx,
                               const char *name, int name_len,
                               const uint8_t *name8, const uint8_t *ext3,
                               uint8_t attr, uint32_t first_cluster,
                               uint32_t size,
                               uint16_t *out_time, uint16_t *out_date) {
    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(vol->dev, sect_lba, sector);
    struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;

    int n_lfn = (name_len + 12) / 13;

    uint8_t sum = 0;
    for (int i = 0; i < 8; i++)
        sum = ((sum & 1) << 7) + (sum >> 1) + name8[i];
    for (int i = 0; i < 3; i++)
        sum = ((sum & 1) << 7) + (sum >> 1) + ext3[i];

    for (int k = 0; k < n_lfn; k++) {
        int seq = k + 1;
        int char_start = k * 13;
        struct fat32_dir_entry *e = &entries[entry_idx + (n_lfn - seq)];
        fill_lfn_entry(e, name, name_len, char_start);
        e->name[0] = (seq == n_lfn) ? (seq | 0x40) : seq;
        e->attributes = 0x0F;
        e->reserved = 0;
        e->create_time_tenth = sum;
        e->cluster_low = 0;
    }

    struct fat32_dir_entry *se = &entries[entry_idx + n_lfn];
    memset(se, 0, sizeof(*se));
    memcpy(se->name, name8, 8);
    memcpy(se->ext, ext3, 3);
    se->attributes = attr;
    se->cluster_high = (first_cluster >> 16) & 0xFFFF;
    se->cluster_low  = first_cluster & 0xFFFF;
    se->file_size = size;

    uint16_t t, d;
    fat32_now(&t, &d);
    se->create_time = t;
    se->create_date = d;
    se->mod_time    = t;
    se->mod_date    = d;
    se->access_date = d;

    ata_write_sector(vol->dev, sect_lba, sector);

    if (out_time) *out_time = t;
    if (out_date) *out_date = d;
}

/* 写目录项 */
static void write_dirent(struct fat32_volume *vol, uint32_t sector_lba,
                         uint32_t entry_idx, const char *name,
                         uint8_t attr, uint32_t first_cluster, uint32_t size,
                         uint16_t *out_time, uint16_t *out_date) {
    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(vol->dev, sector_lba, sector);
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

    uint16_t t, d;
    fat32_now(&t, &d);
    e->create_time = t;
    e->create_date = d;
    e->mod_time    = t;
    e->mod_date    = d;
    e->access_date = d;

    ata_write_sector(vol->dev, sector_lba, sector);

    if (out_time) *out_time = t;
    if (out_date) *out_date = d;
}

/* 更新目录项的 first_cluster / file_size */
static void update_dirent(struct fat32_volume *vol, uint32_t sector_lba,
                          uint32_t entry_idx, uint32_t first_cluster,
                          uint32_t size,
                          uint16_t *out_time, uint16_t *out_date) {
    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(vol->dev, sector_lba, sector);
    struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;
    struct fat32_dir_entry *e = &entries[entry_idx];

    e->cluster_high = (first_cluster >> 16) & 0xFFFF;
    e->cluster_low  = first_cluster & 0xFFFF;
    e->file_size    = size;

    uint16_t t, d;
    fat32_now(&t, &d);
    e->mod_time    = t;
    e->mod_date    = d;
    e->access_date = d;

    ata_write_sector(vol->dev, sector_lba, sector);

    if (out_time) *out_time = t;
    if (out_date) *out_date = d;
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
    if (find_entry_in_dir(vol, dir.cur_cluster, name, -1, &found, &sect_lba, &idx, NULL)) {
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
        file->create_time = found.create_time;
        file->create_date = found.create_date;
        file->mod_time    = found.mod_time;
        file->mod_date    = found.mod_date;
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

    
    /* 判断名字是否需要 LFN */
    int name_len = strlen(name);

    /* 195 = 15 个 LFN 项 × 13 字符。超过则一扇区放不下。 */
    if (name_len > 195) return ENAMETOOLONG;

    if (is_83_compatible(name)) {
        memset(file, 0, sizeof(*file));
        file->first_cluster = 0;
        file->current_cluster = 0;
        file->current_offset = 0;
        file->file_size = 0;
        file->dirent_sector_lba = sect_lba;
        file->dirent_entry_idx = idx;
        file->dirent_valid = 1;

        uint16_t t, d;
        write_dirent(vol, sect_lba, idx, name, 0x20, 0, 0, &t, &d);
        file->create_time = t;
        file->create_date = d;
        file->mod_time    = t;
        file->mod_date    = d;

        ata_flush(vol->dev);
        return OK;
    }

    /* 需要 LFN */
    uint8_t name8[8], ext3[3];
    if (make_short_name(vol, dir.cur_cluster, name, name8, ext3) < 0) {
        return ENAMETOOLONG;
    }

    int n_lfn = (name_len + 12) / 13;
    int total = n_lfn + 1;

    /* 重新找连续 total 项 */
    uint32_t lba2, idx2, last2 = 0;
    if (!find_n_free_dirents(vol, dir.cur_cluster, total,
                             &lba2, &idx2, &last2)) {
        /* 目录满，扩展一簇 */
        uint32_t new_cluster = alloc_cluster(vol);
        if (new_cluster == 0) return ENOSPC;
        set_fat_entry(vol, last2, new_cluster);

        uint8_t zero[SECTOR_SIZE];
        memset(zero, 0, SECTOR_SIZE);
        for (int s = 0; s < vol->bpb.sectors_per_cluster; s++) {
            write_sector_from_cluster(vol, new_cluster, s, zero);
        }

        lba2 = vol->data_start_lba +
               (new_cluster - 2) * vol->bpb.sectors_per_cluster;
        idx2 = 0;

        if (total > (int)(vol->bpb.bytes_per_sector /
                    sizeof(struct fat32_dir_entry))) {
            return ENAMETOOLONG;
        }
    }

    /* 填 file 结构（短名项在 lba2/idx2+n_lfn） */
    memset(file, 0, sizeof(*file));
    file->first_cluster = 0;
    file->current_cluster = 0;
    file->current_offset = 0;
    file->file_size = 0;
    file->dirent_sector_lba = lba2;
    file->dirent_entry_idx  = idx2 + n_lfn;
    file->dirent_valid = 1;

    uint16_t t, d;
    write_lfn_sequence(vol, lba2, idx2, name, name_len,
                       name8, ext3, 0x20, 0, 0, &t, &d);
    file->create_time = t;
    file->create_date = d;
    file->mod_time    = t;
    file->mod_date    = d;
    ata_flush(vol->dev);
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
                ata_read_sector(vol->dev, lba, sector);
            } else {
                memset(sector, 0, SECTOR_SIZE);
            }

            uint32_t copy_len = (to_write < (SECTOR_SIZE - sector_offset))
                              ? to_write : (SECTOR_SIZE - sector_offset);
            memcpy(sector + sector_offset, buffer + written, copy_len);

            ata_write_sector(vol->dev, lba, sector);

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
        uint16_t t, d;
        update_dirent(vol, file->dirent_sector_lba, file->dirent_entry_idx,
                      file->first_cluster, file->file_size, &t, &d);
        file->mod_time = t;
        file->mod_date = d;
    }

    /* 一次 flush，覆盖本次写入的所有扇区 */
    ata_flush(vol->dev);

    return (int)size;
}

/* ---------- 截断 ---------- */

int fat32_truncate(struct fat32_volume *vol, struct fat32_file *file) {
    if (!vol || !vol->valid || !file) return EINVAL;

    /* 释放簇链（空文件 first_cluster < 2，跳过） */
    if (file->first_cluster >= 2) {
        free_cluster_chain(vol, file->first_cluster);
    }

    file->first_cluster = 0;
    file->current_cluster = 0;
    file->current_offset = 0;
    file->file_size = 0;

    if (file->dirent_valid) {
        uint16_t t, d;
        update_dirent(vol, file->dirent_sector_lba, file->dirent_entry_idx,
                      0, 0, &t, &d);
        file->mod_time = t;
        file->mod_date = d;
    }
    ata_flush(vol->dev);
    return OK;
}

/* ---------- unlink ---------- */

int fat32_unlink(struct fat32_volume *vol, const char *path) {
    if (!vol || !vol->valid || !path) return EINVAL;

    char dir_path[PATH_MAX_LEN];
    char name[64];
    if (split_parent_name(path, dir_path, name) < 0) return EINVAL;

    struct fat32_dir parent;
    if (fat32_opendir(vol, dir_path, &parent) != OK) return ENOENT;

    struct fat32_dir_entry e;
    uint32_t sect_lba, idx;
    int lfn_count = 0;
    if (!find_entry_in_dir(vol, parent.start_cluster, name, -1,
                           &e, &sect_lba, &idx, &lfn_count)) {
        return ENOENT;
    }
    if (e.attributes & 0x10) return EISDIR;

    /* 释放簇链 */
    uint32_t fc = (e.cluster_high << 16) | e.cluster_low;
    if (fc >= 2) free_cluster_chain(vol, fc);

    /* 标记短名项 + 前面的 LFN 项为 0xE5 */
    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(vol->dev, sect_lba, sector);
    struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;
    int eps = vol->bpb.bytes_per_sector / sizeof(struct fat32_dir_entry);

    for (int i = 0; i < lfn_count; i++) {
        int j = (int)idx - lfn_count + i;
        if (j >= 0 && j < eps) entries[j].name[0] = 0xE5;
    }
    entries[idx].name[0] = 0xE5;
    ata_write_sector(vol->dev, sect_lba, sector);
    ata_flush(vol->dev);
    return OK;
}

/* ---------- mkdir ---------- */

int fat32_mkdir(struct fat32_volume *vol, const char *path) {
    if (!vol || !vol->valid || !path) return EINVAL;

    char dir_path[PATH_MAX_LEN];
    char name[64];
    if (split_parent_name(path, dir_path, name) < 0) return EINVAL;

    struct fat32_dir parent;
    if (fat32_opendir(vol, dir_path, &parent) != OK) return ENOENT;

    if (find_entry_in_dir(vol, parent.start_cluster, name, -1,
                          NULL, NULL, NULL, NULL)) {
        return EEXIST;
    }

    uint32_t new_cluster = alloc_cluster(vol);
    if (new_cluster == 0) return ENOSPC;

    /* 新簇第一扇区写 . 和 .. */
    uint8_t sector[SECTOR_SIZE];
    memset(sector, 0, SECTOR_SIZE);
    struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;

    /* "." → 自己 */
    memset(entries[0].name, ' ', 8);
    memset(entries[0].ext, ' ', 3);
    entries[0].name[0] = '.';
    entries[0].attributes = 0x10;
    entries[0].cluster_high = (new_cluster >> 16) & 0xFFFF;
    entries[0].cluster_low  = new_cluster & 0xFFFF;

    /* ".." → 父目录 */
    memset(entries[1].name, ' ', 8);
    memset(entries[1].ext, ' ', 3);
    entries[1].name[0] = '.';
    entries[1].name[1] = '.';
    entries[1].attributes = 0x10;
    uint32_t pc = parent.start_cluster;
    entries[1].cluster_high = (pc >> 16) & 0xFFFF;
    entries[1].cluster_low  = pc & 0xFFFF;

    write_sector_from_cluster(vol, new_cluster, 0, sector);

    uint8_t zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);
    for (int s = 1; s < vol->bpb.sectors_per_cluster; s++) {
        write_sector_from_cluster(vol, new_cluster, s, zero);
    }

    /* 在父目录写目录项 */
    uint32_t last_cluster = 0;
    uint32_t sect_lba, idx;
    if (!find_free_dirent(vol, parent.start_cluster, &sect_lba, &idx, &last_cluster)) {
        uint32_t nc = alloc_cluster(vol);
        if (nc == 0) { free_cluster_chain(vol, new_cluster); return ENOSPC; }
        set_fat_entry(vol, last_cluster, nc);
        for (int s = 0; s < vol->bpb.sectors_per_cluster; s++) {
            write_sector_from_cluster(vol, nc, s, zero);
        }
        sect_lba = vol->data_start_lba +
                   (nc - 2) * vol->bpb.sectors_per_cluster;
        idx = 0;
    }

    int name_len = strlen(name);
    if (name_len > 195) {
        free_cluster_chain(vol, new_cluster);
        return ENAMETOOLONG;
    }

    if (is_83_compatible(name)) {
        write_dirent(vol, sect_lba, idx, name, 0x10, new_cluster, 0, NULL, NULL);
        ata_flush(vol->dev);
        return OK;
    }

    uint8_t name8[8], ext3[3];
    if (make_short_name(vol, parent.start_cluster, name, name8, ext3) < 0) {
        free_cluster_chain(vol, new_cluster);
        return ENAMETOOLONG;
    }

    int n_lfn = (name_len + 12) / 13;
    int total = n_lfn + 1;
    uint32_t lba2, idx2, last2 = 0;
    if (!find_n_free_dirents(vol, parent.start_cluster, total,
                             &lba2, &idx2, &last2)) {
        uint32_t nc = alloc_cluster(vol);
        if (nc == 0) { free_cluster_chain(vol, new_cluster); return ENOSPC; }
        set_fat_entry(vol, last2, nc);
        for (int s = 0; s < vol->bpb.sectors_per_cluster; s++) {
            write_sector_from_cluster(vol, nc, s, zero);
        }
        lba2 = vol->data_start_lba + (nc - 2) * vol->bpb.sectors_per_cluster;
        idx2 = 0;
    }

    write_lfn_sequence(vol, lba2, idx2, name, name_len,
                       name8, ext3, 0x10, new_cluster, 0, NULL, NULL);
    ata_flush(vol->dev);
    return OK;
}

/* ---------- rmdir ---------- */

int fat32_rmdir(struct fat32_volume *vol, const char *path) {
    if (!vol || !vol->valid || !path) return EINVAL;

    char dir_path[PATH_MAX_LEN];
    char name[64];
    if (split_parent_name(path, dir_path, name) < 0) return EINVAL;

    struct fat32_dir parent;
    if (fat32_opendir(vol, dir_path, &parent) != OK) return ENOENT;

    struct fat32_dir_entry e;
    uint32_t sect_lba, idx;
    int lfn_count = 0;
    if (!find_entry_in_dir(vol, parent.start_cluster, name, -1,
                           &e, &sect_lba, &idx, &lfn_count)) {
        return ENOENT;
    }
    if (!(e.attributes & 0x10)) return ENOTDIR;

    uint32_t target_cluster = (e.cluster_high << 16) | e.cluster_low;
    if (target_cluster == vol->root_cluster) return EINVAL;

    /* 检查空：除 . / .. 外不能有项 */
    int eps = vol->bpb.bytes_per_sector / sizeof(struct fat32_dir_entry);
    int sectors = vol->bpb.sectors_per_cluster;
    uint32_t c = target_cluster;
    uint32_t guard = 1024;
    int empty = 1;

    while (c >= 2 && c < 0x0FFFFFF8 && guard-- > 0) {
        for (int s = 0; s < sectors && empty; s++) {
            uint8_t sector[SECTOR_SIZE];
            read_sector_from_cluster(vol, c, s, sector);
            struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;
            for (int j = 0; j < eps; j++) {
                struct fat32_dir_entry *p = &entries[j];
                if (p->name[0] == 0x00) { c = 0x0FFFFFF8; break; }  /* 到末尾 */
                if (p->name[0] == 0xE5) continue;
                if (is_lfn_entry(p)) continue;
                if (p->name[0] == '.' &&
                    (p->name[1] == ' ' ||
                     (p->name[1] == '.' && p->name[2] == ' '))) {
                    continue;
                }
                empty = 0;
                break;
            }
        }
        if (!empty) break;
        c = get_next_cluster(vol, c);
    }
    if (!empty) return ENOTEMPTY;

    free_cluster_chain(vol, target_cluster);

    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(vol->dev, sect_lba, sector);
    struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;
    for (int i = 0; i < lfn_count; i++) {
        int j = (int)idx - lfn_count + i;
        if (j >= 0 && j < eps) entries[j].name[0] = 0xE5;
    }
    entries[idx].name[0] = 0xE5;
    ata_write_sector(vol->dev, sect_lba, sector);
    ata_flush(vol->dev);
    return OK;
}

/* ---------- rename ---------- */

int fat32_rename(struct fat32_volume *vol, const char *old_path,
                 const char *new_path) {
    if (!vol || !vol->valid || !old_path || !new_path) return EINVAL;

    char old_dir[PATH_MAX_LEN], old_name[64];
    if (split_parent_name(old_path, old_dir, old_name) < 0) return EINVAL;

    char new_dir[PATH_MAX_LEN], new_name[64];
    if (split_parent_name(new_path, new_dir, new_name) < 0) return EINVAL;

    struct fat32_dir old_parent;
    if (fat32_opendir(vol, old_dir, &old_parent) != OK) return ENOENT;

    struct fat32_dir_entry e;
    uint32_t old_lba, old_idx;
    int old_lfn = 0;
    if (!find_entry_in_dir(vol, old_parent.start_cluster, old_name, -1,
                           &e, &old_lba, &old_idx, &old_lfn)) {
        return ENOENT;
    }

    uint32_t first_cluster = (e.cluster_high << 16) | e.cluster_low;
    uint32_t file_size = e.file_size;
    uint8_t attr = e.attributes;

    struct fat32_dir new_parent;
    if (fat32_opendir(vol, new_dir, &new_parent) != OK) return ENOENT;

    if (find_entry_in_dir(vol, new_parent.start_cluster, new_name, -1,
                          NULL, NULL, NULL, NULL)) {
        return EEXIST;
    }

    int name_len = strlen(new_name);
    if (name_len > 195) return ENAMETOOLONG;

    uint32_t last_cluster = 0;
    uint32_t sect_lba, idx;
    if (!find_free_dirent(vol, new_parent.start_cluster,
                          &sect_lba, &idx, &last_cluster)) {
        uint32_t nc = alloc_cluster(vol);
        if (nc == 0) return ENOSPC;
        set_fat_entry(vol, last_cluster, nc);
        uint8_t zero[SECTOR_SIZE];
        memset(zero, 0, SECTOR_SIZE);
        for (int s = 0; s < vol->bpb.sectors_per_cluster; s++) {
            write_sector_from_cluster(vol, nc, s, zero);
        }
        sect_lba = vol->data_start_lba +
                   (nc - 2) * vol->bpb.sectors_per_cluster;
        idx = 0;
    }

    if (is_83_compatible(new_name)) {
        write_dirent(vol, sect_lba, idx, new_name,
                     attr, first_cluster, file_size, NULL, NULL);
    } else {
        uint8_t name8[8], ext3[3];
        if (make_short_name(vol, new_parent.start_cluster,
                            new_name, name8, ext3) < 0) {
            return ENAMETOOLONG;
        }
        int n_lfn = (name_len + 12) / 13;
        int total = n_lfn + 1;
        uint32_t lba2, idx2, last2 = 0;
        if (!find_n_free_dirents(vol, new_parent.start_cluster, total,
                                 &lba2, &idx2, &last2)) {
            uint32_t nc = alloc_cluster(vol);
            if (nc == 0) return ENOSPC;
            set_fat_entry(vol, last2, nc);
            uint8_t zero[SECTOR_SIZE];
            memset(zero, 0, SECTOR_SIZE);
            for (int s = 0; s < vol->bpb.sectors_per_cluster; s++) {
                write_sector_from_cluster(vol, nc, s, zero);
            }
            lba2 = vol->data_start_lba +
                   (nc - 2) * vol->bpb.sectors_per_cluster;
            idx2 = 0;
        }
        write_lfn_sequence(vol, lba2, idx2, new_name, name_len,
                           name8, ext3, attr, first_cluster, file_size, NULL, NULL);
    }

    /* 删源目录项 */
    uint8_t sector[SECTOR_SIZE];
    ata_read_sector(vol->dev, old_lba, sector);
    struct fat32_dir_entry *entries = (struct fat32_dir_entry*)sector;
    int eps = vol->bpb.bytes_per_sector / sizeof(struct fat32_dir_entry);
    for (int i = 0; i < old_lfn; i++) {
        int j = (int)old_idx - old_lfn + i;
        if (j >= 0 && j < eps) entries[j].name[0] = 0xE5;
    }
    entries[old_idx].name[0] = 0xE5;
    ata_write_sector(vol->dev, old_lba, sector);
    ata_flush(vol->dev);
    return OK;
}