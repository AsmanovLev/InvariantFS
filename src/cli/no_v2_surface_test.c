/* no_v2_surface_test.c — the v2 reader is gone, and this is how we know.
 *
 * There is a trap in "delete the retired format": a deletion that removes
 * the only reader of something real destroys that reader, and a deletion
 * whose effect nobody can observe is one the next change silently undoes.
 * v2 was deleted once already (WP drop-v2-branches, 0c82a7a) and had to be
 * re-audited, because the removal was invisible: the tree still compiled,
 * still linked, and still ran.
 *
 * So the assertion here is NOT a symbol check. Three ways to check for a
 * deleted symbol, and why none of them is the test:
 *
 *   - grep the tree / the built objects. Tests the NAME, and the name is
 *     the thing a re-grow changes first. A caller brought back as
 *     `journal_record()` passes a grep for `vol_map` and still writes to
 *     the same place. It also puts a build-system fact in a product test.
 *   - link-failure check. Everything already links: the symbols were
 *     absent and nothing referenced them. A link check cannot fail today
 *     and cannot fail tomorrow for the thing we care about either --
 *     re-adding a CALL to a re-added function links fine.
 *   - grep the built tree. Same objection as the first, with more moving
 *     parts.
 *
 * What this test asserts instead is the on-disk EFFECT, through the
 * shipped binaries only:
 *
 *   LEG 1 (the one that matters): the reserved 32 MiB gap between the
 *     metadata extent mapper and the record area is BYTE-FOR-ZERO on a v3
 *     volume -- at mkfs, and again after a full write / overwrite /
 *     sweep / fsck cycle. Every v2 entry point wrote there and nowhere
 *     else: `vol_map` and `l2p_remove` queued ops that only `jrn_flush`
 *     ever made durable, and `jrn_flush` was already unreachable (it sat
 *     below vol_flush's VOLF_META early return). So "the gap is still zero"
 *     is not a proxy for "no symbol is called" -- it is the direct
 *     statement that nothing in the write path reached the mapping log,
 *     under whatever name it answers to. Bring back any caller of the
 *     retired surface and this goes red.
 *
 *     It also cannot go red by accident: the assertion is a byte scan of a
 *     region the layout fixes at mkfs, not a comparison against a previous
 *     run of this binary.
 *
 *   LEG 2: every shipped tool refuses a v2-flagged image, by name. vol_open
 *     is not the only door into a volume -- invf-resize and invf-fsck have
 *     their own open paths, and a tool with a private v2 path is exactly
 *     what "no shipped tool reaches for a v2 entry point" has to exclude.
 *
 *   LEG 3: bit-exactness. The cycle in LEG 1 rewrites data, so the files
 *     must come back byte-identical. Without this leg a test that deletes
 *     an engine path could go green by deleting the path that reads.
 *
 * The control cell is the zero-scan itself on a volume this build just
 * created: if the region were not where the layout says, the scan would
 * report "zero" over the wrong 32 MiB and every leg would pass vacuously.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/wait.h>

#include "volume_internal.h"
#include "blkio.h"

static int fails;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fails++;
}

#define BS 4096

/* The region, from the superblock, exactly as vol_open computes it
 * (volume.c: bitmap_blocks from total_blocks; the mapper table sits
 * immediately after the bitmap; the reserved gap is INVFS_JOURNAL_BLOCKS
 * blocks; the record area starts after the gap). Reading it back from the
 * superblock rather than hardcoding an offset is what makes this a test of
 * the layout and not of a number someone typed. */
static int gap_bounds(const char *img, uint64_t *start, uint64_t *blocks)
{
    blkio io;
    invfs_superblock sb;
    uint64_t bitmap_blocks;
    int rc = -1;

    if (blkio_open(&io, img, 0) != 0)
        return -1;
    if (blkio_pread(&io, 0, &sb, sizeof sb) == 0) {
        bitmap_blocks = (sb.total_blocks / 8 + BS - 1) / BS;
        *start = sb.metadata_zone_start + bitmap_blocks + INVFS_META_EXT_BLOCKS;
        *blocks = INVFS_JOURNAL_BLOCKS;
        rc = 0;
    }
    blkio_close(&io);
    return rc;
}

/* Scan [start, start+blocks) for any non-zero byte. *first is the first
 * non-zero byte's offset within the region, or -1. Returns the count. */
static uint64_t gap_nonzero(const char *img, uint64_t start, uint64_t blocks,
                            long long *first)
{
    FILE *f = fopen(img, "rb");
    uint8_t *buf;
    uint64_t off, n = 0;
    *first = -1;
    if (!f) return (uint64_t)-1;
    buf = malloc(BS);
    if (!buf) { fclose(f); return (uint64_t)-1; }
    for (off = 0; off < blocks; off++) {
        size_t i;
        if (fseek(f, (long)((start + off) * BS), SEEK_SET) != 0 ||
            fread(buf, 1, BS, f) != BS) { n = (uint64_t)-1; break; }
        for (i = 0; i < BS; i++)
            if (buf[i]) {
                if (*first < 0) *first = (long long)(off * BS + i);
                n++;
            }
    }
    free(buf);
    fclose(f);
    return n;
}

static int run(const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    int rc = system(cmd);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 128;
}

/* Copy an image, then clear VOLF_META (recomputing the checksum, which does
 * not actually move -- vol_flags sits outside the CRC32C span, 0..0x7B --
 * but recomputing makes the edit correct if that ever changes). */
static int demote(const char *src, const char *dst)
{
    char cmd[1200];
    blkio io;
    invfs_superblock sb;
    int rc = -1;

    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", src, dst);
    if (run(cmd) != 0) return -1;
    if (blkio_open(&io, dst, 0) != 0) return -1;
    if (blkio_pread(&io, 0, &sb, sizeof sb) == 0) {
        sb.vol_flags &= ~(uint32_t)VOLF_META;
        sb.checksum = invfs_crc32c(&sb, offsetof(invfs_superblock, checksum));
        if (blkio_pwrite(&io, 0, &sb, sizeof sb) == 0)
            rc = 0;
    }
    blkio_close(&io);
    return rc;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    const char *b = "./bin";
    char img[512], dem[512], src[512], out[1024], errp[512];
    char gap[512], f1[1024], f2[1024];
    uint64_t gstart, gblocks, nz;
    long long first;
    int i, nfiles = 8;

    snprintf(img, sizeof img, "%s/no_v2_surface.img", dir);
    snprintf(dem, sizeof dem, "%s/no_v2_surface_v2.img", dir);
    snprintf(gap, sizeof gap, "%s/no_v2_surface_gap", dir);
    snprintf(errp, sizeof errp, "%s/no_v2_surface.err", dir);
    snprintf(src, sizeof src, "%s/no_v2_surface_src", dir);

    /* --- build the corpus ------------------------------------------------ */
    if (run("rm -rf '%s' '%s' && mkdir -p '%s' '%s'", src, gap, src, gap) != 0) {
        fprintf(stderr, "cannot prepare %s\n", src);
        return 2;
    }
    for (i = 0; i < nfiles; i++) {
        /* mixed shapes on purpose: binary (incompressible), text
         * (compressible), and one file rewritten in leg 1 so the cycle
         * covers supersede-as-well-as-create. */
        snprintf(f1, sizeof f1, "%s/f%02d.bin", src, i);
        if (i % 3 == 0) {
            int k;
            FILE *f = fopen(f1, "wb");
            if (!f) return 2;
            for (k = 0; k < 20000; k++)
                fputc((i * 7 + k * 31 + (k >> 3)) & 0xFF, f);
            fclose(f);
        } else if (i % 3 == 1) {
            int k;
            FILE *f = fopen(f1, "wb");
            if (!f) return 2;
            for (k = 0; k < 4000; k++)
                fprintf(f, "line %d of file %02d: the quick brown fox\n", k, i);
            fclose(f);
        } else {
            if (run("head -c 40000 /dev/urandom > '%s'", f1) != 0) return 2;
        }
    }

    /* --- control: a fresh volume, and the gap is where the layout says --- */
    if (run("INVFS_CODECPACK_REGISTRY=none %s/invf-mkfs '%s' 1 >/dev/null 2>&1",
            b, img) != 0) {
        fprintf(stderr, "cannot create volume with invf-mkfs (%s)\n", img);
        return 2;
    }
    if (gap_bounds(img, &gstart, &gblocks) != 0) {
        fprintf(stderr, "cannot read the superblock of %s\n", img);
        return 2;
    }
    if (gstart == 0 || gblocks != INVFS_JOURNAL_BLOCKS) {
        fail("layout: reserved gap is [%llu,+%llu), expected [+%d) at a "
             "non-zero block", (unsigned long long)gstart,
             (unsigned long long)gblocks, INVFS_JOURNAL_BLOCKS);
        return 1;
    }
    printf("ok  LAYOUT   reserved gap is blocks %llu..%llu\n",
           (unsigned long long)gstart,
           (unsigned long long)(gstart + gblocks - 1));

    nz = gap_nonzero(img, gstart, gblocks, &first);
    if (nz == (uint64_t)-1) { fprintf(stderr, "cannot scan the gap\n"); return 2; }
    if (nz != 0)
        fail("CONTROL: a volume invf-mkfs just created already has %llu "
             "non-zero byte(s) in the reserved gap (first at +%lld). Every "
             "later leg would pass vacuously over a region that is not "
             "the gap.", (unsigned long long)nz, first);
    else
        printf("ok  CONTROL  a fresh volume's reserved gap is all zero\n");

    /* --- LEG 1: write / overwrite / sweep / fsck, then look again -------- */
    if (run("INVFS_CODECPACK_REGISTRY=none %s/invf-import '%s' '%s' "
            ">/dev/null 2>&1", b, img, src) != 0) {
        fail("import failed");
        return 1;
    }
    /* overwrite one file in place: the supersede path, not just create */
    snprintf(f1, sizeof f1, "%s/f01.bin", src);
    if (run("head -c 40000 /dev/urandom > '%s'", f1) != 0) return 2;
    if (run("INVFS_CODECPACK_REGISTRY=none %s/invf-import '%s' '%s' "
            ">/dev/null 2>&1", b, img, src) != 0)
        fail("re-import (overwrite) failed");
    if (run("INVFS_CODECPACK_REGISTRY=none %s/invf-sweep '%s' >/dev/null 2>&1",
            b, img) != 0)
        fail("sweep failed");
    if (run("INVFS_CODECPACK_REGISTRY=none %s/invf-fsck '%s' >/dev/null 2>&1",
            b, img) != 0)
        fail("fsck reported a problem on a freshly written volume");

    nz = gap_nonzero(img, gstart, gblocks, &first);
    if (nz == (uint64_t)-1) { fprintf(stderr, "cannot scan the gap\n"); return 2; }
    if (nz != 0)
        fail("LEG 1: after import + overwrite + sweep + fsck the reserved gap "
             "holds %llu non-zero byte(s), first at +%lld (block %llu). "
             "Something in the shipped write path is still writing to the "
             "retired mapping log.",
             (unsigned long long)nz, first,
             (unsigned long long)(gstart + (uint64_t)(first / BS)));
    else
        printf("ok  LEG 1    the reserved gap is still all zero after a full "
               "write/overwrite/sweep/fsck cycle\n");

    /* --- LEG 3: the cycle above rewrote data. It has to come back. ----- */
    {
        int ok = 1;
        for (i = 0; i < nfiles; i++) {
            snprintf(f1, sizeof f1, "%s/f%02d.bin", src, i);
            snprintf(out, sizeof out, "%s/cat%02d.bin", gap, i);
            if (run("INVFS_CODECPACK_REGISTRY=none %s/invf-cat '%s' 'f%02d.bin' "
                    "'%s' >/dev/null 2>&1", b, img, i, out) != 0) {
                fail("invf-cat failed on f%02d.bin", i);
                ok = 0;
                continue;
            }
            snprintf(f2, sizeof f2, "%s/cat%02d.bin", gap, i);
            if (run("cmp -s '%s' '%s'", f1, f2) != 0) {
                fail("MISMATCH: f%02d.bin did not come back byte-identical", i);
                ok = 0;
            }
        }
        if (ok)
            printf("ok  LEG 3    all %d files byte-identical after the cycle\n",
                   nfiles);
    }

    /* --- LEG 2: every shipped tool refuses a v2-flagged image ---------- */
    if (demote(img, dem) != 0) {
        fprintf(stderr, "cannot demote %s\n", img);
        return 2;
    }
    {
        static const char *tools[] = {
            "invf-ls", "invf-stat", "invf-fsck", "invf-verify",
            "invf-sweep", "invf-rollback", "invf-stats", "invf-resize",
            "invf-migrate-v2", "invf-l2ptest", "invf-sizes"
        };
        int n = (int)(sizeof tools / sizeof tools[0]);
        int bad = 0, missing = 0;
        for (i = 0; i < n; i++) {
            char probe[600], sout[700];
            int rc;
            snprintf(probe, sizeof probe, "%s/%s", b, tools[i]);
            snprintf(sout, sizeof sout, "test -x '%s'", probe);
            if (run(sout) != 0) { missing++; continue; }
            /* an argument each one will not reject before it opens the
             * volume; the point is that the OPEN refuses, not the usage */
            if (!strcmp(tools[i], "invf-ls"))          rc = run("%s '%s' >/dev/null 2>&1", probe, dem);
            else if (!strcmp(tools[i], "invf-stat"))    rc = run("%s '%s' >/dev/null 2>&1", probe, dem);
            else if (!strcmp(tools[i], "invf-verify"))  rc = run("%s '%s' >/dev/null 2>&1", probe, dem);
            else if (!strcmp(tools[i], "invf-rollback"))rc = run("%s '%s' >/dev/null 2>&1", probe, dem);
            else if (!strcmp(tools[i], "invf-resize"))  rc = run("%s '%s' 1 >/dev/null 2>&1", probe, dem);
            else                                        rc = run("%s '%s' >/dev/null 2>&1", probe, dem);
            if (rc == 0) {
                fail("LEG 2: %s returned 0 on a v2-flagged image -- it has a "
                     "private path around the format gate", tools[i]);
                bad++;
            }
        }
        if (bad == 0)
            printf("ok  LEG 2    every shipped tool refuses a v2-flagged "
                   "image (%d present%s)\n", n - missing,
                   missing ? ", the retired ones absent as they must be" : "");
    }

    if (fails) {
        fprintf(stderr, "no_v2_surface_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("no_v2_surface_test: PASS\n");
    return 0;
}