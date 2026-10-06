/* sib_walk_test.c — WP-delete-siblings-short-walk-leaves-orphans:
 * vol_delete_siblings() must not act on a walk that STOPPED, and must not
 * act on one that merely PRETENDED to finish.
 *
 * THE DEFECT (src/core/vol_records.c:194, inside vol_delete_siblings):
 *
 *     vol_walk(v, del_siblings_cb, &c);     // status discarded
 *
 * The callback collects the `name!partN` siblings of a container and the
 * caller unlinks exactly `c.n` of them. A walk that stops reports
 * found == n, because it saw exactly what it stored -- so with the status
 * on the floor there is no way to tell a whole sibling set from a PREFIX of
 * one. What the prefix purge leaves behind is a permanent data leak: the
 * sweep skips internal '!' names and invf-fsck counts them as live files,
 * so nothing will ever collect the survivors.
 *
 * WHY "NOTHING" AND NOT "THE PREFIX" -- and the test measures both halves
 * ================================================================
 *
 * Leg B (STOP) is the assigned defect: arm `walk_dir_stop`, the seam at
 * the top of walk_dir, and the walk returns -1 having delivered a prefix
 * of the namespace. The prefix purge frees the containers it reached and
 * leaves the rest live forever.
 *
 * Leg C (SKIP) is the arm that makes "unlink half a container's parts is
 * worse than leaving them all" NON-HYPOTHETICAL. `walk_dir_row` makes
 * one entry's inode row fail to read, and the LENIENT walk `continue`s past
 * it -- the walk returns 0, "complete", having silently skipped exactly one
 * sibling. A prefix purge then frees 3 of a 4-part container and leaves the
 * fourth live: a `name` whose `!part1..3` are gone and whose `!part0` (and
 * possibly the recipe that says how many there were) is not. That state is
 * not recoverable by anything. So the fix both (a) reads the walk's status
 * through the existing vol_walk_t receipt and refuses on a short walk, and
 * (b) asks for the STRICT walk, because (a) alone cannot see a walk that
 * reports complete while having skipped an entry.
 *
 * ORACLE: the NAMES. Not a count. A count cannot tell "b.tar!part0..3 were
 * freed and z.tar!part0..3 were not" from "one of each was", which are
 * different volumes. Every leg below lists, by name, what is still on the
 * volume afterwards.
 *
 * THE CONTROL ARM IS NOT OPTIONAL. Without it, "refuse everything" passes:
 * a function that never unlinks anything satisfies every refusal assertion
 * here. So the first measured leg runs the SAME corpus and the SAME calls
 * with nothing armed and asserts that all eight siblings are gone.
 *
 * ARMING (src/core/vol_fault.h). Both seams live in src/core/vol_dirs.c, so
 * they are re-armed with invfs_vol_dirs_fault_reload(). unsetenv+setenv is
 * NOT a substitute: unsetenv frees the string, setenv usually gets the same
 * address back, the spec is compared BY POINTER, the countdown stays spent,
 * and the leg then measures the healthy path and goes green proving nothing
 * (src/core/vol_fault.h:68-87).
 *
 * THE SEAM'S ORDINAL IS NOT A CONSTANT. walk_dir is RECURSIVE and
 * consults `walk_dir_stop` once per directory LEVEL, so the ordinal is a
 * level, not an entry; `walk_dir_row` is consulted once per ENTRY of the
 * level being walked. Which entry sits at position 1 depends on the listing
 * order, and the listing order is a qsort of whatever the dirent tree yields
 * (src/core/vol_dirs.c:266). So this test SEARCHES every position, prints a
 * per-position trace of exactly which siblings each arm reaches, and only
 * uses a position that is DISCRIMINATING -- it reached at least one sibling
 * and not all of them. A hard-coded ordinal would silently degrade into "the
 * arm never fired" the moment the fixture changed, and the leg would go
 * green measuring the healthy path.
 *
 * STALE BINARY. `make -j4` does not relink the CLI/test binaries -- they are
 * prerequisites of `make test`, not of `all`. So the LAST check here compares
 * /proc/self/exe's mtime against the mtimes of the sources this test's
 * behaviour depends on, and exits 3 if the binary is older than any of them.
 * (Deliberately an mtime comparison and NOT a grep for a literal out of the
 * fix: the binary being read is this binary, so a literal written here would
 * be present whether or not the fix was in the build, and the guard would
 * pass always -- which is the exact failure mode a staleness guard exists
 * to catch.)
 *
 * exit 0 = pass, 1 = failure, 2 = setup error, 3 = stale binary.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "volume_internal.h"
#include "vol_fault.h"
#include "vol_walk.h"

extern void invfs_vol_dirs_fault_reload(void);

/* ---- the corpus ----------------------------------------------------- */

/* Two decomposed TARs in the volume ROOT, with a directory between them so
 * the walk has somewhere to stop:
 *
 *     b.tar  b.tar!part0 .. b.tar!part3
 *     m_dir/                       <-- the walk stops on entering this
 *     z.tar  z.tar!part0 .. z.tar!part3
 *
 * strcmp order at the root is exactly that ("b.tar" < "b.tar!part0" <
 * "m_dir" < "z.tar"), and walk_dir recurses into a directory at the point
 * it reaches it in the listing (src/core/vol_dirs.c:918), so a stop on
 * entering m_dir has delivered every b.tar sibling and no z.tar one. That is
 * the partial view this finding is about. */
#define NC      2
static const char *cname[NC] = { "b.tar", "z.tar" };
#define NPARTS  4
#define BRACKET "m_dir"

static char sib[NC][NPARTS][64];
static int  sib_live[NC][NPARTS];

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

static void info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("       ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
}

/* ---- a small, compressible ustar (4 members -> 4 !partN siblings) --- */

static size_t build_tar(uint8_t **out)
{
    const char *names[4] = { "m0", "m1", "m2", "m3" };
    size_t pos = 0, cap = 8192;
    uint8_t *buf = malloc(cap);
    int i;

    for (i = 0; i < 4; i++) {
        uint8_t hdr[512], payload[400];
        size_t plen = sizeof payload, j;
        unsigned sum = 0;

        memset(hdr, 0, sizeof hdr);
        memset(payload, 'a' + i, plen);
        memcpy(hdr, names[i], strlen(names[i]));
        memcpy(hdr + 100, "0000644\0", 8);
        memcpy(hdr + 108, "0000000\0", 8);
        memcpy(hdr + 116, "0000000\0", 8);
        memcpy(hdr + 124, "0001000\0", 8);
        memcpy(hdr + 136, "00000000000\0", 12);
        memcpy(hdr + 148, "        ", 8);
        hdr[156] = '0';
        memcpy(hdr + 257, "ustar", 5);
        memcpy(hdr + 263, "00", 2);
        for (j = 0; j < sizeof hdr; j++) sum += hdr[j];
        snprintf((char *)hdr + 148, 8, "%06o", sum);
        hdr[154] = '\0';

        for (j = 0; pos + 512 + plen + 512 > cap; j++) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + pos, hdr, 512);
        memcpy(buf + pos + 512, payload, plen);
        pos += 512 + plen;
        while (pos % 512) buf[pos++] = 0;
    }
    memset(buf + pos, 0, 512 * 2);
    pos += 512 * 2;
    *out = buf;
    return pos;
}

/* ---- fixture ------------------------------------------------------- */

static invfs_volume *g_v;
static uint8_t *g_tar;
static size_t   g_tar_len;

static void rescan(void)
{
    int c, p;
    for (c = 0; c < NC; c++)
        for (p = 0; p < NPARTS; p++)
            sib_live[c][p] = vol_find(g_v, sib[c][p]) != 0;
}

static int build_fixture(void)
{
    int c, p, n = 0;
    for (c = 0; c < NC; c++) {
        for (p = 0; p < NPARTS; p++) {
            snprintf(sib[c][p], sizeof sib[c][p], "%s!part%d", cname[c], p);
            (void)vol_unlink(g_v, sib[c][p]);   /* start from a clean slate */
        }
        (void)vol_unlink(g_v, cname[c]);
        if (vol_replace_file(g_v, cname[c], g_tar, g_tar_len) == 0) return -1;
        if (vol_create_tar_file(g_v, cname[c], g_tar, g_tar_len) == 0) return -1;
    }
    if (!vol_find(g_v, BRACKET) && vol_mkdir(g_v, BRACKET) == 0) return -1;
    rescan();
    for (c = 0; c < NC; c++)
        for (p = 0; p < NPARTS; p++)
            n += sib_live[c][p];
    return n;
}

/* The survivor list, BY NAME. This is the oracle the whole file turns on.
 * The FREED list is printed beside it on purpose: on a short walk the names
 * that vanish are not the caller's -- they belong to whatever else the walk
 * happened to reach first -- and an assertion that only counts survivors
 * cannot see that. */
static void list(const char *tag, int want_live)
{
    int c, p, n = 0;
    char line[1024];
    size_t used = 0;

    line[0] = 0;
    for (c = 0; c < NC; c++)
        for (p = 0; p < NPARTS; p++)
            if (!!sib_live[c][p] == want_live) {
                used += (size_t)snprintf(line + used, sizeof line - used,
                                         "%s%s", n++ ? " " : "", sib[c][p]);
                if (used >= sizeof line - 1) break;
            }
    if (!n) info("%s: %s -- none", tag, want_live ? "still ON THE VOLUME"
                                                   : "freed (gone by name)");
    else      info("%s: %s %d name(s): %s",
                   tag, want_live ? "still ON THE VOLUME" : "FREED", n, line);
}

/* ---- arming -------------------------------------------------------- */

static void arm(const char *site, int pos)
{
    char spec[64];
    snprintf(spec, sizeof spec, "%s:%d", site, pos);
    setenv("INVFS_FAULT", spec, 1);
    invfs_vol_dirs_fault_reload();   /* the ONLY way to re-arm these sites */
}

static void disarm(void)
{
    unsetenv("INVFS_FAULT");
    invfs_vol_dirs_fault_reload();
}

/* ---- the probe walk (read-only) ------------------------------------ */

/* Records which fixture siblings the walk delivered. Identical before and
 * after the fix -- it calls vol_walk directly and never mutates -- so the
 * POSITION SEARCH cannot itself depend on the fix. That matters: the fix
 * makes the purge write NOTHING on a short walk, so a search that
 * discriminated on "some siblings went and some did not" would find no
 * discriminating position at all on fixed code and would conclude the arm
 * had not fired. */
#define MAXSEEN 64
static char seen[MAXSEEN][64];
static int  nseen;

static int probe_cb(void *ctx_, const char *path, uint64_t ino,
                    uint32_t type, uint64_t size, int64_t mtime)
{
    int c, p;
    (void)ctx_; (void)ino; (void)type; (void)size; (void)mtime;
    for (c = 0; c < NC; c++)
        for (p = 0; p < NPARTS; p++)
            if (strcmp(path, sib[c][p]) == 0 && nseen < MAXSEEN)
                snprintf(seen[nseen++], sizeof seen[0], "%s", path);
    return 0;
}

static int probe_c[NC], probe_p[NPARTS];

static int probe_at(const char *site, int pos, int *total)
{
    int rc, i, n = 0;
    for (i = 0; i < NC; i++) probe_c[i] = 0;
    for (i = 0; i < NPARTS; i++) probe_p[i] = 0;
    arm(site, pos);
    nseen = 0;
    rc = vol_walk(g_v, probe_cb, NULL);
    disarm();
    for (i = 0; i < nseen; i++) {
        int c, p;
        for (c = 0; c < NC; c++)
            for (p = 0; p < NPARTS; p++)
                if (strcmp(seen[i], sib[c][p]) == 0) { probe_c[c]++; probe_p[p]++; }
    }
    for (i = 0; i < NC; i++) n += probe_c[i];
    *total = n;
    return rc;
}

/* ---- capture the core's own voice ---------------------------------- */

static char err_buf[16384];
static char g_scratch[512];

static void run_capture(void (*fn)(void))
{
    char tmp[640];
    int fd, s1, s2;
    ssize_t got;

    snprintf(tmp, sizeof tmp, "%s/sib_walk_test.err", g_scratch);
    fflush(stdout); fflush(stderr);
    fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { fn(); return; }
    s1 = dup(1); s2 = dup(2);
    dup2(fd, 1); dup2(fd, 2);
    fn();
    fflush(stdout); fflush(stderr);
    dup2(s1, 1); dup2(s2, 2);
    close(s1); close(s2);
    lseek(fd, 0, SEEK_SET);
    got = read(fd, err_buf, sizeof err_buf - 1);
    close(fd);
    if (got < 0) got = 0;
    err_buf[got] = 0;
}

/* ---- stale-binary guard (mtimes, NOT a grep for the fix) ----------- */

static int mtime_of(const char *path, time_t *out)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *out = st.st_mtime;
    return 0;
}

static int stale_binary(const char *self)
{
    static const char *deps[] = {
        "src/core/vol_records.c", "src/core/vol_dirs.c", "src/core/vol_walk.h",
        "src/core/vol_fault.h", "src/core/volume_internal.h", "src/core/volume.h",
        "src/core/vol_cpack.c",
    };
    const char *root = getenv("PWD");
    time_t me, d;
    char p[1024];
    size_t i;

    if (!root || !*root) root = ".";
    if (mtime_of(self, &me) != 0) {
        fprintf(stderr, "\nsib_walk_test: STALE BINARY -- cannot stat %s\n", self);
        return 1;
    }
    for (i = 0; i < sizeof deps / sizeof deps[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", root, deps[i]);
        if (mtime_of(p, &d) != 0) continue;   /* not found: nothing to say */
        if (d > me) {
            fprintf(stderr,
                "\nsib_walk_test: STALE BINARY -- %s is older than %s.\n"
                "    This binary was built before the sources it measures. The\n"
                "    test binaries are prerequisites of `make test`, not of\n"
                "    `all`, so `make -j4` does not relink them:\n"
                "        rm -f bin/invf-sib_walk_test && make bin/invf-sib_walk_test\n",
                self, p);
            return 1;
        }
    }
    return 0;
}

/* ---- the legs ------------------------------------------------------ */

static void purge_both(void)
{
    (void)vol_delete_siblings(g_v, cname[0]);
    (void)vol_delete_siblings(g_v, cname[1]);
}
static void purge_first(void)  { (void)vol_delete_siblings(g_v, cname[0]); }
static void purge_second(void) { (void)vol_delete_siblings(g_v, cname[1]); }

#define NPOS 12

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    int err = 0, i, n;
    int stop_pos = -1, ndisc_stop = 0, saw_complete_stop = 0, reach_stop = 0;
    int row_pos  = -1, ndisc_row  = 0, saw_complete_row  = 0, reach_row = 0;
    int ctrl_msg = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("sib_walk_test: vol_delete_siblings must not act on a walk that "
           "stopped, nor on one that skipped an entry and called itself "
           "complete\n");

    snprintf(img, sizeof img, "%s/invf-sib-walk-test.img", dir);
    snprintf(g_scratch, sizeof g_scratch, "%s", dir);
    unlink(img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 48 >/dev/null 2>&1",
                 root, img);
        if (system(cmd) != 0) {
            fprintf(stderr, "sib_walk_test: cannot create volume with invf-mkfs\n");
            return 2;
        }
    }
    g_v = vol_open(img, &err);
    if (!g_v) {
        fprintf(stderr, "sib_walk_test: vol_open failed: err=%d\n", err);
        return 2;
    }
    g_tar_len = build_tar(&g_tar);

    n = build_fixture();
    ok(n == NC * NPARTS,
       "fixture: %d decomposed containers at the volume root, %d '!partN' "
       "siblings live (%d expected)", NC, n, NC * NPARTS);

    /* ---- POSITION SEARCH: the STOP seam (one per directory LEVEL) ---- */
    info("position search, walk_dir_stop (consulted once per directory "
         "LEVEL -- it is not an entry ordinal):");
    for (i = 1; i <= NPOS; i++) {
        int total;
        int rc = probe_at("walk_dir_stop", i, &total);
        info("  pos %2d: walk rc=%d, reached %d/%d sibling(s)  [b.tar %d/%d, "
             "z.tar %d/%d]%s", i, rc, total, NC * NPARTS,
             probe_c[0], NPARTS, probe_c[1], NPARTS,
             rc == 0 ? "  (walk COMPLETE -- arm past the last level)"
                     : "  (walk STOPPED)");
        if (rc == 0) saw_complete_stop = 1;
        if (rc != 0 && total > 0 && total < NC * NPARTS) {
            if (stop_pos < 0) { stop_pos = i; reach_stop = total; }
            ndisc_stop++;
        }
    }
    ok(saw_complete_stop,
       "position search (stop): some position ran past the last directory "
       "level and the walk reported COMPLETE, so the countdown really is "
       "per-level and the trace above is complete");
    ok(stop_pos > 0,
       "position search (stop): %d discriminating position(s); the walk "
       "STOPPED having reached >=1 and <%d of the siblings -- that is the "
       "partial view under test", ndisc_stop, NC * NPARTS);

    /* ---- POSITION SEARCH: the ROW seam (once per ENTRY of a level) ---- */
    info("position search, walk_dir_row (consulted once per ENTRY; the "
         "LENIENT walk steps over a row it cannot read and still says "
         "complete):");
    for (i = 1; i <= NPOS; i++) {
        int total;
        int rc = probe_at("walk_dir_row", i, &total);
        info("  pos %2d: walk rc=%d, reached %d/%d sibling(s)  [b.tar %d/%d, "
             "z.tar %d/%d]%s", i, rc, total, NC * NPARTS,
             probe_c[0], NPARTS, probe_c[1], NPARTS,
             rc == 0 ? "  (walk says COMPLETE)"
                     : "  (walk STOPPED)");
        if (rc == 0) saw_complete_row = 1;
        /* DISCRIMINATING for this seam = the walk CLAIMS to be complete and
         * still missed at least one sibling. That combination is the whole
         * point: it is the shape no receipt on the return value can see. */
        if (rc == 0 && total > 0 && total < NC * NPARTS) {
            if (row_pos < 0) { row_pos = i; reach_row = total; }
            ndisc_row++;
        }
    }
    ok(saw_complete_row,
       "position search (row): the lenient walk reported COMPLETE at some "
       "position -- a skipped entry does not stop it");
    ok(row_pos > 0,
       "position search (row): %d discriminating position(s); the walk said "
       "'complete' having silently skipped >=1 sibling", ndisc_row);

    /* ---- CONTROL ARM: the same corpus, nothing armed ------------------ */
    if (build_fixture() != NC * NPARTS) {
        fprintf(stderr, "sib_walk_test: fixture rebuild failed\n");
        return 2;
    }
    run_capture(purge_both);
    rescan();
    list("control", 1); list("control", 0);
    {
        int alive = 0;
        for (i = 0; i < NC; i++) { int p; for (p = 0; p < NPARTS; p++) alive += sib_live[i][p]; }
        ok(alive == 0,
           "CONTROL (nothing armed): all %d siblings are GONE -- a purge that "
           "refused everything would fail this, which is why the arm is "
           "mandatory", NC * NPARTS);
    }
    ctrl_msg = (strstr(err_buf, "REFUSED") != NULL);
    ok(!ctrl_msg,
       "CONTROL (nothing armed): the purge reported nothing to refuse about");

    /* ---- LEG B: the walk STOPPED, and the caller purges z.tar ---------- */
    /* z.tar's siblings are the ones the stop kept the walk FROM reaching, so
     * this is the caller's own data. The prefix the walk DID reach holds
     * b.tar's names, which the callback's own prefix test rejects -- so the
     * old code unlinks nothing, returns 0, and says nothing. What is left
     * on the volume is exactly the permanent leak: '!' names the sweep skips
     * and invf-fsck counts as live, with no pass that will ever retry. The
     * survivor set is therefore the SAME before and after the fix; what the
     * fix changes is that it is REPORTED instead of silent. */
    if (stop_pos > 0 && build_fixture() == NC * NPARTS) {
        int p, z_alive = 0, b_alive = 0;
        arm("walk_dir_stop", stop_pos);
        run_capture(purge_second);
        disarm();
        rescan();
        info("stopped walk at position %d reached %d of %d siblings; the "
             "caller asked for z.tar", stop_pos, reach_stop, NC * NPARTS);
        list("stopped walk, purging z.tar", 1);
        list("stopped walk, purging z.tar", 0);
        for (p = 0; p < NPARTS; p++) { z_alive += sib_live[1][p]; b_alive += sib_live[0][p]; }
        ok(z_alive == NPARTS,
           "STOPPED walk, purging z.tar: all %d of ITS OWN siblings are still "
           "on the volume by name (%d survive). This is the leak -- and it is "
           "unchanged by the fix, which is why the fix's job here is to make "
           "it VISIBLE rather than silent",
           NPARTS, z_alive);
        ok(b_alive == NPARTS,
           "STOPPED walk, purging z.tar: b.tar's %d siblings are untouched. "
           "The callback's own 'name!' prefix test rejects them, so a short "
           "walk cannot free ANOTHER container's payload -- the damage is "
           "confined to leaving data behind, never to destroying live data",
           b_alive);
        ok(strstr(err_buf, "REFUSED") != NULL,
           "STOPPED walk, purging z.tar: the purge said so on stderr. The old "
           "code returned 0 here, which the callers read as \"nothing to "
           "purge\" -- indistinguishable from a clean volume");
        ok(strstr(err_buf, "NOT reclaimed by this pass") != NULL &&
           strstr(err_buf, "stored TWICE") != NULL,
           "STOPPED walk, purging z.tar: the refusal tells the operator the "
           "space is NOT reclaimed by this pass and that the container is "
           "stored TWICE until one -- the cost of refusing rather than "
           "purging the prefix, stated in the message rather than discovered "
           "from a full volume");
    }

    /* ---- LEG C: the same walk STOPPED, and the caller purges b.tar ------ */
    /* b.tar's siblings ARE inside the prefix, so here the old code unlinks
     * all four and returns 4 -- and cannot tell that it did so on a walk
     * that never finished. This is the leg the decision turns on: the fix
     * refuses, so the answer does not depend on WHERE the walk happened to
     * stop, and a caller can never be handed a partial sibling set. */
    if (stop_pos > 0 && build_fixture() == NC * NPARTS) {
        int alive = 0, c, p;
        arm("walk_dir_stop", stop_pos);
        run_capture(purge_first);
        disarm();
        rescan();
        list("stopped walk, purging b.tar", 1);
        list("stopped walk, purging b.tar", 0);
        for (c = 0; c < NC; c++) for (p = 0; p < NPARTS; p++) alive += sib_live[c][p];
        ok(alive == NC * NPARTS,
           "STOPPED walk, purging b.tar: the prefix DID cover all %d of "
           "b.tar's siblings and the old code freed every one of them (%d/%d "
           "survive). A purge that refuses frees NOTHING -- the same answer "
           "for a container inside the prefix as for one outside it, which is "
           "the only way a caller can rely on it",
           NPARTS, alive, NC * NPARTS);
        for (c = 0; c < NC; c++) for (p = 0; p < NPARTS; p++)
            ok(sib_live[c][p], "STOPPED walk (b.tar): %s is still on the volume",
               sib[c][p]);
    }

    /* ---- LEG D: the walk SKIPPED an entry and said "complete" ----------- */
    /* The LENIENT walk `continue`s past an entry whose row it cannot read
     * and still returns 0 -- so a receipt on the return value sees a whole
     * walk. This is the case that makes "freeing half a container's parts"
     * concrete rather than theoretical: the row seam at row_pos makes ONE
     * b.tar sibling unreadable, and the old code frees the other three and
     * leaves that one -- a container three-quarters destroyed, whose recipe
     * (which says how many parts there were) may or may not be among the
     * survivors. Nothing recovers that. */
    if (row_pos > 0 && build_fixture() == NC * NPARTS) {
        int alive = 0, c, p, b_alive = 0;
        arm("walk_dir_row", row_pos);
        run_capture(purge_first);
        disarm();
        rescan();
        info("skipping walk at position %d reported rc=0 having reached %d of "
             "%d siblings", row_pos, reach_row, NC * NPARTS);
        list("skipping walk, purging b.tar", 1);
        list("skipping walk, purging b.tar", 0);
        for (c = 0; c < NC; c++) for (p = 0; p < NPARTS; p++) alive += sib_live[c][p];
        for (p = 0; p < NPARTS; p++) b_alive += sib_live[0][p];
        ok(alive == NC * NPARTS,
           "SKIPPING walk: it reported COMPLETE having reached %d of %d "
           "siblings, and all %d must survive (%d do)",
           reach_row, NC * NPARTS, NC * NPARTS, alive);
        ok(b_alive == NPARTS,
           "SKIPPING walk: b.tar kept ALL %d of its parts (kept %d). Freeing "
           "3 of a 4-part container's parts leaves a name nothing can "
           "reconstruct, and the walk returned 0 -- so this is the case the "
           "receipt ALONE cannot see, and the reason the purge asks for the "
           "STRICT walk as well", NPARTS, b_alive);
        ok(strstr(err_buf, "REFUSED") != NULL,
           "SKIPPING walk: the purge reported the entry it could not read "
           "rather than acting on the rest of the set");
        for (c = 0; c < NC; c++) for (p = 0; p < NPARTS; p++)
            ok(sib_live[c][p], "SKIPPING walk: %s is still on the volume",
               sib[c][p]);
    }

    /* ---- stale-binary guard (LAST: it must not hide the legs) --------- */
    if (stale_binary("/proc/self/exe"))
        return 3;
    ok(1, "STALE-BINARY guard: this binary is newer than every source its "
          "legs depend on, so they measured the current build");

    vol_close(g_v);
    unlink(img);
    free(g_tar);

    printf("\nsib_walk_test: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
