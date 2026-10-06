/* no_ckp0_surface_test.c — the CKP0 sweep checkpoint is gone, and this is
 * how we know.
 *
 * The sibling argument (no_v2_surface_test.c) applies here, and it is worth
 * restating because it decided the shape of this file. A deletion whose
 * effect nobody can observe is one the next change silently undoes:
 *
 *   - grep the tree / the built objects. Tests the NAME, and the name is
 *     what a re-grow changes first. A writer brought back as
 *     `write_sweep_marker()` passes a grep for `vol_ckp_begin` and still
 *     writes to the same 56 bytes.
 *   - link-failure check. Everything already links; re-adding a CALL to a
 *     re-added function links fine.
 *
 * What this asserts instead is the on-disk EFFECT, through the shipped
 * binaries only.
 *
 * THE REGION. The CKP0 descriptor was the 56 bytes of block 0 at
 * [0x220, 0x258). That span is RESERVED and stays reserved-zero: the
 * on-disk layout is not renumbered by this change. It is a different
 * region from the 32 MiB gap that no_v2_surface_test.c scans, and it needs
 * its own scan -- "the gap is zero" says nothing about block 0.
 *
 *   LEG 1: the slot is byte-for-zero on a volume invf-mkfs just created,
 *   and again after a full import / overwrite / sweep / fsck / rollback /
 *   resize cycle. Every writer of that slot wrote there and nowhere else,
 *   so "the slot is still zero" is the direct statement that nothing in the
 *   shipped binaries reached it, whatever the writer is called. Bring back
 *   any caller of the retired surface and this goes red.
 *
 *   LEG 2: the CONTROL cell. The assertion is a byte scan of a fixed span,
 *   so it could pass vacuously over the wrong 56 bytes. Two controls: the
 *   span is read at the offset the layout says (a constant, checked
 *   against a descriptor that IS live in the same block -- RT30 at 0x9D0),
 *   and the scan itself is exercised against a descriptor deliberately
 *   planted in the span, which must be REPORTED. A green control that was
 *   never made to go red proves nothing, and that has happened on this
 *   project before.
 *
 *   LEG 3: bit-exactness. The cycle in LEG 1 rewrites data, so the files
 *   must come back byte-identical. Without this leg a test that deletes an
 *   engine path could go green by deleting the path that reads.
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

/* The retired slot's span. Named here, not taken from invarifs.h, because
 * invarifs.h no longer defines it -- the constant is the POINT of the
 * test, and inheriting it from a header that no longer carries it would
 * make the test follow a re-grow instead of catching one. */
#define CKP0_SLOT_OFF 0x220
#define CKP0_SLOT_LEN 56           /* 0x220..0x258 */

/* A descriptor that IS live in the same block, used as the control that
 * the scan is looking at block 0 at all. */
#define LIVE_DESC_OFF 0x9D0       /* RT30 */
#define LIVE_DESC_LEN 64

/* The region, read back from the superblock the way vol_open reads it, so
 * this is a test of the layout and not of a number someone typed. */
static int vol_geometry(const char *img, uint64_t *total_blocks,
                        int *is_meta)
{
    blkio io;
    invfs_superblock sb;
    int rc = -1;

    if (blkio_open(&io, img, 0) != 0)
        return -1;
    if (blkio_pread(&io, 0, &sb, sizeof sb) == 0) {
        *total_blocks = sb.total_blocks;
        *is_meta = (sb.vol_flags & VOLF_META) != 0;
        rc = 0;
    }
    blkio_close(&io);
    return rc;
}

/* Count non-zero bytes in [off, off+len) of the image's block 0. *first is
 * the offset of the first one, or -1. */
static int slot_scan(const char *img, uint64_t off, uint64_t len,
                     uint64_t *first_out)
{
    FILE *f = fopen(img, "rb");
    uint8_t *buf;
    uint64_t i;
    int nz = 0;

    *first_out = (uint64_t)-1;
    if (!f) return -1;
    buf = malloc(BS);
    if (!buf) { fclose(f); return -1; }
    if (fseek(f, (long)off, SEEK_SET) != 0 ||
        fread(buf, 1, (size_t)len, f) != len) {
        free(buf); fclose(f); return -1;
    }
    for (i = 0; i < len; i++)
        if (buf[i]) {
            if (*first_out == (uint64_t)-1) *first_out = off + i;
            nz++;
        }
    free(buf);
    fclose(f);
    return nz;
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

/* Write a byte pattern over the slot. The control leg calls this on a
 * throwaway COPY of the volume so the real one under test is untouched. */
static int plant(const char *img)
{
    blkio io;
    uint8_t pat[CKP0_SLOT_LEN];
    int i, rc = -1;

    for (i = 0; i < CKP0_SLOT_LEN; i++)
        pat[i] = (uint8_t)(i + 1);
    if (blkio_open(&io, img, 0) != 0) return -1;
    if (blkio_pwrite(&io, CKP0_SLOT_OFF, pat, sizeof pat) == 0)
        rc = 0;
    blkio_close(&io);
    return rc;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    const char *b = "./bin";
    char img[512], can[512], src[512], out[1024], f1[1024], f2[1024];
    uint64_t total_blocks = 0, first = 0;
    int is_meta = 0, i, nz, nfiles = 6;

    snprintf(img, sizeof img, "%s/no_ckp0.img", dir);
    snprintf(can, sizeof can, "%s/no_ckp0_canary.img", dir);
    snprintf(src, sizeof src, "%s/no_ckp0_src", dir);

    /* --- corpus: mixed shapes, and one file rewritten in LEG 1 so the
     *     cycle covers supersede-as-well-as-create --- */
    if (run("rm -rf '%s' && mkdir -p '%s'", src, src) != 0) {
        fprintf(stderr, "cannot prepare %s\n", src);
        return 2;
    }
    for (i = 0; i < nfiles; i++) {
        snprintf(f1, sizeof f1, "%s/f%02d.bin", src, i);
        if (i % 2 == 0) {
            int k;
            FILE *f = fopen(f1, "wb");
            if (!f) return 2;
            for (k = 0; k < 4000; k++)
                fprintf(f, "line %d of file %02d: the quick brown fox\n", k, i);
            fclose(f);
        } else if (run("head -c 40000 /dev/urandom > '%s'", f1) != 0) {
            return 2;
        }
    }

    /* --- LEG 2a: the control cell. A fresh volume, read at the offset the
     *     layout says, and the scan proven to SEE a live descriptor in the
     *     same block. --- */
    if (run("rm -f '%s' && INVFS_CODECPACK_REGISTRY=none %s/invf-mkfs '%s' 1 "
            ">/dev/null 2>&1", img, b, img) != 0) {
        fprintf(stderr, "cannot create volume with invf-mkfs (%s)\n", img);
        return 2;
    }
    if (vol_geometry(img, &total_blocks, &is_meta) != 0) {
        fprintf(stderr, "cannot read the superblock of %s\n", img);
        return 2;
    }
    if (!is_meta) {
        fail("CONTROL: invf-mkfs produced a volume without VOLF_META; every "
             "leg below would be testing a format this build refuses");
        return 1;
    }
    if (total_blocks == 0) {
        fail("CONTROL: the superblock reports total_blocks == 0");
        return 1;
    }
    printf("ok  CONTROL  the volume is v3, %llu blocks\n",
           (unsigned long long)total_blocks);

    /* RT30 is written by the publish path, so on a volume with content it
     * is non-zero. Checking it first is what makes the zero in LEG 1 a
     * statement about the SLOT rather than about an unreadable block 0. */
    if (run("INVFS_CODECPACK_REGISTRY=none %s/invf-import '%s' '%s' "
            ">/dev/null 2>&1", b, img, src) != 0) {
        fail("import failed");
        return 1;
    }
    nz = slot_scan(img, LIVE_DESC_OFF, LIVE_DESC_LEN, &first);
    if (nz <= 0) {
        fail("CONTROL: the RT30 descriptor at 0x%X is empty on a volume that "
             "has just been written to. A scan reporting \"zero\" over "
             "block 0 would be vacuous, so this cell must be RED before any "
             "other cell is believed.", LIVE_DESC_OFF);
        return 1;
    }
    printf("ok  CONTROL  the scan reads block 0: RT30 at 0x%X holds %d "
           "non-zero byte(s)\n", LIVE_DESC_OFF, nz);

    /* --- LEG 2b: THE DETECTOR BITES. Plant a descriptor in the retired
     *     slot on a throwaway copy; the scan must report it. A control
     *     that has never gone red has not been tested. --- */
    if (run("cp '%s' '%s'", img, can) != 0 || plant(can) != 0) {
        fprintf(stderr, "cannot build the canary image\n");
        return 2;
    }
    nz = slot_scan(can, CKP0_SLOT_OFF, CKP0_SLOT_LEN, &first);
    if (nz != CKP0_SLOT_LEN || first != CKP0_SLOT_OFF) {
        fail("CANARY: a descriptor planted over [0x%X,0x%X) was not fully "
             "reported (nonzero=%d, first=0x%llX). The scan cannot be "
             "trusted to report a real one, so LEG 1 below would pass "
             "vacuously.", CKP0_SLOT_OFF, CKP0_SLOT_OFF + CKP0_SLOT_LEN,
             nz, (unsigned long long)first);
        return 1;
    }
    printf("ok  CANARY   a descriptor planted at 0x%X is reported (%d "
           "non-zero bytes) -- the detector bites\n",
           CKP0_SLOT_OFF, nz);
    unlink(can);

    /* --- LEG 1: the full cycle, then look at the slot again --- */
    nz = slot_scan(img, CKP0_SLOT_OFF, CKP0_SLOT_LEN, &first);
    if (nz < 0) { fprintf(stderr, "cannot scan the slot\n"); return 2; }
    if (nz != 0)
        fail("LEG 1: the retired slot at 0x%X already holds %d non-zero "
             "byte(s) after an import, first at 0x%llX. Something in the "
             "write path is still writing the sweep checkpoint.",
             CKP0_SLOT_OFF, nz, (unsigned long long)first);
    else
        printf("ok  LEG 1a   the retired slot is byte-for-zero after "
               "mkfs + import\n");

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

    /* the two consumers: rollback restores the v3 save point (and used to
     * read the retired descriptor), resize rewrites block 0 outright */
    if (run("INVFS_CODECPACK_REGISTRY=none %s/invf-rollback '%s' "
            ">/dev/null 2>&1", b, img) != 0)
        fail("invf-rollback failed on a volume a sweep had just armed");
    snprintf(f2, sizeof f2, "%s/no_ckp0_grow.img", dir);
    if (run("cp '%s' '%s'", img, f2) != 0) return 2;
    if (run("INVFS_CODECPACK_REGISTRY=none %s/invf-sweep '%s' >/dev/null 2>&1",
            b, f2) != 0)
        fail("sweep before resize failed");
    {
        /* grow: a shrink needs a free tail, which a swept volume has not
         * got, and the block-0 commit runs on either */
        char sz[64];
        snprintf(sz, sizeof sz, "%lluG",
                 (unsigned long long)(total_blocks / 1024 / 1024 * 2 + 1));
        if (run("INVFS_CODECPACK_REGISTRY=none %s/invf-resize '%s' %s "
                ">/dev/null 2>&1", b, f2, sz) != 0)
            fail("invf-resize failed on a swept volume");
    }

    nz = slot_scan(img, CKP0_SLOT_OFF, CKP0_SLOT_LEN, &first);
    if (nz < 0) { fprintf(stderr, "cannot scan the slot\n"); return 2; }
    if (nz != 0)
        fail("LEG 1: after import + overwrite + sweep + fsck + rollback the "
             "retired slot holds %d non-zero byte(s), first at 0x%llX. "
             "Something in the shipped path is still writing the sweep "
             "checkpoint.", nz, (unsigned long long)first);
    else
        printf("ok  LEG 1b   the retired slot is byte-for-zero after a full "
               "import/overwrite/sweep/fsck/rollback/resize cycle\n");

    /* --- LEG 3: the cycle above rewrote data. It has to come back. --- */
    {
        int ok = 1;
        for (i = 0; i < nfiles; i++) {
            snprintf(f1, sizeof f1, "%s/f%02d.bin", src, i);
            snprintf(out, sizeof out, "%s/cat%02d.bin", dir, i);
            if (run("INVFS_CODECPACK_REGISTRY=none %s/invf-cat '%s' 'f%02d.bin' "
                    "'%s' >/dev/null 2>&1", b, img, i, out) != 0) {
                fail("invf-cat failed on f%02d.bin", i);
                ok = 0;
                continue;
            }
            if (run("cmp -s '%s' '%s'", f1, out) != 0) {
                fail("MISMATCH: f%02d.bin did not come back byte-identical", i);
                ok = 0;
            }
        }
        if (ok)
            printf("ok  LEG 3    all %d files byte-identical after the cycle\n",
                   nfiles);
    }

    if (fails) {
        fprintf(stderr, "no_ckp0_surface_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("no_ckp0_surface_test: PASS\n");
    return 0;
}