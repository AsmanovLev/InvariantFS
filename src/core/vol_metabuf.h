/* vol_metabuf.h — WP-M2: v3 metadata base-page format + block allocator.
 *
 * The v3 stable tier is built from fixed-size base pages addressed by
 * invfs_blkptr (design-meta-v3.md §12). This module owns exactly three
 * things and nothing else:
 *
 *   1. the page wire format — serialize/parse + CRC32C over a 4 KiB page,
 *      with the header frozen by WP-M1 in invarifs.h (D3: page == block);
 *   2. the metadata block allocator — a bootstrap cursor over the two
 *      root-area pages WP-M1 reserves, then free-space allocation/reclaim
 *      through the existing bitmap allocator;
 *   3. the double-slot root publish — write the new base root into the
 *      RT30 root_slot[] and bump seq (higher gen + valid CRC wins on read,
 *      the l2p_replay idiom).
 *
 * Entry encoding, tree traversal, delta, fold and fsck validation are
 * explicitly WP-M3/WP-M4 and live elsewhere.
 */
#ifndef INVFS_VOL_METABUF_H
#define INVFS_VOL_METABUF_H

#include <stdint.h>
#include <stddef.h>

#include "invarifs.h"

/* volume.h's include guard closes at line 339 while the file runs to 675,
 * so a second inclusion re-declares the post-guard structs (GCC: "conflicting
 * types for invfs_seal_report"). WP-M2's translation units already get
 * volume.h through volume_internal.h, so forward-declare the type here
 * instead of including volume.h again. TODO(WP-M2): the stale guard is a
 * pre-existing volume.h bug; fixing it is outside this WP's file scope. */
typedef struct invfs_volume invfs_volume;

/* invfs_blkptr.flags values (WP-M1 left the field reserved; WP-M2 defines
 * it). The page's own level field lives in invfs_page_hdr.level. */
#define INVFS_BP_LEAF      0x00000001u
#define INVFS_BP_INTERNAL  0x00000002u
#define INVFS_BP_ROOT      0x00000004u
#define INVFS_BP_PINNED    0x00000008u

/* Root-area pages WP-M1 reserves immediately after the mapper table
 * (mkfs.c: "root_pba = mapper_pba + INVFS_META_EXT_BLOCKS", two pages
 * zeroed and made durable before RT30). The bootstrap allocator walks
 * these before touching the bitmap. */
#define INVFS_MBUF_BOOT_PAGES 2u

/* ---- page accessors (all operate on a full 4 KiB page buffer) --------- */

/* CRC32C over the page with invfs_page_hdr.checksum read as zero (the
 * RDP0/CKP0 convention). */
uint32_t mbuf_page_crc(const uint8_t *page);
/* Build an empty (nentries=0) valid page header. The checksum is left 0;
 * mbuf_page_seal / mbuf_write fills it. */
void     mbuf_page_init(uint8_t *page, uint16_t level, uint64_t gen);
/* Compute and store the page checksum. */
void     mbuf_page_seal(uint8_t *page);
/* 1 = magic "BPG3" and checksum match; 0 otherwise (torn/foreign page). */
int      mbuf_page_validate(const uint8_t *page);
invfs_page_hdr       *mbuf_page_hdr(uint8_t *page);
const invfs_page_hdr *mbuf_page_chdr(const uint8_t *page);

/* ---- page IO ---------------------------------------------------------- */

/* Read the page at pba (page_size bytes). 0 = ok, -1 = io error. Bounds
 * are checked against the volume geometry. */
int mbuf_read(invfs_volume *v, uint64_t pba, uint8_t *page_out);
/* CRC-seal `page` and write it at pba. Does NOT barrier (the caller
 * decides, per the design's structure-before-reference ordering). */
int mbuf_write(invfs_volume *v, uint64_t pba, uint8_t *page);

/* Fill `p` from a sealed page: pba, the page's gen, the page's checksum.
 * flags is caller-supplied (INVFS_BP_*). */
void mbuf_ptr_set(invfs_blkptr *p, uint64_t pba, const uint8_t *page,
                  uint32_t flags);

/* Read the page a blkptr names and verify it against the pointer's
 * gen + checksum (the design's "bad CRC is a hard read error"). 0 = ok. */
int mbuf_read_ptr(invfs_volume *v, const invfs_blkptr *p, uint8_t *page_out);
/* Same verification, discarding the page bytes. */
int mbuf_verify_ptr(invfs_volume *v, const invfs_blkptr *p);

/* ---- allocator -------------------------------------------------------- */

/* Initialise the bootstrap + free-space cursors from the volume geometry
 * and RT30. Idempotent; safe to call after alloc_state_reset. */
void mbuf_init(invfs_volume *v);
/* Allocate one 4 KiB metadata page, written as a valid empty leaf at
 * generation `gen`. Returns the pba, or 0 on ENOSPC/io error. `gen` is
 * uint64 to match invfs_blkptr.gen (the WP doc sketched u16 before WP-M1
 * froze the 64-bit field; truncating would break gen comparison). */
uint64_t mbuf_alloc(invfs_volume *v, uint64_t gen);
/* Return a metadata page to the free pool. */
void mbuf_free(invfs_volume *v, uint64_t pba);
/* The effective page size; D3 fixes 4 KiB == INVFS_BLOCK_SIZE. */
uint32_t mbuf_page_size(const invfs_volume *v);

/* ---- RT30 root-area descriptor ---------------------------------------- */

/* Read block 0's RT30 at INVFS_RT30_OFF, validate version + CRC, and
 * persist it on the volume (v->rt30 / v->rt30_present). 0 = present and
 * valid, 1 = no descriptor at all (a base that was never written, the RDP0
 * convention), 2 = WP86: a descriptor that is NAMED but fails version/CRC --
 * damage, which must never be presented as an empty root -- -1 = io error. */
int mbuf_rt30_load(invfs_volume *v);
/* Recompute the descriptor CRC and write it back to block 0. 0 = ok. */
int mbuf_rt30_store(invfs_volume *v);

/* Publish `root_pba` as the current base root. The page at root_pba must
 * already be durable and carry header gen `root_gen`; this helper refuses
 * otherwise. Alternates root_slot[seq & 1], bumps seq, stores RT30 and
 * barriers. 0 = ok. */
int mbuf_root_publish(invfs_volume *v, uint64_t root_pba, uint64_t root_gen);
/* Select the live root: read both root slots, keep the one whose page
 * validates and carries the higher header gen (tie -> seq parity). 0 = ok
 * (pba_out/gen_out filled), 1 = no root named at all (empty base),
 * -1 = WP86: damage -- the descriptor is torn, or RT30 names a root page
 * that does not validate. The caller must turn -1 into a hard error (EIO),
 * never into "absent": a torn root page used to present the whole volume as
 * an empty filesystem. */
int mbuf_root_read(invfs_volume *v, uint64_t *root_pba_out,
                   uint64_t *root_gen_out);

#endif /* INVFS_VOL_METABUF_H */
