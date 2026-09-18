/* user/test_console.c
 * 测试 console_* 系统调用族（含光标控制）
 */
#include <stddef.h>

/* ---------- 系统调用号 ---------- */
#define SYS_EXIT                     1
#define SYS_WRITE                    12
#define SYS_CONSOLE_CLEAR            13
#define SYS_CONSOLE_SET_COLOR        14
#define SYS_CONSOLE_SET_CURSOR       15
#define SYS_CONSOLE_GET_CURSOR       16
#define SYS_CONSOLE_SAVE_CURSOR      17
#define SYS_CONSOLE_RESTORE_CURSOR   18
#define SYS_CONSOLE_DEBUG_WRITE      19

/* ---------- VGA 颜色常量 ---------- */
#define VGA_BLACK        0
#define VGA_LIGHT_GRAY   7
#define VGA_LIGHT_GREEN  10
#define VGA_LIGHT_RED    12
#define VGA_YELLOW       14

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

static void debug_str(const char *str) {
    int len = 0;
    while (str[len]) len++;
    syscall(SYS_CONSOLE_DEBUG_WRITE, (int)str, len, 0);
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
    write_str("  HeliumOS Console Test\n");
    write_str("========================================\n");

    /* ---------- T1: console_write（和 sys_write 等价） ---------- */
    write_str("[T1] console_write\n");

    /* ---------- T2: 颜色 ---------- */
    write_str("[T2] colors\n");
    syscall(SYS_CONSOLE_SET_COLOR, VGA_LIGHT_RED, VGA_BLACK, 0);
    write_str("     this line is RED\n");
    syscall(SYS_CONSOLE_SET_COLOR, VGA_LIGHT_GREEN, VGA_BLACK, 0);
    write_str("     this line is GREEN\n");
    syscall(SYS_CONSOLE_SET_COLOR, VGA_YELLOW, VGA_BLACK, 0);
    write_str("     this line is YELLOW\n");
    syscall(SYS_CONSOLE_SET_COLOR, VGA_LIGHT_GRAY, VGA_BLACK, 0);
    write_str("     color reset\n");

    /* ---------- T3: 光标定位 ---------- */
    write_str("[T3] cursor set/get\n");
    int out[2] = {0, 0};
    syscall(SYS_CONSOLE_GET_CURSOR, (int)out, 0, 0);
    write_str("     current: x=");
    write_dec(out[0]);
    write_str(" y=");
    write_dec(out[1]);
    write_str("\n");

    /* 到 (5, 15) 写一个 '@' */
    syscall(SYS_CONSOLE_SET_CURSOR, 5, 15, 0);
    write_str("@");

    /* 回到原位继续 */
    syscall(SYS_CONSOLE_SET_CURSOR, out[0], out[1], 0);
    write_str("     wrote '@' at (5,15)\n");

    /* ---------- T4: 保存/恢复光标 ---------- */
    write_str("[T4] save/restore cursor\n");
    syscall(SYS_CONSOLE_SAVE_CURSOR, 0, 0, 0);
    syscall(SYS_CONSOLE_SET_CURSOR, 0, 20, 0);
    write_str("<< this is at (0,20) >>");
    syscall(SYS_CONSOLE_RESTORE_CURSOR, 0, 0, 0);
    write_str("     cursor restored\n");

    /* ---------- T5: 调试输出 ---------- */
    write_str("[T5] debug_write (check QEMU -serial stdio)\n");
    debug_str("[test_console] debug message 1\n");
    debug_str("[test_console] debug message 2\n");
    write_str("     sent to serial\n");

    /* ---------- T6: 清屏（放最后，否则前面输出全没了） ---------- */
    write_str("\n");
    write_str("[T6] clear in 3 seconds...\n");
    /* 简单延时：用 sleep 系统调用（当前已实现） */
    syscall(5, 3000, 0, 0);   /* SYS_SLEEP = 5 */
    syscall(SYS_CONSOLE_CLEAR, 0, 0, 0);

    /* 清屏后重新打印 */
    syscall(SYS_CONSOLE_SET_COLOR, VGA_LIGHT_GREEN, VGA_BLACK, 0);
    write_str("Console cleared! Test done.\n");
    syscall(SYS_CONSOLE_SET_COLOR, VGA_LIGHT_GRAY, VGA_BLACK, 0);

    exit_proc(0);
}