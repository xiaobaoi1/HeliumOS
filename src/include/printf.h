#ifndef PRINTF_H
#define PRINTF_H

#include <stdarg.h>
#include <stddef.h>

void kprintf(const char *fmt, ...);
void vkprintf(const char *fmt, va_list args);

/* 格式化到字符串。返回需要的长度（不含 '\0'），
 * 与标准 C snprintf 一致。size==0 时不写 buf。 */
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

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