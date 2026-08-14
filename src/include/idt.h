#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* 中断门描述符（8字节） */
struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t always0;
    uint8_t flags;
    uint16_t base_high;
} __attribute__((packed));

/* IDT指针（用于lidt指令） */
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* 初始化IDT并重映射PIC */
void idt_init(void);

/* 设置中断门 */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);

#endif