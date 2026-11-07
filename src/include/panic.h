#ifndef PANIC_H
#define PANIC_H

#include <isr.h>

/* 内核致命错误：打印消息 + CR2/CR3 + 停止 CPU。不返回。 */
void panic(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

/* 带完整寄存器快照的 panic。regs 可为 NULL。 */
void panic_regs(const struct registers *regs, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));

#endif