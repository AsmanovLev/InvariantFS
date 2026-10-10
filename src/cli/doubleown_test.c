/* doubleown_test.c — WP303: fsck must see row/recipe incoherence, the
 * visible seam of double ownership.
 *
 * The gap this exists for: two live inodes naming one data block (double
 * ownership -- the shape the leg2 root cause produced transiently) passed
 * `invf-fsck` silent. The volume reported OK while one file was already
 * unreadable: its row disagreed with its recipe, and the read path
 * (vol_read_inode: row size != recipe size -> -1) refuses such files while
 * no checker asked. The fix (vol_recipe_coherence + the recipe audit's
 * INVFS_RECIPE_BAD_INCOHERENT kind + attribution in vol_fsck.c) mirrors
 * the read path's own predicates: row size == recipe header size, entries
 * inside it without overlap, entries covering exactly the row's size.
 *
 * What SHARING is and is not here: this test never verdicts on sharing
 * alone. Dedupe legitimately leaves two agreeing rows on one blob (leg D
 * pins that: same size, same blob, audit silent -- informationally
 * identical to deduped identical files, so no offline checker can tell
 * them apart; the leg-8-style documented trap). What IS a verdict is the
 * disagreement: a row pointed at another file's recipe, or a recipe whose
 * entries do not tile its row. The double-owned block is named as
 * attribution on that verdict (inode pair + pba), never as a verdict.
 *
 * Legs:
 *   A  two sound files (different sizes) + empty + dir + symlink
 *      -> audit clean (checked=2: only content files count), fsck clean
 *   B  a second inode published on A's blob with a DIFFERENT row size
 *      (the real v3 publish pair, as in pbaref_test's hookctl)
 *      -> audit DETECTS (bad=1, kind INCOHERENT, id+name), fsck DAMAGED
 *   B2 the audit tracks the read path: C unreadable, A bit-exact
 *   C  unlink the sharer -> clean again (detection, not a sticky verdict)
 *   D  same-size sharer -> audit SILENT (documented limit: this shape is
 *      dedupe-identical), reads back A's bytes
 *   E  hand-built short-coverage recipe (same header, trimmed tail entry)
 *      -> INCOHERENT via the tiling sum; the read path returns 0 here
 *      (it only enforces bounds/overlap, not coverage), which the leg
 *      records rather than hides
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_delta.h"

/* Red-leg compatibility: the INCOHERENT kind arrives with the fix. Defined
 * here so this test still COMPILES against the unpatched engine -- where
 * it must fail its assertions (no detection), not fail its build. */
#ifndef INVFS_RECIPE_BAD_INCOHERENT
#define INVFS_RECIPE_BAD_INCOHERENT 3
#endif

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  OK    %s\n", what);
    }
}

static invfs_volume *g_v;
static char g_img[512];

static uint8_t *mk_pattern(size_t n, unsigned seed)
{
    uint8_t *b = (uint8_t *)malloc(n ? n : 1);
    size_t i;
    if (!b)
        return NULL;
    srand(seed);
    for (i = 0; i < n; i++)
        b[i] = (uint8_t)(rand() & 0xFF);
    return b;
}

/* The audit as the check, with the numbers spelled out. */
static void expect_clean(const char *what, uint64_t checked)
{
    invfs_recipe_audit a;
    invfs_fsck_report rep;
    char b[192];
    int rc = vol_recipe_audit(g_v, &a);
    snprintf(b, sizeof b, "%s: audit completes", what);
    ok(rc == 0, b);
    if (rc != 0)
        return;
    snprintf(b, sizeof b, "%s: no offenders (bad=%llu of checked=%llu)",
             what, (unsigned long long)a.nfault_total,
             (unsigned long long)a.checked);
    ok(a.nfault_total == 0, b);
    snprintf(b, sizeof b, "%s: %llu live inode(s) with content checked",
             what, (unsigned long long)a.checked);
    ok(a.checked == checked, b);

    memset(&rep, 0, sizeof rep);
    if (vol_fsck_scan(g_v, &rep, 0) != 0) {
        snprintf(b, sizeof b, "%s: fsck scan completes", what);
        ok(0, b);
        return;
    }
    snprintf(b, sizeof b, "%s: fsck reports no unreadable recipe", what);
    ok(rep.recipe_bad == 0 && !rep.recipe_partial, b);
    snprintf(b, sizeof b, "%s: fsck does not call the volume damaged", what);
    ok(!rep.damaged, b);
}

static void expect_incoherent(const char *what, uint64_t id, const char *name)
{
    invfs_recipe_audit a;
    invfs_fsck_report rep;
    char b[224];
    size_t i;
    int found = 0, named = 0, kind_ok = 0;

    if (vol_recipe_audit(g_v, &a) != 0) {
        snprintf(b, sizeof b, "%s: audit completes", what);
        ok(0, b);
        return;
    }
    snprintf(b, sizeof b, "%s: the incoherent recipe is DETECTED "
             "(bad=%llu of checked=%llu)", what,
             (unsigned long long)a.nfault_total,
             (unsigned long long)a.checked);
    ok(a.nfault_total == 1, b);
    for (i = 0; i < a.nfault; i++) {
        if (a.fault[i].id == id) {
            found = 1;
            if (!strcmp(a.fault[i].name, name))
                named = 1;
            if (a.fault[i].kind == INVFS_RECIPE_BAD_INCOHERENT)
                kind_ok = 1;
        }
    }
    snprintf(b, sizeof b, "%s: the message names inode %llu", what,
             (unsigned long long)id);
    ok(found, b);
    snprintf(b, sizeof b, "%s: the message names it as %s", what, name);
    ok(named, b);
    snprintf(b, sizeof b, "%s: it is classified INCOHERENT (not MISSING, "
             "not CORRUPT)", what);
    ok(kind_ok, b);

    /* The entry point the operator runs. */
    memset(&rep, 0, sizeof rep);
    if (vol_fsck_scan(g_v, &rep, 0) != 0) {
        snprintf(b, sizeof b, "%s: fsck scan completes", what);
        ok(0, b);
        return;
    }
    snprintf(b, sizeof b, "%s: fsck counts 1 unreadable recipe", what);
    ok(rep.recipe_bad == 1, b);
    snprintf(b, sizeof b, "%s: fsck marks the volume DAMAGED", what);
    ok(rep.damaged, b);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    uint8_t *pa = NULL, *pb = NULL;
    size_t asz = 200000, bsz = 100000;
    uint64_t id_a = 0, id_b = 0, id_c = 0, id_d = 0, id_e = 0;
    int err = 0;

    printf("doubleown_test: fsck must see row/recipe incoherence\n");

    snprintf(g_img, sizeof g_img, "%s/invf-doubleown-test.img", dir);
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
        fprintf(stderr, "doubleown_test: vol_open failed: err=%d\n", err);
        return 2;
    }

    pa = mk_pattern(asz, 303031);
    pb = mk_pattern(bsz, 909090);
    if (!pa || !pb) {
        printf("  out of memory\n");
        return 2;
    }

    /* ---- leg A: a sound volume (empty/dir/symlink never counted) ----- */
    id_a = vol_write_bulk(g_v, "a.bin", pa, asz, NULL);
    id_b = vol_write_bulk(g_v, "b.bin", pb, bsz, NULL);
    ok(id_a && id_b && id_a != id_b, "leg A: two files on two inodes");
    {
        invfs_meta_pub m;
        uint64_t id_e0, id_tiny;
        memset(&m, 0, sizeof m);
        m.type = INVFS_ITYP_REG;
        m.mode = 0644;
        id_e0 = vol_write_bulk(g_v, "empty", NULL, 0, &m);
        ok(id_e0 != 0, "leg A: empty file created");
        ok(vol_mkdir(g_v, "sub") != 0, "leg A: directory created");
        /* Raw-blob symlink/dir/empty skips are recipe_fsck_test's beat;
         * here empty+dir pin the count: only content files are checked. */
        id_tiny = vol_write_bulk(g_v, "tiny", (const uint8_t *)"t", 1,
                                 NULL);
        ok(id_tiny != 0, "leg A: 1-byte file created");
    }
    expect_clean("leg A (sound volume)", 3);
    {
        uint8_t *d = NULL;
        size_t n = 0;
        int rc = vol_read_inode(g_v, id_a, 0, &d, &n);
        ok(rc == 0 && n == asz && memcmp(d, pa, asz) == 0,
           "leg A: a.bin bit-exact");
        free(d);
    }

    /* ---- leg B: second live owner on A's blob, different row size ---- */
    {
        invfs_inode in;
        uint8_t *blob = NULL;
        size_t blen = 0;
        uint8_t addr[INVFS_RECIPE_ADDR_LEN];
        ok(vol_inode_get(g_v, id_a, &in) == 1, "leg B: A's row reads");
        ok(vol_recipe_load(g_v, in.recipe_addr, &blob, &blen) == 0 && blob,
           "leg B: A's blob loads");
        ok(vol_recipe_store(g_v, blob, blen, addr) == 0,
           "leg B: same bytes re-store at the same address");
        ok(memcmp(addr, in.recipe_addr, sizeof addr) == 0,
           "leg B: content-blind key collides (one blob, two rows)");
        free(blob);
        /* Half A's size: the row claims 100000, the recipe says 200000. */
        id_c = vol_create_content_node(g_v, "c.bin", 100000, addr);
        ok(id_c != 0 && id_c != id_a,
           "leg B: second inode published on A's blob");
    }
    expect_incoherent("leg B (c.bin shares A's blob at the wrong size)",
                      id_c, "c.bin");

    /* ---- leg B2: the audit tracks what the read path sees ------------- */
    {
        uint8_t *d = NULL;
        size_t n = 0;
        int rc = vol_read_inode(g_v, id_c, 0, &d, &n);
        ok(rc != 0, "leg B2: the read path refuses c.bin too");
        free(d);
        d = NULL;
        n = 0;
        rc = vol_read_inode(g_v, id_a, 0, &d, &n);
        ok(rc == 0 && n == asz && memcmp(d, pa, asz) == 0,
           "leg B2: a.bin untouched (no collateral)");
        free(d);
    }

    /* ---- leg C: unlink the sharer -> clean again ---------------------- */
    ok(vol_unlink(g_v, "c.bin") == 0, "leg C: c.bin unlinked");
    expect_clean("leg C (sharer gone)", 3);

    /* ---- leg D: same-size sharer -- the documented silent shape ------- */
    {
        invfs_inode in;
        uint8_t *blob = NULL;
        size_t blen = 0;
        uint8_t addr[INVFS_RECIPE_ADDR_LEN];
        uint64_t pba0 = 0;
        ok(vol_inode_get(g_v, id_a, &in) == 1, "leg D: A's row reads");
        ok(vol_recipe_load(g_v, in.recipe_addr, &blob, &blen) == 0 && blob,
           "leg D: A's blob loads");
        {
            invfs_ast_hdr ah;
            const invfs_ast_block_entry *ents = NULL;
            size_t nn = 0;
            if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &nn) == 0 &&
                ents && nn > 0)
                pba0 = ents[0].pba;
        }
        ok(vol_recipe_store(g_v, blob, blen, addr) == 0,
           "leg D: blob re-stored");
        free(blob);
        /* SAME size: row and recipe agree everywhere. */
        id_d = vol_create_content_node(g_v, "d.bin", asz, addr);
        ok(id_d != 0 && id_d != id_a,
           "leg D: same-size sharer published");
        printf("  note leg D: d.bin shares A's blob (first pba %llu); this "
               "is dedupe-identical, so no offline checker can tell them "
               "apart\n", (unsigned long long)pba0);
    }
    {
        invfs_recipe_audit a;
        char b[192];
        ok(vol_recipe_audit(g_v, &a) == 0, "leg D: audit completes");
        snprintf(b, sizeof b,
                 "leg D: audit stays silent on agreeing sharers (bad=%llu)",
                 (unsigned long long)a.nfault_total);
        ok(a.nfault_total == 0, b);
    }
    {
        uint8_t *d = NULL;
        size_t n = 0;
        int rc = vol_read_inode(g_v, id_d, 0, &d, &n);
        ok(rc == 0 && n == asz && memcmp(d, pa, asz) == 0,
           "leg D: d.bin reads back A's bytes (the silent trap, pinned)");
        free(d);
    }
    ok(vol_unlink(g_v, "d.bin") == 0, "leg D: sharer removed");

    /* ---- leg E: short-coverage recipe (header agrees, tiling does not) */
    {
        invfs_inode in;
        uint8_t *blob = NULL;
        size_t blen = 0;
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents = NULL;
        size_t nn = 0;
        invfs_ast_block_entry *mod = NULL;
        uint8_t *nblob = NULL;
        size_t nblen = 0;
        uint8_t naddr[INVFS_RECIPE_ADDR_LEN];
        ok(vol_inode_get(g_v, id_a, &in) == 1, "leg E: A's row reads");
        ok(vol_recipe_load(g_v, in.recipe_addr, &blob, &blen) == 0 && blob,
           "leg E: A's blob loads");
        ok(vol_ast_recipe_parse(blob, blen, &ah, &ents, &nn) == 0 && ents &&
           nn > 1, "leg E: A's recipe parses with >1 entry");
        mod = (invfs_ast_block_entry *)malloc(nn * sizeof *mod);
        ok(mod != NULL, "leg E: scratch for the modified recipe");
        if (!mod) {
            free(blob);
            printf("  out of memory\n");
            return 2;
        }
        /* Drop the tail entry: the survivors stay inside the header,
         * stay ordered, and keep the lengths their stored segments
         * decode to -- but the entries no longer cover the row. (Trimming
         * a length instead would trip the codec's own got==length check;
         * the dropped tail is the shape the decode cannot see.) */
        memcpy(mod, ents, nn * sizeof *mod);
        ok(vol_ast_recipe_serialize(ah.file_size, mod, (uint32_t)(nn - 1),
                                    &nblob, &nblen) == 0,
           "leg E: short-coverage recipe serializes");
        free(mod);
        free(blob);
        if (!nblob) {
            printf("  serialize failed\n");
            return 2;
        }
        ok(vol_recipe_store(g_v, nblob, nblen, naddr) == 0,
           "leg E: short recipe stored");
        free(nblob);
        id_e = vol_create_content_node(g_v, "e.bin", asz, naddr);
        ok(id_e != 0, "leg E: e.bin published on the short recipe");
    }
    expect_incoherent("leg E (e.bin's entries do not cover its row)",
                      id_e, "e.bin");
    {
        uint8_t *d = NULL;
        size_t n = 0;
        int rc = vol_read_inode(g_v, id_e, 0, &d, &n);
        /* The read path enforces bounds/overlap, not coverage: it answers
         * 0 with a garbage tail. Recorded, not hidden. */
        printf("  note leg E: read path on the short recipe rc=%d len=%zu\n",
               rc, n);
        ok(rc == 0 && n == asz, "leg E: read path does not catch this one");
        free(d);
    }
    ok(vol_unlink(g_v, "e.bin") == 0, "leg E: e.bin unlinked");
    expect_clean("leg E (short recipe gone)", 3);

    free(pa);
    free(pb);
    vol_flush(g_v);
    vol_close(g_v);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
