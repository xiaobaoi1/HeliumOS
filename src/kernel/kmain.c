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
#include <device.h>
#include <keyboard.h>
#include <tty.h>
#include <ipc.h>


static const char *shell_env[] = {
    "PATH=SYS:/",
    "HOME=SYS:/",
    "TERM=helium",
    NULL,
};

/* ---------- 内部辅助：从一个 ELF 路径创建进程 ---------- */

static struct task *create_task_from_elf(struct fat32_volume *vol,
                                          const char *path,
                                          int envc, char *const envp[]) {
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
    struct task *task = task_create(entry, pgd, 0, NULL,
                                    envc, envp,
                                    SPAWN_FD_INHERIT,
                                    SPAWN_FD_INHERIT,
                                    SPAWN_FD_INHERIT);

    if (!task) {
        kprintf("[KERNEL] Failed to create task for %s\n", path);
        return NULL;
    }

    return task;
}

/* ---------- 创建 IDLE 进程 ---------- */

void create_idle_task(struct fat32_volume *vol) {
    struct task *task = create_task_from_elf(vol, "/IDLE.ELF", 0, NULL);
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
    struct task *task = create_task_from_elf(vol, "/SHELL.ELF", 3, (char *const*)shell_env);
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
    dev_init();
    serial_init();
    screen_init();
    tty_init();
    ipc_init();

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

    keyboard_init();
    serial_register_dev();

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



    kprintf("[KERNEL] Processes created. Starting scheduler...\n");

    scheduler_start();

    while (1) __asm__("hlt");
}