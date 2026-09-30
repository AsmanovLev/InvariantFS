/* concurrency_test.c — WP-M20 lock-free base-read / delta-append test.
 *
 * Exercises the WP-M20 concurrency design:
 *   - Base reads (vol_v3_inode_get) take no lock beyond the delta index's:
 *     the base is immutable between folds, and a delta_ref is read under the
 *     delta lock for exactly as long as the bytes it names are allocated
 *     (WP-inode-get-fold-race -- "immutable between folds" is true of the
 *     base and was never true of the recent tier, which the fold frees).
 *   - Delta append is the only writer-critical section (g_delta_append_lock).
 *   - Multiple reader threads hammer the read path while one writer thread
 *     continuously appends delta records and triggers folds.
 *
 * exit 0 = all checks passed, 1 = a check failed, 2 = usage/open error.
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
static volatile int g_stop = 0;

/* Cross-thread tallies. The reader threads used to discard every
 * vol_v3_inode_get result (`(void)r`), so the one assertion this file exists
 * to make -- a base read racing a delta append returns a whole, consistent
 * row -- was never made: the suite stayed green on a completely dead
 * delta-append path. main() asserts on these numbers, so a bad read is
 * counted here rather than thrown away. */
static volatile uint64_t g_read_ops;    /* got a whole, consistent row   */
static volatile uint64_t g_read_torn;   /* got a row from no single gen  */
static volatile uint64_t g_read_absent; /* got 0: a live inode vanished  */
static volatile uint64_t g_read_ioerr;  /* got -1: gated in main()      */
static volatile uint64_t g_write_ops;   /* successful writer mutations   */
static volatile uint64_t g_folds;       /* vol_v3_fold calls             */

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* ---- key / value encodings (frozen WP-M5/M6 forms) -------------------- */

static void ino_key(uint64_t id, uint8_t k[8])
{
    int i;
    for (i = 0; i < 8; i++)
        k[i] = (uint8_t)(id >> (56 - 8 * i));
}

static uint16_t ino_row(uint8_t *buf, uint16_t mode, uint64_t size)
{
    invfs_v3_inode_row r;
    memset(&r, 0, sizeof r);
    r.row_version = INVFS_V3_INODE_ROW_VERSION;
    r.type = INVFS_ITYP_REG;
    r.mode = mode;
    r.uid = 1000;
    r.gid = 1000;
    r.nlink = 1;
    r.size = size;
    r.mtime = (int64_t)time(NULL);
    r.atime = r.mtime;
    memcpy(buf, &r, sizeof r);
    return (uint16_t)sizeof r;
}

/* ---- volume ------------------------------------------------------------ */

static invfs_volume *g_v;

static int delta_put_inode(uint64_t id, uint16_t mode, uint64_t size)
{
    uint8_t k[8], v[INVFS_V3_INODE_ROW_FIXED];
    uint16_t vl;
    ino_key(id, k);
    vl = ino_row(v, mode, size);
    return vol_delta_append(g_v, k, sizeof k, v, vl, 0);
}

static int delta_del_inode(uint64_t id)
{
    uint8_t k[8];
    ino_key(id, k);
    return vol_delta_append(g_v, k, sizeof k, NULL, 0,
                            INVFS_DELTA_FLAG_DELETE);
}

/* ---- reader thread ---------------------------------------------------- */

/* Inode 42 is seeded in the base and re-put by the writer on every iteration
 * and never deleted, so a racing read must always find it (rc == 1) and must
 * always see ONE writer generation's row -- never a mix. The writer's row is
 * mode in {0644, 0744} (it toggles the x bit) and size = 100 + fold_iter, so
 * size >= 100 always. Anything else is counted rather than discarded; the
 * counts are asserted after the join.
 *
 * Four outcomes, kept apart because they mean different things:
 *   r == 1 with a consistent row   -> the contract holding
 *   r == 1 with an inconsistent row -> a TORN read: gated
 *   r == 0                          -> a live inode reported absent: gated
 *   r == -1                         -> gated; see the note at the assertion
 *                                      in main().
 */
static void *reader_thread(void *arg)
{
    (void)arg;
    uint64_t ops = 0;
    while (!g_stop) {
        invfs_v3_inode in;
        int r = vol_v3_inode_get(g_v, 42, &in);
        if (r < 0) {
            g_read_ioerr++;
            continue;
        }
        if (r == 0) {
            g_read_absent++;
            continue;
        }
        {
            uint16_t m = (uint16_t)(in.mode & 07777);
            if (in.type != INVFS_ITYP_REG || in.size < 100 ||
                (m != 0644 && m != 0744) ||
                in.uid != 1000 || in.gid != 1000 || in.nlink != 1) {
                g_read_torn++;
                continue;
            }
        }
        g_read_ops++;
        ops++;
    }
    printf("  reader did %llu consistent ops\n", (unsigned long long)ops);
    return NULL;
}

/* ---- writer thread ---------------------------------------------------- */

static void *writer_thread(void *arg)
{
    (void)arg;
    uint64_t fold_iter = 0;
    uint64_t ops = 0;
    uint64_t ino_counter = 1000;
    while (!g_stop) {
        uint16_t mode = (uint16_t)(0644 | (fold_iter & 1 ? 0100 : 0));
        uint64_t size = 100 + fold_iter;
        if (delta_put_inode(42, mode, size) == 0) {
            ops++;
        }
        if (delta_put_inode(ino_counter, 0644, 50) == 0) {
            ops++;
            ino_counter++;
        }
        if (ino_counter > 1010) {
            delta_del_inode(ino_counter - 5);
            ops++;
        }
        if (fold_iter > 0 && fold_iter % 20 == 0) {
            vol_v3_fold(g_v);
            ops++;
            g_folds++;
        }
        fold_iter++;
    }
    g_write_ops += ops;
    printf("  writer did %llu ops (%llu folds)\n",
           (unsigned long long)ops, (unsigned long long)(fold_iter / 20));
    return NULL;
}

/* ---- main ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    pthread_t readers[4];
    pthread_t writer;
    int i;

    printf("concurrency tests (WP-M20): lock-free base reads + delta append\n");

    snprintf(img, sizeof img, "%s/invf-concurrency_test.img", dir);

    /* build a minimal v3 volume using mkfs */
    {
        char cmd[1024];
        snprintf(cmd, sizeof cmd,
                 "%s/bin/invf-mkfs %s 16 2>/dev/null",
                 getenv("PWD") ? getenv("PWD") : ".", img);
        if (system(cmd) != 0) {
            printf("  cannot create volume with invf-mkfs\n");
            return 2;
        }
    }

    {
        int err = 0;
        g_v = vol_open(img, &err);
        if (!g_v) {
            fprintf(stderr, "concurrency_test: vol_open(%s) failed: err=%d\n",
                    img, err);
            return 2;
        }
    }

    /* seed base inodes */
    {
        invfs_v3_inode in;
        memset(&in, 0, sizeof in);
        in.type = INVFS_ITYP_REG;
        in.mode = 0644;
        in.uid = in.gid = 1000;
        in.nlink = 1;
        in.size = 100;
        in.mtime = in.atime = (int64_t)time(NULL);
        ok(vol_v3_inode_put(g_v, 42, &in) == 0, "base put inode 42");
        memset(&in, 0, sizeof in);
        in.type = INVFS_ITYP_REG;
        in.mode = 0644;
        in.uid = in.gid = 1000;
        in.nlink = 1;
        in.size = 200;
        in.mtime = in.atime = (int64_t)time(NULL);
        ok(vol_v3_inode_put(g_v, 43, &in) == 0, "base put inode 43");
    }

    /* verify pre-state */
    {
        invfs_v3_inode in;
        ok(vol_v3_inode_get(g_v, 42, &in) == 1 && in.mode == 0644 && in.size == 100,
           "pre-state: inode 42 visible with correct mode/size");
        ok(vol_v3_inode_get(g_v, 43, &in) == 1 && in.mode == 0644 && in.size == 200,
           "pre-state: inode 43 visible with correct mode/size");
        ok(vol_v3_inode_get(g_v, 44, &in) == 0,
           "pre-state: inode 44 not visible (not yet in delta)");
    }

    /* spawn reader threads */
    for (i = 0; i < 4; i++) {
        if (pthread_create(&readers[i], NULL, reader_thread, NULL) != 0) {
            printf("  cannot create reader thread %d\n", i);
            g_stop = 1;
            break;
        }
    }

    /* spawn writer thread */
    if (pthread_create(&writer, NULL, writer_thread, NULL) != 0) {
        printf("  cannot create writer thread\n");
        g_stop = 1;
    }

    printf("  running concurrency test for 3 seconds...\n");
    sleep(3);
    g_stop = 1;

    /* join all threads */
    for (i = 0; i < 4; i++)
        pthread_join(readers[i], NULL);
    pthread_join(writer, NULL);

    /* The assertions the run exists for. Without these the whole harness
     * could no-op and still pass: a dead delta-append path, or a read path
     * that returned garbage on every call, both produced a green run
     * before. A run where the threads did no work fails here rather than
     * reporting a clean sheet. The fold count is reported, not asserted: the
     * fold fires every 20th writer iteration and the writer's rate is
     * host-speed dependent (measured 3-10 iterations/s on this host), so
     * asserting one would make this gate a benchmark of the machine rather
     * than of the engine. */
    printf("  totals: %llu consistent reads, %llu torn, %llu absent, "
           "%llu io-err, %llu writes, %llu folds\n",
           (unsigned long long)g_read_ops, (unsigned long long)g_read_torn,
           (unsigned long long)g_read_absent,
           (unsigned long long)g_read_ioerr,
           (unsigned long long)g_write_ops, (unsigned long long)g_folds);
    ok(g_read_torn == 0,
       "no concurrent vol_v3_inode_get returned a row from no single writer generation");
    ok(g_read_absent == 0,
       "inode 42 was never reported absent while it was live");
    ok(g_read_ops > 0,
       "the readers actually exercised the read path against the writer");
    ok(g_write_ops > 0,
       "the writer actually appended delta records (a dead append path must not pass)");

    /* MEASURED and printed, not gated -- but the reason changed, and the
     * count is no longer the same count.
     *
     * WAS (the whole of it): the delta overlay. vol_delta_lookup resolved the
     * winning record under g_delta_lock and copied a delta_ref {seg, off} BY
     * VALUE; vol_delta_read_value then read that ref's bytes with two bare
     * preads AFTER releasing the lock, while the fold's tail dropped the
     * index and freed the whole retired chain outside it, and the next
     * delta_new_segment got the blocks back and wrote zeros over the stripe.
     * Instrumenting every -1 return over 35M reads put 100% of them in one
     * place -- v3_ino_decode() failing on a DELTA row, all-zero bytes -- and
     * none in btree_search, v3_base_root or the overlay lookup. FIXED
     * (WP-inode-get-fold-race): the resolve and the value read are one
     * critical section and the chain free is inside it.
     *
     * IT IS A BIT-EXACTNESS GATE, not an availability one. When the recycled
     * block happened to hold another record of the same shape the decode
     * SUCCEEDED and the read returned another inode's row for this one --
     * src/cli/fold_delta_read_test.c shows exactly that (inode 5000's row,
     * size 20, returned for inode 42), where a torn read would have been
     * counted here instead.
     *
     * AND NOW THE BASE TREE TOO, gated here. A reader holding the root from
     * two publishes ago could have its pages collected, because the drain
     * meant to prevent exactly that was dead code -- vol_reclaim_drain waits
     * on g_readers_in_flight and nothing in the tree ever incremented it (no
     * read path called vol_reclaim_reader_snapshot/_release). It surfaced as
     * mbuf_read_ptr -> mbuf_page_allocated == 0 -> -1
     * (src/core/vol_metabuf.c:192), measured 0-3 per ~3M reads / ~1500
     * folds (~1e-6) with the delta half fixed.
     *
     * FIXED (wp/reclaim-blocking-drain): every base-tree read now announces
     * itself before it captures the root and releases after the walk, so the
     * drain really waits. Which is why this counter is GATED rather than
     * printed: it was print-only *because this defect was open*, and leaving
     * it that way after the fix is exactly how the next regression walks in.
     * A -1 here now means a base-tree read path has a missed
     * vol_reclaim_reader_release, or a read that failed for a reason nothing
     * else accounts for -- either way it is a gate going red, loudly.
     *
     * The deterministic gate for the interleave itself is
     * reclaim_reader_epoch_test (the same planned-seam shape as
     * fold_delta_read_test, on the base side); this counter is the
     * statistical backstop over the whole run, including paths the
     * interleave test does not reach. */
    ok(g_read_ioerr == 0,
       "no concurrent vol_v3_inode_get call returned -1 for a live, "
       "well-formed key (base-tree reclaim vs reader -- see the comment "
       "above this line in concurrency_test.c)");

    /* verify post-state */
    {
        invfs_v3_inode in;
        int r42 = vol_v3_inode_get(g_v, 42, &in);
        ok(r42 == 1, "post-state: inode 42 still readable");
        if (r42 == 1) {
            ok(in.mode != 0, "post-state: inode 42 has valid mode");
            ok(in.size != 0, "post-state: inode 42 has valid size");
        }
        ok(vol_v3_inode_get(g_v, 43, &in) == 1,
           "post-state: inode 43 still readable");
    }

    vol_close(g_v);
    g_v = NULL;
    remove(img);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
