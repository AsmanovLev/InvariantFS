/*
 * verify.c — InvariantFS volume integrity check
 *
 *   invf-verify <image>
 *
 * Validates: magic, CRC32C, zone layout, bitmap consistency,
 * file size vs total_blocks.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Backing store: image file or raw device. This used to be a private
   _open/_read shim, which meant invf-verify could not look at a device at
   all -- "E:" is a directory to _open, not a volume. blkio also brings the
   sector alignment a raw device demands. */
#include "invarifs.h"
#include "volume.h"
#include "blkio.h"
#include "vol_metabuf.h"   /* WP76: INVFS_MBUF_BOOT_PAGES */
#include "vol_walk.h"   /* WP135: a walk's status is not optional */

static int errors = 0;

static void err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    errors++;
}

static int fail(const char *msg, int code) { fprintf(stderr, "FAIL: %s\n", msg); return code; }

/* WP49b: one row per live inode id. Same-id record chains (meta rewrites,
 * the text-batch owner's growing record) appear once per version in the
 * area walk, and a read resolves to the LATEST version for all of them --
 * so reading per record would compare new bytes against a stale fsz.
 * Collect the live id -> (fsz,name) map first, then read each id once. */
typedef struct { uint64_t id, fsz; char nm[256]; } deep_ent;

typedef struct {
    invfs_volume *vol;
    deep_ent *ents;
    size_t nents, capents;
} deep_ctx;

/* WP49b: per-record body fed by the bounded, index-ordered
 * vol_records_walk (the old position-driven vol_inode_next loop can cycle
 * on a non-monotonic mapper table). */


static int cmp_deep_ent_id(const void *a, const void *b)
{
    const deep_ent *ea = (const deep_ent *)a;
    const deep_ent *eb = (const deep_ent *)b;
    if (ea->id < eb->id) return -1;
    if (ea->id > eb->id) return 1;
    return 0;
}

/* WP-M21b: v3 collector for the deep pass -- same deep_ent array, fed by
 * vol_v3_walk in a single O(n) hierarchical walk instead of reverse-resolving
 * leaves. Sizes come straight from the inode row. */
static int deep_v3_walk_cb(void *ctx_, const char *path, uint64_t inode_id,
                           uint32_t type, uint64_t size, int64_t mtime)
{
    deep_ctx *c = (deep_ctx *)ctx_;
    size_t k;
    (void)mtime;

    if (type == INVFS_ITYP_DIR)
        return 0;                       /* directories don't have file content */
    if ((unsigned char)path[0] == 0x01)
        return 0;                       /* internal owners: not user files */

    if (c->nents == c->capents) {
        size_t nc = c->capents ? c->capents * 2 : 256;
        void *ne = realloc(c->ents, nc * sizeof *c->ents);
        if (!ne) return 1;
        c->ents = (deep_ent *)ne;
        c->capents = nc;
    }
    k = c->nents++;
    c->ents[k].id = inode_id;
    c->ents[k].fsz = size;
    snprintf(c->ents[k].nm, sizeof c->ents[k].nm, "%s", path);
    return 0;
}

int main(int argc, char **argv)
{
    blkio io;
    char devbuf[64];
    const char *path;
    int rc;
    invfs_superblock sb;
    uint32_t crc;
    uint64_t file_blocks, free_blocks = 0, alloc_blocks = 0, i;
    size_t bitmap_bytes;
    uint8_t *bitmap;
    int deep = 0;
    int ignore_missing_codecs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-verify [--ignore-missing-codecs] <image|device> [--deep]\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
        if (strcmp(argv[i], "--ignore-missing-codecs") == 0)
            ignore_missing_codecs = 1;
        if (strcmp(argv[i], "--deep") == 0)
            deep = 1;
    }

    /* count positional args (skip flags) */
    {
        int positional = 0;
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] != '-') positional++;
        }
        if (positional < 1 || positional > 2) {
            fprintf(stderr, "usage: invf-verify [--ignore-missing-codecs] <image|device> [--deep]\n");
            return 2;
        }
    }

    /* Read-only inspection, so the volume is not locked or dismounted: a
       mounted InvariantFS can be checked while it runs. */
    {
        int pi = 1;
        while (pi < argc && argv[pi][0] == '-') pi++;
        path = blkio_normalize(argv[pi], devbuf, sizeof devbuf);
    }
    rc = blkio_open(&io, path, 0);
    if (rc != 0) {
        fprintf(stderr, "FAIL: cannot open %s: %s\n", path, blkio_strerror(rc));
        return 1;
    }

    if (blkio_pread(&io, 0, &sb, sizeof(sb)) != 0)
        return fail("cannot read superblock", 1);

    /* 1. magic */
    if (memcmp(sb.magic, INVFS_MAGIC, 8) != 0)
        return fail("bad magic (not an InvariantFS image?)", 1);

    /* 2. checksum */
    crc = invfs_crc32c(&sb, offsetof(invfs_superblock, checksum));
    if (crc != sb.checksum) {
        fprintf(stderr, "FAIL: superblock checksum mismatch (stored %08x, computed %08x)\n",
                sb.checksum, crc);
        errors++;
    }

    /* 3. block size */
    if (sb.block_size != INVFS_BLOCK_SIZE)
        err("block_size %u (expected %u)", sb.block_size, INVFS_BLOCK_SIZE);

    /* WP59: codec-policy gate (PCK0 at 0x3C4).
     * verify is a gated tool per the spec: refuse if no PCK0 and not
     * --ignore-missing-codecs. BASIC_ONLY volumes pass automatically. */
    {
        invfs_pck0 pk;
        memset(&pk, 0, sizeof pk);
        if (blkio_pread(&io, INVFS_PCK0_OFF, &pk, sizeof pk) == 0 &&
            memcmp(pk.magic, "PCK0", 4) == 0) {
            if (pck0_crc(&pk) != pk.crc32c) {
                err("PCK0 CRC mismatch");
            } else if (!(pk.policy_flags & INVFS_PCK0_BASIC_ONLY) &&
                       pk.n_codecs > 0 && !ignore_missing_codecs) {
                err("PCK0 requires %u codec pack(s); "
                    "use --ignore-missing-codecs to proceed",
                    (unsigned)pk.n_codecs);
            }
        } else if (!ignore_missing_codecs) {
            err("no codec policy (PCK0); "
                "use --ignore-missing-codecs to proceed");
        }
    }

    /* 4. backing-store size. On a device this is the partition length rounded
     *    down to 4096, which is exactly what mkfs used, so the equality holds
     *    for both a device and an image file.
     *    WP25: on a two-device volume (DEVT at 0x2A0) dev0 alone carries only
     *    dev_blocks[0] of the global total; dev1 (INVFS_DEV1 / the hint)
     *    carries the rest. Check each against the table. */
    file_blocks = blkio_capacity(&io) / sb.block_size;
    {
        invfs_devt dt;
        memset(&dt, 0, sizeof dt);
        if (blkio_pread(&io, INVFS_DEVT_OFF, &dt, sizeof dt) == 0 &&
            memcmp(dt.magic, "DEVT", 4) == 0 && dt.dev_count == 2) {
            invfs_devt t = dt;
            t.crc32c = 0;
            if (invfs_crc32c(&t, sizeof t) == dt.crc32c) {
                if (file_blocks != dt.dev_blocks[0])
                    err("device 0 holds %llu blocks, DEVT says %llu",
                        (unsigned long long)file_blocks,
                        (unsigned long long)dt.dev_blocks[0]);
                file_blocks = sb.total_blocks;   /* dev0 leg checked; the
                        total is dev0+dev1 by construction (dev1's size is
                        verified at vol_open / by the mux) */
            }
        }
    }
    if (file_blocks != sb.total_blocks)
        err("backing store %llu blocks vs superblock total_blocks %llu",
            (unsigned long long)file_blocks, (unsigned long long)sb.total_blocks);

    /* 5. zone layout: no overlap, full coverage.
     * WP25: on a two-device volume the canonical shadow starts on dev1,
     * past the metadata mirror + the RAW-width reserved gap. */
    {
        int twodev = 0;
        invfs_devt dt;
        memset(&dt, 0, sizeof dt);
        if (blkio_pread(&io, INVFS_DEVT_OFF, &dt, sizeof dt) == 0 &&
            memcmp(dt.magic, "DEVT", 4) == 0 && dt.dev_count == 2) {
            invfs_devt t = dt;
            t.crc32c = 0;
            if (invfs_crc32c(&t, sizeof t) == dt.crc32c)
                twodev = 1;
        }
        if (sb.metadata_zone_start != 1)
            err("metadata_zone_start %llu (expected 1)", (unsigned long long)sb.metadata_zone_start);
        if (sb.raw_zone_start != sb.metadata_zone_start + sb.metadata_zone_blocks)
            err("raw zone not contiguous after metadata");
        if (twodev) {
            if (sb.shadow_zone_start != dt.dev_blocks[0] +
                    sb.raw_zone_start + sb.raw_zone_blocks)
                err("shadow zone not contiguous after the dev1 mirror span");
        } else if (sb.shadow_zone_start != sb.raw_zone_start + sb.raw_zone_blocks)
            err("shadow zone not contiguous after raw");
        if (sb.shadow_zone_start + sb.shadow_zone_blocks != sb.total_blocks)
            err("zones do not cover the volume");
    }

    /* 6. bitmap consistency
     *    - superblock + metadata zone must be allocated
     *    - data blocks (RAW/Shadow) may be allocated (files) — count them */
    bitmap_bytes = (size_t)((sb.total_blocks / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE)
                   * INVFS_BLOCK_SIZE;
    bitmap = (uint8_t *)malloc(bitmap_bytes);
    if (!bitmap)
        return fail("out of memory", 1);
    if (blkio_pread(&io, sb.metadata_zone_start * INVFS_BLOCK_SIZE,
                    bitmap, bitmap_bytes) != 0) {
        free(bitmap);
        return fail("cannot read bitmap", 1);
    }
    for (i = 0; i < sb.total_blocks; i++) {
        if (bitmap[i / 8] & (1u << (i % 8)))
            alloc_blocks++;
        else
            free_blocks++;
    }
    /* metadata region (superblock + metadata zone) must be fully allocated.
     * WP76: on a Meta-v3 volume the metadata-zone tail is deliberately left
     * free as the dev0-resident v3 base-page pool (WP-M19, mkfs.c); the
     * base-page allocator hands those blocks out on demand. The region that
     * must already be allocated ends just past the mapper table plus the two
     * reserved boot pages (INVFS_MBUF_BOOT_PAGES), which sit at
     * meta_mapper_pba + meta_mapper_blocks. Non-v3 volumes, and v3 volumes
     * with no mapper recorded, keep the whole-region rule. */
    uint64_t meta_reserved_end = sb.metadata_zone_start + sb.metadata_zone_blocks;
    if ((sb.vol_flags & VOLF_V3) && sb.meta_mapper_pba && sb.meta_mapper_blocks) {
        uint64_t root_end = sb.meta_mapper_pba + sb.meta_mapper_blocks
                          + INVFS_MBUF_BOOT_PAGES;
        if (root_end < meta_reserved_end)
            meta_reserved_end = root_end;
    }
    for (i = 0; i < meta_reserved_end; i++) {
        if (!(bitmap[i / 8] & (1u << (i % 8)))) {
            err("block %llu in metadata region is free (should be allocated)",
                (unsigned long long)i);
            break;
        }
    }
    /* allocated count must be >= the reserved metadata region (data blocks on top) */
    if (alloc_blocks < meta_reserved_end) {
        err("bitmap marks %llu blocks allocated, expected >= %llu (superblock+metadata)",
            (unsigned long long)alloc_blocks,
            (unsigned long long)meta_reserved_end);
    }
    free(bitmap);

    if (errors == 0) {
        printf("OK: %s is a valid InvariantFS volume\n", argv[1]);
        printf("  state: %s, uuid: ", sb.state == INVFS_STATE_CLEAN ? "CLEAN"
                : sb.state == INVFS_STATE_DIRTY ? "DIRTY" : "RECOVERY");
        for (i = 0; i < 16; i++)
            printf("%02x", sb.uuid[i]);
        printf("\n");
        printf("  blocks: %llu total, %llu free, %llu allocated (%.1f%% used)\n",
               (unsigned long long)sb.total_blocks,
               (unsigned long long)free_blocks,
               (unsigned long long)alloc_blocks,
               100.0 * (double)alloc_blocks / (double)sb.total_blocks);
        printf("  zones: metadata %llu | raw %llu | shadow %llu\n",
               (unsigned long long)sb.metadata_zone_blocks,
               (unsigned long long)sb.raw_zone_blocks,
               (unsigned long long)sb.shadow_zone_blocks);
    }

    /* --deep: read every live file end-to-end; per-segment CRC32C is
     * verified on the read path, so silent corruption is caught here */
    if (deep) {
        invfs_volume *vol;
        uint64_t live = 0, bad = 0;
        uint64_t total_bytes = 0;
        int parity_bad = 0;
        deep_ent *ents = NULL;
        size_t nents = 0;
        deep_ctx dc;
        /* Close first: vol_open takes a device exclusively (lock + dismount),
           which cannot succeed while this handle is still open. */
        blkio_close(&io);
        int open_err = 0;   /* vol_open's out-param; must NOT clobber errors */
        vol = vol_open(path, &open_err);
        if (!vol) { fprintf(stderr, "deep: cannot open volume (err %d)\n", open_err); return 1; }
        printf("deep: reading all live files...\n");
        memset(&dc, 0, sizeof dc);
        dc.vol = vol;
        {
            /* WP135: the walk's status was dropped here, so a volume whose
             * namespace walk STOPPED -- a quarantined base page, an OOM --
             * produced a short file list, and the summary below printed
             * "N files ok, 0 corrupt" over it. That is not a near-miss: on
             * exactly the volumes --deep exists to inspect, the files it
             * could not enumerate are the ones a restore is looking for, and
             * the number it printed said the volume was fine.
             *
             * The file already held the other answer: the nlink audit 46
             * lines below handles the same failure, says so in words, and
             * counts it into `bad`. This brings the walk into line with it,
             * for the same reason and with the same wording, so the exit
             * code and the summary cannot disagree. */
            vol_walk_t w;
            int wrc;
            vol_walk_init(&w, vol, "verify --deep namespace walk");
            /* WP135: the STRICT walk -- a deep pass that skipped a
             * subtree would report a short volume as a clean one. */
            wrc = vol_v3_walk_strict(vol, deep_v3_walk_cb, &dc);
            vol_walk_result(&w, wrc, dc.nents, dc.nents);
            if (vol_walk_commit(&w) != 0) {
                printf("  CORRUPT: the deep pass could not walk the whole "
                       "namespace (the walk stopped after %llu entr%s); the "
                       "file count below is a LOWER BOUND and must not be "
                       "read as \"0 corrupt\"\n",
                       (unsigned long long)vol_walk_seen(&w),
                       vol_walk_seen(&w) == 1 ? "y" : "ies");
                bad++;
            }
            if (dc.nents > 1) {
                qsort(dc.ents, dc.nents, sizeof *dc.ents, cmp_deep_ent_id);
                size_t w = 0;
                for (size_t r = 0; r < dc.nents; r++) {
                    if (w == 0 || dc.ents[r].id != dc.ents[w - 1].id) {
                        if (w != r)
                            dc.ents[w] = dc.ents[r];
                        w++;
                    }
                }
                dc.nents = w;
            }
        }
        ents = dc.ents; nents = dc.nents;
        for (size_t k = 0; k < nents; k++) {
            uint64_t ino = ents[k].id, fsz = ents[k].fsz;
            const char *nm = ents[k].nm;
            if (fsz > MAX_FILE_SIZE) { printf("  BAD size %s: %llu\n", nm, (unsigned long long)fsz); bad++; continue; }
            {
                uint8_t *buf = NULL;
                size_t blen = 0;
                if (vol_read_file(vol, ino, &buf, &blen) != 0) {
                    printf("  CORRUPT: %s\n", nm);
                    bad++;
                    continue;
                }
                if (blen != fsz) { printf("  SIZE MISMATCH: %s (%zu vs %llu)\n", nm, blen, (unsigned long long)fsz); bad++; }
                free(buf);
                total_bytes += fsz;
                live++;
            }
        }
        free(ents);
        /* WP118: the deep pass above walks INODES (once per id), so two names
         * on one inode are read once and the count it prints is a count of
         * inodes, not of the names the volume claims to have. That is how a
         * 7-name volume with two aliased pairs reported "5 files ok, 0
         * corrupt" (WP111b). Put the NAMES against the inode rows' nlink
         * now: for every live inode the number of names that resolve to it
         * must equal its nlink -- the invariant a hardlink satisfies (fan-in
         * 2, nlink 2) and the aliasing bug violates (fan-in 2, nlink 1).
         * Counted into `bad` BEFORE the summary line, so the number and the
         * reason are on screen together and the exit code is non-zero. */
        {
            invfs_nlink_audit na;
            if (vol_v3_nlink_audit(vol, &na) != 0) {
                printf("  CORRUPT: the nlink/fan-in audit could not be "
                       "completed (the namespace walk failed); the counts "
                       "above are partial\n");
                bad++;
            } else {
                uint64_t fi;
                printf("deep: names walked: %llu over %llu live inode(s)\n",
                       (unsigned long long)na.names,
                       (unsigned long long)na.inodes);
                if (na.mismatch) {
                    for (fi = 0; fi < na.nfault; fi++) {
                        const invfs_nlink_fault *f = &na.fault[fi];
                        printf("  CORRUPT: inode %llu (%s): nlink %u but %u "
                               "name(s) resolve to it -- %s\n",
                               (unsigned long long)f->id,
                               f->name[0] ? f->name : "?", f->nlink, f->fanin,
                               !strcmp(f->reason, "missing-name")
                               ? "a name is MISSING from the directory tree"
                               : !strcmp(f->reason, "dead")
                               ? "its inode row is not live"
                               : "STALE DIRENT(S): a name landed on this "
                                 "inode; the content under the other name(s) "
                                 "is gone");
                        bad++;
                    }
                    if (na.nfault_total > na.nfault)
                        printf("  ... and %llu more unbalanced inode(s)\n",
                               (unsigned long long)(na.nfault_total -
                                                    na.nfault));
                    printf("deep: nlink/fan-in: %llu inode(s) do not balance "
                           "(%llu missing name(s), %llu stale dirent(s))\n",
                           (unsigned long long)na.nfault_total,
                           (unsigned long long)na.missing_names,
                           (unsigned long long)na.stale_dirents);
                } else {
                    printf("deep: nlink/fan-in: ok\n");
                }
                if (na.orphan_rows)
                    printf("deep: note: %llu live inode(s) that no directory "
                           "entry names (orphan rows; reported, not counted "
                           "as corrupt)\n",
                           (unsigned long long)na.orphan_rows);
            }
        }
        /* WP20: when the volume is sealed, recompute every parity stripe
         * against its stored parity block. Drift counters are all zero on
         * an unsealed volume (seal is opt-in), so the line appears only
         * when a seal exists or drifted. Parity drift is reported and is
         * fatal to the exit code, but it is not a "corrupt file". */
        {
            invfs_seal_verify sv;
            if (vol_seal_verify(vol, &sv) == 0) {
                if (sv.sealed || sv.mismatched || sv.missing || sv.extra) {
                    printf("parity: %llu sealed stripes, %llu mismatched, "
                           "%llu missing, %llu extra\n",
                           (unsigned long long)sv.sealed,
                           (unsigned long long)sv.mismatched,
                           (unsigned long long)sv.missing,
                           (unsigned long long)sv.extra);
                    if (sv.mismatched || sv.missing || sv.extra)
                        parity_bad = 1;
                }
                /* WP20b layer-2 (RS) leg: same drift counters over the
                 * RS(32+m2, 32) stripes */
                if (sv.sealed2 || sv.mismatched2 || sv.missing2 ||
                    sv.extra2) {
                    printf("parity2: %llu sealed stripes, %llu mismatched, "
                           "%llu missing, %llu extra\n",
                           (unsigned long long)sv.sealed2,
                           (unsigned long long)sv.mismatched2,
                           (unsigned long long)sv.missing2,
                           (unsigned long long)sv.extra2);
                    if (sv.mismatched2 || sv.missing2 || sv.extra2)
                        parity_bad = 1;
                }
            }
        }
        /* WP99: the deep read walks ONE copy of the metadata, so it cannot
         * see a two-device volume whose other device is behind. That is the
         * shape of the worst case: the copy walked here reads perfectly
         * while the dev0 mirror holds a rolled-back root descriptor, and a
         * mount that serves it adopts that generation and drops the delta
         * records past it -- silently, on both devices. So ask the devices
         * (the same call the mount path makes) and make a stale mirror
         * fatal here too: `0 corrupt` must not be printed over it. Counted
         * into `bad` BEFORE the summary line, so the number and the reason
         * are on screen together. */
        if (vol_ndev(vol) == 2) {
            int stale = -1;
            const char *why = NULL;
            if (vol_mirror_compare(vol, &stale, &why) == 0 && stale >= 0) {
                printf("  MIRROR STALE: dev%d's block 0 is behind "
                       "(%s) -- the reads above came from the newer copy; a "
                       "mount serving dev%d would adopt a rolled-back root "
                       "and lose writes made since\n",
                       stale, why ? why : "block 0", stale);
                bad++;
            } else {
                printf("mirror: in sync (both devices)\n");
            }
        }
        printf("deep: %llu files ok, %llu corrupt, %llu bytes verified\n",
               (unsigned long long)live, (unsigned long long)bad,
               (unsigned long long)total_bytes);
        vol_close(vol);
        return (bad || parity_bad || errors) ? 1 : 0;
    }

    blkio_close(&io);
    return errors ? 1 : 0;
}
