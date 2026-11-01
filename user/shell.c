#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "signal.h"
#include "stdlib.h"
#include <arpa/inet.h>

/* VGA 颜色常量（用户态独立定义） */
#define VGA_BLACK        0
#define VGA_LIGHT_GRAY   7
#define VGA_LIGHT_BLUE   9
#define VGA_LIGHT_GREEN  10
#define VGA_LIGHT_RED    12

#define CMD_MAX          128
#define ARGV_MAX         64

static void set_color(int fg, int bg);
static void expand_buf_reset(void);
static int  expand_argv(char **in, int argc, char **out, int out_max);

extern char **environ;

/* ---------- 词展开：引号 + $VAR + glob ---------- */

#define WORD_MAX  256
#define EXPAND_BUF_SIZE  16384

static char g_expand_buf[EXPAND_BUF_SIZE];
static int  g_expand_pos = 0;

static void expand_buf_reset(void) { g_expand_pos = 0; }

static char *expand_buf_alloc(int size) {
    if (size <= 0 || g_expand_pos + size > EXPAND_BUF_SIZE) return NULL;
    char *p = g_expand_buf + g_expand_pos;
    g_expand_pos += size;
    return p;
}

static int glob_match(const char *pat, const char *str) {
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            while (*str) {
                if (glob_match(pat, str)) return 1;
                str++;
            }
            return 0;
        }
        if (*pat == '?') {
            if (!*str) return 0;
            pat++; str++;
            continue;
        }
        if (*pat != *str) return 0;
        pat++; str++;
    }
    return *str == '\0';
}

static int glob_expand(const char *pattern, char **out, int out_max) {
    char dir[256], pat[256];
    const char *last_slash = NULL;
    for (const char *p = pattern; *p; p++) {
        if (*p == '\\' || *p == '/') last_slash = p;
    }

    if (last_slash) {
        int dlen = last_slash - pattern;
        if (dlen >= (int)sizeof(dir)) return 0;
        if (dlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
        else {
            memcpy(dir, pattern, dlen);
            dir[dlen] = '\0';
        }
        strncpy(pat, last_slash + 1, sizeof(pat) - 1);
        pat[sizeof(pat) - 1] = '\0';
    } else {
        if (getcwd(dir, sizeof(dir)) <= 0) return 0;
        strncpy(pat, pattern, sizeof(pat) - 1);
        pat[sizeof(pat) - 1] = '\0';
    }

    int fd = fs_opendir(dir);
    if (fd < 0) return 0;

    struct dirent ent;
    int n = 0;
    while (fs_readdir(fd, &ent) == 1 && n < out_max) {
        if (ent.name[0] == '.') continue;
        if (!glob_match(pat, ent.name)) continue;

        char full[256];
        int dlen = strlen(dir);
        if (dlen > 0 && (dir[dlen-1] == '/' || dir[dlen-1] == '\\'))
            snprintf(full, sizeof(full), "%s%s", dir, ent.name);
        else
            snprintf(full, sizeof(full), "%s/%s", dir, ent.name);

        char *copy = (char*)malloc(strlen(full) + 1);
        if (!copy) break;
        strcpy(copy, full);
        out[n++] = copy;
    }
    fs_closedir(fd);

    /* 字典序排序（插入排序，n 很小） */
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && strcmp(out[j-1], out[j]) > 0; j--) {
            char *tmp = out[j];
            out[j] = out[j-1];
            out[j-1] = tmp;
        }
    }
    return n;
}

/* 展开单个 token：
 *   '...' → 内容原样
 *   "..." → 内容里的 $VAR 展开，* ? 不展开
 *   其他  → $VAR 展开，* ? 标记为 glob 需后续匹配
 * 引号本身被移除。 */
static int expand_word(const char *in, char *out, int out_size, int *glob_flag) {
    int o = 0;
    int glob = 0;
    int in_single = 0, in_double = 0;
    const char *p = in;

    while (*p) {
        char c = *p;

        if (in_single) {
            if (c == '\'') { in_single = 0; p++; continue; }
            if (o >= out_size - 1) return -1;
            out[o++] = c; p++;
            continue;
        }

        if (in_double) {
            if (c == '"') { in_double = 0; p++; continue; }
            if (c == '\\' && p[1]) {
                char n = p[1];
                if (n == '"' || n == '\\' || n == '$') {
                    if (o >= out_size - 1) return -1;
                    out[o++] = n; p += 2; continue;
                }
            }
            if (c == '$') {
                p++;
                char name[64]; int nl = 0;
                if (*p == '{') {
                    p++;
                    while (*p && *p != '}' && nl < 63) name[nl++] = *p++;
                    if (*p == '}') p++;
                } else {
                    while ((*p >= 'a' && *p <= 'z') ||
                           (*p >= 'A' && *p <= 'Z') ||
                           (*p >= '0' && *p <= '9') || *p == '_') {
                        if (nl < 63) name[nl++] = *p;
                        p++;
                    }
                }
                name[nl] = '\0';
                const char *val = (nl > 0) ? getenv(name) : NULL;
                if (val) {
                    while (*val) {
                        if (o >= out_size - 1) return -1;
                        out[o++] = *val++;
                    }
                }
                continue;
            }
            if (o >= out_size - 1) return -1;
            out[o++] = c; p++;
            continue;
        }

        /* 引号外 */
        if (c == '\'') { in_single = 1; p++; continue; }
        if (c == '"')  { in_double = 1; p++; continue; }
        if (c == '\\' && p[1]) {
            if (o >= out_size - 1) return -1;
            out[o++] = p[1]; p += 2; continue;
        }
        if (c == '$') {
            p++;
            char name[64]; int nl = 0;
            if (*p == '{') {
                p++;
                while (*p && *p != '}' && nl < 63) name[nl++] = *p++;
                if (*p == '}') p++;
            } else {
                while ((*p >= 'a' && *p <= 'z') ||
                       (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_') {
                    if (nl < 63) name[nl++] = *p;
                    p++;
                }
            }
            name[nl] = '\0';
            const char *val = (nl > 0) ? getenv(name) : NULL;
            if (val) {
                while (*val) {
                    if (o >= out_size - 1) return -1;
                    out[o++] = *val++;
                }
            }
            continue;
        }
        if (c == '*' || c == '?') glob = 1;
        if (o >= out_size - 1) return -1;
        out[o++] = c; p++;
    }

    out[o] = '\0';
    *glob_flag = glob;
    return 0;
}

/* 对 argv 整体做展开。返回新的 argc（可能比原 argc 多，因为 glob）。
 * out[] 指向 expand_buf 内的字符串，下一次 expand_argv 前调用 expand_buf_reset 会失效。 */
static int expand_argv(char **in, int argc, char **out, int out_max) {
    int n = 0;
    for (int i = 0; i < argc; i++) {
        char tmp[WORD_MAX];
        int glob = 0;
        if (expand_word(in[i], tmp, sizeof(tmp), &glob) < 0) continue;
        if (tmp[0] == '\0' && !glob) continue;  /* "" 空参数直接丢弃，简化 */

        if (glob) {
            char *matches[32];
            int nm = glob_expand(tmp, matches, 32);
            if (nm == 0) {
                /* 无匹配：保留原 pattern（POSIX sh 行为） */
                if (n < out_max) {
                    out[n] = expand_buf_alloc(strlen(tmp) + 1);
                    if (out[n]) { strcpy(out[n], tmp); n++; }
                }
            } else {
                for (int j = 0; j < nm && n < out_max; j++) {
                    out[n] = expand_buf_alloc(strlen(matches[j]) + 1);
                    if (out[n]) { strcpy(out[n], matches[j]); n++; }
                    free(matches[j]);
                }
            }
        } else {
            if (n < out_max) {
                out[n] = expand_buf_alloc(strlen(tmp) + 1);
                if (out[n]) { strcpy(out[n], tmp); n++; }
            }
        }
    }
    out[n] = NULL;
    return n;
}

/* ---------- 命令历史 ---------- */
#define HIST_MAX  16
static char g_history[HIST_MAX][CMD_MAX];
static int  g_hist_count = 0;

static void add_history(const char *line) {
    if (!line || !line[0]) return;
    if (g_hist_count > 0 &&
        strcmp(g_history[g_hist_count - 1], line) == 0) return;

    if (g_hist_count < HIST_MAX) {
        strncpy(g_history[g_hist_count], line, CMD_MAX - 1);
        g_history[g_hist_count][CMD_MAX - 1] = '\0';
        g_hist_count++;
    } else {
        for (int i = 1; i < HIST_MAX; i++)
            strcpy(g_history[i - 1], g_history[i]);
        strncpy(g_history[HIST_MAX - 1], line, CMD_MAX - 1);
        g_history[HIST_MAX - 1][CMD_MAX - 1] = '\0';
    }
}

/* ---------- 行重绘 ----------
 * 假设光标在行内某处：\r 回行首 → 打 prompt（绿色）→ 打 buf（灰色）
 * → \x1b[K 清到行尾 → 回退 (len - cursor) 个字符到 cursor 处。
 * 一次 write 完成，避免多次系统调用。 */
static void line_redraw(const char *buf, int len, int cursor) {
    char out[1024];
    int o = 0;

    out[o++] = '\r';
    /* 绿色 prompt "\x1b[92m$ \x1b[37m" */
    const char *p = "\x1b[92m$ \x1b[37m";
    while (*p) out[o++] = *p++;

    for (int i = 0; i < len && o < 900; i++) out[o++] = buf[i];
    out[o++] = '\x1b'; out[o++] = '['; out[o++] = 'K';

    int back = len - cursor;
    while (back-- > 0 && o < 1000) {
        out[o++] = '\x1b'; out[o++] = '['; out[o++] = 'D';
    }

    write(1, out, o);
}

/* ---------- 基础输入：读一行，带退格支持 ---------- */

static int read_line(char *buf, int max) {
    int len = 0;
    int cursor = 0;
    int hist_pos = -1;      /* -1 = 不在历史中 */
    char saved[CMD_MAX];    /* 浏览历史时暂存新行 */
    int have_saved = 0;

    buf[0] = '\0';

    /* 打印 prompt */
    set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    write(1, "$ ", 2);
    set_color(VGA_LIGHT_GRAY, VGA_BLACK);

    for (;;) {
        char c;
        if (read(0, &c, 1) != 1) return -1;   /* 连接断了 / pty master 关闭 */

        /* ---------- ESC 序列 ---------- */
        if (c == 0x1B) {
            char seq[4];
            int sl = 0;
            for (int i = 0; i < 3; i++) {
                char x;
                if (read(0, &x, 1) != 1) break;
                seq[sl++] = x;
                if (i == 0 && x != '[' && x != 'O') break;
                if (i >= 1 && ((x >= 'A' && x <= 'Z') || x == '~')) break;
            }
            if (sl >= 2 && (seq[0] == '[' || seq[0] == 'O')) {
                char final = seq[sl - 1];

                if (final == 'A') {              /* 上：历史 */
                    if (g_hist_count == 0) continue;
                    if (hist_pos == -1) {
                        strncpy(saved, buf, CMD_MAX - 1);
                        saved[CMD_MAX - 1] = '\0';
                        have_saved = 1;
                        hist_pos = g_hist_count - 1;
                    } else if (hist_pos > 0) {
                        hist_pos--;
                    } else {
                        continue;
                    }
                    strncpy(buf, g_history[hist_pos], CMD_MAX - 1);
                    buf[CMD_MAX - 1] = '\0';
                    len = strlen(buf);
                    cursor = len;
                    line_redraw(buf, len, cursor);
                } else if (final == 'B') {       /* 下：历史 */
                    if (hist_pos == -1) continue;
                    hist_pos++;
                    if (hist_pos >= g_hist_count) {
                        hist_pos = -1;
                        if (have_saved) {
                            strncpy(buf, saved, CMD_MAX - 1);
                            buf[CMD_MAX - 1] = '\0';
                        } else {
                            buf[0] = '\0';
                        }
                    } else {
                        strncpy(buf, g_history[hist_pos], CMD_MAX - 1);
                        buf[CMD_MAX - 1] = '\0';
                    }
                    len = strlen(buf);
                    cursor = len;
                    line_redraw(buf, len, cursor);
                } else if (final == 'C') {       /* 右 */
                    if (cursor < len) { cursor++; write(1, "\x1b[C", 3); }
                } else if (final == 'D') {       /* 左 */
                    if (cursor > 0) { cursor--; write(1, "\x1b[D", 3); }
                } else if (final == 'H') {       /* Home */
                    while (cursor > 0) { cursor--; write(1, "\x1b[D", 3); }
                } else if (final == 'F') {       /* End */
                    while (cursor < len) { cursor++; write(1, "\x1b[C", 3); }
                } else if (sl >= 3 && seq[1] == '3' && seq[2] == '~') {
                    /* Delete */
                    if (cursor < len) {
                        for (int i = cursor; i < len - 1; i++) buf[i] = buf[i + 1];
                        len--;
                        buf[len] = '\0';
                        line_redraw(buf, len, cursor);
                    }
                }
                continue;
            }
            continue;
        }

        /* ---------- 控制字符 ---------- */
        if (c == '\r' || c == '\n') {
            while (cursor < len) { cursor++; write(1, "\x1b[C", 3); }
            write(1, "\r\n", 2);
            buf[len] = '\0';
            return len;
        }

        if (c == 0x03) {                         /* ^C */
            write(1, "^C\r\n", 4);
            return 0;
        }

        if (c == 0x04) {                         /* ^D */
            if (len == 0) { write(1, "\r\n", 2); return -1; }
            continue;
        }

        if (c == 0x15) {                         /* ^U：清整行 */
            len = 0; cursor = 0; buf[0] = '\0';
            line_redraw(buf, 0, 0);
            continue;
        }

        if (c == 0x7F || c == '\b') {            /* 退格 */
            if (cursor > 0) {
                for (int i = cursor - 1; i < len - 1; i++) buf[i] = buf[i + 1];
                len--;
                cursor--;
                buf[len] = '\0';
                line_redraw(buf, len, cursor);
            }
            continue;
        }

        /* 可打印字符 */
        if (c >= 0x20 && c < 0x7F) {
            if (len >= max - 1) continue;
            for (int i = len; i > cursor; i--) buf[i] = buf[i - 1];
            buf[cursor] = c;
            len++;
            buf[len] = '\0';
            if (cursor == len - 1) {
                write(1, &c, 1);
                cursor++;
            } else {
                cursor++;
                line_redraw(buf, len, cursor);
            }
            continue;
        }
    }
}

/* 原地拆分 argv，保留引号字符（留给 expand_word 判断"引号内不展开"）。
 * 引号内的空格不分割。 */
static int tokenize(char *cmd, char **argv, int max) {
    int argc = 0;
    char *p = cmd;
    char *w = cmd;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc >= max) break;

        argv[argc++] = w;

        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"' || *p == '\'') {
                char q = *p;
                *w++ = *p++;
                while (*p && *p != q) {
                    *w++ = *p++;
                }
                if (*p == q) *w++ = *p++;
            } else {
                *w++ = *p++;
            }
        }

        if (*p) p++;
        *w++ = '\0';
    }

    return argc;
}

/* ---------- 颜色辅助 ---------- */

/* VGA 颜色编号 → ANSI SGR 前景码。
 * VGA 0-7 对应标准色 30-37（按 VGA 色序：黑红绿棕蓝紫青灰），
 * VGA 8-15 对应亮色 90-97。
 * 注：标准 ANSI 31 是红、34 是蓝，VGA 色序里 1 是蓝、4 是红——映射表按 VGA 色序排。 */
static void set_color(int fg, int bg) {
    (void)bg;  /* 背景固定黑，不改 */

    static const int vga_to_sgr[16] = {
        /* 0-7 标准色 */
        30, 34, 32, 36, 31, 35, 33, 37,
        /* 8-15 亮色 */
        90, 94, 92, 96, 91, 95, 93, 97,
    };

    int sgr = (fg >= 0 && fg < 16) ? vga_to_sgr[fg] : 37;
    printf("\033[%dm", sgr);
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
    printf("  ifconfig [ip mask gw]  show or set network\n");
    printf("  dns [ip]               show or set DNS\n");
    printf("  kill <pid>   send SIGTERM to process\n");
    printf("  sink <prog>  run program with stdout discarded\n");
    printf("  <prog>       run program\n");
}

/* 列出目录内容。返回条目数，出错返回负值。 */
static int list_dir(const char *path, int long_fmt) {
    int fd = fs_opendir(path);
    if (fd < 0) return fd;

    struct dirent ent;
    int n;
    int count = 0;
    while ((n = fs_readdir(fd, &ent)) == 1) {
        if (ent.attributes & 0x10) set_color(VGA_LIGHT_BLUE, VGA_BLACK);
        else                       set_color(VGA_LIGHT_GRAY, VGA_BLACK);

        if (long_fmt) {
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

    if (n < 0) return n;
    return count;
}

static void cmd_ls(int argc, char **argv) {
    int long_fmt = 0;
    int argi = 1;

    if (argc >= 2 && strcmp(argv[1], "-l") == 0) {
        long_fmt = 1;
        argi = 2;
    }

    /* 无路径参数：列出当前目录 */
    if (argi >= argc) {
        int cnt = list_dir(".", long_fmt);
        if (cnt < 0) {
            set_color(VGA_LIGHT_RED, VGA_BLACK);
            printf("ls: cannot open '.' (error %d)\n", cnt);
            set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        } else {
            printf("total: %d\n", cnt);
        }
        return;
    }

    int multi = (argc - argi > 1);
    int had_error = 0;

    for (int i = argi; i < argc; i++) {
        const char *path = argv[i];

        /* 先试目录 */
        int d = fs_opendir(path);
        if (d >= 0) {
            fs_closedir(d);
            if (multi) printf("%s:\n", path);
            int cnt = list_dir(path, long_fmt);
            if (cnt < 0) {
                set_color(VGA_LIGHT_RED, VGA_BLACK);
                printf("ls: readdir '%s' failed (error %d)\n", path, cnt);
                set_color(VGA_LIGHT_GRAY, VGA_BLACK);
                had_error = 1;
            } else if (multi) {
                printf("total: %d\n", cnt);
            }
        } else {
            /* 不是目录：当文件处理 */
            int fd = fs_open(path, FS_O_RDONLY);
            if (fd >= 0) {
                if (long_fmt) {
                    struct fstat_buf st;
                    if (fs_fstat(fd, &st) == 0) {
                        printf("%04d-%02d-%02d %02d:%02d  %7u  %s\n",
                               st.mtime.year, st.mtime.month, st.mtime.day,
                               st.mtime.hour, st.mtime.minute,
                               st.size, path);
                    } else {
                        printf("%s\n", path);
                    }
                } else {
                    printf("%s\n", path);
                }
                fs_close(fd);
            } else {
                set_color(VGA_LIGHT_RED, VGA_BLACK);
                printf("ls: cannot access '%s' (error %d)\n", path, fd);
                set_color(VGA_LIGHT_GRAY, VGA_BLACK);
                had_error = 1;
            }
        }
    }

    (void)had_error;
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
    // tty_clear();
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

static void cmd_ifconfig(int argc, char **argv) {
    if (argc == 1) {
        struct net_if_info info;
        if (net_get_if(&info) < 0) {
            set_color(VGA_LIGHT_RED, VGA_BLACK);
            printf("ifconfig: read failed\n");
            set_color(VGA_LIGHT_GRAY, VGA_BLACK);
            return;
        }
        printf("eth0  inet %s\n", inet_ntoa(info.ip));
        printf("      mask %s\n", inet_ntoa(info.mask));
        printf("      gw   %s\n", inet_ntoa(info.gw));
        printf("      mac  %02x:%02x:%02x:%02x:%02x:%02x\n",
               info.mac[0], info.mac[1], info.mac[2],
               info.mac[3], info.mac[4], info.mac[5]);
        return;
    }
    if (argc == 4) {
        uint32_t ip = inet_addr(argv[1]);
        uint32_t mask = inet_addr(argv[2]);
        uint32_t gw = inet_addr(argv[3]);
        if (ip == 0 || mask == 0) {
            set_color(VGA_LIGHT_RED, VGA_BLACK);
            printf("ifconfig: invalid ip/mask\n");
            set_color(VGA_LIGHT_GRAY, VGA_BLACK);
            return;
        }
        if (net_set_if(ip, mask, gw) < 0) {
            set_color(VGA_LIGHT_RED, VGA_BLACK);
            printf("ifconfig: set failed\n");
            set_color(VGA_LIGHT_GRAY, VGA_BLACK);
            return;
        }
        printf("ifconfig: ip=%s mask=%s gw=%s\n",
               argv[1], argv[2], argv[3]);
        return;
    }
    printf("usage: ifconfig                     show\n");
    printf("       ifconfig <ip> <mask> <gw>    set\n");
}

static void cmd_dns(int argc, char **argv) {
    if (argc == 1) {
        uint32_t d = net_get_dns();
        if (d == 0) printf("dns: not set\n");
        else printf("dns: %s\n", inet_ntoa(d));
        return;
    }
    if (argc == 2) {
        uint32_t d = inet_addr(argv[1]);
        if (d == 0) {
            set_color(VGA_LIGHT_RED, VGA_BLACK);
            printf("dns: invalid ip\n");
            set_color(VGA_LIGHT_GRAY, VGA_BLACK);
            return;
        }
        net_set_dns(d);
        printf("dns: set to %s\n", argv[1]);
        return;
    }
    printf("usage: dns [ip]\n");
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
static int split_by_pipe(char **argv, char ***segs, int max) {
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
            .in_pty = -1,
            .out_pty = -1,
            .err_pty = -1,
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

int main(int _argc, char **_argv) {
    (void)_argc; (void)_argv;

    struct sigaction sa = {0};
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa, 0);

    /* 切 pty raw mode：shell 自己做行编辑、回显、信号处理 */
    term_set_raw(1);

    printf("Welcome to HeliumOS Shell!\n");
    printf("Type 'help' for commands.\n\n");

    char cmd[CMD_MAX];
    char *argv[ARGV_MAX];

    while (1) {
        int n = read_line(cmd, sizeof(cmd));
        if (n < 0) break;        /* ^D EOF 或连接断开 */
        if (n == 0) continue;    /* ^C 或空行 */

        add_history(cmd);        /* 记录原始输入（展开前） */

        int argc = tokenize(cmd, argv, ARGV_MAX);
        if (argc == 0) continue;
        argv[argc] = NULL;

        expand_buf_reset();
        char *eargv[ARGV_MAX];
        int eargc = expand_argv(argv, argc, eargv, ARGV_MAX);
        if (eargc == 0) continue;
        for (int i = 0; i < eargc; i++) argv[i] = eargv[i];
        argv[eargc] = NULL;
        argc = eargc;

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
        } else if (strcmp(c, "ifconfig") == 0) {
            cmd_ifconfig(argc, argv);
        } else if (strcmp(c, "dns") == 0) {
            cmd_dns(argc, argv);
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
                .in_pty = -1,
                .out_pty = -1,
                .err_pty = -1,
                .flags  = 0,
                .envp   = (unsigned int)environ,
            };
            if (argc < 2) {
                printf("usage: sink <prog>\n");
            } else {
                int h = spawn(argv[1], argv + 1, &p);
                if (h >= 0) {
                    int st; wait(h, &st, 0);
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
                struct spawn_params sp = {
                    .size = sizeof(sp),
                    .in_fd = SPAWN_FD_INHERIT,
                    .out_fd = SPAWN_FD_INHERIT,
                    .err_fd = SPAWN_FD_INHERIT,
                    .in_pipe = -1, .out_pipe = -1, .err_pipe = -1,
                    .in_pty = -1, .out_pty = -1, .err_pty = -1,
                    .flags = 0,
                    .envp = (uint32_t)environ,
                };
                int h = spawn(c, argv, &sp);
                if (h < 0) {
                    set_color(VGA_LIGHT_RED, VGA_BLACK);
                    printf("spawn '%s' failed (error %d)\n", c, h);
                    set_color(VGA_LIGHT_GRAY, VGA_BLACK);
                } else {
                    /* 切换 pty 前台到子进程。spawn_params 的 out_pid 由内核回填 */
                    if (sp.out_pid > 0) pty_set_fg(-1, sp.out_pid);

                    int status = 0;
                    int wr = wait(h, &status, 0);

                    /* 子进程退出或自己被其他信号打断后，恢复前台为 shell */
                    pty_set_fg(-1, getpid());

                    if (wr == 0 && status != 0) {
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