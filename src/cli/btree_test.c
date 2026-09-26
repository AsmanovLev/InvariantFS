/*
 * btree_test.c -- WP-M3 unit harness for the immutable base B+-tree.
 *
 * Exercises point search, COW upsert, split/merge, ordered scan, CRC/gen
 * verification, the structural check, and the reachability-diff reclaim
 * primitive over a synthetic v3 volume (a real blkio over a scratch file).
 *
 *   invf-btree_test [scratch-dir]
 *
 * No vol_open/mkfs: the v3 write path is not wired yet (WP-M3 scope), so the
 * harness builds the minimum WP-M2 geometry it needs, exactly like
 * metabuf_test.c.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/statvfs.h>

#include "volume_internal.h"
#include "vol_metabuf.h"
#include "vol_btree.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* ---- synthetic volume ------------------------------------------------- */

#define FB_TOTAL        32768u
#define FB_META_START   1u
#define FB_META_BLOCKS  64u
#define FB_RAW_START    65u
#define FB_RAW_BLOCKS   64u
#define FB_SHADOW_START 129u
#define FB_SHADOW_BLOCKS (FB_TOTAL - FB_SHADOW_START)
#define FB_MAPPER_PBA   3u
#define FB_MAPPER_BLOCKS 2u

/* ---- scratch-store accounting ----------------------------------------- */

/* The scratch image is FB_TOTAL blocks, and the N=5000 split/delete case
 * dirties ~26k of them (~103 MiB), so the store has to be able to hold the
 * whole image. The tree cannot tell "the backing store is full" from "the
 * tree is broken": mbuf_write() collapses every store error to -1
 * (vol_metabuf.c:107) and btree_upsert() propagates it verbatim. So the
 * harness has to. Filing a full scratch dir as a broken tree sends the next
 * reader to vol_btree.c instead of to the filesystem that filled up. */
#define SCRATCH_NEED_MIB ((unsigned long long)(FB_TOTAL * INVFS_BLOCK_SIZE) >> 20)

static const char *g_scratch_dir = "/tmp";

/* Free MiB on the scratch filesystem, or -1 if it cannot be determined. */
static long long scratch_avail_mib(const char *dir)
{
    struct statvfs sfs;
    if (statvfs(dir, &sfs) != 0)
        return -1;
    return (long long)(((unsigned long long)sfs.f_bavail * sfs.f_frsize) >> 20);
}

/* An operation the store could not back. The remaining model checks of the
 * case describe a tree that was never fully built, so they are not evidence
 * of anything: abort the case on one clearly attributed failure instead of
 * letting a storage shortfall masquerade as a tree defect. The suite still
 * goes red (failures is incremented) -- a case that could not run has not
 * passed. */
static void op_aborted(const char *what, uint64_t done, uint64_t n)
{
    long long avail = scratch_avail_mib(g_scratch_dir);
    failures++;
    if (avail < 0) {
        printf("  FAIL  %s: aborted at %llu/%llu; free space on the scratch "
               "store is unknown (statvfs(%s) failed), so this is UNDIAGNOSED "
               "-- check the store by hand\n",
               what, (unsigned long long)done, (unsigned long long)n,
               g_scratch_dir);
    } else if ((unsigned long long)avail < SCRATCH_NEED_MIB) {
        printf("  FAIL  %s: aborted at %llu/%llu -- scratch store %s has %lld "
               "MiB free, this test needs %llu MiB. Storage limit, NOT a tree "
               "defect; the rest of this case did not run.\n",
               what, (unsigned long long)done, (unsigned long long)n,
               g_scratch_dir, avail, SCRATCH_NEED_MIB);
    } else {
        printf("  FAIL  %s: aborted at %llu/%llu with %lld MiB free on %s -- "
               "the store is NOT full, so this IS a tree defect\n",
               what, (unsigned long long)done, (unsigned long long)n,
               avail, g_scratch_dir);
    }
}

static int image_make(const char *path, uint64_t blocks)
{
    blkio io;
    if (blkio_open(&io, path, BLKIO_CREATE) != 0)
        return -1;
    if (blkio_chsize(&io, blocks * INVFS_BLOCK_SIZE) != 0) {
        blkio_close(&io);
        return -1;
    }
    blkio_close(&io);
    return 0;
}

static void fake_vol_reset(invfs_volume *v)
{
    uint64_t i, alloc = 0;
    size_t bb = (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;

    memset(v->bitmap, 0, bb);
    for (i = 0; i <= FB_META_START + FB_META_BLOCKS - 1; i++)
        bit_set(v->bitmap, i);
    for (i = 0; i < FB_TOTAL; i++)
        if (bit_get(v->bitmap, i))
            alloc++;
    v->free_blocks = FB_TOTAL - alloc;
    alloc_state_reset(v);
    v->mb_boot_cursor = 0;
    v->mb_boot_end = 0;
    v->mb_alloc_cursor = 0;
    v->mb_alloc_fail_run = 0;
    memset(&v->rt30, 0, sizeof v->rt30);
    memcpy(v->rt30.magic, "RT30", 4);
    v->rt30.version = INVFS_RT30_VERSION;
    v->rt30.page_size = INVFS_V3_PAGE_SIZE_DEFAULT;
    v->rt30_present = 1;
    mbuf_init(v);
}

static int fake_vol_open(invfs_volume *v, const char *path)
{
    memset(v, 0, sizeof *v);
    v->path = strdup(path);
    if (!v->path)
        return -1;
    if (blkio_open(&v->io, path, 0) != 0)
        return -1;
    v->io_open[0] = 1;
    v->ndev = 1;
    v->dev0_present = 1;

    v->sb.total_blocks = FB_TOTAL;
    v->sb.block_size = INVFS_BLOCK_SIZE;
    v->sb.metadata_zone_start = FB_META_START;
    v->sb.metadata_zone_blocks = FB_META_BLOCKS;
    v->sb.raw_zone_start = FB_RAW_START;
    v->sb.raw_zone_blocks = FB_RAW_BLOCKS;
    v->sb.shadow_zone_start = FB_SHADOW_START;
    v->sb.shadow_zone_blocks = FB_SHADOW_BLOCKS;
    v->sb.meta_mapper_pba = FB_MAPPER_PBA;
    v->sb.meta_mapper_blocks = FB_MAPPER_BLOCKS;

    v->bitmap_blocks = (FB_TOTAL / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    v->bitmap = (uint8_t *)calloc(1, (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE);
    if (!v->bitmap)
        return -1;
    fake_vol_reset(v);
    return 0;
}

static void fake_vol_close(invfs_volume *v)
{
    blkio_close(&v->io);
    free(v->bitmap);
    free(v->meta_type_bitmap);
    free(v->path);
    memset(v, 0, sizeof *v);
}

static uint64_t bits_set(const uint8_t *bm, uint64_t n)
{
    uint64_t i, c = 0;
    for (i = 0; i < n; i++)
        if (bit_get(bm, i))
            c++;
    return c;
}

/* ---- keys / values ---------------------------------------------------- */

#define KEY_LEN 48
#define MAXN    20000

static uint64_t g_keys[MAXN];       /* numeric key ids (0..N-1 permuted) */
static uint32_t g_ver[MAXN];        /* current version per key id */
static uint8_t  g_live[MAXN];       /* 1 = insert()ed, not deleted */

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;

static uint64_t rnd(void)
{
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

static void key_bytes(uint64_t v, uint8_t *out)
{
    int i;
    for (i = 0; i < 8; i++)
        out[i] = (uint8_t)(v >> (56 - 8 * i));
    for (i = 8; i < KEY_LEN; i++)
        out[i] = 0xA5;
}

static uint16_t val_bytes(uint64_t key, uint32_t ver, uint8_t *out)
{
    uint16_t len = (uint16_t)(1 + (key % 24));
    uint16_t i;
    for (i = 0; i < len; i++)
        out[i] = (uint8_t)(key * 31u + ver * 7u + i * 13u);
    return len;
}

static bt_key K(uint64_t v, uint8_t *buf)
{
    bt_key k;
    key_bytes(v, buf);
    k.p = buf;
    k.n = KEY_LEN;
    return k;
}

static void make_perm(uint64_t *out, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        out[i] = i;
    for (i = n; i > 1; i--) {
        uint64_t j = rnd() % i;
        uint64_t t = out[i - 1];
        out[i - 1] = out[j];
        out[j] = t;
    }
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Search `key` and compare the returned value against the current version. */
static int expect_get(invfs_volume *v, invfs_blkptr root, uint64_t key,
                      uint32_t ver, int want_found)
{
    uint8_t kb[KEY_LEN];
    uint8_t want[32];
    uint16_t wl;
    bt_val val;
    int found = -1;
    int rc;

    rc = btree_search(v, root, K(key, kb), &val, &found);
    if (rc != 0)
        return -1;
    if (!want_found)
        return found ? -1 : 0;
    if (!found)
        return -1;
    wl = val_bytes(key, ver, want);
    if (val.n != wl || memcmp(val.p, want, wl) != 0)
        return -1;
    return 0;
}

static int upsert_key(invfs_volume *v, invfs_blkptr *root, uint64_t key,
                      uint32_t ver)
{
    uint8_t kb[KEY_LEN];
    uint8_t vb[32];
    uint16_t vl = val_bytes(key, ver, vb);
    bt_val val;
    val.p = vb;
    val.n = vl;
    return btree_upsert(v, *root, K(key, kb), val, root);
}

static int delete_key(invfs_volume *v, invfs_blkptr *root, uint64_t key)
{
    uint8_t kb[KEY_LEN];
    return btree_delete(v, *root, K(key, kb), root);
}

/* ---- scan model ------------------------------------------------------- */

typedef struct {
    uint64_t *keys;
    int n;
    int expect_lo;      /* range bounds as key ids; 0 = unbounded */
    int expect_hi;
} scan_acc;

static int scan_cb(void *ctx, bt_key k, bt_val val)
{
    scan_acc *a = (scan_acc *)ctx;
    uint64_t v = 0;
    uint8_t want[32];
    uint16_t wl;
    int i;
    (void)val;
    for (i = 0; i < 8; i++)
        v = (v << 8) | k.p[i];
    a->keys[a->n++] = v;
    /* the callback value must match the model's current version */
    wl = val_bytes(v, g_ver[v], want);
    if (val.n != wl || memcmp(val.p, want, wl) != 0) {
        failures++;
        printf("  FAIL  scan value mismatch key=%llu\n",
               (unsigned long long)v);
    }
    checks++;
    return 0;
}

static int check_scan_range(invfs_volume *v, invfs_blkptr root,
                            int want_lo, int want_hi)
{
    uint64_t lo = (uint64_t)want_lo, hi = (uint64_t)want_hi;
    uint8_t lob[KEY_LEN], hib[KEY_LEN];
    uint64_t got[MAXN];
    uint64_t model[MAXN];
    int gn = 0, mn = 0, i;
    scan_acc acc;
    bt_key kl, kh;

    acc.keys = got;
    acc.n = 0;
    acc.expect_lo = want_lo;
    acc.expect_hi = want_hi;

    if (want_lo == 0 && want_hi == 0) {
        kl.p = NULL; kl.n = 0;
        kh.p = NULL; kh.n = 0;
    } else {
        kl = K(lo, lob);
        kh = K(hi, hib);
    }
    if (btree_scan(v, root, kl, kh, scan_cb, &acc) != 0)
        return -1;
    gn = acc.n;

    for (i = 0; i < (int)MAXN; i++) {
        if (!g_live[i])
            continue;
        if (want_lo == 0 && want_hi == 0)
            model[mn++] = g_keys[i];
        else if (g_keys[i] >= lo && g_keys[i] < hi)
            model[mn++] = g_keys[i];
    }
    qsort(model, (size_t)mn, sizeof model[0], cmp_u64);
    if (gn != mn)
        return -1;
    for (i = 0; i < mn; i++)
        if (got[i] != model[i])
            return -1;
    return 0;
}

static invfs_blkptr empty_root(void)
{
    invfs_blkptr r;
    memset(&r, 0, sizeof r);
    return r;
}

/* ---- WP88: a leaf that needs THREE pages, not two ----------------------
 *
 * bt_split_point() can only cut an overflowing node in two, and for a
 * variable-width run there are orderings with NO cut index at which both
 * halves fit a 4 KiB page. The production trigger was a v3 recipe leaf: a
 * 3072-byte Q2R3 recipe chunk (36-byte chunk key) between narrower recipe
 * blobs (33-byte keys). Encoded, that leaf is
 *
 *   309 + 693 + 85 + 3112 + 693 + 693 + 245 + 85 + 20 = 5935 bytes
 *
 * and every cut leaves an overfull half: s=3 -> right 4848, s=4 -> left 4219.
 * bt_ins_rec() then returned -1, vol_v3_recipe_store() failed, vol_write_commit()
 * failed, and the containerpack sweep reported "member inode failed" and
 * abandoned the decomposition of the whole container (WP88 Bug B).
 *
 * The keys and value widths below are byte-for-byte that leaf, so this test
 * fails on any tree whose split cannot produce three page-fitting pieces. */
#define RKEY_LEN   33   /* 1 prefix byte + 32-byte recipe address */
#define CKEY_LEN   36   /* RKEY_LEN + 3-byte chunk index (continuation) */
#define RKEY_RECIPE_PREFIX 0x21

static void rkey_bytes(uint8_t *out, uint8_t nbytes, uint32_t addr)
{
    int i;
    out[0] = RKEY_RECIPE_PREFIX;
    for (i = 0; i < 4; i++)
        out[1 + i] = (uint8_t)(addr >> (24 - 8 * i));
    for (i = 5; i < RKEY_LEN; i++)
        out[i] = 0x5A;
    /* the chunk key continues its recipe key, so it sorts immediately after */
    for (i = RKEY_LEN; i < nbytes; i++)
        out[i] = 0x00;
}

static void test_wide_records(invfs_volume *v)
{
    /* addr order, then value width -- the 3072-byte chunk sits at index 3 */
    static const uint32_t addrs[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const uint16_t vlen[8] = { 272, 656, 48, 3072, 656, 656, 208, 48 };
    /* insertion order: the seven narrow recipes land in one leaf (2823 bytes),
     * THEN the wide chunk is inserted into the middle of it. That is the
     * production order -- a leaf that already held recipes overflows on the
     * Q2R3 chunk -- and it is the only order that reaches the unsplittable
     * case (insert the chunk first and the node splits cleanly in two). */
    static const int order[8] = { 0, 1, 2, 4, 5, 6, 7, 3 };
    static uint8_t vbuf[8][3072];
    invfs_blkptr root = empty_root();
    bt_stat st;
    int i, all_ok = 1;

    printf("wide records: a leaf that needs a 3-way split\n");
    for (i = 0; i < 8; i++) {
        uint16_t j;

        for (j = 0; j < vlen[i]; j++)
            vbuf[i][j] = (uint8_t)(addrs[i] * 7u + j);
    }
    for (i = 0; i < 8; i++) {
        int r = order[i];
        uint8_t kb[CKEY_LEN];
        uint16_t n = (r == 3) ? CKEY_LEN : RKEY_LEN;
        bt_key k;
        bt_val val;

        rkey_bytes(kb, n, addrs[r]);
        k.p = kb;
        k.n = n;
        val.p = vbuf[r];
        val.n = vlen[r];
        if (btree_upsert(v, root, k, val, &root) != 0)
            all_ok = 0;
    }
    ok(all_ok, "every upsert succeeds (no unsplittable leaf)");
    ok(btree_check(v, root, &st, NULL, 0) == 0, "structural check");
    ok(st.nkeys == 8, "all 8 records are reachable from the root");
    for (i = 0; i < 8; i++) {
        uint8_t kb[CKEY_LEN];
        uint16_t n = (i == 3) ? CKEY_LEN : RKEY_LEN;
        bt_key k;
        bt_val val;
        int found = -1;
        uint16_t j, bad = 0;

        rkey_bytes(kb, n, addrs[i]);
        k.p = kb;
        k.n = n;
        if (btree_search(v, root, k, &val, &found) != 0 || !found ||
            val.n != vlen[i]) {
            ok(0, "wide record reads back");
            continue;
        }
        for (j = 0; j < vlen[i]; j++)
            if (val.p[j] != vbuf[i][j])
                bad = 1;
        ok(!bad, "wide record reads back byte-for-byte");
    }
    /* the tree must still be a legal B+-tree, i.e. no page is overfull */
    ok(st.height >= 1, "a root page was created for the three pieces");
}

/* ---- WP89: the SAME three-way split, one level up (into a parent) -------
 *
 * test_wide_records pins the child half of the three-way split: a LEAF that
 * needs three pages. Its tree stays one level deep, so the parent splice in
 * bt_ins_rec -- the branch that has to make room for TWO new separators in
 * an already-split internal page -- never runs.
 *
 * The production trigger was exactly one level up. The sweep's dedupe pass
 * stores a fresh recipe per merged file, so the v3 base tree's root is an
 * internal page with a dozen-odd leaf children; one of those leaves then
 * three-way splits under a wide Q2R3 chunk and the splice ran. The loop
 *
 *     for (j = n; j > i + add; j--) e[j] = e[j - 1];
 *
 * is the two-way loop with only its bound generalised, so it moves the
 * parent's tail up by ONE slot; with add == 2 the topmost destination
 * (n + add - 1) was never written and kept uninitialised malloc memory. The
 * page went out as a parent whose LAST child was null: its own CRC and gen
 * check out, so nothing downstream could tell -- until the next
 * btree_search for a key past that separator walked into the hole and
 * returned -1. That is the "vol_v3_recipe_store failed" in the qcow2 e2e's
 * dedupe pass, the batch flush that followed it, and the non-zero sweep
 * exit (WP89).
 *
 * The shape below reproduces it deterministically. A narrow recipe encodes to
 * 337 B (37 B of key/len framing + a 300 B value), so 24 of them fill a
 * dozen-record leaf and split it: the root grows into an internal page with
 * several leaf children. A wide Q2R3 chunk encodes to 3112 B (36 B key +
 * 3072 B value), and each chunk is addressed to sort into the middle of a
 * leaf that already holds records on both sides. The leaf it lands in ends
 * up as 4 narrow + chunk + 3 narrow = 5491 B in 8 records, and every cut
 * index leaves an overfull half (s=5 -> left 4480; s=4 -> right 4143; s=6
 * -> left 4817; s=7 -> left 5154), so bt_split_point returns -1,
 * bt_split3_point cuts it 4/3/1, and the internal page has to splice TWO
 * separators. */
#define WI_RECIPES 24          /* 24 * 337 B: the root leaf splits       */
#define WI_NARROW  300u        /* value width of a narrow recipe         */
#define WI_WIDE    3072u       /* value width of a Q2R3 recipe chunk     */
#define WI_CHUNKS  3           /* wide chunks, spread over the leaves    */

static void test_wide_split_internal(invfs_volume *v)
{
    invfs_blkptr root = empty_root();
    static uint8_t nbuf[WI_RECIPES][WI_NARROW];
    static uint8_t wbuf[WI_CHUNKS][WI_WIDE];
    uint8_t kb[CKEY_LEN];
    bt_stat st;
    char err[128];
    int i, upserts_ok = 1, nread = 0;

    printf("wide records: a 3-way split spliced into an internal page\n");
    for (i = 0; i < WI_RECIPES; i++) {
        uint16_t j;
        for (j = 0; j < WI_NARROW; j++)
            nbuf[i][j] = (uint8_t)((i + 1) * 11u + j);
    }
    for (i = 0; i < WI_CHUNKS; i++) {
        uint16_t j;
        for (j = 0; j < WI_WIDE; j++)
            wbuf[i][j] = (uint8_t)((i + 1) * 31u + j);
    }
    /* the narrow recipes first: the root leaf overflows and the tree grows
     * an internal root with several leaf children */
    for (i = 0; i < WI_RECIPES; i++) {
        bt_key k;
        bt_val val;
        rkey_bytes(kb, RKEY_LEN, (uint32_t)(i + 1));
        k.p = kb;
        k.n = RKEY_LEN;
        val.p = nbuf[i];
        val.n = WI_NARROW;
        if (btree_upsert(v, root, k, val, &root) != 0)
            upserts_ok = 0;
    }
    ok(upserts_ok, "every narrow upsert succeeds");
    ok(btree_check(v, root, &st, NULL, 0) == 0, "structural check before");
    ok(st.height >= 2, "the root is an internal page (the splice has a parent)");

    /* then one wide chunk per second leaf, addressed so it sorts into the
     * middle of a leaf that already holds records on both sides. At least
     * one of the three lands mid-leaf; that one forces the three-way split
     * the parent has to splice. */
    upserts_ok = 1;
    for (i = 0; i < WI_CHUNKS; i++) {
        bt_key k;
        bt_val val;
        rkey_bytes(kb, CKEY_LEN, (uint32_t)(4 + i * 7));
        k.p = kb;
        k.n = CKEY_LEN;
        val.p = wbuf[i];
        val.n = WI_WIDE;
        if (btree_upsert(v, root, k, val, &root) != 0)
            upserts_ok = 0;
    }
    ok(upserts_ok, "every wide upsert succeeds (no lost separator)");

    /* The invariant: after an internal page takes a three-way split it still
     * names every child it held plus the two new pieces, so the whole key
     * space is reachable and the tree is still a legal B+-tree. A splice that
     * shifts the tail by one leaves a null child in the published page: the
     * page CRC and gen are valid, so only the reachability count and a search
     * past the hole can see it. */
    err[0] = 0;
    ok(btree_check(v, root, &st, err, sizeof err) == 0,
       "structural check after the splices");
    if (err[0])
        printf("        btree_check: %s\n", err);
    ok(st.nkeys == WI_RECIPES + WI_CHUNKS,
       "every record is reachable from the root after the splices");

    for (i = 0; i < WI_RECIPES; i++) {
        bt_key k;
        bt_val val;
        int found = -1;
        uint16_t j, bad = 0;

        rkey_bytes(kb, RKEY_LEN, (uint32_t)(i + 1));
        k.p = kb;
        k.n = RKEY_LEN;
        if (btree_search(v, root, k, &val, &found) != 0 || !found ||
            val.n != WI_NARROW) {
            ok(0, "narrow recipe survives the splices");
            continue;
        }
        for (j = 0; j < WI_NARROW; j++)
            if (val.p[j] != nbuf[i][j])
                bad = 1;
        if (bad)
            ok(0, "narrow recipe survives the splices");
        else
            nread++;
    }
    ok(nread == WI_RECIPES, "every narrow recipe reads back byte-for-byte");

    nread = 0;
    for (i = 0; i < WI_CHUNKS; i++) {
        bt_key k;
        bt_val val;
        int found = -1;
        uint16_t j, bad = 0;

        rkey_bytes(kb, CKEY_LEN, (uint32_t)(4 + i * 7));
        k.p = kb;
        k.n = CKEY_LEN;
        if (btree_search(v, root, k, &val, &found) != 0 || !found ||
            val.n != WI_WIDE) {
            ok(0, "wide chunk survives the splices");
            continue;
        }
        for (j = 0; j < WI_WIDE; j++)
            if (val.p[j] != wbuf[i][j])
                bad = 1;
        if (bad)
            ok(0, "wide chunk survives the splices");
        else
            nread++;
    }
    ok(nread == WI_CHUNKS, "every wide chunk reads back byte-for-byte");
}

static int verify_model(invfs_volume *v, invfs_blkptr root, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++) {
        if (!g_live[i])
            continue;
        if (expect_get(v, root, g_keys[i], g_ver[i], 1) != 0)
            return -1;
    }
    return 0;
}

static void reset_model(uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++) {
        g_ver[i] = 1;
        g_live[i] = 1;
    }
    make_perm(g_keys, n);
}

/* ---- tests ------------------------------------------------------------ */

static void test_empty(invfs_volume *v)
{
    invfs_blkptr root = empty_root(), nr;
    bt_stat st;
    uint8_t kb[KEY_LEN];
    int found = 1;

    printf("empty tree\n");
    ok(btree_search(v, root, K(1, kb), NULL, &found) == 0 && found == 0,
       "search on empty root reports not-found");
    ok(check_scan_range(v, root, 0, 0) == 0, "scan on empty root is empty");
    ok(btree_delete(v, root, K(1, kb), &nr) == 0 && nr.pba == 0,
       "delete on empty root stays empty");
    ok(btree_check(v, root, &st, NULL, 0) == 0 && st.n_pages == 0 &&
       st.height == 0, "structural check on empty root");
}

static void test_basic(invfs_volume *v)
{
    const uint64_t N = 200;
    invfs_blkptr root = empty_root();
    bt_stat st;
    uint64_t i;
    uint8_t vb[32];
    uint16_t vl;
    bt_val val;

    printf("insert / search / duplicate / delete (N=%llu)\n",
           (unsigned long long)N);
    reset_model(N);

    /* One check per operation, as before: a store that refuses a write must
     * not turn the remaining N-i inserts into phantom failures, but the
     * per-operation accounting is real coverage and stays. */
    for (i = 0; i < N; i++) {
        if (upsert_key(v, &root, g_keys[i], 1) != 0) {
            op_aborted("insert", i, N);
            return;
        }
        ok(1, "upsert");
    }
    ok(verify_model(v, root, N) == 0, "every inserted key is found with its value");
    ok(btree_check(v, root, &st, NULL, 0) == 0, "structural check after inserts");
    ok(st.nkeys == N, "nkeys == N");
    ok(check_scan_range(v, root, 0, 0) == 0, "full scan equals sorted model");

    /* duplicate-key upsert replaces in place (key count unchanged) */
    vl = val_bytes(g_keys[42], 2, vb);
    val.p = vb;
    val.n = vl;
    {
        uint8_t kb[KEY_LEN];
        ok(btree_upsert(v, root, K(g_keys[42], kb), val, &root) == 0,
           "duplicate-key upsert");
    }
    g_ver[42] = 2;
    ok(expect_get(v, root, g_keys[42], 2, 1) == 0, "replaced value is returned");
    ok(btree_check(v, root, &st, NULL, 0) == 0 && st.nkeys == N,
       "replace does not change nkeys");

    for (i = 0; i < N; i++) {
        if (delete_key(v, &root, g_keys[i]) != 0) {
            op_aborted("delete", i, N);
            return;
        }
        g_live[i] = 0;
        ok(1, "delete");
    }
    ok(verify_model(v, root, N) == 0, "all deleted keys are gone");
    ok(btree_check(v, root, &st, NULL, 0) == 0 && st.nkeys == 0,
       "delete-all empties the tree");
    ok(root.pba == 0, "empty root collapses to pba 0");
}

static void test_cow(invfs_volume *v)
{
    const uint64_t N = 1600, K0 = 800;
    invfs_blkptr root = empty_root(), rootA, rootB;
    uint8_t page0[INVFS_BLOCK_SIZE], page1[INVFS_BLOCK_SIZE];
    uint64_t i;
    bt_stat st;

    printf("COW sharing / retained root (N=%llu, retain=%llu)\n",
           (unsigned long long)N, (unsigned long long)K0);
    reset_model(N);

    for (i = 0; i < K0; i++)
        if (upsert_key(v, &root, g_keys[i], 1) != 0)
            break;
    if (i != K0) {
        op_aborted("build retained root", i, K0);
        return;
    }
    ok(root.pba != 0, "build retained root");
    rootA = root;
    ok(mbuf_read(v, rootA.pba, page0) == 0, "snapshot retained root page");

    for (i = K0; i < N; i++)
        if (upsert_key(v, &root, g_keys[i], 1) != 0)
            break;
    if (i != N) {
        op_aborted("continue mutating into a new root", i - K0, N - K0);
        return;
    }
    ok(1, "continue mutating into a new root");
    rootB = root;

    for (i = 0; i < K0; i++) {
        if (expect_get(v, rootA, g_keys[i], 1, 1) != 0)
            break;
    }
    ok(i == K0, "retained root still reads its old values");

    {
        int extra_absent = 1;
        for (i = K0; i < N; i++) {
            if (expect_get(v, rootA, g_keys[i], 1, 0) != 0) {
                extra_absent = 0;
                break;
            }
        }
        ok(extra_absent, "retained root does not see later mutations");
    }
    ok(verify_model(v, rootB, N) == 0, "new root sees every key");

    ok(mbuf_read(v, rootA.pba, page1) == 0 &&
       memcmp(page0, page1, INVFS_BLOCK_SIZE) == 0,
       "retained root page is byte-identical after later upserts");
    ok(btree_check(v, rootA, &st, NULL, 0) == 0, "retained root is structurally valid");
    ok(btree_check(v, rootB, &st, NULL, 0) == 0, "new root is structurally valid");
    ok(check_scan_range(v, rootB, 0, 0) == 0, "new-root scan equals sorted model");
}

static void test_split_merge(invfs_volume *v)
{
    const uint64_t N = 5000;
    invfs_blkptr root = empty_root();
    bt_stat st1, st2;
    uint64_t i;
    uint64_t *order;

    printf("split (height>=3) and merge on delete (N=%llu)\n",
           (unsigned long long)N);
    reset_model(N);
    for (i = 0; i < N; i++)
        if (upsert_key(v, &root, g_keys[i], 1) != 0)
            break;
    if (i != N) {
        op_aborted("build the split tree", i, N);
        return;
    }
    ok(i == N, "built the split tree");
    ok(btree_check(v, root, &st1, NULL, 0) == 0, "structural check after splits");
    ok(st1.height >= 3, "tree gained at least two internal levels");
    ok(st1.nkeys == N, "all keys present");

    /* scan a random-equivalent subset range */
    {
        uint64_t lo = N / 4, hi = (3 * N) / 4;
        /* find the numeric bounds present; range uses key values 0..N-1 */
        ok(check_scan_range(v, root, (int)lo, (int)hi) == 0,
           "sub-range scan equals model");
    }

    order = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)N);
    if (!order) {
        ok(0, "alloc delete order");
        return;
    }
    for (i = 0; i < N; i++)
        order[i] = i;
    for (i = N; i > 1; i--) {
        uint64_t j = rnd() % i, t = order[i - 1];
        order[i - 1] = order[j];
        order[j] = t;
    }
    /* Three ways out of this loop, and only one of them is a tree finding:
     * delete_key() failing is the store refusing the write, whereas a
     * verify_model()/btree_check() trip is the tree itself misbehaving (it
     * says so on stdout) and must keep flowing into the checks below. */
    {
        int op_err = 0;
        for (i = 0; i < N; i++) {
            uint64_t idx = order[i];
            if (delete_key(v, &root, g_keys[idx]) != 0) {
                op_err = 1;
                break;
            }
            g_live[idx] = 0;
            if ((i % 500) == 0 || i == N - 1) {
                if (verify_model(v, root, N) != 0)
                    break;
                {
                    bt_stat stc;
                    char err[128];
                    if (btree_check(v, root, &stc, err, sizeof err) != 0) {
                        printf("  check failed mid-delete: %s\n", err);
                        break;
                    }
                }
            }
        }
        if (op_err) {
            op_aborted("delete every key in random order", i, N);
            free(order);
            return;
        }
    }
    ok(i == N, "deleted every key in random order");
    ok(verify_model(v, root, N) == 0, "no deleted key survives");
    ok(btree_check(v, root, &st2, NULL, 0) == 0 && st2.nkeys == 0,
       "tree collapses to empty after delete-all");
    ok(root.pba == 0, "root is pba 0 when empty");
    free(order);
}

static void test_crc_gen(invfs_volume *v)
{
    const uint64_t N = 40;
    invfs_blkptr root = empty_root();
    uint8_t page[INVFS_BLOCK_SIZE];
    uint8_t kb[KEY_LEN];
    uint64_t i;
    bt_val val;
    int found;

    printf("CRC / gen verification\n");
    reset_model(N);
    for (i = 0; i < N; i++) {
        uint64_t want = (root.pba ? root.gen : 0) + 1;
        if (upsert_key(v, &root, g_keys[i], 1) != 0)
            break;
        if (root.gen != want) {
            ok(0, "upsert bumps the root gen by one");
            break;
        }
    }
    ok(i == N, "gen is monotone across upserts");
    ok(mbuf_read(v, root.pba, page) == 0 &&
       mbuf_page_hdr(page)->gen == root.gen,
       "page header gen matches the root blkptr gen");

    /* a bad blkptr checksum must be refused, not silently mis-read */
    {
        invfs_blkptr bad = root;
        bad.checksum ^= 1u;
        ok(btree_search(v, bad, K(g_keys[0], kb), &val, &found) != 0,
           "search refuses a bad blkptr CRC");
        bad = root;
        bad.gen += 1;
        ok(btree_search(v, bad, K(g_keys[0], kb), &val, &found) != 0,
           "search refuses a gen mismatch");
    }

    /* a torn page body on disk must be refused by the page CRC */
    {
        uint8_t torn[INVFS_BLOCK_SIZE];
        ok(mbuf_read(v, root.pba, torn) == 0, "read root page");
        torn[100] ^= 0x01;
        ok(blkio_pwrite(&v->io, root.pba * INVFS_BLOCK_SIZE, torn,
                        INVFS_BLOCK_SIZE) == 0,
           "tear the root page on disk");
        ok(btree_search(v, root, K(g_keys[0], kb), &val, &found) != 0,
           "search refuses a torn page");
    }
}

static void test_reclaim(invfs_volume *v)
{
    const uint64_t N = 300;
    invfs_blkptr root = empty_root();
    uint64_t baseline;
    uint64_t i, allocated, d1, d2;
    bt_stat st;
    int freed;

    printf("reachability-diff reclaim (N=%llu)\n", (unsigned long long)N);
    /* Drain WP-M2's two pre-allocated bootstrap pages first, so the baseline
     * below reflects a plain bitmap state and the tree only uses tracked
     * free-space allocations (bootstrap pages are handed out without bitmap
     * bookkeeping by mbuf_alloc). */
    d1 = mbuf_alloc(v, 1);
    d2 = mbuf_alloc(v, 1);
    if (d1)
        mbuf_free(v, d1);
    if (d2)
        mbuf_free(v, d2);
    baseline = bits_set(v->bitmap, v->sb.total_blocks);
    reset_model(N);
    for (i = 0; i < N; i++) {
        invfs_blkptr next = root;
        if (upsert_key(v, &next, g_keys[i], 1) != 0)
            break;
        if (root.pba) {
            freed = btree_reclaim(v, root, next);
            if (freed < 0) {
                ok(0, "per-step reclaim");
                break;
            }
        }
        root = next;
    }
    ok(i == N, "built with per-step reclaim");
    ok(btree_check(v, root, &st, NULL, 0) == 0, "reclaimed tree is valid");
    allocated = bits_set(v->bitmap, v->sb.total_blocks) - baseline;
    ok(allocated == st.n_pages,
       "every allocated metadata page is reachable from the root");

    freed = btree_reclaim(v, root, empty_root());
    ok(freed == (int)st.n_pages, "final reclaim frees exactly the reachable set");
    ok(bits_set(v->bitmap, v->sb.total_blocks) == baseline,
       "reclaim returns the allocator to its baseline");
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    invfs_volume *v;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-btree_test [scratch-dir]\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE,
                    INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    printf("btree tests (WP-M3)\n");

    g_scratch_dir = dir;

    /* Preflight. Without this the split case dies of ENOSPC partway through
     * and reports half a dozen tree-invariant failures that say nothing
     * about the tree (the store, not the tree, ran out). Refuse up front
     * and name the limit. This is a hard failure, NOT a skip: ~4.7k model
     * checks would go unrun, and an unrun check is not a passed check. */
    {
        long long avail = scratch_avail_mib(dir);
        if (avail >= 0 && (unsigned long long)avail < SCRATCH_NEED_MIB) {
            failures++;
            printf("  FAIL  scratch store %s has %lld MiB free; the %llu MiB "
                   "test image does not fit, so no check in this suite can run\n",
                   dir, avail, SCRATCH_NEED_MIB);
            printf("%d checks, %d failure(s)\n", checks, failures);
            return 1;
        }
    }

    snprintf(img, sizeof img, "%s/invf-btree_test.img", dir);
    if (image_make(img, FB_TOTAL) != 0) {
        printf("  cannot create scratch image %s\n", img);
        return 1;
    }
    v = (invfs_volume *)calloc(1, sizeof *v);
    if (!v) {
        printf("  out of memory\n");
        return 1;
    }
    if (fake_vol_open(v, img) != 0) {
        printf("  cannot open scratch volume\n");
        return 1;
    }

    test_empty(v);
    fake_vol_reset(v);
    test_basic(v);
    fake_vol_reset(v);
    test_cow(v);
    fake_vol_reset(v);
    test_split_merge(v);
    fake_vol_reset(v);
    test_crc_gen(v);
    fake_vol_reset(v);
    test_reclaim(v);
    fake_vol_reset(v);
    test_wide_records(v);
    fake_vol_reset(v);
    test_wide_split_internal(v);

    fake_vol_close(v);
    free(v);
    remove(img);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}

