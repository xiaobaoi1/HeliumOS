#include <elf.h>
#include <fat32.h>
#include <pmm.h>
#include <vmm.h>
#include <printf.h>
#include <string.h>

uint32_t load_elf_from_disk(const char *path, uint32_t *pgd) {
    struct fat32_file file;
    if (!fat32_open_file(path, &file)) {
        kprintf("[ELF] Failed to open file: %s\n", path);
        return 0;
    }

    struct elf_header header;
    int ret = fat32_read_file(&file, (uint8_t*)&header, 0, sizeof(header));
    if (ret != sizeof(header)) {
        kprintf("[ELF] Failed to read ELF header (got %d)\n", ret);
        return 0;
    }
    if (header.magic != ELF_MAGIC) {
        kprintf("[ELF] Invalid ELF magic: 0x%x\n", header.magic);
        return 0;
    }
    kprintf("[ELF] Entry point: 0x%x, ph_count: %d\n", header.entry, header.ph_count);

    for (int i = 0; i < header.ph_count; i++) {
        struct elf_program_header ph;
        ret = fat32_read_file(&file, (uint8_t*)&ph, header.ph_offset + i * header.ph_entry_size, sizeof(ph));
        if (ret != sizeof(ph)) {
            kprintf("[ELF] Failed to read program header %d\n", i);
            return 0;
        }
        if (ph.type == PT_LOAD) {
            if (ph.vaddr < USER_SPACE_START) {
                kprintf("[ELF] Warning: segment at 0x%x below user space, skipping.\n", ph.vaddr);
                continue;
            }
            kprintf("[ELF] Loading segment: vaddr=0x%x, filesz=%d, memsz=%d\n", ph.vaddr, ph.filesz, ph.memsz);

            uint32_t pages_needed = (ph.memsz + 4095) / 4096;
            uint32_t virt_start = ph.vaddr & ~0xFFF;
            for (int j = 0; j < pages_needed; j++) {
                uint32_t phys = pmm_alloc_page();
                if (!phys) {
                    kprintf("[ELF] Failed to allocate page for segment.\n");
                    return 0;
                }
                vmm_map_user_page(pgd, virt_start + j * 4096, phys, PTE_WRITE | PTE_USER);
            }

            // 拷贝数据
            uint32_t offset = 0;
            while (offset < ph.filesz) {
                uint32_t virt_addr = ph.vaddr + offset;
                uint32_t page_offset = virt_addr & 0xFFF;
                uint8_t temp[SECTOR_SIZE];
                uint32_t read_size = (ph.filesz - offset) > SECTOR_SIZE ? SECTOR_SIZE : (ph.filesz - offset);
                int ret_read = fat32_read_file(&file, temp, ph.offset + offset, read_size);
                if (ret_read != read_size) {
                    kprintf("[ELF] FAT32 read error (got %d, expected %d)\n", ret_read, read_size);
                    return 0;
                }
                uint32_t phys = vmm_get_phys(pgd, virt_addr & ~0xFFF);
                if (!phys) {
                    kprintf("[ELF] Failed to get phys address for 0x%x\n", virt_addr);
                    return 0;
                }
                uint8_t *dest = (uint8_t*)(phys + page_offset);
                memcpy(dest, temp, read_size);
                offset += read_size;
            }

            // 清零剩余部分 (memsz > filesz)
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

    // 测试部分，不需要
    uint32_t test_phys = vmm_get_phys(pgd, 0x40000000);
    kprintf("[ELF] Verification: 0x40000000 -> phys 0x%x\n", test_phys);
    if (test_phys) {
        uint8_t *p = (uint8_t*)test_phys;
        kprintf("[ELF] First two bytes at 0x40000000: %x %x\n", p[0], p[1]);
    }

    return header.entry;
}