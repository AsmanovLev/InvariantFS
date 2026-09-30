/* walk_status_test.c — WP135: "a walk stopped" must not read as "a walk
 * finished", at any of the five sites, and the receipt that says so must not
 * cry wolf on an empty volume.
 *
 * The class. A v3 walk is fallible: vol_v3_inode_get() returns -1 for a
 * quarantined or unreadable base page and the walk stops there, having
 * delivered a PREFIX. Every primitive returns a status for exactly that.
 * Five callers took the status and dropped it, and each turned a partial
 * answer into a whole one on its own terms:
 *
 *   1 src/core/vol_sweep.c:1853  the sweep collect reported a short live set
 *     as COMPLETE, so the daemon printed an ordinary DONE line and swept a
 *     subset -- dead segments accumulate behind a clean log (AGENTS.md 2.5)
 *   2 src/cli/verify.c:335      `invf-verify --deep` said "0 corrupt"
 *   3 src/cli/fuse_fs.c:228     the name table lost whole SUBTREES and the
 *     user's `ls` got ENOENT for files still on the disk
 *   4 tools/invf-sweep.c:1768   warned "walk did not complete", then swept
 *   5 src/core/vol_btree.c:4854 vol_v3_name_of answered "no such name"
 *
 * The engine half of site 3 and sites 1, 2, 4 and 5 are here. The FUSE half
 * of site 3 -- the name table, the ENOENT, the DONE line -- is
 * walk_status_fuse_test.c, which includes fuse_fs.c the way
 * readdir_error_test.c does.
 *
 * ARMING. Every failure below is arranged with the test-only seam in
 * src/core/vol_fault.h: INVFS_FAULT="<site>:<n>", one shot. Two rules this
 * test obeys without exception, because breaking either produces a GREEN leg
 * that proves nothing:
 *
 *   - invfs_vol_fault_reload() is how a site is re-armed, and its state is
 *     per translation unit, so this test uses the two cross-TU doors
 *     (invfs_vol_btree_fault_reload for vol_btree.c's iter_live_inodes,
 *     invfs_vol_dirs_fault_reload for vol_dirs.c's three). unsetenv+setenv
 *     is NOT a substitute: unsetenv frees the string, setenv usually gets
 *     the same address back, the spec is compared BY POINTER, the countdown
 *     stays spent, and the leg measures the healthy path.
 *
 *   - the fault is one shot, so a leg that arms a site and then makes two
 *     calls sees exactly one failure. The green control around each red one
 *     is what proves the seam is positional and not sticky.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>

#include "volume_internal.h"
#include "vol_fault.h"
#include "vol_walk.h"

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

/* ---- walk callbacks ------------------------------------------------- */

/* counts the namespace entries the walk reached; also the list, so a leg can
 * say WHICH of them arrived (a prefix is the claim being tested). */
typedef struct { int n; char path[16][64]; } walk_seen;
static walk_seen g_seen;

static int walk_record_cb(void *c_, const char *path, uint64_t ino,
                          uint32_t type, uint64_t size, int64_t mtime)
{
    walk_seen *w = (walk_seen *)c_;
    (void)ino; (void)type; (void)size; (void)mtime;
    if (w->n < 16)
        snprintf(w->path[w->n], sizeof w->path[0], "%s", path);
    w->n++;
    return 0;
}

static int walk_count_cb(void *c_, const char *path, uint64_t ino,
                         uint32_t type, uint64_t size, int64_t mtime)
{
    int *n = (int *)c_;
    (void)path; (void)ino; (void)type; (void)size; (void)mtime;
    (*n)++;
    return 0;
}

static int walk_saw(const walk_seen *w, const char *path)
{
    int i;
    for (i = 0; i < w->n && i < 16; i++)
        if (strcmp(w->path[i], path) == 0)
            return 1;
    return 0;
}

/* ---- harness -------------------------------------------------------- */

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

/* Arm a site in vol_dirs.c / vol_btree.c and make sure the arming is really
 * fresh. The reload is the whole point: without it the second arming of the
 * same spec is a no-op and the leg below quietly measures the healthy path. */
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

/* run a command with stdout+stderr captured into buf; return the exit status */
static int run_capture(const char *cmd, char *buf, size_t cap)
{
    char tmp[512], line[1200];
    FILE *f;
    size_t got = 0;
    int rc;

    snprintf(tmp, sizeof tmp, "%s/walk_status_test.out", getenv("WST_SCRATCH"));
    snprintf(line, sizeof line, "%s >%s 2>&1", cmd, tmp);
    rc = system(line);
    f = fopen(tmp, "r");
    if (f) {
        got = fread(buf, 1, cap - 1, f);
        fclose(f);
    }
    buf[got] = 0;
    return rc;
}

/* Run one function with stderr redirected to a file, and hand back what it
 * said. Used to assert on a DIAGNOSTIC rather than on a return value,
 * because "it said so" and "it returned -1" are different claims and only
 * the first one is the property under test. */
static char err_cap[8192];

static int capture_stderr(void (*fn)(void *), void *arg, char *out, size_t cap)
{
    char tmp[600];
    FILE *f;
    size_t got = 0;
    int saved;

    snprintf(tmp, sizeof tmp, "%s/walk_status_test.err", getenv("WST_SCRATCH"));
    fflush(stderr);
    saved = dup(2);
    if (!freopen(tmp, "w", stderr)) return -1;
    fn(arg);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    f = fopen(tmp, "r");
    if (f) { got = fread(out, 1, cap - 1, f); fclose(f); }
    out[got] = 0;
    return 0;
}

/* The two helpers below do discard their return value, because the claims
 * being tested are about what was PRINTED. The compiler says so anyway --
 * `(void)` does not silence warn_unused_result on this toolchain, which is
 * the property vol_walk.h relies on, demonstrated in the file that
 * measures it. Stashing the value is the way to write this so the build
 * stays quiet. */
static int g_sink;

static void abandon_short(void *arg)
{
    g_sink = vol_walk_abandon((vol_walk_t *)arg);
}

static void commit_unarmed(void *arg)
{
    g_sink = vol_walk_commit((vol_walk_t *)arg);
}

/* Does the binary we are about to EXEC actually contain `needle`?
 *
 * This exists because the two CLI legs are the only parts of this file that
 * can measure a STALE build, and the failure they produce is unreadable.
 * `make -j6` does not relink bin/invf-verify, so a core change leaves it
 * stale until it is rebuilt -- and when site 2's search runs against a stale
 * one, no walk position can produce the deep pass's refusal (the refusal
 * does not exist in that binary), so the search reports
 *
 *     FAIL 2a. exactly one walk position is the deep pass's own (hits=0, pos 0)
 *
 * which reads as "the fix regressed" and is in fact "you are running the old
 * binary". That is the exact confusion this file exists to end, committed
 * inside the file meant to end it.
 *
 * The second half of the diagnosis is below, and it exists because "the
 * binary lacks the string" has TWO causes whose remedies are opposite.
 */
static int binary_older_than(const char *bin, const char *src)
{
    struct stat sb, ss;
    if (stat(bin, &sb) != 0 || stat(src, &ss) != 0)
        return -1;
    return sb.st_mtime < ss.st_mtime;
}

/* Which of the two things it can be.
 *
 * It earns its keep on a real case. After the v2 purge, the hand-resolved
 * merge in tools/invf-sweep.c kept the receipt calls and dropped the
 * refusal -- so the binary was CURRENT and the SOURCE was wrong. A guard
 * that said only "STALE -- rebuild", which is what this one said at first,
 * sends the reader to rebuild; they see no change and get no explanation.
 * The mtime compare is a hint and not a proof (a checkout or a touch can
 * move either), which is why the message states both facts rather than
 * picking one silently. */
static const char *diagnose(const char *bin, const char *src, int fresh)
{
    static char msg[512];
    int older = binary_older_than(bin, src);

    if (fresh < 0)
        snprintf(msg, sizeof msg, "%s could not be read at all.", bin);
    else if (older == 1)
        snprintf(msg, sizeof msg,
                 "%s is STALE: it is older than %s. Rebuild: rm -f %s && "
                 "make %s", bin, src, bin, bin);
    else
        snprintf(msg, sizeof msg,
                 "%s is CURRENT (not older than %s) and still lacks the "
                 "refusal, so the fix is missing from the SOURCE as well -- "
                 "a merge dropped it. Rebuilding will not help; restore the "
                 "refusal.", bin, src);
    return msg;
}

static int binary_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long sz;
    int found = 0;

    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64L * 1024 * 1024) { fclose(f); return -1; }
    buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) == (size_t)sz)
        found = memmem(buf, (size_t)sz, needle, strlen(needle)) != NULL;
    free(buf);
    fclose(f);
    return found;
}

static int ino_of(invfs_volume *v, const char *path)
{
    uint64_t ino = 0;
    if (vol_v3_path_lookup(v, path, &ino) != 1)
        return 0;
    return (int)ino;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512], empty[512], cmd[1400], out[32768];
    invfs_volume *v;
    int err = 0, rc, i;

    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("WST_SCRATCH", dir, 1);
    printf("walk_status_test: a v3 walk that stopped must not read as a walk "
           "that finished\n");

    snprintf(img, sizeof img, "%s/invf-walk-status-test.img", dir);
    mkvol(img, 32);
    v = vol_open(img, &err);
    if (!v) { fprintf(stderr, "vol_open failed: %d\n", err); return 2; }

    /* A tree with two directories, each holding a file, plus a root file.
     * vol_v3_path_list_dir sorts by (name_len, name), so the root entries
     * come back d1, d2, f0 and the Nth iteration of v3_walk_dir's entry
     * loop is predictable -- which is what lets the fault name a position
     * instead of "somewhere". */
    if (!vol_mkdir(v, "d1") || !vol_mkdir(v, "d2")) return 2;
    for (i = 0; i < 2; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "d%d/f%d", i + 1, i);
        if (!vol_create_file(v, nm, (const uint8_t *)"x", 1)) return 2;
    }
    if (!vol_create_file(v, "f0", (const uint8_t *)"y", 1)) return 2;
    vol_flush(v);

    /* =================================================================
     * SITE 1 — src/core/vol_sweep.c:1853, the collect.
     *
     * THE WRONG ANSWER, before the fix: vol_collect_sweepables_grow()
     * returned 0. The collect had found 0 inodes (the base scan failed
     * before the callback ever ran), stored 0, and `found <= n` — 0 <= 0 —
     * read as "the walk reached the end of the live set: COMPLETE". The FUSE
     * worker then printed an ordinary DONE line and the sweep proceeded to
     * rewrite whatever it had.
     *
     * The two counts agreeing is the trap: on a walk that STOPS, found
     * always equals n, because the walk saw exactly what it stored. So
     * found == n proves "not truncated by the cap" and never "reached the
     * end". Only the walk's own status can say the second thing.
     * ================================================================= */
    {
        uint64_t *ids = NULL;
        size_t cap = 0, n = 0, found = 0;
        vol_walk_t w;

        /* green control first: same call, no fault. */
        disarm();
        rc = vol_collect_sweepables_grow(v, &ids, &cap, &n, &found);
        ok(rc == 0,
           "1a. CONTROL: a healthy collect returns 0 (rc=%d, n=%zu found=%zu)",
           rc, n, found);
        ok(rc == 0 && found == n && n > 0,
           "1b. and the counts reconcile on a healthy volume (n=%zu found=%zu)",
           n, found);
        free(ids); ids = NULL; cap = 0; n = 0; found = 0;

        /* red: the live-set base scan fails. */
        arm_btree("iter_live_inodes:1");
        rc = vol_collect_sweepables_grow(v, &ids, &cap, &n, &found);
        ok(rc == -1,
           "1c. a collect whose live-set WALK stopped returns -1, not 0 "
           "(rc=%d)", rc);
        if (rc == 0)
            printf("        ^ reported COMPLETE over a walk that stopped: the "
                   "sweep would print DONE and rewrite a subset\n");
        ok(found <= n,
           "1d. the counts still agree (n=%zu found=%zu) — which is exactly "
           "why the counts alone could never catch this", n, found);
        ok(found == 0,
           "1e. and the walk stored nothing, so the subset is empty (n=%zu)", n);
        free(ids);

        /* the receipt says the same thing, and refuses to yield a count. */
        vol_walk_init(&w, v, "test");
        vol_walk_result(&w, -1, 0, 0);
        ok(vol_walk_n(&w) == VOL_WALK_NO_COUNT,
           "1f. vol_walk_n() refuses to hand out a count from a stopped walk "
           "(got %zu, sentinel %zu)", vol_walk_n(&w), VOL_WALK_NO_COUNT);
        ok(vol_walk_commit(&w) == -1,
           "1g. vol_walk_commit() reports the shortfall (-1)");
        disarm();

        /* the engine surface underneath it, one call deeper */
        {
            int wrc = 0;
            uint64_t probe[8];
            size_t pf = 0;
            size_t pn;
            disarm();
            pn = vol_collect_sweepables_ex(v, probe, 8, &pf, &wrc);
            ok(wrc == 0 && pn == 5,
               "1h. CONTROL: vol_collect_sweepables_ex reports rc=0 and 5 "
               "inodes on a healthy volume (rc=%d n=%zu)", wrc, pn);
            arm_btree("iter_live_inodes:1");
            pn = vol_collect_sweepables_ex(v, probe, 8, &pf, &wrc);
            ok(wrc != 0,
               "1i. and rc != 0 when the walk stops (rc=%d n=%zu) — this "
               "out-param is the whole fix", wrc, pn);
            disarm();
        }
    }

    /* =================================================================
     * SITE 3, engine half — src/core/vol_dirs.c.
     *
     * (a) vol_v3_path_list_dir: `else if (vol_v3_path_lookup(...) != 1)
     *     return 0;` collapsed "the name resolves to nothing" and "the
     *     lookup FAILED" into one answer, and 0 is an EMPTY DIRECTORY.
     *     Damage in the CHILDREN range already produced -EIO further down,
     *     so the same volume answered `ls` two different ways depending on
     *     which range was unreadable.
     *
     * (b) v3_walk_dir: `if (... != 1) continue;` dropped the entry AND —
     *     because the recursion is inside the same loop — its whole
     *     SUBTREE, from a walk that returned 0, i.e. COMPLETE.
     * ================================================================= */
    {
        invfs_dirent ents[16];

        /* (a) green: a directory that reads. */
        rc = vol_v3_path_list_dir(v, "d1", ents, 16);
        ok(rc == 1, "3a. CONTROL: listing d1 returns its 1 entry (rc=%d)", rc);
        ok(rc == 1 && strcmp(ents[0].name, "f0") == 0,
           "3b. and names it %s", rc == 1 ? ents[0].name : "?");

        /* (a) red: d1's OWN dirent is unreadable. */
        arm_dirs("path_list_dir_lookup:1");
        rc = vol_v3_path_list_dir(v, "d1", ents, 16);
        ok(rc == -EIO,
           "3c. a directory whose own dirent EIOs returns -EIO (rc=%d, want "
           "%d)", rc, -EIO);
        if (rc == 0)
            printf("        ^ returned 0 = AN EMPTY DIRECTORY: `ls d1` prints "
                   "nothing and exits 0 on a volume it could not read\n");
        /* one shot: the next call is the real one again */
        rc = vol_v3_path_list_dir(v, "d1", ents, 16);
        ok(rc == 1, "3d. and the call after the spent fault is the real one "
                    "(rc=%d)", rc);
        disarm();

        /* (b) green: the whole tree is reachable. */
        memset(&g_seen, 0, sizeof g_seen);
        rc = vol_v3_walk(v, walk_record_cb, &g_seen);
        ok(rc == 0 && g_seen.n == 5,
           "3e. CONTROL: a healthy walk returns 0 and delivers all 5 namespace "
           "entries (rc=%d n=%d)", rc, g_seen.n);
        ok(walk_saw(&g_seen, "d2/f1"),
           "3f. including the nested one");

        /* (b) red: the second root entry (d2) cannot be resolved. Its file
         * d2/f1 is what disappears.
         *
         * There are TWO walks here and the difference between them IS the
         * finding. vol_v3_walk() is lenient: it steps over an entry it
         * cannot resolve and returns 0, i.e. COMPLETE, having silently
         * dropped that entry AND its subtree. vol_v3_walk_strict() stops.
         * Upstream added the strict variant and gave it exactly one caller
         * (pba_ref_ensure, whose map gates every data-block free); the four
         * callers whose partial views get PRINTED -- verify's file list,
         * the mount's name table, the sweep's live set, the reverse name
         * lookup -- were left on the lenient one. So both halves are
         * asserted: the lenient one loses a subtree and claims success,
         * and the strict one is what actually reports it. */
        /* ":3", not ":2": v3_walk_dir consults the seam once per entry
         * INCLUDING the entries it recurses into, and the root sorts d1,
         * d2, f0 -- so check 1 is d1, check 2 is d1's own child, and check
         * 3 is d2. ":3" is therefore the first check that lands on a
         * DIRECTORY, which is the case that takes a subtree. ":2" would
         * lose one file and call that the same bug. */
        memset(&g_seen, 0, sizeof g_seen);
        arm_dirs("v3_walk_dir_entry:3");
        rc = vol_v3_walk(v, walk_record_cb, &g_seen);
        ok(rc == 0,
           "3g. the LENIENT walk returns 0 having stepped over the entry -- "
           "COMPLETE, and the caller cannot tell (rc=%d, %d entries)",
           rc, g_seen.n);
        ok(!walk_saw(&g_seen, "d2/f1") && !walk_saw(&g_seen, "d2"),
           "3g2. and the entry AND its whole subtree are simply not in the "
           "list: that is the shape that made a mount lose a directory");
        disarm();

        memset(&g_seen, 0, sizeof g_seen);
        arm_dirs("v3_walk_dir_entry:3");
        rc = vol_v3_walk_strict(v, walk_record_cb, &g_seen);
        ok(rc < 0,
           "3h. the STRICT walk returns an error on the same damage (rc=%d) -- "
           "and all four of the callers this WP moves use THIS one", rc);
        if (rc == 0)
            printf("        ^ returned 0 (COMPLETE) having skipped an entry and "
                   "its subtree: d2/ and everything under it are gone\n");
        ok(g_seen.n > 0 && g_seen.n < 5,
           "3i. and what it delivered is a PREFIX: %d of 5 entries", g_seen.n);
        ok(!walk_saw(&g_seen, "d2/f1"),
           "3i2. the subtree under the unreadable entry is not in the list");
        disarm();

        /* the second seam: the entry RESOLVES but its inode ROW cannot be
         * read. Same shape, one level in -- the type of an entry is what
         * decides whether its subtree is walked at all, so a skipped row
         * drops a directory just as surely. */
        memset(&g_seen, 0, sizeof g_seen);
        arm_dirs("v3_walk_dir_row:2");
        rc = vol_v3_walk_strict(v, walk_record_cb, &g_seen);
        ok(rc < 0,
           "3j. an unreadable inode ROW stops the strict walk too (rc=%d, "
           "%d entries)", rc, g_seen.n);
        disarm();

        memset(&g_seen, 0, sizeof g_seen);
        rc = vol_v3_walk_strict(v, walk_record_cb, &g_seen);
        ok(rc == 0 && g_seen.n == 5,
           "3k. both walks recover once the fault is spent (rc=%d n=%d)",
           rc, g_seen.n);
    }

    /* =================================================================
     * SITE 5 — src/core/vol_btree.c:4854, vol_v3_name_of.
     *
     * THE WRONG ANSWER: `(void)vol_v3_walk(...)` then `return c.found ? 1 : 0`.
     * A walk that stopped found nothing, so it answered 0, ABSENT. Two
     * callers act on that: vol_sweep_name_of() turned it into "this file is
     * gone" and skipped the inode, and the iterator's per-inode fallback
     * turned it into NULL, which the API documents as the ordinary
     * "no dirent reference" case.
     * ================================================================= */
    {
        char nm[256];
        uint64_t parent = 0, ino = 0;

        ok(vol_v3_path_lookup(v, "d1/f0", &ino) == 1,
           "5a. fixture: resolved d1/f0 to inode %llu",
           (unsigned long long)ino);

        rc = vol_v3_name_of(v, ino, nm, sizeof nm, &parent);
        ok(rc == 1, "5b. CONTROL: the reverse lookup finds the name (rc=%d, "
                    "name=%s)", rc, nm);

        arm_dirs("v3_walk_dir_stop:1");
        rc = vol_v3_name_of(v, ino, nm, sizeof nm, &parent);
        ok(rc == -1,
           "5c. a stopped walk makes the reverse lookup return -1, not 0 "
           "(rc=%d)", rc);
        if (rc == 0)
            printf("        ^ answered 0 = \"this inode has no name\": the "
                   "sweep then skips it as deleted\n");

        /* and the wrapper on top, which used to flatten the -1 to 0 */
        arm_dirs("v3_walk_dir_stop:1");
        rc = vol_sweep_name_of(v, ino, nm, sizeof nm);
        ok(rc == -1,
           "5d. vol_sweep_name_of propagates it too (rc=%d) — it used to be "
           "`(rc > 0) ? 1 : 0`", rc);
        disarm();

        rc = vol_v3_name_of(v, ino, nm, sizeof nm, &parent);
        ok(rc == 1, "5e. and the lookup recovers once the fault is spent "
                    "(rc=%d)", rc);
    }

    /* =================================================================
     * THE LATCH — the half of the wrapper that warn_unused_result cannot
     * reach. A caller that drops a short walk with an explicit (void) cast
     * gets no build warning at all, so the volume remembers it instead and
     * says so at vol_close().
     * ================================================================= */
    {
        vol_walk_t w;
        size_t after;

        disarm();
        after = vol_walk_reap(v);
        ok(after == 0,
           "6a. CONTROL: no unclaimed walks outstanding (reap=%zu)", after);

        /* the FORGETTING caller: records the result, never discharges it */
        vol_walk_init(&w, v, "forgotten");
        vol_walk_result(&w, -1, 4, 4);
        after = vol_walk_reap(v);
        ok(after == 1,
           "6b. a short walk nobody claimed is visible to the volume "
           "(reap=%zu)", after);

        /* the CHECKING caller */
        vol_walk_init(&w, v, "claimed");
        vol_walk_result(&w, -1, 4, 4);
        rc = vol_walk_commit(&w);
        after = vol_walk_reap(v);
        ok(after == 0, "6c. and invisible once committed (reap=%zu, commit "
            "said %d)", after, rc);

        /* the DELIBERATE caller */
        vol_walk_init(&w, v, "abandoned");
        vol_walk_result(&w, -1, 4, 4);
        ok(vol_walk_abandon(&w) == -1,
           "6d. vol_walk_abandon is the named other exit, and still reports "
           "the shortfall");
        after = vol_walk_reap(v);
        ok(after == 0, "6e. and it discharges the latch too (reap=%zu)", after);

        /* 6f. ABANDONING RECORDS THE DECISION.
         *
         * The question this closes: "does the latch also cover a caller
         * that walks away without thinking it was a walk end?" Two parts,
         * and they have different answers.
         *
         * A caller that records a result and then RETURNS EARLY, calling
         * neither discharge, is covered already -- the latch is still set
         * and vol_close names it. That is leg 6b.
         *
         * The case that was NOT covered is the one abandon itself creates:
         * it clears the latch, by design, so "I looked at a partial
         * listing and proceeded anyway" used to leave no trace at all. The
         * watermark sweep pass is a live user of that path. It now writes
         * the decision down at the point it is made, because an absence
         * nobody can measure is not a safeguard. */
        vol_walk_init(&w, v, "the watermark pass");
        vol_walk_result(&w, -1, 7, 40);
        capture_stderr(abandon_short, &w, err_cap, sizeof err_cap);
        ok(strstr(err_cap, "the watermark pass") != NULL &&
           strstr(err_cap, "proceeding on the partial result") != NULL,
           "6f. vol_walk_abandon says, in words, which walk stopped and that "
           "its caller is proceeding anyway: %s",
           strstr(err_cap, "proceeding") ? "said" : "SAID NOTHING");
        if (!strstr(err_cap, "proceeding"))
            printf("        ^ abandon returned -1 and the volume forgot: the "
                   "decision is gone and nothing can detect it later\n");

        /* and an abandoned COMPLETE walk says nothing, because there is
         * nothing to say -- a diagnostic that fires on the boring path is a
         * diagnostic people learn to ignore. */
        vol_walk_init(&w, v, "healthy");
        vol_walk_result(&w, 0, 7, 7);
        capture_stderr(abandon_short, &w, err_cap, sizeof err_cap);
        ok(err_cap[0] == 0,
           "6g. and stays silent for a walk that was whole (%d bytes of "
           "diagnostic)", (int)strlen(err_cap));

        /* 6h. THE OTHER DIRECTION: a receipt that was never recorded.
         *
         * vol_walk_complete() is true on an untouched receipt, so a caller
         * whose control flow skipped the walk -- an early return ABOVE the
         * vol_walk_result() call, a branch that never reached it -- would
         * commit it and hear 0, "the walk was whole", about a walk that
         * never ran. Same mistake as ignoring a short one: a walk end
         * nobody thought was a walk end. Both discharge calls now reject
         * it. */
        vol_walk_init(&w, v, "never ran");
        capture_stderr(commit_unarmed, &w, err_cap, sizeof err_cap);
        ok(g_sink == -1,
           "6h. committing a receipt that never recorded a walk is an error, "
           "not a clean pass (rc=%d)", g_sink);
        if (g_sink == 0)
            printf("        ^ reported a walk that never ran as a walk that "
                   "ran and was whole\n");
        ok(strstr(err_cap, "without ever recording a walk result") != NULL,
           "6i. and it says why, naming the walk and the mistake: %s",
           strstr(err_cap, "without ever recording") ? "said" : "SAID NOTHING");
        if (!strstr(err_cap, "without ever recording"))
            printf("        ^ captured stderr was: [%s]\n", err_cap);
    }

    vol_flush(v);
    vol_close(v);

    /* =================================================================
     * SITES 2 and 4 — the two CLIs, as subprocesses. They are separate
     * mains, so a fresh process is also a fresh fault countdown; that is why
     * these two legs do not need the reload doors.
     * ================================================================= */

    /* SITE 2: invf-verify --deep on a volume whose walk stops.
     * WRONG ANSWER before the fix: "N files ok, 0 corrupt", exit 0.
     *
     * Identification is BY OUTPUT, never by a pinned ordinal. The deep pass
     * is not the first vol_v3_walk in the process -- vol_open's own scan,
     * and the nlink audit afterwards, are walks too -- and how many precede
     * it moves whenever any of them grows a walk. So every position in a
     * range is tried and the one that produces the DEEP PASS'S OWN message
     * is selected; requiring exactly one match is also what proves the
     * refusal is the deep pass's and not the nlink audit's, which walks
     * immediately afterwards and is the neighbour most likely to be
     * mistaken for it.
     *
     * And when NO position matches, the reason is printed. A bare "hits=0"
     * is indistinguishable from a regression, and on this project that
     * ambiguity has a known cause: bin/invf-verify is not relinked by
     * `make -j6`, so a core change leaves it stale and the refusal string
     * is not in it at all. That is checked first, before the search, and
     * reported as what it is.
     *
     * SEVERAL positions matching is correct and expected, and the reason is
     * worth writing down because it looks like a bug: v3_walk_dir is
     * RECURSIVE, and the seam is consulted at the top of every recursion,
     * so one top-level walk spends one countdown value PER DIRECTORY LEVEL
     * on its way down. The volume here is root + d1 + d2, so the deep
     * pass's single walk answers to three consecutive positions. Asserting
     * "exactly one" was wrong -- it is true only on a FLAT tree, which is
     * why the first version of this test passed on a two-file image and
     * failed on one with directories in it.
     *
     * The discriminator is the MESSAGE, not the count: the deep pass and
     * the nlink audit print different strings, so matching on the deep
     * pass's own is what proves the refusal is the deep pass's. */
    {
        int n, st, hits = 0, hit_n = 0, hit_st = 0;
        char hit[32768], bin[600], spec[64];
        const char *verify_bin;
        int fresh;

        hit[0] = 0;
        verify_bin = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(bin, sizeof bin, "%s/bin/invf-verify", verify_bin);
        fresh = binary_has(bin, "could not walk the whole namespace");
        ok(fresh == 1,
           "2-0. the invf-verify under test CARRIES the deep pass's refusal "
           "string");
        if (fresh != 1)
            printf("        ^ the site-2 legs below CANNOT pass against it, "
                   "and their failure would say nothing about the code. %s\n",
                   diagnose(bin, "src/cli/verify.c", fresh));

        for (n = 1; n <= 8; n++) {
            snprintf(spec, sizeof spec, "v3_walk_dir_stop:%d", n);
            snprintf(cmd, sizeof cmd, "INVFS_FAULT=%s %s/bin/invf-verify --deep %s",
                     spec, verify_bin, img);
            st = run_capture(cmd, out, sizeof out);
            if (strstr(out, "could not walk the whole namespace") != NULL) {
                if (hits == 0) {
                    snprintf(hit, sizeof hit, "%s", out);
                    hit_n = n;
                    hit_st = st;   /* the status OF THE HIT, not of the last */
                }
                hits++;
            } else {
                /* Say where every OTHER position went, so a future shift is
                 * diagnosable from one run instead of by bisecting. */
                printf("        (position %d: %s)\n", n,
                       strstr(out, "nlink/fan-in audit could not") ? "the nlink audit's walk"
                                                                    : "no walk failure reached a report");
            }
        }
        ok(hits >= 1,
           "2a. at least one walk position in 1..8 is the deep pass's own "
           "(hits=%d, first at %d; more than one is correct -- the walk is "
           "recursive, so it spends one countdown value per directory level)",
           hits, hit_n);
        if (hits == 0)
            printf("        ^ the binary is current (2-0 passed) but no walk "
                   "position stopped the deep pass. The walk it is on has "
                   "moved outside 1..8; the per-position trace above says "
                   "where the others went.\n");
        if (hits > 1)
            printf("        (positions %d..%d all reach the deep pass: one "
                   "walk spends one countdown value per directory level, and "
                   "this tree has %d)\n", hit_n, hit_n + hits - 1, hits);
        ok(hit_st != 0,
           "2b. invf-verify --deep exits NON-ZERO when the walk stopped "
           "(status=%d)", hit_st);
        if (hit_st == 0)
            printf("        ^ exit 0 over a namespace it could not walk\n");
        /* The SUMMARY line, not the bare substring: the refusal message
         * itself contains the phrase "0 corrupt" (in quotes, telling the
         * reader not to believe it), and matching that would have made this
         * leg pass or fail for a reason that has nothing to do with the
         * defect. The claim is about what the operator is TOLD at the end. */
        ok(strstr(hit, "LOWER BOUND") != NULL,
           "2c. it SAYS the walk did not complete and names the count it "
           "printed as a lower bound");
        ok(strstr(hit, " files ok, 0 corrupt") == NULL,
           "2d. and its summary line never reports a clean run over a "
           "namespace it could not walk");
        ok(strstr(hit, "deep: 0 files ok, 1 corrupt") != NULL,
           "2e. it reports the stopped walk AS the corruption, which is what "
           "makes the exit code non-zero");
    }
    /* green control: same command, no fault — the volume is clean. */
    {
        int st;
        snprintf(cmd, sizeof cmd, "%s/bin/invf-verify --deep %s",
                 getenv("PWD") ? getenv("PWD") : ".", img);
        st = run_capture(cmd, out, sizeof out);
        ok(st == 0, "2e. CONTROL: the same command on a healthy volume exits 0 "
                    "(status=%d)", st);
        ok(strstr(out, "0 corrupt") != NULL,
           "2g. and reports 0 corrupt — so 2d is measuring the walk, not a "
           "permanently-broken oracle");
    }

    /* SITE 4: the mutating sweep must ABORT, not warn and continue.
     * WRONG ANSWER before the fix: "warning: v3 directory walk did not
     * complete" on stderr, then stages 3-7 ran anyway and rewrote whatever
     * the partial collect had named. */
    {
        int n, st, e2 = 0, hits = 0, hit_n = 0, hit_st = 0;
        int refusals_all = 0, refusals_none = 0;
        size_t blen = 0;
        uint8_t *buf = NULL;
        invfs_volume *v2;
        int target;
        char hit[32768];
        hit[0] = 0;

        /* What the volume looks like going in, by the only oracle this
         * project trusts for bit-exactness: read the bytes back. */
        v2 = vol_open(img, &e2);
        if (!v2) { fprintf(stderr, "reopen failed: %d\n", e2); return 2; }
        target = ino_of(v2, "d1/f0");
        ok(vol_read_file(v2, target, &buf, &blen) == 0 && blen == 1 &&
           buf && buf[0] == 'x',
           "4a. fixture: d1/f0 reads back bit-exactly before the sweep");
        free(buf);
        vol_close(v2);

        /* Same problems as leg 2 and the same answers: invf-sweep's prepare
         * stage captures a savepoint (a walk), so the collect is not the
         * first one; the walk is recursive, so several positions can match;
         * and bin/invf-sweep is not relinked by `make -j6`, so a stale one
         * would make the search return nothing and say nothing useful. */
        {
            char sbin[600];
            const char *sweep_bin = getenv("PWD") ? getenv("PWD") : ".";
            int sfresh;
            snprintf(sbin, sizeof sbin, "%s/bin/invf-sweep", sweep_bin);
            /* The needle is a LITERAL out of the refusal message, not a
             * phrase from it: the message is formatted ("Refusing to %s",
             * with %s = "sweep" or "plan"), so "Refusing to sweep" exists
             * only at RUN time and searching the binary for it reports a
             * current build as stale. The first version of this guard did
             * exactly that, which is a nice illustration of why the guard
             * needs testing rather than eyeballing. */
            sfresh = binary_has(sbin, "a partial live set is not a smaller sweep");
            ok(sfresh == 1,
               "4-0. the invf-sweep under test CARRIES the refusal");
            if (sfresh != 1)
                printf("        ^ the site-4 legs below CANNOT pass against "
                       "it. %s\n", diagnose(sbin, "tools/invf-sweep.c", sfresh));
        }
        for (n = 1; n <= 8; n++) {
            snprintf(cmd, sizeof cmd,
                     "INVFS_FAULT=v3_walk_dir_stop:%d %s/bin/invf-sweep %s",
                     n, getenv("PWD") ? getenv("PWD") : ".", img);
            st = run_capture(cmd, out, sizeof out);
            if (strstr(out, "Refusing to sweep") != NULL ||
                strstr(out, "Refusing to plan") != NULL) {
                if (strstr(out, "It stopped partway through") != NULL)
                    refusals_all++;
                else if (strstr(out, "before delivering ANY entry") != NULL)
                    refusals_none++;
                if (hits == 0) {
                    snprintf(hit, sizeof hit, "%s", out);
                    hit_n = n;
                    hit_st = st;   /* the status OF THE HIT, not of the last */
                }
                hits++;
            }
        }
        ok(hits >= 1,
           "4b. at least one walk position in 1..8 is the collect's own "
           "(hits=%d, first at %d; more than one is correct -- the walk is "
           "recursive and spends one countdown value per directory level)",
           hits, hit_n);
        ok(hit_st != 0,
           "4c. invf-sweep exits NON-ZERO when the collect walk stopped "
           "(status=%d)", hit_st);
        if (hit_st == 0)
            printf("        ^ exit 0: it warned and swept a volume it had just "
                   "admitted it could not read\n");
        ok(strstr(hit, "did not complete") != NULL,
           "4d. and it SAYS the walk did not complete, in words");
        ok(strstr(hit, "warning: v3 directory walk did not complete") == NULL,
           "4e. the old warn-and-continue path is gone");
        /* "nothing was rewritten": the stages that rewrite. The free-block
         * count is deliberately NOT the assertion here -- prepare's
         * savepoint capture legitimately allocates, and it runs before the
         * collect, so a block-count comparison would be measuring the
         * rollback window rather than the sweep. */
        ok(strstr(hit, "[3/7] transform") == NULL,
           "4f. the transform stage -- the one that re-encodes -- never runs");
        ok(strstr(hit, "[5/7] dedupe") == NULL &&
           strstr(hit, "[6/7] batches") == NULL,
           "4g. nor dedupe or the batch GC, which is where space is freed");
        ok(strstr(hit, "[7/7] finalize") == NULL,
           "4h. and it stops before the durability point, so the pass leaves "
           "no half-swept generation behind");

        /* 4i. THE TWO CASES ARE NOT THE SAME VOLUME STATE.
         *
         * A walk that stopped having already delivered entries produced a
         * PARTIAL live set. A walk that stopped before delivering anything
         * produced NO live set at all -- and the stages below would then
         * run over an empty list and report a clean sweep of a volume they
         * never read. That is strictly worse than a subset, and it reads in
         * the summary as "0 files", i.e. as an EMPTY VOLUME.
         *
         * Asserted across the whole search rather than at a pinned
         * position, because which case a position lands in depends on the
         * tree's shape -- this fixture has directories, so a shallow
         * position stops before the first entry is delivered and a deeper
         * one stops partway through. What has to hold is that the two are
         * distinguished AT ALL and that both are reachable here: an arm
         * that names only one of them is the silent one. */
        ok(refusals_all > 0 && refusals_none > 0,
           "4i. and the refusal tells a PARTIAL collect from NO live set at "
           "all: across the positions tried here, %d said partial and %d "
           "said nothing was delivered", refusals_all, refusals_none);
        if (!refusals_all || !refusals_none)
            printf("        ^ one of the two wordings never appeared, so one "
                   "of the two cases is unlabelled: an operator cannot tell "
                   "a subset from a volume that was never read\n");

        /* and the bytes are untouched, which is the point of all of it */
        v2 = vol_open(img, &e2);
        if (!v2) { fprintf(stderr, "reopen failed: %d\n", e2); return 2; }
        buf = NULL; blen = 0;
        ok(vol_read_file(v2, target, &buf, &blen) == 0 && blen == 1 &&
           buf && buf[0] == 'x',
           "4i. d1/f0 is STILL bit-exactly 'x' after the refused sweep");
        free(buf);
        vol_close(v2);
    }

    /* =================================================================
     * THE CASE THE WRAPPER MUST NOT BREAK: an EMPTY tree.
     *
     * A volume with no live inodes and a namespace with no entries is a
     * COMPLETE walk of length ZERO. That is the right answer, and it is a
     * common one — a fresh image, a volume whose files are all gone, a
     * directory the user just made. A wrapper that treated "nothing came
     * back" as a failure would have traded a silent wrong answer for a loud
     * false alarm, which is a different bug and a worse one at 3am with a
     * fresh volume in front of you.
     *
     * So: vol_walk_n() is 0 here, not VOL_WALK_NO_COUNT; vol_walk_commit()
     * is 0 here; the collect returns 0 here; and nothing is latched.
     * ================================================================= */
    {
        uint64_t *ids = NULL;
        size_t cap = 0, n = 1, found = 1;
        vol_walk_t w;
        invfs_volume *ve;
        int e3 = 0, esz = 0;

        snprintf(empty, sizeof empty, "%s/invf-walk-status-empty.img", dir);
        mkvol(empty, 8);
        ve = vol_open(empty, &e3);
        if (!ve) { fprintf(stderr, "empty vol_open failed: %d\n", e3); return 2; }

        rc = vol_v3_walk(ve, walk_count_cb, &esz);
        ok(rc == 0 && esz == 0,
           "7a. a walk of an empty namespace COMPLETES with 0 entries "
           "(rc=%d n=%d)", rc, esz);

        vol_walk_init(&w, ve, "empty");
        vol_walk_result(&w, rc, (size_t)esz, (size_t)esz);
        ok(vol_walk_complete(&w) == 1,
           "7b. the receipt calls an empty walk COMPLETE, not short");
        ok(vol_walk_n(&w) == 0,
           "7c. and vol_walk_n() is 0 — the sentinel is (size_t)-1, never 0 "
           "(got %zu)", vol_walk_n(&w));
        ok(vol_walk_n(&w) != VOL_WALK_NO_COUNT,
           "7d. i.e. 0 is a COUNT here, not \"no count\"");
        ok(vol_walk_commit(&w) == 0,
           "7e. vol_walk_commit() accepts it: nothing to complain about");

        rc = vol_collect_sweepables_grow(ve, &ids, &cap, &n, &found);
        ok(rc == 0 && n == 0 && found == 0,
           "7f. the sweep collect on an empty volume SUCCEEDS with an empty "
           "list (rc=%d n=%zu found=%zu)", rc, n, found);
        free(ids);

        ok(vol_walk_reap(ve) == 0,
           "7g. and nothing is latched: no false alarm to report at close");
        vol_close(ve);
    }

    /* the same shape from the other side: a walk merely TRUNCATED by the
     * caller's cap IS short, and must not be mistaken for an empty one. */
    {
        vol_walk_t w;
        vol_walk_init(&w, v, "truncated");
        vol_walk_result(&w, 0, 2, 9);      /* 9 seen, 2 stored: the cap bit */
        ok(vol_walk_complete(&w) == 0,
           "8a. found > n is short: a truncated walk is not a complete one");
        ok(vol_walk_n(&w) == VOL_WALK_NO_COUNT,
           "8b. and vol_walk_n() withholds the truncated count");
        ok(vol_walk_seen(&w) == 9,
           "8c. while vol_walk_seen() still gives the honest 9");
        ok(vol_walk_commit(&w) == -1, "8d. and commit reports it");
    }

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
