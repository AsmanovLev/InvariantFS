/*
 * chmod_acl_write_test.c — "chmod reported success, changed the mode, and the
 * access ACL it was supposed to fold the mode into was never written".
 *
 * THE DEFECT
 * ----------
 * invf_chmod (src/cli/fuse_fs.c) folds a new mode into a stored POSIX access
 * ACL and then applies the mode. The ACL READ is checked: a row read that
 * fails is -EIO and the chmod is refused, and the comment above it says why
 * in as many words — an ACL left standing while the mode below changes leaves
 * the file's effective permissions out of step with what the owner asked for.
 *
 * The ACL WRITE was not checked. acl_chmod_masq returns 1 ("named entries
 * remain, store the blob back") or 0 ("the ACL is now exactly the mode, drop
 * it"), and the result of vol_set_xattr / vol_remove_xattr was DISCARDED on
 * both branches. A failed write therefore left:
 *
 *     mode    = the new mode      (applied, and the chmod returned 0)
 *     ACL     = the old ACL       (ACL_MASK and ACL_OTHER never refolded)
 *
 * Because the mount deliberately does not negotiate default_permissions
 * (AGENTS.md 2.9), perm_check_cred is the SOLE object-level permission
 * authority, and it evaluates the ACL whenever one is present — the mode triad
 * is only the fallback. So on a healthy mount the file's EFFECTIVE
 * permissions stayed at the pre-chmod values while `ls` reported the new
 * mode. Both directions are wrong and both are silent:
 *
 *   chmod 0600 on a file whose ACL grants uid 1000  -> the mode says
 *       -rw------- and the mount still lets uid 1000 read it. That is a
 *       PERMISSION WIDENING: the owner removed access and the mount kept
 *       granting it.
 *   chmod 0666 on a file whose ACL denies uid 1000  -> the mode says -rw-rw-
 *       rw- and the mount still refuses. The chmod silently did not happen.
 *
 * WHAT IS ASSERTED, AND WHY IT IS NOT THE ERNO
 * ---------------------------------------------
 * The primary assertion is on the permission DECISION (perm_check_cred for a
 * hand-built uid 1000), read BEFORE and AFTER, together with the mode read
 * before and after. A control that only checked for -EIO would go green
 * against a fix that merely renamed the return value while leaving the mode
 * applied and the ACL stale -- which IS the widening. The errno is asserted
 * separately, and only after the decision.
 *
 * And the assertion is "BOTH halves are the OLD ones", not "the decision
 * flipped". Nothing happened, so the old permissions still hold and the old
 * decision is the correct one; asserting a denial there would be asserting a
 * fix that quietly applied half the chmod. The defect's exact signature is a
 * SPLIT STATE -- the mode moved while the decision stayed on the old ACL, so
 * `ls` and the mount disagree about who can read the file -- and that is what
 * these legs catch.
 *
 * THE CONTROL ARM IS NOT OPTIONAL
 * -------------------------------
 * Legs 3-5 assert that a refused chmod changed nothing. A "fix" that simply
 * refused every chmod on an ACL-bearing file would pass all three, and be a
 * different defect: chmod is the only way an owner has to change a mode at
 * all. So legs 1 and 2 are the control: with no fault armed, a chmod on an
 * ACL-bearing file returns 0 AND the permission decision really does change --
 * narrowed, then widened back.
 *
 * HOW THE WRITE IS FAILED
 * -----------------------
 * INVFS_FAULT="xattr_row_write:1" / "xattr_row_unlink:1"
 * (src/core/vol_fault.h), the WRITE-side twins of the existing
 * "xattr_row_read" site. They stand in for the delta append failing, which
 * is otherwise unreachable from a test (you cannot exhaust the disk on
 * purpose), and they return the same -1 the function already returns for every
 * other error, so invf_chmod fails exactly as it would for a real ENOSPC.
 * Both live in vol_btree.c, so the exported door
 * invfs_vol_btree_fault_reload() reaches their arming state.
 *
 * SEAM DISCIPLINE (three traps this project has paid for):
 *   1. The RELOAD is load-bearing, not the unsetenv/setenv. Arming state is
 *      per-translation-unit static compared BY POINTER (vol_fault.h), so
 *      unsetenv() + setenv() with the same value very often gets the same
 *      address back, the engine sees no change, the countdown stays spent,
 *      and the leg quietly measures the HEALTHY path and goes green proving
 *      nothing. arm() below always calls the exported reload first.
 *   2. The ordinal is not assumed. These sites are at function entry, so
 *      :1 should be the only hit — but "should" is exactly what a red control
 *      is not allowed to assume, so leg 0 CALIBRATES: it arms the site and
 *      calls vol_set_xattr directly, requiring the engine call itself to fail.
 *      If the seam does not fire, the red legs below are measuring nothing and
 *      say so.
 *   3. The test refuses to run against a binary older than the sources it
 *      measures (see stale_binary()).
 *
 * THE SHAPE
 * ---------
 * invf_chmod and perm_check_cred are static in fuse_fs.c and libfuse is the
 * only thing that ever calls them, so this file includes fuse_fs.c with main
 * renamed and stubs the libfuse and tmpstore symbols invf-fuse links.
 * Neither function is modified, stubbed or re-implemented: the assertions
 * are on the shipped code.
 *
 * NOT RUN UNDER $(TESTISO)... it IS run under it, and for the same reason
 * acl_eio_test is: this test never asks the kernel for a permission. It
 * builds struct acreds by hand and calls the evaluator directly, so there is
 * no real uid denial anywhere in it and therefore nothing for the one-entry
 * fake-root uid map under run-unit-isolated.sh to swallow.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _GNU_SOURCE          /* O_PATH, RENAME_NOREPLACE (see fuse_fs.c) */
#define FUSE_USE_VERSION 31  /* must match fuse_fs.c before any header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

/* NOTE: volume_internal.h deliberately does NOT appear here -- see the header
 * comment of src/cli/readdir_error_test.c, which includes fuse_fs.c the same
 * way for the same reason. */

#define main invf_fuse_daemon_main_unused
#include "fuse_fs.c"
#undef main

/* The cross-TU door onto vol_btree.c's arming state; see arm() below and the
 * declaration comment in src/core/vol_fault.h. This test is a different
 * translation unit from the site, so it cannot reload that state by hand. */
extern void invfs_vol_btree_fault_reload(void);

/* ---- libfuse / tmpstore stubs ---------------------------------------- */

struct fuse_context *fuse_get_context(void) { return NULL; }
int  fuse_daemonize(int foreground) { (void)foreground; return 0; }
void fuse_destroy(struct fuse *f) { (void)f; }
void fuse_unmount(struct fuse *f) { (void)f; }
struct fuse_session *fuse_get_session(struct fuse *f) { (void)f; return NULL; }
int  fuse_loop_mt_31(struct fuse *f, int clone_fd) { (void)f; (void)clone_fd; return 0; }
int  fuse_mount(struct fuse *f, const char *mp) { (void)f; (void)mp; return 0; }
struct fuse *_fuse_new_31(struct fuse_args *args, const struct fuse_operations *op,
                           size_t op_size, struct libfuse_version *ver, void *data)
{ (void)args; (void)op; (void)op_size; (void)ver; (void)data; return NULL; }
int  fuse_set_signal_handlers(struct fuse_session *se) { (void)se; return 0; }
void fuse_remove_signal_handlers(struct fuse_session *se) { (void)se; }
int  tmpstore_init(size_t max_bytes, tmp_area_mode mode)
{ (void)max_bytes; (void)mode; return 0; }
void tmpstore_destroy(void) { }

/* ---- harness ---------------------------------------------------------- */

static int checks, failures;

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

static void fail(const char *what, const char *why)
{
    checks++;
    failures++;
    printf("  FAIL  %s\n", what);
    printf("        ^ %s\n", why);
}

/* Trap 3: refuse to measure a binary older than the sources it measures.
 * make -j4 all does NOT relink the CLI/test binaries -- they build only as
 * prerequisites of `make test` -- so a red control can be running last week's
 * binary against today's sources and go green for entirely the wrong reason.
 * Observed twice on this project. */
static int stale_binary(const char *const *srcs, int n)
{
    char self[PATH_MAX];
    ssize_t n1 = readlink("/proc/self/exe", self, sizeof self - 1);
    struct stat bs;
    int i;
    if (n1 <= 0) { printf("  cannot read /proc/self/exe\n"); return 1; }
    self[n1] = 0;
    if (stat(self, &bs) != 0) { printf("  cannot stat %s\n", self); return 1; }
    for (i = 0; i < n; i++) {
        struct stat ss;
        if (stat(srcs[i], &ss) != 0) continue;
        if (ss.st_mtime > bs.st_mtime) {
            printf("  REFUSING TO MEASURE A STALE BINARY.\n");
            printf("    binary %s\n      mtime %lld\n", self, (long long)bs.st_mtime);
            printf("    source %s\n      mtime %lld  (NEWER)\n",
                   srcs[i], (long long)ss.st_mtime);
            printf("    rebuild with:\n"
                   "      rm -f %s && make %s\n", self, "invf-chmod_acl_write_test");
            return 1;
        }
    }
    return 0;
}

/* Re-arm a one-shot site. The RELOAD is the load-bearing part; see the header
 * comment on seam discipline. */
static void arm(const char *spec)
{
    invfs_vol_btree_fault_reload();
    setenv("INVFS_FAULT", spec, 1);
}

static void disarm(void)
{
    unsetenv("INVFS_FAULT");
    invfs_vol_btree_fault_reload();
}

/* ---- the two ACL shapes ----------------------------------------------- */

#define DENIED_UID 1000u

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static size_t ent(uint8_t *e, uint16_t tag, uint16_t perm, uint32_t id)
{
    put16(e + 0, tag); put16(e + 2, perm); put32(e + 4, id);
    return 8;
}

/* An ACL with a NAMED USER entry, so acl_chmod_masq returns 1 and invf_chmod
 * takes the vol_set_xattr branch. uid DENIED_UID's grant is exactly what
 * ACL_MASK caps, which is what makes the narrowing direction measurable. */
static size_t build_acl_named(uint8_t *b, uint16_t mask_perm, uint16_t other_perm)
{
    uint8_t *e = b + 4;
    memcpy(b, &(uint32_t){ INVFS_ACL_VERSION }, 4);
    e += ent(e, ACL_USER_OBJ,  6,        ACL_UNDEF_ID);
    e += ent(e, ACL_USER,      mask_perm, DENIED_UID);
    e += ent(e, ACL_GROUP_OBJ, 6,        ACL_UNDEF_ID);
    e += ent(e, ACL_MASK,      mask_perm, ACL_UNDEF_ID);
    e += ent(e, ACL_OTHER,     other_perm, ACL_UNDEF_ID);
    return (size_t)(e - b);
}

/* An ACL with no named entries and no MASK: acl_chmod_masq returns 0, so
 * invf_chmod takes the vol_remove_xattr branch. A different write, and a
 * different fault site. */
static size_t build_acl_plain(uint8_t *b, uint16_t user_perm, uint16_t group_perm,
                              uint16_t other_perm)
{
    uint8_t *e = b + 4;
    memcpy(b, &(uint32_t){ INVFS_ACL_VERSION }, 4);
    e += ent(e, ACL_USER_OBJ,  user_perm,  ACL_UNDEF_ID);
    e += ent(e, ACL_GROUP_OBJ, group_perm, ACL_UNDEF_ID);
    e += ent(e, ACL_OTHER,     other_perm, ACL_UNDEF_ID);
    return (size_t)(e - b);
}

static void creds_as(struct acreds *c, uid_t uid)
{
    memset(c, 0, sizeof *c);
    c->uid = uid;
    c->gid = 0;
    c->ngr = 0;
    c->bypass = 0;        /* NOT root: the bypass short-circuits everything */
}

/* The mode the mount is currently reporting for a path. Read back through the
 * shipped lookup, so it is the number `ls` would print. */
static unsigned cur_mode(const char *path)
{
    char ename[300];
    invfs_meta_pub m;
    if (meta_for_path(path, ename, sizeof ename, &m) != 1) return 0;
    return m.mode & 07777u;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    const char *root = getenv("PWD") ? getenv("PWD") : ".";
    char img[512], cmd[1024];
    uint8_t acl[128];
    size_t alen;
    invfs_meta_pub meta;
    invfs_volume *v;
    uint64_t ino_named, ino_plain;
    struct acreds c;
    int err = 0, rc;

    static const char *const SRCS[] = {
        "src/cli/chmod_acl_write_test.c",
        "src/cli/fuse_fs.c",
        "src/core/vol_btree.c",
    };

    printf("chmod_acl_write_test: a chmod whose ACL write fails must not leave "
           "the mode applied and the ACL stale\n");

    if (stale_binary(SRCS, (int)(sizeof SRCS / sizeof SRCS[0])))
        return 2;

    snprintf(img, sizeof img, "%s/invf-chmod-acl-write-test.img", dir);
    unlink(img);

    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null", root, img);
    if (system(cmd) != 0) {
        printf("  cannot create volume with invf-mkfs\n");
        return 2;
    }
    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "chmod_acl_write_test: vol_open(%s) failed: err=%d\n", img, err);
        return 2;
    }
    g_vol = v;

    memset(&meta, 0, sizeof meta);
    meta.type = INVFS_ITYP_REG;
    meta.mode = 0666;
    meta.uid  = 0;      /* the chmod caller runs with the bypass (see below) */
    meta.gid  = 0;
    meta.nlink = 1;

    ino_named = vol_create_file_with_meta(v, "named", NULL, 0, &meta);
    if (!ino_named) { printf("  cannot create /named\n"); return 2; }
    ino_plain = vol_create_file_with_meta(v, "plain", NULL, 0, &meta);
    if (!ino_plain) { printf("  cannot create /plain\n"); return 2; }

    alen = build_acl_named(acl, /*mask*/ 6, /*other*/ 6);
    if (!acl_blob_valid(acl, alen)) { printf("  the test ACL is malformed\n"); return 2; }
    if (vol_set_xattr(v, ino_named, XATTR_ACL_ACCESS, acl, alen) != 0) {
        printf("  cannot set the access ACL on /named\n");
        return 2;
    }
    alen = build_acl_plain(acl, 6, 6, 6);
    if (!acl_blob_valid(acl, alen)) { printf("  the test ACL is malformed\n"); return 2; }
    if (vol_set_xattr(v, ino_plain, XATTR_ACL_ACCESS, acl, alen) != 0) {
        printf("  cannot set the access ACL on /plain\n");
        return 2;
    }
    vol_flush(v);

    /* The daemon's file table is refreshed lazily (fuse_fs.c:326). The setup
     * above went through the engine directly, so do what every create/unlink
     * in fuse_fs.c does -- without it meta_for_path answers "no such file" and
     * every leg below would be measuring ENOENT instead. */
    pthread_mutex_lock(&g_io_lock);
    g_table_stale = 1;
    pthread_mutex_unlock(&g_io_lock);

    creds_as(&c, DENIED_UID);

    /* ---- 0. CALIBRATION: the seam actually fires ----------------------
     * "The write failed" is the premise of every red leg below. If the fault
     * is not firing -- a spent countdown, a reloaded address, a site that was
     * never reached -- those legs are measuring the HEALTHY path and would go
     * green for entirely the wrong reason. So require the engine call itself
     * to fail while the site is armed, and to succeed once it is spent. */
    arm("xattr_row_write:1");
    alen = 8;
    rc = vol_set_xattr(v, ino_plain, "user.calib", "x", 1);
    if (rc == 0)
        fail("the xattr_row_write seam fires on a real xattr write",
             "it did not fail, so the red legs below would measure the healthy "
             "path and prove nothing");
    else
        ok(1, "the xattr_row_write seam fires on a real xattr write");
    disarm();
    rc = vol_set_xattr(v, ino_plain, "user.calib", "x", 1);
    ok(rc == 0, "and the same write succeeds once the one-shot is spent");

    /* ---- 1. GREEN CONTROL: a healthy narrowing chmod NARROWS ---------- */
    rc = perm_check_cred(&c, "/named", R_OK);
    ok(rc == 0, "before anything: the named ACL entry ALLOWS uid 1000 on "
                "/named (mode 0666, other = rw)");
    if (rc != 0)
        printf("        ^ the fixture is wrong: the red legs below need an "
               "ALLOW here to have something to lose\n");

    rc = invf_chmod("/named", 0600, NULL);
    ok(rc == 0, "CONTROL: a healthy chmod 0600 on an ACL-bearing file succeeds");
    ok(cur_mode("/named") == 0600, "CONTROL: and the mode really is 0600");
    rc = perm_check_cred(&c, "/named", R_OK);
    if (rc == 0)
        fail("CONTROL: a healthy chmod 0600 really narrows (uid 1000 denied)",
             "the mode changed but the permission decision did not -- a fix "
             "that stops the ACL from governing at all would pass every red "
             "leg below");
    else
        ok(1, "CONTROL: a healthy chmod 0600 really narrows (uid 1000 denied)");

    /* ---- 2. GREEN CONTROL: a healthy widening chmod WIDENS ------------ */
    rc = invf_chmod("/named", 0666, NULL);
    ok(rc == 0, "CONTROL: a healthy chmod 0666 on an ACL-bearing file succeeds");
    ok(cur_mode("/named") == 0666, "CONTROL: and the mode really is 0666");
    rc = perm_check_cred(&c, "/named", R_OK);
    if (rc != 0)
        fail("CONTROL: a healthy chmod 0666 really widens (uid 1000 allowed)",
             "the mode widened but the ACL still denies -- chmod would be "
             "broken on the ordinary path, which is not this finding");
    else
        ok(1, "CONTROL: a healthy chmod 0666 really widens (uid 1000 allowed)");

    /* ---- 3. THE RED, NARROWING: the write fails ------------------------
     * The mode is 0666 and the ACL grants uid 1000 rw. The owner asks for
     * 0600. The fold is computed, the write is attempted, the write FAILS.
     *
     * What must then be true is NOT "uid 1000 is denied". Nothing happened,
     * so uid 1000 is still allowed -- that is correct, and asserting a denial
     * here would be asserting a fix that quietly applied half the chmod.
     * The defect's exact signature is a SPLIT STATE: the mode moved to the
     * new value while the decision stayed on the old ACL, so `ls` and the
     * mount disagreed about who could read the file. Both halves must be the
     * OLD ones, together.
     *
     * Read before and after, because only the pair is meaningful. */
    {
        unsigned m0 = cur_mode("/named");
        int d0 = perm_check_cred(&c, "/named", R_OK);
        ok(m0 == 0666 && d0 == 0, "precondition: /named is 0666 and uid 1000 "
                                   "is ALLOWED");

        arm("xattr_row_write:1");
        rc = invf_chmod("/named", 0600, NULL);
        if (rc == 0)
            fail("a chmod whose ACL write failed must be REFUSED, not reported 0",
                 "it returned 0: the caller was told the mode was changed");
        else
            ok(1, "a chmod whose ACL write failed is REFUSED (not reported 0)");
        printf("        (refused with %s)\n",
               rc == -EIO ? "-EIO" : "another error");

        {
            unsigned m1 = cur_mode("/named");
            int d1 = perm_check_cred(&c, "/named", R_OK);
            if (d1 != d0)
                fail("after the failed ACL write the DECISION must be the OLD "
                     "answer -- the file is untouched, so uid 1000's access is "
                     "unchanged",
                     "the decision moved while the write was reported as "
                     "failing, which means something was applied anyway");
            else
                ok(1, "after the failed ACL write the DECISION is the OLD "
                      "answer (nothing was applied)");
            printf("        (decision: %s -> %s)\n",
                   d0 == 0 ? "ALLOWED" : "DENIED", d1 == 0 ? "ALLOWED" : "DENIED");
            if (m1 != m0)
                fail("THE WIDENING: the mode must NOT have been applied either",
                     "the mode is now 0600 while the ACL still grants uid 1000 "
                     "-- on a mount without default_permissions the ACL IS "
                     "the decision, so `ls` says -rw------- and the mount "
                     "still lets uid 1000 read it. THE HALF-APPLIED STATE.");
            else
                ok(1, "and the MODE is the OLD one too: `ls` and the mount "
                      "still agree, nothing is left half-changed");
            printf("        (mode: %04o -> %04o)\n", m0, m1);
        }
    }

    /* ---- 4. THE RED, WIDENING: the same failure, the other direction --- */
    {
        unsigned m0, m1;
        int d0, d1;
        if (invf_chmod("/named", 0600, NULL) != 0) {
            printf("  cannot re-narrow /named with a healthy chmod\n");
            return 2;
        }
        m0 = cur_mode("/named");
        d0 = perm_check_cred(&c, "/named", R_OK);
        ok(m0 == 0600 && d0 != 0, "precondition: /named is 0600 and uid 1000 "
                                   "is DENIED");

        arm("xattr_row_write:1");
        rc = invf_chmod("/named", 0666, NULL);
        if (rc == 0)
            fail("the same failure in the WIDENING direction must be refused too",
                 "it returned 0: the owner was told the file was opened up");
        else
            ok(1, "the same failure in the WIDENING direction is refused too");

        m1 = cur_mode("/named");
        d1 = perm_check_cred(&c, "/named", R_OK);
        if (d1 != d0)
            fail("in the widening direction the DECISION must also be the OLD "
                 "answer", "the decision moved while the write failed");
        else
            ok(1, "in the widening direction the DECISION is the OLD answer "
                  "too (nothing was applied)");
        if (m1 != m0)
            fail("in the widening direction the MODE must not have been "
                 "applied either",
                 "the mode now says 0666 while the ACL still denies: the "
                 "widening silently did not happen and the caller was told "
                 "it did");
        else
            ok(1, "and the MODE is the OLD one there too");
        printf("        (mode %04o -> %04o, decision %s -> %s)\n", m0, m1,
               d0 == 0 ? "ALLOWED" : "DENIED", d1 == 0 ? "ALLOWED" : "DENIED");
    }

    /* ---- 5. the OTHER write: the REMOVE branch -------------------------
     * acl_chmod_masq returns 0 when the folded ACL is now exactly the mode, and
     * invf_chmod then DROPS the xattr. That is a second write with the same
     * unchecked return, and on /plain it is the same split state: the mode
     * narrows to 0600 while the ACL still grants uid 1000 through ACL_OTHER. */
    arm("xattr_row_unlink:1");
    {
        unsigned m0 = cur_mode("/plain");
        int d0 = perm_check_cred(&c, "/plain", R_OK);
        unsigned m1;
        int d1;
        ok(m0 == 0666 && d0 == 0, "precondition: /plain is 0666 and uid 1000 "
                                   "is ALLOWED (through ACL_OTHER)");
        rc = invf_chmod("/plain", 0600, NULL);
        if (rc == 0)
            fail("a chmod whose ACL REMOVE failed must be refused too",
                 "it returned 0: the caller was told the mode was changed");
        else
            ok(1, "a chmod whose ACL REMOVE failed is REFUSED");
        m1 = cur_mode("/plain");
        d1 = perm_check_cred(&c, "/plain", R_OK);
        if (d1 != d0)
            fail("on the remove branch the DECISION must be the OLD answer too",
                 "the decision moved while the remove was reported as failing");
        else
            ok(1, "on the remove branch the DECISION is the OLD answer too");
        if (m1 != m0)
            fail("on the remove branch the MODE must not have been applied "
                 "either", "the same split state on the second write");
        else
            ok(1, "and the mode was not applied on that branch either");
        printf("        (mode %04o -> %04o, decision %s -> %s)\n", m0, m1,
               d0 == 0 ? "ALLOWED" : "DENIED", d1 == 0 ? "ALLOWED" : "DENIED");
    }
    disarm();

    /* ---- 6. the one-shot really is spent, and the path still works ----
     * A fix that leaves the site permanently broken, or that wedges the
     * volume after one refusal, passes legs 3-5 and is not a fix. */
    rc = invf_chmod("/named", 0666, NULL);
    ok(rc == 0, "with no fault armed, chmod works again");
    rc = perm_check_cred(&c, "/named", R_OK);
    ok(rc == 0, "and the widening really took effect this time");
    rc = invf_chmod("/plain", 0644, NULL);
    ok(rc == 0, "and a chmod on the remove-branch file works too");
    ok(cur_mode("/plain") == 0644, "with the mode applied");

    vol_close(v);
    unlink(img);

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
