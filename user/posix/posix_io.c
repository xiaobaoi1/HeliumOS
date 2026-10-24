#include <fcntl.h>
#include <posix_internal.h>
#include "syscall.h"
#include <stddef.h>

#define SEEK_SET 0

struct posix_fd;
int posix_alloc_fd(int type, int handle);
struct posix_fd *posix_get_fd(int fd);
void posix_free_fd(int fd);

#define FD_TYPE_FILE 1
#define FD_TYPE_PIPE 2

int open(const char *path, int flags, ...) {
    int h = fs_open(path, flags);
    if (h < 0) return -1;
    return posix_alloc_fd(FD_TYPE_FILE, h);
}

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

long posix_read(int fd, void *buf, size_t count) {
    if (fd == 0) return read(0, buf, count); // read 是原生接口，下同
    
    struct posix_fd *pfd = posix_get_fd(fd);
    if (!pfd) return -1;
    
    if (pfd->type == FD_TYPE_FILE) return fs_read(pfd->handle, buf, count);
    if (pfd->type == FD_TYPE_PIPE) return pipe_read(pfd->handle, buf, count);
    return -1;
}

long posix_write(int fd, const void *buf, size_t count) {
    if (fd == 1 || fd == 2) return write(fd, buf, count);
    
    struct posix_fd *pfd = posix_get_fd(fd);
    if (!pfd) return -1;
    
    if (pfd->type == FD_TYPE_FILE) return fs_write(pfd->handle, buf, count);
    if (pfd->type == FD_TYPE_PIPE) return pipe_write(pfd->handle, buf, count);
    return -1;
}

long posix_lseek(int fd, long offset, int whence) {
    if (whence != SEEK_SET) return -1;
    struct posix_fd *pfd = posix_get_fd(fd);
    if (!pfd || pfd->type != FD_TYPE_FILE) return -1;
    if (fs_seek(pfd->handle, offset) < 0) return -1;
    return offset;
}

int posix_ftruncate(int fd, long length) {
    (void)length;
    struct posix_fd *pfd = posix_get_fd(fd);
    if (!pfd || pfd->type != FD_TYPE_FILE) return -1;
    return fs_truncate(pfd->handle);
}