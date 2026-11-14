#include "sys/stat.h"
#include "syscall.h"

int fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    (void)dirfd;
    (void)flags;
    if (!buf) return -1;

    int fd = fs_open(path, FS_O_RDONLY);
    if (fd >= 0) {
        struct fstat_buf st;
        if (fs_fstat(fd, &st) == 0) {
            buf->st_size = st.size;
            buf->st_mode = S_IFREG | 0644;
            buf->st_mtime = 0;
        } else {
            buf->st_size = 0;
            buf->st_mode = S_IFREG | 0644;
        }
        fs_close(fd);
        return 0;
    }

    int dd = fs_opendir(path);
    if (dd >= 0) {
        fs_closedir(dd);
        buf->st_size = 0;
        buf->st_mode = S_IFDIR | 0755;
        buf->st_mtime = 0;
        return 0;
    }
    return -1;
}

int stat(const char *path, struct stat *buf) {
    return fstatat(-1, path, buf, 0);
}

int mkdir(const char *path, unsigned int mode) {
    (void)mode;
    return fs_mkdir(path);
}