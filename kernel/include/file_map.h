#ifndef FILE_MAP_H
#define FILE_MAP_H

#include <stdint.h>
#include <volume.h>

#define FILE_MAP_MAX 32

struct file_mapping {
    int      used;
    int      refcount;
    struct volume *vol;
    uint32_t first_cluster;   /* FAT32 的唯一标识 */
    uint32_t file_size;
    uint32_t num_pages;
    uint32_t *pages;          /* 物理页数组 */
};

void file_map_init(void);
struct file_mapping *file_map_get(struct volume *vol, uint32_t first_cluster, uint32_t size);
void file_map_ref(struct file_mapping *fm);
void file_map_unref(struct file_mapping *fm);
uint32_t file_map_get_page(struct file_mapping *fm, uint32_t page_idx);

#endif