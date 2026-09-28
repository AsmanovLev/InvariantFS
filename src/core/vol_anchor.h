/* vol_anchor.h — the ANC0 tail anchor: a redundant LOCATION for the two
 * block-0 descriptors that decide whether a v3 volume can be opened.
 *
 * The reasoning, and the reason it is a location and not parity, is in
 * invarifs.h next to the descriptor. This header is the API.
 *
 * THREE RULES this module exists to enforce, and which the rest of the tree
 * depends on:
 *
 *  1. THE TAIL IS OURS ONLY IF WE WROTE IT. anchor_pba() is a position, not a
 *     claim. Whether the block is an anchor is decided at open by a probe
 *     that must pass the fingerprint, and until that probe succeeds the
 *     refresh path is a no-op. Every volume made before this change has an
 *     ordinary data block in that position, and a refresh that adopted or
 *     created it would overwrite a file. This is the whole backward-
 *     compatibility story and it is one boolean.
 *  2. A REFUSAL IS NOT AN ABSENCE. A stale, foreign or damaged anchor is
 *     reported as its own state, so an operator can tell "this volume has
 *     never had an anchor" from "this volume HAS one and it did not match".
 *  3. A FALLBACK THAT FIRES SILENTLY IS WORSE THAN NO FALLBACK. Every
 *     adoption prints, once per open, naming the source.
 */
#ifndef INVFS_VOL_ANCHOR_H
#define INVFS_VOL_ANCHOR_H

#include <stdint.h>

#include "invarifs.h"

typedef struct invfs_volume invfs_volume;

/* Probe verdicts. Only INVFS_ANCHOR_OK makes the mirror usable; only
 * INVFS_ANCHOR_ABSENT means "this volume never had one". */
#define INVFS_ANCHOR_OK                0
#define INVFS_ANCHOR_ABSENT            1   /* no ANC0 magic: pre-anchor volume */
#define INVFS_ANCHOR_REFUSED_GEOMETRY  2   /* present + valid, but not THIS image */
#define INVFS_ANCHOR_REFUSED_DAMAGE    3   /* named but version/CRC fails */
#define INVFS_ANCHOR_IO                (-1)

/* The anchor block: total_blocks - 1, or 0 when the volume is too small to
 * have one. Deliberately computable from the device size alone. */
uint64_t anchor_pba(const invfs_volume *v);

/* CRC over an anchor descriptor with its crc32c field read 0 (the RT30 / SPT0
 * convention). */
uint32_t anchor_crc(const invfs_anc0 *a);
/* Fingerprint check, one arm at a time. `uuid` may be NULL, in which case
 * the uuid arm is skipped -- that shape exists for the raw-block tooling,
 * which has no superblock in hand, not for the open path, which must check
 * all four. */
int      anchor_fp_matches(const invfs_anc0 *a, uint64_t total_blocks,
                           uint32_t block_size, uint32_t format_version,
                           const uint8_t *uuid);
/* Verdict for a descriptor already read off the media. */
int      anchor_state_of(const invfs_anc0 *a);

/* Fill `out` from the volume's live descriptor state. Pure: touches no I/O. */
void     anchor_build(const invfs_volume *v, invfs_anc0 *out);
/* Write `a` at block `pba` of a raw block-device handle, then barrier. This
 * is the shape invf-mkfs needs: mkfs holds a blkio, not an open volume, and
 * on a two-device volume the anchor's block is LOCAL to dev1. */
int      anchor_write_raw(void *io, uint64_t pba, const invfs_anc0 *a);

/* Read the anchor off an open volume and classify it. The verdict is left in
 * v->anchor_state; `out` is filled only on INVFS_ANCHOR_OK. */
int      anchor_probe(invfs_volume *v, invfs_anc0 *out);

/* Rewrite the anchor from the volume's live state, if and only if the open
 * probe found one (rule 1). Returns 0 when the refresh happened or was
 * correctly skipped, -1 when the write failed -- in which case it has
 * already said so out loud and set the sticky flag fsck reports. */
int      anchor_refresh(invfs_volume *v);

/* Zero the anchor block and forget the volume's claim on it. Used by the
 * resize commit, which changes total_blocks and therefore MOVES the anchor's
 * address. `a` is the descriptor that was read at the OLD address, or NULL
 * if there was none. */
int      anchor_invalidate_at(invfs_volume *v, uint64_t old_pba,
                             const invfs_anc0 *a);

/* Diagnostics. */
int      anchor_adopted(const invfs_volume *v);   /* 1 = the mirror was used */
int      anchor_refresh_failed(const invfs_volume *v);
const char *anchor_state_name(int state);

#endif /* INVFS_VOL_ANCHOR_H */

/* The anchor block is the last block of the device (anchor_pba). It lies INSIDE
 * the shadow zone -- shadow_zone_start + shadow_zone_blocks == total_blocks for
 * a single-device volume -- so the ordinary allocator's bitmap test would hand
 * it out like any other free block. Reserving its bit in the bitmap at mkfs is
 * not enough on its own: the bitmap is a DERIVED cache (AGENTS.md 2.4) and a
 * rebuild from replay leaves the anchor free, because no delta record ever
 * names it. That is not a hypothetical -- it is the exact situation the anchor
 * exists for, since repairing a damaged volume is when the bitmap gets
 * rebuilt. So the exclusion is structural: every allocator test goes through
 * this, and the property holds even with no bitmap at all.
 *
 * Returns 1 if `pba` may be handed out, 0 if it is the anchor (or is otherwise
 * not a real block). */
int anchor_block_protected(const invfs_volume *v, uint64_t pba);
