#include <unistd.h>
#include "syscall.h"

/* lseek / ftruncate 由 user/libc/fd.c 提供（fd 语义）。
 * libc 版直接操作 fs fd，语义不同，这里不重复定义。
 * NetSurf 等程序通过 stdio 层（fseek / fs_seek）访问，不经 lseek。 */

int rmdir(const char *path) {
    return fs_rmdir(path);
}

int unlinkat(int dirfd, const char *pathname, int flags) {
    (void)dirfd;
    if (flags & AT_REMOVEDIR)
        return fs_rmdir(pathname);
    return fs_unlink(pathname);
}

int access(const char *pathname, int mode) {
    (void)mode;
    int fd = fs_open(pathname, FS_O_RDONLY);
    if (fd >= 0) { fs_close(fd); return 0; }
    int dd = fs_opendir(pathname);
    if (dd >= 0) { fs_closedir(dd); return 0; }
    return -1;
}

int unlink(const char *path) {
    return fs_unlink(path);
}

int isatty(int fd) {
    /* 0/1/2 视为 tty，其他走 fs/pipe/pty 判断 */
    if (fd >= 0 && fd <= 2) return 1;
    return 0;
}

int execvp(const char *file, char *const argv[]) {
    (void)file; (void)argv;
    return -1;   /* 不支持 */
}