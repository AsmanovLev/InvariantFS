/* orphan_test.c — WP121: the safety test for the v3 orphan collector.
 *
 * The collector (btree_collect_orphans, src/core/vol_btree.c) frees base
 * pages that no live root can reach. A wrong answer to "can this page be
 * reached" is SILENT DATA LOSS, not a crash, so this driver is built to
 * fail on the wrong answer rather than to pass on an empty run.
 *
 * Subcommands (the image is a v3 volume made by `INVFS_V3=1 invf-mkfs`):
 *
 *   build <img> <nfiles> <ngen>
 *       Write nfiles files with deterministic content through the public
 *       write path, fold them into the base (so the content is in the base
 *       tree, not just the delta), then force ngen further base
 *       generations with vol_v3_inode_put on scratch ids. Each of those
 *       publishes a root and abandons the previous one, which is the leak.
 *       Prints: FILES=<n> PAGES_ALLOC=<n> PAGES_LIVE=<n> PAGES_ORPHAN=<n>
 *
 *   verify <img> <nfiles>
 *       Read every file back with vol_read_named and compare byte for byte
 *       against the regenerated content. Prints OK / FAIL lines and exits
 *       nonzero on any mismatch or read error. THIS is what catches a
 *       collector that freed a live page.
 *
 *   collect <img>
 *       Run vol_reclaim_orphans once and print COLLECTED=<n>. Prints
 *       COLLECTED=0 when the env gate is off -- the caller asserts on it.
 *
 *   fold <img> <nfiles> <ngen>
 *       The PRODUCTION path, not a direct collector call: write content so
 *       the delta is non-empty (an empty delta makes vol_v3_fold a no-op
 *       and the hook never runs), force generations, then vol_v3_fold. That
 *       reaches fold_reclaim_hook -> the RT30-slot guard ->
 *       vol_reclaim_orphans, which is exactly the chain the FUSE drain
 *       walks. The collector's "collected N pages" line goes to stderr, so
 *       the caller greps for it. Prints the same census `build` prints.
 *
 *   gate-off <img>
 *       Assert the shipped default: with INVFS_RECLAIM_ORPHANS unset, the
 *       collector frees exactly nothing. Run in a clean environment.
 *
 *   slots <img>
 *       Print SLOT0=<pba>:<gen> SLOT1=<pba>:<gen> NEWEST=<pba> OLDER=<pba>
 *       for both RT30 slots, and whether each root page still validates.
 *
 *   walk <img> <root-pba>
 *       Reachability-walk the tree at <root-pba> and print
 *       PAGES=<n> UNALLOCATED=<n>. UNALLOCATED > 0 means the tree is
 *       standing on freed blocks -- the exact hazard WP86's two-slot
 *       fallback turns into a silently truncated namespace.
 *
 *   damage-newest <img>
 *       Scribble over the newest RT30 slot's root page so it fails
 *       mbuf_page_validate, forcing the next mbuf_root_read to adopt the
 *       other slot. This models WP86's damage tolerance.
 *
 *   naive-collect <img>
 *       NEGATIVE CONTROL. Emulate the liveness predicate the task warns
 *       about -- "named by the newest slot" instead of "named by ANY
 *       slot" -- by pointing BOTH RT30 slots at the newest root before
 *       running the real collector, then restoring the descriptor before
 *       the flush. The collector then frees exactly the pages the wrong
 *       predicate would have freed, without a second walker existing in
 *       the test. `pinned-collect` additionally sets v->pinned_root to the
 *       older root, which is the differential: the same emulation WITHOUT
 *       the pin frees the older root's pages, WITH it they must survive.
 *       The caller corrupts the newest root afterwards and asserts whether
 *       the fallback tree is standing on unallocated pages.
 *
 *   reuse <img> <root-pba> <root-gen> <nallocs>
 *       SILENT CORRUPTION PROBE. Snapshot the page set of the tree at
 *       <root-pba>, hand <nallocs> fresh blocks to mbuf_alloc, and print
 *       TREE_PAGES=<n> REUSED=<n>: how many of those pages the allocator
 *       handed out. REUSED > 0 means the reader's fallback tree has been
 *       overwritten underneath it -- wrong data, no error. UNALLOCATED in
 *       `walk` is the same fact one step earlier. Allocation goes through
 *       mbuf_alloc rather than through file writes on purpose: a write
 *       publishes a new root, which would overwrite the damage and take
 *       the reader off the fallback.
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
#include "vol_reclaim.h"

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

/* xorshift32 seeded from (index, variant). Same bytes on every run and on
 * every host, so the readback comparison needs no sidecar file. */
static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Length is a function of the index alone. */
static size_t content_len(int i)
{
    return (size_t)(701 + (size_t)((i * 2654435761u) % 3600u));
}

static void content_fill(int i, int variant, uint8_t *buf, size_t len)
{
    /* Text-ish, so the file goes through the real compress path rather
     * than being stored verbatim -- a verbatim file would not exercise
     * the recipe pages the base tree points at. */
    static const char *w[] = { "alpha", "bravo", "charlie", "delta", "echo",
                               "foxtrot", "golf", "hotel", "india", "juliet" };
    uint32_t s = 0x9E3779B9u ^ (uint32_t)(i * 2654435761u) ^
                 (uint32_t)(variant * 40503u);
    size_t o = 0;
    int rep = 0;

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
        if ((++rep & 7) == 0 && o + 1 <= len) {
            buf[o++] = '\n';
            rep = 0;
        }
    }
    while (o < len)
        buf[o++] = '.';
}

static void fname(int i, char *buf, size_t n)
{
    snprintf(buf, n, "wp121_file_%04d.txt", i);
}

/* ---- page census ------------------------------------------------------ */

static uint64_t count_alloc_pages(invfs_volume *v)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    uint64_t b, n = 0;
    for (b = 1; b < v->sb.total_blocks; b++) {
        if (!bit_get(v->bitmap, b))
            continue;
        if (mbuf_read(v, b, page) != 0)
            continue;
        if (mbuf_page_validate(page))
            n++;
    }
    return n;
}

/* ---- allocation-aware reachability walk ------------------------------- */

/* Deliberately NOT btree_check: btree_check only reads pages, and a freed
 * but intact page reads perfectly. This one asks the question the safety
 * argument is actually about -- is every page of this tree still
 * ALLOCATED -- and answers it from the bitmap.
 *
 * Internal node wire format (vol_btree.c bt_write/bt_read): page header,
 * then nentries x { u16 klen; key[klen]; invfs_blkptr child }. Leaf
 * entries are { u16 klen; key; u16 vlen; value } and have no child. */
static int walk_alloc(invfs_volume *v, uint64_t root_pba, uint8_t *seen,
                      uint64_t *pages, uint64_t *unalloc)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    const invfs_page_hdr *h;
    const uint8_t *p, *end;
    uint64_t b = root_pba;
    uint16_t n, i;

    if (b == 0 || b >= v->sb.total_blocks)
        return -1;
    if (bit_get(seen, b))
        return 0;
    bit_set(seen, b);
    if (mbuf_read(v, b, page) != 0)
        return -1;
    if (!mbuf_page_validate(page))
        return -1;
    (*pages)++;
    if (!bit_get(v->bitmap, b))
        (*unalloc)++;

    h = mbuf_page_chdr(page);
    n = h->nentries;
    if (h->level == INVFS_PAGE_LEVEL_LEAF)
        return 0;
    p = page + sizeof(invfs_page_hdr);
    end = page + INVFS_BLOCK_SIZE;
    for (i = 0; i < n; i++) {
        uint16_t kl;
        uint64_t child;
        if ((size_t)(end - p) < 2 + sizeof(invfs_blkptr))
            return -1;
        memcpy(&kl, p, sizeof kl);
        p += 2 + kl;
        memcpy(&child, p, sizeof child);
        p += sizeof(invfs_blkptr);
        if (child == 0)
            continue;
        if (walk_alloc(v, child, seen, pages, unalloc) != 0)
            return -1;
    }
    return 0;
}

static int walk_report(invfs_volume *v, uint64_t root_pba)
{
    uint8_t *seen = calloc(1, (size_t)((v->sb.total_blocks + 7u) / 8u));
    uint64_t pages = 0, unalloc = 0;
    int rc;

    if (!seen)
        return fail("out of memory");
    rc = walk_alloc(v, root_pba, seen, &pages, &unalloc);
    free(seen);
    if (rc != 0)
        return fail("walk of root %llu failed (rc=%d)", (unsigned long long)root_pba, rc);
    printf("PAGES=%llu UNALLOCATED=%llu\n", (unsigned long long)pages,
           (unsigned long long)unalloc);
    return 0;
}

/* ---- slot helpers ----------------------------------------------------- */

/* Read both RT30 slots straight off the descriptor and report each root's
 * gen and whether its page still passes mbuf_page_validate. *newest_gen /
 * *older_gen carry the gen of whichever slot won each role, so a caller can
 * hand the adopted root to the reuse probe without re-deriving it. */
static int slot_info(invfs_volume *v, uint64_t pba[2], uint64_t gen[2],
                     int valid[2], uint64_t *newest, uint64_t *older,
                     uint64_t *newest_gen, uint64_t *older_gen)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    int i, ni;

    if (!v->rt30_present)
        return -1;
    for (i = 0; i < 2; i++) {
        pba[i] = v->rt30.root_slot[i];
        gen[i] = 0;
        valid[i] = 0;
        if (!pba[i] || pba[i] >= v->sb.total_blocks)
            continue;
        if (mbuf_read(v, pba[i], page) != 0)
            continue;
        if (!mbuf_page_validate(page))
            continue;
        gen[i] = mbuf_page_chdr(page)->gen;
        valid[i] = 1;
    }
    /* The newest VALID slot is the one mbuf_root_read adopts. Prefer
     * validity over gen parity: after damage that is the whole point. */
    if (valid[0] && (!valid[1] || gen[0] > gen[1]))
        ni = 0;
    else if (valid[1])
        ni = 1;
    else
        ni = -1;
    *newest = ni < 0 ? 0 : pba[ni];
    *older = ni < 0 ? 0 : pba[1 - ni];
    if (newest_gen)
        *newest_gen = ni < 0 ? 0 : gen[ni];
    if (older_gen)
        *older_gen = ni < 0 ? 0 : gen[1 - ni];
    return 0;
}

/* ---- subcommands ------------------------------------------------------ */

static int cmd_build(const char *img, int nfiles, int ngen)
{
    invfs_volume *v;
    int err, i;
    uint64_t pba[2], gen[2], newest, older, alloc, live = 0;
    int valid[2];
    char ebuf[128];
    bt_stat bs;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);

    /* (1) real content through the public write path */
    for (i = 0; i < nfiles; i++) {
        char name[64];
        size_t len = content_len(i);
        uint8_t *buf = malloc(len);
        uint64_t id;
        if (!buf)
            return fail("out of memory");
        content_fill(i, 0, buf, len);
        fname(i, name, sizeof name);
        id = vol_replace_file(v, name, buf, len);
        free(buf);
        if (id == 0)
            return fail("vol_replace_file(%s) failed", name);
    }
    /* (2) push the delta into the base so the tree -- not the overlay --
     * holds the namespace. That is what makes the orphan pages matter:
     * they are the superseded COW copies of THIS content. */
    if (vol_v3_fold(v) != 0)
        return fail("vol_v3_fold failed");

    /* (3) force generations. Each vol_v3_inode_put publishes a new base
     * root and abandons the old one; that abandoned page is the leak, one
     * per call. Scratch ids above every real id so the namespace the
     * bit-exactness check reads is untouched. */
    for (i = 0; i < ngen; i++) {
        invfs_v3_inode ino;
        uint64_t id = 0x7000000000000000ull + (uint64_t)i;
        memset(&ino, 0, sizeof ino);
        ino.type = INVFS_ITYP_REG;
        ino.mode = 0100644;
        ino.nlink = 1;
        ino.size = (uint64_t)(i + 1);
        ino.mtime = ino.atime = 1700000000;
        if (vol_v3_inode_put(v, id, &ino) != 0)
            return fail("vol_v3_inode_put(gen %d) failed", i);
    }
    if (vol_flush(v) != 0)
        return fail("vol_flush failed");

    alloc = count_alloc_pages(v);
    {
        invfs_blkptr root;
        if (vol_v3_base_root(v, &root) != 0)
            return fail("vol_v3_base_root failed");
        if (btree_check(v, root, &bs, ebuf, sizeof ebuf) != 0)
            return fail("btree_check: %s", ebuf);
        live = bs.n_pages;
    }
    if (slot_info(v, pba, gen, valid, &newest, &older, NULL, NULL) != 0)
        return fail("RT30 not present");
    printf("FILES=%d GENS=%d SLOT0=%llu:%llu:%d SLOT1=%llu:%llu:%d "
           "NEWEST=%llu OLDER=%llu PAGES_ALLOC=%llu PAGES_LIVE=%llu "
           "PAGES_ORPHAN=%llu\n",
           nfiles, ngen,
           (unsigned long long)pba[0], (unsigned long long)gen[0], valid[0],
           (unsigned long long)pba[1], (unsigned long long)gen[1], valid[1],
           (unsigned long long)newest, (unsigned long long)older,
           (unsigned long long)alloc, (unsigned long long)live,
           (unsigned long long)(alloc > live ? alloc - live : 0));
    vol_close(v);
    return 0;
}

/* The PRODUCTION path, not a direct collector call: write content so the
 * delta is non-empty (an empty delta makes vol_v3_fold a no-op and the hook
 * never runs), force generations, then vol_v3_fold. That reaches
 * fold_reclaim_hook -> the RT30-slot guard -> vol_reclaim_orphans, which is
 * exactly the chain the FUSE drain walks. Prints the same census `build`
 * prints so the caller can assert orphans actually went away. */
static int cmd_fold(const char *img, int nfiles, int ngen)
{
    invfs_volume *v;
    int err, i;
    uint64_t pba[2], gen[2], newest, older, alloc, live = 0;
    int valid[2];
    char ebuf[128];
    bt_stat bs;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    for (i = 0; i < nfiles; i++) {
        char name[64];
        size_t len = content_len(i);
        uint8_t *buf = malloc(len);
        uint64_t id;
        if (!buf)
            return fail("out of memory");
        content_fill(i, 0, buf, len);
        fname(i, name, sizeof name);
        id = vol_replace_file(v, name, buf, len);
        free(buf);
        if (id == 0)
            return fail("vol_replace_file(%s) failed", name);
    }
    for (i = 0; i < ngen; i++) {
        invfs_v3_inode ino;
        uint64_t id = 0x7000000000000000ull + (uint64_t)i;
        memset(&ino, 0, sizeof ino);
        ino.type = INVFS_ITYP_REG;
        ino.mode = 0100644;
        ino.nlink = 1;
        ino.size = (uint64_t)(i + 1);
        ino.mtime = ino.atime = 1700000000;
        if (vol_v3_inode_put(v, id, &ino) != 0)
            return fail("vol_v3_inode_put(gen %d) failed", i);
    }
    if (vol_v3_fold(v) != 0)
        return fail("vol_v3_fold failed");
    if (vol_flush(v) != 0)
        return fail("vol_flush failed");
    alloc = count_alloc_pages(v);
    {
        invfs_blkptr root;
        if (vol_v3_base_root(v, &root) != 0)
            return fail("vol_v3_base_root failed");
        if (btree_check(v, root, &bs, ebuf, sizeof ebuf) != 0)
            return fail("btree_check: %s", ebuf);
        live = bs.n_pages;
    }
    if (slot_info(v, pba, gen, valid, &newest, &older, NULL, NULL) != 0)
        return fail("RT30 not present");
    printf("FOLD FILES=%d GENS=%d SLOT0=%llu:%llu:%d SLOT1=%llu:%llu:%d "
           "NEWEST=%llu OLDER=%llu PAGES_ALLOC=%llu PAGES_LIVE=%llu "
           "PAGES_ORPHAN=%llu\n",
           nfiles, ngen,
           (unsigned long long)pba[0], (unsigned long long)gen[0], valid[0],
           (unsigned long long)pba[1], (unsigned long long)gen[1], valid[1],
           (unsigned long long)newest, (unsigned long long)older,
           (unsigned long long)alloc, (unsigned long long)live,
           (unsigned long long)(alloc > live ? alloc - live : 0));
    vol_close(v);
    return 0;
}

static int cmd_verify(const char *img, int nfiles)
{
    invfs_volume *v;
    int err, i, bad = 0;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    for (i = 0; i < nfiles; i++) {
        char name[64];
        size_t len = content_len(i), got = 0;
        uint8_t *want = malloc(len), *got_buf = NULL;
        fname(i, name, sizeof name);
        content_fill(i, 0, want, len);
        if (vol_read_named(v, name, &got_buf, &got) != 0) {
            bad++;
            fprintf(stderr, "  READ-FAIL %s\n", name);
            free(want);
            continue;
        }
        if (got != len || memcmp(want, got_buf, len) != 0) {
            bad++;
            fprintf(stderr, "  MISMATCH %s: got %llu B, want %llu B\n",
                    name, (unsigned long long)got, (unsigned long long)len);
        }
        free(got_buf);
        free(want);
    }
    vol_close(v);
    if (bad) {
        printf("VERIFY FAIL bad=%d\n", bad);
        return 1;
    }
    printf("VERIFY OK files=%d byte-identical via vol_read_named\n", nfiles);
    return 0;
}

static int cmd_collect(const char *img)
{
    invfs_volume *v;
    int err, rc;
    uint64_t freed = 0;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    rc = vol_reclaim_orphans(v, &freed);
    if (rc < 0) {
        vol_close(v);
        return fail("vol_reclaim_orphans failed");
    }
    printf("COLLECTED=%llu\n", (unsigned long long)freed);
    vol_close(v);
    return 0;
}

static int cmd_gate_off(const char *img)
{
    invfs_volume *v;
    int err, rc;
    uint64_t freed = 0;

    if (getenv("INVFS_RECLAIM_ORPHANS"))
        return fail("INVFS_RECLAIM_ORPHANS is set; the default-off check "
                    "is meaningless in this environment");
    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    rc = vol_reclaim_orphans(v, &freed);
    vol_close(v);
    if (rc < 0)
        return fail("vol_reclaim_orphans failed with the gate off");
    if (freed != 0)
        return fail("the collector freed %llu pages with INVFS_RECLAIM_ORPHANS "
                    "unset -- the gate is not default-off",
                    (unsigned long long)freed);
    printf("GATE OK: collector is a no-op with INVFS_RECLAIM_ORPHANS unset\n");
    return 0;
}

static int cmd_slots(const char *img)
{
    invfs_volume *v;
    int err;
    uint64_t pba[2], gen[2], newest, older, ngen, ogen;
    int valid[2];

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    if (slot_info(v, pba, gen, valid, &newest, &older, &ngen, &ogen) != 0) {
        vol_close(v);
        return fail("RT30 not present");
    }
    printf("SLOT0=%llu:%llu:%d SLOT1=%llu:%llu:%d NEWEST=%llu OLDER=%llu "
           "NEWESTGEN=%llu OLDERGEN=%llu\n",
           (unsigned long long)pba[0], (unsigned long long)gen[0], valid[0],
           (unsigned long long)pba[1], (unsigned long long)gen[1], valid[1],
           (unsigned long long)newest, (unsigned long long)older,
           (unsigned long long)ngen, (unsigned long long)ogen);
    vol_close(v);
    return 0;
}

/* Scribble over the newest slot's root page. The bytes chosen keep the
 * magic intact and flip the CRC, which is the damage mbuf_page_validate
 * has to catch -- the realistic "the CRC32C over the root page did not
 * match" case, not a shredded sector. */
static int cmd_damage_newest(const char *img)
{
    invfs_volume *v;
    int err, i;
    uint64_t pba[2], gen[2], newest, older;
    int valid[2];
    uint8_t page[INVFS_BLOCK_SIZE];

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    if (slot_info(v, pba, gen, valid, &newest, &older, NULL, NULL) != 0) {
        vol_close(v);
        return fail("RT30 not present");
    }
    if (!newest) {
        vol_close(v);
        return fail("no valid RT30 slot to damage");
    }
    if (mbuf_read(v, newest, page) != 0) {
        vol_close(v);
        return fail("cannot read root %llu", (unsigned long long)newest);
    }
    /* flip a byte inside nentries' value region: magic still BPG3, CRC no
     * longer matches. mbuf_page_validate must reject it. */
    for (i = 0; i < 64; i++)
        page[sizeof(invfs_page_hdr) + i] ^= 0xA5u;
    if (io_pwrite(&v->io, newest * (uint64_t)INVFS_BLOCK_SIZE, page,
                  INVFS_BLOCK_SIZE) != 0) {
        vol_close(v);
        return fail("cannot write damaged root");
    }
    printf("DAMAGED=%llu (older slot %llu left intact)\n",
           (unsigned long long)newest, (unsigned long long)older);
    vol_close(v);
    return 0;
}

/* NEGATIVE CONTROL. See the file header. Only v->rt30 in RAM is touched and
 * the descriptor is deliberately NOT left doctored -- the two slot values
 * are restored before the flush -- so the on-disk RT30 still names both
 * roots exactly as it did before this ran. What changes on disk is only
 * which pages the collector considered reachable. */
static int emulate_wrong_predicate(const char *img, int pin_older, const char *tag)
{
    invfs_volume *v;
    int err;
    uint64_t pba[2], gen[2], newest, older, freed = 0, save[2];
    int valid[2];
    invfs_blkptr pinned;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    if (slot_info(v, pba, gen, valid, &newest, &older, NULL, NULL) != 0) {
        vol_close(v);
        return fail("RT30 not present");
    }
    if (!newest || !older || newest == older) {
        vol_close(v);
        return fail("need two distinct valid slots to emulate the wrong "
                    "predicate (newest=%llu older=%llu)",
                    (unsigned long long)newest, (unsigned long long)older);
    }
    save[0] = pba[0];
    save[1] = pba[1];

    /* "live == reachable from the newest root" <=> both slots name the
     * newest root. The shipped collector is unchanged; only its input is. */
    v->rt30.root_slot[0] = newest;
    v->rt30.root_slot[1] = newest;
    if (pin_older) {
        /* WP121 leg 5: the save point's pinned root is a liveness source in
         * its own right. Pinning the older root here -- while the slots
         * name only the newest -- is what makes the differential test: the
         * same emulation WITHOUT the pin frees the older root's pages, and
         * WITH it they must survive. */
        uint8_t page[INVFS_BLOCK_SIZE];
        if (mbuf_read(v, older, page) != 0 || !mbuf_page_validate(page)) {
            vol_close(v);
            return fail("cannot read the older root %llu", (unsigned long long)older);
        }
        mbuf_ptr_set(&pinned, older, page, INVFS_BP_LEAF | INVFS_BP_ROOT);
        v->pinned_root = pinned;
    }

    if (vol_reclaim_orphans(v, &freed) < 0) {
        v->rt30.root_slot[0] = save[0];
        v->rt30.root_slot[1] = save[1];
        vol_close(v);
        return fail("collector failed under the emulated predicate");
    }
    /* Restore the descriptor BEFORE the flush so nothing can persist the
     * doctored slots. */
    v->rt30.root_slot[0] = save[0];
    v->rt30.root_slot[1] = save[1];
    /* Make the frees durable; the RT30 restore above means the on-disk
     * descriptor is the honest one. */
    if (freed && vol_flush(v) != 0) {
        vol_close(v);
        return fail("flush failed");
    }
    printf("%s-COLLECTED=%llu (newest=%llu older=%llu)\n", tag,
           (unsigned long long)freed, (unsigned long long)newest,
           (unsigned long long)older);
    vol_close(v);
    return 0;
}

/* Collect the pba set reachable from root_pba into a byte-per-block map. */
static int reach_set(invfs_volume *v, uint64_t root_pba, uint8_t *in_tree)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    const invfs_page_hdr *h;
    const uint8_t *p, *end;
    uint64_t b = root_pba;
    uint16_t n, i;

    if (b == 0 || b >= v->sb.total_blocks)
        return -1;
    if (in_tree[b])
        return 0;
    in_tree[b] = 1;
    if (mbuf_read(v, b, page) != 0)
        return -1;
    if (!mbuf_page_validate(page))
        return -1;
    h = mbuf_page_chdr(page);
    n = h->nentries;
    if (h->level == INVFS_PAGE_LEVEL_LEAF)
        return 0;
    p = page + sizeof(invfs_page_hdr);
    end = page + INVFS_BLOCK_SIZE;
    for (i = 0; i < n; i++) {
        uint16_t kl;
        uint64_t child;
        if ((size_t)(end - p) < 2 + sizeof(invfs_blkptr))
            return -1;
        memcpy(&kl, p, sizeof kl);
        p += 2 + kl;
        memcpy(&child, p, sizeof child);
        p += sizeof(invfs_blkptr);
        if (child && reach_set(v, child, in_tree) != 0)
            return -1;
    }
    return 0;
}

/* SILENT CORRUPTION PROBE.
 *
 * UNALLOCATED > 0 in a walk is a structural statement; this turns it into
 * the catastrophe it actually is. A page of the reader's fallback tree
 * that sits in the FREE pool is not merely unreadable -- the next
 * mbuf_alloc hands it out, writes somebody else's bytes into it, and the
 * reader, still following the fallback root, reads those bytes as tree
 * nodes. No error, no crash, wrong data.
 *
 * Snapshot the fallback tree's page set, write nfiles files to force fresh
 * allocation, then report how many of those pages the allocator handed out
 * to the new content. REUSED > 0 is that overlap, observed.
 */
static int cmd_reuse(const char *img, uint64_t root_pba, uint64_t root_gen,
                     int nfiles)
{
    invfs_volume *v;
    int err, i;
    uint8_t *in_tree;
    uint64_t b, reused = 0, total = 0;

    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "FAIL: vol_open err=%d\n", err);
        return 1;
    }
    in_tree = calloc(1, (size_t)v->sb.total_blocks);
    if (!in_tree) {
        vol_close(v);
        fprintf(stderr, "FAIL: out of memory\n");
        return 1;
    }
    if (reach_set(v, root_pba, in_tree) != 0) {
        free(in_tree);
        vol_close(v);
        return fail("reach_set(%llu) failed", (unsigned long long)root_pba);
    }
    for (b = 1; b < v->sb.total_blocks; b++)
        if (in_tree[b])
            total++;
    /* Force fresh allocations. This is mbuf_alloc directly, not file
     * writes: a write publishes a new root, which would overwrite the
     * damage and take the reader off the fallback, destroying the very
     * state under test. What the probe needs is the allocator consuming
     * the free pool, and that is exactly mbuf_alloc's job. */
    for (i = 0; i < nfiles; i++) {
        if (!mbuf_alloc(v, root_gen + 100 + (uint64_t)i))
            break;
    }
    if (vol_flush(v) != 0) {
        free(in_tree);
        vol_close(v);
        return fail("flush failed");
    }
    for (b = 1; b < v->sb.total_blocks; b++) {
        uint8_t page[INVFS_BLOCK_SIZE];
        if (!in_tree[b])
            continue;
        if (!bit_get(v->bitmap, b))
            continue;    /* still free: a latent corruption, not a realised one */
        if (mbuf_read(v, b, page) != 0)
            continue;
        /* Allocated AND still a valid BPG3 page is not proof of reuse --
         * the old page was one too. The allocator stamps every page it
         * hands out with the generation it was asked for, so a page of the
         * FALLBACK tree whose gen is now greater than the fallback root's
         * gen was handed out after the fallback was adopted. */
        if (mbuf_page_validate(page) &&
            mbuf_page_chdr(page)->gen > root_gen)
            reused++;
    }
    printf("TREE_PAGES=%llu REUSED=%llu\n", (unsigned long long)total,
           (unsigned long long)reused);
    free(in_tree);
    vol_close(v);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <build|verify|collect|gate-off|slots|"
                        "damage-newest|naive-collect|pinned-collect|walk> "
                        "<img> [args]\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "build") && argc == 5)
        return cmd_build(argv[2], atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(argv[1], "fold") && argc == 5)
        return cmd_fold(argv[2], atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(argv[1], "verify") && argc == 4)
        return cmd_verify(argv[2], atoi(argv[3]));
    if (!strcmp(argv[1], "collect") && argc == 3)
        return cmd_collect(argv[2]);
    if (!strcmp(argv[1], "gate-off") && argc == 3)
        return cmd_gate_off(argv[2]);
    if (!strcmp(argv[1], "slots") && argc == 3)
        return cmd_slots(argv[2]);
    if (!strcmp(argv[1], "damage-newest") && argc == 3)
        return cmd_damage_newest(argv[2]);
    if (!strcmp(argv[1], "naive-collect") && argc == 3)
        return emulate_wrong_predicate(argv[2], 0, "NAIVE");
    if (!strcmp(argv[1], "pinned-collect") && argc == 3)
        return emulate_wrong_predicate(argv[2], 1, "PINNED");
    if (!strcmp(argv[1], "reuse") && argc == 6)
        return cmd_reuse(argv[2], strtoull(argv[3], NULL, 0),
                         strtoull(argv[4], NULL, 0), atoi(argv[5]));
    if (!strcmp(argv[1], "walk") && argc == 4) {
        invfs_volume *v;
        int err, rc;
        v = vol_open(argv[2], &err);
        if (!v) {
            fprintf(stderr, "FAIL: vol_open err=%d\n", err);
            return 1;
        }
        rc = walk_report(v, strtoull(argv[3], NULL, 0));
        vol_close(v);
        return rc;
    }
    fprintf(stderr, "FAIL: bad arguments\n");
    return 2;
}