#ifndef FILE_MAP_H
#define FILE_MAP_H

#include <stdint.h>
#include <volume.h>
#include <fat32.h>      /* 新增：struct file_mapping 内嵌 fat32_file */

#define FILE_MAP_MAX 128

/* 全局缓存页上限（16 MB） */
#define FILE_MAP_TOTAL_PAGES_MAX    4096
/* 单文件缓存页上限（4 MB） */
#define FILE_MAP_PER_FILE_PAGES_MAX 1024
/* 顺序读预读页数（×4KB = 32KB） */
#define FILE_MAP_READAHEAD          8

struct file_mapping {
    int      used;
    int      refcount;
    uint8_t  invalid;
    uint8_t  reserved[3];
    struct volume *vol;
    uint32_t first_cluster;
    uint32_t file_size;
    uint32_t num_pages;
    uint32_t cached_pages;
    uint32_t *pages;

    /* P0：簇链遍历游标。fat32_read_file 用这个推进，
     * 避免每页从首簇重遍历（O(N²) → O(N)）。 */
    struct fat32_file chain_cache;

    /* P1：顺序检测。0xFFFFFFFF = 上次无读。 */
    uint32_t last_page_idx;

    struct file_mapping *lru_prev;
    struct file_mapping *lru_next;
};

void file_map_init(void);
struct file_mapping *file_map_get(struct volume *vol,
                                  uint32_t first_cluster, uint32_t size);
void file_map_ref(struct file_mapping *fm);
void file_map_unref(struct file_mapping *fm);

uint32_t file_map_get_page(struct file_mapping *fm, uint32_t page_idx);
int      file_map_read_page(struct file_mapping *fm, uint32_t page_idx,
                            uint8_t *buf);
void     file_map_invalidate(struct volume *vol, uint32_t first_cluster);
void     file_map_shrink(void);
void     file_map_stats(void);

#endif