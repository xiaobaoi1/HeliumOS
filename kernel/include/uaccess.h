#ifndef UACCESS_H
#define UACCESS_H

#include <stdint.h>

/* 检查 [addr, addr+size) 是否完全在用户空间且已映射。
 * 用 current_task->pgd。
 * 返回 0 成功，-1 失败。 */
int check_user_range(uint32_t addr, uint32_t size);

/* 从用户空间拷贝字符串到内核缓冲。跨页时逐页检查。
 * 返回拷贝的字节数（含 '\0'），失败返回 -1。 */
int strncpy_from_user(char *dst, const char *src, uint32_t max_len);

/* 定长拷贝：用户 → 内核。返回 0 成功，-1 失败。 */
int copy_from_user(void *dst, uint32_t u_addr, uint32_t n);

/* 定长拷贝：内核 → 用户。返回 0 成功，-1 失败。 */
int copy_to_user(uint32_t u_addr, const void *src, uint32_t n);

/* 判断一个未映射页是否在合法可分配范围内（heap 或 VMA）。
 * 返回 1 合法，0 非法。 */
int is_legal_user_page(uint32_t page);

#endif