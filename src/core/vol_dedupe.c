/* vol_dedupe.c — WP12(h) offline per-segment dedupe.
 * Split from volume.c.
 *
 * WP27: the remap is a RECORD REWRITE now (the house pattern), never an
 * L2P re-key: a file's AST entries carry their segments' pbas, so merging
 * a duplicate means appending a new record version whose losing entries
 * point at the winner's pba, then position-killing the old one. The
 * loser's blocks return to the bitmap through the retire path's refcount
 * gate (pba_ref): the extent is freed exactly when its last live
 * reference dies. The WP16f PB7 guard simplifies with it: references are
 * visible in the records directly.
 */

#include "volume_internal.h"


/* ---- WP12(h): offline per-segment dedupe ------------------------------
 *
 * Port of the Windows-prototype pass (src/sweep.c sweep_dedupe) into the
 * engine: BLAKE3 over the STORED bytes of every live segment ([8B header]
 * + csize payload -- a complete, deterministic function of what is on
 * disk), keep ONE physical copy per distinct hash, rewrite the losers'
 * records onto the winner's pba, and return the duplicate blocks to the
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
 *    vol_tz_flush, and retiring frees the old record's extents -- merging
 *    one first would free the canonical copy under the survivor the dup
 *    was rewritten onto.
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
 * the bytes under a live reference.
 *
 * The rewrites + frees are in-memory until the caller's vol_flush (bitmap
 * slice + the record appends land together), exactly like the sweep's own
 * RAW->Shadow moves. Returns the number of merged segments, <0 on error
 * (a failed merge leaves the file's previous record intact -- the new
 * version is appended only complete, and the retire is the house
 * new-first ordering). */
typedef struct {
    uint8_t  hash[32];
    uint64_t inode, lba, pba;
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


/* The CURRENT pba of (inode, lba): re-read the live record and find the
 * entry. 0 = gone/unreadable (leave it alone). The sweep holds the volume
 * exclusively, so the only rewrites since the hash pass are OUR OWN
 * earlier merges of the same file -- the pba equality check in
 * dedup_remap_one is the not-blind rule that catches them. */
static uint64_t dedup_cur_pba(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    uint8_t *rec = NULL;
    uint32_t rl = 0;
    uint64_t pba = 0;
    invfs_ast_hdr ah;
    size_t base = sizeof(invfs_inode_rec), ent0;
    uint32_t j;

    if (meta_read_record_by_id(v, inode, &rec, &rl, NULL, 0, NULL) != 0)
        return 0;
    if (rl >= base + INVFS_AST_HDR_V1_LEN &&
        invfs_ast_hdr_parse(rec + base, rl - base, &ah) == 0 &&
        rl >= base + ah.hdr_len +
             (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
        ent0 = base + ah.hdr_len;
        for (j = 0; j < ah.num_blocks; j++) {
            const invfs_ast_block_entry *e =
                (const invfs_ast_block_entry *)
                (rec + ent0 + (size_t)j * sizeof(*e));
            if (e->block_id == lba) { pba = e->pba; break; }
        }
    }
    free(rec);
    return pba;
}


/* one merge intent: entry (inode, lba) currently at cur_pba moves onto
 * canon_pba (the loser's extent derives from its framed header at free
 * time, cross-checked against the bitmap). */
typedef struct {
    uint64_t inode, lba, cur_pba, canon_pba;
} merge_ent;

static int merge_ent_cmp(const void *a, const void *b)
{
    const merge_ent *x = (const merge_ent *)a, *y = (const merge_ent *)b;
    if (x->inode != y->inode) return x->inode < y->inode ? -1 : 1;
    return x->lba < y->lba ? -1 : x->lba > y->lba;
}

/* Apply one file's pending merges in ONE record version: read the live
 * record, patch every intent's entry onto its winner (an entry whose pba
 * moved on is skipped -- never blind), append [new version][position-kill
 * DELT], move the refcounts, and free each loser's extent when its last
 * live reference died. ONE append + one tombstone per file per pass --
 * the WP27 churn rule (a per-segment rewrite would churn a 250-segment
 * file 250 times).
 * Returns: 0 = merged (>= 1 applied); 1 = nothing applied (all moved on);
 * 2 = inode area full (the churn backstop's compaction is impossible
 * under a live checkpoint -- the caller stops the pass cleanly, keeping
 * what merged); -1 = error. *freed_out takes the reclaimed block count,
 * *applied_out the count of intents actually applied. */
static int dedup_remap_file(invfs_volume *v, uint64_t inode,
                            const merge_ent *ms, size_t nm,
                            uint64_t *freed_out, size_t *applied_out)
{
    uint8_t *rec = NULL, *combo = NULL;
    uint32_t rl = 0;
    uint64_t old_pos = 0;
    invfs_ast_hdr ah;
    size_t base = sizeof(invfs_inode_rec), ent0, total;
    uint32_t j;
    size_t m, applied = 0;
    invfs_inode_rec tomb;
    uint32_t crc_nu, crc_tb;
    int rc = -1;
    int tried_compact = 0;

    *freed_out = 0;
    *applied_out = 0;
retry:
    if (meta_read_record_by_id(v, inode, &rec, &rl, NULL, 0,
                               &old_pos) != 0 || !old_pos) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[dedupe] remap %llu: live record read "
                    "failed\n", (unsigned long long)inode);
        return -1;
    }
    if (rl < base + INVFS_AST_HDR_V1_LEN ||
        invfs_ast_hdr_parse(rec + base, rl - base, &ah) != 0 ||
        rl < base + ah.hdr_len +
             (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry))
        goto out;
    ent0 = base + ah.hdr_len;

    /* build the new record version: the old one copied verbatim (name,
     * times, the ext with its heat) with every intent's entry patched
     * onto its winner's pba */
    total = (size_t)rl + 4 + sizeof(tomb) + 4;
    combo = (uint8_t *)malloc(total);
    if (!combo) goto out;
    memcpy(combo, rec, rl);
    for (m = 0; m < nm; m++) {
        for (j = 0; j < ah.num_blocks; j++) {
            invfs_ast_block_entry *e = (invfs_ast_block_entry *)
                (combo + ent0 + (size_t)j * sizeof(*e));
            if (e->block_id == ms[m].lba) {
                if (e->pba == ms[m].cur_pba) {
                    e->pba = ms[m].canon_pba;
                    applied++;
                }
                break;
            }
        }
    }
    if (!applied) { rc = 1; goto out; }

    crc_nu = invfs_crc32c(combo, rl);
    memcpy(combo + rl, &crc_nu, 4);
    memset(&tomb, 0, sizeof tomb);
    tomb.magic = TOMBSTONE_MAGIC;
    v->hot.tombstones++;
    tomb.rec_len = (uint32_t)sizeof tomb;
    tomb.inode_id = inode;
    tomb.file_size = old_pos;      /* v2 position kill */
    {
        invfs_inode_rec *oh = (invfs_inode_rec *)rec;
        tomb.name_len = oh->name_len;
        memcpy(tomb.name, oh->name, sizeof tomb.name);
    }
    crc_tb = invfs_crc32c((uint8_t *)&tomb, sizeof tomb);
    memcpy(combo + rl + 4, &tomb, sizeof tomb);
    memcpy(combo + rl + 4 + sizeof tomb, &crc_tb, 4);

    if (v->inode_area_pos + total > v->inode_area_end) {
        /* churn backstop: reclaim the dead prefix and RE-READ (the
         * compaction moves every record: old_pos and the entry offsets
         * are rebuilt fresh) */
        free(combo);
        free(rec);
        rec = NULL;
        combo = NULL;
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[dedupe] area full, compacting\n");
        if (!tried_compact && inode_area_make_room(v, total) == 0) {
            tried_compact = 1;
            applied = 0;
            goto retry;
        }
        /* the dead prefix is unreachable under a live checkpoint: stop
         * the pass cleanly, everything merged so far stays merged */
        rc = 2;
        goto out;
    }
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, combo, total) != 0) {
        goto out;
    }
    {
        invfs_inode_rec *nh = (invfs_inode_rec *)combo;
        char nm[INVFS_MAX_NAME + 1];
        size_t nlen = nh->name_len < INVFS_MAX_NAME ? nh->name_len
                                                    : INVFS_MAX_NAME;
        memcpy(nm, nh->name, nlen);
        nm[nlen] = 0;
        idx_put(v, nm, nlen, inode, v->inode_area_pos,
                nh->file_size, nh->ctime);
        idx_put_id(v, inode, v->inode_area_pos);
    }
    v->inode_area_pos += total;
    /* the refcount move: the old version's references die with it, the
     * new version's (the winners among them) arrive with it */
    pba_ref_apply(v, rec, rl, -1);
    pba_ref_apply(v, combo, rl, +1);
    *applied_out = applied;
    for (m = 0; m < nm; m++) {
        size_t k;
        int seen = 0;
        /* the loser's extent goes back exactly when its last live
         * reference died; two intents never free one extent twice */
        for (k = 0; k < m; k++)
            if (ms[k].cur_pba == ms[m].cur_pba) { seen = 1; break; }
        if (seen) continue;
        /* the cheap rule: free when the loser pba has no live reference
         * left (the map counts all live records) */
        if (pba_ref_count(v, ms[m].cur_pba) == 0) {
            uint64_t plen = 0;
            if (seg_extent_checked(v, ms[m].cur_pba, &plen) == 0) {
                vol_free_blocks(v, ms[m].cur_pba, plen);
                *freed_out += plen;
            }
        }
    }
    rc = 0;
out:
    free(combo);
    free(rec);
    return rc;
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
    int marked = 0;             /* vol_mark_dirty done (first real merge) */
    int rc = 0;

    if (!v) return -1;
    if (!vol_write_enabled(v)) return 0;   /* read-only: nothing to merge */

    segs = (dedup_seg *)malloc(cap * sizeof(dedup_seg));
    if (!segs) return -1;

    /* pass 1: hash the stored bytes of every live, dedup-eligible segment.
     * WP27: the address comes from the record's entry; the physical
     * extent derives from the segment's own framed header. */
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
            uint64_t pba = 0;
            uint8_t hdrb[8];
            uint32_t csize;

            memcpy(&e, rec + base + ah.hdr_len +
                   (size_t)i * sizeof(e), sizeof(e));
            if (e.zone == INVFS_ZONE_TEXT)
                continue;   /* WP10 §11: shared PPMd batches, owner-owned */
            if (e.algo == INVFS_ALGO_JXL || e.algo == INVFS_ALGO_APE ||
                e.algo == INVFS_ALGO_EXER)
                continue;   /* whole-file blobs: unique by construction */
            pba = e.pba;
            if (pba == 0 || pba >= v->sb.total_blocks)
                continue;
            if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, hdrb, 8) != 0)
                continue;
            memcpy(&csize, hdrb, 4);
            /* the payload must fit inside the volume; 0 is never a real
             * segment's size */
            if (csize == 0 ||
                (uint64_t)csize + 8 >
                    (v->sb.total_blocks - pba) * INVFS_BLOCK_SIZE)
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

    /* pass 2: group by hash, collect the merge intents, then rewrite each
     * losing record ONCE with all its merges applied (the WP27 churn
     * rule: a record append per SEGMENT would churn a 250-segment file
     * 250 times; per-file is one append + one position-kill) */
    if (n > 1)
        qsort(segs, n, sizeof(dedup_seg), dedup_cmp);
    pba_ref_ensure(v);
    {
        merge_ent *mi = NULL;
        size_t nmi = 0, capmi = 0;
        size_t i = 0;
        while (i < n) {
            size_t j = i + 1;
            while (j < n && memcmp(segs[i].hash, segs[j].hash, 32) == 0) j++;
            if (j - i > 1) {
                uint64_t canon_pba = segs[i].pba;
                size_t k;
                for (k = i + 1; k < j; k++) {
                    const dedup_seg *s = &segs[k];
                    uint64_t cur_pba;
                    /* re-read the CURRENT record: a segment deduped earlier
                     * in this same run already points at its winner, and a
                     * record that moved on is not ours to touch */
                    cur_pba = dedup_cur_pba(v, s->inode, s->lba);
                    if (!cur_pba || cur_pba == canon_pba)
                        continue;   /* already shared / moved on */
                    if (nmi == capmi) {
                        size_t nc = capmi ? capmi * 2 : 256;
                        merge_ent *nm2 = (merge_ent *)realloc(mi, nc * sizeof *nm2);
                        if (!nm2) { free(mi); rc = -1; goto out; }
                        mi = nm2;
                        capmi = nc;
                    }
                    mi[nmi].inode = s->inode;
                    mi[nmi].lba = s->lba;
                    mi[nmi].cur_pba = cur_pba;
                    mi[nmi].canon_pba = canon_pba;
                    nmi++;
                }
            }
            i = j;
        }
        if (nmi > 1)
            qsort(mi, nmi, sizeof *mi, merge_ent_cmp);
        i = 0;
        while (i < nmi) {
            size_t j = i + 1;
            uint64_t freed_one = 0;
            size_t applied = 0;
            int mrc;
            while (j < nmi && mi[j].inode == mi[i].inode) j++;
            if (!marked && vol_mark_dirty(v) != 0) { rc = -1; break; }
            marked = 1;
            mrc = dedup_remap_file(v, mi[i].inode, mi + i, j - i,
                                   &freed_one, &applied);
            if (mrc < 0) { rc = -1; free(mi); goto out; }
            if (mrc == 2) {
                /* inode area full (compaction impossible under a live
                 * checkpoint): the pass stops cleanly; everything merged
                 * so far stays merged */
                printf("dedupe: inode area full; stopping with %zu "
                       "segments merged\n", merged);
                free(mi);
                goto stop;
            }
            if (mrc == 0) {
                merged += applied;
                freed_blocks += freed_one;
            }
            i = j;
        }
        free(mi);
    }
stop:
    printf("dedupe: merged %zu segments, freed %llu blocks\n",
           merged, (unsigned long long)freed_blocks);
out:
    free(blob);
    free(segs);
    return rc ? rc : (int)merged;
}
