#include <audio.h>
#include <pci.h>
#include <io.h>
#include <printf.h>
#include <pmm.h>
#include <uaccess.h>
#include <task.h>
#include <scheduler.h>
#include <stdint.h>

#define AC97_VENDOR          0x8086
#define AC97_DEVICE          0x2415

/* Native Audio Mixer（BAR0，I/O） */
#define AC97_REG_RESET       0x00
#define AC97_REG_MASTER_VOL  0x02
#define AC97_REG_PCM_VOL     0x18
#define AC97_REG_PCM_RATE    0x2C

/* Native Audio Bus Master（BAR1，I/O） */
#define PO_BDBAR             0x10
#define PO_CIV               0x14
#define PO_LVI               0x15
#define PO_SR                0x16
#define PO_PICB              0x18
#define PO_CR                0x1B
#define GLOB_CNT             0x2C

#define PO_SR_DCH            0x0001
#define PO_CR_RPBM           0x01

#define AUDIO_MAX_BYTES      (1024 * 1024)
#define AUDIO_DESC_BYTES     (64 * 1024)
#define AUDIO_DESC_COUNT     (AUDIO_MAX_BYTES / AUDIO_DESC_BYTES)
#define AUDIO_DMA_PAGES      (AUDIO_MAX_BYTES / 4096)

static uint16_t g_nam = 0;
static uint16_t g_nabm = 0;
static int g_ready = 0;
static int g_playing = 0;
static int g_volume = 80;

static uint32_t g_bdl_phys = 0;
static uint32_t g_dma_phys = 0;
static volatile uint32_t *g_bdl = NULL;
static uint8_t *g_dma = NULL;

static uint16_t ac97_read(uint16_t base, uint8_t reg) {
    return inw((uint16_t)(base + reg));
}

static void ac97_write(uint16_t base, uint8_t reg, uint16_t val) {
    outw((uint16_t)(base + reg), val);
    (void)inb(0x80);
}

/* 在系统调用上下文里睡眠，让其他任务继续运行。 */
static void audio_sleep_ms(uint32_t ms) {
    struct task *cur = get_current_task();
    if (!cur || ms == 0) return;
    cur->sleep_ticks = ms;
    block_current(TASK_STATE_SLEEPING);
    schedule();
}

static uint16_t volume_reg(int percent) {
    if (percent <= 0) return 0x8000;          /* mute */
    if (percent > 100) percent = 100;
    uint32_t atten = (uint32_t)(100 - percent) * 63u / 100u;
    if (atten > 63) atten = 63;
    return (uint16_t)((atten << 8) | atten);
}

void audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume = percent;
    if (g_nam) {
        uint16_t v = volume_reg(percent);
        ac97_write(g_nam, AC97_REG_MASTER_VOL, v);
        ac97_write(g_nam, AC97_REG_PCM_VOL, v);
    }
}

int audio_get_volume(void) {
    return g_volume;
}

static void audio_dma_stop(void) {
    if (!g_nabm) return;
    outb((uint16_t)(g_nabm + PO_CR), 0);
    outw((uint16_t)(g_nabm + PO_SR), 0x1C);
}

/* 播放 44.1kHz / 16-bit / stereo PCM。阻塞直到 DMA 完成。 */
int audio_play_user(uint32_t uaddr, uint32_t len) {
    if (!g_ready || !g_nabm || !g_dma || !g_bdl) return -1;
    if (len == 0) return 0;
    if (len > AUDIO_MAX_BYTES) return -2;
    if (uaddr + len < uaddr) return -3;
    if (check_user_range(uaddr, len) < 0) return -3;
    if (g_playing) return -4;
    if ((len & 1) != 0) len--;

    g_playing = 1;

    if (copy_from_user(g_dma, uaddr, len) < 0) {
        g_playing = 0;
        return -5;
    }

    int n = (int)((len + AUDIO_DESC_BYTES - 1) / AUDIO_DESC_BYTES);
    if (n < 1) n = 1;
    if (n > AUDIO_DESC_COUNT) n = AUDIO_DESC_COUNT;

    for (int i = 0; i < n; i++) {
        uint32_t off = (uint32_t)i * AUDIO_DESC_BYTES;
        uint32_t chunk = len - off;
        if (chunk > AUDIO_DESC_BYTES) chunk = AUDIO_DESC_BYTES;
        g_bdl[i * 2] = g_dma_phys + off;
        /* 低 16 位 = 样本数（16-bit word），bit31 = IOC */
        g_bdl[i * 2 + 1] = (chunk / 2) | 0x80000000u;
    }

    audio_dma_stop();
    outl((uint16_t)(g_nabm + PO_BDBAR), g_bdl_phys);
    outb((uint16_t)(g_nabm + PO_LVI), (uint8_t)(n - 1));
    outb((uint16_t)(g_nabm + PO_CR), PO_CR_RPBM);

    /* 不使用 IOCE：AC'97 中断线未注册 handler，边沿/电平中断会淹没
     * 整个系统。DMA 完成状态通过轮询 PO_SR.DCH 检测。 */

    /* 给 DMA 一点启动时间，再轮询“缓冲区耗尽”状态。 */
    audio_sleep_ms(2);
    for (int i = 0; i < 20000; i++) {
        uint16_t sr = inw((uint16_t)(g_nabm + PO_SR));
        if (sr & PO_SR_DCH) break;
        audio_sleep_ms(1);
    }

    audio_dma_stop();
    g_playing = 0;
    return (int)len;
}

int audio_init(void) {
    const struct pci_device *dev = pci_find(AC97_VENDOR, AC97_DEVICE);
    if (!dev) {
        const struct pci_device *hda = pci_find_class(0x04, 0x03);
        if (hda) {
            uint32_t bar = hda->bar[0];
            uint32_t phys = PCI_BAR_MEM_ADDR(bar);
            if (!(bar & 1) && pci_bar_accessible(phys)) {
                volatile uint32_t *mmio = (volatile uint32_t*)phys;
                uint16_t gcap = (uint16_t)(mmio[0] & 0xFFFF);
                uint16_t states = (uint16_t)(mmio[2] & 0xFFFF);
                kprintf("[AUDIO] HDA loaded: vendor=%04x device=%04x "
                        "gcap=%04x codecs=%04x\n",
                        hda->vendor_id, hda->device_id, gcap, states);
                return 0;
            }
            kprintf("[AUDIO] HDA found but MMIO unavailable\n");
            return -1;
        }
        kprintf("[AUDIO] no supported sound card found\n");
        return -1;
    }

    uint32_t bar0 = dev->bar[0];
    uint32_t bar1 = dev->bar[1];
    if (!(bar0 & 1)) {
        kprintf("[AUDIO] AC97 BAR0 is not I/O space\n");
        return -1;
    }
    g_nam = (uint16_t)PCI_BAR_IO_ADDR(bar0);
    if (bar1 & 1) g_nabm = (uint16_t)PCI_BAR_IO_ADDR(bar1);

    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func,
                                   PCI_COMMAND);
    cmd |= 0x7;          /* I/O + memory + bus master */
    pci_write_config(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);

    if (g_nabm) {
        /* 释放 AC'97 冷复位，否则 bus master 寄存器不可用。 */
        outl((uint16_t)(g_nabm + GLOB_CNT), 0x00000002);
        for (volatile int i = 0; i < 100000; i++) { }
    }

    ac97_write(g_nam, AC97_REG_RESET, 1);
    for (volatile int i = 0; i < 100000; i++) { }
    uint16_t id = ac97_read(g_nam, 0x28);
    uint16_t vol = ac97_read(g_nam, AC97_REG_MASTER_VOL);
    if (id == 0xFFFF && vol == 0xFFFF) {
        kprintf("[AUDIO] AC97 at I/O 0x%x probe failed\n", g_nam);
        return -1;
    }

    audio_set_volume(g_volume);
    /* PCM Front DAC Rate：固定 44.1kHz（多数 AC'97 codec 支持 VRA） */
    ac97_write(g_nam, AC97_REG_PCM_RATE, 44100);

    if (!g_nabm) {
        kprintf("[AUDIO] AC97 has no bus-master BAR; playback disabled\n");
        return 0;
    }

    g_bdl_phys = pmm_alloc_page();
    g_dma_phys = pmm_alloc_pages(AUDIO_DMA_PAGES);
    if (!g_bdl_phys || !g_dma_phys) {
        kprintf("[AUDIO] AC97 DMA allocation failed\n");
        if (g_dma_phys) pmm_free_pages(g_dma_phys, AUDIO_DMA_PAGES);
        if (g_bdl_phys) pmm_free_page(g_bdl_phys);
        g_bdl_phys = g_dma_phys = 0;
        return -1;
    }
    g_bdl = (volatile uint32_t*)g_bdl_phys;
    g_dma = (uint8_t*)g_dma_phys;
    for (uint32_t i = 0; i < AUDIO_DMA_PAGES * 4096u; i++) g_dma[i] = 0;
    for (int i = 0; i < AUDIO_DESC_COUNT * 2; i++) g_bdl[i] = 0;

    audio_dma_stop();
    g_ready = 1;

    kprintf("[AUDIO] AC97 loaded successfully: vendor=%04x device=%04x "
            "io=0x%x bm=0x%x ext_id=%04x dma=%p\n",
            dev->vendor_id, dev->device_id, g_nam, g_nabm, id,
            (void*)g_dma_phys);
    return 0;
}
