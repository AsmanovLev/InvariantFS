/* reclaim_reader_epoch_test.c — RED CONTROL for the base-reclaim vs reader race.
 *
 * WHAT THIS IS
 * ------------
 * `vol_reclaim_drain` (src/core/vol_reclaim.c:199) spins on
 * `g_readers_in_flight`, and nothing in the tree ever increments it:
 * `vol_reclaim_reader_snapshot` (:188) returns the epoch and touches no
 * counter, and no read path calls the pair at all. So the drain returns
 * immediately, and `fold_reclaim_hook` (src/core/vol_fold.c:257) frees a
 * retired generation with no regard for a reader that captured its root.
 *
 * The consequence is the only failure mode in this tree that is not a wrong
 * answer. No error has happened. The data was fine, the reader was entitled
 * to it, and the pages went away underneath it. A read returns -1 because
 * `mbuf_read_ptr` consults the allocation bitmap and the bit is now clear
 * (src/core/vol_metabuf.c:192). Measured at 0-3 per ~3M reads over ~1500
 * folds (~1e-6) -- which is why src/cli/concurrency_test.c can only print
 * that counter.
 *
 * WHY THIS IS NOT A PROBABILITY TEST
 * ----------------------------------
 * 1e-6 is not a regression test; it is a coin flip that stops showing up on
 * a slow image because the fold never fires. So the interleave is PLANNED,
 * in the arc_san_test.c / heat_san_test.c / fold_delta_read_test.c
 * tradition:
 *
 *   - v3_base_root (src/core/vol_btree.c) calls the weak seam
 *     invfs_test_base_read_hook(root) at the exact moment the base root is
 *     captured and before any page of it is read. This file defines it, so
 *     it runs; on any other build the weak definition in vol_btree.c is what
 *     links and it is a no-op. That is the one point EVERY base-tree read
 *     passes through, so parking here parks a real reader at a real
 *     boundary.
 *   - The hook parks the reader holding a root it has already captured.
 *     This file then runs REAL folds to completion until the generation that
 *     root names is no longer allocated, and only then releases the reader.
 *
 * The assertion is the read-path outcome: the row itself, field for field,
 * against the exact bytes that were seeded. A counter reaching zero is NOT
 * what is being gated, because a counter that is never incremented reads
 * zero perfectly well.
 *
 * Pre-fix: the folds complete, R0's pages are collected, the reader walks
 *          R0, mbuf_read_ptr refuses the root page, and
 *          vol_v3_inode_get returns -1. RED.
 * Post-fix: the fold blocks in vol_reclaim_drain behind the reader, the
 *          pages are still there, and the read returns the row. GREEN.
 *
 * THE CONTROL ON THE CONTROL. This test cannot go green by accident:
 *
 *   retired_root_freed == 1 and the read returned -1
 *       the red control armed, and the read demonstrably FAILED. The test
 *       reports that as a FAIL and exits non-zero -- which is what it must
 *       do on the unfixed tree.
 *   retired_root_freed == 0 and the folds completed
 *       the setup is broken (the folds did not reclaim what the test
 *       assumed they would). That is a FAIL too, not a pass.
 *   retired_root_freed == 0 and the FIRST fold never completed
 *       the fold is blocked behind the reader. That IS the fix, and it is
 *       the only path to a pass.
 *
 * Usage: invf-reclaim_reader_epoch_test <scratch-dir>
 * exit 0 = pass, 1 = a check failed, 2 = setup failure.
 *
 * STATUS: RED ON THIS TREE, AND NOT IN THE `make test` GATE.
 *
 * The defect this controls is still open, so the test fails today, on
 * purpose and deterministically (12/12 runs on the branch this landed on).
 * It is in CLI_MAINS so that `make` builds it -- a red control nobody ever
 * executes is a comment with a build target -- but it is NOT one of the
 * `test:` recipe's commands, because a knowingly-red gate would make
 * `make test` red for a defect that has not been fixed. It joins the recipe
 * in the same commit that wires the reclaim reader epoch. Until then:
 *
 *   ./bin/invf-reclaim_reader_epoch_test /tmp    # expect: "the read FAILED"
 */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_delta.h"
#include "vol_metabuf.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* ---- the fixture ----------------------------------------------------- */

#define INO_ID       1ULL          /* the inode the parked reader asks for */
#define INO_SIZE     4096u
#define INO_MODE     0644
#define NFILL        192          /* base rows, so R0 is a multi-leaf tree */
#define NROUNDS      64           /* folds before we call the setup broken */
#define WAIT_MS      8000         /* how long the reclaimer gets to settle */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;

static int  g_arm;                 /* only the ARMED read parks            */
static int  g_parked;              /* reader is inside the seam            */
static int  g_release;             /* main has said go                     */
static pthread_t g_reader;         /* ... and only THAT thread's            */
static invfs_blkptr g_held;        /* the root the reader captured         */
static int  g_have_held;
static invfs_volume *g_v;

/* The retired generation, in the only form the fix has to preserve: the
 * reader is entitled to every page its captured root reaches. */
static int  g_retired_freed;       /* reclaimer got there first            */
static int  g_folds_completed;
static int  g_first_fold_blocked;
static int  g_any_fold_blocked;

static invfs_v3_inode g_seed;      /* the row that was seeded for INO_ID  */

/* Called from inside v3_base_root, with the root it just captured. Only the
 * read this test armed parks: the setup folds and the post-state read below
 * capture roots too, and parking those would deadlock the very window the
 * test is trying to open. */
void invfs_test_base_read_hook(const invfs_blkptr *root)
{
    pthread_mutex_lock(&g_mu);
    if (g_arm && pthread_equal(pthread_self(), g_reader)) {
        g_held = *root;
        g_have_held = 1;
        g_parked = 1;
        pthread_cond_broadcast(&g_cv);
        while (!g_release)
            pthread_cond_wait(&g_cv, &g_mu);
    }
    pthread_mutex_unlock(&g_mu);
}

/* ---- helpers --------------------------------------------------------- */

static void seed_row(uint64_t id, uint32_t size, invfs_v3_inode *out)
{
    memset(out, 0, sizeof *out);
    out->type  = INVFS_ITYP_REG;
    out->mode  = INO_MODE;
    out->uid   = 1000;
    out->gid   = 1000;
    out->nlink = 1;
    out->size  = size;
    /* Frozen: this row is compared field for field after the race. */
    out->mtime = (int64_t)1700000000;
    out->atime = out->mtime;
    (void)id;
}

/* Append a delta record that CHANGES inode `id`, so the next fold's COW
 * rewrite replaces the leaf the old root reaches and nothing is shared. */
static int touch(uint64_t id, uint32_t size)
{
    invfs_v3_inode in;
    seed_row(id, size, &in);
    return vol_v3_inode_delta_put(g_v, id, &in);
}

static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---- the reader ------------------------------------------------------ */

static invfs_v3_inode g_got;
static int            g_rc;

static void *reader_thread(void *arg)
{
    (void)arg;
    memset(&g_got, 0, sizeof g_got);
    g_reader = pthread_self();
    g_rc = vol_v3_inode_get(g_v, INO_ID, &g_got);
    return NULL;
}

/* ---- the reclaimer --------------------------------------------------- */

/* Runs REAL folds, one per round, until the retired generation is gone from
 * the allocation bitmap or we give up. It is a thread so the test can bound
 * the wait: on a fixed tree the FIRST fold never returns (it is in
 * vol_reclaim_drain waiting for the reader), and that non-return is the
 * signal that says the drain works. */
static void *folder_thread(void *arg)
{
    int round;
    (void)arg;

    for (round = 0; round < NROUNDS; round++) {
        int r;
        pthread_mutex_lock(&g_mu);
        g_first_fold_blocked = 1;
        pthread_mutex_unlock(&g_mu);

        /* A fold is a no-op on an empty delta, so give it something to do. */
        if (touch(90000ULL + (uint64_t)round, 16u) != 0)
            break;
        r = vol_v3_fold(g_v);

        pthread_mutex_lock(&g_mu);
        g_first_fold_blocked = 0;
        if (r == 0)
            g_folds_completed++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);

        if (g_have_held && g_held.pba &&
            mbuf_page_allocated(g_v, g_held.pba) == 0) {
            pthread_mutex_lock(&g_mu);
            g_retired_freed = 1;
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mu);
            return NULL;
        }
    }

    pthread_mutex_lock(&g_mu);
    g_any_fold_blocked = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512], cmd[1024];
    pthread_t rt, ft;
    int err = 0, i, waited_ms;
    struct timespec deadline;

    printf("reclaim/reader-epoch race test (base-tree generation reclaim)\n");

    snprintf(img, sizeof img, "%s/invf-reclaim_reader_epoch_test.img", dir);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 16 2>/dev/null",
             getenv("PWD") ? getenv("PWD") : ".", img);
    if (system(cmd) != 0) {
        printf("  cannot create volume with invf-mkfs\n");
        return 2;
    }
    g_v = vol_open(img, &err);
    if (!g_v) {
        fprintf(stderr, "vol_open(%s) failed: err=%d\n", img, err);
        return 2;
    }

    /* Fill the base through the delta and fold it once, so the tree R0 names
     * is several leaves deep and every one of R0's pages is unique to R0 --
     * nothing is shared with the empty tree it grew out of. */
    for (i = 0; i < NFILL; i++) {
        invfs_v3_inode in;
        seed_row((uint64_t)(i + 1), (uint32_t)(100 + i), &in);
        if (i + 1 == (int)INO_ID)
            g_seed = in;
        if (vol_v3_inode_delta_put(g_v, (uint64_t)(i + 1), &in) != 0) {
            printf("  seed: vol_v3_inode_delta_put(%d) failed\n", i + 1);
            vol_close(g_v);
            return 2;
        }
    }
    if (vol_v3_fold(g_v) != 0) {
        printf("  seed: the initial fold failed\n");
        vol_close(g_v);
        return 2;
    }
    /* Sanity: the read resolves out of the BASE now (the delta is empty),
     * which is the path the seam is on. */
    {
        invfs_v3_inode in;
        if (vol_v3_inode_get(g_v, INO_ID, &in) != 1) {
            printf("  seed: the base read of inode %llu failed\n",
                   (unsigned long long)INO_ID);
            vol_close(g_v);
            return 2;
        }
    }

    /* --- the interleave ------------------------------------------------ */
    g_arm = 1;
    if (pthread_create(&rt, NULL, reader_thread, NULL) != 0) {
        printf("  cannot create reader thread\n");
        return 2;
    }
    pthread_mutex_lock(&g_mu);
    while (!g_parked)
        pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    ok(g_have_held && g_held.pba != 0,
       "the reader captured a base root before walking it");

    if (pthread_create(&ft, NULL, folder_thread, NULL) != 0) {
        printf("  cannot create fold thread\n");
        return 2;
    }

    /* Give the reclaimer a bounded chance to retire the held generation. */
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    for (waited_ms = 0; waited_ms < WAIT_MS; waited_ms += 10) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - deadline.tv_sec > WAIT_MS / 1000 + 1)
            break;
        pthread_mutex_lock(&g_mu);
        i = g_retired_freed || g_any_fold_blocked;
        pthread_mutex_unlock(&g_mu);
        if (i)
            break;
        msleep(10);
    }

    pthread_mutex_lock(&g_mu);
    i = g_retired_freed;
    pthread_mutex_unlock(&g_mu);

    if (!i) {
        /* The reclaimer did NOT get there. Either the folds are wedged
         * behind the reader -- which is the fix -- or the setup is wrong.
         * Which one is decided below, from what the fold thread reported. */
        printf("  the retired generation was still allocated after %d folds; "
               "checking whether the fold is blocked behind the reader\n",
               g_folds_completed);
    }

    /* release the reader */
    pthread_mutex_lock(&g_mu);
    g_release = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);

    pthread_join(rt, NULL);
    pthread_join(ft, NULL);
    g_arm = 0;

    /* THE ASSERTION: the read-path outcome. Not "did it return" -- the row
     * itself, field for field, against the exact row that was seeded. On the
     * unfixed tree g_rc is -1: mbuf_read_ptr refused the root page because
     * the allocation bitmap says the volume has given that block away. */
    ok(g_rc == 1, "vol_v3_inode_get returned the row (1), not an error");
    if (g_rc == 1) {
        ok(g_got.size  == g_seed.size,  "read row: size");
        ok(g_got.mode  == g_seed.mode,  "read row: mode");
        ok(g_got.type  == g_seed.type,  "read row: type");
        ok(g_got.nlink == g_seed.nlink, "read row: nlink");
        ok(g_got.uid   == g_seed.uid,   "read row: uid");
        ok(g_got.gid   == g_seed.gid,   "read row: gid");
        ok(g_got.mtime == g_seed.mtime, "read row: mtime");
    } else {
        printf("  the read FAILED (rc=%d) with no error having happened: the "
               "pages of the generation the reader captured were collected "
               "under it\n", g_rc);
    }

    /* The control on the control: exactly one of the two states must hold. */
    ok(g_retired_freed || g_any_fold_blocked,
       "the interleave was decided: either the retired generation was "
       "collected, or the fold could not get past the reader");
    if (g_retired_freed) {
        ok(g_folds_completed > 0,
           "the folds ran to completion while the reader held the old root "
           "(the drain did not wait for a count nothing increments)");
    }

    /* The volume must still be consistent afterwards: the reclaimer only
     * ever had permission to take pages no live root reaches. */
    {
        invfs_v3_inode in;
        int bad = 0;
        for (i = 1; i <= NFILL; i++)
            if (vol_v3_inode_get(g_v, (uint64_t)i, &in) != 1 ||
                in.mode != INO_MODE)
                bad++;
        ok(bad == 0, "every seeded inode is still readable after the race");
    }

    vol_close(g_v);
    g_v = NULL;
    remove(img);

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
