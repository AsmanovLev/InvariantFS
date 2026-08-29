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
 * See impl_docs/WP10-textzone-codec-registry.md §2. */
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
    INVFS_CLASS_DEFER_ENOSPC     = 9  /* sweep deferred for free space (WP16b):
                                       * the file waits RAW and is
                                       * re-evaluated EVERY sweep; stamped
                                       * {algo,gen} of the declining codec */
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
#define VOLF_META2    0x00000002  /* records may carry "INO2" metadata ext */

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
   image and vol_open reported three names.

   META2 slack: records written by format v2 append an "INO2" metadata block
   (uid/gid/mode/type/times/symlink target/xattrs) AFTER the AST recipe. The
   slack covers the largest ext this implementation can produce so v2 records
   never trip the bound a v1-only scanner enforces. */
#define INVFS_META_TARGET_MAX 1024u  /* symlink target cap */
#define INVFS_META_XATTR_MAX  4096u  /* total xattr TLV bytes per inode */
#define INVFS_META_SLACK \
    (sizeof(invfs_meta_ext_hdr) + (size_t)INVFS_META_TARGET_MAX + INVFS_META_XATTR_MAX)

#define INVFS_MAX_REC_LEN \
    (sizeof(invfs_inode_rec) + sizeof(invfs_ast_recipe_header) + \
     (size_t)MAX_SEGMENTS * sizeof(invfs_ast_block_entry) + \
     (size_t)INVFS_MAX_CHILD_BLOB + INVFS_META_SLACK)

/* Reserve a writer must see free in the inode area before it accepts data it
   would otherwise have to drop at Close. */
#define INVFS_INODE_REC_MAX INVFS_MAX_REC_LEN

/* L2P journal entry (append-only)
 *
 * pad[3] is the WP19 heat home (it rides inside the CRC region, so the
 * counters are as durable as the mapping itself):
 *   pad[0..1] = u16 LE read-heat  -- saturating, +1 per open-session that
 *               touches the entry, >>= 1 per sweep run (exponential decay)
 *   pad[2]    = u8 write-heat     -- saturating, born 1 on create, old+1
 *               carried across a rewrite, -1 per sweep run
 * Keyed by (inode,lba) by construction, so it survives pba remaps (dedupe
 * re-keys preserve it). New entries (rewrites, transcodes) start cold:
 * read-heat resets (seeded by INVFS_HEAT_INIT at create), write-heat
 * accumulates across rewrites only. 0,0,0 on pre-WP19 volumes = cold --
 * no migration, nothing asserts pad==0. */
typedef struct {
    uint8_t  type;                  /* MAP/UNMAP/SWEEP/CHECKPOINT */
    uint8_t  pad[3];                /* WP19 heat: rheat u16 LE + wheat u8 */
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
