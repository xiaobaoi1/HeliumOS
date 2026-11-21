#include "stdlib.h"
#include "syscall.h"
#include "string.h"
#include <stdio.h>

#define ALIGN_UP(x, a)  (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_8(x)      ALIGN_UP((x), 8)

struct block {
    size_t        size;
    int           free;
    int           is_mmap;    /* 1 = 来自 mmap */
    struct block *next;
};

#define HEADER_SIZE  ((size_t)sizeof(struct block))
#define MIN_SPLIT    16

/* 大分配走 mmap 的阈值 */
#define MMAP_THRESHOLD  (64 * 1024)

static struct block *head = NULL;
static size_t g_grow_size = 0;

/* 扩展物理内存：brk 对齐到 4KB */
static void *heap_grow(size_t need) {
    int cur = sys_brk(0);
    if (cur < 0) return NULL;

    int new_brk = cur + (int)need;
    new_brk = (new_brk + 4095) & ~4095;

    if (sys_brk(new_brk) != 0) return NULL;

    g_grow_size = (size_t)(new_brk - cur);
    return (void*)cur;
}

void *malloc(size_t size) {
    if (size == 0) size = 1;   /* 避免零大小分配被部分调用方当成 OOM */
    size = ALIGN_8(size);

    /* 大分配：优先 mmap；失败则回退到 brk */
    if (size >= MMAP_THRESHOLD) {
        size_t total = size + HEADER_SIZE;
        size_t map_size = (total + 4095) & ~4095u;
        unsigned int addr = sys_mmap(0, map_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((int)addr > 0) {
            struct block *b = (struct block*)addr;
            b->size    = size;
            b->free    = 0;
            b->is_mmap = 1;
            b->next    = NULL;
            return (char*)b + HEADER_SIZE;
        }
        fprintf(stderr, "MALLOC mmap fail size=%u, fallback brk\n", (unsigned)size);
    }

    /* 小分配：走 brk */
    if (head == NULL) {
        void *p = heap_grow(sizeof(struct block) + size + 4096);
        if (!p) return NULL;
        head = (struct block*)p;
        head->size    = g_grow_size - HEADER_SIZE;
        head->free    = 1;
        head->is_mmap = 0;
        head->next    = NULL;
    }

    for (struct block *b = head; b; b = b->next) {
        if (b->free && b->size >= size) {
            if (b->size >= size + HEADER_SIZE + MIN_SPLIT) {
                struct block *nb = (struct block*)((char*)b + HEADER_SIZE + size);
                nb->size    = b->size - size - HEADER_SIZE;
                nb->free    = 1;
                nb->is_mmap = 0;
                nb->next    = b->next;
                b->next = nb;
                b->size = size;
            }
            b->free = 0;
            return (char*)b + HEADER_SIZE;
        }
    }

    void *p = heap_grow(HEADER_SIZE + size);
    if (!p) {
        fprintf(stderr, "MALLOC brk fail size=%u\n", (unsigned)size);
        return NULL;
    }

    struct block *nb = (struct block*)p;
    nb->size    = g_grow_size - HEADER_SIZE;
    nb->free    = 0;
    nb->is_mmap = 0;
    nb->next    = head;
    head = nb;

    return (char*)nb + HEADER_SIZE;
}

void free(void *ptr) {
    if (!ptr) return;
    struct block *b = (struct block*)((char*)ptr - HEADER_SIZE);

    if (b->is_mmap) {
        size_t total = b->size + HEADER_SIZE;
        size_t map_size = (total + 4095) & ~4095u;
        sys_munmap((unsigned int)b, map_size);
        return;
    }

    b->free = 1;
}

void *calloc(size_t n, size_t size) {
    size_t total = n * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) { free(ptr); return NULL; }

    struct block *b = (struct block*)((char*)ptr - HEADER_SIZE);
    if (b->size >= new_size) return ptr;

    void *np = malloc(new_size);
    if (!np) return NULL;
    memcpy(np, ptr, b->size);
    free(ptr);
    return np;
}

extern void __libc_run_atexit(void);

void exit(int status) {
    __libc_run_atexit();
    proc_exit(status);
}