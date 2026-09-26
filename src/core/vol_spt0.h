/* vol_spt0.h — WP-M16: v3 save point (SPT0 descriptor).
 *
 * The save point anchors a persistent view of the volume at capture time:
 * {base_root, delta_end}. base_root is the pinned RT30 root pba; delta_end
 * is the byte offset within the delta segment chain at capture.
 *
 * K=1: refuse a second save point while one is live.
 *
 * Capture: record {base_root, delta_end}, write SPT0, set pinned_root.
 * Rollback: publish base_root via RT30 double-slot, truncate delta to
 *           delta_end, replay.
 * Drop: clear SPT0, release pin.
 *
 * WP96: that pair is the save point's METADATA. The DATA its recipes address
 * is covered by two independent layers, both in vol_spt0.c:
 *
 *   1. the pin (SPN0). At capture every block the captured generation's
 *      recipes name is marked in a bitmap of total_blocks, persisted in its
 *      own block run, and named by a block-0 descriptor. While a save point
 *      is live, spt0_block_pinned() (consulted by vol_free_blocks, so at
 *      EVERY free, not per call site) keeps those blocks allocated. A bare
 *      sweep drops the previous window and captures a new one before its
 *      walk, so a replaced recipe's blocks are held for exactly one
 *      generation and are freed by the next capture's reclaim pass;
 *      spt0_drop (invf-sweep --realize) releases the hold immediately.
 *   2. the restore-time data check. spt0_restore walks the pinned
 *      generation's recipes and verifies every segment they name before
 *      publishing anything; a failure is SPT0_RC_DAMAGED and NOTHING is
 *      written. This works with no pin at all (INVFS_SPT0_NOPIN=1, a
 *      capture that could not allocate the mark set, a pre-WP96 save point),
 *      so a hole in layer 1 degrades to "rollback unavailable", never to
 *      silent corruption.
 */
#ifndef INVFS_VOL_SPT0_H
#define INVFS_VOL_SPT0_H

#include <stdint.h>
#include <stddef.h>

#include "invarifs.h"

/* volume.h's include guard caveat (see vol_metabuf.h) */
typedef struct invfs_volume invfs_volume;

/* WP86: the pinned base tree does not walk (an unreadable page, a structural
 * failure). Distinct from SPT0_RC_ERROR so the CLI can say "the save point is
 * damaged" instead of "rollback failed": the volume is untouched. */
#define SPT0_RC_ERROR     (-1)
#define SPT0_RC_DAMAGED   3

/* ---- lifecycle -------------------------------------------------------- */

/* Load SPT0 from block 0 at INVFS_SPT0_OFF. Sets v->savepoint_live and
 * v->pinned_root if CRC-valid. 0 = present and valid, 1 = absent/torn,
 * -1 = io error. */
int spt0_load(invfs_volume *v);

/* Persist the current spt0 to block 0 (CRC is computed before write).
 * 0 = ok, -1 = io error. */
int spt0_store(invfs_volume *v);

/* ---- save point operations -------------------------------------------- */

/* Capture a save point: record current base_root + delta_end, write SPT0,
 * set pinned_root. K=1: refuses if savepoint_live is already set.
 * 0 = ok, -1 = error, 1 = refused (K=1 violation or not a v3 volume),
 * SPT0_RC_DAMAGED (WP86) = the base tree is damaged; nothing is pinned. */
int spt0_capture(invfs_volume *v);

/* Rollback to the captured save point:
 *   1. publish(base_root) via RT30 double-slot + seq++
 *   2. truncate_delta(delta_end)
 *   3. rebuild delta index by replay
 *   4. clear savepoint (write zeroed SPT0)
 * 0 = ok, -1 = error, 1 = no save point, SPT0_RC_DAMAGED (WP86) = the pinned
 * base tree does not walk, so the rollback is REFUSED and nothing is written. */
int spt0_restore(invfs_volume *v);

/* Drop the save point: clear SPT0, reset pinned_root.
 * 0 = ok (was absent), 1 = was live and now cleared, -1 = io error. */
int spt0_drop(invfs_volume *v);

/* Query the save point state. out may be NULL. Returns 1 if live, 0 if not. */
int spt0_info(const invfs_volume *v, invfs_spt0 *out);

/* WP96: does the live save point's pin name any block of [pba, pba+n)?
 * vol_free_blocks calls this before it frees; 1 = at least one block is held
 * (it must stay allocated), 0 = free normally. A zero-cost no-op when no pin
 * is armed, which is every volume captured before WP96. */
int spt0_block_pinned(invfs_volume *v, uint64_t pba, uint64_t nblocks);

#endif /* INVFS_VOL_SPT0_H */
