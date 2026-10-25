#include "posix_internal.h"
#include <stddef.h>

static struct posix_fd g_fd_table[POSIX_MAX_FD] = {
    {1, FD_TYPE_FILE, 0, 0}, /* 0: stdin */
    {1, FD_TYPE_FILE, 0, 0}, /* 1: stdout */
    {1, FD_TYPE_FILE, 0, 0}, /* 2: stderr */
};

int posix_alloc_fd(int type, int handle) {
    for (int i = 3; i < POSIX_MAX_FD; i++) {
        if (!g_fd_table[i].used) {
            g_fd_table[i].used = 1;
            g_fd_table[i].type = type;
            g_fd_table[i].handle = handle;
            g_fd_table[i].offset = 0;
            return i;
        }
    }
    return -1;
}

struct posix_fd *posix_get_fd(int fd) {
    if (fd < 0 || fd >= POSIX_MAX_FD || !g_fd_table[fd].used) return NULL;
    return &g_fd_table[fd];
}

void posix_free_fd(int fd) {
    if (fd >= 0 && fd < POSIX_MAX_FD) {
        g_fd_table[fd].used = 0;
    }
}