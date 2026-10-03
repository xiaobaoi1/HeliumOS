#include <partition.h>
#include <ata.h>
#include <printf.h>
#include <string.h>
#include <stdint.h>

int mbr_parse(const struct ata_device *dev,
              struct mbr_partition *out, int max) {
    if (!dev || !out || max <= 0) return 0;

    uint8_t sector[512];
    ata_read_sector(dev, 0, sector);

    /* MBR 签名：offset 510-511 = 0x55 0xAA */
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return 0;
    }

    int n = 0;
    for (int i = 0; i < MBR_MAX_PRIMARY && n < max; i++) {
        const uint8_t *entry = &sector[446 + i * 16];

        uint8_t type = entry[4];
        if (type == 0) continue;   /* 空项 */

        out[n].bootable = entry[0];
        out[n].type     = type;
        memcpy(&out[n].start_lba,    entry + 8,  4);
        memcpy(&out[n].sector_count, entry + 12, 4);
        n++;
    }
    return n;
}