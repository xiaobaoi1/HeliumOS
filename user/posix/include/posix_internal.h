#ifndef POSIX_INTERNAL_H
#define POSIX_INTERNAL_H

#define POSIX_MAX_FD 64
#define FD_TYPE_FILE 1
#define FD_TYPE_PIPE 2

struct posix_fd {
    int used;
    int type;
    int handle;
    unsigned int offset;
};

int posix_alloc_fd(int type, int handle);
struct posix_fd *posix_get_fd(int fd);
void posix_free_fd(int fd);

#endif