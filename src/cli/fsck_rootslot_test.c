/* fsck_rootslot_test.c — WP fsck-rt-same-page: two RT30 slots, two
 * different meanings.
 *
 * RT30 is a DOUBLE-slot root descriptor. mbuf_root_publish writes one slot per
 * publish and bumps seq (vol_metabuf.c:479-481), so consecutive publishes
 * alternate slots. That leaves exactly two shapes a healthy volume can end up
 * in, and they are not the same shape:
 *
 *   SAME PAGE, SAME GEN. One root named twice. An ordinary shipped path gets
 *   here: when a pass publishes NO new root, the rollback's mbuf_root_publish
 *   (vol_spt0.c:1299) writes the still-current root into the OTHER slot.
 *   There is nothing to choose between the two names -- it is one root -- so
 *   fsck must report it and NOT call it damage.
 *
 *   TWO DISTINCT PAGES, SAME GEN. The publish order is genuinely not
 *   observable from the volume. This is what the ambiguous-publish detector
 *   exists for, and it must keep firing.
 *
 * The old tiebreak fired on `gen == best_gen` alone. gen is read out of the
 * page's own header, so the SAME-page shape lands on that test by
 * construction, and a clean volume came out DAMAGED with exit 3.
 *
 * Every leg asserts the CONSTRUCTION before the verdict. A suite that only
 * checks "fsck was happy" passes just as well on a driver that never built
 * the shape it thinks it built, so `verify` re-derives the two slots off the
 * closed image and refuses to run if they are not what the mode claims.
 *
 *   build <img> <nfiles>
 *       A v3 volume written through the public write path and folded. Prints
 *       ROOT=<pba>:<gen> SLOT0=<pba>:<gen> SLOT1=<pba>:<gen>.
 *
 *   republish <img>
 *       THE ROLLBACK PUBLISH, verbatim: mbuf_root_publish(current_root,
 *       current_gen) -- the exact call spt0_restore makes at vol_spt0.c:1299.
 *       No page is damaged. Result: both slots name ONE page at one gen.
 *
 *   fork-root <img>
 *       THE NEGATIVE CONTROL. Copies the current root page byte for byte into
 *       a FRESHLY ALLOCATED block and publishes THAT at the same gen.
 *       mbuf_page_crc (vol_metabuf.c:39-45) CRCs the page CONTENT with the
 *       checksum field zeroed and does not include the pba, so the copy
 *       validates and carries the identical gen. The result is the real
 *       ambiguous publish: two DISTINCT valid pages, one generation, no torn
 *       write and no reliance on luck.
 *
 *   verify <img> <clean|ambiguous>
 *       Assert the slot shape the mode names, then the verdict:
 *         clean     -- FAIL if damaged, or slots_ambiguous > 0, or
 *                      slots_same_root != 1.
 *         ambiguous -- FAIL if !damaged or slots_ambiguous != 1. This
 *                      is the leg that must not regress: silencing it would
 *                      trade a false positive for silent corruption.
 *
 * exit 0 = pass, 1 = assertion failed, 2 = usage/open error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_metabuf.h"

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    return 1;
}

/* ---- deterministic content ------------------------------------------- */

static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static size_t content_len(int i)
{
    return (size_t)(701 + (size_t)((i * 2654435761u) % 3600u));
}

static void content_fill(int i, uint8_t *buf, size_t len)
{
    static const char *w[] = { "alpha", "bravo", "charlie", "delta", "echo",
                               "foxtrot", "golf", "hotel", "india", "juliet" };
    uint32_t s = 0x9E3779B9u ^ (uint32_t)(i * 2654435761u);
    size_t o = 0;

    if (!s)
        s = 1;
    while (o < len) {
        const char *t = w[xs32(&s) % 10];
        size_t tl = strlen(t);
        if (o) {
            if (o + 1 > len) break;
            buf[o++] = ' ';
        }
        if (o + tl > len) {
            size_t room = len - o;
            memcpy(buf + o, t, room);
            o = len;
            break;
        }
        memcpy(buf + o, t, tl);
        o += tl;
    }
    while (o < len)
        buf[o++] = '.';
}

/* ---- slot census ------------------------------------------------------ */

/* Per slot: pba, the gen out of the page header, whether the page still
 * validates, and whether the volume still owns the block. The last one is
 * WP123's question and is not what this driver is about -- it is here so a
 * FAIL names every candidate reason rather than just "the shape was wrong". */
static void slot_census(invfs_volume *v, uint64_t pba[2], uint64_t gen[2],
                        int valid[2], int alloc[2])
{
    uint8_t page[INVFS_BLOCK_SIZE];
    int i;

    for (i = 0; i < 2; i++) {
        pba[i] = v->rt.root_slot[i];
        gen[i] = 0;
        valid[i] = 0;
        alloc[i] = 0;
        if (!pba[i] || pba[i] >= v->sb.total_blocks)
            continue;
        alloc[i] = (v->bitmap && bit_get(v->bitmap, pba[i])) ? 1 : 0;
        if (mbuf_read(v, pba[i], page) != 0)
            continue;
        if (!mbuf_page_validate(page))
            continue;
        gen[i] = mbuf_page_chdr(page)->gen;
        valid[i] = 1;
    }
}

static void show_slots(invfs_volume *v, const char *tag)
{
    uint64_t pba[2], gen[2];
    int valid[2], alloc[2];

    slot_census(v, pba, gen, valid, alloc);
    printf("%s SLOT0=%llu:%llu:%d:%d SLOT1=%llu:%llu:%d:%d SEQ=%llu\n", tag,
           (unsigned long long)pba[0], (unsigned long long)gen[0],
           valid[0], alloc[0],
           (unsigned long long)pba[1], (unsigned long long)gen[1],
           valid[1], alloc[1],
           (unsigned long long)v->rt.seq);
}

/* ---- subcommands ------------------------------------------------------ */

static int cmd_build(const char *img, int nfiles)
{
    invfs_volume *v;
    uint64_t pba[2], gen[2], rp = 0, rg = 0;
    int valid[2], alloc[2], err, i, rc;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);

    for (i = 0; i < nfiles; i++) {
        char name[64];
        size_t len = content_len(i);
        uint8_t *buf = malloc(len);
        uint64_t id;
        if (!buf) { vol_close(v); return fail("out of memory"); }
        content_fill(i, buf, len);
        snprintf(name, sizeof name, "wp_fsck_rootslot_%04d.txt", i);
        id = vol_replace_file(v, name, buf, len);
        free(buf);
        if (id == 0) {
            vol_close(v);
            return fail("vol_replace_file(%s) failed", name);
        }
    }
    if (vol_fold(v) != 0) { vol_close(v); return fail("vol_fold failed"); }
    if (vol_flush(v) != 0) { vol_close(v); return fail("vol_flush failed"); }

    rc = mbuf_root_read(v, &rp, &rg);
    slot_census(v, pba, gen, valid, alloc);
    printf("FILES=%d ROOT=%llu:%llu ROOT_RC=%d SLOT0=%llu:%llu:%d:%d "
           "SLOT1=%llu:%llu:%d:%d SEQ=%llu\n",
           nfiles, (unsigned long long)rp, (unsigned long long)rg, rc,
           (unsigned long long)pba[0], (unsigned long long)gen[0],
           valid[0], alloc[0],
           (unsigned long long)pba[1], (unsigned long long)gen[1],
           valid[1], alloc[1], (unsigned long long)v->rt.seq);
    vol_close(v);
    if (rc != 0 || !rp)
        return fail("the built volume has no root to publish");
    return 0;
}

/* The rollback publish, verbatim. spt0_restore does exactly this at
 * vol_spt0.c:1299 when the save point's base_root is still the current root:
 * no page is written, no page is damaged, the current root is simply named
 * from the other slot. That is the whole false positive. */
static int cmd_republish(const char *img)
{
    invfs_volume *v;
    uint64_t pba[2], gen[2], rp = 0, rg = 0;
    int valid[2], alloc[2], err, rc;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    rc = mbuf_root_read(v, &rp, &rg);
    if (rc != 0 || !rp) {
        vol_close(v);
        return fail("no root to republish (rc=%d)", rc);
    }
    slot_census(v, pba, gen, valid, alloc);
    /* Land in the slot that is NOT already naming the current root, which is
     * where mbuf_root_publish's seq parity sends the next publish. */
    if (mbuf_root_publish(v, rp, rg) != 0) {
        vol_close(v);
        return fail("mbuf_root_publish(%llu, gen %llu) failed",
                    (unsigned long long)rp, (unsigned long long)rg);
    }
    if (vol_flush(v) != 0) { vol_close(v); return fail("vol_flush failed"); }
    show_slots(v, "REPUBLISHED");
    vol_close(v);
    return 0;
}

/* The negative control: a genuinely ambiguous publish, built rather than
 * waited for. See the header for why a byte copy is the right primitive. */
static int cmd_fork_root(const char *img)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    invfs_volume *v;
    uint64_t pba[2], gen[2], rp = 0, rg = 0, fork;
    int valid[2], alloc[2], err, rc;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    rc = mbuf_root_read(v, &rp, &rg);
    if (rc != 0 || !rp) {
        vol_close(v);
        return fail("no root to fork (rc=%d)", rc);
    }
    if (mbuf_read(v, rp, page) != 0) {
        vol_close(v);
        return fail("cannot read root page %llu", (unsigned long long)rp);
    }
    /* A block of our own, so the copy is a real distinct page rather than a
     * rewrite of the one we are copying. */
    fork = mbuf_alloc(v, 0xE0000000ull);
    if (!fork || fork == rp) {
        vol_close(v);
        return fail("mbuf_alloc did not hand out a distinct block (got %llu, "
                    "root is %llu)", (unsigned long long)fork,
                    (unsigned long long)rp);
    }
    /* Verbatim bytes: the page's own header carries the same gen, and its
     * CRC32C covers the content only, so the copy validates as itself. */
    if (io_pwrite(&v->io, fork * (uint64_t)INVFS_BLOCK_SIZE, page,
                  INVFS_BLOCK_SIZE) != 0) {
        vol_close(v);
        return fail("cannot write the forked page to block %llu",
                    (unsigned long long)fork);
    }
    if (vol_bitmap_flush(v) != 0) {
        vol_close(v);
        return fail("vol_bitmap_flush failed");
    }
    slot_census(v, pba, gen, valid, alloc);
    if (mbuf_root_publish(v, fork, rg) != 0) {
        vol_close(v);
        return fail("mbuf_root_publish(%llu, gen %llu) refused -- the copy did "
                    "not validate", (unsigned long long)fork,
                    (unsigned long long)rg);
    }
    if (vol_flush(v) != 0) { vol_close(v); return fail("vol_flush failed"); }
    show_slots(v, "FORKED");
    printf("ORIGIN=%llu:%llu FORK=%llu:%llu\n",
           (unsigned long long)rp, (unsigned long long)rg,
           (unsigned long long)fork, (unsigned long long)rg);
    vol_close(v);
    return 0;
}

static int cmd_verify(const char *img, const char *mode)
{
    invfs_fsck_report rep;
    invfs_volume *v;
    uint64_t pba[2], gen[2];
    int valid[2], alloc[2], err, want_amb;

    want_amb = (strcmp(mode, "ambiguous") == 0);
    if (!want_amb && strcmp(mode, "clean") != 0)
        return fail("mode must be 'clean' or 'ambiguous' (got '%s')", mode);

    /* Reopen from the CLOSED image: the shape under test is what the next
     * operator's invf-fsck will see, not what this process still holds in
     * memory. */
    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    slot_census(v, pba, gen, valid, alloc);
    vol_close(v);

    printf("SLOT0=%llu:%llu:%d:%d SLOT1=%llu:%llu:%d:%d\n",
           (unsigned long long)pba[0], (unsigned long long)gen[0],
           valid[0], alloc[0],
           (unsigned long long)pba[1], (unsigned long long)gen[1],
           valid[1], alloc[1]);

    /* THE SHAPE, asserted first. Without this the verdict below proves
     * nothing: a driver that failed to construct the case would report the
     * same verdict and the leg would pass for the wrong reason. */
    if (!valid[0] || !valid[1]) {
        return fail("both slots must hold a VALID page: slot0 valid=%d "
                    "alloc=%d, slot1 valid=%d alloc=%d -- the construction "
                    "did not happen", valid[0], alloc[0], valid[1], alloc[1]);
    }
    if (gen[0] != gen[1]) {
        return fail("both slots must hold the same gen to exercise the "
                    "tiebreak: gen0=%llu gen1=%llu",
                    (unsigned long long)gen[0], (unsigned long long)gen[1]);
    }
    if (!alloc[0] || !alloc[1])
        return fail("both slot pages must be ALLOCATED (WP123's invariant, "
                    "not this driver's): alloc0=%d alloc1=%d",
                    alloc[0], alloc[1]);
    if (want_amb && pba[0] == pba[1]) {
        return fail("mode 'ambiguous' needs two DISTINCT pages; both slots "
                    "name pba %llu -- the negative control is vacuous",
                    (unsigned long long)pba[0]);
    }
    if (!want_amb && pba[0] != pba[1]) {
        return fail("mode 'clean' needs both slots to name the SAME page; "
                    "they name %llu and %llu",
                    (unsigned long long)pba[0], (unsigned long long)pba[1]);
    }

    memset(&rep, 0, sizeof rep);
    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    if (vol_fsck_scan(v, &rep, 0) != 0) {
        vol_close(v);
        return fail("vol_fsck_scan failed");
    }
    vol_close(v);

    printf("VERDICT damaged=%d ambiguous=%llu same_root=%llu torn=%llu "
           "bad_pages=%llu\n", rep.damaged,
           (unsigned long long)rep.slots_ambiguous,
           (unsigned long long)rep.slots_same_root,
           (unsigned long long)rep.slots_torn,
           (unsigned long long)rep.bad_pages);

    if (want_amb) {
        /* The detection this suite exists to protect. A fix that silenced
         * the equal-gen tiebreak outright -- the easy way to stop the false
         * positive -- fails HERE, and fails loudly. */
        if (!rep.damaged)
            return fail("TWO DIFFERENT roots are valid at the same gen "
                        "(pba %llu and %llu, gen %llu) and fsck did NOT report "
                        "damage -- an ambiguous publish would now pass "
                        "silently", (unsigned long long)pba[0],
                        (unsigned long long)pba[1],
                        (unsigned long long)gen[0]);
        if (rep.slots_ambiguous != 1)
            return fail("expected slots_ambiguous == 1 for a genuinely "
                        "ambiguous publish, got %llu",
                        (unsigned long long)rep.slots_ambiguous);
        if (rep.slots_same_root != 0)
            return fail("a two-distinct-page publish was also counted as the "
                        "same root (%llu)",
                        (unsigned long long)rep.slots_same_root);
        printf("OK: fsck still CATCHES two different roots at the same "
               "generation (pba %llu and %llu, gen %llu) -- DAMAGED\n",
               (unsigned long long)pba[0], (unsigned long long)pba[1],
               (unsigned long long)gen[0]);
        return 0;
    }

    /* The false positive this WP removes. */
    if (rep.damaged)
        return fail("a volume whose two slots name the SAME root (pba %llu, "
                    "gen %llu) was reported DAMAGED: ambiguous=%llu torn=%llu "
                    "-- a clean volume must not send an operator to a repair",
                    (unsigned long long)pba[0], (unsigned long long)gen[0],
                    (unsigned long long)rep.slots_ambiguous,
                    (unsigned long long)rep.slots_torn);
    if (rep.slots_ambiguous)
        return fail("the same-root shape was counted as an ambiguous publish "
                    "(%llu)",
                    (unsigned long long)rep.slots_ambiguous);
    if (rep.slots_same_root != 1)
        return fail("expected slots_same_root == 1 so the operator is TOLD "
                    "the two slots name one root, got %llu",
                    (unsigned long long)rep.slots_same_root);
    printf("OK: both slots name one root (pba %llu, gen %llu) and fsck "
           "reports it as such, not as damage\n",
           (unsigned long long)pba[0], (unsigned long long)gen[0]);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <build|republish|fork-root|verify> "
                        "<img> [args]\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "build") && argc == 4)
        return cmd_build(argv[2], atoi(argv[3]));
    if (!strcmp(argv[1], "republish") && argc == 3)
        return cmd_republish(argv[2]);
    if (!strcmp(argv[1], "fork-root") && argc == 3)
        return cmd_fork_root(argv[2]);
    if (!strcmp(argv[1], "verify") && argc == 4)
        return cmd_verify(argv[2], argv[3]);
    fprintf(stderr, "FAIL: bad arguments\n");
    return 2;
}
