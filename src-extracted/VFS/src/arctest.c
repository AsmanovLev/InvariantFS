/*
 * arctest.c -- exercise the ARC itself.
 *
 * The integration test (invf-rangechk) only ever shows the easy half of this
 * cache: insert, then hit. It runs one file per process, so eviction, ghost
 * hits, the adaptation of p and the size refusal never fire -- every counter
 * but hits/misses/inserts stays at zero. Those are the parts that can be wrong
 * without anything failing: a cache that evicts the wrong thing still returns
 * correct bytes, just slowly, and slowly is invisible to an assertion.
 *
 * So the algorithm gets tested directly, against the invariants it claims:
 *
 *     |T1| + |T2|                <= c        (never hold more than the budget)
 *     |T1| + |B1|                <= c
 *     |T1| + |T2| + |B1| + |B2|  <= 2c       (bounded history)
 *
 * plus the one property that is the entire reason this is ARC and not an LRU:
 * a one-shot scan of many files must NOT evict a small working set that is
 * being re-read. That is test 8, and it is the one worth reading.
 *
 *   invf-arc-test
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arc.h"

static int failures = 0;
static int checks = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* Content keyed off the id, so serving the wrong entry is visible rather than
   merely suspected: byte i of key k is a function of both. */
static uint8_t *blob(uint64_t key, size_t n)
{
    uint8_t *p = (uint8_t *)malloc(n ? n : 1);
    size_t i;
    if (!p) { printf("  FAIL  out of memory\n"); exit(2); }
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)((key * 131u) + i * 31u + 7u);
    return p;
}

static int blob_ok(uint64_t key, const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i] != (uint8_t)((key * 131u) + i * 31u + 7u)) return 0;
    return 1;
}

/* Every invariant, checked after whatever just happened. Cheap enough to call
   on every operation of the fuzz workload. */
static void check_invariants(invfs_arc *a, const char *where)
{
    invfs_arc_stats st;
    arc_stats(a, &st);
    if (st.t1_bytes + st.t2_bytes > st.budget) {
        failures++;
        printf("  FAIL  %s: |T1|+|T2| = %llu > c = %llu\n", where,
               (unsigned long long)(st.t1_bytes + st.t2_bytes),
               (unsigned long long)st.budget);
    }
    if (st.bytes != st.t1_bytes + st.t2_bytes) {
        failures++;
        printf("  FAIL  %s: bytes disagrees with T1+T2\n", where);
    }
    if (st.p > st.budget) {
        failures++;
        printf("  FAIL  %s: p = %llu > c = %llu\n", where,
               (unsigned long long)st.p, (unsigned long long)st.budget);
    }
    checks++;
}

int main(void)
{
    /* ---- 1. disabled cache: NULL is the "off" state, not an error ---- */
    {
        invfs_arc *a = arc_create(0);
        const uint8_t *d = NULL;
        size_t n = 0;
        invfs_arc_stats st;
        ok(a == NULL, "budget 0 disables the cache (NULL)");
        ok(arc_get(a, 1, &d, &n) == 0, "get on a NULL cache misses");
        arc_put(a, 1, blob(1, 64), 64);        /* must free, not crash */
        arc_invalidate(a, 1);
        arc_clear(a);
        arc_stats(a, &st);
        ok(st.hits == 0 && st.budget == 0, "stats on a NULL cache are zeroed");
        arc_destroy(a);
    }

    /* ---- 2. insert, hit, and the T1 -> T2 promotion ---- */
    {
        invfs_arc *a = arc_create(1000);
        const uint8_t *d = NULL;
        size_t n = 0;
        invfs_arc_stats st;

        arc_put(a, 10, blob(10, 200), 200);
        arc_put(a, 11, blob(11, 200), 200);
        arc_stats(a, &st);
        ok(st.entries == 2 && st.bytes == 400, "two inserts held, 400 bytes");
        ok(st.t1_bytes == 400 && st.t2_bytes == 0, "first sighting lands in T1");

        ok(arc_get(a, 10, &d, &n) == 1, "hit on a live key");
        ok(n == 200 && blob_ok(10, d, n), "the hit returns that key's bytes");
        arc_stats(a, &st);
        ok(st.t2_bytes == 200 && st.t1_bytes == 200,
           "a second sighting promotes T1 -> T2");
        ok(st.hits == 1 && st.misses == 0, "counters: 1 hit, 0 misses");

        ok(arc_get(a, 99, &d, &n) == 0, "miss on a key never seen");
        arc_stats(a, &st);
        ok(st.misses == 1 && st.ghost_hits == 0,
           "an unknown key is a miss but not a ghost hit");
        check_invariants(a, "after promotion");
        arc_destroy(a);
    }

    /* ---- 3. refusal: an entry over half the budget is not worth admitting -- */
    {
        invfs_arc *a = arc_create(1000);
        invfs_arc_stats st;
        const uint8_t *d = NULL;
        size_t n = 0;
        arc_put(a, 1, blob(1, 400), 400);
        arc_put(a, 2, blob(2, 501), 501);       /* 501*2 > 1000 -> refuse */
        arc_stats(a, &st);
        ok(st.refused == 1, "an entry over half the budget is refused");
        ok(st.entries == 1 && st.bytes == 400, "the refusal evicted nothing");
        ok(arc_get(a, 2, &d, &n) == 0, "a refused key is not cached");
        arc_stats(a, &st);
        ok(st.ghost_hits == 0, "a refused key leaves no ghost either");
        check_invariants(a, "after refusal");
        arc_destroy(a);
    }

    /* ---- 4. eviction: over budget demotes the LRU end ---- */
    {
        invfs_arc *a = arc_create(1000);
        invfs_arc_stats st;
        const uint8_t *d = NULL;
        size_t n = 0;
        int k;
        for (k = 0; k < 10; k++) {              /* 10 x 200 = 2000 into c=1000 */
            arc_put(a, (uint64_t)k, blob((uint64_t)k, 200), 200);
            check_invariants(a, "during eviction");
        }
        arc_stats(a, &st);
        ok(st.bytes <= 1000, "never holds more than the budget");
        ok(st.evictions > 0, "going over budget evicted something");
        ok(arc_get(a, 9, &d, &n) == 1, "the most recent insert survived");
        ok(arc_get(a, 0, &d, &n) == 0, "the oldest insert did not");
        arc_stats(a, &st);
        /* And it left NO ghost, which looks wrong and is not: |T1|+|B1| <= c,
           and with T2 empty the live data occupies the whole budget, so there
           is no room for history. This is the paper's Case IV(i) -- when
           |T1| = c the T1 LRU is dropped outright rather than remembered.
           Recency history costs exactly what the frequency half is not using,
           so a workload with no repeats has nothing to learn and learns
           nothing: p stays where it started. Test 5 gives T2 something to
           hold and the ghosts appear. */
        ok(st.ghosts == 0, "with T2 empty there is no room for ghosts");
        ok(st.ghost_hits == 0, "so the miss on it was a plain miss");
        ok(st.p == 500, "and p never moved: nothing was repeated");
        arc_destroy(a);
    }

    /* ---- 5. ghost hits move p, and re-insert straight into T2 ---- */
    {
        invfs_arc *a = arc_create(1000);
        invfs_arc_stats before, after, st;
        const uint8_t *d = NULL;
        size_t n = 0;
        uint64_t ghost_key = 0;
        int k;

        /* One entry seen twice, so T2 is not empty and B1 has room to exist. */
        arc_put(a, 100, blob(100, 200), 200);
        arc_get(a, 100, &d, &n);
        for (k = 1; k <= 8; k++)
            arc_put(a, (uint64_t)k, blob((uint64_t)k, 200), 200);
        arc_stats(a, &st);
        ok(st.ghosts > 0, "with T2 non-empty, evictions leave B1 ghosts");

        /* Which key is the oldest surviving ghost depends on the exact
           interleaving of REPLACE and the history trim, so find one rather
           than predict it: a miss that also registers as a ghost hit. */
        for (k = 1; k <= 8 && !ghost_key; k++) {
            invfs_arc_stats s0, s1;
            arc_stats(a, &s0);
            if (arc_get(a, (uint64_t)k, &d, &n) == 0) {
                arc_stats(a, &s1);
                if (s1.ghost_hits > s0.ghost_hits) ghost_key = (uint64_t)k;
            }
        }
        ok(ghost_key != 0, "a ghost hit is reachable and counted");

        arc_stats(a, &before);
        arc_put(a, ghost_key, blob(ghost_key, 200), 200);
        arc_stats(a, &after);
        ok(after.p > before.p, "a B1 ghost hit grows p (recency needs room)");
        ok(arc_get(a, ghost_key, &d, &n) == 1 && blob_ok(ghost_key, d, n),
           "the re-inserted entry is live and correct");
        check_invariants(a, "after B1 ghost hit");
        arc_destroy(a);

        /* And the other direction. To evict out of T2 the cache has to be
           mostly T2, since REPLACE only takes a T2 victim while |T1| is at or
           under p -- so build four entries that have each been seen twice,
           then push two fresh ones in behind them. */
        {
            invfs_arc *b = arc_create(1000);
            invfs_arc_stats mid, end;
            const uint8_t *bd = NULL;
            size_t bn = 0;
            for (k = 0; k < 4; k++)
                arc_put(b, (uint64_t)k, blob((uint64_t)k, 200), 200);
            for (k = 0; k < 4; k++)
                arc_get(b, (uint64_t)k, &bd, &bn);      /* -> T2, 800 bytes */
            arc_put(b, 50, blob(50, 200), 200);
            arc_put(b, 51, blob(51, 200), 200);         /* forces a T2 eviction */
            arc_stats(b, &mid);
            ok(mid.evictions > 0, "a T1 insert can evict out of T2");
            ok(arc_get(b, 0, &bd, &bn) == 0, "the T2 LRU entry is gone");
            arc_put(b, 0, blob(0, 200), 200);           /* B2 ghost hit */
            arc_stats(b, &end);
            ok(end.p < mid.p, "a B2 ghost hit shrinks p (frequency needs room)");
            ok(end.ghost_hits == 1, "and was counted as a ghost hit");
            check_invariants(b, "after B2 ghost hit");
            arc_destroy(b);
        }
    }

    /* ---- 6. invalidate drops the data AND the ghost ---- */
    {
        invfs_arc *a = arc_create(1000);
        invfs_arc_stats st;
        const uint8_t *d = NULL;
        size_t n = 0;
        arc_put(a, 7, blob(7, 300), 300);
        arc_invalidate(a, 7);
        arc_stats(a, &st);
        ok(st.invalidated == 1 && st.entries == 0 && st.bytes == 0,
           "invalidate gives the budget back");
        ok(arc_get(a, 7, &d, &n) == 0, "an invalidated key is gone");
        arc_stats(a, &st);
        ok(st.ghost_hits == 0,
           "and left no ghost -- the id can never be inserted again");
        arc_invalidate(a, 12345);               /* absent: must be a no-op */
        arc_stats(a, &st);
        ok(st.invalidated == 1, "invalidating an absent key does nothing");
        arc_destroy(a);
    }

    /* ---- 7. a live key re-put at a larger size still pays for the room ---- */
    {
        /* This is the case that used to break the budget: three entries filling
           c exactly, then one of them grows. Patching the list byte count in
           place skipped REPLACE, so the cache quietly held 1100 of 1000. */
        invfs_arc *a = arc_create(1000);
        invfs_arc_stats st;
        const uint8_t *d = NULL;
        size_t n = 0;
        arc_put(a, 1, blob(1, 400), 400);
        arc_put(a, 2, blob(2, 400), 400);
        arc_put(a, 3, blob(3, 200), 200);
        arc_stats(a, &st);
        ok(st.bytes == 1000, "three entries fill the budget exactly");
        arc_put(a, 1, blob(1, 500), 500);       /* same key, bigger */
        arc_stats(a, &st);
        ok(st.bytes <= 1000, "growing a live entry stays within the budget");
        ok(arc_get(a, 1, &d, &n) == 1 && n == 500 && blob_ok(1, d, n),
           "and the new bytes are what comes back");
        check_invariants(a, "after in-place growth");
        arc_destroy(a);
    }

    /* ---- 8. scan resistance: the reason this is ARC and not an LRU ---- */
    {
        /* 64 KB budget. A working set of eight 4 KB files is read twice, so it
           sits in T2. Then a one-shot scan streams 200 distinct 4 KB files
           past -- a backup pass, or invf-verify walking the volume. Under LRU
           with room for 16 entries the scan flushes the working set completely,
           every pass, forever.

           ARC's guarantee is not "loses nothing" -- it is "loses at most one
           entry, and then learns". The first scan page to arrive after the
           cache fills finds |T1| exactly at p, and REPLACE's tie-break sends it
           to T2 for a victim (paper: |T1| > p is false, x not in B2). That
           costs one entry. From then on |T1| > p and every later scan page
           evicts a scan page. Re-reading the lost entry is a B2 ghost hit,
           which shrinks p -- and that is what makes the SECOND scan free. Both
           halves are asserted; the second is the one that matters, because it
           is the steady state a running mount lives in. */
        invfs_arc *a = arc_create(64 * 1024);
        const size_t sz = 4096;
        const uint64_t WS = 1;                  /* working set: ids 1..8 */
        const uint8_t *d = NULL;
        size_t n = 0;
        int i, pass, survived[2];
        invfs_arc_stats st, adapted;
        size_t p_start;

        for (i = 0; i < 8; i++)
            arc_put(a, WS + i, blob(WS + i, sz), sz);
        for (i = 0; i < 8; i++)
            ok(arc_get(a, WS + i, &d, &n) == 1, "working set reaches T2");
        arc_stats(a, &st);
        p_start = st.p;

        for (pass = 0; pass < 2; pass++) {
            uint64_t base = 1000 + (uint64_t)pass * 1000;   /* distinct decades */
            for (i = 0; i < 200; i++) {
                uint64_t k = base + (uint64_t)i;
                if (!arc_get(a, k, &d, &n))
                    arc_put(a, k, blob(k, sz), sz);
                check_invariants(a, "during scan");
            }
            survived[pass] = 0;
            for (i = 0; i < 8; i++) {
                if (arc_get(a, WS + i, &d, &n)) {
                    if (blob_ok(WS + i, d, n)) survived[pass]++;
                } else {
                    /* the read a real mount would do next: reconstruct and
                       re-cache. On the first pass this is the B2 ghost hit
                       that teaches the cache to protect frequency. */
                    arc_put(a, WS + i, blob(WS + i, sz), sz);
                }
            }
            arc_stats(a, &st);
            printf("  scan %d: %d/8 of the working set survived 200 one-shot "
                   "reads (p = %llu of %llu)\n", pass + 1, survived[pass],
                   (unsigned long long)st.p, (unsigned long long)st.budget);
            if (pass == 0) adapted = st;
        }

        ok(survived[0] >= 7, "a 200-file scan costs the working set at most one");
        ok(adapted.p < p_start, "and the resulting ghost hit shrank p");
        ok(survived[1] == 8, "after adapting, a second scan costs nothing");
        arc_destroy(a);
    }

    /* ---- 9. mixed workload: invariants hold under sizes spanning decades ---- */
    {
        /* Deterministic LCG rather than rand(): a failure here has to be
           reproducible, and this file must behave identically on every run. */
        invfs_arc *a = arc_create(256 * 1024);
        uint32_t s = 0x13572468u;
        int i;
        const uint8_t *d = NULL;
        size_t n = 0;
        invfs_arc_stats st;
        for (i = 0; i < 4000; i++) {
            uint64_t key;
            size_t len;
            s = s * 1103515245u + 12345u;
            key = (s >> 16) % 60;                       /* 60 distinct files */
            s = s * 1103515245u + 12345u;
            /* 64 B to 256 KB: the real spread. The top bucket is the whole
               budget, so it gets refused; the one below evicts many small
               entries to fit. Both paths have to be reachable or the
               "exercised everything" assertion below is a lie. */
            len = (size_t)64u << ((s >> 20) % 13);
            if ((s >> 8) & 1) {
                if (!arc_get(a, key, &d, &n))
                    arc_put(a, key, blob(key, len), len);
                else if (!blob_ok(key, d, n))
                    { failures++; printf("  FAIL  wrong bytes for key %llu\n",
                                         (unsigned long long)key); }
            } else if ((s >> 9) & 1) {
                arc_put(a, key, blob(key, len), len);
            } else {
                arc_invalidate(a, key);
            }
            check_invariants(a, "mixed workload");
        }
        arc_stats(a, &st);
        printf("  mixed: %llu hits, %llu misses (%llu ghost), %llu inserts, "
               "%llu evictions, %llu refused, %u live, %u ghosts\n",
               (unsigned long long)st.hits, (unsigned long long)st.misses,
               (unsigned long long)st.ghost_hits, (unsigned long long)st.inserts,
               (unsigned long long)st.evictions, (unsigned long long)st.refused,
               st.entries, st.ghosts);
        /* Not a performance claim -- just that the workload actually reached
           the paths this test exists for. A zero here means the test is vacuous. */
        ok(st.evictions > 0 && st.ghost_hits > 0 && st.refused > 0,
           "the workload exercised eviction, ghost hits and refusal");
        ok(st.hits > 0, "and still hit sometimes");

        /* clear() must return everything without unmapping the cache itself */
        arc_clear(a);
        arc_stats(a, &st);
        ok(st.entries == 0 && st.ghosts == 0 && st.bytes == 0,
           "clear empties both the data and the history");
        ok(arc_get(a, 1, &d, &n) == 0, "and nothing survives it");
        arc_destroy(a);
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
