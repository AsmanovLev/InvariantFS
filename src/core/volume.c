/*
 * volume.c — InvariantFS volume access layer
 *
 *   vol_open / vol_close
 *   vol_alloc  (bitmap free-list cursor)
 *   vol_write_raw (allocate blocks in RAW zone + write)
 *   vol_read_block (read raw bytes at pba)
 *   vol_flush  (bitmap range + two-device commit tail)
 *
 * Metadata zone layout:
 *   [0 .. bitmap_blocks):            block bitmap
 *   [bitmap_blocks .. +mapper):      metadata extent mapper (MET0 + table)
 *   [bitmap_blocks+mapper .. +res):  reserved (unused on this format)
 *   [bitmap_blocks+reserved .. end): append-only record area
 *
 * The v3 namespace itself is NOT here: it is the RT30 double slot plus a
 * COW B+ tree base and an append-only Delta Log (vol_metabuf.c,
 * vol_delta.c, vol_btree.c, vol_fold.c). The record area above is the
 * metadata EXTENT pool the tree's base pages are allocated from.
 */

/* _GNU_SOURCE for PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP (the pba-ref map's
 * own lock, see g_pba_ref_mu below) — the vol_cpack.c pattern */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "volume_internal.h"
#include "vol_metabuf.h"
#include "vol_delta.h"
#include "vol_spt0.h"
#include "vol_anchor.h"


static const uint64_t JOURNAL_BLOCKS = INVFS_JOURNAL_BLOCKS;
static int g_sweep_ui_active;

void invfs_sweep_ui_set(int active)
{
    g_sweep_ui_active = active ? 1 : 0;
}

int invfs_sweep_ui_active(void)
{
    return g_sweep_ui_active;
}


/* vol_bm_dirty() (volume_internal.h) widens the dirty byte range so
 * vol_flush writes just that slice instead of the whole bitmap. */


/* CRC convention: over the 24-byte descriptor with the crc32c field
 * itself read as zero (i.e. the bytes 0x100..0x117 of block 0). */
static uint32_t rdp0_crc(const invfs_rdp0 *rd)
{
    invfs_rdp0 t = *rd;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}


/* Persist (rd != NULL) or clear (rd == NULL) the RDP0 descriptor by
 * read-modify-write of the whole block 0. vol_write_sb only ever writes
 * the 144-byte struct at offset 0, so the reserved tail survives state
 * flips; on a raw device the full-block write is what the alignment
 * demands anyway. The in-memory copy follows the disk state. */
int vol_write_rdp0(invfs_volume *v, const invfs_rdp0 *rd)
{
    uint8_t blk[INVFS_BLOCK_SIZE];
    if (io_seek(&v->io, 0) != 0 ||
        io_read(&v->io, blk, sizeof blk) != 0)
        return -1;
    if (rd) {
        invfs_rdp0 t = *rd;
        memcpy(t.magic, "RDP0", 4);
        t.pad = 0;
        t.crc32c = 0;
        t.crc32c = rdp0_crc(&t);
        memcpy(blk + INVFS_RDP0_OFF, &t, sizeof t);
        v->rd = t;
        v->rd_present = 1;
    } else {
        memset(blk + INVFS_RDP0_OFF, 0, sizeof(invfs_rdp0));
        memset(&v->rd, 0, sizeof v->rd);
        v->rd_present = 0;
    }
    if (io_seek(&v->io, 0) != 0 ||
        io_write(&v->io, blk, sizeof blk) != 0)
        return -1;
    return 0;
}



/* ==================== WP25: two-device mux ====================
 * The engine addresses the volume in the GLOBAL block space (the
 * concatenation of dev0 + dev1; the DEVT descriptor at block 0 carries
 * the per-device sizes). The mux is the only place that knows the split:
 *
 *   - [0, meta_end_bytes): the metadata span (superblock+descriptors,
 *     bitmap, journal, inode area). Mirrored on BOTH devices at the SAME
 *     local offsets. Writes are writethrough to both before the commit
 *     barrier; a dev0 write failure logs + drops dev0 for the rest of the
 *     session (its DEVT sync_seq then necessarily lags -> the next open
 *     resyncs it from dev1, newest state wins); a dev1 failure latches
 *     the volume (WP22c semantics: dev1 is the canonical store).
 *   - [meta_end, dev0_blocks*4K): dev0 data (the RAW zone + the tier
 *     arena). Read/write dev0; a failure is the caller's business (the
 *     read path fails over to the dev1 mirror, see seg_read_checked).
 *   - [dev0_blocks*4K, ...): dev1 data (the canonical shadow zone), local
 *     offset = global - dev0 end.
 *
 * Degraded mount (dev0 absent): metadata reads come from the dev1 mirror,
 * dev1 data reads work, anything needing dev0 fails (-1) so the engine's
 * failover paths (RAW mirror, tier copy skip) engage; writes are refused
 * upstream (read-only degraded mount).
 *
 * With ndev < 2 every call is a straight passthrough to dev0's blkio --
 * the single-device path is byte-identical to the pre-WP25 one. */

static uint64_t mux_dev0_bytes(const invfs_volume *v)
{
    return v->dev0_blocks * (uint64_t)INVFS_BLOCK_SIZE;
}


int vmux_seek(invfs_volume *v, uint64_t off)
{
    v->mux_pos = off;
    return 0;
}


/* one non-straddling slice; vmux_read/vmux_write split at the routing
 * boundaries (metadata span end, dev0/dev1 boundary) */
static int vmux_pread1(invfs_volume *v, uint64_t off, void *buf, size_t len)
{
    if (off < v->meta_end_bytes) {
        /* metadata: dev0 primary (unless absent or session-stale), fail
         * over to the dev1 mirror on any io failure. The mirror sits at
         * the SAME local offset on dev1. A session-stale mirror
         * (dev_skip[1]: its DEVT did not check out at open) is NOT a
         * failover source -- serving known-old metadata could resurrect
         * superseded records; fail loudly instead. */
        if (v->io_open[0] && !v->dev_skip[0] &&
            blkio_pread(&v->io, off, buf, len) == 0)
            return 0;
        if (v->io_open[0] && !v->dev_skip[0] && !v->metaread_logged) {
            v->metaread_logged = 1;
            fprintf(stderr, "vol: metadata read failing over to the dev1 "
                    "mirror (dev0 io error)\n");
        }
        if (v->io_open[1] && !v->dev_skip[1])
            return blkio_pread(&v->io2, off, buf, len);
        return -1;
    }
    if (off < mux_dev0_bytes(v)) {
        /* dev0 data (RAW zone / tier arena) */
        if (!v->io_open[0] || v->dev_skip[0])
            return -1;   /* degraded or dead: the engine fails over */
        return blkio_pread(&v->io, off, buf, len);
    }
    /* dev1 data (canonical shadow) */
    if (!v->io_open[1] || v->dev_skip[1])
        return -1;
    return blkio_pread(&v->io2, off - mux_dev0_bytes(v), buf, len);
}


int vmux_pread(invfs_volume *v, uint64_t off, void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;

    if (v->ndev < 2 && !v->degraded) {
        return blkio_pread(&v->io, off, buf, len);
    }
    while (len) {
        size_t n = len;
        if (off < v->meta_end_bytes && off + n > v->meta_end_bytes)
            n = (size_t)(v->meta_end_bytes - off);
        if (off < mux_dev0_bytes(v) && off + n > mux_dev0_bytes(v))
            n = (size_t)(mux_dev0_bytes(v) - off);
        if (vmux_pread1(v, off, out, n) != 0)
            return -1;
        out += n;
        off += n;
        len -= n;
    }
    return 0;
}


int vmux_read(invfs_volume *v, void *buf, size_t len)
{
    uint64_t off = v->mux_pos;
    int rc = vmux_pread(v, off, buf, len);
    if (rc == 0) v->mux_pos = off + len;
    return rc;
}


static int vmux_pwrite1(invfs_volume *v, uint64_t off,
                        const void *buf, size_t len)
{
    if (off < v->meta_end_bytes) {
        /* metadata: writethrough mirror to BOTH devices (same local
         * offset). dev0 fail -> log + sticky skip + continue on dev1
         * (rule 3: not a full latch; the DEVT sync_seq bump in vol_flush
         * then necessarily misses dev0 -> staleness is detectable).
         * dev1 fail -> the canonical store is gone: latch (WP22c). */
        int wrote = 0;
        if (v->degraded) {
            fprintf(stderr, "vol: metadata write refused: DEGRADED mount "
                    "(dev0 absent), the volume is read-only\n");
            return -1;
        }
        if (v->io_open[0] && !v->dev_skip[0]) {
            if (blkio_pwrite(&v->io, off, buf, len) != 0) {
                v->dev_skip[0] = 1;
                fprintf(stderr, "vol: dev0 metadata write failed at %llu; "
                        "continuing on dev1 (dev0 will resync at next "
                        "open)\n", (unsigned long long)off);
            } else {
                wrote = 1;
            }
        }
        if (v->io_open[1] && !v->dev_skip[1]) {
            if (blkio_pwrite(&v->io2, off, buf, len) != 0) {
                vol_io_error_latch(v, "metadata mirror write (dev1)");
                return -1;
            }
            wrote = 1;
        }
        return wrote ? 0 : -1;
    }
    if (off < mux_dev0_bytes(v)) {
        if (!v->io_open[0] || v->dev_skip[0]) {
            /* WP25 degraded: the op needs dev0, which is absent -- fail
             * loudly (EIO) with the plain reason */
            if (v->degraded && !v->metaread_logged) {
                v->metaread_logged = 1;
                fprintf(stderr, "vol: write to device 0 refused: DEGRADED "
                        "mount (dev0 absent) -- EIO. Reattach dev0 for "
                        "read-write.\n");
            }
            return -1;
        }
        return blkio_pwrite(&v->io, off, buf, len);
    }
    if (!v->io_open[1] || v->dev_skip[1])
        return -1;
    return blkio_pwrite(&v->io2, off - mux_dev0_bytes(v), buf, len);
}


int vmux_pwrite(invfs_volume *v, uint64_t off, const void *buf, size_t len)
{
    const uint8_t *in = (const uint8_t *)buf;

    if (v->ndev < 2 && !v->degraded) {
        return blkio_pwrite(&v->io, off, buf, len);
    }
    while (len) {
        size_t n = len;
        if (off < v->meta_end_bytes && off + n > v->meta_end_bytes)
            n = (size_t)(v->meta_end_bytes - off);
        if (off < mux_dev0_bytes(v) && off + n > mux_dev0_bytes(v))
            n = (size_t)(mux_dev0_bytes(v) - off);
        if (vmux_pwrite1(v, off, in, n) != 0)
            return -1;
        in += n;
        off += n;
        len -= n;
    }
    return 0;
}


int vmux_write(invfs_volume *v, const void *buf, size_t len)
{
    uint64_t off = v->mux_pos;
    int rc = vmux_pwrite(v, off, buf, len);
    if (rc == 0) v->mux_pos = off + len;
    return rc;
}


void vmux_close(invfs_volume *v)
{
    if (v->io_open[0]) { blkio_close(&v->io);  v->io_open[0] = 0; }
    if (v->io_open[1]) { blkio_close(&v->io2); v->io_open[1] = 0; }
}


/* CRC convention: over the descriptor with the crc32c field read as zero
 * (the RDP0 rule). */
uint32_t devt_crc(const invfs_devt *d)
{
    invfs_devt t = *d;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}


/* Persist the in-memory DEVT, mirrored by the mux to every writable
 * (non-skipped) device.
 *
 * WP99: this wrote the WHOLE of block 0 back (read-modify-write of 4 KiB)
 * with only the 188 DEVT bytes patched. Block 0 is the one region every
 * v3 publisher rewrites: the RT30 root descriptor (0x9D0) is stored there
 * by mbuf_rt30_store / mbuf_root_publish, and the SPT0 save-point
 * descriptor (0xA00) likewise. A read-modify-write of the block is
 * therefore a lost-update against any of them, and the DEVT bump is about
 * to run on every Meta-v3 flush (WP99), which turns that race from a rare
 * interleaving into a per-flush one. The DEVT occupies 0x2A0..0x35C, which
 * overlaps neither the superblock (0x00..0x90) nor the RT30/SPT0
 * descriptors, so writing just the descriptor is equivalent for the DEVT
 * and cannot clobber its neighbours. */
int vol_write_devt(invfs_volume *v)
{
    invfs_devt t;
    if (v->ndev != 2) return 0;
    t = v->devt;
    memcpy(t.magic, "DEVT", 4);
    t.crc32c = 0;
    t.crc32c = devt_crc(&t);
    if (vmux_seek(v, INVFS_DEVT_OFF) != 0 ||
        vmux_write(v, &t, sizeof t) != 0)
        return -1;
    v->devt = t;
    return 0;
}


/* Storage barrier over both present devices. 0 = all good; 1 = dev0's
 * barrier failed (dev0 is now session-skipped and provably stale -- the
 * DEVT bump here lands on dev1 only, so the divergence is visible at the
 * next open and triggers the resync); -1 = dev1 failed (the caller
 * latches: the canonical store may have lost acknowledged writes --
 * WP22c). */
int vmux_barrier(invfs_volume *v, const char *what)
{
    if (v->ndev < 2 && !v->degraded)
        return blkio_flush(&v->io) == 0 ? 0 : -1;
    if (v->io_open[0] && !v->dev_skip[0] && blkio_flush(&v->io) != 0) {
        v->dev_skip[0] = 1;
        fprintf(stderr, "vol: dev0 barrier failed (%s); continuing on "
                "dev1, dev0 is stale until the next open resyncs it\n",
                what ? what : "barrier");
        v->devt.sync_seq++;
        if (vol_write_devt(v) != 0)
            return -1;
    }
    if (v->io_open[1] && blkio_flush(&v->io2) != 0)
        return -1;
    return v->dev_skip[0] ? 1 : 0;
}


/* Metadata mirror resync (WP25 rule 5): open found one device's metadata
 * span older than the other's (sync_seq mismatch), or a write/barrier
 * failure session-skipped a device. The whole span [0, meta_end) is
 * copied from the newer device to the stale one (both hold it at
 * identical local offsets); the DEVT bump in vol_flush then marks them
 * equal. Newest state wins; the run is logged. */
static int mirror_resync(invfs_volume *v)
{
    int loser = v->resync_winner ^ 1;
    blkio *src = v->resync_winner ? &v->io2 : &v->io;
    blkio *dst = loser ? &v->io2 : &v->io;
    uint8_t *buf = (uint8_t *)malloc(BLKIO_BOUNCE);
    uint64_t off = 0, end = v->meta_end_bytes;

    if (!buf) return -1;
    fprintf(stderr, "vol: mirror resync: dev%d <- dev%d (%llu bytes of "
            "metadata; newest state wins)\n", loser, v->resync_winner,
            (unsigned long long)end);
    while (off < end) {
        size_t n = (size_t)((end - off) > BLKIO_BOUNCE ? BLKIO_BOUNCE
                                                       : end - off);
        if (blkio_pread(src, off, buf, n) != 0) {
            fprintf(stderr, "vol: mirror resync: read failed at %llu\n",
                    (unsigned long long)off);
            free(buf);
            return -1;
        }
        if (blkio_pwrite(dst, off, buf, n) != 0) {
            free(buf);
            fprintf(stderr, "vol: mirror resync: dev%d write failed\n",
                    loser);
            if (loser == 1) {
                /* the canonical store refused: WP22c territory */
                vol_io_error_latch(v, "mirror resync write (dev1)");
            }
            return -1;   /* a dev0 loser stays session-skipped */
        }
        off += n;
    }
    free(buf);
    if (blkio_flush(dst) != 0) {
        if (loser == 1)
            vol_io_error_latch(v, "mirror resync barrier (dev1)");
        return -1;
    }
    v->dev_skip[loser] = 0;
    v->resync_pending = 0;
    return 0;
}


/* The two-device commit tail (WP25 rule 5), in one place so both flush
 * paths use the same protocol: repair a stale mirror (newest state wins),
 * then bump the DEVT sync_seq so the next open can certify that both
 * devices carry the same state.
 *
 * WP99: this used to be the inline tail of the v2 path only. vol_flush's
 * VOLF_V3 branch returns before it, so on Meta-v3 the DEVT sync_seq was
 * frozen at its mkfs value for the life of the volume -- which made BOTH
 * halves of the mirror protocol inert: the staleness DETECTION could never
 * fire, and a divergence detected some other way (a dev0 barrier failure,
 * which does bump the DEVT from inside vmux_barrier) was never repaired.
 * The failure was silent and it was data loss: rewinding dev0's block 0
 * rolls the RT30 root descriptor (0x9D0, inside block 0 on v3) back, the
 * stale generation is adopted as live, and the next publish drops every
 * delta record written past it -- on BOTH devices, because the mirror is
 * write-through -- while invf-fsck still walked the tree it could reach
 * and printed OK.
 *
 * The v3 caller runs this AFTER the barrier that made the published root
 * durable, so the DEVT bump certifies a generation both devices hold. It is
 * deliberately NOT accompanied by a bare vol_write_sb here: on v3 the
 * superblock's live neighbour in block 0 is the RT30 root, which the
 * publish path already stores and barriers, and v->sb.state is the v2
 * crash-state machine, whose v3 activation is a separate question. The
 * block-0 image that reaches the devices does get rewritten every flush
 * anyway, because the DEVT lives in it. */
static int vol_commit_mirror(invfs_volume *v)
{
    if (v->ndev != 2 || v->degraded)
        return 0;
    if (v->resync_pending)
        mirror_resync(v);   /* failures keep the loser skipped */
    v->devt.sync_seq++;
    if (vol_write_devt(v) != 0) {
        vol_io_error_latch(v, "DEVT write");
        return -1;
    }
    return 0;
}


int vol_ndev(const invfs_volume *v)     { return v ? v->ndev : 0; }
int vol_degraded(const invfs_volume *v) { return v && v->degraded; }
int vol_mirror_stale(const invfs_volume *v)
{
    return v && (v->resync_pending || v->dev_skip[0] || v->dev_skip[1]);
}


/* ================= WP99: which device is stale? =================
 * The two-device volumes keep their metadata span mirrored write-through,
 * so "is one of them behind?" has to be answerable from the devices
 * themselves, without trusting any in-RAM state. Two independent signals
 * exist, and neither is sufficient alone:
 *
 *   DEVT.sync_seq   the protocol's own sequence, bumped once per
 *                   two-device flush (vol_commit_mirror). It is blind on
 *                   any volume whose flushes never reached the bump --
 *                   which, before WP99, was EVERY Meta-v3 volume, because
 *                   vol_flush returned out of its VOLF_V3 branch above the
 *                   tail that holds the bump. The DEVT on such a volume
 *                   still carries its mkfs value forever, so a rewound (or
 *                   half-lost) dev0 block 0 brings back a matching
 *                   sync_seq and the comparison sees nothing. It is also
 *                   blind in the other direction: a dev0 that lost only the
 *                   0x9D0..0xA00 region of block 0 keeps an intact DEVT.
 *
 *   block-0 gen     the RT30 root generation at INVFS_RT30_OFF: monotone,
 *                   rewritten on every publish, and on Meta-v3 it is the
 *                   ONLY content of block 0 that orders two copies of the
 *                   same volume against each other. It does not exist on
 *                   v2 (the area is reserved-zero), so a signal that is
 *                   absent on this format is skipped, not treated as a
 *                   mismatch.
 *
 * A divergence is declared when EITHER available signal says so. DEVT wins
 * when the two disagree: a dev0 whose DEVT is behind is provably behind
 * the last commit even if its RT30 looks current, which is the case a
 * partial block-0 loss produces.
 *
 * This is the ONE place the question is asked. The mount decision
 * (wp25_open_dev1) and the reporting verdict (invf-fsck, invf-verify) both
 * call it, so the two can never disagree about whether a volume is safe.
 *
 * `stale` (out) is the LOSING device, 0 or 1, or -1 when the devices
 * agree / the volume is not comparable. `why` (out) names the signal that
 * fired, or the reason the devices could not be compared. Returns 0 when
 * the two devices were compared, -1 when they could not be (single
 * device, degraded, a device absent, or an io error). */
static void mirror_signal(const uint8_t *blk, unsigned long long *devt_seq,
                          int *devt_ok, unsigned long long *gen, int *gen_ok)
{
    const invfs_devt *d = (const invfs_devt *)(blk + INVFS_DEVT_OFF);
    const invfs_rt30 *r = (const invfs_rt30 *)(blk + INVFS_RT30_OFF);

    *devt_ok = 0;
    *gen_ok = 0;
    *devt_seq = 0;
    *gen = 0;
    if (memcmp(d->magic, "DEVT", 4) == 0 && d->version == INVFS_DEVT_VERSION
        && devt_crc(d) == d->crc32c) {
        *devt_seq = d->sync_seq;
        *devt_ok = 1;
    }
    if (memcmp(r->magic, "RT30", 4) == 0 && r->version == INVFS_RT30_VERSION
        && invfs_crc32c(r, offsetof(invfs_rt30, crc32c)) == r->crc32c) {
        *gen = r->seq;
        *gen_ok = 1;
    }
}


int vol_mirror_compare(invfs_volume *v, int *stale, const char **why)
{
    uint8_t b0[INVFS_BLOCK_SIZE], b1[INVFS_BLOCK_SIZE];
    unsigned long long d0, d1, g0, g1;
    int dok0, dok1, gok0, gok1;

    if (stale) *stale = -1;
    if (why)   *why = NULL;
    if (!v) return -1;
    /* Not comparable: a single-device volume has no mirror, and a degraded
     * mount (dev0 absent) has nothing to compare dev1 against. */
    if (v->ndev != 2 || v->degraded || !v->dev0_present ||
        !v->io_open[0] || !v->io_open[1]) {
        if (why) *why = "not a two-device mount";
        return -1;
    }
    /* Read each device DIRECTLY. vmux_pread would hide exactly what is being
     * looked for: it serves the metadata span from dev0 unless dev0 is
     * already known to be skipped. */
    if (blkio_pread(&v->io, 0, b0, sizeof b0) != 0 ||
        blkio_pread(&v->io2, 0, b1, sizeof b1) != 0) {
        if (why) *why = "block 0 unreadable on a device";
        return -1;
    }
    mirror_signal(b0, &d0, &dok0, &g0, &gok0);
    mirror_signal(b1, &d1, &dok1, &g1, &gok1);

    if (dok0 && dok1 && d0 != d1) {
        if (stale) *stale = d0 < d1 ? 0 : 1;
        if (why) *why = "DEVT sync_seq";
        return 0;
    }
    if (dok0 != dok1) {
        /* One device carries a valid device table and the other does not:
         * the one without it cannot be the newer copy. */
        if (stale) *stale = dok0 ? 1 : 0;
        if (why) *why = "DEVT presence";
        return 0;
    }
    if (gok0 && gok1 && g0 != g1) {
        if (stale) *stale = g0 < g1 ? 0 : 1;
        if (why) *why = "block-0 root generation";
        return 0;
    }
    if (gok0 != gok1) {
        if (stale) *stale = gok0 ? 1 : 0;
        if (why) *why = "block-0 root descriptor presence";
        return 0;
    }
    if (!dok0 && !gok0 && !gok1) {
        if (why) *why = "neither device carries a DEVT or an RT30 root";
        return -1;
    }
    if (why) *why = "in sync";
    return 0;
}


/* DEVT sanity: a descriptor that claims 2 devices must agree with the
 * superblock it sits next to. */
static int devt_sane(const invfs_devt *d, const invfs_superblock *sb)
{
    if (d->version != INVFS_DEVT_VERSION) return 0;
    if (d->dev_count != 2) return 0;
    if (!d->dev_blocks[0] || !d->dev_blocks[1]) return 0;
    if (d->dev_blocks[0] + d->dev_blocks[1] != sb->total_blocks) return 0;
    if (memcmp(d->vol_uuid, sb->uuid, 16) != 0) return 0;
    return 1;
}


/* Open + validate device 1 of a 2-device volume. The path comes from
 * INVFS_DEV1 (wins) or the DEVT path hint. Returns 0 with io2 open and
 * locked, or -1 (loud). */
static int wp25_open_dev1(invfs_volume *v, const char *hint)
{
    const char *d1 = getenv("INVFS_DEV1");
    invfs_devt d2;
    int rc;

    if (!d1 || !*d1) d1 = hint;
    if (!d1 || !*d1) {
        fprintf(stderr, "vol_open: %s: two-device volume, device 1 not "
                "given (set INVFS_DEV1)\n", v->path);
        return -1;
    }
    rc = blkio_open(&v->io2, d1,
                    blkio_looks_like_device(d1) ? BLKIO_EXCLUSIVE : 0);
    if (rc != 0) {
        fprintf(stderr, "vol_open: %s: device 1 %s: %s\n", v->path, d1,
                blkio_strerror(rc));
        return -1;
    }
    v->io_open[1] = 1;
#ifndef _WIN32
    if (flock(v->io2.fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "vol_open: %s: device 1 %s is in use by another "
                "process\n", v->path, d1);
        vmux_close(v);
        return -1;
    }
#endif
    v->path2 = strdup(d1);
    /* dev1 carries the canonical data: refuse to run without it */
    if (blkio_capacity(&v->io2) <
        v->devt.dev_blocks[1] * (uint64_t)INVFS_BLOCK_SIZE) {
        fprintf(stderr, "vol_open: %s: device 1 %s is smaller than the "
                "device table says (%llu blocks)\n", v->path, d1,
                (unsigned long long)v->devt.dev_blocks[1]);
        return -1;
    }
    if (blkio_pread(&v->io2, INVFS_DEVT_OFF, &d2, sizeof d2) != 0 ||
        memcmp(d2.magic, "DEVT", 4) != 0 || devt_crc(&d2) != d2.crc32c) {
        /* no readable table on dev1: treat it as stale (dev0's table is
         * authoritative); the next flush resyncs dev1's metadata span
         * wholesale, which rewrites its block 0 */
        fprintf(stderr, "vol_open: %s: device 1 has no valid DEVT; "
                "treating it as stale\n", v->path);
        v->resync_pending = 1;
        v->resync_winner = 0;
        v->dev_skip[1] = 1;    /* metadata writes skip dev1 until resync */
        return 0;
    }
    /* WP99: a SELF-VALID device table that is not this volume's device 1 is
     * a different volume (or a device that was re-purposed), not a stale
     * copy of ours. That distinction is load-bearing, because the "treat it
     * as stale" branch above ends in mirror_resync, which copies this
     * volume's whole metadata span over the loser: accepting a foreign
     * device here means the first write DESTROYS it. The check used to be
     * `!devt_sane(...)` folded into the branch above, so a wrong
     * INVFS_DEV1 -- which is an operator pointing the tool at the wrong
     * image, and which tools/test-multidev.sh leg 9 was doing to the E
     * volume -- mounted happily and quietly overwrote the other volume.
     * A stale device 1 still carries a valid table for THIS volume, which
     * is exactly what identifies it, so nothing legitimate is refused. */
    if (!devt_sane(&d2, &v->sb)) {
        fprintf(stderr, "vol_open: %s: %s carries a valid device table, but "
                "it is not this volume's device 1 (geometry %llu+%llu blocks, "
                "uuid %02x%02x%02x%02x vs this volume's %llu+%llu, "
                "%02x%02x%02x%02x); refusing to mount rather than resync over "
                "it -- check INVFS_DEV1 / the DEVT device-1 hint\n",
                v->path, d1,
                (unsigned long long)d2.dev_blocks[0],
                (unsigned long long)d2.dev_blocks[1],
                d2.vol_uuid[0], d2.vol_uuid[1], d2.vol_uuid[2], d2.vol_uuid[3],
                (unsigned long long)v->devt.dev_blocks[0],
                (unsigned long long)v->devt.dev_blocks[1],
                v->sb.uuid[0], v->sb.uuid[1], v->sb.uuid[2], v->sb.uuid[3]);
        return -1;
    }
    if (d2.dev_blocks[0] != v->devt.dev_blocks[0] ||
        d2.dev_blocks[1] != v->devt.dev_blocks[1]) {
        fprintf(stderr, "vol_open: %s: device 1 DEVT disagrees with "
                "device 0 on the geometry; refusing to mount\n", v->path);
        return -1;
    }
    /* WP99: the divergence decision now comes from vol_mirror_compare,
     * which asks both devices for the two staleness signals and declares a
     * divergence when EITHER available one fires. The v2 behaviour is a
     * strict subset of it: the DEVT sync_seq branch below used to be the
     * ONLY check, so on Meta-v3 -- where no flush ever bumped sync_seq, see
     * vol_commit_mirror -- it could never fire, and a rewound dev0 block 0
     * was adopted as the live generation. The RT30 root generation is the
     * signal that sees it. */
    {
        int stale = -1;
        const char *why = NULL;
        if (vol_mirror_compare(v, &stale, &why) == 0 && stale >= 0) {
            v->resync_pending = 1;
            v->resync_winner = stale ^ 1;
            if (stale == 0)
                v->dev_skip[0] = 1;
            fprintf(stderr, "vol_open: metadata mirror divergence: dev%d "
                    "is stale (%s disagrees); reads fail over to dev%d, "
                    "resync at the next flush\n",
                    stale, why ? why : "block 0", stale ^ 1);
        }
    }
    return 0;
}


/* Degraded bootstrap (WP25 rule 5): the dev0 image is absent. With
 * INVFS_DEV1 set we open device 1 alone, read its block 0 directly, and
 * continue as a READ-ONLY volume serving every structure from the dev1
 * mirror. */
static int wp25_open_degraded(invfs_volume *v)
{
    const char *d1 = getenv("INVFS_DEV1");
    uint8_t blk[INVFS_BLOCK_SIZE];
    invfs_devt d2;
    int rc;

    if (!d1 || !*d1)
        return -1;
    rc = blkio_open(&v->io2, d1,
                    blkio_looks_like_device(d1) ? BLKIO_EXCLUSIVE : 0);
    if (rc != 0) {
        fprintf(stderr, "vol_open: degraded open: device 1 %s: %s\n",
                d1, blkio_strerror(rc));
        return -1;
    }
    v->io_open[1] = 1;
    if (blkio_pread(&v->io2, 0, blk, sizeof blk) != 0)
        goto bad;
    memcpy(&v->sb, blk, sizeof v->sb);
    if (memcmp(v->sb.magic, INVFS_MAGIC, 8) != 0 ||
        invfs_crc32c(&v->sb, offsetof(invfs_superblock, checksum)) !=
            v->sb.checksum)
        goto bad;
    /* The degraded leg is the same format gate as the primary one in
     * vol_open, and says the same thing: the mirror's superblock decides
     * the format, and this build reads v3 only. */
    if (!(v->sb.vol_flags & VOLF_V3)) {
        fprintf(stderr, "vol_open: %s: this is not a format v3 volume: the "
                "device 1 image reports %s (VOLF_V3 is not set). This build "
                "reads format v3 only (INVFS_VERSION=%s).\n",
                d1, (v->sb.vol_flags & VOLF_ASTV2) ? "format v2" : "format v1",
                INVFS_VERSION_STRING);
        goto bad;
    }
    memcpy(&d2, blk + INVFS_DEVT_OFF, sizeof d2);
    if (memcmp(d2.magic, "DEVT", 4) != 0 || devt_crc(&d2) != d2.crc32c ||
        !devt_sane(&d2, &v->sb)) {
        fprintf(stderr, "vol_open: degraded open: %s is not device 1 of a "
                "two-device InvariantFS volume (no valid DEVT)\n", d1);
        goto bad;
    }
    if (blkio_capacity(&v->io2) <
        d2.dev_blocks[1] * (uint64_t)INVFS_BLOCK_SIZE) {
        fprintf(stderr, "vol_open: degraded open: %s is smaller than the "
                "device table says\n", d1);
        goto bad;
    }
    v->path2 = strdup(d1);
    v->devt = d2;
    v->devt_present = 1;
    v->ndev = 2;
    v->dev0_present = 0;
    v->degraded = 1;
    v->dev0_blocks = d2.dev_blocks[0];
    v->dev1_blocks = d2.dev_blocks[1];
    v->meta_end_bytes = (v->sb.metadata_zone_start +
                         v->sb.metadata_zone_blocks) *
                        (uint64_t)INVFS_BLOCK_SIZE;
    v->raw_mirror = (d2.flags & INVFS_DEVTF_RAW_MIRROR) != 0;
    v->arena_start = v->sb.raw_zone_start + v->sb.raw_zone_blocks;
    v->arena_blocks = v->dev0_blocks > v->arena_start
                    ? v->dev0_blocks - v->arena_start : 0;
    return 0;
bad:
    vmux_close(v);
    return -1;
}


/* WP-M1: log the RT30 v3 root-area descriptor the open settled on. This used
 * to do its own read of block 0 and complain on failure; it now reports the
 * state mbuf_rt30_load() already resolved, because a volume that recovered
 * off the ANC0 tail anchor has a perfectly good descriptor and must not also
 * be told it is "presenting an empty namespace". A descriptor that really is
 * absent or torn is still a warning, not a refusal -- the RDP0 "absent"
 * convention, which keeps a v3 mkfs interrupted before the RT30 write
 * openable. */
static void v3_probe_rt30(invfs_volume *v)
{
    const invfs_rt30 *rt = &v->rt30;

    if (!v->rt30_present) {
        fprintf(stderr, "vol_open: %s: RT30 v3 root descriptor absent or "
                "torn; presenting an empty namespace\n", v->path);
        return;
    }
    if (getenv("INVFS_DEBUG"))
        fprintf(stderr, "vol_open: v3 root descriptor: page_size=%u "
                "seq=%llu root_slot=%llu/%llu delta_pba=%llu%s\n",
                (unsigned)rt->page_size,
                (unsigned long long)rt->seq,
                (unsigned long long)rt->root_slot[0],
                (unsigned long long)rt->root_slot[1],
                (unsigned long long)rt->delta_pba,
                v->anchor_adopted ? " (from the ANC0 tail anchor)" : "");
}


static invfs_volume *vol_open_inner(const char *path, int *err_out)
{
    int dummy_err = 0;
    int *err = err_out ? err_out : &dummy_err;
    char devbuf[64];
    const char *real;
    int rc;
    int is_v3 = 0;
    invfs_volume *v = (invfs_volume *)calloc(1, sizeof(invfs_volume));
    if (!v) { *err = -1; return NULL; }
    (void)pthread_rwlock_init(&v->meta_lock, NULL);
    (void)pthread_mutex_init(&v->rc_mu, NULL);
    /* WP-heat-table-concurrent-safe: the read-heat table is reached from the
     * lock-free read path (fuse_fs.c releases g_io_lock before vol_read_range),
     * so it carries its own leaf mutex rather than relying on a caller's. */
    heat_locks_init(v);
    /* WP-cpack-map-copy-out: same reason, same shape. The parsed !mbrmap
     * cache grows (realloc) and compacts on the lock-free read path, so it
     * carries its own leaf mutex rather than relying on a caller's. */
    cpack_locks_init(v);

    /* "W:" is the shorthand a user types; CreateFileW needs "\\.\W:". Store
       the normalized form, so diagnostics name what was actually opened. */
    real = blkio_normalize(path, devbuf, sizeof devbuf);
    v->path = strdup(real);
    v->dev0_present = 1;

    /* A device is taken exclusively -- locked and dismounted. Two processes
       each holding their own in-memory bitmap and L2P would corrupt the
       volume between them, which is why an image file is opened without
       FILE_SHARE_WRITE too. It also stops Windows from mounting whatever
       filesystem it believes is there and writing that filesystem's metadata
       over ours. */
    rc = blkio_open(&v->io, real,
                    blkio_looks_like_device(real) ? BLKIO_EXCLUSIVE : 0);
    if (rc != 0) {
        /* WP25: with INVFS_DEV1 set a missing dev0 is the DEGRADED leg:
         * open device 1 alone and serve read-only from the mirror. */
        if (wp25_open_degraded(v) != 0) {
            fprintf(stderr, "vol_open: %s: %s\n", real, blkio_strerror(rc));
            *err = -2;
            free(v->path);
            heat_locks_destroy(v);
            cpack_locks_destroy(v);   /* WP-cpack-map-copy-out */
            free(v);
            return NULL;
        }
        fprintf(stderr, "vol_open: DEGRADED: %s: device 0 absent; serving "
                "READ-ONLY from the dev1 mirror %s (reattach dev0 for "
                "read-write)\n", real, v->path2);
    } else {
        v->io_open[0] = 1;
    }
#ifndef _WIN32
    /* POSIX twin of the Windows no-share open above: a lingering FUSE daemon
     * finishing its drain and an offline tool (invf-cp/invf-sweep) writing
     * the same image corrupt it between their in-memory bitmaps/L2P (seen
     * in the wild: stale-position record appends clobbering fresh records).
     * LOCK_NB: fail loudly instead of waiting. Released by close().
     * Set INVFS_RO_LOCK=1 / INVFS_ALLOW_SHARED=1 for concurrent read-only utilities. */
    int ltype = (getenv("INVFS_ALLOW_SHARED") || getenv("INVFS_RO_LOCK")) ? LOCK_SH : LOCK_EX;
    if ((v->io_open[0] && flock(v->io.fd, ltype | LOCK_NB) != 0) ||
        (v->io_open[1] && flock(v->io2.fd, ltype | LOCK_NB) != 0)) {
        fprintf(stderr, "vol_open: %s: image is in use by another process\n",
                real);
        *err = -2;
        goto fail;
    }
#endif
    if (io_seek(&v->io, 0) != 0 ||
        io_read(&v->io, &v->sb, sizeof(v->sb)) != 0) { *err = -3; goto fail; }
    if (memcmp(v->sb.magic, INVFS_MAGIC, 8) != 0) { *err = -4; goto fail; }
    if (invfs_crc32c(&v->sb, offsetof(invfs_superblock, checksum)) != v->sb.checksum)
        { *err = -5; goto fail; }

    /* VOLF_V3 is the authoritative format marker, and it is the first thing
     * checked. Every structure below this line is read through it.
     *
     * A volume without it is not readable by this build. Say so plainly, and
     * say what the operator can do -- but do not write a migration advisory
     * for a format that is not going to come back: there is no converter to
     * name (invf-migrate-v2 went with the machinery it migrated, and no
     * v1/v2 -> v3 path has ever existed in-tree; invf-mkfs has refused to
     * CREATE such a volume since v0.5.0). The two facts that are worth the
     * operator's time are the one they already half-know (this build reads
     * v3 only) and the one they cannot guess (their image is untouched, so
     * trying something else cannot hurt it). */
    is_v3 = (v->sb.vol_flags & VOLF_V3) != 0;
    if (!is_v3) {
        const int is_v1 = !(v->sb.vol_flags & VOLF_ASTV2);
        fprintf(stderr,
                "vol_open: %s: this is not a format v3 volume: the image "
                "reports %s (VOLF_V3 is not set). This build reads format v3 "
                "only (INVFS_VERSION=%s) and carries no reader for any other "
                "format.\n"
                "  The image is untouched by this attempt. To get at the data, "
                "create a new volume with invf-mkfs and re-import from a copy "
                "of the source tree, or restore from a backup.\n",
                real, is_v1 ? "format v1" : "format v2", INVFS_VERSION_STRING);
        *err = -12;
        goto fail;
    }

    /* WP25: the DEVT device table at 0x2A0 (block 0 reserved area, the
     * RDP0 convention: absent = zeros = single-device). When it names two
     * devices, open device 1 here so every structure read below (bitmap,
     * metadata extents) can fail over to the mirror. dev1 holds the
     * canonical data: a volume that cannot reach dev1 is refused loudly
     * (the degraded leg covers dev0-absent only). */
    if (!v->devt_present) {
        invfs_devt dt;
        memset(&dt, 0, sizeof dt);
        if (io_seek(&v->io, INVFS_DEVT_OFF) == 0 &&
            io_read(&v->io, &dt, sizeof dt) == 0 &&
            memcmp(dt.magic, "DEVT", 4) == 0) {
            if (devt_crc(&dt) != dt.crc32c || dt.version != INVFS_DEVT_VERSION) {
                fprintf(stderr, "vol_open: %s: DEVT device table is torn; "
                        "cannot tell the device geometry -- refusing to "
                        "mount (reattach both devices / run invf-fsck)\n",
                        real);
                *err = -5; goto fail;
            }
            if (dt.dev_count == 2) {
                if (!devt_sane(&dt, &v->sb)) {
                    fprintf(stderr, "vol_open: %s: DEVT/superblock "
                                    "mismatch; refusing to mount\n", real);
                    *err = -5; goto fail;
                }
                v->devt = dt;
                v->devt_present = 1;
            } else if (dt.dev_count != 1) {
                fprintf(stderr, "vol_open: %s: DEVT dev_count %u "
                                "unsupported\n", real, dt.dev_count);
                *err = -5; goto fail;
            }
        }
        if (v->devt_present && v->devt.dev_count == 2) {
            v->ndev = 2;
            v->dev0_blocks = v->devt.dev_blocks[0];
            v->dev1_blocks = v->devt.dev_blocks[1];
            v->meta_end_bytes = (v->sb.metadata_zone_start +
                                 v->sb.metadata_zone_blocks) *
                                (uint64_t)INVFS_BLOCK_SIZE;
            v->raw_mirror = (v->devt.flags & INVFS_DEVTF_RAW_MIRROR) != 0;
            v->arena_start = v->sb.raw_zone_start + v->sb.raw_zone_blocks;
            v->arena_blocks = v->dev0_blocks > v->arena_start
                            ? v->dev0_blocks - v->arena_start : 0;
            if (blkio_capacity(&v->io) <
                v->dev0_blocks * (uint64_t)INVFS_BLOCK_SIZE) {
                fprintf(stderr, "vol_open: %s: device 0 is smaller than "
                        "the device table says (%llu blocks)\n", real,
                        (unsigned long long)v->dev0_blocks);
                *err = -3; goto fail;
            }
            if (wp25_open_dev1(v, v->devt.dev1_hint) != 0) {
                fprintf(stderr, "vol_open: %s: device 1 (the canonical "
                        "data store) is required; refusing to mount. "
                        "Reattach it, or for a dev0-absent READ-ONLY "
                        "mount set INVFS_DEV1 and point the tool at the "
                        "missing dev0 path.\n", real);
                *err = -2; goto fail;
            }
        } else {
            v->ndev = 1;
            v->dev0_blocks = v->sb.total_blocks;
        }
    }

    /* WP30 Phase 3: load MET0 descriptor at 0x3A0.
     * v0.3.0+: dynamic metadata extents are mandatory.
     * format_version 0 = legacy volume (no dynamic extents support):
     * allow read-only fallback so users can mount and extract data.
     * Writes are disabled via VOLF_READONLY (every mutation path refuses). */
    if (v->sb.format_version == 0) {
        fprintf(stderr, "vol_open: volume format version 0 (pre-v0.3.0) opened in "
                        "READ-ONLY mode; dynamic metadata extents are unavailable. "
                        "Reformat with invf-mkfs to upgrade.\n");
        v->sb.vol_flags |= VOLF_READONLY;
        v->met0_present = 0;
    } else {
        invfs_met0 m0;
        if (io_seek(&v->io, INVFS_MET0_OFF) == 0 &&
            io_read(&v->io, &m0, sizeof(m0)) == 0 &&
            memcmp(m0.magic, "MET0", 4) == 0) {
            if (meta_met0_crc(&m0) == m0.crc32c && m0.version == 1) {
                v->met0 = m0;
                v->met0_present = 1;
                v->meta_active_extent = m0.active_extent;
                v->meta_active_offset = m0.active_offset;
            } else {
                fprintf(stderr, "vol_open: MET0 descriptor CRC/version "
                                "mismatch; volume may be corrupted\n");
                goto fail;
            }
        } else {
            fprintf(stderr, "vol_open: MET0 descriptor not found at offset 0x%llX\n",
                    (unsigned long long)INVFS_MET0_OFF);
            goto fail;
        }
    }

    /* WP59: PCK0 codec-policy descriptor at 0x3C4 (past MET0).
     * Valid magic+CRC loads the descriptor; anything else reads as absent.
     * A CRC mismatch is a torn write -- treated as absent (the volume was
     * written without a known policy; the gate logic handles this). */
    {
        invfs_pck0 pk;
        if (io_seek(&v->io, INVFS_PCK0_OFF) == 0 &&
            io_read(&v->io, &pk, sizeof pk) == 0 &&
            memcmp(pk.magic, "PCK0", 4) == 0) {
            if (pck0_crc(&pk) == pk.crc32c &&
                pk.version == INVFS_PCK0_VERSION &&
                pk.n_codecs <= INVFS_PCK0_MAX_CODECS) {
                v->pk = pk;
                v->pk_present = 1;
            } else {
                fprintf(stderr, "vol_open: PCK0 descriptor CRC/version "
                                "mismatch; treated as absent\n");
            }
        }
    }

    /* WP20b: second block-0 read for the RDP0 redundancy descriptor at
     * 0x100 (past the 144-byte superblock struct; pre-WP20b images carry
     * zeros there -> absent). Valid magic+crc loads the persisted
     * redundancy config; anything else leaves the layer-1 default. */
    v->seal_k1 = SEAL_STRIPE_K;
    {
        invfs_rdp0 rd;
        if (io_seek(&v->io, INVFS_RDP0_OFF) == 0 &&
            io_read(&v->io, &rd, sizeof rd) == 0 &&
            memcmp(rd.magic, "RDP0", 4) == 0) {
            if (rdp0_crc(&rd) == rd.crc32c) {
                v->rd = rd;
                v->rd_present = 1;
            } else {
                fprintf(stderr, "vol_open: RDP0 descriptor CRC mismatch; "
                                "redundancy config ignored\n");
            }
        }
    }
    if (v->rd_present) {
        int l2_ok = (v->rd.l2_algo == INVFS_RDP0_L2_RS_VM ||
                     v->rd.l2_algo == INVFS_RDP0_L2_RS_CAUCHY) &&
                    v->rd.k2 == SEAL2_K &&
                    v->rd.m2 >= SEAL2_M2_MIN && v->rd.m2 <= SEAL2_M2_MAX;
        if (v->rd.l1_algo == INVFS_RDP0_L1_XOR &&
            v->rd.k1 >= 8 && v->rd.k1 <= 128)
            v->seal_k1 = v->rd.k1;
        if (v->rd.l2_algo && !l2_ok) {
            fprintf(stderr, "vol_open: unsupported RDP0 layer-2 shape "
                    "(algo %u, k2 %u, m2 %u); layer 2 ignored\n",
                    (unsigned)v->rd.l2_algo, (unsigned)v->rd.k2,
                    (unsigned)v->rd.m2);
            v->rd.l2_algo = 0;
        }
    }

    /* WP18: an armed resize (RSZ0) takes precedence over everything below --
     * the metadata the current superblock describes may already be partly
     * overwritten, so the roll-forward runs before the bitmap/journal/inode
     * reads. A committed-but-uncleared descriptor is just swept up. */
    {
        invfs_rsz0 rz;
        if (io_seek(&v->io, INVFS_RSZ0_OFF) == 0 &&
            io_read(&v->io, &rz, sizeof rz) == 0 &&
            memcmp(rz.magic, "RSZ0", 4) == 0) {
            if (rsz0_crc(&rz) != rz.crc32c || !rsz0_sane(v, &rz)) {
                /* torn/desc corrupt: the apply it armed never started (the
                 * arm precedes it), so the old metadata is intact -- ignore
                 * the descriptor and let the RECOVERY path below decide */
                fprintf(stderr, "vol_open: ignoring a corrupt RSZ0 resize "
                                "descriptor\n");
            } else if (v->degraded) {
                /* the roll-forward WRITES the metadata span; the dev1
                 * mirror alone cannot run it (the mirror would diverge
                 * from the half-moved dev0 state). Reattach dev0. */
                fprintf(stderr, "vol_open: %s: an interrupted resize is "
                        "pending and the volume is DEGRADED (dev0 absent); "
                        "reattach dev0 to finish the resize\n", real);
                *err = -10; goto fail;
            } else if (v->sb.total_blocks == rz.old_total) {
                if (vol_rsz0_apply(v, &rz) != 0) {
                    fprintf(stderr, "vol_open: resize roll-forward failed; "
                                    "volume left for invf-fsck/retry\n");
                    *err = -10; goto fail;
                }
                fprintf(stderr, "vol_open: completed an interrupted resize "
                        "(%llu -> %llu blocks)\n",
                        (unsigned long long)rz.old_total,
                        (unsigned long long)v->sb.total_blocks);
            } else if (v->sb.total_blocks == rz.new_sb.total_blocks) {
                /* committed, only the descriptor clear was lost */
                uint8_t z[sizeof(invfs_rsz0)];
                memset(z, 0, sizeof z);
                if (io_seek(&v->io, INVFS_RSZ0_OFF) == 0)
                    io_write(&v->io, z, sizeof z);
            } else {
                fprintf(stderr, "vol_open: RSZ0 resize descriptor matches "
                                "neither the current nor its target size; "
                                "ignored\n");
            }
        }
    }

    v->bitmap_blocks = (v->sb.total_blocks / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    v->bitmap = (uint8_t *)calloc(1, (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE);
    if (!v->bitmap) { *err = -6; goto fail; }
    if (io_seek(&v->io, v->sb.metadata_zone_start * INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, v->bitmap, (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE) != 0)
        { *err = -7; goto fail; }

    /* WP30 Phase 5: load metadata extent mapper table (v0.3.0+ only).
     * For format_version=0 (read-only legacy) there is no MET0/mapper; the
     * legacy contiguous inode area is read-only in v3 (vol_open refused
     * writes via VOLF_READONLY). */
    if (v->met0_present) {
        if (meta_mapper_load(v) != 0) { *err = -7; goto fail; }
        v->journal_start = v->sb.metadata_zone_start + v->bitmap_blocks +
                           INVFS_META_EXT_BLOCKS;
    } else {
        /* Legacy layout: journal starts right after the bitmap */
        v->journal_start = v->sb.metadata_zone_start + v->bitmap_blocks;
    }
    v->journal_pos = v->journal_start * INVFS_BLOCK_SIZE;
    v->inode_area_start = v->journal_start + JOURNAL_BLOCKS;
    v->inode_area_pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    v->inode_area_end = (v->sb.metadata_zone_start + v->sb.metadata_zone_blocks)
                        * INVFS_BLOCK_SIZE;

    /* WP30: on a mapper volume the append cursor is extent-relative.
     * Rebase the cursor onto the active extent so writers append inside
     * extent 0 and readers find what was written there. */
    if (v->met0_present && v->meta_mapper &&
        v->met0.extent_count > 0 &&
        v->met0.active_extent < (uint64_t)v->met0.extent_count) {
        uint64_t act = meta_mapper_get(v, (size_t)v->met0.active_extent);
        if (act) {
            uint64_t esz = invfs_meta_ext_size(act);
            uint64_t off = v->met0.active_offset < esz ? v->met0.active_offset : esz;
            uint64_t first_pba = invfs_meta_ext_pba(act);
            v->inode_area_pos = first_pba * (uint64_t)INVFS_BLOCK_SIZE + off;
            if (v->inode_area_end < first_pba * (uint64_t)INVFS_BLOCK_SIZE + esz)
                v->inode_area_end = first_pba * (uint64_t)INVFS_BLOCK_SIZE + esz;
        }
    }

    /* WP-M1: bring up the metadata-v3 engine. The RT30 descriptor is
     * validated for the round-trip.
     *
     * WP-M5: bring the metadata-v3 base-tree engine up. The root lives
     * in RT30 and the allocator + dirty-bitmap flush are owned by
     * vol_metabuf/vol_btree; the inode API (vol_v3_inode_*) writes the
     * base tree directly (no delta yet). The v2 write engine still does
     * not apply to a v3 namespace, so refuse its mutations: keep
     * needs_recovery = 1 (the engine-level backstop vol_write_enabled /
     * vol_mark_dirty consult). VOLF_READONLY is cleared *only* so the
     * shared metadata allocator (alloc_blocks) can hand out base pages;
     * the inode path persists the dirty bitmap before it publishes a
     * root, the superblock is never rewritten, so the on-disk READONLY
     * state is untouched. */
    /* WP-M21: idx_init retired (the in-memory name index is gone;
     * v3 resolves names through the dirent btree). */
    /* ANC0 tail anchor, probed ONCE here and before anything reads
     * block 0's descriptors. The probe is what decides whether the tail
     * block is this volume's anchor at all: on a volume made before the
     * anchor existed its last block is an ordinary data block, and from
     * here on every refresh of the mirror is gated on the answer, so
     * that block is never written. A refusal (a valid anchor belonging
     * to some other geometry) leaves v->anchor_state as a refusal, which
     * is deliberately NOT the same answer as "absent" -- see
     * vol_anchor.c. Only a v3 volume probes: a v2 volume has no RT30 or
     * SPT0 to mirror and its tail is never touched. */
    anchor_probe(v, NULL);
    if (mbuf_rt30_load(v) < 0) { *err = -6; goto fail; }
    v3_probe_rt30(v);
    mbuf_init(v);
    /* WP-M5: skip the WP-M2 bootstrap pool (see v3_ready in
     * vol_btree.c) -- its cursor resets every open, so reusing it would
     * let a COW write clobber a live page from a previous session. */
    v->mb_boot_cursor = v->mb_boot_end;
    v->v3_mbuf_ready = 1;
    v->needs_recovery = 1;
    /* WP-M19: a degraded v3 mount (dev0 absent) serves only from the
     * dev1 metadata mirror; keep it READ-ONLY, exactly like the v2
     * degraded leg. A full two-device v3 mount needs the READONLY bit
     * clear so the shared metadata allocator can hand out base pages. */
    if (v->degraded)
        v->sb.vol_flags |= VOLF_READONLY;
    else
        v->sb.vol_flags &= ~(VOLF_READONLY | VOLF_RO_SPACE);
    /* WP-M10: replay the append-only delta chain named by RT30 into the
     * in-memory index (D1). Overlay reads are WP-M11 and fold is WP-M14;
     * this only reconstructs the recent tier so a crash/remount keeps
     * it. A torn tail is truncated inside vol_delta_mount. */
    if (vol_delta_mount(v) != 0) { *err = -6; goto fail; }
    /* WP-M16: load the save-point descriptor if one exists. */
    if (spt0_load(v) < 0) { *err = -6; goto fail; }
    if (!invfs_sweep_ui_active())
        fprintf(stderr, "vol_open: %s: format v3 (metadata-v3 inode tree): "
                "base root engine up, v2 paths refused\n", real);

    /* WP25: with the name/id indexes live, load the tier + RAW-mirror
     * indexes from their owner records (2-device volumes only; a degraded
     * mount needs the RAW mirror for reads). */
    if (v->ndev == 2) {
        v->tier_owner = vol_find(v, "\x01tier0");
        v->rawm_owner = vol_find(v, "\x01rawm");
        wp25_index_load(v);
    }

    v->alloc_cursor = v->sb.raw_zone_start;
    if (v->next_inode_id == 0)
        v->next_inode_id = 1;
    /* WP85: generation 1 is the mount's own; a rollback bumps it and every
     * write session stamped before the bump goes stale (refused, not
     * re-anchored -- see the write_gen declaration). */
    if (v->write_gen == 0)
        v->write_gen = 1;
    /* ENOSPC defaults for images created before the policy fields */
    if (v->sb.reserved_blocks == 0)
        v->sb.reserved_blocks = (uint32_t)(v->sb.total_blocks / 128 + 64);
    if (v->sb.hard_min_blocks == 0)
        v->sb.hard_min_blocks = (uint32_t)(v->sb.total_blocks / 1024 + 16);
    v->free_blocks = vol_count_free(v);
    alloc_state_reset(v);
    /* H5: a volume whose latch persisted in the superblock re-evaluates it
     * at open: space freed while it was offline (fsck reclaim, a resize,
     * a delete in a session that never flushed the flag clear) must not
     * keep it read-only. In RAM only -- persisted by the first flush. */
    vol_readonly_unlatch(v);

    /* Reconstructed-content cache.
     *
     * INVFS_ARC_BYTES sets the budget; 0 disables the cache outright, which is
     * how the tests measure what it is worth and how a memory-tight host opts
     * out. The default is deliberately modest -- this is a userspace FS process
     * that already buffers whole files for writes, and a cache that competes
     * with that is a worse trade than a slower read.
     *
     * K/M/G suffixes are accepted, and anything unparseable falls back to the
     * default with a complaint. Both matter because the failure was silent and
     * looked like success: "256M" parsed as 256 *bytes*, which leaves the cache
     * switched on and refusing every entry over 128 bytes, and a typo parsed as
     * 0, which switches it off. Either way reads got slow and nothing said why. */
    {
        const char *ab = getenv("INVFS_ARC_BYTES");
        size_t budget = 256u << 20;      /* 256 MB */
        if (ab) {
            char *endp = NULL;
            unsigned long long want = strtoull(ab, &endp, 10);
            unsigned long long mult = 1;
            int ok = (endp != ab);
            if (ok) {
                while (*endp == ' ' || *endp == '\t') endp++;
                switch (*endp) {
                    case 'k': case 'K': mult = 1024ull; endp++; break;
                    case 'm': case 'M': mult = 1024ull * 1024; endp++; break;
                    case 'g': case 'G': mult = 1024ull * 1024 * 1024; endp++; break;
                    default: break;
                }
                if (*endp == 'b' || *endp == 'B') endp++;
                while (*endp == ' ' || *endp == '\t') endp++;
                if (*endp != '\0') ok = 0;
                if (want > (unsigned long long)SIZE_MAX / mult) ok = 0;
            }
            if (ok) {
                budget = (size_t)(want * mult);
            } else {
                fprintf(stderr, "[vol] INVFS_ARC_BYTES=\"%s\" is not a size; "
                                "using the default %llu bytes\n",
                        ab, (unsigned long long)budget);
            }
        }
        v->arc = arc_create(budget);     /* NULL == disabled, callers cope */
        v->arc_budget = (uint64_t)budget;
        if (getenv("INVFS_DEBUG"))
            printf("[vol_open] content cache: %s (%llu bytes)\n",
                   v->arc ? "on" : "off", (unsigned long long)budget);
    }

    /* WP16b codec profile: INVFS_PROFILE = fast|balanced|dense|archive,
     * default balanced. v1 effects: the generic sweep's ZSTD level, and the
     * effective name is published back to the environment so every pack
     * subprocess inherits it (the tools hold one volume per process, so the
     * env IS the volume's context; setenv around each pack exec would be
     * equivalent -- the sweep is single-threaded). An unknown value keeps
     * the default with a complaint (the INVFS_ARC_BYTES convention). */
    {
        const char *pf = getenv("INVFS_PROFILE");
        int p = INVFS_PROFILE_BALANCED;
        if (pf && *pf) {
            int w = invfs_profile_parse(pf);
            if (w < 0)
                fprintf(stderr, "[vol] INVFS_PROFILE=\"%s\" is not a profile; "
                                "using balanced\n", pf);
            else
                p = w;
        }
        v->profile = (uint8_t)p;
#ifdef _WIN32
        {
            /* _putenv does not copy its argument: the string must outlive
             * the volume (static storage). */
            static char pfbuf[64];
            snprintf(pfbuf, sizeof pfbuf, "INVFS_PROFILE=%s",
                     invfs_profile_name(p));
            _putenv(pfbuf);
        }
#else
        setenv("INVFS_PROFILE", invfs_profile_name(p), 1);
#endif
        if (getenv("INVFS_DEBUG")) {
            int ga = invfs_profile_generic_algo(p);
            if (ga == INVFS_ALGO_ZSTD)
                printf("[vol_open] profile: %s (generic zstd level %d)\n",
                       invfs_profile_name(p), invfs_profile_zstd_level(p));
            else
                printf("[vol_open] profile: %s (generic %s)\n",
                       invfs_profile_name(p),
                       ga == INVFS_ALGO_LZ4 ? "lz4" : "verbatim");
        }
    }

    /* WP19: INVFS_HEAT_INIT seeds the read-heat of entries created from
     * now on (import/mkfs-time friendliness: pre-warm files you already
     * know are hot). u16, default 0; the ARC_BYTES complaint convention. */
    {
        const char *hi = getenv("INVFS_HEAT_INIT");
        if (hi && *hi) {
            char *endp = NULL;
            unsigned long want = strtoul(hi, &endp, 10);
            if (endp == hi || *endp != '\0' || want > 0xFFFFul) {
                fprintf(stderr, "[vol] INVFS_HEAT_INIT=\"%s\" is not a u16; "
                                "using 0\n", hi);
            } else {
                v->heat_init = (uint16_t)want;
            }
        }
    }
    /* WP22c test hook: see sync_fail_at (volume_internal.h). Parsed once
     * here like the other env knobs; fires once per process. */
    {
        const char *sf = getenv("INVFS_SYNC_FAIL_AT");
        if (sf && *sf) {
            char *endp = NULL;
            unsigned long long n = strtoull(sf, &endp, 10);
            if (endp != sf && *endp == '\0' && n > 0)
                v->sync_fail_at = n;
        }
    }
    /* Same shape, for the durability point: see flush_fail_at. */
    {
        const char *ff = getenv("INVFS_FLUSH_FAIL_AT");
        if (ff && *ff) {
            char *endp = NULL;
            unsigned long long n = strtoull(ff, &endp, 10);
            if (endp != ff && *endp == '\0' && n > 0)
                v->flush_fail_at = n;
        }
    }
    /* WP20b: a live descriptor means a seal config exists -- start the
     * dirty bitmap (all-ones: the first reseal of a session is a full
     * pass, what happened while unmounted is unknowable). */
    if (v->rd_present && (v->rd.l1_algo || v->rd.l2_algo))
        seal_dirty_reset(v);
    /* WP27: build the pba reference map at open: the map must count the
     * live set EXACTLY for the retire/dedupe free gates, and building it
     * from the records the open scan just CRC-validated is free I/O.
     * Building it lazily (the first retire) would lose records superseded
     * between open and that first use, and a retire's -1 on a
     * never-counted record would drive a live sharer's count to zero. */
    pba_ref_ensure(v);
    *err = 0;
    /* Unclean shutdown. Reading always works -- that is how you find out
     * what survived. For WRITES the old behavior was a hard latch: every
     * mount after an unclean stop stayed read-only until a manual
     * `invf-fsck -f`, which made any ungraceful kill (openrc shutdown
     * storms, OOM, host crash) boot-blocking for root-FS duty.
     * Auto-recovery instead: if the full-record scan just completed with
     * ZERO anomalies (every CRC verified, no torn tail), replay already
     * rebuilt the exact on-disk truth and nothing was lost -- clear DIRTY
     * and continue read-write. Any real damage keeps the conservative
     * manual-fsck path. INVFS_AUTO_RECOVER=0 opts out. */
    if (v->sb.state != INVFS_STATE_CLEAN) {
        const char *ar = getenv("INVFS_AUTO_RECOVER");
        int ro_flag = (v->sb.vol_flags & VOLF_READONLY) != 0;
        if (v->degraded) {
            fprintf(stderr, "vol_open: %s: DEGRADED read-only mount "
                    "(device 0 absent); the recorded state 0x%02X is "
                    "left untouched until reattach\n",
                    real, (unsigned)v->sb.state);
            v->needs_recovery = 1;
        } else if (!ro_flag && v->scan_anomalies == 0 &&
            (!ar || strcmp(ar, "0") != 0)) {
            v->sb.state = INVFS_STATE_CLEAN;
            if (vol_write_sb(v) == 0)
                fprintf(stderr, "vol_open: %s not closed cleanly but scan is "
                                "anomaly-free; recovered to CLEAN (rw)\n",
                        real);
            else
                v->needs_recovery = 1;
        } else {
            fprintf(stderr,
                    "vol_open: %s was not closed cleanly (state=0x%02X%s); "
                    "read-only until recovery. Run `invf-fsck -f %s`.\n",
                    real, (unsigned)v->sb.state,
                    v->scan_anomalies ? ", damaged records" : "",
                    path);
            v->needs_recovery = 1;
        }
    }
    if (v->degraded)
        v->needs_recovery = 1;   /* a degraded mount is always read-only */
    return v;
fail:
    vol_delta_close(v);   /* WP-M10: free any replay index (no-op if none) */
    io_close(&v->io);
    pthread_rwlock_destroy(&v->meta_lock);
    if (v->bitmap) free(v->bitmap);
    free(v->meta_mapper);
    free(v->meta_type_bitmap);
    /* WP126: the orphan collector's candidate set. Rebuilt from nothing on
     * the next open, so there is nothing to persist -- but it is one bit
     * per block plus an array, and a process that opens volumes in a loop
     * (the unit and e2e drivers) would grow it without this. */
    free(v->orph.pba);
    free(v->orph.inlist);
    free(v->heat_tab);
    heat_locks_destroy(v);   /* WP-heat-table-concurrent-safe */
    cpack_locks_destroy(v);  /* WP-cpack-map-copy-out */
    free(v->pba_ref);
    free(v->tier);
    free(v->rawm);
    free(v->path2);
    free(v->path);
    free(v);
    return NULL;
}


invfs_volume *vol_open(const char *path, int *err)
{
    return vol_open_inner(path, err);
}


void vol_close(invfs_volume *v)
{
    if (!v) return;
    /* WP135: the anti-forgetting half of the walk receipt. A caller that
     * drops a walk's status -- including with an explicit (void) cast,
     * which is what silenced warn_unused_result at all five of the sites
     * this WP is about -- leaves a short walk latched on the volume. Close
     * is the one point in every tool's life that the caller does not
     * control, so it is where the question gets asked out loud. Without it
     * "please check the status" is a comment, and the failure mode of a
     * comment is that it is not read. */
    {
        size_t unclaimed = vol_walk_reap(v);
        if (unclaimed)
            fprintf(stderr, "vol_close: %zu v3 walk%s on this volume stopped "
                    "early and %s never accounted for the result. Anything "
                    "that acted on the output of those walks acted on a "
                    "partial listing.\n",
                    unclaimed, unclaimed == 1 ? "" : "s",
                    unclaimed == 1 ? "its caller" : "their callers");
    }
    /* WP27: read heat accrues per session in RAM and persists at the
     * sweep's decay pass (or an explicit vol_heat_persist) -- never here:
     * a close that appended record rewrites per read file would churn the
     * inode area on every read-only mount, and under a live checkpoint
     * (compaction barred) the churn can fill the area outright. */
    /* Close is the only place that can honestly write CLEAN, and it can only
       do so after the maps are down. There was no flush here at all: every
       tool that mutated the volume had to remember to call vol_flush itself,
       and forgetting cost the whole run silently. Only a session that
       actually dirtied the volume writes anything, so invf-ls and invf-cat
       stay read-only.
       WP24-lite: a time-travel handle skips the whole block -- it never
       dirtied the device (vol_mark_dirty refuses), so there is nothing to
       flush and the CLEAN mark is not this view's to write. */
    if (v->dirty) {
        /* WP80/A: v3 sets needs_recovery unconditionally at open (the M5
         * backstop), so it cannot distinguish a real this-session io error
         * there -- io_latched can. A latched volume must never write CLEAN:
         * its in-RAM state may sit past bytes that never reached the device. */
        int latched = v->io_latched ||
                      (v->needs_recovery && !(v->sb.vol_flags & VOLF_V3));
        if (latched) {
            /* WP22c: an io error latched this session. Do NOT run the
             * usual final flush: the in-RAM journal/bitmap may reflect
             * mutations whose records never reached the device (the
             * re-anchored tail), and persisting them over the
             * last-barriered state would invent exactly the F2 mismatch
             * (a live record whose map is gone). Leave the device at the
             * last successful barrier; the next mount recovers. */
            fprintf(stderr, "vol_close: an io error was latched this "
                    "session; the final flush is skipped and the volume "
                    "stays dirty for recovery at the next mount\n");
        } else if (vol_flush(v) == 0) {
            /* WP80/B: the CLEAN superblock describes the state vol_flush
             * just persisted, so it must never be written before that state
             * is durable. Barrier BEFORE the CLEAN write -- not after, and
             * not opt-in. A buffered image file needs it for power-loss
             * safety; a device-backed mount needs it to order the device
             * cache. The documented opt-out (INVFS_CLOSE_NOBARRIER=1) is
             * for buffered image files on hosts where the barrier cost is
             * unacceptable; the flush ordering above still makes process
             * death survivable either way. */
            const char *nb = getenv("INVFS_CLOSE_NOBARRIER");
            if ((!nb || strcmp(nb, "0") == 0) &&
                vmux_barrier(v, "close") < 0) {
                vol_io_error_latch(v, "close barrier");
                fprintf(stderr, "vol_close: barrier before CLEAN failed; "
                        "volume stays dirty and will be recovered at the "
                        "next mount\n");
            } else {
                v->sb.state = INVFS_STATE_CLEAN;
                if (vol_write_sb(v) != 0)
                    fprintf(stderr, "vol_close: could not mark volume clean; "
                                    "next mount will recover\n");
            }
        } else {
            fprintf(stderr, "vol_close: final flush failed; volume stays "
                            "dirty and will be recovered at next mount\n");
        }
    }
    /* WP-M10: drop the in-memory delta index (the log itself is already
     * durable; no flush is needed). No-op for a v2 handle. */
    vol_delta_close(v);
    io_close(&v->io);
    pthread_rwlock_destroy(&v->meta_lock);
    /* WP-M21: idx_clear retired (in-memory name index gone). */
    arc_destroy(v->arc);
    cpack_map_cache_reset(v);
    {
        int i;
        for (i = 0; i < 2; i++) { free(v->rcache[i].blob); v->rcache[i].blob = NULL; }
        pthread_mutex_destroy(&v->rc_mu);
    }
    free(v->heat_tab);
    free(v->pba_ref);
    free(v->seal_dirty);
    free(v->spn_bitmap);        /* WP96: the save point's in-memory mark set */
    free(v->tz);
    free(v->bz);
    free(v->bitmap);
    free(v->meta_mapper);
    heat_locks_destroy(v);   /* WP-heat-table-concurrent-safe */
    cpack_locks_destroy(v);  /* WP-cpack-map-copy-out (after the map reset) */
    free(v->tier);
    free(v->rawm);
    free(v->path2);
    free(v->path);
    free(v);
}


void vol_arc_stats(invfs_volume *v, invfs_arc_stats *out)
{
    arc_stats(v ? v->arc : NULL, out);
}


/* ---- WP10: memory policy (sweep-time admission only) ---- */

#define INVFS_DEC_MEM_DEFAULT (512ull << 20)   /* 512 MiB (WP10 §6) */


/* Recreate the content cache with a new budget (the arc has no resize).
 * 0 keeps the current one, so INVFS_ARC_BYTES at vol_open stays the
 * fallback when no explicit budget was set. */
void vol_set_arc_budget(invfs_volume *v, uint64_t bytes)
{
    if (!v || bytes == 0) return;
    arc_destroy(v->arc);
    v->arc = arc_create((size_t)bytes);   /* NULL == disabled, callers cope */
    v->arc_budget = bytes;
}


void vol_set_dec_mem_limit(invfs_volume *v, uint64_t bytes)
{
    if (v) v->dec_mem_limit = bytes;      /* 0 = default */
}


uint64_t vol_get_dec_mem_limit(invfs_volume *v)
{
    return (v && v->dec_mem_limit) ? v->dec_mem_limit : INVFS_DEC_MEM_DEFAULT;
}


unsigned vol_get_profile(const invfs_volume *v)
{
    return v ? v->profile : INVFS_PROFILE_BALANCED;
}

/* Persist the superblock. The checksum covers bytes 0..0x7B, and `state`
   lives at 0x18 -- inside that range -- so it has to be recomputed here.
   It was not, which was harmless only for as long as nothing inside the
   checksummed range ever changed: the ENOSPC policy fields and the READONLY
   flag sit at 0x80 and beyond deliberately. The moment `state` starts moving
   (which is the whole point of crash detection) a stale checksum turns the
   volume unopenable -- vol_open rejects it with err -5. */
int vol_write_sb(invfs_volume *v)
{
    v->sb.checksum = invfs_crc32c(&v->sb, offsetof(invfs_superblock, checksum));
    if (io_seek(&v->io, 0) != 0 ||
        io_write(&v->io, &v->sb, sizeof(v->sb)) != 0)
        return -1;
    return 0;
}



int vol_flush(invfs_volume *v)
{
    /* Test hook, same shape as sync_fail_at (volume_internal.h): the Nth
     * flush of this process reports -1 without touching the image. It is
     * here, at the top of the function, so it fires for every caller --
     * the durability point at the end of a sweep, the seal tail, the
     * in-FUSE worker's close -- and not only on the v3 branch below. */
    if (v->flush_fail_at && --v->flush_fail_at == 0) {
        vol_io_error_latch(v, "flush (INVFS_FLUSH_FAIL_AT)");
        return -1;
    }
    /* WP24-lite: a time-travel handle never persists. Every mutation was
     * already refused at vol_mark_dirty, so nothing is pending and the
     * flush contract is vacuously satisfied -- and the superblock write
     * below would otherwise land on the PRESENT volume's block 0. */
    /* WP25: a degraded mount (dev0 absent) has nothing pending --
     * vol_mark_dirty refused every mutation -- so a flush attempt would
     * only trip the mirror's read-only refusal. */
    if (v->degraded) return 0;
    /* WP75: the volume keeps its block bitmap dirty in RAM between
     * publishes -- v3_publish (via vol_v3_bitmap_flush) is the only other
     * writer. A flush must persist it too, or the sweep's post-publish
     * dedupe frees (and any allocation after the last publish) are dropped
     * at close and a reopen can re-hand those blocks.
     * Structure-before-reference: make the bitmap durable before
     * returning, mirroring v3_publish. */
    if (vol_v3_bitmap_flush(v) != 0) {
        vol_io_error_latch(v, "v3 bitmap flush");
        return -1;
    }
    if (vmux_barrier(v, "v3 bitmap") < 0)
        return -1;
    /* WP98: the WP25 tier/RAW-mirror indexes need no journal to be
     * durable. The ordering rule is the copies' blocks BEFORE the owner
     * record that names them, and the owner blob is published after the
     * barrier above, so the blocks it names are already durable -- and
     * publish_blob_inode CRC-frames the index bytes, so a torn flush
     * leaves the previous index, never a half one. */
    if (v->ndev == 2 && !v->degraded &&
        (v->rawm_dirty || v->tier_dirty)) {
        if (wp25_owner_sync(v) != 0) {
            vol_io_error_latch(v, "mirror/tier owner sync");
            return -1;
        }
    }
    /* WP99: the two-device commit tail runs AFTER the barrier above,
     * which is what makes the ordering right: the published root (RT30,
     * inside block 0) is durable on both devices BEFORE the sync_seq
     * bump certifies it. */
    if (vol_commit_mirror(v) != 0)
        return -1;
    return 0;
}


/* fsync/fdatasync entry point (WP4ab): everything vol_flush persists
 * (the dirty bitmap range) becomes durable against power loss, not just
 * process death. vol_write_commit
 * has already pushed the data blocks themselves with io_write, so one
 * barrier at the end covers the whole pending state.
 *
 * WP22c/F1: the barrier is also the only place a buffered backing store
 * reports a write error (a pwrite lands in the page cache and returns
 * success; the dm-flakey error window kills the dirty pages in writeback
 * and surfaces EIO here). A failed barrier therefore means the append
 * cursors may already sit past bytes that will never reach the device:
 * latch the volume and re-anchor the tail (vol_io_error_latch). Only a
 * successful barrier moves inode_area_durable. */
int vol_sync(invfs_volume *v)
{
    if (!v) return -1;
    /* WP24-lite: nothing of this handle's can be in flight (mutations are
     * refused), so the durability contract is already met without touching
     * the device. */
    /* WP80/A: v3 is the default full format now, not the empty/read-only
     * skeleton the WP-M1 comment described. Its pending state is the dirty
     * block bitmap, which vol_flush persists (WP75); the delta records are
     * already barriered at append (see Bug C in docs/architecture/META-V3.md)
     * and the data segments they name are written before that barrier, so a
     * vol_flush + one barrier covers the whole fsync contract exactly as the
     * v2 path does. Returning early here silently made FUSE .fsync a no-op. */
    if (vol_flush(v) != 0) return -1;   /* flush latches its own failures */
#ifndef _WIN32
    /* WP22c test hook (tools/test-flushfail.sh): the Nth vol_sync of the
     * process simulates the error window -- the un-barriered inode-area
     * tail dies in "writeback" (zeroed on the image) and the barrier
     * reports EIO. WP80: the hook now runs on v3 too (the early return
     * above used to hide it). v3 has no v2 inode area, so the zeroing is a
     * no-op there -- but the latch, the refused later mutations and the
     * no-CLEAN close are exercised on v3, which is the failure path that
     * matters now. */
    if (v->sync_fail_at && --v->sync_fail_at == 0) {
        if (v->inode_area_pos > v->inode_area_durable) {
            static const uint8_t z[INVFS_BLOCK_SIZE];
            uint64_t p = v->inode_area_durable;
            while (p < v->inode_area_pos) {
                size_t n = (size_t)(v->inode_area_pos - p);
                if (n > sizeof z) n = sizeof z;
                if (io_seek(&v->io, p) != 0 || io_write(&v->io, z, n) != 0)
                    break;   /* the device is erroring anyway: latch below */
                p += n;
            }
        }
        vol_io_error_latch(v, "sync (INVFS_SYNC_FAIL_AT)");
        return -1;
    }
#endif
    if (vmux_barrier(v, "sync") < 0) {
        vol_io_error_latch(v, "sync");
        return -1;
    }
    v->inode_area_durable = v->inode_area_pos;
    return 0;
}


/* allocate n consecutive free blocks in a zone; returns start block or 0.
 * type: INVFS_ALLOC_DATA (0) = data blocks, INVFS_ALLOC_META (1) = metadata blocks.
 * WP30: metadata allocations are tracked separately in meta_type_bitmap. */
uint64_t alloc_blocks(invfs_volume *v, uint64_t zone_start, uint64_t zone_len,
                             uint64_t n, int use_reserve, int type)
{
    uint64_t zone_end = zone_start + zone_len;
    uint64_t i, count = 0, start = 0;
    /* Cursor and free count belong to the zone being scanned, not to the
     * volume: with one shared cursor every spill into SHADOW rewound the
     * next RAW attempt to the zone head, so a full RAW zone was rescanned
     * end to end for every 64 KB segment. */
    uint64_t *cursor, *zone_free, *fail_run;

    if (zone_start == v->sb.shadow_zone_start) {
        cursor = &v->shadow_cursor; zone_free = &v->shadow_free;
        fail_run = &v->shadow_fail_run;
    } else if (v->ndev == 2 && v->arena_blocks &&
               zone_start == v->arena_start) {
        /* WP25: the dev0 tier arena (redundant acceleration copies only) */
        cursor = &v->arena_cursor;  zone_free = &v->arena_free;
        fail_run = &v->arena_fail_run;
    } else {
        cursor = &v->raw_cursor;    zone_free = &v->raw_free;
        fail_run = &v->raw_fail_run;
    }

    if (v->sb.vol_flags & VOLF_READONLY)
        return 0;

    /* Skip the scan when this zone cannot satisfy n. Either it has too few
     * free blocks outright, or a previous scan proved no run this long
     * exists. This is what turns a hopeless RAW retry from 786k bitmap
     * probes into one compare -- per 64 KB segment, so ~100M probes per
     * 8 MB file before this check existed. */
    if (*zone_free < n)
        return 0;
    if (*fail_run && n >= *fail_run)
        return 0;

    /* ENOSPC policy: ordinary writes must leave the reserve + hard-min
     * untouched; sweep/transcodes (use_reserve) may drain the reserve but
     * never the hard-min floor. Hitting the floor flips the volume to
     * READONLY so applications get a clean ENOSPC/EROFS instead of data
     * loss (fsck can then reclaim orphaned blocks). H5: VOLF_RO_SPACE
     * marks it as the SPACE latch (not an operator hold) so
     * vol_readonly_unlatch may release it when space comes back. */
    {
        uint64_t guard = (use_reserve ? 0 : v->sb.reserved_blocks)
                         + v->sb.hard_min_blocks;
        if (v->free_blocks <= guard) {
            if (v->free_blocks <= v->sb.hard_min_blocks &&
                !(v->sb.vol_flags & VOLF_READONLY)) {
                v->sb.vol_flags |= VOLF_READONLY | VOLF_RO_SPACE;
                fprintf(stderr, "[alloc] READONLY: free=%llu hard_min=%u\n",
                        (unsigned long long)v->free_blocks,
                        (unsigned)v->sb.hard_min_blocks);
            }
            return 0;  /* ENOSPC */
        }
    }

    /* WP30 Phase 4: data allocations must respect metadata reservation */
    if (type == INVFS_ALLOC_DATA && v->sb.meta_reserved_pct > 0) {
        uint64_t meta_reserve =
            (v->sb.total_blocks * v->sb.meta_reserved_pct) / 100;
        if (v->free_blocks - n < meta_reserve)
            return 0;  /* ENOSPC */
    }

    i = *cursor;
    if (i < zone_start || i >= zone_end)
        i = zone_start;

    for (count = 0; count < zone_len; count++) {
        if (i >= zone_end) { i = zone_start; start = 0; }
        /* The anchor is the last block of the device and therefore inside the
         * shadow zone's range, so the bitmap test alone would hand it out. See
         * anchor_block_protected for why the bitmap bit is not a sufficient
         * reservation. */
        if (!bit_get(v->bitmap, i) && !anchor_block_protected(v, i)) {
            if (start == 0) start = i;
            if (i - start + 1 == n) {
                uint64_t k;
                for (k = start; k <= i; k++) {
                    bit_set(v->bitmap, k);
                    /* WP30: track metadata allocations separately */
                    if (type == INVFS_ALLOC_META && v->meta_type_bitmap)
                        bit_set(v->meta_type_bitmap, k);
                }
                vol_bm_dirty(v, start);
                vol_bm_dirty(v, i);
                /* WP20b: fresh shadow content invalidates its stripes'
                 * parity (the block's old content was zero-as-absent) */
                if (zone_start == v->sb.shadow_zone_start)
                    seal_dirty_mark(v, start, n);
                *cursor = (i + 1 < zone_end) ? i + 1 : zone_start;
                v->free_blocks -= n;
                *zone_free -= n;
                /* WP30: track metadata free blocks separately */
                if (type == INVFS_ALLOC_META)
                    v->meta_free_blocks -= n;
                return start;
            }
        } else {
            start = 0;
        }
        i++;
    }
    /* Scanned the whole zone without a run of n: the space is there but too
     * fragmented. Record the smallest run size known to fail so later, larger
     * requests skip the scan. Any free() in this zone clears the hint. */
    if (*fail_run == 0 || n < *fail_run)
        *fail_run = n;
    return 0;  /* ENOSPC */
}

/* WP-DZ: allocate `nblocks` for RAW-class content out of the shared free
 * pool. The zone fields of the superblock are ADVISORY POLICY, not hard
 * regions: the raw extent is the preferred home of raw-class content (and
 * its fair share), and once that share cannot satisfy the request the same
 * allocation simply continues into shadow-space blocks. Either way the
 * content class is RAW -- *zone_out is always INVFS_ZONE_RAW. Placement no
 * longer decides what a block IS, so there is no spill path and no
 * mixed-zone file: the sweep's RAW walk keys on the zone TAG, so every
 * raw-class segment stays sweepable wherever it landed (pre-WP-DZ the
 * spill tagged overflow segments BINARY, which made the sweep skip the
 * file forever). Raw-class blocks in the shadow extent are ordinary
 * occupants there: the seal's pba-range stripes cover them like any other
 * occupied shadow block. The reverse crossing does not exist --
 * shadow-class requests keep their canonical shadow-side placement (the
 * seal's stripes are defined over the shadow extent, and on two-device
 * volumes dev1 is the canonical side); only the RAW class is elastic.
 * The preference costs O(1) when RAW is full: alloc_blocks short-circuits
 * on the per-region free count / fail-run hint before touching the
 * bitmap. */
uint64_t alloc_raw_or_shadow(invfs_volume *v, uint64_t nblocks, int *zone_out)
{
    uint64_t pba = alloc_blocks(v, v->sb.raw_zone_start, v->sb.raw_zone_blocks,
                                nblocks, 0, INVFS_ALLOC_DATA);
    if (pba == 0)
        pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                           nblocks, 0, INVFS_ALLOC_DATA);
    if (zone_out) *zone_out = INVFS_ZONE_RAW;
    return pba;
}

/* ==================== WP30: Dynamic Metadata Extents ==================== */

/* WP30: allocate a metadata extent from the free pool.
 * v0.3.0+: mapper table is pre-allocated at mkfs; just find a free slot.
 * size_class: 0=64KB, 1=128KB, 2=256KB... up to 15=2GB
 * Returns mapper entry index (0 on error). */
uint64_t alloc_meta_extent(invfs_volume *v, uint8_t size_class)
{
    uint64_t extent_size = 65536ULL << size_class;
    uint64_t nblocks = extent_size / INVFS_BLOCK_SIZE;
    uint64_t pba;

    if (!v->meta_mapper)
        return 0;  /* Mapper not loaded - should not happen after vol_open */

    /* Check meta reservation: ensure free_blocks won't drop below
     * (meta_reserved_pct of total) after this allocation */
    if (v->sb.meta_reserved_pct > 0) {
        uint64_t meta_reserve = (v->sb.total_blocks * v->sb.meta_reserved_pct) / 100;
        if (v->free_blocks - nblocks < meta_reserve)
            return 0;  /* Would violate meta reservation */
    }

    pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                       nblocks, 0, INVFS_ALLOC_META);
    if (pba == 0)
        return 0;

    /* Find a free mapper entry */
    if (v->meta_mapper_n >= INVFS_META_EXT_ENTRIES)
        return 0;

    uint64_t idx = 0;
    for (idx = 0; idx < INVFS_META_EXT_ENTRIES; idx++) {
        if (invfs_meta_ext_pba(v->meta_mapper[idx]) == 0)
            break;
    }
    if (idx >= INVFS_META_EXT_ENTRIES)
        return 0;

    v->meta_mapper[idx] = invfs_meta_ext_encode(pba, size_class);
    if (idx >= v->meta_mapper_n)
        v->meta_mapper_n = idx + 1;

    return idx + 1;  /* 1-based index for callers */
}

/* WP30: try to extend the active extent in-place if physically adjacent
 * run is free. Returns 1 if extended, 0 if not possible, -1 on error. */
int extend_meta_extent(invfs_volume *v, uint64_t extent_idx, uint8_t new_size_class)
{
    uint64_t old_entry, old_pba, old_size;
    uint8_t old_class;
    uint64_t old_blocks, new_blocks;
    uint64_t adj_pba, adj_blocks;

    if (extent_idx >= INVFS_META_EXT_ENTRIES || extent_idx >= v->meta_mapper_n)
        return 0;

    old_entry = v->meta_mapper[extent_idx];
    old_pba = invfs_meta_ext_pba(old_entry);
    old_class = invfs_meta_ext_class(old_entry);
    old_size = invfs_meta_ext_size(old_entry);
    old_blocks = old_size / INVFS_BLOCK_SIZE;

    if (new_size_class <= old_class)
        return 0;  /* Can only extend, not shrink */

    new_blocks = (65536ULL << new_size_class) / INVFS_BLOCK_SIZE;
    if (new_blocks <= old_blocks)
        return 0;

    adj_blocks = new_blocks - old_blocks;
    adj_pba = old_pba + old_blocks;

    if (bit_get(v->bitmap, adj_pba) == 0) {
        uint64_t k;
        int can_extend = 1;
        for (k = 1; k < adj_blocks; k++) {
            if (bit_get(v->bitmap, adj_pba + k) != 0) {
                can_extend = 0;
                break;
            }
        }
        if (can_extend) {
            for (k = 0; k < adj_blocks; k++) {
                bit_set(v->bitmap, adj_pba + k);
                if (v->meta_type_bitmap)
                    bit_set(v->meta_type_bitmap, adj_pba + k);
            }
            vol_bm_dirty(v, old_pba);
            vol_bm_dirty(v, adj_pba + adj_blocks - 1);
            v->free_blocks -= adj_blocks;
            v->meta_free_blocks -= adj_blocks;

            v->meta_mapper[extent_idx] = invfs_meta_ext_encode(old_pba, new_size_class);
            return 1;
        }
    }
    return 0;  /* Adjacent run not free */
}


/* write raw-class data (RAW-preferred over the shared pool, WP-DZ);
 * returns first pba (0 on error) */
uint64_t vol_write_raw(invfs_volume *v, const uint8_t *data, size_t len)
{
    uint64_t nblocks = (len + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    uint64_t pba = alloc_raw_or_shadow(v, nblocks, NULL);
    if (pba == 0) return 0;
    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, data, (size_t)nblocks * INVFS_BLOCK_SIZE) != 0)
        return 0;
    /* WP25: raw-zone writes dual-write to the dev1 mirror (raw_mirror) */
    if (v->ndev == 2 && v->raw_mirror && !v->degraded &&
        pba >= v->sb.raw_zone_start &&
        pba < v->sb.raw_zone_start + v->sb.raw_zone_blocks) {
        if (wp25_rawm_write(v, pba, data, nblocks) != 0) {
            vol_io_error_latch(v, "raw mirror write (dev1)");
            return 0;
        }
    }
    return pba;
}




/* ---- WP22d: consistent-cut machinery ------------------------------------
 * WP27: file records carry their segments' pbas, so "the record is the
 * map": a live record can never name an unmapped segment any more. The
 * cut's per-version "broken" test degrades to an entry-pba sanity check
 * (rec_pba_miss); drop-torn DATA is content-level (segment CRC at read
 * time; fsck's INVFS_FSCK_CONTENT pass) exactly as it always was.
 */

/* Count a record's AST entries whose pba is invalid (0, or past the volume
 * end); the first few lost ranges are collected for the loud log
 * (miss_off/miss_len, up to *miss_n... in: capacity is 4, out: stored
 * count). An entry with length 0 never references data. */
uint64_t rec_pba_miss(const uint8_t *rec, uint32_t rec_len,
                      uint64_t total_blocks, uint64_t *miss_off,
                      uint64_t *miss_len, unsigned *miss_n)
{
    invfs_ast_hdr ah;
    size_t off;
    uint32_t i;
    uint64_t miss = 0;

    *miss_n = 0;
    if (rec_len < INVFS_REC_HDR_LEN + 1)
        return 1;   /* no room for a name byte */
    off = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)rec) - rec);
    if (off + INVFS_AST_HDR_V1_LEN > rec_len ||
        invfs_ast_hdr_parse(rec + off, rec_len - off, &ah) != 0)
        return 1;   /* unparseable header: treat as broken */
    off += ah.hdr_len;
    for (i = 0; i < ah.num_blocks; i++) {
        invfs_ast_block_entry e;
        if (off + sizeof(e) > rec_len) { miss++; break; }
        memcpy(&e, rec + off, sizeof(e));
        off += sizeof(e);
        if (e.length == 0) continue;
        if (e.pba == 0 || e.pba >= total_blocks) {
            if (*miss_n < 4) {
                miss_off[*miss_n] = e.file_offset;
                miss_len[*miss_n] = e.length;
                (*miss_n)++;
            }
            miss++;
        }
    }
    return miss;
}
















/* read one block worth of raw bytes at pba */
int vol_read_block(invfs_volume *v, uint64_t pba, void *buf)
{
    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, buf, INVFS_BLOCK_SIZE) != 0) {
        /* WP25: a raw-zone block on an absent/failed dev0 is served by
         * its dev1 mirror block (mirror = 1:1 block copy) */
        uint64_t mpba = 0, mlen = 0;
        if (v->ndev == 2 &&
            pba >= v->sb.raw_zone_start &&
            pba < v->sb.raw_zone_start + v->sb.raw_zone_blocks &&
            wp25_rawm_lookup(v, pba, &mpba, &mlen) == 0 && mlen >= 1) {
            if (io_seek(&v->io, mpba * INVFS_BLOCK_SIZE) == 0 &&
                io_read(&v->io, buf, INVFS_BLOCK_SIZE) == 0)
                return 0;
        }
        return -1;
    }
    return 0;
}


/* Write one segment as whole blocks.
 *
 * phys_blocks is ceil(payload / block size) and alloc_blocks hands over that
 * many blocks exclusively, so the slack at the end of the last block belongs
 * to this segment and to nothing else. Writing only the payload left that
 * block partially covered, which forced the block layer to read it back first
 * to preserve bytes that were never anyone's data. On flash that read costs
 * about what the write costs: a full-tree copy issued 103418 reads against
 * 103466 writes, very nearly one wasted read per segment. Padding to the block
 * boundary makes the transfer aligned at both ends, so it needs no read at
 * all, and zeroing the slack beats leaving whatever the block held before.
 *
 * buf must have room for phys_blocks * INVFS_BLOCK_SIZE bytes.
 */
int write_segment_blocks(invfs_volume *v, uint64_t pba, uint8_t *buf,
                                size_t payload, uint64_t phys_blocks)
{
    size_t span = (size_t)phys_blocks * INVFS_BLOCK_SIZE;
    if (span > payload)
        memset(buf + payload, 0, span - payload);
    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, buf, span) != 0) {
        /* WP25 rule 3: with raw_mirror the segment's second copy lands on
         * dev1; a dev0 RAW write failure is logged and the mirror becomes
         * the surviving copy (reads fail over to it) -- NOT a full latch.
         * A dev1 (mirror) write failure latches. */
        if (v->ndev == 2 && v->raw_mirror && !v->degraded &&
            pba >= v->sb.raw_zone_start &&
            pba < v->sb.raw_zone_start + v->sb.raw_zone_blocks) {
            if (!v->rawio_logged) {
                v->rawio_logged = 1;
                fprintf(stderr, "vol: dev0 RAW write failed at pba %llu; "
                        "continuing on the dev1 mirror (raw_mirror=1)\n",
                        (unsigned long long)pba);
            }
            if (wp25_rawm_write(v, pba, buf, phys_blocks) != 0) {
                vol_io_error_latch(v, "raw mirror write (dev1)");
                return -1;
            }
            return 0;
        }
        return -1;
    }
    /* WP25: the RAW mirror -- every raw-zone segment is dual-written to
     * dev1 (SSD loss loses nothing). The mirror is an ordinary canonical
     * shadow allocation owned by "\x01rawm". */
    if (v->ndev == 2 && v->raw_mirror && !v->degraded &&
        pba >= v->sb.raw_zone_start &&
        pba < v->sb.raw_zone_start + v->sb.raw_zone_blocks) {
        if (wp25_rawm_write(v, pba, buf, phys_blocks) != 0) {
            vol_io_error_latch(v, "raw mirror write (dev1)");
            return -1;
        }
    }
    /* WP20b: overwriting occupied shadow blocks dirties their stripes */
    seal_dirty_mark(v, pba, phys_blocks);
    return 0;
}


/* ---- WP27: segment extents --------------------------------------------
 * The 32-byte AST entry carries the pba but no physical block count (the
 * wire format has no room); a framed segment's extent derives from its own
 * 8-byte header: [4B csize LE][4B crc32c] at pba, plen = ceil((csize+8)/
 * 4096). csize == 0 never names a written segment (an empty file has no
 * entries; blob creators refuse empty blobs), so it reads as "unknown". */
int seg_extent(invfs_volume *v, uint64_t pba, uint32_t *csize_out,
               uint64_t *plen_out)
{
    uint8_t hdr[8];
    uint32_t csize;
    uint64_t plen;
    if (!pba || pba >= v->sb.total_blocks) return -1;
    if (io_pread(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE, hdr, 8) != 0)
        return -1;
    memcpy(&csize, hdr, 4);
    if (!csize) return -1;
    plen = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    if (plen > v->sb.total_blocks - pba) return -1;
    if (csize_out) *csize_out = csize;
    if (plen_out) *plen_out = plen;
    return 0;
}


/* The destructive-free gate: derive the extent AND prove the whole derived
 * run is still marked allocated in the in-memory bitmap (a segment's
 * blocks are one contiguous exclusively-owned run). A torn header almost
 * certainly fails one of the two checks; the failure mode is a leak
 * (fsck reclaims), never an over-free. */
int seg_extent_checked(invfs_volume *v, uint64_t pba, uint64_t *plen_out)
{
    uint64_t plen = 0, b;
    if (seg_extent(v, pba, NULL, &plen) != 0)
        return -1;
    for (b = pba; b < pba + plen; b++)
        if (!bit_get(v->bitmap, b))
            return -1;
    *plen_out = plen;
    return 0;
}


/* ---- WP27: pba reference map (PB7 without the L2P) --------------------
 * Counts, per segment pba, how many LIVE records' AST entries name it
 * (dedupe shares, WP4b session aliases, hardlink twin records). Only
 * non-owner records' non-TEXT entries count: owner records ("\x01...")
 * free through the owner WAL, and TEXT member entries name owner-owned
 * batch blocks the member's retire must never free (the WP10 §7 gate). */
static uint64_t pba_ref_hash(uint64_t pba)
{
    pba ^= pba >> 30; pba *= 0xbf58476d1ce4e5b9ULL;
    pba ^= pba >> 27; pba *= 0x94d049bb133111ebULL;
    return pba ^ (pba >> 31);
}

/* WP unlink-takes-map-after-dirent-drop: THE MAP'S MISSING LOCK.
 *
 * Every pba_ref primitive below is a read-modify-write of v->pba_ref, and
 * two of them REPLACE the array: pba_ref_free() does free(v->pba_ref), and
 * pba_ref_grow() does free(v->pba_ref); v->pba_ref = nt. A second writer is
 * therefore not a lost update, it is a use-after-free. The map was
 * single-writer by ACCIDENT -- it relied on every caller happening to be
 * under the FUSE daemon's one big mutex (g_io_lock, src/cli/fuse_fs.c:32),
 * which is a `static` in that translation unit. Nothing in src/core/ can
 * take it, and the offline tools are not linked against it: invf-sweep runs
 * its sweep on a worker thread (tools/invf-sweep.c) and holds nothing. They
 * are safe only because each is a single writer per volume, which is a
 * property of their structure and not a guarantee this code made or checked.
 *
 * So the lock lives here, next to the data it protects. File-static rather
 * than a volume member: it needs no init/destroy pairing on the
 * invfs_volume allocation and no ABI change, and a process opens one volume
 * at a time in every caller that exists (the daemon has exactly one g_vol),
 * so a process-wide lock costs nothing that a per-volume one would not.
 *
 * RECURSIVE, for two reasons that are both real: pba_ref_ensure walks the
 * namespace and calls pba_ref_modify once per block entry, and the retire
 * paths nest -- vol_v3_unlink calls vol_delete_siblings, which calls
 * vol_v3_unlink again. */
static pthread_mutex_t g_pba_ref_mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

/* Depth of the hold/release pairs below. A count and not a flag, so a
 * nested retire inside an outer one cannot unlock early. */
static int g_pba_ref_held;

/* Span a critical section that is not a single function: a retire has to
 * take the map, drop the name, drop the row, and subtract -- and all four
 * have to be one atomic thing with respect to the map, because a rebuild
 * that lands between the first and the last rebuilds against a live set the
 * pending -1 does not belong to. See vol_v3_unlink. */
void vol_pba_ref_hold(invfs_volume *v)
{
    (void)v;
    pthread_mutex_lock(&g_pba_ref_mu);
    g_pba_ref_held++;
}

void vol_pba_ref_release(invfs_volume *v)
{
    (void)v;
    if (g_pba_ref_held > 0)
        g_pba_ref_held--;
    pthread_mutex_unlock(&g_pba_ref_mu);
}

static void pba_ref_free(invfs_volume *v)
{
    free(v->pba_ref);
    v->pba_ref = NULL;
    v->pba_ref_mask = 0;
    v->pba_ref_on = 0;
    v->pba_ref_stale = 0;
}

/* WP pba-ref-v3-incremental: a recipe was published by a path that does not
 * adjust the map itself (vol_v3_inode_delta_put with a new recipe_addr).
 * The next pba_ref_ensure rebuilds from the live set before any free gate
 * can read a count that predates that publish. */
void pba_ref_invalidate(invfs_volume *v)
{
    pthread_mutex_lock(&g_pba_ref_mu);
    if (v) v->pba_ref_stale = 1;
    pthread_mutex_unlock(&g_pba_ref_mu);
}

/* The converse: the caller kept the map exact by hand (the sweep's segment
 * remap, dedupe's remap), so the recipe it just published is accounted for. */
void pba_ref_validate(invfs_volume *v)
{
    pthread_mutex_lock(&g_pba_ref_mu);
    if (v) v->pba_ref_stale = 0;
    pthread_mutex_unlock(&g_pba_ref_mu);
}

/* drop the map (a path that rewrote records without the apply hooks --
 * the in-place sweep fallback, the fsck rebuild); the next retire/dedupe
 * rebuilds it lazily from the live set */
void pba_ref_reset(invfs_volume *v)
{
    pthread_mutex_lock(&g_pba_ref_mu);
    pba_ref_free(v);
    pthread_mutex_unlock(&g_pba_ref_mu);
}

/* grow to 2x and rehash; failure drops the whole map (the callers' free
 * decisions then run WITHOUT sharing knowledge -- the pre-PB7 leak
 * direction, never corruption: a refcount can only be lost, and a count
 * that reads 0 for a shared pba would over-free, so the map DEGRADES TO
 * "never free" instead: pba_ref_count returns 2 on a missing map). */
static int pba_ref_grow(invfs_volume *v)
{
    size_t ncap = (v->pba_ref_mask + 1) * 2, i;
    pba_ref_ent *nt = (pba_ref_ent *)calloc(ncap, sizeof *nt);
    if (!nt) { pba_ref_free(v); return -1; }
    for (i = 0; i <= v->pba_ref_mask; i++) {
        if (v->pba_ref[i].pba) {
            size_t k = (size_t)pba_ref_hash(v->pba_ref[i].pba) & (ncap - 1);
            while (nt[k].pba) k = (k + 1) & (ncap - 1);
            nt[k] = v->pba_ref[i];
        }
    }
    free(v->pba_ref);
    v->pba_ref = nt;
    v->pba_ref_mask = ncap - 1;
    return 0;
}

/* The body, called with g_pba_ref_mu held. Split out rather than unlocked
 * inline because it has an early return on a grow failure, and a return
 * that forgot an unlock is a deadlock rather than a wrong answer. */
static void pba_ref_modify_locked(invfs_volume *v, uint64_t pba, int delta)
{
    size_t k;
    if (!v->pba_ref_on || !v->pba_ref || !pba || pba >= v->sb.total_blocks)
        return;
    if (delta > 0 && v->pba_ref_n * 10 >= (v->pba_ref_mask + 1) * 7 &&
        pba_ref_grow(v) != 0)
        return;
    k = (size_t)pba_ref_hash(pba) & v->pba_ref_mask;
    while (v->pba_ref[k].pba && v->pba_ref[k].pba != pba)
        k = (k + 1) & v->pba_ref_mask;
    if (v->pba_ref[k].pba) {
        if (delta > 0) v->pba_ref[k].n++;
        else if (v->pba_ref[k].n) v->pba_ref[k].n--;
    } else if (delta > 0) {
        v->pba_ref[k].pba = pba;
        v->pba_ref[k].n = 1;
        v->pba_ref_n++;
    }
}

void pba_ref_modify(invfs_volume *v, uint64_t pba, int delta)
{
    pthread_mutex_lock(&g_pba_ref_mu);
    pba_ref_modify_locked(v, pba, delta);
    pthread_mutex_unlock(&g_pba_ref_mu);
}

/* delta = +1 (record appended) / -1 (record killed). Owner records and
 * TEXT entries are skipped (see the section comment). Decrement floors at
 * 0: a record absent from the build (dead before the map existed) must not
 * drive counts negative. */
static void pba_ref_apply_locked(invfs_volume *v, const uint8_t *rec,
                                uint32_t rec_len, int delta)
{
    invfs_ast_hdr ah;
    const invfs_inode_rec *rh;
    size_t base, off;
    uint32_t i;

    if (!v->pba_ref_on || !v->pba_ref) return;
    if (rec_len < INVFS_REC_HDR_LEN + 1) return;
    rh = (const invfs_inode_rec *)rec;
    if (rh->magic != INODE_REC_MAGIC) return;
    if (rh->name_len && rh->name[0] == 0x01) return;   /* owner: WAL-owned */
    base = (size_t)(invfs_rec_cbody(rh) - rec);
    if (rec_len < base + INVFS_AST_HDR_V1_LEN) return;
    if (invfs_ast_hdr_parse(rec + base, rec_len - base, &ah) != 0) return;
    off = base + ah.hdr_len;
    if ((size_t)ah.num_blocks * sizeof(invfs_ast_block_entry) >
        rec_len - off)
        return;
    for (i = 0; i < ah.num_blocks; i++) {
        const invfs_ast_block_entry *e = (const invfs_ast_block_entry *)
            (rec + off + (size_t)i * sizeof(*e));
        if (e->zone == INVFS_ZONE_TEXT || !e->pba) continue;
        pba_ref_modify_locked(v, e->pba, delta);
    }
}

void pba_ref_apply(invfs_volume *v, const uint8_t *rec, uint32_t rec_len,
                   int delta)
{
    pthread_mutex_lock(&g_pba_ref_mu);
    pba_ref_apply_locked(v, rec, rec_len, delta);
    pthread_mutex_unlock(&g_pba_ref_mu);
}

uint32_t pba_ref_count(invfs_volume *v, uint64_t pba)
{
    size_t k;
    uint32_t n;
    pthread_mutex_lock(&g_pba_ref_mu);
    if (!v->pba_ref_on || !v->pba_ref) { pthread_mutex_unlock(&g_pba_ref_mu); return 2; }  /* unknown: never free */
    k = (size_t)pba_ref_hash(pba) & v->pba_ref_mask;
    n = 0;
    while (v->pba_ref[k].pba) {
        if (v->pba_ref[k].pba == pba) { n = v->pba_ref[k].n; break; }
        k = (k + 1) & v->pba_ref_mask;
    }
    pthread_mutex_unlock(&g_pba_ref_mu);
    return n;
}

/* Build the map from the live name-index set (one read per live record).
 * Idempotent. Runs lazily on the first retire/dedupe of a session; the
 * open path does not pay for it. */
/* WP48: single-pass pba reference build. The previous implementation
 * iterated the name index and called meta_read_record_by_id() per name;
 * on a mapper volume every record whose id-index hint sat in an older
 * extent was re-found by a FULL extent walk, i.e. O(N^2) reads (observed
 * as a 7-hour spin with ~1.3e10 read syscalls on a 66k-record volume).
 * The shared walker visits each live record exactly once. */
typedef struct { invfs_volume *v; } pba_ref_ensure_ctx;



static int pba_ref_v3_walk_cb(void *ctx_, const char *path, uint64_t inode_id,
                              uint32_t type, uint64_t size, int64_t mtime)
{
    pba_ref_ensure_ctx *c = (pba_ref_ensure_ctx *)ctx_;
    invfs_volume *v = c->v;
    invfs_v3_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0, i;
    (void)path; (void)size; (void)mtime;

    if (type == INVFS_ITYP_DIR || !inode_id) return 0;
    if (path && (unsigned char)path[0] == 0x01) return 0;
    /* A symlink's blob is its target string, not an AST, so it names no
     * segment and must contribute no reference. Stated through the same
     * predicate the read path dispatches on (src/core/volume.h) rather
     * than left to the parse happening to fail.
     *
     * This one is not merely redundant. Every fabricated entry a symlink
     * donates lands here as a +1 on a block a live recipe owns, and
     * pba_ref_modify floors at 0, so the phantom reference is never given
     * back: the block reads as shared, and when its real owner goes the
     * count goes 2 -> 1 and vol_free_blocks' gate is never satisfied.
     * A leaked extent, from a name on a volume. Measured in
     * src/cli/dedupe_symlink_test leg F3 against a control volume
     * identical but for the symlink: pba_ref_count(canon) 3 vs 2.
     *
     * The LOAD is preserved for the same reason as in the checkers: this
     * walk is not a health check, but nothing downstream wants a missing
     * blob diagnosed here either -- the recipe audit in vol_btree.c
     * reports that, and it does its own load. The guard sits directly
     * above the parse, after the load, so the shape of this function
     * stays "load, then decide" like every sibling reader. */
    if (invfs_inode_content_is_raw_blob(type)) return 0;
    /* ---- below here, an unreadable row is NOT a row with no references ----
     *
     * Everything above this line is a statement about the inode: a
     * directory, an owner record, a symlink. All of them are TRUE statements,
     * and a return of 0 is the walk's "I have nothing to add".
     *
     * Everything from here on is a statement about whether this build can
     * be trusted, and a return of 0 there is a lie with a body count behind
     * it. This map is the SOLE gate on every data-block free
     * (vol_v3_free_recipe_blocks :179, the write commit's retire loop
     * vol_write.c:860, the sweep remap vol_sweep.c:1418, dedupe's loser
     * retire vol_dedupe.c:275), so a live reference the map does not hold is
     * a block that gets freed while a live recipe still names it -- silent,
     * cross-file, and invisible to invf-verify --deep, which checks
     * readability and length and cannot see that an invfs_ast_block_entry
     * carries a pba rather than a content hash.
     *
     *   two files share pba X   (a correct map says 2)
     *   B's row is unreadable   (this used to `return 0`)
     *   B is unlinked           (the -1 takes the count to 0)
     *   X is FREED              -- while A still names it
     *
     * So each of these is a non-zero return: it aborts v3_walk_dir, and
     * pba_ref_ensure below throws the whole partial map away rather than
     * declaring it exact. Non-zero is not "skip this inode" -- a skip is
     * precisely the bug.
     *
     * The cost is named here because it is the thing a reviewer should
     * check the shape against. A quarantined b+ tree range reads EIO for
     * every key inside it (AGENTS.md 2.6), so a volume in that state can no
     * longer build this map, and with no map every data-block free is
     * refused: the volume leaks rather than frees wrong. That is the correct
     * trade -- the alternative is the chain above -- and it is the same one
     * the map already makes for a missing entry (pba_ref_count returns 2,
     * "unknown: never free"), so no new state and no new policy: a build
     * that could not see everything lands in the state a build that has not
     * run yet is already in.
     */
    {
        int grc = vol_v3_inode_get(v, inode_id, &in);
        if (grc != 1) {
            fprintf(stderr, "pba_ref: inode %llu's row is not readable "
                    "(rc=%d); the reference map cannot be built exact, so no "
                    "block will be freed through it this session\n",
                    (unsigned long long)inode_id, grc);
            return -1;
        }
    }
    if (in.size == 0) return 0;
    /* A RECIPE BLOB is the same question asked one step further down, and
     * skipping one is the SAME wrong free by a different route: the row is
     * live and names its segments, so their pbas must be counted, and a blob
     * that will not load contributes nothing. Nothing distinguishes "this
     * blob is gone" from "these blocks are free" at the gate. The restore
     * side of the savepoint already refuses on exactly this condition
     * (spn_data_check_ino, src/core/vol_spt0.c:1182-1187), which is the
     * proof that the two ends are supposed to agree and that this one did
     * not. */
    if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob) {
        fprintf(stderr, "pba_ref: inode %llu's recipe blob does not load; "
                "the reference map cannot be built exact, so no block will be "
                "freed through it this session\n",
                (unsigned long long)inode_id);
        return -1;
    }
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 || !ents) {
        fprintf(stderr, "pba_ref: inode %llu's recipe blob does not parse; "
                "the reference map cannot be built exact, so no block will be "
                "freed through it this session\n",
                (unsigned long long)inode_id);
        free(blob);
        return -1;
    }
    for (i = 0; i < n_ents; i++) {
        if (ents[i].zone != INVFS_ZONE_TEXT && ents[i].pba) {
            pba_ref_modify(v, ents[i].pba, +1);
        }
    }
    free(blob);
    return 0;
}

/* The body, called with g_pba_ref_mu held (see pba_ref_modify_locked for why
 * the early returns live in a separate function). */
static int pba_ref_ensure_locked(invfs_volume *v)
{
    pba_ref_ensure_ctx c;
    /* WP pba-ref-v3-incremental: an existing map is only reused when nothing
     * has published a recipe behind its back. v3 has no per-record birth /
     * death hook (vol_v3_inode_delta_put is the single choke point and it
     * used to be silent here), so a map built at vol_open over an EMPTY
     * volume stayed "on" and authoritative-looking for the rest of the
     * session: every inode created afterwards was invisible to it, and the
     * first count that reached 0 on a live pba freed a block a live recipe
     * still named. Rebuilding here costs one walk per recipe change -- the
     * write commit already paid exactly that (vol_write.c pba_ref_reset +
     * pba_ref_ensure) -- and a caller that forgets to invalidate can only
     * cost a walk, never a free on a stale count. */
    if (v->pba_ref_on && !v->pba_ref_stale) return 0;
    pba_ref_free(v);
    v->pba_ref_mask = 1023;
    v->pba_ref = (pba_ref_ent *)calloc(v->pba_ref_mask + 1,
                                     sizeof *v->pba_ref);
    if (!v->pba_ref) { v->pba_ref_mask = 0; return -1; }
    v->pba_ref_n = 0;
    v->pba_ref_on = 1;
    c.v = v;
    /* vol_v3_walk_STICT, and the status is CHECKED.
     *
     * Both halves of that are the fix. The walk is strict because a row it
     * cannot read is a live reference the map would not hold, and
     * pba_ref_v3_walk_cb now aborts the walk when that happens (and when a
     * recipe blob will not load or parse, which is the same wrong free by a
     * different route). The status is checked because an unchecked walk
     * status is how the walk's answer was being thrown away: it returned 0
     * for "I saw everything" and 1 for "I stopped early", and both were
     * discarded, so the line below set pba_ref_stale = 0 -- the "this map is
     * exact" flag -- on a map built from a partial view of the namespace.
     *
     * A partial map is the one thing this function must never return. Every
     * reference it is missing is a block the four free gates will read as
     * unshared.
     *
     * So an incomplete build DISCARDS the map: pba_ref_free leaves
     * pba_ref_on = 0 and pba_ref = NULL, and that is a state the map's own
     * API already answers for -- pba_ref_count returns 2 ("unknown: never
     * free", :3469), pba_ref_modify is a no-op (:3416), pba_ref_apply is a
     * no-op (:3446). No caller has to learn a new state, and none of the
     * four free gates can be reached with it. They are also free to ignore
     * this function's return value, which they all do today
     * (vol_ast.c:177, vol_write.c:849, vol_sweep.c:1185, vol_dedupe.c:475,
     * vol_dirs.c:552, :649) -- with the map absent, "ignored" means
     * "never free", which is the safe direction and the reason the ignore is
     * tolerable rather than a second defect.
     *
     * pba_ref_stale is left 1 so the next call tries again: a row that could
     * not be read may well be readable a moment later (that is the whole
     * shape of a fold racing a read), and a permanent failure costs a walk
     * per attempt on a volume that is already damaged. */
    if (vol_v3_walk_strict(v, pba_ref_v3_walk_cb, &c) != 0) {
        pba_ref_free(v);
        v->pba_ref_stale = 1;
        return -1;
    }
    v->pba_ref_stale = 0;
    return 0;
}

int pba_ref_ensure(invfs_volume *v)
{
    int rc;
    pthread_mutex_lock(&g_pba_ref_mu);
    rc = pba_ref_ensure_locked(v);
    pthread_mutex_unlock(&g_pba_ref_mu);
    return rc;
}


const invfs_superblock *vol_sb(invfs_volume *v)
{
    return &v->sb;
}


/* WP59: codec-policy accessors */
int vol_pck0_present(const invfs_volume *v)
{
    return v && v->pk_present;
}

const invfs_pck0 *vol_pck0(const invfs_volume *v)
{
    return (v && v->pk_present) ? &v->pk : NULL;
}

int vol_pck0_gate_failed(const invfs_volume *v)
{
    return v && v->pk_gate_failed;
}

const char *vol_pck0_gate_msg(const invfs_volume *v)
{
    return v ? v->pk_gate_msg : "";
}


const uint8_t *vol_bitmap(invfs_volume *v, uint64_t *blocks_out)
{
    if (blocks_out) *blocks_out = v->sb.total_blocks;
    return v->bitmap;
}



uint64_t vol_inode_area_pos(invfs_volume *v) { return v->inode_area_pos; }

uint64_t vol_inode_area_start(invfs_volume *v) { return v->inode_area_start * INVFS_BLOCK_SIZE; }

uint64_t vol_inode_area_end(invfs_volume *v) { return v->inode_area_end; }

uint64_t vol_journal_pos(invfs_volume *v)   { return v->journal_pos; }


/* Bytes left for new inode records. The area is append-only, so this is what
   stands between the volume and "create silently returns 0": callers that can
   still report an error to the application should check it before accepting
   data, not after. A record is INVFS_REC_HDR_LEN + the name + the AST recipe,
   so this is an upper bound on what will fit, not a file count. */
uint64_t vol_inode_area_free(invfs_volume *v)
{
    return v->inode_area_pos < v->inode_area_end
         ? v->inode_area_end - v->inode_area_pos : 0;
}



uint64_t vol_inode_next(invfs_volume *v, uint64_t pos, uint32_t *magic_out,
                        uint64_t *inode_out, uint64_t *size_out,
                        char *name_out, size_t name_cap, uint32_t *rec_len_out)
{
    invfs_inode_rec rec_h;
    /* WP30 (v0.3.0+): records live in dynamic metadata extents tracked by
     * the Mapper. The scan walks one extent at a time; bounds are defined
     * by the extent sizes, NOT by inode_area_end. inode_area_pos still
     * marks the CRC-validated tail of the active extent (vol_open trims at
     * the first torn record). For legacy format_version=0 the contiguous
     * inode area is still valid, so we fall back to it. */
    for (;;) {
        uint64_t end;
        size_t cur_ei = 0;
        int in_mapper = 0;
        if (v->met0_present && v->meta_mapper) {
            size_t ei;
            int found = 0;
            pthread_rwlock_rdlock(&v->meta_lock);
            for (ei = 0; ei < (size_t)v->met0.extent_count; ei++) {
                uint64_t entry = meta_mapper_get(v, ei);
                if (!entry) break;
                uint64_t pba = invfs_meta_ext_pba(entry);
                uint64_t sz = invfs_meta_ext_size(entry);
                uint64_t start = pba * INVFS_BLOCK_SIZE;
                uint64_t stop = start + sz;
                if ((int)ei == (int)v->met0.active_extent && stop > v->inode_area_pos)
                    stop = v->inode_area_pos;
                if (pos >= start && pos < stop) {
                    end = stop;
                    cur_ei = ei;
                    found = 1;
                    in_mapper = 1;
                    break;
                }
                if (pos < start) {
                    pos = start;
                    end = stop;
                    cur_ei = ei;
                    found = 1;
                    in_mapper = 1;
                    break;
                }
            }
            pthread_rwlock_unlock(&v->meta_lock);
            if (!found) return 0;
        } else {
            end = v->inode_area_pos;
        }
        while (pos + INVFS_REC_HDR_LEN <= end) {
            if (vol_read_raw(v, pos, &rec_h, sizeof(rec_h)) != 0)
                break;
            if (rec_h.magic != INODE_REC_MAGIC && rec_h.magic != TOMBSTONE_MAGIC)
                break;  /* end of valid records in this extent */
            if (rec_h.rec_len < INVFS_REC_HDR_LEN + 1 ||
                rec_h.rec_len > INVFS_MAX_REC_LEN)
                break;
            if (magic_out)  *magic_out = rec_h.magic;
            if (inode_out)  *inode_out = rec_h.inode_id;
            if (size_out)   *size_out = rec_h.file_size;
            if (rec_len_out) *rec_len_out = rec_h.rec_len;
            if (name_out && name_cap) {
                /* name_len comes off disk, so clamp against the field it
                   indexes, the bytes actually present before the body, and the
                   caller's buffer. The name is at offset INVFS_REC_HDR_LEN in
                   the on-disk record, not in the 36-byte prefix copy rec_h. */
                size_t n = rec_h.name_len;
                size_t present = (size_t)rec_h.rec_len - INVFS_REC_HDR_LEN - 1;
                if (n > INVFS_MAX_NAME) n = INVFS_MAX_NAME;
                if (n > present) n = present;
                if (n > name_cap - 1) n = name_cap - 1;
                if (vol_read_raw(v, pos + INVFS_REC_HDR_LEN, name_out, n) != 0)
                    n = 0;
                name_out[n] = 0;
            }
            return pos + rec_h.rec_len + 4;  /* +4: trailing CRC32C */
        }
        /* Out of records in this extent. Continue to next extent. */
        if (!in_mapper) return 0;
        size_t next_ei = cur_ei + 1;
        pthread_rwlock_rdlock(&v->meta_lock);
        uint64_t ne = (next_ei < (size_t)v->met0.extent_count)
                      ? meta_mapper_get(v, next_ei) : 0;
        uint64_t next_pos = ne ? invfs_meta_ext_pba(ne) * INVFS_BLOCK_SIZE : 0;
        pthread_rwlock_unlock(&v->meta_lock);
        if (!ne) return 0;
        pos = next_pos;
        /* loop and scan the next extent */
    }
}


/* Bug J companion: route any record-area append through the mapper.
 * On a v0.3.0+ mapper volume the append position comes from
 * meta_get_append_pos (extents grow automatically); on a legacy volume
 * the caller's inode_area_pos is the continue-position as before. The
 * callers then io_seek(*rec_pos_out), write, and bump active_offset /
 * inode_area_pos. rc 0 = ok, -1 = error, -2 = ENOSPC. */
int vol_append_slot(invfs_volume *v, uint64_t rec_size,
                    uint64_t *rec_pos_out)
{
    if (v->met0_present && v->meta_mapper) {
        uint64_t abs_pba, offset;
        int rc = meta_get_append_pos(v, rec_size, &abs_pba, &offset);
        if (rc != 0) return rc;
        /* WP52: never hand out a slot that runs past its extent. The
         * sizing in meta_get_append_pos must already guarantee this; the
         * check turns a silent cross-extent write (the owner-record
         * overflow class) into a clean, latched failure instead. */
        {
            uint64_t entry = meta_mapper_get(v, (size_t)v->met0.active_extent);
            uint64_t esz = entry ? invfs_meta_ext_size(entry) : 0;
            uint64_t epba = entry ? invfs_meta_ext_pba(entry) : 0;
            if (!entry || abs_pba != epba * INVFS_BLOCK_SIZE ||
                offset > esz || rec_size > esz - offset) {
                fprintf(stderr, "vol_append_slot: %llu-byte record does not "
                        "fit its metadata extent (off=%llu size=%llu); "
                        "refusing\n",
                        (unsigned long long)rec_size,
                        (unsigned long long)offset,
                        (unsigned long long)esz);
                return -1;
            }
        }
        *rec_pos_out = abs_pba + offset;
        v->met0.active_offset = offset + rec_size;
        /* bump the cursor so concurrent appends stay ahead of us */
        v->inode_area_pos = *rec_pos_out + rec_size;
        return 0;
    }
    *rec_pos_out = v->inode_area_pos;
    return 0;
}


/* WP52: append slot for the large owner records. On a mapper volume the
 * record gets its own dedicated, size-classed extent (meta_get_owner_append_pos)
 * and the shared file-record cursor is left untouched -- so the owner's
 * per-flush rewrite neither overflows an extent nor drags the record stream
 * into a flush storm. On a legacy volume this is the same contiguous-cursor
 * append as vol_append_slot. Never bumps inode_area_pos on a mapper volume:
 * that cursor is the file-record tail the walker trims the active extent at,
 * and the owner record no longer lives in the active extent. rc 0 = ok,
 * -1 = error, -2 = ENOSPC. */
int vol_append_owner_slot(invfs_volume *v, uint64_t rec_size,
                          uint64_t *ext_slot, uint64_t *rec_pos_out)
{
    if (v->met0_present && v->meta_mapper) {
        uint64_t pba = 0, offset = 0;
        int rc = meta_get_owner_append_pos(v, rec_size, ext_slot,
                                           &pba, &offset);
        if (rc != 0) return rc;
        *rec_pos_out = pba + offset;
        return 0;
    }
    *rec_pos_out = v->inode_area_pos;
    return 0;
}


/* raw byte-range read at absolute volume offset (for tools) */
int vol_read_raw(invfs_volume *v, uint64_t offset, void *buf, size_t len)
{
    if (io_seek(&v->io, offset) != 0 || io_read(&v->io, buf, len) != 0)
        return -1;
    return 0;
}


/* free n physical blocks (clear bitmap bits), bounded by volume size */
/* ENOSPC policy helpers */
int vol_write_enabled(invfs_volume *v)
{
    /* WP-M6: a v3 volume has no v2 record stream; the M5 backstop keeps
     * needs_recovery set for the whole session (vol_open sets it
     * unconditionally), so it cannot be the gate here -- an inherited DIRTY
     * state is replayed at open and the namespace must be writable through
     * FUSE. Only the READONLY latch applies.
     * WP-M19: a degraded v3 mount (dev0 absent) is read-only by
     * construction too -- it is serving from the dev1 metadata mirror.
     * WP80/A: io_latched is different from needs_recovery -- it means a
     * flush/sync barrier failed THIS session, so the append tail is past
     * unpersisted bytes. The v3 namespace must refuse mutations then just
     * like v2, or a latched volume keeps appending into the hole. */
    if (v->sb.vol_flags & VOLF_V3)
        return !(v->sb.vol_flags & VOLF_READONLY) && !v->degraded &&
               !v->io_latched;
    /* A volume awaiting recovery is read-only for the same reason a
       READONLY-flagged one is: the callers that check this are the ones that
       would otherwise append records, and appending onto maps that were never
       finished is how a single crash becomes two. */
    if (v->needs_recovery) return 0;
    /* WP25: a degraded mount (dev0 absent) is read-only by construction */
    if (v->degraded) return 0;
    return !(v->sb.vol_flags & VOLF_READONLY);
}


/* flip the READONLY flag; persists on next vol_flush (caller flushes).
 * An operator hold (ro=1) is VOLF_READONLY ALONE -- never auto-released
 * (vol_readonly_unlatch only releases the space latch, VOLF_RO_SPACE).
 * A manual release (ro=0) clears both bits: it overrides either hold. */
void vol_set_readonly(invfs_volume *v, int ro)
{
    if (ro)
        v->sb.vol_flags |= VOLF_READONLY;
    else
        v->sb.vol_flags &= ~(VOLF_READONLY | VOLF_RO_SPACE);
}

/* H5: the hard_min READONLY latch has a return path. alloc_blocks sets
 * VOLF_READONLY|VOLF_RO_SPACE when the free count hits the floor; before
 * WP22a nothing ever cleared the flag (vol_set_readonly(v, 0) had zero
 * callers), so a volume that once touched the floor stayed read-only
 * forever -- including across remounts (the flag persists in the
 * superblock) and even after deletes freed half the volume. A SPACE latch
 * now auto-releases with hysteresis: when free space climbs back above
 * hard_min + 2% of the volume, both bits drop (logged; the next
 * vol_flush's superblock write persists it). The band keeps a workload
 * hovering at the trigger from flapping the flag. An operator hold
 * (VOLF_READONLY alone) is never auto-released. Runs from vol_free_blocks
 * (every real free), from the fsck bitmap rebuild, and at vol_open (a
 * volume freed while offline opens RW). */
void vol_readonly_unlatch(invfs_volume *v)
{
    uint64_t watermark = (uint64_t)v->sb.hard_min_blocks +
                         v->sb.total_blocks / 50;
    if ((v->sb.vol_flags & (VOLF_READONLY | VOLF_RO_SPACE)) ==
            (VOLF_READONLY | VOLF_RO_SPACE) &&
        v->free_blocks > watermark) {
        v->sb.vol_flags &= ~(VOLF_READONLY | VOLF_RO_SPACE);
        fprintf(stderr, "[alloc] RW again: free=%llu above hard_min+2%% "
                "(hard_min=%u); READONLY latch released\n",
                (unsigned long long)v->free_blocks,
                (unsigned)v->sb.hard_min_blocks);
    }
}

uint64_t vol_free_blocks_cached(invfs_volume *v)
{
    return v->free_blocks;
}


/* blocks that ordinary writes must leave untouched (reserve + floor) */
uint64_t vol_write_guard(invfs_volume *v)
{
    return (uint64_t)v->sb.reserved_blocks + v->sb.hard_min_blocks;
}


/* The real free of one CONTIGUOUS, wholly-unpinned run: clear the bits, give
 * the space back to its zone, mark the seal dirty, release the ENOSPC latch.
 * vol_free_blocks below is the entry point and the only place a block can be
 * freed; it splits a pinned run into its unpinned parts and calls this once
 * per part. Keeping the body in its own function (rather than recursing on
 * vol_free_blocks) is what makes that split non-recursive and the free
 * counters exact. */
static void vol_free_run(invfs_volume *v, uint64_t pba, uint64_t nblocks)
{
    uint64_t i;
    uint64_t end = pba + nblocks;
    if (end > v->sb.total_blocks)
        end = v->sb.total_blocks;
    /* WP25: a REAL free (retention above holds its blocks) drops the
     * redundant second copy: the dev1 mirror of a raw-zone run, or the
     * dev0 acceleration copy of a canonical dev1-shadow run. */
    if (v->ndev == 2)
        wp25_on_free(v, pba, end - pba);
    for (i = pba; i < end; i++)
        bit_clr(v->bitmap, i);
    if (end > pba) { vol_bm_dirty(v, pba); vol_bm_dirty(v, end - 1); }
    /* WP20b: a freed shadow block changes its stripes' membership */
    seal_dirty_mark(v, pba, end - pba);
    v->free_blocks += nblocks;

    /* Give the freed space back to the zone it came from, and drop the
     * "no run this long exists" hint: a fresh hole may satisfy a request
     * that the last full scan rejected. Rewind the cursor so the scan
     * actually reaches the hole instead of walking past it. */
    if (pba >= v->sb.shadow_zone_start) {
        v->shadow_free += nblocks;
        v->shadow_fail_run = 0;
        if (pba < v->shadow_cursor) v->shadow_cursor = pba;
    } else if (v->ndev == 2 && pba >= v->dev0_blocks &&
               pba < v->sb.shadow_zone_start) {
        /* WP25 reserved dev1 span (metadata mirror): never allocated,
         * never freed -- defensive classification only */
    } else if (v->ndev == 2 && pba >= v->arena_start &&
               pba < v->dev0_blocks) {
        v->arena_free += nblocks;
        v->arena_fail_run = 0;
        if (pba < v->arena_cursor) v->arena_cursor = pba;
    } else if (pba >= v->sb.raw_zone_start) {
        v->raw_free += nblocks;
        v->raw_fail_run = 0;
        if (pba < v->raw_cursor) v->raw_cursor = pba;
    }
    /* H5: freeing is the way out of the space latch -- re-evaluate */
    vol_readonly_unlatch(v);
}

void vol_free_blocks(invfs_volume *v, uint64_t pba, uint64_t nblocks)
{
    uint64_t i;
    uint64_t end = pba + nblocks;
    if (end > v->sb.total_blocks)
        end = v->sb.total_blocks;
    /* WP96: the v3 save point's DATA pin, the one and only hold a v3 volume
     * has (WP21's coarse checkpoint and its retention registry went with the
     * v2 metadata machinery). It keys on the on-disk pin (v->spn_armed,
     * loaded at open) rather than on this session having armed anything: the
     * sweep that armed the window is not the only writer on the volume, and a
     * FUSE write's retire path, a delete, or a later
     * process' sweep must all see the hold. Sitting HERE rather than at the
     * publishers that replace recipes is the point -- vol_v3_free_recipe_
     * blocks has four v3 call sites and the drain frees directly, and a
     * missing one is exactly the silent corruption this refuses: a rollback
     * republishing a recipe over a block somebody else now owns. The blocks
     * stay ALLOCATED (that is what bars reuse) and the next save-point
     * capture's reclaim pass gives them back once no live recipe names
     * them, so the hold costs one generation, not the volume. */
    if (!v->retain_release && spt0_block_pinned(v, pba, end - pba))
        return;
    /* WP96, continued: the per-block split of that run.
     * PER BLOCK, not per run. A held block must not hold its neighbours: the
     * reclaim pass can only discharge the PIN's own debt (it walks the
     * previous mark set), so a block that is unpinned but got dragged along by
     * a pinned neighbour is in NO mark set and NOTHING will ever free it. That
     * is a structural leak, so the run is split and only the unpinned parts
     * are freed; the pinned blocks stay ALLOCATED (that is what bars their
     * reuse) and the next save-point capture's reclaim pass gives them back
     * once no live recipe names them, so the hold costs one generation, not
     * the volume.
     *
     * HONEST SCOPE: on today's publishers (whole recipe extents, all-or-
     * nothing) this split changes no measured behaviour -- the four free-path
     * suites and the 8-sweep pin-cost run are identical with and without it.
     * It is HARDENING against a mixed-extent free, not a measured fix; the
     * measured leak in this WP was spn_reclaim's (src/core/vol_spt0.c), whose
     * own "N blocks reclaimed" log line is what hid it. Keeping it costs one
     * bitmap probe per block on the rare path where a run IS pinned, and it
     * keeps the invariant local to the one choke point: "a block is freed iff
     * no live save point names it". */
    if (v->retain_release || !spt0_block_pinned(v, pba, end - pba)) {
        vol_free_run(v, pba, end - pba);
        return;
    }
    {
        uint64_t run = 0;
        for (i = pba; i < end; i++) {
            if (!spt0_block_pinned(v, i, 1)) { run++; continue; }
            if (run) { vol_free_run(v, i - run, run); run = 0; }
        }
        if (run) vol_free_run(v, end - run, run);
    }
}



uint64_t vol_count_free(invfs_volume *v)
{
    uint64_t i, free = 0;
    for (i = 0; i < v->sb.total_blocks; i++)
        if (!bit_get(v->bitmap, i))
            free++;
    return free;
}

int vol_zone_free(invfs_volume *v, uint64_t *raw_free, uint64_t *raw_total,
                  uint64_t *shadow_free, uint64_t *shadow_total)
{
    if (!v) return -1;
    if (raw_free)    *raw_free = v->raw_free;
    if (raw_total)   *raw_total = v->sb.raw_zone_blocks;
    if (shadow_free) *shadow_free = v->shadow_free;
    if (shadow_total) *shadow_total = v->sb.shadow_zone_blocks;
    return 0;
}


/* Free blocks inside one zone. Called once per mount (and after fsck
 * rewrites the bitmap) to seed the per-zone counters that alloc_blocks
 * then maintains incrementally. */
static uint64_t zone_count_free(invfs_volume *v, uint64_t start, uint64_t len)
{
    uint64_t i, free = 0;
    for (i = start; i < start + len && i < v->sb.total_blocks; i++)
        if (!bit_get(v->bitmap, i))
            free++;
    return free;
}

void alloc_state_reset(invfs_volume *v)
{
    v->raw_cursor    = v->sb.raw_zone_start;
    v->shadow_cursor = v->sb.shadow_zone_start;
    v->raw_free    = zone_count_free(v, v->sb.raw_zone_start,
                                    v->sb.raw_zone_blocks);
    v->shadow_free = zone_count_free(v, v->sb.shadow_zone_start,
                                    v->sb.shadow_zone_blocks);
    /* WP25: the dev0 tier arena (0 blocks on single-device volumes) */
    v->arena_cursor = v->arena_start;
    v->arena_free = v->arena_blocks
                  ? zone_count_free(v, v->arena_start, v->arena_blocks) : 0;
    v->arena_fail_run = 0;
    v->raw_fail_run = v->shadow_fail_run = 0;
    /* WP52 Bug: meta_free_blocks/meta_type_bitmap were declared and mutated
     * (v->meta_free_blocks -= n in alloc_blocks, extend_meta_extent) but
     * NEVER initialized or allocated anywhere in the tree. meta_free_blocks
     * therefore started at 0 and underflowed on the first META allocation
     * (observed 18446744073709238208); meta_type_bitmap stayed NULL, so the
     * per-block META/DATA classification was silently a no-op. The counter
     * has no consumer today, so the only visible effect was the absur+
     * value; initialize both here so the metadata-accounting fields mean
     * what their comments say. Initializing meta_free_blocks is also what
     * keeps the deferred flush watermark path (vol_should_flush) from a
     * bogus value once metadata accounting is ever wired into a decision. */
    {
        uint64_t meta_lo = (uint64_t)v->sb.metadata_zone_start;
        uint64_t meta_hi = meta_lo + v->sb.metadata_zone_blocks;
        if (meta_hi > v->sb.total_blocks) meta_hi = v->sb.total_blocks;
        if (!v->meta_type_bitmap && v->bitmap)
            v->meta_type_bitmap = (uint8_t *)calloc(
                1, (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE);
        v->meta_free_blocks = (meta_hi > meta_lo)
                            ? zone_count_free(v, meta_lo, meta_hi - meta_lo) : 0;
    }
    v->bm_lo = 1; v->bm_hi = 0;   /* on-disk bitmap matches memory */
    if (getenv("INVFS_DEBUG"))
        fprintf(stderr, "[alloc_state_reset] raw_free=%llu/%llu shadow_free=%llu/%llu\n",
                (unsigned long long)v->raw_free, (unsigned long long)v->sb.raw_zone_blocks,
                (unsigned long long)v->shadow_free, (unsigned long long)v->sb.shadow_zone_blocks);
    if (getenv("INVFS_DEBUG_FILE")) {
        FILE *df = fopen(getenv("INVFS_DEBUG_FILE"), "a");
        if (df) { fprintf(df, "[alloc_state_reset] raw_free=%llu/%llu shadow_free=%llu/%llu\n",
                    (unsigned long long)v->raw_free, (unsigned long long)v->sb.raw_zone_blocks,
                    (unsigned long long)v->shadow_free, (unsigned long long)v->sb.shadow_zone_blocks);
                  fclose(df); }
    }
}
