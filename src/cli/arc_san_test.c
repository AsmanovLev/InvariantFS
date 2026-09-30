/*
 * arc_san_test.c — the RED CONTROL for the ARC concurrency defect.
 *
 * WHAT THIS IS
 * ------------
 * Two different things are wrong with the content cache when two threads are
 * in it, and they have different red controls:
 *
 *   1. STRUCTURE. arc.c held no lock, so ht_find/ht_insert/lst_unlink/
 *      lst_push_mru/move_to and every counter in a->st were plain racy
 *      writes. ThreadSanitizer names them at the exact arc.c line.
 *   2. LIFETIME. arc_get handed out a BORROWED pointer "valid until the next
 *      arc_put" (arc.h), and the read path memcpy'd out of it
 *      (vol_read.c:1966). Another thread's arc_replace does
 *      free(victim->data) at arc.c:226/:230 -- so the reader copies out of
 *      freed heap and STILL RETURNS SUCCESS. AddressSanitizer names it, and
 *      because it is the *bytes* that are wrong, this is the bit-exactness
 *      failure, not just a crash.
 *
 * WHY THIS IS NOT A PROBABILITY TEST
 * ---------------------------------
 * A race test that relies on catching a lucky interleaving proves nothing: it
 * passes on the broken tree often enough to be worthless. Neither leg here
 * does that.
 *
 *   - TSAN leg: ARC had NO synchronization at all, so any two concurrent
 *     arc_get/arc_put calls are conflicting accesses with no happens-before
 *     edge BY CONSTRUCTION. TSAN does not need the accesses to overlap in
 *     time; it needs them to be unordered, and unorderedness here was
 *     guaranteed. It reports on the first run, every run.
 *   - ASAN leg: the free is not raced for, it is PLANNED. Thread A opens a
 *     window, a pthread_barrier separates it from thread B's eviction, and A
 *     only reads the bytes after B has returned. The eviction is forced by a
 *     budget sized so the borrowed entry is necessarily the victim: arc_get
 *     promotes it to T2, and arc_replace's tie-break then takes the T2 tail.
 *     No timing, no luck -- the interleaving IS the test.
 *
 * WHY IT COMPILES AGAINST THE UNFIXED TREE
 * ----------------------------------------
 * The fix changes the API the read path must use (arc_get_copy). A test that
 * only compiles against the FIXED tree cannot be a red control, so arc_get_copy
 * is declared WEAK here: on a tree without it the symbol resolves to NULL and
 * the test falls back to exactly the pre-fix borrow. Same source, same
 * command, red before and green after -- which is the only thing that makes
 * "it fails on the unfixed tree" mean anything.
 *
 * Built twice from this one file (see the Makefile):
 *   -fsanitize=thread   -> the structure leg
 *   -fsanitize=address  -> the lifetime leg
 * It links src/core/arc.c ALONE -- arc.c has no dependency outside libc, so a
 * sanitizer build of it costs about a second and needs no volume at all.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "arc.h"

/* Weak on purpose: see the header comment. NULL on a tree that predates the
 * fix, and the test then exercises the pre-fix borrow -- which is the bug. */
extern int arc_get_copy(invfs_arc *a, uint64_t key, size_t off, size_t want,
                        uint8_t *dst, size_t *got) __attribute__((weak));

#define PSIZE    8192
#define NTHREADS 6
#define ITERS    2000
#define FILL     0xC7
#define KEYS     64

static int failures;

/* A window of an entry, opened but NOT yet read.
 *
 * The two-step shape is the point. `open` and `take` are separated by the
 * barrier in the lifetime leg precisely so that on the pre-fix tree the bytes
 * are still inside the cache's own buffer when the eviction frees it. On the
 * fixed tree the copy already happened, inside the cache lock, at `open`. */
typedef struct {
    const uint8_t *borrowed;   /* pre-fix: straight out of the cache */
    uint8_t       *own;        /* post-fix: arc_get_copy's destination */
    size_t         len;
} arc_window;

static int window_open(invfs_arc *a, uint64_t key, size_t off, size_t want,
                       arc_window *w)
{
    w->borrowed = NULL; w->own = NULL; w->len = 0;
    if (arc_get_copy) {
        w->own = (uint8_t *)malloc(want ? want : 1);
        if (!w->own) return 0;
        if (!arc_get_copy(a, key, off, want, w->own, &w->len)) {
            free(w->own); w->own = NULL; w->len = 0;
            return 0;
        }
        return 1;
    }
    {
        /* the pre-fix path, from vol_read.c:1960-1966 */
        const uint8_t *d;
        size_t n;
        if (!arc_get(a, key, &d, &n)) return 0;
        if (off >= n) return 0;
        w->borrowed = d + off;
        w->len = n - off;
        if (w->len > want) w->len = want;
        return 1;
    }
}

static size_t window_take(const arc_window *w, uint8_t *dst)
{
    if (!w->borrowed && !w->own) return 0;
    memcpy(dst, w->own ? w->own : w->borrowed, w->len);
    return w->len;
}

static void window_close(arc_window *w) { free(w->own); w->own = NULL; }

static void ok(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

static uint8_t *fill(size_t n)
{
    uint8_t *b = (uint8_t *)malloc(n ? n : 1);
    if (b) memset(b, FILL, n);
    return b;
}

/* ------------------------------------------------------------------ */
/* leg 1: structure. Many threads, one cache, the shape of the        */
/* whole-file content cache at vol_read.c:1981-1997: try the cache,  */
/* and on a miss decode and insert.                                    */
/* ------------------------------------------------------------------ */

struct racer {
    invfs_arc *a;
    long       id;
    int        short_reads;
    int        bad_bytes;
};

static void *racer(void *argp)
{
    struct racer *r = (struct racer *)argp;
    uint8_t *scratch = (uint8_t *)malloc(PSIZE);
    int i;
    if (!scratch) return NULL;
    for (i = 0; i < ITERS; i++) {
        uint64_t key = 1 + (uint64_t)((r->id * 7 + i) % KEYS);
        arc_window w;
        if (window_open(r->a, key, 0, PSIZE, &w)) {
            size_t got = window_take(&w, scratch);
            size_t j;
            window_close(&w);
            if (got != PSIZE) r->short_reads++;
            for (j = 0; j < got; j++)
                if (scratch[j] != FILL) { r->bad_bytes++; break; }
        } else {
            uint8_t *fresh = fill(PSIZE);
            if (fresh) arc_put(r->a, key, fresh, PSIZE);
        }
    }
    free(scratch);
    return NULL;
}

static void leg_structure(void)
{
    invfs_arc *a;
    pthread_t th[NTHREADS];
    struct racer racers[NTHREADS];
    invfs_arc_stats st;
    long i;

    printf("== structure leg: %d threads x %d ops on one cache ==\n",
           NTHREADS, ITERS);
    fflush(stdout);
    /* a budget that holds a handful of the 64 keys, so the workload is
       mostly inserts and evictions -- the widest possible window on
       arc_replace, ht_insert and the list bookkeeping */
    a = arc_create(16 * PSIZE);
    if (!a) { ok(0, "arc_create"); return; }

    for (i = 0; i < NTHREADS; i++) {
        racers[i].a = a; racers[i].id = i;
        racers[i].short_reads = racers[i].bad_bytes = 0;
        pthread_create(&th[i], NULL, racer, &racers[i]);
    }
    for (i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);

    arc_stats(a, &st);
    printf("   entries=%u ghosts=%u bytes=%zu budget=%zu hits=%llu misses=%llu\n",
           st.entries, st.ghosts, st.bytes, st.budget,
           (unsigned long long)st.hits, (unsigned long long)st.misses);
    for (i = 0; i < NTHREADS; i++) {
        racers[i].short_reads += racers[i].bad_bytes ? 1000 : 0;
        ok(racers[i].short_reads == 0 && racers[i].bad_bytes == 0,
            "every reader saw a whole, correct window");
    }
    /* the ARC byte invariants (arc.c:10-12). These are exactly the numbers a
       double lst_unlink destroys: l->bytes is a size_t, so one extra
       subtraction does not go negative, it goes to ~2^64. */
    ok(st.bytes <= st.budget, "|T1|+|T2| <= budget holds after the storm");
    arc_destroy(a);
}

/* ------------------------------------------------------------------ */
/* leg 2: lifetime. The planned eviction, not a raced one.             */
/* ------------------------------------------------------------------ */

static invfs_arc        *g_a;
static pthread_barrier_t g_gate;
static uint64_t          g_victim = 1;
static arc_window        g_win;
static size_t            g_want;
static uint8_t           g_scratch[PSIZE];

static void *borrower(void *unused)
{
    (void)unused;
    /* g_victim is already live, so this is a hit. The bytes are NOT read
       here: the window is held open across both barriers. */
    window_open(g_a, g_victim, 0, g_want, &g_win);
    pthread_barrier_wait(&g_gate);      /* <- A holds the window open */
    pthread_barrier_wait(&g_gate);      /* <- B has evicted it */
    return NULL;                        /* A reads it only after this */
}

static void leg_lifetime(void)
{
    pthread_t th;
    uint8_t *b;
    size_t i, got;
    int bad = 0;

    printf("== lifetime leg: planned eviction of a borrowed window ==\n");
    printf("   arc_get_copy present: %s\n", arc_get_copy ? "yes" : "no");
    fflush(stdout);

    /* Budget for exactly two entries. g_victim goes in first; the reader's
       lookup promotes it to T2; the third insert then has to take the T2
       tail, which is g_victim. Forced by arc_replace's arithmetic, not by
       timing. */
    g_a = arc_create(2 * PSIZE + 16);
    if (!g_a) { ok(0, "arc_create"); return; }
    g_want = PSIZE;

    b = fill(PSIZE); arc_put(g_a, g_victim, b, PSIZE);
    b = fill(PSIZE); arc_put(g_a, 2, b, PSIZE);

    pthread_barrier_init(&g_gate, NULL, 2);
    pthread_create(&th, NULL, borrower, NULL);
    pthread_barrier_wait(&g_gate);      /* reader has the window open */
    b = fill(PSIZE); arc_put(g_a, 3, b, PSIZE);   /* <- the eviction */
    pthread_barrier_wait(&g_gate);
    pthread_join(th, NULL);
    pthread_barrier_destroy(&g_gate);

    got = window_take(&g_win, g_scratch);
    ok(got == g_want, "the reader got its whole window");
    for (i = 0; i < got; i++)
        if (g_scratch[i] != FILL) { bad = 1; break; }
    ok(!bad, "the window is still the bytes that were cached");

    window_close(&g_win);
    arc_destroy(g_a);
}

int main(void)
{
    /* INVFS_ARC_SAN_LEG = structure | lifetime | both (default both).
       The two failures are independent, and on the unfixed tree the
       structure one is the louder: two threads evicting the same node is a
       DOUBLE-FREE, which aborts before the lifetime leg gets its turn. Run
       them separately to see each. */
    const char *leg = getenv("INVFS_ARC_SAN_LEG");
    int do_struct = 1, do_life = 1;
    if (leg && *leg) {
        do_struct = strcmp(leg, "lifetime") != 0;
        do_life   = strcmp(leg, "structure") != 0;
    }
    if (do_struct) leg_structure();
    if (do_life) leg_lifetime();
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
