/* vol_reclaim.c — WP-M15: reachability diff + delta segment free.
 *
 * Frees metadata that is no longer reachable:
 *   - Base pages reachable from neither the current base root nor a pinned
 *     save-point root (the reachability diff via btree_reclaim).
 *   - Delta segments below the save-point delta_end once a fold has published.
 *
 * Reader drain: a page is freeable only after (a) fold has published newer
 * root AND (b) no reader holds the old root. Uses a simple epoch counter:
 * each fold bumps g_fold_epoch; readers snapshot it on entry and are
 * considered "in flight" until they release.
 */

#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_metabuf.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* WP121: the orphan collector's gate. WP126 bounds its cost.            */
/* ------------------------------------------------------------------ */

/* DEFAULT OFF. The collector frees metadata pages in a way that a wrong
 * answer to "is this page reachable" turns into silent data loss rather
 * than a crash, so it does not ship enabled. INVFS_RECLAIM_ORPHANS=1 turns
 * it on; nothing else does. The value is read once per process and cached:
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
 * vol_reclaim_reader_release. This is a simple counter, not atomic:
 * callers must hold the volume write lock (already held by fold). */
static uint64_t g_fold_epoch = 0;

/* Reader snapshot: incremented on snapshot, decremented on release.
 * Uses a regular uint64_t because callers hold the volume write lock. */
static uint64_t g_readers_in_flight = 0;

uint64_t vol_reclaim_reader_snapshot(void)
{
    return g_fold_epoch;
}

void vol_reclaim_reader_release(void)
{
    if (g_readers_in_flight)
        g_readers_in_flight--;
}

int vol_reclaim_drain(invfs_volume *v)
{
    (void)v;
    while (g_readers_in_flight > 0) {
#ifndef _WIN32
        sched_yield();
#else
        Sleep(0);
#endif
    }
    return 0;
}

uint64_t vol_reclaim_bump_epoch(void)
{
    return ++g_fold_epoch;
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
