#ifndef SHM_H
#define SHM_H

#include <stdint.h>

#define SHM_MAX_OBJECTS  16
#define SHM_NAME_MAX     32
#define SHM_MAX_PAGES    512   /* 2MB 上限 */

struct shm {
    char     name[SHM_NAME_MAX];
    uint32_t num_pages;
    uint32_t refcount;      /* 引用此对象的 VMA 数量 */
    uint32_t *pages;        /* 物理页数组（按需分配） */
    uint8_t  used;
    uint8_t  unlinked;      /* shm_unlink 已调用 */
    uint8_t  reserved[2];
};

void shm_init(void);

struct shm *shm_lookup(const char *name);
struct shm *shm_at(int idx);
int         shm_index(struct shm *s);

void shm_ref(struct shm *s);
void shm_unref(struct shm *s);

/* 系统调用 */
int sys_shm_open(const char *name_user, uint32_t size_bytes, int flags);
int sys_shm_unlink(const char *name_user);

/* 内核态创建 shm（不经过 syscall 参数检查） */
struct shm *shm_create_kernel(const char *name, uint32_t num_pages);

#endif