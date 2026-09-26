#include "syscall.h"
#include "stdio.h"
#include "string.h"

/* VGA 颜色常量（用户态独立定义） */
#define VGA_BLACK        0
#define VGA_LIGHT_GRAY   7
#define VGA_LIGHT_BLUE   9
#define VGA_LIGHT_GREEN  10
#define VGA_LIGHT_RED    12

#define CMD_MAX          128
#define ARGV_MAX         8
#define CAT_BUF_SIZE     512

/* ---------- 基础输入：读一行，带退格支持 ---------- */

static int read_line(char *buf, int max) {
    int i = 0;
    for (;;) {
        char c;
        if (read(0, &c, 1) == 1) {
            if (c == '\n') {
                putchar('\n');
                break;
            }
            if (c == '\b' && i > 0) {
                i--;
                printf("\b \b");
                continue;
            }
            if (c != '\b' && i < max - 1) {
                buf[i++] = c;
                putchar(c);
            }
        }
        __asm__ volatile("pause");
    }
    buf[i] = '\0';
    return i;
}

/* ---------- 参数解析：原地拆分，argv 存指针 ---------- */

static int tokenize(char *cmd, char **argv, int max) {
    int argc = 0;
    char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc >= max) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* ---------- 颜色辅助 ---------- */

static void set_color(int fg, int bg) {
    console_set_color(fg, bg);
}

static void print_error(const char *fmt, ...) {
    /* 简化：只接受一个字符串，格式留给调用者 */
    (void)fmt;
}

/* ---------- 内置命令 ---------- */

static void cmd_help(void) {
    printf("HeliumOS Shell commands:\n");
    printf("  ls [path]    list directory\n");
    printf("  cd [path]    change directory (no arg: show cwd)\n");
    printf("  pwd          print working directory\n");
    printf("  cat <file>   print file contents\n");
    printf("  clear        clear screen\n");
    printf("  help         show this help\n");
    printf("  exit         exit shell\n");
    printf("  <prog>       run program\n");
}

static void cmd_ls(int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : ".";

    int fd = fs_opendir(path);
    if (fd < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("ls: cannot open '%s' (error %d)\n", path, fd);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }

    struct dirent ent;
    int n;
    int count = 0;
    while ((n = fs_readdir(fd, &ent)) == 1) {
        if (ent.attributes & 0x10) {
            set_color(VGA_LIGHT_BLUE, VGA_BLACK);
            printf("%s/\n", ent.name);
        } else {
            set_color(VGA_LIGHT_GRAY, VGA_BLACK);
            printf("%s\n", ent.name);
        }
        count++;
    }

    fs_closedir(fd);
    set_color(VGA_LIGHT_GRAY, VGA_BLACK);

    if (n < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("ls: readdir failed (%d)\n", n);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }
    printf("total: %d\n", count);
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) {
        /* 无参数：显示 cwd */
        char buf[128];
        int n = getcwd(buf, sizeof(buf));
        if (n > 0) printf("%s\n", buf);
        return;
    }
    int r = chdir(argv[1]);
    if (r < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("cd: %s: error %d\n", argv[1], r);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}

static void cmd_pwd(void) {
    char buf[128];
    int n = getcwd(buf, sizeof(buf));
    if (n > 0) {
        printf("%s\n", buf);
    } else {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("pwd: error %d\n", n);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: cat <file>\n");
        return;
    }

    int fd = fs_open(argv[1], FS_O_RDONLY);
    if (fd < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("cat: %s: error %d\n", argv[1], fd);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }

    char buf[CAT_BUF_SIZE];
    int n;
    while ((n = fs_read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
    }
    fs_close(fd);

    if (n < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("\ncat: read error %d\n", n);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}

static void cmd_clear(void) {
    console_clear();
}

/* ---------- 主循环 ---------- */

void _start(int _argc, char **_argv) {
    (void)_argc; (void)_argv;
    printf("Welcome to HeliumOS Shell!\n");
    printf("Type 'help' for commands.\n\n");

    char cmd[CMD_MAX];
    char *argv[ARGV_MAX];

    while (1) {
        /* 提示符：亮绿 */
        set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        printf("$ ");
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);

        int n = read_line(cmd, sizeof(cmd));
        if (n == 0) continue;

        int argc = tokenize(cmd, argv, ARGV_MAX);
        if (argc == 0) continue;

        const char *c = argv[0];

        if (strcmp(c, "exit") == 0) {
            printf("Goodbye!\n");
            break;
        } else if (strcmp(c, "help") == 0) {
            cmd_help();
        } else if (strcmp(c, "ls") == 0) {
            cmd_ls(argc, argv);
        } else if (strcmp(c, "cd") == 0) {
            cmd_cd(argc, argv);
        } else if (strcmp(c, "pwd") == 0) {
            cmd_pwd();
        } else if (strcmp(c, "cat") == 0) {
            cmd_cat(argc, argv);
        } else if (strcmp(c, "clear") == 0) {
            cmd_clear();
        } else {
            /* 其他都当作可执行文件名 */
            argv[argc] = NULL;
            int h = spawn(c, argv);       /* ← 现在返回 handle */
            if (h < 0) {
                set_color(VGA_LIGHT_RED, VGA_BLACK);
                printf("spawn '%s' failed (error %d)\n", c, h);
                set_color(VGA_LIGHT_GRAY, VGA_BLACK);
            } else {
                int status;
                wait(h, &status, 0);
                process_close(h);
            }
        }
    }

    _exit(0);
}