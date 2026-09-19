/* vol_rollback.c — WP21 sweep checkpoint (CKP0) + retention registry
 * + rollback. Split from volume.c. */

#include "volume_internal.h"


/* ---- WP21: CKP0 sweep-checkpoint descriptor --------------------------
 * Same block-0 RMW convention as RDP0 (above). The descriptor makes a
 * sweep checkpoint findable without scanning the inode area for the
 * "\x01reten" owner, and pins the two append pointers the journal staging
 * can roll the volume back to. */

/* CRC convention: over the full descriptor with the crc32c field read as
 * zero (the RDP0 rule). */
uint32_t ckp0_crc(const invfs_ckp0 *ck)
{
    invfs_ckp0 t = *ck;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}


/* Persist (ck != NULL) or clear (ck == NULL) the CKP0 descriptor by
 * read-modify-write of block 0 (see vol_write_rdp0 for why RMW is safe).
 * The in-memory copy follows the disk state; clearing also moves the
 * sequence number into ck_prev_seq so the next arm keeps counting. */
static int vol_write_ckp0(invfs_volume *v, const invfs_ckp0 *ck)
{
    uint8_t blk[INVFS_BLOCK_SIZE];
    if (io_seek(&v->io, 0) != 0 ||
        io_read(&v->io, blk, sizeof blk) != 0)
        return -1;
    if (ck) {
        invfs_ckp0 t = *ck;
        memcpy(t.magic, "CKP0", 4);
        t.crc32c = 0;
        t.crc32c = ckp0_crc(&t);
        memcpy(blk + INVFS_CKP0_OFF, &t, sizeof t);
        v->ck = t;
        v->ck_present = 1;
        v->ck_prev_seq = t.sweep_seq;
    } else {
        v->ck_prev_seq = v->ck.sweep_seq;
        memset(blk + INVFS_CKP0_OFF, 0, sizeof(invfs_ckp0));
        memset(&v->ck, 0, sizeof v->ck);
        v->ck_present = 0;
    }
    if (io_seek(&v->io, 0) != 0 ||
        io_write(&v->io, blk, sizeof blk) != 0)
        return -1;
    return 0;
}


/* ==================== WP21: sweep checkpoint + rollback ================
 *
 * One checkpoint per volume (K=1; K>1 is snapshots, v2). The checkpoint
 * is the CKP0 block-0 descriptor (invarifs.h): the inode-area and journal
 * append pointers at sweep start, plus a staging run holding a byte copy
 * of the used journal prefix.
 *
 * Why the staging exists: the inode area is strictly append-only, so its
 * checkpoint-time prefix is byte-intact at rollback time by construction
 * -- and WP22d made the journal append-only too (slot log), but a
 * compaction REPLACES the active slot wholesale, so the pre-sweep L2P
 * does NOT survive a sweep in place. The stage is the RSZ0 pattern in
 * miniature: durable copy first, descriptor naming it second. The staged
 * bytes are the whole used prefix of the active slot (header + image +
 * log); the restore writes them into BOTH slots, so a higher-sequence
 * post-sweep slot can never win the replay race.
 *
 * While a checkpoint-armed sweep runs, vol_free_blocks does not free
 * (see there): the blocks stay allocated (which bars their reuse) and are
 * remembered in retmap. vol_ckp_end writes the retention registry -- the
 * hidden internal owner "\x01reten" (sharded "\x01reten1"... like the
 * "\x01parity*" owners), one AST entry + L2P map per retained range, so
 * fsck counts the held blocks as live. Retained blocks are never read by
 * anyone; the registry's AST length field deliberately carries BLOCKS,
 * not bytes (the record is never served as a file: 0x01-hidden, verify
 * skips it, fsck consumes only block_id -> L2P).
 *
 * End states:
 *  - sweep done        : CKP0 live + registry holding the retired blocks.
 *  - invf-sweep --realize: the registry owners are deleted -- the retire
 *    path frees exactly the mapped ranges -- and CKP0 is cleared. The
 *    point of no return.
 *  - the next bare sweep (WP22d realize-after-arm): vol_ckp_begin stages
 *    the current journal, arms the new checkpoint, and only THEN deletes
 *    the old registry (those frees are real -- retention for the new run
 *    engages after). The pre-WP22d order (realize, then arm) left the
 *    volume with no net in between; the new order always keeps one live,
 *    and a crash mid-way leaves the new checkpoint live with the old
 *    registry intact (rollback or the next sweep reconciles it).
 *  - invf-rollback     : journal restored from the stage, inode
 *    area decapitated at the checkpoint (post-sweep records, tombstones
 *    and the registry itself vanish wholesale), then the ORDINARY fsck
 *    rebuild reconciles bitmap+journal: retained blocks are referenced by
 *    the resurrected pre-sweep records and stay live; everything the
 *    sweep allocated (including the stage and the registry's own blocks)
 *    is orphaned and reclaimed. Rollback never reads the registry.
 *
 * Crash ordering: before CKP0 there is nothing (a stranded stage is an
 * orphan fsck reclaims); after CKP0 the sweep is just a sweep (its own
 * per-file crash rules apply) with frees neutralized into retmap; the
 * rollback's commit point is the inode-area guard zeroing (journal
 * restore first), and CKP0 is cleared only after the rebuild succeeded.
 * A killed rollback is re-entered: phase 1 is skipped when the append
 * pointers already match the checkpoint, and the fsck rebuild is
 * idempotent. While CKP0 is live, invf-fsck -f REFUSES (its orphan
 * reclaim is the one pass that could free a not-yet-registered retained
 * block out from under a future rollback).
 */

#define RET_OWNER_NAME "\x01reten"

/* ranges per registry owner record: policy cap like SEAL_SHARD (the v2
 * recipe header counts past it -- the cap keeps records small and readable
 * by pre-WP22a binaries); shard N>0 is "\x01reten<N>" */
#define RET_SHARD 65535u

void ret_shard_name(uint64_t shard, char *out, size_t cap)
{
    if (shard == 0)
        snprintf(out, cap, RET_OWNER_NAME);
    else
        snprintf(out, cap, RET_OWNER_NAME "%llu",
                 (unsigned long long)shard);
}


int vol_ckp_armed(const invfs_volume *v)
{
    return v && v->ck_present;
}


int vol_ckp_info(const invfs_volume *v, invfs_ckp0 *out)
{
    if (!v || !v->ck_present) return 0;
    if (out) *out = v->ck;
    return 1;
}


/* defined with vol_ckp_realize below */
static int ret_registry_delete(invfs_volume *v, uint64_t *freed_out);


/* Free the arm's own staging run: the checkpoint machinery's bookkeeping,
 * never rollback-relevant content (the run was allocated seconds ago,
 * post any live checkpoint's cut), so the CKP0-live retention must not
 * hold it. */
static void ckp_free_direct(invfs_volume *v, uint64_t pba, uint64_t nblocks)
{
    v->retain_release = 1;
    vol_free_blocks(v, pba, nblocks);
    v->retain_release = 0;
}


#ifndef _WIN32

/* Test hook (tools/test-rollback.sh): die mid-rollback, right after the
 * journal restore+truncate commit ("restored") or right after the fsck
 * rebuild ("rebuilt"). The next invf-rollback re-enters and finishes --
 * the phases are idempotent by construction. */
static int rb_abort_at(const char *stage)
{
    const char *a = getenv("INVFS_ROLLBACK_ABORT_AT");
    return a && strcmp(a, stage) == 0;
}

#endif


int vol_ckp_begin(invfs_volume *v)
{
    uint64_t jstart, jused, sblocks, pba = 0;
    uint64_t old_stage_pba = 0, old_stage_blocks = 0;
    invfs_ckp0 ck;
    int had_ck, rrc;

    if (!v) return -1;
    {
        const char *e = getenv("INVFS_CHECKPOINT");
        if (e && strcmp(e, "0") == 0)
            return 0;   /* explicit opt-out (ENOSPC-tight volumes) */
    }
    if (!vol_write_enabled(v)) {
        fprintf(stderr, "checkpoint: declined (volume is read-only or "
                        "awaiting recovery)\n");
        return 0;
    }
    /* A live seal pins absolute block numbers into the parity stripes;
     * rolling back under it would silently invalidate them. The sweep
     * itself still runs -- uncheckpointed (the message names the way
     * out). (An armed resize cannot be present here: vol_open applied or
     * ignored it.) */
    if (v->rd_present && (v->rd.l1_algo || v->rd.l2_algo)) {
        fprintf(stderr, "checkpoint: declined (a redundancy seal is live; "
                        "rollback would invalidate parity stripes -- "
                        "invf-sweep --free-redundant first)\n");
        return 0;
    }
    if (v->retain) {
        /* begin twice in one session is a driver bug -- decline, never
         * overwrite the live arm */
        fprintf(stderr, "checkpoint: declined (already armed this "
                        "session)\n");
        return 0;
    }

    /* WP22d: flush BEFORE staging -- the staged journal must be current
     * and in the slot format (a legacy volume migrates on this flush), so
     * the rollback never has to reconcile a legacy stage against a
     * slotted live journal. */
    if (vol_flush(v) != 0)
        return -1;
    had_ck = v->ck_present;
    /* WP59/WP58a follow-up: remember the OUTGOING checkpoint's journal
     * staging run. The old runs are deleted below (they were freed for
     * real), but the old CKP0 also owns a staging allocation that the new
     * CKP0 no longer names -- without freeing it here it becomes an
     * orphan the moment the descriptor is overwritten (observed: two
     * 32-33-block runs per realizing sweep). Capture before
     * vol_write_ckp0 clobbers v->ck. */
    if (had_ck) {
        old_stage_pba = v->ck.stage_pba;
        old_stage_blocks = v->ck.stage_blocks;
    }

    /* Stage the used journal prefix of the ACTIVE slot (see the section
     * comment for why the inode area needs no staging). The staged bytes
     * include the slot header -- a rollback rewrites them into BOTH
     * slots, so a post-sweep slot can never win the sequence race. */
    jstart = v->j_slotted ? jrn_slot_base(v, v->j_slot)
                          : v->journal_start * (uint64_t)INVFS_BLOCK_SIZE;
    jused = v->journal_pos - jstart;
    sblocks = (jused + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    if (sblocks) {
        uint8_t *buf;
        uint64_t left, roff, woff;
        int ok = 1;

        /* the stage is transient pre-sweep staging: the RAW zone is its
         * honest home (a seal never covers RAW, so realizing the stage
         * can never dirty a parity stripe); shadow is the fallback for a
         * nearly-full RAW zone */
        pba = alloc_blocks(v, v->sb.raw_zone_start,
                           v->sb.raw_zone_blocks, sblocks, 1, INVFS_ALLOC_DATA);
        if (!pba)
            pba = alloc_blocks(v, v->sb.shadow_zone_start,
                               v->sb.shadow_zone_blocks, sblocks, 1, INVFS_ALLOC_DATA);
        if (!pba) {
            fprintf(stderr, "checkpoint: declined (no %llu-block run for "
                            "the journal staging)\n",
                    (unsigned long long)sblocks);
            return 0;
        }
        buf = (uint8_t *)malloc(BLKIO_BOUNCE);
        if (!buf) { ckp_free_direct(v, pba, sblocks); return -1; }
        left = jused;
        roff = jstart;
        woff = pba * (uint64_t)INVFS_BLOCK_SIZE;
        while (ok && left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            if (io_seek(&v->io, roff) != 0 || io_read(&v->io, buf, n) != 0 ||
                io_seek(&v->io, woff) != 0 || io_write(&v->io, buf, n) != 0)
                ok = 0;
            roff += n; woff += n; left -= n;
        }
        /* verify the staging by read-back BEFORE the descriptor names it
         * (the RSZ0 rule: a broken copy must fail the arm, never the
         * rollback) */
        roff = jstart;
        woff = pba * (uint64_t)INVFS_BLOCK_SIZE;
        left = jused;
        {
            uint8_t *chk = (uint8_t *)malloc(BLKIO_BOUNCE);
            if (!chk) ok = 0;
            while (ok && left) {
                size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
                if (io_seek(&v->io, roff) != 0 || io_read(&v->io, buf, n) != 0 ||
                    io_seek(&v->io, woff) != 0 || io_read(&v->io, chk, n) != 0 ||
                    memcmp(buf, chk, n) != 0)
                    ok = 0;
                roff += n; woff += n; left -= n;
            }
            free(chk);
        }
        free(buf);
        if (!ok) { ckp_free_direct(v, pba, sblocks); return -1; }
        /* the staging allocation must be durable before CKP0 can name it */
        if (vol_flush(v) != 0) {
            ckp_free_direct(v, pba, sblocks);
            return -1;
        }
    }

    memset(&ck, 0, sizeof ck);
    ck.inode_area_pos = v->inode_area_pos;
    ck.journal_pos = v->journal_pos;
    ck.stage_pba = pba;
    ck.stage_blocks = sblocks;
    ck.sweep_seq = v->ck_prev_seq + 1;
    ck.time_unix = (uint64_t)time(NULL);
    if (vol_write_ckp0(v, &ck) != 0) {
        if (pba) ckp_free_direct(v, pba, sblocks);
        return -1;
    }
    if (!v->retmap)
        v->retmap = (uint8_t *)calloc((size_t)(v->sb.total_blocks + 7) / 8, 1);
    else
        memset(v->retmap, 0, (size_t)(v->sb.total_blocks + 7) / 8);
    /* a NULL retmap degrades retention to "blocks stay allocated, nothing
     * is registered" -- rollback is unaffected (it never reads the
     * registry); realize reclaims via fsck after clearing CKP0 */
    v->ck_stage_pba = pba;
    v->ck_stage_blocks = sblocks;

    /* WP22d realize-AFTER-arm: the previous run's retention registry is
     * deleted only now that the new checkpoint is live. The pre-WP22d
     * order (realize the old checkpoint, then arm) left the volume with
     * no safety net in between -- a torn sweep during that window had
     * neither checkpoint nor (after a no-op sweep's disarm) anything to
     * roll back to. The frees here are REAL (the old checkpoint's point of
     * no return): retention keys on ck_present now, so the delete runs
     * under the release flag -- registry retention for the new run engages
     * only below. A crash between the CKP0 write and this delete leaves
     * the new checkpoint live with the old registry intact (the defensive
     * pre-sweep realize or a rollback reconciles it). */
    if (had_ck) {
        uint64_t rfree = 0;
        v->retain_release = 1;
        rrc = ret_registry_delete(v, &rfree);
        v->retain_release = 0;
        if (rrc < 0)
            fprintf(stderr, "checkpoint: previous registry delete failed; "
                    "the blocks stay held (fsck reclaims after the next "
                    "realize)\n");
        else if (rfree)
            fprintf(stderr, "checkpoint: previous run realized (%llu "
                    "retained blocks freed)\n", (unsigned long long)rfree);
        /* Persist the registry delete (the retire queued UNMAPs) so a later
         * fsck/journal replay does not resurrect the owner's maps. */
        if (vol_flush(v) != 0)
            fprintf(stderr, "checkpoint: realize flush failed\n");
        /* free the outgoing checkpoint's journal staging run: the new CKP0
         * is live, so the old stage is unreachable -- the descriptor used
         * to be the only reference to it. Guard the scratch allocation
         * against the incoming one: alloc_blocks never hands back a live
         * block, and this free runs before the next arm, so a collision is
         * impossible; the check is belt-and-braces. */
        if (old_stage_blocks && old_stage_pba &&
            !(pba == old_stage_pba && sblocks == old_stage_blocks)) {
            ckp_free_direct(v, old_stage_pba, old_stage_blocks);
            if (getenv("INVFS_DEBUG"))
                fprintf(stderr, "checkpoint: previous staging run freed "
                        "(%llu blocks @%llu)\n",
                        (unsigned long long)old_stage_blocks,
                        (unsigned long long)old_stage_pba);
        }
    }

    /* retention engages only from here on: anything freed earlier this
     * session was freed pre-checkpoint and is none of its business */
    v->retain = 1;
    fprintf(stderr, "checkpoint: #%llu armed (inode area @%llu, journal "
            "@%llu, %llu staging blocks)\n",
            (unsigned long long)ck.sweep_seq,
            (unsigned long long)ck.inode_area_pos,
            (unsigned long long)ck.journal_pos,
            (unsigned long long)sblocks);
    return 1;
}


int vol_ckp_end(invfs_volume *v, uint64_t *ranges_out, uint64_t *blocks_out)
{
    uint64_t *ent = NULL;      /* (pba, nblocks) pairs: [0] = staging run */
    uint64_t n_ent = 0, n_blocks = 0, total_ent;
    uint64_t b, run = 0, i;
    uint64_t shard;
    int rc = 0;

    if (ranges_out) *ranges_out = 0;
    if (blocks_out) *blocks_out = 0;
    if (!v || !v->retain) return 0;
    v->retain = 0;   /* the window closes with the registry write */

    /* count maximal retained runs */
    if (v->retmap) {
        for (b = 0; b < v->sb.total_blocks; b++) {
            if (bit_get(v->retmap, b)) { run++; n_blocks++; }
            else if (run) { n_ent++; run = 0; }
        }
        if (run) n_ent++;
    }

    /* A sweep that freed nothing leaves no checkpoint behind: rolling
     * back an identity is pointless, and this keeps a quiescent re-sweep
     * quiescent (no staging cost, no registry record, no CKP0). */
    if (!n_blocks) {
        if (v->ck_stage_blocks)
            ckp_free_direct(v, v->ck_stage_pba, v->ck_stage_blocks);
        v->ck_stage_pba = v->ck_stage_blocks = 0;
        if (v->ck_present) {
            if (getenv("INVFS_DEBUG"))
                fprintf(stderr, "checkpoint: #%llu disarmed (nothing "
                        "retained)\n", (unsigned long long)v->ck.sweep_seq);
            if (vol_write_ckp0(v, NULL) != 0)
                rc = -1;
        }
        return rc;
    }

    total_ent = n_ent + (v->ck_stage_blocks ? 1 : 0);
    ent = (uint64_t *)malloc((size_t)total_ent * 2 * sizeof *ent);
    if (!ent) return -1;
    i = 0;
    if (v->ck_stage_blocks) {
        ent[0] = v->ck_stage_pba;
        ent[1] = v->ck_stage_blocks;
        i = 2;
    }
    run = 0;
    for (b = 0; b < v->sb.total_blocks; b++) {
        if (bit_get(v->retmap, b)) {
            if (!run) ent[i] = b;   /* run start */
            run++;
        } else if (run) {
            ent[i + 1] = run;
            i += 2;
            run = 0;
        }
    }
    if (run) ent[i + 1] = run;

    /* The registry: one owner record per RET_SHARD entries, AST entries
     * zone=BINARY/algo=NONE whose L2P maps pin the ranges -- the seal
     * parity owners' pattern, so fsck counts the held blocks as live and
     * listings/verify skip the owner by the 0x01 prefix. */
    for (shard = 0; shard * RET_SHARD < total_ent && rc == 0; shard++) {
        char nm[32];
        uint64_t oid, base, hi;
        tz_owner o;
        uint32_t want_n;

        ret_shard_name(shard, nm, sizeof nm);
        oid = vol_find(v, nm);
        if (!oid) {
            oid = vol_create_file(v, nm, NULL, 0);
            if (!oid) { rc = -1; break; }
        }
        if (tz_owner_load(v, oid, &o) != 0) { rc = -1; break; }
        base = shard * RET_SHARD;
        hi = base + RET_SHARD;
        if (hi > total_ent) hi = total_ent;
        want_n = (uint32_t)(hi - base);
        if (o.cap < want_n) {
            invfs_ast_block_entry *ne = (invfs_ast_block_entry *)
                realloc(o.ents, want_n * sizeof(*ne));
            if (!ne) { tz_owner_free(&o); rc = -1; break; }
            o.ents = ne;
            o.cap = want_n;
        }
        o.n = 0;
        for (i = base; i < hi; i++) {
            invfs_ast_block_entry *e = &o.ents[o.n++];
            memset(e, 0, sizeof *e);
            e->length = ent[2 * i + 1];   /* BLOCKS, not bytes (see the
                                           * section comment) */
            e->zone = INVFS_ZONE_BINARY;
            e->algo = INVFS_ALGO_NONE;
            e->block_id = (uint32_t)(i - base);
            e->block_offset = 0;
            e->pba = ent[2 * i];   /* WP27: self-describing (fsck derives
                                    * the bitmap from records) */
            if (vol_map(v, oid, i - base, ent[2 * i],
                        (uint32_t)ent[2 * i + 1]) != 0) { rc = -1; break; }
        }
        /* maps durable BEFORE the record names them (the tz/seal rule) */
        if (rc == 0 && vol_flush(v) != 0) rc = -1;
        if (rc == 0 && tz_owner_write(v, oid, nm, &o) != 0) rc = -1;
        tz_owner_free(&o);
    }
    free(ent);
    if (rc == 0) {
        if (ranges_out) *ranges_out = n_ent;
        if (blocks_out) *blocks_out = n_blocks;
    }
    return rc;
}


/* WP58b: a rollback on a mapper volume must also discard the SWEEP's other
 * derived owner records. They live in dedicated metadata extents that the
 * phase-1 linear truncation cannot reach, so a pre-existing or
 * sweep-rewritten "\x01tzb" (text-zone batch owner), "\x01tier*", "\x01rawm"
 * survive with their post-sweep ASTs and keep mapping sweep segments -- which
 * phase 2 then counts used while the rebuilt journal supersedes them, leaving
 * bitmap/journal divergence and orphans (observed: E2 left 23 orphan
 * segments mapped by "\x01tzb"). Owners are derived state: the next sweep
 * regenerates them, and their now-unreferenced blocks are reclaimed by the
 * phase-2 rebuild. Collect names first (the deletes append records, so a live
 * walk would see them mid-iteration; owner overwrites are in-place, but the
 * generic collect-first shape is kept for safety). */
typedef struct {
    invfs_volume *v;
    char (*names)[INVFS_NAME_CAP];
    size_t n, cap;
} owner_purge_ctx;

static int owner_purge_cb(void *ctx_, uint64_t rec_pos,
                          const invfs_inode_rec *h, const uint8_t *rec)
{
    owner_purge_ctx *c = (owner_purge_ctx *)ctx_;
    char nm[INVFS_NAME_CAP];
    size_t nl, maxnl, i;
    (void)rec_pos; (void)rec;
    if (h->magic != INODE_REC_MAGIC) return 0;    /* tombstones */
    maxnl = h->rec_len > INVFS_REC_HDR_LEN + 1
          ? h->rec_len - INVFS_REC_HDR_LEN - 1 : 0;
    if (maxnl > INVFS_MAX_NAME) maxnl = INVFS_MAX_NAME;
    nl = h->name_len < maxnl ? h->name_len : maxnl;
    if (nl < 2 || h->name[0] != 0x01) return 0;   /* not an owner */
    memcpy(nm, h->name, nl); nm[nl] = 0;
    for (i = 0; i < c->n; i++)
        if (strcmp(c->names[i], nm) == 0) return 0;   /* older, same name */
    if (c->n == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 8;
        void *nn = realloc(c->names, ncap * sizeof(*c->names));
        if (!nn) return 1;                        /* purge what we gathered */
        c->names = (char (*)[INVFS_NAME_CAP])nn;
        c->cap = ncap;
    }
    memcpy(c->names[c->n++], nm, nl + 1);
    return 0;
}

static void rollback_purge_owners(invfs_volume *v)
{
    owner_purge_ctx c;
    size_t i;
    memset(&c, 0, sizeof c);
    c.v = v;
    vol_records_walk(v, owner_purge_cb, &c);
    for (i = 0; i < c.n; i++) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "rollback: purging owner %s\n", c.names[i] + 1);
        vol_delete_file(v, c.names[i]);
    }
    free(c.names);
}


/* Delete every registry shard ("\x01reten*"): the retire path frees
 * exactly the ranges the shard maps (zone==BINARY entries, so the
 * batch-owner gate does not hold them). Its PB7 sharer check can find no
 * live reference to a retained block: retained blocks stayed allocated
 * throughout the window, so nothing was ever reallocated onto one.
 * Returns 1 when something was deleted, 0 when absent, -1 on error. */
static int ret_registry_delete(invfs_volume *v, uint64_t *freed_out)
{
    uint64_t freed = 0;
    uint64_t shard;
    int did = 0;

    for (shard = 0; ; shard++) {
        char nm[32];
        uint64_t oid;
        size_t j;
        tz_owner o;
        int have_o = 0;
        ret_shard_name(shard, nm, sizeof nm);
        oid = vol_find(v, nm);
        if (!oid) break;   /* shards exist contiguously (the seal rule) */
        if (!vol_write_enabled(v)) return -1;
        /* Free the retained ranges from the shard's OWN self-describing AST
         * (WP27 entries carry pba). Relying on vol_delete_file's L2P scan
         * alone is not enough: an entry's owner-WAL map can already be
         * gone (a rebuilt/compacted journal), and then the delete removed
         * the registry record while its blocks stayed allocated -- the
         * realize-time "orphans" (observed: 64/realizing sweep). Freeing
         * from the AST is idempotent with the retire's own L2P pass (the
         * bitmap clear is per-block), so both may run. */
        if (tz_owner_load(v, oid, &o) == 0) {
            for (j = 0; j < o.n; j++) {
                uint64_t pba = o.ents[j].pba, len = o.ents[j].length;
                if (len && pba < v->sb.total_blocks &&
                    len <= v->sb.total_blocks - pba) {
                    vol_free_blocks(v, pba, len);
                    freed += len;
                }
            }
            have_o = 1;
            tz_owner_free(&o);
        }
        if (!have_o) {
            for (j = 0; j < v->l2p_count; j++) {
                const invfs_l2p_entry *e = &v->l2p[j];
                if (e->type == INVFS_JRN_MAP && e->inode == oid)
                    freed += e->length;
            }
        }
        if (vol_delete_file(v, nm) != 0) {
            fprintf(stderr, "checkpoint: realize: could not delete %s\n",
                    nm + 1);
            return -1;
        }
        did = 1;
    }
    if (freed_out) *freed_out = freed;
    return did;
}


int vol_ckp_realize(invfs_volume *v, uint64_t *freed_blocks_out)
{
    uint64_t freed = 0;
    int did = 0, rrc;

    if (freed_blocks_out) *freed_blocks_out = 0;
    if (!v) return -1;
    /* A realize IS the end of the retention window: the registry delete
     * must free for real (the point of no return for everything the
     * checkpoint held). Retention keys on ck_present now, and CKP0 is
     * cleared only below, so the delete runs under the release flag. */
    v->retain = 0;
    v->retain_release = 1;
    rrc = ret_registry_delete(v, &freed);
    v->retain_release = 0;
    if (rrc < 0) return -1;
    did = rrc;
    if (v->ck_present) {
        /* WP59/WP58a follow-up: the cleared CKP0 was the only reference to
         * the checkpoint's journal staging run -- free it with the
         * descriptor, or the blocks orphan (observed before this fix). */
        uint64_t spba = v->ck.stage_pba, sblocks = v->ck.stage_blocks;
        if (vol_write_ckp0(v, NULL) != 0) return -1;
        if (spba && sblocks)
            ckp_free_direct(v, spba, sblocks);
        did = 1;
    }
    if (did && vol_flush(v) != 0) return -1;
    if (freed_blocks_out) *freed_blocks_out = freed;
    return did;
}


/* ==================== WP24-lite: read-only time-travel at CKP0 ==========
 *
 * ckp_stage_replay is vol_rollback's non-destructive twin: same staging,
 * same verification, but the staged journal prefix is replayed from
 * MEMORY and nothing is written. vol_open_at calls it in place of
 * l2p_replay; the inode-area scan then stops at the checkpoint's append
 * pointer (see vol_open_inner), so the whole in-memory view -- L2P, name
 * index, WP22d consistent cut -- is exactly the sweep-start state.
 *
 * Why this is sound while the present keeps moving: the checkpoint pins
 * (a) the journal prefix as it was at arm time (the staging run -- a
 * compaction REPLACES the active slot, so the live slots no longer hold
 * it), and (b) every block the cut references: post-checkpoint frees went
 * to retmap, never back to the allocator, and the "\x01reten" registry
 * keeps them counted live for fsck. Both exist exactly while CKP0 lives;
 * a realized checkpoint makes the cut unprovable, so vol_open_at refuses.
 */
int ckp_stage_replay(invfs_volume *v)
{
    uint64_t jstart, jpos, iapos, stage_len, slot_idx = 0;
    uint64_t off, replayed = 0;
    int stage_slotted = 0;
    invfs_jrn_hdr sh;
    uint8_t *buf = NULL;
    uint32_t prev = 0;
    int rc = -3;

    if (!v || !v->ck_present) return -3;

    jstart = v->journal_start * (uint64_t)INVFS_BLOCK_SIZE;
    jpos = v->ck.journal_pos;
    iapos = v->ck.inode_area_pos;

    /* Sniff the first staged block for the JRN0 header (the rollback's
     * rule): the stage is the used prefix of the ACTIVE slot at arm time
     * (slot header + image + chained log), or the pre-WP22d legacy flat
     * log for a checkpoint armed by a pre-slot build. */
    if (v->ck.stage_blocks) {
        if (io_seek(&v->io, v->ck.stage_pba * (uint64_t)INVFS_BLOCK_SIZE) != 0 ||
            io_read(&v->io, &sh, sizeof sh) != 0)
            return -1;
        if (memcmp(sh.magic, INVFS_JRN_MAGIC, 4) == 0 &&
            sh.version == INVFS_JRN_VERSION &&
            invfs_crc32c(&sh, offsetof(invfs_jrn_hdr, crc32c)) == sh.crc32c)
            stage_slotted = 1;
    }
    if (stage_slotted) {
        /* the armed slot: the checkpoint's journal_pos names the append
         * point inside it; the staged content starts at that slot's base */
        uint64_t rel = (jpos / INVFS_BLOCK_SIZE) - v->journal_start;
        uint64_t rem;
        slot_idx = rel / INVFS_JRN_SLOT_BLOCKS;
        rem = rel % INVFS_JRN_SLOT_BLOCKS;
        if (jpos < jstart || slot_idx >= INVFS_JRN_SLOTS || rem == 0)
            return -3;
        stage_len = jpos - (jstart + slot_idx * INVFS_JRN_SLOT_BLOCKS *
                            INVFS_BLOCK_SIZE);
    } else {
        stage_len = jpos - jstart;   /* underflow caught by the check below */
        if (jpos < jstart ||
            (stage_len % sizeof(invfs_l2p_entry)) != 0)
            return -3;
    }

    /* Descriptor sanity (the CRC already proved the bytes; these bounds
     * are what the cut relies on). Same rule as the rollback: a failure
     * here means the descriptor or its staging was clobbered -- refuse
     * LOUDLY; nothing was or will be written. */
    if (stage_len > (uint64_t)INVFS_JOURNAL_BLOCKS * INVFS_BLOCK_SIZE ||
        iapos < v->inode_area_start * (uint64_t)INVFS_BLOCK_SIZE ||
        iapos > v->inode_area_end ||
        (v->ck.stage_blocks &&
         (v->ck.stage_pba >= v->sb.total_blocks ||
          v->ck.stage_blocks > v->sb.total_blocks - v->ck.stage_pba)) ||
        stage_len > v->ck.stage_blocks * (uint64_t)INVFS_BLOCK_SIZE)
        return -3;

    v->l2p_count = 0;
    v->jops_n = 0;
    l2p_idx_reset(v);   /* WP-L2Q: the staged replay below re-seeds it */

    if (stage_len) {
        buf = (uint8_t *)malloc((size_t)stage_len);
        if (!buf) return -1;
        if (io_seek(&v->io, v->ck.stage_pba * (uint64_t)INVFS_BLOCK_SIZE) != 0 ||
            io_read(&v->io, buf, (size_t)stage_len) != 0) {
            free(buf);
            return -1;
        }
    }

    if (stage_slotted) {
        /* verify (rollback phase 1, read-only): the slot header, the whole
         * image's CRC, then the chained log to exactly the staged end --
         * a reused or clobbered stage can never parse. */
        uint64_t ib, jp2;
        uint32_t icrc;
        memcpy(&sh, buf, sizeof sh);
        if (sh.image_bytes > stage_len - INVFS_BLOCK_SIZE ||
            sh.image_bytes % sizeof(invfs_l2p_entry) != 0)
            goto out;
        ib = sh.image_bytes;
        icrc = ib ? invfs_crc32c(buf + INVFS_BLOCK_SIZE, (size_t)ib) : 0;
        if (icrc != sh.image_crc)
            goto out;
        /* the chain seed covers the header fields preceding image_crc
         * (jrn_seed, static in volume.c -- keep the formula in sync) */
        prev = invfs_crc32c(&sh, offsetof(invfs_jrn_hdr, image_crc));
        if (ib) {
            const invfs_l2p_entry *le = (const invfs_l2p_entry *)
                (buf + INVFS_BLOCK_SIZE + ib - sizeof(invfs_l2p_entry));
            prev = le->crc;
        }
        jp2 = INVFS_BLOCK_SIZE + ib;
        while (jp2 + sizeof(invfs_l2p_entry) <= stage_len) {
            const invfs_l2p_entry *e = (const invfs_l2p_entry *)(buf + jp2);
            if (invfs_crc32c_update(prev, e,
                    offsetof(invfs_l2p_entry, crc)) != e->crc)
                break;
            prev = e->crc;
            jp2 += sizeof(*e);
        }
        if (jp2 != stage_len)
            goto out;
        /* verified: apply the image, then the log */
        prev = invfs_crc32c(&sh, offsetof(invfs_jrn_hdr, image_crc));
        for (off = 0; off < ib; off += sizeof(invfs_l2p_entry)) {
            const invfs_l2p_entry *e = (const invfs_l2p_entry *)
                (buf + INVFS_BLOCK_SIZE + off);
            if (l2p_apply(v, e) != 0) { rc = -1; goto out; }
            prev = e->crc;
            replayed++;
        }
        for (off = INVFS_BLOCK_SIZE + ib; off < stage_len;
             off += sizeof(invfs_l2p_entry)) {
            const invfs_l2p_entry *e = (const invfs_l2p_entry *)(buf + off);
            if (l2p_apply(v, e) != 0) { rc = -1; goto out; }
            prev = e->crc;
            replayed++;
        }
        v->j_slotted = 1;
        v->j_slot = (uint32_t)slot_idx;
        v->j_seq = sh.seq;
        v->j_last_crc = prev;
    } else {
        /* legacy flat log: every staged entry must pass its bare CRC (the
         * rollback's rule), then apply in order */
        for (off = 0; off + sizeof(invfs_l2p_entry) <= stage_len;
             off += sizeof(invfs_l2p_entry)) {
            invfs_l2p_entry e;
            memcpy(&e, buf + off, sizeof e);
            if (invfs_crc32c(&e, offsetof(invfs_l2p_entry, crc)) != e.crc)
                goto out;
        }
        for (off = 0; off + sizeof(invfs_l2p_entry) <= stage_len;
             off += sizeof(invfs_l2p_entry)) {
            invfs_l2p_entry e;
            memcpy(&e, buf + off, sizeof e);
            if (l2p_apply(v, &e) != 0) { rc = -1; goto out; }
            replayed++;
        }
        v->j_slotted = 0;
    }
    v->journal_pos = jpos;
    l2p_seed_heat(v);
    if (getenv("INVFS_DEBUG"))
        printf("[ckp_stage_replay] checkpoint #%llu: replayed %llu staged "
               "L2P entries (%s), journal_pos=%llu\n",
               (unsigned long long)v->ck.sweep_seq,
               (unsigned long long)replayed,
               stage_slotted ? "slotted" : "legacy",
               (unsigned long long)v->journal_pos);
    rc = 0;
out:
    free(buf);
    return rc;
}


int vol_rollback(invfs_volume *v, uint64_t *reclaimed_out)
{
    invfs_fsck_report rep;
    uint64_t jstart, jpos, iapos, stage_len;
    int stage_slotted = 0;

    if (reclaimed_out) *reclaimed_out = 0;
    if (!v) return -1;
    if (!v->ck_present) return 1;   /* "no checkpoint" */
    /* A live seal's parity stripes pin absolute block numbers; rolling
     * the volume back under them would silently invalidate the seal. */
    if (v->rd_present && (v->rd.l1_algo || v->rd.l2_algo))
        return -2;

    jstart = v->journal_start * (uint64_t)INVFS_BLOCK_SIZE;
    jpos = v->ck.journal_pos;
    iapos = v->ck.inode_area_pos;

    /* WP22d: the stage holds the journal prefix as it was at arm time --
     * the slot-format bytes (slot header + image + chained log) when the
     * arm happened post-migration, else the legacy flat log. Sniff the
     * first staged block for the JRN0 header to tell them apart. */
    if (v->ck.stage_blocks) {
        invfs_jrn_hdr sh;
        if (io_seek(&v->io, v->ck.stage_pba * (uint64_t)INVFS_BLOCK_SIZE) == 0 &&
            io_read(&v->io, &sh, sizeof sh) == 0 &&
            memcmp(sh.magic, INVFS_JRN_MAGIC, 4) == 0 &&
            sh.version == INVFS_JRN_VERSION &&
            invfs_crc32c(&sh, offsetof(invfs_jrn_hdr, crc32c)) == sh.crc32c)
            stage_slotted = 1;
    }
    if (stage_slotted) {
        /* the armed slot: the checkpoint's journal_pos names the append
         * point inside it; the stage content starts at that slot's base */
        uint64_t rel = (jpos / INVFS_BLOCK_SIZE) - v->journal_start;
        uint64_t idx = rel / INVFS_JRN_SLOT_BLOCKS;
        uint64_t rem = rel % INVFS_JRN_SLOT_BLOCKS;
        if (jpos < jstart || idx >= INVFS_JRN_SLOTS || rem == 0)
            return -3;
        stage_len = jpos - (jstart + idx * INVFS_JRN_SLOT_BLOCKS *
                            INVFS_BLOCK_SIZE);
    } else {
        stage_len = jpos - jstart;   /* underflow caught by the check below */
        if (jpos < jstart ||
            (stage_len % sizeof(invfs_l2p_entry)) != 0)
            return -3;
    }

    /* Descriptor sanity (the CRC already proved the bytes; these bounds
     * are what the writes below rely on). A failure here means the
     * descriptor or its staging was clobbered: refuse LOUDLY and leave
     * the post-sweep state untouched. */
    if (stage_len > (uint64_t)INVFS_JOURNAL_BLOCKS * INVFS_BLOCK_SIZE ||
        iapos < v->inode_area_start * (uint64_t)INVFS_BLOCK_SIZE ||
        iapos > v->inode_area_end ||
        (v->ck.stage_blocks &&
         (v->ck.stage_pba >= v->sb.total_blocks ||
          v->ck.stage_blocks > v->sb.total_blocks - v->ck.stage_pba)) ||
        stage_len > v->ck.stage_blocks * (uint64_t)INVFS_BLOCK_SIZE)
        return -3;

    /* Phase 1: restore the staged journal + decapitate the inode area at
     * the checkpoint -- the commit point. Skipped when a previous killed
     * attempt already landed it (both append pointers match), so a
     * re-run only re-executes the idempotent rebuild. NOTE: no vol_flush
     * in this phase -- it would append journal ops from the stale
     * in-memory table. Nothing here vol_mark_dirty's either, so vol_close
     * stays silent. */
    if (v->inode_area_pos > iapos || v->journal_pos != jpos) {
        if (stage_len) {
            uint8_t *buf = (uint8_t *)malloc((size_t)stage_len);
            uint64_t off;
            if (!buf) return -1;
            if (io_seek(&v->io, v->ck.stage_pba * (uint64_t)INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, buf, (size_t)stage_len) != 0) {
                free(buf);
                return -1;
            }
            /* verify the staging before anything is overwritten (the
             * RSZ0 rule): a reused or clobbered stage can never parse --
             * loud refusal beats a half-foreign journal. */
            if (stage_slotted) {
                /* header (already sniffed) + whole-image CRC + the
                 * chained log to its end */
                invfs_jrn_hdr sh;
                uint64_t ib, jp2;
                uint32_t prev, icrc;
                memcpy(&sh, buf, sizeof sh);
                if (sh.image_bytes > stage_len - INVFS_BLOCK_SIZE ||
                    sh.image_bytes % sizeof(invfs_l2p_entry) != 0) {
                    free(buf);
                    return -3;
                }
                ib = sh.image_bytes;
                icrc = ib ? invfs_crc32c(buf + INVFS_BLOCK_SIZE,
                                         (size_t)ib) : 0;
                if (icrc != sh.image_crc) { free(buf); return -3; }
                prev = invfs_crc32c(&sh, offsetof(invfs_jrn_hdr, image_crc));
                if (ib) {
                    const invfs_l2p_entry *le = (const invfs_l2p_entry *)
                        (buf + INVFS_BLOCK_SIZE + ib -
                         sizeof(invfs_l2p_entry));
                    prev = le->crc;
                }
                jp2 = INVFS_BLOCK_SIZE + ib;
                while (jp2 + sizeof(invfs_l2p_entry) <= stage_len) {
                    const invfs_l2p_entry *e = (const invfs_l2p_entry *)(buf + jp2);
                    if (invfs_crc32c_update(prev, e,
                            offsetof(invfs_l2p_entry, crc)) != e->crc)
                        break;
                    prev = e->crc;
                    jp2 += sizeof(*e);
                }
                if (jp2 != stage_len) { free(buf); return -3; }
                /* both slots get the staged bytes: a half-restored pair
                 * is still consistent (the staged header sequence is the
                 * same in both; a crash mid-restore is re-entered with
                 * CKP0 live) */
                {
                    uint32_t s;
                    for (s = 0; s < INVFS_JRN_SLOTS; s++) {
                        if (io_seek(&v->io, jrn_slot_base(v, s)) != 0 ||
                            io_write(&v->io, buf, (size_t)stage_len) != 0) {
                            free(buf);
                            return -1;
                        }
                    }
                }
                free(buf);
                /* the selector follows the restored content */
                v->sb.pad2 = INVFS_JSEL_SLOT0;
                if (vol_write_sb(v) != 0) return -1;
            } else {
                for (off = 0; off + sizeof(invfs_l2p_entry) <= stage_len;
                     off += sizeof(invfs_l2p_entry)) {
                    invfs_l2p_entry e;
                    memcpy(&e, buf + off, sizeof e);
                    if (invfs_crc32c(&e, offsetof(invfs_l2p_entry, crc)) != e.crc) {
                        free(buf);
                        return -3;
                    }
                }
                /* kill any live slot headers first: the restored bytes
                 * are the legacy flat log and must REPLAY as legacy */
                {
                    uint8_t zb[INVFS_BLOCK_SIZE];
                    memset(zb, 0, sizeof zb);
                    if (io_seek(&v->io, jstart) != 0 ||
                        io_write(&v->io, zb, sizeof zb) != 0 ||
                        io_seek(&v->io, jstart + (uint64_t)INVFS_JRN_SLOT_BLOCKS *
                                INVFS_BLOCK_SIZE) != 0 ||
                        io_write(&v->io, zb, sizeof zb) != 0) {
                        free(buf);
                        return -1;
                    }
                }
                if (io_seek(&v->io, jstart) != 0 ||
                    io_write(&v->io, buf, (size_t)stage_len) != 0) {
                    free(buf);
                    return -1;
                }
                free(buf);
                v->sb.pad2 = INVFS_JSEL_LEGACY;
                if (vol_write_sb(v) != 0) return -1;
            }
        }
        /* the terminator: replay stops exactly at the checkpoint. Only
         * the legacy log needs one (the chained slot log stops itself);
         * in the slotted restore the byte after the staged content simply
         * fails the chain. */
        if (!stage_slotted) {
            invfs_l2p_entry z;
            memset(&z, 0, sizeof z);
            z.crc = ~invfs_crc32c(&z, offsetof(invfs_l2p_entry, crc));
            if (io_seek(&v->io, jpos) != 0 ||
                io_write(&v->io, &z, sizeof z) != 0)
                return -1;
        }
        /* decapitate: zero the whole dead tail [iapos, old append
         * pointer), not just one guard block. The one-block guard was the
         * pre-WP27 shape: with nothing ever appended post-rollback, the
         * scan stopped at the zeros forever. WP27's close-time heat fold
         * DOES append in later read-only sessions, and once the appended
         * stream crosses a single guard block it can resync into the old
         * tail (the fold's [INOD][DELT] pairs share the sweep-era pairs'
         * sizes, so an exact record-boundary landing is likely, not
         * freak): the whole post-sweep history would resurrect. A fully
         * zeroed tail can never parse again. */
        {
            uint8_t *z;
            uint64_t zpos = iapos;
            uint64_t room = v->inode_area_end - iapos;
            uint64_t dead = v->inode_area_pos > iapos
                          ? v->inode_area_pos - iapos : 0;
            if (dead > room) dead = room;
            z = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
            if (!z) return -1;
            memset(z, 0, INVFS_BLOCK_SIZE);
            while (dead) {
                size_t n = dead > INVFS_BLOCK_SIZE ? INVFS_BLOCK_SIZE
                                                   : (size_t)dead;
                if (io_seek(&v->io, zpos) != 0 ||
                    io_write(&v->io, z, n) != 0) {
                    free(z);
                    return -1;
                }
                zpos += n;
                dead -= n;
            }
            free(z);
        }
#ifndef _WIN32
        if (rb_abort_at("restored")) {
            blkio_flush(&v->io);
            kill(getpid(), SIGKILL);
        }
#endif
        v->inode_area_pos = iapos;
        if (l2p_replay(v) != 0) return -1;
    }

    /* Phase 2: the ordinary fsck rebuild (reused, not reimplemented).
     * Bitmap and journal are rebuilt from the now-pre-sweep records
     * against the restored journal: retained blocks are referenced by the
     * resurrected records and stay live; everything the sweep allocated
     * (the new forms, the staging run, the registry's own blocks) is
     * orphaned and reclaimed; the journal is rewritten from the live
     * records; the superblock goes CLEAN. l2p_miss can only reflect
     * damage the pre-sweep state already had (rollback restores it
     * faithfully, damage included) -- reported, not fatal.
     *
     * WP22d note: the checkpoint is being dismantled RIGHT HERE, so the
     * fsck held-for-checkpoint accounting (blocks allocated-unreferenced
     * while CKP0 lives) must not classify this run's garbage as "held" --
     * clear the in-memory ck_present before the scan (the on-disk CKP0 is
     * cleared below, after the rebuild succeeded). */
    /* Phase 1b: discard the checkpoint's retention registry ("\x01reten*").
     * The registry is the SWEEP's output (it lists what the sweep retained),
     * so rolling back must remove it. On a legacy volume it used to die with
     * the linear inode area: the phase-1 truncation zeroes [iapos,
     * inode_area_pos). On a mapper volume the owner record lives in its own
     * dynamic metadata extent, OUTSIDE that truncation, so it survives and
     * the phase-2 rebuild counts its AST ranges (the checkpoint staging run
     * included) as referenced -- the staging run then reads as `missing`
     * once it is freed, and the resurrected (pre-sweep) state is not what
     * the rebuild sees. Delete the registry records here; their blocks are
     * reclaimed by the phase-2 rebuild exactly like the sweep's other
     * garbage. Retention for THIS dismantling is off (real frees). */
    v->retain = 0;
    v->retain_release = 1;
    ret_registry_delete(v, NULL);
    /* and every other sweep-derived owner ("\x01tzb"/tier/rawm): same reason,
     * same mapper-extent survival (see rollback_purge_owners). */
    rollback_purge_owners(v);
    v->retain_release = 0;

    v->ck_present = 0;
    if (vol_fsck_scan(v, &rep, 1) != 0) {
        fprintf(stderr, "rollback: the fsck rebuild failed; the volume is "
                "left for invf-fsck -f review (re-running invf-rollback "
                "is safe)\n");
        return -1;
    }
    if (rep.l2p_miss)
        fprintf(stderr, "rollback: warning: %llu AST segment(s) have no "
                "mapping even in the checkpointed journal -- pre-existing "
                "damage, restored as-is\n",
                (unsigned long long)rep.l2p_miss);

#ifndef _WIN32
    if (rb_abort_at("rebuilt")) {
        blkio_flush(&v->io);
        kill(getpid(), SIGKILL);
    }
#endif

    /* The point of no return has passed: clear the checkpoint last.
     * The cleared CKP0 was the only reference to the checkpoint's journal
     * staging run, so free it with the descriptor (same rule as
     * vol_ckp_realize). Without this an aborted-then-resumed rollback
     * (INVFS_ROLLBACK_ABORT_AT=rebuilt) left the staging run allocated and
     * unreferenced -- the observed E2 "orphans: 1" on block 10310. */
    {
        uint64_t spba = v->ck.stage_pba, sblocks = v->ck.stage_blocks;
        if (vol_write_ckp0(v, NULL) != 0)
            return -1;
        if (spba && sblocks)
            ckp_free_direct(v, spba, sblocks);
    }
    if (vol_flush(v) != 0)
        return -1;
    if (reclaimed_out) *reclaimed_out = rep.orphans;
    return 0;
}
