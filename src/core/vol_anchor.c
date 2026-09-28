/* vol_anchor.c — the ANC0 tail anchor. See invarifs.h for why the anchor is
 * where it is, and vol_anchor.h for the three rules this file enforces.
 *
 * The shape of the whole thing is deliberately small:
 *
 *   anchor_pba()      a POSITION. total_blocks - 1. No read of block 0, so
 *                     a volume whose block 0 is dead can still be found.
 *   anchor_probe()    decides whether that position is OURS, once per open.
 *                     Everything else is gated on the answer.
 *   anchor_refresh()  rewrites the mirror whenever a source descriptor goes
 *                     stale, and does nothing at all on a volume with no
 *                     anchor -- which is the pre-change world, where the
 *                     tail block is somebody's data.
 *   anchor_invalidate_at()  the resize commit's answer: total_blocks moved,
 *                     so the anchor's address moved with it, and the old
 *                     address is either a dead address or (on a grow) an
 *                     interior block that must be given back.
 */

#include <stdio.h>
#include <string.h>

#include "volume_internal.h"
#include "vol_anchor.h"
#include "vol_metabuf.h"
#include "vol_spt0.h"
#include "blkio.h"

uint32_t anchor_crc(const invfs_anc0 *a)
{
    invfs_anc0 t = *a;
    t.crc32c = 0;
    return invfs_crc32c(&t, offsetof(invfs_anc0, crc32c));
}

int anchor_fp_matches(const invfs_anc0 *a, uint64_t total_blocks,
                      uint32_t block_size, uint32_t format_version,
                      const uint8_t *uuid)
{
    if (a->total_blocks != total_blocks)
        return 0;
    if (a->block_size != block_size)
        return 0;
    if (a->format_version != format_version)
        return 0;
    /* The uuid arm is the one that catches a same-size re-mkfs: every
     * geometry field is identical there, and the old anchor's bytes are
     * still sitting in the tail block because mkfs does not wipe the tail.
     * Without this arm a fresh empty volume would be told it has a root. */
    if (uuid && memcmp(a->vol_uuid, uuid, 16) != 0)
        return 0;
    return 1;
}

int anchor_state_of(const invfs_anc0 *a)
{
    if (memcmp(a->magic, INVFS_ANCHOR_MAGIC, 4) != 0)
        return INVFS_ANCHOR_ABSENT;
    if (a->version != INVFS_ANC0_VERSION || anchor_crc(a) != a->crc32c)
        return INVFS_ANCHOR_REFUSED_DAMAGE;
    return INVFS_ANCHOR_OK;
}

const char *anchor_state_name(int state)
{
    switch (state) {
    case INVFS_ANCHOR_OK:               return "ok";
    case INVFS_ANCHOR_ABSENT:           return "absent";
    case INVFS_ANCHOR_REFUSED_GEOMETRY: return "refused-geometry";
    case INVFS_ANCHOR_REFUSED_DAMAGE:   return "refused-damage";
    default:                            return "io-error";
    }
}

uint64_t anchor_pba(const invfs_volume *v)
{
    if (!v || !v->sb.total_blocks)
        return 0;
    /* total_blocks - 1 is the LAST block. It is block 0 only on a one-block
     * volume, which cannot hold a superblock and a bitmap anyway; refuse it
     * rather than anchor the anchor onto block 0 itself. */
    if (v->sb.total_blocks < 2)
        return 0;
    return v->sb.total_blocks - 1;
}

void anchor_build(const invfs_volume *v, invfs_anc0 *out)
{
    memset(out, 0, sizeof *out);
    memcpy(out->magic, INVFS_ANCHOR_MAGIC, 4);
    out->version = INVFS_ANC0_VERSION;
    out->total_blocks = v->sb.total_blocks;
    out->block_size = INVFS_BLOCK_SIZE;
    out->format_version = v->sb.format_version;
    memcpy(out->vol_uuid, v->sb.uuid, 16);
    /* Verbatim copies, CRC fields included: the mirror is the descriptors as
     * they were when they were last STORED, and mbuf_rt30_store /
     * spt0_store have already stamped them by the time they call us. */
    out->rt30 = v->rt30;
    out->spt0 = v->spt0;
    out->crc32c = 0;
    out->crc32c = anchor_crc(out);
}

int anchor_write_raw(void *io_, uint64_t pba, const invfs_anc0 *a)
{
    blkio *io = (blkio *)io_;
    uint64_t off;

    if (!io || !pba)
        return -1;
    off = pba * (uint64_t)INVFS_BLOCK_SIZE;
    if (blkio_pwrite(io, off, a, sizeof *a) != 0)
        return -1;
    /* The mirror is worthless if it is still in the page cache when the
     * power goes, and it is the descriptor that has to survive precisely
     * when something else did not. */
    blkio_flush(io);
    return 0;
}

int anchor_probe(invfs_volume *v, invfs_anc0 *out)
{
    invfs_anc0 a;
    uint64_t pba;
    int st;

    if (!v)
        return INVFS_ANCHOR_IO;
    pba = anchor_pba(v);
    if (!pba) {
        v->anchor_state = INVFS_ANCHOR_ABSENT;
        v->anchor_adopted = 0;
        return v->anchor_state;
    }

    memset(&a, 0, sizeof a);
    if (io_pread(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE, &a, sizeof a) != 0) {
        v->anchor_state = INVFS_ANCHOR_IO;
        v->anchor_adopted = 0;
        return v->anchor_state;
    }
    st = anchor_state_of(&a);
    if (st != INVFS_ANCHOR_OK) {
        v->anchor_state = st;
        v->anchor_adopted = 0;
        if (st == INVFS_ANCHOR_REFUSED_DAMAGE)
            fprintf(stderr,
                    "vol_open: ANC0 tail anchor at block %llu is NAMED but "
                    "fails its own version/CRC -- damaged, and treated as no "
                    "anchor at all. The block-0 descriptors are the only "
                    "source in use.\n",
                    (unsigned long long)pba);
        return st;
    }
    if (!anchor_fp_matches(&a, v->sb.total_blocks, v->sb.block_size,
                           v->sb.format_version, (const uint8_t *)v->sb.uuid)) {
        /* Refuse loudly and specifically. A refusal that looked like "no
         * anchor" would be indistinguishable from a volume that never had
         * one, which is the one thing an operator most needs to tell apart
         * when they are deciding whether to restore a backup. */
        v->anchor_state = INVFS_ANCHOR_REFUSED_GEOMETRY;
        v->anchor_adopted = 0;
        fprintf(stderr,
                "vol_open: ANC0 tail anchor at block %llu REFUSED: it is not "
                "this volume's (anchor says total_blocks=%llu block_size=%u "
                "format_version=%u, this image says %llu/%u/%u) -- a resized, "
                "re-formatted or foreign image must not be told it has a "
                "root. The block-0 descriptors are the only source in use.\n",
                (unsigned long long)pba,
                (unsigned long long)a.total_blocks, a.block_size,
                a.format_version,
                (unsigned long long)v->sb.total_blocks, v->sb.block_size,
                v->sb.format_version);
        return v->anchor_state;
    }
    v->anchor_state = INVFS_ANCHOR_OK;
    v->anchor = a;
    if (out)
        *out = a;
    return v->anchor_state;
}

int anchor_refresh(invfs_volume *v)
{
    invfs_anc0 a;
    uint64_t pba;

    if (!v)
        return -1;
    /* RULE 1. The refresh runs only on a volume whose open probe found an
     * anchor with a matching fingerprint. Everywhere else -- every volume
     * made before this descriptor existed, whose last block is an ordinary
     * data block -- this returns without touching a single byte. */
    if (v->anchor_state != INVFS_ANCHOR_OK)
        return 0;
    pba = anchor_pba(v);
    if (!pba)
        return 0;
    anchor_build(v, &a);
    if (io_pwrite(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE, &a, sizeof a) != 0) {
        /* A deferral the volume cannot record must be said out loud
         * (the vol_cpack.c lesson). The primary descriptor store already
         * succeeded, so refusing to return -1 here would break the volume on
         * a device whose tail is failing -- but staying quiet would leave an
         * operator believing the volume has a second copy when it does not.
         * So: say it once, latch it, and let invf-fsck keep reporting it. */
        if (!v->anchor_refresh_failed) {
            fprintf(stderr,
                    "vol: ANC0 tail anchor refresh FAILED at block %llu: the "
                    "block-0 descriptor was stored but the volume now has NO "
                    "second copy of it. This volume is running unprotected "
                    "against a loss of block 0 until the tail writes again.\n",
                    (unsigned long long)pba);
        }
        v->anchor_refresh_failed = 1;
        return -1;
    }
    vmux_barrier(v, "anchor refresh");
    return 0;
}

int anchor_invalidate_at(invfs_volume *v, uint64_t old_pba, const invfs_anc0 *a)
{
    uint8_t zero[INVFS_BLOCK_SIZE];
    uint64_t off;

    if (!v || !old_pba)
        return 0;
    /* Only wipe a block that really held an anchor. A NULL descriptor means
     * the probe found nothing there, and zeroing a live data block on the
     * strength of a resize is precisely the failure this whole feature must
     * not introduce. */
    if (!a)
        return 0;
    memset(zero, 0, sizeof zero);
    off = old_pba * (uint64_t)INVFS_BLOCK_SIZE;
    if (io_pwrite(&v->io, off, zero, sizeof zero) != 0)
        return -1;
    vmux_barrier(v, "anchor invalidate");
    v->anchor_state = INVFS_ANCHOR_ABSENT;
    v->anchor_adopted = 0;
    return 0;
}

int anchor_adopted(const invfs_volume *v)
{
    return v ? v->anchor_adopted : 0;
}

int anchor_refresh_failed(const invfs_volume *v)
{
    return v ? v->anchor_refresh_failed : 0;
}

int anchor_block_protected(const invfs_volume *v, uint64_t pba)
{
    /* Same guard as anchor_pba: a NULL/unknown volume, or a volume with no
     * total_blocks, has no anchor to protect and must not lose a block. */
    if (!v || !v->sb.total_blocks)
        return 0;
    if (pba != anchor_pba(v))
        return 0;
    /* NOT gated on anchor_state. A pre-anchor volume also has its last block
     * outside every allocation it does today, but if a later change ever makes
     * a zone reach the tail, that block could hold user data -- and excluding
     * it is then the safe answer, not the surprising one. The cost of being
     * wrong in this direction is one unused block; the cost of being wrong in
     * the other direction is overwriting the file that holds a volume's only
     * recoverable root descriptor. */
    return 1;
}
