#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "signal.h"
#include "stdlib.h"

/* VGA 颜色常量（用户态独立定义） */
#define VGA_BLACK        0
#define VGA_LIGHT_GRAY   7
#define VGA_LIGHT_BLUE   9
#define VGA_LIGHT_GREEN  10
#define VGA_LIGHT_RED    12

#define CMD_MAX          128
#define ARGV_MAX         8

extern char **environ;

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
    tty_set_color(fg, bg);
}

/* ---------- 内置命令 ---------- */

static void cmd_help(void) {
    printf("HeliumOS Shell commands:\n");
    printf("  ls [path]    list directory\n");
    printf("  cd [path]    change directory (no arg: show cwd)\n");
    printf("  pwd          print working directory\n");
    printf("  clear        clear screen\n");
    printf("  help         show this help\n");
    printf("  exit         exit shell\n");
    printf("  touch <file> create empty file\n");
    printf("  mkdir <dir>  create directory\n");
    printf("  rmdir <dir>  remove empty directory\n");
    printf("  rm <file>    remove file\n");
    printf("  mv <a> <b>   rename / move\n");
    printf("  cp <a> <b>   copy file\n");
    printf("  date         show current date/time\n");
    printf("  halt         power off\n");
    printf("  reboot       restart\n");
    printf("  sink <prog>  run program with stdout discarded\n");
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

static void cmd_clear(void) {
    tty_clear();
}

static void cmd_touch(int argc, char **argv) {
    if (argc < 2) { printf("usage: touch <file>\n"); return; }
    int fd = fs_open(argv[1], FS_O_WRONLY | FS_O_CREAT);
    if (fd < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("touch: %s: error %d\n", argv[1], fd);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }
    fs_close(fd);
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { printf("usage: mkdir <dir>\n"); return; }
    int r = fs_mkdir(argv[1]);
    if (r < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("mkdir: %s: error %d\n", argv[1], r);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}

static void cmd_rmdir(int argc, char **argv) {
    if (argc < 2) { printf("usage: rmdir <dir>\n"); return; }
    int r = fs_rmdir(argv[1]);
    if (r < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("rmdir: %s: error %d\n", argv[1], r);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) { printf("usage: rm <file>\n"); return; }
    int r = fs_unlink(argv[1]);
    if (r < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("rm: %s: error %d\n", argv[1], r);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}

static void cmd_mv(int argc, char **argv) {
    if (argc < 3) { printf("usage: mv <src> <dst>\n"); return; }
    int r = fs_rename(argv[1], argv[2]);
    if (r < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("mv: %s -> %s: error %d\n", argv[1], argv[2], r);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}

static void cmd_cp(int argc, char **argv) {
    if (argc < 3) { printf("usage: cp <src> <dst>\n"); return; }

    int in = fs_open(argv[1], FS_O_RDONLY);
    if (in < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("cp: %s: error %d\n", argv[1], in);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }

    int out = fs_open(argv[2], FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC);
    if (out < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("cp: %s: error %d\n", argv[2], out);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        fs_close(in);
        return;
    }

    char buf[512];
    int n;
    int err = 0;
    while ((n = fs_read(in, buf, sizeof(buf))) > 0) {
        int w = fs_write(out, buf, n);
        if (w != n) { err = (w < 0) ? w : -1; break; }
    }
    if (n < 0) err = n;

    fs_close(in);
    fs_close(out);

    if (err) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("cp: error %d\n", err);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}

static void cmd_date(void) {
    struct rtc_time t;
    int r = rtc_get_time(&t);
    if (r < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("date: error %d\n", r);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }
    /* 2026-05-15 14:30:00 Sun */
    static const char *wd[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    const char *w = (t.weekday < 7) ? wd[t.weekday] : "???";
    printf("%d-%d-%d %d:%d:%d %s\n",
           t.year, t.month, t.day, t.hour, t.minute, t.second, w);
}

static void cmd_halt(void) {
    set_color(VGA_LIGHT_RED, VGA_BLACK);
    printf("System halting...\n");
    set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    int r = sys_halt();
    if (r < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("halt: error %d\n", r);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}

static void cmd_reboot(void) {
    set_color(VGA_LIGHT_RED, VGA_BLACK);
    printf("System rebooting...\n");
    set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    int r = sys_reboot();
    if (r < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("reboot: error %d\n", r);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}

/* ---------- 主循环 ---------- */

void main(int _argc, char **_argv) {
    (void)_argc; (void)_argv;

    /* shell 忽略 SIGINT —— 按 Ctrl+C 不杀 shell */
    struct sigaction sa = {0};
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa, 0);

    printf("Welcome to HeliumOS Shell!\n");
    printf("Type 'help' for commands.\n\n");

    char cmd[CMD_MAX];
    char *argv[ARGV_MAX];

    while (1) {
        set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        printf("$ ");
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);

        int n = read_line(cmd, sizeof(cmd));
        if (n == 0) continue;

        int argc = tokenize(cmd, argv, ARGV_MAX);
        if (argc == 0) continue;
        argv[argc] = NULL;

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
        } else if (strcmp(c, "clear") == 0) {
            cmd_clear();
        } else if (strcmp(c, "touch") == 0) {
            cmd_touch(argc, argv);
        } else if (strcmp(c, "mkdir") == 0) {
            cmd_mkdir(argc, argv);
        } else if (strcmp(c, "rmdir") == 0) {
            cmd_rmdir(argc, argv);
        } else if (strcmp(c, "rm") == 0) {
            cmd_rm(argc, argv);
        } else if (strcmp(c, "mv") == 0) {
            cmd_mv(argc, argv);
        } else if (strcmp(c, "cp") == 0) {
            cmd_cp(argc, argv);
        } else if (strcmp(c, "date") == 0) {
            cmd_date();
        } else if (strcmp(c, "halt") == 0) {
            cmd_halt();
        } else if (strcmp(c, "reboot") == 0) {
            cmd_reboot();} else if (strcmp(c, "sink") == 0) {
            struct spawn_params p = {
                .size   = sizeof(p),
                .in_fd  = SPAWN_FD_INHERIT,
                .out_fd = SPAWN_FD_NULL,
                .err_fd = SPAWN_FD_INHERIT,
                .in_pipe  = -1,
                .out_pipe = -1,
                .err_pipe = -1,
                .flags  = 0,
                .envp   = (unsigned int)environ,
            };
            if (argc < 2) {
                printf("usage: sink <prog>\n");
            } else {
                int h = spawn(argv[1], argv + 1, &p);
                if (h >= 0) {
                    tty_set_foreground(h);
                    int st; wait(h, &st, 0);
                    tty_set_foreground(-1);
                    process_close(h);
                } else {
                    set_color(VGA_LIGHT_RED, VGA_BLACK);
                    printf("spawn '%s' failed (error %d)\n", argv[1], h);
                    set_color(VGA_LIGHT_GRAY, VGA_BLACK);
                }
            }
        } else {
            /* 找 | */
            int pipe_idx = -1;
            for (int i = 0; i < argc; i++) {
                if (strcmp(argv[i], "|") == 0) {
                    pipe_idx = i;
                    break;
                }
            }

            if (pipe_idx > 0 && pipe_idx < argc - 1) {
                int fds[2];
                if (pipe(fds) < 0) {
                    set_color(VGA_LIGHT_RED, VGA_BLACK);
                    printf("pipe failed\n");
                    set_color(VGA_LIGHT_GRAY, VGA_BLACK);
                } else {
                    argv[pipe_idx] = NULL;

                    struct spawn_params p1 = {
                        .size = sizeof(p1),
                        .in_fd  = SPAWN_FD_INHERIT,
                        .out_fd = SPAWN_FD_INHERIT,
                        .err_fd = SPAWN_FD_INHERIT,
                        .in_pipe  = -1,
                        .out_pipe = fds[1],
                        .err_pipe = -1,
                        .flags = 0,
                        .envp = (uint32_t)environ,
                    };
                    int h1 = spawn(argv[0], &argv[0], &p1);

                    struct spawn_params p2 = {
                        .size = sizeof(p2),
                        .in_fd  = SPAWN_FD_INHERIT,
                        .out_fd = SPAWN_FD_INHERIT,
                        .err_fd = SPAWN_FD_INHERIT,
                        .in_pipe  = fds[0],
                        .out_pipe = -1,
                        .err_pipe = -1,
                        .flags = 0,
                        .envp = (uint32_t)environ,
                    };
                    int h2 = spawn(argv[pipe_idx + 1], &argv[pipe_idx + 1], &p2);

                    ipc_close(fds[0]);
                    ipc_close(fds[1]);

                    if (h1 >= 0) {
                        int st1; wait(h1, &st1, 0); process_close(h1);
                    }
                    if (h2 >= 0) {
                        int st2; wait(h2, &st2, 0); process_close(h2);
                    }
                }
            } else if (pipe_idx >= 0) {
                set_color(VGA_LIGHT_RED, VGA_BLACK);
                printf("syntax error: empty pipe\n");
                set_color(VGA_LIGHT_GRAY, VGA_BLACK);
            } else {
                int h = spawn(c, argv, NULL);
                if (h < 0) {
                    set_color(VGA_LIGHT_RED, VGA_BLACK);
                    printf("spawn '%s' failed (error %d)\n", c, h);
                    set_color(VGA_LIGHT_GRAY, VGA_BLACK);
                } else {
                    tty_set_foreground(h);
                    int status = 0;
                    wait(h, &status, 0);
                    tty_set_foreground(-1);
                    if (status != 0) {
                        set_color(VGA_LIGHT_RED, VGA_BLACK);
                        printf("[shell] child exited with status %d\n", status);
                        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
                    }
                    process_close(h);
                }
            }
        }
    }

    return 0;
}