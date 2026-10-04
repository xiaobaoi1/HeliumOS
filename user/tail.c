/* tail [-n N] [file]
 * 简单实现：读全部到内存，反向找第 N+1 个换行。
 * 文件上限 1MB。 */
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#define TAIL_MAX_SIZE (1024 * 1024)

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
            dprintf(2, "tail: %s: error %d\n", path, fd);
            return 1;
        }
        is_file = 1;
    }

    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) { if (is_file) fs_close(fd); return 1; }
    size_t len = 0;
    char tmp[512];
    int r;
    while ((r = read_chunk(fd, tmp, sizeof(tmp), is_file)) > 0) {
        if (len + r > cap) {
            size_t newcap = cap * 2;
            while (newcap < len + r) newcap *= 2;
            if (newcap > TAIL_MAX_SIZE) {
                dprintf(2, "tail: input too large\n");
                free(buf);
                if (is_file) fs_close(fd);
                return 1;
            }
            char *nb = malloc(newcap);
            if (!nb) { free(buf); if (is_file) fs_close(fd); return 1; }
            memcpy(nb, buf, len);
            free(buf);
            buf = nb;
            cap = newcap;
        }
        memcpy(buf + len, tmp, r);
        len += r;
    }
    if (is_file) fs_close(fd);

    /* 反向找第 n+1 个 '\n'。找不到则从头输出。 */
    size_t start = 0;
    int count = 0;
    for (size_t i = (size_t)len - 1; i >= 0; i--) {
        if (buf[i] == '\n') {
            if (++count == n + 1) {
                start = (size_t)i + 1;
                break;
            }
        }
    }
    if (start < len) write(1, buf + start, len - start);
    free(buf);
    return 0;
}