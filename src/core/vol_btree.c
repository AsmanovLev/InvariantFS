/* vol_btree.c — WP-M3: immutable base B+-tree core.
 *
 * See vol_btree.h for the API contract and invariants. This file owns the
 * entry encoding, the COW traversal, split/merge and the ordered scan; the
 * page wire format + allocator are WP-M2 (vol_metabuf).
 *
 * ---------------------------------------------------------------- encoding
 * A node is an invfs_page_hdr (20 bytes, WP-M1) followed by packed records in
 * ascending byte-lexicographic key order:
 *
 *   leaf      record: [u16 klen][u16 vlen][key klen][val vlen]
 *   internal  record: [u16 klen][key klen][invfs_blkptr child 24]
 *
 * Every record's key is the MINIMUM key of its leaf value / child subtree, so
 * a parent can route and can refresh a separator without descending. Record
 * order is the key order; `nentries` counts records. A 4 KiB page holds as
 * many records as fit, so this is a classic B+-tree with a variable fan-out.
 *
 * ---------------------------------------------------------------- fill
 * The design doc does not fix a fill factor. This WP uses a conservative
 * byte half-full target: after a delete a node is "deficient" when its encoded
 * size is < 2 KiB, and it is merged with (or, if the pair overflows, split
 * evenly with) a sibling. This is a target, not a hard invariant: a node can
 * end up below it when its parent had no sibling to merge with and the
 * deficiency is absorbed by a merge higher up. The structural check therefore
 * requires only that a leaf is non-empty and a non-root internal node has at
 * least two children.
 *
 * ---------------------------------------------------------------- free
 * No mutating call frees a page. COW means a retired page may still be
 * reachable from a retained root (a save point or an in-flight reader), and
 * the design (section 8, D4) makes freeing a reachability diff. btree_reclaim
 * is that diff; WP-M15 schedules it. This is why the tree unit test can check
 * COW sharing byte-for-byte.
 *
 * "Immutable between folds" IS TRUE OF THE PAGES AND WAS READ AS A LICENCE
 * FOR UNGUARDED READS, WHICH IT IS NOT. No page a live root names is ever
 * rewritten, so a walk cannot tear -- that much holds. What immutability does
 * NOT give a reader is LIVENESS: the fold's reachability diff
 * (fold_reclaim_hook) frees the pages of a retired generation, and a reader
 * holding a root from two publishes ago could have a page collected under it,
 * which mbuf_read_ptr reported honestly as -1 via the allocation check
 * (vol_metabuf.c:192). Measured at ~1e-6 of reads.
 *
 * THAT IS NOW CLOSED by the reclaim reader epoch. Every base-tree read in
 * this file announces itself (vol_reclaim_reader_snapshot) BEFORE it captures
 * the root and releases (vol_reclaim_reader_release) after the walk:
 *   - point reads go through v3_base_get(), which owns the section and has a
 *     single exit -- v3_overlay_exists, v3_overlay_get_key, vol_v3_inode_get,
 *     vol_v3_dirent_get and the base half of vol_v3_recipe_load (whose RMC1
 *     chunk loop is why that last one is its own function);
 *   - the scans pair the two calls around the walk: vol_v3_xattr_scan,
 *     v3_delta_shadow_xattr_keys, vol_v3_dirent_scan, vol_v3_inode_alloc,
 *     vol_v3_iter_live_inodes, vol_v3_nlink_audit -- plus spt0_capture in
 *     vol_spt0.c;
 *   - three walks deliberately do NOT participate: vol_v3_fold (it IS the
 *     reclaimer, so it would spin the drain on itself), btree_check in
 *     vol_fsck.c (exclusive), and vol_v3_iter_inodes_at, which walks a
 *     PINNED savepoint root the reclaim keeps as a mark root.
 * The mutating base paths (inode_put/delete, xattr_set, dirent_del, ...) do
 * a btree_search on the current root before their COW and are not announced:
 * in FUSE the fold runs only from the sweep worker under g_io_lock and every
 * mutation holds that same lock, so they are mutually exclusive. That is a
 * property to preserve and cite, not one to rely on silently.
 *
 * The order inside is the part that is easy to get backwards and the reason
 * the announce is not inside v3_base_root: the window between mbuf_root_read
 * returning a root and the reader being counted is exactly the window in
 * which a fold can publish, drain (count still 0) and free. Announce-then-
 * capture is load-bearing; capture-then-announce is the same defect with an
 * extra step.
 *
 * The same sentence used to cover the DELTA, and there it was simply false:
 * the recent tier is freed by the fold the moment the index is dropped. A
 * resolved delta_ref names bytes only while g_delta_lock is held, so
 * vol_delta_read_value holds it (WP-inode-get-fold-race).
 *
 * TODO(WP-M15): the WP-M3 task blurb says upsert "frees old pages via the
 * metabuf allocator"; this implementation deliberately does NOT, because with
 * COW a retired page can still be reachable from a retained root and the API
 * carries no refcount/pin. Freeing is therefore the reachability diff
 * btree_reclaim provides, scheduled by WP-M15 against {current base, pinned
 * save-point root} (design section 8, D4).
 */

#include "volume_internal.h"
#include "vol_fault.h"
#include "vol_btree.h"
#include "vol_metabuf.h"
#include "vol_delta.h"
#include "vol_reclaim.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Test seam: fired by v3_base_root once it has a root and before anyone
 * walks it. Weak, so this no-op definition is what links unless a test
 * supplies its own -- production pays one predicted call on a path that
 * already did a block read, and no state. It exists so the
 * capture/reclaim interleave can be FORCED rather than raced for, at the
 * exact boundary the reclaim reader epoch has to span
 * (src/cli/reclaim_reader_epoch_test.c; the sibling seam for the delta side
 * is invfs_test_delta_read_hook, vol_delta.c). */
__attribute__((weak)) void invfs_test_base_read_hook(const invfs_blkptr *root)
{
    (void)root;
}

/* A node cannot hold more records than this: a leaf record is >= 4 bytes, an
 * internal record >= 26, and the usable payload is 4096-20. The bound also has
 * to cover a sibling pair during a merge (2 * leaf max). */
#define BT_MAX_ENTRIES  2100
/* Rebalance depth guard: a B+-tree over 4 KiB pages is far shallower. */
#define BT_MAX_DEPTH    32
/* A node is "deficient" below this encoded size (see header comment). */
#define BT_MIN_FILL     (INVFS_BLOCK_SIZE / 2)
/* btree_search's value lands in a per-thread buffer; a record length is u16. */
#define BT_VAL_MAX      65535u

/* Decoded record descriptor. k/v point into a caller-owned page buffer. */
typedef struct {
    const uint8_t *k;
    const uint8_t *v;          /* leaf only */
    invfs_blkptr   child;      /* internal only */
    uint16_t       klen;
    uint16_t       vlen;       /* leaf only */
} bt_ent;

/* Result of a recursive insertion. `nptr` is how many sibling pages the node
 * turned into (1 = no split, 2 = the classic 2-way split, 3 = the node needed
 * three pages -- see bt_split3_point). A 3-way split hands the parent TWO new
 * separators, which is why the struct carries `mid` as well. */
typedef struct {
    invfs_blkptr node;         /* new subtree root (leftmost piece) */
    invfs_blkptr mid;          /* middle piece, valid when nptr == 3 */
    invfs_blkptr right;        /* rightmost piece, valid when nptr >= 2 */
    int          nptr;         /* 1, 2 or 3 */
    int          level;
} bt_up;

/* Result of a recursive deletion. node.pba == 0 means "this subtree is now
 * empty" and the caller must drop the entry. */
typedef struct {
    invfs_blkptr node;
    int          underflow;
    int          changed;
    int          level;
} bt_dn;

/* WP86: bt_range / bt_quarantine (the quarantined key ranges) are declared in
 * vol_btree.h next to the API that fills and consumes them. Result of a
 * recursive excision. node.pba == 0 means "this subtree is now
 * empty" and the caller must drop the entry. min_* is the new minimum key of
 * `node` when the node lost its first child, so the caller can keep its
 * separator in step (copied, never a borrowed page pointer). */
typedef struct {
    invfs_blkptr node;
    uint8_t      min_key[BT_QUARANTINE_KEY_MAX];
    uint16_t     min_n;
    uint64_t     dropped;    /* subtrees removed below here */
    int          changed;
} bt_excise;

/* ------------------------------------------------------------------ */
/* key compare + small encode helpers                                 */
/* ------------------------------------------------------------------ */

/* The ordering is vol_key_cmp() in volume_internal.h, shared with the delta
 * log and the fold -- the base tree and the delta log must agree byte for
 * byte or the fold's merge is not a merge. */
static int bt_cmp_key(const bt_ent *e, bt_key key)
{
    return vol_key_cmp(e->k, e->klen, key.p, key.n);
}

static uint16_t rd16(const uint8_t *p)
{
    uint16_t x;
    memcpy(&x, p, sizeof x);
    return x;
}

static void wr16(uint8_t *p, uint16_t x)
{
    memcpy(p, &x, sizeof x);
}

/* Encoded record size for a node at `level`. */
static uint32_t bt_rec_size(int level, const bt_ent *e)
{
    if (level == INVFS_PAGE_LEVEL_LEAF)
        return (uint32_t)4u + e->klen + e->vlen;
    return (uint32_t)2u + e->klen + (uint32_t)sizeof(invfs_blkptr);
}

static uint32_t bt_used(int level, const bt_ent *e, int n)
{
    uint32_t u = (uint32_t)sizeof(invfs_page_hdr);
    int i;
    for (i = 0; i < n; i++)
        u += bt_rec_size(level, &e[i]);
    return u;
}

/* ------------------------------------------------------------------ */
/* page IO                                                            */
/* ------------------------------------------------------------------ */

/* Serialize `n` decoded records as a sealed page at a fresh pba. The blkptr
 * gets LEAF/INTERNAL from the level; the caller ORs ROOT when it applies. */
static int bt_write(invfs_volume *v, int level, uint64_t gen,
                    const bt_ent *e, int n, invfs_blkptr *out)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    uint8_t *p;
    uint64_t pba;
    int i;

    if (!v || !out || n < 0 || n > BT_MAX_ENTRIES)
        return -1;
    if (bt_used(level, e, n) > INVFS_BLOCK_SIZE)
        return -1;
    /* An internal record with no child is a hole in the key space: every
     * search for a key past that separator walks into it and fails, and it
     * is unrecoverable once the page is published (the parent's page CRC
     * and gen both check out). Every internal page in the tree leaves here,
     * so this is the one place that can refuse the malformed page instead
     * of sealing it. WP89 shipped the 3-way-split splice below with exactly
     * such a hole; this turns the next one into a failed insert. */
    if (level != INVFS_PAGE_LEVEL_LEAF)
        for (i = 0; i < n; i++)
            if (e[i].child.pba == 0)
                return -1;

    pba = mbuf_alloc(v, gen);
    /* WP126: tell the orphan collector this is a base page, so it
     * never has to look for it in the block space. */
    btree_orphan_note_alloc(v, pba);
    if (!pba)
        return -1;

    mbuf_page_init(page, (uint16_t)level, gen);
    mbuf_page_hdr(page)->nentries = (uint16_t)n;
    p = page + sizeof(invfs_page_hdr);
    for (i = 0; i < n; i++) {
        wr16(p, e[i].klen);
        p += 2;
        if (e[i].klen) {
            memcpy(p, e[i].k, e[i].klen);
            p += e[i].klen;
        }
        if (level == INVFS_PAGE_LEVEL_LEAF) {
            wr16(p, e[i].vlen);
            p += 2;
            if (e[i].vlen) {
                memcpy(p, e[i].v, e[i].vlen);
                p += e[i].vlen;
            }
        } else {
            memcpy(p, &e[i].child, sizeof e[i].child);
            p += sizeof e[i].child;
        }
    }
    if ((size_t)(p - page) > INVFS_BLOCK_SIZE) {
        mbuf_free(v, pba);
        return -1;
    }
    if (mbuf_write(v, pba, page) != 0) {
        mbuf_free(v, pba);
        return -1;
    }
    mbuf_ptr_set(out, pba, page,
                 level == INVFS_PAGE_LEVEL_LEAF ? INVFS_BP_LEAF
                                                : INVFS_BP_INTERNAL);
    return 0;
}

/* Read + verify (CRC/gen via mbuf_read_ptr) and decode a node. */
static int bt_read(invfs_volume *v, invfs_blkptr ptr, uint8_t *buf,
                   bt_ent *e, int *n_out, int *level_out)
{
    const invfs_page_hdr *h;
    const uint8_t *p, *end;
    int level, n, i;

    if (mbuf_read_ptr(v, &ptr, buf) != 0)
        return -1;
    h = mbuf_page_chdr(buf);
    level = (int)h->level;
    n = (int)h->nentries;
    if (n < 0 || n > BT_MAX_ENTRIES)
        return -1;
    p = buf + sizeof(invfs_page_hdr);
    end = buf + INVFS_BLOCK_SIZE;
    for (i = 0; i < n; i++) {
        uint16_t kl;
        if ((size_t)(end - p) < 2)
            return -1;
        kl = rd16(p);
        p += 2;
        if ((size_t)(end - p) < kl)
            return -1;
        e[i].k = p;
        e[i].klen = kl;
        p += kl;
        if (level == INVFS_PAGE_LEVEL_LEAF) {
            uint16_t vl;
            if ((size_t)(end - p) < 2)
                return -1;
            vl = rd16(p);
            p += 2;
            if ((size_t)(end - p) < vl)
                return -1;
            e[i].v = p;
            e[i].vlen = vl;
            p += vl;
            e[i].child.pba = 0;
        } else {
            if ((size_t)(end - p) < sizeof(invfs_blkptr))
                return -1;
            memcpy(&e[i].child, p, sizeof e[i].child);
            p += sizeof e[i].child;
            e[i].v = NULL;
            e[i].vlen = 0;
        }
    }
    *n_out = n;
    *level_out = level;
    return 0;
}

/* Minimum key of a node (its first record's key), copied into kbuf to avoid
 * keeping a page buffer alive across recursion frames. */
static int bt_first_key(invfs_volume *v, invfs_blkptr ptr,
                        uint8_t *kbuf, bt_key *k)
{
    const invfs_page_hdr *h;
    uint16_t kl;

    if (mbuf_read_ptr(v, &ptr, kbuf) != 0)
        return -1;
    h = mbuf_page_chdr(kbuf);
    if (h->nentries == 0)
        return -1;
    kl = rd16(kbuf + sizeof(invfs_page_hdr));
    if ((size_t)sizeof(invfs_page_hdr) + 2u + kl > INVFS_BLOCK_SIZE)
        return -1;
    k->p = kbuf + sizeof(invfs_page_hdr) + 2;
    k->n = kl;
    return 0;
}

/* Read the page at ptr and rewrite it at a fresh pba with `gen` (contents
 * unchanged). Used when a root collapse would otherwise expose an old gen. */
static int bt_recopy(invfs_volume *v, invfs_blkptr ptr, uint64_t gen,
                     invfs_blkptr *out)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    uint64_t pba;
    invfs_page_hdr *h;

    if (mbuf_read_ptr(v, &ptr, page) != 0)
        return -1;
    pba = mbuf_alloc(v, gen);
    /* WP126: tell the orphan collector this is a base page, so it
     * never has to look for it in the block space. */
    btree_orphan_note_alloc(v, pba);
    if (!pba)
        return -1;
    h = mbuf_page_hdr(page);
    h->gen = gen;
    if (mbuf_write(v, pba, page) != 0) {
        mbuf_free(v, pba);
        return -1;
    }
    mbuf_ptr_set(out, pba, page, ptr.flags);
    return 0;
}

/* ------------------------------------------------------------------ */
/* in-node search helpers                                             */
/* ------------------------------------------------------------------ */

/* First leaf record with key >= target. */
static int bt_lower_bound(const bt_ent *e, int n, bt_key key)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (bt_cmp_key(&e[mid], key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Rightmost internal child whose stored minimum is <= target (0 when target
 * is below the node's minimum; the descent then simply misses). */
static int bt_child_index(const bt_ent *e, int n, bt_key key)
{
    int i = 0, j;
    for (j = 1; j < n; j++) {
        if (bt_cmp_key(&e[j], key) <= 0)
            i = j;
        else
            break;
    }
    return i;
}

/* Choose a split index so both halves fit a page (balanced by encoded size). */
static int bt_split_point(int level, const bt_ent *e, int n)
{
    uint32_t total = bt_used(level, e, n);
    uint32_t half = total / 2;
    uint32_t acc = (uint32_t)sizeof(invfs_page_hdr);
    int s = n / 2, i;

    for (i = 0; i < n - 1; i++) {
        acc += bt_rec_size(level, &e[i]);
        if (acc >= half) {
            s = i + 1;
            break;
        }
    }
    if (s < 1)
        s = 1;
    if (s > n - 1)
        s = n - 1;
    while (s > 1 && bt_used(level, e, s) > INVFS_BLOCK_SIZE)
        s--;
    while (s < n - 1 && bt_used(level, e + s, n - s) > INVFS_BLOCK_SIZE)
        s++;
    if (bt_used(level, e, s) > INVFS_BLOCK_SIZE ||
        bt_used(level, e + s, n - s) > INVFS_BLOCK_SIZE)
        return -1;
    return s;
}

/* Split an overflowing node into 2 or 3 page-fitting pieces.
 *
 * A 2-way split is not always possible. Records are variable-width, so an
 * ordered run can have NO cut index at which both halves fit a page -- the
 * WP88 case is a leaf holding a 3112-byte Q2R3 recipe chunk next to
 * 309/693/85-byte recipes: cutting before the chunk leaves 4848 bytes on the
 * right, cutting after it leaves 4219 on the left. The old code reported that
 * as an insert failure (bt_ins_rec -> -1), which surfaced as
 * "vol_v3_recipe_store failed" and cost the containerpack sweep the whole
 * decomposition of the file.
 *
 * So: try the balanced 2-way split first and keep its exact behaviour; only
 * when it is impossible fall back to greedily packing the run left to right
 * into at most THREE page-fitting pieces. `cut[]` receives the g-1 interior
 * cut indices (the trailing piece runs to n-1) and the return value is g, or
 * 0 when even three pieces cannot hold the run -- which needs a single record
 * too wide for a page, a case bt_write already rejects. Every piece is
 * verified page-fitting before it is returned, so this can never hand
 * bt_write an overfull node.
 */
static int bt_split3_point(int level, const bt_ent *e, int n, int *cut)
{
    int s = bt_split_point(level, e, n);
    int i, s1, s2;

    if (s > 0) {              /* the classic 2-way split still works */
        cut[0] = s;
        return 2;
    }
    if (n < 3)               /* not enough records for three pieces */
        return 0;
    /* A record that cannot fit a page on its own makes every split fail. */
    for (i = 0; i < n; i++)
        if (bt_used(level, &e[i], 1) > INVFS_BLOCK_SIZE)
            return 0;
    /* s1: the largest prefix (leaving >= 2 records behind) that fits a page. */
    s1 = 0;
    for (i = 1; i <= n - 2; i++) {
        if (bt_used(level, e, i) > INVFS_BLOCK_SIZE)
            break;
        s1 = i;
    }
    if (s1 < 1)
        return 0;
    /* s2: the largest second piece that fits a page, leaving >= 1 behind. */
    s2 = s1;
    for (i = s1 + 1; i <= n - 1; i++) {
        if (bt_used(level, e + s1, i - s1) > INVFS_BLOCK_SIZE)
            break;
        s2 = i;
    }
    if (s2 < s1 + 1)
        return 0;
    if (bt_used(level, e + s2, n - s2) > INVFS_BLOCK_SIZE)
        return 0;
    cut[0] = s1;
    cut[1] = s2;
    return 3;
}

/* ------------------------------------------------------------------ */
/* search                                                             */
/* ------------------------------------------------------------------ */

static __thread uint8_t bt_tls_val[BT_VAL_MAX + 1u];

int btree_search(invfs_volume *v, invfs_blkptr root,
                 bt_key key, bt_val *val_out, int *found)
{
    bt_ent e[BT_MAX_ENTRIES];
    uint8_t buf[INVFS_BLOCK_SIZE];
    invfs_blkptr cur;
    int depth = 0;

    if (!v || !found)
        return -1;
    *found = 0;
    if (root.pba == 0)
        return 0;

    cur = root;
    while (cur.pba && depth++ < BT_MAX_DEPTH) {
        int n, level, pos, hit;
        if (bt_read(v, cur, buf, e, &n, &level) != 0)
            return -1;
        if (level != INVFS_PAGE_LEVEL_LEAF) {
            cur = e[bt_child_index(e, n, key)].child;
            if (cur.pba == 0)
                return -1;
            continue;
        }
        pos = bt_lower_bound(e, n, key);
        hit = (pos < n && bt_cmp_key(&e[pos], key) == 0);
        if (hit) {
            if (e[pos].vlen)
                memcpy(bt_tls_val, e[pos].v, e[pos].vlen);
            if (val_out) {
                val_out->p = bt_tls_val;
                val_out->n = e[pos].vlen;
            }
            *found = 1;
        }
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* copy-on-write insert                                               */
/* ------------------------------------------------------------------ */

static int bt_ins_rec(invfs_volume *v, invfs_blkptr node, bt_key key,
                      bt_val val, uint64_t gen, bt_up *out)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    bt_ent *e = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
    int n, level, g;
    int cut[2];

    if (!e)
        return -1;
    if (bt_read(v, node, buf, e, &n, &level) != 0) {
        free(e);
        return -1;
    }
    out->level = level;

    if (level == INVFS_PAGE_LEVEL_LEAF) {
        int pos = bt_lower_bound(e, n, key);
        int dup = (pos < n && bt_cmp_key(&e[pos], key) == 0);
        if (dup) {
            e[pos].v = val.p;
            e[pos].vlen = val.n;
        } else {
            int j;
            if (n >= BT_MAX_ENTRIES) {
                free(e);
                return -1;
            }
            for (j = n; j > pos; j--)
                e[j] = e[j - 1];
            e[pos].k = key.p;
            e[pos].klen = key.n;
            e[pos].v = val.p;
            e[pos].vlen = val.n;
            n++;
        }
        if (bt_used(level, e, n) <= INVFS_BLOCK_SIZE) {
            if (bt_write(v, level, gen, e, n, &out->node) != 0) {
                free(e);
                return -1;
            }
            out->nptr = 1;
            free(e);
            return 0;
        }
    } else {
        int i = bt_child_index(e, n, key);
        bt_up cu;
        if (bt_ins_rec(v, e[i].child, key, val, gen, &cu) != 0) {
            free(e);
            return -1;
        }
        if (vol_key_cmp(key.p, key.n, e[i].k, e[i].klen) < 0) {
            e[i].k = key.p;
            e[i].klen = key.n;
        }
        if (cu.nptr == 1) {
            e[i].child = cu.node;
            if (bt_used(level, e, n) <= INVFS_BLOCK_SIZE) {
                if (bt_write(v, level, gen, e, n, &out->node) != 0) {
                    free(e);
                    return -1;
                }
                out->nptr = 1;
                free(e);
                return 0;
            }
        } else {
            /* cu.nptr is 2 or 3: splice in (nptr - 1) separators at i+1..,
             * each keyed by the FIRST key of the piece it introduces. The
             * leftmost piece stays in record i. */
            uint8_t rkbuf[INVFS_BLOCK_SIZE], mkbuf[INVFS_BLOCK_SIZE];
            bt_key rk;
            bt_key mk = { NULL, 0 };
            int j, add = cu.nptr - 1;
            if (bt_first_key(v, cu.right, rkbuf, &rk) != 0) {
                free(e);
                return -1;
            }
            if (add == 2 && bt_first_key(v, cu.mid, mkbuf, &mk) != 0) {
                free(e);
                return -1;
            }
            e[i].child = cu.node;
            if (n + add > BT_MAX_ENTRIES) {
                free(e);
                return -1;
            }
            /* The tail moves up by `add` slots -- TWO of them for the
             * three-way split, not one. The old `for (j = n; j > i + add;
             * j--)` is the two-way loop generalised by changing only its
             * bound: it shifts every tail record up by exactly one, so with
             * add == 2 the topmost destination (n + add - 1) is never
             * written. That slot kept whatever the malloc gave it, the page
             * went out as a parent whose LAST child was null, and every
             * later btree_search for a key past that separator returned -1
             * (WP89: "vol_v3_recipe_store failed" in the sweep's dedupe
             * pass, then a non-zero sweep exit). Read the source `add`
             * slots lower, not one. */
            for (j = n + add - 1; j > i + add; j--)
                e[j] = e[j - add];
            e[i + 1].k = add == 2 ? mk.p : rk.p;
            e[i + 1].klen = add == 2 ? mk.n : rk.n;
            e[i + 1].child = add == 2 ? cu.mid : cu.right;
            if (add == 2) {
                e[i + 2].k = rk.p;
                e[i + 2].klen = rk.n;
                e[i + 2].child = cu.right;
            }
            n += add;
            if (bt_used(level, e, n) <= INVFS_BLOCK_SIZE) {
                if (bt_write(v, level, gen, e, n, &out->node) != 0) {
                    free(e);
                    return -1;
                }
                out->nptr = 1;
                free(e);
                return 0;
            }
        }
        /* fall through: the internal page now overflows */
    }

    g = bt_split3_point(level, e, n, cut);
    if (g < 2) {
        free(e);
        return -1;
    }
    if (bt_write(v, level, gen, e, cut[0], &out->node) != 0) {
        free(e);
        return -1;
    }
    if (g == 3) {
        if (bt_write(v, level, gen, e + cut[0], cut[1] - cut[0],
                     &out->mid) != 0) {
            free(e);
            return -1;
        }
        if (bt_write(v, level, gen, e + cut[1], n - cut[1],
                     &out->right) != 0) {
            free(e);
            return -1;
        }
    } else if (bt_write(v, level, gen, e + cut[0], n - cut[0],
                        &out->right) != 0) {
        free(e);
        return -1;
    }
    out->nptr = g;
    free(e);
    return 0;
}

int btree_upsert(invfs_volume *v, invfs_blkptr root, bt_key key,
                 bt_val val, invfs_blkptr *new_root_out)
{
    uint64_t gen;
    bt_up up;

    if (!v || !new_root_out)
        return -1;
    if ((key.n && !key.p) || (val.n && !val.p))
        return -1;

    gen = (root.pba ? root.gen : 0) + 1;

    if (root.pba == 0) {
        bt_ent e;
        e.k = key.p;
        e.klen = key.n;
        e.v = val.p;
        e.vlen = val.n;
        if (bt_write(v, INVFS_PAGE_LEVEL_LEAF, gen, &e, 1,
                     new_root_out) != 0)
            return -1;
        new_root_out->flags |= INVFS_BP_ROOT;
        return 0;
    }

    if (bt_ins_rec(v, root, key, val, gen, &up) != 0)
        return -1;

    if (up.nptr == 1) {
        up.node.flags |= INVFS_BP_ROOT;
        *new_root_out = up.node;
        return 0;
    }

    {
        /* The old root became up.nptr pages; the new root is one level up
         * with up.nptr records: record k is keyed by the FIRST key of piece
         * k and owns that piece's subtree. A 3-way split therefore needs 3
         * records, not 2. */
        uint8_t k1[INVFS_BLOCK_SIZE], k2[INVFS_BLOCK_SIZE], k3[INVFS_BLOCK_SIZE];
        bt_key m1, m2, m3;
        bt_ent re[3];
        invfs_blkptr r;
        re[0].child = up.node;
        if (bt_first_key(v, up.node, k1, &m1) != 0)
            return -1;
        re[0].k = m1.p;
        re[0].klen = m1.n;
        if (up.nptr == 3) {
            re[1].child = up.mid;
            if (bt_first_key(v, up.mid, k2, &m2) != 0)
                return -1;
            re[1].k = m2.p;
            re[1].klen = m2.n;
            re[2].child = up.right;
            if (bt_first_key(v, up.right, k3, &m3) != 0)
                return -1;
            re[2].k = m3.p;
            re[2].klen = m3.n;
        } else {
            re[1].child = up.right;
            if (bt_first_key(v, up.right, k2, &m2) != 0)
                return -1;
            re[1].k = m2.p;
            re[1].klen = m2.n;
        }
        if (bt_write(v, up.level + 1, gen, re, up.nptr, &r) != 0)
            return -1;
        r.flags |= INVFS_BP_ROOT;
        *new_root_out = r;
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* copy-on-write delete + rebalance                                   */
/* ------------------------------------------------------------------ */

/* Merge/split the deficient child `i` with a sibling. `pe`/`*pn` are the
 * parent's decoded records; both child and sibling pages are rewritten (COW).
 * ksc1/ksc2 are caller scratch (two page-sized buffers) that keep the new
 * separator keys alive for the parent encode. */
static int bt_rebalance(invfs_volume *v, bt_ent *pe, int *pn, int i,
                        uint64_t gen, uint8_t *ksc1, uint8_t *ksc2)
{
    uint8_t cbuf[INVFS_BLOCK_SIZE], sbuf[INVFS_BLOCK_SIZE];
    bt_ent *ce, *se, *comb;
    int cn, clevel, sn, slevel, j, base, tot, s;
    uint32_t used;

    if (*pn < 2)
        return 0;   /* no sibling: the parent's own underflow propagates */

    ce = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
    se = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
    comb = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
    if (!ce || !se || !comb) {
        free(ce);
        free(se);
        free(comb);
        return -1;
    }
    if (bt_read(v, pe[i].child, cbuf, ce, &cn, &clevel) != 0) {
        free(ce);
        free(se);
        free(comb);
        return -1;
    }
    j = (i + 1 < *pn) ? i + 1 : i - 1;
    if (j < 0 || j >= *pn) {
        free(ce);
        free(se);
        free(comb);
        return 0;
    }
    if (bt_read(v, pe[j].child, sbuf, se, &sn, &slevel) != 0) {
        free(ce);
        free(se);
        free(comb);
        return -1;
    }
    if (slevel != clevel) {
        free(ce);
        free(se);
        free(comb);
        return -1;
    }

    base = (j > i) ? i : j;
    if (j == i + 1) {
        memcpy(comb, ce, sizeof(bt_ent) * (size_t)cn);
        memcpy(comb + cn, se, sizeof(bt_ent) * (size_t)sn);
    } else {
        memcpy(comb, se, sizeof(bt_ent) * (size_t)sn);
        memcpy(comb + sn, ce, sizeof(bt_ent) * (size_t)cn);
    }
    tot = cn + sn;
    used = bt_used(clevel, comb, tot);

    if (used <= INVFS_BLOCK_SIZE) {
        invfs_blkptr m;
        uint16_t kl;
        int x;
        if (bt_write(v, clevel, gen, comb, tot, &m) != 0) {
            free(ce);
            free(se);
            free(comb);
            return -1;
        }
        kl = comb[0].klen;
        memcpy(ksc1, comb[0].k, kl);
        pe[base].k = ksc1;
        pe[base].klen = kl;
        pe[base].child = m;
        for (x = base + 1; x + 1 < *pn; x++)
            pe[x] = pe[x + 1];
        (*pn)--;
    } else {
        invfs_blkptr L, R;
        uint16_t k1, k2;
        s = bt_split_point(clevel, comb, tot);
        if (s <= 0 || s >= tot) {
            free(ce);
            free(se);
            free(comb);
            return -1;
        }
        if (bt_write(v, clevel, gen, comb, s, &L) != 0 ||
            bt_write(v, clevel, gen, comb + s, tot - s, &R) != 0) {
            free(ce);
            free(se);
            free(comb);
            return -1;
        }
        k1 = comb[0].klen;
        k2 = comb[s].klen;
        memcpy(ksc1, comb[0].k, k1);
        memcpy(ksc2, comb[s].k, k2);
        pe[base].k = ksc1;
        pe[base].klen = k1;
        pe[base].child = L;
        pe[base + 1].k = ksc2;
        pe[base + 1].klen = k2;
        pe[base + 1].child = R;
    }
    free(ce);
    free(se);
    free(comb);
    return 0;
}

static int bt_del_rec(invfs_volume *v, invfs_blkptr node, bt_key key,
                      uint64_t gen, int is_root, bt_dn *out, int *deleted)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    uint8_t ksc1[INVFS_BLOCK_SIZE], ksc2[INVFS_BLOCK_SIZE];
    bt_ent *e = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
    int n, level;

    if (!e)
        return -1;
    if (bt_read(v, node, buf, e, &n, &level) != 0) {
        free(e);
        return -1;
    }
    out->level = level;

    if (level == INVFS_PAGE_LEVEL_LEAF) {
        int pos = bt_lower_bound(e, n, key);
        if (pos >= n || bt_cmp_key(&e[pos], key) != 0) {
            out->node = node;
            out->changed = 0;
            free(e);
            return 0;
        }
        {
            int j;
            for (j = pos; j + 1 < n; j++)
                e[j] = e[j + 1];
        }
        n--;
        *deleted = 1;
        if (n == 0) {
            out->node.pba = 0;
            out->node.gen = gen;
            out->node.flags = 0;
            out->underflow = 1;
            out->changed = 1;
            free(e);
            return 0;
        }
        if (bt_write(v, level, gen, e, n, &out->node) != 0) {
            free(e);
            return -1;
        }
        out->underflow = (!is_root && bt_used(level, e, n) < BT_MIN_FILL);
        out->changed = 1;
        free(e);
        return 0;
    }

    {
        int i = bt_child_index(e, n, key);
        bt_dn cd;
        bt_key oldmin;
        oldmin.p = e[i].k;
        oldmin.n = e[i].klen;
        if (bt_del_rec(v, e[i].child, key, gen, 0, &cd, deleted) != 0) {
            free(e);
            return -1;
        }
        if (!cd.changed) {
            out->node = node;
            out->changed = 0;
            free(e);
            return 0;
        }
        if (cd.node.pba == 0) {
            int j;
            for (j = i; j + 1 < n; j++)
                e[j] = e[j + 1];
            n--;
        } else {
            e[i].child = cd.node;
            if (cd.underflow) {
                if (bt_rebalance(v, e, &n, i, gen, ksc1, ksc2) != 0) {
                    free(e);
                    return -1;
                }
            } else if (vol_key_cmp(key.p, key.n, oldmin.p, oldmin.n) == 0) {
                bt_key nm;
                if (bt_first_key(v, cd.node, ksc1, &nm) != 0) {
                    free(e);
                    return -1;
                }
                e[i].k = nm.p;
                e[i].klen = nm.n;
            }
        }
        if (n == 0) {
            out->node.pba = 0;
            out->node.gen = gen;
            out->node.flags = 0;
            out->underflow = 1;
            out->changed = 1;
            free(e);
            return 0;
        }
        if (bt_write(v, level, gen, e, n, &out->node) != 0) {
            free(e);
            return -1;
        }
        out->underflow = (!is_root && bt_used(level, e, n) < BT_MIN_FILL);
        out->changed = 1;
        free(e);
        return 0;
    }
}

int btree_delete(invfs_volume *v, invfs_blkptr root, bt_key key,
                 invfs_blkptr *new_root_out)
{
    uint64_t gen;
    bt_dn dn;
    int deleted = 0, guard = 0;
    invfs_blkptr r;

    if (!v || !new_root_out)
        return -1;
    if (key.n && !key.p)
        return -1;
    if (root.pba == 0) {
        *new_root_out = root;
        return 0;
    }
    gen = root.gen + 1;
    if (bt_del_rec(v, root, key, gen, 1, &dn, &deleted) != 0)
        return -1;
    if (!dn.changed) {
        *new_root_out = root;
        return 0;
    }
    r = dn.node;
    if (r.pba == 0) {
        r.flags = 0;
        *new_root_out = r;
        return 0;
    }

    /* Collapse single-child internal roots (height shrink). */
    while (r.pba && guard++ < BT_MAX_DEPTH) {
        uint8_t buf[INVFS_BLOCK_SIZE];
        const invfs_page_hdr *h;
        uint16_t kl;
        const uint8_t *cp;
        invfs_blkptr child;
        if (mbuf_read_ptr(v, &r, buf) != 0)
            return -1;
        h = mbuf_page_chdr(buf);
        if (h->level == INVFS_PAGE_LEVEL_LEAF || h->nentries != 1)
            break;
        kl = rd16(buf + sizeof(invfs_page_hdr));
        cp = buf + sizeof(invfs_page_hdr) + 2 + kl;
        memcpy(&child, cp, sizeof child);
        r = child;
    }
    if (!r.pba)
        return -1;
    if (r.gen != gen) {
        invfs_blkptr nr;
        if (bt_recopy(v, r, gen, &nr) != 0)
            return -1;
        r = nr;
    }
    r.flags |= INVFS_BP_ROOT;
    *new_root_out = r;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ordered range scan                                                 */
/* ------------------------------------------------------------------ */

static int bt_scan_rec(invfs_volume *v, invfs_blkptr ptr,
                       bt_key lo, bt_key hi, bt_scan_cb cb, void *ctx)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    bt_ent *e = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
    int n, level, i;

    if (!e)
        return -1;
    if (bt_read(v, ptr, buf, e, &n, &level) != 0) {
        free(e);
        return -1;
    }
    if (level == INVFS_PAGE_LEVEL_LEAF) {
        for (i = 0; i < n; i++) {
            bt_key k;
            int rc;
            k.p = e[i].k;
            k.n = e[i].klen;
            if (lo.n && vol_key_cmp(k.p, k.n, lo.p, lo.n) < 0)
                continue;
            if (hi.n && vol_key_cmp(k.p, k.n, hi.p, hi.n) >= 0)
                break;
            {
                bt_val val;
                val.p = e[i].v;
                val.n = e[i].vlen;
                rc = cb(ctx, k, val);
            }
            if (rc) {
                free(e);
                return rc;
            }
        }
        free(e);
        return 0;
    }
    for (i = 0; i < n; i++) {
        bt_key clo;
        int rc;
        clo.p = e[i].k;
        clo.n = e[i].klen;
        if (hi.n && vol_key_cmp(clo.p, clo.n, hi.p, hi.n) >= 0)
            break;
        if (i + 1 < n) {
            if (lo.n && vol_key_cmp(e[i + 1].k, e[i + 1].klen, lo.p, lo.n) <= 0)
                continue;
        }
        rc = bt_scan_rec(v, e[i].child, lo, hi, cb, ctx);
        if (rc) {
            free(e);
            return rc;
        }
    }
    free(e);
    return 0;
}

int btree_scan(invfs_volume *v, invfs_blkptr root,
               bt_key lo, bt_key hi, bt_scan_cb cb, void *ctx)
{
    if (!v || !cb)
        return -1;
    if (root.pba == 0)
        return 0;
    return bt_scan_rec(v, root, lo, hi, cb, ctx);
}

/* ------------------------------------------------------------------ */
/* structural check / stats                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *seen;
    uint64_t total;
    uint64_t pages;
    uint64_t keys;
    uint32_t height;      /* leaf depth (1 = root is a leaf) */
    uint64_t bad;         /* WP86: unreadable pages skipped, not fatal */
    bt_quarantine *q;     /* WP86: where to record the quarantined ranges */
    int      qfull;
    char    *err;
    size_t   errlen;
} bt_ck;

static void bt_ck_err(bt_ck *ck, const char *msg)
{
    if (ck->err && ck->errlen && ck->err[0] == 0)
        snprintf(ck->err, ck->errlen, "%s", msg);
}

static int bt_check_rec(invfs_volume *v, invfs_blkptr ptr, int expect_level,
                        bt_key lo, bt_key hi, uint32_t depth, bt_ck *ck)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    bt_ent *e = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
    int n, level, i;

    if (!e)
        return -1;
    if (ptr.pba == 0 || ptr.pba >= ck->total) {
        bt_ck_err(ck, "null/out-of-range child pba");
        free(e);
        return -1;
    }
    if (bit_get(ck->seen, ptr.pba)) {
        bt_ck_err(ck, "cycle or shared child");
        free(e);
        return -1;
    }
    bit_set(ck->seen, ptr.pba);
    ck->pages++;

    if (bt_read(v, ptr, buf, e, &n, &level) != 0) {
        bt_ck_err(ck, "unreadable page (bad CRC/gen/encoding)");
        free(e);
        return -1;
    }
    if (expect_level >= 0 && level != expect_level) {
        bt_ck_err(ck, "level not monotone");
        free(e);
        return -1;
    }

    if (level == INVFS_PAGE_LEVEL_LEAF) {
        if (ck->height == 0)
            ck->height = depth;
        else if (ck->height != depth) {
            bt_ck_err(ck, "leaves at differing depths");
            free(e);
            return -1;
        }
        if (n < 1) {
            bt_ck_err(ck, "empty leaf");
            free(e);
            return -1;
        }
        for (i = 0; i < n; i++) {
            if (i && vol_key_cmp(e[i - 1].k, e[i - 1].klen, e[i].k, e[i].klen) >= 0) {
                bt_ck_err(ck, "leaf keys not strictly ordered");
                free(e);
                return -1;
            }
            if (lo.n && vol_key_cmp(e[i].k, e[i].klen, lo.p, lo.n) < 0) {
                bt_ck_err(ck, "leaf key below parent bound");
                free(e);
                return -1;
            }
            if (hi.n && vol_key_cmp(e[i].k, e[i].klen, hi.p, hi.n) >= 0) {
                bt_ck_err(ck, "leaf key above parent bound");
                free(e);
                return -1;
            }
        }
        ck->keys += (uint64_t)n;
        free(e);
        return 0;
    }

    if (n < 2) {
        bt_ck_err(ck, "internal node with fewer than 2 children");
        free(e);
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (i && vol_key_cmp(e[i - 1].k, e[i - 1].klen, e[i].k, e[i].klen) >= 0) {
            bt_ck_err(ck, "separators not strictly ordered");
            free(e);
            return -1;
        }
        if (lo.n && vol_key_cmp(e[i].k, e[i].klen, lo.p, lo.n) < 0) {
            bt_ck_err(ck, "separator below parent bound");
            free(e);
            return -1;
        }
        if (hi.n && vol_key_cmp(e[i].k, e[i].klen, hi.p, hi.n) >= 0) {
            bt_ck_err(ck, "separator above parent bound");
            free(e);
            return -1;
        }
    }
    for (i = 0; i < n; i++) {
        bt_key clo, chi;
        int rc;
        clo.p = e[i].k;
        clo.n = e[i].klen;
        chi.p = (i + 1 < n) ? e[i + 1].k : hi.p;
        chi.n = (i + 1 < n) ? e[i + 1].klen : hi.n;
        rc = bt_check_rec(v, e[i].child, level - 1, clo, chi, depth + 1, ck);
        if (rc != 0) {
            free(e);
            return -1;
        }
    }
    free(e);
    return 0;
}

int btree_check(invfs_volume *v, invfs_blkptr root,
                bt_stat *out, char *err, size_t errlen)
{
    bt_ck ck;
    uint64_t bytes;
    int rc;

    if (!v)
        return -1;
    if (out) {
        out->n_pages = 0;
        out->nkeys = 0;
        out->height = 0;
    }
    if (err && errlen)
        err[0] = 0;
    if (root.pba == 0)
        return 0;

    bytes = (v->sb.total_blocks + 7u) / 8u;
    memset(&ck, 0, sizeof ck);
    ck.seen = (uint8_t *)calloc(1, (size_t)bytes);
    if (!ck.seen)
        return -1;
    ck.total = v->sb.total_blocks;
    ck.err = err;
    ck.errlen = errlen;
    rc = bt_check_rec(v, root, -1, (bt_key){NULL, 0}, (bt_key){NULL, 0}, 1, &ck);
    if (out) {
        out->n_pages = ck.pages;
        out->nkeys = ck.keys;
        out->height = ck.height;
    }
    free(ck.seen);
    return rc;
}

/* ------------------------------------------------------------------ */
/* WP86: quarantine walk + range excision                             */
/*                                                                   */
/* A base page whose CRC/gen/encoding does not match its bytes cannot */
/* be repaired: WP-M2 never rewrites a torn page, and a COW tree keeps */
/* no second copy of one. But a B+-tree's parent separators say        */
/* exactly which key range the page owned, so the damage can be        */
/* CONTAINED instead of fatal -- the range is quarantined, every other */
/* subtree stays readable, and a key inside the range fails loudly     */
/* (EIO) rather than answering "absent".                               */
/*                                                                   */
/* btree_check walks strictly and aborts at the first bad page, so it  */
/* both under-reports (one bad page found, the pages after it never   */
/* walked) and cannot say what was lost. btree_check_tolerant keeps    */
/* going: an unreadable page is recorded as a quarantine range and     */
/* skipped, and every other page is still verified. A STRUCTURAL       */
/* failure (cycle, shared child, level or ordering violation) is a      */
/* different animal -- it means the tree logic is broken, not the      */
/* media -- and is reported as such and never tolerated.               */
/* ------------------------------------------------------------------ */

/* Record one quarantined range. 0 = recorded, -1 = it cannot be represented
 * (the array is full, or a bound is longer than any key this engine writes).
 * A half-recorded range would be a WILDCARD -- "matches everything" -- and the
 * excision would drop the whole tree, so an unrepresentable range is never
 * left behind: the caller sees -1, sets qfull and refuses the repair. */
static int bt_ck_quarantine_add(bt_ck *ck, invfs_blkptr ptr, bt_key lo, bt_key hi)
{
    bt_range r;

    if (!ck->q)
        return 0;                    /* the caller did not ask for ranges */
    if (ck->q->n >= BT_QUARANTINE_MAX)
        return -1;
    memset(&r, 0, sizeof r);
    r.pba = ptr.pba;
    if (lo.n) {
        if (lo.n > BT_QUARANTINE_KEY_MAX)
            return -1;
        memcpy(r.lo, lo.p, lo.n);
        r.lo_n = lo.n;
    }
    if (hi.n) {
        if (hi.n > BT_QUARANTINE_KEY_MAX)
            return -1;
        memcpy(r.hi, hi.p, hi.n);
        r.hi_n = hi.n;
    } else {
        r.hi_unbounded = 1;
    }
    ck->q->range[ck->q->n++] = r;
    return 0;
}

/* The same walk as bt_check_rec, but an unreadable page is recorded and
 * skipped instead of aborting. Returns 0 when the readable part of the subtree
 * is sound, -1 on a structural failure (see the header). */
static int bt_check_tol_rec(invfs_volume *v, invfs_blkptr ptr, int expect_level,
                            bt_key lo, bt_key hi, uint32_t depth, bt_ck *ck)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    bt_ent *e = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
    int n, level, i;

    if (!e)
        return -1;
    if (ptr.pba == 0 || ptr.pba >= ck->total) {
        bt_ck_err(ck, "null/out-of-range child pba");
        free(e);
        return -1;
    }
    if (bit_get(ck->seen, ptr.pba)) {
        bt_ck_err(ck, "cycle or shared child");
        free(e);
        return -1;
    }
    bit_set(ck->seen, ptr.pba);
    ck->pages++;

    if (bt_read(v, ptr, buf, e, &n, &level) != 0) {
        bt_ck_err(ck, "unreadable page (bad CRC/gen/encoding)");
        ck->bad++;
        if (bt_ck_quarantine_add(ck, ptr, lo, hi) != 0)
            ck->qfull = 1;
        free(e);
        return 0;               /* contained: the caller keeps walking */
    }
    if (expect_level >= 0 && level != expect_level) {
        bt_ck_err(ck, "level not monotone");
        free(e);
        return -1;
    }

    if (level == INVFS_PAGE_LEVEL_LEAF) {
        if (ck->height == 0)
            ck->height = depth;
        else if (ck->height != depth) {
            bt_ck_err(ck, "leaves at differing depths");
            free(e);
            return -1;
        }
        if (n < 1) {
            bt_ck_err(ck, "empty leaf");
            free(e);
            return -1;
        }
        for (i = 0; i < n; i++) {
            if (i && vol_key_cmp(e[i - 1].k, e[i - 1].klen, e[i].k, e[i].klen) >= 0) {
                bt_ck_err(ck, "leaf keys not strictly ordered");
                free(e);
                return -1;
            }
            if (lo.n && vol_key_cmp(e[i].k, e[i].klen, lo.p, lo.n) < 0) {
                bt_ck_err(ck, "leaf key below parent bound");
                free(e);
                return -1;
            }
            if (hi.n && vol_key_cmp(e[i].k, e[i].klen, hi.p, hi.n) >= 0) {
                bt_ck_err(ck, "leaf key above parent bound");
                free(e);
                return -1;
            }
        }
        ck->keys += (uint64_t)n;
        free(e);
        return 0;
    }

    if (n < 2) {
        bt_ck_err(ck, "internal node with fewer than 2 children");
        free(e);
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (i && vol_key_cmp(e[i - 1].k, e[i - 1].klen, e[i].k, e[i].klen) >= 0) {
            bt_ck_err(ck, "separators not strictly ordered");
            free(e);
            return -1;
        }
        if (lo.n && vol_key_cmp(e[i].k, e[i].klen, lo.p, lo.n) < 0) {
            bt_ck_err(ck, "separator below parent bound");
            free(e);
            return -1;
        }
        if (hi.n && vol_key_cmp(e[i].k, e[i].klen, hi.p, hi.n) >= 0) {
            bt_ck_err(ck, "separator above parent bound");
            free(e);
            return -1;
        }
    }
    for (i = 0; i < n; i++) {
        bt_key clo, chi;
        clo.p = e[i].k;
        clo.n = e[i].klen;
        chi.p = (i + 1 < n) ? e[i + 1].k : hi.p;
        chi.n = (i + 1 < n) ? e[i + 1].klen : hi.n;
        if (bt_check_tol_rec(v, e[i].child, level - 1, clo, chi,
                             depth + 1, ck) != 0) {
            free(e);
            return -1;
        }
    }
    free(e);
    return 0;
}

int btree_check_tolerant(invfs_volume *v, invfs_blkptr root, bt_stat *out,
                         bt_quarantine *q, char *err, size_t errlen)
{
    bt_ck ck;
    uint64_t bytes;
    int rc;

    if (!v)
        return -1;
    if (out) {
        out->n_pages = 0;
        out->nkeys = 0;
        out->height = 0;
    }
    if (err && errlen)
        err[0] = 0;
    if (q) {
        memset(q, 0, sizeof *q);
        q->qfull = 0;
    }
    if (root.pba == 0)
        return 0;

    bytes = (v->sb.total_blocks + 7u) / 8u;
    memset(&ck, 0, sizeof ck);
    ck.seen = (uint8_t *)calloc(1, (size_t)bytes);
    if (!ck.seen)
        return -1;
    ck.total = v->sb.total_blocks;
    ck.err = err;
    ck.errlen = errlen;
    ck.q = q;
    rc = bt_check_tol_rec(v, root, -1, (bt_key){NULL, 0}, (bt_key){NULL, 0},
                          1, &ck);
    if (out) {
        out->n_pages = ck.pages;
        out->nkeys = ck.keys;
        out->height = ck.height;
    }
    if (q) {
        q->bad_pages = ck.bad;
        q->qfull = ck.qfull;
    }
    free(ck.seen);
    return rc;
}

/* ------------------------------------------------------------------ */
/* WP86: excise the quarantined key ranges from the tree             */
/*                                                                   */
/* The repair is NOT a base rebuild. A damaged page cannot be read, so */
/* its keys cannot be copied anywhere; but the tree becomes sound again */
/* by dropping the parent entry that points at it. Every other key     */
/* keeps its page, so the excision costs O(height) page writes and the  */
/* reachability diff frees exactly the pages the dropped subtree held. */
/* The COW discipline is the same as every other mutating call: new     */
/* pages, no publish, and the caller barriers and publishes the new     */
/* root through RT30.                                                  */
/*                                                                   */
/* Stated so no caller is surprised: a key inside an excised range stops */
/* being EIO and becomes "absent". That is the point of -f -- the bytes */
/* are gone and the pass says so loudly -- which is why the excision   */
/* lives behind an explicit fix flag and never in the read path. Keys  */
/* the delta still holds are re-inserted by the fold that follows, so    */
/* only the keys that existed solely in the damaged page are lost.       */
/* ------------------------------------------------------------------ */

/* Is [clo, chi) contained in the quarantine range r? A zero-length chi means
 * unbounded, which can only be contained by an unbounded hi. */
static int bt_range_covers(const bt_range *r, bt_key clo, bt_key chi)
{
    if (r->lo_n && vol_key_cmp(clo.p, clo.n, r->lo, r->lo_n) < 0)
        return 0;
    if (r->hi_n) {
        if (!chi.n)
            return 0;
        if (vol_key_cmp(chi.p, chi.n, r->hi, r->hi_n) > 0)
            return 0;
    }
    return 1;
}

/* One level of the excision. Drops every child entry whose key range falls
 * inside a quarantine range, recurses into the entries that only partially
 * overlap, and collapses a node left with fewer than two children
 * (btree_check requires two, so a one-child node must be replaced by its
 * child on the way up). lo/hi bound this node's own range. */
static int bt_excise_rec(invfs_volume *v, invfs_blkptr node, uint64_t gen,
                         const bt_quarantine *q, bt_key lo, bt_key hi,
                         int level_expect, bt_excise *out)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    uint8_t *kfix = NULL;      /* owned replacement separators */
    bt_ent *e;
    int n, level, i, keep, rc = 0;

    e = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
    if (!e)
        return -1;
    if (bt_read(v, node, buf, e, &n, &level) != 0) {
        free(e);
        return -1;
    }
    if (level_expect >= 0 && level != level_expect) {
        free(e);
        return -1;
    }
    if (level == INVFS_PAGE_LEVEL_LEAF) {
        /* A readable leaf holds no quarantine range (a range only ever comes
         * from an unreadable page), so there is nothing to drop here. */
        free(e);
        out->node = node;
        return 0;
    }

    keep = 0;
    for (i = 0; i < n; i++) {
        bt_key clo, chi;
        int drop = 0, j;

        clo.p = e[i].k;
        clo.n = e[i].klen;
        chi.p = (i + 1 < n) ? e[i + 1].k : hi.p;
        chi.n = (i + 1 < n) ? e[i + 1].klen : hi.n;
        for (j = 0; j < q->n; j++) {
            if (bt_range_covers(&q->range[j], clo, chi)) {
                drop = 1;
                break;
            }
        }
        if (drop) {
            out->dropped++;
            out->changed = 1;
            continue;
        }
        {
            bt_excise sub;
            memset(&sub, 0, sizeof sub);
            if (bt_excise_rec(v, e[i].child, gen, q, clo, chi, level - 1,
                              &sub) != 0) {
                rc = -1;
                goto done;
            }
            out->dropped += sub.dropped;
            if (!sub.changed) {
                e[keep] = e[i];        /* untouched subtree: alias it */
                keep++;
                continue;
            }
            if (sub.node.pba == 0) {
                out->changed = 1;      /* the child collapsed: drop it too */
                continue;
            }
            e[keep] = e[i];
            if (sub.min_n) {
                /* The child's minimum key moved (it lost its first child):
                 * the separator has to follow it, or the parent's own bound
                 * check would reject the tree. The replacement lives in an
                 * owned arena -- the borrowed page bytes stay read-only. */
                if (!kfix) {
                    kfix = (uint8_t *)calloc((size_t)n,
                                              BT_QUARANTINE_KEY_MAX);
                    if (!kfix) {
                        rc = -1;
                        goto done;
                    }
                }
                memcpy(kfix + (size_t)keep * BT_QUARANTINE_KEY_MAX,
                       sub.min_key, sub.min_n);
                e[keep].k = kfix + (size_t)keep * BT_QUARANTINE_KEY_MAX;
                e[keep].klen = sub.min_n;
            }
            e[keep].child = sub.node;
            keep++;
        }
    }

    if (!out->changed) {
        out->node = node;             /* alias: no COW churn */
        goto done;
    }
    if (keep == 0) {
        /* the whole subtree is gone: the caller drops this entry */
        out->dropped++;
        memset(out, 0, sizeof *out);
        out->changed = 1;
        goto done;
    }
    if (keep < 2) {
        /* btree_check requires >= 2 children, so hand the survivor up and
         * let the parent drop this node; its minimum key becomes the
         * parent's separator. */
        uint8_t kbuf[BT_QUARANTINE_KEY_MAX];
        uint16_t kn = e[0].klen;
        invfs_blkptr child = e[0].child;
        uint64_t dropped = out->dropped;
        if (kn > BT_QUARANTINE_KEY_MAX)
            kn = BT_QUARANTINE_KEY_MAX;
        memcpy(kbuf, e[0].k, kn);
        memset(out, 0, sizeof *out);
        out->dropped = dropped;
        out->node = child;
        out->min_n = kn;
        memcpy(out->min_key, kbuf, kn);
        out->changed = 1;
        goto done;
    }
    {
        invfs_blkptr nw;
        if (bt_write(v, level, gen, e, keep, &nw) != 0) {
            rc = -1;
            goto done;
        }
        out->node = nw;
        out->changed = 1;
    }
done:
    free(kfix);
    free(e);
    return rc;
}

int btree_excise(invfs_volume *v, invfs_blkptr root, const bt_quarantine *q,
                 invfs_blkptr *new_root_out)
{
    bt_excise top;
    uint64_t gen;

    if (!v || !q || !new_root_out)
        return -1;
    *new_root_out = root;
    if (root.pba == 0 || q->n == 0)
        return 0;
    if (q->qfull)
        return -1;       /* the set is partial: excising it would be a guess */
    memset(&top, 0, sizeof top);
    /* every rewritten page -- the root included -- carries a fresh gen, so
     * the RT30 slot selection ("higher gen wins") cannot pick the old root */
    gen = root.gen + 1;
    if (bt_excise_rec(v, root, gen, q, (bt_key){NULL, 0}, (bt_key){NULL, 0},
                      -1, &top) != 0)
        return -1;
    if (top.dropped == 0 && !top.changed)
        return 0;                    /* nothing matched: the tree stands */
    if (top.node.pba == 0) {
        /* everything under the root was quarantined: the convention is a
         * fresh empty leaf (v3_publish), never a null root slot. */
        uint8_t page[INVFS_BLOCK_SIZE];
        uint64_t pba;
        pba = mbuf_alloc(v, gen);
        /* WP126: tell the orphan collector this is a base page, so it
         * never has to look for it in the block space. */
        btree_orphan_note_alloc(v, pba);
        if (!pba)
            return -1;
        mbuf_page_init(page, INVFS_PAGE_LEVEL_LEAF, gen);
        if (mbuf_write(v, pba, page) != 0) {
            mbuf_free(v, pba);
            return -1;
        }
        mbuf_ptr_set(new_root_out, pba, page,
                     INVFS_BP_LEAF | INVFS_BP_ROOT);
        return 1;
    }
    if (top.min_n) {
        /* The root itself was replaced by its only surviving child (a root
         * with two children, one of them unreadable -- a small volume). That
         * child carries an OLDER gen, and RT30 keeps whichever slot holds
         * the higher one, so publishing it as it stands would let the next
         * open pick the pre-repair root back and the damage with it. Recopy
         * it at root.gen + 1, exactly as btree_delete does for a root
         * collapse. */
        invfs_blkptr rc;
        if (bt_recopy(v, top.node, root.gen + 1, &rc) != 0)
            return -1;
        rc.flags |= INVFS_BP_ROOT;
        *new_root_out = rc;
        return 1;
    }
    *new_root_out = top.node;   /* the root page was rewritten at gen + 1 */
    return 1;
}

/* The two liveness primitives the repair gates on. Both are pure interval
 * arithmetic over the quarantine set -- no I/O -- so a caller can ask about
 * every key a live object requires without touching the damaged page. */

/* Is `k` inside any quarantined range? [lo, hi), an empty `hi` is unbounded. */
int btree_quarantine_has(const bt_quarantine *q, const uint8_t *k, uint16_t klen)
{
    int j;
    if (!q)
        return 0;
    for (j = 0; j < q->n; j++) {
        const bt_range *r = &q->range[j];
        if (r->lo_n && vol_key_cmp(k, klen, r->lo, r->lo_n) < 0)
            continue;
        if (r->hi_n) {
            if (vol_key_cmp(k, klen, r->hi, r->hi_n) >= 0)
                continue;
        }
        return 1;
    }
    return 0;
}

/* Does any quarantined range intersect the key space [lo, hi)? An empty `hi`
 * is unbounded. Two half-open intervals meet iff each starts before the other
 * ends -- with unbounded ends comparing as +infinity. This is the coarser of
 * the two on purpose: a caller that knows only "every key of inode N's xattrs
 * is somewhere in here" has an INTERVAL, not a key, and the honest question
 * is whether that interval meets a quarantined one. */
int btree_quarantine_overlaps(const bt_quarantine *q,
                              const uint8_t *lo, uint16_t lo_n,
                              const uint8_t *hi, uint16_t hi_n)
{
    int j;
    if (!q)
        return 0;
    for (j = 0; j < q->n; j++) {
        const bt_range *r = &q->range[j];
        /* quarantine.lo < space.hi ? */
        if (hi_n) {
            if (r->lo_n && vol_key_cmp(r->lo, r->lo_n, hi, hi_n) >= 0)
                continue;
        }
        /* space.lo < quarantine.hi ? */
        if (r->hi_n) {
            if (lo_n && vol_key_cmp(lo, lo_n, r->hi, r->hi_n) >= 0)
                continue;
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* reachability-diff reclaim                                          */
/* ------------------------------------------------------------------ */

static int bt_mark_rec(invfs_volume *v, invfs_blkptr ptr, uint8_t *seen,
                       uint64_t total)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    bt_ent *e;
    int n, level, i;

    if (ptr.pba == 0)
        return 0;
    if (ptr.pba >= total)
        return -1;
    if (bit_get(seen, ptr.pba))
        return 0;
    bit_set(seen, ptr.pba);
    if (mbuf_read_ptr(v, &ptr, buf) != 0)
        return -1;
    if (mbuf_page_chdr(buf)->level == INVFS_PAGE_LEVEL_LEAF)
        return 0;
    e = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
    if (!e)
        return -1;
    if (bt_read(v, ptr, buf, e, &n, &level) != 0) {
        free(e);
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (bt_mark_rec(v, e[i].child, seen, total) != 0) {
            free(e);
            return -1;
        }
    }
    free(e);
    return 0;
}

static int bt_free_rec(invfs_volume *v, invfs_blkptr ptr, uint8_t *seen,
                       uint64_t total, uint64_t *freed)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    bt_ent *e;
    int n, level, i;

    if (ptr.pba == 0)
        return 0;
    if (ptr.pba >= total)
        return -1;
    if (bit_get(seen, ptr.pba))
        return 0;   /* still reachable from the kept root */
    bit_set(seen, ptr.pba);
    if (mbuf_read_ptr(v, &ptr, buf) != 0)
        return -1;

    if (mbuf_page_chdr(buf)->level != INVFS_PAGE_LEVEL_LEAF) {
        e = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
        if (!e)
            return -1;
        if (bt_read(v, ptr, buf, e, &n, &level) != 0) {
            free(e);
            return -1;
        }
        mbuf_free(v, ptr.pba);
        (*freed)++;
        for (i = 0; i < n; i++) {
            if (bt_free_rec(v, e[i].child, seen, total, freed) != 0) {
                free(e);
                return -1;
            }
        }
        free(e);
        return 0;
    }
    mbuf_free(v, ptr.pba);
    (*freed)++;
    return 0;
}

int btree_reclaim(invfs_volume *v, invfs_blkptr old_root, invfs_blkptr keep_root)
{
    uint8_t *seen;
    uint64_t bytes, freed = 0;

    if (!v)
        return -1;
    if (old_root.pba == 0)
        return 0;
    bytes = (v->sb.total_blocks + 7u) / 8u;
    seen = (uint8_t *)calloc(1, (size_t)bytes);
    if (!seen)
        return -1;
    if (bt_mark_rec(v, keep_root, seen, v->sb.total_blocks) != 0) {
        free(seen);
        return -1;
    }
    if (bt_free_rec(v, old_root, seen, v->sb.total_blocks, &freed) != 0) {
        free(seen);
        return -1;
    }
    free(seen);
    return (int)freed;
}

/* WP77: same reachability diff, but pages reachable from a live save
 * point's pinned root are kept too (the save point must be able to
 * restore them). pinned_root.pba == 0 is the no-save-point case and is
 * identical to btree_reclaim. */
int btree_reclaim_pinned(invfs_volume *v, invfs_blkptr old_root,
                         invfs_blkptr keep_root, invfs_blkptr pinned_root)
{
    uint8_t *seen;
    uint64_t bytes, freed = 0;

    if (!v)
        return -1;
    if (old_root.pba == 0)
        return 0;
    bytes = (v->sb.total_blocks + 7u) / 8u;
    seen = (uint8_t *)calloc(1, (size_t)bytes);
    if (!seen)
        return -1;
    if (bt_mark_rec(v, keep_root, seen, v->sb.total_blocks) != 0 ||
        (pinned_root.pba != 0 &&
         bt_mark_rec(v, pinned_root, seen, v->sb.total_blocks) != 0)) {
        free(seen);
        return -1;
    }
    if (bt_free_rec(v, old_root, seen, v->sb.total_blocks, &freed) != 0) {
        free(seen);
        return -1;
    }
    free(seen);
    return (int)freed;
}

/* ------------------------------------------------------------------ */
/* WP121: orphan collector. WP126 makes its COST bounded.               */
/*                                                                   */
/* Why this exists, and why it is not btree_reclaim_pinned: every       */
/* publisher of a new base root (v3_publish, vol_btree.c:1993)         */
/* abandons the root it supersedes and frees nothing.                   */
/* btree_reclaim_pinned is a ONE-GENERATION diff: it is handed the      */
/* previous root and frees what that root reaches which the new one    */
/* does not. A root abandoned two publications ago is reachable from   */
/* neither argument, so it is never even visited. Measured on a        */
/* 60-file corpus: 198 allocated base pages, 5 in the live tree, 193   */
/* orphans -- one abandoned page per generation, linear in file count  */
/* and independent of file size.                                        */
/*                                                                   */
/* THE LIVENESS PREDICATE. This is the load-bearing part. RT30 has     */
/* TWO root slots and mbuf_root_publish only ever writes ONE of them   */
/* per publish (vol_metabuf.c:349: slot = rt30.seq & 1), so the other  */
/* slot still names the previous root -- that is WP86's damage         */
/* tolerance, not a bug to clean up here. mbuf_root_read               */
/* (vol_metabuf.c:375) loops over BOTH slots and adopts the highest    */
/* gen among the ones it accepts.                                      */
/*                                                                   */
/* WP121 wrote the next point as "mbuf_page_validate checks magic +    */
/* CRC32C and NOTHING else, so a freed page still validates, and the   */
/* reader silently adopts the OLDER slot." Both halves were true then. */
/* WP-D closed them: mbuf_root_read and this file's orphan_slot_ptr    */
/* now consult the allocation bitmap BEFORE the page bytes, and        */
/* mbuf_read_ptr does the same for every page a tree walk reaches. A  */
/* slot whose block has been freed is refused, loudly and by name,     */
/* instead of adopted; a subtree whose pages have been freed fails the */
/* walk with EIO instead of being read.                                */
/*                                                                   */
/* The predicate below is STILL load-bearing. The reader-side check is */
/* a backstop that turns a silent corruption into a reported one; it   */
/* does not make freeing a page a live root names CORRECT. A volume    */
/* that trips it has already lost the namespace those pages described, */
/* whatever the reader then does with it.                              */
/*                                                                   */
/* Therefore:                                                           */
/*                                                                   */
/*   LIVE(P)  <=>  P is reachable from a root named by ANY of the     */
/*                2 RT30 slots, or from v->pinned_root (a live SPT0    */
/*                save point), or P is named by a blkptr stored       */
/*                inside a value of such a tree.                        */
/*                                                                   */
/* "Any of the 2 slots", never "the newest slot" and never "the        */
/* current root": a depth-2 root stack BOUNDS the leak, it does not    */
/* make freeing safe. Freeing slot[older]'s exclusive pages converts a  */
/* recoverable single-page root tear into a silent adoption of a       */
/* namespace that is half-missing, and no existing test would notice.  */
/*                                                                   */
/* Every other rule below only ever ADDS to the live set, so each of    */
/* them is a one-directional safety margin:                              */
/*   - a slot that cannot be read into a valid blkptr aborts the       */
/*     whole collection (we refuse to reason about a half-known RT30); */
/*   - a page whose gen is greater than the newest live root's gen is   */
/*     treated as LIVE (an uncommitted COW copy);                      */
/*   - only blocks that validate as a BPG3 base page are candidates, so */
/*     no other block type can be collected by construction;           */
/*   - an unreadable candidate is left alone.                           */
/* ------------------------------------------------------------------ */

/* WHAT WP126 CHANGED, AND WHAT IT DID NOT. It changed WHERE the        */
/* collector looks and HOW MUCH that costs. It did not change a single   */
/* one of the rules above; the predicate below is WP121's, verbatim,    */
/* and a page it frees is a page WP121's full-pool sweep would also     */
/* have freed. The red control in tools/test-v3-orphan-reclaim.sh      */
/* still exercises this code, not a copy of it.                         */
/*                                                                   */
/* WP121's cost: one 4 KiB block read per ALLOCATED block, per CALL --  */
/* 48,732 reads on a 1 GB Silesia volume -- and fold_reclaim_hook calls */
/* it on every fold, so a volume that folds often pays the whole volume */
/* every fold. That is the whole reason INVFS_RECLAIM_ORPHANS is        */
/* default-off: not because the collector is wrong, but because it is   */
/* O(volume) on a path a live root filesystem takes thousands of times  */
/* a minute.                                                           */
/*                                                                   */
/* The fix is to stop asking "is this block a base page?" about every   */
/* block in the volume. A v3 base page is a page mbuf_alloc handed      */
/* out, so the collector keeps a CANDIDATE SET of the base pages this  */
/* handle knows about, fed by exactly two sources:                     */
/*                                                                   */
/*   (a) btree_orphan_note_alloc(), called at every mbuf_alloc() site  */
/*       that produces a base page (4 in this file, 1 in vol_fold.c).  */
/*       That is EXACT for anything allocated in this session, costs    */
/*       nothing, and is the steady state: a fold's cost is then one   */
/*       read per live base page plus one per orphan, which is a        */
/*       function of the METADATA and not of the volume at all.        */
/*                                                                   */
/*   (b) A one-pass, budgeted, cursor-carrying scan of the allocation  */
/*       bitmap -- "the seed pass" -- which is the only way to find the */
/*       base pages a PREVIOUS session allocated, because that knowledge */
/*       is not on disk. It reads at most `budget` blocks per call,     */
/*       walks free space a byte at a time, wraps once and then stops   */
/*       for the rest of the open. So the O(volume) term is paid ONCE  */
/*       per open, spread over >= ceil(allocated/budget) folds, and     */
/*       never again.                                                  */
/*                                                                   */
/* The failure mode of a missing candidate is a LEAKED page, never a    */
/* wrong free: the set is a filter that narrows the search, and the     */
/* predicate is unchanged once a page is in it. A base page allocated   */
/* through a path that forgets note_alloc() is still found by the seed */
/* pass. That asymmetry is the reason this shape was chosen over the   */
/* alternatives: an incremental CURSOR over the raw block space        */
/* (without a candidate set) spends its whole per-call read budget on   */
/* DATA blocks that can never be base pages, so on a large volume it   */
/* never even reaches the metadata between calls, and rate-limiting    */
/* alone still leaves the per-call cost O(allocated).                  */
/* ------------------------------------------------------------------ */

/* Deepest tree we will believe. A page claiming a deeper level than any  */
/* real v3 tree is not a page we understand, and an un-understood page is */
/* never freed. */
#define ORPHAN_MAX_LEVEL 32u

/* Seed-pass reads per call. This is the ONLY term that is not a function
 * of the metadata, it is hard-capped, and it goes to zero for good once
 * the seed pass has wrapped the block space. 1024 reads = 4 MiB of reads
 * on a fold, which is invisible next to the fold's own I/O, and it makes
 * a 1 GB volume seed in <= 40 folds. INVFS_RECLAIM_SCAN_BUDGET overrides
 * it (0 = unlimited, i.e. WP121's one-shot behaviour). */
#define ORPHAN_SEED_BUDGET_DEFAULT 1024u

/* A drain (the offline sweep) stops after this many collector calls even
 * if it has not settled. It exists to bound a loop, not to bound work. */
#define ORPHAN_DRAIN_MAX_ROUNDS 0x400000u

/* Resolved once per process, like vol_reclaim.c's orphan_gate: a volume is
 * opened once per process and the budget is a policy, not a per-call input.
 * 0 means "not resolved yet"; a resolved 0 is normalised to unlimited. */
static uint64_t g_orph_budget = 0;

static uint64_t orph_budget(invfs_volume *v)
{
    if (v->orph.budget)
        return v->orph.budget;
    if (!g_orph_budget) {
        const char *e = getenv("INVFS_RECLAIM_SCAN_BUDGET");
        g_orph_budget = (e && e[0]) ? strtoull(e, NULL, 10)
                                    : (uint64_t)ORPHAN_SEED_BUDGET_DEFAULT;
        if (!g_orph_budget)
            g_orph_budget = UINT64_MAX;
    }
    return g_orph_budget;
}

/* The membership bit per block. Allocated lazily on the first
 * note_alloc/collect, and its presence is the only thing that says the
 * candidate set is usable -- a handle that cannot allocate it collects
 * nothing rather than collecting without a filter. */
static int orph_set_ready(invfs_volume *v)
{
    uint64_t bytes;

    if (v->orph.inlist)
        return 0;
    bytes = (v->sb.total_blocks + 7u) / 8u;
    v->orph.inlist = (uint8_t *)calloc(1, (size_t)bytes);
    return v->orph.inlist ? 0 : -1;
}

static int orph_cand_add(invfs_volume *v, uint64_t pba)
{
    if (pba == 0 || pba >= v->sb.total_blocks)
        return 0;
    if (bit_get(v->orph.inlist, pba))
        return 0;
    if (v->orph.n == v->orph.cap) {
        size_t nc = v->orph.cap ? v->orph.cap * 2 : 64;
        uint64_t *np = (uint64_t *)realloc(v->orph.pba, nc * sizeof *np);
        if (!np)
            return -1;      /* the seed pass will find it instead */
        v->orph.pba = np;
        v->orph.cap = nc;
    }
    bit_set(v->orph.inlist, pba);
    v->orph.pba[v->orph.n++] = pba;
    if (v->orph.n > v->orph.peak)
        v->orph.peak = v->orph.n;
    return 0;
}

void btree_orphan_note_alloc(invfs_volume *v, uint64_t pba)
{
    if (!v || pba == 0 || pba >= v->sb.total_blocks || !v->bitmap)
        return;
    if (orph_set_ready(v) != 0)
        return;             /* no filter -> no candidates; the seed pass retries */
    (void)orph_cand_add(v, pba);
}

/* THE SEED PASS. One cursor-carrying lap of the allocation bitmap, budgeted
 * in block reads, that teaches the collector about the base pages an earlier
 * session allocated. Free space costs one byte test per eight blocks and no
 * I/O; only an ALLOCATED block costs a read, and only an allocated block
 * that validates as a base page joins the candidate set.
 *
 * It runs at most once per open. After the wrap, seed_done stays set and
 * this function costs two loads and a branch for the rest of the session --
 * which is what makes the steady-state per-fold cost independent of volume
 * size. */
static void orph_seed_scan(invfs_volume *v, uint64_t budget, uint8_t *page)
{
    uint64_t total = v->sb.total_blocks, b, reads = 0;

    if (v->orph.seed_done || !v->bitmap)
        return;
    b = v->orph.seed_cursor ? v->orph.seed_cursor : 1;
    while (b < total && reads < budget) {
        if (v->bitmap[b >> 3] == 0) {
            b += 8;         /* eight free blocks for one byte test */
            continue;
        }
        if (mbuf_read(v, b, page) != 0) {
            b++;
            continue;       /* unreadable: leave it alone, as always */
        }
        reads++;
        v->orph.seed_reads++;
        if (mbuf_page_validate(page) &&
            mbuf_page_chdr(page)->level <= ORPHAN_MAX_LEVEL)
            (void)orph_cand_add(v, b);
        b++;
    }
    v->orph.seed_cursor = b;
    if (b >= total) {
        v->orph.seed_done = 1;
        v->orph.seed_cursor = 1;
    }
}

/* Offset of invfs_blkptr recipe inside invfs_v3_inode_row. The row is the */
/* only leaf VALUE in the v3 base trees that can contain a blkptr (every   */
/* other value is a dirent child id or an opaque recipe chunk). Today       */
/* every producer memsets it to zero -- vol_dirs.c:376,379,437,            */
/* vol_png.c:1081,1190, vol_textzone.c:1237, vol_write.c:905 and            */
/* vol_records.c:1229 only ever copy it -- so the guard below is a no-op in */
/* practice. It is here so that the day a producer starts populating it,    */
/* this collector cannot be the thing that frees the page it points at. The  */
/* _Static_assert below is what keeps the constant honest: a struct change */
/* that moves the field breaks the build instead of silently disabling the */
/* guard.                                                                    */
#define ORPHAN_ROW_OFF 54u
_Static_assert(offsetof(invfs_v3_inode_row, recipe) == ORPHAN_ROW_OFF,
               "ORPHAN_ROW_OFF must track invfs_v3_inode_row.recipe");

/* Does this leaf value look like an encoded inode row (the only shape    */
/* that carries a blkptr)? Deliberately strict: a false positive costs a   */
/* spurious block read, a false negative is the failure we are guarding    */
/* against, so the version word, the size window and the type field all  */
/* have to agree before the recipe pointer is even looked at.              */
static int orphan_row_recipe(const uint8_t *val, uint16_t n, uint64_t *pba_out)
{
    uint32_t ver;
    uint32_t type;
    uint64_t pba;

    if (n < ORPHAN_ROW_OFF + sizeof(invfs_blkptr))
        return 0;
    if (n > sizeof(invfs_v3_inode_row) + INVFS_V3_INODE_XATTR_MAX)
        return 0;
    memcpy(&ver, val, sizeof ver);
    memcpy(&type, val + 4, sizeof type);
    if (ver != 1u && ver != INVFS_V3_INODE_ROW_VERSION)
        return 0;
    if (type > INVFS_ITYP_BLK)
        return 0;
    memcpy(&pba, val + ORPHAN_ROW_OFF, sizeof pba);
    *pba_out = pba;
    return 1;
}

/* Mark walk used only by the collector. Identical to bt_mark_rec except  */
/* that on a leaf it also follows the recipe blkptr of any inode row it   */
/* finds -- see the ORPHAN_ROW_OFF comment. bt_mark_rec is left alone:    */
/* the reachability diff's contract is tree structure, and changing it    */
/* would change the existing reclaim's behaviour in the same commit.     */
static int bt_mark_rec_deep(invfs_volume *v, invfs_blkptr ptr, uint8_t *seen,
                            uint64_t total)
{
    uint8_t buf[INVFS_BLOCK_SIZE];
    bt_ent *e;
    int n, level, i;

    if (ptr.pba == 0)
        return 0;
    if (ptr.pba >= total)
        return -1;
    if (bit_get(seen, ptr.pba))
        return 0;
    bit_set(seen, ptr.pba);
    v->orph.mark_reads++;
    if (mbuf_read_ptr(v, &ptr, buf) != 0)
        return -1;
    if (mbuf_page_chdr(buf)->level == INVFS_PAGE_LEVEL_LEAF) {
        e = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
        if (!e)
            return -1;
        if (bt_read(v, ptr, buf, e, &n, &level) != 0) {
            free(e);
            return -1;
        }
        for (i = 0; i < n; i++) {
            uint64_t pba;
            invfs_blkptr rp;
            if (!orphan_row_recipe(e[i].v, e[i].vlen, &pba) || !pba)
                continue;
            if (pba >= total)
                continue;   /* already a broken row; reads fail loudly */
            if (bit_get(seen, pba))
                continue;
            memset(&rp, 0, sizeof rp);
            rp.pba = pba;
            if (bt_mark_rec_deep(v, rp, seen, total) != 0) {
                free(e);
                return -1;
            }
        }
        free(e);
        return 0;
    }
    e = (bt_ent *)malloc(sizeof(bt_ent) * BT_MAX_ENTRIES);
    if (!e)
        return -1;
    if (bt_read(v, ptr, buf, e, &n, &level) != 0) {
        free(e);
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (bt_mark_rec_deep(v, e[i].child, seen, total) != 0) {
            free(e);
            return -1;
        }
    }
    free(e);
    return 0;
}

/* Read an RT30 slot into a blkptr. Returns 0 on success, -1 if the slot
 * names a block we cannot turn into a trustworthy pointer. The caller
 * treats -1 as "abort the collection", never as "this slot is dead".
 *
 * WP-D: ask the allocation question before the integrity one, exactly as
 * mbuf_root_read does. This function has its own mbuf_read + validate
 * rather than going through mbuf_read_ptr (it has to build the blkptr from
 * the bytes it just read, so there is no pointer to verify yet), which made
 * it the one RT30 reader that still accepted a freed slot -- and the
 * collector's entire liveness set is derived from these two pointers. */
static int orphan_slot_ptr(invfs_volume *v, uint64_t pba, invfs_blkptr *out,
                           uint64_t *gen_out)
{
    uint8_t page[INVFS_BLOCK_SIZE];

    if (mbuf_page_allocated(v, pba) == 0)
        return -1;
    if (mbuf_read(v, pba, page) != 0)
        return -1;
    if (!mbuf_page_validate(page))
        return -1;
    mbuf_ptr_set(out, pba, page, INVFS_BP_LEAF | INVFS_BP_ROOT);
    if (gen_out)
        *gen_out = out->gen;
    return 0;
}

/* WP126: one bounded, incremental pass of the collector.
 *
 * The order of the three steps is the design, not an accident:
 *
 *   1. Learn the liveness roots (2 block reads). A slot we cannot turn into
 *      a trustworthy blkptr aborts the whole call BEFORE anything is
 *      freed, exactly as in WP121. This is why the mark walk can be moved
 *      below the candidate scan without weakening the predicate: the
 *      refuse-to-run decision does not depend on the mark set.
 *   2. Seed-scan a bounded slice of the block space and examine the whole
 *      candidate set. Neither of these can free anything, so both are
 *      free to be partial.
 *   3. Only if step 2 produced at least one page that LOOKS freeable,
 *      build the complete mark set and decide. This is the term that used
 *      to be paid unconditionally, and it is a function of the live tree,
 *      not of the volume.
 *
 * Steps 1-3 see exactly the same predicate as WP121's single full-pool
 * pass. A page this call does not look at is a page WP121 would have
 * reached on a later fold; a page it does look at gets WP121's verdict. */
int btree_collect_orphans(invfs_volume *v, uint64_t *freed_out)
{
    uint8_t *seen = NULL, page[INVFS_BLOCK_SIZE];
    uint64_t *cand = NULL, *cgen = NULL;   /* looks-freeable pba + its gen */
    uint64_t total, freed = 0, max_root_gen = 0, gen = 0;
    invfs_blkptr root[2];
    int have_root = 0, nroot = 0, ncand = 0, i;
    size_t k, keep;

    if (!v)
        return -1;
    if (freed_out)
        *freed_out = 0;
    /* No descriptor, or no in-RAM bitmap to work from: nothing to say, and
     * nothing that can change inside this open, so a drain must stop here
     * rather than spin on a state it can never leave. */
    if (!v->rt30_present || !v->bitmap) {
        v->orph.cands = 0;
        v->orph.settled = 1;
        return 0;
    }
    total = v->sb.total_blocks;
    if (orph_set_ready(v) != 0)
        return -1;                 /* no filter -> no collection at all */
    v->orph.calls++;

    /* (1) BOTH RT30 slots are liveness roots. See the header comment. */
    for (i = 0; i < 2; i++) {
        uint64_t pba = v->rt30.root_slot[i];
        if (!pba)
            continue;
        if (pba >= total || orphan_slot_ptr(v, pba, &root[nroot], &gen) != 0) {
            /* We do not fully know the RT30. Refuse to collect: a slot we
             * cannot read is exactly the slot the reader would fall back
             * to, and "probably unreachable" is how the volume loses a
             * namespace silently. */
            v->orph.cands = 0;
            v->orph.settled = 1;
            return 0;
        }
        v->orph.cand_reads++;
        if (!have_root || gen > max_root_gen) {
            max_root_gen = gen;
            have_root = 1;
        }
        nroot++;
    }
    if (!have_root) {
        /* RT30 names no root at all: there is no base tree to diff against,
         * so every BPG3 page would look like an orphan. Do not guess. */
        v->orph.cands = 0;
        v->orph.settled = 1;
        return 0;
    }

    /* (2) bounded seed slice + the whole candidate set. No frees here, so
     * a partial pass is always safe; it can only mean "not this call". */
    orph_seed_scan(v, orph_budget(v), page);

    if (v->orph.n) {
        cand = (uint64_t *)malloc(v->orph.n * sizeof *cand);
        cgen = (uint64_t *)malloc(v->orph.n * sizeof *cgen);
        if (!cand || !cgen) {
            free(cand);
            free(cgen);
            return -1;
        }
    }
    /* Compacting pass: this is the term that WP121 spent 48,732 reads on,
     * and it now spends one read per block that could POSSIBLY be a base
     * page. The full pool is not walked at all. */
    keep = 0;
    for (k = 0; k < v->orph.n; k++) {
        uint64_t b = v->orph.pba[k];
        const invfs_page_hdr *h;
        if (b == 0 || b >= total || !bit_get(v->bitmap, b)) {
            if (b < total)
                bit_clr(v->orph.inlist, b);
            continue;               /* somebody else freed it: drop it */
        }
        v->orph.pba[keep++] = b;     /* still allocated: keep it in the set */
        if (mbuf_read(v, b, page) != 0)
            continue;                /* unreadable: leave it alone */
        v->orph.cand_reads++;
        if (!mbuf_page_validate(page))
            continue;                /* not a base page: not ours */
        h = mbuf_page_chdr(page);
        if (h->level > ORPHAN_MAX_LEVEL)
            continue;                /* deeper than any real tree */
        if (h->gen > max_root_gen)
            continue;                /* uncommitted COW copy: live */
        cand[ncand] = b;
        cgen[ncand] = h->gen;
        ncand++;
    }
    v->orph.n = keep;
    v->orph.cands = (uint64_t)keep;
    if (ncand == 0) {
        /* Nothing even looks freeable, so the mark walk -- the only
         * unbounded-ish term left -- is not paid. On a settled volume this
         * is the steady state of a fold that allocated and published
         * without abandoning anything reclaimable. */
        free(cand);
        free(cgen);
        v->orph.settled = 0;
        return 0;
    }

    /* (3) a candidate exists, so pay for the complete mark set. */
    {
        uint64_t bytes = (total + 7u) / 8u;
        seen = (uint8_t *)calloc(1, (size_t)bytes);
        if (!seen) {
            free(cand);
            free(cgen);
            return -1;
        }
        for (i = 0; i < nroot; i++)
            if (bt_mark_rec_deep(v, root[i], seen, total) != 0) {
                free(seen);
                free(cand);
                free(cgen);
                return -1;
            }
        /* a live SPT0 save point pins its own base root. */
        if (v->pinned_root.pba != 0 &&
            bt_mark_rec_deep(v, v->pinned_root, seen, total) != 0) {
            free(seen);
            free(cand);
            free(cgen);
            return -1;
        }
    }

    for (i = 0; i < ncand; i++) {
        uint64_t b = cand[i];
        if (bit_get(seen, b))
            continue;                /* live: reachable from a root */
        if (cgen[i] > max_root_gen)
            continue;                /* the ceiling can only have risen */
        mbuf_free(v, b);
        if (bit_get(v->bitmap, b))
            continue;                /* retention held it: not freed */
        bit_clr(v->orph.inlist, b);
        freed++;
    }
    free(seen);
    free(cand);
    free(cgen);

    /* Drop the freed pages from the candidate set so it stays the size of
     * the live tree plus what this session has not reclaimed yet -- which
     * is the whole reason the next fold is cheap. */
    keep = 0;
    for (k = 0; k < v->orph.n; k++)
        if (bit_get(v->bitmap, v->orph.pba[k]))
            v->orph.pba[keep++] = v->orph.pba[k];
    v->orph.n = keep;

    if (freed_out)
        *freed_out = freed;
    v->orph.settled = (v->orph.seed_done && freed == 0) ? 1 : 0;
    v->orph.freed += freed;
    /* Make the frees durable in the same breath. A crash before this point
     * leaves the blocks allocated (a leak), never shared. */
    if (freed && vol_v3_bitmap_flush(v) != 0)
        return -1;
    return 0;
}

int btree_orphan_stats(const invfs_volume *v, struct invfs_orphan_stats *out)
{
    if (!v || !out)
        return -1;
    out->calls      = v->orph.calls;
    out->cand_reads = v->orph.cand_reads;
    out->seed_reads = v->orph.seed_reads;
    out->mark_reads = v->orph.mark_reads;
    out->freed      = v->orph.freed;
    out->cands      = v->orph.cands;
    out->peak       = v->orph.peak;
    out->settled    = v->orph.settled ? 1 : 0;
    return 0;
}

/* The OFFLINE drain. A sweep has no latency budget, so it should collect
 * everything collectable; folding does not, so folding must not. The loop
 * terminates when a whole pass over a non-empty candidate set freed nothing
 * AND the seed pass has wrapped, which is exactly "the full-pool sweep
 * would have had nothing left to do". */
int btree_collect_orphans_full(invfs_volume *v, uint64_t *freed_out)
{
    struct invfs_orphan_stats st;
    uint64_t total = 0, f = 0;
    unsigned long rounds = 0;
    int rc;

    if (freed_out)
        *freed_out = 0;
    for (;;) {
        rc = btree_collect_orphans(v, &f);
        if (rc != 0)
            return rc;
        total += f;
        if (btree_orphan_stats(v, &st) != 0)
            return -1;
        if (st.settled)
            break;
        if (++rounds >= ORPHAN_DRAIN_MAX_ROUNDS)
            break;
    }
    if (freed_out)
        *freed_out = total;
    return 0;
}

/* ------------------------------------------------------------------ */
/* WP-M5: v3 inode row codec + get/put/delete                          */
/*                                                                    */
/* The stable tier keeps one row per inode in the base B+-tree. The key */
/* is inode_id as u64 big-endian so the tree's byte-lexicographic order  */
/* equals numeric inode order (WP-M5 freezes this; WP-M6's dirent keys   */
/* live in a separate prefix namespace). The value is the versioned      */
/* invfs_v3_inode_row from invarifs.h.                                   */
/*                                                                    */
/* This WP does NOT free superseded pages (COW; the reachability diff    */
/* is btree_reclaim, scheduled by WP-M15) and does NOT implement the     */
/* delta tier (WP-M7): every put/delete rewrites the base tree and       */
/* publishes the root via the WP-M2 double slot.                         */
/* ------------------------------------------------------------------ */

/* Inode key: u64 big-endian, 8 bytes. */
static void v3_ino_key(uint64_t id, uint8_t k[8])
{
    int i;
    for (i = 0; i < 8; i++)
        k[i] = (uint8_t)(id >> (56 - 8 * i));
}

/* Encode the fixed row prefix. WP-M5 writes xattr_len == 0 (the xattr tree
 * is WP-M7); the field is frozen so that WP needs no format break. */
static uint16_t v3_ino_encode(const invfs_v3_inode *in, uint8_t *buf)
{
    invfs_v3_inode_row r;
    memset(&r, 0, sizeof r);
    r.row_version = INVFS_V3_INODE_ROW_VERSION;
    r.type        = in->type;
    r.mode        = in->mode;
    r.uid         = in->uid;
    r.gid         = in->gid;
    r.mtime       = in->mtime;
    r.atime       = in->atime;
    r.nlink       = in->nlink;
    r.rdev        = in->rdev;
    r.size        = in->size;
    r.recipe      = in->recipe;
    memcpy(r.recipe_addr, in->recipe_addr, INVFS_V3_RECIPE_ADDR_LEN);
    r.xattr_len   = 0;
    memcpy(buf, &r, sizeof r);
    return (uint16_t)sizeof r;
}

static int v3_ino_decode(const uint8_t *buf, uint16_t len, invfs_v3_inode *out)
{
    invfs_v3_inode_row r;
    if (len < INVFS_V3_INODE_ROW_FIXED)
        return -1;
    memcpy(&r, buf, sizeof r);
    if (r.row_version != INVFS_V3_INODE_ROW_VERSION)
        return -1;   /* a newer format: fail loudly, never misread */
    if (r.xattr_len > (uint32_t)(len - INVFS_V3_INODE_ROW_FIXED))
        return -1;
    out->type   = r.type;
    out->mode   = r.mode;
    out->uid    = r.uid;
    out->gid    = r.gid;
    out->mtime  = r.mtime;
    out->atime  = r.atime;
    out->nlink  = r.nlink;
    out->rdev   = r.rdev;
    out->size   = r.size;
    out->recipe = r.recipe;
    memcpy(out->recipe_addr, r.recipe_addr, INVFS_V3_RECIPE_ADDR_LEN);
    return 0;
}

/* Bring the v3 base engine up once per handle. mbuf_init resets the
 * bootstrap cursor, so calling it per operation would re-hand out the
 * reserved root-area pages; the open path sets v3_mbuf_ready after calling
 * it, and this guard covers callers that reach the API another way. */
static int v3_ready(invfs_volume *v)
{
    if (!v)
        return -1;
    if (!v->v3_mbuf_ready) {
        mbuf_init(v);
        /* WP-M5: do NOT draw base pages from the WP-M2 bootstrap pool (the
         * two pages WP-M1 reserved after the mapper table). mbuf_init resets
         * that cursor on every open, so handing the same pair out again in a
         * later session would let a COW write overwrite a page the current
         * root still references (an untouched sibling leaf, say). Base pages
         * come from the shared allocator instead, which is bitmap-tracked
         * and durable across reopen. TODO(WP-M5/v3-mkfs): WP-M2 records that
         * a v3 mkfs should leave a base-page region free; until then the
         * reserved pair stays unused. */
        v->mb_boot_cursor = v->mb_boot_end;
        v->v3_mbuf_ready = 1;
    }
    return 0;
}

/* Current base root as a verified blkptr (pba/gen/checksum). pba == 0 means
 * the tree is empty. */
static int v3_base_root(invfs_volume *v, invfs_blkptr *out)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    uint64_t pba = 0, gen = 0;
    int rc;

    if (!out)
        return -1;
    memset(out, 0, sizeof *out);
    rc = mbuf_root_read(v, &pba, &gen);
    if (rc < 0)
        return -1;
    if (rc == 1)
        return 0;   /* no live root slot: empty tree */
    if (mbuf_read(v, pba, page) != 0)
        return -1;
    mbuf_ptr_set(out, pba, page, INVFS_BP_ROOT);
    /* Test seam: the root is captured and nothing of it has been read yet.
     * This is the boundary a reclaim reader epoch must span -- a fold that
     * publishes, drains and frees between the capture above and the walk
     * below takes the pages out from under the caller. See the KNOWN GAP in
     * this file's header. */
    invfs_test_base_read_hook(out);
    return 0;
}

/* Persist the dirty bitmap range. Base pages come from the shared allocator
 * (WP-M2 falls back to the shadow pool because a v3 mkfs still marks the
 * whole metadata zone allocated); their bits must land before RT30 names a
 * page, or a reopen could hand the same block out again. Mirrors the v2
 * partial-bitmap write in vol_flush. */
int vol_v3_bitmap_flush(invfs_volume *v)
{
    uint64_t bm_bytes = (uint64_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
    uint64_t base = v->sb.metadata_zone_start * INVFS_BLOCK_SIZE;
    uint64_t lo, hi;

    if (v->bm_lo > v->bm_hi)
        return 0;   /* clean */
    lo = v->bm_lo;
    hi = v->bm_hi;
    if (hi > bm_bytes)
        hi = bm_bytes;
    /* round out to whole blocks: the unbuffered path must not partially
     * read-modify-write a bitmap block */
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

/* Make the new tree durable, then publish its root. `old_gen` is the gen of
 * the pre-mutation root (0 for an empty tree). An empty result (delete of
 * the last inode) is represented by a fresh empty leaf so RT30 never has to
 * name pba 0. TODO(WP-M4/M5): the M4 fsck walker must accept an empty leaf
 * as the empty-tree root (btree_check currently rejects it); alternatively
 * M6+ can store the empty base as a null root slot. */
static int v3_publish(invfs_volume *v, invfs_blkptr root, uint64_t old_gen)
{
    if (root.pba == 0) {
        uint8_t page[INVFS_BLOCK_SIZE];
        uint64_t gen = old_gen + 1, pba;

        pba = mbuf_alloc(v, gen);
        /* WP126: tell the orphan collector this is a base page, so it
         * never has to look for it in the block space. */
        btree_orphan_note_alloc(v, pba);
        if (!pba)
            return -1;
        mbuf_page_init(page, INVFS_PAGE_LEVEL_LEAF, gen);
        if (mbuf_write(v, pba, page) != 0) {
            mbuf_free(v, pba);
            return -1;
        }
        mbuf_ptr_set(&root, pba, page, INVFS_BP_LEAF | INVFS_BP_ROOT);
    }
    /* structure-before-reference (WP-M3): the COW pages + the allocation
     * bitmap are durable before RT30 points at the new root. */
    if (vol_v3_bitmap_flush(v) != 0)
        return -1;
    if (vmux_barrier(v, "v3 inode pages") < 0)
        return -1;
    return mbuf_root_publish(v, root.pba, root.gen);
}

int vol_v3_base_root(invfs_volume *v, invfs_blkptr *out)
{
    if (v3_ready(v) != 0)
        return -1;
    return v3_base_root(v, out);
}

/* The reclaim reader epoch, applied to a base-tree POINT read: announce,
 * capture, search, release -- one exit, and nothing in between can return.
 *
 * This is the hot path (it is where the ~1e-6 of collected-generation reads
 * was measured), and the shape is deliberate. Every early return a hand-
 * instrumented capture-then-walk has is a place a release can be forgotten,
 * and a forgotten release is not a leak: g_readers_in_flight is process-
 * global and vol_reclaim_drain spins on it under g_io_lock, so ONE missed
 * release wedges every later fold on the mount. A single-exit helper makes
 * that a property of the code's shape rather than of a reviewer's care.
 *
 * The order inside is the part that is easy to get backwards. The announce
 * precedes v3_base_root because the alternative leaves a window: mbuf_root_read
 * returns a root, and a fold that publishes, drains (count still 0) and frees
 * in the gap before the increment leaves the reader walking a root whose
 * generation is already gone. Announce-then-capture is load-bearing;
 * capture-then-announce is the same defect with an extra step.
 *
 * val/found are exactly btree_search's, and its value still points into the
 * per-thread buffer that the next search invalidates -- the caller copies it
 * out, as it did before. */
static int v3_base_get(invfs_volume *v, const uint8_t *key, uint16_t klen,
                       bt_val *val, int *found)
{
    invfs_blkptr root;
    int rc;

    (void)vol_reclaim_reader_snapshot();
    rc = v3_base_root(v, &root);
    if (rc == 0)
        rc = btree_search(v, root, (bt_key){key, klen}, val, found);
    vol_reclaim_reader_release();
    return rc;
}

/* ------------------------------------------------------------------ */
/* WP-M11: overlay read primitive (delta first, then base)             */
/*                                                                    */
/* Every v3 point read resolves its key through the recent tier first: */
/* the delta coalescing index returns the single latest record for the */
/* key (WP-M10), and only a miss falls through to the immutable base    */
/* B+-tree. A delete record is a value at the overlay layer -- it       */
/* shadows a base row/entry rather than falling through -- so the       */
/* caller must test INVFS_DELTA_FLAG_DELETE before the base.            */
/*                                                                    */
/* Ordering with a concurrent fold (design §3/§5): fold writes the key  */
/* into the new base BEFORE removing it from the delta. Consulting the  */
/* delta first is therefore correct under any interleaving and needs no  */
/* read lock: if the delta no longer owns the key the new base already  */
/* has it; if it still does, the delta copy is the same (or newer)      */
/* value. This helper freezes that delta-before-base order -- callers   */
/* must not read the base first.                                        */
/*                                                                    */
/* WHAT THAT DOES NOT SAY (WP-inode-get-fold-race). "Needs no read    */
/* lock" is true of the ORDER and of the INDEX -- vol_delta_lookup      */
/* takes g_delta_lock to read the index at all, and the fold takes the */
/* same lock to replace it. It was read as covering the BYTES, and that */
/* is where the read path broke: the ref this helper hands back names a */
/* block range, the fold frees that range the moment it drops the index */
/* (vol_v3_fold step 3), and the caller read it afterwards with bare    */
/* preads. vol_delta_read_value now holds the same lock across the read, */
/* so a ref is a promise for exactly as long as the caller's next call */
/* -- do not cache one, and do not read one without vol_delta_read_    */
/* value.                                                              */
/* ------------------------------------------------------------------ */

/* 1 = delta wins (*ref filled; a DELETE flag means "shadowed/absent"),
 * 0 = delta miss (the caller must consult the base), -1 = error. */
static int v3_overlay_lookup(invfs_volume *v, const uint8_t *key, uint16_t klen,
                             delta_ref *ref)
{
    return vol_delta_lookup(v, key, klen, ref);
}

/* ------------------------------------------------------------------ */
/* WP-M12: delta-backed mutations (the write half of the overlay)       */
/*                                                                     */
/* Every v3 namespace mutation appends a record to the recent tier      */
/* instead of COW-upserting the base B+-tree: the base stays immutable  */
/* between folds (design §4/§13), so a create/unlink/rename/chmod/xattr */
/* is O(1) append + index, never a base-root publish. The base-only     */
/* helpers (vol_v3_inode_put/... above) are kept unchanged for the fold */
/* (WP-M14) and the WP-M11 overlay driver; the *_delta_* entry points   */
/* below are what the production namespace/attr paths call. The record  */
/* shapes are exactly the ones the WP-M11 overlay already interprets:   */
/* inode row = the frozen invfs_v3_inode_row, dirent = u64 BE child,    */
/* xattr = the raw value at the WP-M7 key (chunked as in the base).     */
/*                                                                     */
/* Ordering (load-bearing): appends are made in the durability order the */
/* WP-M9 chain fixes -- data/recipe -> inode row -> dirent -- and rename */
/* keeps add-before-remove. A delete record is a value at the overlay    */
/* layer (INVFS_DELTA_FLAG_DELETE) that shadows the base entry.          */
/* ------------------------------------------------------------------ */

static int v3_delta_put(invfs_volume *v, const uint8_t *key, uint16_t klen,
                        const uint8_t *val, uint16_t vlen)
{
    return vol_delta_append(v, key, klen, val, vlen, 0);
}

static int v3_delta_del(invfs_volume *v, const uint8_t *key, uint16_t klen)
{
    return vol_delta_append(v, key, klen, NULL, 0, INVFS_DELTA_FLAG_DELETE);
}

/* Overlay existence: 1 = present (delta value or base entry), 0 = absent
 * (delta miss + base miss, or a delta delete shadowing the base), -1 = error. */
static int v3_overlay_exists(invfs_volume *v, const uint8_t *key, uint16_t klen)
{
    delta_ref dr;
    int drc = v3_overlay_lookup(v, key, klen, &dr);
    if (drc < 0)
        return -1;
    if (drc == 1)
        return (dr.flags & INVFS_DELTA_FLAG_DELETE) ? 0 : 1;
    {
        bt_val val;
        int found = 0;
        if (v3_base_get(v, key, klen, &val, &found) != 0)
            return -1;
        return found;
    }
}

/* Overlay point value: 1 = present (value copied to buf, *vlen_out set),
 * 0 = absent (delta delete or neither tier), -1 = error/too small. */
static int v3_overlay_get_key(invfs_volume *v, const uint8_t *key, uint16_t klen,
                              uint8_t *buf, size_t cap, uint16_t *vlen_out)
{
    /* One critical section for the resolve AND the read: a delta_ref names a
     * block range the fold is free to recycle the moment the lock drops
     * (WP-inode-get-fold-race). -2 (value too big for cap) keeps the old
     * meaning: -1 to this function. */
    uint16_t dflags = 0;
    int drc = vol_delta_lookup_value(v, key, klen, buf, cap, &dflags, vlen_out);
    if (drc < 0)
        return -1;
    if (drc == 1) {
        if (dflags & INVFS_DELTA_FLAG_DELETE)
            return 0;
        return 1;
    }
    {
        bt_val val;
        int found = 0;
        if (v3_base_get(v, key, klen, &val, &found) != 0)
            return -1;
        if (!found)
            return 0;
        if (val.n > cap)
            return -1;
        if (val.n)
            memcpy(buf, val.p, val.n);
        if (vlen_out)
            *vlen_out = val.n;
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* WP-M7: v3 xattr tree (base B+-tree namespace)                       */
/*                                                                    */
/* Key (frozen by the WP-M7 doc):                                      */
/*   0x03 || inode_id:u64 BE || name_len:u16 BE || name               */
/* Value: the raw xattr value bytes (the name is in the key). One      */
/* inode's xattrs are therefore a contiguous key range (ordered by      */
/* name_len then name, like WP-M6's dirent keys), so listxattr is a     */
/* single btree_scan and get/remove are point ops. The list adapter     */
/* re-sorts by name because listxattr consumers expect it.             */
/*                                                                    */
/* A base page is 4 KiB, so one leaf record holds only a few KB. For   */
/* values that do not fit, chunk 0 stays at the frozen canonical key   */
/* and continuation chunks reuse the same key with a 0x00 marker and a */
/* u16 BE chunk index appended:                                        */
/*   ... || name || 0x00 || chunk:u16 BE                               */
/* xattr names cannot contain NUL, so the two key shapes are           */
/* unambiguous and each name's keys stay contiguous (the frozen        */
/* canonical key is unchanged). This extends the WP doc, whose         */
/* "large/streamed xattr values" are out of scope but whose e2e gate   */
/* requires a value larger than one page -- TODO(wp-M7): fold this     */
/* into the delta key encoding when WP-M12 lands.                      */
/*                                                                    */
/* Mutations are COW and publish once through the WP-M2 double slot.   */
/* unlink/rmdir at nlink 0 reaches the cascade through                  */
/* vol_v3_inode_delete, which drops the whole xattr range.             */
/*                                                                    */
/* TODO(WP-M12): xattr get/scan still read the base only. WP-M11's      */
/* overlay covers inode rows, dirents and recipes (lookup/getattr/read) */
/* as the WP-M11 doc scopes it; xattr delta records do not exist yet,   */
/* so the overlay for the 0x03 namespace is deferred to the WP that      */
/* starts writing them.                                                 */
/* ------------------------------------------------------------------ */

#define V3_XATTR_FIXED       11u   /* 0x03 + u64 BE + u16 BE */
#define V3_XATTR_CHUNK_EXTRA  3u   /* 0x00 marker + u16 BE chunk index */
/* Continuation records are capped well below a page so WP-M3's splitter can
 * always rebalance a leaf that holds several of them (a record near a full
 * page cannot be partitioned between two siblings). 64 KiB / 1 KiB = 64
 * chunks worst case for one xattr. */
#define V3_XATTR_CHUNK_DATA  1024u
#define V3_XATTR_MAX_CHUNKS  130u
#define V3_XATTR_MAX_TOTAL   (64u * 1024u)

static uint16_t v3_xattr_key(uint8_t *kb, uint64_t ino,
                             const char *name, size_t nlen)
{
    int i;
    kb[0] = (uint8_t)INVFS_V3_XATTR_KEY_PREFIX;
    for (i = 0; i < 8; i++)
        kb[1 + i] = (uint8_t)(ino >> (56 - 8 * i));
    kb[9]  = (uint8_t)(nlen >> 8);
    kb[10] = (uint8_t)(nlen & 0xFF);
    if (nlen)
        memcpy(kb + V3_XATTR_FIXED, name, nlen);
    return (uint16_t)(V3_XATTR_FIXED + nlen);
}

static uint16_t v3_xattr_chunk_key(uint8_t *kb, uint64_t ino,
                                   const char *name, size_t nlen, uint16_t idx)
{
    uint16_t n = v3_xattr_key(kb, ino, name, nlen);
    kb[n]     = 0x00;
    kb[n + 1] = (uint8_t)(idx >> 8);
    kb[n + 2] = (uint8_t)(idx & 0xFF);
    return (uint16_t)(n + V3_XATTR_CHUNK_EXTRA);
}

/* Validate one key in the 0x03 namespace. 1 = canonical (chunk 0) or
 * continuation chunk, 0 = not an xattr key. */
static int v3_xattr_key_decode(const uint8_t *p, uint16_t n,
                               const uint8_t **name_out, uint16_t *nlen_out,
                               int *chunk_out, uint16_t *idx_out)
{
    uint16_t nl, base;
    if (n < V3_XATTR_FIXED || p[0] != (uint8_t)INVFS_V3_XATTR_KEY_PREFIX)
        return 0;
    nl = (uint16_t)(((uint16_t)p[9] << 8) | p[10]);
    if (nl == 0 || nl > INVFS_MAX_NAME)
        return 0;
    base = (uint16_t)(V3_XATTR_FIXED + nl);
    if (n == base) {
        if (chunk_out) *chunk_out = 0;
        if (idx_out)   *idx_out = 0;
    } else if (n == base + V3_XATTR_CHUNK_EXTRA && p[base] == 0x00) {
        if (chunk_out) *chunk_out = 1;
        if (idx_out)
            *idx_out = (uint16_t)(((uint16_t)p[base + 1] << 8) | p[base + 2]);
    } else {
        return 0;
    }
    if (name_out) *name_out = p + V3_XATTR_FIXED;
    if (nlen_out) *nlen_out = nl;
    return 1;
}

/* Largest value an inline / continuation record can hold for this name. */
static uint16_t v3_xattr_inline_cap(uint16_t nlen)
{
    uint32_t used = (uint32_t)sizeof(invfs_page_hdr) + 4u
                    + (V3_XATTR_FIXED + (uint32_t)nlen);
    return (uint16_t)(INVFS_BLOCK_SIZE - used);
}
static uint16_t v3_xattr_chunk_cap(uint16_t nlen)
{
    return (uint16_t)(v3_xattr_inline_cap(nlen) - V3_XATTR_CHUNK_EXTRA);
}

/* A chained mutation: COW upserts/deletes update `root` without publishing
 * until commit. On failure the disk root is untouched (pages leak, as in
 * WP-M5/M6 -- reclaim is WP-M15). */
typedef struct {
    invfs_volume *v;
    invfs_blkptr  root;
    uint64_t      old_gen;
    int           changed;
} v3_xmut;

static int v3_xmut_upsert(v3_xmut *m, bt_key k, bt_val val)
{
    invfs_blkptr nr;
    if (btree_upsert(m->v, m->root, k, val, &nr) != 0)
        return -1;
    m->root = nr;
    m->changed = 1;
    return 0;
}

static int v3_xmut_delete(v3_xmut *m, bt_key k, int *found)
{
    invfs_blkptr nr;
    bt_val val;
    int f = 0;
    if (btree_search(m->v, m->root, k, &val, &f) != 0)
        return -1;
    if (found) *found = f;
    if (!f)
        return 0;
    if (btree_delete(m->v, m->root, k, &nr) != 0)
        return -1;
    m->root = nr;
    m->changed = 1;
    return 0;
}

static int v3_xmut_commit(v3_xmut *m)
{
    if (!m->changed)
        return 0;
    return v3_publish(m->v, m->root, m->old_gen);
}

/* Delete the canonical key and every continuation chunk for one name. */
static int v3_xattr_delete_name(v3_xmut *m, uint64_t ino,
                                const char *name, size_t nl, int *removed)
{
    uint8_t kb[V3_XATTR_FIXED + INVFS_MAX_NAME + V3_XATTR_CHUNK_EXTRA];
    uint16_t kn, idx;
    int found = 0, any = 0;

    kn = v3_xattr_key(kb, ino, name, nl);
    if (v3_xmut_delete(m, (bt_key){kb, kn}, &found) != 0)
        return -1;
    if (found)
        any = 1;
    for (idx = 1; idx <= V3_XATTR_MAX_CHUNKS; idx++) {
        kn = v3_xattr_chunk_key(kb, ino, name, nl, idx);
        if (v3_xmut_delete(m, (bt_key){kb, kn}, &found) != 0)
            return -1;
        if (!found)
            break;
        any = 1;
    }
    if (removed) *removed = any;
    return 0;
}

/* WP-M12: append the new representation BEFORE shadowing any longer old
 * tail (add-before-remove), so a crash mid-replace never loses the value.
 * The key/value shapes mirror the base chunking exactly, so a later fold is
 * a per-key upsert (WP-M14) and the read overlay walks both tiers with one
 * rule. Returns 0 ok, -1 error, -2 ERANGE (too large). */
int vol_v3_xattr_delta_set(invfs_volume *v, uint64_t inode_id,
                           const char *name, const void *val, size_t vlen)
{
    uint8_t kb[V3_XATTR_FIXED + INVFS_MAX_NAME + V3_XATTR_CHUNK_EXTRA];
    size_t nl, off = 0;
    uint16_t cap, kn, idx = 0;

    if (!v || !name)
        return -1;
    if (v->sb.vol_flags & VOLF_READONLY)
        return -1;
    nl = strlen(name);
    if (nl == 0 || nl > INVFS_MAX_NAME)
        return -1;
    if (vlen > V3_XATTR_MAX_TOTAL)
        return -2;
    if (vlen && !val)
        return -1;
    if (v3_ready(v) != 0)
        return -1;

    /* Test-only seam (src/core/vol_fault.h), armed by INVFS_FAULT. This is the
     * WRITE-side twin of "v3_xattr_row_read", and it exists because a failed
     * delta append is otherwise unreachable from a test -- you cannot exhaust
     * the volume on purpose. It returns the same -1 every other failure here
     * returns, so a caller that handles a failed xattr write handles this one
     * identically; a caller that does NOT is the defect
     * (src/cli/chmod_acl_write_test.c). */
    if (invfs_vol_fault("v3_xattr_row_write"))
        return -1;

    cap = v3_xattr_chunk_cap((uint16_t)nl);
    if (cap > V3_XATTR_CHUNK_DATA)
        cap = (uint16_t)V3_XATTR_CHUNK_DATA;
    if (cap == 0)
        return -1;

    if (vlen <= cap) {
        /* one (sub-page) record: the frozen canonical key, raw bytes; an
         * empty value is a present record with vlen 0, not a delete. */
        kn = v3_xattr_key(kb, inode_id, name, nl);
        if (v3_delta_put(v, kb, kn, val ? (const uint8_t *)val : NULL,
                         (uint16_t)vlen) != 0)
            return -1;
        idx = 1;
    } else {
        do {
            size_t chunk = vlen - off;
            if (chunk > cap)
                chunk = cap;
            if (idx == 0)
                kn = v3_xattr_key(kb, inode_id, name, nl);
            else
                kn = v3_xattr_chunk_key(kb, inode_id, name, nl, idx);
            if (v3_delta_put(v, kb, kn, (const uint8_t *)val + off,
                             (uint16_t)chunk) != 0)
                return -1;
            off += chunk;
            idx++;
        } while (off < vlen && idx <= V3_XATTR_MAX_CHUNKS);
        if (off < vlen)
            return -2;   /* more chunks than the format allows */
    }

    /* shadow any continuation key past the new representation's last chunk:
     * the canonical/new chunks already win at their own keys, so only a
     * longer OLD value leaves a stale tail to delete. */
    while (idx <= V3_XATTR_MAX_CHUNKS) {
        int ex;
        kn = v3_xattr_chunk_key(kb, inode_id, name, nl, idx);
        ex = v3_overlay_exists(v, kb, kn);
        if (ex < 0)
            return -1;
        if (!ex)
            break;
        if (v3_delta_del(v, kb, kn) != 0)
            return -1;
        idx++;
    }
    return 0;
}

int vol_v3_xattr_delta_del(invfs_volume *v, uint64_t inode_id, const char *name)
{
    uint8_t kb[V3_XATTR_FIXED + INVFS_MAX_NAME + V3_XATTR_CHUNK_EXTRA];
    size_t nl;
    uint16_t kn, idx;
    int ex;

    /* The DELETE-side twin of the ENODATA/EIO conflation fixed on
     * vol_v3_xattr_get, and the same conflation this function had before that
     * fix: -1 meant BOTH "this inode has no such xattr" and "the existence
     * probe could not be completed", so an unreadable base page was reported
     * to the application as "no such attribute".
     *
     * Same reasoning, same convention: an unreadable row is -EIO and a clean
     * miss is -ENODATA, and they must not share a value. The probe fails the
     * same way the read does -- bt_read makes btree_search return -1 and
     * v3_overlay_get_key passes it through -- so this is the same I/O error
     * the sibling now reports correctly. */
    if (!v || !name)
        return -EINVAL;
    if (v->sb.vol_flags & VOLF_READONLY)
        return -EROFS;
    nl = strlen(name);
    if (nl == 0 || nl > INVFS_MAX_NAME)
        return -EINVAL;
    if (v3_ready(v) != 0)
        return -EIO;

    /* Test-only seam (src/core/vol_fault.h), armed by INVFS_FAULT: the
     * DELETE-side twin of "v3_xattr_row_write" above, for the same reason and
     * with the same contract. */
    if (invfs_vol_fault("v3_xattr_row_unlink"))
        return -1;

    kn = v3_xattr_key(kb, inode_id, name, nl);
    ex = v3_overlay_exists(v, kb, kn);
    if (ex < 0)
        return -EIO;
    if (ex == 0)
        return -ENODATA;                 /* a clean miss, not damage */
    if (v3_delta_del(v, kb, kn) != 0)
        return -1;
    for (idx = 1; idx <= V3_XATTR_MAX_CHUNKS; idx++) {
        kn = v3_xattr_chunk_key(kb, inode_id, name, nl, idx);
        ex = v3_overlay_exists(v, kb, kn);
        if (ex < 0)
            return -1;
        if (!ex)
            break;
        if (v3_delta_del(v, kb, kn) != 0)
            return -1;
    }
    return 0;
}

int vol_v3_xattr_set(invfs_volume *v, uint64_t inode_id, const char *name,
                     const void *val, size_t vlen)
{
    uint8_t kb[V3_XATTR_FIXED + INVFS_MAX_NAME + V3_XATTR_CHUNK_EXTRA];
    invfs_blkptr root;
    v3_xmut m;
    size_t nl, off = 0;
    uint16_t cap, kn, idx = 0;

    if (!v || !name)
        return -1;
    if (v->sb.vol_flags & VOLF_READONLY)
        return -1;
    nl = strlen(name);
    if (nl == 0 || nl > INVFS_MAX_NAME)
        return -1;
    if (vlen > V3_XATTR_MAX_TOTAL)
        return -2;
    if (vlen && !val)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    if (v3_base_root(v, &root) != 0)
        return -1;
    m.v = v; m.root = root; m.old_gen = root.gen; m.changed = 0;

    /* replace: drop any previous inline/chunked representation */
    if (v3_xattr_delete_name(&m, inode_id, name, nl, NULL) != 0)
        return -1;

    cap = v3_xattr_chunk_cap((uint16_t)nl);
    if (cap > V3_XATTR_CHUNK_DATA)
        cap = (uint16_t)V3_XATTR_CHUNK_DATA;
    if (cap == 0)
        return -1;
    if (vlen <= cap) {
        /* fits one (sub-page) record: the frozen key/value shape, raw bytes */
        kn = v3_xattr_key(kb, inode_id, name, nl);
        if (v3_xmut_upsert(&m, (bt_key){kb, kn},
                           (bt_val){val ? (const uint8_t *)val : NULL,
                                    (uint16_t)vlen}) != 0)
            return -1;
        return v3_xmut_commit(&m);
    }
    do {
        size_t chunk = vlen - off;
        if (chunk > cap)
            chunk = cap;
        if (idx == 0)
            kn = v3_xattr_key(kb, inode_id, name, nl);
        else
            kn = v3_xattr_chunk_key(kb, inode_id, name, nl, idx);
        if (v3_xmut_upsert(&m, (bt_key){kb, kn},
                           (bt_val){val ? (const uint8_t *)val + off : NULL,
                                    (uint16_t)chunk}) != 0)
            return -1;
        off += chunk;
        idx++;
    } while (off < vlen && idx <= V3_XATTR_MAX_CHUNKS);
    if (off < vlen)
        return -2;   /* would need more chunks than the format allows */
    return v3_xmut_commit(&m);
}

int vol_v3_xattr_del(invfs_volume *v, uint64_t inode_id, const char *name)
{
    invfs_blkptr root;
    v3_xmut m;
    size_t nl;
    int removed = 0;

    if (!v || !name)
        return -1;
    if (v->sb.vol_flags & VOLF_READONLY)
        return -1;
    nl = strlen(name);
    if (nl == 0 || nl > INVFS_MAX_NAME)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    if (v3_base_root(v, &root) != 0)
        return -1;
    m.v = v; m.root = root; m.old_gen = root.gen; m.changed = 0;
    if (v3_xattr_delete_name(&m, inode_id, name, nl, &removed) != 0)
        return -1;
    if (!removed)
        return -1;                       /* ENODATA */
    return v3_xmut_commit(&m);
}

/* Test-only door (src/core/vol_fault.h): reach the arming state of THIS
 * translation unit, which a test in another one cannot do by hand. See the
 * comment on the declaration for why unsetenv+setenv is not enough. Defined
 * here because this is the file that owns the xattr row-read site. */
void invfs_vol_btree_fault_reload(void)
{
    invfs_vol_fault_reload();
}

/* Read one xattr's chunks and concatenate them.
 *
 * THE RETURN CONTRACT IS FOUR DISTINCT ANSWERS, and that is the whole point
 * of this comment. This function used to return -1 for both "this inode has
 * no such xattr" and "the row could not be read", and every caller read both
 * as "no such xattr" -- which turned a quarantined base page into a file
 * that appeared to carry no POSIX ACL, and the mount into one that had
 * stopped enforcing it (see the FUSE side). The two are now separate values:
 *
 *   0          found. *vlen is the value's length (and, on entry, the size of
 *              the caller's buffer; on a size query pass 0).
 *   -ENODATA   the canonical key is not there. The ONLY place this can come
 *              from is a clean miss at idx == 0.
 *   -EIO       the row could not be read (an unreadable/quarantined base
 *              page, a delta record that will not parse), the value is over
 *              V3_XATTR_MAX_TOTAL, or the accumulator could not be grown.
 *              Never a statement about whether the xattr exists.
 *   -EINVAL    the arguments are not usable (NULL, or a name outside
 *              1..INVFS_MAX_NAME). A caller bug, not a volume condition.
 *   -ERANGE    the caller's buffer is smaller than the value.
 */
int vol_v3_xattr_get(invfs_volume *v, uint64_t inode_id, const char *name,
                     void *val, size_t *vlen)
{
    uint8_t kb[V3_XATTR_FIXED + INVFS_MAX_NAME + V3_XATTR_CHUNK_EXTRA];
    uint8_t one[V3_XATTR_CHUNK_DATA + 1];
    uint8_t *acc = NULL;
    size_t nl, total = 0, cap = 0;
    uint16_t kn, idx;

    if (!v || !name || !vlen)
        return -EINVAL;
    nl = strlen(name);
    if (nl == 0 || nl > INVFS_MAX_NAME)
        return -EINVAL;
    if (v3_ready(v) != 0)
        return -EIO;

    /* WP-M12: walk the canonical + continuation keys through the overlay
     * (delta first, then base) so a value written since the last fold is
     * visible. A delta delete at any key ends the walk; a delta miss falls
     * through to the base chunk. Each per-key value is <= V3_XATTR_CHUNK_DATA
     * (the WP-M7 writer caps every chunk), so `one` is always large enough. */
    for (idx = 0; ; idx++) {
        uint16_t got = 0;
        size_t add;
        int rc;

        if (idx == 0)
            kn = v3_xattr_key(kb, inode_id, name, nl);
        else
            kn = v3_xattr_chunk_key(kb, inode_id, name, nl, idx);
        /* Test-only seam (src/core/vol_fault.h), armed by INVFS_FAULT. It
         * stands in for the ROW READ failing, which in production is a
         * quarantined or otherwise unreadable base page: bt_read makes
         * btree_search return -1 (vol_btree.c:522) and v3_overlay_get_key
         * passes that -1 through (:2893). The site injects the same -1, so
         * nothing downstream -- including the code below that has to tell a
         * read error from a missing key -- can tell it from the real thing. */
        if (invfs_vol_fault("v3_xattr_row_read"))
            rc = -1;
        else
            rc = v3_overlay_get_key(v, kb, kn, one, sizeof one, &got);
        if (rc < 0) {                     /* the row could not be READ */
            free(acc);
            return -EIO;
        }
        if (rc == 0) {
            if (idx == 0) {               /* absent, and that is all it says */
                free(acc);
                return -ENODATA;
            }
            break;                       /* end of the chunk chain */
        }
        add = got;
        if (total + add > V3_XATTR_MAX_TOTAL) {
            free(acc);
            return -EIO;
        }
        if (total + add > cap) {
            size_t ncap = cap ? cap * 2 : 1024;
            uint8_t *na;
            while (ncap < total + add)
                ncap *= 2;
            na = (uint8_t *)realloc(acc, ncap);
            if (!na) { free(acc); return -EIO; }
            acc = na;
            cap = ncap;
        }
        if (add)
            memcpy(acc + total, one, add);
        total += add;

        if (idx >= V3_XATTR_MAX_CHUNKS)
            break;                       /* chain length cap (defensive) */
    }
    if (*vlen == 0) {                    /* size query */
        *vlen = total;
        free(acc);
        return 0;
    }
    if (*vlen < total) {
        free(acc);
        /* -ERANGE, not the literal -2 this used to return. 2 is ENOENT, so
         * the old value was a third thing wearing ERANGE's label: a caller
         * that passed the engine's answer through (as invf_getxattr now
         * does) would have told the application the file did not exist.
         * The FUSE layer had been compensating by mapping every non-(-1)
         * return to -ERANGE, which is exactly the flattening that hid the
         * ENODATA/EIO ambiguity in the first place. */
        return -ERANGE;
    }
    if (total)
        memcpy(val, acc, total);
    *vlen = total;
    free(acc);
    return 0;
}

/* WP-M12: xattr scan = base canonical names + delta canonical names, with a
 * delta delete suppressing the base name (and a delta value adding one).
 * Only canonical (chunk 0) keys name an xattr; the delta writer always
 * appends the canonical record first, so a chunk-only delta entry never
 * names a value on its own. Duplicates are collapsed so a name present in
 * both tiers is emitted once (delta wins). */
typedef struct {
    char name[INVFS_MAX_NAME + 1];
    int  deleted;
} v3_xa_ent;

typedef struct {
    v3_xa_ent *e;
    size_t     n, cap;
    int        oom;
} v3_xa_list;

static void v3_xa_free(v3_xa_list *l)
{
    free(l->e);
    l->e = NULL;
    l->n = l->cap = 0;
}

static v3_xa_ent *v3_xa_find(v3_xa_list *l, const char *nm)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        if (strcmp(l->e[i].name, nm) == 0)
            return &l->e[i];
    return NULL;
}

static v3_xa_ent *v3_xa_add(v3_xa_list *l, const char *nm)
{
    v3_xa_ent *e;
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        v3_xa_ent *ne = (v3_xa_ent *)realloc(l->e, ncap * sizeof *ne);
        if (!ne) {
            l->oom = 1;
            return NULL;
        }
        l->e = ne;
        l->cap = ncap;
    }
    e = &l->e[l->n++];
    snprintf(e->name, sizeof e->name, "%s", nm);
    e->deleted = 0;
    return e;
}

static int v3_xa_canon_name(const uint8_t *key, uint16_t klen,
                            char nbuf[INVFS_MAX_NAME + 1], uint16_t *nlen_out)
{
    const uint8_t *name;
    uint16_t nl;
    int chunk;

    if (!v3_xattr_key_decode(key, klen, &name, &nl, &chunk, NULL) || chunk)
        return 0;
    memcpy(nbuf, name, nl);
    nbuf[nl] = 0;
    if (nlen_out)
        *nlen_out = nl;
    return 1;
}

static int v3_xa_delta_cb(void *ctx_, const uint8_t *key, uint16_t klen,
                          const delta_ref *ref)
{
    v3_xa_list *l = (v3_xa_list *)ctx_;
    char nbuf[INVFS_MAX_NAME + 1];
    v3_xa_ent *e;

    if (!v3_xa_canon_name(key, klen, nbuf, NULL))
        return 0;
    e = v3_xa_find(l, nbuf);
    if (!e)
        e = v3_xa_add(l, nbuf);
    if (!e)
        return 1;                        /* OOM: abort the cursor */
    e->deleted = (ref->flags & INVFS_DELTA_FLAG_DELETE) ? 1 : 0;
    return 0;
}

static int v3_xa_base_cb(void *ctx_, bt_key k, bt_val val)
{
    v3_xa_list *l = (v3_xa_list *)ctx_;
    char nbuf[INVFS_MAX_NAME + 1];
    (void)val;

    if (!v3_xa_canon_name(k.p, k.n, nbuf, NULL))
        return 0;
    if (v3_xa_find(l, nbuf))
        return 0;                        /* a delta record owns the name */
    if (!v3_xa_add(l, nbuf))
        return 1;                        /* OOM: abort the scan */
    return 0;
}

static int v3_xa_ent_cmp(const void *pa, const void *pb)
{
    const v3_xa_ent *a = (const v3_xa_ent *)pa;
    const v3_xa_ent *b = (const v3_xa_ent *)pb;
    size_t an = strlen(a->name), bn = strlen(b->name);
    if (an != bn)
        return an < bn ? -1 : 1;
    return strcmp(a->name, b->name);
}

int vol_v3_xattr_scan(invfs_volume *v, uint64_t inode_id,
                      vol_v3_xattr_cb cb, void *ctx)
{
    uint8_t lo[V3_XATTR_FIXED], hi[V3_XATTR_FIXED];
    invfs_blkptr root;
    v3_xa_list l;
    size_t i;
    int rc;

    if (!v || !cb)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    memset(&l, 0, sizeof l);
    v3_xattr_key(lo, inode_id, NULL, 0);         /* name_len 0: smallest */
    v3_xattr_key(hi, inode_id + 1, NULL, 0);

    rc = vol_delta_range(v, lo, V3_XATTR_FIXED, hi, V3_XATTR_FIXED,
                         v3_xa_delta_cb, &l);
    if (rc != 0 || l.oom) {
        v3_xa_free(&l);
        return -1;
    }
    /* Reclaim reader epoch: announce BEFORE the capture, release after the
     * walk. The release is placed immediately after btree_scan, not at the
     * function's exit, because everything below it (the sort and the caller
     * callback) touches only the collected list. */
    (void)vol_reclaim_reader_snapshot();
    if (v3_base_root(v, &root) != 0) {
        vol_reclaim_reader_release();
        v3_xa_free(&l);
        return -1;
    }
    rc = btree_scan(v, root, (bt_key){lo, V3_XATTR_FIXED},
                    (bt_key){hi, V3_XATTR_FIXED}, v3_xa_base_cb, &l);
    vol_reclaim_reader_release();
    if (rc != 0 || l.oom) {
        v3_xa_free(&l);
        return -1;
    }

    /* the documented key order is name_len then name; the only in-tree
     * caller sorts anyway but the contract is kept. */
    if (l.n > 1)
        qsort(l.e, l.n, sizeof *l.e, v3_xa_ent_cmp);
    rc = 0;
    for (i = 0; i < l.n; i++) {
        if (l.e[i].deleted)
            continue;
        rc = cb(ctx, l.e[i].name, strlen(l.e[i].name));
        if (rc)
            break;
    }
    v3_xa_free(&l);
    return rc;
}

/* Collect every key in one inode's xattr range, then delete them all. Used
 * by vol_v3_inode_delete: a dying inode must not strand its xattrs. */
typedef struct {
    uint8_t *buf;
    size_t   len, cap;
} v3_keybuf;

/* Append one raw key with a u16 length prefix. 0 = ok, -1 = OOM. */
static int v3_keybuf_append(v3_keybuf *b, const uint8_t *k, uint16_t kn)
{
    if (b->len + 2 + kn > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        uint8_t *nb;
        while (ncap < b->len + 2 + kn)
            ncap *= 2;
        nb = (uint8_t *)realloc(b->buf, ncap);
        if (!nb)
            return -1;
        b->buf = nb;
        b->cap = ncap;
    }
    b->buf[b->len]     = (uint8_t)(kn >> 8);
    b->buf[b->len + 1] = (uint8_t)(kn & 0xFF);
    memcpy(b->buf + b->len + 2, k, kn);
    b->len += 2 + kn;
    return 0;
}

static int v3_xattr_collect_cb(void *ctx_, bt_key k, bt_val val)
{
    v3_keybuf *b = (v3_keybuf *)ctx_;
    const uint8_t *name;
    uint16_t nl;
    int chunk;
    (void)val;

    if (!v3_xattr_key_decode(k.p, k.n, &name, &nl, &chunk, NULL))
        return 0;
    return v3_keybuf_append(b, k.p, k.n) == 0 ? 0 : -1;
}

/* vol_delta_range callback for the same collection (WP-M12): the delta side
 * of an inode's xattr keys, including keys already shadowed by a delete. */
static int v3_xattr_collect_delta_cb(void *ctx_, const uint8_t *key,
                                     uint16_t klen, const delta_ref *ref)
{
    v3_keybuf *b = (v3_keybuf *)ctx_;
    (void)ref;
    return v3_keybuf_append(b, key, klen) == 0 ? 0 : 1;
}

/* WP-M12: append a delete for EVERY xattr key of `inode_id` in both tiers,
 * so a dying inode leaves no stranded key for a later fold (WP-M14). The
 * base keys are scanned first, then the delta's; a key present in both is
 * coalesced by the append. Returns 0 ok, -1 error. */
static int v3_delta_shadow_xattr_keys(invfs_volume *v, uint64_t inode_id)
{
    uint8_t lo[V3_XATTR_FIXED], hi[V3_XATTR_FIXED];
    invfs_blkptr root;
    v3_keybuf b;
    size_t off = 0;
    int rc;

    b.buf = NULL;
    b.len = 0;
    b.cap = 0;
    v3_xattr_key(lo, inode_id, NULL, 0);
    v3_xattr_key(hi, inode_id + 1, NULL, 0);

    /* The base SCAN is a read of a freshly captured root, so it is inside
     * the reclaim reader epoch. The delta appends below are not: the section
     * is released before the first vol_delta_* call, so nothing in this
     * function ever holds the announce across g_delta_lock. */
    (void)vol_reclaim_reader_snapshot();
    if (v3_base_root(v, &root) != 0) {
        vol_reclaim_reader_release();
        free(b.buf);
        return -1;
    }
    rc = btree_scan(v, root, (bt_key){lo, V3_XATTR_FIXED},
                    (bt_key){hi, V3_XATTR_FIXED}, v3_xattr_collect_cb, &b);
    vol_reclaim_reader_release();
    if (rc != 0) {
        free(b.buf);
        return -1;
    }
    rc = vol_delta_range(v, lo, V3_XATTR_FIXED, hi, V3_XATTR_FIXED,
                         v3_xattr_collect_delta_cb, &b);
    if (rc != 0) {
        free(b.buf);
        return -1;
    }
    while (off + 2 <= b.len) {
        uint16_t kl = (uint16_t)(((uint16_t)b.buf[off] << 8) | b.buf[off + 1]);
        off += 2;
        if ((size_t)off + kl > b.len) {
            free(b.buf);
            return -1;
        }
        if (v3_delta_del(v, b.buf + off, kl) != 0) {
            free(b.buf);
            return -1;
        }
        off += kl;
    }
    free(b.buf);
    return 0;
}

static int v3_xattr_remove_all(v3_xmut *m, uint64_t inode_id)
{
    uint8_t lo[V3_XATTR_FIXED], hi[V3_XATTR_FIXED];
    v3_keybuf b;
    size_t off = 0;
    int rc;

    b.buf = NULL;
    b.len = 0;
    b.cap = 0;
    v3_xattr_key(lo, inode_id, NULL, 0);
    v3_xattr_key(hi, inode_id + 1, NULL, 0);
    rc = btree_scan(m->v, m->root, (bt_key){lo, V3_XATTR_FIXED},
                    (bt_key){hi, V3_XATTR_FIXED}, v3_xattr_collect_cb, &b);
    if (rc != 0) {
        free(b.buf);
        return -1;
    }
    while (off + 2 <= b.len) {
        uint16_t kl = (uint16_t)(((uint16_t)b.buf[off] << 8) | b.buf[off + 1]);
        off += 2;
        if ((size_t)off + kl > b.len) {
            free(b.buf);
            return -1;
        }
        if (v3_xmut_delete(m, (bt_key){b.buf + off, kl}, NULL) != 0) {
            free(b.buf);
            return -1;
        }
        off += kl;
    }
    free(b.buf);
    return 0;
}

int vol_v3_inode_get(invfs_volume *v, uint64_t inode_id, invfs_v3_inode *out)
{
    uint8_t kb[8];
    bt_val val;
    int found = 0;

    if (!v || !out)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    /* Test-only seam (src/core/vol_fault.h), armed by INVFS_FAULT: the
     * v3_inode_row_read twin of the xattr row-read site at :3352. It stands
     * in for the ROW READ failing, which in production is a quarantined or
     * otherwise unreadable base page, or a torn delta chain -- both answer
     * -1 below. The site injects the same -1, so nothing downstream,
     * including the code that has to tell a read error from a missing row,
     * can tell it from the real thing.
     *
     * This is the read pba_ref_ensure's build depends on being able to make,
     * because the map it builds is the sole gate on every data-block free: a
     * row that cannot be read is a live reference the map does not hold.
     * src/cli/pbaref_v3_test.c (leg "skiprow") fails one of them there. */
    if (invfs_vol_fault("v3_inode_row_read"))
        return -1;
    v3_ino_key(inode_id, kb);

    /* Test-only seam (src/core/vol_fault.h), armed by INVFS_FAULT. It stands
     * in for the inode ROW READ failing, which in production is a
     * quarantined or otherwise unreadable base page: bt_read makes
     * btree_search return -1 (vol_btree.c:522) and v3_base_get passes that
     * through, exactly as it passes through below at the decode. The site
     * injects the same -1, so nothing downstream -- including
     * vol_get_meta_rc, which has to tell this from "there is no such row" --
     * can tell it from the real thing.
     *
     * Placed here, before the delta probe, so it also covers a delta-side
     * read failure: the two are the same question to every caller.
     *
     * The reload door for this file is invfs_vol_btree_fault_reload()
     * (:3288). A test in another translation unit must call it -- see the
     * declaration in vol_fault.h for why unsetenv+setenv is not a
     * substitute. */
    if (invfs_vol_fault("v3_inode_row_read"))
        return -1;

    /* WP-M11: delta first -- a delta row (or delete) shadows the base.
     * WP-inode-get-fold-race: resolve and read in ONE critical section. The
     * lookup used to return a delta_ref and the value read happened after the
     * lock was dropped, which let the fold free the chain and the next
     * segment allocation zero it: this function then decoded zeros and
     * answered -1 for an inode that was present and whose row the fold had
     * already copied into the new base. */
    {
        uint8_t rb[INVFS_V3_INODE_ROW_FIXED];
        uint16_t rlen = 0, dflags = 0;
        int drc = vol_delta_lookup_value(v, kb, sizeof kb, rb, sizeof rb,
                                         &dflags, &rlen);
        if (drc < 0)
            return -1;                   /* -2 too: see volume.h */
        if (drc == 1) {
            if (dflags & INVFS_DELTA_FLAG_DELETE)
                return 0;                /* hidden by a delta delete */
            /* The delta value is the frozen fixed row. TODO(WP-M12): if a
             * later writer inlines xattr bytes (xattr_len > 0) the value can
             * exceed INVFS_V3_INODE_ROW_FIXED; -2 above is that case, and it
             * keeps answering -1 as it always did. */
            if (v3_ino_decode(rb, rlen, out) != 0)
                return -1;
            return 1;
        }
    }

    if (v3_base_get(v, kb, 8, &val, &found) != 0)
        return -1;
    if (!found)
        return 0;
    if (v3_ino_decode(val.p, val.n, out) != 0)
        return -1;
    return 1;
}

int vol_v3_inode_put(invfs_volume *v, uint64_t inode_id,
                     const invfs_v3_inode *in)
{
    uint8_t kb[8];
    uint8_t vb[INVFS_V3_INODE_ROW_FIXED];
    invfs_blkptr root, nr;
    bt_val val;
    uint16_t vl;
    uint64_t old_gen;

    if (!v || !in || in->nlink == 0)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    if (v3_base_root(v, &root) != 0)
        return -1;
    old_gen = root.gen;

    v3_ino_key(inode_id, kb);
    vl = v3_ino_encode(in, vb);
    val.p = vb;
    val.n = vl;
    if (btree_upsert(v, root, (bt_key){kb, 8}, val, &nr) != 0)
        return -1;
    return v3_publish(v, nr, old_gen);
}

int vol_v3_inode_delete(invfs_volume *v, uint64_t inode_id)
{
    uint8_t kb[8];
    invfs_blkptr root;
    bt_val val;
    int found = 0;
    uint64_t old_gen;

    if (!v)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    if (v3_base_root(v, &root) != 0)
        return -1;
    v3_ino_key(inode_id, kb);
    if (btree_search(v, root, (bt_key){kb, 8}, &val, &found) != 0)
        return -1;
    if (!found)
        return 0;   /* absent: nothing to do */
    old_gen = root.gen;
    /* WP-M7 cascade: a dying inode must not strand its xattr keys. */
    {
        v3_xmut m;
        m.v = v;
        m.root = root;
        m.old_gen = old_gen;
        m.changed = 0;
        if (v3_xattr_remove_all(&m, inode_id) != 0)
            return -1;
        if (v3_xmut_delete(&m, (bt_key){kb, 8}, NULL) != 0)
            return -1;
        return v3_xmut_commit(&m);
    }
}

/* WP-M12: the delta-backed inode mutations the namespace paths call. The
 * base helpers above stay the fold/base path (WP-M14 + the WP-M11 driver). */
int vol_v3_inode_delta_put(invfs_volume *v, uint64_t inode_id,
                           const invfs_v3_inode *in)
{
    if (invfs_vol_fault("v3_inode_delta_put"))
        return -1;   /* test hook: pretend the delta append failed */
    uint8_t kb[8];
    uint8_t vb[INVFS_V3_INODE_ROW_FIXED];
    uint16_t vl;

    if (!v || !in || in->nlink == 0)
        return -1;                        /* a zero-nlink row is deleted */
    if (v3_ready(v) != 0)
        return -1;
    /* WP pba-ref-v3-incremental: this is the ONLY place a v3 recipe_addr is
     * published, and it used to be invisible to the pba reference map --
     * the sole gate on every block free. A map built before the publish
     * counted a recipe that no longer exists and missed the one that does,
     * so a pba a live sharer still named could read 0 and be freed. Marking
     * it stale here makes the next pba_ref_ensure rebuild from the live set;
     * the two callers that adjust the map by hand (the sweep's segment
     * remap, dedupe's remap) clear the flag with pba_ref_validate.
     *
     * Both directions count: a row that does not exist yet is as much a
     * change as one whose recipe_addr differs (an inode born after the map
     * was built is precisely the case that used to over-free), while a row
     * that does not change its recipe -- nlink, mode, times -- leaves the
     * map alone. A brand-new row with no content names nothing. */
    {
        static const uint8_t zero_addr[INVFS_V3_RECIPE_ADDR_LEN] = {0};
        invfs_v3_inode old;
        if (vol_v3_inode_get(v, inode_id, &old) != 1)
            pba_ref_invalidate(v);
        else if (memcmp(old.recipe_addr, in->recipe_addr,
                        INVFS_V3_RECIPE_ADDR_LEN) != 0)
            pba_ref_invalidate(v);
        else if (memcmp(in->recipe_addr, zero_addr,
                        INVFS_V3_RECIPE_ADDR_LEN) == 0)
            ; /* nothing to count either way */
    }
    v3_ino_key(inode_id, kb);
    vl = v3_ino_encode(in, vb);
    return v3_delta_put(v, kb, sizeof kb, vb, vl);
}

/* Delete the row and shadow its xattr keys. The xattr deletes are appended
 * BEFORE the row delete so a crash cannot expose a live row whose xattrs
 * have vanished; the reverse order would strand the keys. The inode needs
 * no base check: a delete for an absent (base or delta) row is harmless,
 * but an overlay-absent row is a no-op to match vol_v3_inode_delete. */
int vol_v3_inode_delta_delete(invfs_volume *v, uint64_t inode_id)
{
    uint8_t kb[8];

    if (!v)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    v3_ino_key(inode_id, kb);
    {
        int ex = v3_overlay_exists(v, kb, sizeof kb);
        if (ex < 0)
            return -1;
        if (ex == 0)
            return 0;                     /* absent: nothing to do */
    }
    if (v3_delta_shadow_xattr_keys(v, inode_id) != 0)
        return -1;
    return v3_delta_del(v, kb, sizeof kb);
}


/* ------------------------------------------------------------------ */
/* WP-M8: content-addressed recipe blobs (base B+-tree keyspace)       */
/*                                                                    */
/* The immutable AST recipe (header + entries, exactly the bytes a v2  */
/* record carries after its name) lives in the base tree under         */
/*     0x04 || blake3_256(blob)[32]                                    */
/* not inline in the inode row. Identical recipes map to one key, so   */
/* a rewrite of unchanged content stores only the reference. The inode */
/* row's recipe_addr is the content address; the reader recomputes the */
/* hash and refuses on mismatch (design §12/§16: a bad recipe must     */
/* never be trusted). The tree's own page CRC (mbuf_read_ptr) is the   */
/* first line of defence; BLAKE3 is the second.                        */
/*                                                                    */
/* A blob must fit one base page. WP-M8 caps it at 3800 bytes (header  */
/* + 32 B/segment => ~7.7 MiB of file at 64 KiB segments); larger      */
/* recipes need a multi-page/streamed blob, deferred to WP-M9/WP-M15   */
/* (TODO). The row never points at a leaf directly: a later COW split  */
/* can move the key, so the lookup always goes through the tree.       */
/* ------------------------------------------------------------------ */

#define V3_RECIPE_KEY_LEN      (1u + INVFS_V3_RECIPE_ADDR_LEN)   /* 33 */
/* One page = 4096; header 20 + key record (2+33) + value record (2+n)
 * must fit, with headroom so a split can always partition two records. */
#define V3_RECIPE_BLOB_MAX     INVFS_V3_RECIPE_BLOB_MAX

static void v3_recipe_key(uint8_t kb[V3_RECIPE_KEY_LEN],
                          const uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN])
{
    kb[0] = (uint8_t)INVFS_V3_RECIPE_KEY_PREFIX;
    memcpy(kb + 1, addr, INVFS_V3_RECIPE_ADDR_LEN);
}

static void v3_recipe_chunk_key(uint8_t kb[INVFS_V3_RECIPE_CHUNK_KEY_LEN],
                                const uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN],
                                uint16_t chunk_idx)
{
    kb[0] = (uint8_t)INVFS_V3_RECIPE_KEY_PREFIX;
    memcpy(kb + 1, addr, INVFS_V3_RECIPE_ADDR_LEN);
    kb[33] = 0x00;
    kb[34] = (uint8_t)(chunk_idx >> 8);
    kb[35] = (uint8_t)(chunk_idx & 0xFF);
}

static void v3_blake3(const uint8_t *buf, size_t len,
                      uint8_t out[INVFS_V3_RECIPE_ADDR_LEN])
{
    blake3_hasher hx;
    blake3_hasher_init(&hx);
    blake3_hasher_update(&hx, buf, len);
    blake3_hasher_finalize(&hx, out, INVFS_V3_RECIPE_ADDR_LEN);
}

/* Store `blob` (immutable) under its content address. On success
 * addr_out holds BLAKE3-256(blob). Identical bytes already present are a
 * no-op (dedup) and return the same address. 0 = ok, -1 = error.
 * Recipes <= V3_RECIPE_BLOB_MAX (3800 B) fit directly in one page value.
 * Larger recipes (> 3800 B, up to INVFS_V3_RECIPE_STREAM_MAX) are chunked
 * across multiple keys: descriptor at 0x04||addr, chunks at 0x04||addr||0x00||idx. */
int vol_v3_recipe_store(invfs_volume *v, const uint8_t *blob, size_t blen,
                        uint8_t addr_out[INVFS_V3_RECIPE_ADDR_LEN])
{
    if (invfs_vol_fault("v3_recipe_store"))
        return -1;   /* test hook: pretend the base-tree publish failed */
    uint8_t kb[V3_RECIPE_KEY_LEN], addr[INVFS_V3_RECIPE_ADDR_LEN];
    invfs_blkptr root, nr;
    bt_val val;
    int found = 0;

    if (!v || !blob || blen == 0 || blen > INVFS_V3_RECIPE_STREAM_MAX)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    if (v3_base_root(v, &root) != 0)
        return -1;
    v3_blake3(blob, blen, addr);
    v3_recipe_key(kb, addr);
    /* Dedup: an identical recipe is already immutable, so the reference
     * alone is the whole write. */
    if (btree_search(v, root, (bt_key){kb, V3_RECIPE_KEY_LEN}, &val,
                     &found) != 0)
        return -1;
    if (!found) {
        uint64_t old_gen = root.gen;
        if (blen <= V3_RECIPE_BLOB_MAX) {
            val.p = blob;
            val.n = (uint16_t)blen;
            if (btree_upsert(v, root, (bt_key){kb, V3_RECIPE_KEY_LEN}, val,
                             &nr) != 0)
                return -1;
            if (v3_publish(v, nr, old_gen) != 0)
                return -1;
        } else {
            uint32_t n_chunks = (uint32_t)((blen + INVFS_V3_RECIPE_CHUNK_DATA - 1) /
                                           INVFS_V3_RECIPE_CHUNK_DATA);
            if (n_chunks > 0xFFFFu)
                return -1;
            invfs_v3_recipe_desc desc;
            desc.magic = INVFS_V3_RECIPE_MAGIC_RMC1;
            desc.total_len = (uint32_t)blen;
            desc.n_chunks = (uint16_t)n_chunks;

            invfs_blkptr cur_root = root;
            for (uint32_t i = 0; i < n_chunks; i++) {
                uint8_t ckb[INVFS_V3_RECIPE_CHUNK_KEY_LEN];
                size_t off = (size_t)i * INVFS_V3_RECIPE_CHUNK_DATA;
                size_t clen = (blen - off > INVFS_V3_RECIPE_CHUNK_DATA)
                            ? INVFS_V3_RECIPE_CHUNK_DATA
                            : (blen - off);
                v3_recipe_chunk_key(ckb, addr, (uint16_t)i);
                val.p = blob + off;
                val.n = (uint16_t)clen;
                if (btree_upsert(v, cur_root,
                                 (bt_key){ckb, INVFS_V3_RECIPE_CHUNK_KEY_LEN},
                                 val, &nr) != 0)
                    return -1;
                cur_root = nr;
            }
            /* Insert multi-chunk descriptor under main key */
            val.p = (const uint8_t *)&desc;
            val.n = (uint16_t)sizeof(desc);
            if (btree_upsert(v, cur_root, (bt_key){kb, V3_RECIPE_KEY_LEN},
                             val, &nr) != 0)
                return -1;
            if (v3_publish(v, nr, old_gen) != 0)
                return -1;
        }
    }
    if (addr_out)
        memcpy(addr_out, addr, INVFS_V3_RECIPE_ADDR_LEN);
    return 0;
}

/* Fetch and VERIFY the recipe blob named by `addr`. On success *blob_out is
 * a malloc'd copy the caller frees. 0 = ok, -1 = absent, unreadable, or a
 * BLAKE3 mismatch (hard error -- never returns unverified bytes). */
/* WP-Q2R3 recipe cache. The address IS the BLAKE3 of the bytes, so a hit is
 * always correct and nothing ever needs invalidating -- the only cost is a
 * copy, which is what the caller was going to do anyway. Two slots: the hot
 * inode plus whatever a walk touches next. */
static int v3_rcache_get(invfs_volume *v, const uint8_t *addr,
                         uint8_t **blob_out, size_t *blen_out)
{
    int i, hit = -1;

    pthread_mutex_lock(&v->rc_mu);
    for (i = 0; i < 2; i++)
        if (v->rcache[i].used &&
            memcmp(v->rcache[i].addr, addr, INVFS_V3_RECIPE_ADDR_LEN) == 0) {
            hit = i;
            break;
        }
    if (hit >= 0) {
        uint8_t *copy = (uint8_t *)malloc(v->rcache[hit].len ?
                                          v->rcache[hit].len : 1);
        if (copy) {
            memcpy(copy, v->rcache[hit].blob, v->rcache[hit].len);
            *blob_out = copy;
            *blen_out = v->rcache[hit].len;
        }
    }
    pthread_mutex_unlock(&v->rc_mu);
    return hit >= 0 && *blob_out ? 0 : -1;
}

static void v3_rcache_put(invfs_volume *v, const uint8_t *addr,
                          const uint8_t *blob, size_t len)
{
    int i, victim = 0;
    uint8_t *copy;

    /* one huge recipe must not evict the pair for a trivial slot */
    if (len > (64u << 20)) return;
    copy = (uint8_t *)malloc(len ? len : 1);
    if (!copy) return;
    memcpy(copy, blob, len);
    pthread_mutex_lock(&v->rc_mu);
    for (i = 0; i < 2; i++)
        if (!v->rcache[i].used) { victim = i; break; }
    free(v->rcache[victim].blob);
    memcpy(v->rcache[victim].addr, addr, INVFS_V3_RECIPE_ADDR_LEN);
    v->rcache[victim].blob = copy;
    v->rcache[victim].len = len;
    v->rcache[victim].used = 1;
    pthread_mutex_unlock(&v->rc_mu);
}

/* Forward: the base half owns the reclaim reader epoch across the RMC1
 * chunk loop and has a single exit, so it is a separate function (see its
 * definition below). */
static int v3_recipe_base_blob(invfs_volume *v,
    const uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN], const uint8_t *kb,
    uint8_t **blob_out, size_t *blen_out);

int vol_v3_recipe_load(invfs_volume *v, const uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN],
                       uint8_t **blob_out, size_t *blen_out)
{
    uint8_t kb[V3_RECIPE_KEY_LEN], chk[INVFS_V3_RECIPE_ADDR_LEN];
    uint8_t *blob;

    if (!v || !addr || !blob_out || !blen_out)
        return -1;
    *blob_out = NULL;
    *blen_out = 0;
    if (v3_ready(v) != 0)
        return -1;
    /* WP141: one-shot injectible read failure, for the SAME reason the inode-
     * row site above exists. Every failure path of this function returns -1,
     * so the injected one is indistinguishable to a caller -- which is the
     * point: a recipe that will not load is an expected state on a damaged
     * volume (AGENTS.md 2.10), and the savepoint capture walk has to be
     * reachable in that state without scribbling a volume to get there.
     *
     * It sits BEFORE the rcache hit on purpose. A capture that already loaded
     * this address once in the same session would otherwise be served from
     * the cache and the injected failure would never be reached, so the leg
     * would measure the healthy path and go green. */
    if (invfs_vol_fault("v3_recipe_load"))
        return -1;
    if (v3_rcache_get(v, addr, blob_out, blen_out) == 0)
        return 0;
    v3_recipe_key(kb, addr);

    /* WP-M11: the delta owns the key if it was rewritten since the fold;
     * a delete shadows the base blob. The BLAKE3 check below still governs
     * whatever bytes the delta returns (content-addressing is immutable).
     * WP-inode-get-fold-race: resolve and read in ONE critical section -- a
     * ref handed out by the lookup names blocks the fold recycles the moment
     * the lock drops. A blob bigger than the stack buffer comes back as -2
     * with its length, and the second, exactly-sized attempt is atomic too
     * (a fold in between means the key is in the base, which is the fall
     * through below). */
    {
        uint8_t sbuf[V3_XATTR_CHUNK_DATA];
        uint16_t dflags = 0, dlen = 0, got = 0;
        uint8_t *dbuf = sbuf;
        size_t dcap = sizeof sbuf;
        int drc = vol_delta_lookup_value(v, kb, V3_RECIPE_KEY_LEN,
                                         dbuf, dcap, &dflags, &dlen);
        if (drc == -2) {
            dbuf = (uint8_t *)malloc(dlen ? dlen : 1);
            if (!dbuf)
                return -1;
            dcap = dlen;
            drc = vol_delta_lookup_value(v, kb, V3_RECIPE_KEY_LEN,
                                         dbuf, dcap, &dflags, &dlen);
            got = dlen;
        }
        if (drc < 0) {
            if (dbuf != sbuf)
                free(dbuf);
            return -1;
        }
        if (drc == 1) {
            if (dflags & INVFS_DELTA_FLAG_DELETE) {
                if (dbuf != sbuf)
                    free(dbuf);
                return -1;               /* hidden by a delta delete */
            }
            if (got == 0)
                got = dlen;
            blob = (dbuf == sbuf) ? (uint8_t *)malloc(dlen ? dlen : 1) : dbuf;
            if (!blob) {
                if (dbuf != sbuf)
                    free(dbuf);
                return -1;
            }
            if (blob != dbuf)
                memcpy(blob, dbuf, dlen);
            v3_blake3(blob, dlen, chk);
            if (memcmp(chk, addr, INVFS_V3_RECIPE_ADDR_LEN) != 0) {
                fprintf(stderr, "v3 recipe blob (delta): BLAKE3 mismatch "
                        "(corrupt or forged); refusing the read\n");
                free(blob);
                return -1;
            }
            *blob_out = blob;
            *blen_out = dlen;
            v3_rcache_put(v, addr, blob, dlen);
            return 0;
        }
    }

    return v3_recipe_base_blob(v, addr, kb, blob_out, blen_out);
}

/* The BASE half of vol_v3_recipe_load, split out because it is the one
 * point-read site that is not a one-liner, and for the reason the whole
 * reclaim reader epoch is shaped the way it is.
 *
 * An RMC1 recipe is a descriptor plus N chunk keys, and every chunk is its
 * own btree_search against the SAME captured root. So the epoch section has
 * to span the descriptor read AND the whole chunk loop -- releasing after
 * the first search would leave every chunk search walking a generation the
 * fold is free to collect, which is the defect in its most literal form.
 *
 * That makes the section cover a loop with four early returns and two
 * error paths, which is exactly where a hand-written announce/release pair
 * goes wrong. So it is not hand-written: this function owns the section and
 * has one exit, and the code below is otherwise the code that was there. */
static int v3_recipe_base_blob(invfs_volume *v,
    const uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN], const uint8_t *kb,
    uint8_t **blob_out, size_t *blen_out)
{
    uint8_t chk[INVFS_V3_RECIPE_ADDR_LEN];
    invfs_blkptr root;
    bt_val val;
    uint8_t *blob = NULL;
    int found = 0;
    int rc = -1;

    (void)vol_reclaim_reader_snapshot();
    if (v3_base_root(v, &root) != 0)
        goto out;
    if (btree_search(v, root, (bt_key){kb, V3_RECIPE_KEY_LEN}, &val,
                     &found) != 0)
        goto out;
    if (!found)
        goto out;

    /* WP-M25: check if this is an RMC1 multi-chunk descriptor */
    if (val.n == sizeof(invfs_v3_recipe_desc)) {
        invfs_v3_recipe_desc desc;
        memcpy(&desc, val.p, sizeof(desc));
        if (desc.magic == INVFS_V3_RECIPE_MAGIC_RMC1) {
            uint32_t total_len = desc.total_len;
            uint16_t n_chunks = desc.n_chunks;
            if (total_len == 0 || total_len > INVFS_V3_RECIPE_STREAM_MAX)
                goto out;
            blob = (uint8_t *)malloc(total_len);
            if (!blob)
                goto out;
            for (uint16_t i = 0; i < n_chunks; i++) {
                uint8_t ckb[INVFS_V3_RECIPE_CHUNK_KEY_LEN];
                bt_val cval;
                int cfound = 0;
                size_t off = (size_t)i * INVFS_V3_RECIPE_CHUNK_DATA;
                size_t exp_len = (total_len - off > INVFS_V3_RECIPE_CHUNK_DATA)
                               ? INVFS_V3_RECIPE_CHUNK_DATA
                               : (total_len - off);
                v3_recipe_chunk_key(ckb, addr, i);
                if (btree_search(v, root, (bt_key){ckb, INVFS_V3_RECIPE_CHUNK_KEY_LEN},
                                 &cval, &cfound) != 0 || !cfound || cval.n != exp_len) {
                    free(blob);
                    blob = NULL;
                    goto out;
                }
                memcpy(blob + off, cval.p, exp_len);
            }
            v3_blake3(blob, total_len, chk);
            if (memcmp(chk, addr, INVFS_V3_RECIPE_ADDR_LEN) != 0) {
                fprintf(stderr, "v3 recipe blob %p: BLAKE3 mismatch (corrupt or "
                        "forged); refusing the read\n", (const void *)addr);
                free(blob);
                blob = NULL;
                goto out;
            }
            *blob_out = blob;
            *blen_out = total_len;
            v3_rcache_put(v, addr, blob, total_len);
            rc = 0;
            goto out;
        }
    }

    /* btree_search's value points into a per-thread buffer valid only until
     * the next search: copy it out before doing anything else. */
    blob = (uint8_t *)malloc(val.n ? val.n : 1);
    if (!blob)
        goto out;
    if (val.n)
        memcpy(blob, val.p, val.n);
    v3_blake3(blob, val.n, chk);
    if (memcmp(chk, addr, INVFS_V3_RECIPE_ADDR_LEN) != 0) {
        fprintf(stderr, "v3 recipe blob %p: BLAKE3 mismatch (corrupt or "
                "forged); refusing the read\n", (const void *)addr);
        free(blob);
        blob = NULL;
        goto out;
    }
    *blob_out = blob;
    *blen_out = val.n;
    v3_rcache_put(v, addr, blob, val.n);
    rc = 0;

out:
    vol_reclaim_reader_release();
    return rc;
}





/* ------------------------------------------------------------------ */
/* WP-M6: v3 dirent tree (base B+-tree namespace)                      */
/*                                                                    */
/* Key layout (frozen here; WP-M7's delta keys must reuse it):        */
/*   parent_inode_id:u64 BE || name_len:u16 BE || name bytes          */
/* Value: child_inode_id:u64 BE (8 bytes).                            */
/*                                                                    */
/* A directory's own anchor entry has name_len == 0 (parent == the     */
/* directory inode, value == that inode). It sorts first inside the    */
/* directory's key range and separates "the directory exists" from     */
/* "this directory has children", which is what readdir/rmdir need.   */
/*                                                                    */
/* Writes go straight to the base tree (no delta yet, WP-M7) and      */
/* publish the new root through the WP-M2 double slot, exactly like    */
/* WP-M5's inode rows.                                                 */
/* ------------------------------------------------------------------ */

/* Escapes outside the listed set must not collide with the 8-byte inode
 * keys: a dirent key is always >= 10 bytes, so an inode key can never equal
 * it, and byte-lexicographic order keeps the two namespaces disjoint. */
#define V3_DIRENT_KEY_FIXED 10u

/* Build a key; returns its length. `kb` must hold 10 + name_len bytes. */
static uint16_t v3_dirent_key(uint8_t *kb, uint64_t parent,
                              const char *name, size_t nlen)
{
    int i;
    for (i = 0; i < 8; i++)
        kb[i] = (uint8_t)(parent >> (56 - 8 * i));
    kb[8] = (uint8_t)(nlen >> 8);
    kb[9] = (uint8_t)(nlen & 0xFF);
    if (nlen)
        memcpy(kb + V3_DIRENT_KEY_FIXED, name, nlen);
    return (uint16_t)(V3_DIRENT_KEY_FIXED + nlen);
}

static uint16_t v3_dirent_key_len(const uint8_t *kb)
{
    return (uint16_t)(V3_DIRENT_KEY_FIXED +
                      (((uint16_t)kb[8] << 8) | kb[9]));
}

static void v3_dirent_val(uint8_t vb[8], uint64_t child)
{
    int i;
    for (i = 0; i < 8; i++)
        vb[i] = (uint8_t)(child >> (56 - 8 * i));
}

static uint64_t v3_dirent_val_get(const uint8_t *p, uint16_t n)
{
    uint64_t id = 0;
    int i;
    if (!p || n < 8)
        return 0;
    for (i = 0; i < 8; i++)
        id = (id << 8) | p[i];
    return id;
}

int vol_v3_dirent_get(invfs_volume *v, uint64_t parent, const char *name,
                      uint64_t *child_out)
{
    uint8_t kb[V3_DIRENT_KEY_FIXED + INVFS_MAX_NAME];
    bt_val val;
    uint16_t kn;
    size_t nlen = name ? strlen(name) : 0;
    int found = 0;

    if (!v || nlen > INVFS_MAX_NAME)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    kn = v3_dirent_key(kb, parent, name, nlen);

    /* Test-only seam (src/core/vol_fault.h), armed by INVFS_FAULT. Stands in
     * for the DIRENT row read failing -- an unreadable or quarantined base
     * page holding this name's entry -- and injects the same -1 the read
     * failure below produces, so the name-resolution path cannot tell it
     * from the real thing. It is the second trigger for the same fail-open
     * the xattr row read is: when this returns -1, vol_v3_path_lookup
     * returns -1, and a caller that only tests the name for existence goes
     * on as though the name were not there. */
    if (invfs_vol_fault("v3_dirent_row_read"))
        return -1;

    /* WP-M11: delta first; a delete shadows the base dirent.
     * WP-inode-get-fold-race: one critical section for resolve + read. */
    {
        uint8_t vb[8];
        uint16_t dflags = 0, got = 0;
        int drc = vol_delta_lookup_value(v, kb, kn, vb, sizeof vb,
                                         &dflags, &got);
        if (drc < 0)
            return -1;                   /* -2 too: a dirent value is 8 B */
        if (drc == 1) {
            if (dflags & INVFS_DELTA_FLAG_DELETE)
                return 0;
            if (got != 8)
                return -1;
            if (child_out)
                *child_out = v3_dirent_val_get(vb, got);
            return 1;
        }
    }

    if (v3_base_get(v, kb, kn, &val, &found) != 0)
        return -1;
    if (!found)
        return 0;
    if (child_out)
        *child_out = v3_dirent_val_get(val.p, val.n);
    return 1;
}

int vol_v3_dirent_put(invfs_volume *v, uint64_t parent, const char *name,
                      uint64_t child)
{
    uint8_t kb[V3_DIRENT_KEY_FIXED + INVFS_MAX_NAME];
    uint8_t vb[8];
    invfs_blkptr root, nr;
    uint16_t kn;
    size_t nlen = name ? strlen(name) : 0;
    uint64_t old_gen;

    if (!v || nlen > INVFS_MAX_NAME || child == 0)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    if (v3_base_root(v, &root) != 0)
        return -1;
    old_gen = root.gen;
    kn = v3_dirent_key(kb, parent, name, nlen);
    v3_dirent_val(vb, child);
    if (btree_upsert(v, root, (bt_key){kb, kn}, (bt_val){vb, 8}, &nr) != 0)
        return -1;
    return v3_publish(v, nr, old_gen);
}

int vol_v3_dirent_del(invfs_volume *v, uint64_t parent, const char *name)
{
    uint8_t kb[V3_DIRENT_KEY_FIXED + INVFS_MAX_NAME];
    invfs_blkptr root, nr;
    bt_val val;
    uint16_t kn;
    size_t nlen = name ? strlen(name) : 0;
    int found = 0;
    uint64_t old_gen;

    if (!v || nlen > INVFS_MAX_NAME)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    if (v3_base_root(v, &root) != 0)
        return -1;
    kn = v3_dirent_key(kb, parent, name, nlen);
    if (btree_search(v, root, (bt_key){kb, kn}, &val, &found) != 0)
        return -1;
    if (!found)
        return 0;   /* absent: nothing to do */
    old_gen = root.gen;
    if (btree_delete(v, root, (bt_key){kb, kn}, &nr) != 0)
        return -1;
    return v3_publish(v, nr, old_gen);
}

/* WP-M12: the delta-backed dirent mutations. The value is the same u64 BE
 * child id the base stores, so the WP-M11 merge treats both streams alike;
 * the insert is always appended BEFORE the source delete on rename
 * (add-before-remove, design §3). */
int vol_v3_dirent_delta_put(invfs_volume *v, uint64_t parent,
                            const char *name, uint64_t child)
{
    uint8_t kb[V3_DIRENT_KEY_FIXED + INVFS_MAX_NAME];
    uint8_t vb[8];
    uint16_t kn;
    size_t nlen = name ? strlen(name) : 0;

    if (!v || nlen > INVFS_MAX_NAME || child == 0)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    kn = v3_dirent_key(kb, parent, name, nlen);
    v3_dirent_val(vb, child);
    return v3_delta_put(v, kb, kn, vb, sizeof vb);
}

int vol_v3_dirent_delta_del(invfs_volume *v, uint64_t parent, const char *name)
{
    uint8_t kb[V3_DIRENT_KEY_FIXED + INVFS_MAX_NAME];
    uint16_t kn;
    size_t nlen = name ? strlen(name) : 0;
    int ex;

    if (!v || nlen > INVFS_MAX_NAME)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    kn = v3_dirent_key(kb, parent, name, nlen);
    ex = v3_overlay_exists(v, kb, kn);
    if (ex < 0)
        return -1;
    if (ex == 0)
        return 0;   /* absent: nothing to do */
    return v3_delta_del(v, kb, kn);
}

/* Decode one raw dirent key + child id and hand the user callback the
 * NUL-terminated name. Malformed keys and the anchor (name_len 0) are
 * skipped. Shared by the base stream and the delta stream of the merge. */
static int v3_emit_dirent(vol_v3_dirent_cb cb, void *ctx,
                          const uint8_t *kp, uint16_t klen, uint64_t child)
{
    char name[INVFS_MAX_NAME + 1];
    uint16_t nlen;

    if (klen < V3_DIRENT_KEY_FIXED)
        return 0;
    if (v3_dirent_key_len(kp) != klen)
        return 0;   /* malformed: not one of our keys */
    nlen = (uint16_t)(((uint16_t)kp[8] << 8) | kp[9]);
    if (nlen == 0 || nlen > INVFS_MAX_NAME)
        return 0;   /* anchor (or malformed) */
    memcpy(name, kp + V3_DIRENT_KEY_FIXED, nlen);
    name[nlen] = 0;
    return cb(ctx, name, nlen, child);
}

/* ------------------------------------------------------------------ */
/* WP-M11: readdir = merge of the delta's key range and the base's      */
/*                                                                    */
/* The delta is small (§16), so its matching entries are snapshotted    */
/* (keys copied, values read) BEFORE the base scan runs: the merge then */
/* touches only memory and never issues delta I/O from inside a          */
/* btree_scan callback. On an equal key the delta wins -- the tie-break  */
/* frozen here and reused by WP-M14's fold. A delta delete removes the   */
/* key from the merged stream entirely. Collecting the delta first also  */
/* makes the merge correct under a concurrent fold's add-before-remove:  */
/* a key the fold already published into the base but has not yet        */
/* removed from the delta appears in both (delta wins); one already       */
/* removed from the delta is present in the new base.                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *key;       /* owned copy of the raw dirent key */
    uint16_t klen;
    uint8_t  deleted;   /* delete record / anchor / malformed: never emitted */
    uint64_t child;
} v3_merge_ent;

typedef struct {
    v3_merge_ent *ent;
    size_t        n, cap;
    int           oom;
} v3_merge_list;

static void v3_merge_list_free(v3_merge_list *l)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        free(l->ent[i].key);
    free(l->ent);
    l->ent = NULL;
    l->n = l->cap = 0;
}

/* vol_delta_range callback: snapshot one delta dirent into the list. */
/* The volume is not reachable from a pure delta_ref, so the collect
 * callback needs it through the list struct. */
typedef struct {
    invfs_volume  *v;
    v3_merge_list *l;
} v3_merge_collect_ctx;

static int v3_merge_collect(void *ctx_, const uint8_t *key, uint16_t klen,
                            const delta_ref *ref)
{
    v3_merge_collect_ctx *c = (v3_merge_collect_ctx *)ctx_;
    v3_merge_list *l = c->l;
    v3_merge_ent *e;
    uint16_t nlen;

    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        v3_merge_ent *ne = (v3_merge_ent *)realloc(l->ent, ncap * sizeof *ne);
        if (!ne) {
            l->oom = 1;
            return 1;                    /* abort the cursor */
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
    if (klen)
        memcpy(e->key, key, klen);
    e->klen = klen;
    e->deleted = 1;                      /* anchor/malformed/delete default */
    l->n++;                              /* owned by the list from here */
    if (klen >= V3_DIRENT_KEY_FIXED && v3_dirent_key_len(key) == klen) {
        nlen = (uint16_t)(((uint16_t)key[8] << 8) | key[9]);
        if (nlen >= 1 && nlen <= INVFS_MAX_NAME &&
            !(ref->flags & INVFS_DELTA_FLAG_DELETE) && ref->vlen == 8) {
            uint8_t vb[8];
            uint16_t got = 0;
            if (vol_delta_read_value(c->v, ref, vb, sizeof vb, &got) != 0 ||
                got != 8) {
                l->oom = 1;
                return 1;
            }
            e->child = v3_dirent_val_get(vb, got);
            e->deleted = 0;
        }
    }
    return 0;
}

/* Merge cursor: the delta snapshot `ent[0..n)` at index `i`. */
typedef struct {
    vol_v3_dirent_cb     cb;
    void                *ctx;
    const v3_merge_ent  *ent;
    size_t               n, i;
} v3_merge_scan;

static int v3_merge_emit(v3_merge_scan *m, const v3_merge_ent *e)
{
    if (e->deleted)
        return 0;
    return v3_emit_dirent(m->cb, m->ctx, e->key, e->klen, e->child);
}

/* btree_scan callback: emit delta keys ordered before this base key, then
 * either the delta copy (delta wins on an equal key) or the base entry. */
static int v3_merge_base_cb(void *ctx_, bt_key k, bt_val val)
{
    v3_merge_scan *m = (v3_merge_scan *)ctx_;

    while (m->i < m->n &&
           vol_key_cmp(m->ent[m->i].key, m->ent[m->i].klen, k.p, k.n) < 0) {
        int rc = v3_merge_emit(m, &m->ent[m->i]);
        m->i++;
        if (rc)
            return rc;
    }
    if (m->i < m->n &&
        vol_key_cmp(m->ent[m->i].key, m->ent[m->i].klen, k.p, k.n) == 0) {
        /* equal key: the delta copy shadows the base one (frozen rule) */
        int rc = v3_merge_emit(m, &m->ent[m->i]);
        m->i++;
        return rc;
    }
    return v3_emit_dirent(m->cb, m->ctx, k.p, k.n,
                          v3_dirent_val_get(val.p, val.n));
}

int vol_v3_dirent_scan(invfs_volume *v, uint64_t parent,
                       vol_v3_dirent_cb cb, void *ctx)
{
    uint8_t lo[V3_DIRENT_KEY_FIXED], hi[V3_DIRENT_KEY_FIXED];
    invfs_blkptr root;
    v3_merge_list list;
    v3_merge_collect_ctx cc;
    v3_merge_scan m;
    int rc;

    if (!v || !cb)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    /* [parent||0x0000, (parent+1)||0x0000): the anchor first, then every
     * child, all under one contiguous parent prefix. */
    v3_dirent_key(lo, parent, NULL, 0);
    v3_dirent_key(hi, parent + 1, NULL, 0);

    memset(&list, 0, sizeof list);
    cc.v = v;
    cc.l = &list;
    rc = vol_delta_range(v, lo, V3_DIRENT_KEY_FIXED, hi, V3_DIRENT_KEY_FIXED,
                         v3_merge_collect, &cc);
    if (rc != 0 || list.oom) {
        v3_merge_list_free(&list);
        return -1;
    }

    /* Reclaim reader epoch. This is the LONG one: a readdir of a large
     * directory holds the announce for the whole walk, which is what makes
     * the drain's cost real (see the measurement in the WP). Released
     * immediately after btree_scan; the delta-tail flush below works from
     * the already-collected list. */
    (void)vol_reclaim_reader_snapshot();
    if (v3_base_root(v, &root) != 0) {
        vol_reclaim_reader_release();
        v3_merge_list_free(&list);
        return -1;
    }
    m.cb = cb;
    m.ctx = ctx;
    m.ent = list.ent;
    m.n = list.n;
    m.i = 0;
    rc = btree_scan(v, root, (bt_key){lo, V3_DIRENT_KEY_FIXED},
                    (bt_key){hi, V3_DIRENT_KEY_FIXED},
                    v3_merge_base_cb, &m);
    vol_reclaim_reader_release();
    if (rc == 0) {
        /* flush the delta tail (keys after the last base key) */
        while (m.i < m.n) {
            int erc = v3_merge_emit(&m, &m.ent[m.i]);
            m.i++;
            if (erc) {
                rc = erc;
                break;
            }
        }
    }
    v3_merge_list_free(&list);
    return rc;
}

/* Highest 8-byte inode key currently in the base tree (0 = none). Used to
 * resume the id allocator after a reopen: RT30 carries no counter, and
 * reusing an id would alias two inodes. One full scan per mount, not per
 * create. */
static int v3_max_inode_cb(void *ctx, bt_key k, bt_val val)
{
    uint64_t *max = (uint64_t *)ctx;
    uint64_t id = 0;
    int i;
    (void)val;
    if (k.n != 8)
        return 0;   /* a dirent key is >= 10 bytes: inode rows only */
    for (i = 0; i < 8; i++)
        id = (id << 8) | k.p[i];
    if (id > *max)
        *max = id;
    return 0;
}

/* WP-M12: the delta side of the resume scan. Inode rows written since the
 * last fold live only in the recent tier, so without this an id that exists
 * solely as a delta row would be handed out again after a remount and two
 * inodes would alias. klen 8 is the inode namespace (dirents >= 10, xattrs
 * >= 11, recipes 33). A delete record still names the id, which is exactly
 * what we want (never reuse an id that a tombstone shadows). */
static int v3_max_inode_delta_cb(void *ctx, const uint8_t *key, uint16_t klen,
                                 const delta_ref *ref)
{
    uint64_t *max = (uint64_t *)ctx;
    uint64_t id = 0;
    int i;
    (void)ref;
    if (klen != 8)
        return 0;
    for (i = 0; i < 8; i++)
        id = (id << 8) | key[i];
    if (id > *max)
        *max = id;
    return 0;
}

uint64_t vol_v3_inode_alloc(invfs_volume *v)
{
    if (!v)
        return 0;
    /* WP111: recover the high-water mark from the on-disk namespace ONCE per
     * mount. The gate is a dedicated flag, not `next_inode_id <= ROOT`: any
     * other code path that bumps the counter (vol_write_begin burns one on
     * the FUSE write path's first call after every remount) would otherwise
     * shut this off permanently, and every later create would hand out an id
     * that is already live -- two dirents naming one inode row, the older
     * file's content silently replaced, invf-fsck clean. The early returns
     * below deliberately leave the flag clear so the next call retries. */
    if (!v->v3_id_recovered) {
        invfs_blkptr root;
        uint64_t max = 0;
        if (v3_ready(v) != 0)
            return 0;
        /* Reclaim reader epoch. This is a READ and the easy one to miss:
         * the high-water-mark scan runs once per mount, from the create
         * path, and it walks the whole base tree exactly as a readdir
         * would. */
        (void)vol_reclaim_reader_snapshot();
        if (v3_base_root(v, &root) != 0) {
            vol_reclaim_reader_release();
            return 0;
        }
        if (btree_scan(v, root, (bt_key){NULL, 0}, (bt_key){NULL, 0},
                       v3_max_inode_cb, &max) != 0) {
            vol_reclaim_reader_release();
            return 0;
        }
        vol_reclaim_reader_release();
        /* WP-M12: a create since the last fold is delta-only, so the base
         * scan alone would miss it. Unbounded range; only 8-byte keys count. */
        if (vol_delta_range(v, NULL, 0, NULL, 0,
                            v3_max_inode_delta_cb, &max) != 0)
            return 0;
        if (v->next_inode_id > max + 1)
            max = v->next_inode_id - 1;   /* never go backwards */
        v->next_inode_id = max + 1;
        if (v->next_inode_id <= INVFS_V3_ROOT_INO)
            v->next_inode_id = INVFS_V3_ROOT_INO + 1;
        v->v3_id_recovered = 1;
    }
    return v->next_inode_id++;
}

/* ------------------------------------------------------------------ */
/* WP-M18: reverse dirent lookup (inode id -> name for sweep driver)  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t target;
    char    *name;
    size_t   name_cap;
    uint64_t *parent_out;
    int      found;
} v3_name_of_ctx;

static int v3_name_of_walk_cb(void *ctx_, const char *path, uint64_t ino,
                              uint32_t type, uint64_t size, int64_t mtime)
{
    v3_name_of_ctx *c = (v3_name_of_ctx *)ctx_;
    (void)type; (void)size; (void)mtime;
    if (ino == c->target) {
        size_t n = strlen(path);
        if (n >= c->name_cap)
            return -1;
        memcpy(c->name, path, n + 1);
        c->found = 1;
        return 1;
    }
    return 0;
}

/* Reverse dirent lookup: find one name that maps to `inode_id`.
 * For nlink == 1 this is unique; for nlink > 1 any name suffices.
 * Returns 1 found, 0 absent, -1 error. *name may be "": absent.
 * *parent_out is set to the parent inode on success (may be NULL). */
int vol_v3_name_of(invfs_volume *v, uint64_t inode_id,
                   char *name, size_t name_cap,
                   uint64_t *parent_out)
{
    v3_name_of_ctx c;
    invfs_v3_inode in;
    int rc;

    if (!v || !name || name_cap == 0)
        return -1;
    name[0] = 0;
    if (parent_out)
        *parent_out = 0;

    if (vol_v3_inode_get(v, inode_id, &in) != 1)
        return 0;

    c.target = inode_id;
    c.name = name;
    c.name_cap = name_cap;
    c.parent_out = parent_out;
    c.found = 0;

    /* WP135: the status was dropped here, so a walk that STOPPED -- a
     * quarantined base page, an OOM -- answered 0, "absent". Two callers
     * act on that: vol_sweep_name_of() turns it into "this file is gone"
     * and skips the inode, and the iterator's per-inode fallback turns it
     * into NULL, which v3_iter_live_inodes documents as the ordinary "no
     * dirent reference" case. A short walk is -1, which is what this
     * function's own contract already promised. */
    /* WP135: the STRICT walk. "This inode has no name" and "the tree
     * could not be read" are different answers and only one of them is 0.
     * The lenient walk cannot express the difference: it steps over the
     * unreadable row and returns 0, which is what this function did. */
    rc = vol_v3_walk_strict(v, v3_name_of_walk_cb, &c);
    if (rc < 0)
        return -1;
    return c.found ? 1 : 0;
}

/* WP-M21b: compose the full "dir/sub/file" path of an inode by walking
 * parent inodes up to the root (vol_v3_name_of gives one leaf + its
 * parent per hop). v2 record names are full relative paths and every
 * name-keyed consumer (vol_stat/vol_find/create_blob_file, the sweep
 * collector's dedupe table) expects that shape, while the v3 dirent
 * tree only stores leaves. Returns 1 composed, 0 not found, -1 error
 * (too deep / cyclic / buffer too small).
 *
 * The hop loop below runs ONCE on a well-formed volume, and that is not
 * an optimisation: v3_name_of_walk_cb copies the whole path and never
 * assigns *parent_out, so vol_v3_name_of() always reports parent 0 and
 * the loop exits on it. The cost is therefore one reverse walk of the
 * ENTIRE dirent tree per call -- 3,399 directory listings on the
 * 46,245-inode reproducer, each one a vol_delta_range() over the delta
 * index. That is the FUSE open and read path (vol_read.c:1230), not an
 * offline tool. WP117's second commit made vol_delta_range O(log G + k)
 * and took one measured vol_v3_path_of from 44.5 ms to 1.3 ms, but the
 * whole-tree walk is still there. NOT fixed here: it needs its own WP
 * and its own risk assessment. */
int vol_v3_path_of(invfs_volume *v, uint64_t inode_id, char *buf,
                   size_t cap)
{
    char parts[64][INVFS_MAX_NAME + 1];
    uint64_t chain[64];
    uint64_t cur = inode_id;
    int n = 0, i;
    size_t total = 0;

    if (!v || !buf || cap == 0)
        return -1;
    buf[0] = 0;
    if (inode_id == INVFS_V3_ROOT_INO)
        return 0;                       /* root has no name */
    while (cur != INVFS_V3_ROOT_INO && cur != 0) {
        uint64_t parent = 0;
        if (n >= 64)
            return -1;                  /* too deep: refuse, never loop */
        if (vol_v3_name_of(v, cur, parts[n], sizeof parts[n],
                           &parent) != 1)
            return 0;                   /* unlinked between hops */
        chain[n] = cur;
        n++;
        cur = parent;
        for (i = 0; i < n - 1; i++)
            if (chain[i] == cur)
                return -1;              /* cyclic dirent: corruption */
    }
    for (i = n - 1; i >= 0; i--) {
        size_t l = strlen(parts[i]);
        if (l == 0 || l > INVFS_MAX_NAME)
            return -1;
        if (total + l + (total ? 1 : 0) + 1 > cap)
            return -1;                  /* buffer too small */
        if (total)
            buf[total++] = '/';
        memcpy(buf + total, parts[i], l);
        total += l;
        buf[total] = 0;
    }
    return n > 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* WP-M18: live-set iteration for the sweep driver                     */
/* ------------------------------------------------------------------ */

/* WP117: one-shot inode-id -> leaf-name map for the iterator below.
 *
 * vol_v3_name_of() answers "what dirent names this inode" with a full
 * reverse dirent walk of the whole tree (see the note on
 * vol_v3_path_of), and every directory listing it performs is one
 * vol_delta_range(), which walks the whole capacity of an open-addressed
 * hash table. The iterator used to call it once PER LIVE INODE, so a
 * sweep cost O(K x D x G) -- K inodes, D directory listings per lookup
 * (3,399 measured on the 46,245-inode reproducer), G the delta index
 * capacity (262,144). That put vol_heat_sweep_begin at a projected
 * 6-14 days. One vol_v3_walk() builds the whole map, so the cost drops
 * to D x G + K -- the single walk the sweep's collect stage already pays.
 *
 * The stored string is the dirent's LEAF name, exactly what
 * The stored string is the full mount-relative path of ONE dirent
 * naming the inode -- byte for byte what vol_v3_name_of() returned, so
 * every caller of vol_v3_iter_live_inodes keeps its current contract
 * (heat_promote_v3_cb hands the name straight to vol_stat_full, which
 * needs the full path, not a basename). The first name wins on a
 * hardlink (nlink > 1), which is also vol_v3_name_of()'s "any name
 * suffices" rule: both walk the tree in the same order and stop at the
 * first dirent whose child id matches. A path too long for the caller's
 * buffer is stored as "", which is again what vol_v3_name_of() leaves
 * behind for such a path (it refuses rather than truncates). */
typedef struct {
    uint64_t id;
    uint32_t off;          /* name offset in pool, +1; 0 = empty slot */
} v3_nidx_slot;

typedef struct {
    v3_nidx_slot *tab;
    size_t cap;            /* power of two, load factor <= 0.5 */
    size_t n;
    char *pool;            /* NUL-separated names */
    size_t pool_n, pool_cap;
    int oom;
} v3_name_index;

typedef struct {
    invfs_volume *v;
    int (*cb)(invfs_volume *v, uint64_t inode_id, const char *name, void *ctx);
    void *ctx;
    uint64_t *visited;
    size_t visited_cap;
    size_t visited_n;
    int oom;
    v3_name_index nidx;    /* WP117: built once, before the base scan */
    int nidx_ok;
    char namebuf[INVFS_MAX_NAME + 1];   /* fallback path only */
} v3_iter_ctx;

static size_t v3_nidx_hash(uint64_t id)
{
    id ^= id >> 33; id *= 0xff51afd7ed558ccdULL;
    id ^= id >> 33; id *= 0xc4ceb9fe1a85ec53ULL;
    id ^= id >> 33;
    return (size_t)id;
}

static void v3_nidx_free(v3_name_index *ni)
{
    if (!ni)
        return;
    free(ni->tab);
    free(ni->pool);
    ni->tab = NULL;
    ni->pool = NULL;
    ni->cap = ni->n = ni->pool_n = ni->pool_cap = 0;
}

static int v3_nidx_grow(v3_name_index *ni)
{
    size_t nc = ni->cap ? ni->cap * 2 : 1024, i;
    v3_nidx_slot *nt = (v3_nidx_slot *)calloc(nc, sizeof *nt);

    if (!nt) { ni->oom = 1; return -1; }
    for (i = 0; i < ni->cap; i++) {
        size_t h;
        if (!ni->tab[i].off)
            continue;
        h = v3_nidx_hash(ni->tab[i].id) & (nc - 1);
        while (nt[h].off)
            h = (h + 1) & (nc - 1);
        nt[h] = ni->tab[i];
    }
    free(ni->tab);
    ni->tab = nt;
    ni->cap = nc;
    return 0;
}

/* Record id -> `leaf`. Returns 1 on success (an id already present
 * keeps its first name), 0 on OOM. */
static int v3_nidx_put(v3_name_index *ni, uint64_t id, const char *leaf)
{
    size_t h, nl;

    if (!ni)
        return 0;
    if (ni->cap) {
        h = v3_nidx_hash(id) & (ni->cap - 1);
        while (ni->tab[h].off) {
            if (ni->tab[h].id == id)
                return 1;                   /* first name wins (hardlink) */
            h = (h + 1) & (ni->cap - 1);
        }
    }
    if ((ni->n + 1) * 2 >= ni->cap && v3_nidx_grow(ni) != 0)
        return 0;
    nl = strlen(leaf) + 1;
    if (ni->pool_n + nl > ni->pool_cap) {
        size_t nc = ni->pool_cap ? ni->pool_cap : 4096;
        char *np;
        while (nc < ni->pool_n + nl) {
            if (nc > 0xF0000000u) { ni->oom = 1; return 0; }
            nc *= 2;
        }
        np = (char *)realloc(ni->pool, nc);
        if (!np) { ni->oom = 1; return 0; }
        ni->pool = np;
        ni->pool_cap = nc;
    }
    h = v3_nidx_hash(id) & (ni->cap - 1);
    while (ni->tab[h].off)
        h = (h + 1) & (ni->cap - 1);
    memcpy(ni->pool + ni->pool_n, leaf, nl);
    ni->tab[h].id = id;
    ni->tab[h].off = (uint32_t)(ni->pool_n + 1);
    ni->pool_n += nl;
    ni->n++;
    return 1;
}

/* The dirent path for an inode, or NULL if no dirent names it. */
static const char *v3_nidx_get(const v3_name_index *ni, uint64_t id)
{
    size_t h;

    if (!ni || !ni->cap)
        return NULL;
    h = v3_nidx_hash(id) & (ni->cap - 1);
    while (ni->tab[h].off) {
        if (ni->tab[h].id == id)
            return ni->pool + (ni->tab[h].off - 1);
        h = (h + 1) & (ni->cap - 1);
    }
    return NULL;
}

/* One dirent's mount-relative path per inode, exactly as
 * vol_v3_name_of() would have reported it: the full path, or "" when
 * the path does not fit the iterator's name buffer. vol_v3_walk's own
 * path buffer is 600 bytes, so a path longer than either is a corrupt
 * namespace rather than a name to keep. */
static int v3_nidx_build_cb(void *ctx_, const char *path, uint64_t ino,
                            uint32_t type, uint64_t size, int64_t mtime)
{
    v3_name_index *ni = (v3_name_index *)ctx_;

    (void)type; (void)size; (void)mtime;
    if (!ni || !path)
        return 0;
    return v3_nidx_put(ni, ino,
                       strlen(path) < INVFS_MAX_NAME + 1 ? path : "")
           ? 0
           : 1;                                  /* 1 stops the walk */
}

/* One vol_v3_walk() for the whole iteration. 0 = map ready, -1 = could
 * not build it (the caller then falls back to the per-inode walk). */
static int v3_nidx_build(invfs_volume *v, v3_name_index *ni)
{
    int rc;

    memset(ni, 0, sizeof *ni);
    rc = vol_v3_walk(v, v3_nidx_build_cb, ni);
    if (rc != 0 || ni->oom) {
        v3_nidx_free(ni);
        return -1;
    }
    return 0;
}

/* The one place the iterator resolves a name. Returns NULL when the
 * inode has no dirent reference -- the documented "may be NULL" case.
 * Falls back to the per-inode reverse walk only if the one-shot map
 * could not be built, i.e. exactly the pre-WP117 behaviour. */
static const char *v3_iter_name(v3_iter_ctx *ic, uint64_t inode_id)
{
    uint64_t parent;

    if (ic->nidx_ok)
        return v3_nidx_get(&ic->nidx, inode_id);
    ic->namebuf[0] = 0;
    if (vol_v3_name_of(ic->v, inode_id, ic->namebuf,
                       sizeof ic->namebuf, &parent) != 1)
        return NULL;
    return ic->namebuf;
}

/* WP-M21b: the visited set became a real open-addressed table (0 = empty;
 * inode id 0 is never live). The original append-anywhere + fixed-window
 * probe was a probabilistic dedupe -- fine for the rare nlink>1 double
 * visit, not fine for the delta pass below, which must reliably tell
 * "this delta row already reported from base" apart from "delta-only
 * row". Load factor is kept <= 0.5 with rehash on growth. */
static int v3_iter_seen(const v3_iter_ctx *ic, uint64_t id)
{
    size_t h;
    if (!ic->visited_cap)
        return 0;
    h = id & (ic->visited_cap - 1);
    while (ic->visited[h] != 0) {
        if (ic->visited[h] == id)
            return 1;
        h = (h + 1) & (ic->visited_cap - 1);
    }
    return 0;
}

/* Returns 1 on success (already present counts as success), 0 on OOM
 * (sets ic->oom). */
static int v3_iter_mark(v3_iter_ctx *ic, uint64_t id)
{
    size_t h;

    if (v3_iter_seen(ic, id))
        return 1;
    if (ic->visited_n * 2 >= ic->visited_cap) {
        size_t nc = ic->visited_cap ? ic->visited_cap * 2 : 64;
        size_t i;
        uint64_t *nv = (uint64_t *)calloc(nc, sizeof *nv);
        if (!nv) { ic->oom = 1; return 0; }
        for (i = 0; i < ic->visited_cap; i++) {
            if (ic->visited[i]) {
                size_t hh = ic->visited[i] & (nc - 1);
                while (nv[hh])
                    hh = (hh + 1) & (nc - 1);
                nv[hh] = ic->visited[i];
            }
        }
        free(ic->visited);
        ic->visited = nv;
        ic->visited_cap = nc;
    }
    h = id & (ic->visited_cap - 1);
    while (ic->visited[h])
        h = (h + 1) & (ic->visited_cap - 1);
    ic->visited[h] = id;
    ic->visited_n++;
    return 1;
}

/* WP145: test-only seam (src/core/vol_fault.h), armed by
 * INVFS_FAULT="iter_live_inodes_row:<n>". It stands in for ONE live-inode row
 * of the iteration failing to read -- a quarantined or otherwise unreadable
 * base page, which is what stops a real walk here -- and it is consulted once
 * per row the iteration is about to DELIVER, immediately before the caller's
 * callback runs. So the ordinal in the spec is a position WITHIN the walk: n=1
 * leaves exactly one inode visited and every later one unreached.
 *
 * The distinction from the iter_live_inodes site above is the whole point of
 * this one, and it is not a nicety. That site fires before the first row, so
 * every caller sees the same thing: nothing at all. It cannot tell a caller
 * that it "saw something", which is the state that actually has to be
 * recognised -- a walk that delivered a prefix is not a small whole set, and
 * no count derived from it is a count of the volume.
 *
 * ONE site name, checked from both passes (the base scan below and the delta
 * pass), because they are two halves of ONE iteration: whichever half carries
 * the live rows consumes the countdown, so the ordinal means the same thing
 * whether or not the volume has been folded.
 *
 * Unset in production, where it costs one getenv and one pointer compare. */
static int v3_iter_row_stop(void)
{
    return invfs_vol_fault("iter_live_inodes_row");
}

static int v3_iter_base_cb(void *ctx_, bt_key k, bt_val val)
{
    v3_iter_ctx *ic = (v3_iter_ctx *)ctx_;
    uint64_t inode_id = 0;
    int i;

    if (k.n != 8)
        return 0;
    for (i = 0; i < 8; i++)
        inode_id = (inode_id << 8) | k.p[i];

    /* consult delta overlay: DELETE flag = skip this inode */
    {
        delta_ref dr;
        int drc = vol_delta_lookup(ic->v, k.p, 8, &dr);
        if (drc < 0)
            return -1;
        if (drc == 1 && (dr.flags & INVFS_DELTA_FLAG_DELETE))
            return 0;
    }

    /* every base id is marked visited: nlink > 1 hardlinks share one row
     * (one key in the inode range, so no intra-base dupes) and the delta
     * pass below skips rows whose id was already reported from base */
    if (!v3_iter_mark(ic, inode_id))
        return -1;

    if (v3_iter_row_stop())            /* WP145: see v3_iter_row_stop */
        return -1;

    return ic->cb(ic->v, inode_id, v3_iter_name(ic, inode_id), ic->ctx);
}

/* WP-M21b: delta pass. Rows created (or recreated) since the last fold
 * live ONLY in the delta -- the base scan above never sees their keys,
 * so a fresh v3 volume (or any volume swept between folds) iterated as
 * empty: measured, invf-cp of 5 files then sweep reported "live entries:
 * 0 (of 0 walked)". Visit every delta PUT in the inode range whose id
 * the base pass did not already report. */
static int v3_iter_delta_cb(void *ctx_, const uint8_t *key, uint16_t klen,
                            const delta_ref *ref)
{
    v3_iter_ctx *ic = (v3_iter_ctx *)ctx_;
    uint64_t inode_id = 0;
    int i;

    if (klen != 8)
        return 0;
    for (i = 0; i < 8; i++)
        inode_id = (inode_id << 8) | key[i];
    if (ref->flags & INVFS_DELTA_FLAG_DELETE)
        return 0;                   /* created and deleted between folds */
    if (v3_iter_seen(ic, inode_id))
        return 0;                   /* base row (possibly updated): done */
    if (!v3_iter_mark(ic, inode_id))
        return -1;

    if (v3_iter_row_stop())            /* WP145: see v3_iter_row_stop */
        return -1;

    return ic->cb(ic->v, inode_id, v3_iter_name(ic, inode_id), ic->ctx);
}

/* Public entry point. Callback is invoked once per live inode (nlink > 1
 * visited once). Callback receives name (may be NULL if not found via
 * dirent). Returns 0 complete, -1 error, callback non-zero propagated. */
int vol_v3_iter_live_inodes(invfs_volume *v,
    int (*cb)(invfs_volume *v, uint64_t inode_id, const char *name, void *ctx),
    void *ctx)
{
    invfs_blkptr root;
    v3_iter_ctx ic;
    uint64_t max_id = 0;
    uint8_t lo[8], hi[8];
    int rc;

    if (!v || !cb)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    /* WP86: the base root read below can now fail on a torn root (it used to
     * answer "no root"), and `root` was left uninitialised for the scan that
     * follows -- an uninitialised blkptr into btree_scan. */
    memset(&root, 0, sizeof root);

    /* find the highest inode id currently allocated so the scan range
     * upper bound is tight */
    {
        uint64_t m = 0;
        (void)vol_delta_range(v, NULL, 0, NULL, 0,
                              v3_max_inode_delta_cb, &m);
        max_id = m;
    }
    /* Reclaim reader epoch. One section spans BOTH base walks and the name
     * index build between them (which is itself a dirent walk), and it is
     * released before the delta pass at the end. This is the sweep's collect
     * walker, so on a large volume it is the longest single hold on the
     * counter in the tree -- the drain's measured worst case. */
    /* WP135: test-only seam -- the BASE scan fails, which is the state that
     * costs the most. vol_v3_iter_live_inodes skips the delta pass entirely
     * once rc is set (see `if (rc == 0)` below), so the caller loses every
     * inode created since the last fold as well -- and the count it stored
     * still matches the count it SAW, which is how the sweep read the
     * shortfall as COMPLETE. Unset in production. */
    if (invfs_vol_fault("iter_live_inodes"))
        return -1;

    (void)vol_reclaim_reader_snapshot();
    if (v3_base_root(v, &root) == 0 && root.pba != 0) {
        uint64_t m = 0;
        (void)btree_scan(v, root, (bt_key){NULL, 0}, (bt_key){NULL, 0},
                         v3_max_inode_cb, &m);
        if (m > max_id)
            max_id = m;
    }
    if (max_id == 0)
        max_id = INVFS_V3_ROOT_INO;

    v3_ino_key(INVFS_V3_ROOT_INO, lo);
    v3_ino_key(max_id + 1, hi);

    memset(&ic, 0, sizeof ic);
    ic.v = v;
    ic.cb = cb;
    ic.ctx = ctx;
    /* WP117: ONE dirent walk for the whole iteration feeds the name map
     * both passes below read from. Before this, each live inode paid its
     * own whole-tree reverse walk (see v3_name_index). */
    ic.nidx_ok = (v3_nidx_build(v, &ic.nidx) == 0);

    rc = btree_scan(v, root, (bt_key){lo, 8}, (bt_key){hi, 8},
                    v3_iter_base_cb, &ic);
    vol_reclaim_reader_release();
    if (rc != 0 || ic.oom)
        rc = -1;
    /* WP-M21b: rows created since the last fold exist only in the delta;
     * visit the ones the base pass did not already report. Same key range
     * (max_id above already accounts for the delta's highest id). */
    if (rc == 0) {
        int drc = vol_delta_range(v, lo, 8, hi, 8, v3_iter_delta_cb, &ic);
        if (drc != 0 || ic.oom)
            rc = -1;
    }
    v3_nidx_free(&ic.nidx);
    free(ic.visited);
    return rc;
}

/* ------------------------------------------------------------------ */
/* WP118: nlink vs dirent fan-in                                       */
/* ------------------------------------------------------------------ */

/* The invariant: for every live inode, the number of directory entries
 * (names) that RESOLVE to it -- its fan-in -- equals its nlink. Hardlinks
 * are why this is the right invariant and "inode ids must be unique" is the
 * wrong one: `ln a b` puts two dirents on ONE id and bumps nlink to 2, so a
 * uniqueness check false-positives on every correct volume that uses
 * hardlinks while still missing the corruption that actually happens -- two
 * names landing on one id whose nlink was never bumped, which silently
 * replaces one file's content with another's (WP111b, commit 1771a0d: a
 * 7-name volume that reported "5 files ok, 0 corrupt" and fsck OK).
 *
 *   fanin == nlink   correct
 *   fanin <  nlink   a name is MISSING: the row claims links no dirent
 *                    accounts for (a lost name, or a count that was never
 *                    decremented)
 *   fanin >  nlink   a STALE dirent: more names resolve to the id than the
 *                    row claims -- the aliasing corruption
 *
 * One case is counted but NOT fatal: a live row that NO name resolves to
 * (fan-in 0, nlink > 0) is an orphan row. The v3 write order is row first,
 * dirent second (vol_v3_create_content_node), so a crash between the two
 * legitimately leaves a row nobody names -- and nothing in the repair path
 * removes it, so making it fatal would leave a volume permanently DAMAGED
 * over a state fsck cannot fix. It is reported, loudly, on its own line;
 * the named-inode accounting above is what the exit code follows.
 *
 * Deliberately NOT audited:
 *   - directories: their nlink is the POSIX "2" (itself + parent), not a
 *     count of names, so fan-in cannot be compared against it;
 *   - engine-internal owners (names starting 0x01: the tz batch owner, the
 *     seal parity owners, the retention registry): their link counts are the
 *     engine's own bookkeeping, not a namespace invariant;
 *   - the root inode, which has no name at all.
 *
 * `names` counts what vol_v3_walk -- the walk the read/verify path itself
 * uses -- reaches, so a name whose inode row does not resolve is outside
 * both sides of the comparison (that loss is reported by the base-page
 * damage path, which owns it).
 *
 * Cost: one btree range scan of the inode keyspace, one delta pass (both
 * O(pages)) and one hierarchical namespace walk. Deliberately NOT
 * vol_v3_iter_live_inodes: that resolves a name per inode through a full
 * reverse dirent walk (O(depth x live) each -- the WP117 quadratic), and
 * this check runs inside fsck, where a large volume must still finish. */

typedef struct {
    uint64_t id;
    uint32_t nlink;      /* 0 = the row was not seen (name-only entry) */
    uint32_t fanin;
    char     name[192];  /* one name that resolves to it */
} nlink_ent;

typedef struct {
    nlink_ent *tab;
    size_t cap, n;       /* cap is a power of two; id 0 marks an empty slot */
    invfs_nlink_audit *a;
    invfs_volume *v;
    int oom;
} nlink_ctx;

static int nlink_grow(nlink_ctx *c)
{
    size_t nc = c->cap ? c->cap * 2 : 256;
    nlink_ent *nt = (nlink_ent *)calloc(nc, sizeof *nt);
    size_t i;
    if (!nt) { c->oom = 1; return -1; }
    for (i = 0; i < c->cap; i++) {
        if (c->tab[i].id) {
            size_t h = (size_t)(c->tab[i].id ^ (c->tab[i].id >> 32)) & (nc - 1);
            while (nt[h].id)
                h = (h + 1) & (nc - 1);
            nt[h] = c->tab[i];
        }
    }
    free(c->tab);
    c->tab = nt;
    c->cap = nc;
    return 0;
}

/* find-or-insert; *fresh is set when this call created the entry. NULL on
 * OOM (c->oom is then set). */
static nlink_ent *nlink_slot(nlink_ctx *c, uint64_t id, int *fresh)
{
    size_t mask, h;
    if (fresh)
        *fresh = 0;
    if (!c->cap || (c->n + 1) * 2 >= c->cap) {
        if (nlink_grow(c) != 0)
            return NULL;
    }
    mask = c->cap - 1;
    h = (size_t)(id ^ (id >> 32)) & mask;
    while (c->tab[h].id) {
        if (c->tab[h].id == id)
            return &c->tab[h];
        h = (h + 1) & mask;
    }
    c->tab[h].id = id;
    c->n++;
    if (fresh)
        *fresh = 1;
    return &c->tab[h];
}

static void nlink_fault(invfs_nlink_audit *a, const char *what,
                        uint64_t id, uint32_t nlink, uint32_t fanin,
                        const char *name)
{
    if (a->nfault < INVFS_NLINK_FAULT_MAX) {
        invfs_nlink_fault *f = &a->fault[a->nfault++];
        f->id = id;
        f->nlink = nlink;
        f->fanin = fanin;
        f->reason = what;
        snprintf(f->name, sizeof f->name, "%s", name ? name : "");
    }
    a->nfault_total++;
}

/* pass 1: every live, non-directory inode row. The base tree first, then
 * the delta for rows created since the last fold. An 8-byte key is an inode
 * row: dirent keys are >= 10 bytes and carry a parent prefix, xattr keys
 * start 0x03, recipe keys 0x04 (invarifs.h). */
static int nlink_row_cb(void *ctx_, bt_key k, bt_val val)
{
    nlink_ctx *c = (nlink_ctx *)ctx_;
    uint64_t id = 0;
    invfs_v3_inode in;
    nlink_ent *e;
    int fresh;

    (void)val;
    if (k.n != 8)
        return 0;
    {
        int i;
        for (i = 0; i < 8; i++)
            id = (id << 8) | k.p[i];
    }
    if (id == INVFS_V3_ROOT_INO)
        return 0;
    /* vol_v3_inode_get consults the delta overlay, so a row the delta
     * deleted reads back absent and a row the delta replaced reads back
     * current -- one truth for both passes. */
    if (vol_v3_inode_get(c->v, id, &in) != 1)
        return 0;
    if (in.type == INVFS_ITYP_DIR)
        return 0;                       /* dir nlink is not a name count */
    if (in.nlink == 0 || in.nlink == 0xFFFFFFFFu)
        return 0;                       /* malformed / wrap sentinel */
    e = nlink_slot(c, id, &fresh);
    if (!e)
        return -1;
    e->nlink = in.nlink;
    /* an id the delta shadows is visited by BOTH passes (base key, delta
     * key); count the row once. The nlink it carries is the same either
     * way -- vol_v3_inode_get resolves the overlay -- so re-reading it is
     * harmless; counting it twice would not be. */
    if (fresh)
        c->a->inodes++;
    return 0;
}

static int nlink_row_delta_cb(void *ctx_, const uint8_t *key, uint16_t klen,
                              const delta_ref *ref)
{
    (void)ref;
    if (klen != 8)
        return 0;
    return nlink_row_cb(ctx_, (bt_key){key, klen}, (bt_val){NULL, 0});
}

/* pass 2: every name, hierarchically. */
static int nlink_name_cb(void *ctx_, const char *path, uint64_t ino,
                        uint32_t type, uint64_t size, int64_t mtime)
{
    nlink_ctx *c = (nlink_ctx *)ctx_;
    nlink_ent *e;
    int fresh;

    (void)size; (void)mtime;
    if (!path || !path[0])
        return 0;
    if ((unsigned char)path[0] == 0x01)
        return 0;                       /* engine-internal owner entry */
    c->a->names++;
    if (type == INVFS_ITYP_DIR)
        return 0;                       /* see the header comment */
    /* defensive: a row the pass-1 filter would reject must not be counted as
     * a stale dirent. (The walk already skips names whose row does not
     * resolve at all, so this is belt and braces.) */
    {
        invfs_v3_inode in;
        if (vol_v3_inode_get(c->v, ino, &in) == 1 &&
            (in.nlink == 0 || in.nlink == 0xFFFFFFFFu))
            return 0;
    }
    e = nlink_slot(c, ino, &fresh);
    if (!e)
        return -1;
    if (fresh || !e->name[0])
        snprintf(e->name, sizeof e->name, "%s", path);
    e->fanin++;
    return 0;
}

int vol_v3_nlink_audit(invfs_volume *v, invfs_nlink_audit *out)
{
    nlink_ctx c;
    invfs_blkptr root;
    uint8_t lo[8], page[INVFS_BLOCK_SIZE];
    size_t i;
    int rc;

    if (!v || !out)
        return -1;
    memset(out, 0, sizeof *out);
    if (v3_ready(v) != 0)
        return -1;

    memset(&c, 0, sizeof c);
    c.a = out;
    c.v = v;

    memset(&root, 0, sizeof root);
    v3_ino_key(INVFS_V3_ROOT_INO, lo);
    rc = 0;
    /* Reclaim reader epoch. The announce has to sit OUTSIDE the condition
     * below, because v3_base_root is inside it: announcing after the capture
     * would leave exactly the window this mechanism exists to close. */
    (void)vol_reclaim_reader_snapshot();
    if (v3_base_root(v, &root) == 0 && root.pba &&
        mbuf_read(v, root.pba, page) == 0) {
        /* the root page's level decides the blkptr flags btree_scan wants
         * (the same pairing spt0_tree_ok / vol_v3_iter_inodes_at build) */
        mbuf_ptr_set(&root, root.pba, page,
                     mbuf_page_chdr(page)->level == INVFS_PAGE_LEVEL_LEAF
                     ? INVFS_BP_ROOT | INVFS_BP_LEAF
                     : INVFS_BP_ROOT | INVFS_BP_INTERNAL);
        rc = btree_scan(v, root, (bt_key){lo, 8}, (bt_key){NULL, 0},
                        nlink_row_cb, &c);
    }
    vol_reclaim_reader_release();
    /* The delta pass is NOT conditional on the base tree: a volume whose
     * rows have not been folded yet has an EMPTY base root slot and every
     * inode row in the delta, which is the state a freshly mounted volume
     * is in most of the time. Skipping the delta with the base scan would
     * leave every name pointing at a row pass 1 never saw -- the check
     * would then "detect" the whole namespace as dead. */
    if (rc == 0)
        rc = vol_delta_range(v, lo, 8, NULL, 0, nlink_row_delta_cb, &c);
    if (rc == 0)
        rc = vol_v3_walk(v, nlink_name_cb, &c);
    if (c.oom)
        rc = -1;

    /* verdict: every entry whose fan-in does not match its nlink */
    for (i = 0; i < c.cap; i++) {
        nlink_ent *e = &c.tab[i];
        if (!e->id)
            continue;
        if (e->nlink == 0) {
            /* a name resolved here but pass 1 never saw a live row for it */
            out->dead_names++;
            nlink_fault(out, "dead", e->id, 0, e->fanin, e->name);
            continue;
        }
        if (e->fanin == 0) {
            /* a live row no directory entry names: reported, not fatal */
            out->orphan_rows++;
            nlink_fault(out, "orphan-row", e->id, e->nlink, 0, e->name);
            continue;
        }
        if (e->fanin == e->nlink)
            continue;
        if (e->fanin > e->nlink) {
            out->stale_dirents += e->fanin - e->nlink;
            nlink_fault(out, "stale-dirent", e->id, e->nlink, e->fanin,
                        e->name);
        } else {
            out->missing_names += e->nlink - e->fanin;
            nlink_fault(out, "missing-name", e->id, e->nlink, e->fanin,
                        e->name);
        }
    }
    if (out->stale_dirents || out->missing_names || out->dead_names)
        out->mismatch = 1;

    free(c.tab);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Recipe resolvability (fsck)                                         */
/* ------------------------------------------------------------------ */
/* A v3 inode row does not carry its content: it carries a 32-byte
 * BLAKE3 content address, and the recipe blob lives under its own key
 * (0x04 || addr) in the SAME base tree the page walk already verified
 * (v3_recipe_key, above). So the tree walk cannot find it -- not by
 * following a pointer, because there is no pointer to follow, and not by
 * counting keys, because a key that is gone leaves no trace in the page's
 * own CRC. The only thing in the volume that still says the blob must be
 * there is the address in the row, and nothing in the fsck path was
 * reading it. That is the whole defect: the read path resolves the
 * address (vol_read_inode -> vol_v3_recipe_load) and fsck did not, so a
 * volume with an unreadable file passed every structural check fsck ran.
 *
 * The walk itself is the one vol_v3_iter_live_inodes already performs
 * (base range + delta overlay, one visit per inode, names resolved), so
 * the audit adds no new traversal shape to the volume. */

typedef struct {
    invfs_recipe_audit *a;
} recipe_audit_ctx;

static void recipe_fault(invfs_recipe_audit *a, uint64_t id, uint32_t kind,
                         uint64_t size, const char *name)
{
    if (a->nfault < INVFS_RECIPE_BAD_MAX) {
        invfs_recipe_fault *f = &a->fault[a->nfault++];
        f->id = id;
        f->kind = kind;
        f->size = size;
        snprintf(f->name, sizeof f->name, "%s", name ? name : "");
    }
    a->nfault_total++;
}

static int recipe_audit_cb(invfs_volume *v, uint64_t inode_id, const char *name,
                           void *ctx_)
{
    recipe_audit_ctx *c = (recipe_audit_ctx *)ctx_;
    static const uint8_t zero_addr[INVFS_V3_RECIPE_ADDR_LEN];
    invfs_v3_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    uint32_t kind;

    if (vol_v3_inode_get(v, inode_id, &in) != 1)
        return 0;
    if (in.type == INVFS_ITYP_DIR)
        return 0;                  /* a directory has no recipe */
    /* Two more of the read path's cases (vol_read.c): an empty or
     * address-less row hands back zero bytes, so a row that matches is not
     * an unreadable file. NOT the whole of them -- the read path also
     * short-circuits on the inode TYPE, which is the case this audit used
     * to miss, below. */
    if (in.size == 0 ||
        memcmp(in.recipe_addr, zero_addr, INVFS_V3_RECIPE_ADDR_LEN) == 0)
        return 0;

    c->a->checked++;
    if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) == 0 && blob) {
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents = NULL;
        size_t n_ents = 0;
        /* A raw-blob type is readable once it LOADS: that is the whole of
         * what the read path demands of it (vol_read.c returns the blob
         * verbatim, with no vol_ast_recipe_parse at all). Parsing it as an
         * AST anyway is what made every symlink on a v3 volume report as
         * lost content and the volume report DAMAGED -- a checker stricter
         * than the path it audits, inventing damage on a healthy volume.
         * Only the PARSE is skipped, never the load above, so a genuinely
         * missing symlink blob is still caught, and an AST-typed inode
         * still gets its CORRUPT verdict below. */
        if (invfs_inode_content_is_raw_blob(in.type)) {
            free(blob);
            return 0;
        }
        if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) == 0) {
            free(blob);
            return 0;
        }
        kind = INVFS_RECIPE_BAD_CORRUPT;
    } else {
        kind = INVFS_RECIPE_BAD_MISSING;
    }
    c->a->bad++;
    recipe_fault(c->a, inode_id, kind, in.size, name);
    free(blob);                    /* NULL on the load-failure path */
    return 0;
}

int vol_v3_recipe_audit(invfs_volume *v, invfs_recipe_audit *out)
{
    recipe_audit_ctx c;
    int rc;

    if (!v || !out)
        return -1;
    memset(out, 0, sizeof *out);
    if (v3_ready(v) != 0)
        return -1;
    memset(&c, 0, sizeof c);
    c.a = out;
    rc = vol_v3_iter_live_inodes(v, recipe_audit_cb, &c);
    return rc < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* WP96: live-set iteration at an ARBITRARY save-point generation       */
/* ------------------------------------------------------------------ */

/* vol_v3_iter_live_inodes above always walks the CURRENT base root and the
 * CURRENT delta overlay. WP96 needs the inode set of a PAST generation --
 * the one an SPT0 save point pinned -- twice: at capture, to take the data
 * pin, and at restore, to verify the pinned state before republishing it.
 * That state is exactly the pair SPT0 records:
 *
 *     { base inode rows at `root_pba` } overlaid by
 *     { delta records written before `delta_end` }
 *
 * The delta half CANNOT come from v->delta_index: the index coalesces, so it
 * only knows the newest record per key, and the whole point of this walk is
 * the generation where a later record has since replaced the pinned one. So
 * the pinned prefix is replayed from the log itself, oldest segment to
 * newest, and every 8-byte-key (inode) record in it is collected with its
 * position; last-wins within a key is decided by log order, exactly as
 * vol_delta_mount resolves it.
 *
 * The prefix test needs the chain geometry, because delta_end is measured in
 * the log's own units: spt0_capture counts `delta_bump` bytes of the then-
 * head segment plus INVFS_DELTA_SEG_BYTES for every older one. So a record
 * written at (segment, depth d from the head, offset off) belongs to the
 * pinned prefix iff
 *
 *     d >= delta_segs                  -> appended after the capture
 *     d <  delta_segs - 1              -> a full older segment: in
 *     d == delta_segs - 1              -> the captured head: off < bump,
 *                                          bump = delta_end - (segs-1)*SEG
 *
 * and a segment that is not in the chain at all is not in the prefix. That
 * those records are still PHYSICALLY there is spt0_delta_intact's job in
 * vol_spt0.c, not this walk's (a fold resets the chain, and then the pinned
 * state is unrecoverable -- a refusal, not a walk).
 *
 * Cost: one sequential pass over the pinned prefix of the log (the same bytes
 * vol_delta_mount replays at every open), one sequential pass over the base
 * tree under the root, and an O(k log k) sort of the k inode records the
 * prefix holds. No name resolution (callers want recipes, not paths) and no
 * recursion beyond btree_scan's own. */

/* one inode record inside the pinned log prefix */
typedef struct {
    uint64_t id;
    uint64_t seg;
    uint64_t off;
    uint64_t seq;          /* log order; the highest wins for a key */
    uint16_t flags;
    uint16_t vlen;
} v3_gen_rec;

typedef struct {
    invfs_volume *v;
    int (*cb)(invfs_volume *v, uint64_t inode_id,
              const invfs_v3_inode *in, void *ctx);
    void *ctx;
    uint64_t delta_end;
    uint64_t delta_segs;
    uint64_t delta_head_pba; /* the head segment AT capture */
    uint64_t head_bump;
    int64_t   head_depth;    /* where that head sits in the CURRENT chain */
    uint64_t *seg_pba;       /* chain_pba[depth], head first */
    size_t    nseg;
    v3_gen_rec *recs;        /* the prefix's inode records, log order */
    size_t    nrecs, crecs;
    uint64_t *visited;
    size_t    visited_cap;
    size_t    visited_n;
    int       oom;
} v3_gen_ctx;

static int v3_gen_rec_push(v3_gen_ctx *gc, const v3_gen_rec *r)
{
    if (gc->nrecs == gc->crecs) {
        size_t nc = gc->crecs ? gc->crecs * 2 : 64;
        v3_gen_rec *nr = (v3_gen_rec *)realloc(gc->recs, nc * sizeof *nr);
        if (!nr) { gc->oom = 1; return -1; }
        gc->recs = nr;
        gc->crecs = nc;
    }
    gc->recs[gc->nrecs++] = *r;
    return 0;
}

static int v3_gen_rec_cmp(const void *a, const void *b)
{
    const v3_gen_rec *x = (const v3_gen_rec *)a, *y = (const v3_gen_rec *)b;
    if (x->id != y->id)
        return x->id < y->id ? -1 : 1;
    if (x->seq != y->seq)
        return x->seq < y->seq ? -1 : 1;
    return 0;
}

/* Replay the pinned prefix of the log, collecting its inode records. The
 * chain is walked newest -> oldest to build seg_pba, then replayed oldest ->
 * newest (log order) so `seq` is monotone. */
static int v3_gen_prefix_replay(invfs_volume *v, v3_gen_ctx *gc)
{
    uint64_t cur = v->delta_seg_pba, seq = 0;
    uint32_t guard = 0;
    size_t d;

    if (gc->delta_end == 0 || !gc->delta_segs)
        return 0;                            /* the generation had no log */
    while (cur && guard++ < DELTA_MAX_SEGMENTS) {
        invfs_delta_seg_hdr h;
        uint64_t *np;
        if (delta_read_hdr(v, cur, &h) != 0)
            break;
        np = (uint64_t *)realloc(gc->seg_pba, (gc->nseg + 1) * sizeof *np);
        if (!np)
            return -1;
        gc->seg_pba = np;
        gc->seg_pba[gc->nseg++] = cur;
        if (h.prev_pba == cur)
            break;
        cur = h.prev_pba;
    }
    /* Where the CAPTURED head sits in the chain as it is NOW. The log is
     * append-only, so a sweep that filled the head rolled a NEW segment in
     * front of it: the captured head is then at depth > 0 and everything in
     * front of it is post-capture. (Depth-from-the-head alone cannot say
     * this -- that is exactly how a post-sweep recipe used to be mistaken for
     * a pinned one.) */
    gc->head_depth = -1;
    for (d = 0; d < gc->nseg; d++)
        if (gc->seg_pba[d] == gc->delta_head_pba) {
            gc->head_depth = (int64_t)d;
            break;
        }
    if (gc->head_depth < 0)
        return -1;                           /* the prefix is gone; the caller
                                             * refuses the rollback */
    for (d = gc->nseg; d-- > 0; ) {
        uint8_t *buf;
        size_t plen, off = 0, limit;
        invfs_delta_seg_hdr h;

        if ((int64_t)d > gc->head_depth)
            continue;                        /* newer than the captured head */
        if (delta_read_hdr(v, gc->seg_pba[d], &h) != 0)
            return -1;
        buf = (uint8_t *)malloc((size_t)INVFS_DELTA_SEG_BYTES);
        if (!buf)
            return -1;
        if (io_pread(&v->io, gc->seg_pba[d] * (uint64_t)INVFS_BLOCK_SIZE,
                     buf, (size_t)INVFS_DELTA_SEG_BYTES) != 0) {
            free(buf);
            return -1;
        }
        plen = (size_t)(INVFS_DELTA_SEG_BYTES - h.hdr_size);
        /* The captured head contributes only its used prefix; every older
         * segment contributed all of itself (delta_end counts their full
         * stride). `off` below is PAYLOAD-relative (the records start after
         * the segment header) while head_bump is a SEGMENT offset, so the cut
         * has to be translated -- miss that by hdr_size and the first record
         * written after the capture is read as if it were inside it, which
         * hands the restore a post-sweep recipe. */
        limit = plen;
        if ((int64_t)d == gc->head_depth) {
            uint64_t usable = gc->head_bump > h.hdr_size
                            ? gc->head_bump - h.hdr_size : 0;
            limit = (size_t)usable;
        }
        if (limit > plen)
            limit = plen;
        while (off < limit) {
            uint16_t kl, vl, fl;
            size_t rl;
            int rc = vol_delta_rec_parse(buf + h.hdr_size, plen, off,
                                         &kl, &vl, &fl, &rl);
            v3_gen_rec r;
            if (rc <= 0)
                break;                       /* clean end or torn tail */
            if (kl == 8) {
                uint64_t id = 0;
                uint16_t i;
                for (i = 0; i < 8; i++)
                    id = (id << 8) | buf[h.hdr_size + off +
                                          INVFS_DELTA_REC_HDR_LEN + i];
                if (id >= INVFS_V3_ROOT_INO) {
                    r.id = id;
                    r.seg = gc->seg_pba[d];
                    r.off = (uint64_t)h.hdr_size + off;
                    r.seq = seq;
                    r.flags = fl;
                    r.vlen = vl;
                    if (v3_gen_rec_push(gc, &r) != 0) {
                        free(buf);
                        return -1;
                    }
                }
            }
            seq++;
            off += rl;
        }
        free(buf);
        if (gc->oom)
            return -1;
    }
    if (gc->nrecs > 1)
        qsort(gc->recs, gc->nrecs, sizeof *gc->recs, v3_gen_rec_cmp);
    return 0;
}

/* how many consecutive entries share recs[lo]'s id */
static size_t v3_gen_run(const v3_gen_ctx *gc, size_t lo)
{
    size_t n = 1;
    while (lo + n < gc->nrecs && gc->recs[lo + n].id == gc->recs[lo].id)
        n++;
    return n;
}

/* the winning prefix record for `id`, or NULL. Records are sorted by
 * (id, log order), so the last entry of the id's run is the one replay would
 * have kept. */
static const v3_gen_rec *v3_gen_find(const v3_gen_ctx *gc, uint64_t id)
{
    size_t lo = 0, hi = gc->nrecs;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (gc->recs[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < gc->nrecs && gc->recs[lo].id == id)
        return &gc->recs[lo + v3_gen_run(gc, lo) - 1];
    return NULL;
}

static int v3_gen_seen_id(const v3_gen_ctx *gc, uint64_t id)
{
    size_t h;
    if (!gc->visited_cap)
        return 0;
    h = id & (gc->visited_cap - 1);
    while (gc->visited[h]) {
        if (gc->visited[h] == id)
            return 1;
        h = (h + 1) & (gc->visited_cap - 1);
    }
    return 0;
}

static int v3_gen_mark(v3_gen_ctx *gc, uint64_t id)
{
    size_t h;

    if (gc->visited_n * 2 >= gc->visited_cap) {
        size_t nc = gc->visited_cap ? gc->visited_cap * 2 : 64;
        uint64_t *nv = (uint64_t *)calloc(nc, sizeof *nv);
        size_t i;
        if (!nv) { gc->oom = 1; return 0; }
        for (i = 0; i < gc->visited_cap; i++) {
            if (gc->visited[i]) {
                size_t hh = gc->visited[i] & (nc - 1);
                while (nv[hh]) hh = (hh + 1) & (nc - 1);
                nv[hh] = gc->visited[i];
            }
        }
        free(gc->visited);
        gc->visited = nv;
        gc->visited_cap = nc;
    }
    h = id & (gc->visited_cap - 1);
    while (gc->visited[h]) h = (h + 1) & (gc->visited_cap - 1);
    gc->visited[h] = id;
    gc->visited_n++;
    return 1;
}

static int v3_gen_emit_row(v3_gen_ctx *gc, uint64_t id, const uint8_t *row,
                           uint16_t rlen)
{
    invfs_v3_inode in;
    if (!v3_gen_mark(gc, id))
        return -1;
    if (v3_ino_decode(row, rlen, &in) != 0)
        return 0;                        /* a row we cannot decode names no
                                         * recipe we could verify */
    if (in.nlink == 0)
        return 0;
    return gc->cb(gc->v, id, &in, gc->ctx);
}

static int v3_gen_emit_ref(v3_gen_ctx *gc, uint64_t id, const v3_gen_rec *r)
{
    uint8_t rb[INVFS_V3_INODE_ROW_FIXED];
    uint16_t rlen = 0;
    delta_ref ref;

    memset(&ref, 0, sizeof ref);
    ref.seg = r->seg;
    ref.off = r->off;
    ref.flags = r->flags;
    ref.vlen = r->vlen;
    if (vol_delta_read_value(gc->v, &ref, rb, sizeof rb, &rlen) != 0)
        return -1;
    return v3_gen_emit_row(gc, id, rb, rlen);
}

static int v3_gen_base_cb(void *ctx_, bt_key k, bt_val val)
{
    v3_gen_ctx *gc = (v3_gen_ctx *)ctx_;
    const v3_gen_rec *r;
    uint64_t id = 0;
    uint16_t i;

    if (k.n != 8)                          /* not an inode row: dirent (>= 10
                                         * bytes), xattr (0x03), recipe (0x04) */
        return 0;
    for (i = 0; i < 8; i++)
        id = (id << 8) | k.p[i];
    if (id < INVFS_V3_ROOT_INO)
        return 0;
    r = v3_gen_find(gc, id);
    if (r) {
        if (r->flags & INVFS_DELTA_FLAG_DELETE)
            return 0;                      /* the pinned generation deleted it */
        return v3_gen_emit_ref(gc, id, r);
    }
    return v3_gen_emit_row(gc, id, val.p, val.n);
}

/* the delta pass: pinned-prefix records for inodes the base tree never had */
static int v3_gen_delta_pass(v3_gen_ctx *gc)
{
    size_t i = 0;

    while (i < gc->nrecs) {
        size_t run = v3_gen_run(gc, i);
        const v3_gen_rec *r = &gc->recs[i + run - 1];
        if (!v3_gen_seen_id(gc, r->id) &&
            !(r->flags & INVFS_DELTA_FLAG_DELETE)) {
            if (v3_gen_emit_ref(gc, r->id, r) != 0)
                return -1;
        }
        i += run;
    }
    return 0;
}

int vol_v3_iter_inodes_at(invfs_volume *v, uint64_t root_pba,
                          uint64_t delta_end, uint64_t delta_segs,
                          uint64_t delta_head_pba,
                          int (*cb)(invfs_volume *v, uint64_t inode_id,
                                    const invfs_v3_inode *in, void *ctx),
                          void *ctx)
{
    v3_gen_ctx gc;
    invfs_blkptr root;
    uint8_t lo[8];
    int rc = 0;

    if (!v || !cb)
        return -1;
    memset(&root, 0, sizeof root);
    memset(&gc, 0, sizeof gc);
    gc.v = v;
    gc.cb = cb;
    gc.ctx = ctx;
    gc.delta_end = delta_end;
    gc.delta_segs = delta_segs;
    gc.delta_head_pba = delta_head_pba;
    gc.head_bump = (delta_segs > 1)
                 ? delta_end - (delta_segs - 1) * INVFS_DELTA_SEG_BYTES
                 : delta_end;
    if (gc.head_bump < INVFS_DELTA_SEG_HDR_LEN)
        gc.head_bump = INVFS_DELTA_SEG_HDR_LEN;

    if (v3_gen_prefix_replay(v, &gc) != 0 || gc.oom) {
        free(gc.seg_pba);
        free(gc.recs);
        return -1;
    }
    if (root_pba) {
        /* the root page's level decides the blkptr flags btree_scan wants
         * (the same pairing spt0_tree_ok builds) */
        uint8_t page[INVFS_BLOCK_SIZE];
        if (mbuf_read(v, root_pba, page) != 0) {
            free(gc.seg_pba);
            free(gc.recs);
            return -1;
        }
        mbuf_ptr_set(&root, root_pba, page,
                     mbuf_page_chdr(page)->level == INVFS_PAGE_LEVEL_LEAF
                     ? INVFS_BP_ROOT | INVFS_BP_LEAF
                     : INVFS_BP_ROOT | INVFS_BP_INTERNAL);
    }
    v3_ino_key(INVFS_V3_ROOT_INO, lo);
    if (root_pba)
        rc = btree_scan(v, root, (bt_key){lo, 8}, (bt_key){NULL, 0},
                        v3_gen_base_cb, &gc);
    if (rc == 0)
        rc = v3_gen_delta_pass(&gc);
    if (rc != 0 || gc.oom)
        rc = -1;
    free(gc.seg_pba);
    free(gc.recs);
    free(gc.visited);
    return rc;
}
