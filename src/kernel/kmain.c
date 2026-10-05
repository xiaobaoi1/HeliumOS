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
#include <rtc.h>
#include <acpi.h>
#include <pci.h>
#include <partition.h>
#include <panic.h>
#include <irq.h>
#include <vma.h>
#include <shm.h>
#include <net.h>
#include <rtl8139.h>
#include <net_proto.h>
#include <io.h>
#include <net_sock.h>
#include <tcp.h>
#include <kmap.h>


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
                                    SPAWN_FD_INHERIT,
                                    -1, -1, -1);

    if (!task) {
        kprintf("[KERNEL] Failed to create task for %s\n", path);
        return NULL;
    }

    return task;
}

/* ---------- 创建 IDLE 进程 ---------- */

void create_idle_task(struct fat32_volume *vol) {
    struct task *task = create_task_from_elf(vol, "/IDLE", 0, NULL);
    if (!task) {
        panic("cannot create idle task");
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
    struct task *task = create_task_from_elf(vol, "/SHELL", 3, (char *const*)shell_env);
    if (!task) {
        panic("cannot create shell task\n");
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
    irq_init();
    serial_init();
    screen_init();
    tty_init();
    ipc_init();

    kprintf("========================================\n");
    kprintf(" HeliumOS Kernel Started (1GB/3GB)\n");
    kprintf("========================================\n");

    pmm_init(addr);
    heap_init();
    vma_init();
    shm_init();
    gdt_init();
    tss_init();
    vmm_init();
    kmap_init();
    idt_init();
    rtc_init();
    acpi_init(addr);

    volume_init();
    fs_init();

    keyboard_init();
    serial_register_dev();

    pci_init();
    ata_init();
    net_init();
    rtl8139_init();
    net_proto_init();
    net_sock_init();
    tcp_init();

    /* 遍历所有 ATA 设备的所有 MBR 主分区，尝试挂载 FAT32。
     * 第一个成功的是 SYS，其余按 A/B/C... 编号。 */
    struct fat32_volume *sys_vol = NULL;
    int mounted = 0;

    for (int d = 0; d < ata_device_count(); d++) {
        const struct ata_device *dev = ata_get_device(d);
        if (!dev) continue;

        struct mbr_partition parts[MBR_MAX_PRIMARY];
        int np = mbr_parse(dev, parts, MBR_MAX_PRIMARY);
        if (np == 0) {
            kprintf("[KERNEL] Device %d: no MBR partition\n", d);
            continue;
        }

        for (int i = 0; i < np; i++) {
            /* 只尝试 FAT32 类型（0x0B / 0x0C）。
             * 其他类型不是 FAT32，跳过，不打日志。 */
            if (parts[i].type != 0x0B && parts[i].type != 0x0C) {
                continue;
            }
            
            struct fat32_volume *vol = fat32_mount(dev, parts[i].start_lba);
            if (!vol) continue;

            char name[VOL_NAME_LEN];
            if (mounted == 0) {
                name[0] = 'S'; name[1] = 'Y'; name[2] = 'S'; name[3] = '\0';
                sys_vol = vol;
            } else if (mounted - 1 < 26) {
                name[0] = 'A' + (char)(mounted - 1);
                name[1] = '\0';
            } else {
                /* 超过 26 个卷——VOL_MAX 是 8，不会到这里 */
                continue;
            }

            volume_register(name, VOL_FS_FAT32, parts[i].start_lba,
                            parts[i].sector_count, vol);
            mounted++;
        }
    }

    if (!sys_vol) {
        panic("no FAT32 volume found");
    }

    create_idle_task(sys_vol);
    create_shell_task(sys_vol);

    kprintf("[KERNEL] Processes created. Starting scheduler...\n");

    // kprintf("[TEST] size of task: %d\n", sizeof(struct task));

        /* 构造 ICMP echo request 到网关 10.0.2.2 */

    kprintf("[TASK] sizeof(task)=%d\n", sizeof(struct task));
    if(sizeof(struct task) > PAGE_SIZE){
        panic("size of task (%u) bigger than PAGE_SIZE(4096)", sizeof(struct task));
    }
    

    scheduler_start();

    while (1) __asm__("hlt");
}