#include <volume.h>
#include <errno.h>
#include <string.h>
#include <printf.h>
#include <stddef.h>

static struct volume volumes[VOL_MAX];
static struct volume *default_volume = NULL;

void volume_init(void) {
    for (int i = 0; i < VOL_MAX; i++) {
        volumes[i].used = 0;
        volumes[i].name[0] = '\0';
    }
    default_volume = NULL;
    kprintf("[VOL] Initialized (%d slots)\n", VOL_MAX);
}

/* 名字校验：非空，长度 < VOL_NAME_LEN，只允许字母和数字 */
static int is_valid_name(const char *name) {
    if (!name || name[0] == '\0') return 0;
    int len = 0;
    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9'))) {
            return 0;
        }
        len++;
        if (len >= VOL_NAME_LEN) return 0;
    }
    return 1;
}

int volume_register(const char *name, uint8_t fs_type,
                    uint32_t start_lba, uint32_t sector_count,
                    void *fs_private) {
    if (!is_valid_name(name)) return EINVAL;
    if (fs_type == VOL_FS_NONE) return EINVAL;

    /* 名字不能重复 */
    if (volume_lookup(name)) return EEXIST;

    /* 找一个空槽 */
    for (int i = 0; i < VOL_MAX; i++) {
        if (!volumes[i].used) {
            struct volume *v = &volumes[i];

            /* 拷贝名字 */
            int j;
            for (j = 0; name[j] && j < VOL_NAME_LEN - 1; j++) {
                v->name[j] = name[j];
            }
            v->name[j] = '\0';

            v->used = 1;
            v->fs_type = fs_type;
            v->start_lba = start_lba;
            v->sector_count = sector_count;
            v->fs_private = fs_private;

            /* 第一个注册的卷成为默认卷 */
            if (!default_volume) default_volume = v;

            kprintf("[VOL] Registered '%s' (fs_type=%d, lba=%d, sectors=%d)\n",
                    v->name, fs_type, start_lba, sector_count);
            return OK;
        }
    }
    return ENOSPC;
}

struct volume *volume_lookup(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < VOL_MAX; i++) {
        if (volumes[i].used && strcmp(volumes[i].name, name) == 0) {
            return &volumes[i];
        }
    }
    return NULL;
}

int volume_list(struct volume **out, int max) {
    int n = 0;
    for (int i = 0; i < VOL_MAX && n < max; i++) {
        if (volumes[i].used) {
            out[n++] = &volumes[i];
        }
    }
    return n;
}

struct volume *volume_get_default(void) {
    return default_volume;
}