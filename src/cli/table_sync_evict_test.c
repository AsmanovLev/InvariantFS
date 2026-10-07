/* table_sync_evict_test.c — WP140: a lookup that could not be COMPLETED must
 * not evict a name that is live.
 *
 * The defect, at src/cli/fuse_fs.c:349:
 *
 *     id = vol_find(g_vol, name);
 *     if (!id) { table_remove_name(name); return; }
 *
 * vol_find returns a uint64_t, so "there is no such name" and "the lookup
 * could not be completed" are both the value 0. table_sync_one_locked() is the
 * mount's incremental answer to "what is this name" and it runs after EVERY
 * mutation, so one transient dirent-row read error -- a quarantined base page,
 * a fold racing the read -- takes a live file out of the name table for the
 * rest of the mount. Nothing is written to the volume, so the bytes are safe;
 * the MOUNT starts answering ENOENT for a file that is on it.
 *
 * Why this is LATENT and why that is not a defence: invf_readdir is
 * engine-backed (fuse_fs.c:1267 calls vol_list_dir), so `ls` keeps listing the
 * file while `open`/`cat` say it is not there. The two halves of the mount
 * disagree about the same name, and the user-visible artefact is exactly the
 * "ls says it is there, cat says no such file" contradiction that costs an
 * afternoon every time it is met.
 *
 * The assertion is made through the FUSE op table — invf_open, invf_getattr,
 * invf_read, invf_readdir, invf_truncate, invf_unlink — which is the code the
 * kernel calls, and against invf-cat as an INDEPENDENT oracle: invf-cat goes
 * to the volume and never consults the name table, so it is what proves the
 * bytes are on the disk while the mount has stopped serving them.
 *
 * SEAM DISCIPLINE (src/core/vol_fault.h). The failure is arranged with
 * INVFS_FAULT="dirent_row_read:<n>" -- the dirent row read inside
 * vol_dirent_get, which is the read a quarantined base page fails. Two
 * things about it are load-bearing:
 *
 *   - The site lives in src/core/vol_btree.c, so arming it needs
 *     invfs_vol_btree_fault_reload(). unsetenv+setenv is NOT equivalent: it
 *     frees the old spec string, setenv is very often handed the same address
 *     back, the pointer compare sees no change, the countdown stays SPENT and
 *     the leg runs against the healthy path -- green, and proving nothing.
 *
 *   - The ORDINAL is not a constant. truncate(2) resolves the path for the
 *     traversal check, for the permission check, once more for the on-demand
 *     sweep, and then once inside the sync itself, and how many of those there
 *     are is not something this test may assume. So the position is SEARCHED
 *     rather than pinned, and every probe is identified by the signal that
 *     says "the failure landed on the sync's own lookup": either the entry was
 *     taken out of the table (what the defect does) or the daemon said it kept
 *     it (what the fix does). Requiring at-least-one match, and requiring that
 *     NOT every probed position matched, are both asserted -- a search that
 *     matched everywhere would be measuring nothing.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _GNU_SOURCE          /* O_PATH, RENAME_NOREPLACE (see fuse_fs.c) */
#define FUSE_USE_VERSION 31  /* must match fuse_fs.c before any header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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

static const char *g_root = ".";
static char g_img[512];

static void mkvol(const char *img, int mb)
{
    char cmd[1024];
    unlink(img);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s %d >/dev/null 2>&1",
             g_root, img, mb);
    if (system(cmd) != 0) {
        fprintf(stderr, "cannot create volume with invf-mkfs\n");
        exit(2);
    }
}

static void arm_btree(const char *spec)
{
    setenv("INVFS_FAULT", spec, 1);
    invfs_vol_btree_fault_reload();
}
static void disarm(void)
{
    unsetenv("INVFS_FAULT");
    invfs_vol_btree_fault_reload();
}

/* ---- stderr capture ---------------------------------------------------
 * The claim under test on the fixed side is a LOG LINE, so the log is
 * captured through a real pipe (dup2 onto fd 2) with a reader thread, for the
 * reason walk_status_fuse_test.c gives: a return value alone would pass on a
 * build that said nothing to the operator. */
#define CAPMAX 65536
static char cap_buf[CAPMAX];
static size_t cap_len;
static pthread_mutex_t cap_mtx = PTHREAD_MUTEX_INITIALIZER;
static int cap_saved = -1;
static int cap_pipe[2] = { -1, -1 };

static void *cap_reader(void *arg)
{
    (void)arg;
    for (;;) {
        char b[512];
        ssize_t k = read(cap_pipe[0], b, sizeof b);
        if (k <= 0)
            break;
        pthread_mutex_lock(&cap_mtx);
        if (cap_len + (size_t)k < CAPMAX - 1) {
            memcpy(cap_buf + cap_len, b, (size_t)k);
            cap_len += (size_t)k;
            cap_buf[cap_len] = 0;
        }
        pthread_mutex_unlock(&cap_mtx);
        /* No early break on cap_stop (same tail-drop shape as the other
         * cap harnesses): the reader drains to EOF/EBADF. */
    }
    return NULL;
}

static void cap_start(void)
{
    pthread_t th;
    fflush(stderr);
    if (pipe(cap_pipe) != 0)
        return;
    /* Non-blocking, so cap_stop_and_get() can drain what is already in the
     * pipe SYNCHRONOUSLY. Without this the reader thread may not have copied
     * the last line yet when the buffer is read, and the leg then misses the
     * very message it is looking for -- a red control that stops seeing the
     * defect because of a scheduling race, which is the failure mode this file
     * exists to avoid. */
    fcntl(cap_pipe[0], F_SETFL, O_NONBLOCK);
    cap_saved = dup(2);
    dup2(cap_pipe[1], 2);
    close(cap_pipe[1]);
    cap_len = 0; cap_buf[0] = 0;
    pthread_create(&th, NULL, cap_reader, NULL);
    pthread_detach(th);
}

static void cap_stop_and_get(char *out, size_t cap)
{
    fflush(stderr);
    if (cap_saved >= 0) { dup2(cap_saved, 2); close(cap_saved); cap_saved = -1; }
    if (cap_pipe[0] >= 0) {
        for (;;) {                       /* drain what is already there */
            char b[512];
            ssize_t k = read(cap_pipe[0], b, sizeof b);
            if (k <= 0)
                break;
            pthread_mutex_lock(&cap_mtx);
            if (cap_len + (size_t)k < CAPMAX - 1) {
                memcpy(cap_buf + cap_len, b, (size_t)k);
                cap_len += (size_t)k;
                cap_buf[cap_len] = 0;
            }
            pthread_mutex_unlock(&cap_mtx);
        }
        close(cap_pipe[0]);
        cap_pipe[0] = -1;
    }
    pthread_mutex_lock(&cap_mtx);
    snprintf(out, cap, "%s", cap_buf);
    pthread_mutex_unlock(&cap_mtx);
}

/* The daemon's own words about the one thing under test: it could not refresh
 * this name, and it is keeping the entry rather than taking it out. The test
 * matches on the MESSAGE, never on a position, because the position is not a
 * constant. */
#define KEEP_MARK "did not complete"
static const char *KEEP_LINE = "keeping the entry";

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

/* ---- the fixture ------------------------------------------------------
 * VICTIM is the live file whose table entry a failed sync must not take out.
 * The trigger is a real mutation on a real live name, driven through the FUSE
 * op table, so the leg is about the mount and not about the helper: setfattr
 * on the file. It is chosen over truncate(2) for one reason -- it touches NO
 * bytes, so the byte-exactness oracle stays meaningful across the 17 trigger
 * runs the position search costs. (truncate-to-same-length was tried first
 * and the volume's answer was not stable across repeated runs, which would
 * have put a data question inside a metadata test.) */
#define VICTIM "a"
#define VICTIM_XATTR "user.invfs.wp140"
static const char PAYLOAD[] = "INVARIANT-BYTES-0123456789";
#define PLAIN (sizeof PAYLOAD - 1)

static void fixture_rebuild(void)
{
    g_table_degraded = 0;
    g_table_stale = 0;
    pthread_mutex_lock(&g_io_lock);
    table_rebuild_locked();
    pthread_mutex_unlock(&g_io_lock);
}

static int trigger(void)
{
    return invf_setxattr("/" VICTIM, VICTIM_XATTR, "1", 1, 0);
}

/* One probe: arm the seam at `n`, run the trigger, disarm, and report which
 * signal -- if any -- says the failure landed on the SYNC's own lookup.
 *   1 = the entry was taken out of the table  (the defect)
 *   2 = the daemon said it kept the entry      (the fix)
 *   0 = the arming landed somewhere else, or nowhere at all
 * The trigger is re-runnable (same length in, same length out) and the table
 * is rebuilt around every probe, so one probe cannot colour the next. */
static int probe(int n)
{
    char spec[64], log[CAPMAX];
    int rc;

    fixture_rebuild();
    snprintf(spec, sizeof spec, "dirent_row_read:%d", n);
    cap_start();
    arm_btree(spec);
    rc = trigger();
    disarm();
    cap_stop_and_get(log, sizeof log);
    if (rc != 0)
        return 0;                     /* the trigger itself did not run */
    if (strstr(log, KEEP_MARK) && strstr(log, KEEP_LINE))
        return 2;
    if (find_entry(VICTIM) == NULL)
        return 1;
    return 0;
}

/* Search the seam's ordinals. Every probe is traced; the search succeeds only
 * if at least one position produced the identifying signal AND at least one
 * did not (a search that matched everywhere, or nowhere, would be a leg
 * measuring the healthy path). */
#define MAXPOS 16
static int find_sync_position(int *tried, int *matched)
{
    int n, t = 0, m = 0, hit = 0;
    for (n = 1; n <= MAXPOS; n++) {
        int sig = probe(n);
        t++;
        printf("        [search] dirent_row_read:%-2d -> %s\n", n,
               sig == 1 ? "SIGNAL: the entry was EVICTED (the defect)" :
               sig == 2 ? "SIGNAL: the daemon said it KEPT the entry" :
                          "no signal (the failure landed elsewhere, or nowhere)");
        if (sig) { m++; if (!hit) hit = n; }
    }
    *tried = t;
    *matched = m;
    return (m >= 1 && m < t) ? hit : 0;
}

/* ---- byte-exactness oracles ------------------------------------------
 *
 * Two of them, because one is not enough to say WHO is wrong.
 *
 * read_by_name() resolves the name straight through the engine
 * (vol_find + vol_read_file) and never touches the name table. It is what says
 * "the file and its bytes are on the volume" from inside the process, at every
 * step.
 *
 * cat_matches() runs the real `invf-cat` binary as a subprocess, which is the
 * out-of-process oracle an operator would use. It can only run once the volume
 * is closed -- the engine takes an exclusive "in use by another process" lock
 * on the image, which is itself worth knowing: there is no way to read the
 * volume out from under a live daemon, so the daemon's own answers are all a
 * user has while it is mounted. */
static int read_by_name(const char *name, const char *expect, size_t len)
{
    uint64_t ino = 0;
    uint8_t *buf = NULL;
    size_t blen = 0;
    int good;

    ino = vol_find(g_vol, name);
    if (ino == 0)
        return 0;
    if (vol_read_file(g_vol, ino, &buf, &blen) != 0)
        return 0;
    good = (blen == len) && (len == 0 || memcmp(buf, expect, len) == 0);
    free(buf);
    return good;
}

static int cat_matches(const char *name, const char *expect, size_t len,
                       const char *dir)
{
    char out[512], logp[512], cmd[4096];
    char got[512], ctl[4096];
    FILE *f;
    size_t n;
    int good = 0;

    snprintf(out, sizeof out, "%s/%s", dir, "invf-cat-out.bin");
    snprintf(logp, sizeof logp, "%s/%s", dir, "invf-cat-err.txt");
    snprintf(cmd, sizeof cmd,
             "%s/bin/invf-cat %s %s %s >%s 2>%s", g_root, g_img, name, out,
             logp, logp);
    if (system(cmd) != 0) {
        ctl[0] = 0;
        f = fopen(logp, "rb");
        if (f) {
            n = fread(ctl, 1, sizeof ctl - 1, f);
            ctl[n] = 0;
            fclose(f);
        }
        printf("        [invf-cat] FAILED for %s: %s", name, ctl);
        unlink(logp);
        return 0;
    }
    unlink(logp);
    f = fopen(out, "rb");
    if (!f) {
        printf("        [invf-cat] no output file: %s\n", out);
        return 0;
    }
    n = fread(got, 1, sizeof got - 1, f);
    fclose(f);
    unlink(out);
    if (n == len && memcmp(got, expect, len) == 0)
        good = 1;
    else
        printf("        [invf-cat] %s: got %zu bytes, want %zu\n", name, n, len);
    return good;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    invfs_volume *v;
    int err = 0, pos = 0, tried = 0, matched = 0, rc = 0;
    char log[CAPMAX];

    setvbuf(stdout, NULL, _IONBF, 0);
    if (getenv("PWD")) g_root = getenv("PWD");
    printf("table_sync_evict_test: a lookup that could not be completed must "
           "not take a live name out of the table\n");

    snprintf(g_img, sizeof g_img, "%s/invf-table-sync-evict-test.img", dir);
    mkvol(g_img, 32);
    v = vol_open(g_img, &err);
    if (!v) { fprintf(stderr, "vol_open failed: %d\n", err); return 2; }
    g_vol = v;

    /* ---- fixture ---------------------------------------------------- */
    if (!vol_create_file(v, VICTIM, (const uint8_t *)PAYLOAD, PLAIN)) return 2;
    if (!vol_create_file(v, "scratch", (const uint8_t *)"S", 1)) return 2;
    if (!vol_mkdir(v, "d1")) return 2;
    if (!vol_create_file(v, "d1/f0", (const uint8_t *)"x", 1)) return 2;
    vol_flush(v);
    fixture_rebuild();

    ok(find_entry(VICTIM) != NULL, "0a. fixture: the table holds " VICTIM);
    {
        struct fuse_file_info fi;
        memset(&fi, 0, sizeof fi);
        ok(invf_open("/" VICTIM, &fi) == 0, "0b. and open() on it succeeds");
    }
    seen_reset();
    rc = invf_readdir("/", NULL, record_filler, 0, NULL, 0);
    ok(rc == 0 && seen_has(VICTIM),
       "0c. and readdir(\"/\") names it (rc=%d, %d names)", rc, nseen);
    ok(read_by_name(VICTIM, PAYLOAD, PLAIN),
       "0d. and the ENGINE reads its bytes off the volume, bit-exactly -- the "
       "oracle that says the file is there, from outside the name table");

    /* ---- control: the trigger alone changes nothing -------------------- */
    {
        struct fuse_file_info fi;
        memset(&fi, 0, sizeof fi);
        cap_start();
        ok(trigger() == 0, "1a. CONTROL: an UNARMED setfattr on the live "
           "file succeeds (rc=0)");
        cap_stop_and_get(log, sizeof log);
        ok(strstr(log, KEEP_MARK) == NULL,
           "1b. and says nothing about a lookup it did not fail");
        ok(find_entry(VICTIM) != NULL,
           "1c. and leaves the entry in the table");
        ok(read_by_name(VICTIM, PAYLOAD, PLAIN),
           "1d. and the bytes are unchanged");
        memset(&fi, 0, sizeof fi);
        ok(invf_open("/" VICTIM, &fi) == 0,
           "1e. and the mount still opens it");
    }

    /* ---- find the seam's ordinal ------------------------------------- */
    printf("        --- searching the seam's ordinal (per-position trace) ---\n");
    pos = find_sync_position(&tried, &matched);
    ok(pos > 0,
       "2a. the position whose failure lands on the SYNC's own lookup was "
       "found (position %d of %d probed, %d signalled)", pos, tried, matched);
    if (pos <= 0) {
        printf("        ^ no position produced the identifying signal. Either "
               "the seam moved where this test can no longer reach it, or the "
               "daemon is evicting without saying so. Both are worth failing "
               "on: a red control that has quietly stopped testing anything is "
               "worse than no red control.\n");
    }
    ok(matched < tried,
       "2b. and NOT every position signalled (%d of %d) -- so the arming is "
       "demonstrably position-sensitive and the legs below are not measuring a "
       "no-op", matched, tried);

    /* ---- THE USER-VISIBLE SYMPTOM ------------------------------------
     * One trigger, one assertion, all on the SAME trigger run, because the
     * claim is that ONE transient failure has this whole shape and not that
     * four separate runs each misbehave. The table is rebuilt first, so the
     * entry under test is present before the failure. */
    if (pos > 0) {
        struct fuse_file_info fi;
        struct stat st;
        char spec[64];
        int se;

        fixture_rebuild();
        snprintf(spec, sizeof spec, "dirent_row_read:%d", pos);
        cap_start();
        arm_btree(spec);
        rc = trigger();
        disarm();
        cap_stop_and_get(log, sizeof log);
        printf("        --- one transient failed lookup (position %d), then "
               "the mount answers ---\n%s"
               "        -----------------------------------------------------\n",
               pos, log[0] ? log : "        (the daemon said nothing at all)\n");
        ok(rc == 0, "3a. the mutation itself still succeeded (rc=%d): the "
           "failure was in the READ that refreshes the name table, not in the "
           "write", rc);

        /* `ls` first. It passes before and after the fix, and that is the
         * point: invf_readdir is engine-backed, so the listing half of the
         * mount never consulted the table that lost the name. That asymmetry
         * is what makes this defect LATENT, and it is why the operator sees
         * `ls` and `cat` disagree about the same file. */
        seen_reset();
        rc = invf_readdir("/", NULL, record_filler, 0, NULL, 0);
        ok(rc == 0 && seen_has(VICTIM),
           "3b. readdir(\"/\") still names " VICTIM " (rc=%d, %d names) -- `ls` "
           "goes to the engine, which is why this one was never broken",
           rc, nseen);

        /* now the half that WAS broken */
        se = snapshot_entry(VICTIM, NULL, NULL, NULL);
        ok(se == 1,
           "3c. the mount's LOOKUP of " VICTIM " still answers (rc=%d, want 1). "
           "0 means ABSENT and -1 means DEGRADED; before the fix a transient "
           "read error produced 0, i.e. the daemon told the kernel a file that "
           "is on the disk does not exist", se);

        memset(&fi, 0, sizeof fi);
        rc = invf_open("/" VICTIM, &fi);
        ok(rc == 0, "3d. open() on it succeeds (rc=%d, want 0)", rc);
        if (rc == -ENOENT)
            printf("        ^ ENOENT on a file that is on the volume and that "
                   "readdir just listed: `ls` says it is there, `cat` says it "
                   "is not\n");

        memset(&st, 0, sizeof st);
        memset(&fi, 0, sizeof fi);
        rc = invf_getattr("/" VICTIM, &st, &fi);
        ok(rc == 0 && (uint64_t)st.st_size == PLAIN,
           "3e. getattr() reports its real size (rc=%d size=%lld, want %zu)",
           rc, (long long)st.st_size, PLAIN);

        {
            char buf[64];
            memset(buf, 0, sizeof buf);
            memset(&fi, 0, sizeof fi);
            rc = invf_read("/" VICTIM, buf, sizeof buf, 0, &fi);
            ok(rc == (int)PLAIN && memcmp(buf, PAYLOAD, PLAIN) == 0,
               "3f. read() returns its bytes, bit-exactly (rc=%d, want %zu; "
               "first byte %s)", rc, PLAIN,
               rc > 0 ? (memcmp(buf, PAYLOAD, PLAIN) ? "WRONG" : "ok") : "none");
        }

        ok(read_by_name(VICTIM, PAYLOAD, PLAIN),
           "3g. and the ENGINE still resolves the name and returns the same "
           "bytes, bit for bit -- so the file never left the volume and it is "
           "the MOUNT that was wrong, not the disk");

        /* Not a one-call window. The eviction did not mark the table stale, and
         * the rebuild only runs on a MISS, so nothing retried. An unrelated
         * mutation afterwards must not be what finally fixes it -- and before
         * the fix it is the only thing that could. */
        memset(&fi, 0, sizeof fi);
        rc = invf_unlink("/d1/f0");
        ok(rc == 0, "3h. an unrelated mutation afterwards succeeds (rc=%d)", rc);
        ok(find_entry(VICTIM) != NULL,
           "3i. and " VICTIM " is still in the table -- before the fix the loss "
           "was not even one operation wide; it lasted until some other op "
           "marked the table stale, or the next mount");
        memset(&fi, 0, sizeof fi);
        rc = invf_open("/" VICTIM, &fi);
        ok(rc == 0, "3j. and the mount still opens it (rc=%d)", rc);

        /* ---- the green control that keeps the fix honest --------------
         * KEEP is only right for a name whose lookup FAILED. A name that was
         * really deleted must still leave the table, or the fix has turned the
         * name table into a graveyard of tombstones -- and a resurrected name
         * resolves to an inode whose blocks unlink already handed back to the
         * allocator, which is a worse failure than the one being fixed. */
        memset(&fi, 0, sizeof fi);
        rc = invf_unlink("/scratch");
        ok(rc == 0, "4a. CONTROL: a real unlink(\"scratch\") succeeds (rc=%d)", rc);
        ok(find_entry("scratch") == NULL,
           "4b. and a genuinely DELETED name does leave the table");
        seen_reset();
        rc = invf_readdir("/", NULL, record_filler, 0, NULL, 0);
        ok(rc == 0 && !seen_has("scratch"),
           "4c. and readdir no longer names it (rc=%d) -- only a lookup that "
           "positively said ABSENT may evict", rc);
        memset(&fi, 0, sizeof fi);
        rc = invf_open("/scratch", &fi);
        ok(rc == -ENOENT,
           "4d. and open() on it is ENOENT, as it must be (rc=%d, want %d)",
           rc, -ENOENT);
        ok(read_by_name(VICTIM, PAYLOAD, PLAIN),
           "4e. and the victim of the failed lookup is still byte-exact at the "
           "end of the run");
    } else {
        ok(0, "3a. SKIPPED: no seam position was found, so the user-visible "
           "legs never ran. A red control that stops testing anything must "
           "fail, and it just did");
    }

    g_vol = NULL;
    vol_close(v);

    /* The last oracle, and the only one that runs the real binary. It has to
     * come after vol_close: the engine holds the image exclusively ("in use by
     * another process"), so there is no way to check the volume from outside
     * while the daemon has it open. That is worth stating plainly -- for the
     * whole of a mount, the daemon's own answers are the only ones there are,
     * which is why "the mount says ENOENT" is not softened by "the tool says
     * otherwise" when the tool cannot be run. */
    ok(cat_matches(VICTIM, PAYLOAD, PLAIN, dir),
       "5a. and once the daemon has closed it, `invf-cat` on the image reads "
       "the same bytes -- nothing was ever lost, in RAM or on disk");

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
