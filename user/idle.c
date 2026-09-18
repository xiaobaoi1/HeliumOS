// user/idle.c
void _start(void) {
    while (1) {
        // 空转，或者使用 pause 指令（但 pause 在用户态可用，对性能友好）
        // 这里使用简单循环，不做任何系统调用
        __asm__ volatile("pause");  // 减少功耗，但并非必须
    }
}