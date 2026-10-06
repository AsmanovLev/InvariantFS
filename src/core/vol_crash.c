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
 * What "durable" buys depends on the backing store AND the platform, and the
 * platform that matters -- POSIX -- buys less than this file used to claim.
 * Stated exactly, per branch of blkio_open:
 *
 *   - Windows + raw device: FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH
 *     (blkio.c:243). Every write reaches the device as it is issued, so the
 *     write ordering above is the handle's own.
 *   - Windows + image file: buffered. Ordering survives process death.
 *   - POSIX (Linux), image file AND raw device alike: opened plain O_RDWR --
 *     no O_DIRECT, no O_SYNC, no O_DSYNC (blkio.c:296-298). Every write is a
 *     pwrite() into the page cache and the ONLY durability point is
 *     blkio_flush() -> fsync(fd) (blkio.c:626).
 *
 * So on Linux the guarantee is: after a barrier returns 0, the bytes have
 * been handed to the host storage stack and survive process death (which is
 * what the crash tests inject). fsync() does NOT make each write
 * write-through, does NOT order one pwrite against another at the device, and
 * cannot promise anything about a device that lies about its own volatile
 * cache. Every ordering rule in this file is therefore enforced by
 * InvariantFS, not by the open flags: the structures that must land in a
 * given order are written in that order and then made durable TOGETHER by one
 * barrier, and each is magic/CRC-framed so a partially completed flush is
 * detected on the next mount rather than adopted. Power-loss ordering on
 * Linux is delegated to the host filesystem and device honouring fsync(2).
 * InvariantFS adds nothing to it. (README.md, AGENTS.md 2.2 and
 * docs/SECURITY.md have always said this out loud; this comment did not.)
 *
 * ONE ordering is NOT established at all, and it is worth naming because a
 * reader of rule 2 above may assume it: the allocation bitmap is not made
 * durable BEFORE a delta record. A record is barriered at append
 * (vol_delta.c:571) and the bitmap is only written at vol_flush
 * (volume.c:2641), so a crash inside the deferred window leaves a durable
 * record naming blocks whose allocation bits are still clear on disk. That is
 * safe by DERIVATION, not by ordering: the bitmap is a derived cache
 * (vol_fsck.c:25) and replay re-reserves every block a replayed segment
 * spans (dl_reserve_segment, vol_delta.c:409) before any allocator call can
 * hand it out again. The deferred window can therefore strand space, never
 * alias a live block. The v3 commit-point table is
 * docs/architecture/META.md 4.1; ADR-009 is the decision behind the
 * v3 fsync contract.
 *
 * WP80: the barrier before CLEAN -- vol_close barriers before it writes the
 * CLEAN superblock (INVFS_CLOSE_NOBARRIER=1 is the documented opt-out), and
 * vol_sync does the same on v3 as on v2.
 */
int vol_mark_dirty(invfs_volume *v)
{
    if (!vol_write_enabled(v)) {
        if (v->degraded)
            fprintf(stderr, "vol: write refused: DEGRADED mount (device 0 "
                    "absent) -- the volume is read-only (EROFS). Reattach "
                    "dev0 for read-write.\n");
        return -1;
    }
    if (v->dirty) return 0;
    v->sb.state = INVFS_STATE_DIRTY;
    if (vol_write_sb(v) != 0) {
        return -1;
    }
    v->dirty = 1;
    return 0;
}


/* WP29: deferred flush with watermarks.  During a bulk import the
   per-record flush was the dominant cost (20K flushes x ~16 MB bitmap
   range each).  Records are authoritative and fsck rebuilds the bitmap,
   so deferring is safe -- orphaned blocks from a crash are freed on the
   next mount.  We still flush when the active metadata extent is >80 %
   full (compaction may be triggered next).  vol_close() and vol_sync()
   always flush. */
static int vol_should_flush(const invfs_volume *v)
{
    /* WP52: the inode area is a SEQUENCE of dynamic metadata extents, not
     * a single linear address region, so the mapper's active extent is the
     * real "inode area" here: flush when IT is >80 % full. Without the
     * mapper the legacy [inode_area_start, inode_area_end) ratio applies. */
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
    /* WP80/A: io_latched is the "failed THIS session" flag. needs_recovery
     * cannot gate the message on v3 -- vol_open sets it unconditionally for
     * the whole session, so the latch would be silent exactly where it is
     * most important. */
    if (!v->io_latched)
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
