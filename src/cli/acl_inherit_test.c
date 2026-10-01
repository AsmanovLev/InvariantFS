/*
 * acl_inherit_test.c -- "a name that could not be resolved became an object
 * with no ACL on it".
 *
 * THE DEFECT
 * ----------
 * Two create paths read the parent's POSIX ACL (correctly, and fail-closed --
 * parent_default_acl returns -EIO rather than "none"), fold it against the
 * create mode, and then have to STAMP the result onto the object they just
 * created. Both stamped it by resolving the object's own name again:
 *
 *   src/cli/fuse_fs.c:1400  invf_mkdir   uint64_t ino2 = vol_find(g_vol, anchor);
 *   src/cli/fuse_fs.c:1764  invf_create  uint64_t ino2 = vol_find(g_vol, path + 1);
 *
 * vol_find returns a uint64_t, so 0 is both "there is no such name" and "the
 * lookup could not be completed". Both sites read that one value as the first:
 *
 *     if (ino2) { vol_set_xattr(... XATTR_ACL_ACCESS ...); ... }
 *
 * so an unreadable dirent row on a name the daemon had itself written one line
 * earlier produced a directory/file that EXISTS, carries the ACL-MASKED mode
 * triad, and carries NO ACL AT ALL.
 *
 * WHY THAT IS A FAIL-OPEN AND NOT A MISLABEL
 * -------------------------------------------
 * This mount does not negotiate default_permissions (AGENTS.md 2.9), so
 * perm_check_cred / perm_check_traversal_cred are the SOLE object-level
 * permission authority -- there is no kernel second opinion to fall back on.
 * An object with no access ACL is evaluated on its mode triad alone
 * (fuse_fs.c:796-800), and the mode triad is strictly COARSER than the ACL it
 * came from: acl_create_masq folds the request's mode into the ACL, so the
 * triad reproduces u::, the group class (as the mask) and o:: -- but every
 * NAMED entry is a per-identity decision the triad cannot express. Dropping
 * the ACL therefore hands the whole owning group what the ACL denied a named
 * member of it. It also drops the child's own default ACL, so every
 * descendant created below it stops inheriting the restriction at all.
 *
 * WHAT IS ASSERTED, AND WHY IT IS NOT THE ERNO
 * ---------------------------------------------
 * The assertion is on the permission DECISION. A control that only checked for
 * -EIO would go green against a fix that merely renamed the return value and
 * left the ACL unwritten -- which IS the fail-open. So the created object's
 * effective access is measured with perm_check_cred, by hand, with a
 * credential the kernel never sees: uid 2000 / gid 0, which the parent's
 * default ACL DENIES and the bare mode triad ALLOWS.
 *
 * HOW THE LOOKUP IS FAILED
 * ------------------------
 * INVFS_FAULT="v3_dirent_row_read:<n>" (src/core/vol_fault.h), the one-shot
 * site in vol_v3_dirent_get (src/core/vol_btree.c:4427) that stands in for
 * the dirent row read failing. It injects the same -1 the real failure
 * produces, so the name-resolution path cannot tell it from the real thing.
 *
 * THE ORDINAL IS NOT A CONSTANT, AND THAT IS THE POINT
 * -----------------------------------------------------
 * vol_v3_path_lookup consults the site ONCE PER PATH COMPONENT, and
 * invf_mkdir/invf_create resolve several names before the one under test:
 * perm_check_traversal, perm_check_parent, parent_default_acl, then the
 * mkdir/replace itself, and only then the stamp. So "<n>" is not a fixed
 * offset into anything, it moves with the path depth and with whatever the
 * preceding calls do. This test therefore SEARCHES the positions: every one is
 * run, each is classified, and the whole table is printed. Two things are
 * then required of that table, and the second is as load-bearing as the first:
 *
 *   - AT LEAST ONE position is the fail-open (the object was created and left
 *     with no ACL, and the decision widened). That is the defect.
 *   - AT LEAST ONE position is NOT. Without it, a table in which every
 *     position "fails open" would prove nothing: it would be equally
 *     consistent with a harness whose fault always lands somewhere that
 *     refuses, and with one that never armed the fault at all. The positions
 *     that refuse are the ones that prove the fault is landing at DIFFERENT
 *     sites and the search is discriminating.
 *
 * Re-arming between legs uses invfs_vol_btree_fault_reload() /
 * invfs_vol_dirs_fault_reload(). unsetenv+setenv is NOT a substitute: the
 * arming state is per-translation-unit static compared by POINTER, so
 * setenv() with the same value usually gets the same address back, the
 * countdown stays spent, and the leg goes GREEN measuring the healthy path.
 *
 * THE SHAPE
 * ---------
 * invf_mkdir, invf_create and perm_check_cred are static in fuse_fs.c and
 * libfuse is the only thing that ever calls them, so this file includes
 * fuse_fs.c with main renamed and stubs the libfuse and tmpstore symbols
 * invf-fuse links. None of them is modified, stubbed or re-implemented: every
 * assertion is on the shipped code.
 *
 * NOT RUN UNDER $(TESTISO)'s uid assumptions. This test never asks the kernel
 * for a permission; it builds struct acreds by hand and calls the evaluator
 * directly, so it is a pure function of the volume contents plus the armed
 * fault. There is no real uid denial anywhere in it, and therefore nothing
 * here that the one-entry fake-root uid map could turn into a vacuous pass.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error (including a STALE BINARY).
 */
#define _GNU_SOURCE          /* O_PATH, RENAME_NOREPLACE (see fuse_fs.c) */
#define FUSE_USE_VERSION 31  /* must match fuse_fs.c before any header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

/* NOTE: volume_internal.h deliberately does NOT appear here -- see the
 * header comment of src/cli/acl_eio_test.c, which includes fuse_fs.c the
 * same way for the same reason. */

#define main invf_fuse_daemon_main_unused
#include "fuse_fs.c"
#undef main

/* The cross-TU doors onto the arming state; see arm() below and the
 * declaration comment in src/core/vol_fault.h. This test is a different
 * translation unit from the sites, so it cannot reload that state by hand. */
extern void invfs_vol_btree_fault_reload(void);
extern void invfs_vol_dirs_fault_reload(void);

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

/* ---- the credential, and the ACL that is supposed to stop it ----------
 *
 * The parent's default ACL is
 *
 *     u::rwx        owner
 *     g::---        the owning group: nothing
 *     g:0:---       a NAMED group (gid 0) that is also denied
 *     m::rwx        the mask
 *     o::---        other: nothing
 *
 * mkdir(0777) under it must give the child a mode triad of 0770 -- owner and
 * group class rwx, other nothing -- AND an access ACL whose group class is
 * empty. The triad is the coarse half; the ACL is what takes the group class
 * away again. The credential is uid 2000, gid 0:
 *
 *   child gid is 0 (fuse_get_context() is stubbed to NULL, so mkdir stamps
 *   owner and group 0), so this caller is in the owning group and the triad
 *   alone grants it X_OK. acl_eval meets g::--- and g:0:--- and denies.
 *
 * So the two worlds disagree, and the disagreement IS the widening: every uid
 * in group 0 may enter a directory whose POSIX ACL denied every member of
 * group 0. */
#define CREDS_UID 2000u
#define CREDS_GID 0u

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static size_t build_default_acl(uint8_t *b)
{
    uint8_t *e;
    memcpy(b, &(uint32_t){ INVFS_ACL_VERSION }, 4);
    e = b + 4;
    put16(e + 0, ACL_USER_OBJ);  put16(e + 2, 7); put32(e + 4, ACL_UNDEF_ID); e += 8;
    put16(e + 0, ACL_GROUP_OBJ); put16(e + 2, 0); put32(e + 4, ACL_UNDEF_ID); e += 8;
    put16(e + 0, ACL_GROUP);     put16(e + 2, 0); put32(e + 4, CREDS_GID);  e += 8;
    put16(e + 0, ACL_MASK);      put16(e + 2, 7); put32(e + 4, ACL_UNDEF_ID); e += 8;
    put16(e + 0, ACL_OTHER);     put16(e + 2, 0); put32(e + 4, ACL_UNDEF_ID); e += 8;
    return (size_t)(e - b);
}

static void creds_as(struct acreds *c)
{
    memset(c, 0, sizeof *c);
    c->uid = CREDS_UID;
    c->gid = CREDS_GID;
    c->ngr = 0;
    c->bypass = 0;        /* NOT root: the bypass short-circuits everything */
}

/* Re-arm the one-shot site. The reloads are the load-bearing part, NOT the
 * unsetenv/setenv -- see the header comment. */
static void arm(const char *spec)
{
    invfs_vol_btree_fault_reload();
    invfs_vol_dirs_fault_reload();
    setenv("INVFS_FAULT", spec, 1);
}

static void disarm(void)
{
    unsetenv("INVFS_FAULT");
    invfs_vol_btree_fault_reload();
    invfs_vol_dirs_fault_reload();
}

/* ---- the volume ------------------------------------------------------- */

static const char *g_dir;
static char g_img[512];
static invfs_volume *g_v;
static uint8_t g_dacl[64];
static size_t g_daclen;

/* Build /p (a directory carrying the restrictive default ACL above). Done
 * ONCE, before any arming: the fault must only be able to land inside the
 * operation under test, never inside the setup. */
static int setup(void)
{
    invfs_meta_pub m;
    uint64_t pino;
    int err = 0;
    char cmd[1024];

    snprintf(g_img, sizeof g_img, "%s/invf-acl-inherit-test.img", g_dir);
    unlink(g_img);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
             getenv("PWD") ? getenv("PWD") : ".", g_img);
    if (system(cmd) != 0) {
        printf("  cannot create volume with invf-mkfs\n");
        return 0;
    }
    g_v = vol_open(g_img, &err);
    if (!g_v) {
        fprintf(stderr, "acl_inherit_test: vol_open(%s) failed: err=%d\n", g_img, err);
        return 0;
    }
    g_vol = g_v;

    g_daclen = build_default_acl(g_dacl);
    if (!acl_blob_valid(g_dacl, g_daclen)) {
        printf("  the test default ACL is malformed\n");
        return 0;
    }

    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_DIR;
    m.mode = 0755;
    m.uid = 0; m.gid = 0; m.nlink = 2;
    if (!vol_mkdir(g_v, "p")) { printf("  cannot mkdir /p\n"); return 0; }

    /* the default ACL lives on the directory's own ANCHOR record, which is
     * exactly the name parent_default_acl resolves */
    pino = vol_find(g_v, "p/");
    if (!pino) { printf("  cannot resolve the /p anchor\n"); return 0; }
    if (vol_set_xattr(g_v, pino, XATTR_ACL_DEFAULT, g_dacl, g_daclen) != 0) {
        printf("  cannot set /p's default ACL\n");
        return 0;
    }
    vol_flush(g_v);
    /* the daemon's file table is refreshed lazily (fuse_fs.c:326); the setup
     * above went through the engine directly, so do what every create does */
    pthread_mutex_lock(&g_io_lock);
    g_table_stale = 1;
    pthread_mutex_unlock(&g_io_lock);
    return 1;
}

/* ---- one leg ---------------------------------------------------------- */

/* Does `path` still carry an access ACL, and what is the DECISION for the
 * credential above? The fault is spent by the time this runs (the site is
 * one-shot), so what comes back is a fact about the object, not about the
 * injected error -- which is the whole point of measuring here. */
static int child_has_acl(const char *path)
{
    uint8_t buf[INVFS_META_XATTR_MAX];
    size_t vlen = sizeof buf;
    uint64_t ino = vol_find(g_v, path[0] == '/' ? path + 1 : path);
    if (!ino) return 0;
    return vol_get_xattr(g_v, ino, XATTR_ACL_ACCESS, buf, &vlen) == 0;
}

enum { LEG_REFUSED, LEG_INHERITED, LEG_FAILOPEN };

static int run_leg(int pos, int is_mkdir, const char *name, int quiet)
{
    char path[300];
    struct acreds c;
    int rc, cls, has;

    snprintf(path, sizeof path, "/p/%s", name);

    /* arm the pos-th dirent row read of this operation. Everything above is
     * setup; nothing after is inside the fault. */
    {
        char spec[64];
        snprintf(spec, sizeof spec, "v3_dirent_row_read:%d", pos);
        arm(spec);
    }

    if (is_mkdir) {
        rc = invf_mkdir(path, 0777);
    } else {
        struct fuse_file_info fi;
        memset(&fi, 0, sizeof fi);
        fi.flags = 0;
        rc = invf_create(path, 0666, &fi);
    }
    disarm();      /* one shot, and the rest of the leg is a healthy read */

    has = child_has_acl(path);
    creds_as(&c);
    if (rc != 0)                 cls = LEG_REFUSED;
    else if (has)                cls = LEG_INHERITED;
    else                         cls = LEG_FAILOPEN;

    if (!quiet)
        printf("  [pos %2d] %-4s rc=%-4d access-ACL=%-7s "
               "X_OK/R_OK(uid %u,gid %u) = %-7s  %s\n",
               pos, name, rc, has ? "present" : "ABSENT",
               (unsigned)CREDS_UID, (unsigned)CREDS_GID,
               perm_check_cred(&c, path, is_mkdir ? X_OK : R_OK) == 0
                   ? "ALLOWED" : "denied",
               cls == LEG_FAILOPEN ? "<== THE FAIL-OPEN" : "");
    return cls;
}

/* ---- stale-binary guard ----------------------------------------------- */

/* `make -j4` does not relink the CLI/test binaries -- they are built only as
 * prerequisites of `make test` -- so a control run against an old binary goes
 * GREEN measuring code that is no longer in the tree. This checks the
 * binary's own mtime against the sources it was compiled from and refuses to
 * measure anything if it is behind. It deliberately does NOT look for a
 * string from the fix: a red control that requires the fix's own message
 * cannot demonstrate the defect it exists to demonstrate. */
static int binary_is_stale(const char *argv0)
{
    static const char *deps[] = {
        "src/cli/acl_inherit_test.c", "src/cli/fuse_fs.c",
        "src/core/vol_btree.c", "src/core/vol_dirs.c",
        "src/core/vol_records.c", "src/core/vol_ast.c", NULL
    };
    struct stat bs;
    int i, stale = 0;

    if (stat(argv0, &bs) != 0) return 0;      /* cannot tell: do not block */
    for (i = 0; deps[i]; i++) {
        struct stat ss;
        if (stat(deps[i], &ss) != 0) continue;
        if (ss.st_mtime > bs.st_mtime) {
            printf("  STALE  %s is newer than %s\n", deps[i], argv0);
            stale = 1;
        }
    }
    if (stale) {
        printf("\n  *** STALE BINARY: this control would be measuring a build\n"
               "      that is no longer the tree. `make -j4` does NOT relink the\n"
               "      test binaries -- they are prerequisites of `make test` only.\n"
               "      Rebuild explicitly:\n"
               "          rm -f %s && make %s\n", argv0, argv0);
    }
    return stale;
}

#define NPOS 14

int main(int argc, char **argv)
{
    const char *bin = (argc > 0) ? argv[0] : "bin/invf-acl_inherit_test";
    struct acreds c;
    int pos, n_open, n_ref, n_inh;

    printf("acl_inherit_test: an object created while its own name cannot be "
           "resolved must not be left without the ACL it inherited\n");

    if (binary_is_stale(bin))
        return 2;

    g_dir = (argc > 1) ? argv[1] : ".";
    if (!setup())
        return 2;

    /* ---- 1. the healthy control: no fault, inheritance works --------- */
    creds_as(&c);
    {
        int rc = invf_mkdir("/p/h", 0777);
        ok(rc == 0, "with nothing armed, mkdir under the restricted parent succeeds");
        ok(child_has_acl("/p/h"),
           "and the new directory DOES carry the inherited access ACL");
        rc = perm_check_cred(&c, "/p/h", X_OK);
        if (rc == 0)
            printf("        ^ uid %u/gid %u was ALLOWED into /p/h: the "
                   "inherited ACL is not being evaluated at all\n",
                   (unsigned)CREDS_UID, (unsigned)CREDS_GID);
        ok(rc == -EACCES,
           "and the inherited ACL DENIES uid 2000/gid 0 -- the decision the "
           "ACL exists to make");
    }
    {
        struct fuse_file_info fi;
        int rc;
        memset(&fi, 0, sizeof fi);
        rc = invf_create("/p/hf", 0666, &fi);
        ok(rc == 0, "with nothing armed, create under the restricted parent succeeds");
        ok(child_has_acl("/p/hf"),
           "same on the create path: the new file carries the inherited access ACL");
        rc = perm_check_cred(&c, "/p/hf", R_OK);
        if (rc == 0)
            printf("        ^ uid %u/gid %u was ALLOWED to read /p/hf: the "
                   "inherited ACL is not being evaluated at all\n",
                   (unsigned)CREDS_UID, (unsigned)CREDS_GID);
        ok(rc == -EACCES,
           "and on the create path too the inherited ACL DENIES it");
    }
    disarm();

    /* ---- 2. SEARCH the seam ordinals, on the mkdir path ---------------
     * Both the "at least one fail-open" and the "at least one not" halves
     * are required; see the header comment on why the second is not
     * bookkeeping. */
    printf("\n  -- mkdir path: INVFS_FAULT=v3_dirent_row_read:<n> --\n");
    n_open = n_ref = n_inh = 0;
    for (pos = 1; pos <= NPOS; pos++) {
        char name[32];
        int cls;
        snprintf(name, sizeof name, "m%d", pos);
        cls = run_leg(pos, 1, name, 0);
        if (cls == LEG_FAILOPEN)  n_open++;
        else if (cls == LEG_REFUSED)  n_ref++;
        else                      n_inh++;
    }
    ok(n_open == 0,
       "no position left a created directory with no ACL and a widened decision");
    if (n_open)
        printf("        ^ %d position(s) created the object and left it with "
               "no ACL, so the mount\n          fell back to the mode triad and "
               "ALLOWED what the ACL denied\n", n_open);
    ok(n_ref > 0 && n_inh > 0,
       "the search discriminates: some positions refuse, some inherit "
       "(so the fault really does land at different sites)");
    if (!(n_ref > 0 && n_inh > 0))
        printf("        ^ %d refused / %d inherited. A control whose fault "
               "never landed at all, or\n          that always landed at the "
               "same site, produces a table like this one.\n", n_ref, n_inh);

    /* ---- 3. the same search on the create path ----------------------- */
    printf("\n  -- create path: INVFS_FAULT=v3_dirent_row_read:<n> --\n");
    n_open = n_ref = n_inh = 0;
    for (pos = 1; pos <= NPOS; pos++) {
        char name[32];
        int cls;
        snprintf(name, sizeof name, "f%d", pos);
        cls = run_leg(pos, 0, name, 0);
        if (cls == LEG_FAILOPEN)  n_open++;
        else if (cls == LEG_REFUSED)  n_ref++;
        else                      n_inh++;
    }
    ok(n_open == 0,
       "no position left a created FILE with no ACL and a widened decision");
    if (n_open)
        printf("        ^ %d position(s) created the file and left it with "
               "no ACL\n", n_open);
    ok(n_ref > 0 && n_inh > 0,
       "the create-path search discriminates too (some refuse, some inherit)");
    if (!(n_ref > 0 && n_inh > 0))
        printf("        ^ %d refused / %d inherited on the create path\n",
               n_ref, n_inh);

    /* ---- 4. the healthy answers are unchanged ------------------------
     * A fix that made every mkdir fail, or that stamped the ACL onto the
     * wrong inode, passes legs 2 and 3 and fails here. */
    {
        int rc = invf_mkdir("/p/h2", 0777);
        ok(rc == 0, "an unarmed mkdir still succeeds after all the faulted legs");
        ok(child_has_acl("/p/h2"), "and still gets the ACL");
        rc = perm_check_cred(&c, "/p/h2", X_OK);
        ok(rc == -EACCES, "and the decision is still the ACL's");
    }

    vol_close(g_v);
    unlink(g_img);

    printf("\n%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
