/* find path [-name pattern]
 * 递归遍历目录。pattern 支持 '*' 和 '?' 通配。
 * 目录输出带 '/' 后缀。退出码：0 有结果，1 无结果，2 错误。 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"

#define PATH_BUF 512

static int glob_match(const char *pat, const char *s) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            while (*s) {
                if (glob_match(pat, s)) return 1;
                s++;
            }
            return 0;
        } else if (*pat == '?') {
            if (!*s) return 0;
            pat++; s++;
        } else {
            if (*pat != *s) return 0;
            pat++; s++;
        }
    }
    return *s == '\0';
}

static int find_recursive(const char *path, const char *pattern,
                          int has_pattern) {
    int fd = fs_opendir(path);
    if (fd < 0) return 0;

    struct dirent ent;
    int count = 0;

    while (fs_readdir(fd, &ent) == 1) {
        char full[PATH_BUF];
        int plen = strlen(path);
        if (plen > 0 && path[plen - 1] == '/')
            snprintf(full, sizeof(full), "%s%s", path, ent.name);
        else
            snprintf(full, sizeof(full), "%s/%s", path, ent.name);

        int is_dir = (ent.attributes & 0x10) != 0;

        if (!has_pattern || glob_match(pattern, ent.name)) {
            if (is_dir) printf("%s/\n", full);
            else        printf("%s\n", full);
            count++;
        }

        if (is_dir) {
            count += find_recursive(full, pattern, has_pattern);
        }
    }

    fs_closedir(fd);
    return count;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(2, "usage: find path [-name pattern]\n");
        return 2;
    }

    const char *path = argv[1];
    const char *pattern = NULL;
    int argi = 2;

    if (argi + 1 < argc && strcmp(argv[argi], "-name") == 0) {
        pattern = argv[argi + 1];
        argi += 2;
    }

    if (argi < argc) {
        dprintf(2, "find: unexpected argument '%s'\n", argv[argi]);
        return 2;
    }

    int count = find_recursive(path, pattern, pattern != NULL);
    return count > 0 ? 0 : 1;
}