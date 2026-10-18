/* hexdump [file]
 * canonical 格式：偏移 + 16 字节十六进制 + ASCII 列。
 * 无 file 时读 stdin。 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"

#define BUF_SIZE 512

static int hexdump_fd(int fd, int is_file) {
    unsigned char buf[BUF_SIZE];
    unsigned int offset = 0;
    int n;

    while ((n = is_file ? fs_read(fd, buf, sizeof(buf))
                        : read(0, buf, sizeof(buf))) > 0) {
        int i = 0;
        while (i < n) {
            int chunk = n - i;
            if (chunk > 16) chunk = 16;

            printf("%08x  ", offset + i);

            for (int j = 0; j < 16; j++) {
                if (j < chunk) printf("%02x ", buf[i + j]);
                else           printf("   ");
                if (j == 7) putchar(' ');
            }

            putchar('|');
            for (int j = 0; j < chunk; j++) {
                unsigned char c = buf[i + j];
                putchar((c >= 0x20 && c < 0x7F) ? c : '.');
            }
            putchar('|');
            putchar('\n');

            i += chunk;
        }
        offset += n;
    }

    printf("%08x\n", offset);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) {
        hexdump_fd(0, 0);
        return 0;
    }
    if (argc > 2) {
        dprintf(2, "usage: hexdump [file]\n");
        return 2;
    }

    int fd = fs_open(argv[1], FS_O_RDONLY);
    if (fd < 0) {
        dprintf(2, "hexdump: %s: error %d\n", argv[1], fd);
        return 2;
    }
    hexdump_fd(fd, 1);
    fs_close(fd);
    return 0;
}