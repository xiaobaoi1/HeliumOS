#include <gdt.h>
#include <tss.h>
#include <printf.h>
#include <string.h>

static struct gdt_entry gdt[6];  /* 6 个描述符 */
static struct gdt_ptr gp;

/* 设置一个 GDT 描述符 */
static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access      = access;
}

void gdt_init(void) {
    kprintf("[GDT] Initializing...\n");

    /* NULL 描述符（必须） */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* 内核代码段 (Ring 0) */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* 内核数据段 (Ring 0) */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* 用户代码段 (Ring 3) */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    /* 用户数据段 (Ring 3) */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* TSS 段（将在 tss_init 中填充） */
    gdt_set_gate(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

    /* 加载 GDT */
    gp.limit = sizeof(gdt) - 1;
    gp.base = (uint32_t)gdt;
    __asm__ volatile("lgdt (%0)" :: "r"(&gp));

    /* 刷新段寄存器 */
    __asm__ volatile(
        "mov $0x10, %%ax;"
        "mov %%ax, %%ds;"
        "mov %%ax, %%es;"
        "mov %%ax, %%fs;"
        "mov %%ax, %%gs;"
        "mov %%ax, %%ss;"
        "ljmp $0x08, $1f;"
        "1:"
        :::
        "eax", "memory"
    );

    kprintf("[GDT] Loaded.\n");
}