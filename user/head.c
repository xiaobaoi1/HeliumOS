/* head [-n N] [file]
 * 无 file 时读 stdin。默认前 10 行。 */
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

static int read_chunk(int fd, char *buf, int n, int is_file) {
    return is_file ? fs_read(fd, buf, n) : read(0, buf, n);
}

int main(int argc, char **argv) {
    int n = 10;
    int argi = 1;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        n = atoi(argv[2]);
        argi = 3;
    }
    if (n <= 0) return 0;

    const char *path = (argi < argc) ? argv[argi] : NULL;
    int fd = 0, is_file = 0;
    if (path) {
        fd = fs_open(path, FS_O_RDONLY);
        if (fd < 0) {
            dprintf(2, "head: %s: error %d\n", path, fd);
            return 1;
        }
        is_file = 1;
    }

    char buf[512];
    int lines = 0;
    int r;
    while (lines < n && (r = read_chunk(fd, buf, sizeof(buf), is_file)) > 0) {
        for (int i = 0; i < r; i++) {
            putchar(buf[i]);
            if (buf[i] == '\n' && ++lines >= n) goto done;
        }
    }
done:
    if (is_file) fs_close(fd);
    return 0;
}