/* user/test_dev.c
 * 测试设备层系统调用
 */

#include <stddef.h>

#define SYS_EXIT          1
#define SYS_WRITE         12

#define SYS_DEV_OPEN      41
#define SYS_DEV_READ      42
#define SYS_DEV_CLOSE     45

#define DEV_TYPE_KEYBOARD 1

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

static void write_str(const char *str) {
    int len = 0;
    while (str[len]) len++;
    syscall(SYS_WRITE, 1, (int)str, len);
}

static void write_dec(int v) {
    char buf[16];
    int i = 0;
    if (v == 0) { write_str("0"); return; }
    if (v < 0) { write_str("-"); v = -v; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    char out[16];
    for (int j = 0; j < i; j++) out[j] = buf[i - 1 - j];
    out[i] = '\0';
    write_str(out);
}

static void exit_proc(int status) {
    syscall(SYS_EXIT, status, 0, 0);
}

void _start(void) {
    write_str("\n");
    write_str("========================================\n");
    write_str("  HeliumOS Device Test\n");
    write_str("========================================\n");

    /* 打开键盘设备 */
    write_str("[T1] dev_open(DEV_KEYBOARD)\n");
    int dev = syscall(SYS_DEV_OPEN, DEV_TYPE_KEYBOARD, 0, 0);
    if (dev < 0) {
        write_str("  FAIL: dev_open returned ");
        write_dec(dev);
        write_str("\n");
        exit_proc(1);
    }
    write_str("  OK: dev = ");
    write_dec(dev);
    write_str("\n");

    /* 轮询读取按键，按 ESC 退出 */
    write_str("[T2] ESC to break\n");
    write_str("     input:\n> ");

    char c;
    int total = 0;
    while (1) {
        int n = syscall(SYS_DEV_READ, dev, (int)&c, 1);
        if (n == 1) {
            if (c == 27) {   /* ESC */
                write_str("\n  ESC pressed, exiting\n");
                break;
            }
            char buf[2] = {c, 0};
            write_str(buf);
            total++;
        }
        __asm__ volatile("pause");
    }

    write_str("  total keys: ");
    write_dec(total);
    write_str("\n");

    /* 关闭设备 */
    write_str("[T3] dev_close\n");
    int r = syscall(SYS_DEV_CLOSE, dev, 0, 0);
    write_str("  dev_close returned ");
    write_dec(r);
    write_str("\n");

    write_str("========================================\n");
    write_str("  DONE\n");
    write_str("========================================\n");

    exit_proc(0);
}