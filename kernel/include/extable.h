#ifndef EXTABLE_H
#define EXTABLE_H

#include <stdint.h>

struct extable_entry {
    uint32_t fault_addr;   /* 触发 fault 的指令地址 */
    uint32_t fixup_addr;   /* 出错时跳到这里 */
};

/* 查异常表。命中返回 fixup 地址；未命中返回 0。 */
uint32_t extable_lookup(uint32_t fault_eip);

#endif