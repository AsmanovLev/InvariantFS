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

/* WP121: free base pages that no live root can reach -- the FULL-POOL
 * orphan collector, as opposed to the one-generation diff above. The gate
 * is inside: returns 0 having done nothing unless INVFS_RECLAIM_ORPHANS=1
 * is set in the environment. It is DEFAULT OFF; see the WP126 report for
 * why, and for what would have to be true to promote it. Returns the page
 * count freed, or -1.
 *
 * WP126: this is the FOLD-path entry point and it is ONE bounded,
 * incremental pass. Its block-read cost is a function of the metadata, not
 * of the volume. The offline sweep should use the full drain below, which
 * has no latency budget to protect. */
int vol_reclaim_orphans(struct invfs_volume *v, uint64_t *freed_out);

/* WP126: the same collector, drained to settlement. The offline sweep's
 * entry point. Same gate, same predicate; the only difference is that it
 * keeps going until a whole pass frees nothing AND the one-pass seed scan
 * of pages a previous session allocated has wrapped the block space. */
int vol_reclaim_orphans_full(struct invfs_volume *v, uint64_t *freed_out);

/* WP126: the collector's own counters, for measurement. Purely
 * observational -- nothing in the collector's behaviour depends on them.
 * The struct is declared in vol_btree.h; including it here would drag the
 * B+-tree header into every caller of this one. */
struct invfs_orphan_stats;
int vol_reclaim_orphan_stats(struct invfs_volume *v,
                             struct invfs_orphan_stats *out);

/* Free delta segments in the chain starting at head_pba.
 * Segments with pba < delta_end are freed (delta_end=0 means all). */
int vol_reclaim_delta_segments(struct invfs_volume *v,
                              uint64_t head_pba,
                              uint64_t delta_end);

#endif /* INVFS_VOL_RECLAIM_H */
