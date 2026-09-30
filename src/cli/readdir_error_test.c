/*
 * readdir_error_test.c — "an error became an absence" in the FUSE layer.
 *
 * The defect: invf_readdir's grow loop broke on a negative vol_list_dir
 * return and then returned 0, so an ENOMEM (the dedup-set calloc) or a
 * B+ tree scan failure became AN EMPTY DIRECTORY REPORTED AS SUCCESS.
 * `ls` on a directory that cannot be read printed nothing and exited 0.
 * invf_listxattr had the identical shape: a failed xattr scan returned 0
 * and told getfattr(1) the object carries no xattrs.
 *
 * The control has to be a C test, not a shell suite, and the reason is the
 * failure mode itself: what has to be asserted is that a directory which
 * HAS entries did not get reported as an empty one. A shell suite can only
 * see the exit code, and a build that returned the right errno *after*
 * already having told the kernel the directory was empty would still
 * satisfy it. So this calls the entry points directly, with a recording
 * fuse_fill_dir_t, and counts what the kernel would have been told.
 *
 * invf_readdir and invf_listxattr are static in fuse_fs.c and libfuse is
 * the only thing that ever calls them, so this file includes fuse_fs.c
 * with main renamed and stubs the eleven libfuse symbols and two tmpstore
 * symbols that invf-fuse links. invf_readdir itself is NOT modified,
 * stubbed or re-implemented: the assertions are on the shipped function.
 *
 * The listing failure is arranged by INVFS_FAULT (src/core/vol_fault.h) --
 * unset in production. Both cases are therefore reachable deterministically
 * without exhausting memory or corrupting a tree.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _GNU_SOURCE          /* O_PATH, RENAME_NOREPLACE (see fuse_fs.c) */
#define FUSE_USE_VERSION 31  /* must match fuse_fs.c before any header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* NOTE: volume_internal.h deliberately does NOT appear here. src/core/
 * volume.h closes its include guard at line 850 instead of at end-of-file
 * (reported separately), so any TU that reaches it by two different paths
 * re-declares its tail and will not compile. Everything this test needs is
 * in the public volume.h. */

#define main invf_fuse_daemon_main_unused
#include "fuse_fs.c"          /* brings in <fuse3/fuse.h> and tmpstore.h */
#undef main

/* ---- libfuse / tmpstore stubs (see the header comment) ----------------
 * Placed after the include so the real prototypes are in scope and these
 * match them exactly. None of them is reached by this test -- they exist
 * only so the binary links without -lfuse3.
 *
 * fuse_get_context() returning NULL is the interesting one: acreds_get()
 * then reads uid 0, which is the documented CAP_DAC_OVERRIDE bypass, so
 * the permission gate in every entry point passes and these cases measure
 * the error mapping rather than the ACL evaluator. */
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

/* the kernel-side half of readdir: everything the filler was told. */
#define SEEN_MAX 64
static char seen[SEEN_MAX][256];
static int  nseen;

static int record_filler(void *buf, const char *name,
                         const struct stat *st, off_t off, enum fuse_fill_dir_flags f)
{
    (void)buf; (void)st; (void)off; (void)f;
    if (nseen < SEEN_MAX)
        snprintf(seen[nseen++], sizeof seen[0], "%s", name);
    return 0;
}

static int seen_has(const char *name)
{
    int i;
    for (i = 0; i < nseen; i++)
        if (strcmp(seen[i], name) == 0)
            return 1;
    return 0;
}

static void seen_reset(void) { nseen = 0; }

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    int err = 0, rc, i;
    struct fuse_file_info fi;
    invfs_volume *v;

    printf("readdir_error_test: a failed listing must not look like an "
           "empty directory\n");

    snprintf(img, sizeof img, "%s/invf-readdir-error-test.img", dir);
    unlink(img);

    /* a real v3 volume with a real directory in it: the control only means
     * something if the directory HAS entries when the listing is failed */
    {
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 getenv("PWD") ? getenv("PWD") : ".", img);
        if (system(cmd) != 0) {
            printf("  cannot create volume with invf-mkfs\n");
            return 2;
        }
    }
    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "readdir_error_test: vol_open(%s) failed: err=%d\n",
                img, err);
        return 2;
    }
    g_vol = v;                       /* the daemon's only handle on the volume */

    for (i = 0; i < 5; i++) {
        char name[64];
        snprintf(name, sizeof name, "f%d", i);
        if (vol_create_file(v, name, NULL, 0) == 0) {
            printf("  cannot create %s\n", name);
            return 2;
        }
    }

    /* ---- 1. the green control: a directory that reads fine ------------- */
    seen_reset();
    rc = invf_readdir("/", NULL, record_filler, 0, NULL, 0);
    ok(rc == 0, "readdir on a healthy directory returns 0");
    ok(nseen == 7, "readdir reports 5 files + . + ..");
    ok(seen_has("f0") && seen_has("f4"),
       "readdir names the entries the directory actually has");

    /* ---- 2. the red control: the listing FAILS -------------------------
     * INVFS_FAULT makes vol_list_dir return -ENOMEM on its first call --
     * the same errno the dedup-set calloc returns. Before the fix this
     * readdir returned 0 and filled only "." and "..", i.e. `ls -a` on a
     * five-file directory printed nothing and exited 0. */
    setenv("INVFS_FAULT", "vol_list_dir:1", 1);
    seen_reset();
    rc = invf_readdir("/", NULL, record_filler, 0, NULL, 0);
    ok(rc == -ENOMEM, "readdir propagates the listing failure (ENOMEM)");
    if (rc == 0) {
        printf("        ^ readdir returned SUCCESS on a directory with %d "
               "entries -- the empty-directory failure\n", nseen);
    }
    ok(nseen == 0, "readdir emits nothing when it fails (no truncated listing)");

    /* one shot: the next call is the real one again */
    seen_reset();
    rc = invf_readdir("/", NULL, record_filler, 0, NULL, 0);
    ok(rc == 0 && nseen == 7, "readdir recovers once the fault is spent");

    /* ---- 3. the same class in listxattr --------------------------------
     * vol_list_xattr returned -1 both for "this inode has no xattrs" and
     * for "the xattr store could not be read"; invf_listxattr mapped both
     * to 0. The engine never used -1 for the empty case, so this was
     * always an error being reported as an empty xattr list. */
    setenv("INVFS_FAULT", "vol_list_xattr:1", 1);
    rc = invf_listxattr("/", NULL, 0);
    ok(rc == -EIO, "listxattr propagates a failed xattr scan (EIO)");
    if (rc == 0)
        printf("        ^ listxattr reported success on an unreadable xattr "
               "store\n");

    setenv("INVFS_FAULT", "", 1);    /* disarm */
    rc = invf_listxattr("/", NULL, 0);
    ok(rc == 0, "listxattr still reports 0 (no xattrs) for a healthy inode");

    /* ---- 4. the fault is POSITIONAL: only the nth call fails ----------
     * Arming ":2" must leave the first listing intact and fail the second.
     * If the seam ever became "fail from here on", readdir would start
     * reporting errors for a directory it can in fact read, which is the
     * other way this control can lie. */
    setenv("INVFS_FAULT", "vol_list_dir:2", 1);
    seen_reset();
    rc = invf_readdir("/", NULL, record_filler, 0, NULL, 0);
    ok(rc == 0 && nseen == 7, "the listing before the armed call still works");
    seen_reset();
    rc = invf_readdir("/", NULL, record_filler, 0, NULL, 0);
    ok(rc == -ENOMEM && nseen == 0,
       "the armed call fails with nothing emitted (no truncated listing)");
    seen_reset();
    rc = invf_readdir("/", NULL, record_filler, 0, NULL, 0);
    ok(rc == 0 && nseen == 7, "and the call after it is unaffected");

    unsetenv("INVFS_FAULT");

    /* ---- 5. invf_write must not report a read-only volume as ENOSPC ----
     * AGENTS.md 2.10 documents EROFS for exactly this case, and a caller
     * that retries on ENOSPC never gives up, so the wrong errno is not
     * merely cosmetic. The latch is set through the public
     * vol_set_readonly() rather than by filling the volume: the free-block
     * floor cannot be reached deterministically on a small image, but the
     * latch is the state the entry point has to read either way. */
    memset(&fi, 0, sizeof fi);
    {
        int before = (int)vol_free_blocks_cached(v);
        vol_set_readonly(v, 1);
        fi.fh = 0;
        rc = invf_write("/f0", "x", 1, 0, &fi);
        ok(rc == -EROFS, "write on a read-only volume returns EROFS");
        if (rc == -ENOSPC)
            printf("        ^ write reported ENOSPC on a read-only volume: "
                   "callers retry ENOSPC and give up on EROFS\n");
        vol_set_readonly(v, 0);
        ok((int)vol_free_blocks_cached(v) == before,
           "the refused write allocated nothing");
    }

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
