/* vol_reclaim.c — WP-M15: reachability diff + delta segment free.
 *
 * Frees metadata that is no longer reachable:
 *   - Base pages reachable from neither the current base root nor a pinned
 *     save-point root (the reachability diff via btree_reclaim).
 *   - Delta segments below the save-point delta_end once a fold has published.
 *
 * Reader drain: a page is freeable only after (a) fold has published newer
 * root AND (b) no reader holds the old root. Every base-tree read
 * announces itself (vol_reclaim_reader_snapshot) BEFORE it captures the
 * root and releases (vol_reclaim_reader_release) after the walk; the fold
 * bumps g_fold_epoch and drains before it frees.
 */

#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_metabuf.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* WP121: the orphan collector's gate. WP126 bounds its cost.            */
/* ------------------------------------------------------------------ */

/* DEFAULT ON since 2026-09-28 (author's call). The collector frees metadata
 * pages in a way that a wrong answer to "is this page reachable" turns into
 * silent data loss rather than a crash, so this used to ship opt-in
 * (INVFS_RECLAIM_ORPHANS=1) with that safety argument recorded here instead
 * of in the code. Both promotion conditions WP121 listed are now closed, the
 * reader enforces the invariant tree-wide (WP123 at the root slot, then WP-D
 * 7ea939d), and the sweep is the only thing that can return the space -- so
 * it ships enabled, and INVFS_RECLAIM_ORPHANS=0 turns it off.
 *
 * WHAT A READER SHOULD TAKE FROM THIS: the dangerous property has NOT gone
 * away. A default-ON pass that frees COW B+tree base pages still rests
 * entirely on its liveness predicate, and what stands behind it is the READER
 * refusing an unallocated page (mbuf_read_ptr) -- not the reclaimer's own
 * caution. A change that widens what counts as reachable is a safety change,
 * not a performance one, and the cost measurement quoted below is the ceiling
 * of what turning it ON buys, not a licence to widen it.
 *
 * The value is read once per process and cached:
 * a volume is opened once per process, so re-reading getenv per fold buys
 * nothing and makes the gate depend on call order.
 *
 * HISTORY, because the default changed and the old reasons are worth keeping.
 * The gate was default-OFF from WP126 through 2026-09-28, and the two
 * promotion conditions WP121 listed for flipping it are now both closed:
 *
 *   (1) Cost. Measured, not asserted (AUDIT.md 8): +0.29% sweep wall at 8 GiB
 *       over n=5 per arm with 10/10 exit 0, and +2072 kB peak RSS on a 1 TiB
 *       volume -- 0.0002% -- because `inlist` is calloc'd and untouched zero
 *       pages never fault in, which is 0.06x the 32 MiB a naive
 *       total_blocks/8 would imply.
 *
 *   (2) The reader must enforce the invariant, not just the reclaimer, so a
 *       wrong liveness predicate cannot damage a volume SILENTLY. Closed in
 *       two halves: WP123 at the root slot, then WP-D (7ea939d) tree-wide --
 *       mbuf_read_ptr refuses a page whose block the allocation bitmap
 *       reports free, so every base-tree walk in vol_btree.c inherits the
 *       check.
 *
 * A third defect had to be fixed first, and it is the reason this gate was
 * flipping to no-op: 3b70ab2. Commit 8b9a21c's mechanical uint64_t ->
 * invfs_blkptr type fix left v->pinned_root with checksum=0 and gen=0, so
 * every mark walk over it returned -1 and the collector aborted on exactly
 * the sweeps that create orphans worth collecting -- while exiting 0. Flipping
 * the default before that would have shipped a permanently failing collector
 * to every user as a warning on every sweep. The reader now also enforces
 * the RT30-slot condition (vol_btree.c, vol_fold.c). */
static int orphan_gate_state = -1;   /* -1 = not yet read */

static int orphan_gate(void)
{
    const char *e;
    if (orphan_gate_state >= 0)
        return orphan_gate_state;
    e = getenv("INVFS_RECLAIM_ORPHANS");
    /* Default ON since 2026-09-28 (author's call). The collector is not a
     * speculative extra: on a transform sweep it returns 4.46 MiB per sweep
     * (1142 blocks over 300 files, AUDIT.md 9), and a volume that fills latches
     * read-only for good (volume.c:2875) with sweep offline-only, so stranded
     * metadata space has no other way back. Measured cost of the collector:
     * +0.29% sweep wall (8 GiB, n=5/arm, 10/10 exit 0) and +2072 kB peak RSS on
     * a 1 TiB volume (0.0002%), because `inlist` is calloc'd and untouched
     * zero pages never fault in -- AUDIT.md 8. Both promotion conditions in
     * the note above are met: the reader side is fixed tree-wide
     * (7ea939d) and the producer side, which made every walk over
     * v->pinned_root return -1 and silently disabled the collector on exactly
     * the sweeps that create orphans, is fixed (3b70ab2).
     *
     * INVFS_RECLAIM_ORPHANS=0 turns it back off for a bisect. Anything else
     * other than "1" or "0" is a typo and is rejected loudly rather than
     * silently read as a policy the caller did not ask for. */
    if (!e || !e[0]) {
        orphan_gate_state = 1;
    } else if (!strcmp(e, "1")) {
        orphan_gate_state = 1;
    } else if (!strcmp(e, "0")) {
        orphan_gate_state = 0;
    } else {
        fprintf(stderr,
                "warning: INVFS_RECLAIM_ORPHANS=%s is neither \"0\" nor \"1\"; "
                "treating it as \"0\" (collector off). Use 0 to disable the "
                "orphan collector, 1 or unset to enable it.\n", e);
        orphan_gate_state = 0;
    }
    return orphan_gate_state;
}

static int orph_stats_wanted(void)
{
    const char *e = getenv("INVFS_RECLAIM_STATS");
    return e && e[0] == '1' && e[1] == '\0';
}

/* One gate, two drains. `full` picks the offline sweep's drain-to-
 * settlement loop over the fold path's single bounded pass; the gate and
 * every other rule apply identically to both. */
static int orph_gated(invfs_volume *v, uint64_t *freed_out, int full)
{
    uint64_t freed = 0;
    int rc;

    if (freed_out)
        *freed_out = 0;
    if (!v || !orphan_gate())
        return 0;
    rc = full ? btree_collect_orphans_full(v, &freed)
              : btree_collect_orphans(v, &freed);
    if (rc != 0)
        return -1;
    if (freed_out)
        *freed_out = freed;
    if (freed)
        fprintf(stderr, "vol_reclaim: collected %llu orphaned v3 base page(s)\n",
                (unsigned long long)freed);
    if (orph_stats_wanted()) {
        struct invfs_orphan_stats st;
        if (btree_orphan_stats(v, &st) == 0)
            fprintf(stderr,
                    "vol_reclaim: orphan stats calls=%llu cands=%llu "
                    "peak=%llu cand_reads=%llu seed_reads=%llu mark_reads=%llu "
                    "freed=%llu settled=%d\n",
                    (unsigned long long)st.calls, (unsigned long long)st.cands,
                    (unsigned long long)st.peak,
                    (unsigned long long)st.cand_reads,
                    (unsigned long long)st.seed_reads,
                    (unsigned long long)st.mark_reads,
                    (unsigned long long)st.freed, st.settled);
    }
    return (int)freed;
}

/* Free base pages that no live root can reach. The gate is HERE, not at
 * the call sites, so every path that reaches the collector is gated by
 * exactly one rule and there is no call site that forgot it.
 *
 * This is the FOLD-path entry point: one bounded, incremental pass, whose
 * cost is a function of the metadata rather than of the volume.
 *
 * Returns the number of pages freed, 0 when the gate is off or the
 * collector declined to run, -1 on error. */
int vol_reclaim_orphans(invfs_volume *v, uint64_t *freed_out)
{
    return orph_gated(v, freed_out, 0);
}

/* The OFFLINE-SWEEP entry point: the same collector, drained to
 * settlement. A sweep holds the volume exclusively and has no latency
 * budget, so it should collect everything collectable; a fold must not.
 * Splitting the two is what lets the fold path be bounded without costing
 * the sweep any of the compression win. */
int vol_reclaim_orphans_full(invfs_volume *v, uint64_t *freed_out)
{
    return orph_gated(v, freed_out, 1);
}

int vol_reclaim_orphan_stats(invfs_volume *v, struct invfs_orphan_stats *out)
{
    if (!v)
        return -1;
    return btree_orphan_stats(v, out);
}

/* ------------------------------------------------------------------ */
/* g_fold_epoch — reader drain                                         */
/* ------------------------------------------------------------------ */

/* Global epoch counter. Bumped on every fold publish; readers snapshot
 * on entry and are considered in-flight until they call
 * vol_reclaim_reader_release. The fold bumps it, but it is touched from
 * every base-tree read thread, so it is atomic too. */
static uint64_t g_fold_epoch = 0;

/* The reader count, and it is REAL: every base-tree read announces itself
 * with vol_reclaim_reader_snapshot() and releases with
 * vol_reclaim_reader_release(). Before this was wired the counter was
 * declared, drained on, and never incremented by anything, so
 * vol_reclaim_drain was a no-op and fold_reclaim_hook freed a retired
 * generation under a reader that had already captured its root -- a read
 * returning -1 at mbuf_read_ptr (vol_metabuf.c:192) with no error having
 * happened, measured at ~1e-6 of reads.
 *
 * __atomic_*, NOT <stdatomic.h>: this is the house idiom (blkio.c:594-604)
 * and it keeps the header out of every includer.
 *
 * SEQ_CST on the count AND on the epoch, because these two counters ARE
 * the whole synchronisation argument. The reader announces, then reads the
 * root slot; the reclaimer publishes the new root, then reads the count.
 * With both sides sequentially consistent, "the reclaimer saw zero" forces
 * the reader's root read to have happened after the publish, so the reader
 * captured the NEW root. Relaxing either side reopens the window this
 * whole mechanism exists to close.
 *
 * GLOBAL, while a volume is a per-handle object. Fine today because there
 * is exactly one vol_open per process; a second volume in one process
 * would make a reader on either stall the other's reclaim. That is a
 * property to keep true, not one to rely on silently. */
static uint64_t g_readers_in_flight = 0;

/* How long the drain waits for a reader before giving up on this
 * generation. NOT a liveness parameter for a healthy volume -- a reader
 * that finishes frees the drain immediately -- it is the bound on the cost
 * of a MISSED release, which is a mount-wide hang otherwise (the count is
 * process-global and the drain spins under g_io_lock; see fold_reclaim_hook).
 *
 * 30 s, chosen against the measured worst case: the longest base-tree walk
 * a drain can be waiting behind (vol_iter_live_inodes / vol_dirent_scan
 * over a large namespace) is single-digit seconds, so 30 s is ~an order of
 * magnitude of headroom over a walk that has merely gone slow, while still
 * bounding the stall at something an operator will notice and can act on.
 * Tunable because a pathological volume (a 5M-file sweep walker on a cold
 * image) may want more: INVFS_RECLAIM_DRAIN_MS. At the bound the drain
 * returns -1, its caller SKIPS the free rather than doing it unsafely, and
 * the condition is printed. Space is not reclaimed this fold; the next
 * fold tries again. That is recoverable, a hang is not. */
#ifndef INVFS_RECLAIM_DRAIN_MS_DEFAULT
#define INVFS_RECLAIM_DRAIN_MS_DEFAULT 30000u
#endif

/* Below this wait, spin: a point read announces and releases within
 * microseconds, and paying a millisecond of sleep for it would put that
 * latency into every fold. Above it, sleep 1 ms at a time so a long wait
 * costs a long wait and not a saturated core. */
#define RECLAIM_DRAIN_SPIN_MS 2u

/* Read once per process and cached, for the same reason the orphan gate
 * above caches its getenv: a volume is opened once per process, so re-reading
 * the environment per fold buys nothing and makes the bound depend on call
 * order. Two threads racing here compute the same value and store the same
 * one, so the benign race is not worth a lock on the path that decides
 * whether a fold is about to block. */
static uint32_t reclaim_drain_timeout_ms(void)
{
    static uint32_t cached = 0;
    if (cached == 0) {
        const char *e = getenv("INVFS_RECLAIM_DRAIN_MS");
        long ms = e ? strtol(e, NULL, 10) : (long)INVFS_RECLAIM_DRAIN_MS_DEFAULT;
        cached = (ms > 0 && ms < 3600000L) ? (uint32_t)ms
                                            : INVFS_RECLAIM_DRAIN_MS_DEFAULT;
    }
    return cached;
}

static uint64_t reclaim_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

uint64_t vol_reclaim_reader_snapshot(void)
{
    /* The increment comes FIRST, before anything this caller will read.
     * Announce-then-capture is load-bearing: the window between the root
     * read returning and the reader being counted is exactly the window in
     * which a fold can publish, drain (count still 0) and free, and the
     * reader then walks a root whose pages are already gone. Capture-then-
     * announce is the same bug with an extra step. */
    (void)__atomic_add_fetch(&g_readers_in_flight, 1, __ATOMIC_SEQ_CST);
    return __atomic_load_n(&g_fold_epoch, __ATOMIC_SEQ_CST);
}

void vol_reclaim_reader_release(void)
{
    /* Unconditional, and deliberately so: the old `if (count) count--`
     * guard turns an unbalanced release into a silent underflow that a
     * later drain would trip over. Every announce in the tree has exactly
     * one release on every path out, and that is checkable by reading it. */
    (void)__atomic_sub_fetch(&g_readers_in_flight, 1, __ATOMIC_SEQ_CST);
}

int vol_reclaim_drain(invfs_volume *v)
{
    uint64_t start = reclaim_now_ms();
    uint32_t bound = reclaim_drain_timeout_ms();

    (void)v;
    for (;;) {
        uint64_t waited;
        if (__atomic_load_n(&g_readers_in_flight, __ATOMIC_SEQ_CST) == 0)
            return 0;
        waited = reclaim_now_ms() - start;
        if (waited >= bound) {
            /* LOUD on purpose. Reaching this line means a base-tree read
             * announced itself and did not release, which under g_io_lock
             * is a wedged mount; the cost of staying quiet is that the next
             * person to see a stall has nothing to grep for. Say which
             * count, how long, and what the caller is about to give up. */
            fprintf(stderr,
                    "[reclaim] TIMEOUT: %llu base-tree reader(s) still in "
                    "flight after %llu ms; a read path is missing a "
                    "vol_reclaim_reader_release(). This reclaim is being "
                    "SKIPPED (space is not freed this round) -- the volume "
                    "is intact, but every later fold will hit this too "
                    "until the read path is fixed.\n",
                    (unsigned long long)
                        __atomic_load_n(&g_readers_in_flight, __ATOMIC_SEQ_CST),
                    (unsigned long long)waited);
            return -1;
        }
        if (waited < RECLAIM_DRAIN_SPIN_MS) {
#ifndef _WIN32
            sched_yield();
#else
            Sleep(0);
#endif
        } else {
            struct timespec ts = { 0, 1000000L };   /* 1 ms */
            nanosleep(&ts, NULL);
        }
    }
}

uint64_t vol_reclaim_bump_epoch(void)
{
    return __atomic_add_fetch(&g_fold_epoch, 1, __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------------------ */
/* Reachability diff — base pages                                      */
/* ------------------------------------------------------------------ */

/* Public: reachability diff mark + free for base pages.
 * Frees pages in old_root that are not reachable from keep_root or from
 * the live save point's pinned_root. A restored base must still be able
 * to reference its pages, so the pinned root is a second mark root. */
int vol_reclaim_mark_and_free(invfs_volume *v, invfs_blkptr old_root,
                              invfs_blkptr keep_root,
                              invfs_blkptr pinned_root)
{
    if (!v)
        return -1;

    /* WP77: pinned_root.pba == 0 when no save point is live; the
     * multi-root walk then degenerates to the single-root btree_reclaim. */
    return btree_reclaim_pinned(v, old_root, keep_root, pinned_root);
}

/* ------------------------------------------------------------------ */
/* Delta segment free                                                  */
/* ------------------------------------------------------------------ */

/* Chain walk guard: a corrupt prev_pba cycle must not loop forever. */
#define RECLAIM_MAX_DELTA_SEGMENTS (1u << 20)

/* Read the delta segment header at pba. Returns 0 on success, -1 on error. */
static int reclaim_delta_read_hdr(invfs_volume *v, uint64_t pba,
                                  invfs_delta_seg_hdr *out)
{
    uint8_t raw[INVFS_DELTA_SEG_HDR_LEN];

    if (pba == 0 || pba >= v->sb.total_blocks)
        return -1;
    if (io_pread(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE, raw, sizeof raw) != 0)
        return -1;
    if (memcmp(raw, INVFS_DELTA_SEG_MAGIC, 4) != 0)
        return -1;

    if (out) {
        uint32_t v_be = ((uint32_t)raw[4] << 24) | ((uint32_t)raw[5] << 16) |
                        ((uint32_t)raw[6] << 8)  | (uint32_t)raw[7];
        uint32_t hs_be = ((uint32_t)raw[8] << 24) | ((uint32_t)raw[9] << 16) |
                         ((uint32_t)raw[10] << 8) | (uint32_t)raw[11];
        uint32_t sb_be = ((uint32_t)raw[12] << 24) | ((uint32_t)raw[13] << 16) |
                         ((uint32_t)raw[14] << 8) | (uint32_t)raw[15];
        uint64_t seq_be = ((uint64_t)raw[16] << 56) | ((uint64_t)raw[17] << 48) |
                          ((uint64_t)raw[18] << 40) | ((uint64_t)raw[19] << 32) |
                          ((uint64_t)raw[20] << 24) | ((uint64_t)raw[21] << 16) |
                          ((uint64_t)raw[22] << 8)  | (uint64_t)raw[23];
        uint64_t prev_be = ((uint64_t)raw[24] << 56) | ((uint64_t)raw[25] << 48) |
                           ((uint64_t)raw[26] << 40) | ((uint64_t)raw[27] << 32) |
                           ((uint64_t)raw[28] << 24) | ((uint64_t)raw[29] << 16) |
                           ((uint64_t)raw[30] << 8)  | (uint64_t)raw[31];
        uint64_t next_be = ((uint64_t)raw[32] << 56) | ((uint64_t)raw[33] << 48) |
                           ((uint64_t)raw[34] << 40) | ((uint64_t)raw[35] << 32) |
                           ((uint64_t)raw[36] << 24) | ((uint64_t)raw[37] << 16) |
                           ((uint64_t)raw[38] << 8)  | (uint64_t)raw[39];

        out->version    = v_be;
        out->hdr_size   = hs_be;
        out->seg_blocks = sb_be;
        out->seg_seq    = seq_be;
        out->prev_pba   = prev_be;
        out->next_pba   = next_be;
        memcpy(out->magic, INVFS_DELTA_SEG_MAGIC, 4);
    }
    return 0;
}

/* Walk the delta chain from head_pba (newest to oldest) and free all
 * segments whose offset (pba) is less than delta_end. delta_end=0 means
 * free everything in the chain. */
int vol_reclaim_delta_segments(invfs_volume *v, uint64_t head_pba,
                               uint64_t delta_end)
{
    uint64_t pba = head_pba;
    uint64_t freed = 0;
    uint32_t i = 0;

    if (!v || head_pba == 0)
        return 0;

    while (pba && i < RECLAIM_MAX_DELTA_SEGMENTS) {
        invfs_delta_seg_hdr h;
        uint64_t next;

        if (reclaim_delta_read_hdr(v, pba, &h) != 0)
            break;

        if (delta_end == 0 || pba < delta_end) {
            uint64_t b, end = pba + INVFS_DELTA_SEG_BLOCKS;
            if (end > v->sb.total_blocks)
                end = v->sb.total_blocks;
            for (b = pba; b < end; b++)
                mbuf_free(v, b);
            freed++;
        }

        next = h.prev_pba;
        if (next == pba)
            break;
        pba = next;
        i++;
    }
    return (int)freed;
}
