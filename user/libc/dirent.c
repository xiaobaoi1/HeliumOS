#include "dirent.h"
#include "syscall.h"
#include "stdlib.h"

struct __user_DIR {
    int fd;
    struct dirent ent;
};

DIR *opendir(const char *name) {
    int fd = fs_opendir(name);
    if (fd < 0) return 0;
    DIR *d = (DIR*)malloc(sizeof(*d));
    if (!d) { fs_closedir(fd); return 0; }
    d->fd = fd;
    return d;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) return 0;
    if (fs_readdir(dirp->fd, &dirp->ent) != 1) return 0;
    return &dirp->ent;
}

int closedir(DIR *dirp) {
    if (!dirp) return -1;
    fs_closedir(dirp->fd);
    free(dirp);
    return 0;
}

int dirfd(DIR *dirp) {
    return dirp ? dirp->fd : -1;
}