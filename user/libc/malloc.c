#include "stdlib.h"
#include "syscall.h"
#include "string.h"

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

/* 扩展物理内存：brk 对齐到 4KB */
static void *heap_grow(size_t need) {
    int cur = brk(0);
    if (cur < 0) return NULL;

    int new_brk = cur + (int)need;
    new_brk = (new_brk + 4095) & ~4095;

    if (brk(new_brk) != 0) return NULL;

    return (void*)cur;
}

void *malloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN_8(size);

    /* 大分配：走 mmap */
    if (size >= MMAP_THRESHOLD) {
        size_t total = size + HEADER_SIZE;
        size_t map_size = (total + 4095) & ~4095u;
        unsigned int addr = mmap(0, map_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1);
        if ((int)addr <= 0) return NULL;

        struct block *b = (struct block*)addr;
        b->size    = size;
        b->free    = 0;
        b->is_mmap = 1;
        b->next    = NULL;
        return (char*)b + HEADER_SIZE;
    }

    /* 小分配：走 brk */
    if (head == NULL) {
        void *p = heap_grow(sizeof(struct block) + size + 4096);
        if (!p) return NULL;
        head = (struct block*)p;
        head->size    = 4096;
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
    if (!p) return NULL;

    struct block *nb = (struct block*)p;
    nb->size    = size;
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
        munmap((unsigned int)b, map_size);
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

void exit(int status) {
    _exit(status);
}