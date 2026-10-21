/* vol_storage.h — WP651 slice 1: the STORAGE domain of a mounted volume.
 *
 * This header owns the two-device / metadata-mirror state that used to sit
 * in the open `struct invfs_volume` (volume_internal.h). It is the first of
 * the six per-domain headers the WP splits the shared struct into; the rule
 * it implements is WP651 §Design 1: the storage state is visible to exactly
 * one domain — its owner — and everyone else calls an accessor.
 *
 * THE OWNER is volume.c: the device MUX (vmux_*), the two-device open legs
 * (wp25_open_dev1 / wp25_open_degraded), the mirror resync, the DEVT writer
 * and the flush commit tail all live there, and they are the only code that
 * mutates this state. It reads and writes `v->st.*` directly.
 *
 * EVERYONE ELSE (src/core/vol_*.c and beyond) calls the accessors below and
 * never names a field. There is deliberately no getter for state nobody
 * outside the owner asks about — devt_present, the resync pair, meta_end,
 * the mux cursor, raw_mirror, path2 and the two one-shot log latches are
 * owner-private in practice, so WP651 rule 2 (no speculative getters) leaves
 * them accessor-less. Adding one needs a caller in the same change.
 *
 * NOT moved by this slice (still in the open struct, and still reachable
 * from every TU): `io`, `io2`, `io_open[]`, `ndev`, `dev0_present`. io2 and
 * io_open have readers in vol_cpack.c / vol_fold.c and in the src/cli test
 * binaries, so moving them needs those include/call-site edits; they are the
 * next storage-domain increment, not this one. The bitmap cursors and the
 * block-pool allocator state named in the WP651 slice table are likewise a
 * later increment — this slice is the DEVT/mirror half of "devices".
 *
 * No behaviour change: every field below is the same field it was, with its
 * comment, at the same lifecycle point. The only semantic content added is
 * vol_st_devt_commit(), which encodes an invariant the two open legs and the
 * resize apply each used to hand-maintain (WP651 rule 3).
 */
#ifndef INVFS_VOL_STORAGE_H
#define INVFS_VOL_STORAGE_H

#include <stdint.h>

#include "invarifs.h"   /* invfs_devt, INVFS_DEVTF_RAW_MIRROR */

struct invfs_volume;

/* ---- WP25: two-device volumes (DEVT descriptor, invarifs.h) ----
 * v->io is device 0; v->io2 is device 1 (valid when ndev==2 and
 * dev0_present... io_open[] tracks which blkio_open calls succeeded
 * so vmux_close never closes a half-initialized backend. */
typedef struct invfs_vol_storage {
    char    *path2;          /* dev1 path as opened (diagnostics) */
    int      dev_skip[2];    /* session exclusion after a write/barrier
                              * failure on that device (next open resyncs) */
    int      degraded;       /* read-only degraded mount (dev0 absent) */
    invfs_devt devt;         /* the device table as read/written */
    int      devt_present;   /* a valid DEVT was read at open */
    int      resync_pending; /* open found a stale mirror (seq mismatch) */
    int      resync_winner;  /* device index holding the newest metadata */
    uint64_t dev0_blocks;    /* dev0 size in blocks (== sb.total_blocks
                              * when ndev < 2) */
    uint64_t dev1_blocks;
    uint64_t meta_end_bytes; /* mirrored metadata span [0, this) */
    uint64_t mux_pos;        /* virtual cursor behind io_seek/io_read */
    int      raw_mirror;     /* DEVTF_RAW_MIRROR */
    int      rawio_logged;   /* one-shot "dev0 raw write failed" log */
    int      metaread_logged;/* one-shot metadata failover log */
} invfs_vol_storage;

/* ---- accessor surface (implemented in volume.c, the domain owner) ----
 * Every declaration here has a caller outside volume.c; see the comment at
 * the top of this file for why the rest of the state has none. */

/* The mount is read-only because dev0 is absent: metadata came from the dev1
 * mirror, every live block is readable from dev1, and writes fail loudly.
 * volume.h's vol_degraded() is the public spelling of the same question;
 * this one is for core modules that already hold the volume. NULL-safe:
 * a NULL volume is not degraded, it is nothing. */
int vol_st_degraded(const struct invfs_volume *v);

/* dev0's size in blocks. On a single-device volume that is sb.total_blocks;
 * on a two-device volume it is the DEVT's dev_blocks[0], and it is the
 * address the mux routes on: global pbas below it live on dev0, at and above
 * it on dev1 at (pba - dev0_blocks). Callers that compute a dev1-relative
 * offset or a dev0 capacity bound need this, not the superblock. */
uint64_t vol_st_dev0_blocks(const struct invfs_volume *v);

/* 1 when device `dev` (0 or 1) is session-skipped: a write or barrier on it
 * failed since this mount, so the mux will not offer it again and the next
 * open resyncs it. A skipped dev0 also means its tier-arena acceleration
 * copies are unreachable, which is what the read path asks about. An
 * out-of-range index reads as "not skipped" rather than indexing past the
 * pair. */
int vol_st_dev_skipped(const struct invfs_volume *v, int dev);

/* The device table as last read or written, for a caller that has to reason
 * about the geometry (dev_blocks, sync_seq, dev1_hint). The pointer is the
 * owner's storage: read it, do not write through it, and do not keep it
 * across a call that can rewrite the table (vol_write_devt, the resize
 * apply). NULL only for a NULL volume. */
const invfs_devt *vol_st_devt(const struct invfs_volume *v);

/* Adopt a rewritten device table, and keep the cached per-device block
 * counts agreeing with it in the same step. THE INVARIANT (WP651 rule 3):
 * dev1_blocks is a copy of devt.dev_blocks[1], so a table change that moved
 * that field and left the copy behind would make the two disagree for the
 * rest of the mount — which is how a resize that grew the tail device would
 * keep routing on the old span. The caller owns the CRC and the on-disk
 * write; this only adopts what the caller proved. dev0_blocks is NOT
 * touched: the DEVT's dev_blocks[0] is fixed by mkfs and the resize that
 * calls this grows dev1 only. */
void vol_st_devt_commit(struct invfs_volume *v, const invfs_devt *dt);

#endif /* INVFS_VOL_STORAGE_H */
