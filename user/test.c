/* user/test.c
 * HeliumOS 综合自测程序
 *
 * 当前 spawn 不支持传参，所以只跑全部。
 * 将来加 argv 支持后，可按模块选择。
 */

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "errno.h"

/* VGA 颜色 */
#define C_BLACK   0
#define C_BLUE    9
#define C_GREEN   10
#define C_RED     12
#define C_YELLOW  14
#define C_GRAY    7

/* ---------- 测试计数器 ---------- */
static int g_pass = 0;
static int g_fail = 0;

/* ---------- 报告辅助 ---------- */

static void ok(const char *name) {
    console_set_color(C_GREEN, C_BLACK);
    printf("  [PASS] %s\n", name);
    console_set_color(C_GRAY, C_BLACK);
    g_pass++;
}

static void fail(const char *name, const char *why) {
    console_set_color(C_RED, C_BLACK);
    printf("  [FAIL] %s: %s\n", name, why);
    console_set_color(C_GRAY, C_BLACK);
    g_fail++;
}

static void check(int cond, const char *name) {
    if (cond) ok(name);
    else      fail(name, "condition false");
}

static void check_eq(int got, int expected, const char *name) {
    if (got == expected) {
        ok(name);
    } else {
        char buf[80];
        snprintf(buf, sizeof(buf), "got %d, expected %d", got, expected);
        fail(name, buf);
    }
}

/* 打印分段标题 */
static void section(const char *title) {
    printf("\n");
    console_set_color(C_BLUE, C_BLACK);
    printf("--- %s ---\n", title);
    console_set_color(C_GRAY, C_BLACK);
}

/* ---------- 1. 字符串库 ---------- */

static void test_strings(void) {
    section("strings");

    char buf[32];

    /* strcpy + strlen */
    strcpy(buf, "hello");
    check_eq((int)strlen(buf), 5, "strlen(\"hello\")");

    /* strcmp */
    check(strcmp(buf, "hello") == 0,  "strcmp equal");
    check(strcmp(buf, "world") != 0,  "strcmp not equal");
    check(strcmp("a", "b") < 0,       "strcmp less");

    /* memset */
    memset(buf, 'A', 5);
    buf[5] = '\0';
    check(buf[0] == 'A' && buf[4] == 'A', "memset");
    check_eq((int)strlen(buf), 5, "memset length");

    /* memcpy */
    char src[8] = "abcdef";
    char dst[8];
    memcpy(dst, src, 7);
    check(strcmp(dst, "abcdef") == 0, "memcpy");

    /* memcmp */
    check(memcmp("abc", "abc", 3) == 0, "memcmp equal");
    check(memcmp("abc", "abd", 3) != 0, "memcmp not equal");

    /* strncpy */
    char tmp[8];
    strncpy(tmp, "xyz", 5);
    check(strcmp(tmp, "xyz") == 0, "strncpy");

    /* strncmp */
    check(strncmp("hello", "help", 3) == 0, "strncmp partial eq");
}

/* ---------- 2. printf 格式化 ---------- */

static void test_printf(void) {
    section("printf / snprintf");

    char buf[64];

    /* 整数 */
    snprintf(buf, sizeof(buf), "%d", -12345);
    check(strcmp(buf, "-12345") == 0, "%d negative");

    snprintf(buf, sizeof(buf), "%u", 4000000000u);
    check(strcmp(buf, "4000000000") == 0, "%u large");

    /* 16 进制 */
    snprintf(buf, sizeof(buf), "%x", 0xdeadbeef);
    check(strcmp(buf, "deadbeef") == 0, "%x lower");
    snprintf(buf, sizeof(buf), "%X", 0xcafe);
    check(strcmp(buf, "CAFE") == 0, "%X upper");

    /* 字符和字符串 */
    snprintf(buf, sizeof(buf), "%c%c%c", 'a', 'b', 'c');
    check(strcmp(buf, "abc") == 0, "%c triple");

    snprintf(buf, sizeof(buf), "[%s]", "mid");
    check(strcmp(buf, "[mid]") == 0, "%s embedded");

    /* 多参数混合 */
    snprintf(buf, sizeof(buf), "%d/%u/%x", -7, 42u, 0x1f);
    check(strcmp(buf, "-7/42/1f") == 0, "mixed %d %u %x");

    /* %% */
    snprintf(buf, sizeof(buf), "100%%");
    check(strcmp(buf, "100%") == 0, "%% escape");

    /* 截断 */
    snprintf(buf, 4, "abcdef");
    check(strcmp(buf, "abc") == 0, "snprintf truncate");

    /* NULL 字符串 */
    snprintf(buf, sizeof(buf), "%s", (const char*)0);
    check(strcmp(buf, "(null)") == 0, "%s NULL");
}

/* ---------- 3. 内存管理 ---------- */

static void test_mem(void) {
    section("memory / malloc");

    /* brk 查询 */
    int old_brk = brk(0);
    check(old_brk > 0, "brk(0) query");

    /* malloc + write + free */
    unsigned char *p = (unsigned char*)malloc(64);
    check(p != NULL, "malloc(64)");
    if (p) {
        memset(p, 0xAB, 64);
        check(p[0] == 0xAB && p[63] == 0xAB, "malloc write pattern");
        free(p);
    }

    /* calloc 全零 */
    unsigned char *q = (unsigned char*)calloc(16, 4);
    check(q != NULL, "calloc(16, 4)");
    if (q) {
        int all_zero = 1;
        for (int i = 0; i < 64; i++) if (q[i]) all_zero = 0;
        check(all_zero, "calloc zeroed");
        free(q);
    }

    /* realloc 扩展 */
    int *arr = (int*)malloc(4 * sizeof(int));
    check(arr != NULL, "malloc for realloc");
    if (arr) {
        arr[0] = 10; arr[1] = 20; arr[2] = 30; arr[3] = 40;
        arr = (int*)realloc(arr, 8 * sizeof(int));
        check(arr != NULL, "realloc grow");
        if (arr) {
            check(arr[0] == 10 && arr[3] == 40, "realloc preserves");
            arr[7] = 70;
            check(arr[7] == 70, "realloc new space");
            free(arr);
        }
    }

    /* 大量分配 + 完整性 */
    void *slots[32];
    int all_ok = 1;
    for (int i = 0; i < 32; i++) {
        slots[i] = malloc(48);
        if (!slots[i]) { all_ok = 0; break; }
        ((int*)slots[i])[0] = i;
        ((int*)slots[i])[11] = i * 7;   /* 48 bytes = 12 ints */
    }
    check(all_ok, "malloc x32");

    if (all_ok) {
        for (int i = 0; i < 32; i++) {
            if (((int*)slots[i])[0] != i || ((int*)slots[i])[11] != i * 7) {
                all_ok = 0;
                break;
            }
        }
        check(all_ok, "malloc data integrity");
        for (int i = 0; i < 32; i++) free(slots[i]);
    }

    /* malloc(0) 返回 NULL */
    check(malloc(0) == NULL, "malloc(0) -> NULL");

    /* 释放 NULL 无副作用 */
    free(NULL);
    ok("free(NULL) safe");
}

/* ---------- 4. 文件系统 ---------- */

static void test_fs(void) {
    section("filesystem");

    /* 1. 打开已知文件 */
    int fd = fs_open("SYS:/SHELL.ELF", FS_O_RDONLY);
    check(fd >= 0, "fs_open SYS:/SHELL.ELF");

    if (fd >= 0) {
        /* ELF magic */
        unsigned char buf[4];
        int n = fs_read(fd, buf, 4);
        check_eq(n, 4, "fs_read 4 bytes");

        unsigned int magic = (unsigned)buf[0]
                           | ((unsigned)buf[1] << 8)
                           | ((unsigned)buf[2] << 16)
                           | ((unsigned)buf[3] << 24);
        check(magic == 0x464C457F, "ELF magic 0x464C457F");

        /* seek 回 0 再读 */
        check_eq(fs_seek(fd, 0), 0, "fs_seek to 0");
        n = fs_read(fd, buf, 4);
        check_eq(n, 4, "fs_read after seek");

        /* 读超过剩余长度 -> 截断 */
        fs_seek(fd, 0);
        char big[256];
        n = fs_read(fd, big, sizeof(big));
        check(n > 0 && n <= (int)sizeof(big), "fs_read truncate to file size");

        fs_close(fd);
    }

    /* 2. 打开不存在的文件 */
    fd = fs_open("SYS:/NOT_EXIST_12345.TXT", FS_O_RDONLY);
    check(fd < 0, "fs_open nonexistent -> negative");

    /* 3. opendir + readdir */
    fd = fs_opendir("SYS:/");
    check(fd >= 0, "fs_opendir SYS:/");

    if (fd >= 0) {
        struct dirent ent;
        int count = 0;
        int n;
        while ((n = fs_readdir(fd, &ent)) == 1) {
            count++;
            if (count >= 100) break;
        }
        check(count > 0, "fs_readdir counts > 0");
        fs_closedir(fd);
    }

    /* 4. getcwd */
    char cwd[64];
    int n = getcwd(cwd, sizeof(cwd));
    check(n > 0, "getcwd returns");

    /* 5. chdir 到 /BOOT（如果存在） */
    int r = chdir("SYS:/BOOT");
    if (r == 0) {
        char cwd2[64];
        getcwd(cwd2, sizeof(cwd2));
        /* 检查 cwd2 里有没有 "BOOT" */
        int found = 0;
        for (int i = 0; cwd2[i] && cwd2[i+1] && cwd2[i+2] && cwd2[i+3]; i++) {
            if (cwd2[i] == 'B' && cwd2[i+1] == 'O' &&
                cwd2[i+2] == 'O' && cwd2[i+3] == 'T') {
                found = 1;
                break;
            }
        }
        check(found, "chdir /BOOT + getcwd");
        chdir("SYS:/");
    } else {
        printf("  (skip chdir: /BOOT not accessible, ret=%d)\n", r);
    }
}

/* ---------- 5. 设备层 ---------- */

static void test_dev(void) {
    section("device");

    /* 打开键盘 */
    int kb = dev_open(DEV_TYPE_KEYBOARD, 0);
    check(kb >= 0, "dev_open KEYBOARD");

    if (kb >= 0) {
        /* 非阻塞读 —— 有数据就读，没数据返回 0 */
        char c;
        int n = dev_read(kb, &c, 1);
        check(n >= 0, "dev_read non-blocking (>= 0)");
        dev_close(kb);
    }

    /* 打开不存在的设备类型 */
    int bad = dev_open(99, 0);
    check(bad < 0, "dev_open invalid type -> negative");
}

/* ---------- 6. 控制台 ---------- */

static void test_console(void) {
    section("console");

    int x, y;
    console_get_cursor(&x, &y);
    check(x >= 0 && x < 80 && y >= 0 && y < 25,
          "console_get_cursor in range");

    /* save / restore 一圈 */
    console_save_cursor();

    console_set_cursor(0, 0);
    /* 不写东西，纯粹移动 */

    console_restore_cursor();

    int x2, y2;
    console_get_cursor(&x2, &y2);
    check(x2 == x && y2 == y, "save/restore cursor");

    /* 彩色输出（视觉检查） */
    console_set_color(C_YELLOW, C_BLACK);
    printf("  (visual) this line should be YELLOW\n");
    console_set_color(C_BLUE, C_BLACK);
    printf("  (visual) this line should be BLUE\n");
    console_set_color(C_GRAY, C_BLACK);
    ok("color output");
}

/* ---------- 7. 错误码 ---------- */

static void test_errno(void) {
    section("errno");

    /* 野指针 -> EFAULT */
    check_eq(fs_open((const char*)0x00001000, FS_O_RDONLY),
             EFAULT, "fs_open(bad ptr) -> EFAULT");

    /* 不存在 -> 负值 */
    check(fs_open("SYS:/NOT_EXIST_9999.TXT", FS_O_RDONLY) < 0,
          "fs_open(no file) -> negative");

    /* 无效 fd */
    char c;
    check(fs_read(-1, &c, 1) < 0, "fs_read(-1) -> negative");

    /* chdir 到不存在 */
    check(chdir("SYS:/NOT_EXIST_DIR") < 0,
          "chdir(no dir) -> negative");
}

/* ---------- 8. 用户指针边界 ---------- */

static void test_ptr_bounds(void) {
    section("user pointer bounds");

    /* 低地址 */
    check(fs_open((const char*)0x1000, FS_O_RDONLY) == EFAULT,
          "low ptr rejected");

    /* 内核地址 */
    check(fs_open((const char*)0x00100000, FS_O_RDONLY) == EFAULT,
          "kernel ptr rejected");

    /* 合法路径仍能工作 */
    int fd = fs_open("SYS:/SHELL.ELF", FS_O_RDONLY);
    check(fd >= 0, "valid path still works");
    if (fd >= 0) fs_close(fd);
}

/* ---------- 9. 阻塞系统调用返回值（本次重构核心） ---------- */

static void test_blocking_syscall(void) {
    section("blocking syscall return value");

    /* spawn 一个不会退出的进程（IDLE.ELF 用 pause 空转） */
    int h = spawn("IDLE.ELF", NULL);
    check(h >= 0, "spawn IDLE.ELF");
    if (h < 0) return;

    /* 给 IDLE 一点时间进入运行态 */
    sleep_ms(20);

    /* wait 超时应该返回 EAGAIN */
    int status = 0;
    int r = wait(h, &status, 30);
    check_eq(r, EAGAIN, "wait timeout -> EAGAIN");

    /* 再次 wait 应该还是 EAGAIN，证明上次超时没有副作用 */
    r = wait(h, &status, 30);
    check_eq(r, EAGAIN, "wait timeout again -> EAGAIN");

    /* kill 后 wait 应该立即成功，且返回退出状态 */
    r = kill(h, 99);
    check_eq(r, OK, "kill -> OK");

    r = wait(h, &status, 100);
    check_eq(r, OK, "wait after kill -> OK");
    check_eq(status, 99, "wait status = 99");

    /* close 后 handle 应该失效 */
    r = process_close(h);
    check_eq(r, OK, "process_close -> OK");

    r = process_close(h);
    check(r < 0, "process_close again -> error");

    r = wait(h, &status, 10);
    check(r < 0, "wait invalid handle -> error");
}

/* ---------- 10. spawn 错误路径 ---------- */

static void test_spawn_errors(void) {
    section("spawn error paths");

    int h = spawn("SYS:/NOT_EXIST_99999.ELF", NULL);
    check(h < 0, "spawn nonexistent -> negative");

    h = spawn("", NULL);
    check(h < 0, "spawn empty path -> negative");

    h = spawn((const char*)0x1000, NULL);     /* 内核地址 */
    check(h < 0, "spawn kernel ptr -> negative");

    h = spawn((const char*)0x70000000, NULL); /* 未映射的用户地址 */
    /* 允许返回值不同，只要不是有效 handle */
    check(h < 0, "spawn unmapped user ptr -> negative");

    h = spawn("SYS:/BOOT", NULL);             /* 是目录不是 ELF */
    check(h < 0, "spawn directory -> negative");
}

/* ---------- 11. 文件系统边界 ---------- */

static void test_fs_boundaries(void) {
    section("fs boundaries");

    int fd = fs_open("SYS:/SHELL.ELF", FS_O_RDONLY);
    check(fd >= 0, "fs_open SHELL.ELF");
    if (fd < 0) return;

    /* seek 超出文件大小 */
    int r = fs_seek(fd, 0x10000000);
    check(r < 0, "fs_seek beyond EOF -> error");

    /* 读一遍到 EOF */
    r = fs_seek(fd, 0);
    check_eq(r, 0, "fs_seek to 0");

    char buf[512];
    int total = 0, n;
    while ((n = fs_read(fd, buf, sizeof(buf))) > 0) {
        total += n;
        if (total > 500000) break;      /* 防止死循环 */
    }
    check(n == 0, "fs_read at EOF -> 0");
    check(total > 0, "fs_read total > 0");

    /* NULL 缓冲区 */
    fs_seek(fd, 0);
    r = fs_read(fd, NULL, 100);
    check(r < 0, "fs_read NULL buf -> error");

    /* 零长度读 */
    r = fs_read(fd, buf, 0);
    check(r < 0, "fs_read zero size -> error");

    fs_close(fd);

    /* 无效 fd */
    r = fs_read(-1, buf, 10);
    check(r < 0, "fs_read(-1) -> error");

    r = fs_read(999, buf, 10);
    check(r < 0, "fs_read(999) -> error");

    r = fs_close(-1);
    check(r < 0, "fs_close(-1) -> error");

    /* 目录不能用 fs_open 打开（当前实现） */
    fd = fs_open("SYS:/BOOT", FS_O_RDONLY);
    if (fd >= 0) {
        /* 如果实现允许打开目录，至少 read 应该失败 */
        r = fs_read(fd, buf, 10);
        check(r < 0, "fs_read on dir -> error");
        fs_close(fd);
    } else {
        ok("fs_open on dir rejected");
    }
}

/* ---------- 12. 多次打开同一文件 ---------- */

static void test_multi_open(void) {
    section("multiple opens");

    int fd1 = fs_open("SYS:/SHELL.ELF", FS_O_RDONLY);
    int fd2 = fs_open("SYS:/SHELL.ELF", FS_O_RDONLY);
    check(fd1 >= 0 && fd2 >= 0, "two opens same file");
    check(fd1 != fd2, "fd1 != fd2");

    if (fd1 >= 0 && fd2 >= 0) {
        char b1[16], b2[16], b3[16], b4[16];

        /* 两个 fd 独立读同一区域 */
        fs_read(fd1, b1, 16);
        fs_read(fd2, b2, 16);
        check(memcmp(b1, b2, 16) == 0, "identical content from two fds");

        /* fd1 前进，fd2 seek 回 0 */
        fs_read(fd1, b3, 16);
        fs_seek(fd2, 0);
        fs_read(fd2, b4, 16);
        check(memcmp(b2, b4, 16) == 0, "fd2 seek back gives same bytes");

        /* fd1 前进后再读的位置应与 b1 不同（内容不同或 offset 不同） */
        /* 仅验证 fs_seek 独立生效 */

        fs_close(fd1);
        fs_close(fd2);
    }

    /* 同时打开 16 个 */
    int fds[16];
    int ok_count = 0;
    for (int i = 0; i < 16; i++) {
        fds[i] = fs_open("SYS:/SHELL.ELF", FS_O_RDONLY);
        if (fds[i] >= 0) ok_count++;
    }
    check_eq(ok_count, 16, "open 16 handles simultaneously");
    for (int i = 0; i < 16; i++) {
        if (fds[i] >= 0) fs_close(fds[i]);
    }
}

/* ---------- 13. brk 边界 ---------- */

static void test_brk_boundaries(void) {
    section("brk boundaries");

    int base = brk(0);
    check(base > 0, "brk(0) query");

    /* 扩展 8KB */
    int r = brk(base + 8192);
    check_eq(r, 0, "brk extend 8KB");

    /* 读一下扩展区 */
    volatile char *p = (volatile char*)base;
    p[0] = 1;
    p[8191] = 2;
    check(p[0] == 1 && p[8191] == 2, "brk extended region writable");

    /* 收缩到 4KB */
    r = brk(base + 4096);
    check_eq(r, 0, "brk shrink to 4KB");

    /* 再扩展回来 */
    r = brk(base + 16384);
    check_eq(r, 0, "brk extend to 16KB");

    /* 回到原位 */
    r = brk(base);
    check_eq(r, 0, "brk back to base");

    /* 低于 heap_base 应该失败 */
    r = brk(0x40000000);
    check(r < 0, "brk below heap_base -> error");

    r = brk(0x4FFFFFFF);
    check(r < 0, "brk just below heap_base -> error");
}

/* ---------- 14. 字符串边界 ---------- */

static void test_string_boundaries(void) {
    section("string boundaries");

    check_eq((int)strlen(""), 0, "strlen empty");
    check_eq((int)strlen("a"), 1, "strlen single");

    char buf[16];

    memcpy(buf, "abc", 0);
    ok("memcpy 0 bytes");

    check_eq(memcmp("abc", "abd", 0), 0, "memcmp 0 bytes");

    memset(buf, 'X', 0);
    ok("memset 0 bytes");

    strncpy(buf, "hello", 5);
    buf[5] = '\0';
    check(strcmp(buf, "hello") == 0, "strncpy exact length");

    strncpy(buf, "ab", 5);
    check(buf[0] == 'a' && buf[1] == 'b' &&
          buf[2] == '\0' && buf[4] == '\0', "strncpy zero pad");

    /* strcmp 空串 */
    check(strcmp("", "") == 0, "strcmp both empty");
    check(strcmp("", "a") < 0, "strcmp empty < a");
    check(strcmp("a", "") > 0, "strcmp a > empty");
}

/* ---------- 15. printf 边界 ---------- */

static void test_printf_boundaries(void) {
    section("printf boundaries");

    char buf[64];

    snprintf(buf, sizeof(buf), "%d", -2147483647 - 1);
    check(strcmp(buf, "-2147483648") == 0, "%d INT_MIN");

    snprintf(buf, sizeof(buf), "%u", 0xFFFFFFFFu);
    check(strcmp(buf, "4294967295") == 0, "%u UINT_MAX");

    snprintf(buf, sizeof(buf), "%d", 0);
    check(strcmp(buf, "0") == 0, "%d zero");

    snprintf(buf, sizeof(buf), "%x", 0);
    check(strcmp(buf, "0") == 0, "%x zero");

    snprintf(buf, sizeof(buf), "[%s]", "");
    check(strcmp(buf, "[]") == 0, "%s empty");

    /* 长字符串 */
    char long_str[80];
    memset(long_str, 'a', 50);
    long_str[50] = '\0';
    snprintf(buf, sizeof(buf), "%s", long_str);
    check(strcmp(buf, long_str) == 0, "%s 50 chars");

    /* snprintf size 0 */
    int n = snprintf(buf, 0, "abc");
    check_eq(n, 3, "snprintf size 0 returns needed len");

    /* snprintf size 1 */
    buf[0] = 'X';
    n = snprintf(buf, 1, "abc");
    check(buf[0] == '\0', "snprintf size 1 null terminates");
    check_eq(n, 3, "snprintf size 1 returns needed len");
}

/* ---------- 16. chdir 相对路径 ---------- */

static void test_chdir_relative(void) {
    section("chdir relative paths");

    char cwd[128];

    /* 从 SYS:/ 开始 */
    check_eq(chdir("SYS:/"), 0, "chdir SYS:/");

    /* 相对路径打开 */
    int fd = fs_open("SHELL.ELF", FS_O_RDONLY);
    check(fd >= 0, "open relative SHELL.ELF");
    if (fd >= 0) fs_close(fd);

    /* 尝试进入 /BOOT */
    int r = chdir("BOOT");
    if (r == 0) {
        getcwd(cwd, sizeof(cwd));
        int found = 0;
        for (int i = 0; cwd[i]; i++) {
            if (cwd[i] == 'B' && cwd[i+1] == 'O' &&
                cwd[i+2] == 'O' && cwd[i+3] == 'T') {
                found = 1;
                break;
            }
        }
        check(found, "chdir BOOT -> cwd contains BOOT");

        /* 从 /BOOT 用 .. 回到根 */
        r = chdir("..");
        check_eq(r, 0, "chdir ..");
    } else {
        ok("(skip chdir BOOT: not accessible)");
    }

    /* 不存在的相对路径 */
    chdir("SYS:/");
    r = chdir("NOT_EXIST_12345");
    check(r < 0, "chdir nonexistent relative -> error");

    /* 绝对路径覆盖 cwd */
    check_eq(chdir("SYS:/"), 0, "chdir absolute override");
}

/* ---------- 17. spawn/close 循环 ---------- */

static void test_spawn_stress(void) {
    section("spawn stress");

    int failures = 0;
    for (int i = 0; i < 8; i++) {
        int h = spawn("IDLE.ELF", NULL);
        if (h < 0) { failures++; continue; }

        sleep_ms(5);

        if (kill(h, i) != OK) { failures++; process_close(h); continue; }

        int status = -1;
        if (wait(h, &status, 100) != OK || status != i) {
            failures++;
            process_close(h);
            continue;
        }
        process_close(h);
    }

    if (failures == 0) {
        ok("8 rounds spawn/kill/wait/close");
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d of 8 failed", failures);
        fail("spawn stress", buf);
    }
}

/* ---------- 主入口 ---------- */

void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    /* 清屏，保证从干净状态开始 */
    console_clear();

    console_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    printf("  HeliumOS Self Test\n");
    printf("========================================\n");
    console_set_color(C_GRAY, C_BLACK);

    test_strings();
    test_printf();
    test_mem();
    test_fs();
    test_dev();
    test_console();
    test_errno();
    test_ptr_bounds();
    test_blocking_syscall();
    test_spawn_errors();
    test_fs_boundaries();
    test_multi_open();
    test_brk_boundaries();
    test_string_boundaries();
    test_printf_boundaries();
    test_chdir_relative();
    test_spawn_stress();

    printf("\n");
    console_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    if (g_fail == 0) {
        console_set_color(C_GREEN, C_BLACK);
        printf("  ALL PASSED (%d tests)\n", g_pass);
    } else {
        console_set_color(C_RED, C_BLACK);
        printf("  %d PASSED, %d FAILED\n", g_pass, g_fail);
    }
    console_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    console_set_color(C_GRAY, C_BLACK);

    return g_fail;
}