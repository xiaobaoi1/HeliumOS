#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

// 初始化物理内存管理（传入 GRUB 传入的 info 结构地址）
void pmm_init(uint32_t multiboot_info_addr);

// 分配一个物理页（返回物理地址），失败返回 0
uint32_t pmm_alloc_page(void);

// 释放一个物理页
void pmm_free_page(uint32_t phys_addr);

// 获取总空闲页数（调试用）
uint32_t pmm_get_free_count(void);

#endif