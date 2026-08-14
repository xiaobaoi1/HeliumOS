#include <screen.h>
#include <serial.h>
#include <pmm.h>
#include <vmm.h>
#include <gdt.h>
#include <tss.h>
#include <task.h>
#include <printf.h>
#include <stdint.h>

/* 硬编码用户程序：jmp $ (死循环) */
static uint8_t user_program[] = { 0xEB, 0xFE };

void kmain(uint32_t magic, uint32_t addr) {
    screen_init();
    serial_init();

    kprintf("========================================\n");
    kprintf(" HeliumOS Kernel Started (1GB/3GB)\n");
    kprintf("========================================\n");

    pmm_init(addr);
    gdt_init();
    tss_init();

    /* 初始化虚拟内存管理 */
    vmm_init();
    idt_init();

    /* 创建一个用户进程页目录 */
    uint32_t *user_pgd = vmm_create_process_page_directory();
    if (!user_pgd) {
        kprintf("[KERNEL] ERROR: Failed to create user page directory.\n");
        while (1) __asm__ volatile("hlt");
    }

    /* 在用户空间中映射代码页（将硬编码程序放到物理页） */
    uint32_t code_phys = pmm_alloc_page();
    if (!code_phys) {
        kprintf("[KERNEL] ERROR: Failed to allocate code page.\n");
        while (1) __asm__ volatile("hlt");
    }

    /* 复制用户程序到物理页（物理地址直接访问） */
    uint32_t *code_virt = (uint32_t*)code_phys;
    for (int i = 0; i < sizeof(user_program); i++) {
        ((uint8_t*)code_virt)[i] = user_program[i];
    }

    /* 在用户页目录中映射代码页到 0x40000000（用户空间起始地址） */
    vmm_map_user_page(user_pgd, 0x40000000, code_phys, PTE_WRITE | PTE_USER);

    /* 创建进程 */
    struct task *task = task_create(0x40000000, user_pgd);
    if (!task) {
        kprintf("[KERNEL] ERROR: Failed to create task.\n");
        while (1) __asm__ volatile("hlt");
    }

    kprintf("[KERNEL] Running first user process...\n");

    /* 切换到用户态运行 */
    task_run_first(task);

    while (1) __asm__ volatile("hlt");
}