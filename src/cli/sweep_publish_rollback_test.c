/* sweep_publish_rollback_test.c — WP sweep-rollback-test-conflict.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS THE RED CONTROL FOR
 *
 * A v3 sweep transform that allocates and writes replacement segments and
 * then CANNOT publish the recipe that would name them must give every one of
 * those blocks back. `vol_v3_recipe_store` is the single point at which the
 * new recipe becomes reachable and the one step in the pass that can fail for
 * want of space; the blocks written before it are, until then, allocated and
 * referenced by nothing.
 *
 * That rollback lived in `vol_sweep_one_v3` (src/core/vol_sweep.c). A merge
 * rewrote the publish block and mis-braced it: the `else` carrying the
 * rollback was bound to the `vol_ast_recipe_serialize` test instead of the
 * `vol_v3_recipe_store` test. `vol_ast_recipe_serialize` essentially never
 * fails, so the rollback became UNREACHABLE -- a failed publish fell out of
 * the outer `if` having done nothing at all, `any_swept` stayed 1, the
 * function returned "swept", and every block it had recorded in `remap[]`
 * stayed allocated under no reachable name.
 *
 * Measured through tools/test-sweep-publish-rollback.sh, 36 files / 1 MiB of
 * incompressible data on a 0.07 GiB v3 image, one sweep, identical corpus on
 * both trees:
 *
 *                      free before -> after        unclaimed       outcome
 *   48191ad (parent)   257 -> 315   (drop  -58)   0.4 -> 0.2 MiB   rolled back 221 / 13
 *   c64382d (regressed) 257 ->  94   (drop +163)   0.4 -> 1.0 MiB   NOTHING
 *
 * 163 blocks, unrecoverable by any later sweep or by `invf-fsck -f`, which is
 * what wedges the volume into a user-visible ENOSPC on a write that needed
 * 46 blocks.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS NOT THE SHELL SUITE AGAIN
 *
 * tools/test-sweep-publish-rollback.sh already exists and already caught this.
 * It measures a NET over the whole sweep, which is the weakest instrument
 * available: it cannot say WHICH blocks went missing, so it cannot tell an
 * abandoned transform's stranded segments from anything else the pass did.
 * This test measures the stranded set BY ADDRESS:
 *
 *   unclaimed = { b : b allocated in the bitmap, b in the data region,
 *                    no live recipe names b }
 *
 * computed by walking the live inode set and parsing each recipe, exactly as
 * `vol_sweep_stats` derives its `unclaimed` field. That is the defect's own
 * definition, counted in blocks and resolved to pbas -- the set of blocks the
 * abandoned transform wrote and never gave back is exactly the set that grew.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS ASSERTED, AND WHY NONE OF IT CAN PASS BY ACCIDENT
 *
 * Leg 1 (the premise, and the anti-vacuity leg). The sweep's own rollback
 * message is captured by redirecting fd 2 to a file for the duration of the
 * call. The test requires it to be present, and requires it to name at least
 * one segment. A build that never reached the rollback -- which is what the
 * regression looked like -- prints no such line and FAILS this leg, so the
 * suite cannot go green by never entering the state at all.
 *
 * Leg 2 (the fingerprint). The message carries the block count and the
 * segment count. It asserts `blocks == 17 * segments`: a 64 KiB segment is
 * 17 physical blocks with the 8-byte segment header, so this is the original
 * defect's "17 per orphaned segment", INVERTED -- it is now the number of
 * blocks handed BACK. With the rollback unreachable the log is silent and leg
 * 1 has already failed; with a rollback that frees the wrong blocks the count
 * does not match and this leg fails.
 *
 * Leg 3 (by address). The unclaimed block set did not grow. Not "did not grow
 * much": not at all. This is the leg that is the claim rather than a proxy.
 *
 * Leg 4 (the net, kept). Free blocks did not drop. This is the ORIGINAL
 * assertion, in its original form -- the shell suite's leg 2, unchanged --
 * and it is kept precisely so that the in-process by-address measurement and
 * the end-to-end net measurement have to agree.
 *
 * Leg 5 (the data-safety half). t.bin is still RAW -- its recipe address is
 * byte-identical to what it was before the pass -- and still reads back
 * byte-identical. A rollback that freed the OLD segments instead of the new
 * ones would pass legs 2-4 and fail this.
 *
 * The sweep is driven through `vol_sweep_one_ex`, the shipped entry point the
 * offline driver calls, not through a re-implementation of the lines that
 * were wrong.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include "volume_internal.h"
#include "vol_btree.h"

/* A 64 KiB segment carries an 8-byte header and is stored in 4 KiB blocks:
 * ceil((65536 + 8) / 4096) == 17. This is the "17 per orphaned segment" the
 * suite's failure message names; leg 2 asserts it against the rollback. */
#define SEG_BYTES   65536
#define SEG_BLOCKS  17

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", what); }
    else        { printf("  OK    %s\n", what); }
}

static invfs_volume *g_v;
static char g_img[512];

/* ------------------------------------------------------------------ */
/* deterministic incompressible bytes                                    */
/* ------------------------------------------------------------------ */

/* xorshift64*: fixed seed, so the corpus is byte-identical on every host and
 * every run. No /dev/urandom -- this gate has to be reproducible. */
static void fill_incompressible(uint8_t *p, size_t n, uint64_t seed)
{
    size_t i;
    uint64_t x = seed ? seed : 0x9E3779B97F4A7C15ull;
    for (i = 0; i < n; i++) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        p[i] = (uint8_t)((x * 0x2545F4914F6CDD1Dull) >> 56);
    }
}

static uint64_t write_file(const char *name, const uint8_t *body, size_t n)
{
    invfs_meta_pub m;
    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode = 0644;
    return vol_v3_write_bulk(g_v, name, body, n, &m);
}

/* ------------------------------------------------------------------ */
/* the stranded set, by address                                         */
/* ------------------------------------------------------------------ */

/* One bit per block; `named` is the live-recipe claim set. */
typedef struct {
    uint8_t *named;      /* named by some live recipe          */
    uint8_t *alloc;      /* set in the bitmap                   */
    uint64_t n;
} blockmap;

static int bm_index(const blockmap *m, uint64_t b)
{
    return b < m->n && bit_get(m->alloc, b) && !bit_get(m->named, b);
}

typedef struct { blockmap *m; } named_ctx;

static int named_cb(invfs_volume *v, uint64_t inode_id, const char *name,
                    void *ctx_)
{
    named_ctx *c = (named_ctx *)ctx_;
    blockmap *m = c->m;
    invfs_v3_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0, i;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;
    (void)name;

    if (vol_v3_inode_get(v, inode_id, &in) != 1) return 0;
    if (in.size == 0) return 0;
    if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob) return 0;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) == 0 && ents) {
        for (i = 0; i < n_ents; i++) {
            uint64_t pba = ents[i].pba, plen = 0, k;
            /* zone == TEXT names a SHARED batch segment owned by the batch
             * registry (tz_v3_gc), exactly as vol_sweep_stats counts it. */
            if (ents[i].zone == INVFS_ZONE_TEXT || !pba) continue;
            /* The WHOLE extent, not just its head: a recipe entry names the
             * segment's first pba, and one 64 KiB segment occupies 17
             * consecutive blocks. vol_sweep_stats claims them the same way
             * (seg_extent_checked, then a per-block set), and these numbers
             * are only comparable to `invf-stats`'s unclaimed because of it
             * -- claiming the head alone reports every volume full. */
            if (seg_extent_checked(v, pba, &plen) != 0 || plen == 0) continue;
            if (plen > m->n - pba) plen = m->n - pba;
            for (k = 0; k < plen; k++)
                m->named[(pba + k) / 8] |= (uint8_t)(1u << ((pba + k) % 8));
        }
    }
    free(blob);
    return 0;
}

/* The data region, per vol_sweep_stats: from raw_zone_start to the end. */
static uint64_t count_unclaimed(blockmap *m)
{
    uint64_t b, k = 0;
    for (b = g_v->sb.raw_zone_start; b < m->n; b++)
        if (bm_index(m, b)) k++;
    return k;
}

static void snapshot_unclaimed(blockmap *m)
{
    uint64_t b;
    memset(m->named, 0, (size_t)((m->n + 7) / 8));
    memset(m->alloc, 0, (size_t)((m->n + 7) / 8));
    for (b = 0; b < m->n; b++)
        if (bit_get(g_v->bitmap, b))
            m->alloc[b / 8] |= (uint8_t)(1u << (b % 8));
    { named_ctx c; c.m = m; vol_v3_iter_live_inodes(g_v, named_cb, &c); }
}

/* ------------------------------------------------------------------ */
/* stderr capture: the rollback message is the only proof the test     */
/* actually reached the path it claims to test                          */
/* ------------------------------------------------------------------ */

static int cap_saved_fd = -1;
static int cap_fd = -1;

static void cap_begin(const char *path)
{
    fflush(stderr);
    cap_saved_fd = dup(2);
    cap_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (cap_fd >= 0) dup2(cap_fd, 2);
}

static void cap_end(const char *path)
{
    fflush(stderr);
    if (cap_saved_fd >= 0) { dup2(cap_saved_fd, 2); close(cap_saved_fd); cap_saved_fd = -1; }
    if (cap_fd >= 0) { close(cap_fd); cap_fd = -1; }
    (void)path;
}

/* Pull "rolled back <B> rewritten block(s) across <S> segment(s)" out of the
 * captured sweep output. Returns 1 if both numbers were found. */
static int parse_rollback(const char *buf, unsigned long long *blocks,
                          unsigned long long *segs)
{
    const char *p = buf;
    while ((p = strstr(p, "rolled back ")) != NULL) {
        if (sscanf(p, "rolled back %llu rewritten block(s) across %llu segment(s)",
                   blocks, segs) == 2)
            return 1;
        p += 12;
    }
    return 0;
}

#define MAX_ENTS 8192

static int recipe_pbas(const uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN],
                       uint64_t *out, size_t cap, size_t *n_out)
{
    uint8_t *blob = NULL;
    size_t blen = 0, i;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;

    *n_out = 0;
    if (vol_v3_recipe_load(g_v, addr, &blob, &blen) != 0 || !blob) return -1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 ||
        ents == NULL) { free(blob); return -1; }
    for (i = 0; i < n_ents && *n_out < cap; i++)
        out[(*n_out)++] = ents[i].pba;
    free(blob);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    const size_t target = 1048576;          /* 1 MiB = 16 segments */
    uint8_t *tbuf = NULL, *piece = NULL, *step = NULL, *back = NULL;
    size_t back_len = 0, n_old = 0, i;
    uint64_t id = 0, free_before, free_after;
    uint64_t unc_before, unc_after;
    uint64_t old_pbas[MAX_ENTS];
    uint8_t old_addr[INVFS_V3_RECIPE_ADDR_LEN];
    invfs_v3_inode before, after;
    blockmap bm;
    unsigned long long rb_blocks = 0, rb_segs = 0;
    char cap_path[600], cmd[900];
    char *capbuf = NULL;
    long caplen;
    FILE *cf;
    int err = 0, rc, n;
    unsigned long long grew;

    snprintf(g_img, sizeof g_img, "%s/wp-sweep-rb.img", dir);
    snprintf(cap_path, sizeof cap_path, "%s/wp-sweep-rb.log", dir);
    unlink(g_img);
    unlink(cap_path);

    /* 0.07 GiB. Small on purpose: this gate is about block accounting, not
     * throughput, and every probe here is an O(volume) pass. The argument
     * is GIBYTES and mkfs refuses below 64 MB, so 0.07 is the floor that
     * still leaves room for the 1 MiB target plus the filler. */
    snprintf(cmd, sizeof cmd, "bin/invf-mkfs %s 0.07 2>/dev/null", g_img);
    if (system(cmd) != 0) {
        fprintf(stderr, "setup: invf-mkfs failed (run from the repo root)\n");
        return 2;
    }
    g_v = vol_open(g_img, &err);
    if (!g_v) { fprintf(stderr, "setup: vol_open failed (%d)\n", err); return 2; }

    tbuf   = (uint8_t *)malloc(target);
    piece  = (uint8_t *)malloc(target);
    step   = (uint8_t *)malloc(65536);
    if (!tbuf || !piece || !step) { fprintf(stderr, "setup: malloc\n"); return 2; }
    fill_incompressible(tbuf,   target, 0xC0FFEE01ull);
    fill_incompressible(piece,  target, 0xC0FFEE02ull);
    fill_incompressible(step,   65536,  0xC0FFEE03ull);

    printf("leg 1-5: a v3 sweep transform that cannot publish must roll back\n");
    printf("        corpus: 1 x %zu bytes incompressible = %zu segments\n",
           target, target / SEG_BYTES);

    /* The file the transform will run on. Incompressible by construction, so
     * the generic floor cannot shrink it and the per-segment remap path is
     * the one that runs. */
    id = write_file("t.bin", tbuf, target);
    ok(id != 0, "the incompressible target file is written");
    if (!id) return 1;

    /* ---- fill to the point where the recipe cannot be published ----
     * The state the defect needs: room to write replacement segments, no
     * room to name them. The bulk filler goes in 1 MiB pieces because
     * vol_write_range's atomic ENOSPC precheck models a call's worst case as
     * every touched segment stored verbatim, so one huge write needs more
     * blocks than the data pool holds and is refused before a byte lands.
     * Then short 64 KiB steps until one is REFUSED: the refusal is the
     * floor, and it is where the defect lives. */
    for (n = 0; n < 200; n++) {
        char nm[64];
        if (vol_free_blocks_cached(g_v) <= 300) break;
        snprintf(nm, sizeof nm, "piece.%d", n);
        if (!write_file(nm, piece, target)) break;
    }
    for (n = 0; n < 400; n++) {
        char nm[64];
        snprintf(nm, sizeof nm, "step.%d", n);
        if (!write_file(nm, step, 65536)) break;
    }
    free_before = vol_free_blocks_cached(g_v);
    if (free_before > 300) {
        fprintf(stderr, "setup: could not reach a publish-hostile slack "
                        "(still %llu free)\n", (unsigned long long)free_before);
        return 2;
    }

    bm.n = g_v->sb.total_blocks;
    bm.named = (uint8_t *)calloc((size_t)((bm.n + 7) / 8), 1);
    bm.alloc = (uint8_t *)calloc((size_t)((bm.n + 7) / 8), 1);
    if (!bm.named || !bm.alloc) { fprintf(stderr, "setup: bitmap\n"); return 2; }

    ok(vol_v3_inode_get(g_v, id, &before) == 1, "the target's row is readable");
    memcpy(old_addr, before.recipe_addr, sizeof old_addr);
    ok(recipe_pbas(old_addr, old_pbas, MAX_ENTS, &n_old) == 0 && n_old > 0,
       "PREMISE: its recipe names at least one data block");
    printf("        recipe: %zu entries; free %llu; data region %llu..%llu\n",
           n_old, (unsigned long long)free_before,
           (unsigned long long)g_v->sb.raw_zone_start,
           (unsigned long long)bm.n);

    snapshot_unclaimed(&bm);
    unc_before = count_unclaimed(&bm);
    printf("        unclaimed before the transform: %llu block(s)\n",
           (unsigned long long)unc_before);

    /* ---- the transform, through the shipped entry point, with stderr
     * captured so the rollback message can be asserted on ---- */
    cap_begin(cap_path);
    rc = vol_sweep_one_ex(g_v, id, "t.bin", NULL, NULL);
    cap_end(cap_path);

    cf = fopen(cap_path, "rb");
    if (!cf) { fprintf(stderr, "setup: cannot read the captured sweep log\n"); return 2; }
    fseek(cf, 0, SEEK_END);
    caplen = ftell(cf);
    fseek(cf, 0, SEEK_SET);
    if (caplen < 0) caplen = 0;
    capbuf = (char *)calloc((size_t)caplen + 1, 1);
    if (capbuf && caplen > 0) {
        size_t got = fread(capbuf, 1, (size_t)caplen, cf);
        capbuf[got] = 0;
    }
    fclose(cf);

    printf("leg 1: the pass really reached the publish-rollback path\n");
    ok(parse_rollback(capbuf ? capbuf : "", &rb_blocks, &rb_segs),
       "PREMISE: the sweep said it rolled back (the path under test ran)");
    ok(rb_segs >= 1,
       "PREMISE: it rolled back at least one segment, so the suite is not "
       "passing on a transform that never wrote anything");
    printf("        rolled back %llu block(s) across %llu segment(s)\n",
           rb_blocks, rb_segs);

    printf("leg 2: the fingerprint -- 17 blocks per orphaned segment, given back\n");
    ok(rb_segs > 0 && rb_blocks == (unsigned long long)SEG_BLOCKS * rb_segs,
       "every block the abandoned transform wrote was handed back "
       "(blocks == 17 * segments; pre-fix 17 per orphaned segment STRANDED)");

    printf("leg 3: by address -- the unclaimed set did not grow\n");
    snapshot_unclaimed(&bm);
    unc_after = count_unclaimed(&bm);
    grew = unc_after > unc_before ? unc_after - unc_before : 0;
    printf("        unclaimed after: %llu block(s) (was %llu)\n",
           (unsigned long long)unc_after, (unsigned long long)unc_before);
    ok(grew == 0,
       "no block that is allocated in the data region and named by NO live "
       "recipe appeared -- the stranded set, resolved to pbas, is unchanged");
    if (grew) {
        uint64_t b, shown = 0;
        for (b = g_v->sb.raw_zone_start; b < bm.n && shown < 8; b++)
            if (bm_index(&bm, b)) {
                printf("        stranded pba %llu\n", (unsigned long long)b);
                shown++;
            }
    }

    printf("leg 4: the net -- free blocks did not drop\n");
    free_after = vol_free_blocks_cached(g_v);
    printf("        free before %llu, after %llu\n",
           (unsigned long long)free_before, (unsigned long long)free_after);
    ok(free_after >= free_before,
       "the sweep left the volume no smaller than it found it");

    printf("leg 5: the data-safety half -- the file is still RAW and bit-exact\n");
    ok(vol_v3_inode_get(g_v, id, &after) == 1, "the target's row is still readable");
    ok(memcmp(after.recipe_addr, old_addr, INVFS_V3_RECIPE_ADDR_LEN) == 0,
       "the recipe address did not move: the transform never published");
    {
        int all_alloc = 1;
        for (i = 0; i < n_old; i++)
            if (old_pbas[i] && !bit_get(g_v->bitmap, old_pbas[i])) all_alloc = 0;
        ok(all_alloc, "every block the ORIGINAL recipe names is still allocated "
                      "(the rollback gave back the NEW blocks, not the old ones)");
    }
    rc = vol_read_file(g_v, id, &back, &back_len);
    ok(rc == 0 && back && back_len == target && memcmp(back, tbuf, target) == 0,
       "the file reads back byte-identical after the abandoned pass");
    free(back);

    free(capbuf);
    free(bm.named);
    free(bm.alloc);
    free(tbuf); free(piece); free(step);
    vol_close(g_v);
    unlink(g_img);
    unlink(cap_path);

    printf("\nsweep publish rollback: %d checks, %d failure(s)\n",
           checks, failures);
    return failures ? 1 : 0;
}