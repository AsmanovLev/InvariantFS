/* vol_rollback.c — WP-M21: sweep checkpoint + retention registry + rollback
 * are RETIRED. The v3 metadata model (dynamic extents, fold/reclaim) makes
 * the v2 CKP0/"\x01reten" machinery redundant -- the inode area no longer
 * carries absolute-position tombstones that a checkpoint could protect, and
 * the fold replaces the cut-and-replay shape with a delta overlay.
 *
 * The function signatures are kept as stubs so that callers (invf-sweep,
 * invf-rollback, invf-fsck, the FUSE SIGUSR1 sweep path) compile unchanged;
 * every entry point is now a no-op that reports "no checkpoint" / "no
 * retention". invf-rollback reports "nothing to roll back to" and exits
 * cleanly. invf-sweep runs uncheckpointed (its own choice; retention no
 * longer constrains the sweep).
 *
 * The volume struct keeps the v->ck / v->ck_present / v->retain / v->retmap
 * fields as zero-init (the open path still validates the on-disk CKP0
 * descriptor so legacy volumes mount unchanged, but the validation result
 * is unused -- see vol_open_inner). Free accounting treats them as never
 * set, so vol_free_blocks / vol_metabuf both short-circuit the retention
 * branch. The seal exclusion code (vol_seal.c) skips the retention shard
 * loop (records never exist on v3). */

#include "volume_internal.h"


/* ---- WP21 stubs (kept for binary compat with callers) ---------------- */

uint32_t ckp0_crc(const invfs_ckp0 *ck)
{
    invfs_ckp0 t = *ck;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}


void ret_shard_name(uint64_t shard, char *out, size_t cap)
{
    /* WP-M21: the retention registry is gone. The function is preserved
     * only because vol_seal.c used to call it during view load; the seal
     * path no longer reads reten shards, so this should never be invoked.
     * Write a sentinel so a debug trace is greppable. */
    if (shard == 0)
        snprintf(out, cap, "\x01reten");
    else
        snprintf(out, cap, "\x01reten%llu",
                 (unsigned long long)shard);
    (void)cap;
}


int vol_ckp_armed(const invfs_volume *v)
{
    if (!v) return 0;
    return v->ck_present;
}


int vol_ckp_info(const invfs_volume *v, invfs_ckp0 *out)
{
    if (!v || !v->ck_present) return 0;
    if (out) *out = v->ck;
    return 1;
}


int vol_ckp_begin(invfs_volume *v, int no_realize)
{
    (void)v; (void)no_realize;
    /* WP-M21: checkpointing is retired; the sweep runs uncheckpointed.
     * Return 0 ("declined") so invf-sweep logs the same message it always
     * did and continues without a checkpoint. */
    return 0;
}


int vol_ckp_end(invfs_volume *v, uint64_t *ranges_out, uint64_t *blocks_out)
{
    if (ranges_out) *ranges_out = 0;
    if (blocks_out) *blocks_out = 0;
    (void)v;
    /* Nothing was retained (no checkpoint armed). */
    return 0;
}


int vol_ckp_realize(invfs_volume *v, uint64_t *freed_blocks_out)
{
    if (freed_blocks_out) *freed_blocks_out = 0;
    (void)v;
    /* Nothing to realize. */
    return 0;
}


int vol_rollback(invfs_volume *v, uint64_t *reclaimed_out)
{
    if (reclaimed_out) *reclaimed_out = 0;
    (void)v;
    /* No checkpoint -> nothing to roll back to. invf-rollback prints its own
     * message and exits 0. */
    return 1;
}


/* ckp_stage_replay (read-only time-travel) is gone -- it was the open_at
 * path that replayed the staged journal. Without a checkpoint there is no
 * staged journal; vol_open_at returns -3 (no checkpoint) when its caller
 * detects ck_present == 0. The function is kept as a stub to keep the
 * symbol exported for any out-of-tree consumer. */
int ckp_stage_replay(invfs_volume *v)
{
    (void)v;
    return -3;
}
