#include <idt.h>
#include <io.h>
#include <printf.h>
#include <string.h>

/* 外部符号：中断入口表（由汇编提供） */
extern uint32_t isr_entry_table[32];
extern uint32_t irq_entry_table[16];
extern uint32_t isr80;  // int 0x80 入口

static struct idt_entry idt[256];
static struct idt_ptr idt_ptr;

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel       = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

static void idt_load(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt;
    __asm__ volatile("lidt (%0)" :: "r"(&idt_ptr));
}

/* 初始化PIC（8259A）重映射 */
static void pic_init(void) {
    __asm__ volatile(
        /* ICW1: 初始化，边缘触发，级联 */
        "mov $0x11, %%al\n"
        "out %%al, $0x20\n"
        "out %%al, $0xA0\n"

        /* ICW2: 主PIC偏移0x20 (32)，从PIC偏移0x28 (40) */
        "mov $0x20, %%al\n"
        "out %%al, $0x21\n"
        "mov $0x28, %%al\n"
        "out %%al, $0xA1\n"

        /* ICW3: 主PIC连接从片 */
        "mov $0x04, %%al\n"
        "out %%al, $0x21\n"
        "mov $0x02, %%al\n"
        "out %%al, $0xA1\n"

        /* ICW4: 8086模式 */
        "mov $0x01, %%al\n"
        "out %%al, $0x21\n"
        "out %%al, $0xA1\n"

        /* 暂时屏蔽所有中断（OCW1） */
        "mov $0xFF, %%al\n"
        "out %%al, $0x21\n"
        "out %%al, $0xA1\n"
        ::: "eax", "memory"
    );
}

void idt_init(void) {
    kprintf("[IDT] Initializing...\n");

    /* 清空IDT */
    memset(idt, 0, sizeof(idt));

    /* 设置异常门 0-31 (Ring 0) */
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, isr_entry_table[i], 0x08, 0x8E);
    }

    /* 设置IRQ门 32-47 (Ring 0) */
    for (int i = 0; i < 16; i++) {
        idt_set_gate(32 + i, irq_entry_table[i], 0x08, 0x8E);
    }

    /* 设置系统调用门 int 0x80 (Ring 3 允许) */
    idt_set_gate(0x80, (uint32_t)&isr80, 0x08, 0xEE);  // P=1, DPL=3, 32位中断门

    /* 加载IDT */
    idt_load();

    /* 初始化PIC */
    pic_init();

    kprintf("[IDT] Initialization complete.\n");
}

/* 取消屏蔽特定IRQ（方便后续启用） */
void pic_unmask_irq(uint8_t irq) {
    uint16_t port;
    uint8_t mask;
    if (irq < 8) {
        port = 0x21;
        mask = inb(port) & ~(1 << irq);
    } else {
        port = 0xA1;
        mask = inb(port) & ~(1 << (irq - 8));
    }
    outb(port, mask);
}