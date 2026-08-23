/*
 * crc32c.c — CRC32C (Castagnoli, polynomial 0x1EDC6F41)
 * Table-driven, little-endian variant used by BTRFS/EXT4/CRC32C-SSE.
 */
#include "invarifs.h"

static uint32_t crc32c_table[256];
static int crc32c_table_ready = 0;

static void crc32c_init(void)
{
    uint32_t i, j;
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (j = 0; j < 8; j++) {
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        }
        crc32c_table[i] = c;
    }
    crc32c_table_ready = 1;
}

uint32_t invfs_crc32c(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    if (!crc32c_table_ready)
        crc32c_init();

    while (len--)
        crc = (crc >> 8) ^ crc32c_table[(crc ^ *p++) & 0xFF];

    return crc ^ 0xFFFFFFFFu;
}
