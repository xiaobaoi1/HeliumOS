#ifndef PRINTF_H
#define PRINTF_H

#include <stdarg.h>

// 内核主打印函数（输出到串口，后续可加 VGA）
void kprintf(const char *fmt, ...);

// 内部核心函数（支持自定义输出回调，供扩展使用）
void vkprintf(const char *fmt, va_list args);

#endif