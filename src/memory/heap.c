#include <heap.h>
#include <pmm.h>
#include <printf.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/*
 * 简化版 slab 分配器
 * - 9 个大小类：8, 16, 32, 64, 128, 256, 512, 1024, 2048
 * - 每个 slab 占一页（4KB），页头是 struct slab
 * - 数据区从页头之后开始，切成 obj_size 的块
 * - 空闲块用块内存储的 next 指针串成链表
 * - 大对象（> 2048）暂不支持，返回 NULL
 */

#define NUM_CACHES      9
#define MAX_OBJ_SIZE    2048


/* 大对象（> MAX_OBJ_SIZE）：直通 pmm，一页内。
 * 页头 16 字节，双 magic 防与 slab 页头碰撞。 */
#define LARGE_MAGIC1    0xB16B00B5u
#define LARGE_MAGIC2    0xDEADBEEFu

struct large_header {
    uint32_t magic1;
    uint32_t magic2;
    uint32_t pages;    /* 当前固定 1 */
    uint32_t size;     /* 用户请求的字节数 */
};

#define LARGE_MAX_SIZE  (4096 - (uint32_t)sizeof(struct large_header))


static const uint32_t cache_sizes[NUM_CACHES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048
};

struct slab {
    struct slab *next;       /* 下一个 slab */
    uint32_t     obj_size;   /* 对象大小 */
    uint32_t     obj_count;  /* 本 slab 能放多少对象 */
    uint32_t     free_count; /* 空闲对象数量 */
    void        *free_list;  /* 空闲对象链表 */
};

struct kmem_cache {
    uint32_t     obj_size;
    struct slab *slabs;
    uint32_t     total_free;  /* 空闲对象总数 */
    uint32_t     total_slabs; /* slab 数量 */
};

static struct kmem_cache caches[NUM_CACHES];

/* 找到合适的 cache 索引，失败返回 -1 */
static int size_to_cache(size_t size) {
    for (int i = 0; i < NUM_CACHES; i++) {
        if (size <= cache_sizes[i]) return i;
    }
    return -1;
}

/* 创建一个新 slab */
static struct slab *slab_create(uint32_t obj_size) {
    uint32_t phys = pmm_alloc_page();
    if (!phys) return NULL;

    struct slab *s = (struct slab*)phys;
    uint32_t header_size = sizeof(struct slab);

    /* 数据区（物理地址=虚拟地址，恒等映射） */
    uint8_t *data = (uint8_t*)phys + header_size;
    uint32_t avail = 4096 - header_size;

    s->next = NULL;
    s->obj_size = obj_size;
    s->obj_count = avail / obj_size;
    s->free_count = s->obj_count;
    s->free_list = NULL;

    /* 把数据区切成对象，依次串到 free_list */
    for (uint32_t i = 0; i < s->obj_count; i++) {
        void *obj = data + i * obj_size;
        *(void**)obj = s->free_list;
        s->free_list = obj;
    }
    return s;
}

void heap_init(void) {
    for (int i = 0; i < NUM_CACHES; i++) {
        caches[i].obj_size = cache_sizes[i];
        caches[i].slabs = NULL;
        caches[i].total_free = 0;
        caches[i].total_slabs = 0;
    }
    kprintf("[HEAP] Initialized (slab, max=%d bytes)\n", MAX_OBJ_SIZE);
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    /* 大对象：直通 pmm，一页内 */
    if (size > MAX_OBJ_SIZE) {
        if (size > LARGE_MAX_SIZE) return NULL;
        uint32_t phys = pmm_alloc_page();
        if (!phys) return NULL;
        struct large_header *h = (struct large_header*)phys;
        h->magic1 = LARGE_MAGIC1;
        h->magic2 = LARGE_MAGIC2;
        h->pages  = 1;
        h->size   = (uint32_t)size;
        return (void*)(phys + sizeof(struct large_header));
    }

    /* 小对象：slab 路径 */
    int idx = size_to_cache(size);
    if (idx < 0) return NULL;

    struct kmem_cache *c = &caches[idx];

    /* 找一个有空闲的 slab */
    struct slab *s = c->slabs;
    while (s) {
        if (s->free_count > 0) break;
        s = s->next;
    }

    /* 没有空闲，创建新 slab */
    if (!s) {
        s = slab_create(c->obj_size);
        if (!s) {
            kprintf("[HEAP] ERROR: out of memory\n");
            return NULL;
        }
        s->next = c->slabs;
        c->slabs = s;
        c->total_free += s->obj_count;
        c->total_slabs++;
    }

    /* 从 free_list 取一个对象 */
    void *obj = s->free_list;
    s->free_list = *(void**)obj;
    s->free_count--;
    c->total_free--;

    return obj;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void *ptr) {
    if (!ptr) return;

    uint32_t phys = (uint32_t)ptr;
    uint32_t page = phys & ~0xFFF;

    /* 大对象识别：读页头双 magic */
    struct large_header *h = (struct large_header*)page;
    if (h->magic1 == LARGE_MAGIC1 && h->magic2 == LARGE_MAGIC2) {
        uint32_t pages = h->pages;
        h->magic1 = 0;
        h->magic2 = 0;
        for (uint32_t i = 0; i < pages; i++) {
            pmm_free_page(page + i * 4096);
        }
        return;
    }

    /* slab 路径 */
    struct slab *s = (struct slab*)page;

    if (s->obj_size == 0 || s->obj_size > MAX_OBJ_SIZE) {
        kprintf("[HEAP] WARN: kfree on non-slab ptr %p\n", ptr);
        return;
    }
    if (((uint32_t)ptr & 0xFFF) < sizeof(struct slab)) {
        kprintf("[HEAP] WARN: kfree on slab header %p\n", ptr);
        return;
    }

    int idx = size_to_cache(s->obj_size);
    if (idx < 0) return;

    *(void**)ptr = s->free_list;
    s->free_list = ptr;
    s->free_count++;
    caches[idx].total_free++;
}

void heap_stats(void) {
    kprintf("[HEAP] Stats:\n");
    for (int i = 0; i < NUM_CACHES; i++) {
        struct kmem_cache *c = &caches[i];
        if (c->total_slabs == 0) continue;
        kprintf("  size=%d  slabs=%d  free=%d\n",
                c->obj_size, c->total_slabs, c->total_free);
    }
}