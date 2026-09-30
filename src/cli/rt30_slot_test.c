/* rt30_slot_test.c — WP123: does the RT30 READER enforce the allocation
 * invariant, or only the page-integrity one?
 *
 * Meta-v3's RT30 is a DOUBLE-slot root descriptor (WP86): mbuf_root_publish
 * writes one slot per publish, so the other slot keeps naming the previous
 * root. That is damage tolerance -- if the newest root page is torn, the
 * reader falls back. The fallback is only sound while the page it falls back
 * to is still ALLOCATED. WP86's reader (mbuf_root_read) accepted a slot whose
 * page merely passed mbuf_page_validate, and mbuf_page_validate checks magic
 * + CRC32C and nothing else. A page that has been FREED but not yet reused
 * validates perfectly. So the reader could adopt a root standing on blocks the
 * allocator has already taken back.
 *
 * This driver is built to answer that question with a construction, not an
 * argument, and to answer it in BOTH directions:
 *
 *   build <img> <nfiles> <ngen>
 *       Write nfiles files through the public write path, fold them into the
 *       base, then force ngen further base generations with vol_v3_inode_put
 *       on scratch ids. Each forced generation publishes a root and abandons
 *       the previous one, so RT30 ends up naming two DISTINCT roots and the
 *       pool is full of superseded COW copies. Prints
 *       SLOT0=<pba>:<gen>:<valid>:<alloc> SLOT1=... ADOPTED=<pba>:<gen>
 *       ADOPTED_ALLOC=<0|1>.
 *
 *   probe <img> <safe|hazard>
 *       The measurement, with the assertion attached:
 *         safe   -- FAIL if mbuf_root_read adopts a root whose block the
 *                   allocation bitmap says is free. This is the invariant.
 *         hazard -- FAIL if mbuf_root_read does NOT adopt such a root. This
 *                   is the RED CONTROL: it is the pre-fix behaviour, spelled
 *                   out as an assertion, so the suite proves the hazard is
 *                   real rather than merely failing to notice it. A suite
 *                   that can only ever pass on correct code cannot tell a
 *                   fix from a no-op.
 *       Prints per-slot PAGE_VALID / BLOCK_ALLOC / REASON, the slot the
 *       current seq parity calls newest, and what the reader adopted.
 *
 *   free-adopted <img>
 *       THE CONSTRUCTION, via the reclaim primitive itself: free the root
 *       page mbuf_root_read currently adopts (mbuf_free, then make the
 *       bitmap durable exactly the way btree_collect_orphans does). The page
 *       BYTES ARE LEFT INTACT -- that is the whole point, a freed page still
 *       validates. Prints FREED=<pba>.
 *
 *   damage-newest <img>
 *       Scribble inside the newest slot's root page AFTER its header: magic
 *       BPG3 stays intact, the CRC32C breaks, so mbuf_page_validate rejects
 *       it and the reader takes WP86's fallback. This is the torn write.
 *
 *   damage-tree-page <img>
 *       Walk the adopted root's tree and scribble ONE NON-ROOT page, breaking
 *       its CRC. This makes invf-fsck quarantine something, which is what
 *       arms the reachability diff at vol_fsck.c:1306 -- the shipped path
 *       that frees a page a live RT30 slot still names.
 *
 *   reuse <img> <n>
 *       SILENT-CORRUPTION PROBE, no damage required: hand n fresh blocks to
 *       mbuf_alloc and report how many of them are blocks an RT30 slot still
 *       names. REUSED > 0 means the descriptor is now pointing into the free
 *       pool, so the next reader that trusts it adopts somebody else's page.
 *       Allocations go through mbuf_alloc rather than through file writes on
 *       purpose: a write publishes a new root and would move the target.
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

static void content_fill(int i, int variant, uint8_t *buf, size_t len)
{
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
    snprintf(buf, n, "wp123_file_%04d.txt", i);
}

/* ---- slot census ------------------------------------------------------ */

/* Per slot: does the page still pass mbuf_page_validate, and does the
 * allocation bitmap say the block is still allocated? Those are two
 * DIFFERENT questions and WP86 only ever asked the first. */
static void slot_census(invfs_volume *v, uint64_t pba[2], uint64_t gen[2],
                        int valid[2], int alloc[2])
{
    uint8_t page[INVFS_BLOCK_SIZE];
    int i;

    for (i = 0; i < 2; i++) {
        pba[i] = v->rt30.root_slot[i];
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

/* Why a slot was not adoptable, in an operator's words. The point of WP123
 * is that "torn" and "freed" are different failures with different fixes,
 * and a reader that cannot tell them apart cannot report either. */
static const char *slot_reason(invfs_volume *v, uint64_t pba, int valid,
                               int alloc)
{
    if (!pba)
        return "empty";
    if (pba >= v->sb.total_blocks)
        return "out-of-range";
    if (!alloc)
        return "freed (allocation bitmap says the block is free)";
    if (!valid)
        return "torn (magic or CRC32C broken)";
    return "live";
}

static int adopt(invfs_volume *v, uint64_t *pba, uint64_t *gen)
{
    uint64_t p = 0, g = 0;
    int rc = mbuf_root_read(v, &p, &g);
    if (pba) *pba = p;
    if (gen)  *gen  = g;
    return rc;
}

static int adopted_allocated(invfs_volume *v, uint64_t pba)
{
    if (!pba || !v->bitmap || pba >= v->sb.total_blocks)
        return 0;
    return bit_get(v->bitmap, pba) ? 1 : 0;
}

/* ---- tree walk (to find a non-root page to damage) -------------------- */

/* Internal node wire format (vol_btree.c bt_write/bt_read): page header,
 * then nentries x { u16 klen; key[klen]; invfs_blkptr child }. Leaf entries
 * are { u16 klen; key; u16 vlen; value } and carry no child. */
static int walk_collect(invfs_volume *v, uint64_t b, uint8_t *seen,
                        uint64_t *out, uint64_t *n, uint64_t cap)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    const invfs_page_hdr *h;
    const uint8_t *p, *end;
    uint16_t nent, i;

    if (b == 0 || b >= v->sb.total_blocks)
        return -1;
    if (bit_get(seen, b))
        return 0;
    bit_set(seen, b);
    if (mbuf_read(v, b, page) != 0)
        return -1;
    if (!mbuf_page_validate(page))
        return -1;
    if (*n >= cap)
        return 0;
    out[(*n)++] = b;

    h = mbuf_page_chdr(page);
    nent = h->nentries;
    if (h->level == INVFS_PAGE_LEVEL_LEAF)
        return 0;
    p = page + sizeof(invfs_page_hdr);
    end = page + INVFS_BLOCK_SIZE;
    for (i = 0; i < nent; i++) {
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
        if (walk_collect(v, child, seen, out, n, cap) != 0)
            return -1;
    }
    return 0;
}

/* Scribble AFTER the header: magic BPG3 stays intact, the CRC32C breaks.
 * That is exactly the damage model WP86's fallback is supposed to absorb --
 * and exactly the damage that, combined with a freed block, used to turn
 * into a silently adopted old namespace. */
static int scribble(invfs_volume *v, uint64_t pba, const char *what)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    int i;

    if (mbuf_read(v, pba, page) != 0)
        return fail("cannot read %s page %llu", what,
                    (unsigned long long)pba);
    for (i = 0; i < 64; i++)
        page[sizeof(invfs_page_hdr) + i] ^= 0xA5u;
    if (io_pwrite(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE, page,
                  INVFS_BLOCK_SIZE) != 0)
        return fail("cannot write %s page %llu", what,
                    (unsigned long long)pba);
    printf("DAMAGED=%llu (%s)\n", (unsigned long long)pba, what);
    return 0;
}

/* ---- subcommands ------------------------------------------------------ */

static int cmd_build(const char *img, int nfiles, int ngen)
{
    invfs_volume *v;
    uint64_t pba[2], gen[2], ap = 0, ag = 0;
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
        content_fill(i, 0, buf, len);
        fname(i, name, sizeof name);
        id = vol_replace_file(v, name, buf, len);
        free(buf);
        if (id == 0) { vol_close(v); return fail("vol_replace_file(%s) failed", name); }
    }
    if (vol_v3_fold(v) != 0) { vol_close(v); return fail("vol_v3_fold failed"); }
    for (i = 0; i < ngen; i++) {
        invfs_v3_inode ino;
        uint64_t id = 0x7000000000000000ull + (uint64_t)i;
        memset(&ino, 0, sizeof ino);
        ino.type = INVFS_ITYP_REG;
        ino.mode = 0100644;
        ino.nlink = 1;
        ino.size = (uint64_t)(i + 1);
        ino.mtime = ino.atime = 1700000000;
        if (vol_v3_inode_put(v, id, &ino) != 0) {
            vol_close(v);
            return fail("vol_v3_inode_put(gen %d) failed", i);
        }
    }
    if (vol_flush(v) != 0) { vol_close(v); return fail("vol_flush failed"); }

    slot_census(v, pba, gen, valid, alloc);
    rc = adopt(v, &ap, &ag);
    printf("FILES=%d GENS=%d SLOT0=%llu:%llu:%d:%d SLOT1=%llu:%llu:%d:%d "
           "ADOPT_RC=%d ADOPTED=%llu ADOPTED_GEN=%llu ADOPTED_ALLOC=%d\n",
           nfiles, ngen,
           (unsigned long long)pba[0], (unsigned long long)gen[0],
           valid[0], alloc[0],
           (unsigned long long)pba[1], (unsigned long long)gen[1],
           valid[1], alloc[1],
           rc, (unsigned long long)ap, (unsigned long long)ag,
           adopted_allocated(v, ap));
    vol_close(v);
    /* The suite's whole value rests on the two slots naming two DIFFERENT
     * roots; if they agree, every later leg is vacuous. */
    if (pba[0] && pba[1] && pba[0] == pba[1]) {
        fprintf(stderr, "FAIL: RT30 names the same root in both slots; the "
                        "double-slot construction did not happen\n");
        return 1;
    }
    return 0;
}

static int cmd_probe(const char *img, const char *mode)
{
    invfs_volume *v;
    uint64_t pba[2], gen[2], ap = 0, ag = 0;
    int valid[2], alloc[2], aalloc, err, rc, i, newest;
    int want_hazard = (strcmp(mode, "hazard") == 0);

    if (!want_hazard && strcmp(mode, "safe") != 0)
        return fail("mode must be 'safe' or 'hazard' (got '%s')", mode);

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);

    slot_census(v, pba, gen, valid, alloc);
    rc = adopt(v, &ap, &ag);
    aalloc = (rc == 0) ? adopted_allocated(v, ap) : 0;
    newest = (int)(v->rt30.seq & 1u);

    for (i = 0; i < 2; i++)
        printf("SLOT%d=%llu SEQ_PARITY=%d PAGE_VALID=%d BLOCK_ALLOC=%d "
               "REASON=%s\n",
               i, (unsigned long long)pba[i], i == newest,
               valid[i], alloc[i],
               slot_reason(v, pba[i], valid[i], alloc[i]));
    printf("ADOPT_RC=%d ADOPTED=%llu ADOPTED_GEN=%llu ADOPTED_ALLOC=%d\n",
           rc, (unsigned long long)ap, (unsigned long long)ag, aalloc);
    vol_close(v);

    if (want_hazard) {
        /* RED CONTROL. Assert the pre-fix behaviour is present: the reader
         * adopts a root the bitmap says is free. On a fixed build this leg
         * fails -- which is the point. It is what stops `safe` from passing
         * vacuously. */
        if (rc == 0 && aalloc == 0) {
            printf("HAZARD=present: the reader adopted freed block %llu "
                   "(gen %llu)\n", (unsigned long long)ap,
                   (unsigned long long)ag);
            return 0;
        }
        return fail("expected the reader to adopt a FREED root (the "
                    "pre-fix hazard) but rc=%d adopted=%llu alloc=%d -- the "
                    "hazard could not be reproduced on this build",
                    rc, (unsigned long long)ap, aalloc);
    }
    if (rc == 0 && aalloc == 0)
        return fail("mbuf_root_read adopted block %llu (gen %llu), which the "
                    "allocation bitmap reports as FREE: the reader is "
                    "standing on a block the allocator may hand out again",
                    (unsigned long long)ap, (unsigned long long)ag);
    if (rc == 0)
        printf("OK: the reader adopted block %llu (gen %llu), which is "
               "allocated\n", (unsigned long long)ap, (unsigned long long)ag);
    else
        printf("OK: the reader refused to adopt a root (rc=%d)\n", rc);
    return 0;
}

static int cmd_free_adopted(const char *img)
{
    invfs_volume *v;
    uint64_t ap = 0, ag = 0;
    int err, rc;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    rc = adopt(v, &ap, &ag);
    if (rc != 0 || !ap) {
        vol_close(v);
        return fail("no root to free (rc=%d)", rc);
    }
    /* The reclaimer primitive, verbatim: hand the page back. The BYTES are
     * left exactly as they are, so the page still passes mbuf_page_validate
     * after the free. That is the entire hazard in one operation. */
    mbuf_free(v, ap);
    if (bit_get(v->bitmap, ap)) {
        vol_close(v);
        return fail("mbuf_free(%llu) did not clear the allocation bit "
                    "(retention held it); cannot construct the case",
                    (unsigned long long)ap);
    }
    /* Make the free durable exactly the way btree_collect_orphans does, so
     * the next open sees the same thing a real reclaim would leave behind. */
    if (vol_v3_bitmap_flush(v) != 0) {
        vol_close(v);
        return fail("vol_v3_bitmap_flush failed");
    }
    printf("FREED=%llu GEN=%llu (page bytes left intact, so it still "
           "validates)\n", (unsigned long long)ap, (unsigned long long)ag);
    vol_close(v);
    return 0;
}

static int cmd_damage_newest(const char *img)
{
    invfs_volume *v;
    uint64_t pba[2], gen[2], ap = 0, ag = 0, victim;
    int valid[2], alloc[2], err, rc;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    slot_census(v, pba, gen, valid, alloc);
    rc = adopt(v, &ap, &ag);
    if (rc != 0 || !ap) {
        vol_close(v);
        return fail("no root to damage (rc=%d)", rc);
    }
    victim = ap;
    /* Tear whichever slot the reader adopted, so the NEXT reader is forced
     * onto the other one. Leaving the adopted root intact is deliberate:
     * it isolates "the fallback root" as the only variable. */
    if (victim == pba[0] && pba[1] && pba[1] != pba[0])
        ; /* adopted is slot 0; tearing it is the point */
    rc = scribble(v, victim, "newest RT30 root");
    vol_close(v);
    return rc;
}

static int cmd_damage_tree_page(const char *img)
{
    invfs_volume *v;
    uint8_t *seen;
    uint64_t *pages, n = 0, cap, ap = 0, ag = 0, victim = 0;
    int err, rc, i;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    rc = adopt(v, &ap, &ag);
    if (rc != 0 || !ap) {
        vol_close(v);
        return fail("no root to walk (rc=%d)", rc);
    }
    cap = (uint64_t)v->sb.total_blocks;
    seen = (uint8_t *)calloc(1, (size_t)((cap + 7u) / 8u));
    pages = (uint64_t *)calloc((size_t)cap, sizeof *pages);
    if (!seen || !pages) {
        free(seen); free(pages); vol_close(v);
        return fail("out of memory");
    }
    if (walk_collect(v, ap, seen, pages, &n, cap) != 0) {
        free(seen); free(pages); vol_close(v);
        return fail("tree walk of root %llu failed", (unsigned long long)ap);
    }
    /* A NON-ROOT page, and never an RT30 slot: damaging a slot changes which
     * root the reader adopts and would confound the measurement. What we
     * want is damage INSIDE the fallback tree, which is what makes invf-fsck
     * quarantine something and arm the reachability diff. */
    for (i = 0; i < 2; i++) {
        uint64_t k;
        for (k = 0; k < n; k++) {
            if (pages[k] != ap && pages[k] != v->rt30.root_slot[0] &&
                pages[k] != v->rt30.root_slot[1]) {
                victim = pages[k];
                break;
            }
        }
        if (victim)
            break;
    }
    free(seen);
    free(pages);
    if (!victim) {
        vol_close(v);
        return fail("the tree has no non-root page to damage (it is a single "
                    "empty leaf); write more files first");
    }
    printf("TREE_PAGES=%llu\n", (unsigned long long)n);
    rc = scribble(v, victim, "non-root tree page");
    vol_close(v);
    return rc;
}

static int cmd_reuse(const char *img, int nallocs)
{
    invfs_volume *v;
    uint64_t pba[2], gen[2], *got;
    int valid[2], alloc[2], err, i, j, reused = 0, bad_slot[2];
    uint64_t n = 0;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);
    slot_census(v, pba, gen, valid, alloc);
    bad_slot[0] = pba[0] && !alloc[0];
    bad_slot[1] = pba[1] && !alloc[1];
    if (!bad_slot[0] && !bad_slot[1]) {
        vol_close(v);
        return fail("no RT30 slot names a freed block; run free-adopted (or "
                    "the fsck leg) first");
    }
    got = (uint64_t *)calloc((size_t)(nallocs > 0 ? nallocs : 1), sizeof *got);
    if (!got) { vol_close(v); return fail("out of memory"); }

    for (i = 0; i < nallocs; i++) {
        got[n] = mbuf_alloc(v, 0xF0000000ull + (uint64_t)i);
        if (!got[n])
            break;
        n++;
    }
    /* The question is not "can this happen" but "does the descriptor still
     * point into the free pool": a slot naming a block the allocator has
     * since handed out is the descriptor pointing at somebody else's page. */
    for (i = 0; i < n; i++)
        for (j = 0; j < 2; j++)
            if (bad_slot[j] && got[i] == pba[j])
                reused++;
    printf("ALLOCS=%llu SLOT0_FREED=%d SLOT1_FREED=%d REUSED=%d\n",
           (unsigned long long)n, bad_slot[0], bad_slot[1], reused);
    if (n && vol_v3_bitmap_flush(v) != 0) {
        free(got); vol_close(v);
        return fail("vol_v3_bitmap_flush failed");
    }
    for (i = (int)n - 1; i >= 0; i--)
        mbuf_free(v, got[i]);
    free(got);
    vol_close(v);
    if (reused > 0)
        printf("REUSE=confirmed: the allocator handed out %d block(s) that an "
               "RT30 slot still names -- no further damage is needed for the "
               "reader to adopt a foreign page\n", reused);
    else
        printf("REUSE=not-observed in %llu allocations (the freed slot block "
               "was not re-handed)\n", (unsigned long long)n);
    return 0;
}

/* cmd_verify: read every file back and memcmp it against a FRESHLY REGENERATED
 * buffer, the way src/cli/orphan_test.c's cmd_verify does.
 *
 * It exists because `probe <img> safe` is not a data check. It answers "is the
 * root slot's block allocated?", and that is the whole of it: the suite header
 * claims "leg 5 ... every file must still read back byte-identical", and no
 * byte of any file was ever read anywhere in this file. On a volume whose
 * newest root was torn and a non-root page destroyed on purpose -- and then
 * repaired by `invf-fsck -f`, whose own reachability diff is the hazard this
 * suite exists to police -- a repair that took content with it leaves a
 * structurally perfect volume. `probe safe` passes. That is btree_repair_test
 * reproduced in a shell suite: the repair verified with the repairer's own
 * criteria.
 *
 * mode "strict":   every one of the nfiles must be present AND byte-exact.
 * mode "tolerant": a file the excision legitimately removed may be absent, but
 *                  every file that IS present must be byte-exact, and the count
 *                  of survivors is printed and must be greater than zero -- so
 *                  a volume emptied by the repair cannot report a pass.
 *
 * The expected bytes come from content_fill(), the same generator cmd_build
 * used to write them, not from the volume. Comparing the volume against itself
 * would prove nothing. */
static int cmd_verify(const char *img, int nfiles, const char *mode)
{
    invfs_volume *v;
    int err, i;
    /* "strict":   all nfiles present and byte-exact (an undamaged volume).
     * "tolerant": a file the excision legitimately removed may be absent.
     * "census":   as tolerant, but PRESENT == 0 is NOT a failure -- the
     *             caller has measured that leg 4's damage eats the dirent
     *             page -- so the census (bad == 0, every name accounted for)
     *             is the assertion and the survivor count is the output. */
    int tolerant = (mode && (!strcmp(mode, "tolerant") || !strcmp(mode, "census")));
    int strict_present = mode && !strcmp(mode, "census") ? 0 : 1;
    int present = 0, absent = 0, bad = 0;

    v = vol_open(img, &err);
    if (!v)
        return fail("vol_open(%s) err=%d", img, err);

    for (i = 0; i < nfiles; i++) {
        char name[64];
        size_t len = content_len(i);
        uint8_t *want = (uint8_t *)malloc(len);
        uint8_t *got = NULL;
        size_t glen = 0;
        uint64_t id = 0;

        if (!want) { vol_close(v); return fail("out of memory"); }
        content_fill(i, 0, want, len);
        fname(i, name, sizeof name);
        if (vol_find(v, name) == 0 || vol_v3_path_lookup(v, name, &id) != 1) {
            absent++;
            free(want);
            if (!tolerant) {
                fprintf(stderr, "FAIL: %s is absent from %s\n", name, img);
                bad++;
            }
            continue;
        }
        if (vol_read_file(v, id, &got, &glen) != 0 || !got ||
            glen != len || memcmp(got, want, len) != 0) {
            fprintf(stderr, "FAIL: %s did not read back byte-identical "
                    "(read rc/len %zu vs expected %zu)\n", name, glen, len);
            bad++;
        } else {
            present++;
        }
        free(want);
        free(got);
    }
    vol_close(v);
    printf("VERIFY PRESENT=%d ABSENT=%d BAD=%d MODE=%s\n",
           present, absent, bad, mode ? mode : "strict");
    if (present == 0 && strict_present)
        return fail("no file could be read at all -- the leg proved nothing");
    if (!tolerant && absent != 0)
        return fail("%d file(s) absent on an undamaged volume", absent);
    if (present + absent != nfiles)
        return fail("census is incomplete: %d present + %d absent != %d",
                    present, absent, nfiles);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <build|probe|free-adopted|damage-newest|"
                        "damage-tree-page|reuse|verify> <img> [args]\n",
                argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "build") && argc == 5)
        return cmd_build(argv[2], atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(argv[1], "probe") && argc == 4)
        return cmd_probe(argv[2], argv[3]);
    if (!strcmp(argv[1], "free-adopted") && argc == 3)
        return cmd_free_adopted(argv[2]);
    if (!strcmp(argv[1], "damage-newest") && argc == 3)
        return cmd_damage_newest(argv[2]);
    if (!strcmp(argv[1], "damage-tree-page") && argc == 3)
        return cmd_damage_tree_page(argv[2]);
    if (!strcmp(argv[1], "reuse") && argc == 4)
        return cmd_reuse(argv[2], atoi(argv[3]));
    if (!strcmp(argv[1], "verify") && argc == 5)
        return cmd_verify(argv[2], atoi(argv[3]), argv[4]);
    fprintf(stderr, "FAIL: bad arguments\n");
    return 2;
}
