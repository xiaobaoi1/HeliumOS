#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <volume.h>

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

/* 目录项（对用户友好） */
#define DIRENT_NAME_MAX  256
struct dirent {
    char     name[DIRENT_NAME_MAX];
    uint8_t  attributes;         /* bit4 = 目录 */
    uint8_t  reserved[3];
    uint32_t size;
};

/* 每进程的句柄槽位 */
struct fs_handle {
    uint8_t         used;
    uint8_t         obj_type;    /* FS_OBJ_FILE / FS_OBJ_DIR */
    uint8_t         fs_type;     /* VOL_FS_FAT32 ... */
    uint8_t         reserved;
    uint32_t        offset;
    struct volume  *vol;
    union {
        void              *ptr;      /* 保留 */
        struct fat32_file  fat32_file;
        struct fat32_dir   fat32_dir;
    } u;
};

/* ---------- 生命周期 ---------- */
void fs_init(void);
void fs_release_all(struct task *task);   /* 进程退出时调用 */

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