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
#define FS_O_CREAT       0x04
#define FS_O_TRUNC       0x08

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

/* 解析后的时间（和 RTC 布局一致） */
struct fs_time {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  reserved;
};

struct fstat_buf {
    uint32_t size;
    struct fs_time ctime;
    struct fs_time mtime;
    uint8_t  attributes;
    uint8_t  reserved[3];
};



/* ---------- 生命周期 ---------- */
void fs_init(void);
/* 释放一个进程的所有文件句柄。
 *
 * 当前只标 used=0——FAT32 的 fat32_file / fat32_dir 是纯数据，无额外资源。
 *
 * 契约：
 *   - 将来若有文件系统需要在 close 时释放资源（缓冲区、连接、锁等），
 *     在 fs_release_all 内按 fs_type 分派，或给 fs_handle 加 close 回调
 *   - fs_release_all 与 fs_close 的语义必须一致（谁 open 谁 close）
 */
void fs_release_all(struct task *task);

/* ---------- 文件 API ---------- */
fd_t fs_open(const char *path, int flags);
int  fs_read(fd_t fd, void *buf, uint32_t n);
int  fs_write(fd_t fd, const void *buf, uint32_t n);
int  fs_seek(fd_t fd, uint32_t offset);
int  fs_close(fd_t fd);
int  fs_fstat(fd_t fd, struct fstat_buf *out);

/* ---------- 目录 API ---------- */
fd_t fs_opendir(const char *path);
int  fs_readdir(fd_t fd, struct dirent *out);
int  fs_closedir(fd_t fd);
int fs_unlink(const char *path);
int fs_mkdir(const char *path);
int fs_rmdir(const char *path);
int fs_rename(const char *old_path, const char *new_path);

/* ---------- cwd ---------- */
int  fs_chdir(const char *path);
int  fs_getcwd(char *buf, int size);

/* 把 src_task 的 fs_handles[src_fd] 复制到 dst_task 的空闲槽。
 * 返回 dst_task 里的新 fd；失败返回 FD_INVALID。 */
fd_t fs_dup_handle(struct task *src_task, fd_t src_fd, struct task *dst_task);

#endif