#include <tss.h>
#include <gdt.h>
#include <printf.h>

struct tss_entry tss;

void tss_init(void) {
    kprintf("[TSS] Initializing...\n");

    /* 清零 TSS */
    for (int i = 0; i < sizeof(tss) / 4; i++) {
        ((uint32_t*)&tss)[i] = 0;
    }

    tss.ss0 = GDT_KERNEL_DATA;   /* 内核数据段 */
    tss.esp0 = 0;                /* 暂设为0，创建进程时更新 */

    /* 加载 TSS 到 CPU（使用 ltr 指令） */
    __asm__ volatile("ltr %%ax" :: "a"(GDT_TSS));

    kprintf("[TSS] Loaded.\n");
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}