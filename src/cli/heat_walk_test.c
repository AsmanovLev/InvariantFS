/* heat_walk_test.c — WP145: the sweep's HEAT stage must not act on a walk
 * that stopped.
 *
 * The defect. src/core/vol_heat.c took the status of a live-inode walk and
 * threw it away, twice:
 *
 *     :587   (void)vol_iter_live_inodes(v, heat_decay_cb,  &ctx);
 *     :733   (void)vol_iter_live_inodes(v, heat_promote_cb, &ctx);
 *
 * A stopped walk always reports "I saw exactly what I stored" -- found == n,
 * because that is literally true -- so a caller with no truncation signal
 * cannot tell a whole live set from a prefix of one. Both passes therefore
 * did their work on whatever the scan happened to reach and reported
 * nothing. That is placement, not a count: a cold file the walk never
 * reached keeps its heat, and keeps it on the next sweep too, because the
 * next sweep stops on the same unreadable page.
 *
 * WHY THE HEAT TABLE IS THE ORACLE (and not the sweep, and not invf-cat).
 *
 * The thing under test is the DECAY PASS, so the cheapest honest oracle is
 * the value the decay pass is supposed to change, read back through the
 * public format-agnostic xattr API (`vol_get_xattr(v, ino,
 * INVFS_XATTR_HEAT, ...)`) -- the same "invfs.heat" TLV the pass reads and
 * writes. It is the right oracle for three reasons, all of which rule out
 * the alternatives:
 *
 *   - `invf-verify --deep` is NOT an oracle at all (src/cli/verify.c:357-364
 *     checks readability and LENGTH only), and heat is invisible to it.
 *   - running the sweep to observe heat needs two sweep runs to get from a
 *     file to a promoted segment, and it is not deterministic about WHICH
 *     files get promoted (budget = 10% of the live TEXT members).
 *   - the heat TLV is exactly what placement and tiering key on, so a wrong
 *     value here is the damage itself, not a proxy for it.
 *
 * So: a fixture of files that all carry stored rheat = 16, a walk stopped at
 * a position that reaches SOME of them and not others, and the question is
 * which of them decayed.
 *
 * ARMING (src/core/vol_fault.h). The seam is `iter_live_inodes_row` in
 * src/core/vol_btree.c, checked once per live-inode row the iteration is
 * about to deliver -- so the ordinal in INVFS_FAULT is a POSITION WITHIN THE
 * WALK, and position n leaves exactly n rows visited. Two rules this test
 * obeys without exception:
 *
 *   - re-arm with invfs_vol_btree_fault_reload(). unsetenv+setenv is NOT a
 *     substitute: unsetenv frees the string, setenv usually gets the same
 *     address back, the spec is compared BY POINTER, and the countdown stays
 *     spent -- the leg then measures the healthy path and goes green proving
 *     nothing (src/core/vol_fault.h:68-81).
 *   - the ordinal is NOT a constant this test is entitled to. It scans every
 *     position 1..NPOS, prints a per-position trace of exactly which files
 *     decayed, and only calls a position DISCRIMINATING when it observed
 *     at-least-one decayed AND at-least-one not decayed. Which inode sits at
 *     position 1 depends on the root inode and on whether the fixture has
 *     been folded, and a hard-coded position would silently degrade into
 *     "the arm never fired" the moment that changed.
 *
 * STALE BINARY. `make -j4` does not relink the CLI/test binaries -- they are
 * prerequisites of `make test`, not of `all`. So the last check here greps
 * /proc/self/exe for a literal from the diagnostic the fix must print and
 * FAILS if it is absent. A binary built before the fix is the single most
 * likely reason for this control to pass for the wrong reason, so it is
 * asserted rather than assumed.
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
#include <sys/types.h>
#include <errno.h>

#include "volume_internal.h"
#include "vol_fault.h"
#include "vol_walk.h"

/* STALE-BINARY marks: fragments of the two diagnostics vol_heat.c prints
 * when it refuses.
 *
 * They are ASSEMBLED AT RUN TIME, in pieces, and that is not a stylistic
 * flourish. The binary this guard reads is THIS binary, so a mark written
 * out as one literal here would be present whether or not the fix is in the
 * build -- and the guard would pass always, which is precisely the failure
 * mode a stale-binary guard exists to catch. (Caught once during
 * development: the first version of this used a plain literal, and the
 * check reported "ok" on a binary that demonstrably did not contain the
 * fix.) Only the fixed vol_heat.c holds the pieces next to each other.
 *
 * The assertions elsewhere in this file match on short readable fragments
 * ("REFUSED", "UNCHANGED") instead; these two are for the staleness check
 * alone. */
static void stale_marks(char *decay, size_t dn, char *promote, size_t pn)
{
    static const char *d0 = "inode(s) rea", *d1 = "ched, and it stop",
                        *d2 = "ped rather than finished";
    static const char *p0 = "this pass would ex", *p1 = "tract data out of a ",
                        *p2 = "partial view";
    snprintf(decay, dn, "%s%s%s", d0, d1, d2);
    snprintf(promote, pn, "%s%s%s", p0, p1, p2);
}

#define NFILES   6
#define FILESZ   257
#define RHEAT0   16          /* stored read-heat in the fixture: above
                              * INVFS_HEAT_HOT (8), so a decayed value is
                              * still distinguishable from the original and
                              * one halving is plainly visible */
#define RHEAT_DECAYED (RHEAT0 / 2)
#define NPOS     24          /* upper bound on the seam's position search */

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

/* ---- the oracle: the persisted heat TLV --------------------------- */

/* -1 = no TLV, otherwise the stored read heat. */
static int heat_r(invfs_volume *v, uint64_t ino)
{
    uint8_t val[8];
    size_t vl = sizeof val;
    if (vol_get_xattr(v, ino, INVFS_XATTR_HEAT, val, &vl) != 0 || vl < 3)
        return -1;
    return (int)(val[0] | ((uint16_t)val[1] << 8));
}

static int seed_heat(invfs_volume *v, uint64_t ino, uint16_t r)
{
    uint8_t val[4];
    val[0] = (uint8_t)r;
    val[1] = (uint8_t)(r >> 8);
    val[2] = 0;
    val[3] = 0;
    return vol_set_xattr(v, ino, INVFS_XATTR_HEAT, val, sizeof val);
}

/* ---- arming ------------------------------------------------------ */

static void arm_row(const char *spec)
{
    setenv("INVFS_FAULT", spec, 1);
    invfs_vol_btree_fault_reload();     /* the ONLY way to re-arm this site */
}

static void disarm(void)
{
    unsetenv("INVFS_FAULT");
    invfs_vol_btree_fault_reload();
}

/* Run `fn` with BOTH streams redirected to a file, and hand back what it
 * said, so the assertions can be about a DIAGNOSTIC. "It said so" and "it
 * returned -1" are different claims and only the first is the property
 * under test.
 *
 * stdout matters as much as stderr here: the promotion pass's whole-volume
 * summary is a printf, so a leg that only captured stderr would see an
 * empty log and "pass" a check that the summary was suppressed. stdout is
 * unbuffered (setvbuf _IONBF in main), so redirecting the descriptor is
 * enough -- no flush dance. */
static char err_buf[16384];
static char g_scratch[512];

static void run_capture_err(void (*fn)(void *), void *arg)
{
    char tmp[640];
    int fd, s1, s2;
    ssize_t got;

    snprintf(tmp, sizeof tmp, "%s/heat_walk_test.err", g_scratch);
    fflush(stdout);
    fflush(stderr);
    fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    s1 = dup(1);
    s2 = dup(2);
    dup2(fd, 1);
    dup2(fd, 2);
    fn(arg);
    fflush(stdout);
    fflush(stderr);
    dup2(s1, 1);
    dup2(s2, 2);
    close(s1);
    close(s2);
    lseek(fd, 0, SEEK_SET);
    got = read(fd, err_buf, sizeof err_buf - 1);
    close(fd);
    if (got < 0) got = 0;
    err_buf[got] = 0;
}

/* ---- stale-binary guard ------------------------------------------ */

static int binary_contains(const char *lit)
{
    static char buf[1 << 20];
    int fd, hit = 0;
    ssize_t n;
    size_t l = strlen(lit);

    fd = open("/proc/self/exe", O_RDONLY);
    if (fd < 0) return 0;
    while ((n = read(fd, buf, sizeof buf)) > 0 && !hit) {
        ssize_t i;
        for (i = 0; i + (ssize_t)l <= n; i++)
            if (memcmp(buf + i, lit, l) == 0) { hit = 1; break; }
    }
    close(fd);
    return hit;
}

/* ---- the legs ---------------------------------------------------- */

static invfs_volume *g_v;
static uint64_t g_ino[NFILES];

static void leg_decay(void *arg)
{
    (void)arg;
    vol_heat_sweep_begin(g_v);
}

static void leg_promote(void *arg)
{
    (void)arg;
    (void)vol_heat_promote(g_v);
}

/* ---- the POSITION SEARCH ---------------------------------------- */

/* A read-only probe of the SAME iterator the heat stage uses, so the
 * position search can tell "the seam fired here" from "the seam never
 * fired here" without asking the decay pass anything. It has to: the fix
 * makes the decay pass write NOTHING on a short walk, so a search that
 * discriminated on "some files decayed and some did not" would find no
 * discriminating position at all on fixed code -- it would conclude the
 * arm had not fired, which is the false negative this whole test exists to
 * avoid. The probe reads the iterator's OWN status, which is identical
 * before and after the fix.
 *
 * It records WHICH fixture files the walk reached, not how many rows it
 * saw: the iteration also delivers the root inode and the batch owner,
 * which are rows but not files, and the distinction is the difference
 * between "stopped before it got to any file" (an empty subset) and
 * "stopped after it got to some" (the subset this is about). */
static int probe_rows;
static int probe_hit[NFILES];

static int probe_cb(invfs_volume *vv, uint64_t ino, const char *name,
                    void *ctx)
{
    int k;
    (void)vv; (void)name; (void)ctx;
    probe_rows++;
    for (k = 0; k < NFILES; k++)
        if (g_ino[k] == ino) probe_hit[k] = 1;
    return 0;
}

/* arm position `pos`, run the probe walk, report how many fixture files it
 * reached and whether the iterator itself said it stopped */
static int probe_at(int pos, int *files_reached)
{
    char spec[64];
    int rc, n = 0, k;

    snprintf(spec, sizeof spec, "iter_live_inodes_row:%d", pos);
    arm_row(spec);
    probe_rows = 0;
    memset(probe_hit, 0, sizeof probe_hit);
    rc = vol_iter_live_inodes(g_v, probe_cb, NULL);
    disarm();
    for (k = 0; k < NFILES; k++)
        n += probe_hit[k];
    *files_reached = n;
    return rc;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    invfs_volume *v;
    uint8_t src[NFILES][FILESZ];
    int err = 0, i, pos, nmade = 0;
    int seeded = 0, saw_complete = 0;
    int discriminating = 0, ndisc = 0, covered_at_disc = 0;
    int decayed_at_disc = -1;
    int refuted_stderr = 0, unchanged_stderr = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("heat_walk_test: the heat stage must not act on a stopped walk "
           "(%d files, rheat %d)\n", NFILES, RHEAT0);

    snprintf(img, sizeof img, "%s/invf-heat-walk-test.img", dir);
    /* the scratch the per-leg captures write into is the SAME argv[1] the
     * Makefile hands us (/tmp, private under $(TESTISO)); no environment
     * variable, so a leg cannot silently capture into a shared path. */
    snprintf(g_scratch, sizeof g_scratch, "%s", dir);
    unlink(img);
    {
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 4 >/dev/null 2>&1",
                 getenv("PWD") ? getenv("PWD") : ".", img);
        if (system(cmd) != 0) {
            fprintf(stderr, "cannot create volume with invf-mkfs\n");
            return 2;
        }
    }
    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "vol_open failed: %d\n", err);
        return 2;
    }
    g_v = v;

    for (i = 0; i < NFILES; i++) {
        invfs_wsession *ws = NULL;
        char nm[64];
        size_t k;
        snprintf(nm, sizeof nm, "f%02d.bin", i);
        for (k = 0; k < FILESZ; k++)
            src[i][k] = (uint8_t)(i * 53 + k * 11 + (k >> 4));
        if (vol_write_begin(v, nm, 1, &ws) == 0) {
            fprintf(stderr, "begin %s\n", nm); return 2;
        }
        if (vol_write_range(ws, 0, src[i], FILESZ) != 0 ||
            vol_write_commit(ws) != 0) {
            fprintf(stderr, "write %s\n", nm); return 2;
        }
        nmade++;
    }
    ok(nmade == NFILES, "fixture: %d files written to a v3 volume", nmade);

    for (i = 0; i < NFILES; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "f%02d.bin", i);
        g_ino[i] = vol_find(v, nm);
        if (g_ino[i] && seed_heat(v, g_ino[i], RHEAT0) == 0) seeded++;
    }
    ok(seeded == NFILES,
       "fixture: %d files carry a stored heat TLV of rheat=%d "
       "(the value the decay pass is supposed to halve)", seeded, RHEAT0);

    /* ---- the POSITION SEARCH (probe walk, read-only) --------------- */
    for (pos = 1; pos <= NPOS; pos++) {
        int rc, reached = 0;
        rc = probe_at(pos, &reached);
        info("pos %2d: iterator rc=%d, %d row(s), %d/%d fixture file(s) "
             "reached%s", pos, rc, probe_rows, reached, NFILES,
             rc == 0 ? " (walk COMPLETE -- arm past the last row)"
                     : " (walk STOPPED)");
        if (rc == 0) saw_complete = 1;
        /* DISCRIMINATING = the iterator itself said it stopped AND it got
         * to at-least-one fixture file but not all of them. Both halves
         * matter: an empty subset proves nothing about "a subset", and a
         * complete walk is not a stopped walk. */
        if (rc != 0 && reached > 0 && reached < NFILES) {
            if (!discriminating) {
                discriminating = pos;
                covered_at_disc = reached;
            }
            ndisc++;
        }
    }
    ok(saw_complete,
       "position search: the arm is POSITIONAL, not sticky -- some position "
       "ran past the last row and the iterator reported a complete walk, "
       "so the countdown really is per-row and the trace above is complete");
    ok(discriminating,
       "position search: %d discriminating position(s) found -- the "
       "iterator STOPPED having reached >=1 and <%d of the fixture files, "
       "which is the partial walk this test is about", ndisc, NFILES);

    /* ---- control: an UNARMED walk decays everything ---------------- */
    disarm();
    for (i = 0; i < NFILES; i++)
        (void)seed_heat(v, g_ino[i], RHEAT0);
    run_capture_err(leg_decay, NULL);
    {
        int n8 = 0;
        for (i = 0; i < NFILES; i++)
            if (heat_r(v, g_ino[i]) == RHEAT_DECAYED) n8++;
        ok(n8 == NFILES,
           "control (no fault armed): the decay pass halved ALL %d files "
           "(%d at rheat=%d) -- so a failure below is the WALK, not the "
           "fixture", NFILES, n8, RHEAT_DECAYED);
        ok(strstr(err_buf, "REFUSED") == NULL,
           "control (no fault armed): the pass reported nothing to refuse "
           "about");
    }

    /* ---- the claim under test, at the discriminating position -------- */
    if (discriminating) {
        int ndec = 0, nstuck = 0;
        char spec[64];
        for (i = 0; i < NFILES; i++)
            (void)seed_heat(v, g_ino[i], RHEAT0);
        snprintf(spec, sizeof spec, "iter_live_inodes_row:%d",
                 discriminating);
        arm_row(spec);
        run_capture_err(leg_decay, NULL);
        disarm();
        for (i = 0; i < NFILES; i++) {
            if (heat_r(v, g_ino[i]) == RHEAT_DECAYED) ndec++;
            else nstuck++;
        }
        decayed_at_disc = ndec;
        refuted_stderr = strstr(err_buf, "REFUSED") != NULL;
        unchanged_stderr = strstr(err_buf, "UNCHANGED") != NULL;
        info("at position %d the walk reached %d of %d fixture files; "
             "%d decayed, %d did not, and the pass said %s",
             discriminating, covered_at_disc, NFILES, ndec, nstuck,
             refuted_stderr ? "it REFUSED" : "nothing");
    }

    ok(decayed_at_disc == 0,
       "a stopped live-inode walk decays NOTHING: at the discriminating "
       "position the walk reached %d of %d files and %d had their heat "
       "rewritten (must be 0 -- a pass that decays the subset it happened "
       "to see IS the defect)", covered_at_disc, NFILES, decayed_at_disc);
    ok(refuted_stderr,
       "a stopped live-inode walk is REPORTED: the pass said so on stderr "
       "rather than returning as if it had covered the volume");
    ok(unchanged_stderr,
       "the refusal tells the operator what it did and did not change");

    /* ---- the promotion side of the same finding -------------------- */
    for (i = 0; i < NFILES; i++)
        (void)seed_heat(v, g_ino[i], RHEAT0);
    /* heat_any_rhot is what vol_heat_promote() gates on before it walks;
     * set it the way a read touch would (vol_heat.c heat_touch_read). */
    pthread_mutex_lock(&v->heat_mu);
    v->heat_any_rhot = 1;
    pthread_mutex_unlock(&v->heat_mu);
    arm_row("iter_live_inodes_row:1");
    run_capture_err(leg_promote, NULL);
    disarm();
    ok(strstr(err_buf, "promotion REFUSED") != NULL,
       "the promotion pass refuses a stopped walk too -- its candidate "
       "LIST and its 10%% budget would both come from the subset");
    ok(strstr(err_buf, "hot text member(s)") == NULL,
       "the promotion pass printed no whole-volume summary while its scan "
       "was incomplete -- \"heat: N hot text member(s), 0 promoted (budget "
       "M of K live)\" counts N, M and K from the subset it happened to "
       "see, and reads like the volume");

    /* ---- stale-binary guard (LAST: it must not hide the legs) ------ */
    {
        char dmark[128], pmark[128];
        stale_marks(dmark, sizeof dmark, pmark, sizeof pmark);
        if (!binary_contains(dmark)) {
            fprintf(stderr,
                "\nheat_walk_test: STALE BINARY -- %s does not contain the "
                "refusal diagnostic the fix prints. This binary was built "
                "before the fix, or a parallel make did not relink it: the "
                "test binaries are prerequisites of make test, not of all.\n"
                "    rm -f bin/invf-heat_walk_test && make bin/invf-heat_walk_test\n",
                "/proc/self/exe");
            return 3;
        }
        ok(binary_contains(pmark),
           "STALE-BINARY guard: this binary carries both of the fix's "
           "refusal diagnostics, so the legs above measured the fixed code "
           "and not a stale build");
    }

    vol_close(v);
    unlink(img);

    printf("\nheat_walk_test: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}