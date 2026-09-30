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

/* WP86: containment walk. Same verification as btree_check, except an
 * UNREADABLE page (bad CRC/gen/encoding) is recorded as a quarantined key
 * range and skipped so the rest of the tree is still verified -- one torn
 * page must not hide the state of the other 57. A structural failure (cycle,
 * shared child, level or ordering violation) is NOT contained: that is a tree
 * bug, not media damage, and it still returns -1. `q` (may be NULL) receives
 * the quarantined ranges, the number of bad pages and a `qfull` flag when
 * there were more bad pages than the range array can hold (the repair must
 * refuse in that case rather than excise part of the damage). */
#define BT_QUARANTINE_MAX      64u
/* The longest key any v3 namespace can produce: a dirent key is
 * parent:u64 + name_len:u16 + name, an xattr key 0x03 + inode:u64 +
 * name_len:u16 + name -- 11 + INVFS_MAX_NAME(255) is the worst case. Anything
 * longer cannot come from this engine; a range whose bound exceeds it is
 * reported as undescribable and the repair is refused rather than guessed. */
#define BT_QUARANTINE_KEY_MAX  272u
typedef struct {
    uint64_t pba;                            /* the page that failed */
    uint8_t  lo[BT_QUARANTINE_KEY_MAX];      /* inclusive */
    uint16_t lo_n;
    uint8_t  hi[BT_QUARANTINE_KEY_MAX];      /* exclusive; see hi_unbounded */
    uint16_t hi_n;
    int      hi_unbounded;                   /* the rightmost range */
} bt_range;
typedef struct {
    bt_range range[BT_QUARANTINE_MAX];
    int      n;
    uint64_t bad_pages;
    /* 1 = the set is not complete/actionable: more bad pages than the array
     * holds, or a key range longer than BT_QUARANTINE_KEY_MAX. The report is
     * then partial and btree_excise must not run. */
    int      qfull;
} bt_quarantine;
int btree_check_tolerant(invfs_volume *v, invfs_blkptr root,
                         bt_stat *out, bt_quarantine *q,
                         char *err, size_t errlen);

/* WP86: drop the quarantined key ranges from the tree, copy-on-write. The
 * parent entry that points at the unreadable page is removed (and a node left
 * with a single child is collapsed into its parent), so every other key keeps
 * its page: this is O(height) page writes, not a base rebuild, and it cannot
 * lose a key that is still readable. The new root is returned in
 * *new_root_out; the caller barriers and publishes it through RT30 and then
 * reclaims the pages the dropped subtrees held.
 *
 * A key inside an excised range stops being EIO and becomes "absent" -- the
 * bytes are gone. That is why this is an explicit repair, never a read-path
 * fallback: the caller is expected to re-apply the delta afterwards (a fold
 * restores every quarantined key the delta still holds) and to report the
 * loss. 0 = nothing to do, 1 = the tree changed (*new_root_out is new),
 * -1 = io/alloc/encode error (the input tree is untouched). */
int btree_excise(invfs_volume *v, invfs_blkptr root, const bt_quarantine *q,
                 invfs_blkptr *new_root_out);

/* WP: is a quarantined key RANGE still holding something a live object needs?
 *
 * btree_excise drops a key INTERVAL, not a page, and the only thing its
 * caller ever learns about an unreadable page is that interval (from the
 * parent's separators). So "is it safe to drop this interval" is exactly
 * "does a key I can PROVE must exist fall inside it" -- which is what these
 * two answer. Byte-lexicographic, the engine's own key order; both bounds
 * behave as [lo, hi) and a zero-length `hi` means unbounded (+infinity). */
int btree_quarantine_has(const bt_quarantine *q, const uint8_t *k, uint16_t klen);
int btree_quarantine_overlaps(const bt_quarantine *q,
                              const uint8_t *lo, uint16_t lo_n,
                              const uint8_t *hi, uint16_t hi_n);

/* Reachability diff (design §8, D4): free every page reachable from
 * old_root but not from keep_root, via the WP-M2 allocator. Returns the number
 * of pages freed, or -1 on a walk/read error. This is the primitive WP-M15
 * schedules against {current base, pinned save-point root}; it does not itself
 * decide when a reader has drained an old root. */
int btree_reclaim(invfs_volume *v, invfs_blkptr old_root, invfs_blkptr keep_root);

/* WP77: reachability diff that also keeps every page reachable from
 * pinned_root (a live save point's base). pinned_root.pba == 0 behaves
 * exactly like btree_reclaim. */
int btree_reclaim_pinned(invfs_volume *v, invfs_blkptr old_root,
                         invfs_blkptr keep_root, invfs_blkptr pinned_root);

/* WP121: the FULL-POOL orphan collector. Unlike btree_reclaim_pinned this
 * is not a generation diff -- it walks every allocated block in the volume
 * and frees each one that is a v3 base page (BPG3, CRC-valid) and is not
 * reachable from ANY root named by the 2 RT30 slots, nor from
 * v->pinned_root. That "ANY of the 2 slots" is the whole safety argument:
 * mbuf_root_publish writes one slot per publish (vol_metabuf.c:349) and
 * mbuf_root_read falls back to the other when the newer root fails
 * mbuf_page_validate. A depth-2 root stack bounds the leak; it does not make
 * freeing safe.
 *
 * WP-D added the reader-side backstop, which is why the collector is no
 * longer the ONLY thing standing between a wrong predicate and a silent
 * adoption: mbuf_read_ptr refuses any page whose block the bitmap reports
 * free, so a freed subtree now fails the walk with EIO instead of being
 * read, and mbuf_root_read refuses a freed slot. That converts a silent
 * corruption into a loud one; it does not make freeing safe, which is why
 * the liveness predicate below is still the load-bearing part.
 *
 * Caller contract: quiescent. No COW mutation may be in flight (the caller
 * holds the volume write lock and is between publications), because an
 * uncommitted copy is protected only by the gen ceiling, not by a root.
 *
 * Returns 0 on success (freed_out, if non-NULL, holds the page count --
 * 0 when nothing was collected, including every refuse-to-run case), -1 on
 * an io/alloc error. It never reports "collected some but the live set is
 * uncertain": an RT30 slot it cannot turn into a valid blkptr makes it
 * return 0 having freed nothing. */
int btree_collect_orphans(invfs_volume *v, uint64_t *freed_out);

/* WP126: the SAME collector with a BOUNDED, INCREMENTAL cost.
 *
 * WP121's btree_collect_orphans() above walks the whole block space on
 * every call, which is one 4 KiB block read per ALLOCATED block: measured
 * 48,732 reads on one Silesia volume, per call, and fold_reclaim_hook calls
 * it on every fold. This entry point is what the fold path uses instead. It
 * keeps WP121's liveness predicate byte-for-byte -- same RT30-slot set, same
 * pinned_root, same gen ceiling, same BPG3-only candidacy, same refuse-to-
 * run on a half-known RT30 -- and changes only WHICH blocks it looks at per
 * call and HOW MANY reads that costs. It never frees a page the full-pool
 * sweep would have kept, and never keeps one the full-pool sweep would have
 * freed, except for pages the budget had not yet reached this call.
 *
 * The cost is bounded by construction: at most `budget` block reads for the
 * one-pass seed scan of pre-existing pages, plus one read per block in the
 * candidate set (allocated base pages this handle knows about), plus the
 * two RT30 tree walks -- and the tree walks happen ONLY on a call that
 * actually found a freeable candidate, so a fold that finds nothing costs
 * the budget and nothing else. All three terms are functions of the
 * METADATA, not of the volume.
 *
 * Returns 0 on success (freed_out, if non-NULL, holds the page count freed
 * by THIS call) or -1 on an io/alloc error. Same refuse-to-run contract as
 * above. */
int btree_collect_orphans_incr(invfs_volume *v, uint64_t *freed_out);

/* WP126: drain btree_collect_orphans_incr() until it is SETTLED -- the seed
 * pass has wrapped the block space and a whole call over a non-empty
 * candidate set freed nothing. This is the OFFLINE SWEEP's entry point: a
 * sweep is a maintenance operation with no latency budget, so it should
 * collect everything collectable, and this is how it does that while the
 * per-FOLD cost stays bounded. Same return contract. */
int btree_collect_orphans_full(invfs_volume *v, uint64_t *freed_out);

/* WP126: feed the candidate set. Every mbuf_alloc() that produces a v3 base
 * page calls this with the pba it returned, so the collector knows about
 * that page without having to find it in the block space. A page that is
 * never announced simply is not a candidate yet -- a leak, never a
 * wrong free. Idempotent, allocation-failure tolerant (it then finds the
 * page the slow way), and safe to call with pba == 0. */
void btree_orphan_note_alloc(invfs_volume *v, uint64_t pba);

/* WP126: the counters the WP126 report is measured from. Purely
 * observational: nothing in the collector's behaviour depends on them. */
struct invfs_orphan_stats {
    uint64_t calls;       /* collector invocations on this handle */
    uint64_t cand_reads;  /* block reads over the candidate set, cumulative */
    uint64_t seed_reads;  /* block reads spent on the one-pass seed scan */
    uint64_t mark_reads;  /* block reads walking the two RT30 trees + pin */
    uint64_t freed;       /* base pages freed, cumulative */
    uint64_t cands;       /* candidate-set size examined on the last call */
    uint64_t peak;        /* largest candidate-set size seen */
    int      settled;     /* 1 = seed pass done AND last call freed nothing */
};

int btree_orphan_stats(const invfs_volume *v, struct invfs_orphan_stats *out);

#endif /* INVFS_VOL_BTREE_H */
