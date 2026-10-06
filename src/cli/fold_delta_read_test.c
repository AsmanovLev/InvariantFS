/* fold_delta_read_test.c — RED CONTROL for the fold/free vs delta-read race.
 *
 * WHAT THIS IS
 * ------------
 * `vol_inode_get` returned -1 for an inode that was present, live and
 * internally consistent, at ~1e-5 of all reads under a concurrent fold
 * (src/cli/concurrency_test.c measured it; it is gated there now).
 *
 * The cause was NOT "the lookup races the publisher", which is what the
 * original report claimed. Instrumenting every -1 return of vol_inode_get
 * put 100% of them in one place -- ino_decode() failing on a DELTA row
 * (vol_btree.c:3591) -- and the bytes it was handed were all zeros:
 *
 *     TAG3 seg=897947 off=5402 vlen=114 rlen=114 b0=00000000 b4=00000000
 *
 * The sequence, in four steps:
 *
 *   1. vol_delta_lookup resolves the winning record under g_delta_lock and
 *      copies the ref {seg, off, vlen} BY VALUE, then RELEASES the lock.
 *   2. vol_delta_read_value re-reads the record with two bare preads, with no
 *      lock at all. The ref names a block range, not a copy of the bytes.
 *   3. vol_fold's tail drops the index and frees every block of the retired
 *      chain -- outside the lock that guards the index.
 *   4. The next delta_new_segment gets those blocks straight back from the
 *      allocator and writes a segment header plus ZEROS over all 128 KiB.
 *
 * So the reader decoded a recycled, zero-filled block and was told -1 for an
 * inode whose row was sitting intact in the new base. The row was consistent
 * because nothing had damaged it; what the reader had lost was the right to
 * read those bytes.
 *
 * WHY THIS IS NOT A PROBABILITY TEST
 * ----------------------------------
 * The measured rate is ~1e-5: a 3-second race run on a fast tmpfs reproduces
 * it in roughly one run in five, and not at all on a slow image (the fold
 * never fires). A regression test with that failure probability is a test that
 * will be "flaky" forever and then deleted. So the interleave is PLANNED here,
 * in the arc_san_test.c / heat_san_test.c tradition:
 *
 *   - vol_delta_read_value calls the weak seam invfs_test_delta_read_hook(ref)
 *     from inside its critical section. This file defines it, so it runs; on
 *     any other build the weak definition in vol_delta.c is what links and it
 *     is a no-op.
 *   - The hook parks the reader holding a resolved ref. The test then runs a
 *     REAL fold to completion and forces the block range that ref names to be
 *     recycled, and only then releases the reader.
 *
 * The assertions are chosen so the test cannot pass for the wrong reason:
 *
 *   fold_returned_early == 1  the window was open -> the red control really
 *                              did arm, and `recycled` must confirm the block
 *                              was actually handed back and zeroed. If a future
 *                              allocator change stops recycling it, THIS check
 *                              fails rather than the test quietly going green.
 *   fold_returned_early == 0  the fold could not get past the reader, which
 *                              is the fix: the value read and the chain free
 *                              share g_delta_lock.
 *
 * Either way the reader must come back with inode 42's seeded row, field for
 * field -- the read-path outcome, not just a return code.
 *
 * Pre-fix: fold returns, block is zeroed, reader decodes zeros, r == -1. RED.
 * Post-fix: the fold blocks behind the reader, the bytes are still the
 *           record's, r == 1 and the row matches. GREEN.
 *
 * Usage: invf-fold_delta_read_test <scratch-dir>
 * exit 0 = pass, 1 = a check failed, 2 = setup failure.
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

/* ---- the seam -------------------------------------------------------- */

#define INO_ID      42ULL
#define INO_SIZE    100u
#define INO_MODE    0644

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;

static int      g_parked;          /* reader is inside the critical section */
static int      g_release;         /* main has said go                     */
static int      g_arm;             /* only the ARMED read parks            */
static pthread_t g_reader;         /* ... and only THAT thread's            */
static delta_ref g_ref;            /* the ref the reader resolved         */
static int      g_have_ref;
static invfs_volume *g_v;

/* Called from inside vol_delta_read_value, holding g_delta_lock. Only the
 * read the test has ARMED parks: the setup read above and the post-state read
 * below go straight through, exactly as production does. */
void invfs_test_delta_read_hook(const void *ref)
{
    const delta_ref *r = (const delta_ref *)ref;

    pthread_mutex_lock(&g_mu);
    /* Only the reader this test armed. The fold calls vol_delta_read_value
     * too (fold_collect_cb), and parking it here would deadlock the very
     * window the test is trying to open. */
    if (g_arm && pthread_equal(pthread_self(), g_reader)) {
        g_ref = *r;
        g_have_ref = 1;
        g_parked = 1;
        pthread_cond_broadcast(&g_cv);
        while (!g_release)
            pthread_cond_wait(&g_cv, &g_mu);
    }
    pthread_mutex_unlock(&g_mu);
}

/* ---- helpers --------------------------------------------------------- */

static void ino_key(uint64_t id, uint8_t k[8])
{
    int i;
    for (i = 0; i < 8; i++)
        k[i] = (uint8_t)(id >> (56 - 8 * i));
}

static uint16_t ino_row(uint8_t *buf, uint64_t size)
{
    invfs_inode_row r;
    memset(&r, 0, sizeof r);
    r.row_version = INVFS_INODE_ROW_VERSION;
    r.type = INVFS_ITYP_REG;
    r.mode = INO_MODE;
    r.uid = 1000;
    r.gid = 1000;
    r.nlink = 1;
    r.size = size;
    r.mtime = (int64_t)1700000000;      /* frozen: the row is compared */
    r.atime = r.mtime;
    memcpy(buf, &r, sizeof r);
    return (uint16_t)sizeof r;
}

static int delta_put_inode(uint64_t id, uint64_t size)
{
    uint8_t k[8], v[INVFS_INODE_ROW_FIXED];
    ino_key(id, k);
    return vol_delta_append(g_v, k, sizeof k, v, ino_row(v, size), 0);
}

/* The reader thread. One call; the test parks it inside the seam. */
static invfs_inode g_got;
static int            g_rc;

static void *reader_thread(void *arg)
{
    (void)arg;
    memset(&g_got, 0, sizeof g_got);
    g_reader = pthread_self();
    g_rc = vol_inode_get(g_v, INO_ID, &g_got);
    return NULL;
}

static volatile int g_fold_done;

static void *fold_thread(void *arg)
{
    (void)arg;
    (void)vol_fold(g_v);
    g_fold_done = 1;
    return NULL;
}

static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512], cmd[1024];
    pthread_t rt, ft;
    int err = 0, i, recycled = 0, fold_returned_early;
    uint8_t probe[INVFS_INODE_ROW_FIXED];
    delta_ref ref;

    printf("fold/delta-read race test (WP-inode-get-fold-race)\n");

    snprintf(img, sizeof img, "%s/invf-fold_delta_read_test.img", dir);
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

    /* Seed the base with the inode the reader will ask for. The read is
     * deliberately resolved from the DELTA, so the seam is reached: a base
     * read never enters vol_delta_read_value. */
    {
        invfs_inode in;
        uint8_t k[8], v[INVFS_INODE_ROW_FIXED];
        memset(&in, 0, sizeof in);
        in.type = INVFS_ITYP_REG;
        in.mode = INO_MODE;
        in.uid = in.gid = 1000;
        in.nlink = 1;
        in.size = 4321;
        in.mtime = in.atime = (int64_t)1700000000;
        ok(vol_inode_put(g_v, INO_ID, &in) == 0, "base put inode 42");
        /* Now a DELTA row for the same id: newer, different size, so the
         * test can tell which copy the reader returned. */
        ino_key(INO_ID, k);
        ok(vol_delta_append(g_v, k, sizeof k, v, ino_row(v, INO_SIZE), 0) == 0,
           "delta append inode 42");
    }

    /* One record is not always a whole segment; make the retired chain a
     * segment's worth so the free covers the block the ref names. */
    for (i = 0; i < 64; i++)
        delta_put_inode(2000 + (uint64_t)i, 10);

    /* Sanity: the read resolves out of the delta and matches the delta row. */
    {
        invfs_inode in;
        ok(vol_inode_get(g_v, INO_ID, &in) == 1 && in.size == INO_SIZE,
           "pre-state: the delta row shadows the base row");
    }

    /* --- the interleave ------------------------------------------------ */
    g_arm = 1;
    if (pthread_create(&rt, NULL, reader_thread, NULL) != 0) {
        printf("  cannot create reader thread\n");
        return 2;
    }
    /* wait for the reader to park inside vol_delta_read_value */
    pthread_mutex_lock(&g_mu);
    while (!g_parked)
        pthread_cond_wait(&g_cv, &g_mu);
    ref = g_ref;
    pthread_mutex_unlock(&g_mu);
    ok(g_have_ref && ref.vlen > 0, "the reader resolved a delta ref");

    g_fold_done = 0;
    if (pthread_create(&ft, NULL, fold_thread, NULL) != 0) {
        printf("  cannot create fold thread\n");
        return 2;
    }
    /* Give the fold a bounded chance to finish on its own. If it can, the
     * window is open and the test has to arm it. If it cannot -- because the
     * reader is holding the lock its reset needs -- that IS the fix and
     * there is nothing to arm. */
    for (i = 0; i < 500 && !g_fold_done; i++)
        msleep(1);
    fold_returned_early = g_fold_done;

    if (fold_returned_early) {
        /* Force the retired block range to be handed back. Each append that
         * needs a new segment allocates a fresh 32-block stripe from the pool
         * the fold just freed (delta_new_segment zeroes it), so enough records
         * walk the allocator across the whole freed range. */
        uint8_t want[INVFS_INODE_ROW_FIXED];
        (void)ino_row(want, INO_SIZE);
        for (i = 0; i < 4000; i++)
            delta_put_inode(5000 + (uint64_t)i, 20);
        /* Did the bytes the reader is about to read actually change? The
         * record's own layout is frozen -- an inode key is 8 bytes -- so its
         * value sat at off + 10 + 8. Read the whole value back the way
         * vol_delta_read_value does and compare it with the row that was
         * appended there. A partial compare is not enough: every inode row
         * starts with the same row_version, so the first four bytes match
         * whether the block was recycled or not (and on the broken tree the
         * reader is handed ANOTHER inode's row, which is the worse half of
         * this bug). */
        memset(probe, 0, sizeof probe);
        if (io_pread(&g_v->io,
                     ref.seg * (uint64_t)INVFS_BLOCK_SIZE + ref.off +
                     INVFS_DELTA_REC_HDR_LEN + 8,
                     probe, sizeof probe) == 0)
            recycled = (memcmp(probe, want, sizeof want) != 0);
        ok(recycled,
           "RED CONTROL ARMED: the record the reader holds was freed and its "
           "block handed back to the allocator");
    } else {
        printf("  the fold could not complete while the reader held its ref "
               "(the read and the free share one critical section)\n");
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
     * itself, field for field, against the exact bytes that were appended.
     * Pre-fix g_rc is -1 (decoded from the recycled block). */
    ok(g_rc == 1, "vol_inode_get returned the row (1), not an error");
    if (g_rc == 1) {
        ok(g_got.type == INVFS_ITYP_REG, "row: type is REG");
        ok((g_got.mode & 07777) == INO_MODE, "row: mode is 0644");
        ok(g_got.uid == 1000 && g_got.gid == 1000, "row: uid/gid intact");
        ok(g_got.nlink == 1, "row: nlink intact");
        if (g_got.size != INO_SIZE)
            printf("  (got size=%llu mode=%o type=%u nlink=%llu for inode 42, "
                   "which was written with size=%u)\n",
                   (unsigned long long)g_got.size, (unsigned)g_got.mode,
                   (unsigned)g_got.type, (unsigned long long)g_got.nlink,
                   (unsigned)INO_SIZE);
        ok(g_got.size == INO_SIZE,
           "row: size is the delta row's, not the base row's and not garbage");
        ok(g_got.mtime == 1700000000, "row: mtime intact");
    }

    /* And the volume is still whole: the same key reads back correctly with
     * no threads running at all. */
    {
        invfs_inode in;
        int r = vol_inode_get(g_v, INO_ID, &in);
        ok(r == 1 && in.size == INO_SIZE,
           "post-state: inode 42 still reads back with its own value");
    }

    vol_close(g_v);
    g_v = NULL;
    remove(img);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
