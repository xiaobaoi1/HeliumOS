#include "syscall.h"
#include "stdio.h"
#include "string.h"

#define BUF_SIZE 512

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(2, "usage: cat <file> [file...]\n");
        return 1;
    }

    int had_error = 0;
    for (int i = 1; i < argc; i++) {
        int fd = fs_open(argv[i], FS_O_RDONLY);
        if (fd < 0) {
            dprintf(2, "cat: %s: error %d\n", argv[i], fd);
            had_error = 1;
            continue;
        }
        char buf[BUF_SIZE];
        int n;
        while ((n = fs_read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, n);
        }
        fs_close(fd);
        if (n < 0) {
            dprintf(2, "cat: read error %d\n", n);
            had_error = 1;
        }
    }
    return had_error;
}