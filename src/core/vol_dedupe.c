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

/* WP44: pass-1 walk state. The shared vol_records_walk owns the record
 * scan (mapper extents or the legacy area) and its CRC verification; this
 * carries the accumulator and error/stop flags the callback reports. */
typedef struct {
    invfs_volume *v;
    dedup_seg *segs;
    size_t n, cap;
    uint8_t *blob;
    size_t blobcap;
    blake3_hasher *hx;
    int stop;   /* an unparsable record: stop the walk, keep pass 1's
                 * findings (the old linear scan's `break`) */
    int err;    /* OOM: abort the pass */
} dedup_hash_ctx;


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
    size_t base, ent0;
    uint32_t j;

    if (meta_read_record_by_id(v, inode, &rec, &rl, NULL, 0, NULL) != 0)
        return 0;
    if (rl < INVFS_REC_HDR_LEN ||
        ((const invfs_inode_rec *)rec)->name_len > INVFS_MAX_NAME ||
        rl < INVFS_REC_HDR_LEN +
             ((const invfs_inode_rec *)rec)->name_len + 1) {
        free(rec);
        return 0;
    }
    base = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)rec) - rec);
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
    uint8_t *rec = NULL, *combo = NULL, *tomb = NULL;
    uint32_t rl = 0;
    uint64_t old_pos = 0;
    invfs_ast_hdr ah;
    size_t base, ent0, total, tomb_size;
    uint32_t j;
    size_t m, applied = 0;
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
    if (rl < INVFS_REC_HDR_LEN ||
        ((const invfs_inode_rec *)rec)->name_len > INVFS_MAX_NAME ||
        rl < INVFS_REC_HDR_LEN +
             ((const invfs_inode_rec *)rec)->name_len + 1)
        goto out;
    base = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)rec) - rec);
    if (rl < base + INVFS_AST_HDR_V1_LEN ||
        invfs_ast_hdr_parse(rec + base, rl - base, &ah) != 0 ||
        rl < base + ah.hdr_len +
             (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry))
        goto out;
    ent0 = base + ah.hdr_len;

    /* build the new record version: the old one copied verbatim (name,
     * times, the ext with its heat) with every intent's entry patched
     * onto its winner's pba */
    tomb_size = INVFS_REC_HDR_LEN +
                ((const invfs_inode_rec *)rec)->name_len + 1;
    total = (size_t)rl + 4 + tomb_size + 4;
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
    tomb = (uint8_t *)calloc(1, tomb_size);
    if (!tomb) goto out;
    ((invfs_inode_rec *)tomb)->magic = TOMBSTONE_MAGIC;
    v->hot.tombstones++;
    ((invfs_inode_rec *)tomb)->rec_len = (uint32_t)tomb_size;
    ((invfs_inode_rec *)tomb)->inode_id = inode;
    ((invfs_inode_rec *)tomb)->file_size = old_pos;   /* v2 position kill */
    {
        invfs_inode_rec *oh = (invfs_inode_rec *)rec;
        invfs_inode_rec *th = (invfs_inode_rec *)tomb;
        th->name_len = oh->name_len;
        memcpy(th->name, oh->name, oh->name_len);
    }
    crc_tb = invfs_crc32c(tomb, tomb_size);
    memcpy(combo + rl + 4, tomb, tomb_size);
    memcpy(combo + rl + 4 + tomb_size, &crc_tb, 4);

    /* Room for [new version][CRC][tombstone][CRC]. On a v0.3.0+ mapper
     * volume the "area" is dynamic extents and meta_get_append_pos grows
     * them on demand, so the legacy inode_area_pos/end bound does not
     * apply (it would always trip here); the append's own failure is the
     * real ENOSPC. On legacy format_version=0 the linear bound still
     * gates, with the churn backstop's compaction/retry. */
    if (!(v->met0_present && v->meta_mapper) &&
        v->inode_area_pos + total > v->inode_area_end) {
        /* churn backstop: reclaim the dead prefix and RE-READ (the
         * compaction moves every record: old_pos and the entry offsets
         * are rebuilt fresh) */
        free(combo);
        free(rec);
        free(tomb);
        rec = NULL;
        combo = NULL;
        tomb = NULL;
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
    {
        uint64_t npos;
        int rc2 = vol_append_slot(v, (uint64_t)total, &npos);
        if (rc2 != 0) {
            /* mapper volume out of extents/space (or a legacy write
             * refusal): stop the pass cleanly, keeping what merged */
            if (getenv("INVFS_DEBUG"))
                fprintf(stderr, "[dedupe] append slot failed (%d)\n", rc2);
            rc = 2;
            goto out;
        }
        if (io_seek(&v->io, npos) != 0 ||
            io_write(&v->io, combo, total) != 0)
            goto out;
        {
            invfs_inode_rec *nh = (invfs_inode_rec *)combo;
            char nm[INVFS_MAX_NAME + 1];
            size_t nlen = nh->name_len < INVFS_MAX_NAME ? nh->name_len
                                                        : INVFS_MAX_NAME;
            memcpy(nm, nh->name, nlen);
            nm[nlen] = 0;
            idx_put(v, nm, nlen, inode, npos, nh->file_size, nh->ctime);
            idx_put_id(v, inode, npos);
        }
    }
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
    free(tomb);
    return rc;
}


/* pass-1 record callback. The walker has already CRC-verified `rec`
 * (a full [record][CRC] buffer) and skipped torn appends, so the skip
 * rules here are exactly the linear scan's minus its own CRC/bounds
 * plumbing: TOMBSTONE and bad-name records are skipped, only the LIVE
 * newest record version is eligible, and the \x01 internal-owner records
 * are excluded. `rec_pos` is the record's absolute offset (what
 * idx_put_id stores), so the position-kill check is unchanged. */
static int dedup_hash_cb(void *ctx_, uint64_t rec_pos,
                         const invfs_inode_rec *h, const uint8_t *rec)
{
    dedup_hash_ctx *ctx = (dedup_hash_ctx *)ctx_;
    invfs_volume *v = ctx->v;
    invfs_ast_hdr ah;
    size_t base;
    uint32_t i;

    if (h->magic == TOMBSTONE_MAGIC)
        return 0;
    if (h->name_len > INVFS_MAX_NAME ||
        h->rec_len < INVFS_REC_HDR_LEN + h->name_len + 1)
        return 0;
    {
        /* newest-wins + position-kill: only the LIVE record version
         * describes segments that may be remapped */
        uint64_t ip = idx_get_id(v, h->inode_id);
        if (dedup_is_deferred(v, h->inode_id) ||
            vol_find(v, ((const invfs_inode_rec *)rec)->name) != h->inode_id ||
            (ip && ip != rec_pos))
            return 0;
    }
    /* WP59a: anchored files are never dedup-remapped.  Their segments stay
     * pinned to avoid re-encoding through a pack codec. */
    if (invfs_inode_is_anchored(v, h->inode_id))
        return 0;
    /* internal owner records ("\x01tzb", the WP20 "\x01parityN" seal
     * owners): tzb entries are zone==TEXT and skipped below anyway, and
     * parity blocks are NOT framed segments -- hashing them would merge
     * stripes with identical content onto one shared parity block,
     * which a later re-seal write would corrupt for the other sharers */
    if (h->name_len && (uint8_t)((const invfs_inode_rec *)rec)->name[0] == 0x01)
        return 0;
    base = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)rec) - rec);
    if (h->rec_len < base + INVFS_AST_HDR_V1_LEN ||
        invfs_ast_hdr_parse(rec + base, h->rec_len - base, &ah) != 0) {
        ctx->stop = 1;
        return 1;
    }
    if (h->rec_len < base + ah.hdr_len +
                     (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
        ctx->stop = 1;
        return 1;
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
        if (csize > ctx->blobcap) {
            uint8_t *nb = (uint8_t *)realloc(ctx->blob, csize);
            if (!nb) { ctx->err = 1; return -1; }
            ctx->blob = nb;
            ctx->blobcap = csize;
        }
        if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE + 8) != 0 ||
            io_read(&v->io, ctx->blob, csize) != 0)
            continue;
        blake3_hasher_init(ctx->hx);
        blake3_hasher_update(ctx->hx, hdrb, 8);
        blake3_hasher_update(ctx->hx, ctx->blob, csize);
        blake3_hasher_finalize(ctx->hx, ctx->segs[ctx->n].hash, 32);
        ctx->segs[ctx->n].inode = h->inode_id;
        ctx->segs[ctx->n].lba = e.block_id;
        ctx->segs[ctx->n].pba = pba;
        if (++ctx->n >= ctx->cap) {
            dedup_seg *ns;
            size_t ncap = ctx->cap * 2;
            ns = (dedup_seg *)realloc(ctx->segs, ncap * sizeof(dedup_seg));
            if (!ns) { ctx->err = 1; return -1; }
            ctx->segs = ns;
            ctx->cap = ncap;
        }
    }
    return 0;
}


int vol_sweep_dedupe(invfs_volume *v)
{
    size_t n = 0, cap = 1 << 16;
    dedup_seg *segs;
    blake3_hasher hx;
    uint8_t *blob = NULL;
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
     * extent derives from the segment's own framed header.
     * WP44: the scan is the shared vol_records_walk -- on a v0.3.0+
     * mapper volume that visits every dynamic metadata extent, on a
     * legacy format_version=0 volume it falls back to the contiguous
     * [inode_area_start, inode_area_pos) region. The walker CRC-verifies
     * each record and skips torn appends itself (vol_open's rule), so
     * dedup_hash_cb sees only valid records. */
    {
        dedup_hash_ctx ctx;
        int wrc;
        ctx.v = v;
        ctx.segs = segs;
        ctx.n = 0;
        ctx.cap = cap;
        ctx.blob = NULL;
        ctx.blobcap = 0;
        ctx.hx = &hx;
        ctx.stop = 0;
        ctx.err = 0;
        wrc = vol_records_walk(v, dedup_hash_cb, &ctx);
        segs = ctx.segs;        /* the callback may have grown the array */
        blob = ctx.blob;
        n = ctx.n;
        /* a structural stop still runs pass 2 on what was collected;
         * OOM or a walker failure aborts the pass */
        if (ctx.err || (wrc != 0 && !ctx.stop)) { rc = -1; goto out; }
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
