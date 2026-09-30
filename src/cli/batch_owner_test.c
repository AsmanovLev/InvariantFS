/* batch_owner_test.c — the v3 batch REGISTRY is a block owner.
 *
 * THE BUG THIS EXISTS FOR. A v3 text/binary batch segment is owned by TWO
 * things at once: by the recipe of every member that points into it (its
 * zone==TEXT entries), and by the batch registry -- the hidden "\x01tzb"
 * file whose row is what tz_v3_gc frees the block through. The two owners do
 * not die at the same time. A batch stops being named by any live recipe the
 * moment its last member is rewritten or deleted, but its registry row
 * survives until the sweep's stage-6 GC runs.
 *
 * The savepoint reclaim (spn_reclaim, src/core/vol_spt0.c) only ever asked
 * the FIRST question -- "does a live recipe name this block?" -- so during
 * that window it freed a block the registry still owned. The free pool is
 * shared with the metadata zone (one pool, no hard regions, AGENTS.md
 * 2.3), so the very next base-page allocation could take it,
 * mbuf_root_publish could make it the live base root, and the same sweep's
 * stage-6 tz_v3_gc would then free that LIVE page through the row it had
 * never dropped. The fold that follows rebuilds the base from the
 * pre-transform root, and a file written seconds earlier is unreadable:
 * "recipe blob missing/corrupt". End-to-end repro:
 *
 *   python3 tools/fuzz/opseq.py --seed 0x5e9 --only-image 1 --ops 81
 *
 * The window is INTRA-SWEEP (stage 1 frees, stage 6 frees again), so no
 * purely external test can see it: by the time the sweep returns, both the
 * broken and the fixed tree have the block free and the row gone. This driver
 * therefore calls the same two production entry points the sweep's prepare
 * calls, in the same order (tools/invf-sweep.c: spt0_drop, then
 * spt0_capture), and inspects the allocation bitmap across the capture.
 *
 * THE REGISTRY IS PARSED HERE, NOT TAKEN FROM THE ENGINE. The on-disk row
 * format is [4B "TZV3"][4B n][n * {u32 seq, u32 algo, u64 pba, u32 phys}]
 * (TZ_V3_REG_MAGIC and tz_v3_reg_ent, src/core/vol_textzone.c). Reading it
 * with vol_find + vol_read_file -- both long-standing public entry points --
 * keeps this test from depending on any accessor the fix introduces. A
 * regression test that only builds when the fix is present is not a red
 * control, it is a tautology. The 24-byte stride and the field offsets below
 * are that struct's own layout on LP64; a parse that does not add up (a pba
 * off the end of the volume, a zero-length extent) is reported as MALFORMED
 * rather than quietly testing nothing.
 *
 * Subcommands (the image is a v3 volume built by tools/test-v3-batch-owner.sh):
 *
 *   rows <img>
 *       Print the registry's block extents and whether each is currently
 *       ALLOCATED. Prints REGISTRY_ROWS=<n> BLOCKS=<n> FREE=<n>.
 *
 *   capture <img>
 *       THE TEST. Read the registry's extents, run the sweep's prepare
 *       verbatim (spt0_drop + spt0_capture, which is where the reclaim
 *       lives), then re-read the allocation bitmap and assert that no block
 *       the registry named has been freed. Prints
 *       REGISTRY_ROWS=<n> BLOCKS=<n> RECLAIMED_DEAD=<n>. Exits 1 if
 *       RECLAIMED_DEAD > 0, naming the offending blocks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "invarifs.h"
#include "volume_internal.h"
#include "vol_metabuf.h"
#include "vol_spt0.h"

#define BATCH_OWNER_REG_MAGIC 0x33565a54u   /* "TZV3" */
/* tz_v3_reg_ent, on LP64: seq u32 @0, algo u32 @4, pba u64 @8,
 * phys u32 @16, sizeof 24. */
#define BATCH_OWNER_ROW_SZ    24u
#define BATCH_OWNER_OFF_PBA    8u
#define BATCH_OWNER_OFF_PHYS  16u

typedef struct {
    uint64_t pba;
    uint64_t phys;
} batch_extent;

typedef struct {
    batch_extent *ext;
    size_t n;
} batch_registry;

/* The registry as it is ON DISK, parsed here rather than asked of the
 * engine. Returns 0 on success (an absent registry is n == 0, not an
 * error), -1 on a malformed blob. */
static int registry_read(invfs_volume *v, batch_registry *out)
{
    uint8_t *buf = NULL;
    size_t len = 0, i;
    uint64_t owner;

    memset(out, 0, sizeof *out);
    owner = vol_find(v, TZ_OWNER_NAME);
    if (!owner)
        return 0;
    if (vol_read_file(v, owner, &buf, &len) != 0 || !buf) {
        free(buf);
        return 0;
    }
    if (len < 8) {
        free(buf);
        return 0;
    }
    {
        uint32_t magic = 0, n = 0;
        memcpy(&magic, buf, 4);
        memcpy(&n, buf + 4, 4);
        if (magic != BATCH_OWNER_REG_MAGIC) {
            free(buf);
            return 0;
        }
        if ((size_t)n > (len - 8) / BATCH_OWNER_ROW_SZ)
            n = (uint32_t)((len - 8) / BATCH_OWNER_ROW_SZ);
        if (n) {
            out->ext = (batch_extent *)calloc(n, sizeof *out->ext);
            if (!out->ext) {
                free(buf);
                return -1;
            }
        }
        for (i = 0; i < n; i++) {
            const uint8_t *r = buf + 8 + i * BATCH_OWNER_ROW_SZ;
            uint32_t phys = 0;
            memcpy(&out->ext[i].pba, r + BATCH_OWNER_OFF_PBA, 8);
            memcpy(&phys, r + BATCH_OWNER_OFF_PHYS, 4);
            if (out->ext[i].pba >= v->sb.total_blocks ||
                out->ext[i].pba + phys > v->sb.total_blocks) {
                fprintf(stderr, "row %zu: pba %llu + phys %u is outside the "
                        "%llu-block volume -- the registry blob does not "
                        "parse as the format this driver knows\n", i,
                        (unsigned long long)out->ext[i].pba, phys,
                        (unsigned long long)v->sb.total_blocks);
                free(out->ext);
                memset(out, 0, sizeof *out);
                free(buf);
                return -1;
            }
            /* phys == 0 would be a row naming no block. Claim the head
             * anyway: over-claiming one block is a bounded leak,
             * under-claiming a row that does own blocks is the data loss
             * this test exists to catch. */
            out->ext[i].phys = phys ? phys : 1;
        }
        out->n = n;
    }
    free(buf);
    return 0;
}

static void registry_free(batch_registry *r)
{
    free(r->ext);
    r->ext = NULL;
    r->n = 0;
}

static size_t registry_blocks(const batch_registry *r)
{
    size_t i, b = 0;
    for (i = 0; i < r->n; i++)
        b += (size_t)r->ext[i].phys;
    return b;
}

/* Snapshot of every block the registry owns, read straight off the volume.
 *
 * The allocation BIT alone is not a sufficient witness, and finding out why
 * is the whole reason this driver reads bytes. In the broken tree the reclaim
 * frees the batch block and the very next allocation inside the SAME capture
 * takes it again -- observed: the batch at 23308 came back as the new
 * savepoint mark set -- so a bitmap comparison reports "still allocated" for a
 * block that was freed and handed to somebody else. Comparing the 4 KiB the
 * registry's block holds catches that, and catches a plain overwrite too. */
typedef struct {
    uint64_t *pba;
    uint8_t  *bytes;      /* n * INVFS_BLOCK_SIZE */
    size_t    n;
} block_snapshot;

static void snapshot_free(block_snapshot *s)
{
    free(s->pba);
    free(s->bytes);
    memset(s, 0, sizeof *s);
}

static int snapshot_take(invfs_volume *v, const batch_registry *r,
                         block_snapshot *out)
{
    size_t i, k, c = 0;

    memset(out, 0, sizeof *out);
    out->n = registry_blocks(r);
    if (!out->n)
        return 0;
    out->pba = (uint64_t *)calloc(out->n, sizeof *out->pba);
    out->bytes = (uint8_t *)calloc(out->n, INVFS_BLOCK_SIZE);
    if (!out->pba || !out->bytes) {
        snapshot_free(out);
        return -1;
    }
    for (i = 0; i < r->n; i++)
        for (k = 0; k < r->ext[i].phys; k++) {
            uint64_t b = r->ext[i].pba + k;
            if (b >= v->sb.total_blocks)
                continue;
            out->pba[c] = b;
            if (io_pread(&v->io, b * (uint64_t)INVFS_BLOCK_SIZE,
                         out->bytes + c * INVFS_BLOCK_SIZE,
                         INVFS_BLOCK_SIZE) != 0)
                memset(out->bytes + c * INVFS_BLOCK_SIZE, 0,
                       INVFS_BLOCK_SIZE);
            c++;
        }
    out->n = c;
    return 0;
}

/* A block the registry owns has been let go if it is now free, or if what it
 * holds is no longer what it held: either way somebody else owns it. */
static int report_surrendered(invfs_volume *v, const block_snapshot *before,
                              const batch_registry *r, const char *tag)
{
    static uint8_t now[INVFS_BLOCK_SIZE];
    size_t c, i, k, row = 0;
    int bad = 0;

    for (c = 0; c < before->n; c++) {
        uint64_t b = before->pba[c];
        while (row < r->n && b >= r->ext[row].pba + r->ext[row].phys)
            row++;
        if (!mbuf_page_allocated(v, b)) {
            bad++;
            fprintf(stderr, "%s: registry row %zu still owns block %llu, "
                    "which is FREE\n", tag, row, (unsigned long long)b);
            continue;
        }
        if (io_pread(&v->io, b * (uint64_t)INVFS_BLOCK_SIZE, now,
                     INVFS_BLOCK_SIZE) != 0) {
            bad++;
            fprintf(stderr, "%s: registry row %zu owns block %llu, which is "
                    "unreadable\n", tag, row, (unsigned long long)b);
            continue;
        }
        if (memcmp(now, before->bytes + c * INVFS_BLOCK_SIZE,
                   INVFS_BLOCK_SIZE) != 0) {
            bad++;
            fprintf(stderr, "%s: registry row %zu owns block %llu, and its "
                    "contents CHANGED -- it was freed and handed to another "
                    "owner\n", tag, row, (unsigned long long)b);
        }
    }
    (void)i; (void)k;
    return bad;
}

static int cmd_rows(const char *img)
{
    invfs_volume *v;
    batch_registry reg;
    block_snapshot snap;
    size_t i, blocks;
    int err, free_blocks;

    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "FAIL: vol_open(%s) err=%d\n", img, err);
        return 1;
    }
    if (registry_read(v, &reg) != 0) {
        fprintf(stderr, "FAIL: the batch registry blob is malformed\n");
        vol_close(v);
        return 1;
    }
    for (i = 0; i < reg.n; i++)
        printf("ROW pba=%llu phys=%llu\n",
               (unsigned long long)reg.ext[i].pba,
               (unsigned long long)reg.ext[i].phys);
    blocks = registry_blocks(&reg);
    if (snapshot_take(v, &reg, &snap) != 0) {
        fprintf(stderr, "FAIL: out of memory taking the block snapshot\n");
        registry_free(&reg);
        vol_close(v);
        return 1;
    }
    free_blocks = report_surrendered(v, &snap, &reg, "rows");
    printf("REGISTRY_ROWS=%zu BLOCKS=%zu FREE=%d\n", reg.n, blocks,
           free_blocks);
    snapshot_free(&snap);
    registry_free(&reg);
    vol_close(v);
    return 0;
}

static int cmd_capture(const char *img)
{
    invfs_volume *v;
    batch_registry reg;
    block_snapshot before;
    size_t blocks;
    int err, freed, drc, crc;

    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "FAIL: vol_open(%s) err=%d\n", img, err);
        return 1;
    }
    if (!(vol_sb(v)->vol_flags & VOLF_V3)) {
        fprintf(stderr, "FAIL: not a v3 volume\n");
        vol_close(v);
        return 1;
    }
    /* The owner set as it stands BEFORE the reclaim. */
    if (registry_read(v, &reg) != 0) {
        fprintf(stderr, "FAIL: the batch registry blob is malformed\n");
        vol_close(v);
        return 1;
    }
    if (reg.n == 0) {
        fprintf(stderr, "FAIL: the registry is empty; there is no batch to "
                "reclaim and this test would pass vacuously\n");
        vol_close(v);
        return 1;
    }
    blocks = registry_blocks(&reg);
    if (snapshot_take(v, &reg, &before) != 0) {
        fprintf(stderr, "FAIL: out of memory taking the block snapshot\n");
        registry_free(&reg);
        vol_close(v);
        return 1;
    }
    freed = report_surrendered(v, &before, &reg, "before");
    if (freed) {
        fprintf(stderr, "FAIL: %d registry block(s) were ALREADY surrendered "
                "before the capture; the state under test is not the one "
                "tools/test-v3-batch-owner.sh builds\n", freed);
        snapshot_free(&before);
        registry_free(&reg);
        vol_close(v);
        return 1;
    }

    /* The sweep's prepare, verbatim: drop the previous window, then capture
     * (and with it, reclaim) the new one. */
    drc = spt0_drop(v);
    crc = spt0_capture(v);
    printf("SPT0_DROP=%d SPT0_CAPTURE=%d\n", drc, crc);
    if (crc < 0) {
        fprintf(stderr, "FAIL: spt0_capture refused the volume\n");
        snapshot_free(&before);
        registry_free(&reg);
        vol_close(v);
        return 1;
    }

    freed = report_surrendered(v, &before, &reg, "after");
    printf("REGISTRY_ROWS=%zu BLOCKS=%zu RECLAIMED_DEAD=%d\n",
           reg.n, blocks, freed);
    snapshot_free(&before);
    registry_free(&reg);
    if (vol_flush(v) != 0)
        fprintf(stderr, "warning: vol_flush failed\n");
    vol_close(v);
    if (freed) {
        fprintf(stderr, "FAIL: the savepoint reclaim freed %d block(s) the "
                "batch registry still owns. That row is now a stale pointer "
                "into the free pool: the next base-page allocation may take "
                "one of them, and this same sweep's stage-6 tz_v3_gc will "
                "then free that LIVE page through it.\n", freed);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "rows"))
        return cmd_rows(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "capture"))
        return cmd_capture(argv[2]);
    fprintf(stderr,
            "usage: %s rows|capture <img>\n"
            "  rows    -- list the v3 batch registry's block extents\n"
            "  capture -- run the sweep's prepare and assert the reclaim\n"
            "             freed no block the registry still owns\n",
            argv[0]);
    return 2;
}
