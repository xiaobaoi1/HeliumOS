#ifndef KMAP_H
#define KMAP_H

#include <stdint.h>

/* 内核虚拟地址窗口 */
#define KMAP_BASE  0x08000000u
#define KMAP_END   0x10000000u
#define KMAP_SIZE  (KMAP_END - KMAP_BASE)   /* 64 MB */

/* 把 [phys, phys+len) 映射到窗口里，返回内核虚拟地址。
 * 失败返回 NULL。len 会向上对齐到 4KB。 */
void *kmap(uint32_t phys, uint32_t len);

/* 释放之前 kmap 的区域。 */
void kmap_free(void *virt, uint32_t len);

void kmap_init(void);

#endif