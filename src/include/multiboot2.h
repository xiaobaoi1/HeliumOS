#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

// Multiboot2 信息结构（GRUB 传入）
struct multiboot2_info {
    uint32_t total_size;
    uint32_t reserved;
};

// 通用标签头
struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
};

// 内存映射标签（type = 6）
struct multiboot2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;    // 每个条目的大小（至少 24）
    uint32_t entry_version; // 应为 0
    // 后面紧跟着 entries，每个 entry 是 struct multiboot2_mmap_entry
};

// 内存映射条目结构（规范定义）
struct multiboot2_mmap_entry {
    uint64_t addr;   // 基址
    uint64_t len;    // 长度
    uint32_t type;   // 1 = 可用 RAM
    uint32_t reserved;
};

// 标签类型常量
#define MULTIBOOT2_TAG_TYPE_END     0
#define MULTIBOOT2_TAG_TYPE_MMAP    6
#define MULTIBOOT2_TAG_TYPE_ACPI_OLD  14
#define MULTIBOOT2_TAG_TYPE_ACPI_NEW  15

#endif