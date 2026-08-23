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

/* On-disk magic. Exactly 8 bytes, and it is the bytes that decide whether a
   volume opens at all -- so it stays "InvariFS" even though the project is now
   called InvariantFS ("InvariantFS" is 11 bytes and would not fit anyway).
   Changing it is a format break: every existing volume would stop mounting.
   If it ever moves, it moves in format v2 with a compatibility path. */
#define INVFS_MAGIC       "InvariFS\0"   /* 8 bytes */
#define INVFS_BLOCK_SIZE  4096
#define INVFS_VERSION     1
#define INVFS_JOURNAL_BLOCKS 8192  /* 32MB: ~235k entries */

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

/* L2P journal entry types */
#define INVFS_JRN_MAP       0x01
#define INVFS_JRN_UNMAP     0x02
#define INVFS_JRN_SWEEP     0x03
#define INVFS_JRN_CHECKPOINT 0xFF

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
    uint32_t pad2;                  /* 0x8C */
} invfs_superblock;                 /* 0x90 = 144 bytes */

/* volume flags (sb.vol_flags) */
#define VOLF_READONLY 0x00000001

/* AST block entry — one byte-range mapping (kernel binary format) */
typedef struct {
    uint64_t file_offset;           /* offset in original file */
    uint64_t length;                /* length of range */
    uint32_t zone : 2;              /* 0=raw 1=text 2=binary */
    uint32_t algo : 6;              /* 0=none 1=zstd 2=ppmd 3=ape 4=jxl */
    uint32_t block_id : 24;         /* block number in zone */
    uint32_t block_offset;          /* offset within block */
} invfs_ast_block_entry;            /* 24 bytes */

/* AST recipe header */
typedef struct {
    uint32_t version;
    uint32_t file_size;
    uint16_t num_blocks;
    uint16_t num_children;
    uint32_t checksum;              /* CRC32C of recipe */
} invfs_ast_recipe_header;          /* 16 bytes */

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

/* recursion / allocation guards (deep protection) */
#define MAX_AST_CHILDREN      65536u   /* max members per container */
#define MAX_AST_CHILD_NAME    255u
#define MAX_AST_DEPTH         16u      /* nested containers (zip-in-zip) */
#define MAX_FILE_SIZE         (1ull << 40)  /* sanity bound for file_size */
#define MAX_SEGMENTS          0xFFFFu  /* num_blocks fits u16 */

/* Largest children blob a container record may carry. */
#define INVFS_MAX_CHILD_BLOB  (1u << 20)

/* Inode area record (append-only), as it sits on disk. Followed by
   invfs_ast_recipe_header, the block entries, the children blob, and a
   trailing CRC32C over the whole rec_len.

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
    uint32_t name_len;
    char     name[256];
    /* followed by: invfs_ast_recipe_header + entries[] [+ children blob] */
} invfs_inode_rec;
#pragma pack(pop)

/* Longest record this format can produce: the header, the recipe header, the
   most segments num_blocks can count, and the largest children blob the writer
   will build. A record longer than this was not written by this code, so it is
   garbage whatever its magic says.

   The bound used to be a flat 0x10000, which is not a property of anything.
   One 24-byte block entry per 64 KB segment means rec_len grows as
   file_size/2731, so 64 KB capped a file at ~179 MB -- and the scans do not
   skip an over-long record, they STOP, taking inode_area_pos back with them.
   Every file appended after the first big one disappeared, and the next append
   would have overwritten them. A 274 MB claude.exe sat 4th in a 143k-file
   image and vol_open reported three names. */
#define INVFS_MAX_REC_LEN \
    (sizeof(invfs_inode_rec) + sizeof(invfs_ast_recipe_header) + \
     (size_t)MAX_SEGMENTS * sizeof(invfs_ast_block_entry) + \
     (size_t)INVFS_MAX_CHILD_BLOB)

/* Reserve a writer must see free in the inode area before it accepts data it
   would otherwise have to drop at Close. */
#define INVFS_INODE_REC_MAX INVFS_MAX_REC_LEN

/* L2P journal entry (append-only) */
typedef struct {
    uint8_t  type;                  /* MAP/UNMAP/SWEEP/CHECKPOINT */
    uint8_t  pad[3];
    uint64_t inode;                 /* logical file id */
    uint64_t lba;                   /* logical block address */
    uint64_t pba;                   /* physical block address */
    uint32_t length;                /* blocks */
    uint32_t crc;                   /* CRC32C of entry */
} invfs_l2p_entry;                  /* 36 bytes */

#pragma pack(pop)

/* CRC32C (Castagnoli) — software table-based */
uint32_t invfs_crc32c(const void *data, size_t len);

/* Byte-order helpers: all on-disk values are little-endian.
 * Host is assumed little-endian (x86/x64/ARM LE); on BE platforms
 * these become real swaps. Kept trivial for now. */
#define invfs_le16(x) (x)
#define invfs_le32(x) (x)
#define invfs_le64(x) (x)

#endif /* INVARIFS_H */
