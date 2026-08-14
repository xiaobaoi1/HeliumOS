#include <screen.h>
#include <serial.h>
#include <memory.h>
#include <stdint.h>

void kmain(uint32_t magic, uint32_t addr) {
    screen_init();
    serial_init();

    serial_write_string("HeliumOS Kernel started.\n");

    // 1. 初始化物理内存管理器（传入 GRUB 的内存信息地址）
    pmm_init(addr);

    // 2. 测试分配一页
    uint32_t test_page = pmm_alloc_page();
    if (test_page) {
        serial_write_string("[TEST] Allocated physical page at: 0x");
        // 简易输出十六进制（后续会用 printf 替代）
        char hex[9];
        for (int i = 7; i >= 0; i--) {
            uint8_t nibble = (test_page >> (i * 4)) & 0xF;
            hex[7-i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        }
        hex[8] = '\0';
        serial_write_string(hex);
        serial_write_string("\n");
        pmm_free_page(test_page);
    }

    while (1) {
        __asm__ volatile("hlt");
    }
}