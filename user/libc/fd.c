#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <stdint.h>
#include "syscall.h"

#define FD_MAX 256
#define FD_KIND_FS   1
#define FD_KIND_PTY  2
#define FD_KIND_IPC  3

static struct {
    uint8_t used;
    uint8_t kind;
    int handle;
    off_t offset;
} g_fd[FD_MAX] = {
    [0] = {1, 0, 0, 0},
    [1] = {1, 0, 1, 0},
    [2] = {1, 0, 2, 0},
};

static int fd_alloc(int kind, int handle) {
    for (int i = 3; i < FD_MAX; i++) {
        if (!g_fd[i].used) {
            g_fd[i].used = 1;
            g_fd[i].kind = kind;
            g_fd[i].handle = handle;
            g_fd[i].offset = 0;
            return i;
        }
    }
    return -1;
}

int open(const char *path, int flags, ...) {
    int h = fs_open(path, flags); 
    if (h < 0) return -1;
    int fd = fd_alloc(FD_KIND_FS, h);
    if (fd < 0) { fs_close(h); return -1; }
    return fd;
}

int close(int fd) {
    if (fd >= 0 && fd <= 2) return 0;
    if (fd < 0 || fd >= FD_MAX || !g_fd[fd].used) return -1;
    int h = g_fd[fd].handle;
    switch (g_fd[fd].kind) {
        case FD_KIND_FS:  fs_close(h); break;
        case FD_KIND_PTY: pty_close(h); break;
        case FD_KIND_IPC: pipe_close(h); break;
    }
    g_fd[fd].used = 0;
    return 0;
}

ssize_t read(int fd, void *buf, size_t n) {
    if (fd == 0) return sys_read(0, buf, n);
    if (fd < 0 || fd >= FD_MAX || !g_fd[fd].used) return -1;
    int h = g_fd[fd].handle;
    ssize_t r = -1;
    switch (g_fd[fd].kind) {
        case FD_KIND_FS:  r = fs_read(h, buf, n); break;
        case FD_KIND_PTY: r = pty_read(h, buf, n); break;
        case FD_KIND_IPC: r = pipe_read(h, buf, n); break;
    }
    if (r > 0) g_fd[fd].offset += r;
    return r;
}

ssize_t write(int fd, const void *buf, size_t n) {
    if (fd == 1 || fd == 2) return sys_write(fd, buf, n);
    if (fd < 0 || fd >= FD_MAX || !g_fd[fd].used) return -1;
    int h = g_fd[fd].handle;
    ssize_t r = -1;
    switch (g_fd[fd].kind) {
        case FD_KIND_FS:  r = fs_write(h, buf, n); break;
        case FD_KIND_PTY: r = pty_write(h, buf, n); break;
        case FD_KIND_IPC: r = pipe_write(h, buf, n); break;
    }
    if (r > 0) g_fd[fd].offset += r;
    return r;
}

off_t lseek(int fd, off_t offset, int whence) {
    if (fd < 0 || fd >= FD_MAX || !g_fd[fd].used) return -1;
    if (g_fd[fd].kind != FD_KIND_FS) return -1;
    
    off_t new_off = g_fd[fd].offset;
    if (whence == SEEK_SET) {
        new_off = offset;
    } else if (whence == SEEK_CUR) {
        new_off += offset;
    } else if (whence == SEEK_END) {
        struct fstat_buf st;
        if (fs_fstat(g_fd[fd].handle, &st) < 0) return -1;
        new_off = st.size + offset;
    } else {
        return -1;
    }

    if (new_off < 0) return -1;
    if (fs_seek(g_fd[fd].handle, new_off) < 0) return -1;
    g_fd[fd].offset = new_off;
    return new_off;
}

int ftruncate(int fd, off_t length) {
    if (fd < 0 || fd >= FD_MAX || !g_fd[fd].used) return -1;
    if (g_fd[fd].kind != FD_KIND_FS) return -1;
    if (length != 0) return -1;
    if (fs_truncate(g_fd[fd].handle) == 0) {
        g_fd[fd].offset = 0;
        return 0;
    }
    return -1;
}