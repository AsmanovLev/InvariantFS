/*
 * invarifs.h — InvariantFS on-disk structures (from doc/02-on-disk-format.md,
 *              doc/13-linux-rootfs.md)
 *
 * Portable C99. MSVC + GCC compatible (no __attribute__).
 */
#ifndef INVARIFS_H
#define INVARIFS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* On-disk magic. Exactly 8 bytes, and it is the bytes that decide whether a
   volume opens at all -- so it stays "InvariFS" even though the project is now
   called InvariantFS ("InvariantFS" is 11 bytes and would not fit anyway).
   Changing it is a format break: every existing volume would stop mounting.
   If it ever moves, it moves in format v2 with a compatibility path. */
#define INVFS_MAGIC       "InvariFS\0"   /* 8 bytes */
#define INVFS_BLOCK_SIZE  4096
/* Format version 2 (WP27): AST block entries carry the physical block
 * address (24B -> 32B), the L2P journal shrinks to the owner-scoped WAL,
 * heat lives in the INO2 ext, and the bitmap is a rebuildable cache of
 * records+WAL. The on-disk signal is VOLF_ASTV2 in sb.vol_flags (outside
 * the checksum, the v0.9 policy-field convention); a v2 reader refuses a
 * volume without it, loudly, pointing at invf-migrate-v2 (no dual
 * readers). */
#define INVFS_VERSION     2
#define INVFS_JOURNAL_BLOCKS 8192  /* 32MB total: two 16MB journal slots (WP22d) */
#define INVFS_META_RESERVED_PCT_DFLT 10  /* WP30: default 10% free pool for metadata */

/* ---- WP22d: crash-atomic L2P journal (double-buffered slots) ----
 * The journal area is split into two equal slots of INVFS_JRN_SLOT_BLOCKS
 * each. A slot holds: one header block (invfs_jrn_hdr), then the compacted
 * image (the whole live L2P table, one MAP entry per key), then the
 * appended op log (MAP/UNMAP, newest wins). In slotted mode an entry's crc
 * is CHAINED: crc = crc32c_update(prev_crc, entry[0..offsetof(crc)]), the
 * first entry chaining from the header's crc32c -- a hole or a stale tail
 * left by a dropped write breaks the chain and stops the replay exactly
 * there (the bare-CRC terminator of the legacy format is subsumed). The
 * image additionally carries a whole-image CRC (image_crc): a torn
 * compaction invalidates the whole slot instead of replaying half a table.
 *
 * Writes are append-only within a slot: a flush appends the pending ops at
 * the log end and never rewrites durable entries (WP22d/F3: rewriting
 * already-durable positions is what let a drop window silently un-map
 * fsync-acknowledged files). When the log cannot hold the pending ops (or
 * fsck rebuilds the table, or legacy migrates) the whole table is imaged
 * into the INACTIVE slot, barriered, and only then the superblock selector
 * (pad2) is flipped and barriered again. Crash before the flip = old slot
 * intact; crash mid-flip = the selector (outside the sb checksum, so a
 * torn flip can read as garbage) is only a hint: replay picks the
 * highest-sequence CRC-valid slot, which is always the right one (an image
 * is a pure compaction of everything the older slot holds).
 *
 * Capacity: a slot's payload is (INVFS_JRN_SLOT_BLOCKS-1) blocks, so the
 * live L2P table may hold at most ~(4095*4096/36) = 465,920 mappings
 * (vol_map refuses past that). Pre-WP22d volumes carry pad2 == 0 and no
 * slot headers: they replay as the legacy flat log and migrate into slot
 * 1 on the first flush (slot 1, not 0: the flat log starts at the journal
 * base = slot 0's header block, so imaging slot 1 keeps the old log's
 * beginning intact as the fallback until the flip lands; a torn migration
 * is caught by image_crc and replay falls back to the flat log). mkfs
 * keeps writing legacy volumes -- born-legacy, migrated-on-first-flush,
 * so the migration path is exercised by every test. Old binaries mounting
 * a migrated volume replay the active slot's header as a (bad-CRC) entry
 * and see an empty journal -- downgrade was never a guaranteed direction;
 * the geometry (INVFS_JOURNAL_BLOCKS) is unchanged so at least the zone
 * layout stays intact for them.
 */
#define INVFS_JRN_SLOTS       2
#define INVFS_JRN_SLOT_BLOCKS (INVFS_JOURNAL_BLOCKS / INVFS_JRN_SLOTS)
#define INVFS_JRN_MAGIC   "JRN0"
#define INVFS_JRN_VERSION 1

/* sb.pad2 values (the journal slot selector; 0 on pre-WP22d volumes) */
#define INVFS_JSEL_LEGACY 0   /* no slots: the whole area is one flat log */
#define INVFS_JSEL_SLOT0  1
#define INVFS_JSEL_SLOT1  2

#pragma pack(push, 1)
typedef struct {
    char     magic[4];      /* "JRN0" */
    uint32_t version;       /* INVFS_JRN_VERSION */
    uint64_t seq;           /* compaction sequence, monotone per volume */
    uint64_t image_bytes;   /* compacted-image bytes behind the header block */
    uint32_t image_crc;     /* CRC32C over the image bytes (0 when empty) */
    uint32_t crc32c;        /* over the header with this field read as zero */
} invfs_jrn_hdr;            /* 28 bytes, occupies the slot's first block */
#pragma pack(pop)

/* Volume state */
#define INVFS_STATE_CLEAN    0xCA
#define INVFS_STATE_DIRTY    0xDA
#define INVFS_STATE_RECOVERY 0x52

/* Zone identifiers (AST block_entry.zone) */
#define INVFS_ZONE_RAW    0
#define INVFS_ZONE_TEXT   1
#define INVFS_ZONE_BINARY 2

/* Compression algorithms (AST block_entry.algo) */
#define INVFS_ALGO_NONE  0
#define INVFS_ALGO_ZSTD  1
#define INVFS_ALGO_PPMD  2
#define INVFS_ALGO_APE   3
#define INVFS_ALGO_JXL   4
#define INVFS_ALGO_LZ4   5
#define INVFS_ALGO_WV    6  /* WavPack */
#define INVFS_ALGO_FLACR 7  /* FLAC: APE(PCM) blob + sibling "name!recipe" */
#define INVFS_ALGO_TARR  8  /* TAR: recipe blob + sibling "name!recipe" + "name!partN" */
#define INVFS_ALGO_GZR   9  /* GZIP: deflate-replica recipe + "name!partN" (bit-exact) */
#define INVFS_ALGO_PNGR 10  /* PNG: JXL-lossless blob + "name!jxl" + IVPN recipe */
#define INVFS_ALGO_PMP  11  /* MP3: packMP3 blob (MPEG-1 Layer III only, bit-exact) */
#define INVFS_ALGO_RAWIMG 13 /* raw_image codecpack: DICOM/PNM/BMP/TIFF -> lossless JXL */
/* WP14a: a shared binary batch (zone=TEXT semantics = "batched") whose
 * members were x86-BCJ-prefiltered per member slice before concatenation.
 * Exists ONLY as an AST tag: there is deliberately no codec-registry entry
 * (BCJ is a pipeline stage in front of the batch's ZSTD frame, not a codec);
 * the batch payload wire format is [4B usize LE][one zstd stream], exactly
 * like an algo=ZSTD batch, and decode is ZSTD + per-member-slice BCJ inverse
 * at pc=0 (bijective -- applied to the same window it was encoded over). */
#define INVFS_ALGO_ZSTD_BCJ 14
/* WP14b M2: exe-as-container -- a binary-family file (ELF/PE/Mach-O) with
 * embedded media is carved: each embedded JPEG/PNG region (>= 16 KiB,
 * validated by a real marker/chunk walk, cap 64) becomes a "name!exrN"
 * sibling (JPEG -> lossless JXL blob, algo=JXL; PNG stays a ZSTD blob --
 * djxl emits a fresh PNG encoding, so no transcode can pass the bit-exact
 * guard while PNGR is Windows-only, WP12(c)), and the exe's own AST is a
 * single whole-file entry (zone=BINARY, algo=EXER, length = original size)
 * whose segment is ZSTD-19 of the recipe+glue payload:
 *   [4B "EXER"][u32 LE num_parts]
 *   per part: [u64 LE file_offset][u64 LE member_len][u8 codec=INVFS_ALGO_*]
 *   then the original bytes with the carved ranges REMOVED (glue).
 * Read = ZSTD-decompress, read each exrN sibling through the normal path,
 * splice at the offsets; the sweep stores nothing before a full in-memory
 * rebuild memcmps the original (the house 1:1 invariant). */
#define INVFS_ALGO_EXER 15

/* Storage-class flag (WP10): persisted as internal xattr "invfs.class" in the
 * INO2 ext block, value = invfs_class_tlv. Records WHY a file is stored the
 * way it is so sweep can skip/retry without re-deriving from content.
 * See impl_docs/old_docs/WP10-textzone-codec-registry.md §2. */
#define INVFS_XATTR_CLASS "invfs.class"

enum {
    INVFS_CLASS_UNCOMPRESSIBLE   = 1, /* gain < threshold; retry only if a NEWER
                                       * codec generation sniffs positive */
    INVFS_CLASS_CODEC            = 2, /* codec-specific (PMP/JXL/APE/WV) */
    INVFS_CLASS_CONTAINER        = 3, /* container decomposition (TARR/GZR/...) */
    INVFS_CLASS_GENERIC          = 4, /* generic per-segment ZSTD */
    INVFS_CLASS_GENERIC_MEMLIMIT = 5, /* codec rejected by dec_mem policy; retry
                                       * when the limit admits it */
    INVFS_CLASS_GENERIC_GUARD    = 6, /* codec guard refused; first retry
                                       * candidate when codec generation bumps */
    INVFS_CLASS_TEXT             = 7, /* PPMd batch member */
    INVFS_CLASS_BATCHED_BIN      = 8, /* binary batch member (WP14a): zone=TEXT,
                                       * algo = ZSTD (plain binary batch) or
                                       * ZSTD_BCJ (x86-prefiltered batch).
                                       * Same compliance/GC semantics as TEXT */
    INVFS_CLASS_DEFER_ENOSPC     = 9, /* sweep deferred for free space (WP16b):
                                       * the file waits RAW and is
                                       * re-evaluated EVERY sweep; stamped
                                       * {algo,gen} of the declining codec */
    INVFS_CLASS_ANCHORED         = 10 /* WP59a: pinned builtin-readable
                                       * (/.invariantfs/**).  Never assigned
                                       * a pack/container codec; skip dedup
                                       * remap and tier demotion. */
};

#pragma pack(push, 1)
typedef struct {
    uint8_t  cls;    /* INVFS_CLASS_* */
    uint8_t  algo;   /* rejecting/winning codec (INVFS_ALGO_*); 0 if n/a */
    uint16_t gen;    /* rejecting codec's generation; registry generation
                      * snapshot for UNCOMPRESSIBLE */
} invfs_class_tlv;   /* 4 bytes */
#pragma pack(pop)

/* L2P journal entry types */
#define INVFS_JRN_MAP       0x01
#define INVFS_JRN_UNMAP     0x02
#define INVFS_JRN_SWEEP     0x03
#define INVFS_JRN_CHECKPOINT 0xFF

/* WP30: metadata extent WAL record types (owner-scoped, like L2P).
 * WP-M21 removed META_SHRINK/FREE/MERGE (extent consolidation). */
#define INVFS_JRN_META_ALLOC  0x10
#define INVFS_JRN_META_EXTEND 0x11

/* Default zone fractions (of volume after superblock+metadata) */
#define INVFS_RAW_FRACTION_NUM 1   /* RAW = 20% */
#define INVFS_RAW_FRACTION_DEN 5

#pragma pack(push, 1)

/* Superblock — Block 0, 4096 bytes (128 used) */
typedef struct {
    char     magic[8];              /* 0x00 "InvariFS\0" (see INVFS_MAGIC) */
    uint8_t  uuid[16];              /* 0x08 volume UUID */
    uint32_t state;                 /* 0x18 clean/dirty/recovery */
    uint32_t block_size;            /* 0x1C 4096 */
    uint64_t total_blocks;          /* 0x20 */
    uint64_t metadata_zone_start;   /* 0x28 */
    uint64_t metadata_zone_blocks;  /* 0x30 */
    /* WP-DZ: the four zone fields are ADVISORY POLICY, not hard regions --
     * one shared free-block pool; raw-class allocation prefers the raw
     * extent and overflows into shadow-space blocks with the class tag
     * unchanged (zone=0). Seal stripes stay pinned to the shadow pba
     * extent. No format change: old volumes run with this geometry as the
     * initial policy state. */
    uint64_t raw_zone_start;        /* 0x38 */
    uint64_t raw_zone_blocks;       /* 0x40 */
    uint64_t shadow_zone_start;     /* 0x48 */
    uint64_t shadow_zone_blocks;    /* 0x50 */
    uint8_t  root_ast_hash[32];     /* 0x58 BLAKE3 of root AST dir */
    uint32_t sweep_cursor;          /* 0x78 last swept RAW block */
    uint32_t checksum;              /* 0x7C CRC32C of bytes 0..0x7B */
    /* ENOSPC policy (new in v0.9; outside checksum -> 0 on old images) */
    uint32_t reserved_blocks;       /* 0x80 reserve for sweep/transcodes */
    uint32_t hard_min_blocks;       /* 0x84 below this -> READONLY */
    uint32_t vol_flags;             /* 0x88 bit0 = VOLF_READONLY */
    uint32_t pad2;                  /* 0x8C unused (was journal slot selector) */
    uint8_t  format_version;       /* 0x90 format version: 0=legacy, 1=v0.3.0 */
    uint8_t  pad3[3];              /* 0x91 padding */
    /* WP30: dynamic metadata extents (outside checksum - 0 on old images) */
    uint32_t meta_reserved_pct;     /* 0x94 min free pool % for metadata */
    uint64_t meta_mapper_pba;       /* 0x98 pba of metadata mapper table (0=absent) */
    uint32_t meta_mapper_blocks;     /* 0xA0 blocks for mapper table */
    uint32_t meta_extent_min;       /* 0xA4 min extent size class (default 1=128KB) */
} invfs_superblock;                /* 0xA8 = 168 bytes */

/* volume flags (sb.vol_flags) */
#define VOLF_READONLY 0x00000001
#define VOLF_META2    0x00000002  /* records may carry "INO2" metadata ext */
/* WP27: format v2 -- AST entries are 32B and carry the segment pba (the
 * L2P journal is the owner-scoped WAL only). Set by mkfs from format v2 on
 * and by invf-convert on a converted v1 volume; a v2 reader refuses a
 * volume without it (no dual readers -- invf-migrate-v2 is the bridge).
 * Old binaries ignore the bit (it sits outside the superblock checksum)
 * and read the volume as-is, which fails loudly at the first 32B entry. */
#define VOLF_ASTV2    0x00000008
/* WP-M1: format v3 (metadata-v3 programme, design-meta-v3.md). A v3 volume
 * replaces the v2 append-only inode records + owner WAL with a two-tier
 * metadata store (immutable B+-tree base + append-only delta). VOLF_V3 is
 * the authoritative format marker: vol_open checks it BEFORE the VOLF_ASTV2
 * reader gate, because a v3 volume carries no v2 record stream. Lives
 * outside the superblock checksum, like every other policy flag. The v3
 * on-disk skeleton is the RT30 root-area descriptor (block 0, 0x9D0). */
#define VOLF_V3       0x00000010
/* WP22a/H5: WHY the volume is read-only. alloc_blocks raises VOLF_READONLY
 * together with VOLF_RO_SPACE when the free count hits hard_min (the space
 * latch); an operator/tool hold (vol_set_readonly) sets VOLF_READONLY alone.
 * The distinction is what auto-release keys on: only a space latch drops
 * itself when free space climbs back above hard_min + 2% (see
 * vol_readonly_unlatch); an operator hold survives opens, frees and fsck.
 * Pre-WP22a latched volumes carry VOLF_READONLY alone -- they keep the old
 * one-way semantics (never auto-released). Old binaries ignore the new bit
 * (they copy the superblock wholesale) and read the volume as plain
 * read-only, which is the safe answer either way. */
#define VOLF_RO_SPACE 0x00000004  /* READONLY came from the space latch */

/* ---- WP20b: RDP0 redundancy descriptor (block 0 reserved area) ----
 * Lives at byte offset 0x100 of block 0, past the 144-byte superblock
 * struct: vol_open and vol_write_sb only ever touch sizeof(invfs_superblock)
 * bytes at offset 0, and pre-WP20b images carry zeros there, which read as
 * "absent" (magic mismatch). The descriptor makes a configured redundancy
 * scheme findable at mount/bootstrap without scanning the inode area for
 * the "\x01parity*" owners, and persists the stripe geometry (k1/k2/m2)
 * that the parity blocks on disk were computed with.
 *
 *   0x100  char magic[4]        "RDP0"
 *   0x104  u8  l1_algo          0=off, 1=xor (WP20 stripe XOR)
 *   0x105  u8  l2_algo          0=off, 1=rs-vm, 2=rs-cauchy (rs.c)
 *   0x106  u16 k1               layer-1 stripe data blocks (8..128)
 *   0x108  u16 k2               layer-2 stripe data blocks (32)
 *   0x10A  u8  m2               layer-2 parity blocks per stripe (2..8)
 *   0x10B  u8  pad              0
 *   0x10C  u64 parity_area_hint live parity blocks (both layers) at write
 *   0x114  u32 crc32c           CRC32C over 0x100..0x117 -- the full
 *                               24-byte descriptor with this very field
 *                               read as zero
 * 24 bytes total; the rest of block 0 stays reserved-zero. */
#define INVFS_RDP0_OFF 0x100
#define INVFS_RDP0_L1_XOR       1
#define INVFS_RDP0_L2_RS_VM     1
#define INVFS_RDP0_L2_RS_CAUCHY 2
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* 0x100 "RDP0" */
    uint8_t  l1_algo;           /* 0x104 */
    uint8_t  l2_algo;           /* 0x105 */
    uint16_t k1;                /* 0x106 */
    uint16_t k2;                /* 0x108 */
    uint8_t  m2;                /* 0x10A */
    uint8_t  pad;               /* 0x10B */
    uint64_t parity_area_hint;  /* 0x10C */
    uint32_t crc32c;            /* 0x114 */
} invfs_rdp0;                   /* 0x118 = 24 bytes */
#pragma pack(pop)

/* ---- WP18: RSZ0 resize descriptor (block 0 reserved area) ----
 * Lives at byte offset 0x140 of block 0, past the 144-byte superblock and
 * the RDP0 descriptor at 0x100; pre-WP18 images carry zeros there, which
 * read as "absent" (magic mismatch) -- the RDP0 convention.
 *
 * invf-resize can never move the metadata payload (bitmap + journal + inode
 * records) atomically: the bitmap grows into the journal head whenever
 * total_blocks crosses a 32768-block boundary, so the rewrite overlaps and
 * destroys the copy it replaces. The resize therefore runs in two phases:
 * first the whole payload is copied into a collision-free staging area and
 * made durable, then this descriptor and a RECOVERY superblock state are
 * written ("armed"); only then may the payload be rewritten at its
 * post-resize location, from the staging copy alone. A crash before arming
 * leaves the old volume byte-identical (auto-recovery clears RECOVERY); a
 * crash after re-enters the apply here at vol_open -- it is idempotent
 * because it reads only the staging area, never the regions being
 * overwritten. The commit (new superblock + cleared descriptor, one
 * block-0 rewrite) is the last write.
 *
 *   0x140  char magic[4]             "RSZ0"
 *   0x144  u32 version               1
 *   0x148  u64 stage_start           first staging block
 *   0x150  u64 stage_blocks          staging span, blocks
 *   0x158  u64 bm_bytes              staged bitmap bytes
 *   0x160  u64 j_bytes               staged journal bytes
 *   0x168  u64 i_bytes               staged inode-area bytes
 *   0x170  u64 old_total             pre-resize total_blocks (sanity)
 *   0x178  invfs_superblock new_sb   the post-resize superblock image
 *   0x208  u32 crc32c                over the descriptor with this field 0
 * 204 bytes total (0x140..0x20C); the rest of block 0 stays reserved-zero. */
#define INVFS_RSZ0_OFF 0x140
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* 0x140 "RSZ0" */
    uint32_t version;           /* 0x144 */
    uint64_t stage_start;       /* 0x148 */
    uint64_t stage_blocks;      /* 0x150 */
    uint64_t bm_bytes;          /* 0x158 */
    uint64_t j_bytes;           /* 0x160 */
    uint64_t i_bytes;           /* 0x168 */
    uint64_t old_total;         /* 0x170 */
    invfs_superblock new_sb;    /* 0x178 */
    uint32_t crc32c;            /* 0x208 */
} invfs_rsz0;                   /* 0x140 + 204 bytes -> ends 0x20C */
#pragma pack(pop)

/* The staging area's own header, one block at stage_start. The payload
 * follows contiguously: bm_bytes of bitmap, then j_bytes of journal, then
 * i_bytes of inode-area records. payload_crc covers exactly those
 * bm+j+i bytes; the descriptor cross-checks the three lengths. */
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* "RSZS" */
    uint32_t version;           /* 1 */
    uint64_t bm_bytes;
    uint64_t j_bytes;
    uint64_t i_bytes;
    uint32_t payload_crc;
    uint32_t crc32c;            /* over the header with this field 0 */
} invfs_rszs;                   /* 40 bytes, block-padded */
#pragma pack(pop)

/* ---- WP21: CKP0 sweep-checkpoint descriptor (block 0 reserved area) ----
 * Lives at byte offset 0x220 of block 0, past the superblock (0x00..0x90),
 * the RDP0 descriptor (0x100..0x118) and the RSZ0 descriptor
 * (0x140..0x20C -- 0x180, the offset the WP21 spec suggested, lies INSIDE
 * the RSZ0 payload, so CKP0 moved to the first free 0x20 boundary).
 * Pre-WP21 images carry zeros there, which read as "absent" (magic
 * mismatch) -- the RDP0 convention.
 *
 * Written by invf-sweep BEFORE the walk (the checkpoint = the two append
 * pointers at sweep start), then every block the sweep retires is held by
 * the retention registry ("\x01reten" owner inode) instead of being freed.
 * invf-rollback restores the STAGED journal prefix (vol_flush rewrites the
 * journal in place, so the pre-sweep L2P survives only as a copy), zeroes
 * the inode area at inode_area_pos (append-only: the pre-sweep records are
 * byte-intact), and lets the fsck rebuild machinery reconcile the rest.
 *
 *   0x220  char magic[4]        "CKP0"
 *   0x224  u32 crc32c           over the descriptor with this field 0
 *   0x228  u64 inode_area_pos   inode-area append pointer at checkpoint
 *   0x230  u64 journal_pos      journal append pointer at checkpoint
 *   0x238  u64 stage_pba        staging run holding the journal prefix
 *   0x240  u64 stage_blocks     (content length = journal_pos - jstart)
 *   0x248  u64 sweep_seq        per-volume monotone checkpoint counter
 *   0x250  u64 time_unix        checkpoint wall time
 * 56 bytes total; the rest of block 0 stays reserved-zero. */
#define INVFS_CKP0_OFF 0x220
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* 0x220 "CKP0" */
    uint32_t crc32c;            /* 0x224 */
    uint64_t inode_area_pos;    /* 0x228 */
    uint64_t journal_pos;       /* 0x230 */
    uint64_t stage_pba;         /* 0x238 */
    uint64_t stage_blocks;      /* 0x240 */
    uint64_t sweep_seq;         /* 0x248 */
    uint64_t time_unix;         /* 0x250 */
} invfs_ckp0;                   /* 0x258 = 56 bytes */
#pragma pack(pop)

/* WP-M21: CMP0/CMPS gone. The v3 inode area is a sequence of dynamic
 * metadata extents in the mapper; no in-place compaction machinery exists.
 * The reserved layout jumps from CKP0 (0x220..0x258) to DEVT (0x2A0). */

/* ---- WP25: DEVT device-table descriptor (block 0 reserved area) ----
 * Lives at byte offset 0x2A0 of block 0, past the superblock (0x00..0x90),
 * RDP0 (0x100..0x118), RSZ0 (0x140..0x20C), CKP0 (0x220..0x258) and CMP0
 * (0x260..0x290). Single-device (pre-WP25) images carry zeros there, which
 * read as "absent" (magic mismatch) -- the RDP0 convention, and the reason
 * single-device volumes are byte-identical to what they were.
 *
 * A volume is up to TWO backing devices: dev0 (fast) + dev1 (capacity).
 * The global block address space is the concatenation: a global pba P with
 * P < dev_blocks[0] lives on dev0 at local block P, otherwise on dev1 at
 * local block P - dev_blocks[0]. sb.total_blocks = dev0 + dev1 blocks.
 *
 * Both devices hold the metadata span [0, metadata_zone_end) byte-identical
 * (writethrough mirror: superblock+descriptors, bitmap, journal slots,
 * inode area); the mirror is also where the positional redundancy lives:
 * on dev1 the local blocks [0, metadata_end) are the mirror of dev0's
 * metadata, i.e. they occupy GLOBAL blocks [dev0_blocks, dev0_blocks +
 * metadata_end) which the bitmap marks permanently allocated. The shadow
 * zone starts ABOVE that span, so the canonical shadow is entirely on
 * dev1; dev0's tail beyond the RAW zone is the tier arena (acceleration
 * copies only -- no sole copies on dev0, ever).
 *
 * sync_seq certifies mirror freshness: vol_flush bumps it and rewrites the
 * descriptor on every writable device AFTER the flush's other metadata; a
 * device that missed writes (io-latched continue-on-dev1, or a crash that
 * took only one side's DEVT) shows a lower sync_seq at open and is resynced
 * from the newer side at the next flush (newest state wins).
 *
 *   0x2A0  char magic[4]        "DEVT"
 *   0x2A4  u32 version          1
 *   0x2A8  u32 dev_count        1..2 (1 = recorded single-device table)
 *   0x2AC  u32 flags            DEVTF_*
 *   0x2B0  u64 dev_blocks[2]    per-device total blocks (0 = absent)
 *   0x2C0  u64 sync_seq         mirror sync sequence
 *   0x2C8  u8  vol_uuid[16]     copy of sb.uuid (device pairing check)
 *   0x2D8  char dev1_hint[128]  NUL-padded path hint for device 1
 *                               (INVFS_DEV1 env wins over the hint)
 *   0x358  u32 crc32c           over the descriptor with this field 0
 * 188 bytes total; the rest of block 0 stays reserved-zero. */
#define INVFS_DEVT_OFF 0x2A0
#define INVFS_DEVT_VERSION 1
#define INVFS_DEVTF_RAW_MIRROR 0x00000001u  /* RAW segments dual-written */
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* 0x2A0 "DEVT" */
    uint32_t version;           /* 0x2A4 */
    uint32_t dev_count;         /* 0x2A8 */
    uint32_t flags;             /* 0x2AC */
    uint64_t dev_blocks[2];     /* 0x2B0 */
    uint64_t sync_seq;          /* 0x2C0 */
    uint8_t  vol_uuid[16];      /* 0x2C8 */
    char     dev1_hint[128];    /* 0x2D8 */
    uint32_t crc32c;            /* 0x358 */
} invfs_devt;                   /* 0x35C - 0x2A0 = 188 bytes */
#pragma pack(pop)

/* WP-M21: invfs_cmps staging header retired with CMP0. */

/* ---- WP27: CVT0 v1->v2 conversion descriptor (block 0 reserved area) ---
 * Lives at byte offset 0x360 of block 0, past the superblock, RDP0
 * (0x100), RSZ0 (0x140), CKP0 (0x220) and DEVT (0x2A0).
 * Volumes that never saw a converter carry zeros there ("absent").
 *
 * invf-migrate-v2 rewrites a v1 volume in place: records grow 8 bytes per
 * segment (the pba), so the converted inode stream can overlap the one it
 * replaces. The conversion therefore runs the house stage -> arm -> apply
 * -> commit protocol: the v2 products (new inode stream + rebuilt
 * owner-WAL slot image + derived bitmap) are staged contiguously in free
 * space and CRC-verified, then this descriptor arms the conversion (the
 * superblock goes RECOVERY in the same block-0 write), then the payloads
 * are copied home (idempotent: the apply reads only the staging area), and
 * the commit (superblock with VOLF_ASTV2 + cleared descriptor, one block-0
 * write) lands last. A crash anywhere before the commit re-enters the
 * apply at the next invf-migrate-v2 run -- or `invf-migrate-v2 --abort`
 * disarms a conversion whose apply never started.
 *
 *   0x360  char magic[4]        "CVT0"
 *   0x364  u32 version          1
 *   0x368  u64 stage_pba        staging run: CVTS header block + payload
 *   0x370  u64 stage_blocks
 *   0x378  u64 stream_bytes     v2 inode stream bytes
 *   0x380  u64 jrn_bytes        owner-WAL slot image bytes (0 = empty)
 *   0x388  u64 bm_bytes         derived bitmap bytes
 *   0x390  u64 jrn_seq          sequence the rewritten journal carries
 *   0x398  u32 crc32c           over the descriptor with this field 0
 * 64 bytes total; the rest of block 0 stays reserved-zero. */
#define INVFS_CVT0_OFF 0x360
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* 0x360 "CVT0" */
    uint32_t version;           /* 0x364 */
    uint64_t stage_pba;         /* 0x368 */
    uint64_t stage_blocks;      /* 0x370 */
    uint64_t stream_bytes;      /* 0x378 */
    uint64_t jrn_bytes;         /* 0x380 */
    uint64_t bm_bytes;          /* 0x388 */
    uint64_t jrn_seq;           /* 0x390 */
    uint32_t crc32c;            /* 0x398 */
} invfs_cvt0;                   /* 0x39C - 0x360 = 60 bytes, pad to 64 */
#pragma pack(pop)

/* The conversion staging run's own header, one block at stage_pba; the
 * payload follows contiguously: stream_bytes of v2 inode records, then
 * jrn_bytes of journal-slot image, then bm_bytes of bitmap. payload_crc
 * covers exactly those stream+jrn+bm bytes (the RSZS rule). */
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* "CVTS" */
    uint32_t version;           /* 1 */
    uint64_t stream_bytes;
    uint64_t jrn_bytes;
    uint64_t bm_bytes;
    uint32_t payload_crc;
    uint32_t crc32c;            /* over the header with this field 0 */
} invfs_cvts;                   /* 40 bytes, block-padded */
#pragma pack(pop)

/* WP30: MET0 dynamic metadata extent mapper descriptor (block 0 reserved area)
 * Lives at byte offset 0x3A0 of block 0, past the superblock (0x00..0x90),
 * RDP0 (0x100), RSZ0 (0x140), CKP0 (0x220), CMP0 (0x260), DEVT (0x2A0)
 * and CVT0 (0x360). Volumes that never saw dynamic metadata carry zeros there
 * ("absent" - magic mismatch with MET0 magic).
 *
 * The mapper table is stored in the metadata zone (not block 0), and this
 * descriptor points to it. The mapper table format:
 *   - 16384 entries x 8 bytes = 131072 bytes = 32 blocks
 *   - Entry: bits [0,59] = absolute pba, bits [60,63] = size_class (4 bits)
 *   - size_class i = 64KB * 2^i (i=0..15, so 64KB..2GB)
 *   - Entry 0 is always the active extent (appending there)
 *
 * MET0 fields:
 *   0x3A0  char magic[4]        "MET0"
 *   0x3A4  u32 version          1
 *   0x3A8  u64 active_extent    index of active extent in mapper (0=first)
 *   0x3B0  u64 active_offset    byte offset within the active extent
 *   0x3B8  u64 extent_count    number of allocated extents
 *   0x3C0  u32 crc32c           over the descriptor with this field 0
 * 36 bytes total; the rest of block 0 stays reserved-zero. */
#define INVFS_MET0_OFF 0x3A0
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* 0x3A0 "MET0" */
    uint32_t version;          /* 0x3A4 */
    uint64_t active_extent;     /* 0x3A8 index of active extent */
    uint64_t active_offset;    /* 0x3B0 byte offset within active extent */
    uint64_t extent_count;     /* 0x3B8 number of allocated extents */
    uint32_t crc32c;           /* 0x3C0 over descriptor with this field 0 */
} invfs_met0;                  /* 0x3C4 - 0x3A0 = 36 bytes */
#pragma pack(pop)

/* ---- WP59: PCK0 codec-policy descriptor (block 0 reserved area) ----
 * Lives at byte offset 0x3C4 of block 0, past MET0 (0x3A0..0x3C4).
 * Volumes that were mkfs'd before WP59 carry zeros there ("absent").
 *
 * Records the codec-pack configuration a volume was written with, so the
 * engine can gate mount/read paths on whether the required decoders are
 * installed. Written by mkfs (always; BASIC_ONLY when no packs configured);
 * read and validated at vol_open.
 *
 *   0x3C4  char     magic[4]        "PCK0"
 *   0x3C8  u32      version         1
 *   0x3CC  u32      n_codecs        0..63
 *   0x3D0  u32      policy_flags    bit0 = BASIC_ONLY (no pack required)
 *   0x3D4  u64      conf_hash       BLAKE3 low 8B of packs.conf (0=none)
 *   0x3DC  u32      conf_len        length of packs.conf (0=none)
 *   0x3E0  u8       conf_encoding   INVFS_ALGO_* used to store packs.conf
 *   0x3E1  u8       _pad[3]         reserved zero
 *   0x3E4  [n_codecs] codec refs (24 B each, sorted by codec_id):
 *               u32 codec_id      stable id, independent of pack version
 *               u32 algo          INVFS_ALGO_* registry value
 *               u16 version       pack version installed at write time
 *               u16 min_read      minimum reader version that can decode
 *               u8  pack_id[12]   NUL-padded pack name
 *   0x...  u32      crc32c           over the descriptor with this field 0
 *
 * Max 63 codecs: 0x3C4 + 28 (fixed) + 63*24 = 0x3C4 + 1540 = 0x9CC
 * (fits in one block; block 0 is 4096 bytes). */
#define INVFS_PCK0_OFF      0x3C4
#define INVFS_PCK0_VERSION  1
#define INVFS_PCK0_MAX_CODECS 63
#define INVFS_PCK0_BASIC_ONLY 0x00000001u

#pragma pack(push, 1)
typedef struct {
    uint32_t codec_id;          /* stable id, independent of pack version */
    uint32_t algo;              /* INVFS_ALGO_* registry value */
    uint16_t version;           /* pack version at write time */
    uint16_t min_read;          /* minimum reader version to decode */
    char     pack_id[12];       /* NUL-padded pack name (e.g. "raw_image") */
} invfs_codec_ref;              /* 24 bytes */

typedef struct {
    char     magic[4];          /* 0x3C4 "PCK0" */
    uint32_t version;           /* 0x3C8 INVFS_PCK0_VERSION */
    uint32_t n_codecs;          /* 0x3CC 0..63 */
    uint32_t policy_flags;      /* 0x3D0 bit0 = BASIC_ONLY */
    uint64_t conf_hash;         /* 0x3D4 BLAKE3 low 8B of packs.conf */
    uint32_t conf_len;          /* 0x3DC length of packs.conf (0=none) */
    uint8_t  conf_encoding;     /* 0x3E0 INVFS_ALGO_* for packs.conf */
    uint8_t  _pad[3];           /* 0x3E1 reserved zero */
    invfs_codec_ref codecs[63]; /* 0x3E4 */
    uint32_t crc32c;            /* over the descriptor with this field 0 */
} invfs_pck0;                   /* ~1548 bytes; ends well within block 0 */
#pragma pack(pop)

/* ---- WP-M1: RT30 v3 root-area descriptor (block 0 reserved area) ----
 * Lives at byte offset 0x9D0 of block 0 -- the first FREE 16-byte-aligned
 * offset after the existing descriptors. Used ranges (read from this
 * header, not invented): superblock 0x00..0x90, RDP0 0x100..0x118,
 * RSZ0 0x140..0x20C, CKP0 0x220..0x258, CMP0 0x260..0x290,
 * DEVT 0x2A0..0x35C, CVT0 0x360..0x39C, MET0 0x3A0..0x3C4, and PCK0
 * 0x3C4..0x9D0. PCK0 is a fixed-size struct (63 codec refs max), so it
 * always ends at exactly 0x9D0, which is 16-byte aligned; a pre-v3 image
 * carries zeros there, which read as "absent" (magic mismatch) -- the
 * RDP0 convention. The descriptor is 48 bytes and ends at 0xA00, still
 * well inside block 0.
 *
 * The descriptor anchors the metadata-v3 base root (double-slot A/B) and
 * the active delta segment. `seq` is the root generation and the atomicity
 * anchor: on recovery the highest CRC-valid seq wins (the l2p_replay
 * idiom). mkfs writes seq=0 with empty root slots and no delta; the
 * B+-tree/delta engine that fills these lands in WP-M2/M3.
 *
 *   0x9D0  char magic[4]       "RT30"
 *   0x9D4  u32  version        1
 *   0x9D8  u32  page_size      metadata base-page size (default 4096)
 *   0x9DC  u64  root_slot[2]   pba of base root slot A / B (0 = empty)
 *   0x9EC  u64  delta_pba      pba of the active delta segment (0 = none)
 *   0x9F4  u64  seq            root generation (monotone; higher = newer)
 *   0x9FC  u32  crc32c         over the descriptor with this field read 0
 * 48 bytes total; the rest of block 0 stays reserved-zero. */
#define INVFS_RT30_OFF      0x9D0
#define INVFS_RT30_VERSION  1
#define INVFS_V3_PAGE_SIZE_DEFAULT 4096
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* 0x9D0 "RT30" */
    uint32_t version;           /* 0x9D4 INVFS_RT30_VERSION */
    uint32_t page_size;         /* 0x9D8 metadata base-page size */
    uint64_t root_slot[2];      /* 0x9DC base root slot A/B pba (0=empty) */
    uint64_t delta_pba;         /* 0x9EC active delta segment pba (0=none) */
    uint64_t seq;               /* 0x9F4 root generation (monotone) */
    uint32_t crc32c;            /* 0x9FC over descriptor, this field 0 */
} invfs_rt30;                   /* 0x9D0 + 48 -> ends 0xA00 */
#pragma pack(pop)

/* ---- WP-M16: SPT0 v3 save-point descriptor (block 0 reserved area) ----
 * Lives at byte offset 0xA00 of block 0, past the superblock (0x00..0x90),
 * RDP0 (0x100), RSZ0 (0x140), CKP0 (0x220), CMP0 (0x260), DEVT (0x2A0),
 * CVT0 (0x360), MET0 (0x3A0), PCK0 (0x3C4..0x9CC) and RT30 (0x9D0..0xA00).
 * Volumes that never captured a save point carry zeros there ("absent").
 *
 * The save point records {base_root, delta_end} at capture time. base_root
 * is the pinned RT30 root pba at capture; delta_end is the byte offset
 * within the delta segment chain at capture. Rollback publishes base_root
 * via RT30 double-slot, truncates the delta to delta_end, and replays.
 * K=1: refuse a second save point while one is live.
 *
 *   0xA00  char magic[4]       "SPT0"
 *   0xA04  u32  version        1
 *   0xA08  u32  flags          0 (reserved)
 *   0xA0C  u64  base_root      pinned RT30 root pba at capture
 *   0xA14  u64  delta_end      delta byte offset at capture
 *   0xA1C  u32  crc32c         over descriptor with this field 0
 * 32 bytes total; the rest of block 0 stays reserved-zero. */
#define INVFS_SPT0_OFF      0xA00
#define INVFS_SPT0_VERSION  1
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* 0xA00 "SPT0" */
    uint32_t version;           /* 0xA04 INVFS_SPT0_VERSION */
    uint32_t flags;             /* 0xA08 reserved */
    uint64_t base_root;         /* 0xA0C pinned RT30 root pba */
    uint64_t delta_end;         /* 0xA14 delta byte offset at capture */
    uint32_t crc32c;            /* 0xA1C over descriptor with this field 0 */
} invfs_spt0;                   /* 0xA00 + 32 -> ends 0xA20 */
#pragma pack(pop)

/* ---- WP-M1: v3 base-page + block-pointer wire format (design §12) ----
 * Frozen here so WP-M2 (page format + allocator) and WP-M3 (delta/fold)
 * share one definition instead of each inventing its own. A base page is a
 * fixed page_size blob:
 *     [invfs_page_hdr][invfs_blkptr child[] | packed leaf entries]
 * Internal nodes store invfs_blkptr children (extent-relative addressing
 * lands in WP-M2); leaf entries are key/value records appended after the
 * header. `gen` is the COW generation the page was written at (`checksum`
 * is CRC32C over the whole page with the checksum field read as zero).
 * Nothing in this WP parses a page -- these are wire-only declarations. */
#define INVFS_PAGE_MAGIC      "BPG3"
#define INVFS_PAGE_LEVEL_LEAF 0
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* "BPG3" */
    uint64_t gen;               /* COW generation this page was written at */
    uint16_t level;             /* INVFS_PAGE_LEVEL_LEAF or deeper */
    uint16_t nentries;          /* live entries following the header */
    uint32_t checksum;          /* CRC32C over page with this field 0 */
} invfs_page_hdr;               /* 20 bytes */

/* Pointer to a child page / extent. flags is reserved for the page
 * allocator (leaf/internal, pinned, ...); WP-M2 defines its values. */
typedef struct {
    uint64_t pba;               /* physical block address of the target */
    uint32_t checksum;          /* CRC32C of the referenced page */
    uint64_t gen;               /* generation of the referenced page */
    uint32_t flags;             /* reserved; WP-M2 defines */
} invfs_blkptr;                 /* 24 bytes */
#pragma pack(pop)

/* ---- WP-M10: v3 delta-log wire format (design §4/§12, decision D1) ----
 * The recent tier is an append-only, coalescing log of namespace mutations
 * (create/unlink/rename/attr/xattr). Records are written back-to-back after
 * a per-segment header; segments chain oldest <- newest through prev_pba and
 * the active (newest) segment is named by RT30.delta_pba. On mount the log
 * is replayed in chain order into an in-memory index keyed by namespace key
 * (D1: append log + in-memory index). Overlay reads are WP-M11, mutation
 * wiring is WP-M12, mount replay is WP-M13 and fold is WP-M14; this WP is
 * the log + index + replay engine only.
 *
 * Record (frozen by WP-M10; all length fields are big-endian on disk):
 *   key_len:u16 BE | val_len:u16 BE | flags:u16 BE | crc32c:u32 | key | val
 * crc32c is CRC32C over key||val (the field itself is not covered). flags
 * bit0 marks a delete record (shadows the base; value absent, val_len == 0);
 * other bits are reserved 0 and a new WP must claim one. A zeroed record
 * header is the clean end of the log (padding after the last live record),
 * not a zero-length record.
 *
 * Segment header (WP-M10; stripe size is this WP's decision). 32 blocks =
 * 128 KiB, large enough to hold the largest metadata record this path is
 * expected to carry (a WP-M7 xattr value is capped at 64 KiB) plus framing,
 * while keeping a mount-time replay buffer bounded:
 *   magic "DSG3" | version | hdr_size | seg_blocks | seg_seq | prev_pba |
 *   next_pba | crc32c
 * prev_pba walks toward the oldest segment (that is replay order). next_pba
 * is reserved 0 for a future forward walk (WP-M15 reclaim); it is not read
 * by this WP. All multi-byte header fields are big-endian on disk except the
 * CRC, which follows the RT30/page native-u32 convention. */
#define INVFS_DELTA_SEG_MAGIC     "DSG3"
#define INVFS_DELTA_SEG_VERSION   1u
#define INVFS_DELTA_FLAG_DELETE   0x0001u
#define INVFS_DELTA_REC_HDR_LEN   10u
#define INVFS_DELTA_SEG_BLOCKS    32u   /* 128 KiB stripe (this WP's decision) */
#define INVFS_DELTA_SEG_BYTES     ((uint64_t)INVFS_DELTA_SEG_BLOCKS * INVFS_BLOCK_SIZE)
#define INVFS_DELTA_SEG_HDR_LEN   44u   /* sizeof(invfs_delta_seg_hdr) */

#pragma pack(push, 1)
typedef struct {
    uint16_t key_len;           /* big-endian on disk */
    uint16_t val_len;           /* big-endian; 0 for a delete record */
    uint16_t flags;             /* INVFS_DELTA_FLAG_* (big-endian) */
    uint32_t crc32c;            /* CRC32C over key||val (native) */
} invfs_delta_rec_hdr;          /* 10 bytes */

typedef struct {
    char     magic[4];          /* "DSG3" */
    uint32_t version;           /* INVFS_DELTA_SEG_VERSION */
    uint32_t hdr_size;          /* INVFS_DELTA_SEG_HDR_LEN */
    uint32_t seg_blocks;        /* segment capacity in 4 KiB blocks */
    uint64_t seg_seq;           /* monotone segment sequence (newest highest) */
    uint64_t prev_pba;          /* older segment pba (0 = oldest) */
    uint64_t next_pba;          /* newer segment pba (0 = active; reserved 0) */
    uint32_t crc32c;            /* CRC32C over header with this field read 0 */
} invfs_delta_seg_hdr;          /* 44 bytes */
#pragma pack(pop)

/* WP30: Metadata extent entry in the mapper table (8 bytes)
 * Encoded as: bits [0,59] = absolute pba, bits [60,63] = size_class
 * size_class 0..15: extent_size = 64KB << size_class
 * Free entries have pba=0 and size_class=0 (both zero = invalid pba)
 * Entry 0 in the mapper is always the initial/active extent. */
#define INVFS_META_EXT_SIZE_CLASS_MAX 15
#define INVFS_META_EXT_ENTRIES 16384
#define INVFS_META_EXT_BLOCKS 32  /* 16384 * 8 / 4096 */
#define INVFS_META_EXT_MIN_SIZE_CLASS 1  /* 128KB default min (class 1 = 128KB) */

/* Decode/encode mapper entry */
static inline uint64_t invfs_meta_ext_pba(uint64_t entry) { return entry & 0x0FFFFFFFFFFFFFFFULL; }
static inline uint8_t  invfs_meta_ext_class(uint64_t entry) { return (uint8_t)((entry >> 60) & 0xF); }
static inline uint64_t invfs_meta_ext_size(uint64_t entry) { return 65536ULL << invfs_meta_ext_class(entry); }
static inline uint64_t invfs_meta_ext_encode(uint64_t pba, uint8_t size_class) {
    return pba | ((uint64_t)(size_class & 0xF) << 60);
}

/* WP59a: per-file anchor flag in the INO2 ext xattr TLVs.  1-byte value;
 * presence alone is the signal (any non-zero value means anchored). */
#define INVFS_XATTR_ANCHOR "invfs.anchor"

/* WP27: per-file heat counters live in the INO2 ext as an xattr TLV of
 * this name (moved out of the L2P journal pads -- the read path never
 * touches the journal any more). Value, 4 bytes:
 *   [u16 LE rheat][u8 wheat][u8 reserved]
 * Persistence: read touches accrue per-inode in RAM and fold into the
 * records at the sweep's decay pass (or an explicit vol_heat_persist);
 * rewrites carry wheat+1 into the replacement record. Heat is advisory:
 * a crash (or a close without a persist) loses only the pending
 * touches. */
#define INVFS_XATTR_HEAT "invfs.heat"

/* AST block entry — one byte-range mapping (kernel binary format).
 *
 * WP27 (format v2): 24B -> 32B, adding the physical block address.
 * Readers resolve segments directly from the entry's pba; the L2P journal
 * is no longer consulted for file data (it survives only as the
 * owner-scoped WAL: "\x01tzb" batches, "\x01parity*" seal, "\x01reten"
 * retention, "\x01rawm"/"\x01tier0" device sidecars).
 *
 * There is deliberately NO stored physical-block-count field: a segment's
 * extent is derivable from its own framed header ([4B csize][4B crc] at
 * pba -> plen = ceil((csize+8)/4096)). Destructive frees cross-check the
 * derived extent against the bitmap (a contiguous allocated run starting
 * at pba) plus the entry's logical length for the per-segment codecs, so
 * a torn header degrades to a leak (fsck-reclaimable), never an over-free.
 *
 * block_id is 24 bits: at most 2^24 segments per file. At the 64 KB
 * SEGMENT_SIZE that is exactly 2^24 * 2^16 = 2^40 bytes == MAX_FILE_SIZE,
 * so the bitfield, the v2 recipe header's u32 num_blocks and the file-size
 * sanity bound are mutually consistent -- 64 KB segments never need a
 * wider block_id. (Files that large SHOULD rather use fewer, bigger
 * segments; the format simply allows the 64 KB worst case -- a 1 TB file
 * is a ~384 MB recipe, rec_len is u32 and holds it. See doc/02.)
 *
 * block_id REMAINS the semantic slot id: the owner-WAL key for
 * owner-referenced shapes (a batch member's block_id is its batch_seq,
 * block_offset its offset in the DECODED batch; pba duplicates the batch's
 * address so reads never touch the journal). */
typedef struct {
    uint64_t file_offset;           /* offset in original file */
    uint64_t length;                /* length of range */
    uint32_t zone : 2;              /* 0=raw 1=text 2=binary */
    uint32_t algo : 6;              /* 0=none 1=zstd 2=ppmd 3=ape 4=jxl */
    uint32_t block_id : 24;         /* segment index / owner map key */
    uint32_t block_offset;          /* offset within the decoded batch */
    uint64_t pba;                   /* physical block address (WP27) */
} invfs_ast_block_entry;            /* 32 bytes */

/* The format-v1 entry (24B, no pba): physical addresses lived only in the
 * L2P journal, keyed (inode, block_id). Read now only by invf-convert,
 * which resolves pbas from the v1 journal and rewrites the records. */
typedef struct {
    uint64_t file_offset;
    uint64_t length;
    uint32_t zone : 2;
    uint32_t algo : 6;
    uint32_t block_id : 24;
    uint32_t block_offset;
} invfs_ast_block_entry_v1;         /* 24 bytes */

/* recursion / allocation guards (deep protection) */
#define MAX_AST_CHILDREN      65536u   /* max members per container */
#define MAX_AST_CHILD_NAME    255u
#define MAX_AST_DEPTH         16u      /* nested containers (zip-in-zip) */
#define MAX_FILE_SIZE         (1ull << 40)  /* sanity bound for file_size */
#define MAX_SEGMENTS          0xFFFFu  /* v1: num_blocks fits u16 */
#define MAX_SEGMENTS_V2       (1u << 24)  /* v2: block_id is 24 bits; at
                       * 64 KB segments this reaches exactly MAX_FILE_SIZE */

/* AST recipe header, v1 (16 bytes) — the original layout. Still what every
 * writer emits for anything that fits (file_size <= 4 GB - 1 and
 * num_blocks <= 0xFFFF), so existing volumes stay byte-stable. */
typedef struct {
    uint32_t version;               /* 1 */
    uint32_t file_size;
    uint16_t num_blocks;
    uint16_t num_children;
    uint32_t checksum;              /* CRC32C of recipe — see note below */
} invfs_ast_recipe_header_v1;       /* 16 bytes */

/* AST recipe header, v2 (24 bytes) — WP22a. Emitted ONLY when the v1 fields
 * cannot hold the truth (file_size > 4 GB - 1 || num_blocks > 0xFFFF);
 * everything else keeps the v1 header above. Readers branch on `version`
 * (the first u32 in both layouts) and fail LOUDLY on anything else.
 *
 * checksum: in v1 this field was always written as 0 and never verified
 * (integrity comes from the inode record's trailing CRC32C over rec_len,
 * which covers the recipe); v2 keeps exactly that rule -- the field is
 * reserved-zero, the covered span is the same (none beyond the record CRC). */
typedef struct {
    uint32_t version;               /* 2 */
    uint64_t file_size;
    uint32_t num_blocks;
    uint32_t num_children;
    uint32_t checksum;              /* reserved: 0 (see the v1 note) */
} invfs_ast_recipe_header_v2;       /* 24 bytes */

#define INVFS_AST_VERSION_V1  1u
#define INVFS_AST_VERSION_V2  2u
#define INVFS_AST_HDR_V1_LEN  16u
#define INVFS_AST_HDR_V2_LEN  24u

/* Version-agnostic parsed view of either header. hdr_len is where the
 * block entries begin (16 or 24); every consumer of the recipe MUST take
 * the entries offset from here -- a fixed 16 is the v1-only assumption
 * WP22a removed from every site. */
typedef struct {
    uint32_t version;
    uint64_t file_size;
    uint32_t num_blocks;
    uint32_t num_children;
    uint32_t checksum;
    uint32_t hdr_len;               /* INVFS_AST_HDR_V1_LEN / _V2_LEN */
} invfs_ast_hdr;

/* Parse a recipe header at buf[0..avail). 0 on success, -1 on truncation,
 * an unknown version (a newer format — fail loudly, never misread) or a
 * field past the format's own caps (file_size > MAX_FILE_SIZE,
 * num_blocks > MAX_SEGMENTS_V2 — both impossible from this writer, so such
 * a header is corrupt regardless of what the record CRC says). */
static inline int invfs_ast_hdr_parse(const void *buf, size_t avail,
                                      invfs_ast_hdr *out)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t ver;
    if (avail < INVFS_AST_HDR_V1_LEN) return -1;
    memcpy(&ver, p, 4);
    if (ver == INVFS_AST_VERSION_V1) {
        uint32_t fs, ck;
        uint16_t nb, nc;
        memcpy(&fs, p + 4, 4);
        memcpy(&nb, p + 8, 2);
        memcpy(&nc, p + 10, 2);
        memcpy(&ck, p + 12, 4);
        out->version      = ver;
        out->file_size    = fs;
        out->num_blocks   = nb;
        out->num_children = nc;
        out->checksum     = ck;
        out->hdr_len      = INVFS_AST_HDR_V1_LEN;
        return 0;
    }
    if (ver == INVFS_AST_VERSION_V2) {
        uint64_t fs;
        uint32_t nb, nc, ck;
        if (avail < INVFS_AST_HDR_V2_LEN) return -1;
        memcpy(&fs, p + 4, 8);
        memcpy(&nb, p + 12, 4);
        memcpy(&nc, p + 16, 4);
        memcpy(&ck, p + 20, 4);
        out->version      = ver;
        out->file_size    = fs;
        out->num_blocks   = nb;
        out->num_children = nc;
        out->checksum     = ck;
        out->hdr_len      = INVFS_AST_HDR_V2_LEN;
        if (out->file_size > MAX_FILE_SIZE ||
            out->num_blocks > MAX_SEGMENTS_V2)
            return -1;
        return 0;
    }
    return -1;
}

/* Serialize the SMALLEST header that holds the values: v2 only when
 * file_size, num_blocks or num_children overflow v1 (keeps current
 * volumes byte-stable). buf must hold INVFS_AST_HDR_V2_LEN bytes; returns
 * the length written (16 or 24). Values past the format caps return 0
 * (refuse, never fold). */
static inline size_t invfs_ast_hdr_write(void *buf, uint64_t file_size,
                                         uint32_t num_blocks,
                                         uint32_t num_children)
{
    if (file_size > MAX_FILE_SIZE || num_blocks > MAX_SEGMENTS_V2)
        return 0;
    if (file_size > 0xFFFFFFFFu || num_blocks > 0xFFFFu ||
        num_children > 0xFFFFu) {
        invfs_ast_recipe_header_v2 h;
        memset(&h, 0, sizeof h);
        h.version      = INVFS_AST_VERSION_V2;
        h.file_size    = file_size;
        h.num_blocks   = num_blocks;
        h.num_children = num_children;
        memcpy(buf, &h, sizeof h);
        return INVFS_AST_HDR_V2_LEN;
    }
    {
        invfs_ast_recipe_header_v1 h;
        memset(&h, 0, sizeof h);
        h.version      = INVFS_AST_VERSION_V1;
        h.file_size    = (uint32_t)file_size;
        h.num_blocks   = (uint16_t)num_blocks;
        h.num_children = (uint16_t)num_children;
        memcpy(buf, &h, sizeof h);
        return INVFS_AST_HDR_V1_LEN;
    }
}

/* AST child entry — one member of a container (e.g. ZIP member).
 * Serialized right after the block entries:
 *   [u16 name_len][name..][u16 method][u32 csize][u32 usize][u32 crc][u32 data_off]
 * The container KEEPS the original archive bytes (1:1 read-back — git-safe:
 * the same bytes come back); each member is a WINDOW (method + offsets)
 * into those bytes and is extracted on demand (stored / deflate-inflate).
 * No separate member inodes are created. All values are sanity-checked
 * against rec_len on read; never trusted as-is (protection against
 * crafted/corrupt headers -> OOM/UB). */
typedef struct {
    uint32_t name_len;              /* 0..MAX_AST_CHILD_NAME */
    char     name[256];
    uint16_t method;                /* 0=stored, 8=deflate */
    uint32_t csize;                 /* compressed size in container */
    uint32_t usize;                 /* uncompressed size */
    uint32_t crc;                   /* zip CRC-32 (IEEE) of payload */
    uint32_t data_off;              /* member data offset in container */
} invfs_ast_child_entry;            /* in-memory */

/* Largest children blob a container record may carry. */
#define INVFS_MAX_CHILD_BLOB  (1u << 20)

/* Inode area record (append-only), as it sits on disk. Followed by
   the AST recipe header (v1 or v2 — invfs_ast_hdr_parse decides), the
   block entries, the children blob, and a trailing CRC32C over the whole
   rec_len.

   This lived as three separate copies -- volume.c, ls.c, sweep.c -- each with
   its own idea of how long a record may be, which is how the bug below got in.
   One definition, one bound. */
#pragma pack(push, 1)
typedef struct invfs_inode_rec {
    uint32_t magic;
    uint32_t rec_len;      /* total record length incl. header+name+ast */
    uint64_t inode_id;
    uint64_t file_size;
    uint64_t ctime;
    uint32_t name_len;     /* 0..INVFS_MAX_NAME (255) */
    char     name[];       /* WP58a v3: name_len bytes, NUL at name[name_len] */
    /* followed by: recipe header (v1 16B / v2 24B) + entries[] [+ children] */
} invfs_inode_rec;
#pragma pack(pop)

/* WP58a v3: the record is a 36-byte fixed prefix + a variable-length name
 * (flexible array member, raw bytes, NUL-terminated at name[name_len]) + the
 * body (recipe header / entries / children / INO2). The body begins at
 * invfs_rec_body(rec); sizeof(invfs_inode_rec) is only the prefix, never the
 * body offset. Max name slot is INVFS_MAX_NAME + 1 bytes. */
#define INVFS_REC_HDR_LEN ((uint32_t)sizeof(invfs_inode_rec))
#define INVFS_NAME_CAP    (256u)   /* INVFS_MAX_NAME + 1 (NUL) */
static inline uint8_t       *invfs_rec_body (invfs_inode_rec *r)
    { return (uint8_t *)r->name + r->name_len + 1; }
static inline const uint8_t *invfs_rec_cbody(const invfs_inode_rec *r)
    { return (const uint8_t *)r->name + r->name_len + 1; }

/* META2 extension block — format v2 per-inode metadata. Sits AFTER the AST
   recipe (+ children blob), still covered by the record's trailing CRC32C and
   inside rec_len, so v1 scanners skip it via their normal advance rule.
   Wire layout:
     [invfs_meta_ext_hdr][target bytes if LNK][xattr TLVs...]
   xattr TLV = [u16 name_len][name][u16 val_len][value], repeated to the end
   of the block. All fields little-endian-by-host (see invfs_le*). */
#define INVFS_META_MAGIC 0x324F4E49u  /* "INO2" LE */

/* POSIX file types stored in invfs_meta_ext_hdr.type */
#define INVFS_ITYP_REG  0
#define INVFS_ITYP_DIR  1
#define INVFS_ITYP_LNK  2
#define INVFS_ITYP_FIFO 3
#define INVFS_ITYP_SOCK 4
#define INVFS_ITYP_CHR  5
#define INVFS_ITYP_BLK  6

#pragma pack(push, 1)
typedef struct invfs_meta_ext_hdr {
    uint32_t magic;      /* INVFS_META_MAGIC */
    uint16_t ext_len;    /* total bytes: hdr + target + xattrs */
    uint8_t  version;    /* 2 */
    uint8_t  type;       /* INVFS_ITYP_* */
    uint16_t mode;       /* permission bits (incl setuid/sticky) */
    uint32_t uid;
    uint32_t gid;
    int64_t  mtime;
    int64_t  atime;
    uint32_t nlink;
    uint64_t rdev;       /* device number for CHR/BLK */
    uint16_t target_len; /* bytes of symlink target following the header */
} invfs_meta_ext_hdr;    /* 48 bytes */
#pragma pack(pop)

/* ---- WP-M5: metadata-v3 inode row (base B+-tree value) ----------------
 * The v3 stable tier keeps one row per inode in the base B+-tree, keyed by
 * inode_id. WP-M5 freezes the key as u64 big-endian (byte-lexicographic
 * order == numeric order; see vol_btree.c); WP-M6's dirent keys are a
 * separate namespace and never collide with it.
 *
 * This value layout is versioned so WP-M6/M7 extend it by appending fields
 * (and bumping row_version) -- existing fields are never repurposed. All
 * values are little-endian (host LE, like the INO2 ext). The row carries
 * core attributes, nlink and a content-addressed recipe *reference*; the
 * recipe itself is an immutable AST blob (WP-M8), not inlined, so the row
 * stays small and cacheable. The symlink target lives in the recipe blob,
 * as v2 kept it in the INO2 ext.
 *
 *   u32 row_version   = INVFS_V3_INODE_ROW_VERSION
 *   u32 type          INVFS_ITYP_* (invarifs.h)
 *   u16 mode          permission bits
 *   u32 uid, gid
 *   i64 mtime, atime
 *   u32 nlink
 *   u64 rdev
 *   u64 size
 *   invfs_blkptr recipe   -> reserved 0 (WP-M8: the address below is the
 *                            authority; the blkptr is kept for the frozen
 *                            layout and future direct-page use)
 *   u32 xattr_len     TLVs, same encoding as the INO2 ext
 *   u8  recipe_addr[32]  BLAKE3-256 of the immutable recipe blob (all-zero
 *                        = no content). WP-M8 appends this and bumps
 *                        row_version to 2; it is the content address that
 *                        keys the recipe blob (design §12).
 *   [xattr bytes]
 *
 * WP-M5 always writes xattr_len == 0: the xattr tree is WP-M7. The field
 * is frozen here so that WP does not need a format break. */
#define INVFS_V3_INODE_ROW_VERSION 2u
#define INVFS_V3_INODE_XATTR_MAX   4096u
#define INVFS_V3_RECIPE_ADDR_LEN   32u
/* WP-M8: a recipe blob must fit one base page (4096 B). Header 20 +
 * key record (2+33) + value record (2+n) must fit with split headroom;
 * 3800 B is ~7.7 MiB of file at 64 KiB segments. Larger recipes need a
 * multi-page/streamed blob (TODO WP-M9/WP-M15). */
#define INVFS_V3_RECIPE_BLOB_MAX   3800u
#pragma pack(push, 1)
typedef struct {
    uint32_t     row_version;
    uint32_t     type;
    uint16_t     mode;
    uint32_t     uid;
    uint32_t     gid;
    int64_t      mtime;
    int64_t      atime;
    uint32_t     nlink;
    uint64_t     rdev;
    uint64_t     size;
    invfs_blkptr recipe;
    uint32_t     xattr_len;
    uint8_t      recipe_addr[INVFS_V3_RECIPE_ADDR_LEN];
} invfs_v3_inode_row;    /* 114 bytes; [xattr bytes] follow */
#define INVFS_V3_INODE_ROW_FIXED ((uint32_t)sizeof(invfs_v3_inode_row))
#pragma pack(pop)

/* WP-M7: xattr key prefix. Named xattrs live in the base B+-tree under a
 * third key namespace, keyed by
 *     0x03 || inode_id:u64 BE || name_len:u16 BE || name
 * (design-meta-v3.md §12/§15.2; key frozen by the WP-M7 doc). The 0x03 first
 * byte keeps xattr keys disjoint from and after WP-M5's 8-byte inode keys and
 * WP-M6's `parent:u64 BE || name_len || name` dirent keys, whose first byte is
 * the (small) high byte of an inode id. The value is the raw xattr value
 * bytes; the name is in the key, so one inode's xattrs are a contiguous key
 * range (ordered by name_len, then name -- the same shape as WP-M6 dirents)
 * and listxattr is a single ordered scan. */
#define INVFS_V3_XATTR_KEY_PREFIX 0x03u

/* WP-M8: recipe-blob key prefix. Immutable AST recipes are content-addressed
 * and stored in the base B+-tree under
 *     0x04 || blake3_256(serialized recipe)[32]
 * so identical recipes dedup to one key. The 0x04 first byte keeps the
 * keyspace disjoint from the 8-byte inode keys (whose first byte is the
 * small high byte of an inode id), WP-M6 dirent keys (>= 10 bytes) and
 * WP-M7 xattr keys (0x03). Read recomputes BLAKE3 and compares to the key;
 * a mismatch is a hard error, never a best-effort decode (design §16). */
#define INVFS_V3_RECIPE_KEY_PREFIX 0x04u

/* Longest record this format can produce: the header, the (v2) recipe
   header, the most segments a v2 num_blocks can count, and the largest
   children blob the writer will build. A record longer than this was not
   written by this code, so it is garbage whatever its magic says.

   The bound used to be a flat 0x10000, which is not a property of anything.
   One 24-byte block entry per 64 KB segment means rec_len grows as
   file_size/2731, so 64 KB capped a file at ~179 MB -- and the scans do not
   skip an over-long record, they STOP, taking inode_area_pos back with them.
   Every file appended after the first big one disappeared, and the next append
   would have overwritten them. A 274 MB claude.exe sat 4th in a 143k-file
   image and vol_open reported three names.

   WP22a: with the v2 recipe header the worst case is a 1 TB file at 64 KB
   segments -- 2^24 entries, a ~384 MB recipe. Insane as a record, but
   rec_len (u32) holds it and the bound must, or such a file's record would
   stop every scan exactly like the claude.exe case did.

   META2 slack: records written by format v2 append an "INO2" metadata block
   (uid/gid/mode/type/times/symlink target/xattrs) AFTER the AST recipe. The
   slack covers the largest ext this implementation can produce so v2 records
   never trip the bound a v1-only scanner enforces. */
#define INVFS_META_TARGET_MAX 1024u  /* symlink target cap */
#define INVFS_META_XATTR_MAX  4096u  /* total xattr TLV bytes per inode */
#define INVFS_META_SLACK \
    (sizeof(invfs_meta_ext_hdr) + (size_t)INVFS_META_TARGET_MAX + INVFS_META_XATTR_MAX)

#define INVFS_MAX_REC_LEN \
    (sizeof(invfs_inode_rec) + INVFS_NAME_CAP + INVFS_AST_HDR_V2_LEN + \
     (size_t)MAX_SEGMENTS_V2 * sizeof(invfs_ast_block_entry) + \
     (size_t)INVFS_MAX_CHILD_BLOB + INVFS_META_SLACK)

/* Reserve a writer must see free in the inode area before it accepts data it
   would otherwise have to drop at Close. This is a PRE-WRITE heuristic for
   paths that cannot report late failures (the dokan write gate), so it stays
   at the v1-era record size: the exact check (inode_area_pos + rec_size vs
   inode_area_end) runs at every append anyway, and demanding headroom for a
   hypothetical 1 TB file's 384 MB recipe would refuse tiny writes on small
   volumes. */
#define INVFS_INODE_REC_MAX \
    (sizeof(invfs_inode_rec) + INVFS_NAME_CAP + INVFS_AST_HDR_V1_LEN + \
     (size_t)MAX_SEGMENTS * sizeof(invfs_ast_block_entry) + \
     (size_t)INVFS_MAX_CHILD_BLOB + INVFS_META_SLACK)

/* L2P journal entry (append-only)
 *
 * WP27: the journal is now the OWNER-SCOPED WAL only: it maps
 * (owner_inode, ordinal) -> (pba, blocks) for the hidden owner records
 * ("\x01tzb" batches, "\x01parity*" seal, "\x01reten" retention,
 * "\x01rawm"/"\x01tier0" device sidecars). File data segments resolve
 * from their records' AST entries (the pba field); the read path never
 * consults this table.
 *
 * pad[3]: WP19 used to keep the heat counters here; format v2 moved them
 * into the records' INO2 ext (INVFS_XATTR_HEAT). The bytes stay in the
 * wire format (the journal layout is unchanged) but carry 0.
 *
 * crc: in the legacy flat log (pad2 == 0) a bare CRC32C over
 * entry[0..offsetof(crc)]; in slotted mode chained from the previous
 * entry's crc (the first entry from the slot header's) -- see the WP22d
 * note at INVFS_JRN_MAGIC. crc32c_update(0, e, 32) == the bare CRC, so the
 * legacy log is exactly "chain from zero". */
typedef struct {
    uint8_t  type;                  /* MAP/UNMAP/SWEEP/CHECKPOINT */
    uint8_t  pad[3];                /* WP19 heat: rheat u16 LE + wheat u8 */
    uint64_t inode;                 /* logical file id */
    uint64_t lba;                   /* logical block address */
    uint64_t pba;                   /* physical block address */
    uint32_t length;                /* blocks */
    uint32_t crc;                   /* CRC32C of entry */
} invfs_l2p_entry;                  /* 36 bytes */

/* WP30: metadata extent WAL entry (owner-scoped like L2P)
 * Wire format: 24 bytes
 *   [u8 type][u8 pad][u16 ext_idx][u64 pba][u8 size_class][u8 aux][u16 crc]
 * type = INVFS_JRN_META_* (0x10..0x14)
 * ext_idx = mapper table entry index
 * pba = physical block address (0 for SHRINK/FREE)
 * size_class = extent size class (0..15, 64KB << class)
 * aux = used by MERGE (source extent index) or extended size_class for EXTEND */
typedef struct {
    uint8_t  type;                  /* INVFS_JRN_META_* */
    uint8_t  pad[3];                /* reserved zero */
    uint16_t ext_idx;               /* mapper table entry index */
    uint64_t pba;                   /* physical block address */
    uint8_t  size_class;            /* extent size class */
    uint8_t  aux;                   /* type-specific auxiliary */
    uint16_t crc;                   /* CRC16-CCITT over bytes 0..21 */
} invfs_meta_wal;                   /* 24 bytes */

#pragma pack(pop)

/* CRC32C (Castagnoli) — software table-based */
uint32_t invfs_crc32c(const void *data, size_t len);
/* Chained form: crc = invfs_crc32c_update(crc, p, n) over successive chunks
 * equals invfs_crc32c over the concatenation; start from 0. Used where the
 * input does not fit in memory at once (WP18 resize staging). */
uint32_t invfs_crc32c_update(uint32_t crc, const void *data, size_t len);

/* Byte-order helpers: all on-disk values are little-endian.
 * Host is assumed little-endian (x86/x64/ARM LE); on BE platforms
 * these become real swaps. Kept trivial for now. */
#define invfs_le16(x) (x)
#define invfs_le32(x) (x)
#define invfs_le64(x) (x)

/* WP59: CRC convention for PCK0: over the descriptor with crc32c read as
 * zero (the RDP0 rule). Placed here after invfs_crc32c is declared. */
static inline uint32_t pck0_crc(const invfs_pck0 *p)
{
    invfs_pck0 t = *p;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}

#endif /* INVARIFS_H */
