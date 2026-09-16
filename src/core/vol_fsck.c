/* vol_fsck.c — fsck/repair: rebuild L2P + used-bitmap from the inode
 * area. Split from volume.c. */

#include "volume_internal.h"


/* The batch accumulator and the real vol_tz_flush/vol_tz_gc live with the
 * rest of the WP10 write path, right after the storage-class helpers (they
 * need meta_rewrite/vol_stamp_class/vol_delete_inode). */

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
    rec = (uint8_t *)malloc(rh.rec_len);
    if (!rec) return -1;
    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, rec, rh.rec_len) != 0 ||
        io_read(&v->io, &crc_stored, 4) != 0) { free(rec); return -1; }
    crc_calc = invfs_crc32c(rec, rh.rec_len);
    if (crc_calc != crc_stored) { rep->bad_recs++; free(rec); return 0; }

    off = sizeof(invfs_inode_rec);   /* name[] lives inside the header */
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
 * from the name's version stack, which is what makes the cut permanent. */
static int fsck_quarantine(invfs_volume *v, const char *name,
                           uint64_t killpos, uint64_t id)
{
    invfs_inode_rec rec;
    uint32_t crc;
    uint64_t pos = v->inode_area_pos;
    uint8_t *old = NULL;
    uint32_t orl = 0;

    if (pos + sizeof(rec) + 4 > v->inode_area_end)
        return -1;
    memset(&rec, 0, sizeof rec);
    rec.magic = TOMBSTONE_MAGIC;
    v->hot.tombstones++;
    rec.rec_len = (uint32_t)sizeof rec;
    rec.inode_id = id;
    rec.file_size = killpos;      /* v2 position kill */
    rec_set_name(&rec, name);
    crc = invfs_crc32c(&rec, sizeof rec);
    if (io_seek(&v->io, pos) != 0 ||
        io_write(&v->io, &rec, sizeof rec) != 0 ||
        io_write(&v->io, &crc, 4) != 0)
        return -1;
    /* WP27: the killed record's pba references leave the map (the bitmap
     * rebuild below decides their fate from the SURVIVING records) */
    if (meta_read_record_by_id(v, id, &old, &orl, NULL, 0, NULL) == 0) {
        pba_ref_apply(v, old, orl, -1);
        free(old);
    }
    v->inode_area_pos = pos + sizeof rec + 4;
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
    int trc = -1;

    if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &rh, sizeof rh) != 0)
        return -1;
    if (rh.magic != INODE_REC_MAGIC || rh.rec_len < sizeof rh ||
        rh.rec_len > INVFS_MAX_REC_LEN)
        return 1;
    rec = (uint8_t *)malloc(rh.rec_len);
    if (!rec) return -1;
    if (io_seek(&v->io, pos) != 0 || io_read(&v->io, rec, rh.rec_len) != 0)
        { free(rec); return -1; }
    off = sizeof(invfs_inode_rec);
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
        uint32_t crc;
        uint64_t p;
        body = sizeof(invfs_inode_rec) + hl +
               (size_t)k * sizeof(invfs_ast_block_entry) + ext_len;
        if (v->inode_area_pos + body + 4 > v->inode_area_end)
            { free(rec); return -1; }
        nr = (uint8_t *)malloc(body);
        if (!nr) { free(rec); return -1; }
        memset(nr, 0, body);
        memcpy(nr, rec, sizeof(invfs_inode_rec));   /* header incl. name */
        ((invfs_inode_rec *)nr)->rec_len = (uint32_t)body;
        ((invfs_inode_rec *)nr)->file_size = new_size;
        w = nr + sizeof(invfs_inode_rec);
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
        v->inode_area_pos = p + body + 4;
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
    if (rh.rec_len < sizeof rh || rh.rec_len > INVFS_MAX_REC_LEN)
        return 1;
    rec = (uint8_t *)malloc(rh.rec_len);
    if (!rec) return 1;
    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, rec, rh.rec_len) != 0) { free(rec); return 1; }
    off = sizeof(invfs_inode_rec);
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
    used_bytes = (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
    used = (uint8_t *)calloc(1, used_bytes);
    if (!used) return -1;

    /* pass 1: ordered replay of the area -- the name-keyed live set, with
     * the consistent cut applied per record (broken = some AST segment has
     * no mapping in the replayed journal).
     * Scan the FULL metadata zone tail, not just v->inode_area_pos
     * (vol_open truncates the area at the first corrupt record). */
    end = (v->sb.metadata_zone_start + v->sb.metadata_zone_blocks)
          * INVFS_BLOCK_SIZE;
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec rh;
        uint32_t crc_stored, crc_calc;
        uint8_t *rec = NULL;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &rh, sizeof(rh)) != 0) break;
        if (rh.magic != INODE_REC_MAGIC && rh.magic != TOMBSTONE_MAGIC) break;
        if (rh.rec_len < sizeof(invfs_inode_rec) ||
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
            pos += rh.rec_len + 4;
            continue;
        }
        {
            size_t nl = rh.name_len < 256 ? rh.name_len : 256;
            if (rh.magic == INODE_REC_MAGIC) {
                if (nl) {
                    uint64_t moff[4], mlen[4];
                    unsigned mn = 0;
                    uint64_t miss = rec_pba_miss(rec, rh.rec_len,
                                                 v->sb.total_blocks,
                                                 moff, mlen, &mn);
                    if (scanset_inod(&live, rh.name, nl, rh.inode_id, pos,
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
                    scanset_delt(&live, rh.name, nl, rh.inode_id,
                                 rh.file_size);
            }
        }
        free(rec);
        pos += rh.rec_len + 4;
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
                            rh.rec_len >= sizeof rh &&
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
