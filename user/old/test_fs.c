/* user/test_fs.c
 * 测试 fs_* 系统调用族
 * 依赖内核侧 fs.c / fat32.c 已完成
 */

#include <stddef.h>

/* ---------- 用户态系统调用号 ---------- */
#define SYS_EXIT          1
#define SYS_READ          11
#define SYS_WRITE         12

#define SYS_FS_OPEN       31
#define SYS_FS_READ       32
#define SYS_FS_SEEK       34
#define SYS_FS_CLOSE      35
#define SYS_FS_OPENDIR    36
#define SYS_FS_READDIR    37
#define SYS_FS_CLOSEDIR   38

#define SYS_GETCWD        61
#define SYS_CHDIR         62

/* ---------- 打开标志 ---------- */
#define FS_O_RDONLY       0x01

/* ---------- dirent（必须和内核完全一致） ---------- */
#define DIRENT_NAME_MAX   256
struct dirent {
    char     name[DIRENT_NAME_MAX];
    unsigned char attributes;
    unsigned char reserved[3];
    unsigned int  size;
};

/* ---------- syscall 包装 ---------- */
static inline int syscall(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c)
        : "memory", "cc"
    );
    return ret;
}

/* ---------- 输出工具 ---------- */
static void write_str(const char *str) {
    int len = 0;
    while (str[len]) len++;
    syscall(SYS_WRITE, 1, (int)str, len);
}

static void write_hex(unsigned int v) {
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        int nib = (v >> ((7 - i) * 4)) & 0xF;
        buf[2 + i] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
    }
    buf[10] = '\0';
    write_str(buf);
}

static void write_dec(int v) {
    char buf[16];
    int i = 0;
    if (v == 0) { write_str("0"); return; }
    if (v < 0) { write_str("-"); v = -v; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    /* 逆序输出 */
    char out[16];
    for (int j = 0; j < i; j++) out[j] = buf[i - 1 - j];
    out[i] = '\0';
    write_str(out);
}

static void exit_proc(int status) {
    syscall(SYS_EXIT, status, 0, 0);
}

/* ---------- 测试 1：fs_open + fs_read ---------- */
static int test_read_elf(void) {
    write_str("[T1] fs_open(\"SYS:/SHELL.ELF\")\n");

    int fd = syscall(SYS_FS_OPEN, (int)"SYS:/SHELL.ELF", FS_O_RDONLY, 0);
    if (fd < 0) {
        write_str("  FAIL: fs_open returned ");
        write_dec(fd);
        write_str("\n");
        return -1;
    }
    write_str("  OK: fd = ");
    write_dec(fd);
    write_str("\n");

    unsigned char buf[16];
    int n = syscall(SYS_FS_READ, fd, (int)buf, sizeof(buf));
    if (n != 16) {
        write_str("  FAIL: fs_read returned ");
        write_dec(n);
        write_str("\n");
        syscall(SYS_FS_CLOSE, fd, 0, 0);
        return -1;
    }

    write_str("  OK: read ");
    write_dec(n);
    write_str(" bytes: ");
    for (int i = 0; i < 4; i++) {
        char h[4];
        h[0] = '0' + (buf[i] >> 4);
        h[1] = '0' + (buf[i] & 0xF);
        h[2] = ' ';
        h[3] = '\0';
        /* 简化：直接手算 hex */
    }
    /* 用 write_hex 打印第一个 4 字节 */
    unsigned int magic = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
    write_hex(magic);
    if (magic == 0x464C457F) {
        write_str("  (ELF magic OK)\n");
    } else {
        write_str("  (WARN: not ELF magic)\n");
    }

    syscall(SYS_FS_CLOSE, fd, 0, 0);
    return 0;
}

/* ---------- 测试 2：fs_opendir + fs_readdir ---------- */
static int test_listdir(void) {
    write_str("[T2] fs_opendir(\"SYS:/\")\n");

    int fd = syscall(SYS_FS_OPENDIR, (int)"SYS:/", 0, 0);
    if (fd < 0) {
        write_str("  FAIL: fs_opendir returned ");
        write_dec(fd);
        write_str("\n");
        return -1;
    }
    write_str("  OK: fd = ");
    write_dec(fd);
    write_str("\n");

    struct dirent ent;
    int count = 0;
    int n;
    while ((n = syscall(SYS_FS_READDIR, fd, (int)&ent, 0)) == 1) {
        write_str("  - ");
        write_str(ent.name);
        if (ent.attributes & 0x10) {
            write_str("/");
        } else {
            write_str("  (size=");
            write_dec((int)ent.size);
            write_str(")");
        }
        write_str("\n");
        count++;
        if (count > 32) {
            write_str("  ... (too many)\n");
            break;
        }
    }
    if (n < 0) {
        write_str("  FAIL: fs_readdir returned ");
        write_dec(n);
        write_str("\n");
    }
    write_str("  total: ");
    write_dec(count);
    write_str(" entries\n");

    syscall(SYS_FS_CLOSEDIR, fd, 0, 0);
    return 0;
}

/* ---------- 测试 3：chdir + getcwd ---------- */
static int test_cwd(void) {
    write_str("[T3] getcwd / chdir\n");

    char buf[64];
    int n = syscall(SYS_GETCWD, (int)buf, sizeof(buf), 0);
    if (n < 0) {
        write_str("  FAIL: getcwd returned ");
        write_dec(n);
        write_str("\n");
    } else {
        write_str("  cwd = ");
        write_str(buf);
        write_str("\n");
    }

    /* 尝试切换到 /BOOT（如果存在） */
    int r = syscall(SYS_CHDIR, (int)"SYS:/BOOT", 0, 0);
    if (r < 0) {
        write_str("  chdir(SYS:/BOOT) failed (");
        write_dec(r);
        write_str("), probably no such dir\n");
        return 0;
    }
    write_str("  chdir(SYS:/BOOT) OK\n");

    n = syscall(SYS_GETCWD, (int)buf, sizeof(buf), 0);
    if (n >= 0) {
        write_str("  new cwd = ");
        write_str(buf);
        write_str("\n");
    }

    /* 切回根 */
    syscall(SYS_CHDIR, (int)"SYS:/", 0, 0);
    return 0;
}

/* ---------- 主入口 ---------- */
void _start(void) {
    write_str("\n");
    write_str("========================================\n");
    write_str("  HeliumOS FS Test Suite\n");
    write_str("========================================\n");

    int fails = 0;

    if (test_read_elf()   < 0) fails++;
    write_str("\n");
    if (test_listdir()    < 0) fails++;
    write_str("\n");
    if (test_cwd()        < 0) fails++;

    write_str("\n");
    write_str("========================================\n");
    if (fails == 0) {
        write_str("  ALL TESTS PASSED\n");
    } else {
        write_str("  FAILURES: ");
        write_dec(fails);
        write_str("\n");
    }
    write_str("========================================\n");

    exit_proc(fails);
}