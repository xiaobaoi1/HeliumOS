#include <pmm.h>
#include <multiboot2.h>
#include <printf.h>
#include <stddef.h>

// 当前 pmm 只能支持 128MB 的内存

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define MAX_PHYS_MEM (1024 * 1024 * 1024)  /* 1GB */
#define MAX_PAGES (MAX_PHYS_MEM / PAGE_SIZE)
#define BITMAP_SIZE (MAX_PAGES / 8 + 1)

static uint8_t bitmap[BITMAP_SIZE];
static uint32_t free_page_count = 0;

static inline void bitmap_set(uint32_t idx) { bitmap[idx/8] |= (1 << (idx%8)); }
static inline void bitmap_clear(uint32_t idx) { bitmap[idx/8] &= ~(1 << (idx%8)); }
static inline uint8_t bitmap_test(uint32_t idx) { return (bitmap[idx/8] >> (idx%8)) & 1; }

void pmm_init(uint32_t multiboot_info_addr) {
    kprintf("[PMM] Initializing...\n");

    for (uint32_t i = 0; i < MAX_PAGES; i++) bitmap_set(i);
    free_page_count = 0;

    struct multiboot2_info *info = (struct multiboot2_info*)multiboot_info_addr;
    uint8_t *ptr = (uint8_t*)multiboot_info_addr + sizeof(struct multiboot2_info);
    uint8_t *end = (uint8_t*)multiboot_info_addr + info->total_size;

    while (ptr < end) {
        struct multiboot2_tag *tag = (struct multiboot2_tag*)ptr;
        if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
            struct multiboot2_tag_mmap *mmap_tag = (struct multiboot2_tag_mmap*)tag;
            uint32_t entry_size = mmap_tag->entry_size;
            uint8_t *entry_ptr = (uint8_t*)mmap_tag + sizeof(struct multiboot2_tag_mmap);
            uint8_t *entry_end = (uint8_t*)mmap_tag + mmap_tag->size;

            while (entry_ptr < entry_end) {
                struct multiboot2_mmap_entry *entry = (struct multiboot2_mmap_entry*)entry_ptr;
                if (entry->type == 1) {
                    uint64_t base = entry->addr;
                    uint64_t len = entry->len;
                    uint64_t start = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    uint64_t end_phys = (base + len) & ~(PAGE_SIZE - 1);
                    for (uint64_t addr = start; addr < end_phys; addr += PAGE_SIZE) {
                        if (addr < MAX_PHYS_MEM) {
                            uint32_t idx = addr >> PAGE_SHIFT;
                            if (bitmap_test(idx) == 1) {
                                bitmap_clear(idx);
                                free_page_count++;
                            }
                        }
                    }
                }
                entry_ptr += entry_size;
            }
            break;
        }
        ptr += tag->size;
        while ((uintptr_t)ptr % 8 != 0) ptr++;
    }

    /* 强制保留低端 1MB（保护 BIOS 数据） */
    for (uint32_t addr = 0; addr < 0x100000; addr += PAGE_SIZE) {
        uint32_t idx = addr >> PAGE_SHIFT;
        if (bitmap_test(idx) == 0) {
            bitmap_set(idx);
            free_page_count--;
        }
    }

    /* 标记内核自身占用的物理页（虚拟地址=物理地址，直接使用） */
    extern uint32_t _kernel_start, _kernel_end;
    uint32_t kernel_start = (uint32_t)&_kernel_start;
    uint32_t kernel_end   = (uint32_t)&_kernel_end;
    uint32_t start_page = kernel_start >> PAGE_SHIFT;
    uint32_t end_page   = (kernel_end + PAGE_SIZE - 1) >> PAGE_SHIFT;
    for (uint32_t i = start_page; i < end_page; i++) {
        if (bitmap_test(i) == 0) {
            bitmap_set(i);
            free_page_count--;
        }
    }

    /* 标记位图自身占用的页 */
    uint32_t bitmap_start = (uint32_t)bitmap;
    uint32_t bitmap_end = bitmap_start + BITMAP_SIZE;
    for (uint32_t addr = bitmap_start; addr < bitmap_end; addr += PAGE_SIZE) {
        uint32_t idx = addr >> PAGE_SHIFT;
        if (bitmap_test(idx) == 0) {
            bitmap_set(idx);
            free_page_count--;
        }
    }
    
    /* 保留 0x08000000..0x10000000（128MB）：
     * 这段物理地址无法通过恒等映射访问——虚拟同地址是 kmap 窗口。 */
    for (uint32_t addr = 0x08000000; addr < 0x10000000; addr += PAGE_SIZE) {
        uint32_t idx = addr >> PAGE_SHIFT;
        if (bitmap_test(idx) == 0) {
            bitmap_set(idx);
            free_page_count--;
        }
    }

    kprintf("[PMM] Done. Free pages: %d\n", free_page_count);
}

uint32_t pmm_alloc_page(void) {
    for (uint32_t i = 0; i < MAX_PAGES; i++) {
        if (bitmap_test(i) == 0) {
            bitmap_set(i);
            free_page_count--;
            return i << PAGE_SHIFT;
        }
    }
    kprintf("[PMM] ERROR: Out of physical memory!\n");
    return 0;
}

void pmm_free_page(uint32_t phys_addr) {
    uint32_t idx = phys_addr >> PAGE_SHIFT;
    if (idx < MAX_PAGES && bitmap_test(idx) == 1) {
        bitmap_clear(idx);
        free_page_count++;
    }
}

uint32_t pmm_get_free_count(void) {
    return free_page_count;
}

uint32_t pmm_alloc_pages(uint32_t n) {
    if (n == 0 || n > MAX_PAGES) return 0;
    if (n == 1) return pmm_alloc_page();

    /* 线性扫描找连续 n 个空页 */
    for (uint32_t i = 0; i + n <= MAX_PAGES; i++) {
        if (bitmap_test(i)) continue;   /* 起点已占用，跳过 */

        int ok = 1;
        for (uint32_t j = 1; j < n; j++) {
            if (bitmap_test(i + j)) { ok = 0; break; }
        }
        if (!ok) continue;

        for (uint32_t j = 0; j < n; j++) {
            bitmap_set(i + j);
        }
        free_page_count -= n;
        return i << PAGE_SHIFT;
    }
    kprintf("[PMM] ERROR: no contiguous %u pages\n", n);
    return 0;
}

void pmm_free_pages(uint32_t phys, uint32_t n) {
    if (n == 0) return;
    uint32_t idx = phys >> PAGE_SHIFT;
    for (uint32_t i = 0; i < n; i++) {
        if (idx + i < MAX_PAGES && bitmap_test(idx + i)) {
            bitmap_clear(idx + i);
            free_page_count++;
        }
    }
}