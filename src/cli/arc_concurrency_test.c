/*
 * arc_concurrency_test.c — the read path and the content cache, under N
 * concurrent readers, on a live volume.
 *
 * WHY THIS TEST EXISTS
 * --------------------
 * arc.h used to claim the cache needed no locking because "FUSE holds
 * g_io_lock around every vol_* call". It does not. invf_read releases
 * g_io_lock and THEN calls vol_read_range (fuse_fs.c:1407, :1414, under a
 * comment saying so), and the daemon runs fuse_loop_mt (fuse_fs.c:3353).
 * vol_read_range is where the content cache is used:
 *
 *   - the WHOLE-FILE cache, taken by every container and every transcoded
 *     file (vol_read.c:1989 after this WP);
 *   - the shared-batch cache under pba|TZ_ARC_TAG, taken by every text and
 *     binary batch member (vol_read.c:194 after this WP).
 *
 * So the real caller of the parallel decode fan-out was never the thing to
 * worry about -- decode_thread_worker touches no arc_* at all, which is what
 * src/cli/read_parallel_bitexact_test.c covers. It is invf_read. And two
 * things were live here: ARC's hash chains, lists and byte accounting were
 * plain racy writes, and arc_get handed out a BORROWED pointer that a
 * concurrent arc_put could free underneath the reader's memcpy -- returning
 * SUCCESS with whatever the freed heap held.
 *
 * WHAT IS AND IS NOT COVERED HERE, STATED PLAINLY
 * ----------------------------------------------
 * The DETERMINISTIC red control is src/cli/arc_san_test.c: TSAN for the
 * structure (arc.c had no lock at all, so any two concurrent calls are
 * unordered by construction) and ASan for the borrow (the eviction is PLANNED
 * with a barrier, not raced for). That file is what fails on a broken arc.c.
 *
 * This file is the end-to-end guard, and it does two things that harness
 * cannot:
 *
 *   leg 1 -- N threads reading windows of a real multi-segment file at the
 *            same time, each read diffed against the bytes that were written.
 *            This is the bit-exactness property under the exact concurrency
 *            FUSE creates, and each read also fans out across WP94's own
 *            decode workers INSIDE it.
 *   leg 2 -- N threads driving v->arc with the exact sequence vol_read.c
 *            uses (arc_get_copy; on a miss, reconstruct and arc_put), with a
 *            budget far below the working set so eviction is guaranteed, then
 *            the ARC's own byte invariants re-checked. A double lst_unlink
 *            does not make those go negative -- l->bytes is a size_t, so it
 *            goes to ~2^64.
 *
 * Leg 2 reaches the cache DIRECTLY rather than through vol_read_range, and
 * that is a deliberate limitation rather than a shortcut. On a stock build
 * the whole-file branch of vol_read_range needs an EXTERNAL codecpack to be
 * reachable at all: a container (num_children > 0) only exists on v2
 * (vol_sweep.c:1021 records that the builtin ZIP lane is v2-only), and a
 * single-block whole-file algo is JXL/PNGR/APE/PMP, every one of which is a
 * helper this unit does not have. The batch branch needs a sweep to have
 * batched anything first. So a unit test cannot drive either branch to a hit,
 * and one that pretended to would be asserting nothing. The ARC itself is
 * covered exhaustively and deterministically by arc_san_test.c; this leg pins
 * that the object the volume actually owns survives the concurrency the read
 * path creates.
 *
 * Knobs: INVFS_ARC_CTHREADS (readers, default 6)
 *         INVFS_ARC_CREPS    (rounds per reader, default 6)
 *         INVFS_ARC_CSIZE    (bytes, default 8 MiB)
 *         INVFS_ARC_CBUDGET  (cache budget in bytes, default 1 MiB)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "invarifs.h"
#include "volume_internal.h"

#define CHUNK 65536
#define ARCP  4096

static invfs_volume *g_v;
static uint64_t      g_fid;
static uint8_t      *g_ref;        /* the bytes that were written */
static size_t        g_fsize;
static int           g_rounds;
static pthread_barrier_t g_start;

static int      g_fails;
static uint64_t g_arc_badbytes, g_arc_sizefail;

static void ok(int cond, const char *what)
{
    printf("  %s %s\n", cond ? "OK  " : "FAIL", what);
    if (!cond) g_fails++;
}

static long env_num(const char *k, long dflt)
{
    const char *e = getenv(k);
    char *end;
    long v;
    if (!e || !*e) return dflt;
    v = strtol(e, &end, 0);
    return (end == e || v <= 0) ? dflt : v;
}

/* ---- leg 1: N threads reading windows of one file with no lock between
 * them. This is invf_read's shape: a 64 KiB window at a time, at a different
 * offset per thread, so they are not all reading the same bytes. ---- */

static void *window_reader(void *argp)
{
    long me = (long)argp;
    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    int r;
    if (!buf) return NULL;
    pthread_barrier_wait(&g_start);
    for (r = 0; r < g_rounds; r++) {
        uint64_t off = (uint64_t)((((r * 7 + (int)me * 13) %
                                    (int)(g_fsize / CHUNK)) * (int)CHUNK));
        size_t want = g_fsize - (size_t)off;
        int got;
        if (want > CHUNK) want = CHUNK;
        got = vol_read_range(g_v, g_fid, off, want, buf);
        if (got < 0) {
            printf("  FAIL  reader %ld round %d: vol_read_range errored at "
                   "off %llu\n", me, r, (unsigned long long)off);
            __sync_fetch_and_add(&g_fails, 1);
            continue;
        }
        if ((size_t)got != want) {
            printf("  FAIL  reader %ld round %d: got %d bytes, want %zu at "
                   "off %llu\n", me, r, got, want, (unsigned long long)off);
            __sync_fetch_and_add(&g_fails, 1);
            continue;
        }
        if (memcmp(buf, g_ref + off, want) != 0) {
            size_t i;
            for (i = 0; i < want; i++)
                if (buf[i] != g_ref[off + i]) break;
            printf("  FAIL  reader %ld round %d: NOT bit-exact at off %llu; "
                   "first wrong byte +%zu (got 0x%02x want 0x%02x)\n",
                   me, r, (unsigned long long)off, i,
                   buf[i] & 0xff, g_ref[off + i] & 0xff);
            __sync_fetch_and_add(&g_fails, 1);
        }
    }
    free(buf);
    return NULL;
}

/* ---- leg 2: N threads driving the volume's own cache, the way vol_read.c
 * does it: look first, and on a miss reconstruct and insert. The payload is
 * derived from the key, so a window that comes back carrying another key's
 * bytes is a wrong answer and not merely a stale one. ---- */

static void fill_payload(uint64_t key, uint8_t *dst, size_t n)
{
    size_t i;
    uint64_t s = key * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    for (i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        dst[i] = (uint8_t)(s >> 24);
    }
}

static void *arc_worker(void *argp)
{
    long me = (long)argp;
    uint8_t *win   = (uint8_t *)malloc(ARCP);
    uint8_t *fresh = (uint8_t *)malloc(ARCP);
    uint8_t *want  = (uint8_t *)malloc(ARCP);
    int r;
    if (!win || !fresh || !want) goto done;
    pthread_barrier_wait(&g_start);
    for (r = 0; r < g_rounds * 8; r++) {
        uint64_t key = 1000 + (uint64_t)(((int)me * 11 + r * 5) % 48);
        size_t got = 0;
        if (arc_get_copy(g_v->arc, key, 0, ARCP, win, &got)) {
            fill_payload(key, want, ARCP);
            if (got != ARCP || memcmp(win, want, ARCP) != 0)
                __sync_fetch_and_add(&g_arc_badbytes, 1);
            if (got != ARCP) __sync_fetch_and_add(&g_arc_sizefail, 1);
        } else {
            fill_payload(key, fresh, ARCP);
            arc_put(g_v->arc, key, fresh, ARCP);   /* takes ownership */
            fresh = (uint8_t *)malloc(ARCP);      /* ours again */
            if (!fresh) goto done;
        }
    }
done:
    free(win);
    free(fresh);
    free(want);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[256], b[64], cmd[600];
    invfs_volume *v;
    int err = 0;
    int nthreads = (int)env_num("INVFS_ARC_CTHREADS", 6);
    size_t fsize  = (size_t)env_num("INVFS_ARC_CSIZE", 8 * 1024 * 1024);
    size_t budget = (size_t)env_num("INVFS_ARC_CBUDGET", 64 * 1024);
    invfs_arc_stats st;
    long i;

    g_rounds = (int)env_num("INVFS_ARC_CREPS", 6);
    if (nthreads > 32) nthreads = 32;
    if (fsize < CHUNK * 32) fsize = CHUNK * 32;

    printf("arc_concurrency_test: %d threads x %d rounds, %zu B file, "
           "%zu B cache budget\n", nthreads, g_rounds, fsize, budget);

    snprintf(img, sizeof img, "%s/invf-arc-conc.img", dir);
    unlink(img);
    snprintf(b, sizeof b, "%zu", budget);
    setenv("INVFS_ARC_BYTES", b, 1);
    snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s >/dev/null 2>&1", img);
    if (system(cmd) != 0) { printf("  FAIL  mkfs failed\n"); return 1; }

    v = vol_open(img, &err);
    if (!v) { printf("  FAIL  vol_open (%d)\n", err); return 1; }
    g_v = v;
    if (!v->arc) {
        printf("  FAIL  content cache is off (INVFS_ARC_BYTES=%s) -- this "
               "test would prove nothing\n", b);
        vol_close(v);
        return 1;
    }

    /* A multi-segment file: enough entries to reach WP94's fan-out inside
       every one of these reads, so leg 1 covers the nested concurrency too. */
    g_ref = (uint8_t *)malloc(fsize);
    if (!g_ref) { printf("  FAIL  oom\n"); vol_close(v); return 1; }
    {
        size_t seg, k;
        uint64_t s = 0x243F6A8885A308D3ULL;
        for (seg = 0; seg * SEGMENT_SIZE < fsize; seg++) {
            for (k = 0; k < SEGMENT_SIZE && seg * SEGMENT_SIZE + k < fsize; k++) {
                s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                /* compressible, so segments take the LZ4 branch of the decode
                   contract, with an occasional incompressible island so the
                   file is not uniformly one shape */
                g_ref[seg * SEGMENT_SIZE + k] =
                    (k % 997 == 3) ? (uint8_t)(s >> 24)
                                   : (uint8_t)('a' + (s % 26));
            }
        }
    }
    g_fsize = fsize;
    g_fid = vol_write_bulk(v, "arc.bin", g_ref, fsize, NULL);
    if (!g_fid) { printf("  FAIL  write\n"); vol_close(v); return 1; }
    vol_flush(v);
    printf("  ..    inode %llu, %zu bytes, %zu segments\n",
           (unsigned long long)g_fid, fsize, fsize / SEGMENT_SIZE);

    /* leg 1 */
    pthread_barrier_init(&g_start, NULL, (unsigned)nthreads + 1);
    {
        pthread_t th[32];
        for (i = 0; i < nthreads; i++)
            pthread_create(&th[i], NULL, window_reader, (void *)i);
        pthread_barrier_wait(&g_start);
        for (i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    }
    ok(g_fails == 0, "leg 1: concurrent windowed reads are bit-exact");

    /* leg 2 -- the cache itself, on the same live volume.
     * Hits need two threads' references to overlap in time; on a loaded
     * runner (parallel shards) one thread can lap the rest and the whole
     * storm misses -- a scheduling outcome, not a cache defect. So the
     * storm repeats, bounded, until it hits: a cache that genuinely never
     * hits still fails after the last attempt, while the byte-correctness
     * counters below stay cumulative across attempts, so corruption in any
     * storm is still caught. */
    {
        int attempt;
        for (attempt = 0; attempt < 4; attempt++) {
            pthread_barrier_init(&g_start, NULL, (unsigned)nthreads + 1);
            {
                pthread_t th[32];
                for (i = 0; i < nthreads; i++)
                    pthread_create(&th[i], NULL, arc_worker, (void *)i);
                pthread_barrier_wait(&g_start);
                for (i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
            }
            pthread_barrier_destroy(&g_start);
            arc_stats(v->arc, &st);
            if (st.hits > 0)
                break;
            printf("  ..    storm %d: zero hits under load, repeating (bounded)\n",
                   attempt);
        }
    }
    printf("  ..    cache: entries=%u ghosts=%u bytes=%zu budget=%zu "
           "hits=%llu misses=%llu evictions=%llu\n",
           st.entries, st.ghosts, st.bytes, st.budget,
           (unsigned long long)st.hits, (unsigned long long)st.misses,
           (unsigned long long)st.evictions);
    /* counted by the workers themselves, so a cache that handed one key's
       bytes to another, or a short entry, is caught here and not only in the
       accounting */
    ok(g_arc_badbytes == 0, "leg 2: every hit returned its OWN key's bytes");
    ok(g_arc_sizefail == 0, "leg 2: no hit came back short");
    ok(st.bytes <= st.budget, "|T1|+|T2| <= budget after the storm");
    ok(st.evictions > 0, "leg 2 actually evicted (else it proved nothing)");
    ok(st.hits > 0,      "leg 2 actually hit (else it proved nothing)");

    vol_close(v);
    unlink(img);
    free(g_ref);
    printf("%s\n", g_fails ? "arc_concurrency_test: FAILED" :
                            "arc_concurrency_test: PASS");
    return g_fails ? 1 : 0;
}
