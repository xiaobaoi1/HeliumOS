// user/shell.c
#include <string.h>
#include <stddef.h>
#include "syscall.h"


#define VGA_WIDTH    80
#define VGA_HEIGHT   25

static void write_str(const char *str) {
    int len = 0;
    while (str[len]) len++;
    syscall(SYS_WRITE, 1, (int)str, len);
}

static int read_char(char *c) {
    return syscall(SYS_READ, 0, (int)c, 1);
}

static void exit_proc(int status) {
    syscall(SYS_EXIT, status, 0, 0);
}

static int spawn(const char *path) {
    return syscall(SYS_SPAWN, (int)path, 0, 0);
}

static int waitpid(int pid) {
    return syscall(SYS_WAITPID, pid, 0, 0);
}


void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char*)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }
    return s;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void _start(void) {
    write_str("Welcome to HeliumOS Shell!\n");
    char cmd[64];
    int idx = 0;

    while (1) {
        write_str("$ ");
        idx = 0;
        char c;
        while (1) {
            if (read_char(&c) == 1) {
                if (c == '\n') {
                    write_str("\n");
                    break;
                }
                if (c == '\b' && idx > 0) {
                    idx--;
                    write_str("\b \b");
                    continue;
                }
                if (c != '\b' && idx < 63) {
                    cmd[idx++] = c;
                    char buf[2] = {c, 0};
                    write_str(buf);
                }
            }
            __asm__ volatile("pause");
        }
        cmd[idx] = '\0';

        // 简单命令解析
        if (cmd[0] == '\0') continue;
        if (strcmp(cmd, "exit") == 0) {
            write_str("Goodbye!\n");
            break;
        }else{
            int pid = spawn(cmd);
            if (pid < 0) {
                write_str("Failed to spawn\n");
            } else {
                waitpid(pid);
            }
        }
        // if (cmd[0] == 'r' && cmd[1] == 'u' && cmd[2] == 'n' && cmd[3] == ' ') {
        //     char *path = cmd + 4;
        //     int pid = spawn(path);
        //     if (pid < 0) {
        //         write_str("Failed to spawn\n");
        //     } else {
        //         waitpid(pid);
        //     }
        //     continue;
        // }
        // write_str("Unknown command\n");
    }
    exit_proc(0);
}