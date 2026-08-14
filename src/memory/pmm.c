#include <memory.h>
#include <multiboot2.h>
#include <serial.h>
#include <stddef.h>

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define MAX_PHYS_MEM (128 * 1024 * 1024)
#define MAX_PAGES (MAX_PHYS_MEM / PAGE_SIZE)
#define BITMAP_SIZE (MAX_PAGES / 8 + 1)

static uint8_t bitmap[BITMAP_SIZE];
static uint32_t free_page_count = 0;

static inline void bitmap_set(uint32_t idx) { bitmap[idx/8] |= (1 << (idx%8)); }
static inline void bitmap_clear(uint32_t idx) { bitmap[idx/8] &= ~(1 << (idx%8)); }
static inline uint8_t bitmap_test(uint32_t idx) { return (bitmap[idx/8] >> (idx%8)) & 1; }

// 辅助：输出十六进制数字（临时，后续会用 printf）
static void print_hex(uint32_t val) {
    char hex[9];
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (val >> (i*4)) & 0xF;
        hex[7-i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    hex[8] = '\0';
    serial_write_string(hex);
}

static void print_hex64(uint64_t val) {
    char hex[17];
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (val >> (i*4)) & 0xF;
        hex[15-i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    hex[16] = '\0';
    serial_write_string(hex);
}

void pmm_init(uint32_t multiboot_info_addr) {
    serial_write_string("[PMM] Initializing...\n");

    // 1. 默认全部标记为已用
    for (uint32_t i = 0; i < MAX_PAGES; i++) bitmap_set(i);
    free_page_count = 0;

    // 2. 获取 Multiboot2 信息结构
    struct multiboot2_info *info = (struct multiboot2_info*)multiboot_info_addr;
    serial_write_string("[PMM] info->total_size = ");
    print_hex(info->total_size);
    serial_write_string("\n");

    uint8_t *ptr = (uint8_t*)multiboot_info_addr + sizeof(struct multiboot2_info);
    uint8_t *end = (uint8_t*)multiboot_info_addr + info->total_size;

    int found_mmap = 0;

    while (ptr < end) {
        struct multiboot2_tag *tag = (struct multiboot2_tag*)ptr;
        // 打印调试信息
        serial_write_string("[PMM] Tag type = ");
        print_hex(tag->type);
        serial_write_string(", size = ");
        print_hex(tag->size);
        serial_write_string("\n");

        if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
            found_mmap = 1;
            serial_write_string("[PMM] Found MMAP tag.\n");
            struct multiboot2_tag_mmap *mmap_tag = (struct multiboot2_tag_mmap*)tag;
            uint32_t entry_size = mmap_tag->entry_size;
            uint32_t entry_version = mmap_tag->entry_version;
            serial_write_string("[PMM] entry_size = ");
            print_hex(entry_size);
            serial_write_string(", version = ");
            print_hex(entry_version);
            serial_write_string("\n");

            // entries 起始地址（跳过 tag 头和 entry_size/version）
            uint8_t *entry_ptr = (uint8_t*)mmap_tag + sizeof(struct multiboot2_tag_mmap);
            uint8_t *entry_end = (uint8_t*)mmap_tag + mmap_tag->size;

            while (entry_ptr < entry_end) {
                struct multiboot2_mmap_entry *entry = (struct multiboot2_mmap_entry*)entry_ptr;
                if (entry->type == 1) {
                    uint64_t base = entry->addr;
                    uint64_t len = entry->len;
                    serial_write_string("[PMM] Available: base=");
                    print_hex64(base);
                    serial_write_string(", len=");
                    print_hex64(len);
                    serial_write_string("\n");
                    // 对齐到页边界
                    uint64_t start = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    uint64_t end_phys = (base + len) & ~(PAGE_SIZE - 1);
                    for (uint64_t addr = start; addr < end_phys; addr += PAGE_SIZE) {
                        if (addr < MAX_PHYS_MEM) {
                            uint32_t idx = addr >> PAGE_SHIFT;
                            if (bitmap_test(idx) == 1) {
                                bitmap_clear(idx);
                                free_page_count++;
                            }
                        }
                    }
                } else {
                    serial_write_string("[PMM] Reserved region, type=");
                    print_hex(entry->type);
                    serial_write_string("\n");
                }
                entry_ptr += entry_size; // 按 entry_size 步进
            }
            break; // 找到 MMAP 后退出循环
        }
        ptr += tag->size;
        while ((uintptr_t)ptr % 8 != 0) ptr++;
    }

    // 3. 标记内核自身占用的页
    extern uint32_t _start, _end;
    uint32_t kernel_start = (uint32_t)&_start;
    uint32_t kernel_end = (uint32_t)&_end;
    uint32_t start_page = kernel_start >> PAGE_SHIFT;
    uint32_t end_page = (kernel_end + PAGE_SIZE - 1) >> PAGE_SHIFT;
    for (uint32_t i = start_page; i < end_page; i++) {
        if (bitmap_test(i) == 0) {
            bitmap_set(i);
            free_page_count--;
        }
    }

    // 4. 标记位图本身
    uint32_t bitmap_start = (uint32_t)bitmap;
    uint32_t bitmap_end = bitmap_start + BITMAP_SIZE;
    for (uint32_t addr = bitmap_start; addr < bitmap_end; addr += PAGE_SIZE) {
        uint32_t idx = addr >> PAGE_SHIFT;
        if (bitmap_test(idx) == 0) {
            bitmap_set(idx);
            free_page_count--;
        }
    }

    serial_write_string("[PMM] Done. Free pages: ");
    char buf[16];
    int i = 0;
    uint32_t num = free_page_count;
    if (num == 0) buf[i++] = '0';
    else {
        char tmp[16]; int t = 0;
        while (num > 0) { tmp[t++] = '0' + (num % 10); num /= 10; }
        while (t > 0) { buf[i++] = tmp[--t]; }
    }
    buf[i] = '\0';
    serial_write_string(buf);
    serial_write_string("\n");
}

uint32_t pmm_alloc_page(void) {
    for (uint32_t i = 0; i < MAX_PAGES; i++) {
        if (bitmap_test(i) == 0) {
            bitmap_set(i);
            free_page_count--;
            return i << PAGE_SHIFT;
        }
    }
    serial_write_string("[PMM] ERROR: Out of physical memory!\n");
    return 0;
}

void pmm_free_page(uint32_t phys_addr) {
    uint32_t idx = phys_addr >> PAGE_SHIFT;
    if (idx < MAX_PAGES && bitmap_test(idx) == 1) {
        bitmap_clear(idx);
        free_page_count++;
    }
}

uint32_t pmm_get_free_count(void) {
    return free_page_count;
}