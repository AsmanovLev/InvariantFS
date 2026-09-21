/* vol_crash.c — crash consistency (doc/08): dirty marking + recovery
 * gate. Split from volume.c. */

#include "volume_internal.h"


/* ================= crash consistency =================
 *
 * Three rules, and each one closes a hole that was open before:
 *
 * 1. DIRTY before the first mutation, CLEAN after the last flush in
 *    vol_close. Nothing ever set DIRTY, so `state` was decoration: mkfs wrote
 *    CLEAN, no mount contradicted it, and a volume that died mid-write opened
 *    as though nothing had happened.
 *
 * 2. The covering bitmap durable before the record that names the pbas.
 *    WP27: an inode record names its data by physical address (the 32B
 *    AST entry's pba), so the commit ordering is: data blocks (io_write
 *    at allocation) -> bitmap (vol_flush's dirty range) -> owner-WAL ops
 *    (if any) -> the record append itself. WP29: the per-record flush is
 *    replaced by a deferred flush (watermarks at 80 % inode area / 50 %
 *    journal slot, plus vol_close / vol_sync). Records are authoritative
 *    and fsck rebuilds the bitmap, so a crash during the deferred window
 *    leaves orphaned blocks that the next mount reclaims — safe for bulk
 *    import and general workloads alike.
 *
 * 3. Tombstones are the exception: they must be durable BEFORE the frees they
 *    authorize, never after, or a crash leaves a live record whose blocks are
 *    free and reusable. vol_delete_inode already had this right (io_write for
 *    the tombstone, bitmap dirtied in RAM only), so deletes mark DIRTY
 *    without flushing.
 *
 * What "durable" buys depends on the backing store. A device is opened
 * FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH (blkio.c), so write ordering
 * there survives power loss. An image file is buffered, so ordering survives
 * process death -- which is what the crash tests inject -- but power loss
 * needs an actual barrier; INVFS_FSYNC=1 adds one at close.
 */
int vol_mark_dirty(invfs_volume *v)
{
    /* WP24-lite: the time-travel view is read-only by construction -- its
     * append cursors sit AT the checkpoint cut, so an append from this
     * handle would overwrite the first post-checkpoint record of the
     * PRESENT volume. This is the funnel every mutation path passes
     * (DIRTY-before-first-write, rule 1), so the refusal here is the
     * engine-level guarantee; drivers also see EROFS earlier through
     * vol_write_enabled (the handle carries VOLF_READONLY). */
    if (v->time_travel) {
        fprintf(stderr, "vol: write refused: this is a read-only "
                "time-travel view of sweep checkpoint #%llu\n",
                (unsigned long long)v->ck.sweep_seq);
        return -1;
    }
    if (v->needs_recovery) {
        /* WP25: name the degraded case -- it is a mount state, not
         * damage, so the refusal says so plainly (loud EROFS). */
        if (v->degraded)
            fprintf(stderr, "vol: write refused: DEGRADED mount (device 0 "
                    "absent) -- the volume is read-only (EROFS). Reattach "
                    "dev0 for read-write.\n");
        return -1;   /* refuse writes until recovered */
    }
    if (v->dirty) return 0;
    v->sb.state = INVFS_STATE_DIRTY;
    if (vol_write_sb(v) != 0) return -1;
    v->dirty = 1;
    return 0;
}


/* WP29: deferred flush with watermarks.  During a bulk import the
   per-record flush was the dominant cost (20K flushes × ~16 MB bitmap
   range each).  Records are authoritative and fsck rebuilds the bitmap,
   so deferring the flush is safe — orphaned blocks from a crash are
   freed on the next mount.  We still flush when:
     - the inode area is >80 % full (compaction may be triggered next)
     - the journal slot is >50 % full (avoids slot overflow)
   vol_close() and vol_sync() always flush. */
static int vol_should_flush(const invfs_volume *v)
{
    /* inode area watermark: 80 %.
     * WP52: on a v0.3.0+ mapper volume the inode area is a SEQUENCE of
     * dynamic extents, not the legacy [inode_area_start, inode_area_end)
     * region. Those legacy byte/block fields describe the pre-mapper
     * layout and make the ratio meaningless (inode_area_start is a block
     * count subtracted from a byte count, and inode_area_pos sits in a
     * shadow extent while inode_area_end names the first extent). The
     * broken ratio read "full" from the first record, so EVERY append
     * flushed -- and the flush-time owner-record rewrite then allocated a
     * fresh extent each time (the WP47 bigvol regression: ~8k files
     * exhausted the 1.2 GiB shadow). The mapper's active extent is the
     * real "inode area" here: flush when IT is >80 % full. The journal
     * watermark below is layout-independent and still applies. */
    if (v->met0_present && v->meta_mapper) {
        uint64_t entry = meta_mapper_get(v, (size_t)v->met0.active_extent);
        uint64_t esz = entry ? invfs_meta_ext_size(entry) : 0;
        if (esz && v->met0.active_offset * 5 >= esz * 4)
            return 1;
    } else {
        uint64_t area_total = v->inode_area_end - v->inode_area_start;
        uint64_t area_used  = v->inode_area_pos - v->inode_area_start;
        if (area_total > 0 && area_used * 5 >= area_total * 4)
            return 1;
    }
    /* journal watermark: 50 % of one slot */
    if (v->j_slotted && v->jops_n > 0) {
        uint64_t slot_payload = (uint64_t)(INVFS_JRN_SLOT_BLOCKS - 1)
                                * INVFS_BLOCK_SIZE
                                / sizeof(invfs_l2p_entry);
        if (v->jops_n >= slot_payload / 2)
            return 1;
    }
    return 0;
}


/* Call before appending an inode record: mark dirty, then (WP29) conditionally
   flush when watermarks are hit.  The old unconditional flush was the dominant
   cost during bulk import — records are authoritative and fsck rebuilds the
   bitmap, so deferring is safe.  vol_close / vol_sync still guarantee a full
   flush at session end. */
int vol_pre_record(invfs_volume *v)
{
    if (vol_mark_dirty(v) != 0) return -1;
    if (vol_should_flush(v))
        return vol_flush(v);
    return 0;
}


int vol_needs_recovery(invfs_volume *v)
{
    return v && v->needs_recovery;
}


/* WP22c/F1: a flush/sync failed. On a buffered backing store the append
 * path's io_writes report success from the page cache, so a device error
 * surfaces only at a barrier -- possibly many commits after the bytes it
 * covers were written. The dm-flakey error window proved what happens if
 * the session then keeps going: the storm-era dirty pages die in
 * writeback leaving a zero hole in the append-only inode area, every
 * record committed past the hole is valid-CRC yet unreachable at the next
 * mount (the open scan stops at the gap), and fsck frees their blocks as
 * orphans -- fsync-acknowledged files silently gone after a clean
 * unmount.
 *
 * So a failed flush/sync must never leave the in-memory append cursors
 * past unpersisted bytes, and the volume must not continue appending into
 * a known-broken tail:
 *
 *  - the inode-area cursor is re-anchored at inode_area_durable, the last
 *    position a successful barrier pinned. This is deliberately NOT a
 *    device rescan: mid-error-window reads are unreliable, and the anchor
 *    is a conservative lower bound -- records between it and the true end
 *    are reachable-or-not exactly as their own fsyncs were answered (any
 *    commit since the last successful barrier was never acknowledged), so
 *    treating them as lost breaks no promise. The precise end is the next
 *    mount's ordinary open scan (the volume is latched, so nothing
 *    overwrites the tail in between).
 *  - needs_recovery latches: every mutation path (vol_write_enabled /
 *    vol_mark_dirty) fails loudly until a remount + recovery.
 *  - the failure stays loud where it happened: the caller of the failed
 *    flush/sync returns the error, and vol_close will not mark the volume
 *    CLEAN, so the next mount runs recovery instead of silently adopting
 *    a truncated tail.
 *
 * The same class lived in the bitmap and the journal (both rewritten in
 * place by vol_flush from in-memory state): the latch covers them too --
 * with mutation stopped, neither can be extended past unpersisted bytes,
 * and the next flush on a healed device rewrites them whole from the
 * in-memory tables. */
void vol_io_error_latch(invfs_volume *v, const char *what)
{
    if (!v) return;
    if (getenv("INVFS_TRACE_LATCH"))
        fprintf(stderr, "[latch] op='%s' (needs_recovery was %d)\n",
                what, (int)v->needs_recovery);
    if (!v->needs_recovery)
        fprintf(stderr, "vol: %s failed; volume latched until "
                "remount+fsck (inode-area tail re-anchored %llu -> %llu)\n",
                what,
                (unsigned long long)v->inode_area_pos,
                (unsigned long long)v->inode_area_durable);
    v->needs_recovery = 1;
    v->io_latched = 1;
    v->inode_area_pos = v->inode_area_durable;
}


/* 1 when a flush/sync failure latched the volume THIS session: the FUSE
 * read path refuses on it (loud beats maybe-phantom), while a volume
 * that merely OPENED dirty stays readable for inspection. */
int vol_io_latched(invfs_volume *v)
{
    return v && v->io_latched;
}
