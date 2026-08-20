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
            kprintf("[KERNEL] Failed to find entry point for process1.\n");
            while (1) __asm__("hlt");
        }

        if (entry1) {
            struct task *task = task_create(entry1, pgd1);
            if (!task) {
                kprintf("[KERNEL] Failed to create task 1.\n");
                while (1) __asm__("hlt");
            }
        }
    }
    struct task *p, *c;

    
    {
        uint32_t *pgd2 = vmm_create_process_page_directory();
        if (!pgd2) {
            kprintf("[KERNEL] Failed to create page directory for process2.\n");
            while (1) __asm__("hlt");
        }

        uint32_t entry = load_elf_from_disk("/PROC2.ELF", pgd2);
        if (!entry) {
            kprintf("[KERNEL] Failed to find entry point for process2.\n");
            while (1) __asm__("hlt");
        }

        if (entry) {
            struct task *task = task_create(entry, pgd2);
            c = task;
            if (!task) {
                kprintf("[KERNEL] Failed to create task 2.\n");
                while (1) __asm__("hlt");
            }
        }
    }
    {
        uint32_t *pgd3 = vmm_create_process_page_directory();
        if (!pgd3) {
            kprintf("[KERNEL] Failed to create page directory for process3.\n");
            while (1) __asm__("hlt");
        }

        uint32_t entry = load_elf_from_disk("/PROC3.ELF", pgd3);
        if (!entry) {
            kprintf("[KERNEL] Failed to find entry point for process3.\n");
            while (1) __asm__("hlt");
        }

        if (entry) {
            struct task *task = task_create(entry, pgd3);
            p = task;
            if (!task) {
                kprintf("[KERNEL] Failed to create task 3.\n");
                while (1) __asm__("hlt");
            }
        }
    }
    
    c->parent = p;

    kprintf("[KERNEL] Processes created. Starting scheduler...\n");

    scheduler_start();

    while (1) __asm__("hlt");
}