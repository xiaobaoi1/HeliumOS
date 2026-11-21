#include <rtl8139.h>
#include <net.h>
#include <pci.h>
#include <io.h>
#include <pmm.h>
#include <irq.h>
#include <printf.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#define RTL8139_VENDOR   0x10EC
#define RTL8139_DEVICE   0x8139

/* 寄存器偏移 */
#define REG_MAC0      0x00
#define REG_TSD0      0x10   /* 4 × 4 字节发送状态 */
#define REG_TSAD0     0x20   /* 4 × 4 字节发送地址 */
#define REG_RBSTART   0x30
#define REG_CMD       0x37
#define REG_CAPR      0x38
#define REG_CBR       0x3A
#define REG_IMR       0x3C
#define REG_ISR       0x3E
#define REG_TCR       0x40
#define REG_RCR       0x44
#define REG_CONFIG1   0x50

/* CMD 位 */
#define CMD_RST   (1u << 4)
#define CMD_RE    (1u << 3)
#define CMD_TE    (1u << 2)

/* ISR 位 */
#define ISR_ROK   (1u << 0)   /* 接收 OK */
#define ISR_TOK   (1u << 2)   /* 发送 OK */
#define ISR_RER   (1u << 1)   /* 接收错误 */
#define ISR_TER   (1u << 3)   /* 发送错误 */

#define RX_BUF_SIZE   (8 * 1024 + 16)     /* 8208 = RBLEN=0 的 8K+16 */
#define RX_BUF_PAGES  ((RX_BUF_SIZE + 4095) / 4096)   /* 3 页 */

static uint16_t g_io = 0;
static uint8_t  g_mac[6];

static uint32_t g_rx_buf_phys = 0;
static uint8_t *g_rx_buf = NULL;
static uint16_t g_rx_pos = 0;

static uint32_t g_tx_buf_phys[4] = {0};
static uint8_t *g_tx_buf[4] = {NULL};

static int g_ready = 0;

/* ---------- 收发 ---------- */

static int rtl8139_send(const uint8_t *frame, uint32_t len) {
    if (!g_ready || len > 1518 || len == 0) return -1;

    static int tx_cycle = 0;
    int i = tx_cycle;
    tx_cycle = (tx_cycle + 1) & 3;

    memcpy(g_tx_buf[i], frame, len);

    outl(g_io + REG_TSAD0 + i * 4, g_tx_buf_phys[i]);
    outl(g_io + REG_TSD0  + i * 4, len);
    return (int)len;
}

static int rtl8139_recv(uint8_t *buf, uint32_t max, uint32_t *out_len) {
    (void)buf; (void)max; (void)out_len;
    return 0;   /* 由中断处理主动 deliver，不通过此接口 */
}

static void rtl8139_rx_poll(void) {
    if (!g_ready) return;

    for (int iter = 0; iter < 16; iter++) {
        uint16_t cbr = inw(g_io + REG_CBR);
        uint16_t rd  = (uint16_t)(g_rx_pos + 16);   /* 物理读偏移 */

        /* 无数据：CBR == rd（CBR 无偏移，CAPR = rd - 16） */
        if (cbr == rd) break;

        /* 读位置已到缓冲末尾，视为损坏，resync */
        if (rd + 4 > RX_BUF_SIZE) {
            g_rx_pos = (uint16_t)(cbr - 16);
            outw(g_io + REG_CAPR, g_rx_pos);
            break;
        }

        uint16_t status = *(volatile uint16_t*)(g_rx_buf + rd);
        uint16_t length = *(volatile uint16_t*)(g_rx_buf + rd + 2);

        if (length < 4 || length > 1536 || !(status & ISR_ROK)) {
            KLOG_DBG("RTL8139: invalid frame, resync\n");
            g_rx_pos = (uint16_t)(cbr - 16);
            outw(g_io + REG_CAPR, g_rx_pos);
            break;
        }

        /* length 含 CRC，不含帧头 4 字节 */
        uint32_t payload_len = length - 4;
        uint32_t data_off = rd + 4;

        if (payload_len > 0 && payload_len <= NET_FRAME_MAX) {
            uint8_t frame[NET_FRAME_MAX];
            if (data_off + payload_len <= RX_BUF_SIZE) {
                memcpy(frame, g_rx_buf + data_off, payload_len);
            } else {
                uint32_t first = RX_BUF_SIZE - data_off;
                memcpy(frame, g_rx_buf + data_off, first);
                memcpy(frame + first, g_rx_buf, payload_len - first);
            }
            net_deliver(frame, payload_len);
        }

        /* 推进：帧头 4 + length（含 CRC），4 字节对齐 */
        uint32_t new_rd = (rd + 4 + length + 3) & ~3u;
        if (new_rd >= RX_BUF_SIZE) new_rd -= RX_BUF_SIZE;
        g_rx_pos = (uint16_t)(new_rd - 16);
        outw(g_io + REG_CAPR, g_rx_pos);
    }
}

static void rtl8139_irq(void) {
    uint16_t status = inw(g_io + REG_ISR);
    outw(g_io + REG_ISR, status);   /* 写 1 清除 */

    if (status & ISR_ROK) rtl8139_rx_poll();
    /* TOK / RER / TER 暂不处理 */
}

/* ---------- 初始化 ---------- */

void rtl8139_init(void) {
    const struct pci_device *dev = pci_find(RTL8139_VENDOR, RTL8139_DEVICE);
    if (!dev) {
        kprintf("[RTL8139] Not found\n");
        return;
    }

    /* BAR0 是 I/O 空间（bit 0 = 1） */
    uint32_t bar0 = dev->bar[0];
    if (!(bar0 & 0x1)) {
        kprintf("[RTL8139] BAR0 not I/O space\n");
        return;
    }
    g_io = (uint16_t)(bar0 & ~0x3);
    kprintf("[RTL8139] Found at I/O 0x%x irq=%d\n",
            g_io, dev->interrupt_line);

    /* 打开 I/O space (bit 0) + bus master (bit 2) */
    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func,
                                   PCI_COMMAND);
    cmd |= 0x5;
    pci_write_config(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);

    /* 软复位 */
    outb(g_io + REG_CMD, CMD_RST);
    while (inb(g_io + REG_CMD) & CMD_RST) {
        /* 等复位完成 */
    }
    kprintf("[RTL8139] Reset done\n");

    /* 读 MAC */
    for (int i = 0; i < 6; i++) g_mac[i] = inb(g_io + REG_MAC0 + i);

    /* 分配接收缓冲 */
    g_rx_buf_phys = pmm_alloc_pages(RX_BUF_PAGES);
    if (!g_rx_buf_phys) {
        kprintf("[RTL8139] Failed to alloc rx buffer\n");
        return;
    }
    g_rx_buf = (uint8_t*)g_rx_buf_phys;
    memset(g_rx_buf, 0, RX_BUF_SIZE);

    /* 分配 4 个发送缓冲（各一页） */
    for (int i = 0; i < 4; i++) {
        g_tx_buf_phys[i] = pmm_alloc_pages(1);
        if (!g_tx_buf_phys[i]) {
            kprintf("[RTL8139] Failed to alloc tx buffer %d\n", i);
            return;
        }
        g_tx_buf[i] = (uint8_t*)g_tx_buf_phys[i];
    }

    outl(g_io + REG_RBSTART, g_rx_buf_phys);

    /* IMR：开启 ROK | TOK */
    outw(g_io + REG_IMR, ISR_ROK | ISR_TOK);

    /* RCR：AAP | APM | AM | AB，RBLEN=0（8K+16）*/
    outl(g_io + REG_RCR, 0x0F | (0u << 11));

    /* TCR：标准模式 */
    outl(g_io + REG_TCR, 0x03000000);

    /* 初始读指针 */
    g_rx_pos = 0;
    outw(g_io + REG_CAPR, (uint16_t)(0 - 16));

    /* 使能 RE + TE */
    outb(g_io + REG_CMD, CMD_RE | CMD_TE);

    /* 注册 IRQ */
    if (dev->interrupt_line != 0 && dev->interrupt_line != 0xFF) {
        irq_register(dev->interrupt_line, rtl8139_irq, "rtl8139");
    }

    g_ready = 1;

    /* 注册 net 子系统 */
    static struct net_device ndev = {
        .name = "eth0",
        .send = rtl8139_send,
        .recv = rtl8139_recv,
    };
    memcpy(ndev.mac, g_mac, 6);
    net_register(&ndev);

    kprintf("[RTL8139] Ready\n");
}