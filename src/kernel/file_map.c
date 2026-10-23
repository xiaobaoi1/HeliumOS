#include <file_map.h>
#include <pmm.h>
#include <heap.h>
#include <fat32.h>
#include <printf.h>
#include <string.h>
#include <errno.h>

static struct file_mapping g_file_maps[FILE_MAP_MAX];

void file_map_init(void) {
    memset(g_file_maps, 0, sizeof(g_file_maps));
}

struct file_mapping *file_map_get(struct volume *vol, uint32_t first_cluster, uint32_t size) {
    /* 1. 查找是否已缓存 */
    for (int i = 0; i < FILE_MAP_MAX; i++) {
        if (g_file_maps[i].used &&
            g_file_maps[i].vol == vol &&
            g_file_maps[i].first_cluster == first_cluster) {
            return &g_file_maps[i];
        }
    }

    /* 2. 分配新节点 */
    for (int i = 0; i < FILE_MAP_MAX; i++) {
        if (!g_file_maps[i].used) {
            struct file_mapping *fm = &g_file_maps[i];
            memset(fm, 0, sizeof(*fm));
            fm->used = 1;
            fm->refcount = 0;
            fm->vol = vol;
            fm->first_cluster = first_cluster;
            fm->file_size = size;
            fm->num_pages = (size + 4095) / 4096;
            if (fm->num_pages > 0) {
                fm->pages = (uint32_t*)kmalloc(fm->num_pages * sizeof(uint32_t));
                if (!fm->pages) {
                    fm->used = 0;
                    return NULL;
                }
                memset(fm->pages, 0, fm->num_pages * sizeof(uint32_t));
            }
            return fm;
        }
    }

    return NULL; /* 池满 */
}

void file_map_ref(struct file_mapping *fm) {
    if (fm) fm->refcount++;
}

void file_map_unref(struct file_mapping *fm) {
    if (!fm || fm->refcount <= 0) return;
    fm->refcount--;
    if (fm->refcount == 0) {
        for (uint32_t i = 0; i < fm->num_pages; i++) {
            if (fm->pages[i]) pmm_free_page(fm->pages[i]);
        }
        if (fm->pages) kfree(fm->pages);
        fm->used = 0;
    }
}

uint32_t file_map_get_page(struct file_mapping *fm, uint32_t page_idx) {
    if (!fm || page_idx >= fm->num_pages) return 0;

    if (fm->pages[page_idx]) {
        return fm->pages[page_idx];
    }

    /* 缺页：分配物理页并从磁盘读取 */
    uint32_t phys = pmm_alloc_page();
    if (!phys) return 0;
    memset((void*)phys, 0, 4096);

    uint32_t offset = page_idx * 4096;
    uint32_t read_sz = 4096;
    if (offset + read_sz > fm->file_size) {
        read_sz = fm->file_size - offset;
    }

    if (read_sz > 0) {
        struct fat32_volume *fvol = (struct fat32_volume*)fm->vol->fs_private;
        struct fat32_file temp_f = {0};
        temp_f.first_cluster = fm->first_cluster;
        temp_f.file_size = fm->file_size;
        
        /* 直接调用底层 FAT32 读取 */
        fat32_read_file(fvol, &temp_f, (uint8_t*)phys, offset, read_sz);
    }

    fm->pages[page_idx] = phys;
    return phys;
}