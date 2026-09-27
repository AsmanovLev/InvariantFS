/* vol_metabuf.c — WP-M2: v3 metadata base-page format + block allocator.
 *
 * See vol_metabuf.h for the contract. Three concerns, in file order:
 *   1. page wire format / CRC,
 *   2. page + blkptr IO,
 *   3. metadata allocator + RT30 double-slot root publication.
 *
 * Durability rule (design §12 / WP-M2 Crash ordering): mbuf_write writes
 * and seals a page but never barriers; the caller barriers COW pages
 * before publishing a pointer to them, and mbuf_root_publish barriers
 * RT30 after the root page is already durable. A page whose CRC does not
 * match is a hard read error -- WP-M2 never repairs a torn page in place.
 */

#include "volume_internal.h"
#include "vol_metabuf.h"

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* 1. page wire format                                                */
/* ------------------------------------------------------------------ */

invfs_page_hdr *mbuf_page_hdr(uint8_t *page)
{
    return (invfs_page_hdr *)page;
}

const invfs_page_hdr *mbuf_page_chdr(const uint8_t *page)
{
    return (const invfs_page_hdr *)page;
}

/* CRC over the page with hdr.checksum read as zero. Computing it as the
 * concatenation of the bytes before and after the field avoids a 4 KiB
 * copy; invfs_crc32c_update over [0,off) then [off+4, page) is exactly
 * invfs_crc32c over the zeroed page. */
uint32_t mbuf_page_crc(const uint8_t *page)
{
    const size_t off = offsetof(invfs_page_hdr, checksum);
    uint32_t c = invfs_crc32c(page, off);
    return invfs_crc32c_update(c, page + off + sizeof(uint32_t),
                              (size_t)INVFS_BLOCK_SIZE - off - sizeof(uint32_t));
}

void mbuf_page_init(uint8_t *page, uint16_t level, uint64_t gen)
{
    invfs_page_hdr *h = mbuf_page_hdr(page);
    memset(page, 0, INVFS_BLOCK_SIZE);
    memcpy(h->magic, INVFS_PAGE_MAGIC, 4);
    h->gen = gen;
    h->level = level;
    h->nentries = 0;
    h->checksum = 0;
}

void mbuf_page_seal(uint8_t *page)
{
    mbuf_page_hdr(page)->checksum = mbuf_page_crc(page);
}

int mbuf_page_validate(const uint8_t *page)
{
    const invfs_page_hdr *h = mbuf_page_chdr(page);
    if (memcmp(h->magic, INVFS_PAGE_MAGIC, 4) != 0)
        return 0;
    return h->checksum == mbuf_page_crc(page);
}

/* WP123: is `pba` an ALLOCATED block according to the volume's allocation
 * bitmap? This is a SEPARATE question from mbuf_page_validate, and until
 * now nothing asked it.
 *
 * Page integrity answers "are these bytes a whole BPG3 page". Allocation
 * answers "does the volume still own this block". A page that has been
 * handed back to the free pool but not yet re-issued answers YES to the
 * first and NO to the second, because free does not scrub the block: the
 * magic and the CRC32C of whatever was last written there are both still
 * perfect. Anything that treats "validates" as "usable" is therefore one
 * allocator round away from adopting a page it does not own.
 *
 * The authority is v->bitmap, which vol_open populates from the metadata
 * zone BEFORE mbuf_rt30_load runs (volume.c:1463 vs :1545), so a root read
 * at open consults the same bytes the descriptor was written against.
 *
 * A NULL bitmap is NOT a "free" answer. It means this volume has no
 * allocation authority to consult at all (synthetic test volumes that
 * never went through vol_open); refusing there would be inventing damage
 * out of missing information, so we report "cannot tell" (-1) and let the
 * caller's integrity check stand. vol_open allocates v->bitmap
 * unconditionally, so this branch is unreachable for any real volume. */
int mbuf_page_allocated(const invfs_volume *v, uint64_t pba)
{
    if (!v || !v->bitmap)
        return -1;                          /* no authority: cannot tell */
    if (pba == 0 || pba >= v->sb.total_blocks)
        return 0;                           /* out of range: not allocated */
    return bit_get(v->bitmap, pba) ? 1 : 0;
}

uint32_t mbuf_page_size(const invfs_volume *v)
{
    /* D3: base pages are 4 KiB and map 1:1 to blocks. The RT30 page_size
     * field exists so a later 16 KiB variant is a mount-parsed change; the
     * allocator/IO here implement only the frozen 4 KiB size and refuse
     * anything else rather than mis-address the bitmap. TODO(WP-M2): a
     * 16 KiB variant needs a sub-block scheme and is out of this WP. */
    (void)v;
    return INVFS_V3_PAGE_SIZE_DEFAULT;
}

/* ------------------------------------------------------------------ */
/* 2. page + blkptr IO                                                */
/* ------------------------------------------------------------------ */

int mbuf_read(invfs_volume *v, uint64_t pba, uint8_t *page_out)
{
    if (!v || !page_out)
        return -1;
    if (mbuf_page_size(v) != INVFS_BLOCK_SIZE)
        return -1;
    if (pba == 0 || pba >= v->sb.total_blocks)
        return -1;
    if (io_pread(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE, page_out, INVFS_BLOCK_SIZE) != 0)
        return -1;
    return 0;
}

int mbuf_write(invfs_volume *v, uint64_t pba, uint8_t *page)
{
    if (!v || !page)
        return -1;
    if (mbuf_page_size(v) != INVFS_BLOCK_SIZE)
        return -1;
    if (pba == 0 || pba >= v->sb.total_blocks)
        return -1;
    mbuf_page_seal(page);
    if (io_pwrite(&v->io, pba * (uint64_t)INVFS_BLOCK_SIZE, page, INVFS_BLOCK_SIZE) != 0)
        return -1;
    return 0;
}

void mbuf_ptr_set(invfs_blkptr *p, uint64_t pba, const uint8_t *page,
                  uint32_t flags)
{
    const invfs_page_hdr *h = mbuf_page_chdr(page);
    p->pba = pba;
    p->checksum = h->checksum;
    p->gen = h->gen;
    p->flags = flags;
}

int mbuf_read_ptr(invfs_volume *v, const invfs_blkptr *p, uint8_t *page_out)
{
    invfs_page_hdr *h;
    if (!p || !page_out || p->pba == 0)
        return -1;
    if (mbuf_read(v, p->pba, page_out) != 0)
        return -1;
    h = mbuf_page_hdr(page_out);
    if (h->checksum != p->checksum || h->gen != p->gen)
        return -1;
    if (!mbuf_page_validate(page_out))
        return -1;
    return 0;
}

int mbuf_verify_ptr(invfs_volume *v, const invfs_blkptr *p)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    return mbuf_read_ptr(v, p, page);
}

/* ------------------------------------------------------------------ */
/* 3. allocator                                                       */
/* ------------------------------------------------------------------ */

/* bm_dirty is static in volume.c; the metadata allocator hand-marks only
 * the metadata zone's own bits, so it carries the same two-line range
 * widening here rather than exporting the private helper. */
static void mb_bm_dirty(invfs_volume *v, uint64_t i)
{
    uint64_t byte = i / 8;
    if (v->bm_lo > v->bm_hi) { v->bm_lo = byte; v->bm_hi = byte + 1; return; }
    if (byte < v->bm_lo) v->bm_lo = byte;
    if (byte + 1 > v->bm_hi) v->bm_hi = byte + 1;
}

void mbuf_init(invfs_volume *v)
{
    uint64_t start;
    if (!v)
        return;
    v->mb_boot_cursor = 0;
    v->mb_boot_end = 0;
    v->mb_alloc_cursor = v->sb.metadata_zone_start;
    v->mb_alloc_fail_run = 0;
    if (v->rt30_present && v->rt30.page_size != INVFS_BLOCK_SIZE) {
        fprintf(stderr, "vol_metabuf: RT30 page_size=%u is not the supported "
                "4 KiB (D3); metadata allocator disabled\n",
                (unsigned)v->rt30.page_size);
        v->mb_alloc_cursor = 0;
        return;
    }
    /* Bootstrap: WP-M1 reserved INVFS_MBUF_BOOT_PAGES pages immediately
     * after the mapper table and marked them allocated (they sit inside
     * the mkfs-allocated metadata zone). Hand those out before the bitmap
     * scan; they need no allocation bookkeeping. */
    if (v->sb.meta_mapper_pba && v->sb.meta_mapper_blocks) {
        start = v->sb.meta_mapper_pba + v->sb.meta_mapper_blocks;
        v->mb_boot_cursor = start;
        v->mb_boot_end = start + INVFS_MBUF_BOOT_PAGES;
    }
}

/* Primary free-space pool: the metadata zone itself, allocation-bookkept
 * against meta_free_blocks. TODO(WP-M2): current mkfs marks the whole
 * metadata zone allocated (bitmap+journal+inode area), so in practice
 * this finds nothing and mbuf_alloc falls through to the shared pool.
 * Keeping the scan here means a future v3 mkfs that leaves the base-page
 * region free is picked up without touching alloc_blocks' zone counters
 * (which have no metadata branch); the free path is symmetric in
 * mbuf_free. */
static uint64_t mb_alloc_meta_zone(invfs_volume *v)
{
    uint64_t start = v->sb.metadata_zone_start;
    uint64_t end = start + v->sb.metadata_zone_blocks;
    uint64_t i, n;
    if (!v->bitmap || !start || !v->sb.metadata_zone_blocks)
        return 0;
    if (start >= v->sb.total_blocks)
        return 0;
    if (end > v->sb.total_blocks)
        end = v->sb.total_blocks;
    i = v->mb_alloc_cursor;
    if (i < start || i >= end)
        i = start;
    for (n = 0; n < end - start; n++) {
        if (!bit_get(v->bitmap, i)) {
            bit_set(v->bitmap, i);
            if (v->meta_type_bitmap)
                bit_set(v->meta_type_bitmap, i);
            mb_bm_dirty(v, i);
            v->free_blocks--;
            if (v->meta_free_blocks)
                v->meta_free_blocks--;
            v->mb_alloc_cursor = (i + 1 < end) ? i + 1 : start;
            v->mb_alloc_fail_run = 0;
            return i;
        }
        i = (i + 1 < end) ? i + 1 : start;
    }
    v->mb_alloc_fail_run = 1;
    return 0;
}

static int mb_write_empty(invfs_volume *v, uint64_t pba, uint64_t gen)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    mbuf_page_init(page, INVFS_PAGE_LEVEL_LEAF, gen);
    return mbuf_write(v, pba, page);
}

uint64_t mbuf_alloc(invfs_volume *v, uint64_t gen)
{
    uint64_t pba;
    if (!v)
        return 0;
    if (v->rt30_present && v->rt30.page_size != INVFS_BLOCK_SIZE)
        return 0;

    if (v->mb_boot_cursor && v->mb_boot_cursor < v->mb_boot_end) {
        pba = v->mb_boot_cursor;
        if (mb_write_empty(v, pba, gen) != 0)
            return 0;
        v->mb_boot_cursor++;
        return pba;
    }

    pba = mb_alloc_meta_zone(v);
    if (!pba) {
        /* Shared free pool, tagged META: the same path WP30's dynamic
         * metadata extents use (alloc_blocks has no metadata-zone branch,
         * so routing the metadata pool through the shadow zone keeps the
         * per-zone free counters honest). TODO(WP-M2): a dedicated dev0
         * base pool lands with the WP that wires the v3 write path. */
        pba = alloc_blocks(v, v->sb.shadow_zone_start,
                           v->sb.shadow_zone_blocks, 1, 0, INVFS_ALLOC_META);
    }
    if (!pba)
        return 0;   /* ENOSPC, never abort */
    if (mb_write_empty(v, pba, gen) != 0) {
        vol_free_blocks(v, pba, 1);
        return 0;
    }
    return pba;
}

void mbuf_free(invfs_volume *v, uint64_t pba)
{
    if (!v || pba == 0)
        return;
    vol_free_blocks(v, pba, 1);
    /* vol_free_blocks has no metadata-zone branch (metadata pbas sit below
     * raw_zone_start), so keep the metadata counters symmetric here and
     * rewind the cursor so the hole is found again. Mirror the retention
     * guard: while a checkpoint holds blocks, vol_free_blocks leaves the
     * bit set and the counters untouched, so this must too. */
    if (pba >= v->sb.metadata_zone_start &&
        pba < v->sb.metadata_zone_start + v->sb.metadata_zone_blocks &&
        !((v->retain || v->ck_present) && !v->retain_release)) {
        v->meta_free_blocks++;
        v->mb_alloc_fail_run = 0;
        if (pba < v->mb_alloc_cursor && v->mb_alloc_cursor != 0)
            v->mb_alloc_cursor = pba;
    }
}

/* ------------------------------------------------------------------ */
/* RT30 root-area descriptor + double-slot publish                    */
/* ------------------------------------------------------------------ */

int mbuf_rt30_load(invfs_volume *v)
{
    invfs_rt30 rt;
    if (!v)
        return -1;
    v->rt30_present = 0;
    memset(&v->rt30, 0, sizeof v->rt30);
    if (io_pread(&v->io, INVFS_RT30_OFF, &rt, sizeof rt) != 0)
        return -1;
    if (memcmp(rt.magic, "RT30", 4) != 0)
        return 1;   /* no descriptor at all: an empty base (RDP0 rule) */
    if (rt.version != INVFS_RT30_VERSION ||
        invfs_crc32c(&rt, offsetof(invfs_rt30, crc32c)) != rt.crc32c)
        /* WP86: a descriptor that is NAMED but does not validate is damage,
         * not absence. Answering 1 here would present the whole namespace as
         * empty -- every lookup would return "absent" and the operator would
         * see a filesystem with no files in it. 2 = present but torn. */
        return 2;
    v->rt30 = rt;
    v->rt30_present = 1;
    return 0;
}

int mbuf_rt30_store(invfs_volume *v)
{
    if (!v)
        return -1;
    v->rt30.crc32c = invfs_crc32c(&v->rt30, offsetof(invfs_rt30, crc32c));
    if (io_pwrite(&v->io, INVFS_RT30_OFF, &v->rt30, sizeof v->rt30) != 0)
        return -1;
    return 0;
}

int mbuf_root_publish(invfs_volume *v, uint64_t root_pba, uint64_t root_gen)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    invfs_page_hdr *h;
    uint32_t slot;

    if (!v || root_pba == 0)
        return -1;
    if (!v->rt30_present) {
        memset(&v->rt30, 0, sizeof v->rt30);
        memcpy(v->rt30.magic, "RT30", 4);
        v->rt30.version = INVFS_RT30_VERSION;
        v->rt30.page_size = INVFS_V3_PAGE_SIZE_DEFAULT;
        v->rt30_present = 1;
    }
    /* The pointer is only published once its page is durable (caller
     * barriers COW pages first), so verify the page here: a torn or
     * gen-mismatched root is refused, not published. */
    if (mbuf_read(v, root_pba, page) != 0)
        return -1;
    h = mbuf_page_hdr(page);
    if (!mbuf_page_validate(page) || h->gen != root_gen)
        return -1;

    /* WP123: structure-before-reference, enforced HERE rather than by
     * caller discipline. The reader's new allocation check
     * (mbuf_root_read -> mbuf_page_allocated) is only sound if the
     * allocation bitmap is durable at the moment the descriptor becomes
     * durable -- otherwise a crash could leave RT30 naming a page whose
     * allocation bit is still clear on disk, and the very next open would
     * refuse a live root. That is the failure mode this must never have, so
     * the ordering that prevents it cannot live in four separate callers
     * that a fifth caller may forget. (All four existing call sites --
     * v3_publish, vol_v3_fold, the fsck repair and spt0_restore -- already
     * do this; re-flushing an already-clean range is a no-op, so this is
     * belt-and-braces, not duplicated I/O.) */
    if (vol_v3_bitmap_flush(v) != 0)
        return -1;

    slot = (uint32_t)(v->rt30.seq & 1u);
    v->rt30.root_slot[slot] = root_pba;
    v->rt30.seq++;
    /* One barrier covers the bitmap and the descriptor together: the
     * descriptor must never become durable before the allocation it names. */
    if (mbuf_rt30_store(v) != 0)
        return -1;
    return vmux_barrier(v, "rt30 root publish") < 0 ? -1 : 0;
}

int mbuf_root_read(invfs_volume *v, uint64_t *root_pba_out,
                   uint64_t *root_gen_out)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    uint64_t best_pba = 0, best_gen = 0;
    int have = 0, named = 0, i;
    /* WP123: per-slot verdicts, kept so the failure message can tell the
     * two damage modes apart. They are different faults with different
     * remedies -- a torn page is a write that did not land, a freed page is
     * a reclaim that freed a live root -- and a reader that collapses them
     * into "unusable" cannot report either. */
    int slot_alloc[2] = { -1, -1 };
    int slot_valid[2] = { 0, 0 };

    if (!v)
        return -1;
    if (!v->rt30_present) {
        int rc = mbuf_rt30_load(v);
        if (rc < 0)
            return -1;
        if (rc == 1)
            return 1;   /* no descriptor: the base has never been written */
        if (rc == 2)
            return -1;  /* WP86: torn descriptor -- see mbuf_rt30_load */
    }
    for (i = 0; i < 2; i++) {
        uint64_t pba = v->rt30.root_slot[i];
        invfs_page_hdr *h;
        if (!pba)
            continue;
        named = 1;
        /* WP123: the allocation question comes FIRST, and it is
         * independent of the page's contents. A freed page is a freed page
         * whether it still validates, has been scribbled over, or is
         * unreadable -- so asking the bitmap first means a freed slot can
         * never be adopted on the strength of whatever bytes happen to be
         * sitting there now. mbuf_page_allocated returns -1 only when the
         * volume carries no bitmap at all (synthetic volumes); that is
         * "no authority", not "freed", and falls through to the integrity
         * check alone. */
        slot_alloc[i] = mbuf_page_allocated(v, pba);
        if (slot_alloc[i] == 0)
            continue;
        if (mbuf_read(v, pba, page) != 0)
            continue;      /* `named` already records that a slot pointed
                             * somewhere; WP86 made the read-failure case
                             * collapse into the "named but unusable" one */
        h = mbuf_page_hdr(page);
        if (!mbuf_page_validate(page))
            continue;
        slot_valid[i] = 1;
        /* Higher page gen wins; on a tie prefer the slot the current seq
         * parity points at (the most recently published one). */
        if (!have || h->gen > best_gen ||
            (h->gen == best_gen &&
             (uint32_t)i == (uint32_t)(v->rt30.seq & 1u))) {
            have = 1;
            best_pba = pba;
            best_gen = h->gen;
        }
    }
    if (!have) {
        /* WP86: RT30 NAMES a root but no named page validates. That is
         * damage, not an empty tree: answering 1 made the entire namespace
         * read as absent (a torn root page hid every file on the volume).
         * The caller turns -1 into EIO, so a key the delta still holds stays
         * readable and everything else fails loudly.
         *
         * WP123: and it is silent data loss rather than an absence, because
         * a freed page VALIDATES. Report the per-slot verdicts, naming the
         * slot the current seq parity calls newest, so the operator can
         * tell "the newest root is torn" from "the newest root was freed
         * and the volume is standing on a block the allocator may re-issue"
         * without re-deriving it from a hex dump. */
        for (i = 0; i < 2; i++) {
            uint64_t pba = v->rt30.root_slot[i];
            const char *why;
            if (!pba)
                continue;
            if (slot_alloc[i] == 0)
                why = "FREED: the allocation bitmap reports this block as "
                      "free, so the page is no longer the volume's -- its "
                      "bytes still pass the CRC only because free does not "
                      "scrub";
            else if (slot_alloc[i] == 1)
                why = slot_valid[i] ? "unusable"
                                    : "TORN: the page is allocated but its "
                                      "magic or CRC32C does not validate";
            else
                why = "unusable: unreadable, or out of range";
            fprintf(stderr,
                    "vol: RT30 root_slot[%d] (pba %llu, %s per seq %llu) "
                    "is not usable: %s\n",
                    i, (unsigned long long)pba,
                    ((uint32_t)i == (uint32_t)(v->rt30.seq & 1u))
                        ? "the newer slot" : "the older slot",
                    (unsigned long long)v->rt30.seq, why);
        }
        /* The read failing above also sets `named`, so this is just "a slot
         * names a page". */
        return named ? -1 : 1;
    }
    if (root_pba_out)
        *root_pba_out = best_pba;
    if (root_gen_out)
        *root_gen_out = best_gen;
    return 0;
}
