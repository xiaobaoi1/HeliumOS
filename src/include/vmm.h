#ifndef VMM_H
#define VMM_H

#include <stdint.h>

// 页大小
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

// 页表项标志位
#define PTE_PRESENT   0x001
#define PTE_WRITE     0x002
#define PTE_USER      0x004
#define PTE_GLOBAL    0x100

// 内核虚拟地址起始（3GB）
#define KERNEL_VIRTUAL_BASE 0xC0000000

// 初始化分页（在 kmain 中调用）
void vmm_init(void);

// 分配一个物理页并映射到指定虚拟地址
uint32_t vmm_alloc_page(uint32_t virt);

// 释放一个虚拟地址对应的物理页
void vmm_free_page(uint32_t virt);

// 映射物理地址到虚拟地址（手动）
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);

// 获取虚拟地址对应的物理地址
uint32_t vmm_get_phys(uint32_t virt);

#endif