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
 * Durability: append writes the record bytes, then barriers (the append is
 * the writer's critical section in the design). A torn record — crash or a
 * partial write — fails its CRC and ends the replay; everything before it
 * is kept, so the log is always a valid prefix. Segment creation follows
 * structure-before-reference: the header is durable before RT30 names it.
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
 * is full. 0 = ok, -1 = error/ENOSPC/record too large for a segment. */
int vol_delta_append(invfs_volume *v, const uint8_t *key, uint16_t klen,
                     const uint8_t *val, uint16_t vlen, uint16_t flags);

/* Point lookup of the winning record. 1 = found (*out filled), 0 = absent,
 * -1 = error/bad arguments. */
int vol_delta_lookup(invfs_volume *v, const uint8_t *key, uint16_t klen,
                     delta_ref *out);

/* Read a ref's value bytes. `cap` must be >= ref->vlen. On success *vlen_out
 * holds the value length and 0 is returned. A delete record has vlen 0. */
int vol_delta_read_value(invfs_volume *v, const delta_ref *ref,
                         uint8_t *buf, size_t cap, uint16_t *vlen_out);

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
 * call; it must not be retained after the cursor returns. O(k log k) in the
 * number of matching keys, so a small recent tier stays cheap. */
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
