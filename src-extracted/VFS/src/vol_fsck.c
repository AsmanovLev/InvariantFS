/* vol_fsck.c — fsck/repair: rebuild L2P + used-bitmap from the inode
 * area. Split from volume.c. */

#include "volume_internal.h"


/* The batch accumulator and the real vol_tz_flush/vol_tz_gc live with the
 * rest of the WP10 write path, right after the storage-class helpers (they
 * need meta_rewrite/vol_stamp_class/vol_delete_inode). */

/* ================= fsck / repair =================
 * NOTE on physical addressing: AST entries store block_id = segment
 * index (the L2P key), NOT the physical zone block. Physical block
 * addresses live only in the L2P/journal, so a destroyed journal cannot
 * be rebuilt from ASTs with the current format. What fsck CAN do:
 *   - verify inode-record CRCs and record lengths (bad_recs)
 *   - verify that every live AST segment has an L2P entry (l2p_miss)
 *   - rebuild the used-bitmap from (metadata zone + live L2P pbas):
 *     orphans (allocated, unreferenced) are freed, missing (referenced,
 *     free in bitmap) are restored
 *   - rewrite the journal from the in-memory L2P (journal compact)
 *   - mark the superblock CLEAN
 * With fix=1 all fixes are applied and persisted. */
static int fsck_rebuild_one(invfs_volume *v, uint64_t rec_pos, uint64_t inode_id,
                            invfs_l2p_entry **l2p, size_t *n, size_t *cap,
                            uint8_t *used, size_t used_bytes,
                            invfs_fsck_report *rep)
{
    invfs_inode_rec rh;
    invfs_ast_hdr ast_h;
    uint8_t *rec = NULL;
    uint32_t crc_stored, crc_calc;
    size_t off;
    uint32_t i;
    /* H6: per-file l2p_miss detail, printed loudly at the end of the record
     * (name + lost byte ranges; a lost journal tail usually means a whole
     * contiguous run, so the first few ranges + the count say it) */
    uint64_t miss_off[4], miss_len[4];
    unsigned miss_n = 0, miss_total = 0;

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
        uint64_t pba, len;
        if (off + sizeof(e) > rh.rec_len) { rep->bad_recs++; break; }
        memcpy(&e, rec + off, sizeof(e));
        off += sizeof(e);
        /* every live segment must have an L2P entry */
        if (vol_lookup_entry(v, inode_id, e.block_id, &pba, &len) != 0 ||
            pba == 0) {
            rep->l2p_miss++;
            miss_total++;
            if (miss_n < 4) {
                miss_off[miss_n] = e.file_offset;
                miss_len[miss_n] = e.length;
                miss_n++;
            }
            continue;
        }
        /* mark used from L2P (physical) */
        {
            uint64_t b;
            for (b = pba; b < pba + len && b < v->sb.total_blocks; b++)
                bit_set(used, b);
        }
        /* keep entry for journal rewrite */
        if (*n == *cap) {
            size_t ncap = *cap ? *cap * 2 : 256;
            invfs_l2p_entry *nl = (invfs_l2p_entry *)realloc(*l2p, ncap * sizeof(invfs_l2p_entry));
            if (!nl) { free(rec); return -1; }
            *l2p = nl; *cap = ncap;
        }
        (*l2p)[*n].type = INVFS_JRN_MAP;
        (*l2p)[*n].inode = inode_id;
        (*l2p)[*n].lba = e.block_id;
        (*l2p)[*n].pba = pba;
        (*l2p)[*n].length = (uint32_t)len;
        (*l2p)[*n].crc = 0;
        /* WP19: a rebuild has no history to be faithful to -- heat cold */
        memset((*l2p)[*n].pad, 0, sizeof (*l2p)[*n].pad);
        (*n)++;
    }
    if (miss_total) {
        /* H6: name the file and the lost ranges, loudly and on stderr even
         * in -q mode -- data loss is never a quiet event. The file keeps
         * failing reads with EIO per missing segment; the rest of the
         * volume is repaired around it. */
        char nm[257];
        size_t nl = rh.name_len < 256 ? rh.name_len : 256;
        unsigned k;
        memcpy(nm, rh.name, nl);
        nm[nl] = 0;
        fprintf(stderr, "fsck: l2p_miss: %s (inode %llu): %u segment(s) "
                "without an L2P mapping, lost:",
                nm, (unsigned long long)inode_id, miss_total);
        for (k = 0; k < miss_n; k++)
            fprintf(stderr, " [%llu, %llu)",
                    (unsigned long long)miss_off[k],
                    (unsigned long long)(miss_off[k] + miss_len[k]));
        if (miss_total > miss_n)
            fprintf(stderr, " ... and %u more", miss_total - miss_n);
        fprintf(stderr, "\n");
    }
    free(rec);
    return 0;
}


/* ---- live-record set ----------------------------------------------------
 * Liveness must mirror the name index vol_open builds (idx_put / idx_del /
 * idx_del_at) EXACTLY, or fsck and the read path disagree about the same
 * volume -- the WP22c/F2 lesson: a legacy kill-by-id tombstone for one
 * name of a SHARED id (the rename fast path hardlinks the copy onto the
 * old id) used to kill the survivor's record here while the name index
 * kept it, so fsck reported "l2p misses: 0" over a file verify --deep saw
 * as CORRUPT and --repair never engaged. So tombstones apply in write
 * order with index semantics: an INOD upserts its name (last record
 * wins), a legacy DELT kills the name's entry only when the id matches, a
 * v2 DELT only when the position matches. */
typedef struct fsck_name {
    struct fsck_name *next;
    uint64_t id, pos;
    uint32_t nlen;
    char name[1];
} fsck_name;

typedef struct {
    fsck_name **buck;
    size_t mask, count;
} fsck_nameset;

static fsck_name *fsn_find(const fsck_nameset *s, const char *name,
                           size_t nlen)
{
    size_t b = (size_t)(idx_hash(name, nlen) & s->mask);
    fsck_name *e;
    for (e = s->buck[b]; e; e = e->next)
        if (e->nlen == nlen && memcmp(e->name, name, nlen) == 0)
            return e;
    return NULL;
}

static void fsn_grow(fsck_nameset *s)
{
    size_t ncap = (s->mask + 1) * 2, i;
    fsck_name **nb = (fsck_name **)calloc(ncap, sizeof *nb);
    if (!nb) return;
    for (i = 0; i <= s->mask; i++) {
        fsck_name *e = s->buck[i];
        while (e) {
            fsck_name *nx = e->next;
            size_t b = (size_t)(idx_hash(e->name, e->nlen) & (ncap - 1));
            e->next = nb[b]; nb[b] = e;
            e = nx;
        }
    }
    free(s->buck);
    s->buck = nb;
    s->mask = ncap - 1;
}

static void fsn_put(fsck_nameset *s, const char *name, size_t nlen,
                    uint64_t id, uint64_t pos)
{
    fsck_name *e;
    size_t b;
    if (!s->buck) {
        s->buck = (fsck_name **)calloc(1024, sizeof *s->buck);
        if (!s->buck) return;
        s->mask = 1023;
    }
    e = fsn_find(s, name, nlen);
    if (e) { e->id = id; e->pos = pos; return; }   /* last record wins */
    e = (fsck_name *)malloc(sizeof *e + nlen);
    if (!e) return;
    memcpy(e->name, name, nlen);
    e->name[nlen] = 0;
    e->nlen = (uint32_t)nlen;
    e->id = id;
    e->pos = pos;
    b = (size_t)(idx_hash(name, nlen) & s->mask);
    e->next = s->buck[b];
    s->buck[b] = e;
    s->count++;
    if (s->count > s->mask + 1) fsn_grow(s);
}

static void fsn_drop(fsck_nameset *s, const char *name, size_t nlen)
{
    size_t b = (size_t)(idx_hash(name, nlen) & s->mask);
    fsck_name *e, **pp = &s->buck[b];
    for (e = *pp; e; pp = &e->next, e = e->next)
        if (e->nlen == nlen && memcmp(e->name, name, nlen) == 0) break;
    if (!e) return;
    *pp = e->next;
    free(e);
    s->count--;
}

static void fsn_free(fsck_nameset *s)
{
    size_t i;
    if (!s->buck) return;
    for (i = 0; i <= s->mask; i++) {
        fsck_name *e = s->buck[i];
        while (e) { fsck_name *nx = e->next; free(e); e = nx; }
    }
    free(s->buck);
    s->buck = NULL;
}

int vol_fsck_scan(invfs_volume *v, invfs_fsck_report *rep, int fix)
{
    uint64_t pos, end;
    size_t bi;
    size_t l2p_n = 0, l2p_cap = 0;
    invfs_l2p_entry *newl2p = NULL;
    uint8_t *used = NULL;
    size_t used_bytes;
    fsck_nameset live = { NULL, 0, 0 };

    memset(rep, 0, sizeof(*rep));
    used_bytes = (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
    used = (uint8_t *)calloc(1, used_bytes);
    if (!used) return -1;

    /* pass 1: ordered replay of the area -- the name-keyed live set.
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
        if (!rec) { free(used); fsn_free(&live); return -1; }
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, rec, rh.rec_len) != 0 ||
            io_read(&v->io, &crc_stored, 4) != 0) {
            free(rec); free(used); fsn_free(&live); return -1;
        }
        crc_calc = invfs_crc32c(rec, rh.rec_len);
        free(rec);
        if (crc_calc != crc_stored) {
            /* corrupt record: report, skip past it, keep scanning */
            rep->bad_recs++;
            pos += rh.rec_len + 4;
            continue;
        }
        {
            size_t nl = rh.name_len < 256 ? rh.name_len : 256;
            if (rh.magic == INODE_REC_MAGIC) {
                if (nl) fsn_put(&live, rh.name, nl, rh.inode_id, pos);
            } else {
                /* v2 tombstones kill by record position; legacy ones by
                 * id -- both only when the name's CURRENT entry matches,
                 * exactly idx_del_at / idx_del */
                fsck_name *e = nl ? fsn_find(&live, rh.name, nl) : NULL;
                if (e && ((rh.file_size == 0 && e->id == rh.inode_id) ||
                          (rh.file_size != 0 && e->pos == rh.file_size)))
                    fsn_drop(&live, rh.name, nl);
            }
        }
        pos += rh.rec_len + 4;
    }

    /* pass 2: per live record, verify AST<->L2P and collect used blocks */
    if (live.buck) {
        for (bi = 0; bi <= live.mask; bi++) {
            fsck_name *e;
            for (e = live.buck[bi]; e; e = e->next) {
                rep->live_files++;
                if (fsck_rebuild_one(v, e->pos, e->id,
                                     &newl2p, &l2p_n, &l2p_cap,
                                     used, used_bytes, rep) != 0) {
                    free(used); fsn_free(&live);
                    free(newl2p);
                    return -1;
                }
            }
        }
    }
    fsn_free(&live);

    /* metadata zone is always allocated */
    {
        uint64_t meta_end = v->sb.metadata_zone_start + v->sb.metadata_zone_blocks;
        uint64_t b;
        for (b = 0; b < meta_end; b++)
            bit_set(used, b);
    }

    /* compare bitmaps: orphans = in v->bitmap, not in used; missing = reverse */
    {
        uint64_t total = v->sb.total_blocks, i;
        for (i = 0; i < total; i++) {
            int bm = bit_get(v->bitmap, i);
            int us = bit_get(used, i);
            if (bm && !us) rep->orphans++;
            if (us && !bm) rep->missing++;
        }
    }

    rep->l2p_entries = l2p_n;

    /* repair when there is structural damage, or when the volume is merely
     * dirty: a scan that found nothing else is exactly a clean-close
     * simulation, so rewriting bitmap/journal and setting CLEAN is safe.
     * H6: l2p_miss no longer blocks the repair -- the mappings are gone
     * from the journal and cannot be reconstructed, but refusing the
     * journal rewrite kept the whole VOLUME read-only (a DIRTY state
     * mounts read-only) over damage that is per-file. The repair proceeds
     * around them: the affected files are listed by name and lost ranges
     * above, keep failing reads with EIO per missing segment, and the
     * volume mounts RW again after the fix. */
    if (fix && (rep->orphans || rep->missing || rep->bad_recs ||
                rep->l2p_miss || v->sb.state != INVFS_STATE_CLEAN)) {
        /* journal rewrite: keep only live-AST mappings (drops stale
         * entries of records fsck could not verify) */
        if (l2p_n) {
            free(v->l2p);
            v->l2p = newl2p;
            v->l2p_count = l2p_n;
            v->l2p_cap = l2p_n;
            v->l2p_dirty = 0;   /* whole table replaced */
            newl2p = NULL;
        }
        memcpy(v->bitmap, used, used_bytes);
        v->free_blocks = vol_count_free(v);
        alloc_state_reset(v);   /* bitmap replaced: per-zone counters stale */
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
        if (vol_flush(v) != 0) {
            free(used); free(newl2p);
            return -1;
        }
        v->sb.state = INVFS_STATE_CLEAN;
        if (vol_flush(v) != 0) {
            free(used); free(newl2p);
            return -1;
        }
    }
    free(used);
    free(newl2p);
    return 0;
}
