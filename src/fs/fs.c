#include <fs.h>
#include <fat32.h>
#include <task.h>
#include <path.h>
#include <errno.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>

void fs_init(void) {
    kprintf("[FS] Initialized\n");
}

/* ---------- 句柄分配 ---------- */
static fd_t fs_alloc_handle(void) {
    struct task *cur = get_current_task();
    if (!cur) return FD_INVALID;
    for (int i = 0; i < FS_MAX_HANDLES; i++) {
        if (!cur->fs_handles[i].used) {
            memset(&cur->fs_handles[i], 0, sizeof(struct fs_handle));
            cur->fs_handles[i].used = 1;
            return i;
        }
    }
    return FD_INVALID;
}

static struct fs_handle *fs_get(fd_t fd) {
    struct task *cur = get_current_task();
    if (!cur || fd < 0 || fd >= FS_MAX_HANDLES) return NULL;
    if (!cur->fs_handles[fd].used) return NULL;
    return &cur->fs_handles[fd];
}

/* ---------- 打开文件 ---------- */
fd_t fs_open(const char *path, int flags) {
    struct resolved_path rp;
    if (resolve_path(path, &rp) != OK) return FD_INVALID;

    fd_t fd = fs_alloc_handle();
    if (fd < 0) return FD_INVALID;

    struct fs_handle *h = fs_get(fd);
    h->vol = rp.vol;
    h->fs_type = rp.vol->fs_type;
    h->obj_type = FS_OBJ_FILE;
    h->offset = 0;

    switch (rp.vol->fs_type) {
        case VOL_FS_FAT32: {
            struct fat32_volume *fvol = (struct fat32_volume*)rp.vol->fs_private;
            if (!fvol) { h->used = 0; return FD_INVALID; }
            if (fat32_open_file(fvol, rp.path, &h->u.fat32_file) != OK) {
                h->used = 0;
                return FD_INVALID;
            }
            return fd;
        }
        default:
            h->used = 0;
            return FD_INVALID;
    }
}

int fs_read(fd_t fd, void *buf, uint32_t n) {
    struct fs_handle *h = fs_get(fd);
    if (!h || h->obj_type != FS_OBJ_FILE) return EINVAL;
    if (!buf || n == 0) return EINVAL;

    switch (h->fs_type) {
        case VOL_FS_FAT32: {
            struct fat32_volume *fvol = (struct fat32_volume*)h->vol->fs_private;
            if (!fvol) return EINVAL;

            /* 已经到文件末尾 */
            if (h->offset >= h->u.fat32_file.file_size) {
                return 0;    /* EOF */
            }

            /* 截断到剩余字节数 */
            uint32_t remaining = h->u.fat32_file.file_size - h->offset;
            if (n > remaining) n = remaining;

            int r = fat32_read_file(fvol, &h->u.fat32_file, buf, h->offset, n);
            if (r > 0) h->offset += r;
            return r;
        }
        default: return ENOSYS;
    }
}

int fs_seek(fd_t fd, uint32_t offset) {
    struct fs_handle *h = fs_get(fd);
    if (!h || h->obj_type != FS_OBJ_FILE) return EINVAL;
    if (offset > h->u.fat32_file.file_size) return EINVAL;
    h->offset = offset;
    return OK;
}

int fs_close(fd_t fd) {
    struct fs_handle *h = fs_get(fd);
    if (!h) return EINVAL;
    h->used = 0;
    return OK;
}

int fs_write(fd_t fd, const void *buf, uint32_t n) {
    (void)fd; (void)buf; (void)n;
    return ENOSYS;
}

/* ---------- 目录 ---------- */
fd_t fs_opendir(const char *path) {
    struct resolved_path rp;
    if (resolve_path(path, &rp) != OK) return FD_INVALID;

    fd_t fd = fs_alloc_handle();
    if (fd < 0) return FD_INVALID;

    struct fs_handle *h = fs_get(fd);
    h->vol = rp.vol;
    h->fs_type = rp.vol->fs_type;
    h->obj_type = FS_OBJ_DIR;
    h->offset = 0;

    switch (rp.vol->fs_type) {
        case VOL_FS_FAT32: {
            struct fat32_volume *fvol = (struct fat32_volume*)rp.vol->fs_private;
            if (!fvol) { h->used = 0; return FD_INVALID; }
            if (fat32_opendir(fvol, rp.path, &h->u.fat32_dir) != OK) {
                h->used = 0;
                return FD_INVALID;
            }
            return fd;
        }
        default:
            h->used = 0;
            return FD_INVALID;
    }
}

int fs_readdir(fd_t fd, struct dirent *out) {
    struct fs_handle *h = fs_get(fd);
    if (!h || h->obj_type != FS_OBJ_DIR) return EINVAL;

    switch (h->fs_type) {
        case VOL_FS_FAT32: {
            struct fat32_volume *fvol = (struct fat32_volume*)h->vol->fs_private;
            return fat32_readdir(fvol, &h->u.fat32_dir, out);
        }
        default: return ENOSYS;
    }
}

int fs_closedir(fd_t fd) {
    struct fs_handle *h = fs_get(fd);
    if (!h || h->obj_type != FS_OBJ_DIR) return EINVAL;

    switch (h->fs_type) {
        case VOL_FS_FAT32: {
            struct fat32_volume *fvol = (struct fat32_volume*)h->vol->fs_private;
            fat32_closedir(fvol, &h->u.fat32_dir);
            break;
        }
    }
    h->used = 0;
    return OK;
}

/* ---------- cwd ---------- */
int fs_chdir(const char *path) {
    struct resolved_path rp;
    if (resolve_path(path, &rp) != OK) return ENOENT;

    switch (rp.vol->fs_type) {
        case VOL_FS_FAT32: {
            struct fat32_volume *fvol = (struct fat32_volume*)rp.vol->fs_private;
            if (!fat32_is_dir(fvol, rp.path)) return ENOTDIR;
            break;
        }
        default: return ENOSYS;
    }

    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    strcpy(cur->cwd_volume, rp.vol->name);
    strcpy(cur->cwd_path, rp.path);
    return OK;
}

int fs_getcwd(char *buf, int size) {
    struct task *cur = get_current_task();
    if (!cur || !buf || size < 4) return EINVAL;

    int len = 0;
    for (int i = 0; cur->cwd_volume[i] && len < size - 2; i++)
        buf[len++] = cur->cwd_volume[i];
    if (len < size - 1) buf[len++] = ':';
    for (int i = 0; cur->cwd_path[i] && len < size - 1; i++)
        buf[len++] = cur->cwd_path[i];
    buf[len] = '\0';
    return len;
}

/* ---------- 释放所有句柄 ---------- */
void fs_release_all(struct task *task) {
    if (!task) return;
    for (int i = 0; i < FS_MAX_HANDLES; i++) {
        task->fs_handles[i].used = 0;
    }
}