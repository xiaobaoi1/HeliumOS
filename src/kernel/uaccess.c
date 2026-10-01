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

static int copy_bytes(uint32_t u_addr, void *kbuf, uint32_t n, int to_user) {
    if (n == 0) return 0;
    if (check_user_range(u_addr, n) < 0) return -1;

    struct task *cur = get_current_task();
    if (!cur || !cur->pgd) return -1;

    uint8_t *p = (uint8_t*)kbuf;
    while (n > 0) {
        uint32_t phys = vmm_get_phys(cur->pgd, u_addr & ~0xFFF);
        if (!phys) return -1;
        uint32_t off = u_addr & 0xFFF;
        uint32_t chunk = 0x1000 - off;
        if (chunk > n) chunk = n;

        if (to_user) {
            memcpy((void*)(phys + off), p, chunk);
        } else {
            memcpy(p, (void*)(phys + off), chunk);
        }
        u_addr += chunk;
        p += chunk;
        n -= chunk;
    }
    return 0;
}

int copy_from_user(void *dst, uint32_t u_addr, uint32_t n) {
    return copy_bytes(u_addr, dst, n, 0);
}

int copy_to_user(uint32_t u_addr, const void *src, uint32_t n) {
    return copy_bytes(u_addr, (void*)src, n, 1);
}