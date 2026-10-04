/* wc [file...]
 * 无参数时读 stdin。输出 "lines words bytes"。 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"

static int is_space_c(int c) {
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\r' || c == '\f' || c == '\v';
}

static int read_chunk(int fd, char *buf, int n, int is_file) {
    return is_file ? fs_read(fd, buf, n) : read(0, buf, n);
}

static void count_stream(int fd, int is_file,
                         int *lines, int *words, int *bytes) {
    char buf[512];
    int r;
    int in_word = 0;
    while ((r = read_chunk(fd, buf, sizeof(buf), is_file)) > 0) {
        for (int i = 0; i < r; i++) {
            unsigned char c = (unsigned char)buf[i];
            (*bytes)++;
            if (c == '\n') (*lines)++;
            if (is_space_c(c)) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                (*words)++;
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        int l = 0, w = 0, b = 0;
        count_stream(0, 0, &l, &w, &b);
        printf("%d %d %d\n", l, w, b);
        return 0;
    }
    int total_l = 0, total_w = 0, total_b = 0;
    int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        int fd = fs_open(argv[i], FS_O_RDONLY);
        if (fd < 0) {
            dprintf(2, "wc: %s: error %d\n", argv[i], fd);
            continue;
        }
        int l = 0, w = 0, b = 0;
        count_stream(fd, 1, &l, &w, &b);
        fs_close(fd);
        printf("%d %d %d %s\n", l, w, b, argv[i]);
        total_l += l; total_w += w; total_b += b;
        nfiles++;
    }
    if (nfiles > 1) {
        printf("%d %d %d total\n", total_l, total_w, total_b);
    }
    return 0;
}