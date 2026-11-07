#include <ahci.h>
#include <ata.h>
#include <pci.h>
#include <io.h>
#include <kmap.h>
#include <pmm.h>
#include <printf.h>
#include <string.h>
#include <stdint.h>

struct ahci_port_state {
    uint8_t  implemented;
    uint8_t  present;
    uint8_t  port_no;
    uint8_t  pad;
    uint32_t clb_phys;
    uint32_t fb_phys;
    uint32_t ct_phys;
    uint32_t dma_phys;
    uint32_t dma_pages;
};

struct ahci_cmd_header {
    uint8_t  cfl;
    uint8_t  a;
    uint8_t  prdtl;
    uint8_t  prdtl_hi;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} __attribute__((packed));

struct ahci_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;
} __attribute__((packed));

struct ahci_cmd_table {
    uint8_t  cfis[64];
    uint8_t  acmd[32];
    uint8_t  reserved[32];
    struct ahci_prdt_entry prdt[AHCI_PRDT_ENTRIES];
} __attribute__((packed));

struct fis_reg_h2d {
    uint8_t  fis_type;
    uint8_t  pmport_c;
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  reserved[4];
} __attribute__((packed));

static volatile uint32_t *g_abar = NULL;
static int g_ready = 0;
static struct ahci_port_state g_ports[AHCI_MAX_PORTS];
static int g_dev_count = 0;

static inline uint32_t ahci_read(uint32_t off) { return g_abar[off / 4]; }
static inline void ahci_write(uint32_t off, uint32_t v) { g_abar[off / 4] = v; }
static inline uint32_t port_read(uint8_t p, uint32_t o) { return ahci_read(AHCI_PORT_BASE(p) + o); }
static inline void port_write(uint8_t p, uint32_t o, uint32_t v) { ahci_write(AHCI_PORT_BASE(p) + o, v); }

static void port_stop(uint8_t p) {
    uint32_t cmd = port_read(p, PORT_CMD);
    cmd &= ~(CMD_ST | CMD_FRE | CMD_POD | CMD_SUD);
    port_write(p, PORT_CMD, cmd);
    for (int i = 0; i < 1000000; i++) {
        if (!(port_read(p, PORT_CMD) & CMD_CR)) return;
    }
    kprintf("[AHCI] port %d stop timeout\n", p);
}

static void port_start(uint8_t p) {
    for (int i = 0; i < 1000000; i++) {
        if (!(port_read(p, PORT_CMD) & CMD_CR)) break;
    }
    uint32_t cmd = port_read(p, PORT_CMD);
    cmd |= CMD_ST | CMD_FRE;
    port_write(p, PORT_CMD, cmd);
}

static int ahci_init_one(const struct pci_device *dev) {
    uint32_t bar5 = pci_read_config(dev->bus, dev->slot, dev->func, 0x24);
    if (bar5 & 0x1) {
        kprintf("[AHCI] BAR5 is I/O space, unsupported\n");
        return -1;
    }
    uint32_t mmio = bar5 & ~0xF;
    if (mmio == 0 || mmio == 0xFFFFFFFF) {
        kprintf("[AHCI] BAR5 invalid (0x%x)\n", mmio);
        return -1;
    }

    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= 0x5;
    pci_write_config(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);

    void *abar = kmap(mmio, 0x2000);
    if (!abar) {
        kprintf("[AHCI] kmap 0x%x failed\n", mmio);
        return -1;
    }
    g_abar = (volatile uint32_t*)abar;

    uint32_t cap = ahci_read(AHCI_CAP);
    uint32_t vs  = ahci_read(AHCI_VS);
    uint32_t pi  = ahci_read(AHCI_PI);
    kprintf("[AHCI] abar=0x%x cap=0x%x vs=0x%x pi=0x%x\n", mmio, cap, vs, pi);

    ahci_write(AHCI_GHC, ahci_read(AHCI_GHC) | GHC_AE);

    memset(g_ports, 0, sizeof(g_ports));
    g_dev_count = 0;

    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        g_ports[p].implemented = 1;
        g_ports[p].port_no = (uint8_t)p;

        uint32_t ssts = port_read(p, PORT_SSTS);
        uint32_t det = ssts & 0x0F;
        uint32_t sig = port_read(p, PORT_SIG);
        kprintf("[AHCI] port %d: ssts=0x%x det=%u sig=0x%x\n", p, ssts, det, sig);
        if (det != SSTS_DET_PRESENT) continue;

        if (sig != SIG_ATA) {
            kprintf("[AHCI] port %d: sig=0x%x (non-ATA)\n", p, sig);
            continue;
        }

        port_stop(p);

        uint32_t clb = pmm_alloc_page();
        uint32_t fb  = pmm_alloc_page();
        uint32_t ct  = pmm_alloc_page();
        uint32_t dma_pages = AHCI_DMA_BUF_SIZE / 4096;
        uint32_t dma = pmm_alloc_pages(dma_pages);

        if (!clb || !fb || !ct || !dma) {
            kprintf("[AHCI] port %d: alloc failed\n", p);
            if (clb) pmm_free_page(clb);
            if (fb)  pmm_free_page(fb);
            if (ct)  pmm_free_page(ct);
            if (dma) pmm_free_pages(dma, dma_pages);
            continue;
        }
        memset((void*)clb, 0, 4096);
        memset((void*)fb,  0, 4096);
        memset((void*)ct,  0, 4096);

        g_ports[p].clb_phys = clb;
        g_ports[p].fb_phys  = fb;
        g_ports[p].ct_phys  = ct;
        g_ports[p].dma_phys = dma;
        g_ports[p].dma_pages = dma_pages;
        g_ports[p].present = 1;

        port_write(p, PORT_CLB,  clb);
        port_write(p, PORT_CLBU, 0);
        port_write(p, PORT_FB,   fb);
        port_write(p, PORT_FBU,  0);
        port_write(p, PORT_IS, 0xFFFFFFFF);
        port_write(p, PORT_IE, 0);

        port_start(p);
        for (int i = 0; i < 1000000; i++) {
            if (!(port_read(p, PORT_CMD) & CMD_FR)) break;
        }

        kprintf("[AHCI] port %d: SATA disk ready\n", p);
        g_dev_count++;
    }

    g_ready = 1;
    kprintf("[AHCI] %d device(s) ready\n", g_dev_count);
    return g_dev_count > 0 ? 0 : -1;
}

int ahci_init(void) {
    for (int n = 0; ; n++) {
        const struct pci_device *dev = pci_find_class_nth(0x01, 0x06, n);
        if (!dev) return -1;
        if (ahci_init_one(dev) == 0) return 0;
        /* 当前控制器没设备，清理 g_abar / g_ports 再试下一个 */
    }
}

int ahci_ready(void) { return g_ready; }
int ahci_device_count(void) { return g_dev_count; }

int ahci_get_port(int idx) {
    int n = 0;
    for (int p = 0; p < AHCI_MAX_PORTS; p++) {
        if (g_ports[p].present) {
            if (n == idx) return p;
            n++;
        }
    }
    return -1;
}

static int ahci_issue(uint8_t port, int write, uint32_t lba,
                      uint32_t count, uint32_t dma_phys) {
    struct ahci_port_state *ap = &g_ports[port];
    struct ahci_cmd_header *cl = (struct ahci_cmd_header*)ap->clb_phys;
    struct ahci_cmd_table  *ct = (struct ahci_cmd_table*)ap->ct_phys;

    memset(&cl[0], 0, sizeof(cl[0]));
    memset(ct, 0, sizeof(*ct));

    struct fis_reg_h2d *fis = (struct fis_reg_h2d*)ct->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command  = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    fis->lba0 = (lba >> 0)  & 0xFF;
    fis->lba1 = (lba >> 8)  & 0xFF;
    fis->lba2 = (lba >> 16) & 0xFF;
    fis->lba3 = (lba >> 24) & 0xFF;
    fis->lba4 = 0;
    fis->lba5 = 0;
    fis->device = 0x40;
    fis->countl = (count >> 0) & 0xFF;
    fis->counth = (count >> 8) & 0xFF;

    ct->prdt[0].dba  = dma_phys;
    ct->prdt[0].dbau = 0;
    ct->prdt[0].dbc  = (count * 512 - 1) & 0x3FFFFF;

    cl[0].cfl    = sizeof(struct fis_reg_h2d) / 4;
    cl[0].a      = write ? (1 << 6) : (1 << 5);
    cl[0].prdtl  = 1;
    cl[0].ctba   = ap->ct_phys;
    cl[0].ctbau  = 0;

    port_write(port, PORT_IS, 0xFFFFFFFF);
    port_write(port, PORT_CI, 1);

    int timeout = 100000000;
    while (timeout-- > 0) {
        if (!(port_read(port, PORT_CI) & 1)) break;
    }
    if (timeout <= 0) {
        kprintf("[AHCI] port %d cmd timeout\n", port);
        return -1;
    }

    uint32_t is = port_read(port, PORT_IS);
    if (is & (IS_TFES | IS_HBFS | IS_HBDS | IS_IFS | IS_INFS | IS_OFS)) {
        kprintf("[AHCI] port %d error is=0x%x\n", port, is);
        return -1;
    }
    return 0;
}

int ahci_read_sectors(uint8_t port, uint32_t lba, uint32_t count,
                      uint8_t *buffer) {
    if (!g_ready || port >= AHCI_MAX_PORTS || !g_ports[port].present) return -1;
    if (count == 0 || count > AHCI_DMA_BUF_SIZE / 512) return -1;
    uint32_t dma = g_ports[port].dma_phys;
    if (ahci_issue(port, 0, lba, count, dma) < 0) return -1;
    memcpy(buffer, (void*)dma, count * 512);
    return 0;
}

int ahci_write_sectors(uint8_t port, uint32_t lba, uint32_t count,
                       const uint8_t *buffer) {
    if (!g_ready || port >= AHCI_MAX_PORTS || !g_ports[port].present) return -1;
    if (count == 0 || count > AHCI_DMA_BUF_SIZE / 512) return -1;
    uint32_t dma = g_ports[port].dma_phys;
    memcpy((void*)dma, buffer, count * 512);
    return ahci_issue(port, 1, lba, count, dma);
}