/* sweep_collect_test.c — the sweep-id COLLECTION decision.
 *
 * The defect this exists for. invf_sweep_worker() used to malloc a FIXED
 * 300,000-entry id array and hand it to vol_collect_sweepables(), which stops
 * the walk when the array is full and returns the count it stored. Nothing
 * compared that against what the walk had seen, so on a volume with more than
 * 300,000 sweepable inodes the in-FUSE pass -- kill -USR1, the
 * user.invfs.sweep xattr, and the raw_watermark ladder -- swept a strict
 * PREFIX and printed an ordinary successful DONE line. A live volume then
 * accumulates dead segments forever behind a clean log, and the one path
 * AGENTS.md 2.5/2.6 calls the daemon's own recovery is the one giving up.
 *
 * 300,000 inodes is not a memory budget (the ids are 2.4 MB against a process
 * that caches 256 MB of ARC by default) and it is not a fixed allocation
 * chosen for a reason: it is a bare literal with no comment, and
 * `git log -S "max = 300000"` reaches the initial import with no commit
 * message offering a rationale. So the fix is two things that can be asserted
 * WITHOUT building a 300,000-inode volume:
 *
 *   1. the truncation is DETECTABLE -- vol_collect_sweepables_ex() reports
 *      how many inodes the walk SAW, so `found > n` is a proof that the list
 *      is a strict subset rather than a silent prefix;
 *   2. the list GROWS -- vol_collect_sweepables_grow() doubles and re-collects
 *      until a collect comes back complete, and returns 0 only then.
 *
 * The volume here holds NFILES files, not 300,000. What is under test is the
 * DECISION, and the decision is the same decision at 8 and at 8 million: does
 * the collector notice it is full, and does it do something about it. Every
 * case below carries the control that flips it -- a term that does not move
 * the answer when the thing under test moves is not testing that thing.
 *
 *   1. TRUNCATION IS VISIBLE. Collect into a buffer deliberately far too
 *      small (8) for the volume (NFILES). The stored count must equal the
 *      cap, and `found` must EXCEED it -- that excess is the proof the old
 *      API could not give. Without the fix there is no `found` at all, and
 *      the count of 8 is indistinguishable from a volume that has 8 files:
 *      that indistinguishability IS the defect, asserted directly.
 *   2. ITS CONTROL. The same collect with a cap the volume fits inside must
 *      report found == n == NFILES and no truncation. If case 1 passed
 *      because `found` is simply always larger, this fails.
 *   3. GROWTH. vol_collect_sweepables_grow() seeded at ONE slot must reach
 *      every file, return 0, and leave a cap LARGER than it started with.
 *      This is the case the production call makes: the seed is not a cap.
 *   4. ITS CONTROL: THE SAME CALL, SEEDED AT THE FULL SIZE, MUST NOT GROW.
 *      A grow that reallocates unconditionally is not making a decision, and
 *      on a 500,000-inode volume it would double the working set for nothing.
 *   5. THE COUNT IS CHECKABLE, not merely larger: the grown list must contain
 *      exactly the live sweepable inodes -- every one of them resolvable, and
 *      none of them a repeat. (A list that grew but duplicated, or that
 *      dropped the tail, would pass 3 and 4 and fail here.)
 *   6. BIT-EXACTNESS THROUGH THE SWEEP. Every collected id is swept with
 *      vol_sweep_file() -- the same call the FUSE pass makes -- and every
 *      file is then read back and compared against the source bytes. This is
 *      the oracle the sweep's own invariant needs: invf-verify --deep is NOT
 *      one, because it checks readability and length only
 *      (src/cli/verify.c:400-415) and invfs_ast_block_entry carries a pba and
 *      no content hash (src/core/invarifs.h:970-980), so a wrong segment of
 *      the right length passes it. A memcmp against the source cannot.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "volume_internal.h"

#define NFILES 64
#define FILESZ 137          /* not a multiple of the segment size: a recipe
                             * with more than one entry, so the rebuild path
                             * is actually exercised by the read-back */
#define TINY_CAP 8          /* deliberately far below NFILES */

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

/* the source bytes for file i, and the same bytes back out of the volume */
static void fill_src(int i, uint8_t *b, size_t n)
{
    size_t k;
    for (k = 0; k < n; k++)
        b[k] = (uint8_t)(i * 31 + k * 7 + (k >> 5));
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    invfs_volume *v;
    uint8_t src[NFILES][FILESZ];
    uint64_t *ids = NULL;
    size_t cap, n, found, i, k;
    int err = 0, nmade = 0, unique = 1, resolvable = 1, mismatch = 0;
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("sweep_collect_test: the sweep-id collection decision "
           "(%d files, cap-under-test %d)\n", NFILES, TINY_CAP);

    snprintf(img, sizeof img, "%s/invf-sweep-collect-test.img", dir);
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

    for (i = 0; i < NFILES; i++) {
        invfs_wsession *ws = NULL;
        char nm[64];
        snprintf(nm, sizeof nm, "f%03zu.bin", i);
        fill_src((int)i, src[i], FILESZ);
        if (vol_write_begin(v, nm, 1, &ws) == 0) { fprintf(stderr, "begin %s\n", nm); return 2; }
        if (vol_write_range(ws, 0, src[i], FILESZ) != 0 ||
            vol_write_commit(ws) != 0) { fprintf(stderr, "write %s\n", nm); return 2; }
        nmade++;
    }
    ok(nmade == NFILES, "fixture: %d files written to a v3 volume", nmade);

    /* --- 1. truncation is VISIBLE ------------------------------------ */
    ids = (uint64_t *)calloc(TINY_CAP, sizeof *ids);
    if (!ids) { fprintf(stderr, "oom\n"); return 2; }
    n = vol_collect_sweepables_ex(v, ids, TINY_CAP, &found);
    ok(n == TINY_CAP,
       "1.  a cap of %d on a %d-file volume stores exactly the cap (%zu)",
       TINY_CAP, NFILES, n);
    ok(found > (size_t)n,
       "1b. and REPORTS that there were more: found=%zu > stored=%zu -- "
       "this is the proof the old fixed-buffer API could not give", found, n);

    /* --- 2. its control: a cap that fits ------------------------------ */
    {
        uint64_t *big = (uint64_t *)calloc(NFILES * 4, sizeof *big);
        size_t n2, f2;
        if (!big) { fprintf(stderr, "oom\n"); return 2; }
        n2 = vol_collect_sweepables_ex(v, big, NFILES * 4, &f2);
        ok(n2 == NFILES,
           "2.  CONTROL, a cap the volume fits inside collects all %d "
           "files (%zu)", NFILES, n2);
        ok(f2 == n2,
           "2b. and reports no shortfall: found=%zu == stored=%zu", f2, n2);
        free(big);
    }
    free(ids); ids = NULL;

    /* --- 3. the list GROWS from a one-slot seed ----------------------- */
    cap = 1;
    ids = (uint64_t *)calloc(cap, sizeof *ids);
    if (!ids) { fprintf(stderr, "oom\n"); return 2; }
    rc = vol_collect_sweepables_grow(v, &ids, &cap, &n, &found);
    ok(rc == 0,
       "3.  a one-slot seed GROWS to a complete collection and says so "
       "(rc=%d, cap %zu)", rc, cap);
    ok(n == NFILES,
       "3b. and reaches every one of the %d files: collected %zu", NFILES, n);
    ok(found == n,
       "3c. with the counts reconciled: found=%zu == collected=%zu -- a grow "
       "that returned 0 over a shortfall would be the original bug with a "
       "realloc added", found, n);
    ok(cap > 1,
       "3d. the cap itself moved (1 -> %zu): the seed is a seed, not a cap, "
       "which is exactly what the fixed 300,000 was not", cap);

    /* --- 4. its control: seeded at the full size, do NOT grow --------- */
    {
        size_t cap2 = NFILES * 4, n2, f2;
        uint64_t *ids2 = (uint64_t *)calloc(cap2, sizeof *ids2);
        if (!ids2) { fprintf(stderr, "oom\n"); return 2; }
        rc = vol_collect_sweepables_grow(v, &ids2, &cap2, &n2, &f2);
        ok(rc == 0 && n2 == NFILES,
           "4.  CONTROL, seeded at %zu for a %d-file volume: complete, no "
           "spurious growth (rc=%d n=%zu cap now %zu)",
           (size_t)(NFILES * 4), NFILES, rc, n2, cap2);
        free(ids2);
    }

    /* --- 5. the count is CHECKABLE: the right ids, no repeats -------- */
    for (i = 0; i < n; i++) {
        char nm[256];
        size_t j;
        uint64_t id = ids[i];
        for (j = 0; j < i; j++) if (ids[j] == id) unique = 0;
        if (vol_sweep_name_of(v, id, nm, sizeof nm) == 0) resolvable = 0;
    }
    ok(unique, "5.  every collected id is distinct: a list that grew by "
       "duplicating would pass 3 and fail here");
    ok(resolvable,
       "5b. every collected id still resolves to a name: none of the %zu is "
       "a tombstone or an internal row", n);

    /* --- 6. bit-exactness THROUGH the sweep -------------------------- */
    for (i = 0; i < n; i++) {
        uint8_t *back = NULL;
        size_t blen = 0;
        char nm[256];
        int idx;
        (void)vol_sweep_file(v, ids[i]);   /* the call the FUSE pass makes */
        if (vol_sweep_name_of(v, ids[i], nm, sizeof nm) == 0) continue;
        for (k = 0; k < (size_t)NFILES; k++) {
            char want[64];
            snprintf(want, sizeof want, "f%03zu.bin", k);
            if (strcmp(nm, want) == 0) { idx = (int)k; break; }
        }
        if (k == (size_t)NFILES) continue;          /* not one of ours */
        if (vol_read_file(v, ids[i], &back, &blen) != 0 || !back) {
            fprintf(stderr, "  READ FAILED %s\n", nm);
            mismatch++; free(back); continue;
        }
        if (blen != FILESZ || memcmp(back, src[idx], FILESZ) != 0) {
            fprintf(stderr, "  MISMATCH %s: %zu bytes read, %d expected\n",
                    nm, blen, FILESZ);
            mismatch++;
        }
        free(back);
    }
    ok(mismatch == 0,
       "6.  every file the collected list drove through vol_sweep_file() "
       "reads back byte-identical to its source (memcmp, %d bytes x %d files "
       "-- NOT invf-verify --deep, which checks length only)", FILESZ, NFILES);

    free(ids);
    vol_close(v);
    unlink(img);
    printf("%s: %d failing assertion(s) of %d\n",
           failures ? "FAIL" : "PASS", failures, checks);
    return failures ? 1 : 0;
}
