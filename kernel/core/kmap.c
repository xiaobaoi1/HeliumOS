#include <kmap.h>
#include <vmm.h>
#include <pmm.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>

/* 位图跟踪窗口页是否已映射 */
#define KMAP_PAGES   (KMAP_SIZE / 4096)
#define KMAP_BITMAP_SIZE ((KMAP_PAGES + 7) / 8)

static uint8_t kmap_bitmap[KMAP_BITMAP_SIZE];

/* 内核页目录——start.asm 里定义 */
extern uint32_t page_directory[1024];

static inline void bm_set(uint32_t i)   { kmap_bitmap[i/8] |= (1 << (i%8)); }
static inline void bm_clear(uint32_t i) { kmap_bitmap[i/8] &= ~(1 << (i%8)); }
static inline int  bm_test(uint32_t i)  { return (kmap_bitmap[i/8] >> (i%8)) & 1; }

void kmap_init(void) {
    memset(kmap_bitmap, 0, sizeof(kmap_bitmap));
    kprintf("[KMAP] Window 0x%x..0x%x (%u MB)\n",
            KMAP_BASE, KMAP_END, KMAP_SIZE / (1024*1024));
}

void *kmap(uint32_t phys, uint32_t len) {
    if (phys == 0 || len == 0) return NULL;

    uint32_t offset = phys & 0xFFF;
    uint32_t need   = len + offset;                    /* 加上页内偏移 */
    uint32_t pages  = (need + 0xFFF) / 0x1000;
    if (pages > KMAP_PAGES) return NULL;

    /* 找连续空闲页 */
    uint32_t start = 0xFFFFFFFF;
    uint32_t run = 0;
    for (uint32_t i = 0; i < KMAP_PAGES; i++) {
        if (!bm_test(i)) {
            if (run == 0) start = i;
            if (++run == pages) break;
        } else {
            run = 0;
        }
    }
    if (run < pages) return NULL;

    /* 映射 */
    uint32_t virt = KMAP_BASE + start * 0x1000;
    uint32_t phys_page = phys & ~0xFFF;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t v = virt + i * 0x1000;
        uint32_t p = phys_page + i * 0x1000;

        uint32_t pde_idx = v >> 22;
        uint32_t pte_idx = (v >> 12) & 0x3FF;

        uint32_t *pde = &page_directory[pde_idx];
        uint32_t *ptable;

        if (*pde & PTE_PRESENT) {
            ptable = (uint32_t*)(*pde & 0xFFFFF000);
        } else {
            uint32_t pt_phys = pmm_alloc_page();
            if (!pt_phys) return NULL;
            ptable = (uint32_t*)pt_phys;
            for (int k = 0; k < 1024; k++) ptable[k] = 0;
            *pde = pt_phys | PTE_PRESENT | PTE_WRITE;
        }

        ptable[pte_idx] = (p & 0xFFFFF000) | PTE_PRESENT | PTE_WRITE;
        __asm__ volatile("invlpg (%0)" :: "r"(v));
        bm_set(start + i);
    }

    return (void*)(virt + offset);   /* 加上页内偏移 */
}

void kmap_free(void *virt, uint32_t len) {
    if (!virt || len == 0) return;
    uint32_t v = (uint32_t)virt;
    if (v < KMAP_BASE || v >= KMAP_END) return;

    uint32_t offset = v & 0xFFF;
    uint32_t pages = (len + offset + 0xFFF) / 0x1000;
    uint32_t start = ((v - KMAP_BASE) & ~0xFFF) / 0x1000;

    for (uint32_t i = 0; i < pages && start + i < KMAP_PAGES; i++) {
        uint32_t vv = KMAP_BASE + (start + i) * 0x1000;
        uint32_t pde_idx = vv >> 22;
        uint32_t pte_idx = (vv >> 12) & 0x3FF;
        uint32_t pde = page_directory[pde_idx];
        if (pde & PTE_PRESENT) {
            uint32_t *ptable = (uint32_t*)(pde & 0xFFFFF000);
            ptable[pte_idx] = 0;
            __asm__ volatile("invlpg (%0)" :: "r"(vv));
        }
        bm_clear(start + i);
    }
}