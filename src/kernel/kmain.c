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
#include <stddef.h>
#include <heap.h>
#include <path.h>
#include <errno.h>

/* ---------- 内部辅助：从一个 ELF 路径创建进程 ---------- */

static struct task *create_task_from_elf(struct fat32_volume *vol,
                                          const char *path) {
    /* 1. 创建页目录 */
    uint32_t *pgd = vmm_create_process_page_directory();
    if (!pgd) {
        kprintf("[KERNEL] Failed to create page directory for %s\n", path);
        return NULL;
    }

    /* 2. 加载 ELF */
    uint32_t entry = load_elf_from_disk(vol, path, pgd);
    if (!entry) {
        kprintf("[KERNEL] Failed to load %s\n", path);
        /* 释放页目录？这里简化处理，暂不释放 */
        return NULL;
    }

    /* 3. 创建 PCB */
    struct task *task = task_create(entry, pgd);
    if (!task) {
        kprintf("[KERNEL] Failed to create task for %s\n", path);
        return NULL;
    }

    return task;
}

/* ---------- 创建 IDLE 进程 ---------- */

void create_idle_task(struct fat32_volume *vol) {
    struct task *task = create_task_from_elf(vol, "/IDLE.ELF");
    if (!task) {
        kprintf("[KERNEL] FATAL: cannot create idle task\n");
        while (1) __asm__("hlt");
    }

    /* IDLE 的任务：尽量少占 CPU
     * - time_slice 设为 1，让它很快被抢占
     * - 用户程序内部用 pause 指令空转
     */
    task->time_slice = 1;

    kprintf("[KERNEL] IDLE task created (pid=%d)\n", task->pid);
}

/* ---------- 创建 SHELL 进程 ---------- */

void create_shell_task(struct fat32_volume *vol) {
    struct task *task = create_task_from_elf(vol, "/SHELL.ELF");
    if (!task) {
        kprintf("[KERNEL] FATAL: cannot create shell task\n");
        while (1) __asm__("hlt");
    }

    /* SHELL 是交互进程，给它正常时间片
     * 但注意：它不能太大，否则输入回显会卡
     * 默认 10 ticks = 10ms @1000Hz，够用
     */
    /* task->time_slice 已在 task_create 中初始化为 TIME_SLICE_TICKS */

    kprintf("[KERNEL] SHELL task created (pid=%d)\n", task->pid);
}

void kmain(uint32_t magic, uint32_t addr) {
    screen_init();
    serial_init();

    kprintf("========================================\n");
    kprintf(" HeliumOS Kernel Started (1GB/3GB)\n");
    kprintf("========================================\n");

    pmm_init(addr);
    heap_init();
    gdt_init();
    tss_init();
    vmm_init();
    idt_init();

    volume_init();
    fs_init();

    ata_init();
    // fat32_init(2048);

    /* 挂载 FAT32 并注册为 SYS 卷 */
    struct fat32_volume *sys_vol = fat32_mount(2048);
    if (!sys_vol) {
        kprintf("[KERNEL] Failed to mount FAT32\n");
        while (1) __asm__("hlt");
    }
    volume_register("SYS", VOL_FS_FAT32, 2048, 128 * 1024, sys_vol);

    create_idle_task(sys_vol);
    create_shell_task(sys_vol);

    // {
    //     uint32_t *pgd1 = vmm_create_process_page_directory();
    //     if (!pgd1) {
    //         kprintf("[KERNEL] Failed to create page directory for process 1.\n");
    //         while (1) __asm__("hlt");
    //     }

    //     uint32_t entry1 = load_elf_from_disk("/PROC.ELF", pgd1);
    //     if (!entry1) {
    //         kprintf("[KERNEL] Failed to find entry point for process1.\n");
    //         while (1) __asm__("hlt");
    //     }

    //     if (entry1) {
    //         struct task *task = task_create(entry1, pgd1);
    //         if (!task) {
    //             kprintf("[KERNEL] Failed to create task 1.\n");
    //             while (1) __asm__("hlt");
    //         }
    //     }
    // }
    // struct task *p, *c;

    
    // {
    //     uint32_t *pgd2 = vmm_create_process_page_directory();
    //     if (!pgd2) {
    //         kprintf("[KERNEL] Failed to create page directory for process2.\n");
    //         while (1) __asm__("hlt");
    //     }

    //     uint32_t entry = load_elf_from_disk("/SHELL.ELF", pgd2);
    //     if (!entry) {
    //         kprintf("[KERNEL] Failed to find entry point for process2.\n");
    //         while (1) __asm__("hlt");
    //     }

    //     if (entry) {
    //         struct task *task = task_create(entry, pgd2);
    //         // c = task;
    //         if (!task) {
    //             kprintf("[KERNEL] Failed to create task 2.\n");
    //             while (1) __asm__("hlt");
    //         }
    //     }
    // }
    // {
    //     uint32_t *pgd3 = vmm_create_process_page_directory();
    //     if (!pgd3) {
    //         kprintf("[KERNEL] Failed to create page directory for process3.\n");
    //         while (1) __asm__("hlt");
    //     }

    //     uint32_t entry = load_elf_from_disk("/PROC3.ELF", pgd3);
    //     if (!entry) {
    //         kprintf("[KERNEL] Failed to find entry point for process3.\n");
    //         while (1) __asm__("hlt");
    //     }

    //     if (entry) {
    //         struct task *task = task_create(entry, pgd3);
    //         p = task;
    //         if (!task) {
    //             kprintf("[KERNEL] Failed to create task 3.\n");
    //             while (1) __asm__("hlt");
    //         }
    //     }
    // }
    
    // c->parent = p;


    // 临时测试
    void *p1 = kmalloc(16);
    void *p2 = kmalloc(16);
    void *p3 = kmalloc(64);
    void *p4 = kmalloc(2048);
    kprintf("[TEST] kmalloc: p1=%p p2=%p p3=%p p4=%p\n", p1, p2, p3, p4);

    heap_stats();

    kfree(p1); kfree(p2); kfree(p3); kfree(p4);
    kprintf("[TEST] after kfree:\n");
    heap_stats();

    /* 临时测试路径解析 */
    // struct resolved_path rp;

    // if (resolve_path("SYS:/BOOT/GRUB.CFG", &rp) == 0) {
    //     kprintf("[TEST] absolute: vol=%s path=%s\n", rp.vol->name, rp.path);
    // }
    // if (resolve_path("/BOOT/../SHELL.ELF", &rp) == 0) {
    //     kprintf("[TEST] normalize: vol=%s path=%s\n", rp.vol->name, rp.path);
    // }
    // if (resolve_path("BOOT/GRUB.CFG", &rp) == 0) {
    //     kprintf("[TEST] relative: vol=%s path=%s\n", rp.vol->name, rp.path);
    // }

        /* fs 测试 */
        /* 内核早期 fs 测试：直调 fat32 */
    kprintf("[TEST] fat32 open/read:\n");
    {
        struct fat32_file f;
        if (fat32_open_file(sys_vol, "/SHELL.ELF", &f) == OK) {
            uint8_t buf[16];
            int r = fat32_read_file(sys_vol, &f, buf, 0, sizeof(buf));
            kprintf("  read %d bytes: %x %x %x %x\n",
                    r, buf[0], buf[1], buf[2], buf[3]);
        } else {
            kprintf("  open failed\n");
        }
    }

    kprintf("[TEST] fat32 readdir:\n");
    {
        struct fat32_dir d;
        struct dirent ent;
        if (fat32_opendir(sys_vol, "/", &d) == OK) {
            int n;
            while ((n = fat32_readdir(sys_vol, &d, &ent)) == 1) {
                kprintf("  %s%s\n", ent.name,
                        (ent.attributes & 0x10) ? "/" : "");
            }
            fat32_closedir(sys_vol, &d);
        } else {
            kprintf("  opendir failed\n");
        }
    }







    kprintf("[KERNEL] Processes created. Starting scheduler...\n");

    scheduler_start();

    while (1) __asm__("hlt");
}