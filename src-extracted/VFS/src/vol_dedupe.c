/* vol_dedupe.c — WP12(h) offline per-segment dedupe.
 * Split from volume.c. */

#include "volume_internal.h"


/* ---- WP12(h): offline per-segment dedupe ------------------------------
 *
 * Port of the Windows-prototype pass (src/sweep.c sweep_dedupe) into the
 * engine: BLAKE3 over the STORED bytes of every live segment ([8B header]
 * + csize payload -- a complete, deterministic function of what is on
 * disk), keep ONE physical copy per distinct hash, remap the losers'
 * (inode, lba) L2P entries onto the winner's pba -- the same L2P-dup
 * trick TEXT members use -- and return the duplicate blocks to the
 * bitmap. Runs between the sweep walk and vol_tz_gc: the walk's
 * transcodes are what create the duplicates worth finding (every album's
 * identical cover art lands in Shadow as identical segments).
 *
 * Skip rules (all three are load-bearing):
 *  - zone==INVFS_ZONE_TEXT entries (WP10 §11): shared PPMd batches belong
 *    to the owner inode and are never dedup candidates;
 *  - whole-file blobs (JXL/APE): unique by construction;
 *  - inodes sitting in the sweep run's batching accumulators (v->tz text,
 *    v->bz binary): their records are retired by the SAME run's
 *    vol_tz_flush, and retiring frees the id's L2P targets -- merging one
 *    first would free the canonical copy under the survivor the dup was
 *    remapped onto.
 *
 * Liveness uses the same rule as vol_compute_stats: the name must resolve
 * to this id (newest record wins) AND the id index must point at THIS
 * record position (position-kill tombstones leave older same-id versions
 * in the area).
 *
 * No arc_invalidate is needed for the freed pbas: the content cache holds
 * whole-file reconstructions keyed by inode id and decoded TEXT batches
 * keyed by pba|TZ_ARC_TAG (see vol_read_text_slice). Dedupe never touches
 * TEXT pbas, and per-segment RAW/BINARY payloads are decoded on read and
 * never cached (algo_is_whole_file), so no ARC entry can alias a freed
 * block. The merge changes only WHO points at the surviving copy, never
 * the bytes under a live mapping.
 *
 * The remap + free are in-memory until the caller's vol_flush (bitmap
 * slice + journal suffix land together), exactly like the sweep's own
 * RAW->Shadow moves. Returns the number of merged segments, <0 on error
 * (a failed merge restores the victim's mapping before bailing). */
typedef struct {
    uint8_t  hash[32];
    uint64_t inode, lba, pba;
    uint32_t phys;      /* physical blocks of this segment */
} dedup_seg;


static int dedup_cmp(const void *a, const void *b)
{
    const dedup_seg *x = (const dedup_seg *)a, *y = (const dedup_seg *)b;
    int c = memcmp(x->hash, y->hash, 32);
    if (c) return c;
    if (x->inode != y->inode) return x->inode < y->inode ? -1 : 1;
    if (x->lba  != y->lba)  return x->lba  < y->lba  ? -1 : 1;
    return 0;
}


/* is this inode deferred into one of the running sweep's accumulators
 * (text WP10 / binary WP14a)? */
static int dedup_is_deferred(const invfs_volume *v, uint64_t inode_id)
{
    size_t i;
    for (i = 0; i < v->tz_n; i++)
        if (v->tz[i].inode_id == inode_id) return 1;
    for (i = 0; i < v->bz_n; i++)
        if (v->bz[i].inode_id == inode_id) return 1;
    return 0;
}


int vol_sweep_dedupe(invfs_volume *v)
{
    uint64_t pos, end;
    size_t n = 0, cap = 1 << 16;
    dedup_seg *segs;
    blake3_hasher hx;
    uint8_t *blob = NULL;
    size_t blobcap = 0;
    size_t merged = 0;
    uint64_t freed_blocks = 0;
    uint8_t *freed = NULL;
    int marked = 0;             /* vol_mark_dirty done (first real merge) */
    int rc = 0;

    if (!v) return -1;
    if (!vol_write_enabled(v)) return 0;   /* read-only: nothing to merge */

    segs = (dedup_seg *)malloc(cap * sizeof(dedup_seg));
    if (!segs) return -1;

    /* pass 1: hash the stored bytes of every live, dedup-eligible segment */
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    end = v->inode_area_pos;
    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec h;
        invfs_ast_hdr ah;
        uint8_t *rec;
        uint32_t crc_stored, crc_calc;
        uint32_t i;
        size_t base;

        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &h, sizeof(h)) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof(h) || h.rec_len > INVFS_MAX_REC_LEN ||
            pos + h.rec_len + 4 > end) break;
        if (h.magic == TOMBSTONE_MAGIC) { pos += (uint64_t)h.rec_len + 4; continue; }
        if (h.name_len >= sizeof(h.name)) { pos += (uint64_t)h.rec_len + 4; continue; }

        rec = (uint8_t *)malloc(h.rec_len);
        if (!rec) { rc = -1; goto out; }
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, rec, h.rec_len) != 0 ||
            io_read(&v->io, &crc_stored, 4) != 0) { free(rec); rc = -1; goto out; }
        crc_calc = invfs_crc32c(rec, h.rec_len);
        if (crc_calc != crc_stored) {
            /* torn append: skip it, keep scanning (vol_open's rule) */
            free(rec);
            pos += (uint64_t)h.rec_len + 4;
            continue;
        }
        {
            /* newest-wins + position-kill: only the LIVE record version
             * describes segments that may be remapped */
            uint64_t ip = idx_get_id(v, h.inode_id);
            if (dedup_is_deferred(v, h.inode_id) ||
                vol_find(v, h.name) != h.inode_id || (ip && ip != pos)) {
                free(rec);
                pos += (uint64_t)h.rec_len + 4;
                continue;
            }
        }
        /* internal owner records ("\x01tzb", the WP20 "\x01parityN" seal
         * owners): tzb entries are zone==TEXT and skipped below anyway, and
         * parity blocks are NOT framed segments -- hashing them would merge
         * stripes with identical content onto one shared parity block,
         * which a later re-seal write would corrupt for the other sharers */
        if (h.name_len && (uint8_t)h.name[0] == 0x01) {
            free(rec);
            pos += (uint64_t)h.rec_len + 4;
            continue;
        }
        base = sizeof(invfs_inode_rec);
        if (h.rec_len < base + INVFS_AST_HDR_V1_LEN ||
            invfs_ast_hdr_parse(rec + base, h.rec_len - base, &ah) != 0) {
            free(rec); break;
        }
        if (h.rec_len < base + ah.hdr_len +
                         (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
            free(rec); break;
        }
        for (i = 0; i < ah.num_blocks; i++) {
            invfs_ast_block_entry e;
            uint64_t pba = 0, phys = 0;
            uint8_t hdrb[8];
            uint32_t csize;

            memcpy(&e, rec + base + ah.hdr_len +
                   (size_t)i * sizeof(e), sizeof(e));
            if (e.zone == INVFS_ZONE_TEXT)
                continue;   /* WP10 §11: shared PPMd batches, owner-owned */
            if (e.algo == INVFS_ALGO_JXL || e.algo == INVFS_ALGO_APE ||
                e.algo == INVFS_ALGO_EXER)
                continue;   /* whole-file blobs: unique by construction */
            if (vol_lookup_entry(v, h.inode_id, e.block_id, &pba, &phys) != 0 ||
                pba == 0)
                continue;
            if (pba >= v->sb.total_blocks ||
                phys > v->sb.total_blocks - pba)
                continue;   /* stale map -- never hash out of bounds */
            if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, hdrb, 8) != 0)
                continue;
            memcpy(&csize, hdrb, 4);
            /* the payload must fit the blocks the L2P says this segment
             * owns; 0 or overlong means the header is not a segment */
            if (csize == 0 || (uint64_t)csize + 8 > phys * INVFS_BLOCK_SIZE)
                continue;
            if (csize > blobcap) {
                uint8_t *nb = (uint8_t *)realloc(blob, csize);
                if (!nb) { rc = -1; goto out; }
                blob = nb;
                blobcap = csize;
            }
            if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE + 8) != 0 ||
                io_read(&v->io, blob, csize) != 0)
                continue;
            blake3_hasher_init(&hx);
            blake3_hasher_update(&hx, hdrb, 8);
            blake3_hasher_update(&hx, blob, csize);
            blake3_hasher_finalize(&hx, segs[n].hash, 32);
            segs[n].inode = h.inode_id;
            segs[n].lba = e.block_id;
            segs[n].pba = pba;
            segs[n].phys = (uint32_t)phys;
            if (++n >= cap) {
                dedup_seg *ns;
                cap *= 2;
                ns = (dedup_seg *)realloc(segs, cap * sizeof(dedup_seg));
                if (!ns) { rc = -1; goto out; }
                segs = ns;
            }
        }
        free(rec);
        pos += (uint64_t)h.rec_len + 4;
    }
    printf("dedupe: hashed %zu live segments\n", n);

    /* pass 2: group by hash, remap duplicates onto the first copy */
    if (n > 1)
        qsort(segs, n, sizeof(dedup_seg), dedup_cmp);
    freed = (uint8_t *)calloc((size_t)(v->sb.total_blocks / 8 + 1), 1);
    if (!freed) { rc = -1; goto out; }
    {
        size_t i = 0;
        while (i < n) {
            size_t j = i + 1;
            while (j < n && memcmp(segs[i].hash, segs[j].hash, 32) == 0) j++;
            if (j - i > 1) {
                uint64_t canon_pba = segs[i].pba;
                size_t k;
                for (k = i + 1; k < j; k++) {
                    const dedup_seg *s = &segs[k];
                    uint64_t cur_pba = 0, cur_len = 0;
                    /* re-read the CURRENT map: superseded data is not ours
                     * to free, and an already-merged entry needs nothing */
                    if (vol_lookup_entry(v, s->inode, s->lba,
                                         &cur_pba, &cur_len) != 0)
                        continue;
                    if (cur_pba == canon_pba)
                        continue;   /* already shared */
                    if (!marked && vol_mark_dirty(v) != 0) { rc = -1; break; }
                    marked = 1;
                    /* WP19: heat keys on (inode,lba) -- carry it across the
                     * remap instead of rebirthing the entry cold */
                    {
                        uint16_t hr;
                        uint8_t hw;
                        heat_grab(v, s->inode, s->lba, &hr, &hw);
                        l2p_remove(v, s->inode, s->lba);
                        if (vol_map(v, s->inode, s->lba, canon_pba,
                                    (uint32_t)(cur_len ? cur_len : s->phys)) != 0) {
                            /* a failed merge must not strand the file without
                             * a mapping: put the old one back */
                            vol_map(v, s->inode, s->lba, cur_pba,
                                    (uint32_t)(cur_len ? cur_len : s->phys));
                            heat_stamp(v, s->inode, s->lba, hr, hw);
                            rc = -1;
                            break;
                        }
                        heat_stamp(v, s->inode, s->lba, hr, hw);
                    }
                    /* free the duplicate copy once (3+ identical segments
                     * all point at the same loser pba after the first) */
                    if (cur_pba < v->sb.total_blocks &&
                        !(freed[cur_pba >> 3] & (1u << (cur_pba & 7)))) {
                        freed[cur_pba >> 3] |= (uint8_t)(1u << (cur_pba & 7));
                        vol_free_blocks(v, cur_pba,
                                        cur_len ? cur_len : s->phys);
                        freed_blocks += cur_len ? cur_len : s->phys;
                    }
                    merged++;
                }
                if (rc != 0) break;
            }
            i = j;
        }
    }
    printf("dedupe: merged %zu segments, freed %llu blocks\n",
           merged, (unsigned long long)freed_blocks);
out:
    free(freed);
    free(blob);
    free(segs);
    return rc ? rc : (int)merged;
}
