/*
 * mkfs.c — InvariantFS volume creation
 *
 *   invf-mkfs <image|device> [size_gb]
 *   invf-mkfs <img0> <size0_gb> <img1> <size1_gb>    (WP25: two devices)
 *
 * Creates a sparse image file, lays out zones per spec:
 *   Block 0: Superblock
 *   Metadata Zone: bitmap + L2P journal + indexes (reserved)
 *   RAW Zone: 20% of remaining
 *   Shadow Space: 80% of remaining
 *
 * WP-DZ: the RAW/Shadow split written here is the ADVISORY initial policy
 * of the allocator, not a hard region split (one shared free pool;
 * raw-class writes prefer the raw extent and overflow into shadow blocks
 * without changing class -- see alloc_raw_or_shadow in volume.c).
 * Backing store is either an image file or a raw block device -- see blkio.c.
 * On a device the size argument is a cap, not a setting: the partition is as
 * large as it is, and asking for more than it holds is an error rather than
 * something we can grow into.
 *
 *   invf-mkfs vol.img 10      -> 10 GB sparse file
 *   invf-mkfs W:              -> the whole W: partition
 *   invf-mkfs \\.\W: 4        -> the first 4 GB of the W: partition
 *
 * WP25 two-device form: dev0 (fast) + dev1 (capacity), one global block
 * space = dev0 ++ dev1. dev0 holds the metadata span + the RAW zone + the
 * tier arena (its tail); dev1 holds the byte-identical metadata mirror at
 * the same local offsets, then a RAW-width reserved gap (keeps the shadow
 * start aligned across both views), then the whole canonical shadow zone.
 * The DEVT descriptor at 0x2A0 of block 0 (both devices) records the
 * geometry (see invarifs.h); a single-device volume leaves it zeroed, so
 * old images are byte-identical to pre-WP25 ones.
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

/* parse a size in GB (float, the historical convention) */
static uint64_t size_arg_gb(const char *s)
{
    double gb = strtod(s, NULL);
    uint64_t b = (uint64_t)(gb * 1024.0 * 1024.0 * 1024.0);
    return b - b % INVFS_BLOCK_SIZE;
}

/* WP25: erase the head of a to-be-mirrored metadata span on one device
 * (the is_dev path of single-device mkfs, factored so BOTH devices of a
 * two-device volume get it): kill the old filesystem's signatures, both
 * journal slot headers, and the whole inode area. */
static int dev_erase_meta(blkio *io, uint64_t size_bytes,
                          uint64_t bitmap_blocks, uint64_t meta_blocks)
{
    uint64_t jstart = (1 + bitmap_blocks) * (uint64_t)INVFS_BLOCK_SIZE;
    uint64_t istart = (1 + bitmap_blocks + INVFS_JOURNAL_BLOCKS)
                    * (uint64_t)INVFS_BLOCK_SIZE;
    size_t zn = 1024u * 1024u;
    uint8_t *zero;
    int bad = 0;

    if ((uint64_t)zn > size_bytes) zn = (size_t)size_bytes;
    zero = (uint8_t *)calloc(1, zn);
    if (!zero) return -1;
    bad |= blkio_pwrite(io, 0, zero, zn) != 0;
    /* The journal usually falls inside that first megabyte and the inode
       area usually does not, but both depend on the volume size, so zero
       them by their computed offsets rather than by assumption.
       WP22d: the journal is two slots; a stale CRC-valid slot header
       would win the replay (the selector is only a hint) -- so zero the
       WHOLE journal area, not just the two slot header blocks.
       A re-mkfs of a device that previously held a volume must render
       every previous journal byte unreadable: the stale log tail left
       behind by a header-only erase survives a fresh volume's first
       empty compaction (its slot header reuses seq 1, so jrn_seed
       collides with the old volume's) and is replayed into the fresh
       bitmap as phantom owner maps -- the WP27 "orphans after mkfs+import"
       regression. */
    {
        uint64_t jend = jstart +
            (uint64_t)INVFS_JOURNAL_BLOCKS * INVFS_BLOCK_SIZE;
        uint64_t p = jstart;
        while (p < jend && !bad) {
            size_t c = zn;
            if (p + c > jend) c = (size_t)(jend - p);
            bad |= blkio_pwrite(io, p, zero, c) != 0;
            p += c;
        }
    }
    if (istart + INVFS_BLOCK_SIZE <= size_bytes) {
        /* Zero the WHOLE inode area, not just its head block: the
         * record scan stops at the first invalid record, and fresh
         * records written by the first imports outgrow a one-block
         * head within minutes -- anything past it would be the previous
         * volume's still-CRC-valid records coming back from the dead
         * (seen as a flood of l2p_miss on a re-mkfs'd device). */
        uint64_t iend = (1 + meta_blocks) * (uint64_t)INVFS_BLOCK_SIZE;
        uint64_t p = istart;
        while (p < iend && !bad) {
            size_t c = zn;
            if (p + c > iend) c = (size_t)(iend - p);
            bad |= blkio_pwrite(io, p, zero, c) != 0;
            p += c;
        }
    }
    free(zero);
    return bad ? -1 : 0;
}

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
    /* WP25 two-device state (dev1 absent = the single-device path, whose
     * image stays byte-identical to pre-WP25 mkfs) */
    blkio io2;
    const char *path2 = NULL;
    char devbuf2[64];
    uint64_t size2_bytes = 0, dev0_blocks = 0, dev1_blocks = 0;
    int is_dev2 = 0, twodev = 0;
    invfs_devt devt;

    if (argc < 2 || argc == 4 || argc > 5) {
        fprintf(stderr, "usage: invf-mkfs <image|device> [size_gb]\n"
                        "  invf-mkfs vol.img 10    10 GB sparse image file\n"
                        "  invf-mkfs W:            the whole W: partition\n"
                        "two devices (WP25):\n"
                        "  invf-mkfs dev0.img <size0_gb> dev1.img <size1_gb>\n"
                        "  dev0 = fast (metadata + RAW + tier arena),\n"
                        "  dev1 = capacity (metadata mirror + shadow)\n");
        return 2;
    }
    twodev = (argc == 5);
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
        if (argc >= 3) {
            size_bytes = size_arg_gb(argv[2]);
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
        double gb = (argc >= 3) ? strtod(argv[2], NULL) : 4.0;
        size_bytes = (uint64_t)(gb * 1024.0 * 1024.0 * 1024.0);
    }

    if (twodev) {
        path2 = blkio_normalize(argv[3], devbuf2, sizeof devbuf2);
        is_dev2 = blkio_looks_like_device(path2);
        rc = blkio_open(&io2, path2,
                        BLKIO_CREATE | (is_dev2 ? BLKIO_EXCLUSIVE : 0));
        if (rc != 0) {
            fprintf(stderr, "cannot open %s: %s\n", path2,
                    blkio_strerror(rc));
            blkio_close(&io);
            return 1;
        }
        size2_bytes = size_arg_gb(argv[4]);
        if (is_dev2) {
            uint64_t cap = blkio_capacity(&io2);
            if (size2_bytes > cap) {
                fprintf(stderr, "device %s holds %llu bytes, %llu "
                        "requested\n", path2, (unsigned long long)cap,
                        (unsigned long long)size2_bytes);
                blkio_close(&io2);
                blkio_close(&io);
                return 1;
            }
        }
        if (size2_bytes < 16ull * 1024ull * 1024ull) {
            fprintf(stderr, "device 1 too small: %llu bytes (min 16MB)\n",
                    (unsigned long long)size2_bytes);
            blkio_close(&io2);
            blkio_close(&io);
            return 1;
        }
    }

    if (size_bytes < 64ull * 1024ull * 1024ull) {
        fprintf(stderr, "volume too small: %llu bytes (min 64MB)\n",
                (unsigned long long)size_bytes);
        if (twodev) blkio_close(&io2);
        blkio_close(&io);
        return 1;
    }

    if ((rc = blkio_chsize(&io, size_bytes)) != 0) {
        fprintf(stderr, "cannot set size to %llu bytes: %s\n",
                (unsigned long long)size_bytes, blkio_strerror(rc));
        if (twodev) blkio_close(&io2);
        blkio_close(&io);
        return 1;
    }
    if (twodev && (rc = blkio_chsize(&io2, size2_bytes)) != 0) {
        fprintf(stderr, "cannot set device 1 size to %llu bytes: %s\n",
                (unsigned long long)size2_bytes, blkio_strerror(rc));
        blkio_close(&io2);
        blkio_close(&io);
        return 1;
    }

    /* ---- zone layout ---- */
    dev0_blocks = size_bytes / INVFS_BLOCK_SIZE;
    dev1_blocks = twodev ? size2_bytes / INVFS_BLOCK_SIZE : 0;
    total_blocks = dev0_blocks + dev1_blocks;
    bitmap_blocks = div_ceil(total_blocks / 8, INVFS_BLOCK_SIZE);
    /* metadata = bitmap + L2P journal + inode area.
     * The inode area used to be a flat 512 blocks (2 MB) whatever the volume
     * size, which caps a volume at ~6200 files: an inode record is ~336 bytes
     * for a small file. A 2 GB image filled up at 6241 files and every write
     * after that failed. Scale it with the volume instead -- 1/64 of the
     * blocks, which at the ~336-byte record size is roughly one file per
     * 21 KB of volume, with the old 512 as the floor for tiny images. */
    /* INVFS_META_FRAC tunes the inode-area share for churn-heavy
     * workloads: every rewrite appends [new record + tombstone], and a
     * package-manager session can append hundreds of thousands of
     * records. Default 64; rootfs images want 16-24. */
    {
        long frac = 64;
        const char *mf = getenv("INVFS_META_FRAC");
        if (mf) {
            long v = strtol(mf, NULL, 10);
            if (v >= 8 && v <= 256) frac = v;
        }
        inode_blocks = total_blocks / (uint64_t)frac;
    }
    if (inode_blocks < 512) inode_blocks = 512;
    metadata_blocks = bitmap_blocks + INVFS_JOURNAL_BLOCKS + inode_blocks;
    if (metadata_blocks < 256)
        metadata_blocks = 256;
    rem = total_blocks - 1 - metadata_blocks;  /* superblock + metadata */
    raw_blocks = rem / INVFS_RAW_FRACTION_DEN * INVFS_RAW_FRACTION_NUM;  /* 20% */
    if (twodev) {
        /* dev0 = metadata + RAW + tier arena (its tail); dev1 = the
         * metadata mirror + a RAW-width reserved gap + the canonical
         * shadow (see the DEVT comment in invarifs.h) */
        if (1 + metadata_blocks + raw_blocks > dev0_blocks) {
            fprintf(stderr, "device 0 too small: metadata (%llu blocks) + "
                    "RAW (%llu blocks) = %llu, dev0 has %llu\n",
                    (unsigned long long)(1 + metadata_blocks),
                    (unsigned long long)raw_blocks,
                    (unsigned long long)(1 + metadata_blocks + raw_blocks),
                    (unsigned long long)dev0_blocks);
            blkio_close(&io2);
            blkio_close(&io);
            return 1;
        }
        if (1 + metadata_blocks + raw_blocks >= dev1_blocks) {
            fprintf(stderr, "device 1 too small: the mirror span + gap "
                    "(%llu blocks) leaves no shadow in %llu blocks\n",
                    (unsigned long long)(1 + metadata_blocks + raw_blocks),
                    (unsigned long long)dev1_blocks);
            blkio_close(&io2);
            blkio_close(&io);
            return 1;
        }
        shadow_blocks = dev1_blocks - (1 + metadata_blocks + raw_blocks);
    } else {
        shadow_blocks = rem - raw_blocks;
    }

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
    sb.shadow_zone_start = twodev ? dev0_blocks + 1 + metadata_blocks +
                                    raw_blocks
                                  : 1 + metadata_blocks + raw_blocks;
    sb.shadow_zone_blocks = shadow_blocks;
    sb.sweep_cursor = 0;
    /* ENOSPC policy: reserve for sweep/transcodes + hard-min floor;
     * ordinary writes must keep (reserved + hard_min) free */
    sb.reserved_blocks = (uint32_t)(sb.total_blocks / 128 + 64);
    sb.hard_min_blocks = (uint32_t)(sb.total_blocks / 1024 + 16);
    sb.vol_flags = VOLF_META2 | VOLF_ASTV2;   /* fresh volumes are format v2 */
    sb.pad2 = 0;
    sb.checksum = invfs_crc32c(&sb, offsetof(invfs_superblock, checksum));

    /* WP25: the DEVT device table (block 0 at 0x2A0), written on both
     * devices of a two-device volume; zeros (absent) otherwise. */
    memset(&devt, 0, sizeof devt);
    if (twodev) {
        invfs_devt t;
        memcpy(devt.magic, "DEVT", 4);
        devt.version = INVFS_DEVT_VERSION;
        devt.dev_count = 2;
        devt.flags = INVFS_DEVTF_RAW_MIRROR;   /* default raw_mirror=1 */
        devt.dev_blocks[0] = dev0_blocks;
        devt.dev_blocks[1] = dev1_blocks;
        devt.sync_seq = 1;
        memcpy(devt.vol_uuid, sb.uuid, 16);
        /* the hint is the path AS GIVEN (INVFS_DEV1 overrides it). Do
         * NOT absolutize: blkio treats /dev/* paths as raw devices, so a
         * realpath of a /dev/shm image would become unusable -- the
         * as-given relative form is what the tools can always open from
         * the volume's directory. */
        snprintf(devt.dev1_hint, sizeof devt.dev1_hint, "%s", path2);
        t = devt;
        t.crc32c = 0;
        devt.crc32c = invfs_crc32c(&t, sizeof t);
    }
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
     * XFS/Btrfs signatures. WP25: BOTH devices of a two-device volume get
     * the erase -- the mirror span starts byte-identical, not stale. */
    if (is_dev &&
        dev_erase_meta(&io, size_bytes, bitmap_blocks,
                       metadata_blocks) != 0) {
        fprintf(stderr, "cannot erase the old filesystem on %s\n", path);
        if (twodev) blkio_close(&io2);
        blkio_close(&io);
        return 1;
    }
    if (twodev && is_dev2 &&
        dev_erase_meta(&io2, size2_bytes, bitmap_blocks,
                       metadata_blocks) != 0) {
        fprintf(stderr, "cannot erase the old filesystem on %s\n", path2);
        blkio_close(&io2);
        blkio_close(&io);
        return 1;
    }

    /* ---- write superblock (block 0) ---- */
    if (blkio_seek(&io, 0) != 0 ||
        blkio_write(&io, &sb, sizeof(sb)) != 0) {
        fprintf(stderr, "superblock write failed\n");
        if (twodev) blkio_close(&io2);
        blkio_close(&io);
        return 1;
    }

    /* WP20b: no redundancy descriptor on a fresh volume. The superblock
     * write covers only sizeof(sb) bytes of block 0, and while a fresh
     * image file is all zeros (and a device got the 1 MB erase above), a
     * re-mkfs of a same-size image file keeps the old bytes -- blkio_chsize
     * does not wipe. Zero the descriptor explicitly. */
    {
        uint8_t z[sizeof(invfs_rdp0)];
        memset(z, 0, sizeof z);
        if (blkio_seek(&io, INVFS_RDP0_OFF) != 0 ||
            blkio_write(&io, z, sizeof z) != 0) {
            fprintf(stderr, "descriptor area erase failed\n");
            if (twodev) blkio_close(&io2);
            blkio_close(&io);
            return 1;
        }
    }

    /* WP25: the DEVT slot -- the descriptor on a two-device volume, an
     * explicit erase on a single-device one (the RDP0 paranoia twin:
     * zeros read as "absent" either way, and a same-size image re-mkfs
     * must not inherit a stale table). */
    if (blkio_seek(&io, INVFS_DEVT_OFF) != 0 ||
        blkio_write(&io, &devt, sizeof devt) != 0) {
        fprintf(stderr, "device-table write failed\n");
        if (twodev) blkio_close(&io2);
        blkio_close(&io);
        return 1;
    }

    /* ---- bitmap: mark superblock + metadata zone allocated ---- */
    bitmap_bytes = (size_t)bitmap_blocks * INVFS_BLOCK_SIZE;
    bitmap = (uint8_t *)calloc(1, bitmap_bytes);
    if (!bitmap) {
        fprintf(stderr, "out of memory\n");
        if (twodev) blkio_close(&io2);
        blkio_close(&io);
        return 1;
    }
    for (i = 0; i <= metadata_blocks; i++) {  /* block 0..metadata end */
        bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
    }
    if (twodev) {
        /* the dev1 reserved span (metadata mirror + RAW-width gap) is
         * allocated by construction -- never handed out */
        for (i = dev0_blocks; i < sb.shadow_zone_start; i++)
            bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
    }
    if (blkio_seek(&io, sb.metadata_zone_start * INVFS_BLOCK_SIZE) != 0 ||
        blkio_write(&io, bitmap, bitmap_bytes) != 0) {
        fprintf(stderr, "bitmap write failed\n");
        free(bitmap);
        if (twodev) blkio_close(&io2);
        blkio_close(&io);
        return 1;
    }

    /* ---- mark clean ---- */
    sb.state = INVFS_STATE_CLEAN;
    sb.checksum = invfs_crc32c(&sb, offsetof(invfs_superblock, checksum));
    if (blkio_seek(&io, 0) != 0 ||
        blkio_write(&io, &sb, sizeof(sb)) != 0) {
        fprintf(stderr, "superblock re-write failed\n");
        free(bitmap);
        if (twodev) blkio_close(&io2);
        blkio_close(&io);
        return 1;
    }

    /* WP25: the metadata span is byte-identical on BOTH devices (the
     * mirror): block 0 (sb + descriptors + DEVT) and the bitmap land at
     * the same local offsets on dev1; the journal and inode area are
     * zeros there already (fresh image / erased device). */
    if (twodev) {
        int bad = 0;
        bad |= blkio_seek(&io2, 0) != 0 ||
               blkio_write(&io2, &sb, sizeof sb) != 0;
        {
            uint8_t z[sizeof(invfs_rdp0)];
            memset(z, 0, sizeof z);
            bad |= blkio_seek(&io2, INVFS_RDP0_OFF) != 0 ||
                   blkio_write(&io2, z, sizeof z) != 0;
        }
        bad |= blkio_seek(&io2, INVFS_DEVT_OFF) != 0 ||
               blkio_write(&io2, &devt, sizeof devt) != 0;
        bad |= blkio_seek(&io2,
                          sb.metadata_zone_start * INVFS_BLOCK_SIZE) != 0 ||
               blkio_write(&io2, bitmap, bitmap_bytes) != 0;
        if (bad) {
            fprintf(stderr, "device 1 mirror write failed\n");
            free(bitmap);
            blkio_close(&io2);
            blkio_close(&io);
            return 1;
        }
    }
    free(bitmap);

    blkio_flush(&io);
    blkio_close(&io);
    if (twodev) {
        blkio_flush(&io2);
        blkio_close(&io2);
    }

    printf("InvariantFS volume created: %s\n", path);
    if (twodev) {
        printf("  devices:         2 (dev1: %s)\n", path2);
        printf("  dev0:            %llu blocks (metadata + RAW + tier arena)\n",
               (unsigned long long)dev0_blocks);
        printf("  dev1:            %llu blocks (metadata mirror + shadow)\n",
               (unsigned long long)dev1_blocks);
    }
    printf("  size:            %llu bytes (%llu blocks of %u)\n",
           (unsigned long long)(size_bytes + size2_bytes),
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
    if (twodev)
        printf("  tier arena:      blocks %llu .. %llu (dev0 tail, "
               "redundant copies only)\n",
               (unsigned long long)(sb.raw_zone_start + raw_blocks),
               (unsigned long long)(dev0_blocks - 1));
    printf("  state: CLEAN, uuid: ");
    for (i = 0; i < 16; i++)
        printf("%02x", sb.uuid[i]);
    printf("\n");
    return 0;
}
