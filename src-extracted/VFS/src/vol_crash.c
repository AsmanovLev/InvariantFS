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
 * 2. Maps durable before the record that needs them. An inode record names
 *    its data by *segment index* -- invfs_ast_block_entry.block_id is the L2P
 *    key, not a physical block -- so physical addresses exist only in the
 *    journal. A record that is durable while its L2P is not is not merely
 *    stale, it is unreadable and unrebuildable: fsck reports l2p_miss and has
 *    nothing to reconstruct the mapping from. Data blocks themselves are
 *    already durable (io_write at allocation time); only the bitmap and the
 *    journal were lazy, and invf-sweep flushed once at the *end of the whole
 *    run* -- so an interrupted sweep lost every file it had transcoded.
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
    if (v->needs_recovery) return -1;   /* refuse writes until recovered */
    if (v->dirty) return 0;
    v->sb.state = INVFS_STATE_DIRTY;
    if (vol_write_sb(v) != 0) return -1;
    v->dirty = 1;
    return 0;
}


/* Call before appending an inode record: mark dirty, then make the maps
   durable so the record about to land is backed by something readable. */
int vol_pre_record(invfs_volume *v)
{
    if (vol_mark_dirty(v) != 0) return -1;
    return vol_flush(v);
}


int vol_needs_recovery(invfs_volume *v)
{
    return v && v->needs_recovery;
}
