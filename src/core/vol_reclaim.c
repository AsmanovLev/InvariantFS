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
