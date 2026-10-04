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
#define ARGV_MAX         16

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
    printf("  ls [-l] [path]  list directory\n");
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
    printf("  ps           list processes\n");
    printf("  uname        system information\n");
    printf("  kill <pid>   send SIGTERM to process\n");
    printf("  sink <prog>  run program with stdout discarded\n");
    printf("  <prog>       run program\n");
}

static void cmd_ls(int argc, char **argv) {
    int long_fmt = 0;
    const char *path = ".";
    int argi = 1;

    if (argc >= 2 && strcmp(argv[1], "-l") == 0) {
        long_fmt = 1;
        argi = 2;
    }
    if (argc > argi) path = argv[argi];

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
        if (ent.attributes & 0x10) set_color(VGA_LIGHT_BLUE, VGA_BLACK);
        else                       set_color(VGA_LIGHT_GRAY, VGA_BLACK);

        if (long_fmt) {
            /* 解析 FAT32 日期/时间（dir 返回原始格式） */
            uint16_t d = ent.mod_date;
            uint16_t t = ent.mod_time;
            int year  = 1980 + ((d >> 9) & 0x7F);
            int month = (d >> 5) & 0x0F;
            int day   = d & 0x1F;
            int hour  = (t >> 11) & 0x1F;
            int min   = (t >> 5) & 0x3F;

            if (ent.attributes & 0x10) {
                printf("%04d-%02d-%02d %02d:%02d    <DIR>  %s/\n",
                       year, month, day, hour, min, ent.name);
            } else {
                printf("%04d-%02d-%02d %02d:%02d  %7u  %s\n",
                       year, month, day, hour, min, ent.size, ent.name);
            }
        } else {
            if (ent.attributes & 0x10) printf("%s/\n", ent.name);
            else                       printf("%s\n", ent.name);
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

static void cmd_kill(int argc, char **argv) {
    if (argc < 2) { printf("usage: kill <pid>\n"); return; }

    unsigned int pid = (unsigned int)atoi(argv[1]);
    if (pid == 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("kill: invalid pid\n");
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }

    int h = proc_open(pid, PROC_TERMINATE);
    if (h < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("kill: pid %u: error %d\n", pid, h);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }

    int r = kill(h, SIGTERM);
    process_close(h);

    if (r < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("kill: error %d\n", r);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}

/* ---------- 管道 / 重定向 ---------- */

#define PIPE_MAX_SEGS 8

struct redirect {
    const char *in_file;
    const char *out_file;
};

/* 删除 argv[start..start+count-1]。argv 原地修改。 */
static void remove_argv_range(char **argv, int start, int count) {
    int i = start;
    while (argv[i + count]) {
        argv[i] = argv[i + count];
        i++;
    }
    argv[i] = NULL;
}

/* 解析并摘除 argv 里的 < file 和 > file。
 * argv 原地修改。返回 0 成功，-1 语法错误。 */
static int parse_redirect(char **argv, struct redirect *r) {
    r->in_file = NULL;
    r->out_file = NULL;

    int i = 0;
    while (argv[i]) {
        if (strcmp(argv[i], "<") == 0) {
            if (!argv[i + 1] || r->in_file) return -1;
            r->in_file = argv[i + 1];
            remove_argv_range(argv, i, 2);
        } else if (strcmp(argv[i], ">") == 0) {
            if (!argv[i + 1] || r->out_file) return -1;
            r->out_file = argv[i + 1];
            remove_argv_range(argv, i, 2);
        } else {
            i++;
        }
    }
    return 0;
}

/* 按 | 分割 argv。argv 原地修改：| 变成 NULL 终结符。
 * 返回段数（>=1），超限返回 -1。 */
static int split_by_pipe(char **argv, char **segs, int max) {
    if (max < 1) return -1;
    int n = 1;
    segs[0] = argv;

    for (int i = 0; argv[i]; i++) {
        if (strcmp(argv[i], "|") == 0) {
            argv[i] = NULL;
            if (n >= max) return -1;
            segs[n++] = &argv[i + 1];
        }
    }
    return n;
}

/* 执行管道 + 重定向。argv 会被原地修改。 */
static void run_pipeline(char **argv) {
    char **segs[PIPE_MAX_SEGS];
    int nseg = split_by_pipe(argv, segs, PIPE_MAX_SEGS);
    if (nseg <= 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("syntax error: too many pipes\n");
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }

    struct redirect redirs[PIPE_MAX_SEGS];
    for (int i = 0; i < nseg; i++) {
        if (parse_redirect(segs[i], &redirs[i]) < 0) {
            set_color(VGA_LIGHT_RED, VGA_BLACK);
            printf("syntax error in segment %d\n", i + 1);
            set_color(VGA_LIGHT_GRAY, VGA_BLACK);
            return;
        }
        if (!segs[i][0]) {
            set_color(VGA_LIGHT_RED, VGA_BLACK);
            printf("syntax error: empty command\n");
            set_color(VGA_LIGHT_GRAY, VGA_BLACK);
            return;
        }
    }

    /* 创建 pipe（nseg - 1 个） */
    int pipes[PIPE_MAX_SEGS - 1][2];
    int npipe = nseg - 1;
    int made = 0;
    for (int i = 0; i < npipe; i++) {
        if (pipe(pipes[i]) < 0) {
            set_color(VGA_LIGHT_RED, VGA_BLACK);
            printf("pipe failed\n");
            set_color(VGA_LIGHT_GRAY, VGA_BLACK);
            for (int j = 0; j < made; j++) {
                ipc_close(pipes[j][0]);
                ipc_close(pipes[j][1]);
            }
            return;
        }
        made++;
    }

    /* 逐段 spawn */
    int handles[PIPE_MAX_SEGS];
    int open_in[PIPE_MAX_SEGS];
    int open_out[PIPE_MAX_SEGS];
    for (int i = 0; i < nseg; i++) {
        handles[i] = -1;
        open_in[i] = -1;
        open_out[i] = -1;
    }

    int failed = 0;
    for (int i = 0; i < nseg; i++) {
        int in_fd = SPAWN_FD_INHERIT;
        int out_fd = SPAWN_FD_INHERIT;

        if (redirs[i].in_file) {
            in_fd = fs_open(redirs[i].in_file, FS_O_RDONLY);
            if (in_fd < 0) {
                set_color(VGA_LIGHT_RED, VGA_BLACK);
                printf("%s: error %d\n", redirs[i].in_file, in_fd);
                set_color(VGA_LIGHT_GRAY, VGA_BLACK);
                failed = 1;
                break;
            }
            open_in[i] = in_fd;
        }
        if (redirs[i].out_file) {
            out_fd = fs_open(redirs[i].out_file,
                             FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC);
            if (out_fd < 0) {
                set_color(VGA_LIGHT_RED, VGA_BLACK);
                printf("%s: error %d\n", redirs[i].out_file, out_fd);
                set_color(VGA_LIGHT_GRAY, VGA_BLACK);
                failed = 1;
                break;
            }
            open_out[i] = out_fd;
        }

        int in_pipe  = (i > 0)    ? pipes[i - 1][0] : -1;
        int out_pipe = (i < npipe) ? pipes[i][1]     : -1;

        struct spawn_params p = {
            .size = sizeof(p),
            .in_fd = in_fd,
            .out_fd = out_fd,
            .err_fd = SPAWN_FD_INHERIT,
            .in_pipe = in_pipe,
            .out_pipe = out_pipe,
            .err_pipe = -1,
            .flags = 0,
            .envp = (uint32_t)environ,
        };

        handles[i] = spawn(segs[i][0], segs[i], &p);
        if (handles[i] < 0) {
            set_color(VGA_LIGHT_RED, VGA_BLACK);
            printf("spawn '%s' failed (error %d)\n", segs[i][0], handles[i]);
            set_color(VGA_LIGHT_GRAY, VGA_BLACK);
            failed = 1;
            break;
        }
    }

    /* 关 shell 侧的文件 fd */
    for (int i = 0; i < nseg; i++) {
        if (open_in[i]  >= 0) fs_close(open_in[i]);
        if (open_out[i] >= 0) fs_close(open_out[i]);
    }

    /* 关 shell 侧的 pipe handle */
    for (int i = 0; i < npipe; i++) {
        ipc_close(pipes[i][0]);
        ipc_close(pipes[i][1]);
    }

    /* wait 所有已 spawn 的子进程 */
    for (int i = 0; i < nseg; i++) {
        if (handles[i] < 0) continue;
        int st = 0;
        wait(handles[i], &st, 0);
        process_close(handles[i]);
    }

    (void)failed;
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
static void cmd_ps(void) {
    unsigned int pids[32];
    int n = proc_list(pids, 32);
    if (n < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("ps: error %d\n", n);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }
    printf("PID\n");
    for (int i = 0; i < n; i++) {
        printf("%u\n", pids[i]);
    }
    printf("total: %d\n", n);
}

static void cmd_uname(void) {
    struct uname_buf u;
    int r = sys_uname(&u);
    if (r < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("uname: error %d\n", r);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }
    u.sysname[15] = '\0';
    u.release[15] = '\0';
    u.machine[15] = '\0';
    printf("%s %s %s\n", u.sysname, u.release, u.machine);
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
            cmd_reboot();
        } else if (strcmp(c, "ps") == 0) {
            cmd_ps();
        } else if (strcmp(c, "uname") == 0) {
            cmd_uname();
        } else if (strcmp(c, "kill") == 0) {
            cmd_kill(argc, argv);
        } else if (strcmp(c, "sink") == 0) {
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
            /* 检测是否有 | < > */
            int has_special = 0;
            for (int i = 0; i < argc; i++) {
                if (strcmp(argv[i], "|") == 0 ||
                    strcmp(argv[i], "<") == 0 ||
                    strcmp(argv[i], ">") == 0) {
                    has_special = 1;
                    break;
                }
            }

            if (has_special) {
                run_pipeline(argv);
            } else {
                /* 普通 spawn */
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