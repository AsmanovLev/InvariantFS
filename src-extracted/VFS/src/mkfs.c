/*
 * mkfs.c — InvariantFS volume creation
 *
 *   invf-mkfs <image|device> [size_gb]
 *
 * Creates a sparse image file, lays out zones per spec:
 *   Block 0: Superblock
 *   Metadata Zone: bitmap + L2P journal + indexes (reserved)
 *   RAW Zone: 20% of remaining
 *   Shadow Space: 80% of remaining
 *
 * Backing store is either an image file or a raw block device -- see blkio.c.
 * On a device the size argument is a cap, not a setting: the partition is as
 * large as it is, and asking for more than it holds is an error rather than
 * something we can grow into.
 *
 *   invf-mkfs vol.img 10      -> 10 GB sparse file
 *   invf-mkfs W:              -> the whole W: partition
 *   invf-mkfs \\.\W: 4        -> the first 4 GB of the W: partition
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

#include "invarifs.h"
#include "blkio.h"

static void gen_uuid(uint8_t u[16])
{
    /* Not cryptographic — test-grade UUID from time + counter */
#ifdef _WIN32
    LARGE_INTEGER pc;
    QueryPerformanceCounter(&pc);
    uint64_t t = (uint64_t)time(NULL);
    uint64_t c = (uint64_t)pc.QuadPart;
#else
    uint64_t t = (uint64_t)time(NULL);
    uint64_t c = ((uint64_t)rand() << 32) | (uint64_t)rand();
    srand((unsigned)(t ^ (uintptr_t)&c));
#endif
    memcpy(u, &t, 8);
    memcpy(u + 8, &c, 8);
    u[6] = (u[6] & 0x0F) | 0x40;  /* version 4 */
    u[8] = (u[8] & 0x3F) | 0x80;  /* RFC 4122 variant */
}

static uint64_t div_ceil(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

int main(int argc, char **argv)
{
    const char *path;
    char devbuf[64];
    uint64_t size_bytes;
    blkio io;
    invfs_superblock sb;
    uint64_t total_blocks, metadata_blocks, bitmap_blocks, inode_blocks, rem;
    uint64_t raw_blocks, shadow_blocks;
    size_t bitmap_bytes;
    uint8_t *bitmap;
    uint64_t i;
    int is_dev, rc;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: invf-mkfs <image|device> [size_gb]\n"
                        "  invf-mkfs vol.img 10    10 GB sparse image file\n"
                        "  invf-mkfs W:            the whole W: partition\n");
        return 2;
    }
    path = blkio_normalize(argv[1], devbuf, sizeof devbuf);
    is_dev = blkio_looks_like_device(path);

    /* A device is opened exclusively: locked and dismounted, so the
       filesystem currently living there stops writing its own metadata over
       what we are about to lay down. */
    rc = blkio_open(&io, path, BLKIO_CREATE | (is_dev ? BLKIO_EXCLUSIVE : 0));
    if (rc != 0) {
        fprintf(stderr, "cannot open %s: %s\n", path, blkio_strerror(rc));
        return 1;
    }

    if (is_dev) {
        uint64_t cap = blkio_capacity(&io);
        if (argc == 3) {
            double gb = strtod(argv[2], NULL);
            size_bytes = (uint64_t)(gb * 1024.0 * 1024.0 * 1024.0);
            size_bytes -= size_bytes % INVFS_BLOCK_SIZE;
            if (size_bytes > cap) {
                fprintf(stderr, "device %s holds %llu bytes, %llu requested\n",
                        path, (unsigned long long)cap,
                        (unsigned long long)size_bytes);
                blkio_close(&io);
                return 1;
            }
        } else {
            size_bytes = cap;   /* the whole partition */
        }
    } else {
        double gb = (argc == 3) ? strtod(argv[2], NULL) : 4.0;
        size_bytes = (uint64_t)(gb * 1024.0 * 1024.0 * 1024.0);
    }

    if (size_bytes < 64ull * 1024ull * 1024ull) {
        fprintf(stderr, "volume too small: %llu bytes (min 64MB)\n",
                (unsigned long long)size_bytes);
        blkio_close(&io);
        return 1;
    }

    if ((rc = blkio_chsize(&io, size_bytes)) != 0) {
        fprintf(stderr, "cannot set size to %llu bytes: %s\n",
                (unsigned long long)size_bytes, blkio_strerror(rc));
        blkio_close(&io);
        return 1;
    }

    /* ---- zone layout ---- */
    total_blocks = size_bytes / INVFS_BLOCK_SIZE;
    bitmap_blocks = div_ceil(total_blocks / 8, INVFS_BLOCK_SIZE);
    /* metadata = bitmap + L2P journal + inode area.
     * The inode area used to be a flat 512 blocks (2 MB) whatever the volume
     * size, which caps a volume at ~6200 files: an inode record is ~336 bytes
     * for a small file. A 2 GB image filled up at 6241 files and every write
     * after that failed. Scale it with the volume instead -- 1/64 of the
     * blocks, which at the ~336-byte record size is roughly one file per
     * 21 KB of volume, with the old 512 as the floor for tiny images. */
    inode_blocks = total_blocks / 64;
    if (inode_blocks < 512) inode_blocks = 512;
    metadata_blocks = bitmap_blocks + INVFS_JOURNAL_BLOCKS + inode_blocks;
    if (metadata_blocks < 256)
        metadata_blocks = 256;
    rem = total_blocks - 1 - metadata_blocks;  /* superblock + metadata */
    raw_blocks = rem / INVFS_RAW_FRACTION_DEN * INVFS_RAW_FRACTION_NUM;  /* 20% */
    shadow_blocks = rem - raw_blocks;

    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, INVFS_MAGIC, 8);
    gen_uuid(sb.uuid);
    sb.state = INVFS_STATE_DIRTY;  /* until we finish cleanly */
    sb.block_size = INVFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.metadata_zone_start = 1;
    sb.metadata_zone_blocks = metadata_blocks;
    sb.raw_zone_start = 1 + metadata_blocks;
    sb.raw_zone_blocks = raw_blocks;
    sb.shadow_zone_start = 1 + metadata_blocks + raw_blocks;
    sb.shadow_zone_blocks = shadow_blocks;
    sb.sweep_cursor = 0;
    /* ENOSPC policy: reserve for sweep/transcodes + hard-min floor;
     * ordinary writes must keep (reserved + hard_min) free */
    sb.reserved_blocks = (uint32_t)(sb.total_blocks / 128 + 64);
    sb.hard_min_blocks = (uint32_t)(sb.total_blocks / 1024 + 16);
    sb.vol_flags = 0;
    sb.pad2 = 0;
    sb.checksum = invfs_crc32c(&sb, offsetof(invfs_superblock, checksum));

    /* ---- erase whatever filesystem was here ---- *
     * On a device this is not cosmetic, for two separate reasons.
     *
     * First, the superblock is 144 bytes and a sector is up to 4096, so
     * writing it goes through read-modify-write and leaves bytes 144..4095 of
     * the old boot sector intact -- including the 0x55AA signature at offset
     * 510. Windows would go on recognising the partition as FAT32, offer to
     * "repair" it, and eventually write FAT metadata over ours.
     *
     * Second, and worse: an image file is created with CREATE_ALWAYS and is
     * therefore all zeroes, but a device keeps whatever was on it. vol_open
     * scans the inode area and replays the journal until it hits a record
     * that fails its magic or CRC check -- on zeroes that is the very first
     * record, which is why this has never mattered. Re-running mkfs on a
     * device that already held an InvariantFS volume would leave *valid* records
     * there, and the fresh volume would adopt the previous volume's files.
     * Zeroing the head of each area makes the first record invalid again.
     *
     * The 1 MB head covers the FAT32 boot sector and its backup at sector 6,
     * the NTFS boot sector, the ext2/3/4 superblock at 1024, and the
     * XFS/Btrfs signatures. */
    if (is_dev) {
        uint64_t jstart = (1 + bitmap_blocks) * (uint64_t)INVFS_BLOCK_SIZE;
        uint64_t istart = (1 + bitmap_blocks + INVFS_JOURNAL_BLOCKS)
                        * (uint64_t)INVFS_BLOCK_SIZE;
        size_t zn = 1024u * 1024u;
        uint8_t *zero;
        int bad = 0;

        if ((uint64_t)zn > size_bytes) zn = (size_t)size_bytes;
        zero = (uint8_t *)calloc(1, zn);
        if (!zero) {
            fprintf(stderr, "out of memory\n");
            blkio_close(&io);
            return 1;
        }
        bad |= blkio_pwrite(&io, 0, zero, zn) != 0;
        /* The journal usually falls inside that first megabyte and the inode
           area usually does not, but both depend on the volume size, so zero
           them by their computed offsets rather than by assumption. */
        if (jstart + INVFS_BLOCK_SIZE <= size_bytes)
            bad |= blkio_pwrite(&io, jstart, zero, INVFS_BLOCK_SIZE) != 0;
        if (istart + INVFS_BLOCK_SIZE <= size_bytes)
            bad |= blkio_pwrite(&io, istart, zero, INVFS_BLOCK_SIZE) != 0;
        free(zero);
        if (bad) {
            fprintf(stderr, "cannot erase the old filesystem on %s\n", path);
            blkio_close(&io);
            return 1;
        }
    }

    /* ---- write superblock (block 0) ---- */
    if (blkio_seek(&io, 0) != 0 ||
        blkio_write(&io, &sb, sizeof(sb)) != 0) {
        fprintf(stderr, "superblock write failed\n");
        blkio_close(&io);
        return 1;
    }

    /* ---- bitmap: mark superblock + metadata zone allocated ---- */
    bitmap_bytes = (size_t)bitmap_blocks * INVFS_BLOCK_SIZE;
    bitmap = (uint8_t *)calloc(1, bitmap_bytes);
    if (!bitmap) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    for (i = 0; i <= metadata_blocks; i++) {  /* block 0..metadata end */
        bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
    }
    if (blkio_seek(&io, sb.metadata_zone_start * INVFS_BLOCK_SIZE) != 0 ||
        blkio_write(&io, bitmap, bitmap_bytes) != 0) {
        fprintf(stderr, "bitmap write failed\n");
        free(bitmap);
        blkio_close(&io);
        return 1;
    }
    free(bitmap);

    /* ---- mark clean ---- */
    sb.state = INVFS_STATE_CLEAN;
    sb.checksum = invfs_crc32c(&sb, offsetof(invfs_superblock, checksum));
    if (blkio_seek(&io, 0) != 0 ||
        blkio_write(&io, &sb, sizeof(sb)) != 0) {
        fprintf(stderr, "superblock re-write failed\n");
        blkio_close(&io);
        return 1;
    }

    blkio_flush(&io);
    blkio_close(&io);

    printf("InvariantFS volume created: %s\n", path);
    printf("  size:            %llu bytes (%llu blocks of %u)\n",
           (unsigned long long)size_bytes,
           (unsigned long long)total_blocks, INVFS_BLOCK_SIZE);
    printf("  metadata zone:   blocks %llu .. %llu (%llu blocks, %llu MB)\n",
           (unsigned long long)sb.metadata_zone_start,
           (unsigned long long)(sb.metadata_zone_start + metadata_blocks - 1),
           (unsigned long long)metadata_blocks,
           (unsigned long long)(metadata_blocks * INVFS_BLOCK_SIZE / 1024 / 1024));
    printf("  raw zone:        blocks %llu .. %llu (%llu blocks, %llu MB)\n",
           (unsigned long long)sb.raw_zone_start,
           (unsigned long long)(sb.raw_zone_start + raw_blocks - 1),
           (unsigned long long)raw_blocks,
           (unsigned long long)(raw_blocks * INVFS_BLOCK_SIZE / 1024 / 1024));
    printf("  shadow zone:     blocks %llu .. %llu (%llu blocks, %llu MB)\n",
           (unsigned long long)sb.shadow_zone_start,
           (unsigned long long)(sb.shadow_zone_start + shadow_blocks - 1),
           (unsigned long long)shadow_blocks,
           (unsigned long long)(shadow_blocks * INVFS_BLOCK_SIZE / 1024 / 1024));
    printf("  state: CLEAN, uuid: ");
    for (i = 0; i < 16; i++)
        printf("%02x", sb.uuid[i]);
    printf("\n");
    return 0;
}
