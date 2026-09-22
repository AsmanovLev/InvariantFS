/* vol_delta.c — WP-M10: v3 delta log (append, in-memory index, replay).
 *
 * See vol_delta.h for the contract. File order:
 *   1. record + segment wire codecs (the frozen invarifs.h format),
 *   2. the coalescing open-addressing index,
 *   3. segment allocation / rollover,
 *   4. append, lookup, value read, iteration,
 *   5. mount replay + torn-tail truncation.
 *
 * The design doc is silent on the index implementation and the stripe size;
 * both are recorded here (D1 chose append log + in-memory index; the stripe
 * is 32 blocks / 128 KiB — see invarifs.h). Where the doc is silent on a
 * corruption policy this file chooses conservatively and marks a TODO.
 */

#include "volume_internal.h"
#include "vol_delta.h"
#include "vol_metabuf.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* WP-M20: delta append lock — guards the only writer critical section.
 * Base reads are lock-free (immutable base pages); delta reads are lock-free
 * (index published atomically after record bytes written). Only delta append
 * needs serialization. */
static pthread_mutex_t g_delta_lock = PTHREAD_MUTEX_INITIALIZER;

void vol_delta_lock(void)
{
    pthread_mutex_lock(&g_delta_lock);
}

void vol_delta_unlock(void)
{
    pthread_mutex_unlock(&g_delta_lock);
}

/* Chain walk guard: a corrupt prev_pba cycle must not loop forever. A log
 * this long (128 GiB) is far past any fold trigger the design intends. */
/* DELTA_MAX_SEGMENTS is now in vol_delta.h */

/* WP-M14: the oldest-live-record age is the third D2 fold trigger component.
 * It is a handle clock (CLOCK_MONOTONIC seconds), not a wall clock, so a
 * system time change can neither fire nor suppress a fold. Replayed records
 * get the mount time; a live session gets the append time. */
static uint64_t dl_now_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec;
}

/* ------------------------------------------------------------------ */
/* 1. record + segment wire codecs                                    */
/* ------------------------------------------------------------------ */

static uint16_t dl_rd16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static void dl_wr16be(uint8_t *p, uint16_t x)
{
    p[0] = (uint8_t)(x >> 8);
    p[1] = (uint8_t)(x & 0xFF);
}
static uint32_t dl_rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static void dl_wr32be(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)(x >> 24); p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);  p[3] = (uint8_t)x;
}
static uint64_t dl_rd64be(const uint8_t *p)
{
    return ((uint64_t)dl_rd32be(p) << 32) | dl_rd32be(p + 4);
}
static void dl_wr64be(uint8_t *p, uint64_t x)
{
    dl_wr32be(p, (uint32_t)(x >> 32));
    dl_wr32be(p + 4, (uint32_t)x);
}
/* CRC follows the RT30/page convention: native u32, not byte-swapped. */
static uint32_t dl_rd32(const uint8_t *p)
{
    uint32_t x;
    memcpy(&x, p, sizeof x);
    return x;
}

int vol_delta_rec_parse(const uint8_t *buf, size_t len, size_t off,
                        uint16_t *klen, uint16_t *vlen, uint16_t *flags,
                        size_t *rec_len)
{
    uint16_t kl, vl, fl;
    uint32_t crc;
    size_t total;
    const uint8_t *p;

    if (!buf)
        return -1;
    if (off > len || off + INVFS_DELTA_REC_HDR_LEN > len)
        return 0;   /* not enough room for a header: clean end / short tail */
    p = buf + off;
    /* All-zero header: the zeroed padding after the last live record. This
     * has to be tested before the CRC (an all-zero header would otherwise
     * look like a zero-length key at offset 0). */
    {
        static const uint8_t zero[INVFS_DELTA_REC_HDR_LEN] = {0};
        if (memcmp(p, zero, INVFS_DELTA_REC_HDR_LEN) == 0)
            return 0;
    }
    kl = dl_rd16be(p);
    vl = dl_rd16be(p + 2);
    fl = dl_rd16be(p + 4);
    crc = dl_rd32(p + 6);
    if (kl == 0)
        return -1;                       /* an empty key is never valid */
    if (fl & (uint16_t)~INVFS_DELTA_FLAG_DELETE)
        return -1;                       /* reserved flags must be 0 */
    if ((fl & INVFS_DELTA_FLAG_DELETE) && vl != 0)
        return -1;                       /* delete records carry no value */
    total = (size_t)INVFS_DELTA_REC_HDR_LEN + (size_t)kl + (size_t)vl;
    if (total > len - off)
        return -1;                       /* truncated record body */
    if (invfs_crc32c(p + INVFS_DELTA_REC_HDR_LEN, (size_t)kl + vl) != crc)
        return -1;                       /* torn / corrupt record */
    if (klen)    *klen = kl;
    if (vlen)    *vlen = vl;
    if (flags)   *flags = fl;
    if (rec_len) *rec_len = total;
    return 1;
}

size_t vol_delta_scan_valid(const uint8_t *buf, size_t len,
                            uint64_t *nrec, int *saw_bad)
{
    size_t off = 0, last = 0;
    uint64_t n = 0;
    int bad = 0;

    while (off < len) {
        uint16_t kl, vl, fl;
        size_t rl;
        int rc = vol_delta_rec_parse(buf, len, off, &kl, &vl, &fl, &rl);
        if (rc == 0)
            break;                       /* clean end */
        if (rc < 0) {
            bad = 1;
            break;
        }
        off += rl;
        last = off;
        n++;
    }
    if (nrec)
        *nrec = n;
    if (saw_bad)
        *saw_bad = bad;
    return last;
}

static int delta_hdr_decode(const uint8_t *raw, size_t len,
                            invfs_delta_seg_hdr *out)
{
    uint8_t tmp[INVFS_DELTA_SEG_HDR_LEN];
    uint32_t crc;

    if (len < INVFS_DELTA_SEG_HDR_LEN ||
        memcmp(raw, INVFS_DELTA_SEG_MAGIC, 4) != 0)
        return -1;
    if (dl_rd32be(raw + 4) != INVFS_DELTA_SEG_VERSION)
        return -1;
    if (dl_rd32be(raw + 8) != INVFS_DELTA_SEG_HDR_LEN)
        return -1;
    if (dl_rd32be(raw + 12) != INVFS_DELTA_SEG_BLOCKS)
        return -1;
    crc = dl_rd32(raw + 40);
    memcpy(tmp, raw, INVFS_DELTA_SEG_HDR_LEN);
    memset(tmp + 40, 0, 4);
    if (invfs_crc32c(tmp, INVFS_DELTA_SEG_HDR_LEN) != crc)
        return -1;
    if (out) {
        out->version    = INVFS_DELTA_SEG_VERSION;
        out->hdr_size   = INVFS_DELTA_SEG_HDR_LEN;
        out->seg_blocks = INVFS_DELTA_SEG_BLOCKS;
        out->seg_seq    = dl_rd64be(raw + 16);
        out->prev_pba   = dl_rd64be(raw + 24);
        out->next_pba   = dl_rd64be(raw + 32);
        out->crc32c     = crc;
        memcpy(out->magic, INVFS_DELTA_SEG_MAGIC, 4);
    }
    return 0;
}

static void delta_hdr_encode(uint8_t *raw, uint64_t seg_seq, uint64_t prev_pba)
{
    memset(raw, 0, INVFS_DELTA_SEG_HDR_LEN);
    memcpy(raw, INVFS_DELTA_SEG_MAGIC, 4);
    dl_wr32be(raw + 4, INVFS_DELTA_SEG_VERSION);
    dl_wr32be(raw + 8, INVFS_DELTA_SEG_HDR_LEN);
    dl_wr32be(raw + 12, INVFS_DELTA_SEG_BLOCKS);
    dl_wr64be(raw + 16, seg_seq);
    dl_wr64be(raw + 24, prev_pba);
    dl_wr64be(raw + 32, 0);              /* next reserved 0 (see invarifs.h) */
    {
        uint8_t tmp[INVFS_DELTA_SEG_HDR_LEN];
        memcpy(tmp, raw, INVFS_DELTA_SEG_HDR_LEN);
        memset(tmp + 40, 0, 4);
        /* native u32, matching the descriptor convention */
        {
            uint32_t crc = invfs_crc32c(tmp, INVFS_DELTA_SEG_HDR_LEN);
            memcpy(raw + 40, &crc, 4);
        }
    }
}

int delta_read_hdr(invfs_volume *v, uint64_t pba,
                          invfs_delta_seg_hdr *out)
{
    uint8_t raw[INVFS_DELTA_SEG_HDR_LEN];
    if (pba == 0 || pba >= v->sb.total_blocks)
        return -1;
    if (io_pread(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE, raw, sizeof raw) != 0)
        return -1;
    return delta_hdr_decode(raw, sizeof raw, out);
}

/* ------------------------------------------------------------------ */
/* 2. coalescing index (open addressing, linear probe, FNV-1a)         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *key;       /* owned copy; NULL = empty slot */
    uint16_t klen;
    uint16_t flags;
    uint16_t vlen;
    uint64_t seg;
    uint64_t off;
    uint64_t seq;
} delta_slot;

struct delta_index {
    delta_slot *slot;
    size_t      cap;    /* power of two */
    size_t      used;   /* occupied slots (keys are never tombstoned) */
};

static uint32_t dl_hash(const uint8_t *p, uint16_t n)
{
    uint32_t h = 2166136261u;            /* FNV-1a 32 */
    uint16_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static struct delta_index *di_new(void)
{
    struct delta_index *di = (struct delta_index *)calloc(1, sizeof *di);
    if (!di)
        return NULL;
    di->cap = 64;
    di->slot = (delta_slot *)calloc(di->cap, sizeof *di->slot);
    if (!di->slot) {
        free(di);
        return NULL;
    }
    return di;
}

static void di_free(struct delta_index *di)
{
    size_t i;
    if (!di)
        return;
    for (i = 0; i < di->cap; i++)
        free(di->slot[i].key);
    free(di->slot);
    free(di);
}

/* Probe for a key; returns the slot index or (size_t)-1 when absent. */
static size_t di_find(const struct delta_index *di,
                      const uint8_t *key, uint16_t klen)
{
    size_t mask, i;
    if (!di || !di->cap)
        return (size_t)-1;
    mask = di->cap - 1;
    i = dl_hash(key, klen) & mask;
    for (;;) {
        const delta_slot *s = &di->slot[i];
        if (!s->key)
            return (size_t)-1;
        if (s->klen == klen && memcmp(s->key, key, klen) == 0)
            return i;
        i = (i + 1) & mask;
    }
}

static int di_grow(struct delta_index *di)
{
    size_t ncap = di->cap * 2, i;
    delta_slot *ns = (delta_slot *)calloc(ncap, sizeof *ns);
    if (!ns)
        return -1;
    for (i = 0; i < di->cap; i++) {
        delta_slot *s = &di->slot[i];
        size_t j;
        if (!s->key)
            continue;
        j = dl_hash(s->key, s->klen) & (ncap - 1);
        while (ns[j].key)
            j = (j + 1) & (ncap - 1);
        ns[j] = *s;                      /* transfer the key pointer */
    }
    free(di->slot);
    di->slot = ns;
    di->cap = ncap;
    return 0;
}

/* Insert or coalesce one record. *inserted is set when the key is new.
 * Returns 0, or -1 on OOM. */
static int di_put(struct delta_index *di, const uint8_t *key, uint16_t klen,
                  uint64_t seg, uint64_t off, uint64_t seq,
                  uint16_t flags, uint16_t vlen, int *inserted)
{
    size_t mask, i;
    delta_slot *s;

    if (!di)
        return -1;
    if (inserted)
        *inserted = 0;
    if (di->used * 10 >= di->cap * 7) {
        if (di_grow(di) != 0)
            return -1;
    }
    mask = di->cap - 1;
    i = dl_hash(key, klen) & mask;
    for (;;) {
        s = &di->slot[i];
        if (!s->key) {
            uint8_t *k = (uint8_t *)malloc(klen);
            if (!k)
                return -1;
            memcpy(k, key, klen);
            s->key = k;
            s->klen = klen;
            di->used++;
            if (inserted)
                *inserted = 1;
            break;
        }
        if (s->klen == klen && memcmp(s->key, key, klen) == 0)
            break;                       /* coalesce: latest wins */
        i = (i + 1) & mask;
    }
    s->flags = flags;
    s->vlen = vlen;
    s->seg = seg;
    s->off = off;
    s->seq = seq;
    return 0;
}

/* bm_dirty is static in volume.c; mirror its two-line range widening (the
 * same idiom vol_metabuf uses) so reservation bookkeeping stays local. */
static void dl_bm_dirty(invfs_volume *v, uint64_t i)
{
    uint64_t byte = i / 8;
    if (v->bm_lo > v->bm_hi) { v->bm_lo = byte; v->bm_hi = byte + 1; return; }
    if (byte < v->bm_lo) v->bm_lo = byte;
    if (byte + 1 > v->bm_hi) v->bm_hi = byte + 1;
}

/* Make sure a replayed segment's blocks are marked allocated. alloc_blocks
 * already did this for a segment created in this session, but the delta is
 * not flushed through the v2 vol_flush path, so after a crash/remount the
 * on-disk bitmap may not cover it. Replaying the chain re-reserves the
 * storage before any allocator call can hand it out again (the structure-
 * before-reference rule covers the RT30 side). Already-set bits are left
 * alone so the free counters are not double-decremented. */
static void dl_reserve_segment(invfs_volume *v, uint64_t pba)
{
    uint64_t b, end = pba + INVFS_DELTA_SEG_BLOCKS;
    /* alloc_state_reset sets the per-zone cursors; vol_open runs it AFTER
     * this replay, so on the real path the counters are not live yet and
     * will be recomputed from the (now-corrected) bitmap. Only adjust them
     * when they are already live (the synthetic/unit path), and never
     * underflow. */
    int counters_live = (v->shadow_cursor != 0 || v->raw_cursor != 0);
    if (pba >= v->sb.total_blocks)
        return;
    if (end > v->sb.total_blocks)
        end = v->sb.total_blocks;
    for (b = pba; b < end; b++) {
        if (bit_get(v->bitmap, b))
            continue;
        bit_set(v->bitmap, b);
        if (v->meta_type_bitmap)
            bit_set(v->meta_type_bitmap, b);
        dl_bm_dirty(v, b);
        if (counters_live) {
            if (v->free_blocks)
                v->free_blocks--;
            if (b >= v->sb.shadow_zone_start) {
                if (v->shadow_free)
                    v->shadow_free--;
            } else if (b >= v->sb.raw_zone_start) {
                if (v->raw_free)
                    v->raw_free--;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 3. segment creation / rollover                                      */
/* ------------------------------------------------------------------ */

/* Allocate and zero a fresh segment with `prev` as its older neighbour.
 * The zeroed payload is what makes replay safe on reused blocks: a stale
 * record from a previously freed segment can never be mistaken for a live
 * one. Returns the pba, or 0 on failure. */
static uint64_t delta_new_segment(invfs_volume *v, uint64_t prev)
{
    uint64_t pba;
    uint8_t *seg;

    pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                       INVFS_DELTA_SEG_BLOCKS, 0, INVFS_ALLOC_META);
    if (!pba)
        return 0;
    seg = (uint8_t *)calloc(1, (size_t)INVFS_DELTA_SEG_BYTES);
    if (!seg) {
        vol_free_blocks(v, pba, INVFS_DELTA_SEG_BLOCKS);
        return 0;
    }
    delta_hdr_encode(seg, ++v->delta_seg_gen, prev);
    if (io_pwrite(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE, seg, (size_t)INVFS_DELTA_SEG_BYTES) != 0) {
        free(seg);
        vol_free_blocks(v, pba, INVFS_DELTA_SEG_BLOCKS);
        return 0;
    }
    free(seg);
    if (vmux_barrier(v, "delta segment header") < 0)
        return 0;
    return pba;
}

/* ------------------------------------------------------------------ */
/* 4. append / lookup / read / iterate                                 */
/* ------------------------------------------------------------------ */

int vol_delta_append(invfs_volume *v, const uint8_t *key, uint16_t klen,
                     const uint8_t *val, uint16_t vlen, uint16_t flags)
{
    size_t rl;
    uint64_t abs, seq;
    uint32_t crc;
    uint8_t *rec;
    int inserted = 0;

    if (!v || !key || klen == 0)
        return -1;
    if (flags & (uint16_t)~INVFS_DELTA_FLAG_DELETE)
        return -1;
    if ((flags & INVFS_DELTA_FLAG_DELETE) && vlen != 0)
        return -1;
    if (vlen && !val)
        return -1;
    if (v->sb.vol_flags & VOLF_READONLY)
        return -1;
    if (!v->delta_ready && vol_delta_mount(v) != 0)
        return -1;

    rl = (size_t)INVFS_DELTA_REC_HDR_LEN + (size_t)klen + (size_t)vlen;
    if (rl > INVFS_DELTA_SEG_BYTES - INVFS_DELTA_SEG_HDR_LEN)
        return -1;   /* single record larger than a segment (TODO: WP-M12) */

    pthread_mutex_lock(&g_delta_lock);

    if (v->delta_seg_pba == 0 ||
        v->delta_bump + rl > INVFS_DELTA_SEG_BYTES) {
        uint64_t pba = delta_new_segment(v, v->delta_seg_pba);
        if (!pba) {
            pthread_mutex_unlock(&g_delta_lock);
            return -1;
        }
        /* structure-before-reference: the segment (header + zeroed payload)
         * is durable before RT30 names it as the active segment. */
        v->rt30.delta_pba = pba;
        if (!v->rt30_present) {
            /* First delta allocation on a handle whose RT30 was absent (a
             * fresh synthetic volume, or a v3 image interrupted before its
             * descriptor write). Build the descriptor the way
             * mbuf_root_publish does; mbuf_rt30_store only stamps the CRC. */
            memset(&v->rt30, 0, sizeof v->rt30);
            memcpy(v->rt30.magic, "RT30", 4);
            v->rt30.version = INVFS_RT30_VERSION;
            v->rt30.page_size = INVFS_V3_PAGE_SIZE_DEFAULT;
            v->rt30_present = 1;
            v->rt30.delta_pba = pba;
        }
        if (mbuf_rt30_store(v) != 0) {
            pthread_mutex_unlock(&g_delta_lock);
            return -1;
        }
        if (vmux_barrier(v, "delta segment publish") < 0) {
            pthread_mutex_unlock(&g_delta_lock);
            return -1;
        }
        v->delta_segments++;
        v->delta_seg_pba = pba;
        v->delta_bump = INVFS_DELTA_SEG_HDR_LEN;
        v->delta_ready = 1;
    }

    rec = (uint8_t *)malloc(rl);
    if (!rec) {
        pthread_mutex_unlock(&g_delta_lock);
        return -1;
    }
    dl_wr16be(rec, klen);
    dl_wr16be(rec + 2, vlen);
    dl_wr16be(rec + 4, flags);
    crc = invfs_crc32c(key, klen);
    if (vlen)
        crc = invfs_crc32c_update(crc, val, vlen);
    memcpy(rec + 6, &crc, 4);
    memcpy(rec + INVFS_DELTA_REC_HDR_LEN, key, klen);
    if (vlen)
        memcpy(rec + INVFS_DELTA_REC_HDR_LEN + klen, val, vlen);

    abs = v->delta_seg_pba * (uint64_t)INVFS_BLOCK_SIZE + v->delta_bump;
    if (io_pwrite(&v->io, abs, rec, rl) != 0) {
        free(rec);
        pthread_mutex_unlock(&g_delta_lock);
        return -1;
    }
    free(rec);
    /* The append is the writer's critical section (design §9): make the
     * record durable before it becomes the indexed winner. */
    if (vmux_barrier(v, "delta append") < 0) {
        pthread_mutex_unlock(&g_delta_lock);
        return -1;
    }

    seq = ++v->delta_seq;
    /* off is segment-relative: read_value/addressing re-add seg*blocksize. */
    if (di_put(v->delta_index, key, klen, v->delta_seg_pba, v->delta_bump, seq,
               flags, vlen, &inserted) != 0) {
        /* OOM: the record is durable, the index is behind */
        pthread_mutex_unlock(&g_delta_lock);
        return -1;
    }
    /* WP-M14: the first live record anchors the age trigger (delta_oldest_seq
     * == 0 means no record has been tracked yet). A coalesced update to an
     * existing key leaves the anchor alone: that key's record is still live,
     * and an old disjoint record must still be able to age the log out even
     * if a hot key is rewritten. A replay has already set the anchor when the
     * chain was non-empty, so this only fires on the first append of a
     * session with an empty recent tier. */
    if (v->delta_oldest_seq == 0) {
        v->delta_oldest_seq = seq;
        v->delta_oldest_when = dl_now_s();
    }
    v->delta_bytes += rl;
    v->delta_bump += rl;
    if (inserted)
        v->delta_records++;
    pthread_mutex_unlock(&g_delta_lock);
    return 0;
}

int vol_delta_lookup(invfs_volume *v, const uint8_t *key, uint16_t klen,
                     delta_ref *out)
{
    size_t i;
    const delta_slot *s;

    if (!v || !key || klen == 0)
        return -1;
    pthread_mutex_lock(&g_delta_lock);
    if (!v->delta_index) {
        pthread_mutex_unlock(&g_delta_lock);
        return 0;
    }
    i = di_find(v->delta_index, key, klen);
    if (i == (size_t)-1) {
        pthread_mutex_unlock(&g_delta_lock);
        return 0;
    }
    s = &v->delta_index->slot[i];
    if (out) {
        out->seg = s->seg;
        out->off = s->off;
        out->seq = s->seq;
        out->flags = s->flags;
        out->vlen = s->vlen;
    }
    pthread_mutex_unlock(&g_delta_lock);
    return 1;
}

int vol_delta_read_value(invfs_volume *v, const delta_ref *ref,
                         uint8_t *buf, size_t cap, uint16_t *vlen_out)
{
    uint8_t hdr[INVFS_DELTA_REC_HDR_LEN];
    uint16_t kl;

    if (!v || !ref)
        return -1;
    if (vlen_out)
        *vlen_out = 0;
    if (ref->vlen == 0)
        return 0;                        /* delete / empty value */
    if (!buf || cap < ref->vlen)
        return -1;
    if (io_pread(&v->io, ref->seg * (uint64_t)INVFS_BLOCK_SIZE + ref->off,
                 hdr, sizeof hdr) != 0)
        return -1;
    kl = dl_rd16be(hdr);
    if (io_pread(&v->io, ref->seg * (uint64_t)INVFS_BLOCK_SIZE + ref->off +
                 INVFS_DELTA_REC_HDR_LEN + kl, buf, ref->vlen) != 0)
        return -1;
    if (vlen_out)
        *vlen_out = ref->vlen;
    return 0;
}

int vol_delta_iter(invfs_volume *v, vol_delta_iter_cb cb, void *ctx)
{
    struct delta_index *di;
    size_t i;

    if (!v || !cb)
        return -1;
    di = v->delta_index;
    if (!di)
        return 0;
    for (i = 0; i < di->cap; i++) {
        delta_ref r;
        int rc;
        if (!di->slot[i].key)
            continue;
        r.seg = di->slot[i].seg;
        r.off = di->slot[i].off;
        r.seq = di->slot[i].seq;
        r.flags = di->slot[i].flags;
        r.vlen = di->slot[i].vlen;
        rc = cb(ctx, di->slot[i].key, di->slot[i].klen, &r);
        if (rc)
            return rc;
    }
    return 0;
}

/* WP-M11: byte-lexicographic compare of two keys (shorter prefix first),
 * matching btree_scan / bt_cmp, so the delta stream and the base stream
 * merge under one ordering. */
static int dl_key_cmp(const uint8_t *a, uint16_t an,
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

/* One collected key + its winning ref, for the ordered range cursor. */
typedef struct {
    const uint8_t *key;   /* borrowed from the index slot */
    uint16_t       klen;
    delta_ref      ref;
} dl_rentry;

static int dl_rentry_cmp(const void *pa, const void *pb)
{
    const dl_rentry *a = (const dl_rentry *)pa;
    const dl_rentry *b = (const dl_rentry *)pb;
    return dl_key_cmp(a->key, a->klen, b->key, b->klen);
}

int vol_delta_range(invfs_volume *v,
                    const uint8_t *lo, uint16_t lolen,
                    const uint8_t *hi, uint16_t hilen,
                    vol_delta_range_cb cb, void *ctx)
{
    struct delta_index *di;
    dl_rentry *arr = NULL;
    size_t n = 0, cap = 0, i;
    int rc = 0;

    if (!v || !cb)
        return -1;
    if ((lolen && !lo) || (hilen && !hi))
        return -1;
    di = v->delta_index;
    if (!di)
        return 0;                        /* empty recent tier */

    for (i = 0; i < di->cap; i++) {
        const delta_slot *s = &di->slot[i];
        if (!s->key)
            continue;
        if (lolen && dl_key_cmp(s->key, s->klen, lo, lolen) < 0)
            continue;
        if (hilen && dl_key_cmp(s->key, s->klen, hi, hilen) >= 0)
            continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            dl_rentry *na = (dl_rentry *)realloc(arr, ncap * sizeof *na);
            if (!na) {
                free(arr);
                return -1;
            }
            arr = na;
            cap = ncap;
        }
        arr[n].key = s->key;
        arr[n].klen = s->klen;
        arr[n].ref.seg = s->seg;
        arr[n].ref.off = s->off;
        arr[n].ref.seq = s->seq;
        arr[n].ref.flags = s->flags;
        arr[n].ref.vlen = s->vlen;
        n++;
    }
    if (n > 1)
        qsort(arr, n, sizeof *arr, dl_rentry_cmp);
    for (i = 0; i < n; i++) {
        rc = cb(ctx, arr[i].key, arr[i].klen, &arr[i].ref);
        if (rc)
            break;
    }
    free(arr);
    return rc;                           /* 0 = complete, else propagated */
}

uint64_t vol_delta_count(const invfs_volume *v)
{
    return (v && v->delta_index) ? (uint64_t)v->delta_index->used : 0;
}

void vol_delta_stats(const invfs_volume *v, uint64_t *records,
                     uint64_t *segments, uint64_t *bytes)
{
    if (records)  *records  = v ? v->delta_records  : 0;
    if (segments) *segments = v ? v->delta_segments : 0;
    if (bytes)    *bytes    = v ? v->delta_bytes    : 0;
}

/* ------------------------------------------------------------------ */
/* 5. mount replay + truncation                                        */
/* ------------------------------------------------------------------ */

/* Replay one segment's payload into the index. Returns the number of
 * records accepted and sets the truncation point (payload offset one past
 * the last valid record) via *valid_end, and whether the tail was torn. */
static int delta_replay_segment(invfs_volume *v, uint64_t pba,
                                const invfs_delta_seg_hdr *h,
                                size_t *valid_end, uint64_t *accepted,
                                int *torn)
{
    uint8_t *buf;
    size_t plen = (size_t)(INVFS_DELTA_SEG_BYTES - h->hdr_size);
    size_t off = 0, last = 0;
    uint64_t n = 0;
    int bad = 0;

    buf = (uint8_t *)malloc((size_t)INVFS_DELTA_SEG_BYTES);
    if (!buf)
        return -1;
    if (io_pread(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE, buf, (size_t)INVFS_DELTA_SEG_BYTES) != 0) {
        free(buf);
        return -1;
    }
    while (off < plen) {
        uint16_t kl, vl, fl;
        size_t rl;
        int rc;
        rc = vol_delta_rec_parse(buf + h->hdr_size, plen, off,
                                 &kl, &vl, &fl, &rl);
        if (rc == 0)
            break;
        if (rc < 0) {
            bad = 1;
            break;
        }
        {
            int inserted = 0;
            uint64_t local = (uint64_t)h->hdr_size + off;   /* segment-relative */
            int prc = di_put(v->delta_index,
                             buf + h->hdr_size + off + INVFS_DELTA_REC_HDR_LEN,
                             kl, pba, local, ++v->delta_seq, fl, vl, &inserted);
            if (prc != 0) {
                free(buf);
                return -1;
            }
            if (inserted)
                v->delta_records++;
        }
        off += rl;
        last = off;
        n++;
    }
    free(buf);
    if (valid_end)
        *valid_end = last;
    if (accepted)
        *accepted = n;
    if (torn)
        *torn = bad;
    return 0;
}

int vol_delta_mount(invfs_volume *v)
{
    uint64_t *chain = NULL;
    size_t nchain = 0, cap = 0, i;
    int rc = -1;

    if (!v)
        return -1;
    vol_delta_close(v);
    v->delta_index = di_new();
    if (!v->delta_index)
        return -1;
    v->delta_ready = 0;
    v->delta_seg_pba = 0;
    v->delta_bump = 0;
    v->delta_seq = 0;
    v->delta_records = 0;
    v->delta_segments = 0;
    v->delta_bytes = 0;
    v->delta_seg_gen = 0;
    v->delta_oldest_seq = 0;
    v->delta_oldest_when = 0;

    if (!v->rt30_present || v->rt30.delta_pba == 0) {
        v->delta_ready = 1;              /* empty recent tier */
        return 0;
    }
    if (v->rt30.delta_pba >= v->sb.total_blocks) {
        fprintf(stderr, "vol_delta: RT30 delta_pba %llu out of range; "
                "presenting an empty recent tier\n",
                (unsigned long long)v->rt30.delta_pba);
        v->delta_ready = 1;
        return 0;
    }

    /* Collect the chain newest -> oldest. A torn segment header ends the
     * walk: the segment it names and everything older become unreachable
     * (prev_pba is the only link). The design is silent on header
     * corruption; this WP logs and keeps the newer segments it already
     * has rather than refusing the mount (TODO: WP-M13 may tighten this
     * to a repair/quarantine policy). */
    {
        uint64_t cur = v->rt30.delta_pba;
        while (cur && nchain < DELTA_MAX_SEGMENTS) {
            invfs_delta_seg_hdr h;
            if (delta_read_hdr(v, cur, &h) != 0) {
                fprintf(stderr, "vol_delta: segment %llu header invalid; "
                        "recent tier truncated (%llu newer segment(s) kept)\n",
                        (unsigned long long)cur, (unsigned long long)nchain);
                break;
            }
            if (nchain == cap) {
                size_t ncap = cap ? cap * 2 : 8;
                uint64_t *nc = (uint64_t *)realloc(chain, ncap * sizeof *nc);
                if (!nc)
                    goto out;
                chain = nc;
                cap = ncap;
            }
            chain[nchain++] = cur;
            dl_reserve_segment(v, cur);
            if (h.prev_pba == cur)
                break;                   /* self-reference: stop */
            cur = h.prev_pba;
        }
    }

    /* Replay oldest -> newest so a later record coalesces over an earlier
     * one (latest wins). chain[0] is the active/newest segment. */
    for (i = nchain; i-- > 0; ) {
        invfs_delta_seg_hdr h;
        size_t valid_end = 0;
        uint64_t accepted = 0;
        int torn = 0;
        if (delta_read_hdr(v, chain[i], &h) != 0)
            continue;                    /* validated above; be defensive */
        if (delta_replay_segment(v, chain[i], &h, &valid_end, &accepted,
                                 &torn) != 0)
            goto out;
        if (torn) {
            fprintf(stderr, "vol_delta: segment %llu has a torn tail; "
                    "truncated at %llu record(s)\n",
                    (unsigned long long)chain[i],
                    (unsigned long long)accepted);
        }
        v->delta_bytes += valid_end;
        if (h.seg_seq > v->delta_seg_gen)
            v->delta_seg_gen = h.seg_seq;
        if (i == 0) {                    /* newest: the active segment */
            v->delta_seg_pba = chain[i];
            v->delta_bump = h.hdr_size + valid_end;
        }
    }
    v->delta_segments = nchain;
    v->delta_ready = 1;
    /* WP-M14: replay assigns delta_seq in ascending order, so the first live
     * record seen has the lowest seq -- the oldest anchor for the age trigger.
     * Its real append time predates this mount; use the mount time, which
     * understates the age by at most one session and keeps the trigger from
     * firing spuriously on a volume that was idle. */
    if (v->delta_seq > 0) {
        v->delta_oldest_seq = 1;
        v->delta_oldest_when = dl_now_s();
    }
    rc = 0;
out:
    free(chain);
    if (rc != 0) {
        /* Do not leak a half-built index on an I/O error: vol_open's fail
         * path frees the volume without calling vol_delta_close. */
        di_free(v->delta_index);
        v->delta_index = NULL;
        v->delta_ready = 0;
    }
    return rc;
}

/* ---- WP-M16: delta truncation for save-point rollback ----------------- */

int vol_delta_truncate(invfs_volume *v, uint64_t delta_end)
{
    uint64_t *chain = NULL;
    size_t nchain = 0, cap = 0;
    uint64_t *newer = NULL;
    size_t n_newer = 0, cap_newer = 0;
    uint64_t offset;
    size_t i;
    int rc = -1;

    if (!v)
        return -1;
    if (delta_end == 0) {
        delta_end = INVFS_DELTA_SEG_HDR_LEN;
    }

    if (v->delta_seg_pba == 0)
        return 0;

    chain = NULL;
    nchain = cap = 0;

    {
        uint64_t cur = v->delta_seg_pba;
        while (cur && nchain < DELTA_MAX_SEGMENTS) {
            invfs_delta_seg_hdr h;
            if (delta_read_hdr(v, cur, &h) != 0)
                break;
            if (nchain >= cap) {
                size_t ncap = cap ? cap * 2 : 8;
                uint64_t *nc = (uint64_t *)realloc(chain, ncap * sizeof *nc);
                if (!nc)
                    goto out;
                chain = nc;
                cap = ncap;
            }
            chain[nchain++] = cur;
            if (h.prev_pba == cur)
                break;
            cur = h.prev_pba;
        }
    }

    if (nchain == 0) {
        rc = 0;
        goto out;
    }

    offset = 0;
    newer = NULL;
    n_newer = 0;
    cap_newer = 0;

    for (i = 0; i < nchain; i++) {
        uint64_t seg_pba = chain[nchain - 1 - i];
        uint64_t seg_bytes = (i == 0) ? v->delta_bump : INVFS_DELTA_SEG_BYTES;

        if (offset + seg_bytes > delta_end) {
            uint64_t keep_bytes = (i == 0) ? v->delta_bump : INVFS_DELTA_SEG_BYTES;
            uint64_t trunc_off;
            invfs_delta_seg_hdr h;
            uint8_t *buf = NULL;
            uint64_t old_prev_pba;

            if (delta_read_hdr(v, seg_pba, &h) != 0)
                goto out;

            old_prev_pba = h.prev_pba;

            for (size_t j = i + 1; j < nchain; j++) {
                uint64_t npba = chain[nchain - 1 - j];
                if (n_newer >= cap_newer) {
                    size_t ncap = cap_newer ? cap_newer * 2 : 8;
                    uint64_t *nn = (uint64_t *)realloc(newer, ncap * sizeof *nn);
                    if (!nn)
                        goto out;
                    newer = nn;
                    cap_newer = ncap;
                }
                newer[n_newer++] = npba;
            }

            trunc_off = delta_end - offset;
            if (trunc_off < INVFS_DELTA_SEG_HDR_LEN)
                trunc_off = INVFS_DELTA_SEG_HDR_LEN;

            buf = (uint8_t *)malloc((size_t)INVFS_DELTA_SEG_BYTES);
            if (!buf)
                goto out;

            if (io_pread(&v->io, seg_pba * (uint64_t)INVFS_BLOCK_SIZE, buf, (size_t)INVFS_DELTA_SEG_BYTES) != 0) {
                free(buf);
                goto out;
            }

            memset(buf + trunc_off, 0, (size_t)(INVFS_DELTA_SEG_BYTES - trunc_off));

            delta_hdr_encode(buf, h.seg_seq, old_prev_pba);

            if (io_pwrite(&v->io, seg_pba * (uint64_t)INVFS_BLOCK_SIZE, buf, (size_t)INVFS_DELTA_SEG_BYTES) != 0) {
                free(buf);
                goto out;
            }
            free(buf);

            v->delta_seg_pba = seg_pba;
            v->delta_bump = trunc_off;

            for (size_t j = 0; j < n_newer; j++) {
                invfs_delta_seg_hdr nh;
                if (delta_read_hdr(v, newer[j], &nh) == 0) {
                    vol_free_blocks(v, newer[j], INVFS_DELTA_SEG_BLOCKS);
                }
            }

            if (vmux_barrier(v, "delta truncate") < 0)
                goto out;

            rc = 0;
            goto out;
        }
        offset += seg_bytes;
    }

    if (offset < delta_end && nchain > 0) {
        uint64_t last_pba = chain[0];
        invfs_delta_seg_hdr h;

        if (delta_read_hdr(v, last_pba, &h) != 0)
            goto out;

        for (i = 1; i < nchain; i++) {
            uint64_t npba = chain[nchain - i];
            if (n_newer >= cap_newer) {
                size_t ncap = cap_newer ? cap_newer * 2 : 8;
                uint64_t *nn = (uint64_t *)realloc(newer, ncap * sizeof *nn);
                if (!nn)
                    goto out;
                newer = nn;
                cap_newer = ncap;
            }
            newer[n_newer++] = npba;
        }

        v->delta_seg_pba = last_pba;
        v->delta_bump = h.prev_pba == last_pba
                       ? INVFS_DELTA_SEG_HDR_LEN
                       : delta_end - offset + INVFS_DELTA_SEG_HDR_LEN;
        if (v->delta_bump > INVFS_DELTA_SEG_BYTES)
            v->delta_bump = INVFS_DELTA_SEG_BYTES;

        for (size_t j = 0; j < n_newer; j++) {
            invfs_delta_seg_hdr nh;
            if (delta_read_hdr(v, newer[j], &nh) == 0) {
                vol_free_blocks(v, newer[j], INVFS_DELTA_SEG_BLOCKS);
            }
        }

        if (vmux_barrier(v, "delta truncate") < 0)
            goto out;

        rc = 0;
        goto out;
    }

    rc = 0;
out:
    free(chain);
    free(newer);
    return rc;
}

void vol_delta_close(invfs_volume *v)
{
    if (!v)
        return;
    di_free(v->delta_index);
    v->delta_index = NULL;
    v->delta_seg_pba = 0;
    v->delta_bump = 0;
    v->delta_ready = 0;
    v->delta_records = 0;
    v->delta_segments = 0;
    v->delta_bytes = 0;
    v->delta_seq = 0;
    v->delta_seg_gen = 0;
    v->delta_oldest_seq = 0;
    v->delta_oldest_when = 0;
}
