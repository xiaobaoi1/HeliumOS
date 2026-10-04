#include <acpi.h>
#include <io.h>
#include <printf.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <multiboot2.h>

/* ---------- 结构 ---------- */

struct acpi_rsdp_v1 {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
} __attribute__((packed));

struct acpi_rsdp_v2 {
    struct acpi_rsdp_v1 v1;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_gas {
    uint8_t  address_space_id;    /* 0=Mem, 1=I/O, 2=PCI */
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

/* FADT 关键字段偏移（ACPI 规范固定） */
#define FADT_OFF_PM1A_CNT_BLK   64
#define FADT_OFF_PM1B_CNT_BLK   68
#define FADT_OFF_PM1_CNT_LEN    89
#define FADT_OFF_RESET_REG      116
#define FADT_OFF_RESET_VALUE    128

/* SLP_EN = bit 13；SLP_TYPa 在第 10-12 位 */
#define SLP_EN  (1u << 13)

/* ---------- 内部状态 ---------- */

static int      g_ok = 0;
static uint32_t g_pm1a_cnt = 0;
static uint32_t g_pm1b_cnt = 0;
static uint8_t  g_pm1_cnt_len = 0;
static struct acpi_gas g_reset_reg;
static uint8_t  g_reset_val = 0;
static int      g_has_reset_reg = 0;

/* ---------- 校验 ---------- */

static int checksum_ok(const void *p, uint32_t len) {
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += b[i];
    return sum == 0;
}

/* ---------- RSDP 定位 ---------- */

static const struct acpi_rsdp_v2 *rsdp_from_mb(uint32_t mb_addr) {
    if (!mb_addr) return NULL;
    struct multiboot2_info *info = (struct multiboot2_info*)mb_addr;
    uint8_t *ptr = (uint8_t*)mb_addr + sizeof(struct multiboot2_info);
    uint8_t *end = (uint8_t*)mb_addr + info->total_size;

    while (ptr < end) {
        struct multiboot2_tag *tag = (struct multiboot2_tag*)ptr;
        if (tag->type == MULTIBOOT2_TAG_TYPE_END) break;
        if (tag->type == MULTIBOOT2_TAG_TYPE_ACPI_NEW ||
            tag->type == MULTIBOOT2_TAG_TYPE_ACPI_OLD) {
            return (const struct acpi_rsdp_v2*)((uint8_t*)tag + 8);
        }
        ptr += tag->size;
        while ((uintptr_t)ptr % 8 != 0) ptr++;
    }
    return NULL;
}

static int try_rsdp_at(const void *addr, const struct acpi_rsdp_v2 **out) {
    const struct acpi_rsdp_v1 *p = addr;
    if (memcmp(p->signature, "RSD PTR ", 8) != 0) return 0;
    if (!checksum_ok(p, 20)) return 0;

    if (p->revision >= 2) {
        const struct acpi_rsdp_v2 *p2 = addr;
        if (checksum_ok(p2, 36)) {
            *out = p2;
            return 1;
        }
    }
    *out = addr;
    return 1;
}

static const struct acpi_rsdp_v2 *rsdp_scan(void) {
    const struct acpi_rsdp_v2 *out;

    /* 1. EBDA 前 1KB */
    uint16_t ebda_seg = *(volatile uint16_t*)0x40E;
    uint32_t ebda_base = (uint32_t)ebda_seg << 4;
    if (ebda_base >= 0x80000 && ebda_base < 0xA0000) {
        for (uint32_t a = ebda_base; a < ebda_base + 1024; a += 16) {
            if (try_rsdp_at((const void*)a, &out)) return out;
        }
    }

    /* 2. BIOS ROM 0xE0000 - 0xFFFFF */
    for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
        if (try_rsdp_at((const void*)a, &out)) return out;
    }
    return NULL;
}

/* ---------- FADT 查找 ---------- */

static const struct acpi_sdt_header *find_fadt_from_rsdt(uint32_t rsdt_phys) {
    const struct acpi_sdt_header *rsdt =
        (const struct acpi_sdt_header*)rsdt_phys;
    if (!checksum_ok(rsdt, rsdt->length)) {
        KLOG_WARN("ACPI: RSDT checksum bad\n");
        return NULL;
    }

    uint32_t count = (rsdt->length - sizeof(*rsdt)) / 4;
    const uint32_t *ptrs = (const uint32_t*)((uint8_t*)rsdt + sizeof(*rsdt));
    for (uint32_t i = 0; i < count; i++) {
        const struct acpi_sdt_header *h =
            (const struct acpi_sdt_header*)ptrs[i];
        if (memcmp(h->signature, "FACP", 4) != 0) continue;

        if (!checksum_ok(h, h->length)) {
            KLOG_WARN("ACPI: FADT checksum bad\n");
            return NULL;
        }
        return h;
    }
    return NULL;
}

static const struct acpi_sdt_header *find_fadt_from_xsdt(uint64_t xsdt_phys) {
    if (xsdt_phys > 0xFFFFFFFFULL) {
        KLOG_WARN("ACPI: XSDT above 4GB, skipped\n");
        return NULL;
    }

    const struct acpi_sdt_header *xsdt =
        (const struct acpi_sdt_header*)(uint32_t)xsdt_phys;
    if (!checksum_ok(xsdt, xsdt->length)) {
        KLOG_WARN("ACPI: XSDT checksum bad\n");
        return NULL;
    }

    uint32_t count = (xsdt->length - sizeof(*xsdt)) / 8;
    const uint64_t *ptrs = (const uint64_t*)((uint8_t*)xsdt + sizeof(*xsdt));
    for (uint32_t i = 0; i < count; i++) {
        if (ptrs[i] > 0xFFFFFFFFULL) {
            KLOG_DBG("ACPI: table %u above 4GB, skipped\n", i);
            continue;
        }
        const struct acpi_sdt_header *h =
            (const struct acpi_sdt_header*)(uint32_t)ptrs[i];
        if (memcmp(h->signature, "FACP", 4) != 0) continue;

        if (!checksum_ok(h, h->length)) {
            KLOG_WARN("ACPI: FADT checksum bad\n");
            return NULL;
        }
        return h;
    }
    return NULL;
}

/* ---------- 初始化 ---------- */

void acpi_init(uint32_t mb_addr) {
    const struct acpi_rsdp_v2 *rsdp = rsdp_from_mb(mb_addr);
    if (!rsdp) rsdp = rsdp_scan();
    if (!rsdp) {
        kprintf("[ACPI] RSDP not found\n");
        return;
    }
    kprintf("[ACPI] RSDP rev=%d\n", rsdp->v1.revision);

    const struct acpi_sdt_header *fadt = NULL;
    if (rsdp->v1.revision >= 2 && rsdp->xsdt_addr) {
        fadt = find_fadt_from_xsdt(rsdp->xsdt_addr);
    }
    if (!fadt && rsdp->v1.rsdt_addr) {
        fadt = find_fadt_from_rsdt(rsdp->v1.rsdt_addr);
    }
    if (!fadt) {
        kprintf("[ACPI] FADT not found\n");
        return;
    }

    const uint8_t *base = (const uint8_t*)fadt;
    memcpy(&g_pm1a_cnt, base + FADT_OFF_PM1A_CNT_BLK, 4);
    memcpy(&g_pm1b_cnt, base + FADT_OFF_PM1B_CNT_BLK, 4);
    g_pm1_cnt_len = base[FADT_OFF_PM1_CNT_LEN];

    /* RESET_REG 是 FADT 2.0+ 才有的字段 */
    if (fadt->revision >= 2 && fadt->length >= FADT_OFF_RESET_VALUE + 1) {
        memcpy(&g_reset_reg, base + FADT_OFF_RESET_REG, sizeof(g_reset_reg));
        g_reset_val = base[FADT_OFF_RESET_VALUE];
        g_has_reset_reg = 1;
    }

    g_ok = 1;
    kprintf("[ACPI] FADT ok: pm1a=0x%x pm1b=0x%x reset=%d\n",
            g_pm1a_cnt, g_pm1b_cnt, g_has_reset_reg);
}

/* ---------- 电源控制 ---------- */

int acpi_poweroff(void) {
    if (!g_ok || !g_pm1a_cnt) return ENOSYS;

    /* SLP_TYPa 的值由 DSDT 的 _S5_ 对象决定。读取它需要 AML 解析器
     *（暂未实现）。已知的常见值：QEMU = 0，多数真机 = 5。
     * 遍历候选值，命中时立即掉电，后续迭代不会执行。 */
    static const uint16_t candidates[] = {0, 5, 1, 2, 3, 4, 6, 7};
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        uint16_t val = (uint16_t)((candidates[i] << 10) | SLP_EN);

        kprintf("[ACPI] poweroff try SLP_TYPa=%d\n", candidates[i]);

        outw((uint16_t)g_pm1a_cnt, val);
        if (g_pm1b_cnt) outw((uint16_t)g_pm1b_cnt, val);

        /* 给硬件一点时间响应。QEMU 上通常第一个值就命中。 */
        for (volatile int d = 0; d < 1000000; d++);
    }

    kprintf("[ACPI] poweroff failed (no SLP_TYPa worked)\n");
    return EIO;
}

int acpi_reboot(void) {
    /* 1. ACPI RESET_REG */
    if (g_has_reset_reg && g_reset_reg.address != 0) {
        if (g_reset_reg.address_space_id == 1) {
            /* System I/O */
            outb((uint16_t)g_reset_reg.address, g_reset_val);
            return OK;
        }
        if (g_reset_reg.address_space_id == 0 &&
            g_reset_reg.address <= 0xFFFFFFFFULL) {
            /* System Memory */
            *(volatile uint8_t*)(uint32_t)g_reset_reg.address = g_reset_val;
            return OK;
        }
    }

    /* 2. fallback：8042 键盘控制器 */
    for (int i = 0; i < 100; i++) {
        if (!(inb(0x64) & 0x02)) break;
    }
    outb(0x64, 0xFE);
    return OK;
}