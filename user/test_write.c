/* user/test_write.c
 * FAT32 写测试：创建、写、读回、跨簇写入。
 */

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "errno.h"

#define C_BLACK   0
#define C_GREEN   10
#define C_RED     12
#define C_YELLOW  14
#define C_BLUE    9
#define C_GRAY    7

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *n) {
    tty_set_color(C_GREEN, C_BLACK);
    printf("  [PASS] %s\n", n);
    tty_set_color(C_GRAY, C_BLACK);
    g_pass++;
}

static void fail(const char *n, const char *w) {
    tty_set_color(C_RED, C_BLACK);
    printf("  [FAIL] %s: %s\n", n, w);
    tty_set_color(C_GRAY, C_BLACK);
    g_fail++;
}

static void check(int c, const char *n) {
    if (c) ok(n); else fail(n, "false");
}

static void check_eq(int g, int e, const char *n) {
    if (g == e) ok(n);
    else {
        char b[64];
        snprintf(b, sizeof(b), "got %d, want %d", g, e);
        fail(n, b);
    }
}

static void section(const char *t) {
    printf("\n");
    tty_set_color(C_BLUE, C_BLACK);
    printf("--- %s ---\n", t);
    tty_set_color(C_GRAY, C_BLACK);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    tty_clear();
    tty_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    printf("  FAT32 Write Test\n");
    printf("========================================\n");
    tty_set_color(C_GRAY, C_BLACK);

    /* ---------- 1. 创建 + 写 ---------- */
    section("create + write");

    const char *path = "SYS:/WRTEST.TXT";
    int fd = fs_open(path, FS_O_WRONLY | FS_O_CREAT);
    check(fd >= 0, "fs_open O_CREAT");
    if (fd < 0) goto done;

    const char *msg = "hello fat32 write!";
    int n = fs_write(fd, msg, strlen(msg));
    check_eq(n, (int)strlen(msg), "fs_write");
    fs_close(fd);

    /* ---------- 2. 重新打开读回 ---------- */
    section("reopen + read");

    fd = fs_open(path, FS_O_RDONLY);
    check(fd >= 0, "fs_open readonly");
    if (fd >= 0) {
        char buf[64];
        memset(buf, 0, sizeof(buf));
        n = fs_read(fd, buf, sizeof(buf) - 1);
        check(n > 0, "fs_read");
        if (n > 0) {
            check(strcmp(buf, msg) == 0, "read matches written");
        }
        fs_close(fd);
    }

    /* ---------- 3. 跨簇写入（3000 字节 > 2 簇） ---------- */
    section("multi-cluster");

    const char *big_path = "SYS:/BIGTEST.BIN";
    fd = fs_open(big_path, FS_O_WRONLY | FS_O_CREAT);
    check(fd >= 0, "create BIGTEST.BIN");
    if (fd >= 0) {
        char big[3000];
        for (int i = 0; i < 3000; i++) big[i] = (char)(i & 0xFF);

        int total = 0;
        for (int i = 0; i < 3000; i += 1000) {
            int r = fs_write(fd, big + i, 1000);
            if (r > 0) total += r;
        }
        check_eq(total, 3000, "write 3000 bytes");
        fs_close(fd);
    }

    /* ---------- 4. 读回大文件 ---------- */
    section("reopen big + verify");

    fd = fs_open(big_path, FS_O_RDONLY);
    check(fd >= 0, "reopen BIGTEST.BIN");
    if (fd >= 0) {
        char big[3000];
        int total = 0;
        int r;
        while ((r = fs_read(fd, big + total, 3000 - total)) > 0) {
            total += r;
            if (total >= 3000) break;
        }
        check_eq(total, 3000, "read 3000 bytes");

        int ok_data = 1;
        for (int i = 0; i < 3000; i++) {
            if (big[i] != (char)(i & 0xFF)) { ok_data = 0; break; }
        }
        check(ok_data, "data integrity");
        fs_close(fd);
    }

done:
    printf("\n");
    tty_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    if (g_fail == 0) {
        tty_set_color(C_GREEN, C_BLACK);
        printf("  ALL PASSED (%d)\n", g_pass);
    } else {
        tty_set_color(C_RED, C_BLACK);
        printf("  %d pass, %d fail\n", g_pass, g_fail);
    }
    tty_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    tty_set_color(C_GRAY, C_BLACK);

    return g_fail;
}