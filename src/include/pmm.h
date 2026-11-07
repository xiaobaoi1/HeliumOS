#ifndef PMM_H
#define PMM_H

#include <stdint.h>

// 物理内存管理器接口
void pmm_init(uint32_t multiboot_info_addr);

// 分配一个物理页（返回物理地址），失败返回 0
uint32_t pmm_alloc_page(void);

// 释放一个物理页
void pmm_free_page(uint32_t phys_addr);

// 获取空闲页数
uint32_t pmm_get_free_count(void);

/* 分配 n 页物理连续内存。返回首物理地址，失败返回 0。 */
uint32_t pmm_alloc_pages(uint32_t n);

/* 释放 n 页物理连续内存。 */
void     pmm_free_pages(uint32_t phys, uint32_t n);

#endif