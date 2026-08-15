#ifndef ISR_H
#define ISR_H

#include <stdint.h>

/* 寄存器结构（由汇编推送的顺序） */
struct registers {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, user_esp, user_ss;
};

#endif