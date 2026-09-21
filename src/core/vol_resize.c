/* vol_resize.c — WP18 offline resize roll-forward (RSZ0 descriptor).
 * Split from volume.c. */

#include "volume_internal.h"


/* ---- WP18: offline resize roll-forward (descriptor: invarifs.h RSZ0) ---
 * invf-resize (resize.c) cannot move the metadata payload atomically: the
 * bitmap grows into the journal's head whenever total_blocks crosses a
 * 32768-block boundary, so the post-resize locations overlap the metadata
 * they replace. The tool therefore stages the whole payload (bitmap + used
 * journal + used inode records + WP71: the mapper table) in a
 * collision-free region, arms RSZ0 and
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
    uint64_t payload, m_bytes;
    if (rz->version != 1 && rz->version != 2) return 0;
    /* WP71: a v1 descriptor was staged by a pre-mapper-aware binary whose
     * layout math skipped the mapper slot; applying it to a mapper volume
     * would relocate the journal on top of the mapper table and destroy the
     * namespace. Never apply v1 to a mapper volume -- leave it ignored (the
     * RECOVERY path below keeps the volume read-only for invf-fsck -f). */
    if (rz->version == 1 && v->sb.meta_mapper_pba != 0) return 0;
    m_bytes = rz->version >= 2 ? rz->m_bytes : 0;
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
    payload = rz->bm_bytes + rz->j_bytes + rz->i_bytes + m_bytes;
    if (!rz->stage_blocks ||
        payload > (rz->stage_blocks - 1) * (uint64_t)INVFS_BLOCK_SIZE)
        return 0;
    /* WP71: a mapper volume's descriptor must carry the mapper component,
     * and its destination must be the new post-bitmap slot; a legacy
     * volume's must not. */
    if (rz->version >= 2) {
        int want_mapper = n->meta_mapper_pba != 0;
        if (want_mapper) {
            uint64_t bm_new = (n->total_blocks / 8 + INVFS_BLOCK_SIZE - 1) /
                              INVFS_BLOCK_SIZE;
            if (m_bytes != INVFS_META_EXT_BLOCKS * (uint64_t)INVFS_BLOCK_SIZE)
                return 0;
            if (n->meta_mapper_pba != n->metadata_zone_start + bm_new)
                return 0;
            if (rz->new_mapper_pba != n->meta_mapper_pba) return 0;
        } else if (m_bytes != 0 || rz->new_mapper_pba != 0) {
            return 0;
        }
    }
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
    /* WP71: the mapper slot sits between the bitmap and the journal on a
     * mapper volume (mkfs layout); legacy volumes keep the old adjacency. */
    uint64_t map_old = v->sb.meta_mapper_pba ? INVFS_META_EXT_BLOCKS : 0;
    uint64_t map_new = nsb->meta_mapper_pba ? INVFS_META_EXT_BLOCKS : 0;
    uint64_t m_bytes = rz->version >= 2 ? rz->m_bytes : 0;
    uint64_t new_js = nsb->metadata_zone_start + new_bm + map_new;
    uint64_t new_is = new_js + INVFS_JOURNAL_BLOCKS;
    uint64_t old_is = v->sb.metadata_zone_start + old_bm + map_old +
                      INVFS_JOURNAL_BLOCKS;
    int64_t rec_delta = ((int64_t)new_is - (int64_t)old_is) * INVFS_BLOCK_SIZE;
    uint64_t new_iend = (nsb->metadata_zone_start + nsb->metadata_zone_blocks)
                        * (uint64_t)INVFS_BLOCK_SIZE;
    uint64_t sbase = (rz->stage_start + 1) * (uint64_t)INVFS_BLOCK_SIZE;
    uint64_t isrc, icons, odst, wdone;
    uint8_t *buf = NULL, *bm = NULL, *wbuf = NULL, *rec = NULL;
    size_t wlen = 0;
    int rc = -1;

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
        memset(&sh, 0, sizeof sh);
        if (rz->version == 1) {
            /* legacy 40-byte header: [magic4][ver4][bm8][j8][i8][pcrc4][crc4]
             * -- parse positionally, never through the v2 struct. The v1
             * CRC covered all 40 bytes with the crc field zeroed. */
            const uint8_t *hb = buf;
            uint8_t v1h[40];
            uint32_t v1_crc_want, v1_crc_calc;
            if (memcmp(hb, "RSZS", 4) != 0) goto out;
            memcpy(&sh.magic, hb, 4);
            sh.version = 1;
            memcpy(&sh.bm_bytes, hb + 8, 8);
            memcpy(&sh.j_bytes,  hb + 16, 8);
            memcpy(&sh.i_bytes,  hb + 24, 8);
            memcpy(&sh.payload_crc, hb + 32, 4);
            memcpy(&v1_crc_want, hb + 36, 4);
            sh.m_bytes = 0;
            sh.crc32c = v1_crc_want;
            memcpy(v1h, hb, 40);
            memset(v1h + 36, 0, 4);
            v1_crc_calc = invfs_crc32c(v1h, 40);
            if (v1_crc_calc != v1_crc_want) goto out;
        } else {
            memcpy(&sh, buf, sizeof sh);
            if (memcmp(sh.magic, "RSZS", 4) != 0 || sh.version != 2)
                goto out;
            {
                invfs_rszs t = sh;
                uint32_t want = sh.crc32c;
                t.crc32c = 0;
                if (invfs_crc32c(&t, sizeof t) != want)
                    goto out;
            }
        }
        if (sh.bm_bytes != rz->bm_bytes || sh.j_bytes != rz->j_bytes ||
            sh.i_bytes != rz->i_bytes || sh.m_bytes != m_bytes)
            goto out;
        left = rz->bm_bytes + rz->j_bytes + rz->i_bytes + m_bytes;
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

    /* -- WP71: mapper table -- staged bytes verbatim at the new slot
     *    (metadata_zone_start + new bitmap blocks). Must land BEFORE the
     *    journal: the journal's new base sits right past the mapper slot,
     *    and the pre-WP71 bug was exactly this overlap destroying the
     *    table. Entries are absolute pbas; a single-device grow/shrink
     *    never moves data-zone blocks, so the table stays valid as-is. -- */
    if (m_bytes) {
        uint64_t left = m_bytes;
        uint64_t off = sbase + rz->bm_bytes + rz->j_bytes + rz->i_bytes;
        uint64_t dst = nsb->meta_mapper_pba * (uint64_t)INVFS_BLOCK_SIZE;
        if (nsb->meta_mapper_pba == 0) goto out;   /* descriptor inconsistency */
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            if (io_seek(&v->io, off) != 0 || io_read(&v->io, buf, n) != 0 ||
                io_seek(&v->io, dst) != 0 || io_write(&v->io, buf, n) != 0)
                goto out;
            off += n;
            dst += n;
            left -= n;
        }
    }

    /* -- journal: staged bytes verbatim, then a fresh terminator (the
     *    vol_flush convention) so replay can never run into the stale bytes
     *    the overlapping old areas left behind. WP22d: the staged bytes
     *    ARE the winning slot's [header + image + chained log] when the
     *    volume is slotted (the chain terminates the replay by itself;
     *    the terminator is a harmless extra bad entry then) -- and slot 1
     *    of the new area must hold no stale JRN0 header that could compete
     *    with the staged slot 0. -- */
    {
        uint64_t left = rz->j_bytes, off = sbase + rz->bm_bytes;
        uint64_t dst = new_js * (uint64_t)INVFS_BLOCK_SIZE;
        int slotted = 0;
        while (left) {
            size_t n = left > BLKIO_BOUNCE ? BLKIO_BOUNCE : (size_t)left;
            if (io_seek(&v->io, off) != 0 || io_read(&v->io, buf, n) != 0 ||
                io_seek(&v->io, dst) != 0 || io_write(&v->io, buf, n) != 0)
                goto out;
            if (off == sbase + rz->bm_bytes) {
                invfs_jrn_hdr jh;
                memcpy(&jh, buf, sizeof jh);
                slotted = memcmp(jh.magic, INVFS_JRN_MAGIC, 4) == 0 &&
                          jh.version == INVFS_JRN_VERSION;
            }
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
        if (slotted) {
            /* the staged content IS slot 0 of the new area; slot 1 must
             * read as absent (stale bytes there could otherwise parse as
             * a competing header after a later drop) */
            memset(wbuf, 0, INVFS_BLOCK_SIZE);
            if (io_seek(&v->io, (new_js + INVFS_JRN_SLOT_BLOCKS) *
                        (uint64_t)INVFS_BLOCK_SIZE) != 0 ||
                io_write(&v->io, wbuf, INVFS_BLOCK_SIZE) != 0)
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
        /* WP21: the resize moved the journal + inode area, so a live sweep
         * checkpoint's positions are meaningless from here on. Live CKP0 is
         * refused at preflight (see resize.c); a descriptor that still slips
         * through (crash between preflight and commit) is cleared here and
         * its staging/registry blocks are reclaimed by the next sweep's
         * defensive realize -- never freed blindly from under the L2P. */
        memset(blk + INVFS_CKP0_OFF, 0, sizeof(invfs_ckp0));
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
