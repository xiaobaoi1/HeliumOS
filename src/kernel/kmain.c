#include <screen.h>
#include <serial.h>
#include <pmm.h>
#include <printf.h>
#include <stdint.h>
#include <vmm.h>

void kmain(uint32_t magic, uint32_t addr) {
    screen_init();
    kprintf("[KERNEL] Screan initialized.\n");
    serial_init();
    kprintf("[KERNEL] Serial initialized.\n");

    kprintf("========================================\n");
    kprintf(" HeliumOS Kernel Started!\n");
    kprintf("========================================\n");

    // 打印 GRUB 传入参数
    kprintf("Magic: 0x%x, Multiboot2 Info Addr: 0x%x\n", magic, addr);

    // 初始化物理内存管理器
    pmm_init(addr);
    kprintf("[KERNEL] PMM initialized.\n");
    vmm_init();
    kprintf("[KERNEL] VMM initialized.\n");


    // 测试分配一页并打印详细信息
    uint32_t test_page = pmm_alloc_page();
    if (test_page) {
        kprintf("[TEST] Allocated physical page at: %p\n", test_page);
        kprintf("[TEST] Free pages remaining: %d\n", pmm_get_free_count());
        
        // 释放回去
        pmm_free_page(test_page);
        kprintf("[TEST] Freed page. Free pages: %d\n", pmm_get_free_count());
    } else {
        kprintf("[TEST] ERROR: Failed to allocate page!\n");
    }

    kprintf("System ready. Entering idle loop.\n");

    while (1) {
        __asm__ volatile("hlt");
    }
}