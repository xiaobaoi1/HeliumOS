/* user/test_fault.c
 * 用户态异常测试：验证每种异常都被捕获且系统存活。
 */
#include "syscall.h"
#include "stdio.h"

#define C_BLACK   0
#define C_GREEN   10
#define C_RED     12
#define C_YELLOW  14
#define C_BLUE    9
#define C_GRAY    7

static void title(const char *s) {
    tty_set_color(C_BLUE, C_BLACK);
    printf("\n--- %s ---\n", s);
    tty_set_color(C_GRAY, C_BLACK);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    tty_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    printf("  User Fault Test\n");
    printf("========================================\n");
    tty_set_color(C_GRAY, C_BLACK);

    /* 参数模式：arg1 决定触发哪个 */
    int mode = 0;
    if (argc >= 2 && argv[1][0] >= '0' && argv[1][0] <= '9') {
        mode = argv[1][0] - '0';
    }

    title("select mode");

    if (mode == 0) {
        printf("usage: TESTFAULT.ELF <mode>\n");
        printf("  1 = write unmapped (page fault, write)\n");
        printf("  2 = read unmapped (page fault, read)\n");
        printf("  3 = divide by zero\n");
        printf("  4 = invalid opcode\n");
        printf("\nrunning mode 1 by default\n");
        mode = 1;
    }

    switch (mode) {
        case 1: {
            title("mode 1: write unmapped");
            printf("about to write to 0x70000000...\n");
            volatile int *p = (int*)0x70000000;
            *p = 42;
            break;
        }
        case 2: {
            title("mode 2: read unmapped");
            printf("about to read from 0x70000000...\n");
            volatile int *p = (int*)0x70000000;
            int x = *p;
            (void)x;
            break;
        }
        case 3: {
            title("mode 3: divide by zero");
            printf("about to divide by zero...\n");
            volatile int zero = 0;
            volatile int a = 1;
            volatile int b = a / zero;
            (void)b;
            break;
        }
        case 4: {
            title("mode 4: invalid opcode");
            printf("about to execute 0x0F 0x0B (ud2)...\n");
            __asm__ volatile("ud2");
            break;
        }
    }

    /* 上面任何一条成功执行到这里，都说明异常没被捕获 */
    tty_set_color(C_RED, C_BLACK);
    printf("!!! NOT REACHED — fault was not caught !!!\n");
    tty_set_color(C_GRAY, C_BLACK);
    return 1;
}