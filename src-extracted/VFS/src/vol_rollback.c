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
 * -- but vol_flush rewrites the journal from l2p_dirty on every flush
 * (a sweep's first flush rewrites it wholesale), so the pre-sweep L2P
 * does NOT survive the sweep in place. The stage is the RSZ0 pattern in
 * miniature: durable copy first, descriptor naming it second.
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
 *  - invf-sweep --realize (and the next sweep's automatic pre-realize):
 *    the registry owners are deleted -- the retire path frees exactly the
 *    mapped ranges -- and CKP0 is cleared. The point of no return.
 *  - invf-rollback     : journal prefix restored from the stage, inode
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
    invfs_ckp0 ck;

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
    if (v->ck_present) {
        /* the driver realizes the previous checkpoint before arming;
         * reaching this means a bug or a hand-run sequence -- decline,
         * never overwrite */
        fprintf(stderr, "checkpoint: declined (checkpoint #%llu still "
                        "live)\n", (unsigned long long)v->ck.sweep_seq);
        return 0;
    }

    /* Stage the used journal prefix (see the section comment for why the
     * inode area needs no staging). */
    jstart = v->journal_start * (uint64_t)INVFS_BLOCK_SIZE;
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
                           v->sb.raw_zone_blocks, sblocks, 1);
        if (!pba)
            pba = alloc_blocks(v, v->sb.shadow_zone_start,
                               v->sb.shadow_zone_blocks, sblocks, 1);
        if (!pba) {
            fprintf(stderr, "checkpoint: declined (no %llu-block run for "
                            "the journal staging)\n",
                    (unsigned long long)sblocks);
            return 0;
        }
        buf = (uint8_t *)malloc(BLKIO_BOUNCE);
        if (!buf) { vol_free_blocks(v, pba, sblocks); return -1; }
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
        if (!ok) { vol_free_blocks(v, pba, sblocks); return -1; }
        /* the staging allocation must be durable before CKP0 can name it */
        if (vol_flush(v) != 0) {
            vol_free_blocks(v, pba, sblocks);
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
        if (pba) vol_free_blocks(v, pba, sblocks);
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
            vol_free_blocks(v, v->ck_stage_pba, v->ck_stage_blocks);
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


int vol_ckp_realize(invfs_volume *v, uint64_t *freed_blocks_out)
{
    uint64_t freed = 0;
    uint64_t shard;
    int did = 0;

    if (freed_blocks_out) *freed_blocks_out = 0;
    if (!v) return -1;
    /* A realize IS the end of the retention window: if it runs inside a
     * checkpoint-armed session (it normally runs before the next arm),
     * the frees must be real. */
    v->retain = 0;

    /* Delete every registry shard: the retire path frees exactly the
     * ranges the shard maps (zone==BINARY entries, so the batch-owner
     * gate does not hold them). Its PB7 sharer check can find no live
     * reference to a retained block: retained blocks stayed allocated
     * throughout the window, so nothing was ever reallocated onto one. */
    for (shard = 0; ; shard++) {
        char nm[32];
        uint64_t oid;
        size_t j;
        ret_shard_name(shard, nm, sizeof nm);
        oid = vol_find(v, nm);
        if (!oid) break;   /* shards exist contiguously (the seal rule) */
        if (!vol_write_enabled(v)) return -1;
        for (j = 0; j < v->l2p_count; j++) {
            const invfs_l2p_entry *e = &v->l2p[j];
            if (e->type == INVFS_JRN_MAP && e->inode == oid)
                freed += e->length;
        }
        if (vol_delete_file(v, nm) != 0) {
            fprintf(stderr, "checkpoint: realize: could not delete %s\n",
                    nm + 1);
            return -1;
        }
        did = 1;
    }
    if (v->ck_present) {
        if (vol_write_ckp0(v, NULL) != 0) return -1;
        did = 1;
    }
    if (did && vol_flush(v) != 0) return -1;
    if (freed_blocks_out) *freed_blocks_out = freed;
    return did;
}


int vol_rollback(invfs_volume *v, uint64_t *reclaimed_out)
{
    invfs_fsck_report rep;
    uint64_t jstart, jpos, iapos, stage_len;

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
    stage_len = jpos - jstart;   /* underflow caught by the check below */

    /* Descriptor sanity (the CRC already proved the bytes; these bounds
     * are what the writes below rely on). A failure here means the
     * descriptor or its staging was clobbered: refuse LOUDLY and leave
     * the post-sweep state untouched. */
    if (jpos < jstart ||
        (stage_len % sizeof(invfs_l2p_entry)) != 0 ||
        jpos + sizeof(invfs_l2p_entry) >
            jstart + (uint64_t)INVFS_JOURNAL_BLOCKS * INVFS_BLOCK_SIZE ||
        iapos < v->inode_area_start * (uint64_t)INVFS_BLOCK_SIZE ||
        iapos > v->inode_area_end ||
        (v->ck.stage_blocks &&
         (v->ck.stage_pba >= v->sb.total_blocks ||
          v->ck.stage_blocks > v->sb.total_blocks - v->ck.stage_pba)) ||
        stage_len > v->ck.stage_blocks * (uint64_t)INVFS_BLOCK_SIZE)
        return -3;

    /* Phase 1: restore the staged journal prefix + decapitate the inode
     * area at the checkpoint -- the commit point. Skipped when a previous
     * killed attempt already landed it (both append pointers match), so a
     * re-run only re-executes the idempotent rebuild. NOTE: no vol_flush
     * in this phase -- it would rewrite the journal from the stale
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
            /* verify the staging by REPLAYING it before anything is
             * overwritten: every entry must be CRC-valid. A reused or
             * clobbered stage can never pass -- loud refusal beats a
             * half-foreign journal. */
            for (off = 0; off + sizeof(invfs_l2p_entry) <= stage_len;
                 off += sizeof(invfs_l2p_entry)) {
                invfs_l2p_entry e;
                memcpy(&e, buf + off, sizeof e);
                if (invfs_crc32c(&e, offsetof(invfs_l2p_entry, crc)) != e.crc) {
                    free(buf);
                    return -3;
                }
            }
            if (io_seek(&v->io, jstart) != 0 ||
                io_write(&v->io, buf, (size_t)stage_len) != 0) {
                free(buf);
                return -1;
            }
            free(buf);
        }
        /* the terminator: replay stops exactly at the checkpoint (the
         * vol_flush convention) */
        {
            invfs_l2p_entry z;
            memset(&z, 0, sizeof z);
            z.crc = ~invfs_crc32c(&z, offsetof(invfs_l2p_entry, crc));
            if (io_seek(&v->io, jpos) != 0 ||
                io_write(&v->io, &z, sizeof z) != 0)
                return -1;
        }
        /* decapitate: zero the guard so every record scan (vol_open AND
         * fsck's full-tail pass) stops exactly at the checkpoint; the
         * pre-sweep prefix is byte-intact (append-only), the post-sweep
         * bytes past the guard are dead space future appends overwrite */
        {
            uint8_t z[INVFS_BLOCK_SIZE];
            uint64_t room = v->inode_area_end - iapos;
            memset(z, 0, sizeof z);
            if (room > 0) {
                if (room > sizeof z) room = sizeof z;
                if (io_seek(&v->io, iapos) != 0 ||
                    io_write(&v->io, z, (size_t)room) != 0)
                    return -1;
            }
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
     * faithfully, damage included) -- reported, not fatal. */
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

    /* The point of no return has passed: clear the checkpoint last. */
    if (vol_write_ckp0(v, NULL) != 0)
        return -1;
    if (vol_flush(v) != 0)
        return -1;
    if (reclaimed_out) *reclaimed_out = rep.orphans;
    return 0;
}
