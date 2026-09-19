/* vol_btree.h — WP-M3: immutable base B+-tree over the WP-M2 page format.
 *
 * The stable tier of the metadata-v3 design (design-meta-v3.md §3/§4/§5/§12)
 * is a B+-tree whose pages are WP-M2 base pages (4 KiB, 1:1 with blocks) and
 * whose nodes are addressed by invfs_blkptr. This header is the tree API the
 * other meta-v3 WPs build on: WP-M5 (inode rows), WP-M6 (dirents), WP-M11
 * (overlay reads), WP-M14 (fold), WP-M15 (reclaim).
 *
 * Invariants:
 *   - The tree is immutable between folds. Every mutating call is
 *     copy-on-write: it returns a new root and never changes the input tree.
 *   - Keys are byte-lexicographic (memcmp, shorter-prefix-first). This is the
 *     only ordering the design fixes ("ordered"); it makes the delta merge in
 *     WP-M6 well-defined.
 *   - `bt_key.n == 0` is the empty key. It is a real (smallest) key for
 *     search/upsert/delete; for btree_scan it doubles as "unbounded" on the
 *     low side and on the high side (keys in the v3 namespaces are never
 *     empty, so the two uses cannot collide).
 *   - The value returned by btree_search points into a per-thread buffer and
 *     is valid until the next btree_search on the same thread. The k/v passed
 *     to a bt_scan_cb are only valid for the duration of that callback.
 *
 * Durability: btree_upsert/btree_delete write and seal every new page but do
 * NOT barrier or publish. The caller writes the COW pages, barriers
 * (vmux_barrier), and only then publishes the new root through the RT30
 * double slot (mbuf_root_publish) — the structure-before-reference ordering
 * WP-M3's spec fixes. They also do not free the superseded pages: with COW a
 * retired page may still be reachable from a retained root (a save point or an
 * in-flight reader), so freeing is the reachability diff in btree_reclaim,
 * scheduled by WP-M15.
 */
#ifndef INVFS_VOL_BTREE_H
#define INVFS_VOL_BTREE_H

#include <stdint.h>
#include <stddef.h>

#include "invarifs.h"

/* Same stale-include-guard caveat as vol_metabuf.h: volume.h's guard closes
 * before the file ends, so forward-declare rather than include it twice. */
typedef struct invfs_volume invfs_volume;

/* An ordered key / opaque value. Both are borrowed byte ranges. */
typedef struct { const uint8_t *p; uint16_t n; } bt_key;
typedef struct { const uint8_t *p; uint16_t n; } bt_val;

/* Point lookup. `root.pba == 0` is the empty tree. 0 = ok (see *found),
 * -1 = I/O or a bad page/blkptr (torn CRC, gen mismatch, malformed page).
 * On success *val_out points into a per-thread buffer (see header note). */
int btree_search(invfs_volume *v, invfs_blkptr root,
                 bt_key key, bt_val *val_out, int *found);

/* Copy-on-write insert or replace. Returns the new root in *new_root_out
 * (pba == 0 only for an empty tree, which upsert never produces). Replacing
 * an existing key overwrites its value in place in the copied leaf. */
int btree_upsert(invfs_volume *v, invfs_blkptr root, bt_key key,
                 bt_val val, invfs_blkptr *new_root_out);

/* Copy-on-write physical delete (no tombstone). Missing key returns the input
 * root unchanged without writing. Underflow redistributes with or merges into
 * a sibling (byte half-full minimum; the design leaves the fill factor open,
 * see vol_btree.c). An emptied root collapses; *new_root_out.pba may be 0. */
int btree_delete(invfs_volume *v, invfs_blkptr root, bt_key key,
                 invfs_blkptr *new_root_out);

/* Ordered range scan over packed leaf entries. The callback is invoked in
 * ascending key order for every k with lo <= k < hi (lo.n==0 = unbounded low,
 * hi.n==0 = unbounded high). A non-zero return from cb aborts the scan and is
 * propagated as btree_scan's result; 0 continues. Returns 0 when the whole
 * range was delivered. */
typedef int (*bt_scan_cb)(void *ctx, bt_key k, bt_val v);
int btree_scan(invfs_volume *v, invfs_blkptr root,
               bt_key lo, bt_key hi, bt_scan_cb cb, void *ctx);

/* Structural introspection: page/leaf-key counts and tree height (leaf = 1).
 * btree_check walks the whole tree and verifies page CRCs, level monotonicity,
 * strict key order and parent/child separator consistency, and detects cycles
 * or a shared child (a COW bug). 0 = structurally valid (stats in *out);
 * -1 = invalid, with a short reason in err. `out` and `err` may be NULL. */
typedef struct {
    uint64_t n_pages;
    uint64_t nkeys;
    uint32_t height;
} bt_stat;
int btree_check(invfs_volume *v, invfs_blkptr root,
                bt_stat *out, char *err, size_t errlen);

/* Reachability diff (design §8, D4): free every page reachable from
 * old_root but not from keep_root, via the WP-M2 allocator. Returns the number
 * of pages freed, or -1 on a walk/read error. This is the primitive WP-M15
 * schedules against {current base, pinned save-point root}; it does not itself
 * decide when a reader has drained an old root. */
int btree_reclaim(invfs_volume *v, invfs_blkptr old_root, invfs_blkptr keep_root);

#endif /* INVFS_VOL_BTREE_H */
