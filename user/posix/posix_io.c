/* 第 1 步：别名 syscall.h 的原生 read/write，避免与 POSIX 的 read/write 冲突 */
#define read  helio_raw_read
#define write helio_raw_write
#include <fcntl.h>
#include <stddef.h>
#include "posix_internal.h"
#include "syscall.h"
#include "stdio.h"
#undef read
#undef write

/* 第 2 步：引入 unistd.h——它内部 #define read posix_read，
 * 立刻 undef，避免宏污染后续定义。 */
#include "unistd.h"
#undef read
#undef write

#include <stdlib.h>

/* ---------- 以下代码不会再被 read/write 宏污染 ---------- */

/* 1. read */
long read(int fd, void *buf, size_t count) {
    if (count == 0) return 0;
    if (fd == 0) return helio_raw_read(0, buf, (unsigned int)count);
    struct posix_fd *pfd = posix_get_fd(fd);
    if (!pfd) return -1;
    long r = -1;
    if (pfd->type == FD_TYPE_FILE) r = fs_read(pfd->handle, buf, count);
    else if (pfd->type == FD_TYPE_PIPE) r = pipe_read(pfd->handle, buf, count);
    if (r > 0) pfd->offset += r;
    return r;
}

/* 2. write */
long write(int fd, const void *buf, size_t count) {
    if (fd == 1 || fd == 2) return helio_raw_write(fd, buf, (unsigned int)count);
    struct posix_fd *pfd = posix_get_fd(fd);
    if (!pfd) return -1;
    long r = -1;
    if (pfd->type == FD_TYPE_FILE) r = fs_write(pfd->handle, buf, count);
    else if (pfd->type == FD_TYPE_PIPE) r = pipe_write(pfd->handle, buf, count);
    if (r > 0) pfd->offset += r;
    return r;
}

/* 3. open */
int open(const char *path, int flags, ...) {
    int helio_flags = 0;

    if ((flags & 3) == O_RDONLY)      helio_flags |= FS_O_RDONLY;
    else if ((flags & 3) == O_WRONLY) helio_flags |= FS_O_WRONLY;
    else if ((flags & 3) == O_RDWR)   helio_flags |= FS_O_RDWR;

    if (flags & O_CREAT)  helio_flags |= FS_O_CREAT;

    int h = fs_open(path, helio_flags);
    if (h < 0) return -1;

    int fd = posix_alloc_fd(FD_TYPE_FILE, h);
    if (fd >= 0 && (flags & O_TRUNC)) {
        ftruncate(fd, 0);
    }
    return fd;
}

/* 4. close */
int close(int fd) {
    if (fd >= 0 && fd <= 2) return 0;
    struct posix_fd *pfd = posix_get_fd(fd);
    if (!pfd) return -1;
    int r = -1;
    if (pfd->type == FD_TYPE_FILE) r = fs_close(pfd->handle);
    else if (pfd->type == FD_TYPE_PIPE) r = ipc_close(pfd->handle);
    if (r == 0) posix_free_fd(fd);
    return r;
}

/* 5. lseek */
long lseek(int fd, long offset, int whence) {
    struct posix_fd *pfd = posix_get_fd(fd);
    if (!pfd || pfd->type != FD_TYPE_FILE) return -1;
    long new_off = offset;
    if (whence == 1) new_off = pfd->offset + offset;
    else if (whence == 2) {
        struct fstat_buf st;
        if (fs_fstat(pfd->handle, &st) < 0) return -1;
        new_off = st.size + offset;
    }
    else if (whence != 0) return -1;

    if (new_off < 0) return -1;
    if (fs_seek(pfd->handle, new_off) < 0) return -1;
    pfd->offset = new_off;
    return new_off;
}

/* 6. ftruncate */
int ftruncate(int fd, long length) {
    (void)length;
    struct posix_fd *pfd = posix_get_fd(fd);
    if (!pfd || pfd->type != FD_TYPE_FILE) return -1;
    return fs_truncate(pfd->handle);
}

/* 应用层包装：unistd.h 的宏把 read/write 改成 posix_read/posix_write，
 * 这两个包装把调用转回真正的 read/write。 */
long posix_read(int fd, void *buf, size_t count) {
    return read(fd, buf, count);
}

long posix_write(int fd, const void *buf, size_t count) {
    return write(fd, buf, count);
}

/* fdopen */
FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    struct posix_fd *pfd = posix_get_fd(fd);
    if (!pfd) return NULL;
    return __stdio_create_from_handle(pfd->handle, pfd->offset);
}