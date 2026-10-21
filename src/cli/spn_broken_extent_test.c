/* spn_broken_extent_test.c -- a capture whose walk cannot VERIFY a segment's
 * extent must not release the rest of that extent.
 *
 * WHAT THIS IS
 * ------------
 * spn_reclaim frees old_map \ new_map, where new_map is the mark set the
 * generation walk built. The walk marks a segment as an EXTENT:
 * [pba, pba + plen) with plen derived from the frame header's own csize
 * (seg_extent, volume.c). When that derivation fails -- a torn header, or a
 * block of the derived run no longer allocated -- the walk can mark exactly
 * ONE block, the head, because it does not know how far the extent reaches.
 * It counted that as `broken` and moved on, and `broken` was reported in the
 * save point's pin line but did NOT reach the reclaim decision, which
 * consulted `indeterminate` (an inode whose block set is unknown) only.
 *
 * So on a volume that already had N unverifiable segments when the pass
 * started, the previous generation's pin had marked those extents whole, the
 * new walk marked only their heads, and the reclaim freed every remaining
 * block of each extent -- under recipes that still name them. That is the
 * WP632 shape: the pass reports "swept=0", the captured pin says "692 already
 * damaged", and the blocks of those segments come back to the shared pool,
 * where the next sequential allocator hands them out and overwrites them.
 *
 * The rule this test enforces is the one the module already applies twice
 * (an unreadable recipe, the base tree being published): a set that is known
 * to be missing an owner is not a basis for releasing anything. Damage that
 * was already there must be CONTAINED by the pass, not amplified into a free.
 *
 * WHY THIS IS NOT A PROBABILITY TEST
 * ----------------------------------
 * The extent is REAL (an incompressible 64 KiB segment is 17 blocks) and the
 * damage is surgical: one pwrite of a zeroed 8-byte frame header, the
 * torn-write shape dm-flakey produces and the shape the guest had. Capture #1
 * is taken while the volume is healthy (the control on the control: if the
 * setup cannot produce a multi-block extent there is nothing to lose and the
 * test says FAIL rather than passing vacuously), then the header is torn and
 * capture #2 runs the reclaim:
 *
 *   pre-fix:  `broken` never reaches spn_reclaim -> the extent's tail is
 *             freed -> spt0_reclaim_last() > 0 and blocks the recipe names
 *             are no longer allocated. The test reports FAIL (red).
 *   post-fix: the capture still arms its pin (it holds what it can prove),
 *             but it reclaims NOTHING and says so. GREEN.
 *
 * A capture that released the extent is not a leak that "the next pass fixes":
 * the blocks are free for the allocator the moment the pass returns, so the
 * file is destroyed before any later pass could look at it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "volume_internal.h"   /* pulls in volume.h + invarifs.h */
#include "vol_spt0.h"

static int checks = 0;
static int failures = 0;
static char g_img[512];
static invfs_volume *g_v = NULL;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
}

/* The extents of one file, as the walk itself derives them: the recipe's
 * entry pbas, and plen from the on-disk frame header. Returns the entry count,
 * or -1. */
static int file_extents(uint64_t inode_id, uint64_t *pba, uint64_t *plen,
                        size_t max)
{
    invfs_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0, n = 0, i;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;

    if (vol_inode_get(g_v, inode_id, &in) != 1)
        return -1;
    if (vol_recipe_load(g_v, in.recipe_addr, &blob, &blen) != 0)
        return -1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n) != 0) {
        free(blob);
        return -1;
    }
    for (i = 0; i < n && i < max; i++) {
        uint64_t p = 0;
        pba[i] = ents[i].pba;
        plen[i] = (seg_extent(g_v, ents[i].pba, NULL, &p) == 0) ? p : 0;
    }
    free(blob);
    return (int)n;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    static uint8_t payload[3 * 64 * 1024];
    uint8_t *back = NULL;
    size_t back_len = 0;
    uint64_t pba[8], plen[8], b;
    int err = 0, n, rc, fd;
    uint32_t k;

    printf("spn_broken_extent_test: an unverifiable extent is not released\n");

    /* incompressible, so the writer stores it VERBATIM and the segment's
     * extent is a real multi-block run (a compressible one would be a single
     * block and the reclaim would have nothing to give back) */
    k = 0x1234ABCDu;
    for (size_t i = 0; i < sizeof payload; i++) {
        k = k * 1103515245u + 12345u;
        payload[i] = (uint8_t)(k >> 16);
    }

    snprintf(g_img, sizeof g_img, "%s/invf-spn-broken-extent-test.img", dir);
    unlink(g_img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 root, g_img);
        if (system(cmd) != 0) {
            printf("  cannot create volume with invf-mkfs\n");
            return 2;
        }
    }
    g_v = vol_open(g_img, &err);
    if (!g_v) {
        printf("  vol_open failed: err=%d\n", err);
        return 2;
    }

    ok(vol_replace_file(g_v, "victim.bin", payload, sizeof payload) != 0,
       "victim.bin written (96 KiB, incompressible)");
    ok(vol_flush(g_v) == 0, "volume flushed");
    {
        uint64_t ino = vol_find(g_v, "victim.bin");
        ok(ino != 0, "victim.bin resolves");
        n = ino ? file_extents(ino, pba, plen, 8) : -1;
        ok(n >= 2, "victim.bin has at least two segments");
        ok(n >= 1 && plen[0] > 1,
           "the last segment is a multi-block extent (the thing a reclaim "
           "can free)");
        if (n < 1 || plen[n - 1] < 2) {
            printf("  setup produced no multi-block extent (%d segments, "
                   "%llu blocks) -- a vacuous pass proves nothing\n", n,
                   n > 0 ? (unsigned long long)plen[n - 1] : 0ULL);
            vol_close(g_v);
            printf("spn_broken_extent_test: %d checks, %d failures\n",
                   checks, failures);
            return 1;
        }
        /* the byte check the volume must not lose: read it back once, now,
         * so a later failure is the pass and not the writer */
        ok(vol_read_file(g_v, ino, &back, &back_len) == 0 &&
           back_len == sizeof payload &&
           memcmp(back, payload, sizeof payload) == 0,
           "victim.bin reads back bit-exact before any capture");
        free(back);
        back = NULL;

        /* capture #1 on the HEALTHY volume: this is the pin whose mark set
         * covers the whole extent, and therefore the worklist capture #2
         * will compute old_map from */
        ok(spt0_capture(g_v) == 0, "capture #1 taken on the healthy volume");
        ok(spt0_reclaim_last(g_v) == 0,
           "capture #1 reclaimed nothing (there was no previous pin)");
        for (b = pba[n - 1]; b < pba[n - 1] + plen[n - 1]; b++)
            if (!bit_get(g_v->bitmap, b))
                break;
        ok(b == pba[n - 1] + plen[n - 1],
           "capture #1 pins every block of the extent");

        /* a bare sweep's shape: the window is dropped, the mark set stays on
         * disk as the next capture's worklist */
        ok(spt0_drop(g_v) >= 0, "capture #1's window dropped (worklist kept)");
    }

    /* Tear ONE segment's frame header -- 8 zero bytes, the torn-write shape.
     * csize becomes 0, so seg_extent cannot derive an extent at all: this is
     * exactly the "692 already damaged" state the guest pin reported. */
    fd = open(g_img, O_RDWR);
    ok(fd >= 0, "sabotage fd opened");
    if (fd < 0) {
        vol_close(g_v);
        return 1;
    }
    {
        static const uint8_t zeros[8] = { 0 };
        ssize_t w = pwrite(fd, zeros, sizeof zeros,
                           (off_t)(pba[n - 1] * INVFS_BLOCK_SIZE));
        ok(w == (ssize_t)sizeof zeros,
           "the last segment's frame header is torn");
    }
    close(fd);

    /* capture #2 must not turn that damage into a free */
    rc = spt0_capture(g_v);
    ok(rc == 0, "capture #2 still arms a pin (it holds what it can prove)");
    printf("        (capture #2 reclaimed %llu block(s))\n",
           (unsigned long long)spt0_reclaim_last(g_v));
    ok(spt0_reclaim_last(g_v) == 0,
       "capture #2 reclaimed NOTHING: a walk that could not verify an extent "
       "is not a basis for releasing blocks");
    for (b = pba[n - 1]; b < pba[n - 1] + plen[n - 1]; b++)
        if (!bit_get(g_v->bitmap, b))
            break;
    ok(b == pba[n - 1] + plen[n - 1],
       "every block of the torn extent is still allocated (the pass contained "
       "the damage instead of amplifying it)");

    vol_close(g_v);
    printf("spn_broken_extent_test: %d checks, %d failures\n",
           checks, failures);
    return failures ? 1 : 0;
}
