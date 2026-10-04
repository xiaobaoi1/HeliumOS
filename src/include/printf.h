#ifndef PRINTF_H
#define PRINTF_H

#include <stdarg.h>

void kprintf(const char *fmt, ...);
void vkprintf(const char *fmt, va_list args);

/* 分级输出。
 * KLOG_DBG 默认编译期关闭；定义 KERNEL_DEBUG 打开。
 * KLOG_ERR / KLOG_WARN 永远输出。 */
#ifdef KERNEL_DEBUG
#define KLOG_DBG(fmt, ...)  kprintf("[DBG] " fmt, ##__VA_ARGS__)
#else
#define KLOG_DBG(fmt, ...)  ((void)0)
#endif

#define KLOG_ERR(fmt, ...)   kprintf("[ERR] " fmt, ##__VA_ARGS__)
#define KLOG_WARN(fmt, ...)  kprintf("[WARN] " fmt, ##__VA_ARGS__)

#endif