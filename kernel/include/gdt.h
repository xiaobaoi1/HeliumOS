#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* GDT 描述符结构（8 字节） */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

/* GDT 指针（用于 lgdt 指令） */
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* GDT 段选择子常量 */
#define GDT_KERNEL_CODE 0x08   /* 内核代码段 */
#define GDT_KERNEL_DATA 0x10   /* 内核数据段 */
#define GDT_USER_CODE   0x1B   /* 用户代码段 (RPL=3) */
#define GDT_USER_DATA   0x23   /* 用户数据段 (RPL=3) */
#define GDT_TSS         0x28   /* TSS 段 */

/* 初始化 GDT 和 TSS */
void gdt_init(void);

#endif