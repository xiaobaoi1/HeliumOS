#include <vma.h>
#include <task.h>
#include <heap.h>
#include <errno.h>
#include <printf.h>
#include <stddef.h>

static uint32_t page_align_down(uint32_t addr) { return addr & ~0xFFFu; }
static uint32_t page_align_up(uint32_t addr)   { return (addr + 0xFFF) & ~0xFFFu; }

void vma_init(void) {
    /* 无全局状态 */
}

struct vma *vma_find(struct task *t, uint32_t addr) {
    if (!t) return NULL;
    for (struct vma *v = t->vma_list; v; v = v->next) {
        if (addr >= v->start && addr < v->end) return v;
        if (addr < v->start) break;   /* 有序，提前退出 */
    }
    return NULL;
}

int vma_insert(struct task *t, uint32_t start, uint32_t end,
               uint32_t flags, uint8_t type) {
    if (!t) return EINVAL;

    start = page_align_down(start);
    end   = page_align_up(end);
    if (start >= end) return EINVAL;

    /* 查重叠。链表按 start 升序，遇到 start >= end 即可停。 */
    for (struct vma *v = t->vma_list; v; v = v->next) {
        if (v->start >= end) break;
        if (start < v->end && end > v->start) return EEXIST;
    }

    struct vma *node = (struct vma*)kmalloc(sizeof(struct vma));
    if (!node) return ENOMEM;

    node->start = start;
    node->end   = end;
    node->flags = flags;
    node->type  = type;
    node->next  = NULL;
    node->reserved[0] = node->reserved[1] = node->reserved[2] = 0;

    /* 按 start 升序插入 */
    if (!t->vma_list || start < t->vma_list->start) {
        node->next = t->vma_list;
        t->vma_list = node;
        return OK;
    }
    struct vma *prev = t->vma_list;
    while (prev->next && prev->next->start < start) {
        prev = prev->next;
    }
    node->next = prev->next;
    prev->next = node;
    return OK;
}

void vma_remove_range(struct task *t, uint32_t start, uint32_t end) {
    if (!t || start >= end) return;

    struct vma **pp = &t->vma_list;
    while (*pp) {
        struct vma *v = *pp;
        if (v->end <= start || v->start >= end) {
            pp = &v->next;
            continue;
        }

        if (v->start < start && v->end > end) {
            /* VMA 包含被删区间：拆成左右两段 */
            struct vma *right = (struct vma*)kmalloc(sizeof(struct vma));
            if (!right) {
                /* 内存不足，保守处理：不拆，保留整段 */
                KLOG_WARN("VMA: kmalloc failed, skipping split\n");
                pp = &v->next;
                continue;
            }
            right->start = end;
            right->end   = v->end;
            right->flags = v->flags;
            right->type  = v->type;
            right->reserved[0] = right->reserved[1] = right->reserved[2] = 0;
            right->next  = v->next;
            v->end = start;
            v->next = right;
            pp = &right->next;
        } else if (v->start < start) {
            /* 尾部被切 */
            v->end = start;
            pp = &v->next;
        } else if (v->end > end) {
            /* 头部被切 */
            v->start = end;
            pp = &v->next;
        } else {
            /* 完全包含：删除节点 */
            *pp = v->next;
            kfree(v);
        }
    }
}

void vma_free_all(struct task *t) {
    if (!t) return;
    struct vma *v = t->vma_list;
    while (v) {
        struct vma *next = v->next;
        kfree(v);
        v = next;
    }
    t->vma_list = NULL;
}