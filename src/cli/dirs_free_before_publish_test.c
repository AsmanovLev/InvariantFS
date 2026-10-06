/*
 * dirs_free_before_publish_test.c — WP wp/dirs-free-before-publish
 *
 * THE FINDING. vol_create_node (src/core/vol_dirs.c:319) frees the
 * existing inode's data blocks BEFORE it republishes the inode row, and four
 * `return 0` sites sit between the free and the publish. Every one of them
 * leaves a LIVE row naming blocks that are now free and re-allocatable. Its
 * own twin, vol_unlink (:600), deletes the row first and frees second, and
 * says why at :640-644: freeing first "would let a failure between the two
 * leave a LIVE row pointing at freed blocks, which is a bit-exactness
 * violation and strictly worse than the leak this reports."
 *
 * Reachability is ordinary use: vol_replace_file dispatches an O_TRUNC
 * open(2) (src/cli/fuse_fs.c:1541) and an empty create to
 * vol_create_node(vol_dirs.c:990). The trigger is a volume that cannot
 * append the delta record -- a volume filling up, the normal end of every
 * filesystem's life.
 *
 * WHY THIS IS TWO PHASES AND NOT ONE. The injector (src/core/vol_fault.h)
 * is `static inline` with function-local statics, so its state is per
 * translation unit and it only re-reads INVFS_FAULT when getenv() returns a
 * DIFFERENT POINTER. Re-arming mid-process by setenv()-ing the same string
 * is therefore not reliable, and setenv()-ing a different one frees the
 * old value the cache still points at. Rather than add a reload helper to a
 * shared header to work around that, the setup runs in its OWN PROCESS:
 * `setup` writes victim.bin and closes; `red` then opens with the arming
 * already in its environment, so the FIRST inode_delta_put call in
 * the process is the truncate's. No re-arm, no reload helper, no cached
 * state. The `ok(rc == 0)` assertion in the `red` leg is what PROVES that:
 * if the armed call were not the truncate's, the truncate would succeed and
 * this check fails loudly instead of the arm passing vacuously.
 *
 * THE ORACLE IS BYTES AND ADDRESSES, NEVER invf-verify --deep. An
 * invfs_ast_block_entry carries a pba, not a content hash
 * (src/core/invarifs.h:970-980), so a recipe resolving to a valid-but-wrong
 * segment of the right length prints "N files ok, 0 corrupt". Two oracles
 * are used instead:
 *
 *   1. ADDRESS. Read the live row's recipe, parse it, and read the bitmap
 *      bit for every pba it names. A row naming a free block is the defect,
 *      stated in the units the defect lives in.
 *   2. BYTES. vol_read_inode the file back and memcmp against the source
 *      pattern -- the consequence: once the blocks are re-handed to another
 *      file, this file reads THAT file's bytes.
 *
 * Phases (argv[2]):
 *
 *   setup    mkfs, write victim.bin, record the pbas its recipe names.
 *   hookctl  prove the injector fires on inode_delta_put and is
 *            one-shot, so `red` cannot pass vacuously.
 *   red      O_TRUNC with the armed publish. Red without the fix.
 *   ok       the pre-existing safe path: the truncate must still SUCCEED,
 *            the old blocks must still be freed AND re-used by the next
 *            write (so a fix that never frees fails here), and the file
 *            must still be bit-exact.
 *
 * argv[1] is a scratch directory; $INVFS_TEST_SCRATCH wins if set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>

#include "invarifs.h"
#include "volume_internal.h"

#define MAXPB 64

static int checks = 0, failures = 0;
static char g_dir[400];
static char g_img[440];

static void ok(int cond, const char *fmt, ...)
{
    va_list ap;
    checks++;
    printf("  %s  ", cond ? "OK  " : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (!cond) failures++;
}

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* ---- what does the LIVE row name? ---- */

static int inode_row(invfs_volume *v, const char *path, uint64_t *id_out,
                     invfs_inode *in_out)
{
    uint64_t id = 0;
    invfs_inode in;

    if (vol_path_lookup(v, path, &id) != 1) return -1;
    if (vol_inode_get(v, id, &in) != 1) return -1;
    if (id_out) *id_out = id;
    if (in_out) *in_out = in;
    return 0;
}

/* Every non-TEXT pba the LIVE row for `path` names. zone==TEXT entries are
 * shared batch segments owned by the batch registry, and
 * vol_free_recipe_blocks deliberately skips them (vol_ast.c:159-161), so
 * counting them would report blocks this path never freed. */
static int recipe_pbas(invfs_volume *v, const char *path,
                       uint64_t *out, int max, int *n_out)
{
    invfs_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n = 0, i;
    int m = 0;

    *n_out = 0;
    if (inode_row(v, path, NULL, &in) != 0) return -1;
    if (in.size == 0) return 0;
    if (vol_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob)
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

static int shares_pba(const uint64_t *a, int na, const uint64_t *b, int nb)
{
    int i, j, found = 0;
    for (i = 0; i < na; i++)
        for (j = 0; j < nb; j++)
            if (a[i] == b[j]) found++;
    return found;
}

/* ---- the corpus. Two files, same size, disjoint 6-bit residue classes, so
 * a read-back that matches the wrong one is unambiguous at byte 0. ---- */

#define NSEG 8
static const size_t FILE_SZ = NSEG * SEGMENT_SIZE;

static uint8_t *mk_file(int which)
{
    uint8_t *d = (uint8_t *)malloc(FILE_SZ);
    size_t i;
    assert(d);
    for (i = 0; i < FILE_SZ; i++)
        d[i] = which ? (uint8_t)(((i * 61 + 7) & 0x3F) | 0x40)
                     : (uint8_t)(((i * 7) + (i >> 8) * 13 + 3) & 0x3F);
    return d;
}

static void dump_bytes(const char *tag, const uint8_t *b, size_t n)
{
    char p[500];
    FILE *f;
    snprintf(p, sizeof p, "%s/frb_%s.bin", g_dir, tag);
    f = fopen(p, "wb");
    if (f) { fwrite(b, 1, n, f); fclose(f); }
    printf("  [dump] %s\n", p);
}

/* ---- cross-phase persistence of victim.bin's pbas ---- */

static void pba_save(const uint64_t *p, int n)
{
    char path[500];
    FILE *f;
    int i;
    snprintf(path, sizeof path, "%s/frb_pbas.txt", g_dir);
    f = fopen(path, "w");
    if (!f) { fprintf(stderr, "pba_save: cannot write %s\n", path); exit(2); }
    fprintf(f, "%d\n", n);
    for (i = 0; i < n; i++) fprintf(f, "%llu\n", (unsigned long long)p[i]);
    fclose(f);
}

/* Read back the pbas the `setup` phase recorded. A partial read exits rather
 * than silently zeroing the oracle -- an all-zero pba list would make every
 * bitmap check vacuously pass. */
static int pbas_read(uint64_t *out, int max)
{
    char path[500];
    FILE *f;
    int n = 0, i;
    snprintf(path, sizeof path, "%s/frb_pbas.txt", g_dir);
    f = fopen(path, "r");
    if (!f) { fprintf(stderr, "pbas_read: %s missing -- run `setup` first\n", path); exit(2); }
    if (fscanf(f, "%d", &n) != 1 || n <= 0) { fclose(f); exit(2); }
    if (n > max) n = max;
    for (i = 0; i < n; i++)
        if (fscanf(f, "%llu", (unsigned long long *)&out[i]) != 1) {
            fprintf(stderr, "pbas_read: short read at %d\n", i);
            exit(2);
        }
    fclose(f);
    return n;
}

/* ---- volume plumbing ---- */

static invfs_volume *open_vol(void)
{
    int err = 0;
    invfs_volume *v = vol_open(g_img, &err);
    if (!v) { fprintf(stderr, "vol_open(%s) failed: %d\n", g_img, err); exit(2); }
    return v;
}

static void mkfs_fresh(void)
{
    char cmd[800];
    unlink(g_img);
    snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 64 >/dev/null 2>&1", g_img);
    if (system(cmd) != 0) { fprintf(stderr, "mkfs failed\n"); exit(2); }
}

/* How many of `pbas` are FREE in the bitmap, i.e. re-allocatable by the very
 * next write. bit_get is the in-RAM bitmap that vol_free_blocks clears. */
static int count_free(invfs_volume *v, const uint64_t *pbas, int n,
                      uint64_t *first_free)
{
    int i, c = 0;
    for (i = 0; i < n; i++)
        if (!bit_get(v->bitmap, pbas[i])) {
            if (c == 0 && first_free) *first_free = pbas[i];
            c++;
        }
    return c;
}

static void print_row(invfs_volume *v, const char *label, const char *path,
                      const uint8_t *old_addr)
{
    invfs_inode in;
    uint64_t id = 0, pa[MAXPB];
    int na = 0, i, nfree;

    if (inode_row(v, path, &id, &in) != 0) {
        note("  [%s] %s: NO LIVE ROW\n", label, path);
        return;
    }
    recipe_pbas(v, path, pa, MAXPB, &na);
    nfree = count_free(v, pa, na, NULL);
    note("  [%s] %s: inode=%llu type=%d size=%llu recipe_addr=%s\n",
         label, path, (unsigned long long)id, (int)in.type,
         (unsigned long long)in.size,
         old_addr ? (memcmp(in.recipe_addr, old_addr,
                            INVFS_RECIPE_ADDR_LEN) == 0
                        ? "UNCHANGED (still the pre-truncate recipe)"
                        : "moved") : "?");
    note("  [%s] %s: row names %d pba(s), %d of them FREE in the bitmap\n",
         label, path, na, nfree);
    for (i = 0; i < na; i++)
        note("      pba %llu  %s  pba_ref_count=%u\n",
             (unsigned long long)pa[i],
             bit_get(v->bitmap, pa[i]) ? "allocated" : "*** FREE ***",
             pba_ref_count(v, pa[i]));
}

/* ---- THE BIT-EXACTNESS ORACLE ---- */
static int oracle(invfs_volume *v, const char *name, const uint8_t *want,
                  size_t want_len, const char *tag)
{
    uint64_t id = 0;
    uint8_t *buf = NULL;
    size_t len = 0, i, firstbad = want_len;
    int rc, same;

    if (vol_path_lookup(v, name, &id) != 1) {
        printf("  [oracle %s] %s: NAME IS GONE\n", tag, name);
        ok(0, "%s", tag);
        return 0;
    }
    rc = vol_read_inode(v, id, 0, &buf, &len);
    if (rc != 0) {
        printf("  [oracle %s] %s: READ FAILED (rc=%d)\n", tag, name, rc);
        ok(0, "%s", tag);
        return 0;
    }
    same = (len == want_len) && (want_len == 0 || memcmp(buf, want, len) == 0);
    if (same) {
        printf("  [oracle %s] %s: %zu bytes BYTE-EXACT vs source\n",
               tag, name, len);
    } else {
        for (i = 0; i < len && i < want_len; i++)
            if (buf[i] != want[i]) { firstbad = i; break; }
        printf("  [oracle %s] %s: *** NOT byte-exact *** len=%zu want=%zu "
               "first differing byte %zu (got 0x%02x want 0x%02x)\n",
               tag, name, len, want_len, firstbad,
               firstbad < len ? buf[firstbad] : 0,
               firstbad < want_len ? want[firstbad] : 0);
    }
    ok(same, "%s", tag);
    free(buf);
    return same;
}

/* ==================================================================== */
/* setup: a healthy volume with one non-empty file                       */
/* ==================================================================== */
static int leg_setup(void)
{
    uint8_t *a = mk_file(0);
    uint64_t pa[MAXPB];
    int n = 0;
    invfs_volume *v;

    printf("== leg setup: write victim.bin (%zu bytes) ==\n", FILE_SZ);
    mkfs_fresh();
    v = open_vol();
    ok(vol_write_bulk(v, "victim.bin", a, FILE_SZ, NULL) != 0,
       "wrote victim.bin");
    ok(recipe_pbas(v, "victim.bin", pa, MAXPB, &n) == 0 && n > 0,
       "victim.bin's recipe names %d pba(s)", n);
    if (n <= 0) { fprintf(stderr, "no pbas -- cannot test\n"); return 1; }
    oracle(v, "victim.bin", a, FILE_SZ,
           "victim.bin is byte-exact before the truncate");
    print_row(v, "setup", "victim.bin", NULL);
    pba_save(pa, n);
    dump_bytes("victim_source", a, FILE_SZ);
    vol_flush(v);
    vol_close(v);
    free(a);
    return failures;
}

/* ==================================================================== */
/* hookctl: the injector really fires, and really is one-shot            */
/* ==================================================================== */
static int leg_hookctl(void)
{
    uint8_t *a = mk_file(0);
    uint64_t rc;
    invfs_volume *v;

    printf("== leg hookctl: the injector really fires on inode_delta_put ==\n");
    mkfs_fresh();
    setenv("INVFS_FAULT", "inode_delta_put:1", 1);
    v = open_vol();
    rc = vol_write_bulk(v, "ctl.bin", a, FILE_SZ, NULL);
    ok(rc == 0, "a plain write is refused when inode_delta_put:1 is armed");
    /* one-shot: the very next attempt takes the normal path */
    rc = vol_write_bulk(v, "ctl.bin", a, FILE_SZ, NULL);
    ok(rc != 0, "the fault is one-shot: the next write succeeds");
    oracle(v, "ctl.bin", a, FILE_SZ,
           "the file written after the one-shot fault is byte-exact");
    vol_flush(v);
    vol_close(v);
    free(a);
    return failures;
}

/* ==================================================================== */
/* red: the truncate's publish fails, and the row it leaves is the bug    */
/* ==================================================================== */
static int leg_red(void)
{
    uint8_t *a = mk_file(0), *b = mk_file(1);
    uint8_t old_addr[INVFS_RECIPE_ADDR_LEN];
    uint64_t old_pba[MAXPB], new_pba[MAXPB];
    int n_old, n_new = 0, nfree, nshared;
    uint64_t first_free = 0, rc;
    invfs_volume *v;

    printf("== leg red: an O_TRUNC whose delta append fails ==\n");
    n_old = pbas_read(old_pba, MAXPB);
    printf("  [recorded] victim.bin's pre-truncate pbas:");
    { int i; for (i = 0; i < n_old; i++)
          printf(" %llu", (unsigned long long)old_pba[i]); }
    printf("\n");

    /* The arming is in this process's environment BEFORE vol_open, so the
     * first inode_delta_put call in the process is the truncate's.
     * The rc == 0 assertion below is what proves it. */
    setenv("INVFS_FAULT", "inode_delta_put:1", 1);
    v = open_vol();

    {
        invfs_inode in;
        if (inode_row(v, "victim.bin", NULL, &in) != 0) {
            fprintf(stderr, "victim.bin is gone -- run `setup` first\n");
            return 2;
        }
        memcpy(old_addr, in.recipe_addr, sizeof old_addr);
    }
    oracle(v, "victim.bin", a, FILE_SZ,
           "victim.bin is byte-exact when this process starts");
    print_row(v, "before", "victim.bin", old_addr);

    rc = vol_replace_file(v, "victim.bin", NULL, 0);   /* open(O_TRUNC) */
    /* If the armed call were not the truncate's, the truncate would SUCCEED
     * and this fails loudly rather than the arm passing vacuously. */
    ok(rc == 0, "the armed delta publish failed: vol_replace_file returned 0");
    if (rc != 0) {
        printf("  !! arm is vacuous: the truncate SUCCEEDED, so nothing here\n"
               "     is evidence about a failed publish\n");
        goto done;
    }

    print_row(v, "after-failed-trunc", "victim.bin", old_addr);

    /* ASSERTION 1 (ADDRESS): no block a LIVE row names is free */
    nfree = count_free(v, old_pba, n_old, &first_free);
    if (nfree)
        note("  *** %d of %d pbas the LIVE row names are FREE, first at pba "
             "%llu -- re-allocatable by the next write ***\n",
             nfree, n_old, (unsigned long long)first_free);
    ok(nfree == 0,
       "RED-1 (address): after the failed truncate, no pba the LIVE row "
       "names is free");

    /* ASSERTION 2 (BYTES): the consequence */
    if (vol_write_bulk(v, "thief.bin", b, FILE_SZ, NULL) == 0) {
        printf("  (thief.bin write refused -- volume full; ASSERTION 1 "
               "already stands)\n");
    } else {
        recipe_pbas(v, "thief.bin", new_pba, MAXPB, &n_new);
        nshared = shares_pba(old_pba, n_old, new_pba, n_new);
        if (nshared)
            note("  *** thief.bin was handed %d pba(s) the LIVE victim.bin "
                 "row still names ***\n", nshared);
        ok(nshared == 0,
           "RED-2 (address): the next write was not handed a block the LIVE "
           "row names");
        oracle(v, "victim.bin", a, FILE_SZ,
               "RED-3 (BYTES): victim.bin still reads its own bytes after the "
               "failed truncate");
        oracle(v, "thief.bin", b, FILE_SZ, "control: thief.bin is byte-exact");
        {
            uint64_t id; uint8_t *buf; size_t len;
            if (vol_path_lookup(v, "victim.bin", &id) == 1 &&
                vol_read_inode(v, id, 0, &buf, &len) == 0) {
                dump_bytes("victim_readback", buf, len);
                free(buf);
            }
        }
        dump_bytes("thief_source", b, FILE_SZ);
    }

done:
    vol_flush(v);
    vol_close(v);
    free(a);
    free(b);
    return failures;
}

/* ==================================================================== */
/* ok: the truncate still works, still reclaims, still bit-exact          */
/* ==================================================================== */
static int leg_ok(void)
{
    uint8_t *a = mk_file(0), *b = mk_file(1), *c;
    uint8_t old_addr[INVFS_RECIPE_ADDR_LEN];
    uint64_t old_pba[MAXPB], new_pba[MAXPB];
    int n_old, n_new = 0, nfree, nshared;
    uint64_t rc;
    invfs_volume *v;

    printf("== leg ok: an O_TRUNC that succeeds ==\n");
    v = open_vol();
    n_old = pbas_read(old_pba, MAXPB);
    {
        invfs_inode in;
        inode_row(v, "victim.bin", NULL, &in);
        memcpy(old_addr, in.recipe_addr, sizeof old_addr);
    }
    ok(vol_write_bulk(v, "bystander.bin", b, FILE_SZ, NULL) != 0,
       "setup: wrote bystander.bin (shares nothing with victim.bin)");
    oracle(v, "victim.bin", a, FILE_SZ, "setup: victim.bin byte-exact");
    oracle(v, "bystander.bin", b, FILE_SZ, "setup: bystander.bin byte-exact");

    rc = vol_replace_file(v, "victim.bin", NULL, 0);
    ok(rc != 0, "vol_replace_file succeeded (the safe path still works)");
    if (rc == 0) { printf("  !! the truncate refused; the rest is vacuous\n"); goto done; }

    print_row(v, "after-ok-trunc", "victim.bin", old_addr);
    oracle(v, "victim.bin", NULL, 0,
           "after a SUCCESSFUL truncate victim.bin reads as empty");
    oracle(v, "bystander.bin", b, FILE_SZ,
           "the bystander is untouched by the truncate's free");

    nfree = count_free(v, old_pba, n_old, NULL);
    note("  [after-ok-trunc] %d of %d pre-truncate pbas are free\n", nfree, n_old);
    ok(nfree == n_old,
       "RECLAIM: the successful truncate freed every block the old recipe named");

    ok(vol_write_bulk(v, "thief.bin", b, FILE_SZ, NULL) != 0,
       "a new file can be written into the reclaimed space");
    if (recipe_pbas(v, "thief.bin", new_pba, MAXPB, &n_new) == 0) {
        nshared = shares_pba(old_pba, n_old, new_pba, n_new);
        note("  [reuse] the new file re-used %d pre-truncate pba(s)\n", nshared);
        ok(nshared > 0,
           "REUSE: the next write really did land on the freed blocks, so the "
           "reclaim is not a paper no-op");
        oracle(v, "thief.bin", b, FILE_SZ,
               "the file written into the reclaimed space is byte-exact");
    }

    c = mk_file(1);
    ok(vol_write_bulk(v, "victim.bin", c, FILE_SZ, NULL) != 0,
       "rewrote victim.bin after the truncate");
    oracle(v, "victim.bin", c, FILE_SZ,
           "BIT-EXACT: victim.bin is byte-exact after truncate + rewrite");
    dump_bytes("ok_victim_source", c, FILE_SZ);
    oracle(v, "bystander.bin", b, FILE_SZ,
           "the bystander is still byte-exact at the end");
    free(c);

done:
    vol_flush(v);
    vol_close(v);
    free(a);
    free(b);
    return failures;
}

int main(int argc, char **argv)
{
    const char *dir = getenv("INVFS_TEST_SCRATCH"), *phase;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <scratch-dir> <setup|hookctl|red|ok>\n",
                argv[0]);
        return 2;
    }
    if (!dir || !dir[0]) dir = argv[1];
    phase = argv[2];
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    snprintf(g_img, sizeof g_img, "%s/frb.img", dir);
    /* `make test` points this at build/frbtest, which does not exist yet.
     * mkdir -p semantics, since the parent may or may not be there. */
    { char cmd[600];
      snprintf(cmd, sizeof cmd, "mkdir -p '%s'", g_dir);
      if (system(cmd) != 0) {
          fprintf(stderr, "cannot create scratch dir %s\n", g_dir);
          return 2;
      } }

    if      (phase[0] == 's') leg_setup();
    else if (phase[0] == 'h') leg_hookctl();
    else if (phase[0] == 'r') leg_red();
    else if (phase[0] == 'o') leg_ok();
    else { fprintf(stderr, "unknown phase %s\n", phase); return 2; }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
