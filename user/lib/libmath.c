/* libmath — MethylUI 动态库测试。全局变量 + 函数导出。
 * 编译：gcc -m32 -shared -fPIC -nostdlib -o libmath.so libmath.c */
int g_counter = 0;

int add(int a, int b) {
    return a + b;
}

int mul(int a, int b) {
    return a * b;
}

int bump(void) {
    g_counter++;
    return g_counter;
}