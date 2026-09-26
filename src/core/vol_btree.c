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
 * TODO(WP-M15): the WP-M3 task blurb says upsert "frees old pages via the
 * metabuf allocator"; this implementation deliberately does NOT, because with
 * COW a retired page can still be reachable from a retained root and the API
 * carries no refcount/pin. Freeing is therefore the reachability diff
 * btree_reclaim provides, scheduled by WP-M15 against {current base, pinned
 * save-point root} (design section 8, D4).
 */

#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_metabuf.h"
#include "vol_delta.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

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

/* Result of a recursive insertion. */
typedef struct {
    invfs_blkptr node;         /* new subtree root */
    invfs_blkptr right;        /* new right sibling when split != 0 */
    int          split;
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

static int bt_cmp(const uint8_t *a, uint16_t an,
                  const uint8_t *b, uint16_t bn)
{
    uint16_t m = an < bn ? an : bn;
    int c = m ? memcmp(a, b, m) : 0;
    if (c)
        return c < 0 ? -1 : 1;
    if (an < bn)
        return -1;
    if (an > bn)
        return 1;
    return 0;
}

static int bt_cmp_key(const bt_ent *e, bt_key key)
{
    return bt_cmp(e->k, e->klen, key.p, key.n);
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

    pba = mbuf_alloc(v, gen);
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
    int n, level, s;

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
            out->split = 0;
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
        if (bt_cmp(key.p, key.n, e[i].k, e[i].klen) < 0) {
            e[i].k = key.p;
            e[i].klen = key.n;
        }
        if (!cu.split) {
            e[i].child = cu.node;
            if (bt_used(level, e, n) <= INVFS_BLOCK_SIZE) {
                if (bt_write(v, level, gen, e, n, &out->node) != 0) {
                    free(e);
                    return -1;
                }
                out->split = 0;
                free(e);
                return 0;
            }
        } else {
            uint8_t rkbuf[INVFS_BLOCK_SIZE];
            bt_key rk;
            int j;
            if (bt_first_key(v, cu.right, rkbuf, &rk) != 0) {
                free(e);
                return -1;
            }
            e[i].child = cu.node;
            if (n >= BT_MAX_ENTRIES) {
                free(e);
                return -1;
            }
            for (j = n; j > i + 1; j--)
                e[j] = e[j - 1];
            e[i + 1].k = rk.p;
            e[i + 1].klen = rk.n;
            e[i + 1].child = cu.right;
            n++;
            if (bt_used(level, e, n) <= INVFS_BLOCK_SIZE) {
                if (bt_write(v, level, gen, e, n, &out->node) != 0) {
                    free(e);
                    return -1;
                }
                out->split = 0;
                free(e);
                return 0;
            }
        }
        /* fall through: the internal page now overflows */
    }

    s = bt_split_point(level, e, n);
    if (s <= 0 || s >= n) {
        free(e);
        return -1;
    }
    if (bt_write(v, level, gen, e, s, &out->node) != 0) {
        free(e);
        return -1;
    }
    if (bt_write(v, level, gen, e + s, n - s, &out->right) != 0) {
        free(e);
        return -1;
    }
    out->split = 1;
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

    if (!up.split) {
        up.node.flags |= INVFS_BP_ROOT;
        *new_root_out = up.node;
        return 0;
    }

    {
        uint8_t k1[INVFS_BLOCK_SIZE], k2[INVFS_BLOCK_SIZE];
        bt_key m1, m2;
        bt_ent re[2];
        invfs_blkptr r;
        if (bt_first_key(v, up.node, k1, &m1) != 0 ||
            bt_first_key(v, up.right, k2, &m2) != 0)
            return -1;
        re[0].k = m1.p;
        re[0].klen = m1.n;
        re[0].child = up.node;
        re[1].k = m2.p;
        re[1].klen = m2.n;
        re[1].child = up.right;
        if (bt_write(v, up.level + 1, gen, re, 2, &r) != 0)
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
            } else if (bt_cmp(key.p, key.n, oldmin.p, oldmin.n) == 0) {
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
            if (lo.n && bt_cmp(k.p, k.n, lo.p, lo.n) < 0)
                continue;
            if (hi.n && bt_cmp(k.p, k.n, hi.p, hi.n) >= 0)
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
        if (hi.n && bt_cmp(clo.p, clo.n, hi.p, hi.n) >= 0)
            break;
        if (i + 1 < n) {
            if (lo.n && bt_cmp(e[i + 1].k, e[i + 1].klen, lo.p, lo.n) <= 0)
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
            if (i && bt_cmp(e[i - 1].k, e[i - 1].klen, e[i].k, e[i].klen) >= 0) {
                bt_ck_err(ck, "leaf keys not strictly ordered");
                free(e);
                return -1;
            }
            if (lo.n && bt_cmp(e[i].k, e[i].klen, lo.p, lo.n) < 0) {
                bt_ck_err(ck, "leaf key below parent bound");
                free(e);
                return -1;
            }
            if (hi.n && bt_cmp(e[i].k, e[i].klen, hi.p, hi.n) >= 0) {
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
        if (i && bt_cmp(e[i - 1].k, e[i - 1].klen, e[i].k, e[i].klen) >= 0) {
            bt_ck_err(ck, "separators not strictly ordered");
            free(e);
            return -1;
        }
        if (lo.n && bt_cmp(e[i].k, e[i].klen, lo.p, lo.n) < 0) {
            bt_ck_err(ck, "separator below parent bound");
            free(e);
            return -1;
        }
        if (hi.n && bt_cmp(e[i].k, e[i].klen, hi.p, hi.n) >= 0) {
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
            if (i && bt_cmp(e[i - 1].k, e[i - 1].klen, e[i].k, e[i].klen) >= 0) {
                bt_ck_err(ck, "leaf keys not strictly ordered");
                free(e);
                return -1;
            }
            if (lo.n && bt_cmp(e[i].k, e[i].klen, lo.p, lo.n) < 0) {
                bt_ck_err(ck, "leaf key below parent bound");
                free(e);
                return -1;
            }
            if (hi.n && bt_cmp(e[i].k, e[i].klen, hi.p, hi.n) >= 0) {
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
        if (i && bt_cmp(e[i - 1].k, e[i - 1].klen, e[i].k, e[i].klen) >= 0) {
            bt_ck_err(ck, "separators not strictly ordered");
            free(e);
            return -1;
        }
        if (lo.n && bt_cmp(e[i].k, e[i].klen, lo.p, lo.n) < 0) {
            bt_ck_err(ck, "separator below parent bound");
            free(e);
            return -1;
        }
        if (hi.n && bt_cmp(e[i].k, e[i].klen, hi.p, hi.n) >= 0) {
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
    if (r->lo_n && bt_cmp(clo.p, clo.n, r->lo, r->lo_n) < 0)
        return 0;
    if (r->hi_n) {
        if (!chi.n)
            return 0;
        if (bt_cmp(chi.p, chi.n, r->hi, r->hi_n) > 0)
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
        invfs_blkptr root;
        bt_val val;
        int found = 0;
        if (v3_base_root(v, &root) != 0)
            return -1;
        if (btree_search(v, root, (bt_key){key, klen}, &val, &found) != 0)
            return -1;
        return found;
    }
}

/* Overlay point value: 1 = present (value copied to buf, *vlen_out set),
 * 0 = absent (delta delete or neither tier), -1 = error/too small. */
static int v3_overlay_get_key(invfs_volume *v, const uint8_t *key, uint16_t klen,
                              uint8_t *buf, size_t cap, uint16_t *vlen_out)
{
    delta_ref dr;
    int drc = v3_overlay_lookup(v, key, klen, &dr);
    if (drc < 0)
        return -1;
    if (drc == 1) {
        if (dr.flags & INVFS_DELTA_FLAG_DELETE)
            return 0;
        if (dr.vlen > cap)
            return -1;
        if (vlen_out)
            *vlen_out = 0;
        if (vol_delta_read_value(v, &dr, buf, cap, vlen_out) != 0)
            return -1;
        return 1;
    }
    {
        invfs_blkptr root;
        bt_val val;
        int found = 0;
        if (v3_base_root(v, &root) != 0)
            return -1;
        if (btree_search(v, root, (bt_key){key, klen}, &val, &found) != 0)
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

    if (!v || !name)
        return -1;
    if (v->sb.vol_flags & VOLF_READONLY)
        return -1;
    nl = strlen(name);
    if (nl == 0 || nl > INVFS_MAX_NAME)
        return -1;
    if (v3_ready(v) != 0)
        return -1;

    kn = v3_xattr_key(kb, inode_id, name, nl);
    ex = v3_overlay_exists(v, kb, kn);
    if (ex < 0)
        return -1;
    if (ex == 0)
        return -1;                       /* ENODATA */
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

int vol_v3_xattr_get(invfs_volume *v, uint64_t inode_id, const char *name,
                     void *val, size_t *vlen)
{
    uint8_t kb[V3_XATTR_FIXED + INVFS_MAX_NAME + V3_XATTR_CHUNK_EXTRA];
    uint8_t one[V3_XATTR_CHUNK_DATA + 1];
    uint8_t *acc = NULL;
    size_t nl, total = 0, cap = 0;
    uint16_t kn, idx;

    if (!v || !name || !vlen)
        return -1;
    nl = strlen(name);
    if (nl == 0 || nl > INVFS_MAX_NAME)
        return -1;
    if (v3_ready(v) != 0)
        return -1;

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
        rc = v3_overlay_get_key(v, kb, kn, one, sizeof one, &got);
        if (rc < 0) {
            free(acc);
            return -1;
        }
        if (rc == 0) {
            if (idx == 0) {              /* ENODATA */
                free(acc);
                return -1;
            }
            break;                       /* end of the chunk chain */
        }
        add = got;
        if (total + add > V3_XATTR_MAX_TOTAL) {
            free(acc);
            return -1;
        }
        if (total + add > cap) {
            size_t ncap = cap ? cap * 2 : 1024;
            uint8_t *na;
            while (ncap < total + add)
                ncap *= 2;
            na = (uint8_t *)realloc(acc, ncap);
            if (!na) { free(acc); return -1; }
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
        return -2;                       /* ERANGE */
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
    if (v3_base_root(v, &root) != 0) {
        v3_xa_free(&l);
        return -1;
    }
    rc = btree_scan(v, root, (bt_key){lo, V3_XATTR_FIXED},
                    (bt_key){hi, V3_XATTR_FIXED}, v3_xa_base_cb, &l);
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

    if (v3_base_root(v, &root) != 0) {
        free(b.buf);
        return -1;
    }
    rc = btree_scan(v, root, (bt_key){lo, V3_XATTR_FIXED},
                    (bt_key){hi, V3_XATTR_FIXED}, v3_xattr_collect_cb, &b);
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
    invfs_blkptr root;
    bt_val val;
    int found = 0;

    if (!v || !out)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    v3_ino_key(inode_id, kb);

    /* WP-M11: delta first -- a delta row (or delete) shadows the base. */
    {
        delta_ref dr;
        int drc = v3_overlay_lookup(v, kb, sizeof kb, &dr);
        uint8_t rb[INVFS_V3_INODE_ROW_FIXED];
        uint16_t rlen = 0;
        if (drc < 0)
            return -1;
        if (drc == 1) {
            if (dr.flags & INVFS_DELTA_FLAG_DELETE)
                return 0;                /* hidden by a delta delete */
            /* The delta value is the frozen fixed row. TODO(WP-M12): if a
             * later writer inlines xattr bytes (xattr_len > 0) the value can
             * exceed INVFS_V3_INODE_ROW_FIXED; size the buffer from dr.vlen
             * then. Today only the fixed row can appear. */
            if (vol_delta_read_value(v, &dr, rb, sizeof rb, &rlen) != 0)
                return -1;
            if (v3_ino_decode(rb, rlen, out) != 0)
                return -1;
            return 1;
        }
    }

    if (v3_base_root(v, &root) != 0)
        return -1;
    if (btree_search(v, root, (bt_key){kb, 8}, &val, &found) != 0)
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
    uint8_t kb[8];
    uint8_t vb[INVFS_V3_INODE_ROW_FIXED];
    uint16_t vl;

    if (!v || !in || in->nlink == 0)
        return -1;                        /* a zero-nlink row is deleted */
    if (v3_ready(v) != 0)
        return -1;
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

int vol_v3_recipe_load(invfs_volume *v, const uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN],
                       uint8_t **blob_out, size_t *blen_out)
{
    uint8_t kb[V3_RECIPE_KEY_LEN], chk[INVFS_V3_RECIPE_ADDR_LEN];
    invfs_blkptr root;
    bt_val val;
    uint8_t *blob;
    int found = 0;

    if (!v || !addr || !blob_out || !blen_out)
        return -1;
    *blob_out = NULL;
    *blen_out = 0;
    if (v3_ready(v) != 0)
        return -1;
    if (v3_rcache_get(v, addr, blob_out, blen_out) == 0)
        return 0;
    v3_recipe_key(kb, addr);

    /* WP-M11: the delta owns the key if it was rewritten since the fold;
     * a delete shadows the base blob. The BLAKE3 check below still governs
     * whatever bytes the delta returns (content-addressing is immutable). */
    {
        delta_ref dr;
        int drc = v3_overlay_lookup(v, kb, V3_RECIPE_KEY_LEN, &dr);
        if (drc < 0)
            return -1;
        if (drc == 1) {
            if (dr.flags & INVFS_DELTA_FLAG_DELETE)
                return -1;               /* hidden by a delta delete */
            blob = (uint8_t *)malloc(dr.vlen ? dr.vlen : 1);
            if (!blob)
                return -1;
            {
                uint16_t got = 0;
                if (vol_delta_read_value(v, &dr, blob, dr.vlen, &got) != 0 ||
                    got != dr.vlen) {
                    free(blob);
                    return -1;
                }
            }
            v3_blake3(blob, dr.vlen, chk);
            if (memcmp(chk, addr, INVFS_V3_RECIPE_ADDR_LEN) != 0) {
                fprintf(stderr, "v3 recipe blob (delta): BLAKE3 mismatch "
                        "(corrupt or forged); refusing the read\n");
                free(blob);
                return -1;
            }
            *blob_out = blob;
            *blen_out = dr.vlen;
            v3_rcache_put(v, addr, blob, dr.vlen);
            return 0;
        }
    }

    if (v3_base_root(v, &root) != 0)
        return -1;
    if (btree_search(v, root, (bt_key){kb, V3_RECIPE_KEY_LEN}, &val,
                     &found) != 0)
        return -1;
    if (!found)
        return -1;

    /* WP-M25: check if this is an RMC1 multi-chunk descriptor */
    if (val.n == sizeof(invfs_v3_recipe_desc)) {
        invfs_v3_recipe_desc desc;
        memcpy(&desc, val.p, sizeof(desc));
        if (desc.magic == INVFS_V3_RECIPE_MAGIC_RMC1) {
            uint32_t total_len = desc.total_len;
            uint16_t n_chunks = desc.n_chunks;
            if (total_len == 0 || total_len > INVFS_V3_RECIPE_STREAM_MAX)
                return -1;
            blob = (uint8_t *)malloc(total_len);
            if (!blob)
                return -1;
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
                    return -1;
                }
                memcpy(blob + off, cval.p, exp_len);
            }
            v3_blake3(blob, total_len, chk);
            if (memcmp(chk, addr, INVFS_V3_RECIPE_ADDR_LEN) != 0) {
                fprintf(stderr, "v3 recipe blob %p: BLAKE3 mismatch (corrupt or "
                        "forged); refusing the read\n", (const void *)addr);
                free(blob);
                return -1;
            }
            *blob_out = blob;
            *blen_out = total_len;
            v3_rcache_put(v, addr, blob, total_len);
            return 0;
        }
    }

    /* btree_search's value points into a per-thread buffer valid only until
     * the next search: copy it out before doing anything else. */
    blob = (uint8_t *)malloc(val.n ? val.n : 1);
    if (!blob)
        return -1;
    if (val.n)
        memcpy(blob, val.p, val.n);
    v3_blake3(blob, val.n, chk);
    if (memcmp(chk, addr, INVFS_V3_RECIPE_ADDR_LEN) != 0) {
        fprintf(stderr, "v3 recipe blob %p: BLAKE3 mismatch (corrupt or "
                "forged); refusing the read\n", (const void *)addr);
        free(blob);
        return -1;
    }
    *blob_out = blob;
    *blen_out = val.n;
    v3_rcache_put(v, addr, blob, val.n);
    return 0;
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
    invfs_blkptr root;
    bt_val val;
    uint16_t kn;
    size_t nlen = name ? strlen(name) : 0;
    int found = 0;

    if (!v || nlen > INVFS_MAX_NAME)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    kn = v3_dirent_key(kb, parent, name, nlen);

    /* WP-M11: delta first; a delete shadows the base dirent. */
    {
        delta_ref dr;
        int drc = v3_overlay_lookup(v, kb, kn, &dr);
        if (drc < 0)
            return -1;
        if (drc == 1) {
            uint8_t vb[8];
            uint16_t got = 0;
            if (dr.flags & INVFS_DELTA_FLAG_DELETE)
                return 0;
            if (dr.vlen != 8 ||
                vol_delta_read_value(v, &dr, vb, sizeof vb, &got) != 0 ||
                got != 8)
                return -1;
            if (child_out)
                *child_out = v3_dirent_val_get(vb, got);
            return 1;
        }
    }

    if (v3_base_root(v, &root) != 0)
        return -1;
    if (btree_search(v, root, (bt_key){kb, kn}, &val, &found) != 0)
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
           bt_cmp(m->ent[m->i].key, m->ent[m->i].klen, k.p, k.n) < 0) {
        int rc = v3_merge_emit(m, &m->ent[m->i]);
        m->i++;
        if (rc)
            return rc;
    }
    if (m->i < m->n &&
        bt_cmp(m->ent[m->i].key, m->ent[m->i].klen, k.p, k.n) == 0) {
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

    if (v3_base_root(v, &root) != 0) {
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
    if (v->next_inode_id <= INVFS_V3_ROOT_INO) {
        invfs_blkptr root;
        uint64_t max = 0;
        if (v3_ready(v) != 0)
            return 0;
        if (v3_base_root(v, &root) != 0)
            return 0;
        if (btree_scan(v, root, (bt_key){NULL, 0}, (bt_key){NULL, 0},
                       v3_max_inode_cb, &max) != 0)
            return 0;
        /* WP-M12: a create since the last fold is delta-only, so the base
         * scan alone would miss it. Unbounded range; only 8-byte keys count. */
        if (vol_delta_range(v, NULL, 0, NULL, 0,
                            v3_max_inode_delta_cb, &max) != 0)
            return 0;
        v->next_inode_id = max + 1;
        if (v->next_inode_id <= INVFS_V3_ROOT_INO)
            v->next_inode_id = INVFS_V3_ROOT_INO + 1;
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

    (void)vol_v3_walk(v, v3_name_of_walk_cb, &c);
    return c.found ? 1 : 0;
}

/* WP-M21b: compose the full "dir/sub/file" path of an inode by walking
 * parent inodes up to the root (vol_v3_name_of gives one leaf + its
 * parent per hop). v2 record names are full relative paths and every
 * name-keyed consumer (vol_stat/vol_find/create_blob_file, the sweep
 * collector's dedupe table) expects that shape, while the v3 dirent
 * tree only stores leaves. Returns 1 composed, 0 not found, -1 error
 * (too deep / cyclic / buffer too small). Each hop is a full reverse
 * dirent walk, so this is O(depth x live) -- fine for the offline
 * tools, and the sweep collector's per-file vol_stat is the same order.
 * A dirent-index (id -> path) would be the v3-native optimization. */
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

typedef struct {
    invfs_volume *v;
    int (*cb)(invfs_volume *v, uint64_t inode_id, const char *name, void *ctx);
    void *ctx;
    uint64_t *visited;
    size_t visited_cap;
    size_t visited_n;
    int oom;
} v3_iter_ctx;

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

    /* resolve name via dirent lookup */
    {
        char name[INVFS_MAX_NAME + 1];
        uint64_t parent;
        int got;

        name[0] = 0;
        got = vol_v3_name_of(ic->v, inode_id, name, sizeof name, &parent);
        (void)parent;

        return ic->cb(ic->v, inode_id, got > 0 ? name : NULL, ic->ctx);
    }
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
    char name[INVFS_MAX_NAME + 1];
    uint64_t parent;
    int got, i;

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

    name[0] = 0;
    got = vol_v3_name_of(ic->v, inode_id, name, sizeof name, &parent);
    (void)parent;
    return ic->cb(ic->v, inode_id, got > 0 ? name : NULL, ic->ctx);
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

    rc = btree_scan(v, root, (bt_key){lo, 8}, (bt_key){hi, 8},
                    v3_iter_base_cb, &ic);
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
    free(ic.visited);
    return rc;
}
