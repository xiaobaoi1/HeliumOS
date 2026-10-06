#include <shm.h>
#include <task.h>
#include <pmm.h>
#include <heap.h>
#include <errno.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>
#include <uaccess.h>

static struct shm g_shms[SHM_MAX_OBJECTS];

void shm_init(void) {
    memset(g_shms, 0, sizeof(g_shms));
}

struct shm *shm_at(int idx) {
    if (idx < 0 || idx >= SHM_MAX_OBJECTS) return NULL;
    if (!g_shms[idx].used) return NULL;
    return &g_shms[idx];
}

int shm_index(struct shm *s) {
    if (!s) return -1;
    int idx = (int)(s - g_shms);
    if (idx < 0 || idx >= SHM_MAX_OBJECTS) return -1;
    return idx;
}

struct shm *shm_lookup(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (g_shms[i].used && !g_shms[i].unlinked &&
            strcmp(g_shms[i].name, name) == 0) {
            return &g_shms[i];
        }
    }
    return NULL;
}

static struct shm *shm_create(const char *name, uint32_t num_pages) {
    if (num_pages == 0 || num_pages > SHM_MAX_PAGES) return NULL;

    for (int i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (!g_shms[i].used) {
            struct shm *s = &g_shms[i];
            memset(s, 0, sizeof(*s));
            strncpy(s->name, name, SHM_NAME_MAX - 1);
            s->num_pages = num_pages;
            s->pages = (uint32_t*)kmalloc(num_pages * sizeof(uint32_t));
            if (!s->pages) return NULL;
            memset(s->pages, 0, num_pages * sizeof(uint32_t));
            s->used = 1;
            s->refcount = 0;
            return s;
        }
    }
    return NULL;
}

struct shm *shm_create_kernel(const char *name, uint32_t num_pages) {
    return shm_create(name, num_pages);
}

static void shm_destroy(struct shm *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->num_pages; i++) {
        if (s->pages[i]) pmm_free_page(s->pages[i]);
    }
    kfree(s->pages);
    s->pages = NULL;
    s->used = 0;
}

void shm_ref(struct shm *s) {
    if (s) s->refcount++;
}

void shm_unref(struct shm *s) {
    if (!s) return;
    if (s->refcount == 0) return;
    s->refcount--;
    if (s->refcount == 0 && s->unlinked) {
        shm_destroy(s);
    }
}

int sys_shm_open(const char *name_user, uint32_t size_bytes, int flags) {
    (void)flags;
    char name[SHM_NAME_MAX];
    if (strncpy_from_user(name, name_user, sizeof(name)) < 0) return EFAULT;
    if (name[0] == '\0') return EINVAL;

    struct shm *s = shm_lookup(name);
    if (s) return shm_index(s);

    /* 不存在：创建。size_bytes 向上取整到页。 */
    uint32_t num_pages = (size_bytes + 0xFFF) / 0x1000;
    if (num_pages == 0) num_pages = 1;

    s = shm_create(name, num_pages);
    if (!s) return ENOSPC;
    return shm_index(s);
}

int sys_shm_unlink(const char *name_user) {
    char name[SHM_NAME_MAX];
    if (strncpy_from_user(name, name_user, sizeof(name)) < 0) return EFAULT;

    struct shm *s = shm_lookup(name);
    if (!s) return ENOENT;

    s->unlinked = 1;
    if (s->refcount == 0) shm_destroy(s);
    return OK;
}