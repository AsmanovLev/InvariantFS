/*
 * acl_eio_test.c — "a row I/O error became an ACL that does not exist".
 *
 * THE DEFECT
 * ----------
 * vol_get_xattr (src/core/vol_records.c:368) forwards vol_v3_xattr_get
 * (src/core/vol_btree.c:3284) verbatim, and that function returned -1 for
 * BOTH "this inode has no such xattr" and "the row could not be read". The
 * row read fails for real whenever the base page is quarantined: bt_read
 * makes btree_search return -1 (vol_btree.c:522) and v3_overlay_get_key
 * passes that -1 through (vol_btree.c:2893).
 *
 * perm_check_cred (src/cli/fuse_fs.c:721) decided "this inode has an access
 * ACL" with `vol_get_xattr(...) == 0`, so a failed row read left aclp NULL
 * and acl_eval fell back to the plain mode triad (fuse_fs.c:637-640). The
 * mount deliberately does not negotiate default_permissions (AGENTS.md
 * 2.9), which makes perm_check_cred the SOLE object-level permission
 * authority: one unreadable inode row stops that file's ACLs from being
 * evaluated at all, and the mount starts allowing what they denied.
 *
 * invf_getxattr (fuse_fs.c:2722) shared the same -1 and reported the I/O
 * error to the application as -ENODATA -- "this attribute does not exist".
 *
 * WHAT IS ASSERTED, AND WHY IT IS NOT THE ERNO
 * ---------------------------------------------
 * The primary assertion is on the permission DECISION, not on the errno.
 * A control that only checked for -EIO would go green against a fix that
 * merely renamed the return value while leaving the ACL lookup degrading to
 * the mode triad -- which is the fail-open. So: uid 1000 must be DENIED
 * before the fault, must NOT be allowed once the row read fails, and must be
 * denied again once the fault is spent. The errno is asserted separately,
 * and only after the decision.
 *
 * HOW THE ROW READ IS FAILED
 * --------------------------
 * INVFS_FAULT="v3_xattr_row_read:1" (src/core/vol_fault.h), a one-shot site
 * that stands in for the row read failing and injects the same -1 the real
 * failure produces. It sits in the xattr path only, so the path lookup that
 * perm_check_cred does first (vol_find -> the base tree) is untouched and
 * the test measures the ACL fetch, not a failed stat.
 *
 * THE SHAPE
 * ---------
 * perm_check_cred and invf_getxattr are static in fuse_fs.c and libfuse is
 * the only thing that ever calls them, so this file includes fuse_fs.c with
 * main renamed and stubs the libfuse and tmpstore symbols invf-fuse links.
 * Neither function is modified, stubbed or re-implemented: the assertions
 * are on the shipped code.
 *
 * NOT RUN UNDER $(TESTISO). This test never asks the kernel for a
 * permission; it builds the struct acreds by hand and calls the evaluator
 * directly, so it is a pure function of the volume contents plus the armed
 * fault. There is no real uid denial anywhere in it, and therefore nothing
 * here that the one-entry fake-root uid map could turn into a vacuous pass.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _GNU_SOURCE          /* O_PATH, RENAME_NOREPLACE (see fuse_fs.c) */
#define FUSE_USE_VERSION 31  /* must match fuse_fs.c before any header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* NOTE: volume_internal.h deliberately does NOT appear here -- see the
 * header comment of src/cli/readdir_error_test.c, which includes
 * fuse_fs.c the same way for the same reason. */

#define main invf_fuse_daemon_main_unused
#include "fuse_fs.c"
#undef main

/* The cross-TU door onto vol_btree.c's arming state; see arm() below and
 * the declaration comment in src/core/vol_fault.h. This test is a different
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

/* The ACL this file carries. Mode is 0666, owner uid 0, so the plain mode
 * triad ALLOWS uid 1000 (other bits = rw-). The ACL adds one named user
 * entry that grants uid 1000 nothing, which is the only thing standing
 * between that user and the file. acl_eval hits the named-user entry and
 * returns denied (fuse_fs.c:657-659). */
#define DENIED_UID 1000u

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static size_t build_acl(uint8_t *b, uint32_t named_uid, uint16_t named_perm,
                        uint16_t mask_perm, uint16_t other_perm)
{
    uint8_t *e;
    memcpy(b, &(uint32_t){ INVFS_ACL_VERSION }, 4);
    e = b + 4;
    put16(e + 0, ACL_USER_OBJ);  put16(e + 2, 6); put32(e + 4, ACL_UNDEF_ID); e += 8;
    put16(e + 0, ACL_USER);      put16(e + 2, named_perm); put32(e + 4, named_uid); e += 8;
    put16(e + 0, ACL_GROUP_OBJ); put16(e + 2, 6); put32(e + 4, ACL_UNDEF_ID); e += 8;
    put16(e + 0, ACL_MASK);      put16(e + 2, mask_perm); put32(e + 4, ACL_UNDEF_ID); e += 8;
    put16(e + 0, ACL_OTHER);     put16(e + 2, other_perm); put32(e + 4, ACL_UNDEF_ID); e += 8;
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

/* Re-arm the one-shot site.
 *
 * The reload is the load-bearing part, NOT the unsetenv/setenv. The arming
 * state is per-translation-unit static and is compared by POINTER
 * (src/core/vol_fault.h), so unsetenv() frees the old spec string and
 * setenv() with the same value very often gets the SAME address back -- the
 * engine then sees no change, the countdown stays spent, and the leg quietly
 * measures the healthy path. That is how a red control ends up proving
 * nothing, so the reload has to be explicit. invfs_vol_btree_fault_reload()
 * is the exported door onto vol_btree.c's copy of that state, because this
 * test is a different translation unit and cannot reach it by hand. */
static void arm(const char *spec)
{
    invfs_vol_btree_fault_reload();
    setenv("INVFS_FAULT", spec, 1);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    char img[512], cmd[1024];
    uint8_t acl[64];
    size_t alen;
    invfs_meta_pub meta;
    invfs_volume *v;
    uint64_t ino, ino_plain;
    struct acreds c;
    int err = 0, rc;

    printf("acl_eio_test: an unreadable inode row must not stop the mount "
           "from evaluating that file's ACLs\n");

    snprintf(img, sizeof img, "%s/invf-acl-eio-test.img", dir);
    unlink(img);

    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
             getenv("PWD") ? getenv("PWD") : ".", img);
    if (system(cmd) != 0) {
        printf("  cannot create volume with invf-mkfs\n");
        return 2;
    }
    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "acl_eio_test: vol_open(%s) failed: err=%d\n", img, err);
        return 2;
    }
    g_vol = v;

    memset(&meta, 0, sizeof meta);
    meta.type = INVFS_ITYP_REG;
    meta.mode = 0666;              /* the mode triad ALLOWS uid 1000 */
    meta.uid  = 0;
    meta.gid  = 0;
    meta.nlink = 1;

    ino = vol_create_file_with_meta(v, "secret", NULL, 0, &meta);
    if (!ino) { printf("  cannot create /secret\n"); return 2; }

    alen = build_acl(acl, DENIED_UID, /*named_perm*/ 0, /*mask*/ 6, /*other*/ 6);
    if (!acl_blob_valid(acl, alen)) { printf("  the test ACL is malformed\n"); return 2; }
    if (vol_set_xattr(v, ino, XATTR_ACL_ACCESS, acl, alen) != 0) {
        printf("  cannot set the access ACL\n");
        return 2;
    }

    /* A second file, same mode, that genuinely carries NO ACL. It is the
     * control on the fix: an inode with no access ACL must keep falling
     * back to the mode triad, or a "fix" that simply denied everything
     * would pass every other leg here. */
    ino_plain = vol_create_file_with_meta(v, "plain", NULL, 0, &meta);
    if (!ino_plain) { printf("  cannot create /plain\n"); return 2; }
    vol_flush(v);

    /* The daemon's file table is refreshed lazily (fuse_fs.c:326). The setup
     * writes above went through the engine directly, not through an entry
     * point that marks it stale, so do what every create/unlink in
     * fuse_fs.c does. Without this meta_for_path answers "no such file" and
     * every leg below would be measuring ENOENT instead. */
    pthread_mutex_lock(&g_io_lock);
    g_table_stale = 1;
    pthread_mutex_unlock(&g_io_lock);

    creds_as(&c, DENIED_UID);

    /* ---- 1. the green control: the ACL is evaluated and DENIES ------- */
    rc = perm_check_cred(&c, "/secret", R_OK);
    ok(rc == -EACCES, "with the ACL readable, uid 1000 is DENIED on /secret");
    if (rc == 0)
        printf("        ^ /secret was ALLOWED: the ACL is not being "
               "evaluated at all\n");

    /* ---- 2. the mode triad still governs an inode with no ACL --------- */
    rc = perm_check_cred(&c, "/plain", R_OK);
    ok(rc == 0, "an inode with no access ACL still uses the mode triad "
                "(0666 allows uid 1000)");
    if (rc != 0)
        printf("        ^ the fix denies a file that has no ACL: it is "
               "refusing, not consulting the ACL\n");

    /* ---- 3. THE RED: the ACL row read fails --------------------------
     * This is the whole point of the control. The ACL is on the inode and
     * the mode says 0666; the ONLY reason uid 1000 is refused above is
     * the ACL. When the row cannot be read, the decision must not become
     * "allowed". */
    arm("v3_xattr_row_read:1");
    rc = perm_check_cred(&c, "/secret", R_OK);
    if (rc == 0) {
        failures++;
        printf("  FAIL  a failed ACL row read must not ALLOW uid 1000 on "
               "/secret -- it returned 0 (allowed)\n");
        printf("        ^ THE FAIL-OPEN: perm_check_cred degraded to the "
               "mode triad and the mount is now permitting what the "
               "POSIX ACL denied\n");
    } else {
        ok(1, "a failed ACL row read does not ALLOW (it refuses)");
        printf("        (refused with %d = %s)\n", -rc,
               rc == -EIO ? "EIO" : rc == -EACCES ? "EACCES" : "other");
    }
    checks++;   /* the branch above is one check either way */

    /* one shot: the real read is back */
    rc = perm_check_cred(&c, "/secret", R_OK);
    ok(rc == -EACCES, "and the ACL is enforced again once the fault is spent");

    /* ---- 3b. THE SAME FAIL-OPEN, ONE CALL UPSTREAM --------------------
     * The xattr row read is not the only way perm_check_cred can end up
     * not evaluating the ACL. It resolves the NAME first, and vol_find
     * returns a uint64_t whose "no such name" and "the lookup failed" are
     * both 0 -- so an unreadable DIRENT row dropped the resolution and the
     * mount reached acl_eval with aclp still NULL, the plain mode triad,
     * and the same allow. This leg exists so the fix cannot be declared
     * done on the strength of the xattr change alone. */
    arm("v3_dirent_row_read:1");
    rc = perm_check_cred(&c, "/secret", R_OK);
    if (rc == 0) {
        failures++;
        printf("  FAIL  a failed NAME lookup must not ALLOW uid 1000 on "
               "/secret -- it returned 0 (allowed)\n");
        printf("        ^ the same fail-open one call upstream: the ACL was "
               "never read because the name could not be resolved\n");
    } else {
        ok(1, "a failed name lookup does not ALLOW either (it refuses)");
        printf("        (refused with %s)\n", rc == -EIO ? "-EIO" : "another error");
    }
    checks++;

    rc = perm_check_cred(&c, "/secret", R_OK);
    ok(rc == -EACCES, "and the ACL is enforced again after that fault too");

    /* ---- 3c. and the default-ACL path, which is the create-time twin --
     * parent_default_acl has the same two sources of "no restriction"
     * (unresolvable name, unreadable xattr) and both used to answer 0,
     * which creates the object with nothing inherited. */
    {
        struct { const char *site; const char *what; } legs[] = {
            { "v3_dirent_row_read:1", "name lookup" },
            { "v3_xattr_row_read:1", "default-ACL row" },
        };
        size_t li;
        for (li = 0; li < sizeof legs / sizeof legs[0]; li++) {
            arm(legs[li].site);
            rc = parent_default_acl("/plain/child", acl, &alen);
            if (rc >= 0) {
                failures++;
                printf("  FAIL  a failed %s must not read as \"this "
                       "directory has no default ACL\" (got %d)\n",
                       legs[li].what, rc);
            } else {
                ok(1, "a failed parent default-ACL lookup is reported, not "
                      "read as \"no default ACL\"");
            }
            checks++;
        }
    }
    unsetenv("INVFS_FAULT");

    /* the healthy answer is unchanged: /plain has no default ACL */
    rc = parent_default_acl("/plain/child", acl, &alen);
    ok(rc == 0, "a directory with no default ACL still reports 0 (none)");

    /* ---- 4. the same row read, seen through getxattr -----------------
     * Asserted AFTER the decision, and separately: an errno-only control
     * would pass against a fix that renamed the return value and left the
     * ACL lookup degrading. */
    arm("v3_xattr_row_read:1");
    rc = invf_getxattr("/secret", XATTR_ACL_ACCESS, (char *)acl, sizeof acl);
    if (rc == -ENODATA) {
        failures++;
        printf("  FAIL  getxattr must not report an unreadable row as "
               "ENODATA -- it returned -ENODATA\n");
        printf("        ^ the application was told the ACL does not exist; "
               "it exists, it just could not be read\n");
    } else {
        ok(1, "getxattr does not answer ENODATA for an unreadable row");
        printf("        (returned %s)\n",
               rc == -EIO ? "-EIO" : rc == -ERANGE ? "-ERANGE" : "another error");
    }
    checks++;

    unsetenv("INVFS_FAULT");

    /* ---- 5. and the healthy answers are unchanged --------------------
     * A genuinely absent xattr is still ENODATA, a present one is still
     * readable, and a short buffer is still ERANGE. A fix that collapsed
     * all three into one code would pass legs 1-4 and fail here. */
    rc = invf_getxattr("/secret", XATTR_ACL_ACCESS, (char *)acl, sizeof acl);
    ok(rc == (int)alen, "getxattr still returns the ACL bytes on a healthy read");
    rc = invf_getxattr("/plain", "user.nope", NULL, 0);
    ok(rc == -ENODATA, "a genuinely absent xattr is still ENODATA");
    rc = invf_getxattr("/secret", XATTR_ACL_ACCESS, (char *)acl, 4);
    ok(rc == -ERANGE, "a too-small buffer is still ERANGE");

    vol_close(v);
    unlink(img);

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
