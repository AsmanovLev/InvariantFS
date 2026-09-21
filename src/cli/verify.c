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
static int deep_cb(void *ctx_, uint64_t rec_pos,
                   const invfs_inode_rec *h, const uint8_t *rec)
{
    deep_ctx *c = (deep_ctx *)ctx_;
    uint64_t ino = h->inode_id, fsz = h->file_size;
    char nm[256];
    size_t k, nl;
    (void)rec_pos; (void)rec;

    if (h->magic != INODE_REC_MAGIC) return 0;
    /* h->name is not NUL-terminated */
    nl = h->name_len < 255 ? h->name_len : 255;
    memcpy(nm, h->name, nl);
    nm[nl] = 0;
    if ((uint8_t)nm[0] == 0x01) return 0;  /* internal owners
            ("\x01tzb", WP20 "\x01parityN"): not user files; the
            parity leg below checks the seal owners' real payload */
    if (vol_find(c->vol, nm) != ino) return 0;  /* superseded */
    for (k = 0; k < c->nents; k++)
        if (c->ents[k].id == ino) break;
    if (k == c->nents) {
        if (c->nents == c->capents) {
            size_t nc = c->capents ? c->capents * 2 : 256;
            void *ne = realloc(c->ents, nc * sizeof *c->ents);
            if (!ne) return 1;
            c->ents = (deep_ent *)ne;
            c->capents = nc;
        }
        k = c->nents++;
    }
    c->ents[k].id = ino;
    c->ents[k].fsz = fsz;
    memcpy(c->ents[k].nm, nm, sizeof c->ents[k].nm);
    return 0;
}

/* WP-M21b: v3 collector for the deep pass -- same deep_ent array, fed by
 * the M18 live-set iterator instead of the (absent) v2 record stream.
 * The iterator already yields live rows only, so the superseded check
 * deep_cb needs is moot; sizes come straight from the inode row. */
static int deep_v3_cb(invfs_volume *v, uint64_t inode_id, const char *name,
                      void *ctx_)
{
    deep_ctx *c = (deep_ctx *)ctx_;
    invfs_v3_inode in;
    char full[1024];
    size_t k;
    (void)name;

    if (vol_v3_inode_get(v, inode_id, &in) != 1)
        return 0;
    if (vol_v3_path_of(v, inode_id, full, sizeof full) != 1)
        return 0;                       /* unlinked mid-pass: skip */
    if ((unsigned char)full[0] == 0x01)
        return 0;                       /* internal owners: not user files */
    for (k = 0; k < c->nents; k++)
        if (c->ents[k].id == inode_id) break;
    if (k == c->nents) {
        if (c->nents == c->capents) {
            size_t nc = c->capents ? c->capents * 2 : 256;
            void *ne = realloc(c->ents, nc * sizeof *c->ents);
            if (!ne) return 1;
            c->ents = (deep_ent *)ne;
            c->capents = nc;
        }
        k = c->nents++;
    }
    c->ents[k].id = inode_id;
    c->ents[k].fsz = in.size;
    snprintf(c->ents[k].nm, sizeof c->ents[k].nm, "%s", full);
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
    /* metadata region (superblock + metadata zone) must be fully allocated */
    for (i = 0; i < sb.metadata_zone_start + sb.metadata_zone_blocks; i++) {
        if (!(bitmap[i / 8] & (1u << (i % 8)))) {
            err("block %llu in metadata region is free (should be allocated)",
                (unsigned long long)i);
            break;
        }
    }
    /* allocated count must be >= metadata region (data blocks on top) */
    if (alloc_blocks < sb.metadata_zone_start + sb.metadata_zone_blocks) {
        err("bitmap marks %llu blocks allocated, expected >= %llu (superblock+metadata)",
            (unsigned long long)alloc_blocks,
            (unsigned long long)(sb.metadata_zone_start + sb.metadata_zone_blocks));
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
        /* WP-M21b: v3 live set comes from the M18 iterator (base tree +
         * delta overlay); the record walk below finds nothing there. */
        if (vol_sb(vol)->vol_flags & VOLF_V3)
            (void)vol_v3_iter_live_inodes(vol, deep_v3_cb, &dc);
        else
            vol_records_walk(vol, deep_cb, &dc);
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
        printf("deep: %llu files ok, %llu corrupt, %llu bytes verified\n",
               (unsigned long long)live, (unsigned long long)bad,
               (unsigned long long)total_bytes);
        vol_close(vol);
        return (bad || parity_bad || errors) ? 1 : 0;
    }

    blkio_close(&io);
    return errors ? 1 : 0;
}
