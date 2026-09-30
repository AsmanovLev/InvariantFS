/*
 * pbaref_v3_test.c — WP: pba-ref-v3-incremental
 *
 * MEASUREMENT harness for the pba reference map on Meta-v3, the sole gate
 * on every block free (pba_ref_ensure -> src/core/volume.c:3975, v3 branch
 * at :3986-3987; the gate at src/core/vol_ast.c:177-182).
 *
 * Phases (argv[2]), so the map-rebuild leg and the write/unlink legs can be
 * separate processes (each vol_open rebuilds the map at src/core/volume.c:2091)
 * or all inside one session (the map is then never re-derived):
 *
 *   setup        mkfs, write A + B with a shared 64 KiB segment, dedupe so
 *                both recipes name segment P, record P.
 *   sweep        second sweep (vol_sweep_dedupe_ex)  -- leg (i)
 *   rewrite      vol_v3_write_bulk over A             -- leg (ii)
 *   unlink       vol_v3_unlink(B)                    -- leg (iii)
 *   check        report P's allocation state, dump A for `cmp`
 *   red          (i)(ii)(iii) in ONE session, rewrite KEEPS P
 *   rednosweep   same WITHOUT leg (i)
 *   all / allnosweep  (i)(ii)(iii) in one session, rewrite DROPS P
 *   hookctl      red control for "vol_v3_inode_delta_put has no pba_ref hook"
 *
 * The oracle is always read-back-bytes, never invf-verify --deep: an
 * invfs_ast_block_entry carries a pba, not a content hash, so a recipe that
 * resolves to a valid-but-wrong segment passes a deep verify.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "invarifs.h"
#include "volume_internal.h"

/* The cross-TU fault door. The site is v3_inode_row_read, inside
 * vol_v3_inode_get (src/core/vol_btree.c), and the arming state is that
 * file's TU statics -- so a test in this one must reload them through the
 * door, NOT with unsetenv+setenv. That frees the old string, setenv very
 * often gets the same address back, the pointer compare in invfs_vol_fault
 * sees no change, the countdown stays spent, and the leg goes green
 * measuring the healthy path. */
extern void invfs_vol_btree_fault_reload(void);

#define MAXPB 64

static int checks = 0, failures = 0;
static const char *g_dir = "/tmp";
static char g_img[400];

static void ok(int cond, const char *msg)
{
    checks++;
    printf("  %s  %s\n", cond ? "OK  " : "FAIL", msg);
    if (!cond) failures++;
}

/* ---- the shared segment's pba, persisted between phases ---- */
static void pba_save(uint64_t pba)
{
    char p[400];
    FILE *f;
    snprintf(p, sizeof p, "%s/pbaref_pba.txt", g_dir);
    f = fopen(p, "w");
    if (f) { fprintf(f, "%llu\n", (unsigned long long)pba); fclose(f); }
}

static uint64_t pba_load(void)
{
    char p[400];
    FILE *f;
    unsigned long long x = 0;
    snprintf(p, sizeof p, "%s/pbaref_pba.txt", g_dir);
    f = fopen(p, "r");
    if (!f) return 0;
    if (fscanf(f, "%llu", &x) != 1) x = 0;
    fclose(f);
    return (uint64_t)x;
}

static void save_bytes(const char *tag, const uint8_t *b, size_t n)
{
    char p[400];
    FILE *f;
    snprintf(p, sizeof p, "%s/pbaref_%s.bin", g_dir, tag);
    f = fopen(p, "wb");
    if (f) { fwrite(b, 1, n, f); fclose(f); }
}

/* ---- recipe introspection ---- */

static int recipe_pbas(invfs_volume *v, const char *path,
                       uint64_t *out, int max, int *n_out)
{
    uint64_t id = 0;
    invfs_v3_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n = 0, i;
    int m = 0;

    *n_out = 0;
    if (vol_v3_path_lookup(v, path, &id) != 1) return -1;
    if (vol_v3_inode_get(v, id, &in) != 1) return -1;
    if (in.size == 0) return 0;
    if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob)
        return -1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n) == 0 && ents) {
        for (i = 0; i < n && m < max; i++)
            if (ents[i].zone != INVFS_ZONE_TEXT && ents[i].pba)
                out[m++] = ents[i].pba;
    }
    free(blob);
    *n_out = m;
    return 0;
}

static int a_names_pba(invfs_volume *v, uint64_t pba)
{
    uint64_t pa[MAXPB];
    int na = 0, i;
    if (recipe_pbas(v, "file_a.bin", pa, MAXPB, &na) != 0) return 0;
    for (i = 0; i < na; i++) if (pa[i] == pba) return 1;
    return 0;
}

/* how many of P's own extent blocks are still marked allocated; plen_out
 * receives the extent length (0 when the framed header will not parse) */
static uint64_t extent_allocated(invfs_volume *v, uint64_t pba, uint64_t *plen)
{
    uint64_t plen_ = 0, b, n = 0;
    if (plen) *plen = 0;
    if (!pba) return 0;
    if (seg_extent_checked(v, pba, &plen_) != 0) {
        /* the extent no longer validates (freed, or the header rewritten):
         * fall back to the raw first-block bit so the report still says
         * something true about the bitmap. */
        if (plen) *plen = 1;
        return bit_get(v->bitmap, pba) ? 1 : 0;
    }
    if (plen) *plen = plen_;
    for (b = 0; b < plen_; b++)
        if (bit_get(v->bitmap, pba + b)) n++;
    return n;
}

static void report_pba(invfs_volume *v, const char *label, uint64_t pba)
{
    uint64_t plen = 0;
    uint64_t n;
    if (!pba) { printf("  [%s] P=<unset>\n", label); return; }
    n = extent_allocated(v, pba, &plen);
    printf("  [%s] P=%llu  pba_ref_count=%u  allocated=%llu/%llu  A_names_P=%s\n",
           label, (unsigned long long)pba, pba_ref_count(v, pba),
           (unsigned long long)n, (unsigned long long)plen,
           a_names_pba(v, pba) ? "yes" : "no");
}

static void report_both(invfs_volume *v, const char *label, uint64_t pba)
{
    uint64_t pa[MAXPB], pb[MAXPB];
    int na = 0, nb = 0, i;
    recipe_pbas(v, "file_a.bin", pa, MAXPB, &na);
    recipe_pbas(v, "file_b.bin", pb, MAXPB, &nb);
    printf("  [%s] A names %d pbas:", label, na);
    for (i = 0; i < na; i++) printf(" %llu", (unsigned long long)pa[i]);
    printf("\n  [%s] B names %d pbas:", label, nb);
    for (i = 0; i < nb; i++) printf(" %llu", (unsigned long long)pb[i]);
    printf("\n");
    report_pba(v, label, pba);
}

/* ---- the corpus: A and B share one 64 KiB segment ---- */

static uint8_t *mk_file(int which)
{
    size_t seg_sz = SEGMENT_SIZE, file_sz = 8 * seg_sz;
    uint8_t *d = (uint8_t *)malloc(file_sz);
    size_t i, s;
    assert(d);
    for (s = 0; s < file_sz / seg_sz; s++) {
        for (i = 0; i < seg_sz; i++) {
            uint8_t b = (s == 0)
                ? (uint8_t)((i * 37 + 11) & 0xFF)                  /* shared */
                : (uint8_t)(((s + which) * 101 + i + which * 7) & 0xFF);
            d[s * seg_sz + i] = b;
        }
    }
    return d;
}

/* keep_seg0: segment 0 keeps the ORIGINAL shared pattern, so a rewrite
 * ALIASES the old pba (P) instead of replacing it -- the case where P must
 * survive leg (ii) for leg (iii) to mean anything. */
static uint8_t *mk_file_keep(int which, int keep_seg0)
{
    size_t seg_sz = SEGMENT_SIZE;
    uint8_t *d = mk_file(which);
    if (keep_seg0) {
        size_t i;
        for (i = 0; i < seg_sz; i++)
            d[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    }
    return d;
}

static uint64_t find_shared(invfs_volume *v)
{
    uint64_t pa[MAXPB], pb[MAXPB];
    int na = 0, nb = 0, i, j;
    if (recipe_pbas(v, "file_a.bin", pa, MAXPB, &na) != 0) return 0;
    if (recipe_pbas(v, "file_b.bin", pb, MAXPB, &nb) != 0) return 0;
    for (i = 0; i < na; i++)
        for (j = 0; j < nb; j++)
            if (pa[i] == pb[j]) return pa[i];
    return 0;
}

static invfs_volume *open_vol(void)
{
    int err = 0;
    invfs_volume *v = vol_open(g_img, &err);
    if (!v) { fprintf(stderr, "vol_open(%s) failed: %d\n", g_img, err); exit(2); }
    return v;
}

static void mkfs_fresh(void)
{
    char cmd[600];
    unlink(g_img);
    snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 64 >/dev/null 2>&1", g_img);
    if (system(cmd) != 0) { fprintf(stderr, "mkfs failed\n"); exit(2); }
}

/* THE bit-exactness oracle: read back through the normal read path and
 * compare BYTES. */
static int oracle(invfs_volume *v, const char *name, const uint8_t *want,
                  size_t want_len, const char *tag)
{
    uint64_t id = 0;
    uint8_t *buf = NULL;
    size_t len = 0, i, firstbad = 0;
    int rc, same;

    if (vol_v3_path_lookup(v, name, &id) != 1) {
        printf("  [oracle %s] %s: NAME IS GONE\n", tag, name);
        ok(0, tag);
        return 0;
    }
    rc = vol_read_inode(v, id, 0, &buf, &len);
    if (rc != 0) {
        printf("  [oracle %s] %s: READ FAILED (rc=%d)\n", tag, name, rc);
        ok(0, tag);
        return 0;
    }
    same = (len == want_len) && memcmp(buf, want, len) == 0;
    if (same) {
        printf("  [oracle %s] %s: %zu bytes BYTE-EXACT vs source\n", tag, name, len);
    } else {
        for (i = 0; i < len && i < want_len; i++)
            if (buf[i] != want[i]) { firstbad = i; break; }
        printf("  [oracle %s] %s: *** NOT byte-exact *** len=%zu want=%zu "
               "first differing byte %zu (got 0x%02x want 0x%02x)\n",
               tag, name, len, want_len, firstbad,
               firstbad < len ? buf[firstbad] : 0,
               firstbad < want_len ? want[firstbad] : 0);
    }
    ok(same, tag);
    free(buf);
    return same;
}

/* ---- legs ---- */

static void leg_sweep(invfs_volume *v)
{
    invfs_dedupe_stats ds;
    int n = vol_sweep_dedupe_ex(v, &ds, NULL, NULL);
    printf("  [sweep] vol_sweep_dedupe_ex -> %d, segments_merged=%llu, blocks_freed=%llu\n",
           n, (unsigned long long)ds.segments_merged,
           (unsigned long long)ds.blocks_freed);
}

static uint8_t *leg_rewrite(invfs_volume *v, int keep_seg0)
{
    size_t file_sz = 8 * SEGMENT_SIZE;
    uint8_t *d = mk_file_keep(2, keep_seg0);
    uint64_t id = vol_v3_write_bulk(v, "file_a.bin", d, file_sz, NULL);
    ok(id != 0, "leg (ii): vol_v3_write_bulk rewrote file_a.bin");
    save_bytes("a_expect", d, file_sz);
    return d;
}

/* A RANGED rewrite -- the shape a FUSE ranged write takes: only segment 1
 * is re-encoded, every other segment is ALIASED (its old pba stays in the
 * new recipe). This is the rewrite under which A still names P afterwards,
 * which is what makes leg (iii) a test of the refcount rather than of a
 * legitimately dead segment. */
static uint8_t *leg_rewrite_ranged(invfs_volume *v)
{
    size_t seg = SEGMENT_SIZE, file_sz = 8 * seg, i;
    uint8_t *exp = mk_file(1);           /* A's original bytes */
    invfs_wsession *ws = NULL;

    for (i = 0; i < seg; i++)
        exp[seg + i] = (uint8_t)((i * 91 + 200) & 0xFF);   /* patch segment 1 */

    ok(vol_write_begin(v, "file_a.bin", 0, &ws) != 0 && ws,
       "leg (ii): vol_write_begin (ranged, no truncate)");
    ok(ws && vol_write_range(ws, seg, exp + seg, seg) == 0,
       "leg (ii): vol_write_range over segment 1 only");
    ok(ws && vol_write_commit(ws) == 0, "leg (ii): vol_write_commit");
    save_bytes("a_expect", exp, file_sz);
    return exp;
}

static void leg_unlink(invfs_volume *v)
{
    ok(vol_v3_unlink(v, "file_b.bin") == 0, "leg (iii): unlinked file_b.bin");
}

static void leg_assert_pba_live(invfs_volume *v, uint64_t pba, const char *tag)
{
    uint64_t plen = 0;
    uint64_t n = extent_allocated(v, pba, &plen);
    printf("  [%s] P=%llu: %llu of %llu extent blocks still allocated; "
           "A still names P: %s\n", tag, (unsigned long long)pba,
           (unsigned long long)n, (unsigned long long)plen,
           a_names_pba(v, pba) ? "yes" : "no");
    if (!a_names_pba(v, pba)) {
        printf("  [%s] (A does not name P -- this leg proves nothing)\n", tag);
        return;
    }
    ok(n == plen, tag);
}

static void dump_a(invfs_volume *v)
{
    uint64_t id = 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (vol_v3_path_lookup(v, "file_a.bin", &id) == 1 &&
        vol_read_inode(v, id, 0, &buf, &len) == 0) {
        save_bytes("a_readback", buf, len);
        printf("  [dump] %zu bytes of file_a.bin -> %s/pbaref_a_readback.bin\n",
               len, g_dir);
        free(buf);
    } else {
        printf("  [dump] file_a.bin unreadable\n");
    }
}

/* ---- the requested sequence, entirely inside ONE session ---- */

static void run_session(int do_sweep, int ranged)
{
    size_t file_sz = 8 * SEGMENT_SIZE;
    invfs_dedupe_stats ds;
    invfs_volume *v;
    uint8_t *a = mk_file(1), *b = mk_file(2), *expect;
    uint64_t pba;

    mkfs_fresh();
    v = open_vol();
    vol_v3_write_bulk(v, "file_a.bin", a, file_sz, NULL);
    vol_v3_write_bulk(v, "file_b.bin", b, file_sz, NULL);
    vol_sweep_dedupe_ex(v, &ds, NULL, NULL);
    pba = find_shared(v);
    ok(pba != 0, "dedupe made A and B name the same segment P");
    printf("  [setup] shared segment P = %llu\n", (unsigned long long)pba);
    pba_save(pba);
    report_both(v, "0 after dedupe", pba);
    oracle(v, "file_a.bin", a, file_sz, "baseline: A byte-exact after dedupe");

    if (do_sweep) {
        printf("  -- leg (i): rebuild the map with a second sweep\n");
        leg_sweep(v);
        report_both(v, "1 after 2nd sweep", pba);
    } else {
        printf("  -- leg (i) SKIPPED\n");
    }

    if (ranged) {
        printf("  -- leg (ii): RANGED rewrite of A (segments 2..8 aliased, "
               "so A keeps naming P)\n");
        expect = leg_rewrite_ranged(v);
    } else {
        printf("  -- leg (ii): FULL rewrite of A via vol_v3_write_bulk\n");
        expect = leg_rewrite(v, 0);
    }
    report_both(v, "2 after rewrite", pba);
    oracle(v, "file_a.bin", expect, file_sz, "A byte-exact after the rewrite");

    printf("  -- leg (iii): unlink B\n");
    leg_unlink(v);
    report_both(v, "3 after unlink", pba);
    leg_assert_pba_live(v, pba, "check after unlink");
    oracle(v, "file_a.bin", expect, file_sz,
           "A still byte-exact after B's unlink");
    dump_a(v);
    vol_flush(v);
    vol_close(v);
    free(expect);
    free(a); free(b);
}

/* ---- leg: a row pba_ref_ensure's build cannot read ----
 *
 * The map is the sole gate on every data-block free, so the ONE question this
 * leg answers is: what does the map do about a live inode whose row it could
 * not read? The answer it must give is a refusal, because a reference the map
 * is missing is the only kind of map error that costs data (a surplus one
 * costs space).
 *
 * Setup is the cheapest shape that shares a block: A and B hold the same
 * first 64 KiB segment, and a dedupe pass collapses the two copies into one
 * pba both recipes name. Then ONE row read is failed, the build runs, and the
 * OTHER sharer is retired.
 *
 * THE RETIRE IS vol_v3_free_recipe_blocks CALLED DIRECTLY, with B's row still
 * live, and that is deliberate. It is the free half of vol_v3_unlink -- the
 * unlink calls exactly this at src/core/vol_dirs.c:572 -- but calling it
 * directly keeps the state the map is supposed to be built in: B still NAMED.
 * Going through vol_v3_unlink instead would take a SECOND build at
 * vol_dirs.c:552, and that one runs after the dirent has already been
 * dropped at :540, so the walk cannot see the row whose blocks are about to
 * be subtracted. That is a separate defect with its own cause (an ordering,
 * not a skip) and it is not what this leg measures; putting it in the path
 * here would mean a failure of either fix looked like a failure of both.
 * The control leg runs the real vol_v3_unlink, so the unlink path is
 * exercised -- just not as the thing under test.
 *
 * `armed == 0` is the CONTROL: the identical sequence with the fault off. It
 * is what makes the armed leg a measurement of the fault rather than of the
 * scenario -- if the control ever showed the same damage, the scenario would
 * be the defect and this leg would be proving nothing.
 */
static void leg_skiprow(int armed)
{
    size_t file_sz = 8 * SEGMENT_SIZE;
    invfs_dedupe_stats ds;
    invfs_volume *v;
    uint8_t *a = mk_file(1), *b = mk_file(2);
    uint64_t pa[MAXPB];
    uint8_t b_recipe[INVFS_V3_RECIPE_ADDR_LEN];
    uint64_t b_id = 0, pba, plen = 0, donor = 0, donor_plen = 0, n, got;
    int na = 0, rc, map_on;
    char tag[64];

    snprintf(tag, sizeof tag, "%s", armed ? "skiprow" : "skiprowctl");
    printf("  [%s] === the pba-ref build, %s ===\n", tag,
           armed ? "ONE inode-row read made to fail" : "CONTROL: no fault armed");

    mkfs_fresh();
    v = open_vol();
    ok(vol_v3_write_bulk(v, "file_a.bin", a, file_sz, NULL) != 0,
       "wrote file_a.bin");
    ok(vol_v3_write_bulk(v, "file_b.bin", b, file_sz, NULL) != 0,
       "wrote file_b.bin");
    vol_sweep_dedupe_ex(v, &ds, NULL, NULL);
    pba = find_shared(v);
    ok(pba != 0, "dedupe made A and B name the same segment P");
    pba_save(pba);
    printf("  [%s] shared segment P = %llu\n", tag, (unsigned long long)pba);
    /* B's own recipe address: the unlink's `in.recipe_addr`, taken while B is
     * still live and still named. */
    memset(b_recipe, 0, sizeof b_recipe);
    {
        invfs_v3_inode bin;
        if (vol_v3_path_lookup(v, "file_b.bin", &b_id) == 1 &&
            vol_v3_inode_get(v, b_id, &bin) == 1)
            memcpy(b_recipe, bin.recipe_addr, sizeof b_recipe);
    }
    ok(b_recipe[0] != 0, "resolved B's recipe address while B is still named");
    /* A's OTHER segment: the donor that will be laid over P once P is free,
     * so the damage is a VALID segment of the same shape rather than a
     * scribble. A read error would be a weaker demonstration -- it would not
     * distinguish "the block is gone" from "the block now holds something
     * else", and the whole point of the finding is the second one. */
    if (recipe_pbas(v, "file_a.bin", pa, MAXPB, &na) == 0)
        for (int i = 0; i < na; i++)
            if (pa[i] != pba) { donor = pa[i]; break; }
    ok(donor != 0, "resolved a donor segment of the same length to lay over P");
    save_bytes("a_source", a, file_sz);

    /* ---- THE BUILD ----
     * pba_ref_reset drops the map so every attempt below is a real walk, and
     * the fault is armed HERE and nowhere else: everything above is setup,
     * and an arming that survives it spends the countdown on setup.
     *
     * The countdown is SEARCHED rather than fixed. How many inode-row reads
     * a build performs is the VOLUME's business -- the walked row set, the
     * order the directory enumerates in, and reads the walk does on its own
     * account all move it -- so a hard-coded n is a constant that goes stale
     * the moment the corpus changes, and a stale one is the worst kind of
     * test failure: it measures the healthy path and goes green. The search
     * starts the countdown at 1 and walks it up until the build comes back
     * short a live reference (pre-fix) or refuses (post-fix); both are the
     * same statement, that the map did not come back exact.
     *
     * Each attempt is independent: pba_ref_reset discards the map the
     * previous attempt left, so an attempt that hit nothing costs a walk and
     * nothing else. A search that exhausts FAILS -- the leg cannot pass
     * without having actually broken the build. */
    rc = 0;
    if (armed) {
        int n2, found = 0;
        for (n2 = 1; n2 <= 64 && !found; n2++) {
            char spec[64];
            snprintf(spec, sizeof spec, "v3_inode_row_read:%d", n2);
            setenv("INVFS_FAULT", spec, 1);
            invfs_vol_btree_fault_reload();
            pba_ref_reset(v);
            rc = pba_ref_ensure(v);
            unsetenv("INVFS_FAULT");
            invfs_vol_btree_fault_reload();
            if (rc != 0 || pba_ref_count(v, pba) < 2) {
                found = n2;
                printf("  [%s] armed INVFS_FAULT=\"v3_inode_row_read:%d\" "
                       "(via invfs_vol_btree_fault_reload): the build stopped "
                       "being exact there\n", tag, n2);
            }
        }
        ok(found != 0,
           "a single failed inode-row read is enough to make the build "
           "inexact (this assertion is what stops the leg going green on a "
           "countdown that never fired)");
        if (!found) {
            printf("  [%s] no countdown in 1..64 changed the map; the leg is "
                   "vacuous and this is a FAILURE, not a pass\n", tag);
        }
    } else {
        pba_ref_reset(v);
        rc = pba_ref_ensure(v);
    }
    map_on = v->pba_ref_on && v->pba_ref != NULL;
    printf("  [%s] pba_ref_ensure -> %d   map %s   pba_ref_count(P)=%u\n",
           tag, rc, map_on ? "EXISTS" : "ABSENT", pba_ref_count(v, pba));
    if (armed) {
        /* The two legitimate answers, and nothing else. PRE-FIX the build
         * returned 0 and left a map that says less than 2 where two live
         * recipes name P -- the "exact" flag set on an inexact map. POST-FIX
         * it returns non-zero and leaves no map, and "no map" is the state
         * every pba_ref_* already reads as unknown. */
        ok(rc != 0 || pba_ref_count(v, pba) < 2,
           "RED CONTROL: the build did not come back exact");
        ok(rc != 0 || map_on,
           "RED CONTROL: the pre-fix build returns success and an 'exact' map "
           "while missing a live row");
    } else {
        ok(rc == 0 && map_on,
           "control: the build over a healthy volume succeeds and leaves a map");
        ok(pba_ref_count(v, pba) == 2,
           "control: the map counts BOTH live recipes naming P");
    }
    report_pba(v, "after the build", pba);

    /* ---- and now the retire of the OTHER sharer ----
     * The map is now whatever the build above produced, and B's row is still
     * live and still named -- which is the state the unlink's free is
     * supposed to run in (see the comment at the top of this function). */
    ok(vol_v3_free_recipe_blocks(v, b_recipe, 0) == 0,
       "retired B's data blocks (the free half of the unlink)");
    report_pba(v, "after B's blocks are retired", pba);
    ok(a_names_pba(v, pba), "A still names P after B's blocks are retired");
    {
        uint64_t p2 = 0;
        n = extent_allocated(v, pba, &p2);
        plen = p2;
        printf("  [%s] P extent: %llu of %llu blocks still allocated\n", tag,
               (unsigned long long)n, (unsigned long long)plen);
        ok(n == plen,
           "P survives the retire of the other sharer (a count of 0 frees a "
           "block a live recipe still names)");
    }

    /* If the block WAS freed, put somebody else's valid bytes where A's are.
     * This is the "A reads something else" step, done the way a real
     * allocation would do it, so the length is unchanged and
     * invf-verify --deep (which checks readability and length only) has
     * nothing to complain about. */
    if (n < plen) {
        got = alloc_blocks(v, pba, plen, 1, 1, INVFS_ALLOC_DATA);
        printf("  [%s] alloc_blocks(%llu,%llu) -> %llu\n", tag,
               (unsigned long long)pba, (unsigned long long)plen,
               (unsigned long long)got);
        ok(got == pba, "P is handed straight back out to the next writer");
        if (got == pba && donor) {
            uint8_t *dbuf;
            /* Read the donor's frames verbatim -- a RAW copy of the donor's
             * blocks is by construction a valid segment, which is the whole
             * point: the damage has to be readable-but-different, or the leg
             * only proves "the block is gone" and not "A reads something
             * else". */
            if (seg_extent_checked(v, donor, &donor_plen) != 0 || donor_plen == 0) {
                printf("  [%s] donor %llu: framed extent did not validate, "
                       "copying its first block verbatim\n", tag,
                       (unsigned long long)donor);
                donor_plen = 1;
            }
            dbuf = (uint8_t *)malloc((size_t)donor_plen * INVFS_BLOCK_SIZE);
            if (dbuf &&
                io_seek(&v->io, donor * INVFS_BLOCK_SIZE) == 0 &&
                io_read(&v->io, dbuf, (size_t)donor_plen * INVFS_BLOCK_SIZE) == 0) {
                io_pwrite(&v->io, got * INVFS_BLOCK_SIZE, dbuf,
                          (size_t)donor_plen * INVFS_BLOCK_SIZE);
                printf("  [%s] laid donor segment %llu (%llu blocks) over P=%llu "
                       "-- a VALID segment, different bytes\n",
                       tag, (unsigned long long)donor,
                       (unsigned long long)donor_plen, (unsigned long long)pba);
            } else {
                printf("  [%s] could not read the donor segment %llu\n", tag,
                       (unsigned long long)donor);
            }
            free(dbuf);
        }
    }

    /* THE ORACLE. Byte equality against the bytes that went in, never
     * invf-verify --deep: an invfs_ast_block_entry carries a pba, not a
     * content hash, so a recipe that resolves to a valid-but-wrong segment
     * of the right length passes a deep verify. */
    {
        char m[160];
        snprintf(m, sizeof m,
                 "%s: A byte-exact after the other sharer was retired",
                 tag);
        oracle(v, "file_a.bin", a, file_sz, m);
    }
    dump_a(v);
    vol_flush(v);
    vol_close(v);
    free(a); free(b);
}

int main(int argc, char **argv)
{
    const char *phase = (argc > 2) ? argv[2] : "red";

    g_dir = (argc > 1) ? argv[1] : "/tmp";
    snprintf(g_img, sizeof g_img, "%s/pbaref-v3.img", g_dir);
    printf("pbaref_v3_test: phase=%s img=%s\n", phase, g_img);

    if (!strcmp(phase, "red") || !strcmp(phase, "rednosweep")) {
        run_session(!strcmp(phase, "red"), 1);
    } else if (!strcmp(phase, "all") || !strcmp(phase, "allnosweep")) {
        run_session(!strcmp(phase, "all"), 0);
    } else if (!strcmp(phase, "setup")) {
        size_t file_sz = 8 * SEGMENT_SIZE;
        invfs_dedupe_stats ds;
        invfs_volume *v;
        uint8_t *a = mk_file(1), *b = mk_file(2);
        uint64_t pba;
        mkfs_fresh();
        v = open_vol();
        ok(vol_v3_write_bulk(v, "file_a.bin", a, file_sz, NULL) != 0, "wrote file_a.bin");
        ok(vol_v3_write_bulk(v, "file_b.bin", b, file_sz, NULL) != 0, "wrote file_b.bin");
        printf("  [setup] dedupe: %d merged\n", vol_sweep_dedupe_ex(v, &ds, NULL, NULL));
        pba = find_shared(v);
        ok(pba != 0, "dedupe made A and B name the same segment P");
        printf("  [setup] shared segment P = %llu\n", (unsigned long long)pba);
        report_both(v, "setup", pba);
        pba_save(pba);
        save_bytes("a_expect", a, file_sz);
        vol_flush(v); vol_close(v);
        free(a); free(b);
    } else if (!strcmp(phase, "sweep")) {
        invfs_volume *v = open_vol();
        uint64_t pba = pba_load();
        leg_sweep(v);
        report_both(v, "after sweep", pba);
        vol_flush(v); vol_close(v);
    } else if (!strcmp(phase, "rewrite")) {
        invfs_volume *v = open_vol();
        uint64_t pba = pba_load();
        uint8_t *d;
        report_both(v, "before rewrite", pba);
        d = leg_rewrite(v, 1);
        free(d);
        report_both(v, "after rewrite", pba);
        vol_flush(v); vol_close(v);
    } else if (!strcmp(phase, "unlink")) {
        invfs_volume *v = open_vol();
        uint64_t pba = pba_load();
        report_both(v, "before unlink", pba);
        leg_unlink(v);
        report_both(v, "after unlink", pba);
        leg_assert_pba_live(v, pba, "check after unlink");
        dump_a(v);
        vol_flush(v); vol_close(v);
    } else if (!strcmp(phase, "check")) {
        invfs_volume *v = open_vol();
        leg_assert_pba_live(v, pba_load(), "check");
        dump_a(v);
        vol_close(v);
    } else if (!strcmp(phase, "wrongfree")) {
        /* THE CORRUPTING DIRECTION, no hand-built recipe needed.
         *
         * One session: mkfs, open (pba_ref_ensure builds the map over an
         * EMPTY volume, src/core/volume.c:2091), then A and B are created.
         * Nothing on the v3 write path applies a +1 for a new inode
         * (vol_v3_inode_delta_put, src/core/vol_btree.c:3672, has no pba_ref
         * hook), so the map is empty when the two files land. Dedupe merges
         * B's copy of the shared segment into A's pba P and hand-adjusts the
         * map by -1/+1 (src/core/vol_dedupe.c:288-289), which is why the
         * count reads 1 where two live recipes name P. Unlinking B then
         * drives it to 0 and frees P out from under A.
         */
        size_t file_sz = 8 * SEGMENT_SIZE;
        invfs_dedupe_stats ds;
        invfs_volume *v;
        uint8_t *a = mk_file(1), *b = mk_file(2);
        uint64_t pba, plen = 0, n;

        mkfs_fresh();
        v = open_vol();
        vol_v3_write_bulk(v, "file_a.bin", a, file_sz, NULL);
        vol_v3_write_bulk(v, "file_b.bin", b, file_sz, NULL);
        vol_sweep_dedupe_ex(v, &ds, NULL, NULL);
        pba = find_shared(v);
        ok(pba != 0, "wrongfree: A and B name the same segment P");
        pba_save(pba);
        extent_allocated(v, pba, &plen);
        report_pba(v, "wrongfree/after dedupe", pba);
        printf("  [wrongfree] two live recipes name P; the map says %u "
               "(a correct map says 2)\n", pba_ref_count(v, pba));
        oracle(v, "file_a.bin", a, file_sz, "wrongfree: A byte-exact before the unlink");

        ok(vol_v3_unlink(v, "file_b.bin") == 0, "wrongfree: unlinked file_b.bin");
        n = extent_allocated(v, pba, &plen);
        report_pba(v, "wrongfree/after unlink", pba);
        printf("  [wrongfree] P extent %llu of %llu blocks still allocated\n",
               (unsigned long long)n, (unsigned long long)plen);
        ok(n == plen, "wrongfree: P survives the unlink of the OTHER sharer "
                      "(a count of 0 frees a live block)");
        oracle(v, "file_a.bin", a, file_sz,
               "wrongfree: A still byte-exact after the OTHER sharer's unlink");
        /* the bitmap bit is the whole of the damage: the block is now
         * ALLOCATABLE, so the next allocation takes it and A's bytes go. */
        if (n < plen) {
            uint64_t got = alloc_blocks(v, pba, plen, 1, 1, INVFS_ALLOC_DATA);
            printf("  [wrongfree] alloc_blocks(%llu,%llu) -> %llu "
                   "(a live recipe still names it)\n",
                   (unsigned long long)pba, (unsigned long long)plen,
                   (unsigned long long)got);
            ok(got == pba, "wrongfree: P is still HANDED OUT to a new writer");
            if (got == pba) {
                uint8_t blk[INVFS_BLOCK_SIZE];
                memset(blk, 0xFF, sizeof blk);
                if (io_seek(&v->io, got * INVFS_BLOCK_SIZE) == 0 &&
                    io_read(&v->io, blk, INVFS_BLOCK_SIZE) == 0 &&
                    INVFS_BLOCK_SIZE > 8)
                    memset(blk + 8, 0xFF, INVFS_BLOCK_SIZE - 8); /* keep the framed header */
                io_pwrite(&v->io, got * INVFS_BLOCK_SIZE, blk, INVFS_BLOCK_SIZE);
                oracle(v, "file_a.bin", a, file_sz,
                       "wrongfree: A after P was reallocated and overwritten");
            }
        }
        dump_a(v);
        vol_flush(v); vol_close(v);
        free(a); free(b);
    } else if (!strcmp(phase, "hookctl")) {
        /* RED CONTROL for the missing hook at vol_v3_inode_delta_put
         * (src/core/vol_btree.c:3672).
         *
         * One session. A and B share P after dedupe; B is unlinked, leaving
         * A the sole live sharer. A SECOND inode is then published naming P
         * through the real v3 recipe-publish pair (vol_v3_recipe_store +
         * vol_v3_create_content_node) -- the same pair every lane that
         * supersedes or publishes a recipe uses. vol_v3_inode_delta_put has
         * no pba_ref hook, so the map never hears about the new sharer; the
         * next unlink of a P-namer drives the count to 0 and frees P out
         * from under the live inode.
         */
        size_t file_sz = 8 * SEGMENT_SIZE;
        invfs_dedupe_stats ds;
        invfs_volume *v;
        uint64_t pba, id2, id_a = 0, plen = 0;
        uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN];
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents = NULL;
        uint8_t *blob = NULL;
        size_t blen = 0, n = 0;
        invfs_v3_inode in;
        uint8_t *a = mk_file(1), *b = mk_file(2);

        mkfs_fresh();
        v = open_vol();
        vol_v3_write_bulk(v, "file_a.bin", a, file_sz, NULL);
        vol_v3_write_bulk(v, "file_b.bin", b, file_sz, NULL);
        vol_sweep_dedupe_ex(v, &ds, NULL, NULL);
        ok(vol_v3_unlink(v, "file_b.bin") == 0, "control: B unlinked");
        {
            uint64_t pa[MAXPB];
            int na = 0;
            recipe_pbas(v, "file_a.bin", pa, MAXPB, &na);
            pba = na ? pa[0] : 0;
        }
        ok(pba != 0, "control: P resolved from A's recipe");
        pba_save(pba);
        report_pba(v, "control/after B unlink", pba);
        extent_allocated(v, pba, &plen);

        if (vol_v3_path_lookup(v, "file_a.bin", &id_a) != 1) return 2;
        if (vol_v3_inode_get(v, id_a, &in) != 1) return 2;
        if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0) return 2;
        if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n) != 0) return 2;
        if (vol_v3_recipe_store(v, blob, blen, addr) != 0) return 2;
        free(blob);
        id2 = vol_v3_create_content_node(v, "file_c.bin", in.size, addr);
        ok(id2 != 0, "control: second inode (file_c.bin) published naming P");
        report_pba(v, "control/after C published", pba);
        /* the map is stale, not wrong: the next pba_ref_ensure (the one the
         * unlink below takes) must rebuild it to 2. Pre-fix there was no
         * such hook and it stayed at 1, so the unlink's -1 reached 0. */
        pba_ref_ensure(v);
        printf("  [control] after the ensure the map says %u (2 = both A and C)\n",
               pba_ref_count(v, pba));
        ok(pba_ref_count(v, pba) >= 2,
           "control: the map knows about the new sharer");
        oracle(v, "file_c.bin", a, file_sz, "control: C byte-exact when published");

        ok(vol_v3_unlink(v, "file_a.bin") == 0, "control: A unlinked");
        report_pba(v, "control/after A unlink", pba);
        {
            uint64_t p2 = 0;
            uint64_t n2 = extent_allocated(v, pba, &p2);
            printf("  [control] P extent: %llu of %llu blocks still allocated "
                   "(expected %llu)\n", (unsigned long long)n2,
                   (unsigned long long)p2, (unsigned long long)plen);
            ok(n2 == plen,
               "control: P survives the unlink of the other sharer "
               "(pre-fix: freed under the live file_c.bin)");
        }
        oracle(v, "file_c.bin", a, file_sz,
               "control: file_c.bin still byte-exact after P was freed");
        dump_a(v);
        vol_flush(v); vol_close(v);
        free(a); free(b);
    } else if (!strcmp(phase, "skiprow") || !strcmp(phase, "skiprowctl")) {
        leg_skiprow(!strcmp(phase, "skiprow"));
    } else {
        fprintf(stderr, "unknown phase %s\n", phase);
        return 2;
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
