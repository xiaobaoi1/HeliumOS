/* grep [-n] pattern [file...]
 * 固定字符串匹配（无正则）。无 file 时读 stdin。
 * 退出码：0 有匹配，1 无匹配，2 错误。 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"

#define BUF_SIZE  512
#define LINE_MAX  1024

static int grep_fd(int fd, int is_file, const char *pat,
                   const char *fname, int show_lineno, int show_fname) {
    char buf[BUF_SIZE];
    char line[LINE_MAX];
    int line_len = 0;
    int lineno = 0;
    int matches = 0;
    int n;

    while ((n = is_file ? fs_read(fd, buf, sizeof(buf))
                        : read(0, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[line_len] = '\0';
                lineno++;
                if (strstr(line, pat)) {
                    if (show_fname)  printf("%s:", fname);
                    if (show_lineno) printf("%d:", lineno);
                    printf("%s\n", line);
                    matches++;
                }
                line_len = 0;
            } else if (line_len < LINE_MAX - 1) {
                line[line_len++] = c;
            }
        }
    }

    /* 最后一行没有 '\n' */
    if (line_len > 0) {
        line[line_len] = '\0';
        lineno++;
        if (strstr(line, pat)) {
            if (show_fname)  printf("%s:", fname);
            if (show_lineno) printf("%d:", lineno);
            printf("%s\n", line);
            matches++;
        }
    }

    return matches;
}

int main(int argc, char **argv) {
    int show_lineno = 0;
    int argi = 1;

    if (argi < argc && strcmp(argv[argi], "-n") == 0) {
        show_lineno = 1;
        argi++;
    }

    if (argi >= argc) {
        dprintf(2, "usage: grep [-n] pattern [file...]\n");
        return 2;
    }

    const char *pat = argv[argi++];

    /* 无文件：stdin */
    if (argi >= argc) {
        int m = grep_fd(0, 0, pat, NULL, show_lineno, 0);
        return m > 0 ? 0 : 1;
    }

    int show_fname = (argc - argi) > 1;
    int total = 0;
    int had_error = 0;

    for (int i = argi; i < argc; i++) {
        int fd = fs_open(argv[i], FS_O_RDONLY);
        if (fd < 0) {
            dprintf(2, "grep: %s: error %d\n", argv[i], fd);
            had_error = 1;
            continue;
        }
        total += grep_fd(fd, 1, pat, argv[i], show_lineno, show_fname);
        fs_close(fd);
    }

    if (had_error) return 2;
    return total > 0 ? 0 : 1;
}