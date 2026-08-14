#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_USER    0x004

/* 内核空间：低 1GB (0x00000000 - 0x3FFFFFFF) */
#define KERNEL_SPACE_START 0x00000000
#define KERNEL_SPACE_END   0x40000000   /* 1GB 边界 */

/* 用户空间：高 3GB (0x40000000 - 0xFFFFFFFF) */
#define USER_SPACE_START   0x40000000
#define USER_SPACE_END     0xFFFFFFFF

/* 内核页目录中，低 1GB 占用前 256 个页表项（0~255） */
#define KERNEL_PDE_COUNT 256

void vmm_init(void);

/* 创建用户进程页目录（复制内核的前 256 项） */
uint32_t *vmm_create_process_page_directory(void);

/* 在用户页目录中映射虚拟地址到物理地址 */
void vmm_map_user_page(uint32_t *pgd, uint32_t virt, uint32_t phys, uint32_t flags);

#endif