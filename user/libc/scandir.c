#include <dirent.h>
#include <stdlib.h>
#include <string.h>

int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
    (void)compar; // 暂不实现排序
    DIR *d = opendir(dirp);
    if (!d) return -1;
    struct dirent **list = NULL;
    int count = 0, capacity = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (filter && !filter(ent)) continue;
        struct dirent *copy = malloc(sizeof(struct dirent));
        if (!copy) break;
        memcpy(copy, ent, sizeof(struct dirent));
        if (count >= capacity) {
            capacity = capacity ? capacity * 2 : 16;
            list = realloc(list, capacity * sizeof(struct dirent *));
        }
        list[count++] = copy;
    }
    closedir(d);
    *namelist = list;
    return count;
}