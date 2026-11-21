#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

/* 初始化内核堆（在 pmm_init 之后调用） */
void  heap_init(void);

/* 分配内存
 * size == 0 或 size > 2048：返回 NULL
 * 成功返回非 NULL 指针
 */
void *kmalloc(size_t size);

/* 分配并清零 */
void *kzalloc(size_t size);

/* 释放内存，ptr == NULL 时无操作 */
void  kfree(void *ptr);

/* 打印统计信息（调试用） */
void  heap_stats(void);

#endif