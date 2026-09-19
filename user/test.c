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

/* ---------- 主入口 ---------- */

void _start(void) {
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

    _exit(g_fail);
}