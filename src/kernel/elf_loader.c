#include <elf.h>
#include <fat32.h>
#include <pmm.h>
#include <vmm.h>
#include <printf.h>
#include <string.h>

/* 从 FAT32 文件加载 ELF 到内存并返回入口点 */
uint32_t load_elf_from_disk(const char *path, uint32_t *pgd) {
    struct fat32_file file;
    if (!fat32_open_file(path, &file)) {
        kprintf("[ELF] Failed to open file: %s\n", path);
        return 0;
    }

    // 读取 ELF 头部
    struct elf_header header;
    fat32_read_file(&file, (uint8_t*)&header, 0, sizeof(header));
    if (header.magic != ELF_MAGIC) {
        kprintf("[ELF] Invalid ELF magic: 0x%x\n", header.magic);
        return 0;
    }
    kprintf("[ELF] Entry point: 0x%x, ph_count: %d\n", header.entry, header.ph_count);

    // 遍历程序头，加载 PT_LOAD 段
    for (int i = 0; i < header.ph_count; i++) {
        struct elf_program_header ph;
        fat32_read_file(&file, (uint8_t*)&ph, header.ph_offset + i * header.ph_entry_size, sizeof(ph));
        if (ph.type == PT_LOAD) {
            kprintf("[ELF] Loading segment: vaddr=0x%x, filesz=%d, memsz=%d\n", ph.vaddr, ph.filesz, ph.memsz);
            // 为这个段分配物理页并映射到用户空间
            uint32_t pages_needed = (ph.memsz + 4095) / 4096;
            uint32_t virt = ph.vaddr & ~0xFFF;
            for (int j = 0; j < pages_needed; j++) {
                uint32_t phys = pmm_alloc_page();
                if (!phys) {
                    kprintf("[ELF] Failed to allocate page for segment.\n");
                    return 0;
                }
                vmm_map_user_page(pgd, virt + j * 4096, phys, PTE_WRITE | PTE_USER);
            }
            // 拷贝数据
            uint32_t offset = 0;
            while (offset < ph.filesz) {
                uint32_t virt_addr = ph.vaddr + offset;
                uint32_t page_offset = virt_addr & 0xFFF;
                // 读取到临时缓冲区再写入（因为内核直接映射物理地址）
                uint8_t temp[SECTOR_SIZE];
                uint32_t read_size = (ph.filesz - offset) > SECTOR_SIZE ? SECTOR_SIZE : (ph.filesz - offset);
                fat32_read_file(&file, temp, ph.offset + offset, read_size);
                // 计算物理地址并写入
                uint32_t phys = vmm_get_phys(pgd, virt_addr & ~0xFFF);
                if (!phys) {
                    kprintf("[ELF] Failed to get phys address for 0x%x\n", virt_addr);
                    return 0;
                }
                uint8_t *dest = (uint8_t*)(phys + page_offset);
                memcpy(dest, temp, read_size);
                offset += read_size;
            }
            // 如果 memsz > filesz，剩余部分清零
            if (ph.memsz > ph.filesz) {
                uint32_t zero_start = ph.vaddr + ph.filesz;
                uint32_t zero_end = ph.vaddr + ph.memsz;
                for (uint32_t addr = zero_start; addr < zero_end; addr += 4) {
                    uint32_t phys = vmm_get_phys(pgd, addr & ~0xFFF);
                    if (phys) {
                        uint8_t *dest = (uint8_t*)(phys + (addr & 0xFFF));
                        *dest = 0;
                    }
                }
            }
        }
    }
    return header.entry;
}