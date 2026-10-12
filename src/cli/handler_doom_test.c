/*
 * handler_doom_test.c — WP610: the six namespace mutators the WP606/607
 * op-abort doom did NOT touch (mkdir, mknod, symlink, link, rmdir,
 * truncate) must fail clean: a mid-op failure must leave no
 * retained-but-unfenced delta records behind.
 *
 * THE VERDICT (why this test changes no handler). Studied first:
 * commits 3192921 (WP606) and 7ebf578 (WP607). Their shape is
 * vol_delta_doom() on every post-retain failure exit, with frees riding
 * existing fences (retire) and no per-op barriers. Applied to the six:
 *
 *   - Every delta failure inside vol_delta_append dooms inline
 *     (segment-header barrier, rt30 store, publish barrier, record
 *     pwrite, group barrier — vol_delta.c). A failed append therefore
 *     never leaves net growth: either nothing of the op staged (early
 *     returns, alloc/malloc failures) or everything retained so far is
 *     unwound at once.
 *   - None of the six engines reads after staging (the rename GHOST
 *     shape WP606 names as unique to rename): vol_mkdir, vol_rmdir,
 *     vol_create_node (mknod/symlink/special) and vol_hardlink are
 *     read-first then append-only, with no fallible non-append step
 *     between/after the stages (the retire pushes are void). rmdir's
 *     -2 (ENOTEMPTY) is provably pre-stage (count-then-delete).
 *   - vol_write_commit (truncate/resize) stages delta only in the
 *     commit: begin/range/truncate touch session RAM + data segments
 *     (freed by vol_write_abort). The recipe blob goes to the BASE
 *     tree (no delta pending); the read after it (vol_inode_get)
 *     stages no delta. Row/dirent appends doom inline.
 *   - The one net-growth-on-failure path, vol_hardlink's compensation
 *     (dirent-put fails, inline doom, then the nlink-- put-back
 *     succeeds), stages a row byte-identical to the pre-op state: a
 *     benign null record, fenced harmlessly. Dooming it at the FUSE
 *     layer would unwind innocent earlier-but-unfenced ops sharing the
 *     group for zero gain.
 *   - A FUSE-level doom would be UNSAFE for mkdir/symlink/mknod anyway:
 *     vol_ensure_path stages legitimate parent creates before the engine
 *     op runs, so "pending grew" cannot tell failed-op junk from intended
 *     mkdir -p side effects a doom must not destroy.
 *
 * So per the WP610 task ("if a handler provably cannot fail post-retain,
 * write down why and leave it — no decorative wraps") all six are left
 * untouched, and THESE LEGS ARE THE PROOF: each fails its op mid-way via
 * the INVFS_FAULT seams (the production failure each stands in for) and
 * asserts the WP610 property directly — vol_delta_pending() back at its
 * pre-op baseline (no retained-but-unfenced records), the namespace
 * exactly as before (failed creates absent, failed truncates byte-exact,
 * clean refusals refusing), and finally fsck OK (shell suite) over the
 * whole fault sequence.
 *
 * A leg whose fault never fired cannot pass vacuously: every fault leg
 * also asserts the op FAILED (unarmed it would succeed), so a spent or
 * mis-armed seam fails loudly instead of proving nothing.
 *
 * Phases (argv[2]); each runs in its OWN process (the injector is
 * one-shot and per-translation-unit, so arming is setenv-before-open):
 *
 *   hookctl             the seams fire and are one-shot in this binary.
 *   setup               mkfs + fixture files (patterns regenerated, so
 *                       every process agrees on the bytes).
 *   mkdir_put           faulted row put: mkdir fails, name absent.
 *   mkdir_eexist        clean refusal: EEXIST shape, nothing staged.
 *   mknod_put           faulted row put: special create fails, absent.
 *   symlink_put         faulted row put: symlink fails, absent.
 *   symlink_store       faulted recipe store: symlink fails, absent.
 *   link_put            faulted row put: hardlink fails, dest absent,
 *                       source byte-exact.
 *   link_ok             green control: link/unlink cycle, survivor exact.
 *   rmdir_full          clean refusal: -2, contents intact.
 *   rmdir_missing       clean refusal: -1, nothing staged.
 *   rmdir_ok            green control: empty dir removed.
 *   truncate_put        faulted commit row put: commit fails, file keeps
 *                       its old size AND old bytes (the retire runs after
 *                       the put, so nothing was freed either).
 *   truncate_store      faulted recipe store: commit fails, file intact.
 *   truncate_ok         green control: shrink + extend, byte-exact
 *                       (then unlinked, so `verify` sees setup state).
 *   resize_put          faulted commit on a missing name: fails, absent.
 *   resize_ok           green control: create-via-commit, byte-exact
 *                       (then unlinked).
 *   verify              clean reopen: every fixture byte-exact, every
 *                       fault-target name absent, pending 0.
 *
 * argv[1] is a scratch directory holding hd.img; $INVFS_TEST_SCRATCH
 * wins if set. mkfs is $INVF_MKFS or ./bin/invf-mkfs (the shell suite
 * chdir's to /dev/shm, so it exports the absolute path).
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
#include "vol_delta.h"

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

/* Deterministic byte pattern: every process regenerates identical bytes. */
static void mk_pat(uint8_t *b, size_t n, uint64_t seed)
{
    uint64_t x = seed ? seed : 0x9e3779b97f4a7c15ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        b[i] = (uint8_t)(x >> 33);
    }
}

#define SURV_SZ   300000u   /* multi-segment RAW file */
#define SRC_SZ    100000u
#define CHILD_SZ  5000u
#define KEEP_SZ   1000u

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
    const char *mk = getenv("INVF_MKFS");
    unlink(g_img);
    snprintf(cmd, sizeof cmd, "%s %s 64 >/dev/null 2>&1",
             mk && *mk ? mk : "./bin/invf-mkfs", g_img);
    if (system(cmd) != 0) { fprintf(stderr, "mkfs failed\n"); exit(2); }
}

/* vol_delta_pending() baseline: no retained-but-unfenced records. */
static void expect_no_retained(invfs_volume *v, const char *tag)
{
    uint64_t p = vol_delta_pending(v);
    ok(p == 0, "%s: no retained records (pending=%llu)", tag,
       (unsigned long long)p);
}

static int name_present(invfs_volume *v, const char *name)
{
    uint64_t ino = 0;
    return vol_find_rc(v, name, &ino) == 1 && ino != 0;
}

/* Byte-exactness oracle: name must resolve and read back `want`. */
static int oracle(invfs_volume *v, const char *name, const uint8_t *want,
                  size_t want_len, const char *tag)
{
    uint64_t id = 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    int same;

    if (vol_path_lookup(v, name, &id) != 1) {
        ok(0, "%s: %s is GONE", tag, name);
        return 0;
    }
    if (vol_read_inode(v, id, 0, &buf, &len) != 0 || !buf) {
        ok(0, "%s: %s unreadable", tag, name);
        free(buf);
        return 0;
    }
    same = (len == want_len) && (want_len == 0 || memcmp(buf, want, len) == 0);
    ok(same, "%s: %s %zu bytes %s", tag, name, len,
       same ? "BYTE-EXACT" : "NOT byte-exact");
    free(buf);
    return same;
}

/* The FUSE resize path's engine sequence (resize_volume_file without the
 * flush/table half): begin + truncate + commit, abort always after. */
static int sess_commit_to(invfs_volume *v, const char *name, uint64_t newlen)
{
    invfs_wsession *ws = NULL;
    int rc;
    if (!vol_write_begin(v, name, 0, &ws) || !ws)
        return -99;
    rc = vol_write_truncate(ws, newlen);
    if (rc == 0)
        rc = vol_write_commit(ws);
    vol_write_abort(ws);
    return rc;
}

/* ==================================================================== */
/* hookctl + setup                                                       */
/* ==================================================================== */
static int leg_hookctl(void)
{
    uint8_t *a;
    uint64_t rc;
    invfs_volume *v;

    printf("== leg hookctl: the seams fire and are one-shot ==\n");
    mkfs_fresh();
    a = (uint8_t *)malloc(KEEP_SZ);
    mk_pat(a, KEEP_SZ, 7);
    setenv("INVFS_FAULT", "inode_delta_put:1", 1);
    v = open_vol();
    rc = vol_write_bulk(v, "ctl.bin", a, KEEP_SZ, NULL);
    ok(rc == 0, "armed put refuses the write (rc==0 means refused here)");
    rc = vol_write_bulk(v, "ctl.bin", a, KEEP_SZ, NULL);
    ok(rc != 0, "one-shot: the next write succeeds");
    oracle(v, "ctl.bin", a, KEEP_SZ, "post-fault write is byte-exact");
    ok(vol_unlink(v, "ctl.bin") == 0, "hookctl cleans up ctl.bin");
    /* vol_flush barriers but does not close the group (pending stays
     * counted); vol_sync is the fence that drains it. */
    ok(vol_sync(v) == 0, "hookctl state is durable");
    expect_no_retained(v, "synced hookctl state");
    vol_close(v);
    free(a);
    return failures;
}

static int leg_setup(void)
{
    uint8_t *s, *l, *c, *k;
    invfs_volume *v;

    printf("== leg setup: fixture files ==\n");
    mkfs_fresh();
    v = open_vol();
    s = (uint8_t *)malloc(SURV_SZ); mk_pat(s, SURV_SZ, 11);
    l = (uint8_t *)malloc(SRC_SZ);  mk_pat(l, SRC_SZ, 13);
    c = (uint8_t *)malloc(CHILD_SZ); mk_pat(c, CHILD_SZ, 17);
    k = (uint8_t *)malloc(KEEP_SZ); mk_pat(k, KEEP_SZ, 19);
    ok(vol_write_bulk(v, "surv.bin", s, SURV_SZ, NULL) != 0, "wrote surv.bin");
    ok(vol_write_bulk(v, "linksrc.bin", l, SRC_SZ, NULL) != 0, "wrote linksrc.bin");
    ok(vol_write_bulk(v, "keep.bin", k, KEEP_SZ, NULL) != 0, "wrote keep.bin");
    ok(vol_mkdir(v, "subdir") != 0, "made subdir");
    ok(vol_write_bulk(v, "subdir/child.bin", c, CHILD_SZ, NULL) != 0,
       "wrote subdir/child.bin");
    /* Group commit (defaults 64 records / 256 KiB) may hold the setup
     * writes unfenced: the flush below is what makes them durable, and
     * only after it must the retained set be empty. */
    ok(vol_sync(v) == 0, "setup state is durable");
    expect_no_retained(v, "synced setup state");
    oracle(v, "surv.bin", s, SURV_SZ, "setup");
    vol_close(v);
    free(s); free(l); free(c); free(k);
    return failures;
}

/* ==================================================================== */
/* mkdir / mknod / symlink                                               */
/* ==================================================================== */
static int leg_mkdir_put(void)
{
    invfs_volume *v;
    printf("== leg mkdir_put: faulted row put ==\n");
    setenv("INVFS_FAULT", "inode_delta_put:1", 1);
    v = open_vol();
    expect_no_retained(v, "open stages nothing");
    /* Unarmed this mkdir would SUCCEED, so rc==0 proves the seam fired. */
    ok(vol_mkdir(v, "newdir") == 0, "mkdir failed loudly");
    ok(!name_present(v, "newdir"), "failed mkdir left no name");
    expect_no_retained(v, "failed mkdir retained nothing");
    vol_close(v);
    return failures;
}

static int leg_mkdir_eexist(void)
{
    invfs_volume *v;
    printf("== leg mkdir_eexist: clean refusal stages nothing ==\n");
    v = open_vol();
    ok(vol_mkdir(v, "surv.bin") == 0, "mkdir over a file is refused");
    ok(vol_mkdir(v, "subdir") == 0, "mkdir over a dir is refused");
    expect_no_retained(v, "refused mkdirs retained nothing");
    vol_close(v);
    return failures;
}

static int leg_mknod_put(void)
{
    invfs_volume *v;
    printf("== leg mknod_put: faulted row put ==\n");
    setenv("INVFS_FAULT", "inode_delta_put:1", 1);
    v = open_vol();
    ok(vol_create_special(v, "newfifo", INVFS_ITYP_FIFO, 0644, 0) == 0,
       "mknod failed loudly");
    ok(!name_present(v, "newfifo"), "failed mknod left no name");
    expect_no_retained(v, "failed mknod retained nothing");
    vol_close(v);
    return failures;
}

static int leg_symlink_put(void)
{
    invfs_volume *v;
    printf("== leg symlink_put: faulted row put ==\n");
    setenv("INVFS_FAULT", "inode_delta_put:1", 1);
    v = open_vol();
    ok(vol_create_symlink(v, "newlink", "some-target") == 0,
       "symlink failed loudly");
    ok(!name_present(v, "newlink"), "failed symlink left no name");
    expect_no_retained(v, "failed symlink retained nothing");
    vol_close(v);
    return failures;
}

static int leg_symlink_store(void)
{
    invfs_volume *v;
    printf("== leg symlink_store: faulted recipe store ==\n");
    setenv("INVFS_FAULT", "recipe_store:1", 1);
    v = open_vol();
    ok(vol_create_symlink(v, "newlink2", "some-target") == 0,
       "symlink failed loudly");
    ok(!name_present(v, "newlink2"), "failed symlink left no name");
    expect_no_retained(v, "failed symlink retained nothing");
    vol_close(v);
    return failures;
}

/* ==================================================================== */
/* link                                                                  */
/* ==================================================================== */
static int leg_link_put(void)
{
    uint8_t *l;
    invfs_volume *v;
    printf("== leg link_put: faulted row put ==\n");
    setenv("INVFS_FAULT", "inode_delta_put:1", 1);
    v = open_vol();
    l = (uint8_t *)malloc(SRC_SZ); mk_pat(l, SRC_SZ, 13);
    /* Unarmed this link would SUCCEED (rc 0), so rc!=0 proves firing. */
    ok(vol_hardlink(v, "linksrc.bin", "hardlink") != 0, "link failed loudly");
    ok(!name_present(v, "hardlink"), "failed link left no dest name");
    oracle(v, "linksrc.bin", l, SRC_SZ, "source intact after failed link");
    expect_no_retained(v, "failed link retained nothing");
    vol_close(v);
    free(l);
    return failures;
}

static int leg_link_ok(void)
{
    uint8_t *l;
    uint64_t a = 0, b = 0;
    invfs_volume *v;
    printf("== leg link_ok: green control ==\n");
    v = open_vol();
    l = (uint8_t *)malloc(SRC_SZ); mk_pat(l, SRC_SZ, 13);
    ok(vol_hardlink(v, "linksrc.bin", "hardlink-ok") == 0, "link succeeds");
    ok(vol_path_lookup(v, "linksrc.bin", &a) == 1 &&
       vol_path_lookup(v, "hardlink-ok", &b) == 1 && a == b && a != 0,
       "both names share one inode");
    oracle(v, "hardlink-ok", l, SRC_SZ, "alias reads byte-exact");
    ok(vol_unlink(v, "hardlink-ok") == 0, "unlink of the alias succeeds");
    oracle(v, "linksrc.bin", l, SRC_SZ, "survivor exact after unlink");
    ok(vol_sync(v) == 0, "link_ok state is durable");
    expect_no_retained(v, "synced link_ok state");
    vol_close(v);
    free(l);
    return failures;
}

/* ==================================================================== */
/* rmdir                                                                 */
/* ==================================================================== */
static int leg_rmdir_full(void)
{
    uint8_t *c;
    invfs_volume *v;
    printf("== leg rmdir_full: -2 is pre-stage ==\n");
    v = open_vol();
    c = (uint8_t *)malloc(CHILD_SZ); mk_pat(c, CHILD_SZ, 17);
    ok(vol_rmdir(v, "subdir") == -2, "non-empty rmdir refused with -2");
    ok(name_present(v, "subdir/child.bin"), "child still named");
    oracle(v, "subdir/child.bin", c, CHILD_SZ, "child intact");
    expect_no_retained(v, "refused rmdir retained nothing");
    vol_close(v);
    free(c);
    return failures;
}

static int leg_rmdir_missing(void)
{
    invfs_volume *v;
    printf("== leg rmdir_missing: -1 stages nothing ==\n");
    v = open_vol();
    ok(vol_rmdir(v, "nosuchdir") == -1, "missing rmdir refused with -1");
    expect_no_retained(v, "refused rmdir retained nothing");
    vol_close(v);
    return failures;
}

static int leg_rmdir_ok(void)
{
    invfs_volume *v;
    printf("== leg rmdir_ok: green control ==\n");
    v = open_vol();
    ok(vol_mkdir(v, "emptydir") != 0, "empty dir created");
    ok(vol_rmdir(v, "emptydir") == 0, "empty rmdir succeeds");
    ok(!name_present(v, "emptydir"), "removed dir is gone");
    ok(vol_sync(v) == 0, "rmdir_ok state is durable");
    expect_no_retained(v, "synced rmdir_ok state");
    vol_close(v);
    return failures;
}

/* ==================================================================== */
/* truncate (session commit) + create-via-commit                         */
/* ==================================================================== */
static int leg_truncate_put(void)
{
    uint8_t *s;
    invfs_volume *v;
    int rc;
    printf("== leg truncate_put: faulted commit row put ==\n");
    setenv("INVFS_FAULT", "inode_delta_put:1", 1);
    v = open_vol();
    s = (uint8_t *)malloc(SURV_SZ); mk_pat(s, SURV_SZ, 11);
    /* Unarmed this commit would SUCCEED and shrink the file, so rc!=0
     * plus the unchanged size below prove the seam fired at the put. */
    rc = sess_commit_to(v, "surv.bin", 10000);
    ok(rc != 0, "commit failed loudly (rc=%d)", rc);
    oracle(v, "surv.bin", s, SURV_SZ,
           "failed truncate kept old size AND old bytes");
    expect_no_retained(v, "failed commit retained nothing");
    vol_close(v);
    free(s);
    return failures;
}

static int leg_truncate_store(void)
{
    uint8_t *s;
    invfs_volume *v;
    int rc;
    printf("== leg truncate_store: faulted recipe store ==\n");
    setenv("INVFS_FAULT", "recipe_store:1", 1);
    v = open_vol();
    s = (uint8_t *)malloc(SURV_SZ); mk_pat(s, SURV_SZ, 11);
    rc = sess_commit_to(v, "surv.bin", 10000);
    ok(rc != 0, "commit failed loudly (rc=%d)", rc);
    oracle(v, "surv.bin", s, SURV_SZ, "failed truncate kept file intact");
    expect_no_retained(v, "failed commit retained nothing");
    vol_close(v);
    free(s);
    return failures;
}

static int leg_truncate_ok(void)
{
    uint8_t *t, * shrunk;
    invfs_volume *v;
    printf("== leg truncate_ok: green control ==\n");
    v = open_vol();
    t = (uint8_t *)malloc(SRC_SZ); mk_pat(t, SRC_SZ, 23);
    ok(vol_write_bulk(v, "trunc.bin", t, SRC_SZ, NULL) != 0, "wrote trunc.bin");
    ok(sess_commit_to(v, "trunc.bin", CHILD_SZ) == 0, "shrink commits");
    shrunk = (uint8_t *)malloc(CHILD_SZ);
    memcpy(shrunk, t, CHILD_SZ);
    oracle(v, "trunc.bin", shrunk, CHILD_SZ, "shrunk file is a prefix");
    ok(sess_commit_to(v, "trunc.bin", SRC_SZ) == 0, "extend commits");
    {
        /* Extend re-exposes old bytes below the old end and zeros the
         * hole past it: prefix must match, tail must be zero. */
        uint8_t *want = (uint8_t *)malloc(SRC_SZ);
        uint64_t id = 0;
        uint8_t *buf = NULL;
        size_t len = 0;
        memcpy(want, t, CHILD_SZ);
        memset(want + CHILD_SZ, 0, SRC_SZ - CHILD_SZ);
        ok(vol_path_lookup(v, "trunc.bin", &id) == 1 &&
           vol_read_inode(v, id, 0, &buf, &len) == 0 && buf != NULL &&
           len == SRC_SZ && memcmp(buf, want, SRC_SZ) == 0,
           "extended file is old-prefix + zero hole");
        free(want);
        free(buf);
    }
    ok(vol_unlink(v, "trunc.bin") == 0, "truncate_ok cleans up trunc.bin");
    ok(vol_sync(v) == 0, "truncate_ok state is durable");
    expect_no_retained(v, "synced truncate_ok state");
    vol_close(v);
    free(t); free(shrunk);
    return failures;
}

static int leg_resize_put(void)
{
    invfs_volume *v;
    int rc;
    printf("== leg resize_put: faulted create-via-commit ==\n");
    setenv("INVFS_FAULT", "inode_delta_put:1", 1);
    v = open_vol();
    rc = sess_commit_to(v, "brandnew2.bin", CHILD_SZ);
    ok(rc != 0, "commit failed loudly (rc=%d)", rc);
    ok(!name_present(v, "brandnew2.bin"), "failed create left no name");
    expect_no_retained(v, "failed create retained nothing");
    vol_close(v);
    return failures;
}

static int leg_resize_ok(void)
{
    uint8_t *c;
    invfs_volume *v;
    printf("== leg resize_ok: green control ==\n");
    v = open_vol();
    c = (uint8_t *)malloc(CHILD_SZ); mk_pat(c, CHILD_SZ, 29);
    {
        invfs_wsession *ws = NULL;
        int rc;
        ok(vol_write_begin(v, "brandnew.bin", 0, &ws) && ws,
           "begin on a missing name succeeds");
        rc = vol_write_range(ws, 0, c, CHILD_SZ);
        ok(rc == 0, "range stages %u bytes", CHILD_SZ);
        if (rc == 0)
            rc = vol_write_commit(ws);
        ok(rc == 0, "create-via-commit succeeds");
        vol_write_abort(ws);
    }
    oracle(v, "brandnew.bin", c, CHILD_SZ, "created file is byte-exact");
    ok(vol_unlink(v, "brandnew.bin") == 0, "resize_ok cleans up");
    ok(vol_sync(v) == 0, "resize_ok state is durable");
    expect_no_retained(v, "synced resize_ok state");
    vol_close(v);
    free(c);
    return failures;
}

/* ==================================================================== */
/* verify: setup state, byte-exact, nothing retained                     */
/* ==================================================================== */
static int leg_verify(void)
{
    uint8_t *s, *l, *c, *k;
    invfs_volume *v;
    static const char *gone[] = {
        "newdir", "newfifo", "newlink", "newlink2", "hardlink",
        "hardlink-ok", "emptydir", "brandnew.bin", "brandnew2.bin",
        "trunc.bin", "ctl.bin",
    };
    size_t i;

    printf("== leg verify: fixtures exact, fault targets absent ==\n");
    v = open_vol();
    s = (uint8_t *)malloc(SURV_SZ); mk_pat(s, SURV_SZ, 11);
    l = (uint8_t *)malloc(SRC_SZ);  mk_pat(l, SRC_SZ, 13);
    c = (uint8_t *)malloc(CHILD_SZ); mk_pat(c, CHILD_SZ, 17);
    k = (uint8_t *)malloc(KEEP_SZ); mk_pat(k, KEEP_SZ, 19);
    oracle(v, "surv.bin", s, SURV_SZ, "survivor");
    oracle(v, "linksrc.bin", l, SRC_SZ, "survivor");
    oracle(v, "keep.bin", k, KEEP_SZ, "survivor");
    oracle(v, "subdir/child.bin", c, CHILD_SZ, "survivor");
    for (i = 0; i < sizeof gone / sizeof gone[0]; i++)
        ok(!name_present(v, gone[i]), "fault target %s absent", gone[i]);
    expect_no_retained(v, "verify");
    vol_close(v);
    free(s); free(l); free(c); free(k);
    return failures;
}

int main(int argc, char **argv)
{
    const char *scratch;
    const char *phase;
    int rc = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <scratchdir> <phase>\n", argv[0]);
        return 2;
    }
    scratch = getenv("INVFS_TEST_SCRATCH");
    if (!scratch || !*scratch)
        scratch = argv[1];
    snprintf(g_dir, sizeof g_dir, "%s", scratch);
    snprintf(g_img, sizeof g_img, "%s/hd.img", scratch);

    phase = argv[2];
    if (strcmp(phase, "hookctl") == 0) rc = leg_hookctl();
    else if (strcmp(phase, "setup") == 0) rc = leg_setup();
    else if (strcmp(phase, "mkdir_put") == 0) rc = leg_mkdir_put();
    else if (strcmp(phase, "mkdir_eexist") == 0) rc = leg_mkdir_eexist();
    else if (strcmp(phase, "mknod_put") == 0) rc = leg_mknod_put();
    else if (strcmp(phase, "symlink_put") == 0) rc = leg_symlink_put();
    else if (strcmp(phase, "symlink_store") == 0) rc = leg_symlink_store();
    else if (strcmp(phase, "link_put") == 0) rc = leg_link_put();
    else if (strcmp(phase, "link_ok") == 0) rc = leg_link_ok();
    else if (strcmp(phase, "rmdir_full") == 0) rc = leg_rmdir_full();
    else if (strcmp(phase, "rmdir_missing") == 0) rc = leg_rmdir_missing();
    else if (strcmp(phase, "rmdir_ok") == 0) rc = leg_rmdir_ok();
    else if (strcmp(phase, "truncate_put") == 0) rc = leg_truncate_put();
    else if (strcmp(phase, "truncate_store") == 0) rc = leg_truncate_store();
    else if (strcmp(phase, "truncate_ok") == 0) rc = leg_truncate_ok();
    else if (strcmp(phase, "resize_put") == 0) rc = leg_resize_put();
    else if (strcmp(phase, "resize_ok") == 0) rc = leg_resize_ok();
    else if (strcmp(phase, "verify") == 0) rc = leg_verify();
    else { fprintf(stderr, "unknown phase %s\n", phase); return 2; }

    printf("handler_doom_test %s: %d checks, %d failures\n",
           phase, checks, failures);
    return (rc != 0 || failures != 0) ? 1 : 0;
}
