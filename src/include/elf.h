#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <fat32.h>


#define ELF_MAGIC 0x464C457F  // "\x7FELF"

/* ELF 头部 */
struct elf_header {
    uint32_t magic;
    uint8_t  bitness;
    uint8_t  endian;
    uint8_t  version1;
    uint8_t  os_abi;
    uint8_t  abi_version;
    uint8_t  padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t ph_offset;
    uint32_t sh_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t ph_entry_size;
    uint16_t ph_count;
    uint16_t sh_entry_size;
    uint16_t sh_count;
    uint16_t sh_string_index;
} __attribute__((packed));

/* 程序头 (Program Header) */
struct elf_program_header {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} __attribute__((packed));

uint32_t load_elf_from_disk(struct fat32_volume *vol, const char *path,
                            uint32_t *pgd);
                            
#define PT_LOAD 1

#endif