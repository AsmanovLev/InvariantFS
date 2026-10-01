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
#if __has_include("miniz.h")
#include "miniz.h"
#elif __has_include("../codecs/miniz.h")
#include "../codecs/miniz.h"
#endif

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
#include <pthread.h>
#endif

#include "invarifs.h"
#include "volume.h"
#include "vol_walk.h"
#include "blkio.h"
#include "arc.h"
#if __has_include("lz4.h")
#include "lz4.h"
#elif __has_include("../codecs/lz4.h")
#include "../codecs/lz4.h"
#endif
#include "zstd.h"
#include "zlib.h"
#if __has_include("ppmd_codec.h")
#include "ppmd_codec.h"
#elif __has_include("../codecs/ppmd_codec.h")
#include "../codecs/ppmd_codec.h"
#endif
#if __has_include("codec.h")
#include "codec.h"
#elif __has_include("../codecs/codec.h")
#include "../codecs/codec.h"
#endif
#if __has_include("bcj_x86.h")
#include "bcj_x86.h"
#elif __has_include("../codecs/bcj_x86.h")
#include "../codecs/bcj_x86.h"
#endif
#if __has_include("blake3.h")
#include "blake3.h"
#elif __has_include("../codecs/blake3.h")
#include "../codecs/blake3.h"
#endif
#if __has_include("rs.h")
#include "rs.h"
#elif __has_include("../codecs/rs.h")
#include "../codecs/rs.h"
#endif


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
#if __has_include("pngx.h")
#include "pngx.h"
#elif __has_include("../recipes/pngx.h")
#include "../recipes/pngx.h"
#endif

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
int  vmux_pread(struct invfs_volume *v, uint64_t off, void *buf, size_t len);
int  vmux_pwrite(struct invfs_volume *v, uint64_t off, const void *buf, size_t len);
void vmux_close(struct invfs_volume *v);
/* 0 = both present devices barriered; 1 = dev0 failed (skipped, continues
 * on dev1, resync at next open); -1 = dev1 failed (caller latches). */
int  vmux_barrier(struct invfs_volume *v, const char *what);

#define io_seek(c, off)       vmux_seek(v_of_blk(c), (off))

#define io_read(c, buf, len)  vmux_read(v_of_blk(c), (buf), (len))

#define io_write(c, buf, len) vmux_write(v_of_blk(c), (buf), (len))

#define io_pread(c, off, buf, len)  vmux_pread(v_of_blk(c), (off), (buf), (len))

#define io_pwrite(c, off, buf, len) vmux_pwrite(v_of_blk(c), (off), (buf), (len))

#define io_close(c)           vmux_close(v_of_blk(c))


/* ---- volume ---- */
/* The in-memory name/dir/id indexes are GONE. They existed to answer a
   name lookup without re-reading the append-only inode area on every call
   (the quadratic-mount problem they were built for). That area is gone, and
   with it the indexes: names resolve through the dirent B+-tree
   (vol_v3_path_lookup and friends), a directory's liveness through the
   dirents themselves, and an inode's segments through its recipe blob.
   WP-M21 had already reduced the whole set to no-op stubs; these are the
   declarations that kept the call sites compiling. */

/* WP27: per-file heat session state. The durable home is the record's
 * "invfs.heat" xattr TLV (see invarifs.h); in RAM, read touches accrue
 * per-inode (the read path never touches the journal any more) and fold
 * into the records at vol_close / the sweep's decay pass. heat_tab is an
 * open-addressing map inode -> accrued read count this session ([0]=inode,
 * 0 = empty slot; inode ids start at 1). */
/* WP27: the PB7 answer without the L2P: a session-scoped reference map
 * pba -> number of live records' AST entries naming it. Built lazily on
 * first use (retire/dedupe) from the live name-index set, then maintained
 * incrementally by the record append/kill hooks (pba_ref_apply). Only
 * non-owner records' non-TEXT entries count: those are the self-owned
 * segments; batch members reference owner-owned batches (never freed by
 * the member's retire), and owner records free through the owner WAL. */
typedef struct { uint64_t pba; uint32_t n; } pba_ref_ent;

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
     * bitmap -- 480 KB on a 14.65 GB volume -- on every file close, and a
     * WINDOWS device is opened FILE_FLAG_NO_BUFFERING|WRITE_THROUGH, so that
     * was a synchronous ~250 ms per file on flash regardless of file size. */
    uint64_t bm_lo, bm_hi;
    uint64_t free_blocks;     /* cached free count (bitmap scan at open) */
    /* WP135: unclaimed short v3 walks (src/core/vol_walk.h). A walk that
     * stopped and that nobody committed or explicitly abandoned is an open
     * question about this volume; vol_close and the reporting surfaces read
     * and clear it. Relaxed-atomic -- see vol_walk.c. */
    unsigned VOL_WALK_LATCH_FIELD;
    uint64_t next_inode_id;
    /* WP111: the v3 inode-id allocator recovers next_inode_id from the
     * base tree + delta exactly ONCE per mount. That recovery used to be
     * gated on `next_inode_id <= INVFS_V3_ROOT_INO` -- i.e. on the counter
     * still holding its post-vol_open sentinel -- so ANY other code path
     * that incremented the counter first disabled it for the rest of the
     * mount. vol_write_begin (vol_write.c) burns an id on its first call
     * after every remount and is the FUSE write path's first id consumer,
     * so a mount whose first id-consuming operation was a write to an
     * EXISTING file never recovered, and the next create handed out an id
     * that was already live: two dirents, one inode row, the older file's
     * content silently replaced, fsck clean. Recovery is now gated on this
     * flag, which nothing else can close. */
    uint8_t  v3_id_recovered;
    /* WP-M18: the pre-fold root stored at vol_v3_fold_request start so that
     * fold_reclaim_hook (called after fold) can diff old vs new. Cleared
     * after reclaim runs. */
    invfs_blkptr fold_pre_root;
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
    /* WP85: the metadata generation a write session anchors to. Bumped by
     * every rollback (spt0_restore -- the only code path that republishes a
     * root BACKWARDS; the fold and the btree only ever move forward). A
     * session whose stamp is behind this counter was anchored to a
     * generation the volume has retired, and its segments are in no live
     * recipe: it must be refused, not re-anchored. */
    uint64_t write_gen;
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
    /* WP27: heat (the "invfs.heat" INO2-ext TLV, see invarifs.h).
     * heat_init seeds the read-heat of newly created records
     * (INVFS_HEAT_INIT, default 0). heat_any_rhot/whot are "some file sits
     * at/above the hot threshold" summaries -- they let the sweep skip the
     * promotion walk / write-hot scans on cold volumes. heat_tab is the
     * session's per-inode accrued read counts (see the typedef note).
     *
     * WP-heat-table-concurrent-safe: heat_mu guards heat_tab, heat_tab_mask,
     * heat_tab_n, heat_any_rhot and heat_any_whot. These were written with no
     * lock because the premise was "the volume layer is caller-serialized --
     * FUSE holds g_io_lock around every vol_* call", and that premise is
     * FALSE: invf_read releases g_io_lock (src/cli/fuse_fs.c:1407) and THEN
     * calls vol_read_range (:1414), under a comment that says so, and the
     * daemon runs fuse_loop_mt (:3353). Every heat_touch_read() on the read
     * path (src/core/vol_read.c:179, :582, :683, :712, :730, :1807, :2056) is
     * therefore N-at-once on one table, and heat_tab_touch() GROWS the table
     * by free()-ing the old array (src/core/vol_heat.c:68).
     *
     * heat_mu is a STRICT LEAF: every region that holds it is straight-line
     * code over these fields plus calloc/free/memset, and takes no other lock
     * and calls nothing outside libc. So the only orders ever observed are
     * g_io_lock -> heat.mu, g_delta_lock -> heat.mu, btree.mu -> heat.mu,
     * plugin.mu -> heat.mu and arc.mu -> heat.mu -- never the reverse -- and it
     * can never be the outer half of a cycle. heat_fold() deliberately drops
     * it before the per-record xattr work, because vol_get_xattr /
     * vol_set_xattr descend into the v3 tree and those locks sit above this
     * one. Read the summary flags through heat_any_rhot()/heat_any_whot(),
     * never directly. */
    pthread_mutex_t heat_mu;
    uint16_t heat_init;
    int heat_any_rhot, heat_any_whot;
    uint64_t (*heat_tab)[2];       /* [0]=inode (0=empty), [1]=accrued reads */
    size_t heat_tab_mask, heat_tab_n;
    int heat_folded;               /* close-fold already ran */
    /* WP27: pba reference map (PB7 without the L2P). pba_ref_on = the map
     * exists (built lazily by pba_ref_ensure, kept current by
     * pba_ref_apply from the record append/kill hooks). */
    pba_ref_ent *pba_ref;
    size_t pba_ref_mask, pba_ref_n;
    int pba_ref_on;
    /* WP pba-ref-v3-incremental: set when a recipe was published on a path
     * that does not adjust the map itself (every vol_v3_inode_delta_put
     * that CHANGES recipe_addr). pba_ref_ensure rebuilds from the live set
     * when it sees this, so the map can never gate a free on a count that
     * predates an inode. Without it the count reads 0 for a pba a live
     * recipe still names -- the wrong-free direction, not the leak one. */
    int pba_ref_stale;
    /* WP48: set once the id->position index has been reconciled against
     * the (authoritative) name index after a stale hint was detected.
     * Record compaction/rewrites during a session can leave the id index
     * pointing at a vacated position; one O(N) repair from the name index
     * fixes every live id, so the fallback full walk never runs per-call. */
    int id_idx_checked;
    /* WP16b: parsed !mbrmap cache (local-splice reads of seekable
     * containers). Filled on first map read of a container, invalidated
     * when its name (or a "name!..." sibling) is retired, freed at
     * vol_close.
     *
     * WP-cpack-map-copy-out: cpacks_mu guards maps, maps_n and maps_cap.
     * These were written with no lock on the premise that the volume layer
     * is caller-serialized, and that premise is FALSE for the same reason
     * it was false for ARC and for the heat table: invf_read releases
     * g_io_lock (src/cli/fuse_fs.c:1407) and THEN calls vol_read_range
     * (:1423), under fuse_loop_mt (:3362). cpack_map_get
     * (src/core/vol_cpack.c:2722) has exactly ONE caller -- cpack_map_read
     * (:2831) -- and that has three, all inside vol_read_range
     * (src/core/vol_read.c:1259, :1729, :1972), so the grow at
     * src/core/vol_cpack.c:2772-2778 is reachable ONLY from the lock-free
     * read path. The realloc invalidates every pointer into the array, and
     * a retire both frees the entry's payloads and compacts the array over
     * the hole (:2711), so an unguarded reader got freed OR SHIFTED heap
     * and cpack_map_serve returned success with another container's bytes.
     *
     * cpacks_mu is a STRICT LEAF: every region holding it is straight-line
     * code over these fields and the entry struct plus strcmp/strdup/free/
     * memset. The map LOAD runs with it DROPPED, because it calls
     * vol_find / vol_read_file / cpack_recipe_seg and the v3 tree, delta and
     * plugin locks sit above it. So the only orders ever observed are
     * g_io_lock -> cpacks.mu, vol_btree -> cpacks.mu,
     * vol_plugin_client -> cpacks.mu, arc.mu -> cpacks.mu and
     * heat.mu -> cpacks.mu -- never the reverse. g_io_lock is `static` to
     * src/cli/fuse_fs.c:32, so no core file can take it. */
    pthread_mutex_t cpacks_mu;
    /* An array of POINTERS to individually malloc'd entries, not an array
     * of entries. That is the whole point: a grow reallocs this array and
     * frees the old block, so ANY pointer into it dangles -- and even a
     * reader that resolved a reference under the lock could not then hand
     * that reference back, because the address it holds is a realloc'd
     * slot. An entry's own address never moves, so `struct
     * cpack_map_cache *` is a stable handle and the reference count on it
     * is a real lifetime, not a hopeful one. */
    struct cpack_map_cache **maps;
    size_t maps_n, maps_cap;
    /* WP-Q2R3: a 2-slot cache of LOADED recipes. A recipe address IS the
     * BLAKE3 of its bytes, so a cached blob can never go stale -- there is
     * nothing to invalidate. It exists because one recipe LOAD costs a
     * B+ tree walk plus a hash of the whole blob, while a read costs a
     * copy: a member with a large recipe (a >1 GiB member has a multi-megabyte
     * one) read in many small ranges used to reload it every time, which is
     * quadratic. */
    struct {
        uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN];
        uint8_t *blob;
        size_t   len;
        int      used;
    } rcache[2];
    pthread_mutex_t rc_mu;
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
    /* retain_release exempts the save-point machinery's own deliberate frees
     * (the reclaim pass, the tier arena's own compaction) from the data pin
     * in vol_free_blocks -- without it a capture could never collect the
     * blocks it is collecting. */
    int retain_release;
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
     * must fail loudly. 0 = off. WP80: the hook runs on v3 too; v3 has no
     * v2 inode area, so there the zeroing is a no-op and the latch + the
     * refused mutations are what is exercised. */
    uint64_t sync_fail_at;
    /* Test hook (tools/test-sweep-flushfail.sh), same shape as
     * sync_fail_at: when nonzero, the Nth vol_flush of this process
     * latches an I/O error and reports -1 without touching the image.
     * It exists so a caller that OWNS an exit code -- invf-sweep's
     * durability point -- can be shown failing it deterministically.
     * The pre-existing ways to fail a flush (a full volume, a read-only
     * image) all fail EARLIER in a sweep, so they never reach the final
     * vol_flush at all. 0 = off. */
    uint64_t flush_fail_at;
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
    /* WP52: the owners' dedicated mapper extents (1-based; 0 = none). The
     * per-flush owner rewrite reuses/frees these instead of allocating a
     * fresh extent each time. */
    uint64_t tier_owner_ext, rawm_owner_ext;
    /* last vol_tier_migrate run's counters (the sweep driver prints) */
    uint64_t tier_promoted, tier_demoted, tier_blocks;
    /* WP30: dynamic metadata extent state */
    invfs_met0 met0;                /* MET0 descriptor (loaded from disk) */
    int met0_present;               /* MET0 was present at open */
    /* WP59: codec-policy descriptor (PCK0 at block-0 offset 0x3C4) */
    invfs_pck0 pk;                  /* PCK0 descriptor (loaded from disk) */
    int pk_present;                 /* PCK0 was present and CRC-valid at open */
    int pk_gate_failed;             /* mount gate refused (missing codecs) */
    char pk_gate_msg[256];          /* gate refusal reason */
    uint64_t *meta_mapper;          /* in-memory mapper table cache (16384 entries) */
    size_t meta_mapper_n;           /* number of valid entries */
    uint64_t meta_active_extent;    /* index of active extent */
    uint64_t meta_active_offset;    /* byte offset within active extent */
    uint64_t meta_free_blocks;      /* metadata free block counter */
    uint8_t *meta_type_bitmap;      /* per-block type (DATA=0/META=1), allocated */
    /* ---- WP-M2: metadata-v3 base-page state (design §12, D3) ----------
     * rt30 is the RT30 root-area descriptor as loaded from block 0 and
     * CRC-validated (mbuf_rt30_load); rt30_present is 1 exactly when that
     * validation passed. The descriptor's root_slot[2] is the double-slot
     * base root and seq is the monotone publication generation; recovery
     * picks the slot whose page validates and carries the higher gen
     * (mbuf_root_read). v3_probe_rt30 reports this same state for the
     * skeleton log line (it used to do its own read of block 0, which made
     * a volume that had just recovered off the ANC0 tail anchor also print
     * "presenting an empty namespace"); wiring it to mbuf_rt30_load belongs to the
     * WP that makes v3 writable, because volume.c is outside WP-M2's file
     * scope. The allocator's bootstrap cursor hands out the two root-area
     * pages WP-M1 reserved after the mapper table before falling through
     * to the free-space pool; mb_alloc_cursor/fail_run are the per-pool
     * cursors mirroring the v2 raw/shadow pair. */
    invfs_rt30 rt30;
    int      rt30_present;
    /* ---- ANC0 tail anchor (see vol_anchor.c) --------------------------
     * The second LOCATION for rt30 and spt0, at total_blocks - 1, readable
     * without touching block 0. anchor_state is the open-time probe verdict
     * (INVFS_ANCHOR_*): INVFS_ANCHOR_OK only when the tail block really is
     * this volume's anchor, and every path that would refresh the mirror is
     * gated on it -- so on a volume created before the anchor existed, whose
     * last block is ordinary data, nothing here ever writes a byte there.
     * anchor_adopted records that THIS open took the mirror instead of block
     * 0 (and has already said so out loud); anchor_refresh_failed latches
     * when a refresh write failed, so invf-fsck can keep reporting a volume
     * that is running without its second copy. */
    int      anchor_state;            /* INVFS_ANCHOR_* */
    int      anchor_adopted;          /* 1 = recovered off the anchor */
    int      anchor_refresh_failed;   /* sticky: a refresh write failed */
    invfs_anc0 anchor;                /* the probe's descriptor, on OK */
    /* WP-M5: the metadata-v3 base-tree engine has been brought up on this
     * handle (mbuf_init called once). mbuf_init resets the bootstrap
     * cursor, so it must not run per operation. */
    int      v3_mbuf_ready;
    uint64_t mb_boot_cursor;        /* next reserved root-area pba */
    uint64_t mb_boot_end;           /* one past the reserved root-area pages */
    uint64_t mb_alloc_cursor;       /* free-space cursor within the meta zone */
    uint64_t mb_alloc_fail_run;     /* smallest proven-unavailable run (0=?) */
    /* ---- WP-M10: metadata-v3 delta-log state (recent tier) ------------
     * The append-only delta segment chain holds coalescing namespace
     * mutations; delta_index maps a namespace key to the winning record
     * {segment, offset, seq} (D1: append log + in-memory index). Mount
     * replays the chain into the index (vol_delta_mount), bounded by the
     * fold cadence (D2). Overlay reads are WP-M11 and fold is WP-M14; this
     * WP appends, indexes and replays only. delta_index is opaque here so
     * volume_internal.h needs no vol_delta.h include. delta_bump is the
     * next free byte in the active segment; on a fresh segment it starts
     * at INVFS_DELTA_SEG_HDR_LEN. All counters are per-handle diagnostics
     * and are not persisted. */
    struct delta_index *delta_index;   /* coalescing key index (NULL = none) */
    uint64_t delta_seg_pba;            /* active (newest) segment pba (0=none) */
    uint64_t delta_seg_gen;            /* highest segment seq seen/allocated */
    uint64_t delta_bump;               /* next free byte in the active segment */
    uint64_t delta_seq;                /* monotone record sequence counter */
    uint64_t delta_records;            /* live records indexed */
    uint64_t delta_segments;           /* segments in the chain */
    uint64_t delta_bytes;              /* record payload bytes appended */
    int      delta_ready;              /* mount replay done for this handle */
    /* ---- WP-M14: fold trigger accounting (D2) --------------------------
     * The design gives no numeric thresholds; vol_fold.c measures replay
     * latency and fixes them (recorded there and in the WP-M14 doc). These
     * are the raw observations the trigger consults: the sequence number of
     * the oldest live delta record and the handle clock timestamp of its
     * append (or of mount replay, for records that predate this session).
     * Both restart at 0 when the delta is reset, exactly like delta_seq. */
    uint64_t delta_oldest_seq;         /* seq of the oldest live record (0=none) */
    uint64_t delta_oldest_when;        /* CLOCK_MONOTONIC seconds of that record */
    /* ---- WP-M16: v3 save-point state (SPT0 descriptor) --------------- */
    /* savepoint_live: SPT0 was present and CRC-valid at open. */
    /* pinned_root: the base_root pba captured at save-point creation. */
    /* The pin is permanent until the save point is dropped or rolled back; */
    /* folds while a save point is live do NOT update it. */
    invfs_spt0 spt0;                   /* SPT0 descriptor (loaded from disk) */
    int      savepoint_live;            /* SPT0 was present and CRC-valid */
    invfs_blkptr pinned_root;           /* base_root at capture (zeroed = none) */
    /* ---- WP96: v3 save-point DATA pin (SPN0 descriptor) ---------------- */
    /* SPT0 pins {base_root, delta_end} and nothing else, so a sweep that
     * re-encodes a file publishes a new recipe and frees the old segments --
     * which a later invf-rollback republishes over somebody else's bytes. The
     * pin is the v3 re-expression of WP21's retmap: spn_bitmap is a bitmap
     * over total_blocks (one bit per block, lazily loaded) whose set bits are
     * the blocks the live save point's recipes name. spn_armed mirrors the
     * SPN0 descriptor's ARMED bit and is what vol_free_blocks consults, so
     * the hold is a VOLUME property (a FUSE write's retire path sees it too),
     * not a property of the sweep that armed it. spn_pba/spn_blocks name the
     * on-disk run; it is freed only by the next capture, which first reclaims
     * the blocks the previous generation held and no live recipe names.
     * spn_nopin is the debug kill switch (INVFS_SPT0_NOPIN=1): the pin is not
     * taken, so the sweep frees as before and only spt0_restore's data check
     * (WP96 layer 2) stands between a rollback and silent corruption. */
    uint8_t *spn_bitmap;
    uint64_t spn_pba;
    uint64_t spn_blocks;
    uint64_t spn_npinned;
    int      spn_armed;
    int      spn_nopin;
    /* WP137: how many blocks the LAST capture's spn_reclaim actually freed
     * (0 when that capture had no previous window to discharge). That is the
     * only DIRECT measurement of debt -- blocks a live window holds that no
     * live recipe names -- because debt is CREATED during a pass and can
     * only be OBSERVED at the next pass's capture. spt0_reclaim_last() is
     * how the FUSE watermark ladder tells "the fill is high because the data
     * is live" from "the fill is high because the window that pass just
     * armed is holding the generation it replaced"; see fuse_sweep_thread. */
    uint64_t spn_reclaim_freed;
    /* the save point's log geometry at capture (SPN0): chain length and the
     * then-head segment. The pinned-state walk needs them to place the
     * delta_end cut, and the restore re-checks that the head is still
     * reachable -- a fold resets the chain, and then the pinned state is
     * unrecoverable, which must be a refusal rather than a lossy rollback. */
    uint64_t spn_delta_segs;
    uint64_t spn_delta_head;
    /* WP30 Phase 6+: rwlock protecting meta_mapper and MET0 state.
     * Readers hold shared lock (pthread_rwlock_rdlock); writers hold
     * exclusive lock (pthread_rwlock_wrlock). Protects: meta_mapper_get,
     * meta_mapper_set, meta_mapper_flush, meta_met0_persist, and all
     * mutation paths in meta_get_append_pos. */
    pthread_rwlock_t meta_lock;
    /* ---- WP126: orphan-collector work state (see vol_btree.c) ---------
     * WP121's collector cost O(allocated blocks) block reads per call and
     * fold_reclaim_hook calls it on EVERY fold, which is what keeps
     * INVFS_RECLAIM_ORPHANS default-off. This is the state that makes the
     * per-call cost proportional to the metadata the collector can
     * possibly free, instead of to the size of the volume.
     *
     * orph.pba/orph.n is the CANDIDATE SET: the blocks this handle
     * believes are allocated v3 base pages. It is fed by two sources and
     * nothing else:
     *   (a) btree_orphan_note_alloc() at every mbuf_alloc() site that
     *       produces a base page -- exact, for anything allocated in this
     *       session;
     *   (b) a one-pass scan of the allocation bitmap ("the seed pass"),
     *       which is what finds the pages a PREVIOUS session allocated.
     *       The seed pass is budgeted and cursor-carrying, so it costs a
     *       bounded number of reads per fold and stops for good once it
     *       has wrapped the block space.
     * orph.inlist is the O(1) membership index for that set (one bit per
     * block), so note_alloc cannot append a duplicate. All of it is
     * calloc-zeroed per open and is rebuilt from nothing; nothing here is
     * persisted and nothing here is trusted for a free decision. A stale
     * or missing entry costs a leaked page, never a wrong one. */
    struct invfs_orphan {
        uint64_t *pba;
        size_t    n, cap;
        uint8_t  *inlist;       /* membership bit per block, like `bitmap` */
        uint64_t  seed_cursor;  /* next block the seed pass will examine */
        int       seed_done;    /* the seed pass has wrapped the space once */
        uint64_t  budget;       /* seed-pass reads per call; 0 = default */
        /* diagnostics (not persisted, not load-bearing) */
        uint64_t  calls, cand_reads, seed_reads, mark_reads;
        uint64_t  freed, cands, peak;
        /* settled: the last call found the seed pass wrapped and freed
         * nothing, OR hit a state it can never leave (no RT30, no bitmap,
         * a slot it refuses to reason about). It is what bounds the
         * offline drain's loop, and it is set on EVERY return path --
         * a drain that only checked "freed == 0" would spin forever on a
         * volume whose RT30 names no root at all. */
        int       settled;
    } orph;
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
   a name running past its end and into the AST recipe behind it. The v2
   rename path was the only one that checked. Names are validated on the way
   in now, so an over-long one is refused rather than silently truncated to a
   record that no longer describes the file it points at.

   INVFS_MAX_NAME is declared in volume.h so name-building callers can check
   before they start; this asserts it still matches the field it describes. */
typedef char invfs_name_fits[
    (INVFS_MAX_NAME + 1 == (int)INVFS_NAME_CAP) ? 1 : -1];


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

/* One block extent the v3 batch registry (the hidden TZ_OWNER_NAME file) still
 * claims. The registry is an owner of blocks that is NOT a recipe: a batch no
 * live recipe names any more is dead, but its registry row outlives the batch
 * until the sweep's tz GC runs. Every other free path must therefore treat a
 * registry-named block as still claimed, or the row becomes a stale pointer
 * into the free pool and the next allocator may hand the block to somebody
 * else (see the spn_reclaim comment in vol_spt0.c). */
typedef struct {
    uint64_t pba;      /* head block of the batch segment */
    uint64_t phys;     /* blocks in the extent, as the registry recorded it */
} tz_v3_extent;

/* The registry's block extents as a malloc'd ASCENDING array (distinct
 * batches hold distinct, non-overlapping extents, so a binary search on pba
 * is exact). 0 = ok. *out may come back NULL with *n == 0: no registry, or an
 * empty one -- the ordinary case, NOT a failure. -1 = out of memory or a
 * malformed volume, which every caller must treat as "claim everything" (fail
 * closed). The caller frees *out. */
int tz_v3_reg_owned_blocks(invfs_volume *v, tz_v3_extent **out, size_t *n);


/* owner inode state, loaded once and rewritten per change */
typedef struct {
    invfs_ast_block_entry *ents;
    uint32_t n, cap;
    uint8_t *ext;            /* INO2 ext blob, carried verbatim (usually none) */
    uint32_t ext_len;
    uint64_t pos;            /* current record position (position-kill target) */
    uint64_t ext_idx;        /* WP52: this owner's dedicated mapper extent
                              * on a mapper volume (0 = none yet); reused and
                              * freed on supersede so owner rewrites do not
                              * accumulate dead extents */
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

/* Does this name occupy the internal '!' namespace?
 *
 * '!' separates a container's own name from the sibling inodes its payload is
 * stored in: "x.tar" -> "x.tar!part0", "x.tar!recipe", "x.zip!mbr0000-a.txt".
 * The lanes mint those names by APPENDING "!<suffix>" to the name they were
 * given (vol_cpack.c:1232 :1266 :1369 :1403 :1537 :1780 :1923 :1925 :2902
 * :2903 :3000 :3245 :3641 :3683 :3838 :3864 :3923, vol_exer.c:266 :431,
 * vol_png.c:442 :734, vol_sweep.c:250 :458 :516), and the read path finds them
 * by appending the same suffix (vol_read.c:810 :827 :887 :962 :1072 :1168
 * :1204 :1584). It is therefore a RESERVED byte, and a user name carrying one
 * is not merely odd-looking: it is a name the read path will parse as a
 * container path (vol_read_named, vol_read.c:1390) and the unlink cascade
 * will try to purge as siblings (vol_delete_siblings, vol_records.c:250).
 *
 * WP135: for years nothing reserved it, so `a!b` was a legal user file and
 * `rm a` DESTROYED it -- the purge's "is this my sibling" test was a prefix
 * match on "name!" with no shape check -- while reporting success. The
 * reservation is checked here, at the name-introduction sites
 * (vol_v3_create_node, vol_v3_mkdir, vol_v3_rename, vol_v3_hardlink,
 * vol_write_begin), and NOT at lookup: a volume that already holds such a
 * name stays readable and unlinkable, so this is a restriction on creating
 * one, never a way to strand a file that is already there.
 *
 * The whole name is checked, not just the leaf. A '!' in a DIRECTORY
 * component cannot collide with a sibling -- the suffix is appended at the end
 * of the whole path -- but several predicates in the tree test the whole name
 * for a '!' (vol_dirs.c:662 skips the cascade, vol_sweep.c:687 :707 :761
 * :806 :1036 skip the lanes, vol_textzone.c:686, tools/invf-sweep.c:163 :244
 * :826). Allowing one and not the others would leave those predicates reading
 * a directory component as "internal" and silently skip the cascade or the
 * sweep for a real file. Reserved means reserved.
 *
 * COST, stated plainly: a tree containing a file or directory named with a '!'
 * -- legal on Linux -- can no longer be imported, and FUSE refuses to create
 * or rename one. That is a namespace restriction, and it is the price of the
 * alternative, which is silent destruction. Nothing shipped depends on it: the
 * only place a foreign string is interpolated into a sibling name,
 * cpack_mbr_name (vol_cpack.c:1917), runs it through cpack_sanitize
 * (vol_cpack.c:1902), which folds every byte outside [A-Za-z0-9._-] to '_' --
 * so a member called "a!b" becomes "a!mbr0000-a_b". tools/invf-sweep.c:340
 * and :414 build a "%s!%s" path only to RE-RENDER a name that already exists
 * for the dashboard; they mint nothing. */
static inline int name_is_internal_ns(const char *name)
{
    return name && strchr(name, '!') != NULL;
}

/* The same predicate, and it says why. Kept beside name_too_long, which
 * also refuses a name and explains itself -- a silent 0 from a create is
 * indistinguishable from ENOSPC or a corrupt volume to whoever called it, and
 * an operator who is told "rename it without the '!'" can act on it. */
static inline int name_refused_internal_ns(const char *name)
{
    if (!name_is_internal_ns(name)) return 0;
    fprintf(stderr, "invarifs: '!' is reserved: it separates a container from "
            "its internal siblings ('x.tar' -> 'x.tar!part0'), so the name "
            "'%.72s' is refused. Rename it without the '!'.\n", name);
    return 1;
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

/* Widen the dirty byte range to cover the byte holding bit i, so a flush
 * writes just that slice instead of the whole bitmap. This was three
 * byte-identical private copies (volume.c's bm_dirty, vol_delta.c's
 * dl_bm_dirty, vol_metabuf.c's mb_bm_dirty); one definition, next to the
 * bit_* helpers it is the same kind of primitive as. */
static inline void vol_bm_dirty(invfs_volume *v, uint64_t i)
{
    uint64_t byte = i / 8;
    if (v->bm_lo > v->bm_hi) { v->bm_lo = byte; v->bm_hi = byte + 1; return; }
    if (byte < v->bm_lo) v->bm_lo = byte;
    if (byte + 1 > v->bm_hi) v->bm_hi = byte + 1;
}

/* THE v3 key ordering. One definition, three users: the base B+-tree's
 * search/insert/split and page-range check (vol_btree.c), the delta log's
 * index and ordered range cursor (vol_delta.c), and the fold's deterministic
 * pre-apply sort (vol_fold.c). Those three were byte-identical private
 * statics; the delta/base merge is correct only while all three agree, and
 * no compiler, linker or test could see them disagree.
 *
 * The order, precisely: unsigned byte-lexicographic over the whole key,
 * shorter-first on a strict prefix. There is no type tag in the comparison
 * itself. The namespaces sort because their leading bytes do
 * (INVFS_V3_XATTR_KEY_PREFIX 0x03, INVFS_V3_RECIPE_KEY_PREFIX 0x04 --
 * invarifs.h:1337,1349), and because every field after a tag is fixed-width
 * BIG-ENDIAN, byte order over the key IS numeric order on the inode ids,
 * name lengths and BLAKE3 digests inside it.
 *
 * The shorter-first tiebreak is load-bearing, not cosmetic: a value too
 * big for one page record is stored under its canonical key with the
 * continuations appended -- a recipe as 0x04 || addr vs
 * 0x04 || addr || 0x00 || idx (vol_btree.c:3771), an xattr as its name key
 * vs name || 0x00 || chunk:u16 BE (vol_btree.c:2813) -- so in both cases
 * the canonical key is a strict PREFIX of its own continuations, and both
 * must come out adjacent and in that order for the B+-tree's separator and
 * range logic (vol_btree.c:1486, :1710, :1738, :1743) to bound the subtree
 * correctly.
 *
 * Returns <0, 0 or >0 -- never a raw memcmp value. */
static inline int vol_key_cmp(const uint8_t *a, uint16_t an,
                              const uint8_t *b, uint16_t bn)
{
    uint16_t m = an < bn ? an : bn;
    int c = m ? memcmp(a, b, m) : 0;
    if (c)
        return c < 0 ? -1 : 1;
    if (an < bn)
        return -1;
    if (an > bn)
        return 1;
    return 0;
}

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

/* WP59a: is this inode anchored?  Checks the "invfs.anchor" INO2-ext xattr
 * (presence = anchored, value ignored).  Used by sweep, dedup and tier to
 * skip pack/container transcoding, dedup remap and tier demotion. */
static inline int invfs_inode_is_anchored(invfs_volume *v, uint64_t inode_id)
{
    uint8_t val;
    size_t vlen = sizeof(val);
    return vol_get_xattr(v, inode_id, INVFS_XATTR_ANCHOR,
                         &val, &vlen) == 0 && vlen >= 1;
}

/* Persist (rd != NULL) or clear (rd == NULL) the RDP0 descriptor by
 * read-modify-write of the whole block 0. vol_write_sb only ever writes
 * the 144-byte struct at offset 0, so the reserved tail survives state
 * flips; on a raw device the full-block write is what the alignment
 * demands anyway. The in-memory copy follows the disk state. */
int vol_write_rdp0(invfs_volume *v, const invfs_rdp0 *rd);


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
 * What "durable" buys depends on the backing store AND the platform -- see
 * the full statement in src/core/vol_crash.c, which is the authority. In one
 * line: on Windows a raw device is opened unbuffered/write-through, so write
 * ordering is the handle's own; on POSIX, image file AND raw device alike, the
 * open is a plain O_RDWR and the only durability point is fsync() in
 * blkio_flush, so a barrier buys process-death survival and power-loss
 * ordering is delegated to the host filesystem and device. WP80: the barrier
 * before CLEAN is the default everywhere -- vol_close barriers before it
 * writes the CLEAN superblock (INVFS_CLOSE_NOBARRIER=1 is the documented
 * opt-out), and vol_sync does the same on v3 as on v2.
 */
int vol_mark_dirty(invfs_volume *v);

/* Call before appending an inode record: mark dirty, then conditionally flush
   when watermarks are hit (WP29: 80 % inode area or 50 % journal slot).
   vol_close / vol_sync still guarantee a full flush. */
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
                              uint64_t n, int use_reserve, int type);
uint64_t alloc_raw_or_shadow(invfs_volume *v, uint64_t nblocks, int *zone_out);

/* WP30: dynamic metadata extent allocation
 * type parameter: 0 = DATA blocks, 1 = METADATA blocks */
#define INVFS_ALLOC_DATA  0
#define INVFS_ALLOC_META  1

/* H5: release the hard_min space latch (VOLF_READONLY) once free space is
 * back above hard_min + 2% of the volume. Runs from vol_free_blocks, the
 * fsck bitmap rebuild and vol_open; logged, persisted by the next flush. */
void vol_readonly_unlatch(invfs_volume *v);


/* ---- WP30: metadata extent mapper ----------------------------------------- */
int meta_mapper_load(invfs_volume *v);
int meta_mapper_flush(invfs_volume *v);
uint64_t meta_mapper_get(const invfs_volume *v, size_t i);
void meta_mapper_set(invfs_volume *v, size_t i, uint64_t entry);

/* WP30 Phase 3: dynamic metadata extent append path */
uint64_t alloc_meta_extent(invfs_volume *v, uint8_t size_class);
int extend_meta_extent(invfs_volume *v, uint64_t extent_idx, uint8_t new_size_class);
uint32_t meta_met0_crc(const invfs_met0 *m);
int meta_met0_persist(invfs_volume *v);
int meta_get_append_pos(invfs_volume *v, uint64_t rec_size,
                        uint64_t *pba_out, uint64_t *offset_out);

/* WP52: append position for the large owner records. On a mapper volume the
 * record is placed in its OWN dedicated, size-classed extent (never the
 * shared file-record cursor), so per-flush owner rewrites cannot overflow an
 * extent or force the record stream into a flush storm. Falls back to
 * meta_get_append_pos on a legacy volume. rc 0 = ok, -1 = error, -2 = ENOSPC. */
int meta_get_owner_append_pos(invfs_volume *v, uint64_t rec_size,
                              uint64_t *ext_slot, uint64_t *pba_out,
                              uint64_t *offset_out);

/* Bug J companion: route every record-en-area append through the mapper.
 * On a v0.3.0+ mapper volume this wraps meta_get_append_pos and returns
 * the absolute byte position in *rec_pos_out (the caller then io_seek()
 * there, writes, bumps v->met0.active_offset + v->inode_area_pos).
 * On a legacy (format_version=0) volume it falls back to inode_area_pos
 * so the old linear inode area keeps working. rc 0 = ok, -1 = error
 * (mapper missing / record does not fit its extent), -2 = ENOSPC.
 *
 * Flush-safety (WP52): meta_get_append_pos sizes the active extent (or
 * allocates/extends one) to hold the whole record, so the returned slot
 * never runs past its extent and the caller's write stays inside a single
 * backing device; the mapper/MET0 persistence it may do is an ordinary
 * io_write that never recurses into vol_flush. Safe from wp25_owner_sync
 * (itself called from vol_flush) and from tz_owner_write. */
int vol_append_slot(invfs_volume *v, uint64_t rec_size,
                    uint64_t *rec_pos_out);

/* WP52: owner-record append slot. Like vol_append_slot, but on a mapper
 * volume it uses a dedicated extent and leaves the file-record cursor
 * (inode_area_pos / met0.active_offset) untouched. rc 0 = ok, -1 = error,
 * -2 = ENOSPC. */
int vol_append_owner_slot(invfs_volume *v, uint64_t rec_size,
                          uint64_t *ext_slot, uint64_t *rec_pos_out);


/* ---- WP27: segment extents --------------------------------------------
 * AST entries carry pba but no physical length (the 32B wire format has
 * no room); a framed segment's extent is derivable from its own 8-byte
 * header ([4B csize][4B crc32c] at pba -> plen = ceil((csize+8)/4096)).
 * seg_extent derives (0 on success; -1 when the header is unreadable or
 * csize is out of volume bounds). seg_extent_checked additionally
 * cross-checks that every block of the derived run is marked used in the
 * in-memory bitmap -- the destructive-free gate: a torn header degrades
 * to a leak (fsck reclaims), never an over-free. */
int seg_extent(invfs_volume *v, uint64_t pba, uint32_t *csize_out,
               uint64_t *plen_out);
int seg_extent_checked(invfs_volume *v, uint64_t pba, uint64_t *plen_out);

/* ---- the segment framing contract, in ONE place ------------------------
 * "Does this stored frame back this recipe entry?" -- `hdr` is the csize the
 * frame header on disk claims, `len` is the length the entry claims. 0 = the
 * frame is well-formed for that algo, -1 = it is not.
 *
 * This used to live ONLY in the read loops, which made every OTHER decoder of
 * a stored segment -- the sweep's per-segment recompress, above all -- a
 * second, weaker implementation of the same contract. The sweep's version
 * memcpy'd `len` bytes out of a `hdr`-byte allocation, then compressed,
 * CRC'd, allocated and wrote the result back: a corruption the read path
 * refused came back as a file that reads successfully with wrong bytes.
 * Declared here so the sweep CALLS this instead of re-deriving it. */
int ast_frame_ok(uint8_t algo, uint32_t hdr, uint64_t len,
                 const char *algo_name);

/* ---- WP27: pba reference map (PB7 without the L2P) --------------------
 * pba_ref_ensure builds the map from the live name-index set (idempotent,
 * cheap after the first build); pba_ref_apply walks a just-written or
 * just-killed record buffer and adjusts counts (no-op until the map
 * exists); pba_ref_count answers "how many live entries name this pba". */
int  pba_ref_ensure(invfs_volume *v);
void pba_ref_apply(invfs_volume *v, const uint8_t *rec, uint32_t rec_len,
                   int delta);
void pba_ref_modify(invfs_volume *v, uint64_t pba, int delta);
uint32_t pba_ref_count(invfs_volume *v, uint64_t pba);
/* drop the map; the next pba_ref_ensure rebuilds it from the live set */
void pba_ref_reset(invfs_volume *v);
/* WP pba-ref-v3-incremental: the v3 hook pair. vol_v3_inode_delta_put calls
 * pba_ref_invalidate when it publishes a DIFFERENT recipe_addr (the one
 * place every v3 recipe publish funnels through); the two callers that
 * adjust the map themselves (the sweep's segment remap, dedupe's remap)
 * call pba_ref_validate afterwards. pba_ref_ensure rebuilds whenever the
 * flag is set, so a site that forgets to invalidate can only cost a walk,
 * never a free on a count that predates an inode. */
void pba_ref_invalidate(invfs_volume *v);
void pba_ref_validate(invfs_volume *v);

/* WP unlink-takes-map-after-dirent-drop: span a critical section over the
 * MAP that is not a single function call.
 *
 * Every pba_ref primitive above is a read-modify-write of an array that two
 * of them REPLACE (pba_ref_free and pba_ref_grow both free(v->pba_ref)),
 * so they take g_pba_ref_mu in volume.c. That is enough for one call, and
 * not enough for a RETIRE, which is four steps with a correctness relation
 * between them: the map must be taken while the retiring inode is still
 * REACHABLE BY NAME, the name is then dropped, the row is dropped, and only
 * then is the recipe's contribution subtracted. A rebuild landing between
 * the first and the last rebuilds against a live set the pending -1 does not
 * belong to, and the count that comes back is short by exactly the
 * reference being retired -- which is a free of a live block.
 *
 * So a retire holds the map across the whole span:
 *
 *     vol_pba_ref_hold(v);
 *     pba_ref_ensure(v);               // the map, while the name is there
 *     <drop the dirent>                // the walk can no longer reach it
 *     <drop the row>
 *     vol_v3_free_recipe_blocks(...);  // the -1, against a map that held us
 *     vol_pba_ref_release(v);
 *
 * Depth-counted and built on a RECURSIVE mutex, because the retire paths
 * nest: vol_v3_unlink calls vol_delete_siblings, which calls vol_v3_unlink
 * again, so a flag would unlock early. */
void vol_pba_ref_hold(invfs_volume *v);
void vol_pba_ref_release(invfs_volume *v);

/* WP22d consistent-cut scan set: per-name version stack built by the
 * open/fsck inode-area scan. WP27: a version is "broken" when some AST
 * entry carries an invalid pba (0 or past the volume end -- block 0 is the
 * superblock, never a segment, and the record CRC covers the recipe, so a
 * landed record's pbas are trustworthy; an invalid one means the record
 * body itself was torn/rewritten underneath). The live version of a name
 * is the newest non-broken one; a position-kill DELT removes its version
 * UNLESS that version's successor is broken (a torn retire transaction:
 * keep the fallback), and a DELT naming the newest version ever seen
 * kills the name outright (a user delete must not resurrect older
 * versions). */
typedef struct {
    uint64_t pos, id;
    uint64_t size, ctime;
    uint64_t miss;        /* AST segments with an invalid pba (0 = valid) */
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
/* WP27: count the record's AST entries whose pba is invalid (0 or past
 * total_blocks); fills the first few lost ranges for the loud log.
 * Returns the miss count. */
uint64_t rec_pba_miss(const uint8_t *rec, uint32_t rec_len,
                      uint64_t total_blocks, uint64_t *miss_off,
                      uint64_t *miss_len, unsigned *miss_n);

/* WP27 heat: +1 accrued read on the inode, once per session per inode
 * (the lba argument is kept for the call sites' shape; only the inode
 * keys the per-file counters now). Read-only sessions accrue nothing.
 * The accrual is RAM-ONLY: it folds into the record's "invfs.heat" TLV
 * at vol_close / the sweep's decay pass (the read path never touches the
 * journal or the inode area). */
void heat_touch_read(invfs_volume *v, uint64_t inode, uint64_t lba);

/* WP-heat-table-concurrent-safe: the "some file is hot" summaries are part
 * of the same guarded state as heat_tab (heat_touch_read sets heat_any_rhot
 * from a lock-free reader), so they are read through these and never
 * directly. vol_open/vol_close call heat_locks_init/heat_locks_destroy. */
int  heat_any_rhot(const invfs_volume *v);
int  heat_any_whot(const invfs_volume *v);
void heat_locks_init(invfs_volume *v);
void heat_locks_destroy(invfs_volume *v);

/* WP-cpack-map-copy-out: the parsed-!mbrmap cache's leaf mutex, same shape
 * and same reason as heat's. vol_open/vol_close call these. */
void cpack_locks_init(invfs_volume *v);
void cpack_locks_destroy(invfs_volume *v);

/* stored heat of a record (0/0 when the TLV is absent) */
int  heat_read_tlv(const uint8_t *rec, uint32_t rec_len,
                   uint16_t *r, uint8_t *w);
/* effective heat: stored + this session's accrued reads */
uint16_t heat_file_r(const invfs_volume *v, uint64_t inode);
/* max write-heat the file carries (0 = cold/none) */
uint8_t heat_file_maxw(invfs_volume *v, uint64_t inode);
/* persist a write-heat value on the file's live record (the rewrite
 * carry); no-op when the value is unchanged */
void heat_file_setw(invfs_volume *v, uint64_t inode, uint8_t w);
/* build the INO2 ext blob for a fresh/rewritten record: old ext carried
 * verbatim with the heat TLV inserted/replaced. Returns malloc'd blob
 * (NULL = keep no ext), *len_out its length. When old_ext is NULL a
 * minimal defaults ext is fabricated ONLY when there is heat to store. */
uint8_t *heat_ext_merge(const invfs_volume *v, const uint8_t *old_ext,
                        uint32_t old_len, int is_dir, uint16_t rheat,
                        uint8_t wheat, uint32_t *len_out);
/* fold the session's accrued reads into the records (vol_close / the
 * sweep's decay pass); marks the volume dirty when anything changed */
void heat_fold(invfs_volume *v);
/* remove + return the session's accrued reads of `inode` (the write-commit
 * carry moves the old id's pending touches onto the replacement record) */
uint16_t heat_session_take(invfs_volume *v, uint64_t inode);

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
int vol_write_active_id(invfs_volume *v, uint64_t inode_id);

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
 * file, and member_sum + glue_len == file_size.
 *
 * That validation bounds the GLUE cursor. It says nothing about how big
 * parts[i] is: rows[i].len is what gets copied out of it, so every caller
 * must have established parts[i] is at least rows[i].len bytes BEFORE
 * calling. It cannot be checked here -- by the time this returns the copy
 * has happened -- so the pairing is each caller's to prove. Both do:
 * vol_read.c compares against the length vol_read_file() reported, and
 * vol_exer.c's carve compares against the length it allocated. */
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

/* WP75: persist the dirty range of the v3 block bitmap (vol_btree.c). Called
 * by v3_publish before it names a page, and by vol_flush so allocations and
 * frees that land after the last publish are not dropped at close. */
int vol_v3_bitmap_flush(invfs_volume *v);

/* WP-M8: content-addressed immutable recipe blobs (vol_btree.c). */
int vol_v3_recipe_store(invfs_volume *v, const uint8_t *blob, size_t blen,
                        uint8_t addr_out[INVFS_V3_RECIPE_ADDR_LEN]);
int vol_v3_recipe_load(invfs_volume *v,
                       const uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN],
                       uint8_t **blob_out, size_t *blen_out);
/* WP-M8: recipe blob serialize/parse (vol_ast.c). */
int vol_ast_recipe_serialize(uint64_t file_size,
                             const invfs_ast_block_entry *ents, uint32_t n,
                             uint8_t **blob_out, size_t *blen_out);
int vol_ast_recipe_serialize_win(uint64_t file_size,
                                 const invfs_ast_block_entry *ents, uint32_t n,
                                 const invfs_ast_window_entry *wins,
                                 uint32_t n_wins,
                                 uint8_t **blob_out, size_t *blen_out);
int vol_ast_recipe_windows(const uint8_t *blob, size_t blen,
                           const invfs_ast_hdr *hdr,
                           const invfs_ast_window_entry **wins_out,
                           uint32_t *n_out);
uint64_t vol_v3_publish_window_inode(invfs_volume *v, const char *name,
                                     uint64_t src_inode, uint64_t src_off,
                                     uint64_t length, uint64_t src_len,
                                     uint32_t transform,
                                     uint8_t engine, uint8_t level,
                                     uint8_t mem_level, uint8_t strategy,
                                     int8_t window_bits);
int vol_ast_recipe_parse(const uint8_t *blob, size_t blen,
                         invfs_ast_hdr *hdr_out,
                         const invfs_ast_block_entry **ents_out,
                         size_t *nents_out);
int vol_v3_free_recipe_blocks(invfs_volume *v,
                             const uint8_t recipe_addr[INVFS_V3_RECIPE_ADDR_LEN],
                             uint64_t keep_pba);
void vol_v3_release_superseded_blob(
    invfs_volume *v, uint64_t inode_id,
    const uint8_t old_addr[INVFS_V3_RECIPE_ADDR_LEN]);
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

/* WP14b: the exe carve's upgrade retry, for the trap the JXL stamp above
 * also falls into: a GENERIC_MEMLIMIT{EXER} stamp is written on a
 * decode-policy refusal and the generic floor then stores the file, so the
 * RAW-gated dispatch can never see it again. Runs the same carve on the
 * current bytes (vol_exer_retry's own admission re-reads the live limit).
 * 11 = carved, 0 = still refused / nothing to carve, -1 on read failure. */
int vol_exer_retry(invfs_volume *v, uint64_t inode_id, const char *name);

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

/* WP-M21: no online compaction step remains. inode_area_make_room now
 * refuses ENOSPC on the legacy (format_version=0) inode area (which is
 * read-only on mount anyway); on v0.3.0+ it decides purely from the
 * metadata mapper (active extent's free tail OR an allocatable slot). */
int  inode_area_make_room(invfs_volume *v, uint64_t need);

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

/* The sibling-ownership RULE, on a parsed AST: does a file with this
 * header and these entries own "name!..." siblings that must die with it?
 * Both record_owns_siblings (v2 records) and the v3 write session's
 * wsession_load_old_v3 ask this one function, so an overwrite retires the
 * same siblings on both formats. 1 = siblings possible. */
int ast_owns_siblings(const invfs_ast_hdr *ah,
                      const invfs_ast_block_entry *ents, size_t nents);

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
void alloc_state_reset(invfs_volume *v);

#ifndef _WIN32   /* WP11 POSIX-only tool plumbing */
/* tool_tmpdir() now takes the byte count the job will put in the scratch
 * directory and is implemented in src/core/tool_scratch.c, which owns the
 * whole decision (roots, sizing, refusal). The prototype is repeated there
 * rather than moved: this header is what every core TU already includes. */
int tool_tmpdir(char *dir, size_t cap, uint64_t need);
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


/* WP201: the containerpack MAP branch's rollback, shared by its two failure
 * paths so the two cannot drift. Exposed (not static) so the regression test
 * in src/cli/v2rb_rollback_test.c drives the SHIPPED line rather than a copy
 * of it -- a test that re-implemented these three lines would prove nothing
 * about the code that actually runs. */
void cpack_rollback_commit(invfs_volume *v, uint64_t newino,
                           uint64_t inode_id, const char *name,
                           uint64_t old_pos, uint64_t old_size,
                           uint64_t old_ctime);

/* create inode storing one blob (JXL/APE/...) as a single segment */
uint64_t vol_create_blob_file(invfs_volume *v, const char *name,
                                     const uint8_t *blob, size_t blob_len,
                                     uint64_t orig_size, uint32_t algo);
uint64_t vol_v3_publish_blob_inode(invfs_volume *v, uint64_t inode_id,
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
int64_t cpack_map_read(invfs_volume *v, const char *name, uint64_t ino,
                          uint64_t container_size, uint64_t off,
                          uint8_t *dst, size_t len);


/* WP119: the containerpack size guard. Its four constants, and then what the
 * sweep prices a decomposition with, in the units the volume pays. The price
 * of a member's PAYLOAD is now charged where the engine actually stores it --
 * in the shared batch segments -- so the per-member price is the two things
 * a member really adds on top of the bytes it shares with its siblings:
 * CPACK_MEMBER_META (its packed rows) and CPACK_MEMBER_UNBATCHED (the block
 * rounding of the members batching does not take).
 *   CPACK_ZSTD_LANE   the generic binary lane. Every member reaches it even
 *                     with batching switched off, so charging min(zstd19,
 *                     usize) is a FLOOR, not a bet: anything the guard
 *                     accepts is a win before batching, and batching only
 *                     improves it.
 *   CPACK_MEMBER_META  per exposed member -- the engine's own bookkeeping
 *                     that is NOT the member's payload: the recipe row
 *                     (vol_btree.c), the inode row (invarifs.h), the dirent
 *                     the member is reachable by, and the delta record the
 *                     write path appends for it. All four are ROWS: three
 *                     pack into shared COW B+ tree pages and the fourth is
 *                     byte-packed into a delta segment, so none of them
 *                     takes a page of its own. The member's DATA blocks are
 *                     not here -- they are size-dependent and are priced per
 *                     member by cpack_member_data_cost() below.
 *                     MEASURED, not guessed (tools/measure-file-meta-cost.sh):
 *                     importing 2000 / 4000 / 8000 one-byte files into a v3
 *                     volume and differencing the free-block count costs
 *                     721 / 717 B per file, and at that size the payloads
 *                     batch away to nothing, so all of it is bookkeeping.
 *                     1 KiB is that rounded up to a round number.
 *                     The constant this replaces was 4 pages -- 16 KiB -- and
 *                     it was NOT a per-member measurement: it was one run's
 *                     whole-image delta residual (4,935,680 - 1,685,140 =
 *                     3,250,540) divided by that run's 201 members. That
 *                     residual is per-SWEEP overhead (the savepoint window
 *                     pins the pre-sweep generation's blocks; see AGENTS.md
 *                     2.5, which measures 447 blocks of it on a comparable
 *                     corpus), and the next bare sweep gives it back --
 *                     charging it per member forever is what made a 50,000-
 *                     member rootfs container unreachable.
 *   CPACK_MEMBER_UNBATCHED  the block rounding of the members the batching
 *                     stage does NOT take. Those are not a hypothetical:
 *                     measured on six containerpack corpora at 2,000, 4,000,
 *                     12,500, 25,000, 201 and 402 members and across 1 KiB to
 *                     64 KiB members, the fraction that batches is 94.0% to
 *                     94.4% every time (the sweep log's "N parts -> PPMd
 *                     batch" line; the residue is content that fails the text
 *                     or binary family sniff, which the generic per-file lane
 *                     then rounds block by block). 6% of a 4 KiB rounding is
 *                     ~123 B per member, charged at 256 B.
 *                     This term is the ONLY part of the per-member price that
 *                     is a measured RATE rather than a bound, because the
 *                     guard cannot know which members will batch -- the
 *                     decision is made in stage 6, long after the projection.
 *                     It is also the only thing that would be wrong if
 *                     batching regressed, and the way to price that risk is to
 *                     keep this term nonzero: at 0 the guard would accept any
 *                     container of sub-block members, which is the shape
 *                     whose batching saves it. See the red control in
 *                     src/cli/cpack_guard_test.c.
 *   CPACK_ZGAIN_MILLE the INVFS_MIN_GAIN_PCT default (0.5%) in thousandths.
 *   CPACK_REPRO_MAX   the largest single kind-2 re-deflation the read path
 *                     pays per request. A kind-2 entry stores nothing and
 *                     re-deflates its whole raw_len on every read that
 *                     touches it -- quadratic in the member size, invisible
 *                     to any content projection. 4 MiB is twice the largest
 *                     cluster qcow2 allows (cluster_bits <= 21), so no
 *                     conformant image is refused. */
#define CPACK_ZSTD_LANE     19
#define CPACK_MEMBER_META   1024ull
#define CPACK_MEMBER_UNBATCHED 256ull
#define CPACK_ZGAIN_MILLE   5ull
#define CPACK_REPRO_MAX     (4ull << 20)

typedef struct {
    uint64_t fixed;        /* recipe + member table + map */
    uint64_t content;      /* cpack_member_data_cost(sum of the members'
                             * ZSTD-19 projections): the BLOCKS the shared
                             * batch segments take, rounded up */
    uint64_t member_count;
    uint64_t member_cost;  /* member_count * (CPACK_MEMBER_META +
                             * CPACK_MEMBER_UNBATCHED) */
    uint64_t repro_max;    /* largest kind-2 re-deflation, per request */
    uint64_t repro_bytes;  /* the map's per-pass re-deflation total */
    int      content_bound;/* content is an UPPER bound, not a measurement:
                            * the per-member bookkeeping alone already lost,
                            * so no payload was compressed to price it */
} cpack_size_proj;

/* What a payload of this length costs the volume: WHOLE blocks. The allocator
 * hands out 4 KiB blocks, and the batching stage fills them from many members
 * at once (vol_textzone.c concatenates payloads into a TZ_BATCH_MAX buffer and
 * compresses the buffer as ONE unit, and the sweep log says so: "N parts ->
 * PPMd batch"), so a member's marginal DATA cost is its blocks SHARED, not
 * its own rounded up. Rounding the TOTAL up is the sound side of that: the
 * batched segments can never hold more than the sum of the ZSTD-19
 * projections, and the ~6% of members that do not batch pay their own
 * rounding, charged at CPACK_MEMBER_UNBATCHED. */
uint64_t cpack_member_data_cost(uint64_t projected_len);

int cpack_size_guard(uint64_t orig_len, const cpack_size_proj *p,
                     const char **why);


/* ---- the member-table parser (WP-cpack-max-members) ----
 *
 * The total-member bound, and with it the member index bound. These live
 * here, not in vol_cpack.c, for the same reason cpack_size_proj does: the
 * unit test is the thing that has to be able to hold the engine and the
 * bound to the same number, and a #define inside the .c is unreachable
 * from there. The eight container packs mirror the value in their own
 * sources (one .c under tools/codecpacks per pack);
 * tools/test-cpack-max-members.sh
 * asserts all nine agree, because a pack that is left behind silently
 * turns the raise into a no-op for that container type and nothing else
 * says so.
 *
 * This is a MEMORY bound, not an on-disk one. The member count never lands
 * in a header field: the member table is a TEXT sibling (one row per
 * member) and the member map blob carries a uint32_t count. Each member is
 * its own sibling inode, so the AST recipe's uint16_t num_children is 0
 * for every containerpack file. Raising this costs RAM -- 40 B/member for
 * the table, one bit/member for the duplicate-index bitmap -- and nothing
 * on disk. What actually limits a real rootfs is cpack_size_guard above.
 *
 * 2^20 is ~10x the host's whole /usr (105,880 members) and ~6x past the
 * 151,418-member break-even for a 3.5 GB /usr corpus, so in practice the
 * size guard refuses first and refuses with a reason. */
#define CPACK_MAX_MEMBERS 1048576u
#define CPACK_MAX_IDX     (CPACK_MAX_MEMBERS - 1u)

/* One row of the member table. idx names the member's "!mbr<idx>" sibling
 * inode; it is a uint32 in the member map and a decimal string in the
 * table, so it is not what bounds a container. */
typedef struct {
    uint32_t idx;
    uint64_t usize;
    char     sname[25];
} cpack_member;

/* cpack_parse_table()'s distinct return for a well-formed table that is
 * over the member bound. It used to be the same -1 as a malformed row, so
 * "this container is too big" printed the same line as "this table is
 * corrupt" and neither was diagnosable from a log. */
#define CPACK_TABLE_TOOMANY (-2)

/* Parse a member table (the enumerate output, stored verbatim as the
 * "name!mbrt" sibling). Returns 0, -1 (malformed), or CPACK_TABLE_TOOMANY
 * -- and on CPACK_TABLE_TOOMANY *n_out carries the container's REAL member
 * count, so the caller can print the number against the limit instead of
 * "more than the limit". */
int cpack_parse_table(const uint8_t *text, size_t len,
                      cpack_member **out, size_t *n_out,
                      uint64_t *sum_out);


/* WP16a sweep attempt: decompose one RAW container through a container
 * codecpack. See the section header for the pipeline; the return
 * convention mirrors vol_pack_sweep (100+algo on commit, 1 = tools absent
 * -- wait RAW and unstamped, 0 = declined: fall through to text/generic;
 * GENERIC_MEMLIMIT is stamped on a policy refusal, everything else leaves
 * the stamp to the generic path below). */
int vol_containerpack_sweep(invfs_volume *v, uint64_t inode_id,
                                   const char *name, const invfs_codec *pc,
                                   const uint8_t *full, size_t full_len);
uint32_t cpack_map_decomp_gen(invfs_volume *v, const char *name);
int vol_cpack_migrate(invfs_volume *v, uint64_t inode_id, const char *name,
                      const invfs_codec *pc);

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
 * durable first; the owner record names only durable
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
