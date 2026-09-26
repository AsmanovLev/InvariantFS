/* vol_spt0.c — WP-M16: v3 save point (SPT0 descriptor).
 *
 * Implements save point capture, rollback-to-savepoint, and drop.
 * See vol_spt0.h for the API contract.
 */

#include "volume_internal.h"
#include "vol_spt0.h"
#include "vol_btree.h"
#include "vol_delta.h"
#include "vol_metabuf.h"

#include <string.h>

static uint32_t spt0_crc(const invfs_spt0 *s)
{
    invfs_spt0 t = *s;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}

int spt0_load(invfs_volume *v)
{
    invfs_spt0 s;
    if (!v)
        return -1;
    v->savepoint_live = 0;
    memset(&v->spt0, 0, sizeof v->spt0);
    memset(&v->pinned_root, 0, sizeof v->pinned_root);

    if (io_pread(&v->io, INVFS_SPT0_OFF, &s, sizeof s) != 0)
        return -1;
    if (memcmp(s.magic, "SPT0", 4) != 0 ||
        s.version != INVFS_SPT0_VERSION ||
        spt0_crc(&s) != s.crc32c)
        return 1;

    v->spt0 = s;
    v->savepoint_live = 1;
    v->pinned_root.pba = s.base_root;
    v->pinned_root.checksum = 0;
    v->pinned_root.gen = 0;
    v->pinned_root.flags = 0;
    return 0;
}

int spt0_store(invfs_volume *v)
{
    invfs_spt0 s;
    if (!v)
        return -1;
    s = v->spt0;
    s.crc32c = 0;
    s.crc32c = spt0_crc(&s);
    if (io_pwrite(&v->io, INVFS_SPT0_OFF, &s, sizeof s) != 0)
        return -1;
    return 0;
}

/* WP86: a save point is only useful if the base it pins can be walked. The
 * page CRC of the root alone is not enough -- a torn page deeper in the tree
 * makes the pinned root unusable while every check on the root page passes.
 * spt0_tree_ok() walks the whole subtree, so neither capture nor restore can
 * pin (or roll back onto) a damaged base; the caller gets a reason instead of
 * a save point that would hand back an unreadable volume. */
static int spt0_tree_ok(invfs_volume *v, uint64_t pba, char *err, size_t errlen)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    invfs_blkptr root;
    char e[128];

    if (mbuf_read(v, pba, page) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "root page %llu is unreadable",
                     (unsigned long long)pba);
        return 0;
    }
    if (!mbuf_page_validate(page)) {
        if (err && errlen)
            snprintf(err, errlen, "root page %llu fails its CRC",
                     (unsigned long long)pba);
        return 0;
    }
    mbuf_ptr_set(&root, pba, page,
                 mbuf_page_chdr(page)->level == INVFS_PAGE_LEVEL_LEAF
                 ? INVFS_BP_ROOT | INVFS_BP_LEAF
                 : INVFS_BP_ROOT | INVFS_BP_INTERNAL);
    e[0] = 0;
    if (btree_check(v, root, NULL, e, sizeof e) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "%s", e[0] ? e : "base tree is invalid");
        return 0;
    }
    return 1;
}

int spt0_capture(invfs_volume *v)
{
    invfs_blkptr root;

    if (!v)
        return -1;
    if (!(v->sb.vol_flags & VOLF_V3))
        return 1;
    if (v->savepoint_live)
        return 1;

    if (vol_v3_base_root(v, &root) != 0)
        return -1;

    /* WP86: refuse to pin a damaged base. */
    if (!spt0_tree_ok(v, root.pba, NULL, 0))
        return SPT0_RC_DAMAGED;

    memset(&v->spt0, 0, sizeof v->spt0);
    memcpy(v->spt0.magic, "SPT0", 4);
    v->spt0.version = INVFS_SPT0_VERSION;
    v->spt0.flags = 0;
    v->spt0.base_root = root.pba;
    v->spt0.delta_end = 0;

    {
        uint64_t delta_bytes = 0;
        uint64_t cur = v->delta_seg_pba;
        uint64_t chain_bytes[DELTA_MAX_SEGMENTS];
        size_t nsegs = 0;
        invfs_delta_seg_hdr hdr;

        while (cur && nsegs < DELTA_MAX_SEGMENTS) {
            chain_bytes[nsegs++] = delta_bytes;
            if (delta_read_hdr(v, cur, &hdr) != 0)
                break;
            if (nsegs > 1 || v->delta_bump > INVFS_DELTA_SEG_HDR_LEN) {
                delta_bytes += (nsegs == 1) ? v->delta_bump : INVFS_DELTA_SEG_BYTES;
            }
            if (hdr.prev_pba == cur)
                break;
            cur = hdr.prev_pba;
        }
        v->spt0.delta_end = delta_bytes;
    }

    v->pinned_root.pba = v->spt0.base_root;
    v->pinned_root.checksum = 0;
    v->pinned_root.gen = 0;
    v->pinned_root.flags = 0;
    v->savepoint_live = 1;

    if (spt0_store(v) != 0)
        return -1;
    if (vmux_barrier(v, "spt0 capture") < 0)
        return -1;

    return 0;
}

int spt0_restore(invfs_volume *v)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    invfs_page_hdr *h;
    uint64_t base_root;
    int rc;

    if (!v)
        return -1;
    if (!(v->sb.vol_flags & VOLF_V3))
        return 1;
    if (!v->savepoint_live)
        return 1;

    /* WP85: this is the point where the post-savepoint generation stops
     * being live. Every write session opened before this line is anchored
     * to metadata the restore just retired -- and to segments no live
     * recipe names, because SPT0 pins {base_root, delta_end} only and a
     * session's segments live in neither (WP27: they ride the session's
     * own entry table, never the journal). Bump the counter BEFORE the
     * first write so no session can slip an append through the window;
     * the sessions themselves are refused and retired by vol_write_*. */
    v->write_gen++;

    base_root = v->spt0.base_root;

    /* WP86: DETECT a damaged save point, do not use it. Publishing the pinned
     * root when its tree is torn would replace a live (if damaged) volume
     * with one whose every read fails, and would truncate the delta for a
     * state that was never reachable. */
    if (!spt0_tree_ok(v, base_root, NULL, 0)) {
        fprintf(stderr, "invf-spt0: save point base_root %llu is damaged -- "
                "refusing to roll back onto it; run invf-fsck -f to quarantine "
                "the unreadable pages first\n",
                (unsigned long long)base_root);
        return SPT0_RC_DAMAGED;
    }
    if (mbuf_read(v, base_root, page) != 0)
        return -1;
    h = mbuf_page_hdr(page);
    if (!mbuf_page_validate(page))
        return SPT0_RC_DAMAGED;

    if (mbuf_root_publish(v, base_root, h->gen) != 0)
        return -1;

    if (vol_delta_truncate(v, v->spt0.delta_end) != 0)
        return -1;

    vol_delta_close(v);
    if (vol_delta_mount(v) != 0)
        return -1;

    memset(&v->spt0, 0, sizeof v->spt0);
    v->savepoint_live = 0;
    memset(&v->pinned_root, 0, sizeof v->pinned_root);

    rc = spt0_store(v);
    if (rc != 0)
        return rc;
    if (vmux_barrier(v, "spt0 restore") < 0)
        return -1;

    return 0;
}

int spt0_drop(invfs_volume *v)
{
    int was_live;

    if (!v)
        return -1;
    was_live = v->savepoint_live;

    v->savepoint_live = 0;
    memset(&v->pinned_root, 0, sizeof v->pinned_root);
    memset(&v->spt0, 0, sizeof v->spt0);

    if (spt0_store(v) != 0)
        return -1;
    if (vmux_barrier(v, "spt0 drop") < 0)
        return -1;

    return was_live ? 1 : 0;
}

int spt0_info(const invfs_volume *v, invfs_spt0 *out)
{
    if (!v)
        return 0;
    if (out)
        *out = v->spt0;
    return v->savepoint_live;
}
