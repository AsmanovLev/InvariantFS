/* vol_fold.c — WP-M14: fold (merge the delta into the base).
 *
 * The design (design-meta-v3.md §5) is a lazy compaction: apply every live
 * delta record to a COW copy of the base B+-tree, publish the new root
 * atomically through the WP-M2 double slot, then reset the delta. It bounds
 * the recent tier (the D1 mount-latency risk) and is what turns unlink into
 * real reclamation -- earlier coalesced records for a key simply drop instead
 * of becoming tombstones (§1). Fold is NOT required for correctness: the
 * delta may grow until the D2 trigger; only read latency and space depend on
 * it.
 *
 * -------------------------------------------------------------- ordering
 * The frozen publish/reset order (§5):
 *
 *   1. apply the live keys to the base, writing COW pages (no publish yet);
 *   2. barrier the COW pages + the dirty bitmap, then publish the new root
 *      with a bumped RT30 seq (mbuf_root_publish, CRC + higher-gen wins);
 *   3. only THEN reset the delta (new empty segment, clear the index) AND free
 *      the retired chain -- both inside g_delta_lock.
 *
 * A crash between 2 and 3 replays the OLD delta against the NEW base; every
 * key was already applied, so re-applying is idempotent (upsert of the same
 * value, delete of an absent key) and add-before-remove holds across the
 * window. A crash before 2 leaves the pre-fold base + the full delta.
 *
 * A lock-free reader resolves delta-first-then-base. If it sees the key in
 * the delta, that record is the (or a newer) value; if it misses the delta,
 * the key must already be in the published base -- so no transient ENOENT is
 * observable, and no read lock is needed on either structure. "Lock-free" is a
 * claim about the BASE, whose pages are immutable between folds. The recent
 * tier is NOT: step 3 frees it. A resolved delta_ref names bytes only for as
 * long as g_delta_lock is held, which is why step 3 holds that lock across the
 * free and why vol_delta_read_value holds it across the value read
 * (WP-inode-get-fold-race). The two halves are useless apart: either one alone
 * leaves the reader reading a block the volume has already given back.
 *
 * -------------------------------------------------------------- reclaim
 * No mutating B+-tree call frees a page (vol_btree.c), so fold RETAINS every
 * page the pre-fold root referenced. That is intentional here: with COW a
 * retired page may still be reachable from a retained root (a save point or
 * an in-flight reader), and the design (§8, D4) makes freeing a reachability
 * diff. WP-M15 supplies that diff against {current base, pinned save-point
 * root}; this WP only leaves it a clean hook (fold_reclaim_hook below). The
 * delta's superseded segments ARE freed -- under the lock, for the reason
 * above.
 *
 * The base side of that same sentence is now PROTECTED, by the reclaim reader
 * epoch (wp/reclaim-blocking-drain). It used to be a known gap: vol_reclaim_drain
 * waited on g_readers_in_flight and nothing in the tree ever incremented it, so
 * the drain returned immediately and fold_reclaim_hook freed a retired
 * generation with no regard for a reader that captured its root -- a reader
 * holding the root from two publishes ago had its pages collected and
 * mbuf_read_ptr reported it as -1 (src/core/vol_metabuf.c:192), measured at
 * 0-3 per ~3M reads over ~1500 folds (~1e-6). Every base-tree read now
 * announces itself BEFORE it captures the root and releases after the walk
 * (vol_btree.c: v3_base_get and the paired calls around each scan;
 * vol_spt0.c: spt0_capture), the drain really waits, and it is BOUNDED: at
 * the bound it returns -1 and this hook skips BOTH frees rather than doing
 * them unsafely.
 *
 * What that costs, stated plainly because it is the reason the drain is
 * bounded rather than merely correct: the drain blocks under the sweep's
 * g_io_lock, so a large vol_v3_dirent_scan or vol_v3_iter_live_inodes in
 * flight stalls other metadata work for the length of that walk. A missed
 * release is a mount-wide hang, not a leak -- the count is process-global and
 * the drain spins under g_io_lock -- which is why the bound exists and why it
 * is loud. The count is global while a volume is a per-handle object: fine
 * today (one vol_open per process), a cross-volume stall the day that stops
 * being true.
 *
 * -------------------------------------------------------------- trigger
 * D2 fixes the trigger SHAPE (size / record count / age, with the sweep as a
 * secondary actor) but deliberately gives no numbers; this WP measures and
 * records them.
 *
 * Measurement (this tree, 4 KiB base pages / ~200 B inode rows, a
 * workstation-class SSD): mount replay is ~2 us per on-disk record (parse +
 * index insert + CRC), so 64 MiB of delta -- the default byte budget -- is
 * ~128 MiB of segment bytes and ~0.25 s of replay; that is the point where a
 * cold mount stops being O(1) in practice. A record count of 262144 bounds
 * the RAM index at roughly 16 MiB. The age cap (1 hour) bounds the window in
 * which an idle volume keeps disjoint old records live, which matters mainly
 * to reclaim (WP-M15). These are diagnostics, not format: changing them
 * changes when fold runs, never what is on disk.
 */
#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_delta.h"
#include "vol_metabuf.h"
#include "vol_reclaim.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

/* Test hook (tools/test-meta-v3-fold.sh): die inside the publish/reset
 * window. "published" fires after the new base root is durable and published
 * but BEFORE the delta is reset -- the exact crash window the ordering rule
 * exists for. The next mount must replay the old delta against the new base
 * and land on identical content (re-applying is idempotent). Mirrors
 * INVFS_COMPACT_ABORT_AT / INVFS_RESIZE_ABORT_AT. */
static int fold_abort_at(const char *stage)
{
    const char *a = getenv("INVFS_FOLD_ABORT_AT");
    return a && strcmp(a, stage) == 0;
}

/* ---- D2 trigger thresholds (see the file header) ---------------------- */
#define FOLD_TRIGGER_BYTES   (64ull << 20)   /* 64 MiB of indexed payload */
#define FOLD_TRIGGER_RECORDS 262144ull       /* RAM index bound           */
#define FOLD_TRIGGER_AGE_S   3600ull         /* oldest record age cap     */

#define FOLD_FLAG_DELETE     ((uint16_t)INVFS_DELTA_FLAG_DELETE)

void vol_v3_fold_trigger(uint64_t *bytes, uint64_t *records, uint64_t *age_s)
{
    if (bytes)   *bytes   = FOLD_TRIGGER_BYTES;
    if (records) *records = FOLD_TRIGGER_RECORDS;
    if (age_s)   *age_s   = FOLD_TRIGGER_AGE_S;
}

/* CLOCK_MONOTONIC seconds, so an age is immune to wall-clock jumps. */
static uint64_t fold_now_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec;
}

/* Fold below the free-space / metadata-zone floor is refused: the COW path
 * allocates one page per touched node and, on ENOSPC, aborts mid-way with
 * those pages leaked. The trigger is a policy decision, so it may simply
 * decline and let the sweep (WP-M18) retry when pressure drops. Kept local
 * because the volume's hard_min/reserve policy lives in alloc_blocks and is
 * out of WP-M14's file scope. */
static int fold_space_ok(const invfs_volume *v)
{
    uint64_t floor = (uint64_t)v->sb.reserved_blocks +
                     (uint64_t)v->sb.hard_min_blocks;
    return v->free_blocks > floor;
}

/* ---------------------------------------------------------------- ordering */

/* One live delta record, copied out of the index before we mutate the base.
 * The index keeps the winning record per key; earlier coalesced records are
 * simply dropped (the §5 "no tombstone sweep" compaction). */
typedef struct {
    uint8_t  *key;
    uint16_t  klen;
    uint8_t  *val;      /* NULL for a delete record */
    uint16_t  vlen;
    uint16_t  flags;
} fold_ent;

typedef struct {
    fold_ent *ent;
    size_t    n, cap;
    int       oom;
    invfs_volume *v;    /* for vol_delta_read_value */
} fold_list;

static void fold_list_free(fold_list *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        free(l->ent[i].key);
        free(l->ent[i].val);
    }
    free(l->ent);
    l->ent = NULL;
    l->n = l->cap = 0;
}

/* Snapshot the winning record per key, value bytes included, so the base
 * writes below never issue delta I/O from inside a callback. Returns 0 or 1
 * (abort) -- vol_delta_iter propagates a non-zero return. */
static int fold_collect_cb(void *ctx_, const uint8_t *key, uint16_t klen,
                           const delta_ref *ref)
{
    fold_list *l = (fold_list *)ctx_;
    fold_ent *e;

    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 64;
        fold_ent *ne = (fold_ent *)realloc(l->ent, ncap * sizeof *ne);
        if (!ne) {
            l->oom = 1;
            return 1;
        }
        l->ent = ne;
        l->cap = ncap;
    }
    e = &l->ent[l->n];
    memset(e, 0, sizeof *e);
    e->key = (uint8_t *)malloc(klen ? klen : 1);
    if (!e->key) {
        l->oom = 1;
        return 1;
    }
    memcpy(e->key, key, klen);
    e->klen = klen;
    e->flags = ref->flags;
    l->n++;                              /* owned by the list from here */
    if (!(ref->flags & FOLD_FLAG_DELETE)) {
        e->vlen = ref->vlen;
        e->val = (uint8_t *)malloc(ref->vlen ? ref->vlen : 1);
        if (!e->val) {
            l->oom = 1;
            return 1;
        }
        if (ref->vlen) {
            uint16_t got = 0;
            if (vol_delta_read_value(l->v, ref, e->val, ref->vlen,
                                     &got) != 0 || got != ref->vlen) {
                l->oom = 1;              /* I/O failure aborts the fold */
                return 1;
            }
        }
    }
    return 0;
}

/* The ordering is vol_key_cmp() in volume_internal.h, the same one the
 * base B+-tree and the delta log are sorted with -- which is the whole
 * reason this sort may be trusted to make the pre-apply deterministic.
 * This sort is what puts the delta entries into base-key order. */

/* Stable insertion sort of the snapshot into key order (the list is small and
 * this keeps the owned pointers from being shuffled). */
static void fold_sort(fold_list *l)
{
    size_t a, b;
    for (a = 1; a < l->n; a++) {
        fold_ent tmp = l->ent[a];
        b = a;
        while (b > 0 && vol_key_cmp(tmp.key, tmp.klen,
                                    l->ent[b - 1].key,
                                    l->ent[b - 1].klen) < 0) {
            l->ent[b] = l->ent[b - 1];
            b--;
        }
        l->ent[b] = tmp;
    }
}

/* Persisting the dirty bitmap range is vol_v3_bitmap_flush() (vol_btree.c),
 * declared in volume_internal.h -- it was already exported for exactly this
 * reason. This file used to carry a byte-identical private copy; two copies
 * of "round the dirty range out to whole bitmap blocks, write it, clear the
 * range" is one fewer place a persistence decision can be made differently. */

/* Leaves room for WP-M15 to reclaim the pages the pre-fold root referenced
 * once the old root is no longer reachable from a save point or a reader.
 * WP-M14 deliberately frees nothing (see the file header); the arguments
 * survive so M15 changes only this function body.
 *
 * WP-M16: v->pinned_root holds the save-point's base_root if a save point
 * is live; folds must NOT reclaim pages reachable from pinned_root. */
void fold_reclaim_hook(invfs_volume *v, invfs_blkptr old_root,
                      invfs_blkptr new_root)
{
    /* Wait for in-flight readers (g_fold_epoch drain).
     *
     * This BLOCKS, and it blocks under the sweep's g_io_lock (the hook is
     * reached from vol_sweep.c under that lock), so a reader in the middle
     * of a large vol_v3_dirent_scan or vol_v3_iter_live_inodes stalls other
     * metadata work for the length of that walk. That is the known cost of
     * the decision, accepted deliberately: the alternative is freeing a
     * generation under a reader that captured its root, which is a read
     * that fails with no error having happened.
     *
     * A -1 means the drain gave up (a read path that never released). BOTH
     * frees are then skipped, and loudly: the space is not reclaimed this
     * fold, the next fold tries again, and the volume is left untouched
     * rather than damaged. Skipping only the diff and running the collector
     * would be worse -- the collector frees exactly the generations the
     * diff declined to, one generation behind. */
    if (vol_reclaim_drain(v) != 0) {
        fprintf(stderr, "fold: reclaim SKIPPED -- a base-tree reader "
                        "outlived the drain, so this generation is left "
                        "allocated rather than freed under it\n");
        return;
    }
    /* WP121: the fold has just published new_root, so the OTHER RT30 slot
     * still names old_root -- that is WP86's damage tolerance, and
     * mbuf_root_read adopts it whenever new_root's page fails
     * mbuf_page_validate. At the time, freeing old_root's exclusive pages
     * here did not make that fallback fail loudly: mbuf_page_validate
     * consults magic + CRC32C only, and free does not scrub, so the reader
     * adopted a namespace standing on reallocated blocks. That was silent
     * data loss, and it is what INCIDENTS.md's "RT30 fallback root can be
     * reclaimed while still live" entry describes.
     *
     * WP-D changed the consequence but NOT the reason for this guard. The
     * reader now consults the allocation bitmap -- mbuf_root_read for the
     * slot, mbuf_read_ptr for every page the fallback tree reaches -- so
     * freeing here would now produce EIO and a named RT30 diagnostic
     * instead of a silent adoption. That is still a volume that has lost
     * the namespace old_root's pages described, and a fallback root that
     * is reclaimed while still live is still a bug; the only thing WP-D
     * bought is that it stops hiding. Leaving this guard in place is also
     * still what keeps the collector below from being undone one line
     * earlier, every time.
     *
     * The guard is a refusal, not a repair: when the RT30 still names
     * old_root the diff has nothing safe to free, and the collector --
     * which keeps the union of both slots -- reclaims those pages the
     * next time a root is published and neither slot names them. */
    if (old_root.pba != 0 &&
        (old_root.pba == v->rt30.root_slot[0] ||
         old_root.pba == v->rt30.root_slot[1])) {
        (void)vol_reclaim_orphans(v, NULL);
        return;
    }
    /* reachability diff: free base pages in old_root not reachable from
     * new_root or the pinned save-point root */
    (void)vol_reclaim_mark_and_free(v, old_root, new_root, v->pinned_root);
    /* WP121: the diff above is one generation wide, so every root abandoned
     * by an earlier publish stays allocated forever. The collector is what
     * actually reclaims them; it is gated on INVFS_RECLAIM_ORPHANS=1 and is
     * a no-op otherwise.
     *
     * WP126: this is the call site that made the collector unusable on a
     * live root. It runs on EVERY fold, and WP121's collector cost one
     * block read per allocated block per call (48,732 on a 1 GB Silesia
     * volume), so the cost of a fold was proportional to the size of the
     * volume. The collector it reaches now is ONE bounded incremental
     * pass: at most a hard-capped slice of the one-pass seed scan (and
     * only until that scan has wrapped, once per open), plus one read per
     * known base page, plus the two RT30 tree walks -- and the tree walks
     * are paid only on a call that actually found something freeable. All
     * three terms are functions of the METADATA. The offline sweep drains
     * the same collector to settlement (vol_reclaim_orphans_full), which
     * is why this being bounded costs the sweep no compression. */
    (void)vol_reclaim_orphans(v, NULL);
}

/* Reset the recent tier to empty: an empty index and RT30 no longer naming a
 * delta segment. The old chain becomes unreachable from RT30 (its blocks and
 * the index's memory are WP-M15's to free). A crash right here leaves an
 * empty-but-valid recent tier against the new base, which is consistent. */
static int fold_delta_reset(invfs_volume *v)
{
    vol_delta_close(v);                  /* drop the index + segment state */
    v->rt30.delta_pba = 0;
    if (!v->rt30_present) {
        memset(&v->rt30, 0, sizeof v->rt30);
        memcpy(v->rt30.magic, "RT30", 4);
        v->rt30.version = INVFS_RT30_VERSION;
        v->rt30.page_size = INVFS_V3_PAGE_SIZE_DEFAULT;
        v->rt30_present = 1;
    }
    if (mbuf_rt30_store(v) != 0)
        return -1;
    if (vmux_barrier(v, "fold delta reset") < 0)
        return -1;
    /* Rebuild the empty index in memory. vol_delta_mount sees delta_pba == 0
     * and presents the empty recent tier. */
    if (vol_delta_mount(v) != 0)
        return -1;
    v->delta_oldest_seq = 0;
    v->delta_oldest_when = 0;
    return 0;
}

/* ---------------------------------------------------------------- fold */

int vol_v3_fold(invfs_volume *v)
{
    fold_list list;
    invfs_blkptr old_root, root, nr;
    bt_key key;
    bt_val val;
    size_t i;
    int rc = 0, failed_reset = 0;

    if (!v)
        return -1;
    if (!(v->sb.vol_flags & VOLF_V3))
        return -1;                       /* fold is a v3-only operation */
    if (v->sb.vol_flags & VOLF_READONLY)
        return -1;
    if (vol_delta_count(v) == 0)
        return 0;                        /* empty delta: a no-op */

    if (!fold_space_ok(v))
        return -1;

    memset(&list, 0, sizeof list);
    list.v = v;
    rc = vol_delta_iter(v, fold_collect_cb, &list);
    if (rc != 0 || list.oom) {
        fold_list_free(&list);
        return -1;                       /* pre-publish: volume untouched */
    }
    fold_sort(&list);

    if (vol_v3_base_root(v, &old_root) != 0) {
        fold_list_free(&list);
        return -1;
    }
    root = old_root;

    /* (1) apply every live record to a COW copy of the base. A delete whose
     * key is absent is a no-op; upsert of the same value is idempotent when a
     * crash forces a re-fold. */
    for (i = 0; i < list.n; i++) {
        fold_ent *e = &list.ent[i];
        key.p = e->key;
        key.n = e->klen;
        if (e->flags & FOLD_FLAG_DELETE) {
            int found = 0;
            val.p = NULL;
            val.n = 0;
            if (btree_search(v, root, key, &val, &found) != 0) {
                rc = -1;
                break;
            }
            if (!found)
                continue;
            if (btree_delete(v, root, key, &nr) != 0) {
                rc = -1;
                break;
            }
            root = nr;
        } else {
            val.p = e->val;
            val.n = e->vlen;
            if (btree_upsert(v, root, key, val, &nr) != 0) {
                rc = -1;
                break;
            }
            root = nr;
        }
    }
    if (rc != 0) {
        /* Pre-publish failure: RT30 still names old_root and the delta is
         * intact, so the volume is exactly pre-fold. The pages written so
         * far leak (WP-M15 reclaims nothing yet). */
        fold_list_free(&list);
        return -1;
    }

    /* An emptied tree (every base row deleted and no upsert) must still name
     * a durable root page (WP-M5's empty-leaf convention). */
    if (root.pba == 0) {
        uint8_t page[INVFS_BLOCK_SIZE];
        uint64_t gen = old_root.pba ? old_root.gen + 1 : 1;
        uint64_t pba = mbuf_alloc(v, gen);
        /* WP126: the emptied-tree root is a base page like any other. */
        btree_orphan_note_alloc(v, pba);
        if (!pba) {
            fold_list_free(&list);
            return -1;
        }
        mbuf_page_init(page, INVFS_PAGE_LEVEL_LEAF, gen);
        if (mbuf_write(v, pba, page) != 0) {
            mbuf_free(v, pba);
            fold_list_free(&list);
            return -1;
        }
        mbuf_ptr_set(&root, pba, page, INVFS_BP_LEAF | INVFS_BP_ROOT);
    }

    /* (2) durability + atomic publish: bitmap, then COW pages, then RT30 with
     * a bumped seq. mbuf_root_publish refuses a torn/gen-mismatched root. */
    if (vol_v3_bitmap_flush(v) != 0) {
        fold_list_free(&list);
        return -1;
    }
    if (vmux_barrier(v, "fold base pages") < 0) {
        fold_list_free(&list);
        return -1;
    }
    if (mbuf_root_publish(v, root.pba, root.gen) != 0) {
        fold_list_free(&list);
        return -1;
    }
#ifndef _WIN32
    if (fold_abort_at("published")) {
        vmux_barrier(v, "fold abort published");
        kill(getpid(), SIGKILL);
    }
#endif

    /* (3) reset the delta only after the new base is durable and named. A
     * failure here leaves base+delta both carrying the keys, which replay
     * resolves idempotently; a later fold_request retries.
     * WP-M20: hold g_delta_lock so concurrent lock-free readers see either
     * the old delta_index or the new empty one -- never a half-freed pointer.
     *
     * WP-inode-get-fold-race: THE CHAIN FREE BELONGS IN HERE. It used to run
     * two lines further down, OUTSIDE the lock, on the strength of the comment
     * above -- that a reader which resolved a ref under the lock has no use
     * left for the bytes once it drops the lock. It did have use left for
     * them: vol_delta_read_value reads a ref's value with two bare preads, and
     * this is the code that frees the very blocks that ref names the moment
     * the index is gone. The allocator hands them straight back
     * (delta_new_segment writes a header and zeros across a 128 KiB stripe),
     * so the reader decoded zeros and vol_v3_inode_get answered -1 for an
     * inode that was present, live and internally consistent -- ~1e-5 of all
     * reads, and a wrong row rather than an error whenever the recycled block
     * happened to hold one. Dropping the index and freeing what the index
     * named is ONE step as far as a reader is concerned, so it is one critical
     * section; vol_delta_read_value now takes this same lock across its
     * preads, which is what makes the pairing work. */
    uint64_t old_delta_pba = v->rt30.delta_pba;
    vol_delta_lock();
    if (fold_delta_reset(v) != 0)
        failed_reset = 1;
    /* WP-M15: free superseded delta segments (saved before reset cleared them) */
    (void)vol_reclaim_delta_segments(v, old_delta_pba, 0);
    vol_delta_unlock();

    /* WP-M15: bump epoch for reader drain, then reclaim base pages */
    (void)vol_reclaim_bump_epoch();
    fold_reclaim_hook(v, old_root, root);
    fold_list_free(&list);
    return failed_reset ? -1 : 0;
}

/* ---------------------------------------------------------------- trigger */

int vol_v3_fold_request(invfs_volume *v)
{
    uint64_t bytes = 0, records = 0, now;
    int need = 0;
    invfs_blkptr new_root;
    int rc;

    if (!v || !(v->sb.vol_flags & VOLF_V3))
        return -1;
    if (v->sb.vol_flags & VOLF_READONLY)
        return -1;
    if (vol_delta_count(v) == 0)
        return 0;
    vol_delta_stats(v, &records, NULL, &bytes);
    if (bytes >= FOLD_TRIGGER_BYTES || records >= FOLD_TRIGGER_RECORDS)
        need = 1;
    if (!need && v->delta_oldest_when) {
        now = fold_now_s();
        if (now >= v->delta_oldest_when &&
            now - v->delta_oldest_when >= FOLD_TRIGGER_AGE_S)
            need = 1;
    }
    if (!need)
        return 0;

    /* WP-M18: capture pre-fold root for reclaim_hook (called after fold) */
    if (vol_v3_base_root(v, &v->fold_pre_root) != 0)
        return -1;

    rc = vol_v3_fold(v);
    if (rc != 0)
        return -1;

    /* fold succeeded: call the reclaim hook with old and new roots */
    if (vol_v3_base_root(v, &new_root) == 0)
        fold_reclaim_hook(v, v->fold_pre_root, new_root);

    return 1;
}

void vol_v3_fold_reset_age(invfs_volume *v)
{
    if (v)
        v->delta_oldest_when = 0;
}

/* WP-M18: public reclaim trigger (M15 hook). Called by sweep integration
 * after a sweep pass + fold. Currently a no-op stub; M15 implements the
 * reachability diff. Idempotent: safe to call even if fold_request was
 * not called or fold was not needed. */
void vol_reclaim_schedule(invfs_volume *v)
{
    invfs_blkptr new_root;

    if (!v)
        return;
    if (v->fold_pre_root.pba == 0)
        return;
    if (vol_v3_base_root(v, &new_root) != 0)
        return;
    fold_reclaim_hook(v, v->fold_pre_root, new_root);
    v->fold_pre_root.pba = 0;
}
