/*
 * migrate-v2.c — InvariantFS offline format v1 -> v2 conversion (WP27)
 *
 *   invf-migrate-v2 <image|device>          convert, in place
 *   invf-migrate-v2 --abort <image|device>  disarm an interrupted
 *                                           conversion whose apply never
 *                                           started
 *
 * Format v2 (invarifs.h, WP27): AST block entries are 32B and carry the
 * segment's physical address; the L2P journal shrinks to the owner-scoped
 * WAL; heat lives in the records' INO2 ext ("invfs.heat" TLV); the bitmap
 * is a derived cache. A v2 reader refuses a v1 volume loudly (no dual
 * readers) -- this tool is the bridge.
 *
 * The conversion reads the v1 journal (both slot formats, and the legacy
 * flat log) + the v1 records, resolves every live entry's pba, and writes:
 *
 *   - the v2 inode stream: the LIVE records only (the WP22e compaction
 *     rule: v2 position-kill tombstones name absolute positions, which no
 *     length-changing rewrite could preserve -- nothing that references a
 *     position survives the cut, so the new stream is self-consistent),
 *     entries grown 24B -> 32B with the resolved pba, per-file heat
 *     folded into the INO2 ext (rheat = the file's hottest segment,
 *     wheat = max write-heat; absent when the v1 volume carried none --
 *     the v2 "absent reads as born-once" rule);
 *   - the owner-WAL slot image: exactly the live owner ("\x01...") maps,
 *     re-imaged into journal slot 0 with a sequence above every old slot
 *     (a stale sibling slot can never win the replay);
 *   - the derived bitmap: metadata span (+ dev1 mirror span) + every live
 *     record's extents.
 *
 * Crash protocol (the house stage -> arm -> apply -> commit shape; the
 * CVT0/CVTS descriptors at block 0 offset INVFS_CVT0_OFF): the products
 * are staged contiguously in a free-space run and CRC-verified by
 * read-back BEFORE the descriptor arms the conversion (the superblock
 * goes RECOVERY in the same step); the apply copies the staging home
 * (idempotent -- it reads only the staging), journal image first, then
 * the stream, then the bitmap; the commit (superblock with VOLF_ASTV2 +
 * slot-0 selector + cleared descriptor, one block-0 write) lands last.
 * A crash anywhere before the commit re-enters the apply at the next run.
 * --abort is sound exactly because the journal image is applied first and
 * its header always differs from whatever a v1 journal has there (the
 * staged image's sequence is max(old)+1; a legacy v1 log does not even
 * carry the magic): a slot-0 head that differs from the staged one PROVES
 * no apply write ever landed, so clearing CVT0 + RECOVERY returns the
 * volume to its pre-conversion v1 state.
 *
 * OFFLINE ONLY: the volume must be unmounted, CLEAN, not read-only, with
 * no live sweep checkpoint (CKP0 pins absolute journal/inode positions)
 * and no pending compaction (CMP0). A redundancy seal is fine: conversion
 * moves no data block, so the stripes stay valid. Two-device volumes
 * (DEVT) write every metadata change through the mirror.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#endif

#include "invarifs.h"
#include "volume.h"
#include "blkio.h"

static int bit_get(const uint8_t *b, uint64_t i) { return (b[i / 8] >> (i % 8)) & 1; }
static void bit_set(uint8_t *b, uint64_t i) { b[i / 8] |= (uint8_t)(1u << (i % 8)); }

#ifndef _WIN32
/* Test hooks (tools/test-migrate-v2.sh): die right after the staging is
 * verified ("staged"), right after the arm ("armed"), or after the apply
 * landed but before the commit ("applied") -- the three points the
 * crash-safety argument is about. */
static int abort_at(const char *stage)
{
    const char *a = getenv("INVFS_MIGRATE_ABORT_AT");
    if (a && strcmp(a, stage) == 0)
        fflush(stdout);   /* the log must show how far the kill came */
    return a && strcmp(a, stage) == 0;
}
#endif

/* ------------------------------------------------------------------ *
 * the io router (the resize.c pattern): metadata writes mirror to dev1
 * ------------------------------------------------------------------ */
typedef struct {
    blkio   *io, *io2;         /* io2 NULL = single-device */
    uint64_t dev0_bytes;       /* global->local split */
    uint64_t meta_end;         /* mirrored span [0, meta_end) */
} cvio;

static int cv_pread(cvio *r, uint64_t off, void *buf, size_t len)
{
    if (off < r->meta_end || !r->io2)
        return blkio_pread(r->io, off, buf, len);
    if (off < r->dev0_bytes)
        return blkio_pread(r->io, off, buf, len);
    return blkio_pread(r->io2, off - r->dev0_bytes, buf, len);
}

static int cv_pwrite(cvio *r, uint64_t off, const void *buf, size_t len)
{
    if (off < r->meta_end) {
        if (blkio_pwrite(r->io, off, buf, len) != 0)
            return -1;
        if (r->io2 && blkio_pwrite(r->io2, off, buf, len) != 0)
            return -1;
        return 0;
    }
    if (off < r->dev0_bytes || !r->io2)
        return blkio_pwrite(r->io, off, buf, len);
    return blkio_pwrite(r->io2, off - r->dev0_bytes, buf, len);
}

static int cv_flush(cvio *r)
{
    int rc = blkio_flush(r->io);
    if (r->io2 && blkio_flush(r->io2) != 0) rc = -1;
    return rc;
}

/* ------------------------------------------------------------------ *
 * the v1 journal, parsed into a newest-wins (inode,lba) table
 * ------------------------------------------------------------------ */
typedef struct {
    uint64_t inode, lba, pba;
    uint32_t len;              /* blocks */
    uint16_t rheat;            /* the v1 pad: u16 LE read heat */
    uint8_t  wheat;            /* u8 write heat */
    uint8_t  live;
} v1_ent;

typedef struct {
    v1_ent *t;
    size_t mask, n;
} v1_map;

static uint64_t v1_mix(uint64_t inode, uint64_t lba)
{
    uint64_t h = inode * 0x9E3779B97F4A7C15ull ^ lba;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return h;
}

/* backward scan for a key's slot (deletion by rehash keeps the cluster
 * consistent; the table only ever shrinks under UNMAP at load time) */
static int v1_put(v1_map *m, const v1_ent *e)
{
    size_t k, i;
    if (!m->t) {
        m->t = (v1_ent *)calloc(1024, sizeof *m->t);
        if (!m->t) return -1;
        m->mask = 1023;
    } else if ((m->n + 1) * 10 >= (m->mask + 1) * 7) {
        size_t nc = (m->mask + 1) * 2;
        v1_ent *nt = (v1_ent *)calloc(nc, sizeof *nt);
        if (!nt) return -1;
        for (i = 0; i <= m->mask; i++) {
            if (m->t[i].live) {
                size_t j = (size_t)v1_mix(m->t[i].inode, m->t[i].lba) &
                           (nc - 1);
                while (nt[j].live) j = (j + 1) & (nc - 1);
                nt[j] = m->t[i];
            }
        }
        free(m->t);
        m->t = nt;
        m->mask = nc - 1;
    }
    k = (size_t)v1_mix(e->inode, e->lba) & m->mask;
    while (m->t[k].live) {
        if (m->t[k].inode == e->inode && m->t[k].lba == e->lba) {
            m->t[k] = *e;   /* newest wins */
            return 0;
        }
        k = (k + 1) & m->mask;
    }
    m->t[k] = *e;
    m->n++;
    return 0;
}

/* remove key: clear and rehash the cluster tail (no tombstones, so a
 * later lookup never stops short of a live entry behind the hole) */
static void v1_del(v1_map *m, uint64_t inode, uint64_t lba)
{
    size_t k, j;
    if (!m->t) return;
    k = (size_t)v1_mix(inode, lba) & m->mask;
    while (m->t[k].live) {
        if (m->t[k].inode == inode && m->t[k].lba == lba)
            break;
        k = (k + 1) & m->mask;
    }
    if (!m->t[k].live) return;
    m->t[k].live = 0;
    m->n--;
    j = (k + 1) & m->mask;
    while (m->t[j].live) {
        v1_ent e = m->t[j];
        m->t[j].live = 0;
        m->n--;
        k = (size_t)v1_mix(e.inode, e.lba) & m->mask;
        while (m->t[k].live) k = (k + 1) & m->mask;
        m->t[k] = e;
        m->n++;
        j = (j + 1) & m->mask;
    }
}

static const v1_ent *v1_get(const v1_map *m, uint64_t inode, uint64_t lba)
{
    size_t k;
    if (!m->t) return NULL;
    k = (size_t)v1_mix(inode, lba) & m->mask;
    while (m->t[k].live) {
        if (m->t[k].inode == inode && m->t[k].lba == lba)
            return &m->t[k];
        k = (k + 1) & m->mask;
    }
    return NULL;
}

/* apply one journaled entry (replay semantics) */
static int v1_apply(v1_map *m, const invfs_l2p_entry *e)
{
    if (e->type == INVFS_JRN_MAP) {
        v1_ent n;
        memset(&n, 0, sizeof n);
        n.inode = e->inode;
        n.lba = e->lba;
        n.pba = e->pba;
        n.len = e->length;
        n.rheat = (uint16_t)(e->pad[0] | ((uint16_t)e->pad[1] << 8));
        n.wheat = e->pad[2];
        n.live = 1;
        return v1_put(m, &n);
    }
    if (e->type == INVFS_JRN_UNMAP)
        v1_del(m, e->inode, e->lba);
    /* SWEEP/CHECKPOINT carry no mapping */
    return 0;
}

/* Parse the v1 journal (slot format when present, else the legacy flat
 * log) into the table. *seq_out takes the highest slot sequence seen
 * (0 when legacy). -1 on io/alloc failure; a torn slot falls back the
 * way the engine's replay does. */
static int v1_journal_load(cvio *cv, uint64_t jbase, v1_map *m,
                           uint64_t *seq_out)
{
    invfs_jrn_hdr h[2];
    int ok[2] = {0, 0};
    uint32_t s;
    int best = -1;
    uint64_t seq = 0;

    *seq_out = 0;
    for (s = 0; s < INVFS_JRN_SLOTS; s++) {
        uint64_t off = jbase + (uint64_t)s * INVFS_JRN_SLOT_BLOCKS *
                       INVFS_BLOCK_SIZE;
        if (cv_pread(cv, off, &h[s], sizeof h[s]) != 0)
            return -1;
        if (memcmp(h[s].magic, INVFS_JRN_MAGIC, 4) != 0 ||
            h[s].version != INVFS_JRN_VERSION ||
            h[s].image_bytes % sizeof(invfs_l2p_entry) != 0 ||
            h[s].image_bytes >
                (uint64_t)(INVFS_JRN_SLOT_BLOCKS - 1) * INVFS_BLOCK_SIZE ||
            invfs_crc32c(&h[s], offsetof(invfs_jrn_hdr, crc32c)) !=
                h[s].crc32c)
            continue;
        ok[s] = 1;
        if (h[s].seq > seq) seq = h[s].seq;
    }
    if (ok[0] && ok[1]) best = h[0].seq >= h[1].seq ? 0 : 1;
    else if (ok[0]) best = 0;
    else if (ok[1]) best = 1;
    *seq_out = seq;

    if (best >= 0) {
        /* the winning slot: image (whole-image CRC), then the chained
         * log until the chain breaks; a torn image falls back to the
         * other slot (it holds the full pre-compaction state) */
        int order[2];
        int oi;
        order[0] = best;
        order[1] = best ^ 1;
        for (oi = 0; oi < 2; oi++) {
            int sl = order[oi];
            uint64_t base, jend, jp, eoff;
            uint32_t prev;
            uint8_t *img;
            if (!ok[sl]) continue;
            base = jbase + (uint64_t)sl * INVFS_JRN_SLOT_BLOCKS *
                   INVFS_BLOCK_SIZE;
            jend = base + (uint64_t)INVFS_JRN_SLOT_BLOCKS *
                   INVFS_BLOCK_SIZE;
            jp = base + INVFS_BLOCK_SIZE;
            prev = invfs_crc32c(&h[sl], offsetof(invfs_jrn_hdr, image_crc));
            if (h[sl].image_bytes) {
                img = (uint8_t *)malloc((size_t)h[sl].image_bytes);
                if (!img) return -1;
                if (cv_pread(cv, jp, img, (size_t)h[sl].image_bytes) != 0 ||
                    invfs_crc32c(img, (size_t)h[sl].image_bytes) !=
                        h[sl].image_crc) {
                    free(img);
                    continue;
                }
                for (eoff = 0;
                     eoff + sizeof(invfs_l2p_entry) <= h[sl].image_bytes;
                     eoff += sizeof(invfs_l2p_entry)) {
                    const invfs_l2p_entry *e =
                        (const invfs_l2p_entry *)(img + eoff);
                    if (v1_apply(m, e) != 0) { free(img); return -1; }
                    prev = e->crc;
                }
                free(img);
                jp += h[sl].image_bytes;
            }
            while (jp + sizeof(invfs_l2p_entry) <= jend) {
                invfs_l2p_entry e;
                if (cv_pread(cv, jp, &e, sizeof e) != 0)
                    return -1;
                if (invfs_crc32c_update(prev, &e,
                                        offsetof(invfs_l2p_entry, crc)) !=
                        e.crc)
                    break;
                if (v1_apply(m, &e) != 0) return -1;
                prev = e.crc;
                jp += sizeof e;
            }
            return 0;
        }
        fprintf(stderr, "invf-migrate-v2: note: both journal slots failed "
                "verification; trying the legacy flat log\n");
    }

    /* legacy flat log: entries until the first bare-CRC failure */
    {
        uint64_t jp = jbase;
        uint64_t jend = jbase + (uint64_t)INVFS_JOURNAL_BLOCKS *
                        INVFS_BLOCK_SIZE;
        while (jp + sizeof(invfs_l2p_entry) <= jend) {
            invfs_l2p_entry e;
            if (cv_pread(cv, jp, &e, sizeof e) != 0)
                return -1;
            if (invfs_crc32c(&e, offsetof(invfs_l2p_entry, crc)) != e.crc)
                break;
            if (v1_apply(m, &e) != 0) return -1;
            jp += sizeof e;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * the v1 live set: per-name version stacks over the area order
 * (the engine's scanset rules minus brokenness -- a CRC-torn record is
 * skipped before it ever lands here, exactly like the open scan)
 * ------------------------------------------------------------------ */
typedef struct {
    uint64_t pos, id;
} v1_ver;

typedef struct {
    char     name[256];
    uint32_t nlen;
    v1_ver  *vers;
    uint32_t nvers, capvers;
} v1_name;

typedef struct {
    v1_name *n;
    size_t   count, cap;
} v1_live;

static v1_name *v1_find(v1_live *L, const char *name, uint32_t nlen)
{
    size_t i;
    for (i = 0; i < L->count; i++)
        if (L->n[i].nlen == nlen && memcmp(L->n[i].name, name, nlen) == 0)
            return &L->n[i];
    return NULL;
}

static int v1_inod(v1_live *L, const char *name, uint32_t nlen,
                   uint64_t id, uint64_t pos)
{
    v1_name *e = v1_find(L, name, nlen);
    if (!e) {
        if (L->count == L->cap) {
            size_t nc = L->cap ? L->cap * 2 : 256;
            v1_name *nn = (v1_name *)realloc(L->n, nc * sizeof *nn);
            if (!nn) return -1;
            L->n = nn;
            L->cap = nc;
        }
        e = &L->n[L->count++];
        memset(e, 0, sizeof *e);
        memcpy(e->name, name, nlen);
        e->nlen = nlen;
    }
    if (e->nvers == e->capvers) {
        uint32_t nc = e->capvers ? e->capvers * 2 : 4;
        v1_ver *nv = (v1_ver *)realloc(e->vers, nc * sizeof *nv);
        if (!nv) return -1;
        e->vers = nv;
        e->capvers = nc;
    }
    e->vers[e->nvers].pos = pos;
    e->vers[e->nvers].id = id;
    e->nvers++;
    return 0;
}

static void v1_delt(v1_live *L, const char *name, uint32_t nlen,
                    uint64_t id, uint64_t killpos)
{
    v1_name *e = v1_find(L, name, nlen);
    uint32_t i;
    if (!e || !e->nvers) return;
    if (!killpos) {
        /* legacy kill-by-id: a match on the newest version deletes the
         * name (older versions do not resurrect); otherwise it retires
         * exactly that superseded version */
        if (e->vers[e->nvers - 1].id == id) { e->nvers = 0; return; }
        for (i = 0; i < e->nvers; i++) {
            if (e->vers[i].id == id) {
                memmove(e->vers + i, e->vers + i + 1,
                        (e->nvers - i - 1) * sizeof *e->vers);
                e->nvers--;
                return;
            }
        }
        return;
    }
    for (i = 0; i < e->nvers; i++) {
        if (e->vers[i].pos == killpos) {
            memmove(e->vers + i, e->vers + i + 1,
                    (e->nvers - i - 1) * sizeof *e->vers);
            e->nvers--;
            return;
        }
    }
}

static void v1_live_free(v1_live *L)
{
    size_t i;
    for (i = 0; i < L->count; i++)
        free(L->n[i].vers);
    free(L->n);
    memset(L, 0, sizeof *L);
}

/* is this record position the live version of its name? */
static int v1_is_live(const v1_live *L, const char *name, uint32_t nlen,
                      uint64_t pos)
{
    v1_name *e = v1_find((v1_live *)L, name, nlen);
    return e && e->nvers && e->vers[e->nvers - 1].pos == pos;
}

/* ------------------------------------------------------------------ *
 * the "invfs.heat" TLV merge (local twin of the engine's heat_ext_merge:
 * old ext verbatim with the heat TLV inserted/replaced; a minimal
 * defaults ext is fabricated when the record has none and there is heat
 * to store)
 * ------------------------------------------------------------------ */
static uint8_t *cvt_ext_merge(const uint8_t *old_ext, uint32_t old_len,
                              int is_dir, uint16_t rheat, uint8_t wheat,
                              uint32_t *len_out)
{
    static const char HN[] = INVFS_XATTR_HEAT;
    const size_t hn = sizeof(HN) - 1;
    invfs_meta_ext_hdr h;
    const uint8_t *xattrs = NULL, *p;
    size_t xlen = 0, rem, keep = 0, out_len, tlen = 0;
    uint8_t *out, *w;

    memset(&h, 0, sizeof h);
    if (old_ext && old_len >= sizeof h) {
        memcpy(&h, old_ext, sizeof h);
        if (h.magic == INVFS_META_MAGIC && h.ext_len <= old_len &&
            (size_t)sizeof h + h.target_len <= h.ext_len) {
            xattrs = old_ext + sizeof h + h.target_len;
            xlen = h.ext_len - sizeof h - h.target_len;
        } else {
            old_ext = NULL;
        }
    }
    if (!old_ext) {
        h.type = is_dir ? INVFS_ITYP_DIR : INVFS_ITYP_REG;
        h.mode = is_dir ? 0755 : 0644;
        h.nlink = is_dir ? 2 : 1;
        xattrs = NULL;
        xlen = 0;
    }
    p = xattrs;
    rem = xlen;
    while (rem >= 4) {
        uint16_t nl, vl;
        size_t tsz;
        memcpy(&nl, p, 2);
        memcpy(&vl, p + 2 + nl, 2);
        tsz = (size_t)2 + nl + 2 + vl;
        if (tsz > rem || nl == 0) break;
        if (!(nl == hn && memcmp(p + 2, HN, hn) == 0))
            keep += tsz;
        p += tsz;
        rem -= tsz;
    }
    tlen = old_ext ? h.target_len : 0;
    out_len = sizeof h + tlen + keep + (2 + hn + 2 + 4);
    if (out_len > INVFS_META_SLACK) return NULL;
    out = (uint8_t *)malloc(out_len);
    if (!out) return NULL;
    if (old_ext) {
        memcpy(out, old_ext, sizeof h + tlen);  /* header+target verbatim */
        ((invfs_meta_ext_hdr *)out)->ext_len = (uint16_t)out_len;
    } else {
        h.magic = INVFS_META_MAGIC;
        h.version = 2;
        h.ext_len = (uint16_t)out_len;
        h.target_len = 0;
        memcpy(out, &h, sizeof h);
    }
    w = out + sizeof h + tlen;
    p = xattrs;
    rem = xlen;
    while (rem >= 4) {
        uint16_t nl, vl;
        size_t tsz;
        memcpy(&nl, p, 2);
        memcpy(&vl, p + 2 + nl, 2);
        tsz = (size_t)2 + nl + 2 + vl;
        if (tsz > rem || nl == 0) break;
        if (!(nl == hn && memcmp(p + 2, HN, hn) == 0)) {
            memcpy(w, p, tsz);
            w += tsz;
        }
        p += tsz;
        rem -= tsz;
    }
    {
        uint16_t nl16 = (uint16_t)hn, vl16 = 4;
        memcpy(w, &nl16, 2); w += 2;
        memcpy(w, HN, hn); w += hn;
        memcpy(w, &vl16, 2); w += 2;
        *w++ = (uint8_t)rheat;
        *w++ = (uint8_t)(rheat >> 8);
        *w++ = wheat;
        *w++ = 0;
    }
    *len_out = (uint32_t)out_len;
    return out;
}

/* ------------------------------------------------------------------ */

typedef struct {
    cvio      *cv;
    const v1_map *jm;
    uint64_t   total_blocks;
    uint8_t   *used;           /* the derived bitmap */
    /* running products */
    uint8_t   *stream;
    size_t     stream_len, stream_cap;
    invfs_l2p_entry *wal;      /* owner maps, in emission order */
    size_t     wal_n, wal_cap;
    uint64_t   missing;        /* live entries with no v1 map (refusal) */
    uint64_t   records;        /* converted records */
} cvt;

/* the derived extent of one converted entry, cross-checked against the
 * segment's framed header where it has one */
static int cvt_extent(cvio *cv, const char *name, uint64_t pba,
                      uint32_t map_len, uint64_t *plen_out)
{
    uint8_t hb[8];
    uint32_t csize;
    uint64_t plen;
    int framed = !((uint8_t)name[0] == 0x01 &&
                   (memcmp(name + 1, "parity", 6) == 0 ||
                    memcmp(name + 1, "reten", 5) == 0));
    if (!framed) {
        /* parity stripes (raw XOR blocks) and the retention registry
         * (raw block runs) carry no segment frame: the v1 map's length
         * is the whole story */
        if (!map_len) return -1;
        *plen_out = map_len;
        return 0;
    }
    if (cv_pread(cv, pba * (uint64_t)INVFS_BLOCK_SIZE, hb, 8) != 0)
        return -1;
    memcpy(&csize, hb, 4);
    if (!csize) return -1;
    plen = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    if (map_len && map_len != plen) {
        fprintf(stderr, "invf-migrate-v2: segment at pba %llu: the "
                "journal says %u blocks but the framed header says %llu "
                "-- the v1 volume disagrees with itself; refusing to "
                "convert (run the OLD build's invf-fsck -f first)\n",
                (unsigned long long)pba, map_len,
                (unsigned long long)plen);
        return -2;
    }
    *plen_out = plen;
    return 0;
}

/* convert one live v1 record: append the v2 form to the stream, its maps
 * (owner records) to the WAL image, and mark the derived bitmap.
 * 0 ok, -1 io/alloc, -2 conversion refusal (loud; volume untouched) */
static int cvt_one(cvt *c, uint64_t rec_pos)
{
    uint8_t *rec = NULL;
    invfs_inode_rec rh;
    uint32_t crc_stored;
    invfs_ast_hdr ah;
    size_t base = sizeof(invfs_inode_rec), ent0, choff, off;
    size_t ext_len = 0;
    const uint8_t *ext = NULL;
    uint8_t *newrec = NULL, *newext = NULL;
    size_t new_len, new_ext_len = 0;
    uint32_t i, crc;
    uint16_t rheat = 0;
    uint8_t wheat = 0;
    int is_owner, is_dir;
    int rc = -1;

    if (cv_pread(c->cv, rec_pos, &rh, sizeof rh) != 0)
        return -1;
    if (rh.rec_len < sizeof rh || rh.rec_len > INVFS_MAX_REC_LEN)
        return -1;
    rec = (uint8_t *)malloc(rh.rec_len);
    if (!rec) return -1;
    if (cv_pread(c->cv, rec_pos, rec, rh.rec_len) != 0 ||
        cv_pread(c->cv, rec_pos + rh.rec_len, &crc_stored, 4) != 0)
        goto out;
    if (invfs_crc32c(rec, rh.rec_len) != crc_stored)
        goto out;   /* torn record: the live set never names it */
    if (rh.rec_len < base + INVFS_AST_HDR_V1_LEN ||
        invfs_ast_hdr_parse(rec + base, rh.rec_len - base, &ah) != 0)
        goto out;
    ent0 = base + ah.hdr_len;
    if ((size_t)ah.num_blocks * sizeof(invfs_ast_block_entry_v1) >
        rh.rec_len - ent0)
        goto out;
    /* the children blob (v1 stride) ends where the ext begins */
    choff = ent0 + (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry_v1);
    off = choff;
    for (i = 0; i < ah.num_children; i++) {
        uint16_t nl;
        if (rh.rec_len - off < 2) goto out;
        memcpy(&nl, rec + off, 2);
        if (nl > MAX_AST_CHILD_NAME || rh.rec_len - off < (size_t)nl + 22)
            goto out;
        off += 2 + nl + 20;
    }
    if (rh.rec_len - off >= sizeof(invfs_meta_ext_hdr)) {
        invfs_meta_ext_hdr mh;
        memcpy(&mh, rec + off, sizeof mh);
        if (mh.magic == INVFS_META_MAGIC &&
            mh.ext_len <= rh.rec_len - off) {
            ext = rec + off;
            ext_len = mh.ext_len;
        }
    }

    is_owner = rh.name_len && rh.name[0] == 0x01;
    is_dir = rh.name_len && rh.name[rh.name_len - 1] == '/';

    /* the converted entries + the per-file heat fold */
    new_len = base + ah.hdr_len +
              (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry);
    if (new_len + (off - choff) + ext_len + 66 + 8 > INVFS_MAX_REC_LEN)
        goto out;
    /* capacity: the v1 body with the entries grown 24B -> 32B, plus room
     * for the ext outcome: carried (ext_len, inside rh.rec_len), merged
     * (+18 heat TLV), or FABRICATED whole (48B header + 18B TLV = 66),
     * plus the trailing CRC */
    newrec = (uint8_t *)calloc(1, rh.rec_len +
               (size_t)ah.num_blocks *
                   (sizeof(invfs_ast_block_entry) -
                    sizeof(invfs_ast_block_entry_v1)) + 66 + 8);
    if (!newrec) goto out;
    memcpy(newrec, rec, ent0);   /* rec header + recipe header verbatim */
    for (i = 0; i < ah.num_blocks; i++) {
        const invfs_ast_block_entry_v1 *e1 =
            (const invfs_ast_block_entry_v1 *)
            (rec + ent0 + (size_t)i * sizeof *e1);
        invfs_ast_block_entry *e2 = (invfs_ast_block_entry *)
            (newrec + ent0 + (size_t)i * sizeof *e2);
        const v1_ent *jm;
        uint64_t plen = 0, b, bend;
        int xrc;
        e2->file_offset = e1->file_offset;
        e2->length = e1->length;
        e2->zone = e1->zone;
        e2->algo = e1->algo;
        e2->block_id = e1->block_id;
        e2->block_offset = e1->block_offset;
        e2->pba = 0;
        if (!e1->length) continue;   /* no data behind this entry */
        jm = v1_get(c->jm, rh.inode_id, e1->block_id);
        if (!jm || !jm->pba || jm->pba >= c->total_blocks) {
            fprintf(stderr, "invf-migrate-v2: %.64s: segment %u has no "
                    "journal mapping (the v1 SPoF case) -- refusing to "
                    "convert; review with the OLD build's invf-fsck "
                    "first\n", rh.name, e1->block_id);
            c->missing++;
            continue;
        }
        xrc = cvt_extent(c->cv, rh.name, jm->pba, jm->len, &plen);
        if (xrc == -2) { rc = -2; goto out; }
        if (xrc != 0 || !plen || plen > c->total_blocks - jm->pba) {
            fprintf(stderr, "invf-migrate-v2: %.64s: segment %u at pba "
                    "%llu: extent underivable -- refusing to convert\n",
                    rh.name, e1->block_id, (unsigned long long)jm->pba);
            c->missing++;
            continue;
        }
        e2->pba = jm->pba;
        if (jm->rheat > rheat) rheat = jm->rheat;
        if (jm->wheat > wheat) wheat = jm->wheat;
        bend = jm->pba + plen;
        for (b = jm->pba; b < bend; b++)
            bit_set(c->used, b);
        if (is_owner) {
            invfs_l2p_entry *we;
            if (c->wal_n == c->wal_cap) {
                size_t nc = c->wal_cap ? c->wal_cap * 2 : 256;
                invfs_l2p_entry *nw = (invfs_l2p_entry *)
                    realloc(c->wal, nc * sizeof *nw);
                if (!nw) goto out;
                c->wal = nw;
                c->wal_cap = nc;
            }
            we = &c->wal[c->wal_n++];
            memset(we, 0, sizeof *we);
            we->type = INVFS_JRN_MAP;
            we->inode = rh.inode_id;
            we->lba = e1->block_id;
            we->pba = jm->pba;
            we->length = (uint32_t)plen;
        }
    }
    if (c->missing) { rc = -2; goto out; }

    /* the children blob verbatim */
    if (off > choff) {
        memcpy(newrec + new_len, rec + choff, off - choff);
        new_len += off - choff;
    }

    /* heat: carried only when the v1 volume had any (an absent TLV reads
     * as rheat 0 / wheat 1 = born-once -- exactly the no-history answer);
     * otherwise the ext rides verbatim */
    if (rheat || wheat) {
        uint32_t nl2 = 0;
        newext = cvt_ext_merge(ext, (uint32_t)ext_len, is_dir,
                               rheat, wheat ? wheat : 1, &nl2);
        if (!newext) goto out;
        new_ext_len = nl2;
    } else if (ext) {
        newext = (uint8_t *)malloc(ext_len);
        if (!newext) goto out;
        memcpy(newext, ext, ext_len);
        new_ext_len = ext_len;
    }
    if (newext) {
        memcpy(newrec + new_len, newext, new_ext_len);
        new_len += new_ext_len;
    }
    if (new_len > INVFS_MAX_REC_LEN) goto out;

    {
        invfs_inode_rec *nh = (invfs_inode_rec *)newrec;
        nh->rec_len = (uint32_t)new_len;
        crc = invfs_crc32c(newrec, new_len);
        memcpy(newrec + new_len, &crc, 4);
        new_len += 4;
    }
    if (c->stream_len + new_len > c->stream_cap) {
        size_t nc = c->stream_cap ? c->stream_cap : 1 << 20;
        uint8_t *ns;
        while (c->stream_len + new_len > nc) nc *= 2;
        ns = (uint8_t *)realloc(c->stream, nc);
        if (!ns) goto out;
        c->stream = ns;
        c->stream_cap = nc;
    }
    memcpy(c->stream + c->stream_len, newrec, new_len);
    c->stream_len += new_len;
    c->records++;
    rc = 0;
out:
    free(rec);
    free(newrec);
    free(newext);
    return rc;
}

/* first free run of >= need blocks in [lo, hi); 0 = none */
static uint64_t find_free_run(const uint8_t *bitmap, uint64_t lo,
                              uint64_t hi, uint64_t need)
{
    uint64_t i, run = 0, start = 0;
    for (i = lo; i < hi; i++) {
        if (!bit_get(bitmap, i)) {
            if (!run) start = i;
            if (++run >= need) return start;
        } else {
            run = 0;
        }
    }
    return 0;
}

static uint64_t div_ceil(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

int main(int argc, char **argv)
{
    const char *path;
    char devbuf[64];
    blkio io, io2;
    cvio cv;
    invfs_superblock sb;
    invfs_cvt0 cd;
    invfs_cvts sh;
    int rc, twodev = 0;
    uint64_t dev0_blocks = 0;
    char dev1_path[128];
    uint8_t *bitmap = NULL, *buf = NULL, *blk = NULL;
    uint64_t bm_blocks, bm_bytes, jbase, iarea, iarea_end;
    v1_map jm;
    v1_live live;
    uint64_t jrn_seq = 0;
    cvt c;
    uint64_t i, stage_start = 0, stage_blocks = 0, payload;
    uint64_t wal_bytes, stream_pad;
    int do_abort = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-migrate-v2 [--abort] <image|device>\n"
                            "  offline format v1 -> v2 conversion (WP27); the volume must be unmounted\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    if (argc == 3 && strcmp(argv[1], "--abort") == 0) {
        do_abort = 1;
        path = argv[2];
    } else if (argc == 2) {
        path = argv[1];
    } else {
        fprintf(stderr, "usage: invf-migrate-v2 [--abort] <image|device>\n"
                "  offline format v1 -> v2 conversion (WP27); the volume "
                "must be unmounted\n");
        return 2;
    }

    memset(&jm, 0, sizeof jm);
    memset(&live, 0, sizeof live);
    memset(&c, 0, sizeof c);
    memset(&cv, 0, sizeof cv);

    path = blkio_normalize(path, devbuf, sizeof devbuf);
    rc = blkio_open(&io, path,
                    blkio_looks_like_device(path) ? BLKIO_EXCLUSIVE : 0);
    if (rc != 0) {
        fprintf(stderr, "invf-migrate-v2: cannot open %s: %s\n",
                path, blkio_strerror(rc));
        return 1;
    }
    cv.io = &io;

    if (blkio_pread(&io, 0, &sb, sizeof sb) != 0 ||
        memcmp(sb.magic, INVFS_MAGIC, 8) != 0) {
        fprintf(stderr, "invf-migrate-v2: %s: not an InvariantFS volume\n",
                path);
        blkio_close(&io);
        return 1;
    }
    if (invfs_crc32c(&sb, offsetof(invfs_superblock, checksum)) !=
            sb.checksum) {
        fprintf(stderr, "invf-migrate-v2: %s: superblock checksum "
                "mismatch\n", path);
        blkio_close(&io);
        return 1;
    }

    bm_blocks = (sb.total_blocks / 8 + INVFS_BLOCK_SIZE - 1) /
                INVFS_BLOCK_SIZE;
    bm_bytes = bm_blocks * INVFS_BLOCK_SIZE;
    jbase = (sb.metadata_zone_start + bm_blocks) *
            (uint64_t)INVFS_BLOCK_SIZE;
    iarea = (sb.metadata_zone_start + bm_blocks + INVFS_JOURNAL_BLOCKS) *
            (uint64_t)INVFS_BLOCK_SIZE;
    iarea_end = (sb.metadata_zone_start + sb.metadata_zone_blocks) *
                (uint64_t)INVFS_BLOCK_SIZE;

    bitmap = (uint8_t *)malloc((size_t)bm_bytes);
    buf = (uint8_t *)malloc(BLKIO_BOUNCE);
    blk = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    if (!bitmap || !buf || !blk) {
        fprintf(stderr, "invf-migrate-v2: out of memory\n");
        goto fail;
    }

    /* ---- WP25: the device table (metadata writes mirror) ---- */
    memset(dev1_path, 0, sizeof dev1_path);
    {
        invfs_devt dt;
        memset(&dt, 0, sizeof dt);
        if (blkio_pread(&io, INVFS_DEVT_OFF, &dt, sizeof dt) == 0 &&
            memcmp(dt.magic, "DEVT", 4) == 0) {
            invfs_devt t = dt;
            t.crc32c = 0;
            if (invfs_crc32c(&t, sizeof t) == dt.crc32c &&
                dt.version == 1 && dt.dev_count == 2 &&
                dt.dev_blocks[0] && dt.dev_blocks[1] &&
                dt.dev_blocks[0] + dt.dev_blocks[1] == sb.total_blocks &&
                memcmp(dt.vol_uuid, sb.uuid, 16) == 0) {
                twodev = 1;
                dev0_blocks = dt.dev_blocks[0];
                memcpy(dev1_path, dt.dev1_hint, sizeof dev1_path - 1);
            } else {
                fprintf(stderr, "invf-migrate-v2: %s: torn DEVT table; "
                        "refusing\n", path);
                goto fail;
            }
        }
    }
    if (twodev) {
        const char *d1 = getenv("INVFS_DEV1");
        if (d1 && *d1) snprintf(dev1_path, sizeof dev1_path, "%s", d1);
        if (!dev1_path[0]) {
            fprintf(stderr, "invf-migrate-v2: %s: two-device volume; set "
                    "INVFS_DEV1 to device 1\n", path);
            goto fail;
        }
        rc = blkio_open(&io2, dev1_path,
                        blkio_looks_like_device(dev1_path) ?
                        BLKIO_EXCLUSIVE : 0);
        if (rc != 0) {
            fprintf(stderr, "invf-migrate-v2: cannot open device 1 %s: "
                    "%s\n", dev1_path, blkio_strerror(rc));
            goto fail;
        }
        cv.io2 = &io2;
        cv.dev0_bytes = dev0_blocks * (uint64_t)INVFS_BLOCK_SIZE;
        cv.meta_end = iarea_end;
    }

    /* ---- an armed conversion from a killed run takes precedence ---- */
    memset(&cd, 0, sizeof cd);
    if (cv_pread(&cv, INVFS_CVT0_OFF, &cd, sizeof cd) == 0 &&
        memcmp(cd.magic, "CVT0", 4) == 0) {
        {
            invfs_cvt0 t = cd;
            t.crc32c = 0;
            if (invfs_crc32c(&t, sizeof t) != cd.crc32c ||
                cd.version != 1 || !cd.stage_pba || !cd.stage_blocks ||
                cd.stage_pba + cd.stage_blocks > sb.total_blocks ||
                cd.stream_bytes > iarea_end - iarea ||
                cd.bm_bytes != bm_bytes) {
                fprintf(stderr, "invf-migrate-v2: %s: the CVT0 descriptor "
                        "is torn or names impossible bounds; refusing to "
                        "touch the volume\n", path);
                goto fail;
            }
        }
        if (do_abort) {
            /* Sound exactly because the apply writes the journal image
             * FIRST and the staged slot-0 header can never equal the v1
             * journal's head (a fresh sequence above every old slot; a
             * legacy v1 log has no JRN0 magic at all): a mismatch PROVES
             * no apply write ever landed. */
            uint8_t want[INVFS_BLOCK_SIZE], have[INVFS_BLOCK_SIZE];
            uint64_t joff = (cd.stage_pba + 1) *
                            (uint64_t)INVFS_BLOCK_SIZE +
                            div_ceil(cd.stream_bytes, INVFS_BLOCK_SIZE) *
                            INVFS_BLOCK_SIZE;
            if (cv_pread(&cv, joff, want, sizeof want) != 0 ||
                cv_pread(&cv, jbase, have, sizeof have) != 0) {
                fprintf(stderr, "invf-migrate-v2: abort check read "
                        "failed\n");
                goto fail;
            }
            if (memcmp(want, have, sizeof want) == 0) {
                fprintf(stderr, "invf-migrate-v2: the apply already "
                        "started; --abort cannot roll it back -- re-run "
                        "invf-migrate-v2 %s to finish the conversion\n",
                        path);
                goto fail;
            }
            {
                invfs_superblock csb = sb;
                uint8_t zb[sizeof cd];
                csb.state = INVFS_STATE_CLEAN;
                csb.checksum = invfs_crc32c(&csb,
                    offsetof(invfs_superblock, checksum));
                memset(zb, 0, sizeof zb);
                if (cv_pwrite(&cv, 0, &csb, sizeof csb) != 0 ||
                    cv_pwrite(&cv, INVFS_CVT0_OFF, zb, sizeof zb) != 0 ||
                    cv_flush(&cv) != 0) {
                    fprintf(stderr, "invf-migrate-v2: abort write "
                            "failed\n");
                    goto fail;
                }
            }
            printf("invf-migrate-v2: %s: conversion disarmed; the volume "
                   "is back to its pre-conversion v1 state (the staging "
                   "run is dead free space; fsck reclaims it)\n", path);
            blkio_close(&io);
            if (twodev) blkio_close(&io2);
            free(bitmap);
            free(buf);
            free(blk);
            return 0;
        }
        printf("invf-migrate-v2: %s: a previous conversion was "
               "interrupted; resuming the apply\n", path);
        goto apply;
    }
    if (do_abort) {
        fprintf(stderr, "invf-migrate-v2: %s: no conversion is armed "
                "(nothing to abort)\n", path);
        goto fail;
    }

    /* ---- refusal gates: offline, clean, no pinned positions ---- */
    if (sb.vol_flags & VOLF_ASTV2) {
        printf("invf-migrate-v2: %s: already format v2 -- nothing to do\n",
               path);
        blkio_close(&io);
        if (twodev) blkio_close(&io2);
        free(bitmap);
        free(buf);
        free(blk);
        return 0;
    }
    if (sb.state != INVFS_STATE_CLEAN) {
        fprintf(stderr, "invf-migrate-v2: %s: volume is not clean (state "
                "0x%02X) -- close it cleanly with the OLD build first\n",
                path, (unsigned)sb.state);
        goto fail;
    }
    if (sb.vol_flags & VOLF_READONLY) {
        fprintf(stderr, "invf-migrate-v2: %s: volume is read-only\n",
                path);
        goto fail;
    }
    {
        invfs_ckp0 ck;
        invfs_cmp0 cm;
        memset(&ck, 0, sizeof ck);
        memset(&cm, 0, sizeof cm);
        if (cv_pread(&cv, INVFS_CKP0_OFF, &ck, sizeof ck) == 0 &&
            memcmp(ck.magic, "CKP0", 4) == 0) {
            invfs_ckp0 t = ck;
            t.crc32c = 0;
            if (invfs_crc32c(&t, sizeof t) == ck.crc32c) {
                fprintf(stderr, "invf-migrate-v2: %s: a sweep checkpoint "
                        "is live (it pins absolute positions) -- resolve "
                        "it with the OLD build first (invf-rollback / "
                        "invf-sweep --realize)\n", path);
                goto fail;
            }
        }
        if (cv_pread(&cv, INVFS_CMP0_OFF, &cm, sizeof cm) == 0 &&
            memcmp(cm.magic, "CMP0", 4) == 0) {
            invfs_cmp0 t = cm;
            t.crc32c = 0;
            if (invfs_crc32c(&t, sizeof t) == cm.crc32c) {
                fprintf(stderr, "invf-migrate-v2: %s: an inode-area "
                        "compaction is pending -- roll it forward with "
                        "the OLD build's invf-fsck -f first\n", path);
                goto fail;
            }
        }
    }

    if (cv_pread(&cv, sb.metadata_zone_start * (uint64_t)INVFS_BLOCK_SIZE,
                 bitmap, (size_t)bm_bytes) != 0) {
        fprintf(stderr, "invf-migrate-v2: cannot read the block bitmap\n");
        goto fail;
    }

    /* ---- pass 1: the v1 journal table ---- */
    if (v1_journal_load(&cv, jbase, &jm, &jrn_seq) != 0) {
        fprintf(stderr, "invf-migrate-v2: cannot read the v1 journal\n");
        goto fail;
    }
    printf("  v1 journal: %zu live mapping%s\n", jm.n,
           jm.n == 1 ? "" : "s");

    /* ---- pass 2: the live set (area order, CRC-filtered) ---- */
    {
        uint64_t p = iarea;
        while (p + sizeof(invfs_inode_rec) <= iarea_end) {
            invfs_inode_rec rh;
            uint8_t *rb;
            uint32_t crc_stored, crc_calc;
            size_t nl;
            int good;
            if (cv_pread(&cv, p, &rh, sizeof rh) != 0)
                goto fail;
            if (rh.magic != INODE_REC_MAGIC &&
                rh.magic != TOMBSTONE_MAGIC)
                break;
            if (rh.rec_len < sizeof rh || rh.rec_len > INVFS_MAX_REC_LEN ||
                p + rh.rec_len + 4 > iarea_end)
                break;
            rb = (uint8_t *)malloc((size_t)rh.rec_len + 4);
            if (!rb) goto fail;
            if (cv_pread(&cv, p, rb, (size_t)rh.rec_len + 4) != 0) {
                free(rb);
                goto fail;
            }
            memcpy(&crc_stored, rb + rh.rec_len, 4);
            crc_calc = invfs_crc32c(rb, rh.rec_len);
            good = crc_calc == crc_stored;
            free(rb);
            if (good) {
                nl = rh.name_len < sizeof rh.name ?
                     rh.name_len : sizeof rh.name;
                if (nl) {
                    if (rh.magic == INODE_REC_MAGIC) {
                        if (v1_inod(&live, rh.name, (uint32_t)nl,
                                    rh.inode_id, p) != 0)
                            goto fail;
                    } else {
                        v1_delt(&live, rh.name, (uint32_t)nl,
                                rh.inode_id, rh.file_size);
                    }
                }
            }
            p += (uint64_t)rh.rec_len + 4;
        }
    }
    printf("  live set: %zu name%s\n", live.count,
           live.count == 1 ? "" : "s");

    /* ---- pass 3: convert the live records (area order), building the
     * v2 stream, the owner-WAL image and the derived bitmap ---- */
    c.cv = &cv;
    c.jm = &jm;
    c.total_blocks = sb.total_blocks;
    c.used = (uint8_t *)calloc(1, (size_t)bm_bytes);
    if (!c.used) goto fail;
    /* the derived bitmap's fixed spans (the fsck rebuild's rule) */
    for (i = 0; i < sb.metadata_zone_start + sb.metadata_zone_blocks; i++)
        bit_set(c.used, i);
    if (twodev)
        for (i = dev0_blocks; i < sb.shadow_zone_start; i++)
            bit_set(c.used, i);
    {
        uint64_t p = iarea;
        while (p + sizeof(invfs_inode_rec) <= iarea_end) {
            invfs_inode_rec rh;
            size_t nl;
            if (cv_pread(&cv, p, &rh, sizeof rh) != 0)
                goto fail;
            if (rh.magic != INODE_REC_MAGIC &&
                rh.magic != TOMBSTONE_MAGIC)
                break;
            if (rh.rec_len < sizeof rh || rh.rec_len > INVFS_MAX_REC_LEN ||
                p + rh.rec_len + 4 > iarea_end)
                break;
            if (rh.magic == INODE_REC_MAGIC && rh.name_len) {
                nl = rh.name_len < sizeof rh.name ?
                     rh.name_len : sizeof rh.name;
                if (v1_is_live(&live, rh.name, (uint32_t)nl, p) &&
                    cvt_one(&c, p) != 0)
                    goto fail;
            }
            p += (uint64_t)rh.rec_len + 4;
        }
    }
    if (c.missing) {
        fprintf(stderr, "invf-migrate-v2: the volume is untouched "
                "(%llu unresolvable segment reference%s)\n",
                (unsigned long long)c.missing,
                c.missing == 1 ? "" : "s");
        goto fail;
    }
    if (c.stream_len + INVFS_BLOCK_SIZE > iarea_end - iarea) {
        fprintf(stderr, "invf-migrate-v2: the converted stream (%llu "
                "bytes) does not fit the inode area (%llu bytes)\n",
                (unsigned long long)c.stream_len,
                (unsigned long long)(iarea_end - iarea));
        goto fail;
    }
    if (c.wal_n * sizeof(invfs_l2p_entry) >
        (uint64_t)(INVFS_JRN_SLOT_BLOCKS - 1) * INVFS_BLOCK_SIZE) {
        fprintf(stderr, "invf-migrate-v2: the owner WAL (%zu entries) "
                "exceeds a journal slot\n", c.wal_n);
        goto fail;
    }

    /* derived-bitmap sanity against the v1 one: every live-referenced
     * block must already be allocated in the v1 bitmap (a clean v1
     * volume property); the reverse direction is the reclaimed dead
     * weight (dropped record versions, freed-but-marked leftovers). */
    {
        uint64_t miss = 0, orph = 0, usedn = 0;
        for (i = 0; i < sb.total_blocks; i++) {
            int was = bit_get(bitmap, i), now = bit_get(c.used, i);
            if (now && !was) miss++;
            if (was && !now) orph++;
            if (now) usedn++;
        }
        printf("  derived bitmap: %llu blocks live (%llu reclaimed, %llu "
               "missing)\n", (unsigned long long)usedn,
               (unsigned long long)orph, (unsigned long long)miss);
        if (miss) {
            fprintf(stderr, "invf-migrate-v2: %llu referenced block(s) "
                    "are FREE in the v1 bitmap -- the v1 volume already "
                    "disagreed with itself; refusing to convert (run the "
                    "OLD build's invf-fsck -f first)\n",
                    (unsigned long long)miss);
            goto fail;
        }
    }

    /* ---- the WAL slot image (slot 0 format; the sequence above every
     * old slot so a stale sibling can never win the replay) ---- */
    wal_bytes = c.wal_n * sizeof(invfs_l2p_entry);
    {
        invfs_jrn_hdr jh;
        uint32_t prev;
        memset(blk, 0, INVFS_BLOCK_SIZE);
        memset(&jh, 0, sizeof jh);
        memcpy(jh.magic, INVFS_JRN_MAGIC, 4);
        jh.version = INVFS_JRN_VERSION;
        jh.seq = jrn_seq + 1;
        jh.image_bytes = wal_bytes;
        /* chain the entries FIRST (seed = crc over the header fields
         * preceding image_crc -- the engine's jrn_seed), then image_crc
         * covers the final bytes, then the header's own crc */
        prev = invfs_crc32c(&jh, offsetof(invfs_jrn_hdr, image_crc));
        for (i = 0; i < c.wal_n; i++) {
            c.wal[i].crc = invfs_crc32c_update(prev, &c.wal[i],
                offsetof(invfs_l2p_entry, crc));
            prev = c.wal[i].crc;
        }
        jh.image_crc = wal_bytes ? invfs_crc32c(c.wal, (size_t)wal_bytes)
                                 : 0;
        jh.crc32c = invfs_crc32c(&jh, offsetof(invfs_jrn_hdr, crc32c));
        memcpy(blk, &jh, sizeof jh);   /* the staged header block */
    }

    /* ---- staging layout: [CVTS block][stream, block-padded][journal
     * slot-0 image (header block + entries)][bitmap] ---- */
    stream_pad = div_ceil(c.stream_len, INVFS_BLOCK_SIZE) *
                 INVFS_BLOCK_SIZE;
    payload = stream_pad + INVFS_BLOCK_SIZE + wal_bytes + bm_bytes;
    stage_blocks = 1 + div_ceil(payload, INVFS_BLOCK_SIZE);
    stage_start = find_free_run(bitmap,
                    sb.metadata_zone_start + sb.metadata_zone_blocks,
                    sb.total_blocks, stage_blocks);
    if (!stage_start) {
        fprintf(stderr, "invf-migrate-v2: %s: no contiguous %llu-block "
                "free run to stage the conversion (%llu bytes)\n",
                path, (unsigned long long)stage_blocks,
                (unsigned long long)payload);
        goto fail;
    }

    printf("invf-migrate-v2: %s: %llu records -> %llu stream bytes, %zu "
           "WAL maps, staging %llu blocks at %llu\n",
           path, (unsigned long long)c.records,
           (unsigned long long)c.stream_len, c.wal_n,
           (unsigned long long)stage_blocks,
           (unsigned long long)stage_start);

    /* ---- phase 1: stage the products (payload first, header second),
     * verify by read-back BEFORE anything is armed ---- */
    {
        uint64_t off = (stage_start + 1) * (uint64_t)INVFS_BLOCK_SIZE;
        uint32_t pcrc = 0;
        uint64_t left, put;

        left = c.stream_len;
        put = 0;
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            memcpy(buf, c.stream + put, n);
            if (cv_pwrite(&cv, off, buf, n) != 0) {
                fprintf(stderr, "invf-migrate-v2: staging write failed "
                        "(stream)\n");
                goto fail;
            }
            pcrc = invfs_crc32c_update(pcrc, buf, n);
            off += n;
            put += n;
            left -= n;
        }
        /* zero-pad the stream to its block boundary (the guard) */
        if (stream_pad > c.stream_len) {
            uint64_t zl = stream_pad - c.stream_len;
            memset(buf, 0, BLKIO_BOUNCE);
            while (zl) {
                size_t n = zl > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)zl;
                if (cv_pwrite(&cv, off, buf, n) != 0) goto fail;
                pcrc = invfs_crc32c_update(pcrc, buf, n);
                off += n;
                zl -= n;
            }
        }
        /* the journal slot image: the header block, then the entries */
        pcrc = invfs_crc32c_update(pcrc, blk, INVFS_BLOCK_SIZE);
        if (cv_pwrite(&cv, off, blk, INVFS_BLOCK_SIZE) != 0) {
            fprintf(stderr, "invf-migrate-v2: staging write failed "
                    "(journal header)\n");
            goto fail;
        }
        off += INVFS_BLOCK_SIZE;
        left = wal_bytes;
        put = 0;
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            memcpy(buf, (const uint8_t *)c.wal + put, n);
            if (cv_pwrite(&cv, off, buf, n) != 0) goto fail;
            pcrc = invfs_crc32c_update(pcrc, buf, n);
            off += n;
            put += n;
            left -= n;
        }
        left = bm_bytes;
        put = 0;
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            memcpy(buf, c.used + put, n);
            if (cv_pwrite(&cv, off, buf, n) != 0) goto fail;
            pcrc = invfs_crc32c_update(pcrc, buf, n);
            off += n;
            put += n;
            left -= n;
        }

        memset(&sh, 0, sizeof sh);
        memcpy(sh.magic, "CVTS", 4);
        sh.version = 1;
        sh.stream_bytes = c.stream_len;
        sh.jrn_bytes = INVFS_BLOCK_SIZE + wal_bytes;   /* header + image */
        sh.bm_bytes = bm_bytes;
        sh.payload_crc = pcrc;
        sh.crc32c = invfs_crc32c(&sh, sizeof sh);
        memset(blk, 0, INVFS_BLOCK_SIZE);
        memcpy(blk, &sh, sizeof sh);
        if (cv_pwrite(&cv, stage_start * (uint64_t)INVFS_BLOCK_SIZE,
                      blk, INVFS_BLOCK_SIZE) != 0 ||
            cv_flush(&cv) != 0) {
            fprintf(stderr, "invf-migrate-v2: staging header write "
                    "failed\n");
            goto fail;
        }
    }

    /* verify the staging by read-back: header CRC + payload CRC */
    {
        invfs_cvts vh;
        uint64_t off = (stage_start + 1) * (uint64_t)INVFS_BLOCK_SIZE;
        uint64_t left = stream_pad + INVFS_BLOCK_SIZE + wal_bytes +
                        bm_bytes;
        uint32_t pcrc = 0;
        invfs_cvts vh0;
        if (cv_pread(&cv, stage_start * (uint64_t)INVFS_BLOCK_SIZE,
                     &vh, sizeof vh) != 0 ||
            memcmp(vh.magic, "CVTS", 4) != 0 ||
            (vh0 = vh, vh0.crc32c = 0,
             invfs_crc32c(&vh0, sizeof vh0) != vh.crc32c) ||
            vh.stream_bytes != c.stream_len ||
            vh.jrn_bytes != INVFS_BLOCK_SIZE + wal_bytes ||
            vh.bm_bytes != bm_bytes) {
            fprintf(stderr, "invf-migrate-v2: staging header "
                    "verification failed -- volume untouched\n");
            goto fail;
        }
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            if (cv_pread(&cv, off, buf, n) != 0) {
                fprintf(stderr, "invf-migrate-v2: staging read-back "
                        "failed\n");
                goto fail;
            }
            pcrc = invfs_crc32c_update(pcrc, buf, n);
            off += n;
            left -= n;
        }
        if (pcrc != vh.payload_crc) {
            fprintf(stderr, "invf-migrate-v2: staging payload CRC "
                    "mismatch -- volume untouched\n");
            goto fail;
        }
    }

#ifndef _WIN32
    if (abort_at("staged")) {
        cv_flush(&cv);
        kill(getpid(), SIGKILL);
    }
#endif

    /* ---- phase 2: arm (RECOVERY superblock, then the CVT0 descriptor) -- */
    {
        invfs_superblock rsb = sb;
        rsb.state = INVFS_STATE_RECOVERY;
        rsb.checksum = invfs_crc32c(&rsb,
                                    offsetof(invfs_superblock, checksum));
        if (cv_pwrite(&cv, 0, &rsb, sizeof rsb) != 0) {
            fprintf(stderr, "invf-migrate-v2: cannot mark RECOVERY\n");
            goto fail;
        }
        memset(&cd, 0, sizeof cd);
        memcpy(cd.magic, "CVT0", 4);
        cd.version = 1;
        cd.stage_pba = stage_start;
        cd.stage_blocks = stage_blocks;
        cd.stream_bytes = c.stream_len;
        cd.jrn_bytes = INVFS_BLOCK_SIZE + wal_bytes;
        cd.bm_bytes = bm_bytes;
        cd.jrn_seq = jrn_seq + 1;
        {
            invfs_cvt0 t = cd;
            t.crc32c = 0;
            cd.crc32c = invfs_crc32c(&t, sizeof t);
        }
        if (cv_pwrite(&cv, INVFS_CVT0_OFF, &cd, sizeof cd) != 0 ||
            cv_flush(&cv) != 0) {
            fprintf(stderr, "invf-migrate-v2: arm write failed\n");
            goto fail;
        }
    }
    printf("  armed: state=RECOVERY + CVT0 (stage %llu blocks at %llu)\n",
           (unsigned long long)stage_blocks,
           (unsigned long long)stage_start);

#ifndef _WIN32
    if (abort_at("armed")) {
        cv_flush(&cv);
        kill(getpid(), SIGKILL);
    }
#endif

apply:
    /* ---- phase 3: the apply (idempotent; reads only the staging) ----
     * Order: the journal image FIRST (the --abort proof keys on it),
     * then the inode stream, then the derived bitmap. */
    {
        uint64_t base = (cd.stage_pba + 1) * (uint64_t)INVFS_BLOCK_SIZE;
        uint64_t spad = div_ceil(cd.stream_bytes, INVFS_BLOCK_SIZE) *
                        INVFS_BLOCK_SIZE;
        uint64_t src, dst, left;
        uint32_t pcrc = 0;
        uint64_t vlen;
        invfs_cvts vh, vh0;

        /* re-verify the staging before overwriting anything (the RSZ0
         * rule): a clobbered stage can never parse */
        if (cv_pread(&cv, cd.stage_pba * (uint64_t)INVFS_BLOCK_SIZE,
                     &vh, sizeof vh) != 0 ||
            memcmp(vh.magic, "CVTS", 4) != 0 ||
            (vh0 = vh, vh0.crc32c = 0,
             invfs_crc32c(&vh0, sizeof vh0) != vh.crc32c) ||
            vh.stream_bytes != cd.stream_bytes ||
            vh.jrn_bytes != cd.jrn_bytes ||
            vh.bm_bytes != cd.bm_bytes) {
            fprintf(stderr, "invf-migrate-v2: staged conversion failed "
                    "verification; nothing was applied\n");
            goto fail;
        }
        vlen = spad + cd.jrn_bytes + cd.bm_bytes;
        src = base;
        while (vlen) {
            size_t n = vlen > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)vlen;
            if (cv_pread(&cv, src, buf, n) != 0) goto fail;
            pcrc = invfs_crc32c_update(pcrc, buf, n);
            src += n;
            vlen -= n;
        }
        if (pcrc != vh.payload_crc) {
            fprintf(stderr, "invf-migrate-v2: staged payload CRC "
                    "mismatch; nothing was applied\n");
            goto fail;
        }

        /* the journal: slot 0 gets the image; slot 1's header is killed
         * (a stale sibling must never win the replay) */
        src = base + spad;
        dst = jbase;
        left = cd.jrn_bytes;
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            if (cv_pread(&cv, src, buf, n) != 0 ||
                cv_pwrite(&cv, dst, buf, n) != 0) {
                fprintf(stderr, "invf-migrate-v2: apply failed "
                        "(journal)\n");
                goto fail;
            }
            src += n;
            dst += n;
            left -= n;
        }
        memset(buf, 0, BLKIO_BOUNCE);
        if (cv_pwrite(&cv, jbase + (uint64_t)INVFS_JRN_SLOT_BLOCKS *
                                 INVFS_BLOCK_SIZE,
                      buf, INVFS_BLOCK_SIZE) != 0)
            goto fail;

        /* the inode stream + a zero guard block at its end */
        src = base;
        dst = iarea;
        left = cd.stream_bytes;
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            if (cv_pread(&cv, src, buf, n) != 0 ||
                cv_pwrite(&cv, dst, buf, n) != 0) {
                fprintf(stderr, "invf-migrate-v2: apply failed "
                        "(stream)\n");
                goto fail;
            }
            src += n;
            dst += n;
            left -= n;
        }
        memset(buf, 0, BLKIO_BOUNCE);
        {
            uint64_t guard = spad - cd.stream_bytes;
            if (!guard) guard = INVFS_BLOCK_SIZE;
            while (guard) {
                size_t n = guard > BLKIO_BOUNCE ? BLKIO_BOUNCE
                                                : (size_t)guard;
                if (cv_pwrite(&cv, dst, buf, n) != 0) goto fail;
                dst += n;
                guard -= n;
            }
        }

        /* the derived bitmap */
        src = base + spad + cd.jrn_bytes;
        dst = sb.metadata_zone_start * (uint64_t)INVFS_BLOCK_SIZE;
        left = cd.bm_bytes;
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            if (cv_pread(&cv, src, buf, n) != 0 ||
                cv_pwrite(&cv, dst, buf, n) != 0) {
                fprintf(stderr, "invf-migrate-v2: apply failed "
                        "(bitmap)\n");
                goto fail;
            }
            src += n;
            dst += n;
            left -= n;
        }
        if (cv_flush(&cv) != 0) {
            fprintf(stderr, "invf-migrate-v2: apply flush failed\n");
            goto fail;
        }
    }

#ifndef _WIN32
    if (abort_at("applied")) {
        kill(getpid(), SIGKILL);
    }
#endif

    /* ---- phase 4: the commit -- one block-0 write carries the v2
     * superblock AND the cleared descriptor ---- */
    {
        invfs_superblock csb;
        if (cv_pread(&cv, 0, blk, INVFS_BLOCK_SIZE) != 0)
            goto fail;
        memcpy(&csb, blk, sizeof csb);
        csb.state = INVFS_STATE_CLEAN;
        csb.vol_flags |= VOLF_ASTV2;
        csb.pad2 = INVFS_JSEL_SLOT0;
        csb.checksum = invfs_crc32c(&csb,
                                    offsetof(invfs_superblock, checksum));
        memcpy(blk, &csb, sizeof csb);
        memset(blk + INVFS_CVT0_OFF, 0, sizeof cd);
        if (cv_pwrite(&cv, 0, blk, INVFS_BLOCK_SIZE) != 0 ||
            cv_flush(&cv) != 0) {
            fprintf(stderr, "invf-migrate-v2: commit write failed\n");
            goto fail;
        }
    }

    printf("invf-migrate-v2: %s: converted to format v2 (VOLF_ASTV2 set, "
           "journal slot 0 = owner WAL, %llu stream bytes); the staging "
           "run at %llu is dead free space -- invf-fsck -f reclaims it\n",
           path, (unsigned long long)cd.stream_bytes,
           (unsigned long long)cd.stage_pba);
    printf("invf-migrate-v2: OK\n");

    v1_live_free(&live);
    free(jm.t);
    free(c.stream);
    free(c.wal);
    free(c.used);
    free(bitmap);
    free(buf);
    free(blk);
    blkio_close(&io);
    if (twodev) blkio_close(&io2);
    return 0;

fail:
    v1_live_free(&live);
    free(jm.t);
    free(c.stream);
    free(c.wal);
    free(c.used);
    free(bitmap);
    free(buf);
    free(blk);
    blkio_close(&io);
    if (twodev) blkio_close(cv.io2);
    return 1;
}
