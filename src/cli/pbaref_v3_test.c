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
    } else {
        fprintf(stderr, "unknown phase %s\n", phase);
        return 2;
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
