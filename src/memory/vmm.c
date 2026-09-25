#include <vmm.h>
#include <pmm.h>
#include <printf.h>
#include <stddef.h>

extern uint32_t page_directory[1024];

void vmm_init(void) {
    kprintf("[VMM] Paging enabled (identity mapping for kernel).\n");

    uint32_t pd_phys = (uint32_t)page_directory;

    /* 设置递归映射（页目录第 1023 项指向自身） */
    page_directory[1023] = pd_phys | PTE_PRESENT | PTE_WRITE;

    /* 刷新 TLB */
    __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" :: "a"(pd_phys));

    kprintf("[VMM] Kernel uses low 1GB space (0x00000000 - 0x3FFFFFFF).\n");
    kprintf("[VMM] User space starts at 0x40000000.\n");
}

/* 创建用户进程页目录 */
uint32_t *vmm_create_process_page_directory(void) {
    /* 1. 分配一个物理页作为新页目录 */
    uint32_t *new_pgd = (uint32_t*)pmm_alloc_page();
    if (!new_pgd) {
        kprintf("[VMM] ERROR: Failed to allocate page directory for process.\n");
        return NULL;
    }

    /* 2. 复制内核的前 256 项（低 1GB 映射） */
    uint32_t *kernel_pgd = (uint32_t*)page_directory;
    for (int i = 0; i < KERNEL_PDE_COUNT; i++) {
        new_pgd[i] = kernel_pgd[i];
    }

    /* 3. 清空用户空间部分（256~1023 项） */
    for (int i = KERNEL_PDE_COUNT; i < 1024; i++) {
        new_pgd[i] = 0;
    }

    kprintf("[VMM] New process page directory created at physical %p.\n", (uint32_t)new_pgd);
    return new_pgd;
}

/* 映射用户空间虚拟地址到物理地址（仅用于高 3GB 区域） */
void vmm_map_user_page(uint32_t *pgd, uint32_t virt, uint32_t phys, uint32_t flags) {
    if (virt < USER_SPACE_START) {
        kprintf("[VMM] WARNING: Trying to map kernel space in user page directory.\n");
        return;
    }

    uint32_t pde_idx = virt >> 22;          /* 高 10 位 */
    uint32_t pte_idx = (virt >> 12) & 0x3FF; /* 中间 10 位 */

    uint32_t *pde = &pgd[pde_idx];
    uint32_t *ptable = NULL;

    /* 检查页表是否存在 */
    if (*pde & PTE_PRESENT) {
        /* 页表已存在，获取其物理地址 */
        uint32_t ptable_phys = *pde & 0xFFFFF000;
        ptable = (uint32_t*)ptable_phys;   /* 因为内核是平坦映射，物理地址可直接访问 */
    } else {
        /* 分配一个新的页表 */
        uint32_t ptable_phys = pmm_alloc_page();
        if (!ptable_phys) {
            kprintf("[VMM] ERROR: Failed to allocate page table.\n");
            return;
        }
        /* 清零页表（物理地址直接访问） */
        uint32_t *ptable_virt = (uint32_t*)ptable_phys;
        for (int i = 0; i < 1024; i++) {
            ptable_virt[i] = 0;
        }
        /* 设置页目录项 */
        *pde = ptable_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
        ptable = ptable_virt;
    }

    /* 设置页表项 */
    ptable[pte_idx] = (phys & 0xFFFFF000) | (flags & 0xFFF) | PTE_PRESENT;
    kprintf("[VMM] Map: virt 0x%x -> phys 0x%x, PDE=0x%x, PTE=0x%x\n",
        virt, phys, *pde, ptable[pte_idx]);

    /* 刷新 TLB（可选） */
    __asm__ volatile("invlpg (%0)" :: "r"(virt));
}

// 在 vmm.c 中添加：
void vmm_unmap_user_page(uint32_t *pgd, uint32_t virt) {
    if (virt < USER_SPACE_START) return;
    uint32_t pde_idx = virt >> 22;
    uint32_t pte_idx = (virt >> 12) & 0x3FF;
    uint32_t pde = pgd[pde_idx];
    if (!(pde & PTE_PRESENT)) return;
    uint32_t *ptable = (uint32_t*)(pde & 0xFFFFF000);
    if (!(ptable[pte_idx] & PTE_PRESENT)) return;
    ptable[pte_idx] = 0;  // 清除页表项
    __asm__ volatile("invlpg (%0)" :: "r"(virt)); // 刷新 TLB
}

uint32_t vmm_get_phys(uint32_t *pgd, uint32_t virt) {
    uint32_t pde_idx = virt >> 22;
    uint32_t pte_idx = (virt >> 12) & 0x3FF;

    uint32_t pde = pgd[pde_idx];
    if (!(pde & PTE_PRESENT)) {
        return 0;
    }

    uint32_t *ptable = (uint32_t*)(pde & 0xFFFFF000);  // 物理地址直接访问（平坦映射）
    uint32_t pte = ptable[pte_idx];
    if (!(pte & PTE_PRESENT)) {
        return 0;
    }

    return (pte & 0xFFFFF000) + (virt & 0xFFF);
}


void vmm_free_process_address_space(uint32_t *pgd) {
    if (!pgd) return;
    for (int i = KERNEL_PDE_COUNT; i < 1024; i++) {
        if (pgd[i] & PTE_PRESENT) {
            uint32_t pt_phys = pgd[i] & 0xFFFFF000;
            uint32_t *pt = (uint32_t*)pt_phys;
            for (int j = 0; j < 1024; j++) {
                if (pt[j] & PTE_PRESENT) {
                    pmm_free_page(pt[j] & 0xFFFFF000);
                }
            }
            pmm_free_page(pt_phys);
        }
    }
    pmm_free_page((uint32_t)pgd);
}