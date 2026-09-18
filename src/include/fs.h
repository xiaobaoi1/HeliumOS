#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <volume.h>
#include <fat32.h>

/* 每进程最大句柄数 */
#define FS_MAX_HANDLES   32
#define FD_INVALID       (-1)

/* 打开标志 */
#define FS_O_RDONLY      0x01
#define FS_O_WRONLY      0x02
#define FS_O_RDWR        0x03
#define FS_O_CREAT       0x04    /* 预留 */

/* 对象类型 */
#define FS_OBJ_FILE      1
#define FS_OBJ_DIR       2

typedef int fd_t;

/* 前向声明，避免和 task.h 循环包含 */
struct task;

/* 每进程的句柄槽位 */
struct fs_handle {
    uint8_t         used;
    uint8_t         obj_type;    /* FS_OBJ_FILE / FS_OBJ_DIR */
    uint8_t         fs_type;     /* VOL_FS_FAT32 */
    uint8_t         reserved;
    uint32_t        offset;
    struct volume  *vol;
    union {
        struct fat32_file  fat32_file;
        struct fat32_dir   fat32_dir;
    } u;
};

/* ---------- 生命周期 ---------- */
void fs_init(void);
void fs_release_all(struct task *task);

/* ---------- 文件 API ---------- */
fd_t fs_open(const char *path, int flags);
int  fs_read(fd_t fd, void *buf, uint32_t n);
int  fs_write(fd_t fd, const void *buf, uint32_t n);
int  fs_seek(fd_t fd, uint32_t offset);
int  fs_close(fd_t fd);

/* ---------- 目录 API ---------- */
fd_t fs_opendir(const char *path);
int  fs_readdir(fd_t fd, struct dirent *out);
int  fs_closedir(fd_t fd);

/* ---------- cwd ---------- */
int  fs_chdir(const char *path);
int  fs_getcwd(char *buf, int size);

#endif