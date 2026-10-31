#ifndef DESKTOP_FS_H
#define DESKTOP_FS_H

#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include <stdint.h>

struct fs_entry_info {
    char name[64];
    int is_dir;
    int is_exec;
    int size;
};

static int check_elf_magic(const char *path) {
    int fd = fs_open(path, FS_O_RDONLY);
    if (fd < 0) return 0;
    uint8_t magic[4];
    int n = fs_read(fd, magic, 4);
    fs_close(fd);
    return (n == 4 && magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F');
}

static int fs_read_dir(const char *path, struct fs_entry_info *entries, int max_entries) {
    int dir = fs_opendir(path);
    if (dir < 0) return 0;

    int count = 0;
    struct dirent ent;
    while (count < max_entries) {
        memset(&ent, 0, sizeof(ent));
        int rc = fs_readdir(dir, &ent);
        if (rc < 0) break;
        if (ent.name[0] == '\0') break;
        ent.name[DIRENT_NAME_MAX - 1] = '\0';

        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) continue;

        int dup = 0;
        for (int k = 0; k < count; k++) {
            if (strcmp(entries[k].name, ent.name) == 0) { dup = 1; break; }
        }
        if (dup) break;

        strncpy(entries[count].name, ent.name, sizeof(entries[0].name) - 1);
        entries[count].is_dir = (ent.attributes & 0x10) != 0;
        entries[count].size = (int)ent.size;
        entries[count].is_exec = 0;

        if (!entries[count].is_dir) {
            char full_path[256];
            int len = strlen(path);
            if (len > 0 && path[len - 1] == '\\')
                snprintf(full_path, sizeof(full_path), "%s%s", path, ent.name);
            else
                snprintf(full_path, sizeof(full_path), "%s\\%s", path, ent.name);
            
            entries[count].is_exec = check_elf_magic(full_path);
        }
        
        count++;
    }
    fs_closedir(dir);
    return count;
}
#endif