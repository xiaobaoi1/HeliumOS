#include <vmm.h>
#include <memory.h>
#include <printf.h>
#include <string.h>   // 需要 memset 函数

// 页目录和页表的物理地址（由 PMM 分配）
static uint32_t *page_directory = NULL;
static uint32_t *page_table_kernel = NULL;

// 临时身份映射页表（用于开启分页瞬间）
static uint32_t *page_table_temp = NULL;

// ---------- 辅助函数 ----------
// 设置页目录项
static inline void set_pde(uint32_t *pde, uint32_t phys, uint32_t flags) {
    *pde = (phys & 0xFFFFF000) | (flags & 0xFFF) | PTE_PRESENT;
}

// 设置页表项
static inline void set_pte(uint32_t *pte, uint32_t phys, uint32_t flags) {
    *pte = (phys & 0xFFFFF000) | (flags & 0xFFF) | PTE_PRESENT;
}

// ---------- 初始化函数 ----------
void vmm_init(void) {
    kprintf("[VMM] Initializing paging...\n");

    // 1. 分配物理页作为页目录
    uint32_t pd_phys = pmm_alloc_page();
    if (!pd_phys) {
        kprintf("[VMM] ERROR: Failed to allocate page directory!\n");
        return;
    }
    page_directory = (uint32_t*)pd_phys;  // 注意：此时尚未开启分页，物理地址可直接访问
    kprintf("[VMM] Page directory at physical %p\n", pd_phys);

    // 2. 清零页目录（所有页表不存在）
    memset(page_directory, 0, PAGE_SIZE);

    // 3. 分配物理页作为内核页表（映射 0xC0000000 以上的 4MB 空间）
    uint32_t pt_phys = pmm_alloc_page();
    if (!pt_phys) {
        kprintf("[VMM] ERROR: Failed to allocate kernel page table!\n");
        return;
    }
    page_table_kernel = (uint32_t*)pt_phys;
    kprintf("[VMM] Kernel page table at physical %p\n", pt_phys);
    memset(page_table_kernel, 0, PAGE_SIZE);

    // 4. 建立内核页表映射：虚拟地址 0xC0000000 ~ 0xC03FFFFF 映射到物理 0x00000000 ~ 0x003FFFFF
    //    注意：内核自身在物理 0x100000，但为了简化，我们暂时映射整个前 4MB
    for (int i = 0; i < 1024; i++) {
        uint32_t phys = i * PAGE_SIZE;
        uint32_t flags = PTE_WRITE;  // 内核可读可写，不设 PTE_USER（仅 Ring 0）
        set_pte(&page_table_kernel[i], phys, flags);
    }

    // 5. 将内核页表填入页目录（第 768 项对应 0xC0000000 >> 22）
    //    768 = 0xC0000000 / 4MB
    set_pde(&page_directory[768], pt_phys, PTE_WRITE);

    // 6. 建立临时身份映射（用于开启分页瞬间的过渡）
    //    我们分配另一个物理页作为临时页表，映射低 4MB
    uint32_t temp_phys = pmm_alloc_page();
    if (!temp_phys) {
        kprintf("[VMM] ERROR: Failed to allocate temp page table!\n");
        return;
    }
    page_table_temp = (uint32_t*)temp_phys;
    kprintf("[VMM] Temp page table at physical %p\n", temp_phys);
    memset(page_table_temp, 0, PAGE_SIZE);

    // 映射低 4MB（身份映射）
    for (int i = 0; i < 1024; i++) {
        uint32_t phys = i * PAGE_SIZE;
        uint32_t flags = PTE_WRITE;
        set_pte(&page_table_temp[i], phys, flags);
    }

    // 将临时页表填入页目录第 0 项（对应虚拟地址 0x00000000 ~ 0x003FFFFF）
    set_pde(&page_directory[0], temp_phys, PTE_WRITE);

    // 7. 【重要】页目录自我映射（递归映射），方便后续修改页表
    //    将页目录自身映射到虚拟地址 0xFFFFF000（最后 4KB）
    set_pde(&page_directory[1023], pd_phys, PTE_WRITE);

    kprintf("[VMM] Page tables initialized.\n");
}

// ---------- 后续 API 实现（第二阶段再补全） ----------
uint32_t vmm_alloc_page(uint32_t virt) {
    // 将在开启分页后实现
    return 0;
}

void vmm_free_page(uint32_t virt) {
    // 将在开启分页后实现
}

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    // 将在开启分页后实现
}

uint32_t vmm_get_phys(uint32_t virt) {
    // 将在开启分页后实现
    return 0;
}