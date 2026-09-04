/* volume_internal.h — shared internals of the InvariantFS volume layer.
 *
 * Everything the split vol_*.c modules share: the invfs_volume struct
 * definition (moved from volume.c), the small always-available helpers,
 * cross-module constants, and prototypes for the functions that used to be
 * `static` inside volume.c but are needed by more than one translation
 * unit (they keep their names; only the linkage changed).  The public API
 * is unchanged and lives in volume.h.
 */
#ifndef INVFS_VOLUME_INTERNAL_H
#define INVFS_VOLUME_INTERNAL_H

#define _CRT_SECURE_NO_WARNINGS


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "miniz.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <errno.h>
#include <signal.h>
#endif

#include "invarifs.h"
#include "volume.h"
#include "blkio.h"
#include "arc.h"
#include "lz4.h"
#include "zstd.h"
#include "zlib.h"
#include "ppmd_codec.h"
#include "codec.h"
#include "bcj_x86.h"
#include "blake3.h"
#include "rs.h"


/* flacx.c — FLAC frame recipe extract/rebuild (bit-exact). */
/* MUST match src/flacx.c layout: offset, len, kind, data */
typedef struct { uint32_t offset, len; uint8_t kind; uint8_t *data; } flacx_cover;
int flacx_extract(const uint8_t *d, size_t n, uint8_t **recipe, size_t *rlen,
                  flacx_cover **covers, uint32_t *ncovers);
int flacx_rebuild(const uint8_t *wav, size_t wlen, const uint8_t *r, size_t rn,
                  const flacx_cover *covers, uint32_t ncovers,
                  uint8_t **out, size_t *olen);
int flacx_recipe_num_covers(const uint8_t *r, size_t rn);

/* tarx.c — tar container recipe extract/rebuild (bit-exact). */
typedef struct {
    uint8_t  header[512];   /* exact original header bytes */
    uint64_t data_off;      /* payload offset in the original tar */
    uint64_t data_len;      /* payload length */
    uint8_t  pad_kind;      /* 1 = inter-member padding is zeros */
    uint16_t pad_len;       /* 0..511 */
} tarx_member;
int tarx_extract(const uint8_t *tar, size_t tar_len,
                 tarx_member **members_out, size_t *n_out,
                 uint8_t **trailer_out, size_t *trailer_len_out);
int tarx_build_recipe(const tarx_member *m, size_t n,
                      const uint8_t *trailer, size_t trailer_len,
                      uint8_t **recipe_out, size_t *rlen_out);
int tarx_parse_recipe(const uint8_t *r, size_t rlen,
                      tarx_member **members_out, size_t *n_out,
                      uint8_t **trailer_out, size_t *trailer_len_out);
int tarx_recipe_num_parts(const uint8_t *r, size_t rlen);
int tarx_rebuild(const tarx_member *m, size_t n,
                 const uint8_t *trailer, size_t trailer_len,
                 const uint8_t *const *parts, const size_t *plens,
                 uint8_t **out, size_t *olen);

/* pngx.c — PNG repack (JXL lossless + IVPN recipe, bit-exact). */
#include "pngx.h"

/* zlib (deflate replica for bit-exact gzip rebuild); inflate for split. */
int deflateInit2_(z_streamp strm, int level, int method, int windowBits,
                  int memLevel, int strategy, const char *version,
                  int stream_size);
int inflateInit2_(z_streamp strm, int windowBits, const char *version,
                  int stream_size);
int deflate(z_streamp strm, int flush);
int inflate(z_streamp strm, int flush);
uLong deflateBound(z_streamp strm, uLong sourceLen);
#ifdef deflateInit2
# undef deflateInit2
#endif
#define deflateInit2(s, l, m, w, mml, st) deflateInit2_((s), (l), (m), (w), (mml), (st), ZLIB_VERSION, sizeof(z_stream))
#ifdef inflateInit2
# undef inflateInit2
#endif
#define inflateInit2(s, w) inflateInit2_((s), (w), ZLIB_VERSION, sizeof(z_stream))
extern const char *zlibVersion(void);
extern uLong crc32(uLong crc, const Bytef *buf, uInt len);

#ifdef _WIN32
#define strdup _strdup
#endif


#define SEGMENT_SIZE INVFS_SEGMENT_SIZE  /* RAW segment granularity (volume.h) */


/* WP10: ARC keys for decoded PPMd batches are the batch's PBA with this tag
 * bit set. Whole-file cache entries key by inode id (monotonic from 1), so
 * an untagged pba could collide with an inode id and serve the wrong bytes;
 * with bit 63 set the two key spaces can never meet (pbas < total_blocks,
 * ids < 2^63). Used by vol_read_text_slice and vol_tz_gc (invalidate before
 * the block goes back to the bitmap). */
#define TZ_ARC_TAG (1ULL << 63)


/* ---- backing store: image file or raw block device (see blkio.c) ----
   This used to be a private copy of a four-function shim over ReadFile/read.
   It now delegates to blkio, which is shared with mkfs.c and which handles
   the sector alignment a raw device demands. The io_* names are kept so the
   ~100 call sites below read the same as before.

   WP25: the io_* macros now route through the volume's device MUX
   (vmux_*, volume.c): every engine call site passes &v->io (or &c->v->io),
   so the volume pointer is recovered from the embedded member. With one
   device the mux is a passthrough to blkio (byte-identical single-device
   path); with two it routes by offset range, dual-writes the metadata span
   to both devices, and fails metadata reads over to the mirror. */
struct invfs_volume;
int  vmux_seek(struct invfs_volume *v, uint64_t off);
int  vmux_read(struct invfs_volume *v, void *buf, size_t len);
int  vmux_write(struct invfs_volume *v, const void *buf, size_t len);
void vmux_close(struct invfs_volume *v);
/* 0 = both present devices barriered; 1 = dev0 failed (skipped, continues
 * on dev1, resync at next open); -1 = dev1 failed (caller latches). */
int  vmux_barrier(struct invfs_volume *v, const char *what);

#define io_seek(c, off)       vmux_seek(v_of_blk(c), (off))

#define io_read(c, buf, len)  vmux_read(v_of_blk(c), (buf), (len))

#define io_write(c, buf, len) vmux_write(v_of_blk(c), (buf), (len))

#define io_close(c)           vmux_close(v_of_blk(c))


/* ---- volume ---- */
/* In-memory name index. vol_find/vol_is_dir/vol_list_dir used to re-read the
   whole inode area from disk on every lookup, and since callers do many
   lookups per operation the mount scaled quadratically (measured on Dokan:
   1.9 ms/file at 100 files, 5 ms at 200, 10 ms at 400). vol_open already
   walks every record to verify CRCs, so building this costs no extra I/O.

   name_index maps name -> inode id. dir_index maps a directory prefix
   ("a/", "a/b/") to the number of live records under it, which is what
   makes vol_is_dir O(1) without a prefix scan: a directory exists exactly
   when its count is nonzero (its own anchor record counts too). */
typedef struct name_index_entry {
    struct name_index_entry *next;
    uint64_t inode_id;
    uint64_t pos;          /* byte offset of the record in the inode area */
    /* Cached from the record header. A directory listing needs the size and
       time of every entry; reading them back per entry cost one seek+read
       each, which is 1600 disk reads for a 1600-file directory -- and Windows
       enumerates the directory after every single create. */
    uint64_t size;
    uint64_t ctime;
    uint32_t nlen;
    char name[1];          /* NUL-terminated, allocated to fit */
} name_index_entry;


/* inode_id -> record offset. Never pruned on tombstone: the record stays on
   disk and vol_read_inode is expected to still find it, exactly as the old
   scan did. `live` counts the LIVE name-index entries pointing at this id:
   the rename fast path (hardlink + unlink) deliberately shares one id
   between two records, and a retire must not drop maps a surviving record
   still resolves through (WP22c/F2). */
typedef struct id_index_entry {
    struct id_index_entry *next;
    uint64_t id;
    uint64_t pos;
    uint32_t live;         /* live name entries referencing this id */
} id_index_entry;


typedef struct dir_index_entry {
    struct dir_index_entry *next;
    uint64_t count;        /* live records under this prefix */
    uint32_t nlen;
    char name[1];          /* prefix including the trailing '/' */
} dir_index_entry;

/* WP-L2Q: session-persistent (inode,lba) -> newest live L2P table slot.
 * vol_lookup_entry scanned the append-only table from the end on every
 * segment lookup -- O(live entries) per read, on the hot path. The index
 * mirrors the table exactly: built during replay (l2p_apply), updated by
 * vol_map / l2p_remove_mem / the bulk compaction loops (retire, sweep
 * unwind) / rebuilt wholesale by the fsck rebuild and the seal rewrites.
 * Values are table SLOT NUMBERS, not pointers: v->l2p reallocs on growth.
 * inode == 0 marks an empty slot (real inode ids start at 1). The table
 * stays the source of truth: an allocation failure disables the index and
 * every consumer falls back to the newest-wins scan. */
typedef struct {
    uint64_t inode, lba;
    uint64_t slot;            /* index into v->l2p[] */
} l2p_idx_ent;

/* WP25: one tier-arena copy / RAW-mirror index entry. key = the canonical
 * segment's pba (tier: a dev1-shadow pba; rawm: a raw-zone pba), pba = the
 * second copy's pba (tier: dev0 arena; rawm: dev1 shadow), plen = blocks,
 * ord = the owner-record map key (tier: a free-list ordinal; rawm: the
 * raw-zone-relative block index). */
typedef struct wp25_ent {
    uint64_t key, pba;
    uint32_t plen, ord;
} wp25_ent;


/* WP10 §4: one deferred text-batching candidate. Lives only in RAM for the
 * duration of a sweep run; a crash before vol_tz_flush just leaves the file
 * RAW for the next run (invariant: nothing lost). */
typedef struct {
    uint64_t inode_id;
    uint64_t size;         /* size at defer time (sort key only) */
    uint32_t family;       /* INVFS_TEXT_FAMILY_* — the batching sort key */
    char     name[256];
} tz_candidate;


/* WP16b: parsed-seekable-container-map cache, one entry per container
 * touched by the map read path (definition lives with the containerpack
 * machinery, below); the volume only owns the array. */
struct cpack_map_cache;


typedef struct invfs_volume {
    char *path;
    blkio io;
    invfs_superblock sb;
    uint8_t *bitmap;          /* in-memory copy */
    uint64_t bitmap_blocks;
    uint64_t journal_start;   /* block offset of journal within volume */
    uint64_t journal_pos;     /* next journal entry block*4096 + offset */
    uint64_t inode_area_start;/* block offset */
    uint64_t inode_area_pos;
    uint64_t inode_area_end;
    /* WP22c/F1: the inode-area position the last successful barrier pinned
     * (vol_open's scanned end; every successful vol_sync moves it to the
     * then-current tail). On a buffered backing store an append's io_write
     * reports success from the page cache, so a device error surfaces only
     * at the next barrier -- possibly many commits later, with the cursor
     * already past bytes that will never reach the device (the dm-flakey
     * error window left a zero hole in the append-only area and every
     * record committed past it became unreachable at the next mount). A
     * failed flush/sync must therefore never leave the cursor ahead of
     * unpersisted bytes: vol_io_error_latch re-anchors it here and latches
     * the volume read-only until remount + recovery. */
    uint64_t inode_area_durable;
    uint64_t alloc_cursor;    /* free-list cursor */
    /* Per-zone cursors and free counts. One shared cursor made every
     * allocation restart at the zone head after a RAW->Shadow spill, so a
     * nearly-full RAW zone was rescanned end to end -- for every 64 KB
     * segment -- before falling through. Tracking free blocks per zone lets
     * a hopeless zone be skipped without touching the bitmap at all. */
    uint64_t raw_cursor, shadow_cursor;
    uint64_t raw_free, shadow_free;
    /* smallest run length a full scan proved unavailable; 0 = unknown */
    uint64_t raw_fail_run, shadow_fail_run;
    /* Byte range of the bitmap changed since the last flush, as [lo, hi).
     * lo > hi means nothing is dirty. vol_flush used to push the whole
     * bitmap -- 480 KB on a 14.65 GB volume -- on every file close, and the
     * device is opened FILE_FLAG_NO_BUFFERING|WRITE_THROUGH, so that was a
     * synchronous ~250 ms per file on flash regardless of file size. */
    uint64_t bm_lo, bm_hi;
    uint64_t free_blocks;     /* cached free count (bitmap scan at open) */
    /* in-memory L2P */
    invfs_l2p_entry *l2p;
    size_t l2p_count, l2p_cap;
    /* WP-L2Q: the session hash index over the table (NULL = disabled:
     * allocation failed or INVFS_L2P_IDX=0 -- lookups fall back to the
     * newest-wins scan). Never diverges from the table; see the stress
     * leg of tools/test-l2p.sh. */
    l2p_idx_ent *l2p_idx;
    size_t l2p_idx_mask, l2p_idx_n;
    /* WP22d: the journal is append-only within a slot (invarifs.h WP22d
     * note); the in-memory table stays the compacted newest-wins view.
     * jops[] holds the not-yet-journaled ops (MAP/UNMAP, in order) that
     * the next vol_flush appends at the active slot's log end; j_heat[]
     * holds (inode,lba) pairs whose pad bytes changed in RAM only (WP19
     * WRITE-side carries -- read touches are RAM-only since WP-L2Q) and
     * which the next flush re-appends as refresh MAPs (or folds into the
     * compaction image when bulk). */
    invfs_l2p_entry *jops;
    size_t jops_n, jops_cap;
    uint64_t (*j_heat)[2];
    size_t j_heat_n, j_heat_cap;
    int j_slotted;            /* 0 = legacy flat log, 1 = slot format */
    uint32_t j_slot;          /* active slot index when slotted (0/1) */
    uint64_t j_seq;           /* active slot image's sequence number */
    uint32_t j_last_crc;      /* chain crc of the last journaled entry
                               * (the slot header's crc when the log is bare) */
    int j_compact;            /* next flush compacts (fsck rebuild) */
    int j_heat_all;           /* bulk heat change (decay) -> compact */
    uint64_t open_cuts;       /* consistent-cut hides at mount (WP22d) */
    uint64_t next_inode_id;
    /* on-demand sweep pending list (RAM) */
    uint64_t *pending;
    size_t n_pending, cap_pending;
    /* WP4ab: live ranged-write sessions (intrusive list, linked in
     * vol_write_begin, unlinked at commit/abort). The sweep must not
     * touch a file a session has forked: the session aliases the old
     * record's blocks, and a transcode retiring that record mid-session
     * would drop the old id's L2P maps and free blocks the session still
     * reads through its aliases. */
    invfs_wsession *wsessions;
    /* in-memory name index */
    name_index_entry **nbuck;
    size_t nmask, ncount;
    dir_index_entry **dbuck;
    size_t dmask, dcount;
    id_index_entry **ibuck;
    size_t imask, icount;
    /* Reconstructed-content cache. A transcoded file has no decodable
       segments -- only a blob plus a sibling recipe -- so serving a 64 KB
       ranged read means rebuilding the whole file. See arc.h. */
    invfs_arc *arc;
    /* WP10 sweep-time memory policy: peak decoder bytes a codec may need
       per unit; 0 = the 512 MiB default (vol_get_dec_mem_limit). Enforced
       at sweep admission only -- the read path never refuses stored data. */
    uint64_t dec_mem_limit;
    /* WP10: the ARC budget as last set (vol_open parse / vol_set_arc_budget),
       kept because the text-batch target is min(4 MB, budget/2) and the
       WHOLEFILE/compliance policy checks need it. 0 = cache disabled. */
    uint64_t arc_budget;
    /* WP10 §4: deferred text candidates awaiting vol_tz_flush */
    tz_candidate *tz;
    size_t tz_n, tz_cap;
    /* WP14a: deferred binary (executable) candidates awaiting the same
     * vol_tz_flush -- one flush entry point drains both accumulators */
    tz_candidate *bz;
    size_t bz_n, bz_cap;
    /* WP14b M2: part count of the last exe carve (vol_sweep_one rc 11);
     * reporting-only, read by the sweep driver */
    uint32_t last_exer_parts;
    /* WP16b: the codec profile (INVFS_PROFILE_*, codec.h), parsed from
     * INVFS_PROFILE at vol_open. v1 effect: the generic sweep's ZSTD level
     * (invfs_profile_zstd_level); the effective name is also published back
     * to the environment so pack subprocesses inherit it. */
    uint8_t profile;
    /* WP19: heat (invfs_l2p_entry.pad, see invarifs.h). heat_init seeds the
     * read-heat of newly created entries (INVFS_HEAT_INIT, default 0).
     * heat_any_rhot/whot are "some entry sits at/above the hot threshold"
     * summaries -- they let the sweep skip the promotion walk / write-hot
     * scans on cold volumes (recomputed by every decay pass; set on the
     * increment that crosses a threshold). heat_seen is the process-
     * lifetime "counted this session" set behind the once-per-open-session
     * read increment (open addressing of (inode,lba) pairs, [0]=inode --
     * 0 = empty slot; inode ids start at 1). */
    uint16_t heat_init;
    int heat_any_rhot, heat_any_whot;
    uint64_t (*heat_seen)[2];
    size_t heat_seen_cap, heat_seen_n;
    /* WP16b: parsed !mbrmap cache (local-splice reads of seekable
     * containers). Filled on first map read of a container, invalidated
     * when its name (or a "name!..." sibling) is retired, freed at
     * vol_close. */
    struct cpack_map_cache *maps;
    size_t maps_n, maps_cap;
    /* WP20b: redundancy configuration (the RDP0 descriptor at block 0
     * offset 0x100) + dirty-stripe tracking for incremental reseal.
     * seal_k1 is the EFFECTIVE layer-1 stripe size (the descriptor's k1
     * when live, SEAL_STRIPE_K = 32 otherwise). seal_dirty is a bitmap
     * over shadow_zone_blocks, allocated lazily when a seal config
     * exists (descriptor live at open, or vol_seal configures one): a
     * set bit means the block (and hence its stripes) changed since the
     * last successful reseal. It is always allocated ALL-ONES: what
     * happened before this process opened the volume is unknowable, so
     * the first reseal of a session is a full pass; a successful
     * vol_seal zeroes it. */
    invfs_rdp0 rd;
    int rd_present;
    uint32_t seal_k1;
    uint8_t *seal_dirty;
    /* WP21: sweep checkpoint (the CKP0 descriptor at block 0 offset
     * INVFS_CKP0_OFF) + the retention registry (the "\x01reten" owner)
     * behind it. ck/ck_present: the descriptor as read at open (absent =
     * zero-filled). ck_prev_seq: the last armed/realized sequence number,
     * so the next arm increments across a realize. Retention is a VOLUME
     * state, not a session state: while CKP0 is live on disk EVERY process
     * routes vol_free_blocks into retmap instead of freeing (a post-
     * checkpoint rewrite/delete that freed a pre-checkpoint block for real
     * would let a rollback resurrect a record whose pba was reallocated --
     * F4, the leg-5 soak's THIRD STATE). retmap is a bitmap over
     * total_blocks of the blocks held for the checkpoint; they stay
     * allocated in the real bitmap too (that is what bars reuse), retmap
     * is only the realize/registry list. A NULL retmap (every process that
     * did not arm the checkpoint itself) degrades registration to "blocks
     * stay allocated, nothing is registered" -- rollback is unaffected (it
     * never reads the registry); the unregistered ranges are reclaimed by
     * the fsck rebuild after the checkpoint resolves. retain_release: the
     * checkpoint machinery's own deliberate frees (the realize's registry
     * delete, the arm's staging unwind, the no-op disarm) bypass
     * retention. ck_stage_*: the journal staging run allocated at arm
     * time. */
    invfs_ckp0 ck;
    int ck_present;
    uint64_t ck_prev_seq;
    int retain;
    int retain_release;
    uint8_t *retmap;
    uint64_t ck_stage_pba, ck_stage_blocks;
    /* WP24-lite: this handle is a READ-ONLY time-travel view at the live
     * CKP0 checkpoint (vol_open_at): the L2P was replayed from the
     * checkpoint's staged journal prefix and the inode scan stopped at the
     * checkpoint's append pointer, so the in-memory view is exactly the
     * sweep-start state. NOTHING may be written through it -- the
     * checkpoint pins absolute positions of the PRESENT (post-checkpoint)
     * records, and an append at the cut would clobber them. The flag is
     * the engine-level refusal (vol_mark_dirty fails loud; vol_flush and
     * vol_sync are no-ops; vol_close persists nothing); the VOLF_READONLY
     * flag is set too so every vol_write_enabled caller reports EROFS. */
    int time_travel;
    /* Crash consistency (doc/08). `dirty` remembers that the on-disk state
       has already been set to DIRTY this session, so the mark costs one
       superblock write per mount instead of one per mutation.
       `needs_recovery` is set at open when the previous session did not close
       cleanly, and refuses every mutation until vol_recover() has run. */
    int dirty;
    int needs_recovery;
    /* set only by vol_io_error_latch: a flush/sync failed THIS session.
     * Distinct from needs_recovery (which may be inherited from the
     * superblock at open): a merely-unclean volume is read-only but
     * READABLE (inspection); a latched one must not serve possibly
     * phantom content at all. */
    int io_latched;
    /* records that failed CRC/bounds during the open scan; >0 means the
     * volume shows real damage and must not self-recover a DIRTY state */
    uint64_t scan_anomalies;
    /* WP22c test hook (tools/test-flushfail.sh): when nonzero, the Nth
     * vol_sync of this process simulates the dm-flakey error window --
     * every inode-area byte appended since the last successful barrier
     * dies in "writeback" (zeroed on the image) and the barrier reports
     * EIO. The engine must latch + re-anchor, and every later mutation
     * must fail loudly. 0 = off. */
    uint64_t sync_fail_at;
    /* hot population counters, maintained incrementally by idx_put /
     * idx_del_at / idx_del (insert vs update vs removal) and bumped once
     * per DELT append. Seeded for free: the open scan replays every
     * record through the same index calls. Served instantly by the
     * user.invfs.stats virtual xattr -- no record walk needed. */
    struct {
        uint64_t files;        /* non-dir live names */
        uint64_t dirs;         /* "path/" anchors */
        uint64_t tombstones;   /* DELT records appended this volume life */
        uint64_t logical_bytes;/* sum of live file sizes */
    } hot;
    /* ---- WP25: two-device volumes (DEVT descriptor, invarifs.h) ----
     * v->io is device 0; v->io2 is device 1 (valid when ndev==2 and
     * dev0_present... io_open[] tracks which blkio_open calls succeeded
     * so vmux_close never closes a half-initialized backend. */
    blkio    io2;
    char    *path2;          /* dev1 path as opened (diagnostics) */
    int      io_open[2];
    int      ndev;           /* 0 while bootstrapping, then 1 or 2 */
    int      dev0_present;   /* 0: degraded mount, serving from dev1 */
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
    /* dev0 tier arena: [arena_start, arena_start+arena_blocks) -- the dev0
     * tail past the RAW zone. Holds ONLY redundant acceleration copies
     * (heat promotions): canonical data never lands there, so dev0's loss
     * costs nothing (WP25 rule: no sole copies on dev0). */
    uint64_t arena_start, arena_blocks;
    uint64_t arena_free, arena_cursor, arena_fail_run;
    /* tier (dev0 acceleration copies) + rawm (RAW mirror) indexes, both
     * sorted by key (bsearch). key = canonical dev1 pba / raw-zone pba.
     * Persisted through the hidden owner records "\x01tier0" / "\x01rawm"
     * (the WP20 seal-owner pattern: ordinary L2P maps keyed by ordinal,
     * owner AST entry block_id = ordinal, file_offset = key). */
    struct wp25_ent *tier, *rawm;
    size_t   tier_n, tier_cap, rawm_n, rawm_cap;
    int      tier_dirty, rawm_dirty;
    uint64_t tier_owner, rawm_owner;   /* live owner inode ids (0 = none) */
    /* last vol_tier_migrate run's counters (the sweep driver prints) */
    uint64_t tier_promoted, tier_demoted, tier_blocks;
} invfs_volume;

/* v_of_blk: recover the volume from the embedded dev0 blkio (the io_*
 * macros' single argument). Must live after the struct definition. */
static inline struct invfs_volume *v_of_blk(const blkio *io)
{
    return (struct invfs_volume *)((char *)io - offsetof(invfs_volume, io));
}


/* WP19 heat thresholds + pad codec (the full rules live with the heat
 * machinery after the L2P helpers; these are needed by vol_open already):
 * promotion needs rheat >= HOT *after* the run's decay, so a single read
 * burst promotes iff it survives exactly one halving; write-hot =
 * rewritten at least twice inside the last sweep interval. */
#define INVFS_HEAT_HOT     8u

#define INVFS_WHEAT_HOT    2u

/* promotion budget: never more than this many extractions per sweep */
#define INVFS_HEAT_PROMOTE_MAX 64


/* inode area record (append-only); invfs_inode_rec lives in invarifs.h */
#define INODE_REC_MAGIC 0x444F4E49u  /* "INOD" LE */

#define TOMBSTONE_MAGIC 0x544C4544u  /* "DELT" LE */



/* Longest path an on-disk record can hold: name is a fixed 256-byte field,
   so 255 bytes plus a terminator.

   Every writer used to pair name_len = strlen(name) with a strncpy of
   sizeof(name) - 1. For a longer path that stored a length which did not
   match the bytes stored after it, and readers trust name_len -- the index
   build, fsck and ls all copy that many bytes out of the field, so they read
   a name running past its end and into the AST recipe behind it. rename_one
   was the only path that checked. Names are validated on the way in now, so
   an over-long one is refused rather than silently truncated to a record
   that no longer describes the file it points at.

   INVFS_MAX_NAME is declared in volume.h so name-building callers can check
   before they start; this asserts it still matches the field it describes. */
typedef char invfs_name_fits[
    (INVFS_MAX_NAME == sizeof(((invfs_inode_rec *)0)->name) - 1) ? 1 : -1];


/* A decomposed file also has to name its children: "<name>!part<n>",
   "<name>!cover<n>", "<name>!recipe", "<name>!jxl". Those names go through the
   same 256-byte field, so a base name near the limit leaves no room for one.
   The child create would then be refused partway through the loop, after
   earlier parts were already appended -- the transcoder returns 0 and the
   caller keeps the original bytes, but those orphaned part records stay behind
   for fsck to find. Reserve the longest suffix ("!part" plus a 10-digit
   uint32) and decline to decompose at all instead: returning 0 is the same
   "not smaller, store as-is" answer these paths already give. */
#define INVFS_SIBLING_RESERVE 16


/* ---- WP20b: redundancy descriptor + dirty-stripe bitmap ---------------
 * (the seal machinery itself lives with the write-path WP20 code far
 * below; vol_open and the write/alloc/free hooks need these early) */

#define SEAL_STRIPE_K   32u   /* default layer-1 stripe (descriptor k1) */

#define SEAL2_K         32u   /* layer-2 RS stripe data blocks (fixed) */

#define SEAL2_M2_MIN    2u

#define SEAL2_M2_MAX    8u

#define EXE_MAX_MEDIA 64            /* sanity bound on regions per exe */


/* one row of the EXER payload's part table (wire form; the codec is an
 * INVFS_ALGO_* value so the table is self-describing) */
typedef struct {
    uint64_t off, len;
    uint8_t codec;
} exer_row;


/* WP16b DEFER_ENOSPC admission heuristic (sweep-time only): 1 = the volume
 * is too full to attempt a transcode that must hold the NEW shape while the
 * old one is still stored. need_bytes is the caller's worst case; the flat
 * margin covers the records/journal/slack the estimate cannot see (member
 * csizes are unknowable pre-write -- members compress through the pipeline
 * later, so the heuristic prices them at a conservative fraction of their
 * raw size at the call site). Compared in BLOCKS: no byte-space overflow. */
#define INVFS_ENOSPC_MARGIN (64ull << 20)   /* 64 MiB */


/* ==================== WP10: Text Zone write path ====================
 *
 * Text files are not compressed per file; they are concatenated into shared
 * ~4 MB batches (sorted by language family, then size -- the doc/06 grouping
 * win) and each batch is PPMd-compressed once (WP10 §3, ReiserFS-style
 * packing). Three record shapes cooperate:
 *
 *   owner  "\x01tzb"  -- hidden internal inode (0x01-prefixed names are
 *     filtered from vol_list_dir/readdir). One AST entry per sealed batch:
 *     {file_offset=cumulative decoded offset, length=usize, zone=TEXT,
 *      algo=PPMD, block_id=batch_seq}; L2P under the owner id maps
 *     batch_seq -> batch pba, which is what keeps batch blocks live in fsck.
 *     The owner id never changes across rewrites (position-kill tombstones),
 *     so its L2P keys stay valid for the volume's whole life.
 *   member -- one AST entry per slice:
 *     {file_offset=in the file, length=slice_len, zone=TEXT, algo=PPMD,
 *      block_id=batch_seq, block_offset=offset_in_batch}, plus an L2P dup
 *     (member_new_id, batch_seq) -> batch pba (the offline-dedupe trick).
 *   batch segment -- standard framing [4B csize][4B crc32c] around payload
 *     [4B usize LE][2B props][PPMd stream]; allocated in the shadow zone
 *     with use_reserve=1 like every other sweep output.
 *
 * Crash safety: the batch segment and the owner map are journaled BEFORE the
 * owner record names the batch, and the owner record lands before any member
 * record references it. A crash between seal and member commits leaves an
 * orphan batch (vol_tz_gc reclaims it) and the members still RAW (readable).
 * The inverse order is never written, so no state needs recovery.
 *
 * The accumulator is RAM-only by design: deferral changes nothing on disk,
 * so an interrupted sweep simply re-classifies the files next run. */

#define TZ_OWNER_NAME "\x01tzb"


/* owner inode state, loaded once and rewritten per change */
typedef struct {
    invfs_ast_block_entry *ents;
    uint32_t n, cap;
    uint8_t *ext;            /* INO2 ext blob, carried verbatim (usually none) */
    uint32_t ext_len;
    uint64_t pos;            /* current record position (position-kill target) */
    uint64_t ctime;
    uint64_t next_seq;       /* max block_id + 1 -- strictly monotone */
} tz_owner;


/* The seal state a read/verify path needs: the shard owner ids of BOTH
 * layers and the set of blocks they map (the exclusion set: parity blocks
 * are cargo for neither layer). Read-only to load. */
typedef struct {
    uint64_t *shard_id;      /* layer-1 owner inode id per shard */
    size_t   nshards;
    uint64_t *shard2_id;     /* layer-2 owner inode id per shard */
    size_t   nshards2;
    uint8_t *is_par;         /* bitmap over total_blocks: 1 = parity block */
    /* WP21: bitmap over total_blocks: 1 = a retention-registry block.
     * Retained blocks are the pre-sweep forms held for a possible
     * rollback: nobody reads them while the checkpoint is live, and
     * rollback is REFUSED under a live seal, so covering them would only
     * churn the stripes they sit in when a realize frees them. */
    uint8_t *is_ret;
} seal_view;

static inline uint16_t l2p_rheat(const invfs_l2p_entry *e)
{
    return (uint16_t)(e->pad[0] | ((uint16_t)e->pad[1] << 8));
}

static inline void l2p_set_rheat(invfs_l2p_entry *e, uint16_t r)
{
    e->pad[0] = (uint8_t)r;
    e->pad[1] = (uint8_t)(r >> 8);
}

static inline int name_too_long(const char *name)
{
    size_t n = strlen(name);
    if (n <= INVFS_MAX_NAME) return 0;
    fprintf(stderr, "invarifs: name is %llu bytes, the format holds %llu: %.72s...\n",
            (unsigned long long)n, (unsigned long long)INVFS_MAX_NAME, name);
    return 1;
}

static inline int name_too_long_for_children(const char *name)
{
    return strlen(name) + INVFS_SIBLING_RESERVE > INVFS_MAX_NAME;
}


/* Store a name in a record: one clamped length drives both the field and the
   length header, so the two cannot disagree even if a caller skipped the
   check above. Assumes the record was zeroed (all callers calloc or memset). */
static inline void rec_set_name(invfs_inode_rec *rh, const char *name)
{
    size_t n = strlen(name);
    if (n > INVFS_MAX_NAME) n = INVFS_MAX_NAME;
    rh->name_len = (uint32_t)n;
    memcpy(rh->name, name, n);
    rh->name[n] = 0;
}

static inline int bit_get(const uint8_t *b, uint64_t i) { return (b[i / 8] >> (i % 8)) & 1; }

static inline void bit_set(uint8_t *b, uint64_t i) { b[i / 8] |= (uint8_t)(1u << (i % 8)); }

static inline void bit_clr(uint8_t *b, uint64_t i) { b[i / 8] &= (uint8_t)~(1u << (i % 8)); }

/* ---- cross-module prototypes (were file-static in volume.c) ---- */

/* WP20 (the seal parity machinery lives with the write-path WP code far
 * below; the read paths above it need these two): */

/* WP16b (definitions live with the containerpack machinery, below):
 * the local-splice read of a seekable container through its !mbrmap
 * sibling (returns the byte count, -1 on failure), and the per-volume
 * parsed-map cache lifecycle. */

/* defined near vol_count_free; needed by vol_open and the fsck fixup above it */

/* fsck: rebuild L2P/bitmap from inode area (defined after vol_flush) */

/* ---- WP4b: incremental ranged-write sessions ----------------------------
 * A session forks a file under a NEW inode id and builds its AST
 * incrementally, one 64K segment per touched range:
 *   begin  -> remember the old record's id; nothing is copied yet
 *   load   -> (lazy, first write/truncate/commit) read the old record:
 *             plain per-segment NONE/LZ4 content -> alias every old
 *             segment into the new id's L2P (no data copies, per-entry
 *             heat carried); anything swept/container/batched ->
 *             materialize: re-read through the real read path and rewrite
 *             every segment as fresh RAW. A write to a swept file IS an
 *             implicit downgrade to RAW (v1 policy: correct and simple);
 *             the old record's transcode siblings die at commit.
 *   write  -> a touched segment's current plaintext (its own rewrite on
 *             re-touch, the old bytes when aliased) is patched,
 *             recompressed, written to a NEW pba and re-mapped.
 *             Overwrite-in-place never happens.
 *   commit -> re-encode the last segment at its exact tail length, append
 *             the new record, then retire the old id (vol_delete_inode,
 *             the vol_replace_file ordering: the new version is live
 *             first; the retire frees exactly the old blocks nothing
 *             references any more).
 * Crash before commit: the old record is still live and owns its blocks;
 * orphaned new-id maps are purged by fsck, orphan blocks reclaimed.
 * No torn states. */

/* read file data back via AST + L2P (recursive for containers); returns 0 */
/* WP16a (the definition lives with the codecpack exec layer, below) */

/*
 * Sweep one file: recompress all segments RAW(LZ4) -> Shadow(ZSTD-19).
 * Returns 0 on success, -1 on error, 1 if nothing to sweep.
 *
 * The recompressed record lands under a NEW inode id and the old one is
 * tombstoned, rather than being rewritten where it lies. Rewriting in place
 * cannot be made crash-safe, because the record and its mappings share the key
 * (inode_id, block_id): flush the maps first and a crash leaves them pointing
 * at ZSTD blocks while the record still says LZ4; write the record first and a
 * crash leaves the mirror image. Neither is recoverable, since block_id is a
 * segment index and the physical address exists only in the journal. A torn
 * in-place write is worse still -- the CRC fails and vol_open skips the record,
 * so the file simply disappears.
 *
 * It also fixes a failure that needed no crash at all: when alloc_blocks ran
 * out partway through, segments 0..k-1 already had Shadow blocks and new maps
 * while the record still described them as RAW/LZ4, and step 7 was skipped on
 * the error return. The file was left decodable-as-garbage. Now nothing is
 * visible until the whole file is done, and the unwind gives the space back.
 */
/* Carry INO2 metadata across transcode rewrites. The generic ZSTD path
 * copies the old record verbatim (ext included), but the JXL/APE/FLAC/TAR/
 * GZ/PNG/PMP/ZIP branches build fresh records that would silently drop the
 * file's permissions/owner/times. Wrapper resolves the name up front, lets
 * the inner sweep run, and re-applies the captured meta only when the name
 * now belongs to a NEW record that lacks an ext. */

/* WP14b: defer the parts of a just-exploded extraction container
 * ("name!partN", N = 0..) into the batching accumulators, so a TAR swept in
 * this run has its members batched by THIS run's vol_tz_flush instead of
 * sitting one run in per-file ZSTD. The flush re-reads and re-sniffs each
 * part from its live record, so a head sniff is enough here; parts that
 * sniff as nothing stay per-file generic (the absent-stamp walk branch
 * reconsiders them next run). */

/* WP10 write path (defined with the class helpers, after vol_stamp_class):
 * defer into the text/binary accumulators, downgrade a stored file to the
 * generic form on policy violation, and the cheap head-sniff behind the
 * UNCOMPRESSIBLE retry predicate. WP14a: bz_defer is the binary half. */
/* WP16a (the definition lives with the codecpack exec layer, below) */
/* WP14b M2 (definitions live with the container creators, below) */

/* WP21 (definition with the checkpoint machinery, below) */

/* ---- name index ---------------------------------------------------- */

uint64_t idx_hash(const char *s, size_t n);

/* Remember where inode `id` lives. The last record for an id wins, matching
   the old scan, which kept walking and overwrote rec_pos on every hit. */
void idx_put_id(invfs_volume *v, uint64_t id, uint64_t pos);

/* 0 = unknown; callers fall back to a scan */
uint64_t idx_get_id(invfs_volume *v, uint64_t id);

/* live name-index entries currently pointing at this id (the rename fast
   path shares one id between two names; the retire path reads this to keep
   a survivor's mappings) */
uint32_t idx_id_live(const invfs_volume *v, uint64_t id);

/* add `delta` to the live count of every directory prefix of `name`:
   "a/b/c.txt" bumps "a/" and "a/b/"; the anchor "a/" bumps "a/" itself,
   which is what keeps an empty directory visible */
void idx_bump_dirs(invfs_volume *v, const char *name, size_t nlen,
                          int delta);

/* record `name` as live under `id`. Replacing an existing name keeps the
   directory counts untouched -- it is still one live name. */
void idx_put(invfs_volume *v, const char *name, size_t nlen,
                    uint64_t id, uint64_t pos, uint64_t size, uint64_t ctime);

/* A tombstone kills only ITS version of the name: the sweep appends the
   replacement record BEFORE the tombstone for the old inode, so a blind
   delete-by-name would drop the newer file. Mirrors the old vol_find scan. */
void idx_del(invfs_volume *v, const char *name, size_t nlen,
                    uint64_t id);

/* v2 position kill: remove the entry whose record lives exactly at `pos`,
 * whatever its id. Same-id metadata rewrites chain versions under one name,
 * so a plain id match would kill the NEWEST version instead of the one the
 * tombstone names. Falls back to nothing -- callers keep the id path for
 * legacy (file_size==0) tombstones. */
void idx_del_at(invfs_volume *v, const char *name, size_t nlen,
                       uint64_t pos);
const name_index_entry *idx_get(invfs_volume *v, const char *name,
                                       size_t nlen);
uint64_t idx_dir_count(invfs_volume *v, const char *pre, size_t plen);
int idx_dir_live(invfs_volume *v, const char *pre, size_t plen);

/* Persist (rd != NULL) or clear (rd == NULL) the RDP0 descriptor by
 * read-modify-write of the whole block 0. vol_write_sb only ever writes
 * the 144-byte struct at offset 0, so the reserved tail survives state
 * flips; on a raw device the full-block write is what the alignment
 * demands anyway. The in-memory copy follows the disk state. */
int vol_write_rdp0(invfs_volume *v, const invfs_rdp0 *rd);

/* ---- WP21: CKP0 sweep-checkpoint descriptor --------------------------
 * Same block-0 RMW convention as RDP0 (above). The descriptor makes a
 * sweep checkpoint findable without scanning the inode area for the
 * "\x01reten" owner, and pins the two append pointers the journal staging
 * can roll the volume back to. */

/* CRC convention: over the full descriptor with the crc32c field read as
 * zero (the RDP0 rule). */
uint32_t ckp0_crc(const invfs_ckp0 *ck);

/* WP24-lite (definition with the checkpoint machinery in vol_rollback.c):
 * read-only replay of the live checkpoint's staged journal prefix -- the
 * vol_rollback twin that never writes. Verifies the staged bytes exactly
 * the way rollback's phase 1 does (slot-header sniff, whole-image CRC,
 * chained log walked to its end / legacy bare-CRC walk), then folds them
 * into the in-memory L2P: the table afterwards describes precisely the
 * checkpoint cut. Also validates the descriptor bounds the caller's
 * cut-scan relies on. 0 = ok, -3 = descriptor/staging failed verification
 * (the volume's on-disk state was never touched), -1 = io/alloc error. */
int ckp_stage_replay(invfs_volume *v);

/* (Re)allocate the dirty bitmap and mark every shadow block: the state
 * before that moment is simply not tracked, so the next reseal must be a
 * full pass. */
void seal_dirty_reset(invfs_volume *v);

/* Mark the shadow-zone blocks [pba, pba+n) dirty (their stripes need a
 * parity recompute at the next reseal). No-op outside the shadow zone or
 * when no seal config exists (the bitmap is NULL then). */
void seal_dirty_mark(invfs_volume *v, uint64_t pba, uint64_t n);

/* ---- WP18: offline resize roll-forward (descriptor: invarifs.h RSZ0) ---
 * invf-resize (resize.c) cannot move the metadata payload atomically: the
 * bitmap grows into the journal's head whenever total_blocks crosses a
 * 32768-block boundary, so the post-resize locations overlap the metadata
 * they replace. The tool therefore stages the whole payload (bitmap + used
 * journal + used inode records) in a collision-free region, arms RSZ0 and
 * flips the superblock to RECOVERY -- and the apply runs HERE, at the next
 * open, whether that open is the tool's own or any later one after a crash.
 * The apply reads only the staging area, so re-entering it after a crash
 * mid-apply is safe; the commit (new superblock + cleared descriptor, one
 * block-0 rewrite) lands last. */
uint32_t rsz0_crc(const invfs_rsz0 *rz);

/* The descriptor's CRC covers the embedded superblock; these are the
 * invariants the rest of vol_open relies on, verified before anything is
 * written (a descriptor failing here is ignored, never applied).
 * WP25: takes the volume for the device geometry. */
int rsz0_sane(invfs_volume *v, const invfs_rsz0 *rz);

/* Apply an armed resize from its staging area; commit on success.
 * Returns 0 with v->sb already the new superblock, -1 on failure. */
int vol_rsz0_apply(invfs_volume *v, const invfs_rsz0 *rz);

/* Replay the L2P journal from disk into the in-memory table and reseed the
 * WP19 hot summaries; sets v->journal_pos to the end of the valid prefix.
 * vol_open runs this once; the WP21 rollback runs it again after restoring
 * the checkpoint's staged journal bytes. The table allocation grows but
 * never shrinks. Returns 0, -1 on allocation failure. */
int l2p_replay(invfs_volume *v);
/* apply one journaled entry to the in-memory table (replay path; no op
 * journaling). Returns -1 on allocation failure. */
int l2p_apply(invfs_volume *v, const invfs_l2p_entry *e);
/* reseed the WP19 hot summaries from the current in-memory table (the
 * l2p_replay/ckp_stage_replay shared tail) */
void l2p_seed_heat(invfs_volume *v);
/* Persist the superblock. The checksum covers bytes 0..0x7B, and `state`
   lives at 0x18 -- inside that range -- so it has to be recomputed here.
   It was not, which was harmless only for as long as nothing inside the
   checksummed range ever changed: the ENOSPC policy fields and the READONLY
   flag sit at 0x80 and beyond deliberately. The moment `state` starts moving
   (which is the whole point of crash detection) a stale checksum turns the
   volume unopenable -- vol_open rejects it with err -5. */
int vol_write_sb(invfs_volume *v);

/* ================= crash consistency =================
 *
 * Three rules, and each one closes a hole that was open before:
 *
 * 1. DIRTY before the first mutation, CLEAN after the last flush in
 *    vol_close. Nothing ever set DIRTY, so `state` was decoration: mkfs wrote
 *    CLEAN, no mount contradicted it, and a volume that died mid-write opened
 *    as though nothing had happened.
 *
 * 2. Maps durable before the record that needs them. An inode record names
 *    its data by *segment index* -- invfs_ast_block_entry.block_id is the L2P
 *    key, not a physical block -- so physical addresses exist only in the
 *    journal. A record that is durable while its L2P is not is not merely
 *    stale, it is unreadable and unrebuildable: fsck reports l2p_miss and has
 *    nothing to reconstruct the mapping from. Data blocks themselves are
 *    already durable (io_write at allocation time); only the bitmap and the
 *    journal were lazy, and invf-sweep flushed once at the *end of the whole
 *    run* -- so an interrupted sweep lost every file it had transcoded.
 *
 * 3. Tombstones are the exception: they must be durable BEFORE the frees they
 *    authorize, never after, or a crash leaves a live record whose blocks are
 *    free and reusable. vol_delete_inode already had this right (io_write for
 *    the tombstone, bitmap dirtied in RAM only), so deletes mark DIRTY
 *    without flushing.
 *
 * What "durable" buys depends on the backing store. A device is opened
 * FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH (blkio.c), so write ordering
 * there survives power loss. An image file is buffered, so ordering survives
 * process death -- which is what the crash tests inject -- but power loss
 * needs an actual barrier; INVFS_FSYNC=1 adds one at close.
 */
int vol_mark_dirty(invfs_volume *v);

/* Call before appending an inode record: mark dirty, then make the maps
   durable so the record about to land is backed by something readable. */
int vol_pre_record(invfs_volume *v);

/* WP22c/F1: a flush/sync failed -- some buffered writes may never reach
 * the device. Re-anchor the inode-area append cursor at the last position
 * a successful barrier pinned and latch the volume (needs_recovery), so
 * nothing appends into a known-broken tail and every later mutation fails
 * loudly until remount + recovery. Idempotent. */
void vol_io_error_latch(invfs_volume *v, const char *what);

/* allocate n consecutive free blocks in one extent (the raw/shadow/arena
 * pba range); returns start block or 0. WP-DZ: extents are the advisory
 * policy geometry; content-class PREFERENCE lives one level up, in
 * alloc_raw_or_shadow (raw class) -- everything else allocates its
 * canonical extent directly (shadow class never crosses into the raw
 * extent: the seal stripes are defined over the shadow pba range). */
uint64_t alloc_blocks(invfs_volume *v, uint64_t zone_start, uint64_t zone_len,
                              uint64_t n, int use_reserve);
uint64_t alloc_raw_or_shadow(invfs_volume *v, uint64_t nblocks, int *zone_out);

/* H5: release the hard_min space latch (VOLF_READONLY) once free space is
 * back above hard_min + 2% of the volume. Runs from vol_free_blocks, the
 * fsck bitmap rebuild and vol_open; logged, persisted by the next flush. */
void vol_readonly_unlatch(invfs_volume *v);

/* remove all mappings for (inode, lba) from the in-memory table and
 * journal the UNMAP op (append-only; l2p_remove_mem skips the op for
 * replay) */
void l2p_remove(invfs_volume *v, uint64_t inode, uint64_t lba);
void l2p_remove_mem(invfs_volume *v, uint64_t inode, uint64_t lba);

/* ---- WP-L2Q session L2P index ----------------------------------------
 * All no-ops when the index is disabled (v->l2p_idx == NULL). put
 * overwrites the slot of an existing key (newest wins); reslot rewrites
 * the stored table slot of a key only when it currently names old_slot
 * (a compaction moving a NON-newest duplicate must not touch the index);
 * del removes the key (cluster rehash); reset empties the index (replay
 * restart); rebuild derives it from the current table (fsck/seal bulk
 * rewrites). */
void l2p_idx_reset(invfs_volume *v);
void l2p_idx_put(invfs_volume *v, uint64_t inode, uint64_t lba,
                 uint64_t slot);
void l2p_idx_del(invfs_volume *v, uint64_t inode, uint64_t lba);
void l2p_idx_reslot(invfs_volume *v, uint64_t inode, uint64_t lba,
                    uint64_t old_slot, uint64_t new_slot);
void l2p_idx_rebuild(invfs_volume *v);
/* the live entry for the key, NULL when unmapped (or the index is off) */
const invfs_l2p_entry *l2p_idx_get(invfs_volume *v, uint64_t inode,
                                   uint64_t lba);

/* WP22d: queue one journal op for the next flush's append (MAP/UNMAP,
 * CRC restamped from the chain at write time; a MAP op's pad is re-read
 * from the live table at write time) */
int jrn_push_op(invfs_volume *v, const invfs_l2p_entry *e);

/* absolute byte offset of slot `slot`'s header block */
uint64_t jrn_slot_base(const invfs_volume *v, uint32_t slot);

/* WP22d: remember a heat-only change for the next flush (appended as a
 * refresh MAP, or folded into the compaction image when the pending set
 * overflows) */
void jrn_heat_touch(invfs_volume *v, uint64_t inode, uint64_t lba);
/* a MAP's heat was set in the table right after its op was queued (the
 * create paths): keep the queued op's pad in sync when it is the tail,
 * else queue a refresh */
void jrn_pad_sync(invfs_volume *v, const invfs_l2p_entry *e);

/* WP22d consistent-cut mapset: the set of (inode,lba) keys the replayed
 * journal maps. Built once per open/fsck; O(1) membership. */
typedef struct {
    uint64_t inode, lba;          /* inode==0 = empty */
    const invfs_l2p_entry *e;     /* the newest entry with this key */
} mapset_ent;
typedef struct { mapset_ent *tab; size_t mask; } mapset;
int  mapset_build(const invfs_volume *v, mapset *ms);
int  mapset_has(const mapset *ms, uint64_t inode, uint64_t lba);
/* the newest live entry for the key, NULL when unmapped */
const invfs_l2p_entry *mapset_get(const mapset *ms, uint64_t inode,
                                  uint64_t lba);
void mapset_free(mapset *ms);

/* WP22d consistent-cut scan set: per-name version stack built by the
 * open/fsck inode-area scan. A version is "broken" when some AST segment
 * has no mapping in the replayed journal (a drop window ate it). The live
 * version of a name is the newest non-broken one; a position-kill DELT
 * removes its version UNLESS that version's successor is broken (a torn
 * retire transaction: keep the fallback), and a DELT naming the newest
 * version ever seen kills the name outright (a user delete must not
 * resurrect older versions). */
typedef struct {
    uint64_t pos, id;
    uint64_t size, ctime;
    uint64_t miss;        /* AST segments without a mapping (0 = valid) */
    uint8_t  broken;      /* miss != 0, or the header failed to parse */
} scan_ver;
typedef struct scan_name {
    struct scan_name *next;
    scan_ver *vers;
    uint32_t nvers, capvers;
    uint32_t nlen;
    char name[1];
} scan_name;
typedef struct {
    scan_name **buck;
    size_t mask, count;
} scan_set;
int  scanset_inod(scan_set *ss, const char *name, size_t nlen,
                  uint64_t id, uint64_t pos, uint64_t size, uint64_t ctime,
                  uint64_t miss);
void scanset_delt(scan_set *ss, const char *name, size_t nlen,
                  uint64_t id, uint64_t killpos);
/* newest non-broken version, NULL when the name is lost */
const scan_ver *scanset_live(const scan_name *e);
void scanset_free(scan_set *ss);
/* count the record's AST segments missing from the mapset; fills the
 * first few lost ranges for the loud log. Returns the miss count. */
uint64_t rec_l2p_miss(const uint8_t *rec, uint32_t rec_len, uint64_t inode_id,
                      const mapset *ms, uint64_t *miss_off, uint64_t *miss_len,
                      unsigned *miss_n);

/* +1 read-heat on the live mapping for (inode,lba), once per session.
 * Read-only sessions (READONLY flag / awaiting recovery) accrue nothing.
 * WP-L2Q: RAM-only -- read touches never queue journal refreshes and never
 * dirty the volume; the pads persist inside the next compaction image
 * (sweep-decay granularity). The lookup mirrors vol_lookup_entry (the
 * session index answers it O(1)); every later touch of the pair in this
 * session is absorbed by the session set. */
void heat_touch_read(invfs_volume *v, uint64_t inode, uint64_t lba);

/* max write-heat over an inode's live mappings (0 = cold/none) */
uint8_t heat_file_maxw(invfs_volume *v, uint64_t inode);

/* set write-heat on every live mapping of an inode (the rewrite carry) */
void heat_file_setw(invfs_volume *v, uint64_t inode, uint8_t w);

/* grab/restamp one mapping's heat around an l2p_remove+vol_map remap
 * (dedupe): heat keys on (inode,lba), not on the physical slot, so a pba
 * remap must carry it over rather than rebirth the entry cold */
void heat_grab(invfs_volume *v, uint64_t inode, uint64_t lba,
                      uint16_t *r, uint8_t *w);
void heat_stamp(invfs_volume *v, uint64_t inode, uint64_t lba,
                       uint16_t r, uint8_t w);

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
                                size_t payload, uint64_t phys_blocks);

/* 1 while a session for `name` is mid-flight: sweeps (all entry points)
 * skip such a file. The session has forked the file's segment layout --
 * a transcode retiring the old record mid-session would drop the old
 * id's L2P maps the session's aliases resolve through, and the in-place
 * sweep path would free blocks the aliases still name. */
int vol_write_active_name(invfs_volume *v, const char *name);

/* Parse + validate an EXER payload. Disk input, never trusted: magic, part
 * count (1..EXE_MAX_MEDIA), ascending non-overlapping ranges inside
 * [0, file_size), and the glue must account for every byte the parts do
 * not cover. codecid is INVFS_ALGO_JXL or INVFS_ALGO_ZSTD; anything else
 * fails loudly (a newer encoder wrote it). Returns 0 and fills rows[] or
 * -1 on any violation. */
int exer_payload_parse(const uint8_t *pay, size_t pay_len,
                              uint64_t file_size,
                              exer_row *rows, size_t cap, size_t *n_out);

/* Splice an EXER payload's glue + the decoded part buffers into dst
 * (file_size bytes). Rows come pre-validated from exer_payload_parse, so
 * the glue arithmetic cannot overrun: parts are ascending, inside the
 * file, and member_sum + glue_len == file_size. */
void exer_splice(const uint8_t *pay, const exer_row *rows, size_t n,
                        uint8_t *const *parts, uint8_t *dst, uint64_t file_size);

/* ---- WP20: framed-segment read with seal recovery ------------------------
 * Every shadow-zone segment read funnels through here: [4B csize LE]
 * [4B crc32c(payload)] at pba, payload right behind the header, plen = the
 * segment's physical block count from the L2P (0 = unknown: bounds check
 * skipped, the historical behaviour). min_csize is the smallest legal
 * payload (a batch's [usize][props] head needs 6, a plain segment 1, an
 * empty recipe 0). A segment whose csize is out of bounds is corrupt by
 * construction (writers always allocate ceil((csize+8)/4096) blocks), so
 * the bounds check costs the happy path nothing.
 *
 * A shadow-zone segment that fails the bounds/CRC check gets ONE recovery
 * attempt from the WP20 seal parity (seal_recover_segment) before the
 * original failure propagates; the recovered payload is verified against
 * the segment's own framed CRC, so a failed parity reconstruction can
 * never surface as good bytes. RAW-zone segments are not sealed and fail
 * straight away. 0 = ok (*blob_out malloc'd, *csize_out bytes), -1 =
 * unreadable (parity could not help or absent). */
int seg_read_checked(invfs_volume *v, uint64_t pba, uint64_t plen,
                            uint32_t min_csize, uint32_t *csize_out,
                            uint8_t **blob_out);
int vol_read_inode(invfs_volume *v, uint64_t inode_id, unsigned depth,
                          uint8_t **out, size_t *out_len);
int sweep_enospc(invfs_volume *v, uint64_t need_bytes);
uint16_t tz_codec_gen(uint32_t algo);

/* WP10 §12.2: the JXL codec's decode working set from cheap JPEG headers,
 * never a trial decode. Walks the marker stream for SOF0/SOF1/SOF2
 * (0xFFC0-0xC2; baseline/extended/progressive) and returns ~w*h*3 -- the
 * pixel buffer djxl materializes before writing the JPEG back out.
 * 0 = geometry unknown (the caller admits the file and lets cjxl try). */
uint64_t jpeg_raw_estimate(const uint8_t *j, size_t n);
int vol_sweep_file_inner(invfs_volume *v, uint64_t inode_id,
                                int generic_only);

/* Zone of an inode's first AST segment, or -1 if it cannot be read.
   RAW means "never swept". Needed because vol_read_file hands back
   DECODED bytes: a PMP inode still looks exactly like an MP3 to a magic
   test, so without this a re-sweep would transcode it again -- costing
   seconds per file and rewriting flash for no gain. The other container
   codecs dodge this by checking for their sibling "!recipe" inode; a PMP
   blob has no sibling, so it checks the zone directly. */
int vol_inode_first_zone(invfs_volume *v, uint64_t inode_id);

/* WP14b: defer the parts of a just-exploded extraction container
 * ("name!partN", N = 0..) into the batching accumulators, so a TAR swept in
 * this run has its members batched by THIS run's vol_tz_flush instead of
 * sitting one run in per-file ZSTD. The flush re-reads and re-sniffs each
 * part from its live record, so a head sniff is enough here; parts that
 * sniff as nothing stay per-file generic (the absent-stamp walk branch
 * reconsiders them next run). */
void defer_container_parts(invfs_volume *v, const char *name);

/* WP12(b): JPEG upgrade retry for a file the class predicate just re-armed
 * (GENERIC_MEMLIMIT: the raised dec_mem limit admits it; GENERIC_GUARD: a
 * newer codec generation). By the time either stamp exists the file is
 * stored as generic BINARY segments, so the RAW-gated pack loop in
 * vol_sweep_one could never see it again -- this runs the SAME attempt on
 * the current (decoded) content. WP16e: the attempt is the jxl codecpack's
 * (vol_pack_sweep over the registry entry for INVFS_ALGO_JXL, which IS the
 * pack once it is loaded): probe -> estimate admission -> cjxl encode ->
 * djxl decode-back memcmp guard -> new blob inode first, retire the old one
 * after, meta carried across.
 * Outcomes (vol_pack_sweep's): transcode -> CODEC{JXL, pack gen} on the new
 * id, returns 100+algo; still over the limit -> GENERIC_MEMLIMIT re-stamped
 * at the current generation, 0; guard refusal -> GENERIC_GUARD{JXL, current
 * gen}, 0 (a stale gen would refire the retry on every sweep); pack not
 * loaded, its tools absent, or the volume full (DEFER_ENOSPC) -> stamp
 * untouched, the file waits, 0. -1 only on a read failure. */
int vol_jxl_retry(invfs_volume *v, uint64_t inode_id, const char *name);

/* ---- WP14b M2: exe-as-container carve (sweep side) ----
 *
 * A binary-family file (ELF/PE/Mach-O per invfs_binary_family) is scanned
 * for embedded media (exe_scan_media: validated JPEG/PNG streams, each
 * >= 16 KiB, at most EXE_MAX_MEDIA). Admission: the media must be worth
 * carving (sum of member lengths >= 64 KiB AND >= 5% of the file) and the
 * read-time working set (decoded payload + member bytes, ~usize + parts)
 * must fit the decode-memory policy. Then per member: JPEG transcodes via
 * the WP11 machinery (cjxl --lossless_jpeg=1, djxl decode-back memcmp
 * guard; a refusal skips just that member -- its bytes stay in the glue),
 * PNG is recompressed ZSTD-19 only (no pixel transcode can be bit-exact
 * while PNGR is Windows-only). The house invariant is checked as a whole
 * before anything reaches disk: ZSTD-decompress the final blob, splice
 * the decode-proven members at the table offsets, memcmp against the
 * original -- any mismatch abandons the carve.
 *
 * Storage: parts land first as "name!exrN" whole-file blob inodes
 * (algo=JXL / algo=ZSTD), then the main record replaces the file as one
 * EXER segment (children-first, the FLAC note), then the old record is
 * tombstoned and the classes stamped (main CONTAINER{EXER}, JXL parts
 * CODEC{JXL}, ZSTD parts GENERIC{ZSTD}).
 *
 * Returns 1 = carved (the caller reports rc 11), 0 = declined (caller
 * falls through to binary batching), 2 = decode-memory refusal
 * (GENERIC_MEMLIMIT{EXER} stamped; the caller must NOT batch -- the retry
 * re-arms from generic storage when the limit rises, the JXL pattern). */
int vol_exer_carve(invfs_volume *v, uint64_t inode_id,
                          const char *name, const uint8_t *full,
                          size_t full_len, uint32_t *nparts_out);

/* Delete every "name!..." sibling of `name`. Returns how many were retired.
 *
 * A transcoded file keeps its payload in sibling records the read path
 * resolves BY NAME -- "name!recipe", "name!coverN", "name!partN", "name!jxl".
 * Removing or replacing the file it belongs to left those behind: they are
 * valid live records under names nothing reaches any more, so fsck counts them
 * live, sweep skips them as internals, and no pass ever frees them. A 366 KB
 * FLAC overwritten by 15 bytes of text stranded a 36 KB !recipe permanently.
 *
 * Collect the names first: vol_delete_inode appends a tombstone, so a live
 * scan would walk into records it had just written. */
int vol_delete_siblings(invfs_volume *v, const char *name);

/* A transcode that gives up partway has already committed some of its
   children. Their names ("name!partN", "name!recipe", "name!coverN") stay live
   records that no pass can reach: sweep skips internal '!' names and fsck
   counts them as live files, so nothing will ever free them. Purge them on
   every abort -- including the "not smaller, keep the original" verdict, which
   for tar/gz is reached only after the parts are already down, so the cheapest
   possible outcome was silently the most expensive one. */
uint64_t vol_transcode_abort(invfs_volume *v, const char *name);

/* locate the ext inside a raw record; NULL when absent (v1 record) */
const uint8_t *meta_locate_ext(const uint8_t *rec, size_t rec_len,
                                      size_t *ext_len_out);

/* read the latest live record for an inode id; returns malloc'd buffer and
 * optionally its name/position. Walks forward from the index hint so stale
 * hints degrade to a full-area scan instead of wrong answers. */
int meta_read_record_by_id(invfs_volume *v, uint64_t inode_id,
                                  uint8_t **buf_out, uint32_t *rl_out,
                                  char *name_out, size_t name_cap,
                                  uint64_t *pos_out);
int tz_defer(invfs_volume *v, uint64_t inode_id, const char *name,
                    uint64_t size, uint32_t family);

/* WP14a: defer an executable (binary-family) file into the binary
 * accumulator; vol_tz_flush seals it into shared ZSTD(+BCJ) batches. */
int bz_defer(invfs_volume *v, uint64_t inode_id, const char *name,
                    uint64_t size, uint32_t family);

/* ---- owner inode ---- */

/* Load the owner record: entries, ext, position. Absent owner => *o zeroed
 * and o->pos = 0 (caller creates it). Returns 0 on success. */
int tz_owner_load(invfs_volume *v, uint64_t owner, tz_owner *o);
void tz_owner_free(tz_owner *o);

/* Append [INOD(owner, entries)][DELT(position-kill previous)] as one write.
 * The owner keeps its inode id so the owner L2P maps stay valid. Entry
 * file_offsets are rebuilt as the cumulative concatenation of the given
 * entries (so the owner itself stays readable as the concatenation of its
 * live batches, and GC repacks the same way). The name is a parameter so
 * the WP20 seal owners ("\x01parity*") reuse the exact same pattern. */
int tz_owner_write(invfs_volume *v, uint64_t owner, const char *name,
                          tz_owner *o);

/* ---- small policy helpers used by the walk predicate ---- */

/* Cheap head sniff across the registry (UNCOMPRESSIBLE retry gate): one
 * 8 KB window, registry order (= sniff priority), any positive answer
 * qualifies. */
int tz_sniff_any(invfs_volume *v, uint64_t inode_id, const char *name);

/* Read the usize of a TEXT member's batch segment without decoding it
 * (WP10 §6: "store decoded unit sizes to speed this up" -- the [4B usize]
 * sits right behind the 8-byte framing). 1 = batch exceeds unit_limit. */
int tz_member_oversized(invfs_volume *v, uint64_t inode_id,
                               uint64_t unit_limit);

/* Decode and re-store through the generic per-segment path, then stamp
 * cls{calgo}. Two callers:
 *  - policy downgrade (WP10 §6, both directions): stamps GENERIC_MEMLIMIT
 *    {codec} -- NOT bare GENERIC, because the limit is the reason, and
 *    MEMLIMIT is what makes the re-sweep after a RAISED limit find the
 *    file again (§2 table);
 *  - WP19 heat promotion: a read-hot PPMd batch member is extracted to
 *    standalone per-segment ZSTD and stamped GENERIC{ZSTD} (its terminal
 *    form -- a latency upgrade out of the shared batch, not a policy
 *    retry candidate).
 * The old record's blocks retire as usual; TEXT members' batch blocks
 * survive via the retire gate and their L2P dups vanish with the old
 * record (the batch keeps a hole -- GC/compactor territory). */
int vol_store_generic(invfs_volume *v, uint64_t inode_id,
                             const char *name, uint8_t cls, uint8_t calgo);



/* Does this record own "name!..." siblings that must die with it? A
 * ZIP-style container lists AST children; the extraction containers
 * (TARR/GZR/PNGR/FLACR/EXER) carry num_children == 0 but keep their
 * payload in sibling inodes ("name!partN", "name!recipe", "name!jxl",
 * "name!coverN", "name!exrN") the read path resolves by name -- deleting
 * only the anchor strands them as live records nothing reaches (verified:
 * a TAR's parts survived vol_unlink). The sibling walk is O(area), so
 * plain files skip it.
 * 1 = siblings possible (unknown record -> 1: scan conservatively). */
int record_owns_siblings(const uint8_t *rec, uint32_t rl);

/* layer-2 stripes per owner record: one AST entry per parity block */
uint64_t seal2_shard_stripes(uint32_t m2);
void seal_view_free(seal_view *sv);

/* the seal's data-membership rule: occupied in the bitmap, never in the
 * exclusion set (parity of both layers + the retention registry) */
int seal_excluded(const seal_view *sv, uint64_t b);
int seal_view_load(invfs_volume *v, seal_view *sv);

/* A repaired segment image must re-prove itself against its own framing:
 * [4B csize][4B crc32c] and crc32c over the csize payload bytes. crc == 0
 * segments carry no check and can never be recovered-verified. */
int seal_seg_verify(const uint8_t *seg, uint64_t plen,
                           uint32_t *csize_out);

/* One recovery attempt for a framed segment whose read just failed. Reads
 * the whole segment (plen blocks), tries the stripe-syndrome repairs and
 * returns the verified payload (malloc'd, *csize_out bytes). On success the
 * restored block is written back when the volume is writable and the
 * recovery is logged. -1 = no recovery: the caller's original error. */
int seal_recover_segment(invfs_volume *v, uint64_t pba, uint64_t plen,
                                uint32_t *csize_out, uint8_t **blob_out);

/* Rebuild the layer-2 stripe -> parity-pba map from the journal (newest
 * wins). par has n_stripes*m2 slots: stripe*s slot j at par[s*m2+j]. */
void seal2_map_load(const invfs_volume *v, const seal_view *sv,
                           uint64_t shard_stripes, uint32_t m2,
                           uint64_t n_stripes, uint64_t *par);
void ret_shard_name(uint64_t shard, char *out, size_t cap);
void alloc_state_reset(invfs_volume *v);

#ifndef _WIN32   /* WP11 POSIX-only tool plumbing */
int tool_tmpdir(char *dir, size_t cap);
void tool_rm(const char *dir, const char *name);

/* write a whole buffer, creating/truncating; 0 on success */
int tool_write(const char *path, const uint8_t *data, size_t len);
#endif
int run_tool(const char *exe, const char *a1, const char *a2, const char *opts);

/* djxl: JXL blob -> PNG -> parse -> unfilter -> pixels. Returns 0 on ok. */
int invfs_png_from_jxl(invfs_volume *v, uint64_t jxl_inode,
                              uint8_t **rgb, size_t *rgb_len);

/* miniz tdefl wrapper: zlib-wrapped deflate with level 1..10 */
int mz_tdefl_compress(const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t out_cap, int level,
                             size_t *out_len);


/* create inode storing one blob (JXL/APE/...) as a single segment */
uint64_t vol_create_blob_file(invfs_volume *v, const char *name,
                                     const uint8_t *blob, size_t blob_len,
                                     uint64_t orig_size, uint32_t algo);
void cpack_map_cache_reset(invfs_volume *v);

/* Retiring `name` kills the map cached for it, and retiring a "name!..."
 * sibling (a member, the table, the map itself) kills the container's:
 * the next read reloads from the current records. */
void cpack_map_cache_invalidate(invfs_volume *v, const char *name);

/* The WP16b read entry point: serve [off, off+len) of a seekable container
 * by local splice through its cached map. Returns the byte count (clamped
 * at the container size), -1 on any failure -- loud, like a corrupt member
 * on the exec path. */
int cpack_map_read(invfs_volume *v, const char *name, uint64_t ino,
                          uint64_t container_size, uint64_t off,
                          uint8_t *dst, size_t len);


/* WP16a sweep attempt: decompose one RAW container through a container
 * codecpack. See the section header for the pipeline; the return
 * convention mirrors vol_pack_sweep (100+algo on commit, 1 = tools absent
 * -- wait RAW and unstamped, 0 = declined: fall through to text/generic;
 * GENERIC_MEMLIMIT is stamped on a policy refusal, everything else leaves
 * the stamp to the generic path below). */
int vol_containerpack_sweep(invfs_volume *v, uint64_t inode_id,
                                   const char *name, const invfs_codec *pc,
                                   const uint8_t *full, size_t full_len);

/* WP16a read side: rebuild the original container from the recipe blob +
 * the member siblings. Returns 0 and fills dst (want_len bytes) exactly,
 * -1 on any failure (loud: a missing/corrupt member, a bad table, or a
 * pack error all mean the file cannot be served). */
int pack_container_rebuild(invfs_volume *v, const invfs_codec *pc,
                                  const char *name,
                                  const uint8_t *recipe, size_t recipe_len,
                                  uint8_t *dst, size_t want_len);

/* ---- WP25: two-device volumes ----
 * (the mux prototypes live with the io_* macros above the struct;
 *  vol_ndev/vol_degraded/vol_mirror_stale/vol_tier_count/vol_rawm_count
 *  are public in volume.h) */

/* CRC convention: over the full descriptor with the crc32c field read as
 * zero (the RDP0 rule). */
uint32_t devt_crc(const invfs_devt *d);

/* Persist the DEVT descriptor by read-modify-write of block 0 (the RDP0
 * convention), bumped sync_seq and all. vol_flush calls this LAST on
 * 2-device volumes: the seq bump is the mirror commit record (a device the
 * write skipped keeps the older seq -> detected as stale at the next
 * open). */
int vol_write_devt(invfs_volume *v);

/* The volume reads with dev0 absent (degraded mount): every metadata
 * structure comes from the dev1 mirror, all canonical data reads serve
 * from dev1 (canonical shadow placement + the RAW mirror), ops needing
 * dev0 fail loudly. State lives in v->degraded (public probes in
 * volume.h). */

/* Load/rebuild the tier + rawm indexes from their owner records (once per
 * open, after the inode scan built the indexes). */
void wp25_index_load(invfs_volume *v);

/* Flush-time owner-record sync for dirty tier/rawm indexes (the maps are
 * durable first -- jrn_flush ran; the owner record names only durable
 * ordinals). */
int  wp25_owner_sync(invfs_volume *v);

/* The vol_free_blocks hook: a REAL free (post-retention) of a raw-zone pba
 * drops its dev1 mirror; a free of a canonical dev1-shadow pba drops its
 * dev0 acceleration copy (and frees the copy's arena blocks). */
void wp25_on_free(invfs_volume *v, uint64_t pba, uint64_t nblocks);

/* fsck -f rebuild hook: after the used-bitmap rebuild, drop index entries
 * whose canonical key or copy block is no longer allocated (the rebuild
 * never runs vol_free_blocks, so dangling copies would otherwise survive
 * with their backing handed out from under them). */
void wp25_fsck_prune(invfs_volume *v);

/* Read-failover helpers behind seg_read_checked: the dev1 mirror of a
 * raw-zone segment (0 = none), and the dev0 acceleration copy of a
 * canonical dev1 segment (0 = none). */
int  wp25_rawm_lookup(invfs_volume *v, uint64_t raw_pba,
                      uint64_t *mpba_out, uint64_t *plen_out);
int  wp25_tier_lookup(invfs_volume *v, uint64_t cpba,
                      uint64_t *dpba_out, uint64_t *plen_out);

/* vol_tier_migrate (the WP25 rule-9 sweep pass) is public in volume.h. */

/* Mirror one freshly written raw-zone segment onto dev1 (write path of
 * write_segment_blocks / vol_write_raw). 0 = the mirror landed (or is not
 * needed), -1 = dev1 failed (the caller latches). */
int  wp25_rawm_write(invfs_volume *v, uint64_t pba, const uint8_t *buf,
                     uint64_t phys_blocks);

#endif /* INVFS_VOLUME_INTERNAL_H */
