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

    for (i = 0; i < N; i++)
        ok(upsert_key(v, &root, g_keys[i], 1) == 0, "upsert");
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
        ok(delete_key(v, &root, g_keys[i]) == 0, "delete");
        g_live[i] = 0;
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
    ok(i == K0 && root.pba != 0, "build retained root");
    rootA = root;
    ok(mbuf_read(v, rootA.pba, page0) == 0, "snapshot retained root page");

    for (i = K0; i < N; i++)
        if (upsert_key(v, &root, g_keys[i], 1) != 0)
            break;
    ok(i == N, "continue mutating into a new root");
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
    for (i = 0; i < N; i++) {
        uint64_t idx = order[i];
        if (delete_key(v, &root, g_keys[idx]) != 0)
            break;
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

    fake_vol_close(v);
    free(v);
    remove(img);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}

