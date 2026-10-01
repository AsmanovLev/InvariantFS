/* vol_resize.c — WP18 offline resize roll-forward (RSZ0 descriptor).
 * Split from volume.c. */

#include "volume_internal.h"
#include "vol_anchor.h"


/* ---- WP18: offline resize roll-forward (descriptor: invarifs.h RSZ0) ---
 * invf-resize (resize.c) cannot move the metadata payload atomically: the
 * bitmap grows into the journal's head whenever total_blocks crosses a
 * 32768-block boundary, so the post-resize locations overlap the metadata
 * they replace. The tool therefore stages the whole payload (bitmap + used
 * journal + used inode records) in a collision-free region, arms RSZ0 and
 * flips the superblock to RECOVERY -- and the apply runs HERE, at the next
 * open, whether that open is the tool's own or any later one after a crash.
 * The apply reads only the staging area, so re-entering it after a crash
 * mid-apply is safe; the commit (new superblock + cleared descriptor, one
 * block-0 rewrite) lands last. */
uint32_t rsz0_crc(const invfs_rsz0 *rz)
{
    invfs_rsz0 t = *rz;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}


/* The descriptor's CRC covers the embedded superblock; these are the
 * invariants the rest of vol_open relies on, verified before anything is
 * written (a descriptor failing here is ignored, never applied).
 * WP25: on a two-device volume the canonical shadow starts on dev1, past
 * the mirrored metadata span -- the single-device zone contiguity rule
 * would misread that, so the 2-dev geometry gets its own check. */
int rsz0_sane(invfs_volume *v, const invfs_rsz0 *rz)
{
    const invfs_superblock *n = &rz->new_sb;
    uint64_t payload;
    if (rz->version != 1) return 0;
    if (memcmp(n->magic, INVFS_MAGIC, 8) != 0) return 0;
    if (invfs_crc32c(n, offsetof(invfs_superblock, checksum)) != n->checksum)
        return 0;
    if (n->block_size != INVFS_BLOCK_SIZE) return 0;
    if (n->metadata_zone_start != 1) return 0;
    if (n->raw_zone_start != 1 + n->metadata_zone_blocks) return 0;
    if (v->ndev == 2) {
        /* shadow = dev1 canonical area, above the metadata mirror span;
         * dev0's layout (metadata + RAW + tier arena) never moves */
        uint64_t want = v->dev0_blocks + 1 + n->metadata_zone_blocks +
                        n->raw_zone_blocks;
        if (n->shadow_zone_start != want) return 0;
        if (n->raw_zone_start + n->raw_zone_blocks > v->dev0_blocks)
            return 0;
    } else {
        if (n->shadow_zone_start != n->raw_zone_start + n->raw_zone_blocks)
            return 0;
    }
    if (n->shadow_zone_start + n->shadow_zone_blocks != n->total_blocks)
        return 0;
    if (n->total_blocks == rz->old_total) return 0;   /* not a resize */
    payload = rz->bm_bytes + rz->j_bytes + rz->i_bytes;
    if (!rz->stage_blocks ||
        payload > (rz->stage_blocks - 1) * (uint64_t)INVFS_BLOCK_SIZE)
        return 0;
    return 1;
}


#ifndef _WIN32

/* Test hook (tools/test-resize.sh): die mid-roll-forward, after the payload
 * was rewritten at its new location but before the commit. The next open
 * re-enters the apply and finishes -- the exact crash window the staging
 * design exists for. */
static int rsz_abort_at(const char *stage)
{
    const char *a = getenv("INVFS_RESIZE_ABORT_AT");
    return a && strcmp(a, stage) == 0;
}

#endif


/* Apply an armed resize from its staging area; commit on success.
 * Returns 0 with v->sb already the new superblock, -1 on failure. */
int vol_rsz0_apply(invfs_volume *v, const invfs_rsz0 *rz)
{
    const invfs_superblock *nsb = &rz->new_sb;
    uint64_t old_bm = (v->sb.total_blocks / 8 + INVFS_BLOCK_SIZE - 1) /
                      INVFS_BLOCK_SIZE;
    uint64_t new_bm = (nsb->total_blocks / 8 + INVFS_BLOCK_SIZE - 1) /
                      INVFS_BLOCK_SIZE;
    uint64_t new_js = nsb->metadata_zone_start + new_bm;
    uint64_t new_is = new_js + INVFS_JOURNAL_BLOCKS;
    uint64_t old_is = v->sb.metadata_zone_start + old_bm + INVFS_JOURNAL_BLOCKS;
    int64_t rec_delta = ((int64_t)new_is - (int64_t)old_is) * INVFS_BLOCK_SIZE;
    uint64_t new_iend = (nsb->metadata_zone_start + nsb->metadata_zone_blocks)
                        * (uint64_t)INVFS_BLOCK_SIZE;
    uint64_t sbase = (rz->stage_start + 1) * (uint64_t)INVFS_BLOCK_SIZE;
    uint64_t isrc, icons, odst, wdone;
    uint8_t *buf = NULL, *bm = NULL, *wbuf = NULL, *rec = NULL;
    size_t wlen = 0;
    int rc = -1;
    /* ANC0: was there an anchor at the OLD tail? (see below) */
    invfs_anc0 old_anchor;
    int had_anchor = 0;

    buf = (uint8_t *)malloc(BLKIO_BOUNCE);
    wbuf = (uint8_t *)malloc(BLKIO_BOUNCE);
    if (!buf || !wbuf) goto out;

    /* -- staging header + payload CRC: the apply's only input is verified
     *    before anything is overwritten -- */
    {
        invfs_rszs sh;
        uint64_t left, off;
        uint32_t pcrc = 0;
        if (io_seek(&v->io, rz->stage_start * (uint64_t)INVFS_BLOCK_SIZE) != 0 ||
            io_read(&v->io, buf, INVFS_BLOCK_SIZE) != 0)
            goto out;
        memcpy(&sh, buf, sizeof sh);
        if (memcmp(sh.magic, "RSZS", 4) != 0 || sh.version != 1 ||
            sh.bm_bytes != rz->bm_bytes || sh.j_bytes != rz->j_bytes ||
            sh.i_bytes != rz->i_bytes)
            goto out;
        {
            invfs_rszs t = sh;
            uint32_t want = sh.crc32c;
            t.crc32c = 0;
            if (invfs_crc32c(&t, sizeof t) != want)
                goto out;
        }
        left = rz->bm_bytes + rz->j_bytes + rz->i_bytes;
        off = sbase;
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            if (io_seek(&v->io, off) != 0 || io_read(&v->io, buf, n) != 0)
                goto out;
            pcrc = invfs_crc32c_update(pcrc, buf, n);
            off += n;
            left -= n;
        }
        if (pcrc != sh.payload_crc)
            goto out;
    }

    /* -- bitmap: the staged old one, re-sized (zero-grown / tail-cleared) -- */
    {
        uint64_t new_bm_bytes = new_bm * (uint64_t)INVFS_BLOCK_SIZE;
        uint64_t cap = rz->bm_bytes > new_bm_bytes ? rz->bm_bytes
                                                   : new_bm_bytes;
        bm = (uint8_t *)calloc(1, (size_t)cap);
        if (!bm) goto out;
        {
            uint64_t left = rz->bm_bytes, off = sbase, put = 0;
            while (left) {
                size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
                if (io_seek(&v->io, off) != 0 || io_read(&v->io, bm + put, n) != 0)
                    goto out;
                off += n;
                put += n;
                left -= n;
            }
        }
        if (new_bm_bytes > rz->bm_bytes)
            memset(bm + rz->bm_bytes, 0, (size_t)(new_bm_bytes - rz->bm_bytes));
        /* -- ANC0: the resize MOVES the anchor, and this WP INVALIDATES it
         * rather than relocating it. That choice is forced, and worth stating
         * because the alternative looks attractive:
         *
         *   - a GROW would make the new last block freshly-free, so it could
         *     be claimed. But this apply is a crash-recovery roll-forward:
         *     adding an anchor write to the middle of it is a new way for
         *     the commit to half-happen, and the old tail block would be
         *     left allocated at an address nothing will ever read again.
         *   - a SHRINK makes the new last block an arbitrary block that may
         *     hold live file data. Writing an anchor there is not a leak, it
         *     is DATA DESTRUCTION, and nothing at this point can tell the
         *     two apart without a liveness walk of the whole volume inside
         *     a commit.
         *
         * So the anchor is invalidated: the old block is zeroed at the
         * commit (only if the probe below proved one was really there), its
         * bit goes back to the free pool on a grow, and the volume is left
         * anchor-less -- which is exactly, and verifiably, the state of
         * every volume made before this descriptor existed. The cost is
         * that a RESIZED volume has no anchor until it is re-formatted; the
         * alternative costs correctness on shrink. -- */
        if (v->sb.total_blocks >= 2) {
            invfs_anc0 a;
            uint64_t old_tail = v->sb.total_blocks - 1;
            if (io_pread(&v->io, old_tail * (uint64_t)INVFS_BLOCK_SIZE,
                         &a, sizeof a) == 0 &&
                anchor_state_of(&a) == INVFS_ANCHOR_OK &&
                anchor_fp_matches(&a, v->sb.total_blocks, v->sb.block_size,
                                  v->sb.format_version,
                                  (const uint8_t *)v->sb.uuid)) {
                had_anchor = 1;
                old_anchor = a;
                /* grow: the block is now interior and unowned, so give it
                 * back. shrink: its bit is cleared with every other bit past
                 * the new end, just below. */
                if (old_tail < nsb->total_blocks)
                    bm[old_tail / 8] &= (uint8_t)~(1u << (old_tail % 8));
            }
        }

        /* bits past the end of the (new) volume are never allocated */
        {
            uint64_t b, top = new_bm_bytes * 8;
            for (b = nsb->total_blocks; b < top; b++)
                bm[b / 8] &= (uint8_t)~(1u << (b % 8));
        }
        if (io_seek(&v->io, nsb->metadata_zone_start * (uint64_t)INVFS_BLOCK_SIZE) != 0 ||
            io_write(&v->io, bm, (size_t)new_bm_bytes) != 0)
            goto out;
    }

    /* -- the gap: staged bytes verbatim, then a fresh terminator (the
     *    vol_flush convention) so nothing can run into the stale bytes the
     *    overlapping old areas left behind. On a v3 volume the gap holds no
     *    staged bytes at all, so this is the terminator alone. -- */
    {
        uint64_t left = rz->j_bytes, off = sbase + rz->bm_bytes;
        uint64_t dst = new_js * (uint64_t)INVFS_BLOCK_SIZE;
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            if (io_seek(&v->io, off) != 0 || io_read(&v->io, buf, n) != 0 ||
                io_seek(&v->io, dst) != 0 || io_write(&v->io, buf, n) != 0)
                goto out;
            off += n;
            dst += n;
            left -= n;
        }
        {
            invfs_l2p_entry z;
            memset(&z, 0, sizeof z);
            z.crc = ~invfs_crc32c(&z, offsetof(invfs_l2p_entry, crc));
            if (io_seek(&v->io, dst) != 0 ||
                io_write(&v->io, &z, sizeof z) != 0)
                goto out;
        }
    }

    /* -- inode records: verbatim EXCEPT v2 position-kill tombstones, whose
     *    file_size is an absolute record offset and shifts with the area.
     *    Only CRC-valid DELT records are patched (a corrupt one is what the
     *    open scan would skip; it is copied byte-identically) -- */
    isrc = sbase + rz->bm_bytes + rz->j_bytes;
    icons = 0;
    odst = new_is * (uint64_t)INVFS_BLOCK_SIZE;
    wdone = 0;
    while (icons < rz->i_bytes) {
        invfs_inode_rec rh;
        uint32_t rl, crc_stored, crc_calc;
        size_t total;
        if (rz->i_bytes - icons < sizeof(rh)) goto out;
        if (io_seek(&v->io, isrc + icons) != 0 ||
            io_read(&v->io, &rh, sizeof rh) != 0)
            goto out;
        if (rh.magic != INODE_REC_MAGIC && rh.magic != TOMBSTONE_MAGIC)
            goto out;
        if (rh.rec_len < INVFS_REC_HDR_LEN + 1 ||
            rh.rec_len > INVFS_MAX_REC_LEN ||
            (uint64_t)rh.rec_len + 4 > rz->i_bytes - icons)
            goto out;
        rl = rh.rec_len;
        total = (size_t)rl + 4;
        rec = (uint8_t *)realloc(rec, total);
        if (!rec) goto out;
        if (io_seek(&v->io, isrc + icons) != 0 ||
            io_read(&v->io, rec, total) != 0)
            goto out;
        memcpy(&crc_stored, rec + rl, 4);
        crc_calc = invfs_crc32c(rec, rl);
        if (rh.magic == TOMBSTONE_MAGIC && crc_calc == crc_stored) {
            uint64_t kill;
            memcpy(&kill, rec + offsetof(invfs_inode_rec, file_size), 8);
            if (kill) {
                int64_t nk = (int64_t)kill + rec_delta;
                if (nk < (int64_t)(new_is * (uint64_t)INVFS_BLOCK_SIZE) ||
                    (uint64_t)nk >= new_iend)
                    goto out;
                memcpy(rec + offsetof(invfs_inode_rec, file_size), &nk, 8);
                crc_calc = invfs_crc32c(rec, rl);
                memcpy(rec + rl, &crc_calc, 4);
            }
        }
        if (wlen + total > BLKIO_BOUNCE) {
            if (io_seek(&v->io, odst + wdone) != 0 ||
                io_write(&v->io, wbuf, wlen) != 0)
                goto out;
            wdone += wlen;
            wlen = 0;
        }
        memcpy(wbuf + wlen, rec, total);
        wlen += total;
        icons += total;
    }
    if (wlen) {
        if (io_seek(&v->io, odst + wdone) != 0 ||
            io_write(&v->io, wbuf, wlen) != 0)
            goto out;
        wdone += wlen;
    }
    if (wdone != rz->i_bytes) goto out;   /* staging drifted mid-apply */
    /* the scan must stop exactly at the stream end: zero a guard block so
     * whatever the old areas left behind never parses as one more record */
    if (odst + wdone + INVFS_BLOCK_SIZE > new_iend) goto out;
    memset(wbuf, 0, INVFS_BLOCK_SIZE);
    if (io_seek(&v->io, odst + wdone) != 0 ||
        io_write(&v->io, wbuf, INVFS_BLOCK_SIZE) != 0)
        goto out;

#ifndef _WIN32
    if (rsz_abort_at("moved")) {
        vmux_barrier(v, 0);
        kill(getpid(), SIGKILL);
    }
#endif

    /* -- commit: one block-0 rewrite carries the new superblock and clears
     *    the descriptor; the staging area is dead weight in free space from
     *    here on -- */
    {
        uint8_t blk[INVFS_BLOCK_SIZE];
        if (io_seek(&v->io, 0) != 0 || io_read(&v->io, blk, sizeof blk) != 0)
            goto out;
        memcpy(blk, nsb, sizeof *nsb);
        memset(blk + INVFS_RSZ0_OFF, 0, sizeof(invfs_rsz0));
        /* 0x220..0x258 is reserved and nothing has written there since the
         * sweep checkpoint went; the bytes are left exactly as they were
         * found rather than re-zeroed, so this commit writes no field the
         * layout does not account for. */
        /* WP25: the device table follows the growth (the tail device
         * absorbed it) -- same block-0 write, so the table and the
         * superblock never disagree on disk. */
        if (v->ndev == 2) {
            invfs_devt dt = v->devt;
            dt.dev_blocks[1] = nsb->total_blocks - dt.dev_blocks[0];
            dt.crc32c = 0;
            dt.crc32c = devt_crc(&dt);
            memcpy(blk + INVFS_DEVT_OFF, &dt, sizeof dt);
            v->devt = dt;
            v->dev1_blocks = dt.dev_blocks[1];
        }
        if (io_seek(&v->io, 0) != 0 || io_write(&v->io, blk, sizeof blk) != 0)
            goto out;
    }
    /* ANC0: zero the block the anchor used to live in, so the new
     * total_blocks-1 address can never be confused with the old one. This is
     * AFTER the block-0 commit on purpose: that write clears RSZ0, so once
     * it has landed this apply cannot be re-entered, and there is no window
     * in which the superblock says "resized" while the old anchor is still
     * standing. */
    if (had_anchor) {
        uint64_t old_tail = v->sb.total_blocks - 1;
        if (anchor_invalidate_at(v, old_tail, &old_anchor) != 0)
            goto out;
        fprintf(stderr, "vol: ANC0 tail anchor at block %llu invalidated by "
                "the resize: total_blocks moved %llu -> %llu, so the anchor's "
                "address moved with it. This volume has NO anchor until it is "
                "re-formatted -- a loss of block 0 is unrecoverable on it.\n",
                (unsigned long long)old_tail,
                (unsigned long long)v->sb.total_blocks,
                (unsigned long long)nsb->total_blocks);
    }
    vmux_barrier(v, "resize commit");
    v->sb = *nsb;
    rc = 0;
out:
    free(buf);
    free(bm);
    free(wbuf);
    free(rec);
    return rc;
}
