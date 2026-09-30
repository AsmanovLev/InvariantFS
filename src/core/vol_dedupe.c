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
    size_t progress_every;
    invfs_dedupe_progress_fn progress;
    void *progress_user;
    int stop;   /* an unparsable record: stop the walk, keep pass 1's
                 * findings (the old linear scan's `break`) */
    int err;    /* OOM: abort the pass */
} dedup_hash_ctx;

#define DEDUP_PROGRESS_DEFAULT 8192u

static size_t dedup_progress_every(void)
{
    const char *s = getenv("INVFS_DEDUPE_PROGRESS_EVERY");
    char *end = NULL;
    unsigned long n;

    if (!s || !*s) return DEDUP_PROGRESS_DEFAULT;
    n = strtoul(s, &end, 10);
    if (end == s || *end || n == 0 || n > (1UL << 20))
        return DEDUP_PROGRESS_DEFAULT;
    return (size_t)n;
}

static void dedup_report(invfs_dedupe_progress_fn fn, void *user,
                         const char *phase, uint64_t done, uint64_t total,
                         uint64_t candidates, uint64_t merged,
                         uint64_t cross_merged, uint64_t intra_merged,
                         uint64_t freed);

static void dedup_hash_progress(const dedup_hash_ctx *ctx)
{
    if (!ctx->progress_every || !ctx->n ||
        ctx->n % ctx->progress_every != 0)
        return;
    if (ctx->progress)
        dedup_report(ctx->progress, ctx->progress_user,
                     "hash", ctx->n, 0, 0, 0, 0, 0, 0);
    else
        fprintf(stderr, "dedupe: hashing %zu segments\n", ctx->n);
}

static void dedup_report(invfs_dedupe_progress_fn fn, void *user,
                         const char *phase, uint64_t done, uint64_t total,
                         uint64_t candidates, uint64_t merged,
                         uint64_t cross_merged, uint64_t intra_merged,
                         uint64_t freed)
{
    invfs_dedupe_progress p;

    if (!fn) return;
    p.phase = phase;
    p.done = done;
    p.total = total;
    p.candidates = candidates;
    p.merged = merged;
    p.cross_merged = cross_merged;
    p.intra_merged = intra_merged;
    p.freed = freed;
    fn(user, &p);
}


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






/* one merge intent: entry (inode, lba) currently at cur_pba moves onto
 * canon_pba (the loser's extent derives from its framed header at free
 * time, cross-checked against the bitmap). */
typedef struct {
    uint64_t inode, lba, cur_pba, canon_pba;
    int cross_file;
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
static int dedup_remap_file_v3(invfs_volume *v, uint64_t inode,
                               const merge_ent *ms, size_t nm,
                               uint64_t *freed_out, size_t *applied_out,
                               size_t *cross_applied_out)
{
    invfs_v3_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;
    invfs_ast_block_entry *new_ents = NULL;
    size_t applied = 0;
    uint64_t freed = 0;
    size_t m, j;
    uint8_t *new_blob = NULL;
    size_t new_blen = 0;
    uint8_t new_addr[INVFS_V3_RECIPE_ADDR_LEN];

    *freed_out = 0;
    *applied_out = 0;
    *cross_applied_out = 0;

    if (vol_v3_inode_get(v, inode, &in) != 1) return -1;
    /* The destructive end of the same chain the collector guards above.
     * This is the function that REPUBLISHES: it serialises a synthesised
     * recipe over the inode and moves pba refcounts, so a raw-blob type
     * arriving here would have its content overwritten by an AST the
     * caller never wrote. Reached only with an inode id that came out of
     * the collector's candidate list, which no longer contains one -- so
     * this is the invariant stated where it is load-bearing rather than
     * assumed upstream. Returning 1 ("nothing applied") is the honest
     * answer: there are no merges for a symlink. */
    if (invfs_inode_content_is_raw_blob(in.type)) return 1;
    if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob) return -1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 || !ents) {
        free(blob);
        return -1;
    }
    new_ents = (invfs_ast_block_entry *)malloc(n_ents * sizeof(*new_ents));
    if (!new_ents) { free(blob); return -1; }
    memcpy(new_ents, ents, n_ents * sizeof(*new_ents));

    for (m = 0; m < nm; m++) {
        for (j = 0; j < n_ents; j++) {
            if (new_ents[j].block_id == ms[m].lba) {
                if (new_ents[j].pba == ms[m].cur_pba && ms[m].cur_pba != ms[m].canon_pba) {
                    new_ents[j].pba = ms[m].canon_pba;
                    applied++;
                    if (ms[m].cross_file) (*cross_applied_out)++;
                    pba_ref_modify(v, ms[m].cur_pba, -1);
                    pba_ref_modify(v, ms[m].canon_pba, +1);
                }
                break;
            }
        }
    }
    free(blob);

    if (!applied) {
        free(new_ents);
        return 1;
    }

    if (vol_ast_recipe_serialize(in.size, new_ents, (uint32_t)n_ents,
                                 &new_blob, &new_blen) != 0) {
        free(new_ents);
        return -1;
    }
    free(new_ents);

    if (vol_v3_recipe_store(v, new_blob, new_blen, new_addr) != 0) {
        free(new_blob);
        return -1;
    }
    free(new_blob);

    memcpy(in.recipe_addr, new_addr, sizeof(new_addr));
    if (vol_v3_inode_delta_put(v, inode, &in) != 0)
        return -1;
    /* WP pba-ref-v3-incremental: the merge loop above moved the counts
     * itself (-1 per loser pba, +1 per canonical pba), so the map is exact
     * and the staleness vol_v3_inode_delta_put just flagged does not
     * apply -- clearing it here is what keeps a dedupe pass from paying a
     * full rebuild at its next free gate. */
    pba_ref_validate(v);

    /* Free loser extents if their refcount dropped to 0 */
    for (m = 0; m < nm; m++) {
        size_t k;
        int seen = 0;
        for (k = 0; k < m; k++) {
            if (ms[k].cur_pba == ms[m].cur_pba) { seen = 1; break; }
        }
        if (seen) continue;
        if (pba_ref_count(v, ms[m].cur_pba) == 0) {
            uint64_t plen = 0;
            if (seg_extent_checked(v, ms[m].cur_pba, &plen) == 0 && plen > 0) {
                vol_free_blocks(v, ms[m].cur_pba, plen);
                freed += plen;
            }
        }
    }

    *freed_out = freed;
    *applied_out = applied;
    return 0;
}

static int dedup_remap_file(invfs_volume *v, uint64_t inode,
                            const merge_ent *ms, size_t nm,
                            uint64_t *freed_out, size_t *applied_out,
                            size_t *cross_applied_out)
{
    *cross_applied_out = 0;
    return dedup_remap_file_v3(v, inode, ms, nm, freed_out, applied_out,
                               cross_applied_out);
}


static int dedup_v3_walk_cb(void *ctx_, const char *path, uint64_t inode_id,
                            uint32_t type, uint64_t size, int64_t mtime)
{
    dedup_hash_ctx *ctx = (dedup_hash_ctx *)ctx_;
    invfs_volume *v = ctx->v;
    invfs_v3_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;
    uint32_t i;
    (void)path; (void)size; (void)mtime;

    if (type == INVFS_ITYP_DIR || !inode_id) return 0;
    if (path && (unsigned char)path[0] == 0x01) return 0;
    /* A symlink's blob is its TARGET STRING, stored content-addressed like
     * any recipe (vol_v3_create_node, src/core/vol_dirs.c) -- the address
     * does not say "AST". invfs_ast_hdr_parse accepts any blob whose first
     * four bytes are a v1/v2 AST version word, so a symlink that
     * false-parses donates attacker-chosen block entries to this pass and
     * they reach dedup_remap_file_v3, which republishes a SYNTHESISED
     * recipe over the link. Stated through the predicate the READ path
     * dispatches on (src/core/volume.h) so the two cannot drift again.
     *
     * Today this is redundant: every v3 symlink writer takes strlen() over a
     * NUL-terminated buffer and both version words carry a NUL at offset 1,
     * so the blob is one byte long and hdr_parse rejects it before it reads
     * the version. Redundant is not harmless -- it is a property of the
     * caller, and the day a symlink's content stops being strlen-delimited
     * this becomes silent data loss. src/cli/dedupe_symlink_test leg R
     * proves the reachability gap and leg F what happens across it.
     *
     * Placed BEFORE the load, unlike the checkers in vol_spt0.c and
     * vol_btree.c. "Skip the parse, never the load" exists because a
     * checker OWES a load: it is judging the volume and must not pass a
     * blob it could not read. This pass owes nothing -- it is not a health
     * check (the recipe audit in vol_btree.c is), it is a candidate list,
     * and a symlink has no segments to contribute. Reading a blob whose
     * bytes are already known to be the wrong shape would be wasted I/O. */
    if (invfs_inode_content_is_raw_blob(type)) return 0;
    if (vol_v3_inode_get(v, inode_id, &in) != 1) return 0;
    if (in.size == 0) return 0;
    if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob) return 0;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 || !ents) {
        free(blob);
        return 0;
    }

    for (i = 0; i < n_ents; i++) {
        invfs_ast_block_entry e = ents[i];
        uint64_t pba;
        uint8_t hdrb[8];
        uint32_t csize;

        if (e.zone == INVFS_ZONE_TEXT) continue;
        if (e.algo == INVFS_ALGO_JXL || e.algo == INVFS_ALGO_APE || e.algo == INVFS_ALGO_EXER)
            continue;
        pba = e.pba;
        if (pba == 0 || pba >= v->sb.total_blocks) continue;

        if (io_pread(&v->io, pba * INVFS_BLOCK_SIZE, hdrb, 8) != 0) continue;
        memcpy(&csize, hdrb, 4);
        if (csize == 0 || (uint64_t)csize + 8 > (v->sb.total_blocks - pba) * INVFS_BLOCK_SIZE)
            continue;
        if (csize > 16u * 1024u * 1024u)
            continue; /* skip giant monolithic blobs from segment dedupe */
        if (csize > ctx->blobcap) {
            uint8_t *nb = (uint8_t *)realloc(ctx->blob, csize);
            if (!nb) { free(blob); ctx->err = 1; return 1; }
            ctx->blob = nb;
            ctx->blobcap = csize;
        }
        if (io_pread(&v->io, pba * INVFS_BLOCK_SIZE + 8, ctx->blob, csize) != 0)
            continue;

        blake3_hasher_init(ctx->hx);
        blake3_hasher_update(ctx->hx, hdrb, 8);
        blake3_hasher_update(ctx->hx, ctx->blob, csize);
        blake3_hasher_finalize(ctx->hx, ctx->segs[ctx->n].hash, 32);
        ctx->segs[ctx->n].inode = inode_id;
        ctx->segs[ctx->n].lba = e.block_id;
        ctx->segs[ctx->n].pba = pba;
        if (++ctx->n >= ctx->cap) {
            dedup_seg *ns;
            size_t ncap = ctx->cap * 2;
            ns = (dedup_seg *)realloc(ctx->segs, ncap * sizeof(dedup_seg));
            if (!ns) { free(blob); ctx->err = 1; return 1; }
            ctx->segs = ns;
            ctx->cap = ncap;
        }
        dedup_hash_progress(ctx);
    }
    free(blob);
    return 0;
}


int vol_sweep_dedupe_ex(invfs_volume *v, invfs_dedupe_stats *stats,
                        invfs_dedupe_progress_fn progress, void *progress_user)
{
    size_t n = 0, cap = 1 << 16;
    dedup_seg *segs;
    blake3_hasher hx;
    uint8_t *blob = NULL;
    size_t merged = 0;
    uint64_t freed_blocks = 0;
    size_t progress_every = dedup_progress_every();
    invfs_dedupe_stats local_stats;
    invfs_dedupe_progress_fn report = progress;
    void *report_user = progress_user;
    int marked = 0;             /* vol_mark_dirty done (first real merge) */
    int rc = 0;

    memset(&local_stats, 0, sizeof local_stats);
    if (!stats) stats = &local_stats;
    else memset(stats, 0, sizeof *stats);

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
         ctx.progress_every = progress_every;
         ctx.progress = report;
         ctx.progress_user = report_user;
         ctx.stop = 0;
        ctx.err = 0;
        wrc = vol_v3_walk(v, dedup_v3_walk_cb, &ctx);
        segs = ctx.segs;        /* the callback may have grown the array */
        blob = ctx.blob;
        n = ctx.n;
        /* a structural stop still runs pass 2 on what was collected;
         * OOM or a walker failure aborts the pass */
        if (ctx.err || (wrc != 0 && !ctx.stop)) { rc = -1; goto out; }
    }
    stats->segments_hashed = n;
    if (report)
        dedup_report(report, report_user,
                     "hash_done", n, n, 0, 0, 0, 0, 0);
    else
        fprintf(stderr, "dedupe: sorting %zu segment hashes...\n", n);
    if (!report)
        printf("dedupe: hashed %zu live segments\n", n);

    /* pass 2: group by hash, collect the merge intents, then rewrite each
     * losing record ONCE with all its merges applied (the WP27 churn
     * rule: a record append per SEGMENT would churn a 250-segment file
     * 250 times; per-file is one append + one position-kill) */
    if (n > 1)
        qsort(segs, n, sizeof(dedup_seg), dedup_cmp);
    if (report)
        dedup_report(report, report_user, "sort", n, n, 0, 0, 0, 0, 0);
    else
        fprintf(stderr, "dedupe: hash sort complete; scanning duplicate groups...\n");
    pba_ref_ensure(v);
    {
        merge_ent *mi = NULL;
        size_t nmi = 0, capmi = 0, scan_cross = 0, scan_intra = 0;
        size_t i = 0;
        uint64_t last_inode = 0;
        uint8_t *cur_blob = NULL;
        size_t cur_blen = 0;
        invfs_ast_hdr cur_ah;
        const invfs_ast_block_entry *cur_ents = NULL;
        size_t cur_nents = 0;
        size_t next_scan = progress_every;

        while (i < n) {
            size_t j = i + 1;
            while (j < n && memcmp(segs[i].hash, segs[j].hash, 32) == 0) j++;
            if (j - i > 1) {
                uint64_t canon_pba = segs[i].pba;
                size_t k;
                for (k = i + 1; k < j; k++) {
                    const dedup_seg *s = &segs[k];
                    uint64_t cur_pba = 0;

                    {
                        if (s->inode != last_inode) {
                            free(cur_blob);
                            cur_blob = NULL;
                            last_inode = s->inode;
                            invfs_v3_inode in;
                            if (vol_v3_inode_get(v, s->inode, &in) == 1) {
                                /* The third reader of a raw-blob type's
                                 * blob in this file: a symlink names no
                                 * segment, so there is no cur_pba to
                                 * recover. The guard belongs here too
                                 * because this leg is what actually feeds
                                 * cur_pba -- and therefore
                                 * dedup_remap_file_v3 -- and it must not be
                                 * reachable only by way of the collector
                                 * having already filtered. */
                                if (invfs_inode_content_is_raw_blob(in.type)) {
                                    last_inode = 0;   /* nothing cached */
                                } else
                                if (vol_v3_recipe_load(v, in.recipe_addr, &cur_blob, &cur_blen) == 0 && cur_blob) {
                                    if (vol_ast_recipe_parse(cur_blob, cur_blen, &cur_ah, &cur_ents, &cur_nents) != 0) {
                                        free(cur_blob); cur_blob = NULL;
                                    }
                                }
                            }
                        }
                        if (cur_blob && cur_ents) {
                            if (s->lba < cur_nents && cur_ents[s->lba].block_id == s->lba) {
                                cur_pba = cur_ents[s->lba].pba;
                            } else {
                                for (size_t ent_idx = 0; ent_idx < cur_nents; ent_idx++) {
                                    if (cur_ents[ent_idx].block_id == s->lba) {
                                        cur_pba = cur_ents[ent_idx].pba;
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    if (!cur_pba || cur_pba == canon_pba)
                        continue;   /* already shared / moved on */
                    if (nmi == capmi) {
                        size_t nc = capmi ? capmi * 2 : 256;
                        merge_ent *nm2 = (merge_ent *)realloc(mi, nc * sizeof *nm2);
                        if (!nm2) { free(cur_blob); free(mi); rc = -1; goto out; }
                        mi = nm2;
                        capmi = nc;
                    }
                    mi[nmi].inode = s->inode;
                    mi[nmi].lba = s->lba;
                    mi[nmi].cur_pba = cur_pba;
                    mi[nmi].canon_pba = canon_pba;
                    mi[nmi].cross_file = s->inode != segs[i].inode;
                    if (mi[nmi].cross_file) scan_cross++;
                    else scan_intra++;
                    nmi++;
                }
            }
            i = j;
            if (i >= next_scan || i == n) {
                if (report)
                    dedup_report(report, report_user, "scan", i, n,
                                 nmi, merged, stats->cross_merged,
                                 stats->intra_merged, freed_blocks);
                else
                    fprintf(stderr,
                            "dedupe: duplicate scan %zu/%zu segments, candidates=%zu\n",
                            i, n, nmi);
                next_scan = i + progress_every;
            }
        }
        free(cur_blob);
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[dedupe] pass 2: n=%zu, duplicate candidates nmi=%zu\n", n, nmi);
        stats->duplicate_candidates = nmi;
        stats->cross_candidates = scan_cross;
        stats->intra_candidates = scan_intra;
        if (report)
            dedup_report(report, report_user, "scan", n, n,
                         nmi, merged, 0, 0, freed_blocks);
        else
            fprintf(stderr, "dedupe: duplicate candidates %zu\n", nmi);
        if (nmi > 1)
            qsort(mi, nmi, sizeof *mi, merge_ent_cmp);
        i = 0;
        {
            size_t next_progress = progress_every;
        while (i < nmi) {
            size_t j = i + 1;
            uint64_t freed_one = 0;
            size_t applied = 0, cross_one = 0;
            int mrc;
            while (j < nmi && mi[j].inode == mi[i].inode) j++;
            if (!marked && vol_mark_dirty(v) != 0) { rc = -1; break; }
            marked = 1;
            mrc = dedup_remap_file(v, mi[i].inode, mi + i, j - i,
                                   &freed_one, &applied, &cross_one);
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
                stats->segments_merged += applied;
                stats->cross_merged += cross_one;
                stats->intra_merged += applied - cross_one;
            }
            i = j;
            if (i >= next_progress || i == nmi) {
                if (report)
                    dedup_report(report, report_user, "merge", i, nmi,
                                 nmi, merged, stats->cross_merged,
                                 stats->intra_merged, freed_blocks);
                else
                    fprintf(stderr,
                            "dedupe: merge progress %zu/%zu candidate segments, "
                            "merged=%zu, freed=%llu blocks\n",
                            i, nmi, merged,
                            (unsigned long long)freed_blocks);
                next_progress = i + progress_every;
            }
        }
        }
        free(mi);
    }
stop:
    stats->segments_merged = merged;
    stats->blocks_freed = freed_blocks;
    if (report)
        dedup_report(report, report_user, "merge",
                     stats->duplicate_candidates,
                     stats->duplicate_candidates,
                     stats->duplicate_candidates, merged,
                     stats->cross_merged, stats->intra_merged, freed_blocks);
    if (!report)
        printf("dedupe: merged %zu segments, freed %llu blocks\n",
               merged, (unsigned long long)freed_blocks);
out:
    stats->segments_merged = merged;
    stats->blocks_freed = freed_blocks;
    free(blob);
    free(segs);
    return rc ? rc : (int)merged;
}

int vol_sweep_dedupe(invfs_volume *v)
{
    return vol_sweep_dedupe_ex(v, NULL, NULL, NULL);
}
