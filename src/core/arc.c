/*
 * arc.c — Adaptive Replacement Cache, byte-accounted.
 *
 * The algorithm is Megiddo & Modha's ARC with one deliberate departure: the
 * paper caches fixed-size pages, and this caches whole reconstructed files
 * whose sizes span four orders of magnitude (a 200-byte text file and a 300 MB
 * tar in the same cache). So every list is measured in BYTES, and the
 * invariants become
 *
 *     |T1| + |T2|                     <= c
 *     |T1| + |B1|                     <= c
 *     |T1| + |T2| + |B1| + |B2|       <= 2c
 *
 * with |.| meaning bytes. REPLACE evicts in a loop rather than once, because
 * making room for one 40 MB entry can cost many small ones.
 *
 * The adaptation of p is the one place where "bytes" has no literal reading in
 * the paper: there, a ghost hit moves p by max(|B2|/|B1|, 1) *pages*. Here the
 * ratio is taken on entry COUNTS -- which is what the paper's ratio actually
 * measures, how lopsided the two ghost histories are -- and the step is scaled
 * by the size of the entry that caused it, which is the natural byte analogue:
 * a ghost hit on a 40 MB file is stronger evidence about how 40 MB should be
 * spent than a hit on a 4 KB one. Directionally it is the paper: a B1 hit
 * argues recency needs more room, a B2 hit argues frequency does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "arc.h"

/* which list a node is in */
enum { L_NONE = 0, L_T1, L_T2, L_B1, L_B2 };

typedef struct arc_node {
    uint64_t key;
    uint8_t *data;            /* NULL for a ghost */
    size_t   len;             /* kept for ghosts too: it is what the ghost is
                                 evidence about, and REPLACE needs it back */
    int      list;
    struct arc_node *prev, *next;   /* list links, MRU at head */
    struct arc_node *hnext;         /* hash chain */
} arc_node;

typedef struct {
    arc_node *head, *tail;    /* head = MRU */
    size_t    bytes;
    uint32_t  count;
} arc_list;

struct invfs_arc {
    /* WP-arc-concurrent-safe. Everything below is mutable and none of it was
     * atomic, so this mutex is what makes the cache usable from the lock-free
     * read path (fuse_fs.c calls vol_read_range with g_io_lock released, and
     * the daemon is fuse_loop_mt). It is a strict LEAF: no arc_* function takes
     * any other lock and none calls out of this file, so the only order a
     * caller can build is g_io_lock -> mu. It cannot invert against the four
     * subsystems that already lock (vol_delta.c, vol_plugin_client.c,
     * vol_btree.c, volume.c): those are taken inside vol_* calls, which sit
     * ABOVE this one. And g_io_lock is static to fuse_fs.c, so no core file
     * can take it at all.
     *
     * The lock makes each entry atomic. It does NOT make a pointer handed out
     * by arc_get survive the next call -- hence arc_get_copy, which copies
     * under the lock. See arc.h. */
    pthread_mutex_t mu;
    arc_list t1, t2, b1, b2;
    size_t   c;               /* budget, bytes */
    size_t   p;               /* adaptive target for T1, bytes */
    arc_node **buckets;
    size_t   nbuckets;        /* power of two */
    size_t   nnodes;          /* live + ghost, for load factor */
    invfs_arc_stats st;
};

/* ---- hash table (open chaining, power-of-two, grows at load 0.75) ---- */

static size_t arc_hash(uint64_t key, size_t nbuckets)
{
    /* splitmix64 finalizer: inode ids are dense and sequential, so the low
       bits alone would put every id in a handful of buckets */
    uint64_t x = key + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return (size_t)(x & (uint64_t)(nbuckets - 1));
}

static arc_node *ht_find(invfs_arc *a, uint64_t key)
{
    arc_node *n = a->buckets[arc_hash(key, a->nbuckets)];
    while (n && n->key != key) n = n->hnext;
    return n;
}

static void ht_insert(invfs_arc *a, arc_node *n)
{
    size_t h = arc_hash(n->key, a->nbuckets);
    n->hnext = a->buckets[h];
    a->buckets[h] = n;
    a->nnodes++;
}

static void ht_remove(invfs_arc *a, arc_node *n)
{
    size_t h = arc_hash(n->key, a->nbuckets);
    arc_node **pp = &a->buckets[h];
    while (*pp && *pp != n) pp = &(*pp)->hnext;
    if (*pp) { *pp = n->hnext; a->nnodes--; }
}

static void ht_grow(invfs_arc *a)
{
    size_t nb = a->nbuckets * 2, i;
    arc_node **nbk = (arc_node **)calloc(nb, sizeof *nbk);
    if (!nbk) return;                 /* stay at the old size; chains lengthen */
    for (i = 0; i < a->nbuckets; i++) {
        arc_node *n = a->buckets[i];
        while (n) {
            arc_node *next = n->hnext;
            size_t h = arc_hash(n->key, nb);
            n->hnext = nbk[h];
            nbk[h] = n;
            n = next;
        }
    }
    free(a->buckets);
    a->buckets = nbk;
    a->nbuckets = nb;
}

/* ---- list operations, MRU at head ---- */

static void lst_unlink(arc_list *l, arc_node *n)
{
    if (n->prev) n->prev->next = n->next; else l->head = n->next;
    if (n->next) n->next->prev = n->prev; else l->tail = n->prev;
    n->prev = n->next = NULL;
    l->bytes -= n->len;
    l->count--;
}

static void lst_push_mru(arc_list *l, arc_node *n)
{
    n->prev = NULL;
    n->next = l->head;
    if (l->head) l->head->prev = n;
    l->head = n;
    if (!l->tail) l->tail = n;
    l->bytes += n->len;
    l->count++;
}

static arc_list *list_of(invfs_arc *a, int which)
{
    switch (which) {
        case L_T1: return &a->t1;
        case L_T2: return &a->t2;
        case L_B1: return &a->b1;
        case L_B2: return &a->b2;
        default:   return NULL;
    }
}

static void move_to(invfs_arc *a, arc_node *n, int which)
{
    arc_list *from = list_of(a, n->list);
    arc_list *to   = list_of(a, which);
    if (from) lst_unlink(from, n);
    n->list = which;
    if (to) lst_push_mru(to, n);
}

static void node_free(invfs_arc *a, arc_node *n)
{
    arc_list *l = list_of(a, n->list);
    if (l) lst_unlink(l, n);
    ht_remove(a, n);
    free(n->data);
    free(n);
}

/* ---- core ---- */

invfs_arc *arc_create(size_t budget_bytes)
{
    invfs_arc *a;
    if (budget_bytes == 0) return NULL;
    a = (invfs_arc *)calloc(1, sizeof *a);
    if (!a) return NULL;
    if (pthread_mutex_init(&a->mu, NULL) != 0) { free(a); return NULL; }
    a->nbuckets = 256;
    a->buckets = (arc_node **)calloc(a->nbuckets, sizeof *a->buckets);
    if (!a->buckets) { pthread_mutex_destroy(&a->mu); free(a); return NULL; }
    a->c = budget_bytes;
    a->p = budget_bytes / 2;   /* start even; the ghosts will move it */
    a->st.budget = budget_bytes;
    return a;
}

void arc_destroy(invfs_arc *a)
{
    size_t i;
    if (!a) return;
    /* Teardown, not a mutation. The volume calls this from vol_close, after
       every reader has gone; nothing reads a half-destroyed cache. */
    for (i = 0; i < a->nbuckets; i++) {
        arc_node *n = a->buckets[i];
        while (n) { arc_node *next = n->hnext; free(n->data); free(n); n = next; }
    }
    free(a->buckets);
    pthread_mutex_destroy(&a->mu);
    free(a);
}

int arc_get(invfs_arc *a, uint64_t key, const uint8_t **data, size_t *len)
{
    arc_node *n;
    int hit;
    if (!a) return 0;
    pthread_mutex_lock(&a->mu);
    n = ht_find(a, key);
    if (!n || !n->data) {          /* absent, or a ghost -- both are misses */
        a->st.misses++;
        if (n) a->st.ghost_hits++;  /* ...but a ghost hit is worth counting:
                                       it is exactly the miss ARC learns from */
        hit = 0;
    } else {
        /* Case I: a real hit. Seen twice or more now, so it belongs in T2
           regardless of which list it was in. */
        move_to(a, n, L_T2);
        a->st.hits++;
        *data = n->data;
        *len = n->len;
        hit = 1;
    }
    pthread_mutex_unlock(&a->mu);
    return hit;
}

/* arc_get_copy: the form every caller that can run concurrently must use.
   Identical to arc_get up to the hit, and then it COPIES instead of handing
   the pointer out -- while still holding the lock, so no other thread's
   arc_replace can free the buffer between the find and the memcpy. That
   window was the bug: arc.h used to promise a borrowed pointer "valid until
   the next arc_put", and the read path acted on it (vol_read.c:1966).
   It is not an extra copy: every read-path caller was already memcpy'ing out
   of a borrowed pointer immediately afterwards. */
int arc_get_copy(invfs_arc *a, uint64_t key, size_t off, size_t want,
                 uint8_t *dst, size_t *got)
{
    arc_node *n;
    int hit;

    if (got) *got = 0;
    if (!a) return 0;
    pthread_mutex_lock(&a->mu);
    n = ht_find(a, key);
    if (!n || !n->data) {          /* absent, or a ghost -- both are misses */
        a->st.misses++;
        if (n) a->st.ghost_hits++;
        hit = 0;
    } else {
        move_to(a, n, L_T2);
        a->st.hits++;
        /* a hit with nothing left to copy is still a hit: the caller
           distinguishes "miss" (0) from "short" (*got < want) itself */
        if (off < n->len) {
            size_t avail = n->len - off;
            if (avail > want) avail = want;
            if (dst && avail) memcpy(dst, n->data + off, avail);
            if (got) *got = avail;
        }
        hit = 1;
    }
    pthread_mutex_unlock(&a->mu);
    return hit;
}

/* Free room for `need` bytes by demoting LRU entries to their ghost lists.
   `from_b2` reproduces the paper's tie-break: an insert triggered by a B2 hit
   prefers to evict from T1 even when |T1| is exactly at target. */
static void arc_replace(invfs_arc *a, size_t need, int from_b2)
{
    while (a->t1.bytes + a->t2.bytes + need > a->c) {
        arc_node *victim;
        if (a->t1.count > 0 &&
            (a->t1.bytes > a->p || (from_b2 && a->t1.bytes == a->p))) {
            victim = a->t1.tail;
            free(victim->data); victim->data = NULL;
            move_to(a, victim, L_B1);
        } else if (a->t2.count > 0) {
            victim = a->t2.tail;
            free(victim->data); victim->data = NULL;
            move_to(a, victim, L_B2);
        } else if (a->t1.count > 0) {
            /* T2 empty and T1 within target, but the room is still needed */
            victim = a->t1.tail;
            free(victim->data); victim->data = NULL;
            move_to(a, victim, L_B1);
        } else {
            break;                 /* nothing live left to give */
        }
        a->st.evictions++;
    }
}

/* Keep the ghost histories bounded: |T1|+|B1| <= c and the whole directory
   <= 2c. Ghosts cost only a node, but an unbounded B1 would remember every
   file a scan ever touched. */
static void arc_trim_ghosts(invfs_arc *a)
{
    while (a->b1.count > 0 && a->t1.bytes + a->b1.bytes > a->c)
        node_free(a, a->b1.tail);
    while (a->b2.count > 0 &&
           a->t1.bytes + a->t2.bytes + a->b1.bytes + a->b2.bytes > 2 * a->c)
        node_free(a, a->b2.tail);
}

void arc_put(invfs_arc *a, uint64_t key, uint8_t *data, size_t len)
{
    arc_node *n;
    int from_b2 = 0;

    if (!a) { free(data); return; }

    pthread_mutex_lock(&a->mu);

    /* An entry over half the budget would evict nearly everything to get in
       and then be evicted itself by the next insert -- pure thrash, and the
       reconstruction it displaces is as expensive as its own. Refuse it and
       say so; raising INVFS_ARC_BYTES is the answer, not evicting the world. */
    if (len * 2 > a->c) {
        a->st.refused++;
        pthread_mutex_unlock(&a->mu);
        free(data);
        return;
    }

    n = ht_find(a, key);
    if (n && n->data) {
        /* Already live: refresh in place. Take the entry off its list first,
           then buy room for the new size exactly as a fresh insert would --
           a refresh that GROWS the entry has to pay for the growth, or
           |T1|+|T2| <= c quietly stops holding. (Patching the list's byte
           count in place, which is what this used to do, let three entries
           that exactly filled the budget become 110% of it.) Unlinked first
           so arc_replace cannot pick this node as its own victim. */
        move_to(a, n, L_NONE);
        free(n->data);
        n->data = NULL;
        arc_replace(a, len, 0);
        n->len = len;
        n->data = data;
        move_to(a, n, L_T2);        /* a repeat sighting belongs in T2 */
        arc_trim_ghosts(a);
        pthread_mutex_unlock(&a->mu);
        return;
    }

    if (n) {
        /* Case II / III: a ghost hit. This is the whole point of the ghost
           lists -- the miss tells us which half of the cache was too small. */
        size_t step;
        if (n->list == L_B1) {
            uint32_t r = (a->b1.count && a->b2.count > a->b1.count)
                         ? a->b2.count / a->b1.count : 1;
            step = len * (size_t)r;
            a->p = (a->p + step > a->c) ? a->c : a->p + step;
        } else {
            uint32_t r = (a->b2.count && a->b1.count > a->b2.count)
                         ? a->b1.count / a->b2.count : 1;
            step = len * (size_t)r;
            a->p = (a->p > step) ? a->p - step : 0;
            from_b2 = 1;
        }
        arc_replace(a, len, from_b2);
        /* the ghost's recorded size is being replaced by the real one */
        move_to(a, n, L_NONE);
        n->len = len;
        n->data = data;
        move_to(a, n, L_T2);        /* a second sighting: straight to T2 */
        a->st.inserts++;
        arc_trim_ghosts(a);
        pthread_mutex_unlock(&a->mu);
        return;
    }

    /* Case IV: never seen. */
    arc_replace(a, len, 0);
    if (a->nnodes + 1 > a->nbuckets - (a->nbuckets >> 2)) ht_grow(a);
    n = (arc_node *)calloc(1, sizeof *n);
    if (!n) { pthread_mutex_unlock(&a->mu); free(data); return; }
    n->key = key;
    n->len = len;
    n->data = data;
    n->list = L_NONE;
    ht_insert(a, n);
    move_to(a, n, L_T1);            /* first sighting: T1 */
    a->st.inserts++;
    arc_trim_ghosts(a);
    pthread_mutex_unlock(&a->mu);
}

void arc_invalidate(invfs_arc *a, uint64_t key)
{
    arc_node *n;
    if (!a) return;
    pthread_mutex_lock(&a->mu);
    n = ht_find(a, key);
    if (!n) { pthread_mutex_unlock(&a->mu); return; }
    /* Drop the ghost too. Keeping it would make the next insert of this id --
       which cannot happen, ids are not reused -- look like a ghost hit. */
    node_free(a, n);
    a->st.invalidated++;
    pthread_mutex_unlock(&a->mu);
}

void arc_clear(invfs_arc *a)
{
    size_t i;
    if (!a) return;
    pthread_mutex_lock(&a->mu);
    for (i = 0; i < a->nbuckets; i++) {
        arc_node *n = a->buckets[i];
        while (n) { arc_node *next = n->hnext; free(n->data); free(n); n = next; }
        a->buckets[i] = NULL;
    }
    a->nnodes = 0;
    memset(&a->t1, 0, sizeof a->t1);
    memset(&a->t2, 0, sizeof a->t2);
    memset(&a->b1, 0, sizeof a->b1);
    memset(&a->b2, 0, sizeof a->b2);
    a->p = a->c / 2;
    pthread_mutex_unlock(&a->mu);
}

void arc_stats(invfs_arc *a, invfs_arc_stats *out)
{
    if (!out) return;
    if (!a) { memset(out, 0, sizeof *out); return; }
    /* Read under the lock like everything else: the stats xattr is served
       while read threads are inside arc_put, and a torn invfs_arc_stats is a
       report of nonsense rather than of the cache. */
    pthread_mutex_lock(&a->mu);
    *out = a->st;
    out->bytes    = a->t1.bytes + a->t2.bytes;
    out->t1_bytes = a->t1.bytes;
    out->t2_bytes = a->t2.bytes;
    out->entries  = a->t1.count + a->t2.count;
    out->ghosts   = a->b1.count + a->b2.count;
    out->p        = a->p;
    pthread_mutex_unlock(&a->mu);
}
