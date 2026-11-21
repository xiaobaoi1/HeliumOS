#include <e1000.h>
#include <net.h>
#include <pci.h>
#include <io.h>
#include <kmap.h>
#include <pmm.h>
#include <irq.h>
#include <printf.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* ---------- 寄存器偏移（字节） ---------- */
#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_ICR       0x00C0
#define E1000_ITR       0x00C4
#define E1000_IMS       0x00D0
#define E1000_IMC       0x00D8
#define E1000_RCTL      0x0100
#define E1000_TCTL      0x0400
#define E1000_RDBAL     0x2800
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810
#define E1000_RDT       0x2818
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_RAL       0x5400
#define E1000_RAH       0x5404

/* CTRL */
#define CTRL_RST        (1u << 26)
#define CTRL_ASDE       (1u << 5)
#define CTRL_SLU        (1u << 6)

/* RCTL */
#define RCTL_EN         (1u << 1)
#define RCTL_BAM        (1u << 15)
#define RCTL_SECRC      (1u << 26)
#define RCTL_BSIZE_2048 (0u << 16)

/* TCTL */
#define TCTL_EN         (1u << 1)
#define TCTL_PSP        (1u << 3)
#define TCTL_CT_SHIFT   4
#define TCTL_COLD_SHIFT 12

/* ICR */
#define ICR_TXDW        (1u << 0)
#define ICR_LSC         (1u << 2)
#define ICR_RXDMT0      (1u << 4)
#define ICR_RXO         (1u << 6)
#define ICR_RXT0        (1u << 7)

#define RX_STATUS_DD    (1u << 0)
#define TX_CMD_EOP      (1u << 0)
#define TX_CMD_IFCS     (1u << 1)
#define TX_CMD_RS       (1u << 3)
#define TX_STATUS_DD    (1u << 0)

#define E1000_NUM_RX    32
#define E1000_NUM_TX    32
#define E1000_RX_BUF    2048

struct e1000_rx_desc {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

/* ---------- 状态 ---------- */
static volatile uint32_t *g_regs = NULL;
static uint8_t  g_mac[6];
static int      g_ready = 0;

static struct e1000_rx_desc *g_rx_ring = NULL;
static uint32_t g_rx_ring_phys = 0;
static uint8_t *g_rx_buf[E1000_NUM_RX];
static uint32_t g_rx_buf_phys[E1000_NUM_RX];
static int      g_rx_cur = 0;

static struct e1000_tx_desc *g_tx_ring = NULL;
static uint32_t g_tx_ring_phys = 0;
static uint8_t *g_tx_buf[E1000_NUM_TX];
static uint32_t g_tx_buf_phys[E1000_NUM_TX];
static int      g_tx_cur = 0;

/* ---------- MMIO ---------- */
static inline uint32_t e1000_read(uint32_t reg) {
    return g_regs[reg / 4];
}
static inline void e1000_write(uint32_t reg, uint32_t val) {
    g_regs[reg / 4] = val;
}

/* ---------- PCI 查找 ---------- */
static const uint16_t e1000_devs[] = {
    0x100E, 0x100F, 0x100C, 0x100D, 0x10D3, 0x153A, 0x15B8, 0x10EA, 0
};

static const struct pci_device *find_e1000(void) {
    for (int i = 0; e1000_devs[i]; i++) {
        const struct pci_device *d = pci_find(0x8086, e1000_devs[i]);
        if (d) return d;
    }
    return NULL;
}

/* ---------- 发送 ---------- */
static int e1000_send(const uint8_t *frame, uint32_t len) {
    if (!g_ready || len == 0 || len > 1518) return -1;

    int idx = g_tx_cur;

    /* 等上一个用这个槽的传输完成（一般已 DD） */
    for (int i = 0; i < 100000; i++) {
        if (g_tx_ring[idx].status & TX_STATUS_DD) break;
    }

    memcpy(g_tx_buf[idx], frame, len);

    struct e1000_tx_desc *d = &g_tx_ring[idx];
    d->buffer_addr = g_tx_buf_phys[idx];
    d->length      = (uint16_t)len;
    d->cmd         = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status      = 0;
    d->cso         = 0;
    d->css         = 0;
    d->special     = 0;

    g_tx_cur = (idx + 1) % E1000_NUM_TX;
    e1000_write(E1000_TDT, g_tx_cur);

    /* 同步等发送完成 */
    for (int i = 0; i < 100000; i++) {
        if (d->status & TX_STATUS_DD) return (int)len;
    }
    return -1;
}

static int e1000_recv(uint8_t *buf, uint32_t max, uint32_t *out_len) {
    (void)buf; (void)max; (void)out_len;
    return 0;   /* 数据经 IRQ 路径 net_deliver 推送 */
}

/* ---------- 接收 ---------- */
static void e1000_rx_poll(void) {
    if (!g_ready) return;

    while (g_rx_ring[g_rx_cur].status & RX_STATUS_DD) {
        struct e1000_rx_desc *d = &g_rx_ring[g_rx_cur];
        uint32_t len = d->length;
        if (len > 0 && len <= E1000_RX_BUF) {
            net_deliver(g_rx_buf[g_rx_cur], len);
        }
        d->status = 0;
        int old = g_rx_cur;
        g_rx_cur = (g_rx_cur + 1) % E1000_NUM_RX;
        e1000_write(E1000_RDT, old);
    }
}

/* ---------- 中断 ---------- */
static void e1000_irq(void) {
    uint32_t icr = e1000_read(E1000_ICR);
    if (icr & (ICR_RXT0 | ICR_RXDMT0 | ICR_RXO)) {
        e1000_rx_poll();
    }
}

/* ---------- 初始化 ---------- */

void e1000_init(void) {
    const struct pci_device *pci = find_e1000();
    if (!pci) {
        kprintf("[E1000] Not found\n");
        return;
    }

    kprintf("[E1000] Found %04x:%04x at %02x:%02x.%x\n",
            pci->vendor_id, pci->device_id,
            pci->bus, pci->slot, pci->func);

    /* BAR0 必须是 MMIO */
    uint32_t bar0 = pci->bar[0];
    if (bar0 & 0x1) {
        kprintf("[E1000] BAR0 is I/O space, unsupported\n");
        return;
    }
    uint32_t mmio_phys = bar0 & ~0xF;
    if (mmio_phys == 0) {
        kprintf("[E1000] BAR0 invalid\n");
        return;
    }

    /* 打开 I/O space + bus master */
    uint32_t cmd = pci_read_config(pci->bus, pci->slot, pci->func,
                                   PCI_COMMAND);
    cmd |= 0x5;
    pci_write_config(pci->bus, pci->slot, pci->func, PCI_COMMAND, cmd);

    /* kmap MMIO（128KB） */
    g_regs = (volatile uint32_t*)kmap(mmio_phys, 0x20000);
    if (!g_regs) {
        kprintf("[E1000] kmap failed\n");
        return;
    }
    kprintf("[E1000] MMIO 0x%x -> %p\n", mmio_phys, (void*)g_regs);

    /* 复位 */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000000; i++) {
        if (!(e1000_read(E1000_CTRL) & CTRL_RST)) break;
    }

    /* Link up + 自动速度 */
    e1000_write(E1000_CTRL, CTRL_ASDE | CTRL_SLU);

    /* 读 MAC */
    uint32_t ral = e1000_read(E1000_RAL);
    uint32_t rah = e1000_read(E1000_RAH);
    g_mac[0] = ral & 0xFF;
    g_mac[1] = (ral >> 8) & 0xFF;
    g_mac[2] = (ral >> 16) & 0xFF;
    g_mac[3] = (ral >> 24) & 0xFF;
    g_mac[4] = rah & 0xFF;
    g_mac[5] = (rah >> 8) & 0xFF;

    /* 分配 RX 环 */
    g_rx_ring_phys = pmm_alloc_pages(1);
    if (!g_rx_ring_phys) { kprintf("[E1000] rx ring alloc failed\n"); return; }
    g_rx_ring = (struct e1000_rx_desc*)g_rx_ring_phys;
    memset(g_rx_ring, 0, sizeof(struct e1000_rx_desc) * E1000_NUM_RX);

    for (int i = 0; i < E1000_NUM_RX; i++) {
        uint32_t phys = pmm_alloc_pages(1);
        if (!phys) { kprintf("[E1000] rx buf alloc failed\n"); return; }
        g_rx_buf_phys[i] = phys;
        g_rx_buf[i] = (uint8_t*)phys;
        g_rx_ring[i].buffer_addr = phys;
        g_rx_ring[i].status = 0;
    }

    /* 分配 TX 环 */
    g_tx_ring_phys = pmm_alloc_pages(1);
    if (!g_tx_ring_phys) { kprintf("[E1000] tx ring alloc failed\n"); return; }
    g_tx_ring = (struct e1000_tx_desc*)g_tx_ring_phys;
    memset(g_tx_ring, 0, sizeof(struct e1000_tx_desc) * E1000_NUM_TX);

    for (int i = 0; i < E1000_NUM_TX; i++) {
        uint32_t phys = pmm_alloc_pages(1);
        if (!phys) { kprintf("[E1000] tx buf alloc failed\n"); return; }
        g_tx_buf_phys[i] = phys;
        g_tx_buf[i] = (uint8_t*)phys;
        g_tx_ring[i].buffer_addr = phys;
        g_tx_ring[i].status = TX_STATUS_DD;
    }

    /* 配置 RX */
    e1000_write(E1000_RDBAL, g_rx_ring_phys);
    e1000_write(E1000_RDBAH, 0);
    e1000_write(E1000_RDLEN, sizeof(struct e1000_rx_desc) * E1000_NUM_RX);
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, E1000_NUM_RX - 1);

    /* 配置 TX */
    e1000_write(E1000_TDBAL, g_tx_ring_phys);
    e1000_write(E1000_TDBAH, 0);
    e1000_write(E1000_TDLEN, sizeof(struct e1000_tx_desc) * E1000_NUM_TX);
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);

    /* RCTL：EN + BAM + BSIZE=2048 + SECRC */
    e1000_write(E1000_RCTL,
                RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

    /* TCTL：EN + PSP + CT=0x10 + COLD=0x40 */
    e1000_write(E1000_TCTL,
                TCTL_EN | TCTL_PSP |
                (0x10u << TCTL_CT_SHIFT) | (0x40u << TCTL_COLD_SHIFT));

    /* 中断使能 */
    e1000_write(E1000_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_RXO);

    if (pci->interrupt_line && pci->interrupt_line != 0xFF) {
        irq_register(pci->interrupt_line, e1000_irq, "e1000");
    }

    g_ready = 1;

    static struct net_device ndev = {
        .name = "eth0",
        .send = e1000_send,
        .recv = e1000_recv,
    };
    memcpy(ndev.mac, g_mac, 6);
    net_register(&ndev);

    kprintf("[E1000] Ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
}