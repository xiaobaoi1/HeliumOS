#include <uaccess.h>
#include <task.h>
#include <vmm.h>
#include <string.h>
#include <stddef.h>

int check_user_range(uint32_t addr, uint32_t size) {
    if (size == 0) return 0;
    if (addr < USER_SPACE_START) return -1;
    uint64_t end = (uint64_t)addr + size;
    if (end > (uint64_t)USER_SPACE_END + 1) return -1;

    struct task *cur = get_current_task();
    if (!cur || !cur->pgd) return -1;

    uint32_t first_page = addr & ~0xFFF;
    uint32_t last_page  = ((uint32_t)(end - 1)) & ~0xFFF;

    for (uint32_t p = first_page; p <= last_page; p += 4096) {
        if (!vmm_get_phys(cur->pgd, p)) return -1;
    }
    return 0;
}

int strncpy_from_user(char *dst, const char *src, uint32_t max_len) {
    if (!src || !dst || max_len == 0) return -1;
    if ((uint32_t)src < USER_SPACE_START) return -1;

    struct task *cur = get_current_task();
    if (!cur || !cur->pgd) return -1;

    uint32_t u_src = (uint32_t)src;
    uint32_t cur_page = u_src & ~0xFFF;
    if (!vmm_get_phys(cur->pgd, cur_page)) return -1;

    for (uint32_t i = 0; i < max_len; i++) {
        uint32_t addr = u_src + i;
        if (addr < USER_SPACE_START || addr > USER_SPACE_END) return -1;

        uint32_t page = addr & ~0xFFF;
        if (page != cur_page) {
            if (!vmm_get_phys(cur->pgd, page)) return -1;
            cur_page = page;
        }

        char c = src[i];
        dst[i] = c;
        if (c == '\0') return (int)(i + 1);
    }
    return -1;
}

/* 汇编实现：CPU 走 MMU，fault 由 __ex_table 恢复 */
extern int __copy_user(void *dst, uint32_t src, uint32_t n, int to_user);

int copy_from_user(void *dst, uint32_t u_addr, uint32_t n) {
    if (n == 0) return 0;
    if (!dst) return -1;
    return __copy_user(dst, u_addr, n, 0) == 0 ? 0 : -1;
}

int copy_to_user(uint32_t u_addr, const void *src, uint32_t n) {
    if (n == 0) return 0;
    if (!src) return -1;
    return __copy_user((void*)u_addr, (uint32_t)src, n, 1) == 0 ? 0 : -1;
}