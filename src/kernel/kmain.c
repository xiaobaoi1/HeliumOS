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

    ata_init();
    fat32_init(2048);

    /* 创建两个进程 */
    {
        uint32_t *pgd1 = vmm_create_process_page_directory();
        if (!pgd1) {
            kprintf("[KERNEL] Failed to create page directory for process 1.\n");
            while (1) __asm__("hlt");
        }

        uint32_t entry1 = load_elf_from_disk("/PROC.ELF", pgd1);
        if (!entry1) {
            /* 回退到硬编码 jmp $ */
            uint32_t phys = pmm_alloc_page();
            if (phys) {
                ((uint8_t*)phys)[0] = 0xEB;
                ((uint8_t*)phys)[1] = 0xFE;
                vmm_map_user_page(pgd1, 0x40000000, phys, PTE_WRITE | PTE_USER);
                entry1 = 0x40000000;
            }
        }

        if (entry1) {
            struct task *task = task_create(entry1, pgd1);
            if (!task) {
                kprintf("[KERNEL] Failed to create task 1.\n");
                while (1) __asm__("hlt");
            }
        }
    }


    {
        uint32_t *pgd2 = vmm_create_process_page_directory();
        if (!pgd2) {
            kprintf("[KERNEL] Failed to create page directory for process2.\n");
            while (1) __asm__("hlt");
        }

        uint32_t entry = load_elf_from_disk("/PROC2.ELF", pgd2);
        if (!entry) {
            /* 回退到硬编码 jmp $ */
            uint32_t phys = pmm_alloc_page();
            if (phys) {
                ((uint8_t*)phys)[0] = 0xEB;
                ((uint8_t*)phys)[1] = 0xFE;
                vmm_map_user_page(pgd2, 0x40000000, phys, PTE_WRITE | PTE_USER);
                entry = 0x40000000;
            }
        }

        if (entry) {
            struct task *task = task_create(entry, pgd2);
            if (!task) {
                kprintf("[KERNEL] Failed to create task 2.\n");
                while (1) __asm__("hlt");
            }
        }
    }

    kprintf("[KERNEL] Processes created. Starting scheduler...\n");

    scheduler_start();

    while (1) __asm__("hlt");
}