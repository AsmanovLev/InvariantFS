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
static int v3_bitmap_flush(invfs_volume *v)
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
    if (v3_bitmap_flush(v) != 0)
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
    if (v3_base_root(v, &root) != 0)
        return -1;
    v3_ino_key(inode_id, kb);
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
    invfs_blkptr root, nr;
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
    if (btree_delete(v, root, (bt_key){kb, 8}, &nr) != 0)
        return -1;
    return v3_publish(v, nr, old_gen);
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
    if (v3_base_root(v, &root) != 0)
        return -1;
    kn = v3_dirent_key(kb, parent, name, nlen);
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

/* Scan state shared with the btree_scan callback. */
typedef struct {
    vol_v3_dirent_cb cb;
    void             *ctx;
} v3_dirent_scan_state;

static int v3_dirent_scan_cb(void *ctx_, bt_key k, bt_val val)
{
    v3_dirent_scan_state *s = (v3_dirent_scan_state *)ctx_;
    char name[INVFS_MAX_NAME + 1];
    uint16_t nlen;

    if (k.n < V3_DIRENT_KEY_FIXED)
        return 0;
    if (v3_dirent_key_len(k.p) != k.n)
        return 0;   /* malformed: not one of our keys */
    nlen = (uint16_t)(((uint16_t)k.p[8] << 8) | k.p[9]);
    if (nlen == 0 || nlen > INVFS_MAX_NAME)
        return 0;   /* anchor (or malformed) */
    memcpy(name, k.p + V3_DIRENT_KEY_FIXED, nlen);
    name[nlen] = 0;
    return s->cb(s->ctx, name, nlen, v3_dirent_val_get(val.p, val.n));
}

int vol_v3_dirent_scan(invfs_volume *v, uint64_t parent,
                       vol_v3_dirent_cb cb, void *ctx)
{
    uint8_t lo[V3_DIRENT_KEY_FIXED], hi[V3_DIRENT_KEY_FIXED];
    invfs_blkptr root;
    v3_dirent_scan_state s;

    if (!v || !cb)
        return -1;
    if (v3_ready(v) != 0)
        return -1;
    if (v3_base_root(v, &root) != 0)
        return -1;
    /* [parent||0x0000, (parent+1)||0x0000): the anchor first, then every
     * child, all under one contiguous parent prefix. */
    v3_dirent_key(lo, parent, NULL, 0);
    v3_dirent_key(hi, parent + 1, NULL, 0);
    s.cb = cb;
    s.ctx = ctx;
    return btree_scan(v, root, (bt_key){lo, V3_DIRENT_KEY_FIXED},
                      (bt_key){hi, V3_DIRENT_KEY_FIXED},
                      v3_dirent_scan_cb, &s);
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
        v->next_inode_id = max + 1;
        if (v->next_inode_id <= INVFS_V3_ROOT_INO)
            v->next_inode_id = INVFS_V3_ROOT_INO + 1;
    }
    return v->next_inode_id++;
}
