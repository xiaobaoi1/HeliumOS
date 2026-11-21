#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

#define AHCI_CAP     0x00
#define AHCI_GHC     0x04
#define AHCI_IS      0x08
#define AHCI_PI      0x0C
#define AHCI_VS      0x10

#define AHCI_PORT_BASE(n)  (0x100 + (n) * 0x80)

#define PORT_CLB     0x00
#define PORT_CLBU    0x04
#define PORT_FB      0x08
#define PORT_FBU     0x0C
#define PORT_IS      0x10
#define PORT_IE      0x14
#define PORT_CMD     0x18
#define PORT_TFD     0x20
#define PORT_SIG     0x24
#define PORT_SSTS    0x28
#define PORT_SCTL    0x2C
#define PORT_SERR    0x30
#define PORT_SACT    0x34
#define PORT_CI      0x38

#define GHC_AE       (1u << 31)

#define CMD_ST       (1u << 0)
#define CMD_SUD      (1u << 1)
#define CMD_POD      (1u << 2)
#define CMD_CLO      (1u << 3)
#define CMD_FRE      (1u << 4)
#define CMD_FR       (1u << 14)
#define CMD_CR       (1u << 15)

#define SSTS_DET_PRESENT 3

#define SIG_ATA   0x00000101
#define SIG_ATAPI 0xEB140101

#define IS_TFES   (1u << 30)
#define IS_HBFS   (1u << 29)
#define IS_HBDS   (1u << 28)
#define IS_IFS    (1u << 27)
#define IS_INFS   (1u << 26)
#define IS_OFS    (1u << 24)

#define FIS_TYPE_REG_H2D  0x27

#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35

#define AHCI_MAX_PORTS     32
#define AHCI_PRDT_ENTRIES  8
#define AHCI_DMA_BUF_SIZE  (64 * 1024)

int  ahci_init(void);
int  ahci_ready(void);
int  ahci_device_count(void);
int  ahci_get_port(int idx);
int  ahci_read_sectors(uint8_t port, uint32_t lba, uint32_t count, uint8_t *buffer);
int  ahci_write_sectors(uint8_t port, uint32_t lba, uint32_t count, const uint8_t *buffer);

#endif