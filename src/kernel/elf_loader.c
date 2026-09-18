#include <elf.h>
#include <fat32.h>
#include <pmm.h>
#include <vmm.h>
#include <printf.h>
#include <string.h>
#include <errno.h>

uint32_t load_elf_from_disk(struct fat32_volume *vol, const char *path,
                            uint32_t *pgd) {
    struct fat32_file file;
    if (fat32_open_file(vol, path, &file) != OK) {
        kprintf("[ELF] Failed to open file: %s\n", path);
        return 0;
    }

    struct elf_header header;
    int ret = fat32_read_file(vol, &file, (uint8_t*)&header, 0, sizeof(header));
    if (ret != (int)sizeof(header)) {
        kprintf("[ELF] Failed to read ELF header\n");
        return 0;
    }
    if (header.magic != ELF_MAGIC) {
        kprintf("[ELF] Invalid ELF magic: 0x%x\n", header.magic);
        return 0;
    }

    for (int i = 0; i < header.ph_count; i++) {
        struct elf_program_header ph;
        ret = fat32_read_file(vol, &file, (uint8_t*)&ph,
                              header.ph_offset + i * header.ph_entry_size,
                              sizeof(ph));
        if (ret != (int)sizeof(ph)) return 0;

        if (ph.type == PT_LOAD) {
            if (ph.vaddr < USER_SPACE_START) continue;

            uint32_t pages_needed = (ph.memsz + 4095) / 4096;
            uint32_t virt_start = ph.vaddr & ~0xFFF;

            for (uint32_t j = 0; j < pages_needed; j++) {
                uint32_t phys = pmm_alloc_page();
                if (!phys) return 0;
                vmm_map_user_page(pgd, virt_start + j * 4096, phys,
                                  PTE_WRITE | PTE_USER);
            }

            uint32_t offset = 0;
            while (offset < ph.filesz) {
                uint32_t virt_addr = ph.vaddr + offset;
                uint32_t page_offset = virt_addr & 0xFFF;
                uint8_t temp[SECTOR_SIZE];
                uint32_t read_size = (ph.filesz - offset) > SECTOR_SIZE
                                   ? SECTOR_SIZE : (ph.filesz - offset);
                int r = fat32_read_file(vol, &file, temp,
                                        ph.offset + offset, read_size);
                if (r != (int)read_size) return 0;

                uint32_t phys = vmm_get_phys(pgd, virt_addr & ~0xFFF);
                if (!phys) return 0;
                memcpy((uint8_t*)(phys + page_offset), temp, read_size);
                offset += read_size;
            }

            /* .bss 清零 */
            if (ph.memsz > ph.filesz) {
                for (uint32_t addr = ph.vaddr + ph.filesz;
                     addr < ph.vaddr + ph.memsz; addr += 4) {
                    uint32_t phys = vmm_get_phys(pgd, addr & ~0xFFF);
                    if (phys) {
                        *(uint8_t*)(phys + (addr & 0xFFF)) = 0;
                    }
                }
            }
        }
    }
    return header.entry;
}