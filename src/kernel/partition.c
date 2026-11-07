#include <partition.h>
#include <ata.h>
#include <printf.h>
#include <string.h>
#include <stdint.h>

int mbr_parse(const struct ata_device *dev,
              struct mbr_partition *out, int max) {
    if (!dev || !out || max <= 0) return 0;

    uint8_t sector[512];
    if (ata_read_sector(dev, 0, sector) < 0) {
        return 0;
    }

    /* MBR 签名 */
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return 0;
    }

    int n = 0;
    for (int i = 0; i < MBR_MAX_PRIMARY && n < max; i++) {
        const uint8_t *entry = &sector[446 + i * 16];

        uint8_t type = entry[4];
        if (type == 0) continue;

        /* 合理性检查：start_lba 不能是 0（MBR 保护区），
         * sector_count 不能为 0 */
        uint32_t start_lba, sector_count;
        memcpy(&start_lba,    entry + 8,  4);
        memcpy(&sector_count, entry + 12, 4);
        if (start_lba == 0 || sector_count == 0) continue;

        out[n].bootable     = entry[0];
        out[n].type         = type;
        out[n].start_lba    = start_lba;
        out[n].sector_count = sector_count;
        n++;
    }
    return n;
}