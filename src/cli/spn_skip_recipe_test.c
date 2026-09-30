/*
 * spn_skip_recipe_test.c — WP141: the SPT0 save point's RECLAIM must not
 * release a block a recipe the capture walk could not read still names.
 *
 * THE TWO ENDS OF ONE SAVE POINT DISAGREE.
 *
 *   capture  src/core/vol_spt0.c:571-574   vol_v3_recipe_load fails ->
 *                                          c->unreadable++; return 0;  (skip)
 *   reclaim  src/core/vol_spt0.c:736       free every block in old_map that
 *                                          is NOT in new_map
 *   restore  src/core/vol_spt0.c:1182-1187 the SAME load failure -> return -1
 *                                          (damage: refuse the rollback)
 *
 * So a block a live recipe names is absent from the new mark set exactly when
 * that recipe would not load; it WAS pinned last generation, so it is in
 * old_map; it is not in new_map; so spn_reclaim frees it. The walk's own
 * warning (:817-823) says so and continues.
 *
 * spn_reclaim's comment at :715-717 already states the rule for the OTHER owner
 * set -- an unreadable registry is not "no owner", because that is the one
 * answer that can lose data, so it fails closed. The recipe walk never got the
 * same treatment.
 *
 * THE TRIGGER is one-shot injected read failure on the recipe load
 * (`v3_recipe_load`, vol_btree.c, re-armed through the cross-TU door
 * invfs_vol_btree_fault_reload), and it is transient on purpose:
 *
 *   - A PERSISTENT unreadable recipe (a scribbled blob, a quarantined base
 *     page -- both expected states on a damaged volume, AGENTS.md 2.10/2.6)
 *     cannot be used as the oracle here, because the file whose data was
 *     stolen is then unreadable for a reason that has nothing to do with the
 *     reclaim: invf-cat would fail on the missing recipe, and the demonstration
 *     would prove "the recipe is gone", not "the reclaim freed a live
 *     recipe's block". A one-shot read failure isolates the reclaim: the volume
 *     is healthy before and after, so the ONLY thing that changed is what the
 *     reclaim did, and the file reads back -- with the wrong bytes.
 *   - The countdown is SEARCHED (1..16), not fixed, because how many recipe
 *     loads a capture performs before the walk reaches the file is the
 *     volume's business. A hard-coded n goes stale silently and the leg goes
 *     green measuring the healthy path. Each attempt is a fresh image, so an
 *     attempt that hit nothing costs a scenario and nothing else. A search
 *     that exhausts FAILS: the leg cannot pass without having fired.
 *
 * `skippedctl` is the identical sequence with the fault off — green before and
 * after the fix, so the damage is measured against the fault and not against
 * the scenario.
 *
 * THE ORACLE is read-back BYTES, never invf-verify --deep: it checks
 * readability and length only (src/cli/verify.c:357-364), and the damage laid
 * down here is a REAL segment of the same length, so a deep verify reports the
 * volume clean. The shell-level cmp is `bin/invf-cat` against the bytes the
 * file had before the capture.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "invarifs.h"
#include "volume_internal.h"
#include "vol_spt0.h"

/* The cross-TU fault door: the site is vol_v3_recipe_load, inside
 * vol_btree.c, and the arming state is that file's TU statics. Reload through
 * the door -- unsetenv+setenv is NOT a substitute (it frees the old spec
 * string, setenv usually gets the same address back, the pointer compare in
 * invfs_vol_fault sees no change, the countdown stays spent, and this leg goes
 * green measuring the healthy path). */
extern void invfs_vol_btree_fault_reload(void);

#define MAXPB 64

static int checks = 0, failures = 0;
static const char *g_dir = "/srv/bench/scratch";
static char g_img[512];

static void ok(int cond, const char *msg)
{
    checks++;
    printf("  %s  %s\n", cond ? "OK  " : "FAIL", msg);
    if (!cond) failures++;
}

static void dump(const char *tag, const uint8_t *b, size_t n)
{
    char p[600];
    FILE *f;
    snprintf(p, sizeof p, "%s/spn_%s.bin", g_dir, tag);
    f = fopen(p, "wb");
    if (f) { fwrite(b, 1, n, f); fclose(f); }
    printf("  [dump] %zu bytes -> %s\n", n, p);
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
    char cmd[700];
    unlink(g_img);
    snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 64 >/dev/null 2>&1", g_img);
    if (system(cmd) != 0) { fprintf(stderr, "mkfs failed\n"); exit(2); }
}

/* one file's segment pbas, in recipe order */
static int recipe_pbas(invfs_volume *v, const char *path, uint64_t *out,
                       int max, int *n_out)
{
    uint64_t id = 0;
    invfs_v3_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0, i;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n = 0;
    int m = 0;

    *n_out = 0;
    if (vol_v3_path_lookup(v, path, &id) != 1) return -1;
    if (vol_v3_inode_get(v, id, &in) != 1) return -1;
    if (in.size == 0) return 0;
    if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob)
        return -1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n) == 0 && ents)
        for (i = 0; i < n && m < max; i++)
            if (ents[i].pba)
                out[m++] = ents[i].pba;
    free(blob);
    *n_out = m;
    return 0;
}

static int names_pba(invfs_volume *v, const char *path, uint64_t pba)
{
    uint64_t pa[MAXPB];
    int na = 0, i;
    if (recipe_pbas(v, path, pa, MAXPB, &na) != 0) return 0;
    for (i = 0; i < na; i++) if (pa[i] == pba) return 1;
    return 0;
}

/* how many of a segment's own extent blocks are still marked allocated */
static uint64_t extent_allocated(invfs_volume *v, uint64_t pba, uint64_t *plen)
{
    uint64_t pl = 0, b, n = 0;

    if (plen) *plen = 0;
    if (!pba) return 0;
    if (seg_extent_checked(v, pba, &pl) != 0) {     /* header gone: bit only */
        if (plen) *plen = 1;
        return bit_get(v->bitmap, pba) ? 1 : 0;
    }
    if (plen) *plen = pl;
    for (b = 0; b < pl; b++)
        if (bit_get(v->bitmap, pba + b)) n++;
    return n;
}

/* THE bit-exactness oracle: read back through the normal read path and compare
 * BYTES against what went in. */
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
        printf("  [oracle %s] %s: %zu bytes BYTE-EXACT vs source\n", tag,
               name, len);
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
    if (!same) dump("readback", buf, len);
    free(buf);
    return same;
}

/* INCOMPRESSIBLE, and every segment different. That is not decoration: a
 * segment that compresses to one block makes the freed extent a single block,
 * and a single-block extent makes "P now holds another segment" a much weaker
 * statement than it looks. A 64 KiB segment of pseudo-random bytes spans ~17
 * blocks, so the freed run is a real extent. */
static uint8_t *mk_file(void)
{
    size_t seg = SEGMENT_SIZE, file_sz = 8 * seg, s, i;
    uint8_t *d = (uint8_t *)malloc(file_sz);
    uint32_t r;

    assert(d);
    for (s = 0; s < file_sz / seg; s++) {
        r = 0x1234567u ^ (uint32_t)(s * 2654435761u);
        for (i = 0; i < seg; i++) {
            r = r * 1103515245u + 12345u;
            d[s * seg + i] = (uint8_t)((r >> 16) & 0xFF);
        }
    }
    return d;
}

/* One full scenario from a fresh image. Returns 1 if the armed fault fired
 * (i.e. the capture's walk actually skipped this file's recipe), 0 if it did
 * not -- the caller keeps searching the countdown. */
static int attempt(int armed, int n)
{
    size_t file_sz = 8 * SEGMENT_SIZE;
    invfs_volume *v;
    uint8_t *a = mk_file();
    uint64_t pa[MAXPB];
    int na = 0, rc, fired = 0;
    uint64_t P = 0, plen = 0, n_alloc, p2 = 0;
    uint8_t *donor = NULL;
    uint64_t donor_pba = 0, donor_plen = 0;
    char tag[64], spec[64];

    snprintf(tag, sizeof tag, "%s:%d", armed ? "skipped" : "skippedctl", n);
    mkfs_fresh();
    v = open_vol();
    ok(vol_v3_write_bulk(v, "file_a.bin", a, file_sz, NULL) != 0,
       "wrote file_a.bin");

    /* ---- CAPTURE #1: healthy. This is the generation whose pin becomes the
     * next capture's reclaim worklist. */
    rc = spt0_capture(v);
    ok(rc == 0, "capture #1: the save point was captured");
    ok(recipe_pbas(v, "file_a.bin", pa, MAXPB, &na) == 0 && na > 1,
       "capture #1: file_a.bin has a multi-segment recipe");
    P = pa[0];
    extent_allocated(v, P, &plen);
    printf("  [%s] P=%llu, %llu extent blocks\n", tag,
           (unsigned long long)P, (unsigned long long)plen);
    n_alloc = extent_allocated(v, P, &plen);
    ok(n_alloc == plen, "capture #1: P is allocated and pinned");
    dump("reference", a, file_sz);

    /* THE DONOR, copied out of the image NOW, whole: segment 1 of the same
     * file, so it is the same length and the same codec, but different bytes.
     * It has to be the WHOLE extent and it has to be taken while the extent is
     * intact -- the reclaim frees every segment of this file, so after it runs
     * the donor's framed header is gone and its length is unknowable. */
    donor_pba = pa[1];
    extent_allocated(v, donor_pba, &donor_plen);
    ok(donor_plen > 0, "resolved the donor segment's extent length");
    if (donor_plen > 0) {
        donor = (uint8_t *)malloc((size_t)donor_plen * INVFS_BLOCK_SIZE);
        if (donor)
            ok(io_seek(&v->io, donor_pba * (uint64_t)INVFS_BLOCK_SIZE) == 0 &&
               io_read(&v->io, donor,
                       (size_t)donor_plen * INVFS_BLOCK_SIZE) == 0,
               "copied the donor segment out of the image");
    }

    /* ---- what the sweep does between two captures: drop the old window, then
     * capture a fresh one (tools/invf-sweep.c:1700-1706). */
    spt0_drop(v);

    /* ---- CAPTURE #2, with the recipe load made to fail once. */
    if (armed) {
        snprintf(spec, sizeof spec, "v3_recipe_load:%d", n);
        setenv("INVFS_FAULT", spec, 1);
    }
    invfs_vol_btree_fault_reload();
    rc = spt0_capture(v);
    if (armed) {
        unsetenv("INVFS_FAULT");
        invfs_vol_btree_fault_reload();
    }
    ok(rc == 0, "capture #2: the save point was captured");

    /* THE PROOF THAT THE FAULT WAS TRANSIENT, and the reason this leg can use
     * an injected read failure as its trigger at all: the very next read of
     * the same recipe succeeds, on the same volume, with the same bytes. So
     * the file is not damaged -- only what the capture did with it was. */
    ok(names_pba(v, "file_a.bin", P),
       "capture #2: file_a.bin's recipe loads again immediately afterwards");

    /* ---- THE ASSERTION. Pre-fix this is the whole finding: P was in
     * old_map (capture #1 pinned it), is not in the new map (the walk skipped
     * the recipe), is allocated and is not registry-owned -- so
     * spn_reclaim freed it, with a live recipe naming it. */
    n_alloc = extent_allocated(v, P, &p2);
    printf("  [%s] after capture #2: P=%llu has %llu of %llu extent blocks "
           "still allocated\n", tag, (unsigned long long)P,
           (unsigned long long)n_alloc, (unsigned long long)p2);
    if (armed)
        ok(n_alloc == p2,
           "the extent a live recipe names survives a capture whose walk "
           "could not read that recipe (pre-fix: freed under file_a.bin)");

    if (n_alloc < p2 && donor && donor_plen) {
        /* Put somebody ELSE's valid segment where file_a.bin's bytes were.
         *
         * A scribble would be caught by a length check, so the damage has to
         * be a REAL segment of the same length: the donor is verbatim, so its
         * framed CRC is self-consistent, seg_read_checked passes, ast_frame_ok
         * passes (a RAW entry's csize equals its length either way), and the
         * read path hands file_a.bin the DONOR's bytes with no error at all.
         * That is the whole point of the demonstration -- and it is exactly
         * what invf-verify --deep cannot see, because it checks readability
         * and length only (src/cli/verify.c:357-364). */
        /* plen, not p2: p2 is read AFTER the free, when the framed header is
         * gone, so it degenerates to the single-block fallback. plen is the
         * extent length measured while the extent was intact. */
        uint64_t got = alloc_blocks(v, P, plen, 1, 1, INVFS_ALLOC_DATA);
        printf("  [%s] alloc_blocks(%llu,%llu) -> %llu\n", tag,
               (unsigned long long)P, (unsigned long long)plen,
               (unsigned long long)got);
        if (got == P && plen == donor_plen) {
            io_pwrite(&v->io, got * (uint64_t)INVFS_BLOCK_SIZE, donor,
                      (size_t)donor_plen * INVFS_BLOCK_SIZE);
            printf("  [%s] laid segment %llu (%llu blocks) over P=%llu -- a "
                   "VALID segment, the same length, different bytes\n", tag,
                   (unsigned long long)donor_pba,
                   (unsigned long long)donor_plen, (unsigned long long)P);
            ok(1, "P was handed straight back out and now holds another "
                  "segment");
        } else {
            ok(0, "P is handed straight back out, whole, to the next writer");
        }
    }
    oracle(v, "file_a.bin", a, file_sz,
           "file_a.bin byte-exact after the capture that could not read its "
           "recipe");
    vol_flush(v);
    vol_close(v);
    free(a);
    free(donor);

    /* Did the walk really skip THIS file's recipe? The reclaim's behaviour is
     * only meaningful if it did, and this is what stops the leg going green on
     * a countdown that never fired.
     *
     * The discriminator is the PIN, not the damage: spt0_block_pinned asks the
     * live mark set whether it still holds P, and on a one-file volume P is in
     * that set exactly when the walk read the recipe. That reads the same in
     * both worlds -- pre-fix and post-fix, a fired fault leaves P unpinned --
     * so the search is a measurement of the fault and not of the fix. */
    fired = armed && !spt0_block_pinned(v, P, plen);
    if (armed && !fired)
        printf("  [%s] INVFS_FAULT=\"%s\" did not change anything\n", tag, spec);
    return fired;
}

int main(int argc, char **argv)
{
    const char *phase = (argc > 2) ? argv[2] : "skipped";

    if (argc > 1) g_dir = argv[1];
    snprintf(g_img, sizeof g_img, "%s/spn-skip.img", g_dir);
    printf("spn_skip_recipe_test: phase=%s img=%s\n", phase, g_img);

    if (!strcmp(phase, "skipped")) {
        int n, found = 0;
        for (n = 1; n <= 16 && !found; n++)
            if (attempt(1, n)) found = n;
        ok(found != 0,
           "one failed recipe read during the capture walk is enough to free a "
           "live recipe's block (this assertion is what stops the leg going "
           "green on a countdown that never fired)");
        if (!found)
            printf("  [skipped] no countdown in 1..16 changed the reclaim; the "
                   "leg is vacuous and this is a FAILURE, not a pass\n");
    } else if (!strcmp(phase, "skippedctl")) {
        /* the identical sequence with the fault OFF. Pre-fix this passes and
         * post-fix it passes; if it ever showed the same damage, the scenario
         * would be the defect and the armed leg would prove nothing. */
        attempt(0, 0);
        ok(1, "control: the identical sequence with the fault off");
    } else {
        fprintf(stderr, "unknown phase %s\n", phase);
        return 2;
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}