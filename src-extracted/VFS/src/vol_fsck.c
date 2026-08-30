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
    invfs_ast_recipe_header ast_h;
    uint8_t *rec = NULL;
    uint32_t crc_stored, crc_calc;
    size_t off, i;

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
    if (off + sizeof(ast_h) > rh.rec_len) { free(rec); return -1; }
    memcpy(&ast_h, rec + off, sizeof(ast_h));
    off += sizeof(ast_h);

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
    free(rec);
    return 0;
}


int vol_fsck_scan(invfs_volume *v, invfs_fsck_report *rep, int fix)
{
    uint64_t pos, end;
    uint64_t i;
    size_t l2p_n = 0, l2p_cap = 0;
    invfs_l2p_entry *newl2p = NULL;
    uint8_t *used = NULL;
    size_t used_bytes;
    /* v2 tombstones kill by record position (DELT.file_size != 0);
     * legacy ones kill by inode id. Keep both fields per entry. */
    uint64_t *tomb_id = NULL, *tomb_pos = NULL;
    size_t tomb_n = 0, tomb_cap = 0;

    memset(rep, 0, sizeof(*rep));
    used_bytes = (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
    used = (uint8_t *)calloc(1, used_bytes);
    if (!used) return -1;

    end = v->inode_area_pos;
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;

    /* pass 1: tombstones + live inode record offsets */
    {
        uint64_t *live_pos = NULL, *live_id = NULL;
        size_t live_n = 0, live_cap = 0;
        /* scan the FULL metadata zone tail, not just v->inode_area_pos
         * (vol_open truncates the area at the first corrupt record) */
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
            if (!rec) { free(used); free(live_pos); free(live_id); return -1; }
            if (io_seek(&v->io, pos) != 0 ||
                io_read(&v->io, rec, rh.rec_len) != 0 ||
                io_read(&v->io, &crc_stored, 4) != 0) {
                free(rec); free(used); free(live_pos); free(live_id); return -1;
            }
            crc_calc = invfs_crc32c(rec, rh.rec_len);
            if (crc_calc != crc_stored) {
                /* corrupt record: report, skip past it, keep scanning */
                rep->bad_recs++;
                free(rec);
                pos += rh.rec_len + 4;
                continue;
            }
            if (rh.magic == TOMBSTONE_MAGIC) {
                if (tomb_n == tomb_cap) {
                    size_t ncap = tomb_cap ? tomb_cap * 2 : 64;
                    uint64_t *ni = (uint64_t *)realloc(tomb_id, ncap * sizeof(uint64_t));
                    uint64_t *np = (uint64_t *)realloc(tomb_pos, ncap * sizeof(uint64_t));
                    if (!ni || !np) {
                        free(ni); free(np);
                        free(rec); free(used); free(live_pos); free(live_id);
                        return -1;
                    }
                    tomb_id = ni; tomb_pos = np;
                    tomb_cap = ncap;
                }
                tomb_id[tomb_n] = rh.inode_id;
                tomb_pos[tomb_n] = rh.file_size;   /* v2: record position */
                tomb_n++;
            } else if (rh.magic == INODE_REC_MAGIC) {
                if (live_n == live_cap) {
                    live_cap = live_cap ? live_cap * 2 : 256;
                    uint64_t *np = (uint64_t *)realloc(live_pos, live_cap * sizeof(uint64_t));
                    uint64_t *ni = (uint64_t *)realloc(live_id, live_cap * sizeof(uint64_t));
                    if (!np || !ni) {
                        free(np); free(ni);
                        free(rec); free(used); free(live_pos); free(live_id);
                        return -1;
                    }
                    live_pos = np; live_id = ni;
                }
                live_pos[live_n] = pos;
                live_id[live_n] = rh.inode_id;
                live_n++;
            }
            free(rec);
            pos += rh.rec_len + 4;
        }

        /* pass 2: per live inode, verify AST<->L2P and collect used blocks */
        for (i = 0; i < live_n; i++) {
            int killed = 0;
            size_t t;
            for (t = 0; t < tomb_n; t++)
                if ((tomb_pos[t] == 0 && tomb_id[t] == live_id[i]) ||
                    (tomb_pos[t] != 0 && tomb_pos[t] == live_pos[i])) {
                    killed = 1; break;
                }
            if (killed) continue;
            /* Counted here, not in pass 1: a record that a tombstone later
               killed is not a live file. Counting every INOD reported 40004
               live files on a volume holding 40000 -- the 4 rewritten ones
               were counted twice. */
            rep->live_files++;
            if (fsck_rebuild_one(v, live_pos[i], live_id[i],
                                 &newl2p, &l2p_n, &l2p_cap,
                                 used, used_bytes, rep) != 0) {
                free(used); free(live_pos); free(live_id); free(tomb_id); free(tomb_pos);
                free(newl2p);
                return -1;
            }
        }
        free(live_pos); free(live_id);
    }
    free(tomb_id);
    free(tomb_pos);

    /* metadata zone is always allocated */
    {
        uint64_t meta_end = v->sb.metadata_zone_start + v->sb.metadata_zone_blocks;
        uint64_t b;
        for (b = 0; b < meta_end; b++)
            bit_set(used, b);
    }

    /* compare bitmaps: orphans = in v->bitmap, not in used; missing = reverse */
    {
        uint64_t total = v->sb.total_blocks;
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
     * Volumes with l2p_miss keep their read-only hold: mappings are gone
     * from the journal and cannot be reconstructed without human review. */
    if (fix && (rep->orphans || rep->missing || rep->bad_recs ||
                (!rep->l2p_miss && v->sb.state != INVFS_STATE_CLEAN))) {
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
