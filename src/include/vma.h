#ifndef VMA_H
#define VMA_H

#include <stdint.h>

/* VMA 类型 */
#define VMA_TYPE_HEAP    1
#define VMA_TYPE_STACK   2
#define VMA_TYPE_MMAP    3
#define VMA_TYPE_ELF     4
#define VMA_TYPE_PIPE    5

/* 权限位（语义上对齐 PTE_*，但不复用——它们是 VMA 的抽象） */
#define VMA_READ   0x01
#define VMA_WRITE  0x02
#define VMA_EXEC   0x04
#define VMA_USER   0x08

struct task;

/* 一个虚拟地址区间 [start, end)。页对齐，半开。 */
struct vma {
    uint32_t start;
    uint32_t end;
    uint32_t flags;
    uint8_t  type;
    uint8_t  reserved[3];
    struct vma *next;
};

/* 空实现——为将来可能的全局状态预留。 */
void vma_init(void);

/* 查 addr 落在哪个 VMA。未命中返回 NULL。 */
struct vma *vma_find(struct task *t, uint32_t addr);

/* 插入 VMA。start/end 自动页对齐。区间与现有 VMA 重叠时返回 EEXIST。 */
int vma_insert(struct task *t, uint32_t start, uint32_t end,
               uint32_t flags, uint8_t type);

/* 移除 [start, end)。部分重叠时拆分。
 * kmalloc 失败时保守保留——不丢数据。 */
void vma_remove_range(struct task *t, uint32_t start, uint32_t end);

/* 释放该进程的所有 VMA 节点。不改页表。 */
void vma_free_all(struct task *t);

#endif