/* vol_fsck.c — fsck/repair: rebuild L2P + used-bitmap from the inode
 * area. Split from volume.c. */

#include "volume_internal.h"
#include "vol_metabuf.h"
#include "vol_btree.h"
#include "vol_delta.h"
#include "vol_reclaim.h"
#include "vol_spt0.h"


/* The batch accumulator and the real vol_tz_flush/vol_tz_gc live with the
 * rest of the WP10 write path, right after the storage-class helpers (they
 * need meta_rewrite/vol_stamp_class/vol_delete_inode). */

/* WP-M4: the v3 validation path (RT30 + base-tree walk). Defined after the
 * v2 helpers; vol_fsck_scan dispatches to it for VOLF_V3 volumes. WP86 added
 * the containment walk and the -f repair behind the same entry point. */
static int fsck_v3_scan(invfs_volume *v, invfs_fsck_report *rep, int fix);

/* ================= fsck / repair =================
 * WP27 note on physical addressing: AST entries store the segment's pba
 * (24B -> 32B); the physical extent derives from the segment's framed
 * header (seg_extent). The L2P journal is the owner-scoped WAL only. So a
 * destroyed WAL is fully rebuildable from the owner records, and the
 * bitmap is a rebuildable cache: fsck recomputes it from
 * (metadata zone + live records' extents + live owner WAL), complete.
 * What fsck does:
 *   - verify inode-record CRCs and record lengths (bad_recs)
 *   - verify that every live AST entry carries a valid pba (l2p_miss --
 *     the field name kept for the report; under v2 it means "entry with
 *     an invalid pba or an underivable extent")
 *   - WP22d: fold the live set to the consistent cut (a record with an
 *     invalid-pba entry is hidden; the name falls back to its newest
 *     valid version) and, with -f, QUARANTINE the broken versions
 *     (position-kill tombstones, newest first) so the cut is permanent
 *   - rebuild the used-bitmap from (metadata zone + live records' extents
 *     + live owner WAL): orphans (allocated, unreferenced) are freed,
 *     missing (referenced, free in bitmap) are restored
 *   - rewrite the WAL from the live owner records (journal compact)
 *   - mark the superblock CLEAN
 * With fix=1 all fixes are applied and persisted. */

/* WP27: an entry's physical extent, by owner class. The reten registry's
 * length field carries BLOCKS (the ranges are raw blocks, no frame);
 * seal parity is one block; the tier/rawm copies carry the span in bytes;
 * everything else is a framed segment (the header read derives it). */
static int fsck_entry_extent(invfs_volume *v, const char *name,
                             size_t name_len,
                             const invfs_ast_block_entry *e,
                             uint64_t *plen_out)
{
    if (!e->pba || e->pba >= v->sb.total_blocks) return -1;
    if (name_len >= 6 && name[0] == 0x01 && !memcmp(name + 1, "reten", 5)) {
        if (!e->length || e->length > v->sb.total_blocks) return -1;
        *plen_out = e->length;         /* BLOCKS (the reten rule) */
        return 0;
    }
    if (name[0] == 0x01 && memcmp(name + 1, "tzb", 3) != 0 &&
        e->length && e->length % INVFS_BLOCK_SIZE == 0) {
        *plen_out = e->length / INVFS_BLOCK_SIZE;   /* parity/tier/rawm */
        return 0;
    }
    return seg_extent(v, e->pba, NULL, plen_out);
}

static int fsck_rebuild_one(invfs_volume *v, uint64_t rec_pos, uint64_t inode_id,
                            invfs_l2p_entry **l2p, size_t *n, size_t *cap,
                            uint8_t *used, size_t used_bytes,
                            invfs_fsck_report *rep,
                            const char *name, size_t name_len)
{
    invfs_inode_rec rh;
    invfs_ast_hdr ast_h;
    uint8_t *rec = NULL;
    uint32_t crc_stored, crc_calc;
    size_t off;
    uint32_t i;
    int is_owner = name_len && name[0] == 0x01;

    (void)used; (void)used_bytes;
    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, &rh, sizeof(rh)) != 0) return -1;
    if (rh.rec_len < INVFS_REC_HDR_LEN + 1 ||
        rh.rec_len > INVFS_MAX_REC_LEN) return -1;
    rec = (uint8_t *)malloc(rh.rec_len);
    if (!rec) return -1;
    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, rec, rh.rec_len) != 0 ||
        io_read(&v->io, &crc_stored, 4) != 0) { free(rec); return -1; }
    crc_calc = invfs_crc32c(rec, rh.rec_len);
    if (crc_calc != crc_stored) { rep->bad_recs++; free(rec); return 0; }

    off = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)rec) - rec);
    if (off + INVFS_AST_HDR_V1_LEN > rh.rec_len ||
        invfs_ast_hdr_parse(rec + off, rh.rec_len - off, &ast_h) != 0) {
        free(rec);
        return -1;
    }
    off += ast_h.hdr_len;

    for (i = 0; i < ast_h.num_blocks; i++) {
        invfs_ast_block_entry e;
        uint64_t plen = 0;
        if (off + sizeof(e) > rh.rec_len) { rep->bad_recs++; break; }
        memcpy(&e, rec + off, sizeof(e));
        off += sizeof(e);
        /* every live entry must carry a valid pba (post-cut this cannot
         * fire for a live record; kept as the safety net) */
        if (e.length == 0) continue;
        if (!e.pba || e.pba >= v->sb.total_blocks) {
            rep->l2p_miss++;
            continue;
        }
        if (fsck_entry_extent(v, name, name_len, &e, &plen) != 0 ||
            !plen || plen > v->sb.total_blocks - e.pba) {
            /* the extent is underivable (a torn segment header): that is
             * CONTENT damage -- verify --deep's domain, not a mapping
             * loss (the pba is valid, the record stays live, reads fail
             * loudly at the segment CRC). Keep the whole contiguous
             * allocated run the segment sits on (its true span; the
             * seal-repair pass below may still heal it, and a shorter pin
             * would free live tail blocks out from under it). */
            uint64_t b;
            plen = 0;
            for (b = e.pba; b < v->sb.total_blocks &&
                            bit_get(v->bitmap, b); b++)
                plen++;
            if (!plen) plen = 1;   /* fully freed already: pin the head */
        }
        /* mark used (physical) */
        {
            uint64_t b;
            for (b = e.pba; b < e.pba + plen && b < v->sb.total_blocks; b++)
                bit_set(used, b);
        }
        /* owner records rebuild the WAL: keep the entry for the journal
         * rewrite (file records carry their own pbas -- nothing to keep) */
        if (is_owner) {
            if (*n == *cap) {
                size_t ncap = *cap ? *cap * 2 : 256;
                invfs_l2p_entry *nl = (invfs_l2p_entry *)realloc(*l2p, ncap * sizeof(invfs_l2p_entry));
                if (!nl) { free(rec); return -1; }
                *l2p = nl; *cap = ncap;
            }
            (*l2p)[*n].type = INVFS_JRN_MAP;
            (*l2p)[*n].inode = inode_id;
            (*l2p)[*n].lba = e.block_id;
            (*l2p)[*n].pba = e.pba;
            (*l2p)[*n].length = (uint32_t)plen;
            memset((*l2p)[*n].pad, 0, sizeof (*l2p)[*n].pad);
            (*l2p)[*n].crc = 0;
            (*n)++;
        }
    }
    free(rec);
    return 0;
}


/* ---- live-record set ----------------------------------------------------
 * Liveness must mirror the consistent cut vol_open applies (the scan-set
 * in volume.c) EXACTLY, or fsck and the read path disagree about the same
 * volume -- the WP22c/F2 lesson. The set is built with per-name version
 * stacks: an INOD with an invalid-pba entry is BROKEN and never live (the
 * name falls back to its newest valid version); a position-kill DELT
 * removes its version unless that version's successor is broken (a torn
 * retire keeps its fallback). */

/* Append a quarantine tombstone (v2 position-kill) for one broken record.
 * The record stays in the append-only area but every future scan drops it
 * from the name's version stack, which is what makes the cut permanent.
 * WP70: route through vol_append_slot so mapper volumes update both
 * met0.active_offset and inode_area_pos -- a bare io_write at
 * inode_area_pos left met0.active_offset stale, and the next vol_open
 * reset inode_area_pos from met0.active_offset, trimming the active
 * extent before the tombstone and making the walk miss it. */
static int fsck_quarantine(invfs_volume *v, const char *name,
                           uint64_t killpos, uint64_t id)
{
    invfs_inode_rec *rec;
    uint8_t *rb;
    uint32_t crc;
    uint64_t pos;
    uint8_t *old = NULL;
    uint32_t orl = 0;
    size_t nlen = strlen(name);
    size_t rlen;

    if (nlen > INVFS_MAX_NAME) nlen = INVFS_MAX_NAME;
    rlen = INVFS_REC_HDR_LEN + nlen + 1;   /* name + NUL, no body */
    if (vol_append_slot(v, rlen + 4, &pos) != 0)
        return -1;
    rb = (uint8_t *)calloc(1, rlen);
    if (!rb) return -1;
    rec = (invfs_inode_rec *)rb;
    rec->magic = TOMBSTONE_MAGIC;
    v->hot.tombstones++;
    rec->rec_len = (uint32_t)rlen;
    rec->inode_id = id;
    rec->file_size = killpos;      /* v2 position kill */
    rec_set_name(rec, name);
    crc = invfs_crc32c(rb, (uint32_t)rlen);
    if (io_seek(&v->io, pos) != 0 ||
        io_write(&v->io, rb, (uint32_t)rlen) != 0 ||
        io_write(&v->io, &crc, 4) != 0) {
        free(rb);
        return -1;
    }
    /* WP27: the killed record's pba references leave the map (the bitmap
     * rebuild below decides their fate from the SURVIVING records) */
    if (meta_read_record_by_id(v, id, &old, &orl, NULL, 0, NULL) == 0) {
        pba_ref_apply(v, old, orl, -1);
        free(old);
    }
    free(rb);
    return 0;
}

/* WP22d/H6 repair: a name with no fully-valid version is not lost whole
 * when the invalid entries form a SUFFIX of the newest record -- repair
 * by truncation to the longest valid prefix: append a new record
 * version with the tail entries dropped, then position-kill the old one.
 * The readable head survives and the volume reaches CLEAN (quarantine
 * would destroy data that is still fine). Returns 0 when truncated,
 * 1 when the damage is not a clean suffix (caller quarantines),
 * -1 on io/alloc failure. */
static int fsck_truncate_suffix(invfs_volume *v,
                                const char *name, uint64_t pos,
                                uint64_t id, uint64_t *out_size,
                                uint64_t *out_pos)
{
    invfs_inode_rec rh;
    invfs_ast_hdr ah;
    uint8_t *rec = NULL, *nr = NULL, *w;
    size_t off, ext_off = 0, ext_len = 0, ent0, body;
    uint32_t i, k;
    uint64_t new_size, newp = 0;

    if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &rh, sizeof rh) != 0)
        return -1;
    if (rh.magic != INODE_REC_MAGIC || rh.rec_len < INVFS_REC_HDR_LEN + 1 ||
        rh.rec_len > INVFS_MAX_REC_LEN)
        return 1;
    rec = (uint8_t *)malloc(rh.rec_len);
    if (!rec) return -1;
    if (io_seek(&v->io, pos) != 0 || io_read(&v->io, rec, rh.rec_len) != 0)
        { free(rec); return -1; }
    off = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)rec) - rec);
    if (invfs_ast_hdr_parse(rec + off, rh.rec_len - off, &ah) != 0)
        { free(rec); return 1; }
    if (ah.num_children != 0) { free(rec); return 1; }  /* container: quarantine */
    ent0 = off + ah.hdr_len;
    /* first invalid-pba entry; everything from it on must be invalid too
     * (a clean suffix) -- a mid-file hole stays a quarantine case */
    for (k = 0; k < ah.num_blocks; k++) {
        invfs_ast_block_entry e;
        memcpy(&e, rec + ent0 + (size_t)k * sizeof e, sizeof e);
        if (e.length && (!e.pba || e.pba >= v->sb.total_blocks)) break;
    }
    if (k == 0 || k == ah.num_blocks) { free(rec); return 1; }
    for (i = k; i < ah.num_blocks; i++) {
        invfs_ast_block_entry e;
        memcpy(&e, rec + ent0 + (size_t)i * sizeof e, sizeof e);
        if (!e.length || (e.pba && e.pba < v->sb.total_blocks))
            { free(rec); return 1; }
    }
    {
        invfs_ast_block_entry e;
        memcpy(&e, rec + ent0 + (size_t)k * sizeof e, sizeof e);
        new_size = e.file_offset;
    }
    if (new_size == 0) { free(rec); return 1; }
    off = ent0 + (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry);
    if (v->sb.vol_flags & VOLF_META2) { ext_off = off; ext_len = rh.rec_len - off; }
    /* the recipe header follows invfs_ast_hdr_write's own v1/v2 pick */
    {
        size_t hl = (new_size > 0xFFFFFFFFu || k > 0xFFFFu)
                    ? INVFS_AST_HDR_V2_LEN : INVFS_AST_HDR_V1_LEN;
        size_t nlen = ((const invfs_inode_rec *)rec)->name_len;
        size_t present = (size_t)rh.rec_len - INVFS_REC_HDR_LEN - 1;
        uint32_t crc;
        uint64_t p;
        if (nlen > INVFS_MAX_NAME) nlen = INVFS_MAX_NAME;
        if (nlen > present) nlen = present;
        body = INVFS_REC_HDR_LEN + nlen + 1 + hl +
               (size_t)k * sizeof(invfs_ast_block_entry) + ext_len;
        if (vol_append_slot(v, body + 4, &p) != 0)
            { free(rec); return -1; }
        nr = (uint8_t *)malloc(body);
        if (!nr) { free(rec); return -1; }
        memset(nr, 0, body);
        memcpy(nr, rec, INVFS_REC_HDR_LEN + nlen + 1);  /* header + name */
        ((invfs_inode_rec *)nr)->name_len = (uint32_t)nlen;
        nr[INVFS_REC_HDR_LEN + nlen] = 0;
        ((invfs_inode_rec *)nr)->rec_len = (uint32_t)body;
        ((invfs_inode_rec *)nr)->file_size = new_size;
        w = nr + INVFS_REC_HDR_LEN + nlen + 1;
        if (invfs_ast_hdr_write(w, new_size, k, 0) != hl)
            { free(rec); free(nr); return -1; }
        w += hl;
        memcpy(w, rec + ent0, (size_t)k * sizeof(invfs_ast_block_entry));
        w += (size_t)k * sizeof(invfs_ast_block_entry);
        if (ext_len) memcpy(w, rec + ext_off, ext_len);
        crc = invfs_crc32c(nr, (uint32_t)body);
        p = v->inode_area_pos;
        if (io_seek(&v->io, p) != 0 ||
            io_write(&v->io, nr, (uint32_t)body) != 0 ||
            io_write(&v->io, &crc, 4) != 0)
            { free(rec); free(nr); return -1; }
        newp = p;
        pba_ref_apply(v, nr, (uint32_t)body, +1);
    }
    free(rec); free(nr);
    *out_pos = newp;   /* the appended truncated record (pre-tombstone) */
    if (fsck_quarantine(v, name, pos, id) != 0) return -1;  /* kill old ver */
    *out_size = new_size;
    return 0;
}

/* WP22d content-level cut (fsck -f + INVFS_FSCK_CONTENT=1): read every
 * non-TEXT segment of the record at rec_pos through the CRC'd path and
 * mirror the read path's per-algo shape checks. A segment whose bytes
 * were torn by a drop window AFTER its record landed fails here: the
 * pba-level cut cannot see it, the segment CRC + csize checks can.
 * Returns 1 when any segment fails. */
static int rec_content_bad(invfs_volume *v, uint64_t rec_pos,
                           uint64_t inode_id)
{
    invfs_inode_rec rh;
    invfs_ast_hdr ah;
    uint8_t *rec, *scratch = NULL;
    size_t off;
    uint32_t i;
    int bad = 0;

    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, &rh, sizeof rh) != 0)
        return 1;
    if (rh.rec_len < INVFS_REC_HDR_LEN + 1 ||
        rh.rec_len > INVFS_MAX_REC_LEN)
        return 1;
    rec = (uint8_t *)malloc(rh.rec_len);
    if (!rec) return 1;
    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, rec, rh.rec_len) != 0) { free(rec); return 1; }
    off = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)rec) - rec);
    if (off + INVFS_AST_HDR_V1_LEN > rh.rec_len ||
        invfs_ast_hdr_parse(rec + off, rh.rec_len - off, &ah) != 0) {
        free(rec); return 1;
    }
    off += ah.hdr_len;
    for (i = 0; i < ah.num_blocks && !bad; i++) {
        invfs_ast_block_entry e;
        uint64_t pba = 0;
        uint32_t csize = 0;
        uint8_t *blob = NULL;
        if (off + sizeof(e) > rh.rec_len) { bad = 1; break; }
        memcpy(&e, rec + off, sizeof(e));
        off += sizeof(e);
        if (e.zone == INVFS_ZONE_TEXT)
            continue;   /* batch members: the tz layer's own integrity */
        pba = e.pba;
        if (!pba || pba >= v->sb.total_blocks)
            continue;   /* invalid: the pba-level cut already counts it */
        if (seg_read_checked(v, pba, 0, 1, &csize, &blob) != 0) {
            bad = 1;
        } else if (e.algo == INVFS_ALGO_NONE) {
            /* RAW: the frame must cover exactly the logical segment */
            if ((uint64_t)csize != e.length) bad = 1;
        } else if (e.algo == INVFS_ALGO_LZ4 || e.algo == INVFS_ALGO_ZSTD) {
            /* compressed: must inflate to exactly the logical size */
            if (csize == 0 || (uint64_t)csize >= e.length) {
                bad = 1;
            } else {
                if (!scratch) scratch = (uint8_t *)malloc(INVFS_SEGMENT_SIZE);
                if (!scratch) { free(blob); bad = 1; }
                else if (e.algo == INVFS_ALGO_LZ4) {
                    int d = LZ4_decompress_safe((const char *)blob,
                                                (char *)scratch,
                                                (int)csize,
                                                INVFS_SEGMENT_SIZE);
                    if (d != (int)e.length) bad = 1;
                } else {
                    size_t d = ZSTD_decompress(scratch, e.length,
                                               blob, csize);
                    if (ZSTD_isError(d) || d != e.length) bad = 1;
                }
            }
        }
        /* other algos are whole-file blobs (a container part, a swept
         * recipe): csize is unrelated to e.length; seg_read_checked's
         * payload CRC is the check that applies */
        free(blob);
        if (bad) break;
    }
    free(scratch);
    free(rec);
    return bad;
}


/* WP49: mapper-volume pass-1 body fed by the bounded vol_records_walk_ex
 * (position-driven vol_inode_next can cycle on a non-monotonic mapper
 * table). bad_cb keeps fsck's torn/CRC-bad record accounting. */
typedef struct {
    invfs_volume *v;
    invfs_fsck_report *rep;
    scan_set *live;
} fscan_ctx;

static int fscan_cb(void *ctx_, uint64_t pos,
                    const invfs_inode_rec *rh, const uint8_t *rec)
{
    fscan_ctx *c = (fscan_ctx *)ctx_;
    size_t present = (size_t)rh->rec_len - INVFS_REC_HDR_LEN - 1;
    size_t nl = rh->name_len < INVFS_MAX_NAME
              ? rh->name_len : INVFS_MAX_NAME;
    if (nl > present) nl = present;
    if (!nl) return 0;
    if (rh->magic == INODE_REC_MAGIC) {
        uint64_t moff[4], mlen[4];
        unsigned mn = 0;
        uint64_t miss = rec_pba_miss(rec, rh->rec_len, c->v->sb.total_blocks,
                                     moff, mlen, &mn);
        if (scanset_inod(c->live, rh->name, nl, rh->inode_id, pos,
                         rh->file_size, rh->ctime, miss) != 0)
            return 1;
    } else {
        /* v2 tombstones kill by record position; legacy ones by id */
        scanset_delt(c->live, rh->name, nl, rh->inode_id, rh->file_size);
    }
    return 0;
}

static void fscan_bad(void *ctx_, uint64_t pos)
{
    fscan_ctx *c = (fscan_ctx *)ctx_;
    (void)pos;
    c->rep->bad_recs++;
}

int vol_fsck_scan(invfs_volume *v, invfs_fsck_report *rep, int fix)
{
    uint64_t pos, end;
    size_t bi;
    size_t l2p_n = 0, l2p_cap = 0;
    invfs_l2p_entry *newl2p = NULL;
    uint8_t *used = NULL;
    size_t used_bytes;
    scan_set live = { NULL, 0, 0 };

    memset(rep, 0, sizeof(*rep));
    /* WP-M4: a format-v3 volume carries no v2 inode-record stream / owner
     * WAL, so the v2 rebuild below does not apply. The v3 checker validates
     * the RT30 root descriptor and walks the base B+-tree instead, and WP86
     * gave it a repair (the quarantined key ranges are excised, the delta is
     * folded back in, the loss is named). v2 behavior is untouched when
     * VOLF_V3 is clear.
     *
     * Known gap (NOT WP86, reported as a follow-up): the v2-only passes below
     * -- the content cut (fix + INVFS_FSCK_CONTENT=1) and the seal2 repair
     * (--repair) -- do not run on a v3 volume, so a data segment whose bytes
     * were torn after its recipe landed cannot be quarantined there yet. */
    if (v->sb.vol_flags & VOLF_V3)
        return fsck_v3_scan(v, rep, fix);
    used_bytes = (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
    used = (uint8_t *)calloc(1, used_bytes);
    if (!used) return -1;

    /* pass 1: ordered replay of the area -- the name-keyed live set, with
     * the consistent cut applied per record (broken = some AST segment has
     * no mapping in the replayed journal).
     * WP30: on a mapper volume the records live in dynamic extents, so the
     * walk goes through vol_inode_next (mapper-aware). On a legacy volume
     * the full metadata-zone tail scan applies unchanged. */
    int mapper_walk = v->met0_present && v->meta_mapper;
    /* Legacy range: the whole metadata-zone tail (incl. unflushed tail). */
    end = (v->sb.metadata_zone_start + v->sb.metadata_zone_blocks)
          * INVFS_BLOCK_SIZE;
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;

    if (mapper_walk) {
        /* WP49: index-ordered, bounded, cycle-proof; torn records are
         * reported through bad_cb instead of silently skipped. */
        fscan_ctx fc;
        fc.v = v; fc.rep = rep; fc.live = &live;
        if (vol_records_walk_ex(v, fscan_cb, &fc, fscan_bad) != 0) {
            free(used); scanset_free(&live);
            return -1;
        }
    } else {
    for (;;) {
        invfs_inode_rec rh;
        uint32_t crc_stored, crc_calc;
        uint8_t *rec = NULL;
        if (pos + INVFS_REC_HDR_LEN > end) break;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &rh, sizeof(rh)) != 0) {
            break;
        }
        if (rh.magic != INODE_REC_MAGIC && rh.magic != TOMBSTONE_MAGIC) {
            break;
        }
        if (rh.rec_len < INVFS_REC_HDR_LEN + 1 ||
            pos + rh.rec_len + 4 > end) {
            rep->bad_recs++;
            break;
        }
        rec = (uint8_t *)malloc(rh.rec_len);
        if (!rec) { free(used); scanset_free(&live); return -1; }
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, rec, rh.rec_len) != 0 ||
            io_read(&v->io, &crc_stored, 4) != 0) {
            free(rec); free(used); scanset_free(&live);
            return -1;
        }
        crc_calc = invfs_crc32c(rec, rh.rec_len);
        if (crc_calc != crc_stored) {
            /* corrupt record: report, skip past it, keep scanning */
            rep->bad_recs++;
            free(rec);
            pos += (uint64_t)rh.rec_len + 4;
            continue;
        }
        {
            /* the name lives in the full record buffer rec, not in the
             * 36-byte prefix copy rh */
            const invfs_inode_rec *rr = (const invfs_inode_rec *)rec;
            size_t present = (size_t)rh.rec_len - INVFS_REC_HDR_LEN - 1;
            size_t nl = rr->name_len < INVFS_MAX_NAME
                      ? rr->name_len : INVFS_MAX_NAME;
            if (nl > present) nl = present;
            if (rh.magic == INODE_REC_MAGIC) {
                if (nl) {
                    uint64_t moff[4], mlen[4];
                    unsigned mn = 0;
                    uint64_t miss = rec_pba_miss(rec, rh.rec_len,
                                                 v->sb.total_blocks,
                                                 moff, mlen, &mn);
                    if (scanset_inod(&live, rr->name, nl, rh.inode_id, pos,
                                     rh.file_size, rh.ctime,
                                     miss) != 0) {
                        free(rec); free(used); scanset_free(&live);
                        return -1;
                    }
                }
            } else {
                /* v2 tombstones kill by record position; legacy ones by
                 * id -- the scan-set applies them with the consistent-cut
                 * rules (a kill whose successor is broken is skipped) */
                if (nl)
                    scanset_delt(&live, rr->name, nl, rh.inode_id,
                                 rh.file_size);
            }
        }
        free(rec);
        pos += (uint64_t)rh.rec_len + 4;
    }
    }

    /* pass 1b: the cut accounting + (fix) quarantine. Per name: the live
     * version is the newest non-broken one; broken versions NEWER than it
     * are torn writes -- a name with a fallback is "cut" (regressed), a
     * name without one is lost entirely (counted in l2p_miss, listed with
     * its lost ranges). Broken versions older than the live one are silent
     * history (already superseded in the healthy timeline). */
    if (live.buck) {
        for (bi = 0; bi <= live.mask; bi++) {
            scan_name *e;
            for (e = live.buck[bi]; e; e = e->next) {
                const scan_ver *lv = scanset_live(e);
                uint32_t first_broken;
                /* index of the first version newer than the live one */
                if (lv)
                    first_broken = (uint32_t)(lv - e->vers) + 1;
                else
                    first_broken = 0;
                if (first_broken < e->nvers) {
                    /* torn suffix exists: report (the newest carries the
                     * loss detail) */
                    uint32_t newest = e->nvers - 1;
                    uint64_t miss = e->vers[newest].miss;
                    if (lv) {
                        rep->cut_records++;
                        fprintf(stderr,
                                "fsck: l2p cut: %s (inode %llu): newest "
                                "record @%llu lost %llu segment(s); fell "
                                "back to @%llu%s\n",
                                e->name,
                                (unsigned long long)e->vers[newest].id,
                                (unsigned long long)e->vers[newest].pos,
                                (unsigned long long)miss,
                                (unsigned long long)lv->pos,
                                fix ? " (quarantined)" : "");
                    } else {
                        /* H6 format: name the file and the lost ranges,
                         * loudly and on stderr -- data loss is never a
                         * quiet event. With fix, decide the repair FIRST
                         * (suffix damage truncates to the longest mapped
                         * prefix; anything else quarantines), then print
                         * the outcome. */
                        uint64_t moff[4], mlen[4];
                        unsigned mn = 0, k;
                        uint8_t *rec2 = NULL;
                        invfs_inode_rec rh;
                        rep->lost_files++;
                        rep->l2p_miss += miss;
                        if (io_seek(&v->io, e->vers[newest].pos) == 0 &&
                            io_read(&v->io, &rh, sizeof rh) == 0 &&
                            rh.rec_len >= INVFS_REC_HDR_LEN + 1 &&
                            rh.rec_len <= INVFS_MAX_REC_LEN &&
                            (rec2 = malloc(rh.rec_len)) != NULL &&
                            io_seek(&v->io, e->vers[newest].pos) == 0 &&
                            io_read(&v->io, rec2, rh.rec_len) == 0)
                            rec_pba_miss(rec2, rh.rec_len,
                                         v->sb.total_blocks,
                                         moff, mlen, &mn);
                        free(rec2);
                        fprintf(stderr, "fsck: l2p_miss: %s (inode %llu): "
                                "%llu segment(s) with an invalid pba, "
                                "lost:",
                                e->name,
                                (unsigned long long)e->vers[newest].id,
                                (unsigned long long)miss);
                        for (k = 0; k < mn; k++)
                            fprintf(stderr, " [%llu, %llu)",
                                    (unsigned long long)moff[k],
                                    (unsigned long long)(moff[k] + mlen[k]));
                        if (miss > mn)
                            fprintf(stderr, " ... and %llu more",
                                    (unsigned long long)(miss - mn));
                        fprintf(stderr, "%s\n",
                                fix ? " (repair below)"
                                    : " (use -f to repair)");
                    }
                    if (fix) {
                        /* Repair the torn suffix, newest first (the
                         * scan-set's kill-skip rule needs each victim's
                         * successor already gone). A no-fallback name whose
                         * damage is a clean suffix TRUNCATES to the longest
                         * fully-mapped prefix instead of being quarantined
                         * (the readable head survives, the volume reaches
                         * CLEAN). Mark the volume dirty first so a crash
                         * mid-repair cannot read as CLEAN; fsck -f IS the
                         * recovery, so this bypasses vol_mark_dirty's
                         * needs_recovery refusal (that latch is exactly
                         * what is being repaired -- the rebuild tail below
                         * writes CLEAN). */
                        uint32_t q;
                        int truncated = 0;
                        if (!v->dirty) {
                            v->sb.state = INVFS_STATE_DIRTY;
                            if (vol_write_sb(v) != 0) {
                                free(used); scanset_free(&live);
                                
                                free(newl2p);
                                return -1;
                            }
                            v->dirty = 1;
                        }
                        if (!lv) {
                            uint64_t nsz = 0, npos = 0;
                            int trc = fsck_truncate_suffix(
                                v, e->name, e->vers[e->nvers - 1].pos,
                                e->vers[e->nvers - 1].id, &nsz, &npos);
                            if (trc < 0) {
                                free(used); scanset_free(&live);
                                free(newl2p);
                                return -1;
                            }
                            if (trc == 0) {
                                truncated = 1;
                                /* the scan-set drives the rebuild: register
                                 * the truncated version as live (healthy,
                                 * miss=0) or its segments are dropped from
                                 * the rebuilt journal and the file vanishes */
                                if (scanset_inod(&live, e->name, e->nlen,
                                                 e->vers[e->nvers - 1].id,
                                                 npos, nsz,
                                                 e->vers[e->nvers - 1].ctime,
                                                 0) != 0) {
                                    free(used); scanset_free(&live);
                                    free(newl2p);
                                    return -1;
                                }
                                fprintf(stderr, "fsck: l2p_miss: %s: "
                                        "truncated to %llu bytes "
                                        "(readable head kept)\n",
                                        e->name, (unsigned long long)nsz);
                            }
                        }
                        for (q = e->nvers; !truncated && q-- > first_broken; ) {
                            if (fsck_quarantine(v, e->name,
                                                e->vers[q].pos,
                                                e->vers[q].id) != 0) {
                                fprintf(stderr, "fsck: quarantine of %s "
                                        "@%llu failed (inode area full?)\n",
                                        e->name,
                                        (unsigned long long)e->vers[q].pos);
                                free(used); scanset_free(&live);
                                
                                free(newl2p);
                                return -1;
                            }
                        }
                        if (fix && !lv && !truncated)
                            fprintf(stderr, "fsck: l2p_miss: %s: file "
                                    "quarantined\n", e->name);
                    }
                }
            }
        }
    }
    /* pass 1c (fsck -f + INVFS_FSCK_CONTENT=1): the content-level
     * consistent cut. A segment's DATA can be dropped by a window while
     * its map survives (the map is re-durabilized by every compaction;
     * the data is written once), leaving a live record whose bytes fail
     * the segment CRC. Per name, settle the live version = the newest
     * version that is BOTH map-clean and content-clean, quarantining the
     * whole torn suffix above it (newest first, so the scan-set's
     * kill-skip rule is never tripped). Cost is a full data read --
     * recovery-time only, off unless the knob is set (the flakey ladders
     * set it when verify --deep reports CORRUPT). */
    if (fix && live.buck && getenv("INVFS_FSCK_CONTENT")) {
        for (bi = 0; bi <= live.mask; bi++) {
            scan_name *e;
            for (e = live.buck[bi]; e; e = e->next) {
                int64_t live_i = -1, i;
                for (i = (int64_t)e->nvers; i-- > 0; ) {
                    if (e->vers[i].broken)
                        continue;            /* map-broken: never live */
                    if (rec_content_bad(v, e->vers[i].pos, e->vers[i].id)) {
                        e->vers[i].broken = 1;   /* content-broken */
                        rep->corrupt_files++;
                        fprintf(stderr,
                                "fsck: content cut: %s (inode %llu): "
                                "record @%llu fails segment CRC\n",
                                e->name, (unsigned long long)e->vers[i].id,
                                (unsigned long long)e->vers[i].pos);
                        continue;
                    }
                    live_i = i;
                    break;
                }
                /* quarantine the torn suffix above the settled live
                 * version (pass 1b already took the map-broken part that
                 * was visible without content; a position-kill is
                 * idempotent, so an overlap is harmless) */
                for (i = (int64_t)e->nvers; i-- > live_i + 1; ) {
                    if (!v->dirty) {
                        v->sb.state = INVFS_STATE_DIRTY;
                        if (vol_write_sb(v) != 0) {
                            free(used); scanset_free(&live);
                            free(newl2p);
                            return -1;
                        }
                        v->dirty = 1;
                    }
                    if (fsck_quarantine(v, e->name, e->vers[i].pos,
                                        e->vers[i].id) != 0) {
                        fprintf(stderr, "fsck: quarantine of %s @%llu "
                                "failed (inode area full?)\n", e->name,
                                (unsigned long long)e->vers[i].pos);
                        free(used); scanset_free(&live);
                        free(newl2p);
                        return -1;
                    }
                }
            }
        }
    }

    /* pass 2: per live record, verify AST<->L2P and collect used blocks */
    if (live.buck) {
        for (bi = 0; bi <= live.mask; bi++) {
            scan_name *e;
            for (e = live.buck[bi]; e; e = e->next) {
                const scan_ver *lv = scanset_live(e);
                if (!lv) continue;
                rep->live_files++;
                if (fsck_rebuild_one(v, lv->pos, lv->id,
                                     &newl2p, &l2p_n, &l2p_cap,
                                     used, used_bytes, rep,
                                     e->name, e->nlen) != 0) {
                    free(used); scanset_free(&live);
                    free(newl2p);
                    return -1;
                }
            }
        }
    }
    scanset_free(&live);

    /* metadata zone is always allocated */
    {
        uint64_t meta_end = v->sb.metadata_zone_start + v->sb.metadata_zone_blocks;
        uint64_t b;
        for (b = 0; b < meta_end; b++)
            bit_set(used, b);
    }

    /* WP30 mapper volumes: the inode records live in dynamic metadata
     * extents allocated from the shadow zone (alloc_meta_extent, class
     * INVFS_ALLOC_META), NOT in the metadata zone and NOT as a live
     * record's AST data. The rebuild above derives used blocks from live
     * records' AST extents only, so a mapper extent whose records were all
     * superseded/tombstoned (the "\x01reten" retention registry is the
     * canonical case: written, then deleted by --realize) was counted as
     * an orphan even though the extent is still registered in the mapper
     * and allocated. Mark every live mapper extent. */
    if (v->met0_present && v->meta_mapper) {
        size_t mi;
        for (mi = 0; mi < v->meta_mapper_n; mi++) {
            uint64_t entry = v->meta_mapper[mi];
            uint64_t mpba, mblocks, mb;
            if (!entry) continue;
            mpba = invfs_meta_ext_pba(entry);
            mblocks = invfs_meta_ext_size(entry) / INVFS_BLOCK_SIZE;
            if (!mpba || mpba >= v->sb.total_blocks) continue;
            if (mblocks > v->sb.total_blocks - mpba)
                mblocks = v->sb.total_blocks - mpba;
            for (mb = 0; mb < mblocks; mb++)
                bit_set(used, mpba + mb);
        }
    }

    /* WP25: on a two-device volume the dev1 span below the shadow zone --
     * [dev0_total, shadow_zone_start) -- is the reserved metadata mirror
     * (and never holds allocatable data); it is allocated by construction,
     * exactly like the metadata zone itself. */
    if (v->ndev == 2) {
        uint64_t b;
        for (b = v->dev0_blocks; b < v->sb.shadow_zone_start; b++)
            bit_set(used, b);
    }

    /* WP21/WP22d: a live sweep checkpoint owns its journal staging run
     * (referenced by the CKP0 descriptor, not by any AST/L2P entry), so it
     * must not count as orphans. fsck -f is refused while a checkpoint is
     * live, so this only ever affects reporting. */
    if (v->ck_present && v->ck.stage_blocks) {
        uint64_t b;
        for (b = 0; b < v->ck.stage_blocks; b++)
            bit_set(used, v->ck.stage_pba + b);
    }

    /* compare bitmaps: orphans = in v->bitmap, not in used; missing = reverse */
    {
        uint64_t total = v->sb.total_blocks, i;
        for (i = 0; i < total; i++) {
            int bm = bit_get(v->bitmap, i);
            int us = bit_get(used, i);
            if (bm && !us && v->ck_present) rep->held_ckpt++;
            else if (bm && !us) rep->orphans++;
            if (bm && !us && getenv("INVFS_DEBUG"))
                fprintf(stderr, "[fsck] %s block %llu\n",
                        v->ck_present ? "held" : "orphan",
                        (unsigned long long)i);
            if (us && !bm) rep->missing++;
        }
    }

    rep->l2p_entries = l2p_n;

    /* repair when there is structural damage, or when the volume is merely
     * dirty: a scan that found nothing else is exactly a clean-close
     * simulation, so rewriting bitmap/journal and setting CLEAN is safe.
     * H6/WP22d: l2p_miss no longer blocks the repair -- the mappings are
     * gone from the journal and cannot be reconstructed, but refusing the
     * journal rewrite kept the whole VOLUME read-only (a DIRTY state
     * mounts read-only) over damage that is per-file. The repair proceeds
     * around them: the torn versions were quarantined above (the affected
     * names are listed, with their lost ranges), the live set is the
     * consistent cut, and the volume mounts RW again after the fix. */
    if (fix && (rep->orphans || rep->missing || rep->bad_recs ||
                rep->l2p_miss || rep->cut_records || rep->lost_files ||
                rep->corrupt_files ||
                v->sb.state != INVFS_STATE_CLEAN)) {
        /* WAL rewrite: exactly the live owner records' maps (drops stale
         * entries of records fsck could not verify). WP27: an empty owner
         * set still replaces the table -- a volume without owners has an
         * empty WAL. */
        free(v->l2p);
        v->l2p = newl2p;
        v->l2p_count = l2p_n;
        v->l2p_cap = l2p_n;
        newl2p = NULL;
        /* WP-L2Q: the table was replaced wholesale -- the session
         * index re-derives from it */
        l2p_idx_rebuild(v);
        /* WP27: the reference map's world just changed wholesale */
        pba_ref_reset(v);
        memcpy(v->bitmap, used, used_bytes);
        v->free_blocks = vol_count_free(v);
        alloc_state_reset(v);   /* bitmap replaced: per-zone counters stale */
        /* WP25: the rebuild replaced the bitmap wholesale (no
         * vol_free_blocks calls), so the tier/mirror second copies were
         * never re-validated: drop any whose canonical key or copy blocks
         * fell out of the rebuilt allocation map. */
        wp25_fsck_prune(v);
        /* H5: the rebuild may have reclaimed enough to release a space
         * latch the volume was carrying */
        vol_readonly_unlatch(v);
        /* WP20b: the whole occupancy map may have changed -- every seal
         * stripe's membership is now unproven, force a full reseal */
        seal_dirty_reset(v);
        /* alloc_state_reset marks the bitmap clean, which holds on mount but
         * not here: every byte may differ from disk, so flush all of it. */
        v->bm_lo = 0;
        v->bm_hi = (uint64_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
        /* WP22d: the table was replaced wholesale -- the next flush
         * compacts (atomic double-buffer) instead of appending */
        v->jops_n = 0;
        v->j_compact = 1;
        if (vol_flush(v) != 0) {
            free(used); free(newl2p);
            return -1;
        }
        v->sb.state = INVFS_STATE_CLEAN;
        if (vol_flush(v) != 0) {
            free(used); free(newl2p);
            return -1;
        }
        /* the volume is rebuilt and barriered: it IS recovered. Clear the
         * latch so the close path takes the ordinary final flush instead
         * of reporting a latched io error that never happened. */
        v->needs_recovery = 0;
    }
    free(used);
    free(newl2p);
    return 0;
}

/* ================= WP-M4/WP86: metadata-v3 fsck ==========================
 * A v3 volume's namespace lives in the immutable base B+-tree anchored by the
 * RT30 root descriptor (design-meta-v3.md §7/§12/§16). This checker validates
 * the root double-slot and walks every reachable base page.
 *
 * A page whose blkptr.checksum/gen does not match its bytes is a hard read
 * error (mbuf_read_ptr), exactly like the read path -- it is not repairable
 * and the tree keeps no second copy of it. WP-M4 stopped the walk there, which
 * made one torn page fatal twice over: the report counted ONE bad page (the
 * pages behind it were never walked) and `fix` did nothing, so the volume
 * stayed broken forever.
 *
 * WP86 CONTAINS the damage instead:
 *
 *   report  btree_check_tolerant skips the unreadable page, records the key
 *           range it owned, and keeps verifying the rest of the tree, so the
 *           report names every bad page and every quarantined key range. A
 *           key inside a quarantined range reads EIO on the read path
 *           (mbuf_root_read / btree_search already refuse a bad page): it is
 *           never presented as absent, and never as data.
 *   -f      the quarantined ranges are EXCISED (btree_excise): the parent
 *           entry that points at the unreadable page is dropped, every other
 *           key keeps its page, and the delta is folded back in -- which
 *           restores every quarantined key the delta still holds. What is left
 *           -- keys that existed only in the damaged page -- is gone, and the
 *           pass says so with the exact key range, the key count it could
 *           not recover, and the NAMES it could attribute. The damage is
 *           never silenced: the pass that finds it always exits nonzero
 *           (the v2 -f contract), and a *structural* failure is refused
 *           outright rather than "repaired".
 *
 * What no repair can do: a torn ROOT page with no valid sibling slot leaves
 * the base tree unreachable. Only the delta is readable then, and rebuilding
 * the base from the delta alone would silently drop every key that was folded
 * before it -- so -f refuses, loudly, and says what is still readable. */
static void fsck_v3_note(invfs_fsck_report *rep, const char *msg)
{
    rep->v3_damaged = 1;
    fprintf(stderr, "fsck(v3): %s\n", msg);
}

/* Validate the RT30 descriptor itself (magic/version/page_size/CRC). Returns
 * 0 = valid, 1 = absent (an empty base, the RDP0 convention),
 * 2 = WP86: present but torn -- damage, NOT an empty base; the caller must
 * not walk a tree it cannot name, -1 = io error. On success v->rt30 is
 * populated via mbuf_rt30_load. */
static int fsck_v3_rt30(invfs_volume *v, invfs_fsck_report *rep)
{
    int rc = mbuf_rt30_load(v);
    if (rc < 0) {
        fsck_v3_note(rep, "RT30 root descriptor unreadable (io error)");
        return -1;
    }
    if (rc == 1) {
        rep->v3_rt30_bad = 1;
        fsck_v3_note(rep, "RT30 root descriptor absent -- the base was "
                          "never written (empty)");
        return 1;
    }
    if (rc == 2) {
        rep->v3_rt30_bad = 1;
        fsck_v3_note(rep, "RT30 root descriptor is TORN (version/CRC) -- the "
                          "base tree it names is unreachable; only the delta "
                          "(recent writes) is readable");
        return 2;
    }
    rep->v3_root_seq = v->rt30.seq;
    if (v->rt30.page_size != INVFS_BLOCK_SIZE) {
        char b[128];
        rep->v3_rt30_bad = 1;
        snprintf(b, sizeof b, "RT30 page_size=%u is not the supported "
                 "%u-byte page (D3)",
                 (unsigned)v->rt30.page_size, (unsigned)INVFS_BLOCK_SIZE);
        fsck_v3_note(rep, b);
        return 1;
    }
    if (v->rt30.delta_pba && v->rt30.delta_pba >= v->sb.total_blocks) {
        fsck_v3_note(rep, "RT30 delta_pba is out of range");
    }
    return 0;
}

/* Read a root slot page and check it against the RT30 slot value. A slot
 * naming a page that does not validate is "torn". Returns 0 = valid (fills
 * *gen_out), 1 = empty slot, 2 = torn, -1 = io error.
 *
 * TODO(WP-M4): RT30 stores only a pba per slot, not a blkptr, so the WP
 * doc's "blkptr.checksum does not match" cannot be checked literally; the
 * page's own self-CRC is the available check. A slot-level checksum/gen
 * would catch a slot repointed at a different-but-valid page, and belongs
 * with the root-publish format if the design wants it. */
static int fsck_v3_slot_page(invfs_volume *v, uint64_t pba,
                             uint64_t *gen_out)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    const invfs_page_hdr *h;
    if (!pba)
        return 1;
    if (pba >= v->sb.total_blocks)
        return 2;               /* out of range: torn/foreign slot */
    if (mbuf_read(v, pba, page) != 0)
        return -1;
    h = mbuf_page_chdr(page);
    if (!mbuf_page_validate(page))
        return 2;
    if (gen_out)
        *gen_out = h->gen;
    return 0;
}

/* Build a verified blkptr (pba + the page's own checksum/gen + root flags) for
 * a page that RT30 or SPT0 names by pba alone. 0 = ok, -1 = the page does not
 * read or does not validate. A blkptr whose checksum/gen are left zero does NOT
 * work here: mbuf_read_ptr compares them against the page header, so it would
 * reject every valid page. */
static int fsck_v3_ptr_at(invfs_volume *v, uint64_t pba, invfs_blkptr *out)
{
    uint8_t page[INVFS_BLOCK_SIZE];

    memset(out, 0, sizeof *out);
    if (!pba || mbuf_read(v, pba, page) != 0)
        return -1;
    if (!mbuf_page_validate(page))
        return -1;
    mbuf_ptr_set(out, pba, page,
                 mbuf_page_chdr(page)->level == INVFS_PAGE_LEVEL_LEAF
                 ? INVFS_BP_ROOT | INVFS_BP_LEAF
                 : INVFS_BP_ROOT | INVFS_BP_INTERNAL);
    return 0;
}

/* Select the winning root from the RT30 double slot. A slot is a candidate
 * only when its page validates; among candidates the higher header gen wins,
 * tie -> the slot seq parity points at (the most recently published). A
 * non-empty slot whose page does not validate is torn and reported. Returns
 * 0 = ok (possibly empty, *root_out.pba == 0), -1 = io error. */
static int fsck_v3_root(invfs_volume *v, invfs_fsck_report *rep,
                        invfs_blkptr *root_out)
{
    uint64_t best_pba = 0, best_gen = 0;
    uint8_t page[INVFS_BLOCK_SIZE];
    int have = 0, i;
    memset(root_out, 0, sizeof *root_out);

    for (i = 0; i < 2; i++) {
        uint64_t pba = v->rt30.root_slot[i];
        uint64_t gen = 0;
        int rc = fsck_v3_slot_page(v, pba, &gen);
        if (rc == 1)
            continue;               /* empty slot */
        if (rc < 0)
            return -1;
        if (rc == 2) {
            char b[160];
            rep->v3_slots_torn++;
            snprintf(b, sizeof b, "root_slot[%d] pba %llu is torn "
                     "(bad page CRC/magic) -- slot ignored",
                     i, (unsigned long long)pba);
            fsck_v3_note(rep, b);
            continue;
        }
        if (!have || gen > best_gen) {
            have = 1;
            best_pba = pba;
            best_gen = gen;
        } else if (gen == best_gen) {
            /* Both slots validate at the same gen: the publication order is
             * not observable, so the choice is ambiguous (design §7). The
             * seq parity names the most recently published slot; flag it. */
            rep->v3_slots_ambiguous++;
            fsck_v3_note(rep, "both RT30 root slots valid at the same gen "
                              "(ambiguous publish; seq parity used)");
            if ((uint32_t)i == (uint32_t)(v->rt30.seq & 1u))
                best_pba = pba;
        }
    }
    if (!have)
        return 0;                   /* empty base: clean */
    /* Re-read the winning page to seed its blkptr checksum/flags. */
    if (mbuf_read(v, best_pba, page) != 0)
        return -1;
    mbuf_ptr_set(root_out, best_pba, page,
                 mbuf_page_chdr(page)->level == INVFS_PAGE_LEVEL_LEAF
                 ? INVFS_BP_ROOT | INVFS_BP_LEAF
                 : INVFS_BP_ROOT | INVFS_BP_INTERNAL);
    return 0;
}

/* Allocator cross-check: a reachable page must be marked allocated in the
 * metadata bitmap (the v2 "bitmap divergence" analogue). */
static void fsck_v3_bitmap_check(invfs_volume *v, invfs_fsck_report *rep,
                                 uint64_t pba)
{
    if (v->bitmap && !bit_get(v->bitmap, pba)) {
        char b[128];
        rep->v3_reachable_free++;
        snprintf(b, sizeof b, "reachable base page pba %llu is FREE in the "
                 "metadata bitmap", (unsigned long long)pba);
        fsck_v3_note(rep, b);
    }
}

/* The repair's lost-name scan needs the volume handle in a callback. */
static invfs_volume *g_fsck_v;

/* WP86: how many quarantined key ranges hold a key the delta still covers.
 * Those are the ones the repair gets BACK (the fold re-applies them); every
 * other key in a quarantined range existed only in the unreadable page and is
 * gone. Counted before the fold, from the delta index. */
typedef struct {
    const bt_quarantine *q;
    uint64_t recovered;
} fsck_dq_ctx;

static int fsck_delta_in_q_cb(void *ctx_, const uint8_t *key, uint16_t klen,
                              const delta_ref *ref)
{
    fsck_dq_ctx *c = (fsck_dq_ctx *)ctx_;
    int i;
    (void)ref;
    for (i = 0; i < c->q->n; i++) {
        const bt_range *r = &c->q->range[i];
        int ge_lo = 1, lt_hi = 1;
        /* byte-lexicographic compare against the range bounds; keys are short,
         * so a memcmp on the common prefix plus a length tie-break is exact */
        uint16_t m;
        int cc;
        if (r->lo_n) {
            m = klen < r->lo_n ? klen : r->lo_n;
            cc = m ? memcmp(key, r->lo, m) : 0;
            if (cc < 0 || (cc == 0 && klen < r->lo_n))
                ge_lo = 0;
        }
        if (ge_lo && r->hi_n) {
            m = klen < r->hi_n ? klen : r->hi_n;
            cc = m ? memcmp(key, r->hi, m) : 0;
            if (cc > 0 || (cc == 0 && klen >= r->hi_n))
                lt_hi = 0;
        }
        if (ge_lo && lt_hi) {
            c->recovered++;
            return 0;
        }
    }
    return 0;
}

/* WP86: after the repair, name the damage. A dirent whose inode row is gone is
 * a file the volume can no longer show -- the only loss an operator can act on,
 * so it is counted and printed instead of leaving a silently shorter directory.
 * A quarantined dirent range means the names themselves are lost, which is
 * reported too (as unattributable). */
typedef struct {
    uint64_t lost;
    uint64_t shown;
} fsck_names_ctx;

static int fsck_lost_dirent_cb(void *ctx_, const char *name, size_t nlen,
                               uint64_t child)
{
    fsck_names_ctx *c = (fsck_names_ctx *)ctx_;
    invfs_v3_inode in;
    int rc;

    if (!nlen || !child)
        return 0;
    if ((unsigned char)name[0] == 0x01)
        return 0;                       /* internal owner/registry entry */
    rc = vol_v3_inode_get(g_fsck_v, child, &in);
    if (rc < 0)
        return 0;                       /* still unreadable: not "lost" yet */
    if (rc == 1)
        return 0;
    c->lost++;
    if (c->shown < 32)
        fprintf(stderr, "fsck(v3): LOST: %s (inode %llu: its base page is "
                        "unreadable; the data is not recoverable)\n",
                name, (unsigned long long)child);
    c->shown++;
    return 0;
}

static void fsck_v3_lost_names(invfs_volume *v, invfs_fsck_report *rep)
{
    fsck_names_ctx c;
    int rc;

    memset(&c, 0, sizeof c);
    g_fsck_v = v;
    rc = vol_v3_dirent_scan(v, INVFS_V3_ROOT_INO, fsck_lost_dirent_cb, &c);
    if (rc != 0) {
        fprintf(stderr, "fsck(v3): UNATTRIBUTABLE LOSS: the directory entries "
                        "that named the lost files were themselves inside a "
                        "quarantined range, so the names cannot be listed. "
                        "Every key in the quarantined range is gone; restore "
                        "the volume from a backup if the names matter.\n");
        return;
    }
    rep->v3_lost_names = c.lost;
    if (c.lost)
        fprintf(stderr, "fsck(v3): %llu name(s) lost with the quarantined "
                        "pages%s\n", (unsigned long long)c.lost,
                c.lost > c.shown ? " (list truncated)" : "");
    else
        fprintf(stderr, "fsck(v3): no name is left pointing at a lost inode "
                        "row (the entries that named them were inside the "
                        "quarantined range too)\n");
}

/* WP86: the -f repair. Excise the quarantined ranges, publish the new root,
 * reclaim what the dropped subtrees held, and fold the delta back in so every
 * quarantined key the delta still covers comes back.
 *
 * Crash safety is the fold's, unchanged: nothing is published until the new
 * pages and the allocation bitmap are durable, so a crash before the publish
 * leaves the volume exactly as it was (damaged but not worse), and a crash
 * after it replays the delta against the new base idempotently. */
static int fsck_v3_repair(invfs_volume *v, invfs_fsck_report *rep,
                          invfs_blkptr old_root, const bt_quarantine *q)
{
    invfs_blkptr new_root = old_root, nr;
    fsck_dq_ctx dq;
    int changed;

    /* A live save point pins pages this repair is about to free, and it pins a
     * root whose tree is damaged by definition. WP86: it is detected and
     * dropped here, never used (spt0_restore refuses such a save point too). */
    if (v->savepoint_live) {
        fprintf(stderr, "fsck(v3): dropping the save point (its base tree is "
                        "damaged and cannot be used as a rollback target)\n");
        if (spt0_drop(v) < 0) {
            fsck_v3_note(rep, "could not drop the damaged save point");
            return -1;
        }
    }

    memset(&dq, 0, sizeof dq);
    dq.q = q;
    (void)vol_delta_iter(v, fsck_delta_in_q_cb, &dq);
    rep->v3_keys_quarantined = dq.recovered;

    changed = btree_excise(v, old_root, q, &new_root);
    if (changed < 0) {
        fsck_v3_note(rep, "quarantine excision failed (the volume is "
                          "unchanged; re-run with free space)");
        return -1;
    }
    if (changed == 0) {
        fsck_v3_note(rep, "no quarantined range could be excised -- the base "
                          "tree is unchanged");
        return 0;
    }

    /* structure-before-reference (WP-M3): the new pages and the allocation
     * bitmap are durable before RT30 names the new root. */
    if (vol_v3_bitmap_flush(v) != 0 || vmux_barrier(v, "fsck v3 repair pages") < 0) {
        fsck_v3_note(rep, "could not make the repaired pages durable; the "
                          "root was NOT published (the volume is unchanged)");
        return -1;
    }
    if (mbuf_root_publish(v, new_root.pba, new_root.gen) != 0) {
        fsck_v3_note(rep, "RT30 refused the repaired root; the volume is "
                          "unchanged");
        return -1;
    }

    /* Reachability diff against the new root. The save point is gone, so
     * nothing is pinned: the dropped subtrees' pages (including the damaged
     * one) and the rewritten path are freed. */
    (void)vol_reclaim_bump_epoch();
    (void)vol_reclaim_drain(v);
    (void)vol_reclaim_mark_and_free(v, old_root, new_root,
                                    (invfs_blkptr){0, 0, 0, 0});

    /* Fold the delta back in: this restores every quarantined key the delta
     * still holds, and leaves the delta empty as a normal fold does. */
    if (vol_delta_count(v) > 0 && vol_v3_fold(v) != 0)
        fprintf(stderr, "fsck(v3): warning: the delta could not be folded "
                        "back in after the repair; its records are still in "
                        "the delta and will be re-applied by the next fold\n");

    /* What is left is gone: name it. */
    fsck_v3_lost_names(v, rep);
    rep->v3_repaired = 1;
    (void)nr;
    return 0;
}

static int fsck_v3_scan(invfs_volume *v, invfs_fsck_report *rep, int fix)
{
    invfs_blkptr root;
    bt_stat st;
    bt_quarantine q;
    char err[128];
    int rc, i;

    rc = fsck_v3_rt30(v, rep);
    if (rc < 0)
        return -1;
    if (rc == 2) {
        /* WP86: the descriptor is torn, so no root can be named. Only the
         * delta is readable, and rebuilding the base from the delta alone
         * would drop every key folded before it -- refuse, loudly. */
        rep->v3_root_lost = 1;
        return 0;
    }
    if (rc != 0) {
        /* RT30 absent or a page size this build cannot address: the base is
         * not walkable. Damage (if any) is already recorded; report and stop
         * rather than read 4 KiB pages under a 16 KiB descriptor. */
        return 0;
    }
    if (fsck_v3_root(v, rep, &root) < 0)
        return -1;

    if (root.pba == 0) {
        /* No valid root page. Empty base when no slot names anything;
         * WP86: damage when a slot named a page that does not validate. */
        if (v->rt30.root_slot[0] || v->rt30.root_slot[1]) {
            char b[192];
            rep->v3_root_lost = 1;
            snprintf(b, sizeof b,
                     "no valid base root: root_slot[0]=%llu root_slot[1]=%llu "
                     "and neither page validates -- the base tree is "
                     "unreachable; only the delta (recent writes) is readable",
                     (unsigned long long)v->rt30.root_slot[0],
                     (unsigned long long)v->rt30.root_slot[1]);
            fsck_v3_note(rep, b);
        } else if (!rep->v3_damaged) {
            fprintf(stderr, "fsck(v3): base tree empty (clean)\n");
        }
        return 0;
    }

    /* WP86: a live save point over a damaged base is a trap -- it looks like a
     * rollback target and is not one. Report it, and refuse the repair while
     * it is live (fsck_v3_repair drops it, but only under -f). */
    if (v->savepoint_live) {
        invfs_blkptr pinned;
        char e2[128];
        e2[0] = 0;
        if (fsck_v3_ptr_at(v, v->spt0.base_root, &pinned) != 0 ||
            btree_check(v, pinned, NULL, e2, sizeof e2) != 0) {
            char b[192];
            rep->v3_savepoint_bad = 1;
            snprintf(b, sizeof b, "a live save point pins a DAMAGED base tree "
                     "(base_root %llu) -- invf-rollback will refuse it",
                     (unsigned long long)v->spt0.base_root);
            fsck_v3_note(rep, b);
        }
    }

    err[0] = 0;
    rc = btree_check_tolerant(v, root, &st, &q, err, sizeof err);
    rep->v3_pages_walked = st.n_pages;
    rep->v3_keys = st.nkeys;
    rep->v3_quarantined = (uint64_t)q.n;
    if (rc != 0) {
        char b[256];
        if (err[0])
            snprintf(b, sizeof b, "base tree walk failed: %s", err);
        else
            snprintf(b, sizeof b, "base tree walk failed (io error)");
        fsck_v3_note(rep, b);
        /* A structural failure (cycle, shared child, level/ordering) is not
         * media damage and is NOT excised: -f refuses rather than reshape a
         * tree the engine itself built wrong. */
        if (strstr(err, "cycle or shared child"))
            rep->v3_cycles++;
        else
            rep->v3_bad_pages += q.bad_pages ? q.bad_pages : 1;
        return 0;
    }
    if (q.n) {
        /* big enough for two hex keys (3*BT_QUARANTINE_KEY_MAX+1 bytes each)
         * plus the fixed text and the pba -- at 256 the tail carrying
         * "QUARANTINED (those keys read EIO...)" was cut off, which is the
         * part the operator has to read */
        char b[2048];
        rep->v3_bad_pages += q.bad_pages;
        for (i = 0; i < q.n; i++) {
            char lo[3 * BT_QUARANTINE_KEY_MAX + 1];
            char hi[3 * BT_QUARANTINE_KEY_MAX + 1];
            size_t o = 0;
            int j;
            for (j = 0; j < q.range[i].lo_n && o + 3 < sizeof lo; j++)
                o += (size_t)snprintf(lo + o, sizeof lo - o, "%02x",
                                      q.range[i].lo[j]);
            lo[o] = 0;
            if (q.range[i].hi_unbounded) {
                snprintf(hi, sizeof hi, "+inf");
            } else {
                o = 0;
                for (j = 0; j < q.range[i].hi_n && o + 3 < sizeof hi; j++)
                    o += (size_t)snprintf(hi + o, sizeof hi - o, "%02x",
                                          q.range[i].hi[j]);
                hi[o] = 0;
            }
            snprintf(b, sizeof b,
                     "base page pba %llu is unreadable: key range [%s, %s) is "
                     "QUARANTINED (those keys read EIO; everything else is "
                     "readable)", (unsigned long long)q.range[i].pba, lo, hi);
            fsck_v3_note(rep, b);
        }
        if (q.qfull) {
            char b[192];
            snprintf(b, sizeof b, "the quarantine set is incomplete (more "
                     "unreadable pages than it holds, or a key range longer "
                     "than %u bytes): this report is partial and -f will "
                     "refuse to repair", (unsigned)BT_QUARANTINE_KEY_MAX);
            fsck_v3_note(rep, b);
        }
    }

    /* Allocator cross-check: a reachable page must be marked allocated in the
     * metadata bitmap (the v2 "bitmap divergence" analogue). Only the
     * root page is checked here: the check does not expose the visited set,
     * and btree_reclaim (which could mark it) frees pages, which is out of
     * scope. TODO(WP-M4): full reachable-set/bitmap cross-check. */
    fsck_v3_bitmap_check(v, rep, root.pba);

    if (!fix || !q.n)
        return 0;
    if (q.qfull) {
        fsck_v3_note(rep, "refusing to repair: the damage exceeds what one pass "
                          "can quarantine");
        return 0;
    }
    return fsck_v3_repair(v, rep, root, &q);
}
