/* vol_seal.c — the WP20 shadow-zone XOR parity seal and the WP20b layer-2
 * RS seal. Split from volume.c.
 *
 * The seal itself is GONE. All three of its moving parts were format-v2
 * structures:
 *
 *   - the parity bitmap was rebuilt by scanning v->l2p, the owner-scoped
 *     WAL journal;
 *   - the stripe -> parity-pba map was recovered from L2P MAP entries;
 *   - each shard was persisted as a hidden "\x01parityN" inode record
 *     (tz_owner_load -> meta_read_record_by_id -> invfs_inode_rec).
 *
 * With the journal and the inode-record stream retired, vol_seal had
 * nothing left to seal. It was already refusing on this format with a
 * diagnostic -- it did so at the top of the function, precisely so that a
 * seal could not half-run -- and what is gone is the body behind that
 * refusal, which no volume can reach.
 *
 * What remains is the dirty-stripe bookkeeping, because it is still
 * maintained: the allocator marks freed and written shadow blocks through
 * seal_dirty_mark() so that a NEXT reseal knows which stripes changed. It
 * is small and format-independent, and deleting it would mean touching the
 * allocator for no gain -- if parity sealing is implemented on this format
 * it will want exactly this bitmap.
 *
 * `invf-sweep --seal` / `--unseal` therefore still refuse, and still say
 * why. `invf-verify --deep` still asks whether the volume is sealed and is
 * told it is not, which is the truth: nothing can be sealed.
 */

#include "volume_internal.h"


/* (Re)Allocate the dirty bitmap and mark every shadow block: the state
 * before that moment is simply not tracked, so the next reseal must be a
 * full pass. */
void seal_dirty_reset(invfs_volume *v)
{
    size_t bytes = (size_t)((v->sb.shadow_zone_blocks + 7) / 8);
    if (!v->seal_dirty)
        v->seal_dirty = (uint8_t *)malloc(bytes ? bytes : 1);
    if (v->seal_dirty)
        memset(v->seal_dirty, 0xFF, bytes);
}


/* Mark the shadow-zone blocks [pba, pba+n) dirty (their stripes need a
 * parity recompute at the next reseal). No-op outside the shadow zone or
 * when no seal config exists (the bitmap is NULL then). */
void seal_dirty_mark(invfs_volume *v, uint64_t pba, uint64_t n)
{
    uint64_t ss = v->sb.shadow_zone_start;
    uint64_t b, end;
    if (!v->seal_dirty || !n) return;
    if (pba + n <= ss || pba >= ss + v->sb.shadow_zone_blocks) return;
    if (pba < ss) { n -= ss - pba; pba = ss; }
    end = pba + n;
    if (end > ss + v->sb.shadow_zone_blocks)
        end = ss + v->sb.shadow_zone_blocks;
    for (b = pba; b < end; b++)
        bit_set(v->seal_dirty, b - ss);
}


/* ---- the retired entry points ------------------------------------- */

/* Called by seg_read_checked when a shadow-zone segment fails its framing
 * CRC. There is no parity to recompute a stripe syndrome from, so the read
 * fails the way it did before any seal existed: loudly, with the EIO the
 * caller turns into a read error. */
int seal_recover_segment(invfs_volume *v, uint64_t pba, uint64_t plen,
                         uint32_t *csize_out, uint8_t **blob_out)
{
    (void)v; (void)pba; (void)plen;
    (void)csize_out; (void)blob_out;
    return -1;
}


int vol_seal(invfs_volume *v, int unseal, invfs_seal_report *rep)
{
    if (!v || !rep) return -1;
    memset(rep, 0, sizeof *rep);
    fprintf(stderr, "seal: parity sealing is which has no parity seal. "
                    "The implementation that existed was format-v2 "
                    "machinery -- it rebuilt the parity bitmap by scanning "
                    "the L2P journal and persisted each shard as a hidden "
                    "\"\\x01parityN\" inode record -- and it was removed with "
                    "the format. This is not a partial seal that could be "
                    "finished; there is no code left to finish. This build "
                    "reads format v%d only, which has no parity seal.\n", INVFS_FORMAT_VERSION);
    (void)unseal;
    return -1;
}


/* Nothing can be sealed, so nothing is sealed: all-zero counters. That is
 * also what this returned before the v3 gate, because the L2P scan it
 * started from is always empty here. */
int vol_seal_verify(invfs_volume *v, invfs_seal_verify *out)
{
    if (!v || !out) return -1;
    memset(out, 0, sizeof *out);
    return 0;
}


/* No seal configuration exists on this format; the accessor answers "no"
 * and the setter refuses rather than accepting settings nothing honours. */
int vol_redun_state(const invfs_volume *v, uint32_t *k1, int *l2_algo,
                    uint32_t *m2)
{
    (void)v;
    if (k1) *k1 = 0;
    if (l2_algo) *l2_algo = 0;
    if (m2) *m2 = 0;
    return 0;
}

void vol_redun_config(invfs_volume *v, uint32_t k1, int l2_algo, uint32_t m2)
{
    (void)v; (void)k1; (void)l2_algo; (void)m2;
    fprintf(stderr, "seal: --k1/--l2/--m2 are retired with the v2 parity "
                    "seal; they configure nothing on this build\n");
}
