// ========== user/test_brk.c ==========
// 编译命令（见下方）

// 系统调用号（必须与内核一致）
#define SYS_WRITE  12
#define SYS_EXIT    1
#define SYS_BRK    21

// 通用系统调用函数（使用 int 0x80）
static inline int syscall(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c)
        : "memory"
    );
    return ret;
}

// 包装函数
static void write_str(const char *str) {
    int len = 0;
    while (str[len]) len++;
    syscall(SYS_WRITE, 1, (int)str, len);
}

static void exit_proc(int status) {
    syscall(SYS_EXIT, status, 0, 0);
}

static int brk(int new_brk) {
    return syscall(SYS_BRK, new_brk, 0, 0);
}

// 入口点
void _start(void) {
    // 1. 查询当前堆顶
    int old_brk = brk(0);
    write_str("=== Heap Test ===\n");
    write_str("Old brk: 0x");
    // 简易十六进制打印（只为了演示，不写完整 printf）
    // 我们直接用数字转字符输出（仅支持 4 位十六进制，够用）
    char hex_buf[12];
    int i = 0;
    for (int shift = 28; shift >= 0; shift -= 4) {
        int nibble = (old_brk >> shift) & 0xF;
        hex_buf[i++] = (nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10);
    }
    hex_buf[i++] = '\n';
    hex_buf[i] = 0;
    write_str(hex_buf);

    // 2. 扩展堆：申请 1 页（0x1000 字节）
    int new_brk = old_brk + 0x1000;
    if (brk(new_brk) != 0) {
        write_str("ERROR: brk expansion failed!\n");
        exit_proc(-1);
    }
    write_str("Expanded brk to: 0x");
    // 同样方式打印新 brk
    i = 0;
    for (int shift = 28; shift >= 0; shift -= 4) {
        int nibble = (new_brk >> shift) & 0xF;
        hex_buf[i++] = (nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10);
    }
    hex_buf[i++] = '\n';
    hex_buf[i] = 0;
    write_str(hex_buf);

    // 3. 写入新分配的内存（验证是否真的可写）
    char *heap_ptr = (char*)old_brk;   // 堆起始地址
    heap_ptr[0] = 'A';
    heap_ptr[1] = 'l';
    heap_ptr[2] = 'i';
    heap_ptr[3] = 'v';
    heap_ptr[4] = 'e';
    heap_ptr[5] = '!';
    heap_ptr[6] = '\n';
    heap_ptr[7] = 0;
    write_str("Heap content: ");
    write_str(heap_ptr);

    // 4. 收缩堆（归还页）
    if (brk(old_brk) != 0) {
        write_str("ERROR: brk shrink failed!\n");
        exit_proc(-1);
    }
    write_str("Shrunk back to old brk. Test passed!\n");

    // 退出
    exit_proc(0);

    // 永不抵达
    while (1) __asm__("hlt");
}