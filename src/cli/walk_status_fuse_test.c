/* walk_status_fuse_test.c — WP135, the FUSE half of sites 1 and 3.
 *
 * walk_status_test.c covers the engine and the two CLIs. What is left is the
 * part a USER sees, and it is the sharpest evidence the finding is real
 * rather than theoretical:
 *
 *   3  src/cli/fuse_fs.c:228  build_file_table_v3() dropped vol_walk's
 *      status. Underneath, walk_dir() in vol_dirs.c did
 *      `if (vol_path_lookup(...) != 1) continue;` -- and because the
 *      recursion into a directory's contents is inside that same loop, ONE
 *      unresolvable entry took its whole SUBTREE out of the table, while the
 *      walk reported 0, i.e. COMPLETE. fuse_fs.c:132 then cleared
 *      g_table_stale, so nothing ever retried. Every path under a lost
 *      directory missed in snapshot_entry(), and open/read/getattr turned
 *      that into -ENOENT.
 *
 *      So the daemon told the kernel a subtree did not exist. `ls` printed
 *      nothing, `cat` said No such file, and both were wrong in the way that
 *      costs data: the files were on the disk the whole time. The volume was
 *      merely damaged.
 *
 *   1  src/cli/fuse_fs.c, invf_sweep_worker(). The collect's status was
 *      dropped in vol_collect_sweepables_ex, so vol_collect_sweepables_grow
 *      reported COMPLETE over a walk that had stopped, and the worker
 *      printed an ordinary `[sweep] DONE ... failed=0` and went on to
 *      rewrite files. The pass is what AGENTS.md 2.5/2.6 makes the daemon's
 *      own recovery path, so this is unbounded dead-segment accumulation
 *      behind a clean log.
 *
 * invf_readdir, invf_sweep_worker and build_file_table_v3 are static in
 * fuse_fs.c, so this includes it with `main` renamed and stubs the libfuse
 * and tmpstore symbols invf-fuse links -- the same shape as
 * readdir_error_test.c. The functions under test are NOT modified, stubbed
 * or re-implemented here; the assertions are on the shipped ones.
 *
 * STDERR IS CAPTURED through a real pipe (dup2 onto fd 2) rather than by
 * redirecting the process, because the claim under test is a LOG LINE. A
 * test that only checked a return value would pass on a build that
 * returned the right code and said nothing to the operator -- which is most
 * of what was wrong here.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _GNU_SOURCE          /* O_PATH, RENAME_NOREPLACE (see fuse_fs.c) */
#define FUSE_USE_VERSION 31  /* must match fuse_fs.c before any header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>   /* WP209: intptr_t, for the reader's own fd */
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

/* NOTE: volume_internal.h deliberately does NOT appear here (see
 * readdir_error_test.c: volume.h closes its include guard mid-file, so a TU
 * that reaches it by two paths re-declares its tail). */

#define main invf_fuse_daemon_main_unused
#include "fuse_fs.c"          /* brings in <fuse3/fuse.h> and tmpstore.h */
#undef main

#include "vol_fault.h"

/* ---- libfuse / tmpstore stubs --------------------------------------- */
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

/* ---- harness --------------------------------------------------------- */

static int checks, failures;

static void ok(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(cond ? "  ok   " : "  FAIL ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    checks++;
    if (!cond) failures++;
}

static void mkvol(const char *img, int mb)
{
    char cmd[1024];
    unlink(img);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s %d >/dev/null 2>&1",
             getenv("PWD") ? getenv("PWD") : ".", img, mb);
    if (system(cmd) != 0) {
        fprintf(stderr, "cannot create volume with invf-mkfs\n");
        exit(2);
    }
}

static void arm_dirs(const char *spec)
{
    setenv("INVFS_FAULT", spec, 1);
    invfs_vol_dirs_fault_reload();
}
static void arm_btree(const char *spec)
{
    setenv("INVFS_FAULT", spec, 1);
    invfs_vol_btree_fault_reload();
}
static void disarm(void)
{
    unsetenv("INVFS_FAULT");
    invfs_vol_dirs_fault_reload();
    invfs_vol_btree_fault_reload();
}

/* ---- stderr capture ---------------------------------------------------
 * A real pipe plus a reader thread, so a log line that arrives while the
 * call under test is running is not lost to a buffer race. */
#define CAPMAX 65536
static char cap_buf[CAPMAX];
static size_t cap_len;
static pthread_mutex_t cap_mtx = PTHREAD_MUTEX_INITIALIZER;
static int cap_saved = -1;
static int cap_pipe[2] = { -1, -1 };
/* WP209: the reader is joinable, so its handle has to outlive cap_start(). */
static pthread_t cap_thread;
static int cap_thread_live;

/* WP209: the reader takes its OWN fd as an argument.
 *
 * It used to re-read the global `cap_pipe[0]` on every iteration. That is
 * only safe if no reader ever outlives its capture, and one did: the thread
 * was DETACHED and cap_stop_and_get() closed `cap_pipe[0]` under it. A
 * reader still blocked in read() then either saw EBADF or -- once the next
 * cap_start() reused the fd number -- read the NEXT probe's pipe and
 * appended it to the next probe's buffer, because cap_start() had already
 * reset cap_len = 0. One capture could therefore contain another pass's
 * output, which is not cosmetic here: legs 1e-1h assert that the operator
 * pass prints no DONE line at all (1h), and legs 1j-1l assert the
 * watermark pass's DONE carries INCOMPLETE (1k). A splice fails them in
 * BOTH directions, and the 1h direction reads as "the pre-fix bug is back" --
 * a fixed defect reporting itself unfixed. Observed once in ~6 full `make
 * test` runs, under load, never standalone. */
static void *cap_reader(void *arg)
{
    int rfd = (int)(intptr_t)arg;
    for (;;) {
        char b[512];
        ssize_t k = read(rfd, b, sizeof b);
        if (k <= 0)
            break;
        pthread_mutex_lock(&cap_mtx);
        if (cap_len + (size_t)k < CAPMAX - 1) {
            memcpy(cap_buf + cap_len, b, (size_t)k);
            cap_len += (size_t)k;
            cap_buf[cap_len] = 0;
        }
        pthread_mutex_unlock(&cap_mtx);
        /* No early break on cap_stop: the join in cap_stop_and_get
         * already guarantees EOF (dup2 restore closed the last write
         * end), so breaking here only abandons unread pipe data. That
         * dropped the TAIL of a fast pass -- observed as CI engine 1k
         * (watermark DONE missing after a complete INCOMPLETE/PARTIAL
         * log) on a loaded 4-shard run, never standalone. */
    }
    return NULL;
}

static void cap_start(void)
{
    pthread_t th;
    fflush(stderr);
    if (pipe(cap_pipe) != 0)
        return;
    cap_saved = dup(2);
    dup2(cap_pipe[1], 2);
    close(cap_pipe[1]);
    cap_len = 0; cap_buf[0] = 0;
    /* WP209: JOINABLE. This thread was detached, which is what let it
     * outlive its capture and read the next probe's pipe (see cap_reader).
     * cap_stop_and_get() joins it, and the join cannot hang: restoring fd 2
     * with dup2 also closes the last reference to this pipe's write end, so
     * a reader blocked in read() sees EOF and returns. */
    cap_thread_live = pthread_create(&th, NULL, cap_reader,
                                     (void *)(intptr_t)cap_pipe[0]) == 0;
    if (cap_thread_live)
        cap_thread = th;
}

/* WP209: stop the capture WITHOUT leaving anything behind that can write to
 * cap_buf or read from a recycled fd.
 *
 * Order matters and is the whole fix:
 *   1. flush, so nothing the pass printed is still sitting in stdio;
 *   2. restore fd 2 -- dup2 closes the pipe's last write end, so a blocked
 *      reader gets EOF rather than staying blocked;
 *   3. JOIN the reader, so cap_buf is quiescent and no reader survives into
 *      the next cap_start() where cap_len = 0 would splice it;
 *   4. only then close the read end (closing it earlier is what closed the
 *      fd under a live thread);
 *   5. snapshot under the mutex.
 * Steps 2-3 are why this cannot hang and cannot bleed. */
static void cap_stop_and_get(char *out, size_t cap)
{
    fflush(stderr);
    if (cap_saved >= 0) { dup2(cap_saved, 2); close(cap_saved); cap_saved = -1; }
    if (cap_thread_live) {
        pthread_join(cap_thread, NULL);
        cap_thread_live = 0;
    }
    if (cap_pipe[0] >= 0) { close(cap_pipe[0]); cap_pipe[0] = -1; }
    pthread_mutex_lock(&cap_mtx);
    snprintf(out, cap, "%s", cap_buf);
    pthread_mutex_unlock(&cap_mtx);
}

/* ---- the kernel side of readdir -------------------------------------- */
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
        if (strcmp(seen[i], name) == 0) return 1;
    return 0;
}
static void seen_reset(void) { nseen = 0; }

/* Arm a site at an explicit call position. */
static void arm_btree_pos(const char *site, int n)
{
    char spec[64];
    snprintf(spec, sizeof spec, "%s:%d", site, n);
    arm_btree(spec);
}

/* Which vol_iter_live_inodes call is the sweep COLLECT's, rather than the
 * savepoint capture's? Found by trying positions: each run that is refused
 * at the collect is the right one, and 0 means the seam moved somewhere this
 * test can no longer reach (which is itself worth failing on -- a red
 * control that has quietly stopped testing anything is worse than no red
 * control). */
/* Search for the collect's walk position AND KEEP THE LOG OF THE PROBE
 * THAT MATCHED.
 *
 * The log is returned rather than thrown away because the position is not a
 * constant: it moves with the volume's runtime state (whether a save point
 * exists, what the heat collector did on the previous pass), and the search
 * itself perturbs that state by running passes. So a separate "now do the
 * red leg at position N" step can find the collect at a different ordinal
 * than the search did -- which is exactly how this leg became intermittent,
 * and the symptom was a red control quietly measuring a healthy pass.
 *
 * Asserting on the matching probe's own log removes the gap by
 * construction: the run the claims are made about is a run in which the
 * collect demonstrably failed, because that is the condition that selected
 * it. */
static int find_collect_fault_position(int full_pass, char *out, size_t outcap)
{
    int outer, n;
    /* Two outer passes. A probe that hits a FAILED save-point capture
     * refuses the pass before the collect is ever reached, so that probe
     * cannot match however the position is armed -- and the operator mode
     * refuses on a capture failure for a pre-existing reason unrelated to
     * this WP (observed on an untouched main). One retry of the sweep is
     * enough to get past it; without the retry the search can return 0 on a
     * tree where nothing is wrong, and every leg below would then arm
     * nothing and quietly measure the healthy path -- a red control that
     * stops testing anything without saying so. */
    for (outer = 0; outer < 2; outer++) {
        for (n = 1; n <= 8; n++) {
            char log[CAPMAX];
            g_shutdown = 0;
            cap_start();
            arm_btree_pos("iter_live_inodes", n);
            invf_sweep_worker(full_pass);
            disarm();
            cap_stop_and_get(log, sizeof log);
            /* The predicate names the COLLECT's own line, not "REFUSING":
             * that word is also printed when the save-point capture fails,
             * and matching it would let a capture failure masquerade as a
             * found position. Both spellings are accepted so the search
             * works against a build that discards the status as well as one
             * that handles it -- the defective build reports an empty
             * collect as a whole one ("DONE found=0 ... failed=0"), and
             * accepting only the refusal would make the search return 0
             * against exactly the tree it is meant to measure. */
            if (strstr(log, "INCOMPLETE: the live-set WALK did not complete")
                    != NULL ||
                strstr(log, "DONE found=0") != NULL) {
                snprintf(out, outcap, "%s", log);
                return n;
            }
        }
    }
    if (outcap) out[0] = 0;
    return 0;
}

/* Rebuild the name table with the namespace walk broken, so the degraded
 * window is open when the next assertion starts. The fault is one shot, so
 * each window costs one arming and one walk. */
static void build_degraded_window(void)
{
    g_table_degraded = 0;
    g_table_stale = 1;
    arm_dirs("walk_dir_entry:2");
    table_refresh_if_stale_locked();
    disarm();
}

/* The same, left OPEN.
 *
 * build_degraded_window() heals itself on the very next lookup, because
 * snapshot_entry() retries the rebuild on a miss and the one-shot fault is
 * by then spent. That is the fix working -- but it also means the EIO
 * branch is only reachable on a volume that STAYS damaged, which is what a
 * quarantined base page actually is. So this variant re-arms after the
 * first failure, and the retry inside the next lookup fails too. Without
 * this the legs below would be measuring a self-healing volume and would
 * pass for the wrong reason. */
static void build_degraded_window_open(void)
{
    build_degraded_window();
    arm_dirs("walk_dir_entry:2");
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512], log[CAPMAX];
    invfs_volume *v;
    int err = 0, i, rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("walk_status_fuse_test: the daemon must not tell the kernel a "
           "damaged volume is empty\n");

    snprintf(img, sizeof img, "%s/invf-walk-status-fuse-test.img", dir);
    mkvol(img, 32);
    v = vol_open(img, &err);
    if (!v) { fprintf(stderr, "vol_open failed: %d\n", err); return 2; }
    g_vol = v;                    /* the daemon's only handle on the volume */

    /* The ROOT order matters and is chosen deliberately:
     * vol_path_list_dir sorts by (name_len, name), so the root comes back
     * "a", "b", "d1" -- a file, then a DIRECTORY, then the directory the
     * readdir legs use.
     *
     * That is what makes the seam land on a directory rather than on a
     * file. walk_dir consults it once per entry, INCLUDING the entries
     * it recurses into, so ":2" is the second entry the walk touches; with
     * "a" first (a file, no recursion) that second entry is "b", and what
     * goes missing is b's whole SUBTREE -- the claim under test. With "b"
     * first the second check would land on b's own child instead and the
     * control would be measuring one lost FILE, a much smaller claim. */
    if (!vol_mkdir(v, "b") || !vol_mkdir(v, "d1")) return 2;
    if (!vol_create_file(v, "b/x", (const uint8_t *)"x", 1)) return 2;
    if (!vol_create_file(v, "d1/f0", (const uint8_t *)"x", 1)) return 2;
    if (!vol_create_file(v, "a", (const uint8_t *)"y", 1)) return 2;
    vol_flush(v);

    g_table_degraded = 0;
    g_table_stale = 0;
    table_rebuild_locked();
    ok(g_nentries > 0 && !g_table_degraded,
       "0.  fixture: the table builds clean (%d entries, degraded=%d)",
       g_nentries, g_table_degraded);
    ok(find_entry("b/x") != NULL, "0b. and holds b/x");

    /* =================================================================
     * SITE 3 -- the walk drops a subtree, and the kernel is told ENOENT.
     *
     * The shape of the claim under test is a WINDOW, not a state: a rebuild
     * whose walk stops publishes a short table and marks the volume
     * degraded; the next operation that misses retries the rebuild, and
     * because the injected fault is one-shot the retry succeeds. So each
     * assertion below opens its own window with build_degraded_window() and
     * then makes exactly ONE table-touching call.
     *
     * That is also the honest shape of the fix. The defect was not "the
     * table was short for a moment" -- it was that fuse_fs.c:132 cleared
     * g_table_stale unconditionally, so the short table was the FINAL word:
     * the loss was permanent for the rest of the mount, and the only
     * evidence was ENOENT on files that were on the disk.
     * ================================================================= */
    {
        struct fuse_file_info fi;
        struct stat st;
        int se;

        /* the window, opened and closed around one call */
        build_degraded_window_open();
        ok(g_table_degraded == 1,
           "3a. a rebuild whose walk stopped marks the table DEGRADED "
           "(degraded=%d)", g_table_degraded);
        ok(find_entry("b/x") == NULL,
           "3b. and b/x is NOT in the table -- the subtree went with its "
           "parent entry, and so did everything the walk had not reached yet");
        ok(g_table_stale == 1,
           "3c. and the table is left marked STALE, so a later op retries "
           "(stale=%d). This is the fuse_fs.c:132 clear, and it is the "
           "difference between a moment and the rest of the mount", g_table_stale);

        build_degraded_window_open();
        se = snapshot_entry("b/x", NULL, NULL, NULL);
        ok(se == -1,
           "3d. a lookup for the lost name reports DEGRADED (-1), not absent "
           "(0) (rc=%d)", se);
        if (se == 0)
            printf("        ^ reported ABSENT: the daemon just told the "
                   "kernel a file that is on the disk does not exist\n");

        build_degraded_window();
        build_degraded_window_open();
        memset(&fi, 0, sizeof fi);
        rc = invf_open("/b/x", &fi);
        ok(rc == -EIO,
           "3e. open() on it returns EIO, not ENOENT (rc=%d, want %d)",
           rc, -EIO);
        if (rc == -ENOENT)
            printf("        ^ ENOENT: the daemon just told the kernel a file "
                   "that is on the disk does not exist\n");

        build_degraded_window();
        build_degraded_window_open();
        memset(&st, 0, sizeof st);
        memset(&fi, 0, sizeof fi);
        rc = invf_getattr("/b/x", &st, &fi);
        ok(rc == -EIO,
           "3f. getattr() likewise (rc=%d, want %d)", rc, -EIO);

        build_degraded_window();
        build_degraded_window_open();
        {
            char buf[8];
            memset(buf, 0, sizeof buf);
            memset(&fi, 0, sizeof fi);
            rc = invf_read("/b/x", buf, 1, 0, &fi);
            ok(rc == -EIO,
               "3g. read() on it returns EIO too (rc=%d, want %d) -- this is "
               "what `cat` does, and it is the same three call sites the "
               "finding names", rc, -EIO);
        }

        /* A name the short table DOES hold still resolves: the table is
         * short, not useless, and a fix that failed the whole mount closed
         * would be the other way this can go wrong. */
        build_degraded_window_open();
        /* The walk delivered root entry 1 ("a") and stopped on entry 2
         * ("b"), so the table is exactly that one row and nothing else --
         * not b, not b/, not b/x, and not d1 either, which sorts after and
         * was never the unreadable thing. That last part is the shape of
         * the bug in one line: entries the walk never REACHED are lost
         * exactly as thoroughly as the one it failed on. */
        ok(g_nentries == 1 && find_entry("a") != NULL &&
           find_entry("b") == NULL && find_entry("b/x") == NULL &&
           find_entry("d1") == NULL && find_entry("d1/f0") == NULL,
           "3h. the short table holds exactly what the walk reached (%d row: "
           "a). b, b/x AND d1 are all missing, and d1 was never the "
           "unreadable one", g_nentries);
        ok(snapshot_entry("a", NULL, NULL, NULL) == 1,
           "3h2. and a name it holds still resolves normally");
        disarm();

        /* THE FIX, PROPERLY: the window is one operation wide. The very next
         * lookup retries, the one-shot fault is spent, the rebuild completes
         * and the subtree is back. Before the fix the retry never happened
         * and the answer stayed ENOENT until unmount. */
        build_degraded_window();
        ok(g_table_degraded == 1, "3i. window open again (degraded=%d)",
           g_table_degraded);
        se = snapshot_entry("b/x", NULL, NULL, NULL);
        ok(se == 1,
           "3j. and the NEXT operation heals it: b/x resolves again (rc=%d). "
           "Before the fix the stale flag was cleared unconditionally, so "
           "this retry never happened", se);
        ok(g_table_degraded == 0 && g_table_stale == 0,
           "3k. with both marks cleared once the rebuild completes "
           "(degraded=%d stale=%d)", g_table_degraded, g_table_stale);
        ok(find_entry("b/x") != NULL, "3l. and b/x is in the table again");

        /* the green control at the bottom: a genuinely absent name is
         * ABSENT, with ENOENT, once the table is authoritative again. */
        ok(snapshot_entry("no/such/file", NULL, NULL, NULL) == 0,
           "3m. a genuinely absent name is ABSENT again (0), not degraded");
        memset(&fi, 0, sizeof fi);
        rc = invf_open("/no/such/file", &fi);
        ok(rc == -ENOENT,
           "3n. and open() on it is ENOENT once more (rc=%d) -- the EIO is not "
           "a permanent change of what ENOENT means", rc);
        disarm();
    }

    /* the other readdir shape from site 3: the directory's OWN dirent
     * EIOs. vol_path_list_dir used to answer 0 for that, and 0 is an
     * EMPTY DIRECTORY -- while damage in the children range, one level down,
     * has always given -EIO. The same volume, two answers. */
    {
        seen_reset();
        rc = invf_readdir("/d1", NULL, record_filler, 0, NULL, 0);
        ok(rc == 0 && seen_has("f0"),
           "3l-1. CONTROL: listing d1 works and names f0 (rc=%d)", rc);

        arm_dirs("path_list_dir_lookup:1");
        seen_reset();
        rc = invf_readdir("/d1", NULL, record_filler, 0, NULL, 0);
        ok(rc == -EIO,
           "3m. a directory whose own dirent EIOs reports the error "
           "(rc=%d, want %d)", rc, -EIO);
        if (rc == 0)
            printf("        ^ reported an EMPTY directory (rc=0) with %d "
                   "names: `ls d1` prints nothing and exits 0\n", nseen);
        ok(nseen == 0,
           "3n. and emits nothing rather than a truncated listing (%d names)",
           nseen);
        disarm();
    }

    /* =================================================================
     * SITE 1 — the sweep's DONE line, and whether it rewrote anything.
     *
     * Two passes, because the daemon has two and they answer differently by
     * pre-existing policy (fuse_fs.c: the `!full_pass` arm is the operator's
     * USR1/xattr pass and fails CLOSED; `full_pass` is the watermark ladder
     * and fails OPEN, because abandoning that one leaves the volume worse).
     * What the fix must guarantee for BOTH is the same: the log says the
     * collect was incomplete, and says which kind of incomplete, and never
     * presents the pass as whole.
     *
     * The fault position is found rather than pinned: spt0_capture walks the
     * inode set before the collect does, so ":1" would be spent on the
     * savepoint and this leg would measure the healthy path -- a green leg
     * proving nothing, which is the whole failure mode of the test suite
     * this file is a reaction to.
     * ================================================================= */
    {
        size_t blen = 0;
        uint8_t *buf = NULL;
        uint64_t ino = 0;
        char redlog[CAPMAX];
        int pos = find_collect_fault_position(0, redlog, sizeof redlog);
        int wpos;
        char log2[CAPMAX];

        /* a little more to sweep than the fixture has, so the pass is not a
         * no-op for reasons that have nothing to do with the fault */
        for (i = 0; i < 6; i++) {
            char nm[64];
            snprintf(nm, sizeof nm, "zbulk%d", i);
            if (!vol_create_file(v, nm, (const uint8_t *)"abcdefgh", 8))
                return 2;
        }
        vol_flush(v);
        g_table_stale = 1;
        table_refresh_if_stale_locked();

        if (vol_path_lookup(v, "d1/f0", &ino) != 1) return 2;
        ok(vol_read_file(v, ino, &buf, &blen) == 0 && blen == 1 &&
           buf && buf[0] == 'x',
           "1a. fixture: d1/f0 reads back bit-exactly before the pass");
        free(buf);
        ok(pos > 0,
           "1b. the collect's walk position found (position %d) -- without "
           "this the legs below would be measuring the savepoint capture",
           pos);

        /* ---- green control: the operator pass, healthy ----------------
         * Retried up to three times, and the retry is part of the control
         * rather than a fudge: the operator pass FAILS CLOSED when the
         * save-point capture is refused, and that capture is refused
         * intermittently on a freshly built image for reasons that have
         * nothing to do with this WP (it is observed on an untouched main
         * too). A control that failed on that would be measuring the
         * capture, not the collect. */
        {
            int tries;
            for (tries = 0; tries < 3; tries++) {
                g_shutdown = 0;
                cap_start();
                invf_sweep_worker(0);
                cap_stop_and_get(log, sizeof log);
                if (strstr(log, "[sweep] DONE") != NULL)
                    break;
            }
            printf("        --- healthy operator pass (%d attempt%s) ---\n%s"
                   "        -------------------------------------------\n",
                   tries + 1, tries == 0 ? "" : "s", log);
            ok(tries < 3,
               "1c. CONTROL: a healthy pass completes and prints a DONE line "
               "(%d attempts)", tries + 1);
            ok(strstr(log, "INCOMPLETE") == NULL,
               "1d. and prints no INCOMPLETE");
        }

        /* ---- red: the operator pass, collect walk stops --------------- */
        printf("        --- operator pass, collect walk stopped ---\n%s"
               "        --------------------------------------------------\n", redlog);
        snprintf(log, sizeof log, "%s", redlog);

        ok(strstr(log, "INCOMPLETE") != NULL,
           "1e. an incomplete collect prints INCOMPLETE");
        ok(strstr(log, "live-set WALK did not complete") != NULL,
           "1f. naming the WALK, not the buffer -- they are different "
           "failures and the old message claimed the wrong one");
        ok(strstr(log, "REFUSING the pass") != NULL,
           "1g. and the operator-requested pass REFUSES to run at all "
           "(fail closed, the volume is unchanged)");
        /* THE WRONG ANSWER, before the fix: the collect's status was
         * discarded, so found == n (a stopped walk stores exactly what it
         * saw), `complete` was 1, and the operator got this and nothing
         * else: */
        ok(strstr(log, "[sweep] DONE") == NULL,
           "1h. and NO DONE line at all -- before the fix this was the whole "
           "of what the operator was told");

        /* and nothing was rewritten */
        buf = NULL; blen = 0;
        ok(vol_read_file(v, ino, &buf, &blen) == 0 && blen == 1 &&
           buf && buf[0] == 'x',
           "1i. d1/f0 is still bit-exactly 'x' after the refused pass");
        free(buf);

        /* ---- red: the watermark pass, collect walk stops --------------
         * Fail-open by pre-existing policy, so the assertion is about the
         * LOG: a pass over a partial live set must not be readable as a
         * whole one. This is the "unbounded dead-segment accumulation behind
         * a clean log" of AGENTS.md 2.5, and this line is the log. */
        wpos = find_collect_fault_position(1, redlog, sizeof redlog);
        ok(wpos > 0,
           "1j-1. the watermark pass's collect position found (position %d; the "
           "operator pass's was %d -- they need not agree, because the "
           "watermark pass runs the heat collector first, which walks the "
           "inode set too)", wpos, pos);
        printf("        --- watermark pass, collect walk stopped ---\n%s"
               "        -------------------------------------------------\n", redlog);
        snprintf(log2, sizeof log2, "%s", redlog);

        ok(strstr(log2, "INCOMPLETE") != NULL,
           "1j. the watermark pass also says INCOMPLETE even though it does "
           "not refuse");
        {
            /* WP209: find the watermark pass's OWN DONE line, not the first
             * one in the capture. `strstr(log2, "[sweep] DONE")` took the
             * first, which is the operator pass's when both passes' output
             * ends up in one buffer -- and that is not only the harness's
             * splice, it is a legitimate state of the daemon: the log is a
             * stream, and a reader who greps the tail of it must not be told
             * the wrong pass's line is the answer. The anchor is the pass's
             * own start banner, printed by fuse_fs.c immediately before the
             * per-file loop and therefore immediately before its DONE. */
            const char *anchor = strstr(log2, "watermark pass started");
            const char *done = anchor ? strstr(anchor, "[sweep] DONE")
                                      : strstr(log2, "[sweep] DONE");
            char line[512];
            const char *eol = done ? strchr(done, '\n') : NULL;
            size_t len = eol ? (size_t)(eol - done) + 1
                             : (done ? strlen(done) : 0);
            if (len >= sizeof line) len = sizeof line - 1;
            if (len) { memcpy(line, done, len); line[len] = 0; }
            else line[0] = 0;
            ok(strstr(line, "INCOMPLETE") != NULL,
               "1k. and the WATERMARK pass's own DONE line carries the "
               "marker: %s", line);
            if (line[0] && strstr(line, "INCOMPLETE") == NULL)
                printf("        ^ this is the line that let unbounded "
                       "dead-segment accumulation look like a clean pass\n");
        }
        ok(strstr(log2, "PARTIAL") != NULL,
           "1l. and the pass-start line says PARTIAL too, so the operator "
           "cannot read only the DONE line and miss it");
    }

    g_vol = NULL;
    vol_close(v);

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
