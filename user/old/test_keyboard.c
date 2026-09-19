// ========== user/test_keyboard.c ==========
#define SYS_WRITE  12
#define SYS_READ   11
#define SYS_EXIT    1

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

static int read_char(char *c) {
    return syscall(SYS_READ, 0, (int)c, 1);
}

static void exit_proc(int status) {
    syscall(SYS_EXIT, status, 0, 0);
}

void _start(void) {
    write_str("=== Keyboard Test ===\n");
    write_str("Press any key (ESC to exit)\n");
    char c;
    read_char(&c);
    while (1) {
        if (read_char(&c) == 1) {
            if (c == 27) {  // ESC
                write_str("\nESC pressed, exiting.\n");
                break;
            }
            char buf[2] = {c, 0};
            write_str(buf);
        }
        // 没有按键时，让出 CPU 或执行 pause 降低功耗
        // 这里使用 pause 指令（不是系统调用），只是让 CPU 稍微休息
        // __asm__ volatile("pause");
    }
    exit_proc(0);
}