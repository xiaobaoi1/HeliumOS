#ifndef PANIC_H
#define PANIC_H

#include <isr.h>
#include <stdint.h>

/* 底层实现：渲染 + 停机。不直接调用，用下面的宏。 */
void panic_at(const char *file, uint32_t line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));
void panic_regs_at(const struct registers *regs,
                   const char *file, uint32_t line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 4, 5)));

/* 公开宏：自动填 __FILE__ 和 __LINE__ */
#define panic(fmt, ...) \
    panic_at(__FILE__, (uint32_t)__LINE__, fmt, ##__VA_ARGS__)

#define panic_regs(regs, fmt, ...) \
    panic_regs_at((regs), __FILE__, (uint32_t)__LINE__, fmt, ##__VA_ARGS__)

#endif