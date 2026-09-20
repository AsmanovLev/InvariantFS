/* vol_reclaim.h — WP-M15: public declarations for reclaim primitives. */

#ifndef INVFS_VOL_RECLAIM_H
#define INVFS_VOL_RECLAIM_H

#include <stdint.h>
#include "invarifs.h"

struct invfs_volume;

/* Reader drain: snapshot the current fold epoch on entry.
 * The returned value must be passed to vol_reclaim_reader_release when
 * the read operation completes. */
uint64_t vol_reclaim_reader_snapshot(void);

/* Reader drain: call when done with the old root. */
void vol_reclaim_reader_release(void);

/* Wait for all in-flight readers of the previous epoch to drain.
 * Called with volume write lock held. */
int vol_reclaim_drain(struct invfs_volume *v);

/* Bump the fold epoch after publishing a new root. */
uint64_t vol_reclaim_bump_epoch(void);

/* Reachability diff mark + free for base pages.
 * Frees pages in old_root that are not reachable from keep_root or
 * pinned_root. pinned_root may be {0,0} if no save-point is active. */
int vol_reclaim_mark_and_free(struct invfs_volume *v,
                             invfs_blkptr old_root,
                             invfs_blkptr keep_root,
                             invfs_blkptr pinned_root);

/* Free delta segments in the chain starting at head_pba.
 * Segments with pba < delta_end are freed (delta_end=0 means all). */
int vol_reclaim_delta_segments(struct invfs_volume *v,
                              uint64_t head_pba,
                              uint64_t delta_end);

#endif /* INVFS_VOL_RECLAIM_H */
