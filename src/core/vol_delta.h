/* vol_delta.h — WP-M10: v3 delta log (the recent-changes tier).
 *
 * The stable tier (WP-M5/M6/M7) is the immutable base B+-tree. This module
 * owns the other half of the metadata-v3 design (design-meta-v3.md §2/§4/
 * §7/§12, decision D1): an append-only, coalescing on-disk log of namespace
 * mutations plus the in-memory index that makes the latest record per key a
 * single probe. Mount replay rebuilds that index from the durable segment
 * chain so the recent tier survives a crash/remount.
 *
 * What this WP does:   append records (per-record CRC32C), coalesce them by
 *                      namespace key in an open-addressing index, chain and
 *                      roll over fixed-size segments, replay the chain at
 *                      mount, and truncate a torn tail at the last valid
 *                      record.
 * What this WP does NOT do (deliberately):
 *   - overlay reads / delete shadowing in the read path (WP-M11);
 *   - wiring chmod/unlink/rename/xattr into the delta (WP-M12);
 *   - fold into the base tree (WP-M14) or reclaim of superseded segments
 *     (WP-M15); this WP never frees a segment.
 *
 * The record + segment wire format is frozen in invarifs.h (WP-M10). A
 * segment is a fixed INVFS_DELTA_SEG_BLOCKS (32-block / 128 KiB) run of
 * metadata blocks with an invfs_delta_seg_hdr at the front; records are
 * appended at the bump cursor. The chain is walked toward the oldest segment
 * via prev_pba and the active (newest) segment is named by RT30.delta_pba.
 *
 * Durability: append writes the record bytes, then fences them with the
 * group's barrier (WP602 group commit: one barrier per bounded group, not
 * per record -- see vol_delta_append/vol_delta_drain). A torn record --
 * crash or a partial write -- fails its CRC and ends the replay; everything
 * before it is kept, so the log is always a valid prefix. Segment creation
 * follows structure-before-reference: the header is durable before RT30
 * names it.
 */
#ifndef INVFS_VOL_DELTA_H
#define INVFS_VOL_DELTA_H

#include <stdint.h>
#include <stddef.h>

#include "invarifs.h"

/* Same stale-include-guard caveat as vol_metabuf.h/vol_btree.h: volume.h's
 * guard closes before the file ends, so forward-declare rather than include
 * it twice. */
typedef struct invfs_volume invfs_volume;

/* Chain walk guard: a corrupt prev_pba cycle must not loop forever. */
#define DELTA_MAX_SEGMENTS (1u << 20)

/* A reference to the winning on-disk record for one key. `seg` is the
 * segment pba and `off` the byte offset of the record header within it; use
 * vol_delta_read_value to fetch the value bytes. `seq` is the monotone
 * append sequence (higher = newer) and `flags` carries
 * INVFS_DELTA_FLAG_DELETE. */
typedef struct {
    uint64_t seg;
    uint64_t off;
    uint64_t seq;
    uint16_t flags;
    uint16_t vlen;
} delta_ref;

/* ---- lifecycle -------------------------------------------------------- */

/* Replay the segment chain named by RT30.delta_pba into the in-memory index
 * and position the active-segment bump cursor. Idempotent: an existing index
 * is dropped first. A missing descriptor/segment yields an empty index; a
 * torn tail is truncated at the last CRC-valid record (a warning is logged).
 * 0 = ok, -1 = I/O error. Callers must have RT30 loaded (vol_open does). */
int vol_delta_mount(invfs_volume *v);
/* Drop the index and segment state. Safe on a NULL index / v2 handle. */
void vol_delta_close(invfs_volume *v);

/* ---- append + lookup -------------------------------------------------- */

/* Append one record (flags bit0 delete; val may be NULL only when vlen 0).
 * Coalesces by key: the new record becomes the index entry for `key` even
 * when it is a delete marker. Rolls to a fresh segment when the active one
 * is full. 0 = ok, -1 = error/ENOSPC/record too large for a segment.
 *
 * WP602 group commit: 0 means STAGED, not necessarily durable. The record
 * bytes are on the device (its pwrite returned) and the index names it
 * immediately, so the session reads its own writes; durability arrives
 * with the barrier that closes the group -- the count/byte cap auto-drain
 * inside a later append, vol_delta_drain(), or any durability promise the
 * caller already holds (vol_sync, vol_flush, close, fold/checkpoint
 * barriers all fence the whole handle, staged records included). A group
 * is bounded by delta_group_max records / delta_group_bytes bytes
 * (INVFS_DELTA_GROUP_MAX / INVFS_DELTA_GROUP_BYTES at mount, defaults 64 /
 * 256 KiB; a count cap of 1 is the legacy per-record barrier), so the crash
 * window is one group, never the volume. Prefixes of a group are exactly
 * the crash states per-record barriers could already produce (replay keeps
 * the CRC-valid prefix; a row naming a lost blob reads EIO, never corrupt),
 * and a refused barrier fails the append, so a 0 was never issued past
 * bytes of unknown fate. WP605: staged bytes are retained until fenced,
 * and the next fence after any failure healed them -- by rewriting the
 * retained bytes, which resurrected mutations whose caller saw FAIL (leg5
 * GHOST). WP606: a fence failure DOOMS the retained set instead (overlay
 * rolled back at once, mutation bytes freed), and the next fence heals by
 * writing INVFS_DELTA_FLAG_PAD tombstones over the doomed spans (exact
 * length, replay-neutral) while live post-failure stages are still
 * rewritten (drop-victim heal). Failed mutations stay dead, the log stays
 * continuous. Healing is best-effort; the barrier's result still decides
 * the ack. */
int vol_delta_append(invfs_volume *v, const uint8_t *key, uint16_t klen,
                     const uint8_t *val, uint16_t vlen, uint16_t flags);

/* WP606: doom the staged set: every live retained entry leaves pending,
 * its overlay ref rolls back to the pre-stage snapshot (or the slot is
 * dropped when the doomed stage created the key), its mutation bytes are
 * freed, and only off/len survive for the tombstone pad at the next fence.
 * Idempotent. Called on every fence failure the delta paths do not fence
 * themselves (vol_flush / vol_sync barriers); the append/drain paths doom
 * inline under the lock they already hold. */
void vol_delta_doom(invfs_volume *v);

/* WP607: defer content frees to fence time. The caller just staged a row
 * mutation (supersede or delete) that retires content; the blocks are
 * freed at the first successful fence covering the mutation, and only
 * while the mutation is still live (span/seq match for supersedes, DELETE
 * tombstone still up for deletes). An unwound mutation drops its entry and
 * the blocks
 * stay allocated -- the s35/s23 use-after-free class cannot form. The
 * recipe form covers row-supersede sites (create/rename-victim/unlink);
 * the pba form covers the commit path (copies its pba/keep sets).
 * Best-effort: over-cap entries leak to the sweep orphan collector.
 * Callers must NOT hold g_delta_lock (span capture takes it). */
void vol_retire_recipe(invfs_volume *v, uint64_t inode_id,
                       const uint8_t recipe_addr[INVFS_RECIPE_ADDR_LEN],
                       int del);
void vol_retire_pbas(invfs_volume *v, uint64_t inode_id,
                     const uint64_t *pbas, size_t npbas,
                     const uint64_t *keep, size_t nkeep);
/* WP606: encode tombstone pad(s) over exactly `len` bytes at `out`
 * (caller provides `len` bytes). Returns `len`, or 0 when `len` cannot
 * hold a pad. Unit-tested (delta_test): the total is always exact. */
size_t vol_delta_pad_build(uint8_t *out, size_t len);
/* WP607: heal retained spans ahead of a fence the delta paths do not own.
 * Every successful barrier flushes ALL dirty pages device-wide --
 * including doomed spans whose pads were never written. A fence that does
 * not heal first (close-flush, fold, anchor) commits failed mutations raw
 * (the leg5 r17 remount-ghost: doomed at error time, fenced raw at gate
 * unmount). No-op when nothing needs healing; the caller's barrier still
 * decides the ack. Takes g_delta_lock. */
void vol_delta_heal(invfs_volume *v);

/* WP602: close the staged group. Issues one vmux_barrier when records were
 * staged since the last covering barrier and returns its result, so a 0 is
 * the group's durability ack (every staged record durable); 0 with nothing
 * staged (a covering barrier may already have fenced them -- vol_sync's
 * flush path does). -1 = the barrier failed, nothing is claimed. */
int vol_delta_drain(invfs_volume *v);
/* WP602: records staged since the last covering barrier (0 when the group
 * is closed). Tests / crash probes, not a durability query. */
uint64_t vol_delta_pending(invfs_volume *v);

/* Point lookup of the winning record. 1 = found (*out filled), 0 = absent,
 * -1 = error/bad arguments.
 *
 * A ref it returns names a BLOCK RANGE, and the fold frees the retired
 * chain's blocks the moment it drops the index. So a ref does not outlive
 * this call safely: read it with vol_delta_read_value only if you already
 * know the bytes are pinned, and prefer vol_delta_lookup_value below, which
 * cannot get this wrong by construction. */
int vol_delta_lookup(invfs_volume *v, const uint8_t *key, uint16_t klen,
                     delta_ref *out);

/* Resolve a key AND read its value in ONE critical section. This is how an
 * overlay read is meant to reach a delta value: splitting the lookup and the
 * read leaves a fold free to run in between, and the fold's tail frees the
 * blocks the ref names.
 *
 *   1  = found. *flags_out carries INVFS_DELTA_FLAG_DELETE for a shadowing
 *        delete; *vlen_out is the value length and the value is in buf.
 *   0  = miss.
 *  -1  = I/O error / bad arguments.
 *  -2  = found, but the value does not fit `cap`: *vlen_out still holds its
 *        length and buf is untouched. Size a buffer and call again; that call
 *        is atomic too (if a fold completed in between, the key is in the
 *        base, which the caller falls through to).
 */
int vol_delta_lookup_value(invfs_volume *v, const uint8_t *key, uint16_t klen,
                           uint8_t *buf, size_t cap, uint16_t *flags_out,
                           uint16_t *vlen_out);

/* Read a ref's value bytes. `cap` must be >= ref->vlen. On success *vlen_out
 * holds the value length and 0 is returned. A delete record has vlen 0.
 *
 * HOLDS THE DELTA LOCK across both preads. That protects the blocks from a
 * free IN PROGRESS -- not from a free that already happened: a ref resolved
 * before a fold can name blocks the fold has since recycled, which is how the
 * read path used to answer -1 for a live inode. Prefer vol_delta_lookup_value,
 * which never lets a ref escape. */
int vol_delta_read_value(invfs_volume *v, const delta_ref *ref,
                         uint8_t *buf, size_t cap, uint16_t *vlen_out);

/* Test seam, called from inside the critical section of whichever read a
 * caller used, with the ref being read. Weak (see vol_delta.c): a no-op
 * unless a test defines its own, which is how fold_delta_read_test forces the
 * interleave instead of racing for it. */
void invfs_test_delta_read_hook(const void *ref);

/* ---- locking helpers (for callers that mutate delta state) ------------- */

/* Lock the delta append mutex. Call before vol_fold or any operation
 * that rebuilds the delta_index. */
void vol_delta_lock(void);

/* Unlock the delta append mutex. */
void vol_delta_unlock(void);

/* ---- introspection (tests / diagnostics) ------------------------------ */

/* Iterate every indexed key in unspecified order. cb returns non-zero to
 * abort (propagated). 0 = complete. The key pointer is valid only for the
 * duration of the callback. */
typedef int (*vol_delta_iter_cb)(void *ctx, const uint8_t *key, uint16_t klen,
                                 const delta_ref *ref);
int vol_delta_iter(invfs_volume *v, vol_delta_iter_cb cb, void *ctx);

/* ---- WP-M11: ordered range cursor (the readdir merge's delta stream) --- */

/* Visit every indexed key in the half-open byte range [lo, hi) in ascending
 * byte-lexicographic order (the same ordering btree_scan uses), one callback
 * per key (the index already coalesces to the latest record). A zero-length
 * `lo` is unbounded below and a zero-length `hi` is unbounded above -- the
 * btree_scan convention; keys are never empty. cb returns non-zero to abort
 * and that value is propagated. 0 = complete, -1 = bad arguments. The key
 * pointer passed to cb is the index's owned copy and is valid for the whole
 * call; it must not be retained after the cursor returns. WP117: the query
 * runs over a per-thread cached key ordering of the index, so it costs
 * O(log G + k) in the indexed keys G and the k matches -- not O(G) in the
 * hash table's capacity. */
typedef int (*vol_delta_range_cb)(void *ctx, const uint8_t *key, uint16_t klen,
                                  const delta_ref *ref);
int vol_delta_range(invfs_volume *v,
                    const uint8_t *lo, uint16_t lolen,
                    const uint8_t *hi, uint16_t hilen,
                    vol_delta_range_cb cb, void *ctx);

/* Distinct keys currently indexed. */
uint64_t vol_delta_count(const invfs_volume *v);
/* records indexed / segments in the chain / payload bytes appended. */
void vol_delta_stats(const invfs_volume *v, uint64_t *records,
                     uint64_t *segments, uint64_t *bytes);

/* ---- WP-M16: delta truncation for save-point rollback ----------------- */

/* Truncate the delta chain to `delta_end` bytes from the chain start.
 * This drops all records appended after the save point was captured.
 * After truncation, the delta index is reset and must be rebuilt via
 * vol_delta_mount. 0 = ok, -1 = io error. */
int vol_delta_truncate(invfs_volume *v, uint64_t delta_end);

/* Read a delta segment header. Returns 0 on success, -1 on error. */
int delta_read_hdr(invfs_volume *v, uint64_t pba, invfs_delta_seg_hdr *out);

/* ---- pure record parser (unit tests + fuzz; no volume) ---------------- */

/* Parse one record at buf[off] within [0, len). Returns:
 *   1  = valid record; *rec_len bytes consumed, out params filled;
 *   0  = clean end (zeroed padding / not enough room for a header);
 *  -1  = malformed record (short, empty key, or CRC mismatch).
 * This never reads past `len`. */
int vol_delta_rec_parse(const uint8_t *buf, size_t len, size_t off,
                        uint16_t *klen, uint16_t *vlen, uint16_t *flags,
                        size_t *rec_len);

/* Scan a run of records and return the byte offset one past the last
 * CRC-valid record (the torn-tail truncation point). *nrec gets the number
 * of valid records; *saw_bad is set when replay stopped before the clean
 * end (torn/invalid tail). */
size_t vol_delta_scan_valid(const uint8_t *buf, size_t len,
                            uint64_t *nrec, int *saw_bad);

#endif /* INVFS_VOL_DELTA_H */
