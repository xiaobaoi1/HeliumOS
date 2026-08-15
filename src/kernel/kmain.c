#include <screen.h>
#include <serial.h>
#include <pmm.h>
#include <vmm.h>
#include <gdt.h>
#include <tss.h>
#include <idt.h>
#include <task.h>
#include <ata.h>
#include <fat32.h>
#include <elf.h>
#include <printf.h>
#include <stdint.h>

void kmain(uint32_t magic, uint32_t addr) {
    screen_init();
    serial_init();

    kprintf("========================================\n");
    kprintf(" HeliumOS Kernel Started (1GB/3GB)\n");
    kprintf("========================================\n");

    pmm_init(addr);
    gdt_init();
    tss_init();
    vmm_init();
    idt_init();

    // 初始化 ATA
    ata_init();

    // 初始化 FAT32（分区从 LBA 2048 开始）
    fat32_init(2048);

    // 创建用户进程页目录
    uint32_t *user_pgd = vmm_create_process_page_directory();
    if (!user_pgd) {
        kprintf("[KERNEL] Failed to create user page directory.\n");
        while (1) __asm__ volatile("hlt");
    }

    // 从硬盘加载 proc.elf
    uint32_t entry_point = load_elf_from_disk("/PROC.ELF", user_pgd);
    if (!entry_point) {
        kprintf("[KERNEL] Failed to load /PROC.ELF, fallback to hardcoded program.\n");
        // 回退到硬编码 jmp $
        uint32_t code_phys = pmm_alloc_page();
        if (code_phys) {
            uint8_t *code_virt = (uint8_t*)code_phys;
            code_virt[0] = 0xEB;
            code_virt[1] = 0xFE;
            vmm_map_user_page(user_pgd, 0x40000000, code_phys, PTE_WRITE | PTE_USER);
            entry_point = 0x40000000;
        }
    }

    if (entry_point) {
        struct task *task = task_create(entry_point, user_pgd);
        if (task) {
            kprintf("[KERNEL] Running user process at entry 0x%p\n", entry_point);
            task_run_first(task);
        }
    }

    while (1) __asm__ volatile("hlt");
}