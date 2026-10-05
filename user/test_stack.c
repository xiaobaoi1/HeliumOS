/* 栈自动扩展测试 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"

static int deep_recursion(int n) {
    char buf[1024];
    buf[0] = (char)n;
    buf[1023] = (char)(n ^ 0xFF);
    if (n == 0) return buf[0];
    int r = deep_recursion(n - 1);
    /* 回程时验证本帧数据还在 */
    if (buf[0] != (char)n || buf[1023] != (char)(n ^ 0xFF)) {
        return -1;
    }
    return r;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* 64 层 × 1KB = 64KB 栈，远超初始两页 8KB */
    int r = deep_recursion(64);
    if (r == 0) {
        printf("stack extend test: PASS\n");
        return 0;
    } else {
        printf("stack extend test: FAIL (r=%d)\n", r);
        return 1;
    }
}