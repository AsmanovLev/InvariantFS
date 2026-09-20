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
 *   3. only THEN reset the delta (new empty segment, clear the index).
 *
 * A crash between 2 and 3 replays the OLD delta against the NEW base; every
 * key was already applied, so re-applying is idempotent (upsert of the same
 * value, delete of an absent key) and add-before-remove holds across the
 * window. A crash before 2 leaves the pre-fold base + the full delta.
 *
 * A lock-free reader resolves delta-first-then-base. If it sees the key in
 * the delta, that record is the (or a newer) value; if it misses the delta,
 * the key must already be in the published base -- so no transient ENOENT is
 * observable, and no read lock is needed on either structure.
 *
 * -------------------------------------------------------------- reclaim
 * No mutating B+-tree call frees a page (vol_btree.c), so fold RETAINS every
 * page the pre-fold root referenced. That is intentional here: with COW a
 * retired page may still be reachable from a retained root (a save point or
 * an in-flight reader), and the design (§8, D4) makes freeing a reachability
 * diff. WP-M15 supplies that diff against {current base, pinned save-point
 * root}; this WP only leaves it a clean hook (fold_reclaim_hook below), which
 * today is a no-op. The delta's superseded segments are likewise not freed
 * (WP-M15); reset only stops naming the old chain from RT30.
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

/* Byte-lexicographic key compare, matching bt_cmp / dl_key_cmp: shorter
 * prefix first. Used only for the deterministic pre-apply sort. */
static int fold_key_cmp(const fold_ent *a, const fold_ent *b)
{
    uint16_t m = a->klen < b->klen ? a->klen : b->klen;
    int c = m ? memcmp(a->key, b->key, m) : 0;
    if (c)
        return c < 0 ? -1 : 1;
    if (a->klen < b->klen)
        return -1;
    if (a->klen > b->klen)
        return 1;
    return 0;
}

/* Stable insertion sort of the snapshot into key order (the list is small and
 * this keeps the owned pointers from being shuffled). */
static void fold_sort(fold_list *l)
{
    size_t a, b;
    for (a = 1; a < l->n; a++) {
        fold_ent tmp = l->ent[a];
        b = a;
        while (b > 0 && fold_key_cmp(&tmp, &l->ent[b - 1]) < 0) {
            l->ent[b] = l->ent[b - 1];
            b--;
        }
        l->ent[b] = tmp;
    }
}

/* Persist the dirty bitmap range. Base pages come from the shared allocator
 * (WP-M2); their bits must land before RT30 names a page. Mirrors the static
 * helper in vol_btree.c rather than exporting it, so WP-M14 stays inside its
 * file scope. */
static int fold_persist_bitmap(invfs_volume *v)
{
    uint64_t bm_bytes = (uint64_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
    uint64_t base = v->sb.metadata_zone_start * INVFS_BLOCK_SIZE;
    uint64_t lo, hi;

    if (v->bm_lo > v->bm_hi)
        return 0;
    lo = v->bm_lo;
    hi = v->bm_hi;
    if (hi > bm_bytes)
        hi = bm_bytes;
    lo -= lo % INVFS_BLOCK_SIZE;
    hi = ((hi + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE) * INVFS_BLOCK_SIZE;
    if (hi > bm_bytes)
        hi = bm_bytes;
    if (hi > lo) {
        if (io_seek(&v->io, base + lo) != 0 ||
            io_write(&v->io, v->bitmap + lo, (size_t)(hi - lo)) != 0)
            return -1;
    }
    v->bm_lo = 1;
    v->bm_hi = 0;
    return 0;
}

/* Leaves room for WP-M15 to reclaim the pages the pre-fold root referenced
 * once the old root is no longer reachable from a save point or a reader.
 * WP-M14 deliberately frees nothing (see the file header); the arguments
 * survive so M15 changes only this function body. */
static void fold_reclaim_hook(invfs_volume *v, invfs_blkptr old_root,
                              invfs_blkptr new_root)
{
    /* wait for in-flight readers (g_fold_epoch drain) */
    (void)vol_reclaim_drain(v);
    /* reachability diff: free base pages in old_root not reachable from
     * new_root or the pinned save-point root */
    (void)vol_reclaim_mark_and_free(v, old_root, new_root, v->pinned_root);
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
    if (fold_persist_bitmap(v) != 0) {
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
     * resolves idempotently; a later fold_request retries. */
    uint64_t old_delta_pba = v->rt30.delta_pba;
    if (fold_delta_reset(v) != 0)
        failed_reset = 1;

    /* WP-M15: free superseded delta segments (saved before reset cleared them) */
    (void)vol_reclaim_delta_segments(v, old_delta_pba, 0);

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
    return vol_v3_fold(v) == 0 ? 1 : -1;
}

void vol_v3_fold_reset_age(invfs_volume *v)
{
    if (v)
        v->delta_oldest_when = 0;
}
