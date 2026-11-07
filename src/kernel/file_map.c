#include <file_map.h>
#include <pmm.h>
#include <heap.h>
#include <fat32.h>
#include <printf.h>
#include <string.h>
#include <errno.h>

static struct file_mapping g_file_maps[FILE_MAP_MAX];
static uint32_t            g_total_cached_pages = 0;
static struct file_mapping *g_lru_head = NULL;
static struct file_mapping *g_lru_tail = NULL;

/* P1：readahead 临时缓冲。单核安全，32KB BSS。 */
static uint8_t readahead_buf[FILE_MAP_READAHEAD * 4096];

/* ---------- LRU ---------- */

static void lru_unlink(struct file_mapping *fm) {
    if (fm->lru_prev) fm->lru_prev->lru_next = fm->lru_next;
    else if (g_lru_head == fm) g_lru_head = fm->lru_next;

    if (fm->lru_next) fm->lru_next->lru_prev = fm->lru_prev;
    else if (g_lru_tail == fm) g_lru_tail = fm->lru_prev;

    fm->lru_prev = NULL;
    fm->lru_next = NULL;
}

static void lru_push_front(struct file_mapping *fm) {
    fm->lru_prev = NULL;
    fm->lru_next = g_lru_head;
    if (g_lru_head) g_lru_head->lru_prev = fm;
    else            g_lru_tail = fm;
    g_lru_head = fm;
}

static void lru_touch(struct file_mapping *fm) {
    if (!fm || !fm->used) return;
    if (g_lru_head == fm) return;
    lru_unlink(fm);
    lru_push_front(fm);
}

/* ---------- 销毁 ---------- */

static void fm_free_pages(struct file_mapping *fm) {
    if (!fm->pages) return;
    for (uint32_t i = 0; i < fm->num_pages; i++) {
        if (fm->pages[i]) {
            pmm_free_page(fm->pages[i]);
            fm->pages[i] = 0;
        }
    }
    if (fm->cached_pages > 0) {
        if (g_total_cached_pages >= fm->cached_pages)
            g_total_cached_pages -= fm->cached_pages;
        else
            g_total_cached_pages = 0;
        fm->cached_pages = 0;
    }
    kfree(fm->pages);
    fm->pages = NULL;
}

static void fm_destroy(struct file_mapping *fm) {
    if (!fm || !fm->used) return;
    fm_free_pages(fm);
    lru_unlink(fm);
    fm->used = 0;
    fm->invalid = 0;
    fm->num_pages = 0;
    fm->vol = NULL;
    fm->first_cluster = 0;
    fm->file_size = 0;
    fm->refcount = 0;
    fm->last_page_idx = 0xFFFFFFFFu;
}

/* ---------- 读盘（单页，使用 chain_cache） ---------- */

static int fm_read_page_from_disk(struct file_mapping *fm, uint32_t page_idx,
                                  uint32_t phys) {
    memset((void*)phys, 0, 4096);

    uint32_t offset = page_idx * 4096u;
    uint32_t read_sz = 4096;
    if (offset + read_sz > fm->file_size) {
        read_sz = fm->file_size - offset;
    }
    if (read_sz == 0) return 0;

    struct fat32_volume *fvol = (struct fat32_volume*)fm->vol->fs_private;
    if (!fvol) return -1;

    /* P0：复用 chain_cache。fat32_read_file 内部会推进 cached_cluster，
     * 下次读相邻页只前进几簇，不是从头遍历。 */
    int r = fat32_read_file(fvol, &fm->chain_cache,
                            (uint8_t*)phys, offset, read_sz);
    return r == (int)read_sz ? 0 : -1;
}

/* ---------- 生命周期 ---------- */

void file_map_init(void) {
    memset(g_file_maps, 0, sizeof(g_file_maps));
    for (int i = 0; i < FILE_MAP_MAX; i++) {
        g_file_maps[i].last_page_idx = 0xFFFFFFFFu;
    }
    g_total_cached_pages = 0;
    g_lru_head = NULL;
    g_lru_tail = NULL;
}

struct file_mapping *file_map_get(struct volume *vol,
                                  uint32_t first_cluster, uint32_t size) {
    if (!vol || first_cluster < 2 || size == 0) return NULL;

    for (int i = 0; i < FILE_MAP_MAX; i++) {
        struct file_mapping *fm = &g_file_maps[i];
        if (fm->used && !fm->invalid &&
            fm->vol == vol &&
            fm->first_cluster == first_cluster &&
            fm->file_size == size) {
            lru_touch(fm);
            return fm;
        }
    }

    struct file_mapping *fm = NULL;
    for (int i = 0; i < FILE_MAP_MAX; i++) {
        if (!g_file_maps[i].used) { fm = &g_file_maps[i]; break; }
    }
    if (!fm) {
        for (struct file_mapping *p = g_lru_tail; p; p = p->lru_prev) {
            if (p->refcount == 0) { fm_destroy(p); fm = p; break; }
        }
    }
    if (!fm) return NULL;

    memset(fm, 0, sizeof(*fm));
    fm->used          = 1;
    fm->refcount      = 0;
    fm->invalid       = 0;
    fm->vol           = vol;
    fm->first_cluster = first_cluster;
    fm->file_size     = size;
    fm->num_pages     = (size + 4095) / 4096;
    fm->cached_pages  = 0;
    fm->last_page_idx = 0xFFFFFFFFu;

    /* P0：初始化 chain_cache。cached_cluster=0 由 fat32_read_file 内部填充。 */
    memset(&fm->chain_cache, 0, sizeof(fm->chain_cache));
    fm->chain_cache.first_cluster = first_cluster;
    fm->chain_cache.file_size     = size;

    if (fm->num_pages > 0) {
        fm->pages = (uint32_t*)kmalloc(fm->num_pages * sizeof(uint32_t));
        if (!fm->pages) { fm->used = 0; return NULL; }
        memset(fm->pages, 0, fm->num_pages * sizeof(uint32_t));
    }
    lru_push_front(fm);
    return fm;
}

void file_map_ref(struct file_mapping *fm) {
    if (fm) fm->refcount++;
}

void file_map_unref(struct file_mapping *fm) {
    if (!fm || fm->refcount <= 0) return;
    fm->refcount--;
    if (fm->refcount == 0) {
        if (fm->invalid) fm_destroy(fm);
        /* 不 invalid 的情况保持不动：LRU 位置由访问时间决定，
         * 不因 refcount 归零而 touch。让 shrink 按真正的最近使用淘汰。 */
    }
}

/* ---------- 失效 ---------- */

void file_map_invalidate(struct volume *vol, uint32_t first_cluster) {
    if (!vol || first_cluster < 2) return;
    for (int i = 0; i < FILE_MAP_MAX; i++) {
        struct file_mapping *fm = &g_file_maps[i];
        if (!fm->used || fm->invalid) continue;
        if (fm->vol != vol) continue;
        if (fm->first_cluster != first_cluster) continue;
        fm->invalid = 1;
        if (fm->refcount == 0) fm_destroy(fm);
    }
}

/* ---------- 淘汰 ---------- */

void file_map_shrink(void) {
    if (g_total_cached_pages <= FILE_MAP_TOTAL_PAGES_MAX) return;

    struct file_mapping *p = g_lru_tail;
    while (p && g_total_cached_pages > FILE_MAP_TOTAL_PAGES_MAX) {
        struct file_mapping *prev = p->lru_prev;
        if (p->refcount == 0) {
            fm_destroy(p);
        }
        p = prev;
    }
}

/* ---------- mmap 路径（持久映射，无 per-file 上限） ---------- */

uint32_t file_map_get_page(struct file_mapping *fm, uint32_t page_idx) {
    if (!fm || page_idx >= fm->num_pages) return 0;

    if (fm->pages[page_idx]) {
        lru_touch(fm);
        return fm->pages[page_idx];
    }
    if (fm->invalid) return 0;

    if (g_total_cached_pages + 1 > FILE_MAP_TOTAL_PAGES_MAX) {
        file_map_shrink();
    }

    uint32_t phys = pmm_alloc_page();
    if (!phys) return 0;

    if (fm_read_page_from_disk(fm, page_idx, phys) < 0) {
        pmm_free_page(phys);
        return 0;
    }

    fm->pages[page_idx] = phys;
    fm->cached_pages++;
    g_total_cached_pages++;
    lru_touch(fm);
    return phys;
}

/* ---------- read 路径（临时缓存 + readahead） ---------- */

int file_map_read_page(struct file_mapping *fm, uint32_t page_idx, uint8_t *buf) {
    if (!fm || !buf || page_idx >= fm->num_pages) return -1;

    lru_touch(fm);

    /* 命中 */
    if (fm->pages[page_idx]) {
        memcpy(buf, (void*)fm->pages[page_idx], 4096);
        fm->last_page_idx = page_idx;
        return 0;
    }
    if (fm->invalid) return -1;

    /* P1：顺序检测。page 0 且从未读过 → 也算顺序（首次从头读）。 */
    int sequential = (page_idx == 0 && fm->last_page_idx == 0xFFFFFFFFu) ||
                     (fm->last_page_idx != 0xFFFFFFFFu &&
                      page_idx == fm->last_page_idx + 1);

    if (sequential && fm->num_pages > page_idx + 1) {
        uint32_t ra = FILE_MAP_READAHEAD;
        if (page_idx + ra > fm->num_pages)
            ra = fm->num_pages - page_idx;

        /* 按 per-file 容量截断 */
        uint32_t cap_left = (fm->cached_pages < FILE_MAP_PER_FILE_PAGES_MAX)
                          ? (FILE_MAP_PER_FILE_PAGES_MAX - fm->cached_pages)
                          : 0;
        if (ra > cap_left) ra = cap_left;

        if (ra > 1) {
            if (g_total_cached_pages + ra > FILE_MAP_TOTAL_PAGES_MAX) {
                file_map_shrink();
            }

            uint32_t start_off = page_idx * 4096u;
            uint32_t total = ra * 4096u;
            if (start_off + total > fm->file_size)
                total = fm->file_size - start_off;

            struct fat32_volume *fvol =
                (struct fat32_volume*)fm->vol->fs_private;
            if (fvol) {
                int r = fat32_read_file(fvol, &fm->chain_cache,
                                        readahead_buf, start_off, total);
                if (r == (int)total) {
                    /* 逐页安装。有的页可能已被 mmap 填了，跳过。 */
                    for (uint32_t i = 0; i < ra; i++) {
                        if (fm->pages[page_idx + i]) continue;
                        uint32_t phys = pmm_alloc_page();
                        if (!phys) break;
                        memcpy((void*)phys, readahead_buf + i * 4096u, 4096);
                        fm->pages[page_idx + i] = phys;
                        fm->cached_pages++;
                        g_total_cached_pages++;
                    }
                    if (fm->pages[page_idx]) {
                        memcpy(buf, (void*)fm->pages[page_idx], 4096);
                        fm->last_page_idx = page_idx;
                        return 0;
                    }
                }
            }
        }
    }

    /* 单页路径（非顺序 / readahead 失败 / 容量不足） */
    int can_cache = (fm->cached_pages < FILE_MAP_PER_FILE_PAGES_MAX);

    if (can_cache && g_total_cached_pages + 1 > FILE_MAP_TOTAL_PAGES_MAX) {
        file_map_shrink();
    }

    uint32_t phys = pmm_alloc_page();
    if (!phys) return -1;

    if (fm_read_page_from_disk(fm, page_idx, phys) < 0) {
        pmm_free_page(phys);
        return -1;
    }

    memcpy(buf, (void*)phys, 4096);

    if (can_cache) {
        fm->pages[page_idx] = phys;
        fm->cached_pages++;
        g_total_cached_pages++;
    } else {
        pmm_free_page(phys);
    }
    fm->last_page_idx = page_idx;
    return 0;
}

/* ---------- 诊断 ---------- */

void file_map_stats(void) {
    int used = 0, invalid = 0, lru_len = 0;
    for (int i = 0; i < FILE_MAP_MAX; i++) {
        if (g_file_maps[i].used) used++;
        if (g_file_maps[i].used && g_file_maps[i].invalid) invalid++;
    }
    for (struct file_mapping *p = g_lru_head; p; p = p->lru_next) lru_len++;
    kprintf("[FMAP] used=%d invalid=%d lru=%d pages=%u/%u\n",
            used, invalid, lru_len, g_total_cached_pages,
            FILE_MAP_TOTAL_PAGES_MAX);
}