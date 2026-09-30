#include <elf.h>
#include <fat32.h>
#include <pmm.h>
#include <vmm.h>
#include <printf.h>
#include <string.h>
#include <errno.h>

/* 取消注释可打开 ELF 加载调试日志 */
// #define DEBUG_ELF

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
            /* filesz 不能超过 memsz */
            if (ph.filesz > ph.memsz) {
                kprintf("[ELF] filesz > memsz\n");
                return 0;
            }
            /* vaddr + memsz 不能溢出，且不能超出用户空间 */
            if (ph.vaddr + ph.memsz < ph.vaddr) {
                kprintf("[ELF] vaddr + memsz overflow\n");
                return 0;
            }
            if (ph.vaddr + ph.memsz > USER_SPACE_END) {
                kprintf("[ELF] segment beyond user space\n");
                return 0;
            }
            if (ph.vaddr < USER_SPACE_START) continue;

            #ifdef DEBUG_ELF
            kprintf("[ELF] PT_LOAD: vaddr=0x%x filesz=0x%x memsz=0x%x pages=%d\n",
                    ph.vaddr, ph.filesz, ph.memsz, (ph.memsz + 4095) / 4096);
            #endif


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
                uint32_t addr = ph.vaddr + ph.filesz;
                uint32_t end  = ph.vaddr + ph.memsz;
                while (addr < end) {
                    uint32_t page_start = addr & ~0xFFF;
                    uint32_t phys = vmm_get_phys(pgd, page_start);
                    if (!phys) {
                        addr = page_start + 0x1000;
                        continue;
                    }
                    uint32_t page_end = page_start + 0x1000;
                    uint32_t chunk_end = (page_end < end) ? page_end : end;
                    memset((uint8_t*)phys + (addr - page_start), 0, chunk_end - addr);
                    addr = chunk_end;
                }
            }
        }
    }
    return header.entry;
}