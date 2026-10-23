#include "dlfcn.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include <stdint.h>
#include <stddef.h>
#include "stdio.h"

/* ---------- ELF 32 位结构（只定义加载器需要的） ---------- */

#define EI_NIDENT 16

typedef struct {
    unsigned char  e_ident[EI_NIDENT];
    uint16_t       e_type;
    uint16_t       e_machine;
    uint32_t       e_version;
    uint32_t       e_entry;
    uint32_t       e_phoff;
    uint32_t       e_shoff;
    uint32_t       e_flags;
    uint16_t       e_ehsize;
    uint16_t       e_phentsize;
    uint16_t       e_phnum;
    uint16_t       e_shentsize;
    uint16_t       e_shnum;
    uint16_t       e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

typedef struct {
    int32_t  d_tag;
    uint32_t d_val;
} Elf32_Dyn;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;

/* e_type */
#define ET_DYN 3

/* p_type */
#define PT_LOAD    1
#define PT_DYNAMIC 2

/* d_tag */
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_REL      17
#define DT_RELSZ    18
#define DT_JMPREL   23

/* r_info 编码 */
#define R_SYM(r_info)  ((r_info) >> 8)
#define R_TYPE(r_info) ((r_info) & 0xFF)

#define R_386_32      1
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8

/* ---------- 加载器状态 ---------- */

#define DL_MAX_LIBS 8

struct loaded_lib {
    int used;
    char path[64];
    uint32_t base;              /* mmap 返回的基址 */
    uint32_t bias;              /* base - p_vaddr（p_vaddr 通常 0） */

    uint32_t dyn_addr;          /* .dynamic 在进程内的地址 */
    uint32_t dyn_size;

    uint32_t symtab_addr;
    uint32_t strtab_addr;

    int refcount;
};

static struct loaded_lib g_libs[DL_MAX_LIBS];
static char g_errbuf[128];
static int  g_has_err = 0;

static void set_error(const char *msg) {
    strncpy(g_errbuf, msg, sizeof(g_errbuf) - 1);
    g_errbuf[sizeof(g_errbuf) - 1] = '\0';
    g_has_err = 1;
}

char *dlerror(void) {
    if (!g_has_err) return NULL;
    g_has_err = 0;
    return g_errbuf;
}

/* ---------- 按路径查已加载 ---------- */

static struct loaded_lib *find_lib(const char *path) {
    for (int i = 0; i < DL_MAX_LIBS; i++) {
        if (g_libs[i].used && strcmp(g_libs[i].path, path) == 0)
            return &g_libs[i];
    }
    return NULL;
}

/* ---------- 从 ELF 数据读 ---------- */

static uint32_t get_dyn(struct loaded_lib *lib, int32_t tag) {
    Elf32_Dyn *dyn = (Elf32_Dyn*)lib->dyn_addr;
    int n = lib->dyn_size / sizeof(Elf32_Dyn);
    for (int i = 0; i < n; i++) {
        if (dyn[i].d_tag == DT_NULL) break;
        if (dyn[i].d_tag == tag) return dyn[i].d_val;
    }
    return 0;
}

/* ---------- 符号查找（只在单个 handle 内） ---------- */

static const Elf32_Sym *find_sym_in_lib(struct loaded_lib *lib, const char *name) {
    if (!lib->symtab_addr || !lib->strtab_addr) return NULL;

    /* .dynsym 的大小：从 strtab_addr - symtab_addr 不一定准确，
     * 但 GNU hash 或者遍历到 st_name == 0 且 st_value == 0 的哨兵。
     * 第一版简化：遍历 symtab，直到名字为空且 value 为 0。 */
    Elf32_Sym *sym = (Elf32_Sym*)lib->symtab_addr;
    const char *strtab = (const char*)lib->strtab_addr;

    for (int i = 1; i < 512; i++) {   /* 上限保护 */
        if (sym[i].st_name == 0 && sym[i].st_value == 0 &&
            sym[i].st_size == 0 && sym[i].st_info == 0)
            break;
        const char *sname = strtab + sym[i].st_name;
        if (strcmp(sname, name) == 0)
            return &sym[i];
    }
    return NULL;
}

/* ---------- 重定位 ---------- */

static int apply_relocs(struct loaded_lib *lib, uint32_t rel_addr,
                        uint32_t rel_size) {
    if (!rel_addr || !rel_size) return 0;

    Elf32_Rel *rel = (Elf32_Rel*)rel_addr;
    int n = rel_size / sizeof(Elf32_Rel);
    Elf32_Sym *symtab = (Elf32_Sym*)lib->symtab_addr;
    const char *strtab = (const char*)lib->strtab_addr;

    for (int i = 0; i < n; i++) {
        uint32_t r_offset = rel[i].r_offset;
        uint32_t r_info   = rel[i].r_info;
        uint32_t type     = R_TYPE(r_info);
        uint32_t symidx   = R_SYM(r_info);

        uint32_t *loc = (uint32_t*)(lib->bias + r_offset);

        if (type == R_386_RELATIVE) {
            uint32_t addend = *loc;
            *loc = lib->bias + addend;
        } else if (type == R_386_GLOB_DAT || type == R_386_JMP_SLOT) {
            const char *sname = strtab + symtab[symidx].st_name;
            /* 先在当前库找，找不到再查依赖链——第一版只查当前库 */
            const Elf32_Sym *sym = find_sym_in_lib(lib, sname);
            if (!sym) {
                dprintf(2, "[dl] unresolved: %s\n", sname);
                return -1;
            }
            *loc = lib->bias + sym->st_value;
        } else {
            /* 其他类型忽略——-fPIC 的简单库通常只有上面三种 */
        }
    }
    return 0;
}

/* ---------- dlopen ---------- */

void *dlopen(const char *path, int mode) {
    (void)mode;   /* 第一版只做 RTLD_NOW 等价行为 */

    if (!path) { set_error("null path"); return NULL; }

    struct loaded_lib *existing = find_lib(path);
    if (existing) {
        existing->refcount++;
        return existing;
    }

    int slot = -1;
    for (int i = 0; i < DL_MAX_LIBS; i++) {
        if (!g_libs[i].used) { slot = i; break; }
    }
    if (slot < 0) { set_error("too many libs"); return NULL; }

    int fd = fs_open(path, FS_O_RDONLY);
    if (fd < 0) { set_error("cannot open file"); return NULL; }

    Elf32_Ehdr eh;
    if (fs_read(fd, &eh, sizeof(eh)) != (int)sizeof(eh)) {
        fs_close(fd); set_error("cannot read ELF header"); return NULL;
    }
    if (eh.e_ident[0] != 0x7F || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L'  || eh.e_ident[3] != 'F') {
        fs_close(fd); set_error("not ELF"); return NULL;
    }
    if (eh.e_type != ET_DYN) {
        fs_close(fd); set_error("not ET_DYN"); return NULL;
    }
    if (eh.e_phnum == 0 || eh.e_phnum > 16) {
        fs_close(fd); set_error("bad phnum"); return NULL;
    }

    /* 读 program headers */
    Elf32_Phdr ph[16];
    uint32_t ph_bytes = eh.e_phnum * sizeof(Elf32_Phdr);
    if (fs_seek(fd, eh.e_phoff) != 0) {
        fs_close(fd); set_error("seek phdr failed"); return NULL;
    }
    if (fs_read(fd, ph, ph_bytes) != (int)ph_bytes) {
        fs_close(fd); set_error("read phdr failed"); return NULL;
    }

    /* 第一遍：计算所有 PT_LOAD 的虚拟地址范围 */
    struct loaded_lib *lib = &g_libs[slot];
    memset(lib, 0, sizeof(*lib));
    lib->used = 1;
    strncpy(lib->path, path, sizeof(lib->path) - 1);

    uint32_t min_vaddr = 0xFFFFFFFF;
    uint32_t max_vaddr_end = 0;

    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (ph[i].p_vaddr < min_vaddr) min_vaddr = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > max_vaddr_end)
            max_vaddr_end = ph[i].p_vaddr + ph[i].p_memsz;
    }

    if (min_vaddr == 0xFFFFFFFF || max_vaddr_end <= min_vaddr) {
        fs_close(fd); lib->used = 0;
        set_error("no PT_LOAD"); return NULL;
    }

    /* 一次性 mmap 整个库的连续地址空间 */
    uint32_t total_size = (max_vaddr_end - min_vaddr + 0xFFF) & ~0xFFFu;
    unsigned int base = mmap(0, total_size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1);
    if ((int)base <= 0) {
        fs_close(fd); lib->used = 0;
        set_error("mmap failed"); return NULL;
    }

    lib->base = base;
    lib->bias = base - min_vaddr;

    /* 第二遍：把数据读入连续的内存块 */
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0) continue;

        uint8_t *dst = (uint8_t*)(lib->bias + ph[i].p_vaddr);

        if (fs_seek(fd, ph[i].p_offset) != 0) {
            fs_close(fd); lib->used = 0;
            set_error("seek segment failed"); return NULL;
        }

        uint32_t done = 0;
        while (done < ph[i].p_filesz) {
            uint32_t chunk = ph[i].p_filesz - done;
            if (chunk > 4096) chunk = 4096;
            int n = fs_read(fd, dst + done, chunk);
            if (n <= 0) break;
            done += n;
        }
    }

    /* 第二遍：找 PT_DYNAMIC */
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_DYNAMIC) continue;
        lib->dyn_addr = lib->bias + ph[i].p_vaddr;
        lib->dyn_size = ph[i].p_memsz;
        break;
    }
    if (!lib->dyn_addr) {
        fs_close(fd); lib->used = 0;
        set_error("no PT_DYNAMIC"); return NULL;
    }

    /* 解析 .dynamic 拿 symtab/strtab */
    uint32_t dt_symtab = get_dyn(lib, DT_SYMTAB);
    uint32_t dt_strtab = get_dyn(lib, DT_STRTAB);
    if (dt_symtab) lib->symtab_addr = lib->bias + dt_symtab;
    if (dt_strtab) lib->strtab_addr = lib->bias + dt_strtab;

    /* 处理 .rel.dyn */
    uint32_t dt_rel    = get_dyn(lib, DT_REL);
    uint32_t dt_relsz  = get_dyn(lib, DT_RELSZ);
    if (apply_relocs(lib, dt_rel ? lib->bias + dt_rel : 0, dt_relsz) < 0) {
        fs_close(fd); lib->used = 0;
        set_error("reloc failed"); return NULL;
    }

    /* 处理 .rel.plt */
    uint32_t dt_jmprel   = get_dyn(lib, DT_JMPREL);
    uint32_t dt_pltrelsz = get_dyn(lib, DT_PLTRELSZ);
    if (apply_relocs(lib, dt_jmprel ? lib->bias + dt_jmprel : 0, dt_pltrelsz) < 0) {
        fs_close(fd); lib->used = 0;
        set_error("plt reloc failed"); return NULL;
    }

    /* DT_NEEDED：第一版不支持依赖链（库本身无外部依赖）。
     * 若发现 DT_NEEDED，返回错误。 */
    if (get_dyn(lib, DT_NEEDED) != 0) {
        fs_close(fd); lib->used = 0;
        set_error("DT_NEEDED unsupported in v1"); return NULL;
    }

    lib->refcount = 1;
    fs_close(fd);
    return lib;
}

/* ---------- dlsym ---------- */

void *dlsym(void *handle, const char *name) {
    if (!handle || !name) { set_error("null arg"); return NULL; }
    struct loaded_lib *lib = (struct loaded_lib*)handle;
    if (!lib->used) { set_error("invalid handle"); return NULL; }

    const Elf32_Sym *sym = find_sym_in_lib(lib, name);
    if (!sym) { set_error("symbol not found"); return NULL; }

    return (void*)(lib->bias + sym->st_value);
}

/* ---------- dlclose ---------- */

int dlclose(void *handle) {
    if (!handle) return -1;
    struct loaded_lib *lib = (struct loaded_lib*)handle;
    if (!lib->used) return -1;

    lib->refcount--;
    if (lib->refcount > 0) return 0;

    /* 第一版不 munmap——VMA 在进程退出时回收。
     * 只标记 slot 空闲，允许同名再次加载（会重新 mmap）。 */
    lib->used = 0;
    return 0;
}