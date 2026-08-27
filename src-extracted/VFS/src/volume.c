/*
 * volume.c — InvariantFS volume access layer
 *
 *   vol_open / vol_close
 *   vol_alloc  (bitmap free-list cursor)
 *   vol_write_raw (allocate blocks in RAW zone + write)
 *   vol_map    (L2P journal MAP entry + in-memory table)
 *   vol_read_block (read raw bytes at pba)
 *   inode area: append-only records (name + AST recipe)
 *
 * Metadata zone layout:
 *   [0 .. bitmap_blocks):            block bitmap
 *   [bitmap_blocks .. +journal):     L2P journal (append-only)
 *   [bitmap_blocks+journal .. end):  inode/AST area (append-only)
 */
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
#define deflateInit2(s, l, m, w, mml, st) deflateInit2_((s), (l), (m), (w), (mml), (st), ZLIB_VERSION, sizeof(z_stream))
#define inflateInit2(s, w) inflateInit2_((s), (w), ZLIB_VERSION, sizeof(z_stream))
extern const char *zlibVersion(void);
extern uLong crc32(uLong crc, const Bytef *buf, uInt len);

#ifdef _WIN32
#define strdup _strdup
#endif

#define SEGMENT_SIZE (64u * 1024u)  /* RAW segment granularity */

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
   ~100 call sites below read the same as before. */
#define io_seek(c, off)       blkio_seek((c), (off))
#define io_read(c, buf, len)  blkio_read((c), (buf), (len))
#define io_write(c, buf, len) blkio_write((c), (buf), (len))
#define io_close(c)           blkio_close((c))

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
   scan did. */
typedef struct id_index_entry {
    struct id_index_entry *next;
    uint64_t id;
    uint64_t pos;
} id_index_entry;

typedef struct dir_index_entry {
    struct dir_index_entry *next;
    uint64_t count;        /* live records under this prefix */
    uint32_t nlen;
    char name[1];          /* prefix including the trailing '/' */
} dir_index_entry;

/* WP10 §4: one deferred text-batching candidate. Lives only in RAM for the
 * duration of a sweep run; a crash before vol_tz_flush just leaves the file
 * RAW for the next run (invariant: nothing lost). */
typedef struct {
    uint64_t inode_id;
    uint64_t size;         /* size at defer time (sort key only) */
    uint32_t family;       /* INVFS_TEXT_FAMILY_* — the batching sort key */
    char     name[256];
} tz_candidate;

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
    size_t l2p_dirty;         /* first index whose on-disk copy is stale */
    uint64_t next_inode_id;
    /* on-demand sweep pending list (RAM) */
    uint64_t *pending;
    size_t n_pending, cap_pending;
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
    /* Crash consistency (doc/08). `dirty` remembers that the on-disk state
       has already been set to DIRTY this session, so the mark costs one
       superblock write per mount instead of one per mutation.
       `needs_recovery` is set at open when the previous session did not close
       cleanly, and refuses every mutation until vol_recover() has run. */
    int dirty;
    int needs_recovery;
    /* records that failed CRC/bounds during the open scan; >0 means the
     * volume shows real damage and must not self-recover a DIRTY state */
    uint64_t scan_anomalies;
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
} invfs_volume;

static const uint64_t JOURNAL_BLOCKS = INVFS_JOURNAL_BLOCKS;

static void l2p_remove(invfs_volume *v, uint64_t inode, uint64_t lba);
static int vol_write_sb(invfs_volume *v);

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

static int name_too_long(const char *name)
{
    size_t n = strlen(name);
    if (n <= INVFS_MAX_NAME) return 0;
    fprintf(stderr, "invarifs: name is %llu bytes, the format holds %llu: %.72s...\n",
            (unsigned long long)n, (unsigned long long)INVFS_MAX_NAME, name);
    return 1;
}

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

static int name_too_long_for_children(const char *name)
{
    return strlen(name) + INVFS_SIBLING_RESERVE > INVFS_MAX_NAME;
}

/* Store a name in a record: one clamped length drives both the field and the
   length header, so the two cannot disagree even if a caller skipped the
   check above. Assumes the record was zeroed (all callers calloc or memset). */
static void rec_set_name(invfs_inode_rec *rh, const char *name)
{
    size_t n = strlen(name);
    if (n > INVFS_MAX_NAME) n = INVFS_MAX_NAME;
    rh->name_len = (uint32_t)n;
    memcpy(rh->name, name, n);
    rh->name[n] = 0;
}

static int bit_get(const uint8_t *b, uint64_t i) { return (b[i / 8] >> (i % 8)) & 1; }

/* ---- name index ---------------------------------------------------- */

static uint64_t idx_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ULL;   /* FNV-1a 64 */
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void idx_clear(invfs_volume *v)
{
    size_t i;
    if (v->nbuck) {
        for (i = 0; i <= v->nmask; i++) {
            name_index_entry *e = v->nbuck[i];
            while (e) { name_index_entry *nx = e->next; free(e); e = nx; }
        }
        free(v->nbuck);
    }
    if (v->dbuck) {
        for (i = 0; i <= v->dmask; i++) {
            dir_index_entry *e = v->dbuck[i];
            while (e) { dir_index_entry *nx = e->next; free(e); e = nx; }
        }
        free(v->dbuck);
    }
    if (v->ibuck) {
        for (i = 0; i <= v->imask; i++) {
            id_index_entry *e = v->ibuck[i];
            while (e) { id_index_entry *nx = e->next; free(e); e = nx; }
        }
        free(v->ibuck);
    }
    v->nbuck = NULL; v->dbuck = NULL; v->ibuck = NULL;
    v->nmask = v->dmask = v->imask = 0;
    v->ncount = v->dcount = v->icount = 0;
}

/* Grow to keep the load factor near 1. Failure is not fatal: the table
   simply stays smaller and lookups get longer chains. */
static void idx_grow_names(invfs_volume *v)
{
    size_t ncap = (v->nmask + 1) * 2, i;
    name_index_entry **nb = (name_index_entry **)calloc(ncap, sizeof *nb);
    if (!nb) return;
    for (i = 0; i <= v->nmask; i++) {
        name_index_entry *e = v->nbuck[i];
        while (e) {
            name_index_entry *nx = e->next;
            size_t b = (size_t)(idx_hash(e->name, e->nlen) & (ncap - 1));
            e->next = nb[b]; nb[b] = e;
            e = nx;
        }
    }
    free(v->nbuck);
    v->nbuck = nb;
    v->nmask = ncap - 1;
}

static void idx_grow_dirs(invfs_volume *v)
{
    size_t ncap = (v->dmask + 1) * 2, i;
    dir_index_entry **nb = (dir_index_entry **)calloc(ncap, sizeof *nb);
    if (!nb) return;
    for (i = 0; i <= v->dmask; i++) {
        dir_index_entry *e = v->dbuck[i];
        while (e) {
            dir_index_entry *nx = e->next;
            size_t b = (size_t)(idx_hash(e->name, e->nlen) & (ncap - 1));
            e->next = nb[b]; nb[b] = e;
            e = nx;
        }
    }
    free(v->dbuck);
    v->dbuck = nb;
    v->dmask = ncap - 1;
}

static int idx_init(invfs_volume *v)
{
    idx_clear(v);
    v->nbuck = (name_index_entry **)calloc(1024, sizeof *v->nbuck);
    v->dbuck = (dir_index_entry **)calloc(256, sizeof *v->dbuck);
    v->ibuck = (id_index_entry **)calloc(1024, sizeof *v->ibuck);
    if (!v->nbuck || !v->dbuck || !v->ibuck) { idx_clear(v); return -1; }
    v->nmask = 1023;
    v->dmask = 255;
    v->imask = 1023;
    return 0;
}

/* 64-bit mix (splitmix64 finalizer): inode ids are sequential, so the low
   bits alone would pile every id into one bucket after a grow */
static uint64_t idx_mix(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static void idx_grow_ids(invfs_volume *v)
{
    size_t ncap = (v->imask + 1) * 2, i;
    id_index_entry **nb = (id_index_entry **)calloc(ncap, sizeof *nb);
    if (!nb) return;
    for (i = 0; i <= v->imask; i++) {
        id_index_entry *e = v->ibuck[i];
        while (e) {
            id_index_entry *nx = e->next;
            size_t b = (size_t)(idx_mix(e->id) & (ncap - 1));
            e->next = nb[b]; nb[b] = e;
            e = nx;
        }
    }
    free(v->ibuck);
    v->ibuck = nb;
    v->imask = ncap - 1;
}

/* Remember where inode `id` lives. The last record for an id wins, matching
   the old scan, which kept walking and overwrote rec_pos on every hit. */
static void idx_put_id(invfs_volume *v, uint64_t id, uint64_t pos)
{
    size_t b;
    id_index_entry *e;
    if (!v->ibuck) return;
    b = (size_t)(idx_mix(id) & v->imask);
    for (e = v->ibuck[b]; e; e = e->next)
        if (e->id == id) { e->pos = pos; return; }
    e = (id_index_entry *)malloc(sizeof *e);
    if (!e) return;
    e->id = id;
    e->pos = pos;
    e->next = v->ibuck[b];
    v->ibuck[b] = e;
    v->icount++;
    if (v->icount > v->imask + 1) idx_grow_ids(v);
}

/* 0 = unknown; callers fall back to a scan */
static uint64_t idx_get_id(invfs_volume *v, uint64_t id)
{
    size_t b;
    const id_index_entry *e;
    if (!v->ibuck) return 0;
    b = (size_t)(idx_mix(id) & v->imask);
    for (e = v->ibuck[b]; e; e = e->next)
        if (e->id == id) return e->pos;
    return 0;
}

/* add `delta` to the live count of every directory prefix of `name`:
   "a/b/c.txt" bumps "a/" and "a/b/"; the anchor "a/" bumps "a/" itself,
   which is what keeps an empty directory visible */
static void idx_bump_dirs(invfs_volume *v, const char *name, size_t nlen,
                          int delta)
{
    size_t i;
    if (!v->dbuck) return;
    for (i = 0; i < nlen; i++) {
        size_t plen, b;
        dir_index_entry *e, **pp;
        if (name[i] != '/') continue;
        plen = i + 1;                     /* prefix includes the '/' */
        b = (size_t)(idx_hash(name, plen) & v->dmask);
        pp = &v->dbuck[b];
        for (e = *pp; e; pp = &e->next, e = e->next)
            if (e->nlen == plen && memcmp(e->name, name, plen) == 0) break;
        if (e) {
            if (delta < 0) {
                if (e->count) e->count--;
                if (e->count == 0) { *pp = e->next; free(e); v->dcount--; }
            } else {
                e->count++;
            }
            continue;
        }
        if (delta < 0) continue;          /* nothing to decrement */
        e = (dir_index_entry *)malloc(sizeof *e + plen);
        if (!e) continue;
        memcpy(e->name, name, plen);
        e->name[plen] = 0;
        e->nlen = (uint32_t)plen;
        e->count = 1;
        e->next = v->dbuck[b];
        v->dbuck[b] = e;
        v->dcount++;
        if (v->dcount > v->dmask + 1) idx_grow_dirs(v);
    }
}

/* record `name` as live under `id`. Replacing an existing name keeps the
   directory counts untouched -- it is still one live name. */
static void idx_put(invfs_volume *v, const char *name, size_t nlen,
                    uint64_t id, uint64_t pos, uint64_t size, uint64_t ctime)
{
    size_t b;
    name_index_entry *e;
    if (!v->nbuck || nlen == 0 || nlen > 255) return;
    b = (size_t)(idx_hash(name, nlen) & v->nmask);
    for (e = v->nbuck[b]; e; e = e->next)
        if (e->nlen == nlen && memcmp(e->name, name, nlen) == 0) {
            /* update of an existing name: only logical size moves */
            v->hot.logical_bytes += size;
            v->hot.logical_bytes -= e->size;
            e->inode_id = id;
            e->pos = pos;
            e->size = size;
            e->ctime = ctime;
            return;
        }
    /* brand-new name: population counter (dir anchors end with '/') */
    if (nlen && name[nlen - 1] == '/') v->hot.dirs++;
    else { v->hot.files++; v->hot.logical_bytes += size; }
    e = (name_index_entry *)malloc(sizeof *e + nlen);
    if (!e) return;
    memcpy(e->name, name, nlen);
    e->name[nlen] = 0;
    e->nlen = (uint32_t)nlen;
    e->inode_id = id;
    e->pos = pos;
    e->size = size;
    e->ctime = ctime;
    e->next = v->nbuck[b];
    v->nbuck[b] = e;
    v->ncount++;
    idx_bump_dirs(v, name, nlen, +1);
    if (v->ncount > v->nmask + 1) idx_grow_names(v);
}

/* A tombstone kills only ITS version of the name: the sweep appends the
   replacement record BEFORE the tombstone for the old inode, so a blind
   delete-by-name would drop the newer file. Mirrors the old vol_find scan. */
static void idx_del(invfs_volume *v, const char *name, size_t nlen,
                    uint64_t id)
{
    size_t b;
    name_index_entry *e, **pp;
    if (!v->nbuck || nlen == 0 || nlen > 255) return;
    b = (size_t)(idx_hash(name, nlen) & v->nmask);
    pp = &v->nbuck[b];
    for (e = *pp; e; pp = &e->next, e = e->next)
        if (e->nlen == nlen && memcmp(e->name, name, nlen) == 0) break;
    if (!e || e->inode_id != id) return;
    *pp = e->next;
    if (nlen && name[nlen - 1] == '/') v->hot.dirs--;
    else { v->hot.files--; v->hot.logical_bytes -= e->size; }
    free(e);
    v->ncount--;
    idx_bump_dirs(v, name, nlen, -1);
}

/* v2 position kill: remove the entry whose record lives exactly at `pos`,
 * whatever its id. Same-id metadata rewrites chain versions under one name,
 * so a plain id match would kill the NEWEST version instead of the one the
 * tombstone names. Falls back to nothing -- callers keep the id path for
 * legacy (file_size==0) tombstones. */
static void idx_del_at(invfs_volume *v, const char *name, size_t nlen,
                       uint64_t pos)
{
    size_t b;
    name_index_entry *e, **pp;
    if (!v->nbuck || nlen == 0 || nlen > 255 || pos == 0) return;
    b = (size_t)(idx_hash(name, nlen) & v->nmask);
    pp = &v->nbuck[b];
    for (e = *pp; e; pp = &e->next, e = e->next)
        if (e->nlen == nlen && memcmp(e->name, name, nlen) == 0 &&
            e->pos == pos)
            break;
    if (!e) return;
    *pp = e->next;
    if (nlen && name[nlen - 1] == '/') v->hot.dirs--;
    else { v->hot.files--; v->hot.logical_bytes -= e->size; }
    free(e);
    v->ncount--;
    idx_bump_dirs(v, name, nlen, -1);
}

static const name_index_entry *idx_get(invfs_volume *v, const char *name,
                                       size_t nlen)
{
    size_t b;
    const name_index_entry *e;
    if (!v->nbuck || nlen == 0 || nlen > 255) return NULL;
    b = (size_t)(idx_hash(name, nlen) & v->nmask);
    for (e = v->nbuck[b]; e; e = e->next)
        if (e->nlen == nlen && memcmp(e->name, name, nlen) == 0) return e;
    return NULL;
}

static uint64_t idx_dir_count(invfs_volume *v, const char *pre, size_t plen)
{
    size_t b;
    const dir_index_entry *e;
    if (!v->dbuck || plen == 0) return 0;
    b = (size_t)(idx_hash(pre, plen) & v->dmask);
    for (e = v->dbuck[b]; e; e = e->next)
        if (e->nlen == plen && memcmp(e->name, pre, plen) == 0)
            return e->count;
    return 0;
}

static int idx_dir_live(invfs_volume *v, const char *pre, size_t plen)
{
    return idx_dir_count(v, pre, plen) != 0;
}

static void bit_set(uint8_t *b, uint64_t i) { b[i / 8] |= (uint8_t)(1u << (i % 8)); }
static void bit_clr(uint8_t *b, uint64_t i) { b[i / 8] &= (uint8_t)~(1u << (i % 8)); }

/* Widen the dirty byte range to cover the byte holding bit i, so vol_flush
 * can write just that slice instead of the whole bitmap. */
static void bm_dirty(invfs_volume *v, uint64_t i)
{
    uint64_t byte = i / 8;
    if (v->bm_lo > v->bm_hi) { v->bm_lo = byte; v->bm_hi = byte + 1; return; }
    if (byte < v->bm_lo) v->bm_lo = byte;
    if (byte + 1 > v->bm_hi) v->bm_hi = byte + 1;
}

/* defined near vol_count_free; needed by vol_open and the fsck fixup above it */
static void alloc_state_reset(invfs_volume *v);

/* fsck: rebuild L2P/bitmap from inode area (defined after vol_flush) */
static int fsck_rebuild_one(invfs_volume *v, uint64_t rec_pos, uint64_t inode_id,
                            invfs_l2p_entry **l2p, size_t *n, size_t *cap,
                            uint8_t *used, size_t used_bytes,
                            invfs_fsck_report *rep);
int vol_fsck_scan(invfs_volume *v, invfs_fsck_report *rep, int fix);

invfs_volume *vol_open(const char *path, int *err)
{
    char devbuf[64];
    const char *real;
    int rc;
    invfs_volume *v = (invfs_volume *)calloc(1, sizeof(invfs_volume));
    if (!v) { *err = -1; return NULL; }

    /* "W:" is the shorthand a user types; CreateFileW needs "\\.\W:". Store
       the normalized form, so diagnostics name what was actually opened. */
    real = blkio_normalize(path, devbuf, sizeof devbuf);
    v->path = strdup(real);

    /* A device is taken exclusively -- locked and dismounted. Two processes
       each holding their own in-memory bitmap and L2P would corrupt the
       volume between them, which is why an image file is opened without
       FILE_SHARE_WRITE too. It also stops Windows from mounting whatever
       filesystem it believes is there and writing that filesystem's metadata
       over ours. */
    rc = blkio_open(&v->io, real,
                    blkio_looks_like_device(real) ? BLKIO_EXCLUSIVE : 0);
    if (rc != 0) {
        fprintf(stderr, "vol_open: %s: %s\n", real, blkio_strerror(rc));
        *err = -2;
        free(v->path);
        free(v);
        return NULL;
    }
    if (io_seek(&v->io, 0) != 0 ||
        io_read(&v->io, &v->sb, sizeof(v->sb)) != 0) { *err = -3; goto fail; }
    if (memcmp(v->sb.magic, INVFS_MAGIC, 8) != 0) { *err = -4; goto fail; }
    if (invfs_crc32c(&v->sb, offsetof(invfs_superblock, checksum)) != v->sb.checksum)
        { *err = -5; goto fail; }

    v->bitmap_blocks = (v->sb.total_blocks / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    v->bitmap = (uint8_t *)calloc(1, (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE);
    if (!v->bitmap) { *err = -6; goto fail; }
    if (io_seek(&v->io, v->sb.metadata_zone_start * INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, v->bitmap, (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE) != 0)
        { *err = -7; goto fail; }

    v->journal_start = v->sb.metadata_zone_start + v->bitmap_blocks;
    v->journal_pos = v->journal_start * INVFS_BLOCK_SIZE;
    v->inode_area_start = v->journal_start + JOURNAL_BLOCKS;
    v->inode_area_pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    v->inode_area_end = (v->sb.metadata_zone_start + v->sb.metadata_zone_blocks)
                        * INVFS_BLOCK_SIZE;

    /* scan existing inode records: find end of area + max inode id + name index */
    {
        uint64_t p = v->inode_area_pos;
        uint64_t found = 0;
        if (idx_init(v) != 0) { *err = -6; goto fail; }
        while (p + sizeof(invfs_inode_rec) <= v->inode_area_end) {
            invfs_inode_rec rec_h;
            if (io_seek(&v->io, p) != 0 ||
                io_read(&v->io, &rec_h, sizeof(rec_h)) != 0)
                break;
            if (rec_h.magic != INODE_REC_MAGIC && rec_h.magic != TOMBSTONE_MAGIC)
                break;  /* end of records */
            if (rec_h.rec_len < sizeof(invfs_inode_rec) ||
                rec_h.rec_len > INVFS_MAX_REC_LEN ||
                p + rec_h.rec_len + 4 > v->inode_area_end) {
                v->scan_anomalies++;
                break;  /* corrupted tail — stop */
            }
            /* torn-write protection: verify trailing CRC32C; a record
             * whose CRC fails is a half-written append (crash/SIGPIPE),
             * NOT a valid boundary — stop here so later tools never
             * step into garbage. */
            {
                uint8_t *rb = (uint8_t *)malloc((size_t)rec_h.rec_len + 4);
                uint32_t crc_stored, crc_calc;
                if (!rb) break;
                if (io_seek(&v->io, p) != 0 ||
                    io_read(&v->io, rb, (size_t)rec_h.rec_len + 4) != 0) {
                    free(rb); break;
                }
                memcpy(&crc_stored, rb + rec_h.rec_len, 4);
                crc_calc = invfs_crc32c(rb, rec_h.rec_len);
                free(rb);
                if (crc_calc != crc_stored) {
                    fprintf(stderr, "vol_open: corrupt inode record at %llu, "
                            "skipping (rec_len=%u)\n",
                            (unsigned long long)p,
                            (unsigned)rec_h.rec_len);
                    v->scan_anomalies++;
                    /* skip the corrupt record and keep scanning so files
                     * AFTER it stay visible; run fsck -f to clean up */
                    p += (uint64_t)rec_h.rec_len + 4;
                    continue;
                }
            }
            if (rec_h.magic == INODE_REC_MAGIC &&
                rec_h.inode_id >= v->next_inode_id)
                v->next_inode_id = rec_h.inode_id + 1;
            /* feed the name index from this same pass: the record is read and
               CRC-verified here, so the index inherits the exact same
               skip-the-corrupt-record semantics for free. A separate pass
               would stop at the first bad record and hide every name after
               it. Last record for a name wins, so replaying the area in
               order lands on the same answer the old linear scan gave. */
            {
                size_t nl = rec_h.name_len < sizeof(rec_h.name)
                          ? rec_h.name_len : sizeof(rec_h.name) - 1;
                if (rec_h.magic == INODE_REC_MAGIC) {
                    idx_put(v, rec_h.name, nl, rec_h.inode_id, p,
                            rec_h.file_size, rec_h.ctime);
                    idx_put_id(v, rec_h.inode_id, p);
                } else {
                    /* v2 tombstones carry the killed record's byte offset in
                     * the unused file_size field; 0 keeps legacy semantics */
                    if (rec_h.file_size != 0)
                        idx_del_at(v, rec_h.name, nl, rec_h.file_size);
                    else
                        idx_del(v, rec_h.name, nl, rec_h.inode_id);
                }
            }
            p += rec_h.rec_len + 4;
            found++;
        }
        v->inode_area_pos = p;
        if (getenv("INVFS_DEBUG"))
            printf("[vol_open] scanned %llu inode recs, next_inode=%llu, area_pos=%llu, "
                   "index=%llu names/%llu dirs\n",
                   (unsigned long long)found, (unsigned long long)v->next_inode_id,
                   (unsigned long long)v->inode_area_pos,
                   (unsigned long long)v->ncount, (unsigned long long)v->dcount);
    }

    /* replay L2P journal: rebuild in-memory table, continue at end */
    {
        uint64_t jp = v->journal_start * INVFS_BLOCK_SIZE;
        uint64_t jend = (v->journal_start + JOURNAL_BLOCKS) * INVFS_BLOCK_SIZE;
        uint64_t replayed = 0;
        while (jp + sizeof(invfs_l2p_entry) <= jend) {
            invfs_l2p_entry e;
            if (io_seek(&v->io, jp) != 0 ||
                io_read(&v->io, &e, sizeof(e)) != 0)
                break;
            if (invfs_crc32c(&e, offsetof(invfs_l2p_entry, crc)) != e.crc)
                break;  /* end of valid journal */
            if (e.type == INVFS_JRN_MAP) {
                if (v->l2p_count == v->l2p_cap) {
                    v->l2p_cap = v->l2p_cap ? v->l2p_cap * 2 : 256;
    v->l2p = (invfs_l2p_entry *)realloc(v->l2p,
                                    v->l2p_cap * sizeof(invfs_l2p_entry));
                    if (!v->l2p) { *err = -9; goto fail; }
                }
                v->l2p[v->l2p_count++] = e;
                replayed++;
            } else if (e.type == INVFS_JRN_UNMAP) {
                l2p_remove(v, e.inode, e.lba);
            }
            jp += sizeof(e);
        }
        v->journal_pos = jp;
        if (getenv("INVFS_DEBUG"))
            printf("[vol_open] replayed %llu L2P entries, journal_pos=%llu\n",
                   (unsigned long long)replayed, (unsigned long long)v->journal_pos);
    }

    v->alloc_cursor = v->sb.raw_zone_start;
    if (v->next_inode_id == 0)
        v->next_inode_id = 1;
    /* ENOSPC defaults for images created before the policy fields */
    if (v->sb.reserved_blocks == 0)
        v->sb.reserved_blocks = (uint32_t)(v->sb.total_blocks / 128 + 64);
    if (v->sb.hard_min_blocks == 0)
        v->sb.hard_min_blocks = (uint32_t)(v->sb.total_blocks / 1024 + 16);
    v->free_blocks = vol_count_free(v);
    alloc_state_reset(v);

    /* Reconstructed-content cache.
     *
     * INVFS_ARC_BYTES sets the budget; 0 disables the cache outright, which is
     * how the tests measure what it is worth and how a memory-tight host opts
     * out. The default is deliberately modest -- this is a userspace FS process
     * that already buffers whole files for writes, and a cache that competes
     * with that is a worse trade than a slower read.
     *
     * K/M/G suffixes are accepted, and anything unparseable falls back to the
     * default with a complaint. Both matter because the failure was silent and
     * looked like success: "256M" parsed as 256 *bytes*, which leaves the cache
     * switched on and refusing every entry over 128 bytes, and a typo parsed as
     * 0, which switches it off. Either way reads got slow and nothing said why. */
    {
        const char *ab = getenv("INVFS_ARC_BYTES");
        size_t budget = 256u << 20;      /* 256 MB */
        if (ab) {
            char *endp = NULL;
            unsigned long long want = strtoull(ab, &endp, 10);
            unsigned long long mult = 1;
            int ok = (endp != ab);
            if (ok) {
                while (*endp == ' ' || *endp == '\t') endp++;
                switch (*endp) {
                    case 'k': case 'K': mult = 1024ull; endp++; break;
                    case 'm': case 'M': mult = 1024ull * 1024; endp++; break;
                    case 'g': case 'G': mult = 1024ull * 1024 * 1024; endp++; break;
                    default: break;
                }
                if (*endp == 'b' || *endp == 'B') endp++;
                while (*endp == ' ' || *endp == '\t') endp++;
                if (*endp != '\0') ok = 0;
                if (want > (unsigned long long)SIZE_MAX / mult) ok = 0;
            }
            if (ok) {
                budget = (size_t)(want * mult);
            } else {
                fprintf(stderr, "[vol] INVFS_ARC_BYTES=\"%s\" is not a size; "
                                "using the default %llu bytes\n",
                        ab, (unsigned long long)budget);
            }
        }
        v->arc = arc_create(budget);     /* NULL == disabled, callers cope */
        v->arc_budget = (uint64_t)budget;
        if (getenv("INVFS_DEBUG"))
            printf("[vol_open] content cache: %s (%llu bytes)\n",
                   v->arc ? "on" : "off", (unsigned long long)budget);
    }
    *err = 0;
    /* Unclean shutdown. Reading always works -- that is how you find out
     * what survived. For WRITES the old behavior was a hard latch: every
     * mount after an unclean stop stayed read-only until a manual
     * `invf-fsck -f`, which made any ungraceful kill (openrc shutdown
     * storms, OOM, host crash) boot-blocking for root-FS duty.
     * Auto-recovery instead: if the full-record scan just completed with
     * ZERO anomalies (every CRC verified, no torn tail), replay already
     * rebuilt the exact on-disk truth and nothing was lost -- clear DIRTY
     * and continue read-write. Any real damage keeps the conservative
     * manual-fsck path. INVFS_AUTO_RECOVER=0 opts out. */
    if (v->sb.state != INVFS_STATE_CLEAN) {
        const char *ar = getenv("INVFS_AUTO_RECOVER");
        int ro_flag = (v->sb.vol_flags & VOLF_READONLY) != 0;
        if (!ro_flag && v->scan_anomalies == 0 && (!ar || strcmp(ar, "0") != 0)) {
            v->sb.state = INVFS_STATE_CLEAN;
            if (vol_write_sb(v) == 0)
                fprintf(stderr, "vol_open: %s not closed cleanly but scan is "
                                "anomaly-free; recovered to CLEAN (rw)\n",
                        real);
            else
                v->needs_recovery = 1;
        } else {
            fprintf(stderr,
                    "vol_open: %s was not closed cleanly (state=0x%02X%s); "
                    "read-only until recovery. Run `invf-fsck -f %s`.\n",
                    real, (unsigned)v->sb.state,
                    v->scan_anomalies ? ", damaged records" : "",
                    path);
            v->needs_recovery = 1;
        }
    }
    return v;
fail:
    io_close(&v->io);
    if (v->bitmap) free(v->bitmap);
    free(v->path);
    free(v);
    return NULL;
}

void vol_close(invfs_volume *v)
{
    if (!v) return;
    /* Close is the only place that can honestly write CLEAN, and it can only
       do so after the maps are down. There was no flush here at all: every
       tool that mutated the volume had to remember to call vol_flush itself,
       and forgetting cost the whole run silently. Only a session that
       actually dirtied the volume writes anything, so invf-ls and invf-cat
       stay read-only. */
    if (v->dirty) {
        if (vol_flush(v) == 0) {
            v->sb.state = INVFS_STATE_CLEAN;
            if (vol_write_sb(v) != 0)
                fprintf(stderr, "vol_close: could not mark volume clean; "
                                "next mount will recover\n");
        } else {
            fprintf(stderr, "vol_close: final flush failed; volume stays "
                            "dirty and will be recovered at next mount\n");
        }
        /* A buffered image file needs a real barrier for power-loss safety;
           a device is already write-through. Off by default because the
           barrier costs a full device cache flush and the ordering above is
           what makes process death survivable either way. */
        if (getenv("INVFS_FSYNC"))
            blkio_flush(&v->io);
    }
    io_close(&v->io);
    idx_clear(v);
    arc_destroy(v->arc);
    free(v->tz);
    free(v->bz);
    free(v->bitmap);
    free(v->l2p);
    free(v->path);
    free(v);
}

void vol_arc_stats(invfs_volume *v, invfs_arc_stats *out)
{
    arc_stats(v ? v->arc : NULL, out);
}

/* ---- WP10: memory policy (sweep-time admission only) ---- */

#define INVFS_DEC_MEM_DEFAULT (512ull << 20)   /* 512 MiB (WP10 §6) */

/* Recreate the content cache with a new budget (the arc has no resize).
 * 0 keeps the current one, so INVFS_ARC_BYTES at vol_open stays the
 * fallback when no explicit budget was set. */
void vol_set_arc_budget(invfs_volume *v, uint64_t bytes)
{
    if (!v || bytes == 0) return;
    arc_destroy(v->arc);
    v->arc = arc_create((size_t)bytes);   /* NULL == disabled, callers cope */
    v->arc_budget = bytes;
}

void vol_set_dec_mem_limit(invfs_volume *v, uint64_t bytes)
{
    if (v) v->dec_mem_limit = bytes;      /* 0 = default */
}

uint64_t vol_get_dec_mem_limit(invfs_volume *v)
{
    return (v && v->dec_mem_limit) ? v->dec_mem_limit : INVFS_DEC_MEM_DEFAULT;
}

/* The batch accumulator and the real vol_tz_flush/vol_tz_gc live with the
 * rest of the WP10 write path, right after the storage-class helpers (they
 * need meta_rewrite/vol_stamp_class/vol_delete_inode). */

/* ================= fsck / repair =================
 * NOTE on physical addressing: AST entries store block_id = segment
 * index (the L2P key), NOT the physical zone block. Physical block
 * addresses live only in the L2P/journal, so a destroyed journal cannot
 * be rebuilt from ASTs with the current format. What fsck CAN do:
 *   - verify inode-record CRCs and record lengths (bad_recs)
 *   - verify that every live AST segment has an L2P entry (l2p_miss)
 *   - rebuild the used-bitmap from (metadata zone + live L2P pbas):
 *     orphans (allocated, unreferenced) are freed, missing (referenced,
 *     free in bitmap) are restored
 *   - rewrite the journal from the in-memory L2P (journal compact)
 *   - mark the superblock CLEAN
 * With fix=1 all fixes are applied and persisted. */
static int fsck_rebuild_one(invfs_volume *v, uint64_t rec_pos, uint64_t inode_id,
                            invfs_l2p_entry **l2p, size_t *n, size_t *cap,
                            uint8_t *used, size_t used_bytes,
                            invfs_fsck_report *rep)
{
    invfs_inode_rec rh;
    invfs_ast_recipe_header ast_h;
    uint8_t *rec = NULL;
    uint32_t crc_stored, crc_calc;
    size_t off, i;

    (void)used; (void)used_bytes;
    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, &rh, sizeof(rh)) != 0) return -1;
    rec = (uint8_t *)malloc(rh.rec_len);
    if (!rec) return -1;
    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, rec, rh.rec_len) != 0 ||
        io_read(&v->io, &crc_stored, 4) != 0) { free(rec); return -1; }
    crc_calc = invfs_crc32c(rec, rh.rec_len);
    if (crc_calc != crc_stored) { rep->bad_recs++; free(rec); return 0; }

    off = sizeof(invfs_inode_rec);   /* name[] lives inside the header */
    if (off + sizeof(ast_h) > rh.rec_len) { free(rec); return -1; }
    memcpy(&ast_h, rec + off, sizeof(ast_h));
    off += sizeof(ast_h);

    for (i = 0; i < ast_h.num_blocks; i++) {
        invfs_ast_block_entry e;
        uint64_t pba, len;
        if (off + sizeof(e) > rh.rec_len) { rep->bad_recs++; break; }
        memcpy(&e, rec + off, sizeof(e));
        off += sizeof(e);
        /* every live segment must have an L2P entry */
        if (vol_lookup_entry(v, inode_id, e.block_id, &pba, &len) != 0 ||
            pba == 0) {
            rep->l2p_miss++;
            continue;
        }
        /* mark used from L2P (physical) */
        {
            uint64_t b;
            for (b = pba; b < pba + len && b < v->sb.total_blocks; b++)
                bit_set(used, b);
        }
        /* keep entry for journal rewrite */
        if (*n == *cap) {
            size_t ncap = *cap ? *cap * 2 : 256;
            invfs_l2p_entry *nl = (invfs_l2p_entry *)realloc(*l2p, ncap * sizeof(invfs_l2p_entry));
            if (!nl) { free(rec); return -1; }
            *l2p = nl; *cap = ncap;
        }
        (*l2p)[*n].type = INVFS_JRN_MAP;
        (*l2p)[*n].inode = inode_id;
        (*l2p)[*n].lba = e.block_id;
        (*l2p)[*n].pba = pba;
        (*l2p)[*n].length = (uint32_t)len;
        (*l2p)[*n].crc = 0;
        (*n)++;
    }
    free(rec);
    return 0;
}

int vol_fsck_scan(invfs_volume *v, invfs_fsck_report *rep, int fix)
{
    uint64_t pos, end;
    uint64_t i;
    size_t l2p_n = 0, l2p_cap = 0;
    invfs_l2p_entry *newl2p = NULL;
    uint8_t *used = NULL;
    size_t used_bytes;
    /* v2 tombstones kill by record position (DELT.file_size != 0);
     * legacy ones kill by inode id. Keep both fields per entry. */
    uint64_t *tomb_id = NULL, *tomb_pos = NULL;
    size_t tomb_n = 0, tomb_cap = 0;

    memset(rep, 0, sizeof(*rep));
    used_bytes = (size_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
    used = (uint8_t *)calloc(1, used_bytes);
    if (!used) return -1;

    end = v->inode_area_pos;
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;

    /* pass 1: tombstones + live inode record offsets */
    {
        uint64_t *live_pos = NULL, *live_id = NULL;
        size_t live_n = 0, live_cap = 0;
        /* scan the FULL metadata zone tail, not just v->inode_area_pos
         * (vol_open truncates the area at the first corrupt record) */
        end = (v->sb.metadata_zone_start + v->sb.metadata_zone_blocks)
              * INVFS_BLOCK_SIZE;
        pos = v->inode_area_start * INVFS_BLOCK_SIZE;
        while (pos + sizeof(invfs_inode_rec) <= end) {
            invfs_inode_rec rh;
            uint32_t crc_stored, crc_calc;
            uint8_t *rec = NULL;
            if (io_seek(&v->io, pos) != 0 ||
                io_read(&v->io, &rh, sizeof(rh)) != 0) break;
            if (rh.magic != INODE_REC_MAGIC && rh.magic != TOMBSTONE_MAGIC) break;
            if (rh.rec_len < sizeof(invfs_inode_rec) ||
                pos + rh.rec_len + 4 > end) {
                rep->bad_recs++;
                break;
            }
            rec = (uint8_t *)malloc(rh.rec_len);
            if (!rec) { free(used); free(live_pos); free(live_id); return -1; }
            if (io_seek(&v->io, pos) != 0 ||
                io_read(&v->io, rec, rh.rec_len) != 0 ||
                io_read(&v->io, &crc_stored, 4) != 0) {
                free(rec); free(used); free(live_pos); free(live_id); return -1;
            }
            crc_calc = invfs_crc32c(rec, rh.rec_len);
            if (crc_calc != crc_stored) {
                /* corrupt record: report, skip past it, keep scanning */
                rep->bad_recs++;
                free(rec);
                pos += rh.rec_len + 4;
                continue;
            }
            if (rh.magic == TOMBSTONE_MAGIC) {
                if (tomb_n == tomb_cap) {
                    size_t ncap = tomb_cap ? tomb_cap * 2 : 64;
                    uint64_t *ni = (uint64_t *)realloc(tomb_id, ncap * sizeof(uint64_t));
                    uint64_t *np = (uint64_t *)realloc(tomb_pos, ncap * sizeof(uint64_t));
                    if (!ni || !np) {
                        free(ni); free(np);
                        free(rec); free(used); free(live_pos); free(live_id);
                        return -1;
                    }
                    tomb_id = ni; tomb_pos = np;
                    tomb_cap = ncap;
                }
                tomb_id[tomb_n] = rh.inode_id;
                tomb_pos[tomb_n] = rh.file_size;   /* v2: record position */
                tomb_n++;
            } else if (rh.magic == INODE_REC_MAGIC) {
                if (live_n == live_cap) {
                    live_cap = live_cap ? live_cap * 2 : 256;
                    uint64_t *np = (uint64_t *)realloc(live_pos, live_cap * sizeof(uint64_t));
                    uint64_t *ni = (uint64_t *)realloc(live_id, live_cap * sizeof(uint64_t));
                    if (!np || !ni) {
                        free(np); free(ni);
                        free(rec); free(used); free(live_pos); free(live_id);
                        return -1;
                    }
                    live_pos = np; live_id = ni;
                }
                live_pos[live_n] = pos;
                live_id[live_n] = rh.inode_id;
                live_n++;
            }
            free(rec);
            pos += rh.rec_len + 4;
        }

        /* pass 2: per live inode, verify AST<->L2P and collect used blocks */
        for (i = 0; i < live_n; i++) {
            int killed = 0;
            size_t t;
            for (t = 0; t < tomb_n; t++)
                if ((tomb_pos[t] == 0 && tomb_id[t] == live_id[i]) ||
                    (tomb_pos[t] != 0 && tomb_pos[t] == live_pos[i])) {
                    killed = 1; break;
                }
            if (killed) continue;
            /* Counted here, not in pass 1: a record that a tombstone later
               killed is not a live file. Counting every INOD reported 40004
               live files on a volume holding 40000 -- the 4 rewritten ones
               were counted twice. */
            rep->live_files++;
            if (fsck_rebuild_one(v, live_pos[i], live_id[i],
                                 &newl2p, &l2p_n, &l2p_cap,
                                 used, used_bytes, rep) != 0) {
                free(used); free(live_pos); free(live_id); free(tomb_id); free(tomb_pos);
                free(newl2p);
                return -1;
            }
        }
        free(live_pos); free(live_id);
    }
    free(tomb_id);
    free(tomb_pos);

    /* metadata zone is always allocated */
    {
        uint64_t meta_end = v->sb.metadata_zone_start + v->sb.metadata_zone_blocks;
        uint64_t b;
        for (b = 0; b < meta_end; b++)
            bit_set(used, b);
    }

    /* compare bitmaps: orphans = in v->bitmap, not in used; missing = reverse */
    {
        uint64_t total = v->sb.total_blocks;
        for (i = 0; i < total; i++) {
            int bm = bit_get(v->bitmap, i);
            int us = bit_get(used, i);
            if (bm && !us) rep->orphans++;
            if (us && !bm) rep->missing++;
        }
    }

    rep->l2p_entries = l2p_n;

    /* repair when there is structural damage, or when the volume is merely
     * dirty: a scan that found nothing else is exactly a clean-close
     * simulation, so rewriting bitmap/journal and setting CLEAN is safe.
     * Volumes with l2p_miss keep their read-only hold: mappings are gone
     * from the journal and cannot be reconstructed without human review. */
    if (fix && (rep->orphans || rep->missing || rep->bad_recs ||
                (!rep->l2p_miss && v->sb.state != INVFS_STATE_CLEAN))) {
        /* journal rewrite: keep only live-AST mappings (drops stale
         * entries of records fsck could not verify) */
        if (l2p_n) {
            free(v->l2p);
            v->l2p = newl2p;
            v->l2p_count = l2p_n;
            v->l2p_cap = l2p_n;
            v->l2p_dirty = 0;   /* whole table replaced */
            newl2p = NULL;
        }
        memcpy(v->bitmap, used, used_bytes);
        v->free_blocks = vol_count_free(v);
        alloc_state_reset(v);   /* bitmap replaced: per-zone counters stale */
        /* alloc_state_reset marks the bitmap clean, which holds on mount but
         * not here: every byte may differ from disk, so flush all of it. */
        v->bm_lo = 0;
        v->bm_hi = (uint64_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
        if (vol_flush(v) != 0) {
            free(used); free(newl2p);
            return -1;
        }
        v->sb.state = INVFS_STATE_CLEAN;
        if (vol_flush(v) != 0) {
            free(used); free(newl2p);
            return -1;
        }
    }
    free(used);
    free(newl2p);
    return 0;
}
/* Persist the superblock. The checksum covers bytes 0..0x7B, and `state`
   lives at 0x18 -- inside that range -- so it has to be recomputed here.
   It was not, which was harmless only for as long as nothing inside the
   checksummed range ever changed: the ENOSPC policy fields and the READONLY
   flag sit at 0x80 and beyond deliberately. The moment `state` starts moving
   (which is the whole point of crash detection) a stale checksum turns the
   volume unopenable -- vol_open rejects it with err -5. */
static int vol_write_sb(invfs_volume *v)
{
    v->sb.checksum = invfs_crc32c(&v->sb, offsetof(invfs_superblock, checksum));
    if (io_seek(&v->io, 0) != 0 ||
        io_write(&v->io, &v->sb, sizeof(v->sb)) != 0)
        return -1;
    return 0;
}

int vol_flush(invfs_volume *v)
{
    /* persist superblock (state / ENOSPC policy fields / READONLY flag) */
    if (vol_write_sb(v) != 0)
        return -1;
    /* Persist only the part of the bitmap that changed. Dokan flushes on
     * every file close and the device is unbuffered write-through, so the
     * old unconditional full-bitmap write cost a synchronous 480 KB per
     * close on this volume -- about 250 ms on flash, independent of how
     * many bytes the file actually had. A file touches a handful of
     * bitmap bytes; write those. */
    {
        uint64_t bm_bytes = (uint64_t)v->bitmap_blocks * INVFS_BLOCK_SIZE;
        uint64_t base = v->sb.metadata_zone_start * INVFS_BLOCK_SIZE;
        if (v->bm_lo <= v->bm_hi) {
            uint64_t lo = v->bm_lo, hi = v->bm_hi;
            if (hi > bm_bytes) hi = bm_bytes;
            /* round out to INVFS_BLOCK_SIZE so the unbuffered path writes
               whole blocks and never read-modify-writes a partial one */
            lo -= lo % INVFS_BLOCK_SIZE;
            hi = ((hi + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE) * INVFS_BLOCK_SIZE;
            if (hi > bm_bytes) hi = bm_bytes;
            if (hi > lo) {
                if (io_seek(&v->io, base + lo) != 0 ||
                    io_write(&v->io, v->bitmap + lo, (size_t)(hi - lo)) != 0)
                    return -1;
            }
            v->bm_lo = 1; v->bm_hi = 0;   /* clean */
        }
    }
    /* Compact L2P journal from the in-memory table (source of truth after
     * replay + ops). Keeps the journal bounded across many sweep/cp cycles
     * and drops stale MAP entries of tombstoned inodes (vol_delete_inode
     * removes them in-memory without journaling).
     *
     * Only the suffix from l2p_dirty is rewritten. The old code re-seeked
     * and re-wrote every entry on every flush, and Dokan flushes on each
     * file close, so writing n files cost n^2/2 seek+write pairs -- 20 ms
     * per file at 1600 files, all of it in this loop. Appending files now
     * writes only the new entries. */
    {
        uint64_t jstart = v->journal_start * INVFS_BLOCK_SIZE;
        uint64_t jend = (v->journal_start + JOURNAL_BLOCKS) * INVFS_BLOCK_SIZE;
        size_t esz = sizeof(invfs_l2p_entry);
        size_t from = v->l2p_dirty < v->l2p_count ? v->l2p_dirty : v->l2p_count;
        size_t n = v->l2p_count - from;
        uint64_t jp = jstart + (uint64_t)from * esz;

        /* +1 for the terminator that stops replay */
        if (jp + (uint64_t)(n + 1) * esz > jend)
            return -1;   /* journal full */

        if (n) {
            invfs_l2p_entry *buf = (invfs_l2p_entry *)malloc(n * esz);
            size_t i;
            if (!buf) return -1;
            for (i = 0; i < n; i++) {
                buf[i] = v->l2p[from + i];
                buf[i].crc = invfs_crc32c(&buf[i], offsetof(invfs_l2p_entry, crc));
            }
            if (io_seek(&v->io, jp) != 0 ||
                io_write(&v->io, buf, n * esz) != 0) { free(buf); return -1; }
            free(buf);
            jp += (uint64_t)n * esz;
        }
        /* Terminator: replay stops at the first entry whose CRC does not
           check out. Without it a journal that SHRANK (after a delete) left
           the previous, still-valid tail behind, and the next mount replayed
           mappings for blocks that had already been freed. */
        {
            invfs_l2p_entry z;
            memset(&z, 0, sizeof z);
            z.crc = ~invfs_crc32c(&z, offsetof(invfs_l2p_entry, crc));
            if (io_seek(&v->io, jp) != 0 ||
                io_write(&v->io, &z, esz) != 0)
                return -1;
        }
        v->journal_pos = jp;
        v->l2p_dirty = v->l2p_count;
    }
    return 0;
}

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
static int vol_mark_dirty(invfs_volume *v)
{
    if (v->needs_recovery) return -1;   /* refuse writes until recovered */
    if (v->dirty) return 0;
    v->sb.state = INVFS_STATE_DIRTY;
    if (vol_write_sb(v) != 0) return -1;
    v->dirty = 1;
    return 0;
}

/* Call before appending an inode record: mark dirty, then make the maps
   durable so the record about to land is backed by something readable. */
static int vol_pre_record(invfs_volume *v)
{
    if (vol_mark_dirty(v) != 0) return -1;
    return vol_flush(v);
}

int vol_needs_recovery(invfs_volume *v)
{
    return v && v->needs_recovery;
}

/* allocate n consecutive free blocks in a zone; returns start block or 0 */
static uint64_t alloc_blocks(invfs_volume *v, uint64_t zone_start, uint64_t zone_len,
                             uint64_t n, int use_reserve)
{
    uint64_t zone_end = zone_start + zone_len;
    uint64_t i, count = 0, start = 0;
    /* Cursor and free count belong to the zone being scanned, not to the
     * volume: with one shared cursor every spill into SHADOW rewound the
     * next RAW attempt to the zone head, so a full RAW zone was rescanned
     * end to end for every 64 KB segment. */
    uint64_t *cursor, *zone_free, *fail_run;

    if (zone_start == v->sb.shadow_zone_start) {
        cursor = &v->shadow_cursor; zone_free = &v->shadow_free;
        fail_run = &v->shadow_fail_run;
    } else {
        cursor = &v->raw_cursor;    zone_free = &v->raw_free;
        fail_run = &v->raw_fail_run;
    }

    if (v->sb.vol_flags & VOLF_READONLY)
        return 0;

    /* Skip the scan when this zone cannot satisfy n. Either it has too few
     * free blocks outright, or a previous scan proved no run this long
     * exists. This is what turns a hopeless RAW retry from 786k bitmap
     * probes into one compare -- per 64 KB segment, so ~100M probes per
     * 8 MB file before this check existed. */
    if (*zone_free < n)
        return 0;
    if (*fail_run && n >= *fail_run)
        return 0;

    /* ENOSPC policy: ordinary writes must leave the reserve + hard-min
     * untouched; sweep/transcodes (use_reserve) may drain the reserve but
     * never the hard-min floor. Hitting the floor flips the volume to
     * READONLY so applications get a clean ENOSPC/EROFS instead of data
     * loss (fsck can then reclaim orphaned blocks). */
    {
        uint64_t guard = (use_reserve ? 0 : v->sb.reserved_blocks)
                         + v->sb.hard_min_blocks;
        if (v->free_blocks <= guard) {
            if (v->free_blocks <= v->sb.hard_min_blocks &&
                !(v->sb.vol_flags & VOLF_READONLY)) {
                v->sb.vol_flags |= VOLF_READONLY;
                fprintf(stderr, "[alloc] READONLY: free=%llu hard_min=%u\n",
                        (unsigned long long)v->free_blocks,
                        (unsigned)v->sb.hard_min_blocks);
            }
            return 0;  /* ENOSPC */
        }
    }

    i = *cursor;
    if (i < zone_start || i >= zone_end)
        i = zone_start;

    for (count = 0; count < zone_len; count++) {
        if (i >= zone_end) { i = zone_start; start = 0; }
        if (!bit_get(v->bitmap, i)) {
            if (start == 0) start = i;
            if (i - start + 1 == n) {
                uint64_t k;
                for (k = start; k <= i; k++) bit_set(v->bitmap, k);
                bm_dirty(v, start);
                bm_dirty(v, i);
                *cursor = (i + 1 < zone_end) ? i + 1 : zone_start;
                v->free_blocks -= n;
                *zone_free -= n;
                return start;
            }
        } else {
            start = 0;
        }
        i++;
    }
    /* Scanned the whole zone without a run of n: the space is there but too
     * fragmented. Record the smallest run size known to fail so later, larger
     * requests skip the scan. Any free() in this zone clears the hint. */
    if (*fail_run == 0 || n < *fail_run)
        *fail_run = n;
    return 0;  /* ENOSPC */
}

static uint64_t alloc_raw_or_shadow(invfs_volume *v, uint64_t nblocks, int *zone_out)
{
    uint64_t pba = alloc_blocks(v, v->sb.raw_zone_start, v->sb.raw_zone_blocks,
                                nblocks, 0);
    if (pba == 0) {
        pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                           nblocks, 0);
        if (pba) *zone_out = INVFS_ZONE_BINARY;
    } else {
        *zone_out = INVFS_ZONE_RAW;
    }
    return pba;
}

/* write raw data to RAW zone, returns first pba (0 on error) */
uint64_t vol_write_raw(invfs_volume *v, const uint8_t *data, size_t len)
{
    uint64_t nblocks = (len + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    uint64_t pba = alloc_blocks(v, v->sb.raw_zone_start, v->sb.raw_zone_blocks, nblocks, 0);
    if (pba == 0)   /* RAW exhausted -> spill into SHADOW (still uncompressed) */
        pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                           nblocks, 0);
    if (pba == 0) return 0;
    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, data, (size_t)nblocks * INVFS_BLOCK_SIZE) != 0)
        return 0;
    return pba;
}

/* L2P MAP entry: inode logical block -> physical block */
int vol_map(invfs_volume *v, uint64_t inode, uint64_t lba, uint64_t pba, uint32_t length)
{
    invfs_l2p_entry e;
    memset(&e, 0, sizeof(e));
    e.type = INVFS_JRN_MAP;
    e.inode = inode;
    e.lba = lba;
    e.pba = pba;
    e.length = length;
    e.crc = invfs_crc32c(&e, offsetof(invfs_l2p_entry, crc));

    /* In-memory only. vol_flush is the single writer of the journal: it
     * rewrites the dirty suffix of this table and re-stamps the terminator.
     *
     * This used to seek and write the 64-byte record here, on every segment.
     * On an image that is a buffered 64-byte write and invisible; on a raw
     * device opened NO_BUFFERING|WRITE_THROUGH it is a read-modify-write of
     * a 4 KB block plus a synchronous flush to flash -- ~80 ms. Files are
     * split into 64 KB segments, so an 8 MB file paid ~128 of them (~11 s)
     * and an 18 MB file ~20 s, all inside vol_create_file under the Dokan
     * write lock. That is what pushed a callback past opt.Timeout and made
     * the mount die mid-copy with no error, and why a fully compressible
     * 8 MB file cost the same as an incompressible one: the price was per
     * segment, not per byte reaching the device.
     *
     * Dropping the write also fixes a correctness hole. The record went in
     * ON TOP of the terminator without writing a new one, so a crash before
     * the next flush left replay free to run past the end of the live
     * entries into whatever stale, still-CRC-valid records the journal held
     * from an earlier, longer life -- exactly what the terminator exists to
     * prevent. journal_pos is recomputed by vol_flush; nothing reads it in
     * between except stat.c, which reports on a freshly opened volume. */
    if ((uint64_t)(v->l2p_count + 2) * sizeof(e) >
        (uint64_t)JOURNAL_BLOCKS * INVFS_BLOCK_SIZE) {
        fprintf(stderr, "L2P journal full\n");
        return -1;
    }

    /* in-memory */
    if (v->l2p_count == v->l2p_cap) {
        v->l2p_cap = v->l2p_cap ? v->l2p_cap * 2 : 256;
        v->l2p = (invfs_l2p_entry *)realloc(v->l2p, v->l2p_cap * sizeof(invfs_l2p_entry));
        if (!v->l2p) return -1;
    }
    v->l2p[v->l2p_count++] = e;
    return 0;
}

/* in-memory L2P lookup: inode + lba -> pba and phys length (or 0) */
uint64_t vol_lookup(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    uint64_t pba = 0, len = 0;
    vol_lookup_entry(v, inode, lba, &pba, &len);
    return pba;
}

int vol_lookup_entry(invfs_volume *v, uint64_t inode, uint64_t lba,
                     uint64_t *pba_out, uint64_t *len_out)
{
    /* newest wins: scan from the end (L2P is append-only journal) */
    size_t i;
    for (i = v->l2p_count; i-- > 0; ) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->inode == inode && e->lba == lba) {
            *pba_out = e->pba;
            *len_out = e->length;
            return 0;
        }
    }
    return -1;
}

/* remove all mappings for (inode, lba) from the in-memory table */
static void l2p_remove(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    size_t i, w = 0;
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->inode == inode && e->lba == lba) {
            /* everything from here shifts down, so the on-disk suffix
               starting at w is now stale */
            if (w < v->l2p_dirty) v->l2p_dirty = w;
            continue;  /* drop */
        }
        if (w != i) v->l2p[w] = v->l2p[i];
        w++;
    }
    v->l2p_count = w;
    if (v->l2p_dirty > v->l2p_count) v->l2p_dirty = v->l2p_count;
}

/* public wrapper (used by dedupe pass) */
void vol_l2p_remove(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    l2p_remove(v, inode, lba);
}

/* read one block worth of raw bytes at pba */
int vol_read_block(invfs_volume *v, uint64_t pba, void *buf)
{
    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, buf, INVFS_BLOCK_SIZE) != 0)
        return -1;
    return 0;
}

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
static int write_segment_blocks(invfs_volume *v, uint64_t pba, uint8_t *buf,
                                size_t payload, uint64_t phys_blocks)
{
    size_t span = (size_t)phys_blocks * INVFS_BLOCK_SIZE;
    if (span > payload)
        memset(buf + payload, 0, span - payload);
    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, buf, span) != 0)
        return -1;
    return 0;
}

/* ---- inode area (append-only records) ---- */

/* write inode record: name + AST blocks; returns inode_id (0 on error) */
uint64_t vol_create_file(invfs_volume *v, const char *name,
                         const uint8_t *data, size_t len)
{
    size_t i, ast_entries;
    /* Hard format limits: the AST recipe header stores file_size as uint32
       and num_blocks as uint16. Past either, the casts below silently fold
       the value and produce a record that looks valid but reconstructs the
       wrong bytes — the one failure mode this filesystem must not have.
       Refuse the write instead. (The container path already checked
       MAX_SEGMENTS; this one did not.) */
    if (len > 0xFFFFFFFFu ||
        (len + SEGMENT_SIZE - 1) / SEGMENT_SIZE > MAX_SEGMENTS) {
        fprintf(stderr, "invarifs: %s: %llu bytes exceeds the format limit "
                "(%u bytes / %u segments of %u)\n", name,
                (unsigned long long)len, 0xFFFFFFFFu, MAX_SEGMENTS,
                (unsigned)SEGMENT_SIZE);
        return 0;
    }
    if (name_too_long(name)) return 0;
    uint64_t inode_id = v->next_inode_id++;
    if (getenv("INVFS_DEBUG"))
        printf("[create_file] next_inode was %llu -> using %llu\n",
               (unsigned long long)(inode_id - 1), (unsigned long long)inode_id);
    invfs_ast_recipe_header ast_h;
    size_t rec_size;
    uint8_t *rec;
    invfs_inode_rec *rec_h;
    invfs_ast_block_entry *entries;
    uint32_t crc_rec;
    int *seg_lz4 = NULL;   /* per-segment: 1 = lz4, 0 = raw */
    uint32_t *seg_csize = NULL;

    if (len == 0) {
        /* empty file: inode record with 0 AST entries */
        invfs_ast_recipe_header ast_h0;
        size_t rec_size0 = sizeof(invfs_inode_rec) + sizeof(ast_h0);
        uint8_t *rec0 = (uint8_t *)calloc(1, rec_size0);
        invfs_inode_rec *rh0 = (invfs_inode_rec *)rec0;
        uint32_t crc0;
        if (!rec0) return 0;
        memset(&ast_h0, 0, sizeof(ast_h0));
        ast_h0.version = 1;
        ast_h0.file_size = 0;
        rh0->magic = INODE_REC_MAGIC;
        rh0->rec_len = (uint32_t)rec_size0;
        rh0->inode_id = inode_id;
        rh0->file_size = 0;
        rh0->ctime = (uint64_t)time(NULL);
        rec_set_name(rh0, name);
        memcpy(rec0 + sizeof(invfs_inode_rec), &ast_h0, sizeof(ast_h0));
        crc0 = invfs_crc32c(rec0, rec_size0);
        if (v->inode_area_pos + rec_size0 + 4 > v->inode_area_end) { free(rec0); return 0; }
        if (vol_pre_record(v) != 0) { free(rec0); return 0; }
        if (io_seek(&v->io, v->inode_area_pos) != 0 ||
            io_write(&v->io, rec0, rec_size0) != 0 ||
            io_write(&v->io, &crc0, 4) != 0) { free(rec0); return 0; }
        v->inode_area_pos += rec_size0 + 4;
        idx_put(v, name, strlen(name), inode_id,
                v->inode_area_pos - rec_size0 - 4, rh0->file_size, rh0->ctime);
        idx_put_id(v, inode_id, v->inode_area_pos - rec_size0 - 4);
        free(rec0);
        return inode_id;
    }

    /* 1. split file into SEGMENT_SIZE chunks, LZ4-compress each */
    ast_entries = (len + SEGMENT_SIZE - 1) / SEGMENT_SIZE;
    entries = (invfs_ast_block_entry *)calloc(ast_entries, sizeof(invfs_ast_block_entry));
    seg_lz4 = (int *)calloc(ast_entries, sizeof(int));
    seg_csize = (uint32_t *)calloc(ast_entries, sizeof(uint32_t));
    if (!entries || !seg_lz4 || !seg_csize) { free(entries); free(seg_lz4); free(seg_csize); return 0; }
    /* atomic ENOSPC: refuse before writing any segment when the whole
     * file (uncompressed worst case) cannot fit above the reserve+floor */
    {
        uint64_t need = (len + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE
                        + ast_entries;   /* LZ4 overhead blocks */
        if (v->free_blocks <= v->sb.reserved_blocks + v->sb.hard_min_blocks + need) {
            fprintf(stderr, "[create] ENOSPC atomic: need %llu free %llu\n",
                    (unsigned long long)need,
                    (unsigned long long)v->free_blocks);
            free(entries); free(seg_lz4); free(seg_csize); return 0;
        }
    }

    for (i = 0; i < ast_entries; i++) {
        const uint8_t *src = data + (size_t)i * SEGMENT_SIZE;
        size_t slen = (i + 1 == ast_entries) ? len - (size_t)i * SEGMENT_SIZE : SEGMENT_SIZE;
        int cbound = LZ4_compressBound((int)slen);
        /* one block of slack past the payload: the segment goes out as whole
           blocks, so write_segment_blocks zeroes up to the block boundary */
        uint8_t *cbuf = (uint8_t *)malloc((size_t)cbound + 8 + INVFS_BLOCK_SIZE);
        uint8_t hdr[8];
        uint64_t pba, phys_blocks; int seg_zone;
        uint32_t csize, seg_crc;

        if (!cbuf) { free(entries); free(seg_lz4); free(seg_csize); return 0; }
        csize = (uint32_t)LZ4_compress_default((const char *)src, (char *)(cbuf + 8),
                                               (int)slen, cbound);
        if (csize == 0 || csize >= slen) {
            /* store raw */
            csize = (uint32_t)slen;
            memcpy(cbuf + 8, src, slen);
            seg_lz4[i] = 0;
        } else {
            seg_lz4[i] = 1;
        }
        seg_csize[i] = csize;
        seg_crc = invfs_crc32c(cbuf + 8, csize);
        if (getenv("INVFS_DEBUG"))
            printf("[create_file] seg %zu: slen=%zu csize=%u %s\n",
                   i, slen, csize, seg_lz4[i] ? "(lz4)" : "(raw)");

        hdr[0] = (uint8_t)(csize & 0xFF);
        hdr[1] = (uint8_t)((csize >> 8) & 0xFF);
        hdr[2] = (uint8_t)((csize >> 16) & 0xFF);
        hdr[3] = (uint8_t)((csize >> 24) & 0xFF);
        hdr[4] = (uint8_t)(seg_crc & 0xFF);
        hdr[5] = (uint8_t)((seg_crc >> 8) & 0xFF);
        hdr[6] = (uint8_t)((seg_crc >> 16) & 0xFF);
        hdr[7] = (uint8_t)((seg_crc >> 24) & 0xFF);
        memcpy(cbuf, hdr, 8);

        /* allocate physical blocks and write segment */
        phys_blocks = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
        pba = alloc_raw_or_shadow(v, phys_blocks, &seg_zone);
        if (pba == 0) {
            fprintf(stderr, "[create] ENOSPC seg %zu\n", i);
            /* reclaim already-written segments so ENOSPC leaves no
             * orphans (no inode record -> blocks would leak until fsck) */
            {
                size_t k;
                for (k = 0; k < i; k++) {
                    uint64_t pba_k = 0, len_k = 0;
                    if (vol_lookup_entry(v, inode_id, (uint64_t)k,
                                         &pba_k, &len_k) == 0 && pba_k) {
                        vol_free_blocks(v, pba_k, len_k);
                        l2p_remove(v, inode_id, (uint64_t)k);
                    }
                }
            }
            free(cbuf); free(entries); free(seg_lz4); free(seg_csize); return 0;
        }
        if (write_segment_blocks(v, pba, cbuf, (size_t)csize + 8,
                                 phys_blocks) != 0) {
            fprintf(stderr, "[create] write fail seg %zu pba=%llu phys=%llu%s%s\n",
                    i, (unsigned long long)pba, (unsigned long long)phys_blocks,
#ifdef _WIN32
                    " err=", "");
            fprintf(stderr, "%lu", (unsigned long)GetLastError());
#else
                    "", "");
#endif
            free(cbuf); free(entries); free(seg_lz4); free(seg_csize); return 0;
        }
        free(cbuf);

        /* L2P: segment index -> physical start */
        if (vol_map(v, inode_id, (uint64_t)i, pba, (uint32_t)phys_blocks) != 0) {
            fprintf(stderr, "[create] L2P fail seg %zu\n", i);
            free(entries); free(seg_lz4); free(seg_csize); return 0;
        }

        entries[i].file_offset = (uint64_t)i * SEGMENT_SIZE;
        entries[i].length = slen;
        entries[i].zone = (uint32_t)seg_zone;
        entries[i].algo = seg_lz4[i] ? INVFS_ALGO_LZ4 : INVFS_ALGO_NONE;
        entries[i].block_id = (uint32_t)i;   /* segment index (L2P lba) */
        entries[i].block_offset = 0;
    }
    free(seg_lz4);
    free(seg_csize);

    /* 2. AST recipe header */
    memset(&ast_h, 0, sizeof(ast_h));
    ast_h.version = 1;
    ast_h.file_size = (uint32_t)len;
    ast_h.num_blocks = (uint16_t)ast_entries;
    ast_h.num_children = 0;

    /* 3. assemble record: header + name + ast_header + entries */
    rec_size = sizeof(invfs_inode_rec) + sizeof(ast_h) + ast_entries * sizeof(invfs_ast_block_entry);
    rec = (uint8_t *)calloc(1, rec_size);
    if (!rec) { free(entries); return 0; }
    rec_h = (invfs_inode_rec *)rec;
    rec_h->magic = INODE_REC_MAGIC;
    rec_h->rec_len = (uint32_t)rec_size;
    rec_h->inode_id = inode_id;
    rec_h->file_size = len;
    rec_h->ctime = (uint64_t)time(NULL);
    rec_set_name(rec_h, name);

    memcpy(rec + sizeof(invfs_inode_rec), &ast_h, sizeof(ast_h));
    memcpy(rec + sizeof(invfs_inode_rec) + sizeof(ast_h),
           entries, ast_entries * sizeof(invfs_ast_block_entry));
    free(entries);

    crc_rec = invfs_crc32c(rec, rec_size);
    /* append record with trailing CRC32C (4 bytes) */
    if (v->inode_area_pos + rec_size + 4 > v->inode_area_end) {
        fprintf(stderr, "inode area full\n");
        free(rec);
        return 0;
    }
    /* Maps first: block_id in the AST is a segment index, so this record is
       readable only if its L2P is already on disk (see vol_pre_record). */
    if (vol_pre_record(v) != 0) { free(rec); return 0; }
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, rec, rec_size) != 0 ||
        io_write(&v->io, &crc_rec, 4) != 0) {
        free(rec);
        return 0;
    }
    v->inode_area_pos += rec_size + 4;
    idx_put(v, name, strlen(name), inode_id, v->inode_area_pos - rec_size - 4,
            rec_h->file_size, rec_h->ctime);
    idx_put_id(v, inode_id, v->inode_area_pos - rec_size - 4);
    free(rec);
    return inode_id;
}

/* ---- WP4b: incremental ranged-write sessions ----------------------------
 * A session forks the file under a NEW inode id and builds its AST
 * incrementally, one 64K segment per touched range:
 *   begin  -> alias every old segment into the new id's L2P (no copies)
 *   write  -> first touch of a segment reads its old plaintext (old id),
 *             patches, recompresses, writes a NEW pba, re-maps it
 *   commit -> append [INOD(new)][DELT(poskill old)] as ONE combo write,
 *             then drop the old id's L2P aliases WITHOUT freeing blocks
 *             (the new id owns them all now)
 * Crash before commit: old record still live and owns its blocks; orphaned
 * new-id maps are purged by fsck. No torn states. */
static int meta_read_record_by_id(invfs_volume *v, uint64_t inode_id,
                                  uint8_t **buf_out, uint32_t *rl_out,
                                  char *name_out, size_t name_cap,
                                  uint64_t *pos_out);
static const uint8_t *meta_locate_ext(const uint8_t *rec, size_t rec_len,
                                      size_t *ext_len_out);
typedef struct invfs_wsession invfs_wsession;
struct invfs_wsession {
    invfs_volume *v;
    char     name[256];
    uint64_t new_id, old_id, old_pos, old_size;
    int      have_old, committed, loaded;
    uint8_t *old_ext;
    uint32_t old_ext_len;
    invfs_ast_block_entry *ents;
    uint32_t n_ents, cap_ents, old_n_ents;
    uint8_t *touched;
    uint32_t touched_cap;
    uint32_t mat_upto;       /* highest contiguously materialized seg +1 */
    uint64_t logical_size;
};

uint64_t vol_write_begin(invfs_volume *v, const char *name, int truncate,
                         invfs_wsession **out)
{
    invfs_wsession *s;
    if (!out) return 0;
    *out = NULL;
    if (!v || !name || name_too_long(name)) return 0;
    if (v->sb.vol_flags & VOLF_READONLY) return 0;
    s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->v = v;
    snprintf(s->name, sizeof s->name, "%s", name);
    s->new_id = v->next_inode_id++;
    {
        uint64_t old_id = vol_find(v, name);
        if (old_id != 0 && !truncate) {
            s->have_old = 1;
            s->old_id = old_id;
        }
    }
    *out = s;
    return s->new_id;
}

static int wsession_load_old(invfs_wsession *s)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0;
    invfs_ast_recipe_header ah;
    const uint8_t *ext;
    size_t ext_len = 0;
    size_t base = sizeof(invfs_inode_rec);
    uint32_t i;

    if (s->loaded || !s->have_old) { s->loaded = 1; return 0; }
    if (meta_read_record_by_id(s->v, s->old_id, &buf, &rl, NULL, 0,
                               &s->old_pos) != 0 || !s->old_pos) {
        free(buf);
        return -1;
    }
    if (rl < base + sizeof(ah)) { free(buf); return -1; }
    memcpy(&ah, buf + base, sizeof(ah));
    s->old_size = ah.file_size;
    s->logical_size = ah.file_size;
    s->n_ents = ah.num_blocks;
    s->old_n_ents = ah.num_blocks;
    if (s->n_ents) {
        s->cap_ents = s->n_ents;
        s->ents = malloc(s->cap_ents * sizeof(*s->ents));
        if (!s->ents) { free(buf); return -1; }
        memcpy(s->ents, buf + base + sizeof(ah), s->n_ents * sizeof(*s->ents));
    }
    s->touched_cap = s->n_ents ? s->n_ents : 64;
    s->touched = calloc(s->touched_cap, 1);
    if (!s->touched) { free(buf); return -1; }
    ext = meta_locate_ext(buf, rl, &ext_len);
    if (ext && ext_len && ext_len <= 0xFFFF) {
        s->old_ext = malloc(ext_len);
        if (s->old_ext) {
            memcpy(s->old_ext, ext, ext_len);
            s->old_ext_len = (uint32_t)ext_len;
        }
    }
    free(buf);
    /* alias old segments into the new id's L2P (no data copies) */
    for (i = 0; i < s->n_ents; i++) {
        uint64_t pba = 0, plen = 0;
        if (vol_lookup_entry(s->v, s->old_id, i, &pba, &plen) == 0 && pba)
            vol_map(s->v, s->new_id, i, pba, (uint32_t)plen);
    }
    s->loaded = 1;
    return 0;
}

static size_t wsession_seg_plain(invfs_wsession *s, uint32_t i, uint8_t *buf)
{
    memset(buf, 0, SEGMENT_SIZE);
    if (i < s->n_ents && s->ents[i].length > 0 &&
        !(i < s->touched_cap && s->touched[i])) {
        uint64_t off = (uint64_t)i * SEGMENT_SIZE;
        if (vol_read_range(s->v, s->old_id, off, s->ents[i].length, buf) != 0)
            return 0;
        return s->ents[i].length;
    }
    return 0;
}

static int wsession_grow(invfs_wsession *s, uint32_t count)
{
    if (count > s->cap_ents) {
        uint32_t nc = s->cap_ents ? s->cap_ents : 64;
        invfs_ast_block_entry *ne;
        while (nc < count) nc *= 2;
        ne = realloc(s->ents, nc * sizeof(*ne));
        if (!ne) return -1;
        memset(ne + s->cap_ents, 0, (nc - s->cap_ents) * sizeof(*ne));
        s->ents = ne;
        s->cap_ents = nc;
    }
    if (count > s->touched_cap) {
        uint8_t *nt = realloc(s->touched, count);
        if (!nt) return -1;
        memset(nt + s->touched_cap, 0, count - s->touched_cap);
        s->touched = nt;
        s->touched_cap = count;
    }
    return 0;
}

/* materialize zero segments [from,to) so no unmapped LBA ever exists */
static int wsession_zero_fill(invfs_wsession *s, uint32_t from, uint32_t to)
{
    uint32_t k;
    if (to <= from) return 0;
    if (wsession_grow(s, to) != 0) return -1;
    for (k = from; k < to; k++) {
        uint8_t zbuf[SEGMENT_SIZE];
        int cbound = LZ4_compressBound((int)SEGMENT_SIZE);
        uint8_t *cbuf = malloc((size_t)cbound + 8 + INVFS_BLOCK_SIZE);
        uint32_t csize;
        uint8_t hdr[8];
        uint32_t seg_crc;
        uint64_t pba, phys_blocks;
        int zone;
        if (!cbuf) return -1;
        memset(zbuf, 0, SEGMENT_SIZE);
        csize = (uint32_t)LZ4_compress_default((const char *)zbuf,
                                               (char *)(cbuf + 8),
                                               SEGMENT_SIZE, cbound);
        if (csize == 0 || csize >= SEGMENT_SIZE) {
            csize = SEGMENT_SIZE;
            memcpy(cbuf + 8, zbuf, SEGMENT_SIZE);
        }
        seg_crc = invfs_crc32c(cbuf + 8, csize);
        hdr[0]=(uint8_t)(csize&0xFF); hdr[1]=(uint8_t)((csize>>8)&0xFF);
        hdr[2]=(uint8_t)((csize>>16)&0xFF); hdr[3]=(uint8_t)((csize>>24)&0xFF);
        hdr[4]=(uint8_t)(seg_crc&0xFF); hdr[5]=(uint8_t)((seg_crc>>8)&0xFF);
        hdr[6]=(uint8_t)((seg_crc>>16)&0xFF); hdr[7]=(uint8_t)((seg_crc>>24)&0xFF);
        memcpy(cbuf, hdr, 8);
        phys_blocks = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) /
                      INVFS_BLOCK_SIZE;
        pba = alloc_raw_or_shadow(s->v, phys_blocks, &zone);
        if (pba == 0 ||
            write_segment_blocks(s->v, pba, cbuf,
                                 (size_t)csize + 8, phys_blocks) != 0 ||
            vol_map(s->v, s->new_id, k, pba, (uint32_t)phys_blocks) != 0) {
            free(cbuf);
            return -1;
        }
        free(cbuf);
        s->ents[k].file_offset = (uint64_t)k * SEGMENT_SIZE;
        s->ents[k].length = SEGMENT_SIZE;
        s->ents[k].zone = (uint32_t)zone;
        s->ents[k].algo =
            (csize < SEGMENT_SIZE) ? INVFS_ALGO_LZ4 : INVFS_ALGO_NONE;
        s->ents[k].block_id = k;
        s->ents[k].block_offset = 0;
        if (k >= s->n_ents) s->n_ents = k + 1;
        if (k < s->touched_cap) s->touched[k] = 1;
    }
    if (to > s->mat_upto) s->mat_upto = to;
    return 0;
}

int vol_write_range(invfs_wsession *ws, uint64_t offset,
                    const uint8_t *data, size_t len)
{
    invfs_wsession *s = ws;
    uint32_t first, last, j;
    size_t done = 0;

    if (!s || s->committed) return -1;
    if (len == 0) return 0;
    if ((uint64_t)offset + len > 0xFFFFFFFFu) return -1;
    if (wsession_load_old(s) != 0) return -1;

    first = (uint32_t)(offset / SEGMENT_SIZE);
    last  = (uint32_t)((offset + len - 1) / SEGMENT_SIZE);
    if ((uint64_t)last + 1 > MAX_SEGMENTS) return -1;
    /* load_old aliases old segments; anything past the aliased tail must
     * exist as real zero segments before we can jump ahead */
    {
        uint32_t alias_tail = s->have_old ? s->old_n_ents : 0;
        uint32_t filled = s->mat_upto > alias_tail ? s->mat_upto : alias_tail;
        if (first > filled && wsession_zero_fill(s, filled, first) != 0)
            return -1;
    }
    if (wsession_grow(s, last + 1) != 0) return -1;

    for (j = first; j <= last; j++) {
        uint8_t plain[SEGMENT_SIZE];
        size_t base = (size_t)j * SEGMENT_SIZE;
        size_t seg_off = offset + done - base;
        size_t can = SEGMENT_SIZE - seg_off;
        size_t take = len - done < can ? len - done : can;
        size_t plain_len;
        int cbound, lz4_used = 0;
        uint8_t *cbuf, hdr[8];
        uint32_t csize, seg_crc;
        uint64_t pba, phys_blocks;
        int zone;

        plain_len = wsession_seg_plain(s, j, plain);
        memcpy(plain + seg_off, data + done, take);
        if (seg_off + take > plain_len) plain_len = seg_off + take;
        /* compress the WHOLE segment: interior zero tails belong to this
         * entry; LZ4 collapses them to almost nothing */
        plain_len = SEGMENT_SIZE;

        cbound = LZ4_compressBound((int)plain_len);
        cbuf = malloc((size_t)cbound + 8 + INVFS_BLOCK_SIZE);
        if (!cbuf) return -1;
        csize = (uint32_t)LZ4_compress_default((const char *)plain,
                                               (char *)(cbuf + 8),
                                               (int)plain_len, cbound);
        if (csize == 0 || csize >= (uint32_t)plain_len) {
            csize = (uint32_t)plain_len;
            memcpy(cbuf + 8, plain, plain_len);
        } else {
            lz4_used = 1;
        }
        seg_crc = invfs_crc32c(cbuf + 8, csize);
        hdr[0] = (uint8_t)(csize & 0xFF);
        hdr[1] = (uint8_t)((csize >> 8) & 0xFF);
        hdr[2] = (uint8_t)((csize >> 16) & 0xFF);
        hdr[3] = (uint8_t)((csize >> 24) & 0xFF);
        hdr[4] = (uint8_t)(seg_crc & 0xFF);
        hdr[5] = (uint8_t)((seg_crc >> 8) & 0xFF);
        hdr[6] = (uint8_t)((seg_crc >> 16) & 0xFF);
        hdr[7] = (uint8_t)((seg_crc >> 24) & 0xFF);
        memcpy(cbuf, hdr, 8);

        phys_blocks = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) /
                      INVFS_BLOCK_SIZE;
        pba = alloc_raw_or_shadow(s->v, phys_blocks, &zone);
        if (pba == 0 ||
            write_segment_blocks(s->v, pba, cbuf,
                                 (size_t)csize + 8, phys_blocks) != 0 ||
            vol_map(s->v, s->new_id, j, pba, (uint32_t)phys_blocks) != 0) {
            fprintf(stderr, "[wsession] seg %u write fail\n", j);
            free(cbuf);
            return -1;
        }
        free(cbuf);

        s->ents[j].file_offset = (uint64_t)j * SEGMENT_SIZE;
        /* full segment unless it is the last one of the file: interior
         * tails are zeros and MUST be covered by this entry, or reads
         * past plain_len fall into an uncovered hole */
        s->ents[j].length =
            (base + SEGMENT_SIZE <= s->logical_size ||
             j + 1 < s->n_ents) ? SEGMENT_SIZE : plain_len;
        s->ents[j].zone = (uint32_t)zone;
        s->ents[j].algo = lz4_used ? INVFS_ALGO_LZ4 : INVFS_ALGO_NONE;
        s->ents[j].block_id = j;
        s->ents[j].block_offset = 0;
        if (j >= s->n_ents) s->n_ents = j + 1;
        if (j < s->touched_cap) s->touched[j] = 1;
        if (j + 1 > s->mat_upto) s->mat_upto = j + 1;

        {
            uint64_t dend = (uint64_t)j * SEGMENT_SIZE + seg_off + take;
            if (dend > s->logical_size) s->logical_size = dend;
        }
        done += take;
        if (done >= len) break;
    }
    return 0;
}

int vol_write_commit(invfs_wsession *ws)
{
    invfs_wsession *s = ws;
    invfs_volume *v = ws ? ws->v : NULL;
    size_t rec_size, total;
    uint8_t *rec, *combo, *p;
    invfs_inode_rec *rh, tomb;
    invfs_ast_recipe_header ah;
    uint32_t crc_rec, crc_tomb;
    uint64_t now = (uint64_t)time(NULL);

    if (!s || s->committed) return -1;
    if (wsession_load_old(s) != 0) return -1;
    if (s->logical_size > 0xFFFFFFFFu || s->n_ents > MAX_SEGMENTS) return -1;

    memset(&ah, 0, sizeof(ah));
    ah.version = 1;
    ah.file_size = (uint32_t)s->logical_size;
    ah.num_blocks = (uint16_t)s->n_ents;

    rec_size = sizeof(invfs_inode_rec) + sizeof(ah)
             + (size_t)s->n_ents * sizeof(invfs_ast_block_entry)
             + s->old_ext_len;
    rec = calloc(1, rec_size);
    if (!rec) return -1;
    rh = (invfs_inode_rec *)rec;
    rh->magic = INODE_REC_MAGIC;
    rh->rec_len = (uint32_t)rec_size;
    rh->inode_id = s->new_id;
    rh->file_size = (uint32_t)s->logical_size;
    rh->ctime = now;
    rec_set_name(rh, s->name);
    memcpy(rec + sizeof(invfs_inode_rec), &ah, sizeof(ah));
    if (s->n_ents)
        memcpy(rec + sizeof(invfs_inode_rec) + sizeof(ah), s->ents,
               (size_t)s->n_ents * sizeof(invfs_ast_block_entry));
    if (s->old_ext_len)
        memcpy(rec + rec_size - s->old_ext_len, s->old_ext, s->old_ext_len);
    crc_rec = invfs_crc32c(rec, rec_size);

    memset(&tomb, 0, sizeof(tomb));
    tomb.magic = TOMBSTONE_MAGIC;
    tomb.rec_len = (uint32_t)sizeof(tomb);
    if (s->have_old) {
        uint8_t *obuf = NULL;
        uint32_t orl = 0;
        invfs_inode_rec oh;
        if (meta_read_record_by_id(v, s->old_id, &obuf, &orl,
                                   NULL, 0, NULL) == 0 && orl >= sizeof(oh)) {
            memcpy(&oh, obuf, sizeof(oh));
            tomb.name_len = oh.name_len;
            memcpy(tomb.name, oh.name, sizeof(tomb.name));
        }
        free(obuf);
        tomb.inode_id = s->old_id;
        tomb.file_size = s->old_pos;      /* v2 position kill */
        v->hot.tombstones++;
    }

    total = rec_size + 4 + (s->have_old ? sizeof(tomb) + 4 : 0);
    combo = malloc(total);
    if (!combo) { free(rec); return -1; }
    p = combo;
    memcpy(p, rec, rec_size); p += rec_size;
    memcpy(p, &crc_rec, 4);   p += 4;
    if (s->have_old) {
        crc_tomb = invfs_crc32c((uint8_t *)&tomb, sizeof(tomb));
        memcpy(p, &tomb, sizeof(tomb)); p += sizeof(tomb);
        memcpy(p, &crc_tomb, 4);
    }
    free(rec);

    if (vol_mark_dirty(v) != 0 || vol_pre_record(v) != 0 ||
        v->inode_area_pos + total > v->inode_area_end ||
        io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, combo, total) != 0) {
        free(combo);
        return -1;
    }
    v->inode_area_pos += total;
    free(combo);

    idx_put(v, s->name, strlen(s->name), s->new_id,
            v->inode_area_pos - total, (uint32_t)s->logical_size, now);
    idx_put_id(v, s->new_id, v->inode_area_pos - total);

    if (s->have_old) {
        /* blocks re-owned by the new id: drop old aliases, no frees.
         * arc entry for the old id dies naturally -- ids never repeat. */
        uint32_t i;
        arc_invalidate(v->arc, s->old_id);
        for (i = 0; i < s->old_n_ents; i++)
            l2p_remove(v, s->old_id, i);
    }
    s->committed = 1;
    return 0;
}

void vol_write_abort(invfs_wsession *ws)
{
    invfs_wsession *s = ws;
    uint32_t i;
    if (!s) return;
    if (!s->committed) {
        for (i = 0; s->touched && i < s->n_ents && i < s->touched_cap; i++) {
            uint64_t pba = 0, plen = 0;
            if (s->touched[i] &&
                vol_lookup_entry(s->v, s->new_id, i, &pba, &plen) == 0 && pba) {
                vol_free_blocks(s->v, pba, plen);
                l2p_remove(s->v, s->new_id, i);
            }
        }
        for (i = 0; s->have_old && s->ents && i < s->old_n_ents; i++)
            l2p_remove(s->v, s->new_id, i);
    }
    free(s->ents);
    free(s->touched);
    free(s->old_ext);
    free(s);
}


/* ---- AST children: serialize / deserialize / container creation ---- */

/* serialize children after the block entries; malloc'd buf or NULL */
static uint8_t *vol_serialize_children(const invfs_ast_child_entry *ch,
                                       size_t n, size_t *len_out)
{
    size_t total = 0;
    uint8_t *buf, *p;
    for (size_t i = 0; i < n; i++) {
        if (ch[i].name_len > MAX_AST_CHILD_NAME) return NULL;
        total += 2 + ch[i].name_len + 2 + 4 + 4 + 4 + 4;
        if (total > INVFS_MAX_CHILD_BLOB) return NULL;  /* cap children blob */
    }
    buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf) return NULL;
    p = buf;
    for (size_t i = 0; i < n; i++) {
        uint16_t nl = (uint16_t)ch[i].name_len;
        uint16_t method = ch[i].method;
        memcpy(p, &nl, 2); p += 2;
        memcpy(p, ch[i].name, nl); p += nl;
        memcpy(p, &method, 2); p += 2;
        memcpy(p, &ch[i].csize, 4); p += 4;
        memcpy(p, &ch[i].usize, 4); p += 4;
        memcpy(p, &ch[i].crc, 4); p += 4;
        memcpy(p, &ch[i].data_off, 4); p += 4;
    }
    *len_out = total;
    return buf;
}

/* deserialize children with hard bounds against blob_len (no OOM) */
static int vol_deserialize_children(const uint8_t *blob, size_t blob_len,
                                    invfs_ast_child_entry **out, size_t *n_out)
{
    const uint8_t *p = blob, *end = blob + blob_len;
    invfs_ast_child_entry *ch;
    size_t n = 0, cap = 64;
    ch = (invfs_ast_child_entry *)malloc(cap * sizeof(*ch));
    if (!ch) return -1;
    while (p + 2 <= end && n < MAX_AST_CHILDREN) {
        uint16_t nl;
        memcpy(&nl, p, 2); p += 2;
        if (nl > MAX_AST_CHILD_NAME || p + nl + 18 > end) break;  /* corrupt */
        if (n == cap) {
            cap *= 2;
            invfs_ast_child_entry *nc =
                (invfs_ast_child_entry *)realloc(ch, cap * sizeof(*ch));
            if (!nc) { free(ch); return -1; }
            ch = nc;
        }
        ch[n].name_len = nl;
        memcpy(ch[n].name, p, nl); ch[n].name[nl] = 0;
        p += nl;
        if (p + 20 > end) break;
        memcpy(&ch[n].method, p, 2); p += 2;
        memcpy(&ch[n].csize, p, 4); p += 4;
        memcpy(&ch[n].usize, p, 4); p += 4;
        memcpy(&ch[n].crc, p, 4); p += 4;
        memcpy(&ch[n].data_off, p, 4); p += 4;
        n++;
    }
    if (n == 0) { free(ch); *out = NULL; *n_out = 0; return 0; }
    *out = ch;
    *n_out = n;
    return 0;
}

/* create file with container children (ZIP members) — bounded allocs */
uint64_t vol_create_container_file(invfs_volume *v, const char *name,
                                   const uint8_t *data, size_t len,
                                   const invfs_ast_child_entry *children,
                                   size_t nchildren)
{
    if (name_too_long(name)) return 0;
    size_t i, ast_entries;
    uint64_t inode_id = v->next_inode_id++;
    invfs_ast_recipe_header ast_h;
    size_t rec_size, children_blob_len = 0;
    uint8_t *rec, *children_blob = NULL;
    invfs_inode_rec *rec_h;
    invfs_ast_block_entry *entries;
    uint32_t crc_rec;
    int *seg_lz4 = NULL;
    uint32_t *seg_csize = NULL;

    if (nchildren > MAX_AST_CHILDREN) return 0;
    children_blob = vol_serialize_children(children, nchildren, &children_blob_len);
    if (nchildren && !children_blob) return 0;

    ast_entries = (len + SEGMENT_SIZE - 1) / SEGMENT_SIZE;
    /* ast_h.file_size is uint32 — a larger container would be recorded at
       its truncated size and rebuild short */
    if (len > 0xFFFFFFFFu) { free(children_blob); return 0; }
    if (ast_entries > MAX_SEGMENTS) { free(children_blob); return 0; }
    entries = (invfs_ast_block_entry *)calloc(ast_entries, sizeof(invfs_ast_block_entry));
    seg_lz4 = (int *)calloc(ast_entries, sizeof(int));
    seg_csize = (uint32_t *)calloc(ast_entries, sizeof(uint32_t));
    if (!entries || !seg_lz4 || !seg_csize) {
        free(entries); free(seg_lz4); free(seg_csize); free(children_blob);
        return 0;
    }

    for (i = 0; i < ast_entries; i++) {
        const uint8_t *src = data + (size_t)i * SEGMENT_SIZE;
        size_t slen = (i + 1 == ast_entries) ? len - (size_t)i * SEGMENT_SIZE : SEGMENT_SIZE;
        int cbound = LZ4_compressBound((int)slen);
        /* one block of slack past the payload (see vol_create_file) */
        uint8_t *cbuf = (uint8_t *)malloc((size_t)cbound + 8 + INVFS_BLOCK_SIZE);
        uint8_t hdr[8];
        uint64_t pba, phys_blocks; int seg_zone;
        uint32_t csize, seg_crc;

        if (!cbuf) { free(entries); free(seg_lz4); free(seg_csize); free(children_blob); return 0; }
        csize = (uint32_t)LZ4_compress_default((const char *)src, (char *)(cbuf + 8),
                                               (int)slen, cbound);
        if (csize == 0 || csize >= slen) {
            csize = (uint32_t)slen;
            memcpy(cbuf + 8, src, slen);
            seg_lz4[i] = 0;
        } else {
            seg_lz4[i] = 1;
        }
        seg_csize[i] = csize;
        seg_crc = invfs_crc32c(cbuf + 8, csize);
        /* segment header: [4B csize][4B crc32c(data)][data] */
        hdr[0] = (uint8_t)(csize & 0xFF);
        hdr[1] = (uint8_t)((csize >> 8) & 0xFF);
        hdr[2] = (uint8_t)((csize >> 16) & 0xFF);
        hdr[3] = (uint8_t)((csize >> 24) & 0xFF);
        hdr[4] = (uint8_t)(seg_crc & 0xFF);
        hdr[5] = (uint8_t)((seg_crc >> 8) & 0xFF);
        hdr[6] = (uint8_t)((seg_crc >> 16) & 0xFF);
        hdr[7] = (uint8_t)((seg_crc >> 24) & 0xFF);
        memcpy(cbuf, hdr, 8);

        phys_blocks = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
        pba = alloc_blocks(v, v->sb.raw_zone_start, v->sb.raw_zone_blocks, phys_blocks, 0);
        if (pba == 0) {
            fprintf(stderr, "[create] ENOSPC seg %zu\n", i);
            /* reclaim already-written segments (no orphans) */
            {
                size_t k;
                for (k = 0; k < i; k++) {
                    uint64_t pba_k = 0, len_k = 0;
                    if (vol_lookup_entry(v, inode_id, (uint64_t)k,
                                         &pba_k, &len_k) == 0 && pba_k) {
                        vol_free_blocks(v, pba_k, len_k);
                        l2p_remove(v, inode_id, (uint64_t)k);
                    }
                }
            }
            free(cbuf); free(entries); free(seg_lz4); free(seg_csize); free(children_blob);
            return 0;
        }
        if (write_segment_blocks(v, pba, cbuf, (size_t)csize + 8,
                                 phys_blocks) != 0) {
            fprintf(stderr, "[create] write fail seg %zu\n", i);
            free(cbuf); free(entries); free(seg_lz4); free(seg_csize); free(children_blob);
            return 0;
        }
        free(cbuf);
        if (vol_map(v, inode_id, (uint64_t)i, pba, (uint32_t)phys_blocks) != 0) {
            fprintf(stderr, "[create] L2P fail seg %zu\n", i);
            free(entries); free(seg_lz4); free(seg_csize); free(children_blob);
            return 0;
        }
        entries[i].file_offset = (uint64_t)i * SEGMENT_SIZE;
        entries[i].length = slen;
        entries[i].zone = INVFS_ZONE_RAW;
        entries[i].algo = seg_lz4[i] ? INVFS_ALGO_LZ4 : INVFS_ALGO_NONE;
        entries[i].block_id = (uint32_t)i;
        entries[i].block_offset = 0;
    }
    free(seg_lz4);
    free(seg_csize);

    memset(&ast_h, 0, sizeof(ast_h));
    ast_h.version = 1;
    ast_h.file_size = (uint32_t)len;   /* container = original archive bytes (1:1) */
    ast_h.num_blocks = (uint16_t)ast_entries;
    ast_h.num_children = (uint16_t)(nchildren > 0xFFFF ? 0xFFFF : nchildren);

    rec_size = sizeof(invfs_inode_rec) + sizeof(ast_h) +
               ast_entries * sizeof(invfs_ast_block_entry) + children_blob_len;
    rec = (uint8_t *)calloc(1, rec_size);
    if (!rec) { free(entries); free(children_blob); return 0; }
    rec_h = (invfs_inode_rec *)rec;
    rec_h->magic = INODE_REC_MAGIC;
    rec_h->rec_len = (uint32_t)rec_size;
    rec_h->inode_id = inode_id;
    rec_h->file_size = (uint64_t)len;   /* original archive size (1:1) */
    rec_h->ctime = (uint64_t)time(NULL);
    rec_set_name(rec_h, name);

    memcpy(rec + sizeof(invfs_inode_rec), &ast_h, sizeof(ast_h));
    memcpy(rec + sizeof(invfs_inode_rec) + sizeof(ast_h),
           entries, ast_entries * sizeof(invfs_ast_block_entry));
    if (children_blob_len)
        memcpy(rec + sizeof(invfs_inode_rec) + sizeof(ast_h) +
               ast_entries * sizeof(invfs_ast_block_entry),
               children_blob, children_blob_len);
    free(entries);
    free(children_blob);

    crc_rec = invfs_crc32c(rec, rec_size);
    if (v->inode_area_pos + rec_size + 4 > v->inode_area_end) {
        fprintf(stderr, "inode area full\n");
        free(rec);
        return 0;
    }
    if (vol_pre_record(v) != 0) { free(rec); return 0; }
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, rec, rec_size) != 0 ||
        io_write(&v->io, &crc_rec, 4) != 0) {
        free(rec);
        return 0;
    }
    v->inode_area_pos += rec_size + 4;
    idx_put(v, name, strlen(name), inode_id, v->inode_area_pos - rec_size - 4,
            rec_h->file_size, rec_h->ctime);
    idx_put_id(v, inode_id, v->inode_area_pos - rec_size - 4);
    free(rec);
    return inode_id;
}

/* parse ZIP central directory into children (bounded, no OOM);
 * returns number of children, -1 on structural failure */
int vol_zip_parse_children(const uint8_t *z, size_t zlen,
                           invfs_ast_child_entry *ch, size_t maxch)
{
    size_t eocd = zlen >= 22 ? zlen - 22 : 0;
    uint16_t ncen;
    uint32_t cen_off;
    size_t p;
    size_t n = 0;
    while (eocd > 0 && !(z[eocd] == 'P' && z[eocd + 1] == 'K' &&
                         z[eocd + 2] == 5 && z[eocd + 3] == 6))
        eocd--;
    if (!(z[eocd] == 'P' && z[eocd + 1] == 'K' && z[eocd + 2] == 5 && z[eocd + 3] == 6))
        return -1;
    memcpy(&ncen, z + eocd + 10, 2);
    memcpy(&cen_off, z + eocd + 16, 4);
    if (cen_off >= zlen) return -1;
    if (ncen > maxch) ncen = (uint16_t)maxch;
    p = cen_off;
    for (uint16_t i = 0; i < ncen; i++) {
        uint16_t nlen, elen, clen;
        uint32_t usize, crc, local_off;
        if (p + 46 > zlen || !(z[p] == 'P' && z[p + 1] == 'K' &&
                               z[p + 2] == 1 && z[p + 3] == 2))
            break;
        memcpy(&crc, z + p + 16, 4);
        memcpy(&usize, z + p + 24, 4);
        memcpy(&nlen, z + p + 28, 2);
        memcpy(&elen, z + p + 30, 2);
        memcpy(&clen, z + p + 32, 2);
        memcpy(&local_off, z + p + 42, 4);
        if (nlen > MAX_AST_CHILD_NAME || p + 46 + nlen > zlen) break;
        ch[n].name_len = nlen;
        memcpy(ch[n].name, z + p + 46, nlen);
        ch[n].name[nlen] = 0;
        ch[n].method = 0;
        ch[n].csize = 0;
        ch[n].usize = usize;
        ch[n].crc = crc;
        ch[n].data_off = 0;
        {
            /* window into the container: method + compressed size + data
             * offset, from the central directory and local header */
            uint16_t method, lh_nlen, lh_elen;
            uint32_t csize;
            memcpy(&method, z + p + 10, 2);
            memcpy(&csize, z + p + 20, 4);
            if (local_off + 30 <= zlen &&
                z[local_off] == 'P' && z[local_off + 1] == 'K' &&
                z[local_off + 2] == 3 && z[local_off + 3] == 4) {
                memcpy(&lh_nlen, z + local_off + 26, 2);
                memcpy(&lh_elen, z + local_off + 28, 2);
                if (local_off + 30 + lh_nlen + lh_elen + csize <= zlen) {
                    ch[n].method = method;
                    ch[n].csize = csize;
                    ch[n].data_off = (uint32_t)(local_off + 30 + lh_nlen + lh_elen);
                }
            }
        }
        n++;
        p += 46 + nlen + elen + clen;
    }
    return (int)n;
}

/* read children from an inode record (bounded); 0 = none, -1 = corrupt */
int vol_get_children(invfs_volume *v, uint64_t inode_id,
                     invfs_ast_child_entry **out, size_t *n_out)
{
    invfs_ast_recipe_header ast_h;
    uint8_t *rec = NULL;
    size_t rec_len, nblocks;
    uint64_t p;
    uint32_t magic;
    uint64_t ino, fsz;
    uint32_t rl;

    *out = NULL;
    *n_out = 0;
    p = vol_inode_area_start(v);
    /* Jump straight to the record. This used to scan the whole inode area for
       every call, and invf-ls calls it once per listed file -- listing 40k
       files meant 40k full-area scans. 0 means not indexed; fall back to the
       scan, which is also what keeps the old semantics for a stale id. */
    {
        uint64_t ip = idx_get_id(v, inode_id);
        if (ip >= p && ip + sizeof(invfs_inode_rec) <= v->inode_area_pos)
            p = ip;
    }
    while ((p = vol_inode_next(v, p, &magic, &ino, &fsz, NULL, 0, &rl)) != 0) {
        if (magic != INODE_REC_MAGIC || ino != inode_id) continue;
        rec = (uint8_t *)malloc(rl);
        if (!rec) return -1;
        if (vol_read_raw(v, p - rl - 4, rec, rl) != 0) { free(rec); return -1; }
        rec_len = rl;
        if (rec_len < sizeof(invfs_inode_rec) + sizeof(ast_h)) { free(rec); return -1; }
        memcpy(&ast_h, rec + sizeof(invfs_inode_rec), sizeof(ast_h));
        nblocks = ast_h.num_blocks;
        if (rec_len < sizeof(invfs_inode_rec) + sizeof(ast_h) +
                      nblocks * sizeof(invfs_ast_block_entry)) {
            free(rec);
            return -1;
        }
        if (ast_h.num_children == 0) { free(rec); *out = NULL; *n_out = 0; return 0; }
        {
            const uint8_t *children_blob = rec + sizeof(invfs_inode_rec) + sizeof(ast_h) +
                                           nblocks * sizeof(invfs_ast_block_entry);
            int rc = vol_deserialize_children(children_blob,
                                              rec_len - (size_t)(children_blob - rec),
                                              out, n_out);
            free(rec);
            return rc;
        }
    }
    return -1;  /* inode not found */
}

/* find inode record by name; returns inode_id (0 = not found) */
uint64_t vol_find(invfs_volume *v, const char *name)
{
    /* O(1) via the in-memory index; last-record-wins and the
       tombstone-kills-only-its-own-version rule are applied when the
       index is built and maintained, not re-derived here. */
    const name_index_entry *e = idx_get(v, name, strlen(name));
    return e ? e->inode_id : 0;
}

/* ---- virtual directories (prefix-based; mkdir creates an empty anchor
   file named "dir/"; nested files "dir/x" make dir visible) ---- */

/* does a directory exist? anchor "name/" or any file under "name/" */
int vol_is_dir(invfs_volume *v, const char *name)
{
    char pre[300];
    int plen;
    if (name[0] == 0) return 1;   /* root always exists */
    plen = snprintf(pre, sizeof pre, "%s/", name);
    if (plen <= 0 || (size_t)plen >= sizeof pre) return 0;
    /* nonzero live count under "name/" -- the anchor record counts too, so
       an empty directory stays visible, and a tombstoned one does not */
    return idx_dir_live(v, pre, (size_t)plen);
}

/* create empty directory anchor "name/" */
uint64_t vol_mkdir(invfs_volume *v, const char *name)
{
    char anchor[300];
    if (v->sb.vol_flags & VOLF_READONLY) return 0;   /* EROFS */
    if (!name || name[0] == 0 || strlen(name) > 240) return 0;
    if (vol_find(v, name) != 0) return 0;   /* plain file with same name */
    snprintf(anchor, sizeof anchor, "%s/", name);
    return vol_create_file(v, anchor, NULL, 0);
}

/* remove directory: must be empty (only the anchor, no files inside) */
int vol_rmdir(invfs_volume *v, const char *name)
{
    char anchor[300];
    int alen;
    if (v->sb.vol_flags & VOLF_READONLY) return -1;   /* EROFS */
    if (!name || name[0] == 0) return -1;
    alen = snprintf(anchor, sizeof anchor, "%s/", name);
    if (alen <= 0 || (size_t)alen >= sizeof anchor) return -1;
    /* Live records under "name/", minus the anchor itself, are the
       directory's contents. Counting through the index also fixes the old
       scan, which accepted records already killed by a later tombstone and
       so kept an emptied directory ENOTEMPTY forever. */
    if (idx_dir_count(v, anchor, (size_t)alen) >
        (idx_get(v, anchor, (size_t)alen) ? 1u : 0u)) {
        /* Ghost tolerance: unlink-while-open (.fuse_hidden dance) can
         * leave children whose index entry outlived its record. If every
         * listed child resolves to no live name index entry, the dir is
         * de-facto empty -- scrub and remove instead of ENOTEMPTY. */
        int listed = 0, alive = 0;
        char child[600];
        invfs_dirent ents[64];
        int n = vol_list_dir(v, name, ents, 64);
        int k;
        for (k = 0; k < n; k++) {
            if (ents[k].name[0] == 0) continue;
            snprintf(child, sizeof child, "%s/%s", name, ents[k].name);
            listed++;
            if (vol_find(v, child) != 0 || vol_is_dir(v, child)) alive++;
        }
        if (alive == 0 && listed > 0 && n <= 64) {
            for (k = 0; k < n; k++) {
                if (ents[k].name[0] == 0) continue;
                snprintf(child, sizeof child, "%s/%s", name, ents[k].name);
                idx_del(v, child, (size_t)strlen(child), 0);
                idx_bump_dirs(v, child, strlen(child), -1);
            }
            v->dirty = 1;
        } else {
            return -2;   /* genuinely not empty */
        }
    }
    return vol_delete_file(v, anchor);
}

/* auto-create parent anchors for a path: "a/b/c.txt" -> "a/", "a/b/" */
int vol_ensure_path(invfs_volume *v, const char *name)
{
    char tmp[256];
    size_t n = strlen(name);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, name, n + 1);
    for (size_t i = 0; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (tmp[0] && !vol_is_dir(v, tmp))
                vol_mkdir(v, tmp);
            tmp[i] = '/';
        }
    }
    return 0;
}

static int dirent_cmp(const void *a, const void *b)
{
    return strcmp(((const invfs_dirent *)a)->name,
                  ((const invfs_dirent *)b)->name);
}

/* list one directory level: first path component after "dir/" */
int vol_list_dir(invfs_volume *v, const char *dir, invfs_dirent *ents, int max)
{
    char pre[300];
    size_t pren;
    size_t n = 0, b;
    /* Dedup set over the entries collected so far. The old code compared each
       candidate against every entry already found, which is O(n^2) on top of
       the scan; a directory of 1600 files cost ~1.3M strcmp per listing.
       `at` is the slot in `ents`: a subdirectory is usually discovered from a
       file inside it, and its own anchor record -- the one holding its ctime
       -- may only turn up later in the walk. */
    struct dedup { struct dedup *next; size_t at; int is_dir; char name[1]; };
    struct dedup **seen = NULL;
    size_t seen_mask = 0;

    if (dir[0] == 0) { pre[0] = 0; pren = 0; }
    else {
        int pl = snprintf(pre, sizeof pre, "%s/", dir);
        if (pl <= 0 || (size_t)pl >= sizeof pre) return 0;
        pren = (size_t)pl;
    }

    /* Walk the in-memory name index instead of re-reading the inode area.
       Windows enumerates a directory after every file creation, so this path
       ran once per created file and read every record on the volume each
       time: it was the entire cost of writing a file (10.4 ms of a 10.7 ms
       write at 1600 files), and it grew linearly with the file count.
       The index holds exactly the live names -- last-record-wins and the
       tombstone rules are applied as it is maintained -- so the separate
       tombstone post-pass that used to follow is gone too. */
    if (!v->nbuck) return 0;
    {
        size_t sc = 64;
        while (sc < v->ncount) sc *= 2;
        seen = (struct dedup **)calloc(sc, sizeof *seen);
        if (!seen) return -1;
        seen_mask = sc - 1;
    }

    for (b = 0; b <= v->nmask && n < (size_t)max; b++) {
        const name_index_entry *e;
        for (e = v->nbuck[b]; e && n < (size_t)max; e = e->next) {
            const char *rest, *slash;
            size_t nl = e->nlen, rl, flen, hb;
            char first[256];
            int is_dir;
            struct dedup *d;

            if (nl <= pren || memcmp(e->name, pre, pren) != 0) continue;
            rest = e->name + pren;
            rl = nl - pren;
            slash = (const char *)memchr(rest, '/', rl);
            if (slash) { flen = (size_t)(slash - rest); is_dir = 1; }
            else { flen = rl; is_dir = 0; }
            if (flen == 0 || flen >= sizeof first) continue;   /* "a//b" */
            memcpy(first, rest, flen);
            first[flen] = 0;
            /* skip internal '!' siblings (recipe/cover/part/jxl) */
            if (memchr(first, '!', flen)) continue;
            /* hide control-prefixed internal names (the "\x01tzb" owner) */
            if ((uint8_t)first[0] == 0x01) continue;

            hb = (size_t)(idx_hash(first, flen) & seen_mask);
            for (d = seen[hb]; d; d = d->next)
                if (d->is_dir == is_dir && strcmp(d->name, first) == 0) break;
            if (d) {
                /* The anchor record "d/" is the directory's own record; its
                   ctime is the one to report. Any other record that maps to
                   the same entry is a file underneath it. */
                if (is_dir && rl == flen + 1) ents[d->at].ctime = e->ctime;
                continue;
            }
            d = (struct dedup *)malloc(sizeof *d + flen);
            if (!d) continue;
            memcpy(d->name, first, flen + 1);
            d->is_dir = is_dir;
            d->at = n;
            d->next = seen[hb];
            seen[hb] = d;

            memcpy(ents[n].name, first, flen + 1);
            ents[n].is_dir = is_dir;
            /* Carried from the index so the caller needs no per-entry stat.
               A directory's size is meaningless; report 0 unless its own
               anchor record comes along. */
            ents[n].size = is_dir ? 0 : e->size;
            ents[n].ctime = e->ctime;
            n++;
        }
    }

    for (b = 0; b <= seen_mask; b++) {
        struct dedup *d = seen[b];
        while (d) { struct dedup *nx = d->next; free(d); d = nx; }
    }
    free(seen);
    /* Hash order is an implementation detail and would shift between mounts.
       The inode-area scan used to yield creation order; sort by name so the
       listing is at least stable. */
    if (n > 1) qsort(ents, n, sizeof *ents, dirent_cmp);
    return (int)n;
}

/* read file data back via AST + L2P (recursive for containers); returns 0 */
static int invfs_png_from_jxl(invfs_volume *v, uint64_t jxl_inode,
                              uint8_t **rgb, size_t *rgb_len);
static int mz_tdefl_compress(const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t out_cap, int level,
                             size_t *out_len);

/* ---- WP14b M2: exe-as-container shared helpers ----
 * The main record's segment is ZSTD-19 of the EXER payload (see
 * invarifs.h): [4B "EXER"][u32 LE num_parts][per part u64 file_offset +
 * u64 member_len + u8 codec][glue bytes]. codec = INVFS_ALGO_JXL for JPEG
 * members (the sibling "name!exrN" holds the lossless JXL blob),
 * INVFS_ALGO_ZSTD for PNG members (recognized and carved, but stored
 * recompressed-only: djxl's PNG output is a fresh encoding, so a
 * pixel-transcode could never pass the bit-exact guard -- the PNGR
 * machinery that could rebuild one is Windows-only, WP12(c)). */

static void exer_wr64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) { p[i] = (uint8_t)v; v >>= 8; }
}

static uint64_t exer_rd64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

#define EXE_MEDIA_MIN (16 * 1024)   /* smaller finds are not worth carving */
#define EXE_MAX_MEDIA 64            /* sanity bound on regions per exe */

typedef struct {
    uint64_t off, len;
    int kind;                       /* 1 = JPEG, 2 = PNG */
} exe_region;

/* one row of the EXER payload's part table (wire form; the codec is an
 * INVFS_ALGO_* value so the table is self-describing) */
typedef struct {
    uint64_t off, len;
    uint8_t codec;
} exer_row;

/* End of the JPEG stream that starts at p (FF D8 FF validated by the
 * caller): walk the marker stream to EOI. Segments carry their length, so
 * thumbnails inside APPn cannot truncate the walk; after SOS the
 * entropy-coded run ends at the first FF NOT followed by 00 (stuffing),
 * D0-D7 (RSTn) or FF (fill) -- that is the next marker. Valid only if a
 * SOFn appeared before EOI. Returns the offset just past FF D9, 0 on
 * truncation/malformation. */
static size_t jpeg_scan_end(const uint8_t *b, size_t n, size_t p)
{
    int saw_sof = 0;

    p += 2;   /* past SOI */
    while (p + 1 < n) {
        uint8_t m;
        if (b[p] != 0xFF) return 0;
        m = b[p + 1];
        if (m == 0xFF) { p++; continue; }      /* fill bytes */
        if (m == 0x00) return 0;               /* stuffed byte: not a marker */
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            p += 2;                            /* SOI/TEM/RSTn: no length */
            continue;
        }
        if (m == 0xD9)
            return saw_sof ? p + 2 : 0;        /* EOI */
        if (p + 4 > n) return 0;
        {
            unsigned sl = ((unsigned)b[p + 2] << 8) | b[p + 3];
            if (sl < 2 || p + 2 + sl > n) return 0;
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 &&
                m != 0xCC)
                saw_sof = 1;                   /* SOFn (any encoding) */
            p += 2 + sl;
            if (m == 0xDA) {
                /* SOS: entropy run follows the header */
                for (;;) {
                    if (p + 1 >= n) return 0;
                    if (b[p] == 0xFF && b[p + 1] != 0x00 &&
                        !(b[p + 1] >= 0xD0 && b[p + 1] <= 0xD7) &&
                        b[p + 1] != 0xFF)
                        break;
                    p++;
                }
            }
        }
    }
    return 0;
}

/* End of the PNG stream starting at p (8-byte signature validated by the
 * caller): walk [u32 BE len][4B type][data][4B crc] chunks to IEND; IHDR
 * must come first. Returns the offset just past IEND, 0 on truncation or
 * structural garbage. */
static size_t png_scan_end(const uint8_t *b, size_t n, size_t p)
{
    int first = 1;

    p += 8;
    while (p + 12 <= n) {           /* shortest chunk: len+type+crc */
        uint32_t cl = ((uint32_t)b[p] << 24) | ((uint32_t)b[p + 1] << 16) |
                      ((uint32_t)b[p + 2] << 8) | b[p + 3];
        if ((uint64_t)cl + 12 > n - p) return 0;
        if (first && memcmp(b + p + 4, "IHDR", 4) != 0) return 0;
        first = 0;
        if (memcmp(b + p + 4, "IEND", 4) == 0)
            return p + 12 + cl;
        p += 12 + cl;
    }
    return 0;
}

/* Scan an executable image for embedded media worth carving (>= 16 KiB).
 * Regions are returned in file order; a validated stream's bytes are never
 * rescanned (no false hits inside an accepted JPEG/PNG). A random FF D8 FF
 * in code dies in jpeg_scan_end's marker walk long before the cjxl guard
 * would ever see it. */
static size_t exe_scan_media(const uint8_t *b, size_t n,
                             exe_region *out, size_t cap)
{
    size_t p = 0, cnt = 0;

    while (p + EXE_MEDIA_MIN <= n && cnt < cap) {
        size_t end = 0;
        int kind = 0;
        if (b[p] == 0xFF && p + 3 <= n &&
            b[p + 1] == 0xD8 && b[p + 2] == 0xFF) {
            end = jpeg_scan_end(b, n, p);
            kind = 1;
        } else if (b[p] == 0x89 && p + 8 <= n &&
                   memcmp(b + p, "\x89PNG\r\n\x1a\n", 8) == 0) {
            end = png_scan_end(b, n, p);
            kind = 2;
        }
        if (end && end - p >= EXE_MEDIA_MIN) {
            out[cnt].off = p;
            out[cnt].len = end - p;
            out[cnt].kind = kind;
            cnt++;
            p = end;
        } else {
            p++;
        }
    }
    return cnt;
}

/* Parse + validate an EXER payload. Disk input, never trusted: magic, part
 * count (1..EXE_MAX_MEDIA), ascending non-overlapping ranges inside
 * [0, file_size), and the glue must account for every byte the parts do
 * not cover. codecid is INVFS_ALGO_JXL or INVFS_ALGO_ZSTD; anything else
 * fails loudly (a newer encoder wrote it). Returns 0 and fills rows[] or
 * -1 on any violation. */
static int exer_payload_parse(const uint8_t *pay, size_t pay_len,
                              uint64_t file_size,
                              exer_row *rows, size_t cap, size_t *n_out)
{
    uint32_t n, i;
    uint64_t prev_end = 0, member_sum = 0;
    size_t glue_len;

    if (pay_len < 8 || memcmp(pay, "EXER", 4) != 0) return -1;
    n = (uint32_t)pay[4] | ((uint32_t)pay[5] << 8) |
        ((uint32_t)pay[6] << 16) | ((uint32_t)pay[7] << 24);
    if (n == 0 || n > EXE_MAX_MEDIA || (size_t)n > cap) return -1;
    if ((uint64_t)8 + 17ull * n > pay_len) return -1;
    glue_len = pay_len - 8 - 17 * (size_t)n;
    for (i = 0; i < n; i++) {
        const uint8_t *r = pay + 8 + 17 * (size_t)i;
        rows[i].off = exer_rd64(r);
        rows[i].len = exer_rd64(r + 8);
        rows[i].codec = r[16];
        if (rows[i].codec != INVFS_ALGO_JXL &&
            rows[i].codec != INVFS_ALGO_ZSTD)
            return -1;
        if (rows[i].len == 0 || rows[i].len > file_size ||
            rows[i].off > file_size - rows[i].len)
            return -1;
        if (rows[i].off < prev_end) return -1;   /* ascending, no overlap */
        prev_end = rows[i].off + rows[i].len;
        member_sum += rows[i].len;
    }
    if (member_sum + glue_len != file_size) return -1;
    *n_out = n;
    return 0;
}

/* Splice an EXER payload's glue + the decoded part buffers into dst
 * (file_size bytes). Rows come pre-validated from exer_payload_parse, so
 * the glue arithmetic cannot overrun: parts are ascending, inside the
 * file, and member_sum + glue_len == file_size. */
static void exer_splice(const uint8_t *pay, const exer_row *rows, size_t n,
                        uint8_t *const *parts, uint8_t *dst, uint64_t file_size)
{
    size_t gp = 8 + 17 * n;   /* glue cursor: past header + table */
    uint64_t fp = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        memcpy(dst + fp, pay + gp, (size_t)(rows[i].off - fp));
        gp += (size_t)(rows[i].off - fp);
        memcpy(dst + rows[i].off, parts[i], (size_t)rows[i].len);
        fp = rows[i].off + rows[i].len;
    }
    memcpy(dst + fp, pay + gp, (size_t)(file_size - fp));
}

/* WP10 §5 / WP14a: is this AST entry a member slice of a shared batch?
 * zone=TEXT means "batched"; the payload codec is PPMD (text batches,
 * WP10) or ZSTD / ZSTD_BCJ (binary batches, WP14a). */
static int tz_batch_algo(uint32_t algo)
{
    return algo == INVFS_ALGO_PPMD || algo == INVFS_ALGO_ZSTD ||
           algo == INVFS_ALGO_ZSTD_BCJ;
}

/* WP10 §5 + WP14a: read a slice of a shared batch. A member AST entry
 * (zone=TEXT) names its batch by block_id: the member's L2P dup entry maps
 * it to the batch pba, and block_offset is the slice's offset in the
 * DECODED batch. The batch segment carries a [4B usize LE] sub-header in
 * front of the codec blob (PPMd: [2B props][stream]; binary: one zstd
 * frame), usize = decoded batch size, under the usual [4B csize]
 * [4B crc32c] framing. The decoded batch is cached in the ARC keyed by the
 * TAGGED pba (pba | TZ_ARC_TAG), not inode id: one batch is shared by many
 * member inodes, and the segment is freed only by GC (which invalidates the
 * tagged pba key first). Batches reach 4 MB, so this is heap-only -- never
 * the callers' stack segment buffer.
 *
 * WP14a BCJ: an algo==ZSTD_BCJ batch holds member slices that were each
 * x86-BCJ-prefiltered STANDALONE (pc=0, state=0) before concatenation. The
 * inverse is only bijective over exactly that member window, so a partial
 * read of such a slice first decodes the slice [block_offset, +length) as
 * a whole, inverts it, and only then serves the requested sub-window. The
 * cached batch stays in encoded form (the cache is shared with the other
 * members and must never be mutated). */
static int vol_read_text_slice(invfs_volume *v, uint64_t inode_id,
                               const invfs_ast_block_entry *e,
                               uint64_t slice_off, uint8_t *dst, size_t want)
{
    uint64_t pba = 0, plen = 0;
    const uint8_t *batch = NULL;
    uint8_t *fresh = NULL;
    size_t batch_len = 0;
    int rc = -1;

    if (want == 0) return 0;
    if (slice_off + want > e->length)
        return -1;   /* window runs past the member's slice */
    if (vol_lookup_entry(v, inode_id, e->block_id, &pba, &plen) != 0 ||
        pba == 0) {
        fprintf(stderr, "L2P miss: inode %llu text seg %u\n",
                (unsigned long long)inode_id, e->block_id);
        return -1;
    }
    if (!arc_get(v->arc, pba | TZ_ARC_TAG, &batch, &batch_len)) {
        uint8_t hdrb[8];
        uint32_t csize, crc_hdr, usize;
        uint8_t *blob;

        if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
            io_read(&v->io, hdrb, 8) != 0)
            return -1;
        memcpy(&csize, hdrb, 4);
        memcpy(&crc_hdr, hdrb + 4, 4);
        /* payload holds at least [4B usize][2B props] and fits the segment */
        if (csize < 4 + 2 ||
            (plen && (uint64_t)csize + 8 > plen * INVFS_BLOCK_SIZE))
            return -1;
        blob = (uint8_t *)malloc(csize);
        if (!blob) return -1;
        if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE + 8) != 0 ||
            io_read(&v->io, blob, csize) != 0) {
            free(blob); return -1;
        }
        /* deep protection: verify segment CRC32C before decode */
        if (crc_hdr != 0 && invfs_crc32c(blob, csize) != crc_hdr) {
            fprintf(stderr, "segment CRC mismatch: text batch pba %llu\n",
                    (unsigned long long)pba);
            free(blob); return -1;
        }
        memcpy(&usize, blob, 4);
        if (usize == 0 || usize > (64u << 20)) {   /* batches are <= 4 MB */
            free(blob); return -1;
        }
        fresh = (uint8_t *)malloc(usize);
        if (!fresh) { free(blob); return -1; }
        /* the decoder's wire blob starts past the usize LE */
        if (e->algo == INVFS_ALGO_PPMD) {
            if (invfs_ppmd_decode(blob + 4, csize - 4, fresh, usize) != 0) {
                fprintf(stderr, "ppmd decode failed: text batch pba %llu\n",
                        (unsigned long long)pba);
                free(fresh); free(blob); return -1;
            }
        } else if (e->algo == INVFS_ALGO_ZSTD ||
                   e->algo == INVFS_ALGO_ZSTD_BCJ) {
            /* WP14a: one zstd stream for the whole batch (decode-whole is
             * fine at GB/s); the BCJ tag only names the prefilter, which
             * is undone per member slice below, not here */
            const invfs_codec *zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
            if (!zc || !zc->decode ||
                zc->decode(blob + 4, csize - 4, fresh, usize) != 0) {
                fprintf(stderr, "zstd decode failed: binary batch pba %llu\n",
                        (unsigned long long)pba);
                free(fresh); free(blob); return -1;
            }
        } else {
            fprintf(stderr, "text slice: unknown batch algo %u\n",
                    (unsigned)e->algo);
            free(fresh); free(blob); return -1;
        }
        free(blob);
        batch = fresh;
        batch_len = usize;
    }
    if (e->algo == INVFS_ALGO_ZSTD_BCJ) {
        /* invert the per-member prefilter: the window is exactly the
         * member slice, decoded whole, at pc=0 -- the same window the
         * encoder ran over (see the flush feed loop) */
        if ((uint64_t)e->block_offset + e->length <= batch_len) {
            if (slice_off == 0 && want == (size_t)e->length) {
                /* the common case (whole-slice read): no extra copy */
                memcpy(dst, batch + e->block_offset, want);
                invfs_bcj_x86_dec(dst, want);
                rc = 0;
            } else {
                uint8_t *sl = (uint8_t *)malloc(e->length ? e->length : 1);
                if (sl) {
                    memcpy(sl, batch + e->block_offset, e->length);
                    invfs_bcj_x86_dec(sl, e->length);
                    memcpy(dst, sl + slice_off, want);
                    free(sl);
                    rc = 0;
                }
            }
        }
    } else if ((uint64_t)e->block_offset + slice_off + want <= batch_len) {
        memcpy(dst, batch + e->block_offset + slice_off, want);
        rc = 0;
    }
    /* arc_put takes ownership (or frees an oversized entry), so the slice
     * is copied out BEFORE the buffer is handed over. NULL cache frees. */
    if (fresh) arc_put(v->arc, pba | TZ_ARC_TAG, fresh, batch_len);
    return rc;
}

static int vol_read_inode(invfs_volume *v, uint64_t inode_id, unsigned depth,
                          uint8_t **out, size_t *out_len)
{
    uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    uint64_t end = v->inode_area_pos;
    uint8_t *data = NULL;
    size_t len = 0;

    /* The id index knows where this record is; without it every read of every
       file re-read the whole inode area, which is what kept reads quadratic
       after the name index landed. 0 means "not indexed" -- fall back to the
       scan below, which is still the authority. */
    {
        uint64_t ip = idx_get_id(v, inode_id);
        if (ip >= pos && ip + sizeof(invfs_inode_rec) <= end) pos = ip;
    }

    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec rec_h;
        uint32_t crc_stored, crc_calc;
        uint8_t *rec;

        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &rec_h, sizeof(rec_h)) != 0)
            return -1;
        if (rec_h.magic != INODE_REC_MAGIC && rec_h.magic != TOMBSTONE_MAGIC)
            return -1;
        if (rec_h.magic == TOMBSTONE_MAGIC) { pos += rec_h.rec_len + 4; continue; }
        if (rec_h.inode_id != inode_id) { pos += rec_h.rec_len + 4; continue; }

        /* read full record + crc, verify */
        rec = (uint8_t *)malloc(rec_h.rec_len);
        if (!rec) return -1;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, rec, rec_h.rec_len) != 0 ||
            io_read(&v->io, &crc_stored, 4) != 0) {
            free(rec);
            return -1;
        }
        crc_calc = invfs_crc32c(rec, rec_h.rec_len);
        if (crc_calc != crc_stored) {
            fprintf(stderr, "inode record CRC mismatch (inode %llu)\n",
                    (unsigned long long)inode_id);
            free(rec);
            return -1;
        }

        /* parse AST: header + entries after rec header */
        {
            invfs_ast_recipe_header ast_h;
            const invfs_ast_block_entry *ents;
            uint16_t i;
            size_t off = sizeof(invfs_inode_rec);
            memcpy(&ast_h, rec + off, sizeof(ast_h));
            off += sizeof(ast_h);
            ents = (const invfs_ast_block_entry *)(rec + off);

            len = ast_h.file_size;
            data = (uint8_t *)malloc(len ? len : 1);
            if (!data) { free(rec); return -1; }

            for (i = 0; i < ast_h.num_blocks; i++) {
                const invfs_ast_block_entry *e = &ents[i];
                uint64_t pba = 0, phys_len = 0;
                uint32_t hdr, crc_hdr;
                uint8_t *blob;
                size_t dst_off = (size_t)e->file_offset;

                /* Batch member: a slice of a shared batch (PPMd text,
                 * ZSTD/ZSTD_BCJ binary), decoded and cached by pba (heap
                 * only -- see vol_read_text_slice) */
                if (e->zone == INVFS_ZONE_TEXT && tz_batch_algo(e->algo)) {
                    if (vol_read_text_slice(v, inode_id, e, 0,
                                            data + dst_off,
                                            (size_t)e->length) != 0) {
                        free(data); free(rec); return -1;
                    }
                    continue;
                }

                /* segment physical location via L2P */
                if (vol_lookup_entry(v, inode_id, e->block_id, &pba, &phys_len) != 0) {
                    fprintf(stderr, "L2P miss: inode %llu seg %u\n",
                            (unsigned long long)inode_id, e->block_id);
                    free(data); free(rec); return -1;
                }
                /* read 8-byte header: [4B csize][4B crc32c(data)] */
                {
                    uint8_t hdrb[8];
                    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
                        io_read(&v->io, hdrb, 8) != 0) {
                        free(data); free(rec); return -1;
                    }
                    memcpy(&hdr, hdrb, 4);
                    memcpy(&crc_hdr, hdrb + 4, 4);
                }
                if (e->algo == INVFS_ALGO_NONE) {
                    if (hdr != e->length) {
                        fprintf(stderr, "raw segment header corrupt (csize %u, want %llu)\n",
                                hdr, (unsigned long long)e->length);
                        free(data); free(rec); return -1;
                    }
                } else if (e->algo == INVFS_ALGO_LZ4 || e->algo == INVFS_ALGO_ZSTD) {
                    /* compressed-in-place segments: csize < usize is required */
                    if (hdr >= e->length || hdr == 0) {
                        fprintf(stderr, "lz4 segment header corrupt (csize %u)\n", hdr);
                        free(data); free(rec); return -1;
                    }
                }
                /* whole-file blobs (JXL/APE/FLACR) keep csize unrelated to the
                   logical size: the transcoder may be smaller OR larger */
                blob = (uint8_t *)malloc(hdr);
                if (!blob) { free(data); free(rec); return -1; }
                if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE + 8) != 0 ||
                    io_read(&v->io, blob, hdr) != 0) {
                    free(blob); free(data); free(rec); return -1;
                }
                /* deep protection: verify segment CRC32C before decode */
                if (crc_hdr != 0 && invfs_crc32c(blob, hdr) != crc_hdr) {
                    fprintf(stderr, "segment CRC mismatch: inode %llu seg %u (corrupt)\n",
                            (unsigned long long)inode_id, e->block_id);
                    free(blob); free(data); free(rec); return -1;
                }

                if (e->algo == INVFS_ALGO_LZ4) {
                    int got = LZ4_decompress_safe((const char *)blob,
                                                  (char *)(data + dst_off),
                                                  (int)hdr, (int)e->length);
                    if (got != (int)e->length) {
                        fprintf(stderr, "LZ4 decompress error: got %d, want %llu\n",
                                got, (unsigned long long)e->length);
                        free(blob); free(data); free(rec); return -1;
                    }
                } else if (e->algo == INVFS_ALGO_ZSTD) {
                    size_t got = ZSTD_decompress(data + dst_off, e->length, blob, hdr);
                    if (ZSTD_isError(got) || got != e->length) {
                        fprintf(stderr, "ZSTD decompress error: %s\n",
                                ZSTD_isError(got) ? ZSTD_getErrorName(got) : "size mismatch");
                        free(blob); free(data); free(rec); return -1;
                    }
                } else if (e->algo == INVFS_ALGO_JXL) {
                    /* whole file is one JXL blob -> decode to jpeg */
                    uint8_t *jpg = NULL;
                    size_t jpg_len = 0;
                    if (invfs_jxl_decompress(blob, hdr, &jpg, &jpg_len) != 0 ||
                        jpg_len != e->length) {
                        fprintf(stderr, "JXL decompress error\n");
                        free(blob); free(data); free(rec); return -1;
                    }
                    memcpy(data + dst_off, jpg, jpg_len);
                    free(jpg);
                } else if (e->algo == INVFS_ALGO_APE) {
                    /* whole file is one APE blob -> decode to flac */
                    uint8_t *fl = NULL;
                    size_t fl_len = 0;
                    if (invfs_ape_decompress(blob, hdr, &fl, &fl_len) != 0) {
                        fprintf(stderr, "APE decompress error\n");
                        free(blob); free(data); free(rec); return -1;
                    }
                    memcpy(data + dst_off, fl, fl_len < e->length ? fl_len : e->length);
                    free(fl);
                } else if (e->algo == INVFS_ALGO_PMP) {
                    /* whole file is one PMP blob -> decode back to mp3.
                       Length is checked, not clamped: packMP3 is bit-exact
                       or it is nothing, so a short/long result means the
                       blob is corrupt and returning partial audio would
                       break the 1:1 invariant silently. */
                    uint8_t *m = NULL;
                    size_t m_len = 0;
                    if (invfs_pmp_decompress(blob, hdr, &m, &m_len) != 0 ||
                        m_len != e->length) {
                        fprintf(stderr, "PMP decompress error (%s: got %zu, want %llu)\n",
                                rec_h.name, m_len, (unsigned long long)e->length);
                        free(m); free(blob); free(data); free(rec); return -1;
                    }
                    memcpy(data + dst_off, m, m_len);
                    free(m);
                } else if (e->algo == INVFS_ALGO_FLACR) {
                    /* whole file is an APE(PCM) blob; sibling inode
                       "name!recipe" holds the frame recipe. Rebuild the
                       ORIGINAL FLAC bit-exactly: APE->WAV->flacx_rebuild. */
                    uint8_t *wav = NULL, *rcp = NULL, *fl = NULL;
                    size_t wav_len = 0, rcp_len = 0, fl_len = 0;
                    char rname[272];
                    if (invfs_ape_to_wav(blob, hdr, &wav, &wav_len) != 0) {
                        fprintf(stderr, "FLACR: APE->WAV failed for %s\n", rec_h.name);
                        free(blob); free(data); free(rec); return -1;
                    }
                    snprintf(rname, sizeof rname, "%s!recipe", rec_h.name);
                    uint64_t rino = vol_find(v, rname);
                    if (rino == 0) {
                        fprintf(stderr, "FLACR: recipe inode '%s' not found\n", rname);
                        free(wav); free(blob); free(data); free(rec); return -1;
                    }
                    if (vol_read_inode(v, rino, 0, &rcp, &rcp_len) != 0) {
                        fprintf(stderr, "FLACR: cannot read recipe '%s'\n", rname);
                        free(wav); free(blob); free(data); free(rec); return -1;
                    }
                    /* cover payloads: "name!coverN" (v2 recipes) */
                    int ncv = flacx_recipe_num_covers(rcp, rcp_len);
                    flacx_cover covers[16];
                    uint8_t *cdata[16];
                    int ok = 1;
                    for (int ci = 0; ci < ncv && ci < 16; ci++) {
                        char cn[288];
                        snprintf(cn, sizeof cn, "%s!cover%d", rec_h.name, ci);
                        uint64_t cino = vol_find(v, cn);
                        size_t clen = 0;
                        cdata[ci] = NULL;
                        if (cino == 0 ||
                            vol_read_inode(v, cino, 0, &cdata[ci], &clen) != 0 ||
                            clen > 0x7FFFFFFF) {
                            fprintf(stderr, "FLACR: cover '%s' missing\n", cn);
                            ok = 0;
                            break;
                        }
                        covers[ci].data = cdata[ci];
                        covers[ci].len = (uint32_t)clen;
                        covers[ci].offset = 0;
                    }
                    if (ok && flacx_rebuild(wav, wav_len, rcp, rcp_len, covers,
                                            (uint32_t)ncv, &fl, &fl_len) != 0)
                        ok = 0;
                    for (int ci = 0; ci < ncv && ci < 16; ci++) free(cdata[ci]);
                    if (!ok || fl_len != e->length) {
                        fprintf(stderr, "FLACR: rebuild error (got %zu want %llu)\n",
                                fl_len, (unsigned long long)e->length);
                        free(wav); free(rcp); free(blob); free(data); free(rec); return -1;
                    }
                    memcpy(data + dst_off, fl, fl_len);
                    free(fl); free(wav); free(rcp);
                } else if (e->algo == INVFS_ALGO_TARR) {
                    /* blob = recipe: [0x01][zstd...] or [0x00][raw IVFT] */
                    const uint8_t *rcp; size_t rcp_len;
                    uint8_t *rcp_own = NULL;
                    if (hdr > 1 && blob[0] == 1) {
                        size_t rsize = ZSTD_getFrameContentSize(blob + 1, hdr - 1);
                        if (rsize == ZSTD_CONTENTSIZE_ERROR ||
                            rsize == ZSTD_CONTENTSIZE_UNKNOWN || rsize > (1u << 28)) {
                            fprintf(stderr, "TARR: bad recipe frame\n");
                            free(blob); free(data); free(rec); return -1;
                        }
                        rcp_own = (uint8_t *)malloc(rsize ? rsize : 1);
                        if (!rcp_own) { free(blob); free(data); free(rec); return -1; }
                        size_t rr = ZSTD_decompress(rcp_own, rsize, blob + 1, hdr - 1);
                        if (ZSTD_isError(rr)) {
                            fprintf(stderr, "TARR: recipe decompress fail\n");
                            free(rcp_own); free(blob); free(data); free(rec); return -1;
                        }
                        rcp = rcp_own; rcp_len = rr;
                    } else {
                        rcp = blob + 1; rcp_len = hdr - 1;
                    }
                    tarx_member *members = NULL; size_t n = 0;
                    uint8_t *trailer = NULL; size_t tlen = 0;
                    if (tarx_parse_recipe(rcp, rcp_len, &members, &n, &trailer, &tlen) != 0) {
                        fprintf(stderr, "TARR: bad recipe for %s\n", rec_h.name);
                        free(rcp_own); free(blob); free(data); free(rec); return -1;
                    }
                    int np = tarx_recipe_num_parts(rcp, rcp_len);
                    uint8_t **parts = (uint8_t **)calloc(np > 0 ? (size_t)np : 1, sizeof(void *));
                    size_t *plens = (size_t *)calloc(np > 0 ? (size_t)np : 1, sizeof(size_t));
                    int ok = 1;
                    for (int pi = 0; pi < np; pi++) {
                        char pn[320];
                        snprintf(pn, sizeof pn, "%s!part%u", rec_h.name, pi);
                        uint64_t pino = vol_find(v, pn);
                        if (!pino) {
                            fprintf(stderr, "TARR: part '%s' missing\n", pn);
                            ok = 0; break;
                        }
                        if (vol_read_inode(v, pino, 0, &parts[pi], &plens[pi]) != 0) { ok = 0; break; }
                    }
                    uint8_t *fl = NULL; size_t fl_len = 0;
                    if (ok && tarx_rebuild(members, n, trailer, tlen,
                                           (const uint8_t *const *)parts, plens,
                                           &fl, &fl_len) != 0)
                        ok = 0;
                    for (int pi = 0; pi < np; pi++) free(parts[pi]);
                    free(parts); free(plens); free(members); free(trailer);
                    if (!ok || fl_len != e->length) {
                        fprintf(stderr, "TARR: rebuild error (got %zu want %llu)\n",
                                fl_len, (unsigned long long)e->length);
                        free(rcp_own); free(blob); free(data); free(rec); free(fl); return -1;
                    }
                    memcpy(data + dst_off, fl, fl_len);
                    free(fl); free(rcp_own);
                } else if (e->algo == INVFS_ALGO_GZR) {
                    /* blob = recipe: [0x01][zstd] or [0x00][raw]; recipe =
                       [IVGZ][ver][level][mem][crc32][isize][hlen(2)][header] + IVFT */
                    const uint8_t *rcp; size_t rcp_len;
                    uint8_t *rcp_own = NULL;
                    if (hdr > 1 && blob[0] == 1) {
                        size_t rsize = ZSTD_getFrameContentSize(blob + 1, hdr - 1);
                        if (rsize == ZSTD_CONTENTSIZE_ERROR ||
                            rsize == ZSTD_CONTENTSIZE_UNKNOWN || rsize > (1u << 28)) {
                            fprintf(stderr, "GZR: bad recipe frame\n");
                            free(blob); free(data); free(rec); return -1;
                        }
                        rcp_own = (uint8_t *)malloc(rsize ? rsize : 1);
                        if (!rcp_own) { free(blob); free(data); free(rec); return -1; }
                        size_t rr = ZSTD_decompress(rcp_own, rsize, blob + 1, hdr - 1);
                        if (ZSTD_isError(rr)) {
                            fprintf(stderr, "GZR: recipe decompress fail\n");
                            free(rcp_own); free(blob); free(data); free(rec); return -1;
                        }
                        rcp = rcp_own; rcp_len = rr;
                    } else {
                        rcp = blob + 1; rcp_len = hdr - 1;
                    }
                    if (rcp_len < 20 || memcmp(rcp, "IVGZ", 4) != 0 || rcp[4] != 1) {
                        fprintf(stderr, "GZR: bad recipe for %s\n", rec_h.name);
                        free(rcp_own); free(blob); free(data); free(rec); return -1;
                    }
                    int glevel = rcp[5];
                    int gmem = rcp[6];
                    unsigned gcrc = (unsigned)rcp[7] | ((unsigned)rcp[8] << 8) |
                                    ((unsigned)rcp[9] << 16) | ((unsigned)rcp[10] << 24);
                    unsigned gisz = (unsigned)rcp[11] | ((unsigned)rcp[12] << 8) |
                                     ((unsigned)rcp[13] << 16) | ((unsigned)rcp[14] << 24);
                    unsigned ghl = (unsigned)rcp[15] | ((unsigned)rcp[16] << 8);
                    if (17 + ghl + 17 > rcp_len) {
                        fprintf(stderr, "GZR: short recipe\n");
                        free(rcp_own); free(blob); free(data); free(rec); return -1;
                    }
                    const uint8_t *ghdr = rcp + 17;
                    const uint8_t *ivft = rcp + 17 + ghl;
                    size_t ivft_len = rcp_len - 17 - ghl;
                    tarx_member *members = NULL; size_t n = 0;
                    uint8_t *trailer = NULL; size_t tlen = 0;
                    if (tarx_parse_recipe(ivft, ivft_len, &members, &n, &trailer, &tlen) != 0) {
                        fprintf(stderr, "GZR: bad IVFT for %s\n", rec_h.name);
                        free(rcp_own); free(blob); free(data); free(rec); return -1;
                    }
                    int np = tarx_recipe_num_parts(ivft, ivft_len);
                    uint8_t **parts = (uint8_t **)calloc(np > 0 ? (size_t)np : 1, sizeof(void *));
                    size_t *plens = (size_t *)calloc(np > 0 ? (size_t)np : 1, sizeof(size_t));
                    int ok = 1;
                    for (int pi = 0; pi < np; pi++) {
                        char pn[320];
                        snprintf(pn, sizeof pn, "%s!part%u", rec_h.name, pi);
                        uint64_t pino = vol_find(v, pn);
                        if (!pino) {
                            fprintf(stderr, "GZR: part '%s' missing\n", pn);
                            ok = 0; break;
                        }
                        if (vol_read_inode(v, pino, 0, &parts[pi], &plens[pi]) != 0) { ok = 0; break; }
                    }
                    uint8_t *tar = NULL; size_t tar_len = 0;
                    if (ok && tarx_rebuild(members, n, trailer, tlen,
                                           (const uint8_t *const *)parts, plens,
                                           &tar, &tar_len) != 0)
                        ok = 0;
                    for (int pi = 0; pi < np; pi++) free(parts[pi]);
                    free(parts); free(plens); free(members); free(trailer);
                    uint8_t *fl = NULL; size_t fl_len = 0;
                    if (ok) {
                        /* reproduce deflate stream bit-exactly, wrap gzip */
                        z_stream s;
                        memset(&s, 0, sizeof s);
                        if (deflateInit2(&s, glevel, Z_DEFLATED, -15, gmem,
                                         Z_DEFAULT_STRATEGY) == Z_OK) {
                            size_t bound = deflateBound(&s, (uLong)tar_len);
                            uint8_t *stream = (uint8_t *)malloc(bound);
                            s.next_in = tar;
                            s.avail_in = (uInt)(tar_len > 0x7FFFFFFF ? 0x7FFFFFFF : tar_len);
                            s.next_out = stream;
                            s.avail_out = (uInt)bound;
                            int r2 = deflate(&s, Z_FINISH);
                            size_t stream_len = (size_t)s.total_out;
                            deflateEnd(&s);
                            if (r2 == Z_STREAM_END) {
                                fl = (uint8_t *)malloc(ghl + stream_len + 8);
                                if (fl) {
                                    memcpy(fl, ghdr, ghl);
                                    memcpy(fl + ghl, stream, stream_len);
                                    uLong c = crc32(0L, Z_NULL, 0);
                                    c = crc32(c, tar, (uInt)(tar_len > 0x7FFFFFFF ? 0x7FFFFFFF : tar_len));
                                    fl[ghl + stream_len + 0] = (uint8_t)(c & 0xFF);
                                    fl[ghl + stream_len + 1] = (uint8_t)((c >> 8) & 0xFF);
                                    fl[ghl + stream_len + 2] = (uint8_t)((c >> 16) & 0xFF);
                                    fl[ghl + stream_len + 3] = (uint8_t)((c >> 24) & 0xFF);
                                    fl[ghl + stream_len + 4] = (uint8_t)(gisz & 0xFF);
                                    fl[ghl + stream_len + 5] = (uint8_t)((gisz >> 8) & 0xFF);
                                    fl[ghl + stream_len + 6] = (uint8_t)((gisz >> 16) & 0xFF);
                                    fl[ghl + stream_len + 7] = (uint8_t)((gisz >> 24) & 0xFF);
                                    fl_len = ghl + stream_len + 8;
                                    if ((unsigned)(c & 0xFFFFFFFFu) != gcrc) {
                                        fprintf(stderr, "GZR: crc mismatch for %s\n", rec_h.name);
                                        ok = 0;
                                    }
                                } else ok = 0;
                            } else ok = 0;
                            free(stream);
                        } else ok = 0;
                    }
                    free(tar);
                    if (!ok || fl_len != e->length) {
                        fprintf(stderr, "GZR: rebuild error (got %zu want %llu)\n",
                                fl_len, (unsigned long long)e->length);
                        free(fl); free(rcp_own); free(blob); free(data); free(rec); return -1;
                    }
                    memcpy(data + dst_off, fl, fl_len);
                    free(fl); free(rcp_own);
                } else if (e->algo == INVFS_ALGO_PNGR) {
                    /* blob = recipe [0x01][zstd] or [0x00][raw IVPN];
                       pixels from sibling "name!jxl" (djxl -> PNG). */
                    const uint8_t *rcp; size_t rcp_len;
                    uint8_t *rcp_own = NULL;
                    if (hdr > 1 && blob[0] == 1) {
                        size_t rsize = ZSTD_getFrameContentSize(blob + 1, hdr - 1);
                        if (rsize == ZSTD_CONTENTSIZE_ERROR ||
                            rsize == ZSTD_CONTENTSIZE_UNKNOWN || rsize > (1u << 28)) {
                            fprintf(stderr, "PNGR: bad recipe frame\n");
                            free(blob); free(data); free(rec); return -1;
                        }
                        rcp_own = (uint8_t *)malloc(rsize ? rsize : 1);
                        if (!rcp_own) { free(blob); free(data); free(rec); return -1; }
                        size_t rr = ZSTD_decompress(rcp_own, rsize, blob + 1, hdr - 1);
                        if (ZSTD_isError(rr)) {
                            fprintf(stderr, "PNGR: recipe decompress fail\n");
                            free(rcp_own); free(blob); free(data); free(rec); return -1;
                        }
                        rcp = rcp_own; rcp_len = rr;
                    } else {
                        rcp = blob + 1; rcp_len = hdr - 1;
                    }
                    pngx_info pi;
                    memset(&pi, 0, sizeof pi);
                    if (pngx_parse_recipe(rcp, rcp_len, &pi) != 0) {
                        fprintf(stderr, "PNGR: bad recipe for %s (len %zu)\n",
                                rec_h.name, rcp_len);
                        fprintf(stderr, "PNGR: recipe head: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                rcp_len > 0 ? rcp[0] : 0, rcp_len > 1 ? rcp[1] : 0,
                                rcp_len > 2 ? rcp[2] : 0, rcp_len > 3 ? rcp[3] : 0,
                                rcp_len > 4 ? rcp[4] : 0, rcp_len > 5 ? rcp[5] : 0,
                                rcp_len > 6 ? rcp[6] : 0, rcp_len > 7 ? rcp[7] : 0,
                                rcp_len > 8 ? rcp[8] : 0, rcp_len > 9 ? rcp[9] : 0);
                        free(rcp_own); free(blob); free(data); free(rec); return -1;
                    }
                    int ok = 1;
                    uint8_t *rgb = NULL; size_t rgb_len = 0;
                    {
                        char jn[320];
                        snprintf(jn, sizeof jn, "%s!jxl", rec_h.name);
                        uint64_t jino = vol_find(v, jn);
                        if (!jino) {
                            fprintf(stderr, "PNGR: jxl '%s' missing\n", jn);
                            ok = 0;
                        } else if (invfs_png_from_jxl(v, jino, &rgb, &rgb_len) != 0) {
                            fprintf(stderr, "PNGR: djxl failed for %s\n", rec_h.name);
                            ok = 0;
                        }
                    }
                    uint8_t *filt = NULL; size_t filt_len = 0;
                    uint8_t *stream = NULL; size_t stream_len = 0;
                    uint8_t *fl = NULL; size_t fl_len = 0;
                    if (ok && pngx_refilter(rgb, rgb_len, &pi, &filt, &filt_len) != 0)
                        ok = 0;
                    if (ok) {
                        /* deflate replica (zlib or miniz) */
                        if (pi.enc == 0) {
                            z_stream s;
                            memset(&s, 0, sizeof s);
                            if (deflateInit2(&s, pi.level, Z_DEFLATED, 15, pi.mem,
                                             Z_DEFAULT_STRATEGY) == Z_OK) {
                                size_t bound = deflateBound(&s, (uLong)filt_len);
                                stream = (uint8_t *)malloc(bound);
                                s.next_in = filt;
                                s.avail_in = (uInt)(filt_len > 0x7FFFFFFF ? 0x7FFFFFFF : filt_len);
                                s.next_out = stream;
                                s.avail_out = (uInt)bound;
                                int r2 = deflate(&s, Z_FINISH);
                                stream_len = (size_t)s.total_out;
                                deflateEnd(&s);
                                if (r2 != Z_STREAM_END) ok = 0;
                            } else ok = 0;
                        } else {
                            size_t bound = filt_len + filt_len / 4 + 4096;
                            stream = (uint8_t *)malloc(bound);
                            if (mz_tdefl_compress(filt, filt_len, stream, bound,
                                                  pi.level, &stream_len) != 0)
                                ok = 0;
                        }
                    }
                    if (ok && pngx_rebuild(&pi, stream, stream_len, &fl, &fl_len) != 0)
                        ok = 0;
                    free(rgb); free(filt); free(stream);
                    if (!ok || fl_len != e->length) {
                        fprintf(stderr, "PNGR: rebuild error (got %zu want %llu)\n",
                                fl_len, (unsigned long long)e->length);
                        pngx_free(&pi); free(fl); free(rcp_own); free(blob); free(data); free(rec); return -1;
                    }
                    memcpy(data + dst_off, fl, fl_len);
                    pngx_free(&pi); free(fl); free(rcp_own);
                } else if (e->algo == INVFS_ALGO_EXER) {
                    /* WP14b M2: exe-as-container. The blob is ZSTD-19 of
                     * the EXER payload (header + part table + glue); each
                     * member's bytes live in a "name!exrN" sibling read
                     * through the normal path (a JXL sibling decodes to the
                     * member, a ZSTD one just inflates). Splice the members
                     * back over the glue at the table offsets. The payload
                     * is disk input: exer_payload_parse validates every
                     * bound before anything is copied. */
                    uint8_t *pay = NULL;
                    exer_row *rows = NULL;
                    uint8_t *parts[EXE_MAX_MEDIA];
                    size_t n = 0, pi;
                    int ok = 0;
                    size_t dsize = ZSTD_getFrameContentSize(blob, hdr);

                    memset(parts, 0, sizeof parts);
                    if (dsize != ZSTD_CONTENTSIZE_ERROR &&
                        dsize != ZSTD_CONTENTSIZE_UNKNOWN &&
                        dsize >= 8 + 17 &&
                        dsize <= (uint64_t)e->length + 8 + 17 * EXE_MAX_MEDIA)
                        pay = (uint8_t *)malloc(dsize);
                    rows = (exer_row *)malloc(EXE_MAX_MEDIA * sizeof *rows);
                    if (pay && rows) {
                        size_t d = ZSTD_decompress(pay, dsize, blob, hdr);
                        if (!ZSTD_isError(d) && d == dsize &&
                            exer_payload_parse(pay, dsize, e->length,
                                               rows, EXE_MAX_MEDIA, &n) == 0)
                            ok = 1;
                    }
                    for (pi = 0; ok && pi < n; pi++) {
                        char pn[288];
                        uint64_t pino;
                        size_t plen = 0;
                        snprintf(pn, sizeof pn, "%s!exr%zu", rec_h.name, pi);
                        pino = vol_find(v, pn);
                        if (!pino ||
                            vol_read_file(v, pino, &parts[pi], &plen) != 0 ||
                            plen != (size_t)rows[pi].len) {
                            fprintf(stderr, "EXER: part '%s' unreadable\n", pn);
                            ok = 0;
                            break;
                        }
                    }
                    if (ok)
                        exer_splice(pay, rows, n, parts, data, e->length);
                    for (pi = 0; pi < n; pi++) free(parts[pi]);
                    free(pay);
                    free(rows);
                    if (!ok) {
                        fprintf(stderr, "EXER: rebuild failed for %s\n",
                                rec_h.name);
                        free(blob); free(data); free(rec); return -1;
                    }
                } else if (e->algo == INVFS_ALGO_NONE) {
                    memcpy(data + dst_off, blob, hdr);
                } else {
                    /* WP13: a codecpack whole-file blob decodes through the
                     * pack trampoline. An algo this build cannot decode
                     * (pack not loaded) fails LOUDLY -- the historical raw
                     * copy would serve the blob as if it were the file,
                     * silently breaking the 1:1 invariant. */
                    const invfs_codec *pc = invfs_codec_by_algo(e->algo);
                    if (!pc || !pc->decode) {
                        fprintf(stderr, "inode %llu: algo %u requires a "
                                "codecpack that is not loaded\n",
                                (unsigned long long)inode_id, e->algo);
                        free(blob); free(data); free(rec); return -1;
                    }
                    if (pc->decode(blob, hdr, data + dst_off,
                                   (size_t)e->length) != 0) {
                        fprintf(stderr, "%s: pack decode error\n", pc->name);
                        free(blob); free(data); free(rec); return -1;
                    }
                }
                free(blob);
            }
        }
        free(rec);
        *out = data;
        *out_len = len;
        return 0;
    }
    return -1;  /* not found */
}

/* public entry: read file, reconstructing containers from children */
int vol_read_file(invfs_volume *v, uint64_t inode_id, uint8_t **out, size_t *out_len)
{
    return vol_read_inode(v, inode_id, 0, out, out_len);
}

const invfs_superblock *vol_sb(invfs_volume *v)
{
    return &v->sb;
}

const uint8_t *vol_bitmap(invfs_volume *v, uint64_t *blocks_out)
{
    if (blocks_out) *blocks_out = v->sb.total_blocks;
    return v->bitmap;
}

const invfs_l2p_entry *vol_l2p(invfs_volume *v, size_t *count_out)
{
    if (count_out) *count_out = v->l2p_count;
    return v->l2p;
}

uint64_t vol_inode_area_pos(invfs_volume *v) { return v->inode_area_pos; }
uint64_t vol_inode_area_start(invfs_volume *v) { return v->inode_area_start * INVFS_BLOCK_SIZE; }
uint64_t vol_inode_area_end(invfs_volume *v) { return v->inode_area_end; }
uint64_t vol_journal_pos(invfs_volume *v)   { return v->journal_pos; }

/* Bytes left for new inode records. The area is append-only, so this is what
   stands between the volume and "create silently returns 0": callers that can
   still report an error to the application should check it before accepting
   data, not after. A record is sizeof(invfs_inode_rec) + the AST recipe, so
   this is an upper bound on what will fit, not a file count. */
uint64_t vol_inode_area_free(invfs_volume *v)
{
    return v->inode_area_pos < v->inode_area_end
         ? v->inode_area_end - v->inode_area_pos : 0;
}

uint64_t vol_inode_next(invfs_volume *v, uint64_t pos, uint32_t *magic_out,
                        uint64_t *inode_out, uint64_t *size_out,
                        char *name_out, size_t name_cap, uint32_t *rec_len_out)
{
    invfs_inode_rec rec_h;
    /* scan only the CRC-validated extent (vol_open truncated at the first
     * torn/corrupt record); never step into garbage past area_pos */
    uint64_t end = v->inode_area_pos;
    while (pos + sizeof(rec_h) <= end) {
        if (vol_read_raw(v, pos, &rec_h, sizeof(rec_h)) != 0)
            break;
        if (rec_h.magic != INODE_REC_MAGIC && rec_h.magic != TOMBSTONE_MAGIC)
            break;  /* end of valid records */
        if (rec_h.rec_len < sizeof(rec_h) || rec_h.rec_len > INVFS_MAX_REC_LEN)
            break;
        if (magic_out)  *magic_out = rec_h.magic;
        if (inode_out)  *inode_out = rec_h.inode_id;
        if (size_out)   *size_out = rec_h.file_size;
        if (rec_len_out) *rec_len_out = rec_h.rec_len;
        if (name_out && name_cap) {
            /* name_len comes off disk, so clamp against the field it indexes
               as well as the caller's buffer: name is the last member of the
               record header, and a caller with a buffer bigger than the field
               would otherwise let a corrupt length read past it. */
            size_t n = rec_h.name_len;
            if (n > INVFS_MAX_NAME) n = INVFS_MAX_NAME;
            if (n > name_cap - 1) n = name_cap - 1;
            memcpy(name_out, rec_h.name, n);  /* name lives inside rec */
            name_out[n] = 0;
        }
        return pos + rec_h.rec_len + 4;  /* +4: trailing CRC32C */
    }
    return 0;
}

/* raw byte-range read at absolute volume offset (for tools) */
int vol_read_raw(invfs_volume *v, uint64_t offset, void *buf, size_t len)
{
    if (io_seek(&v->io, offset) != 0 || io_read(&v->io, buf, len) != 0)
        return -1;
    return 0;
}

/* free n physical blocks (clear bitmap bits), bounded by volume size */
/* ENOSPC policy helpers */
int vol_write_enabled(invfs_volume *v)
{
    /* A volume awaiting recovery is read-only for the same reason a
       READONLY-flagged one is: the callers that check this are the ones that
       would otherwise append records, and appending onto maps that were never
       finished is how a single crash becomes two. */
    if (v->needs_recovery) return 0;
    return !(v->sb.vol_flags & VOLF_READONLY);
}

/* flip the READONLY flag; persists on next vol_flush (caller flushes) */
void vol_set_readonly(invfs_volume *v, int ro)
{
    if (ro)
        v->sb.vol_flags |= VOLF_READONLY;
    else
        v->sb.vol_flags &= ~VOLF_READONLY;
}
uint64_t vol_free_blocks_cached(invfs_volume *v)
{
    return v->free_blocks;
}

/* blocks that ordinary writes must leave untouched (reserve + floor) */
uint64_t vol_write_guard(invfs_volume *v)
{
    return (uint64_t)v->sb.reserved_blocks + v->sb.hard_min_blocks;
}

void vol_free_blocks(invfs_volume *v, uint64_t pba, uint64_t nblocks)
{
    uint64_t i;
    uint64_t end = pba + nblocks;
    if (end > v->sb.total_blocks)
        end = v->sb.total_blocks;
    for (i = pba; i < end; i++)
        bit_clr(v->bitmap, i);
    if (end > pba) { bm_dirty(v, pba); bm_dirty(v, end - 1); }
    v->free_blocks += nblocks;

    /* Give the freed space back to the zone it came from, and drop the
     * "no run this long exists" hint: a fresh hole may satisfy a request
     * that the last full scan rejected. Rewind the cursor so the scan
     * actually reaches the hole instead of walking past it. */
    if (pba >= v->sb.shadow_zone_start) {
        v->shadow_free += nblocks;
        v->shadow_fail_run = 0;
        if (pba < v->shadow_cursor) v->shadow_cursor = pba;
    } else if (pba >= v->sb.raw_zone_start) {
        v->raw_free += nblocks;
        v->raw_fail_run = 0;
        if (pba < v->raw_cursor) v->raw_cursor = pba;
    }
}

/* Give back everything the atomic sweep path allocated under `new_id` before
   it gave up. Safe to call with new_id == 0 (the in-place path) or with no
   maps yet. Nothing has been made durable at this point -- no record names
   new_id, no flush has happened -- so this is pure in-memory bookkeeping plus
   bitmap bits, and the file is left exactly as it was found. */
static void sweep_unwind(invfs_volume *v, uint64_t new_id)
{
    size_t i, w = 0;
    if (!new_id) return;
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->inode == new_id) {
            uint64_t nblk = e->length;
            if (nblk && e->pba < v->sb.total_blocks &&
                nblk <= v->sb.total_blocks - e->pba)
                vol_free_blocks(v, e->pba, nblk);
        }
    }
    for (i = 0; i < v->l2p_count; i++) {
        if (v->l2p[i].inode == new_id) {
            if (w < v->l2p_dirty) v->l2p_dirty = w;
            continue;
        }
        if (w != i) v->l2p[w] = v->l2p[i];
        w++;
    }
    v->l2p_count = w;
    if (v->l2p_dirty > v->l2p_count) v->l2p_dirty = v->l2p_count;
}

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
static int vol_sweep_file_inner(invfs_volume *v, uint64_t inode_id,
                                int generic_only);

/* WP10 §2: the minimum compression gain (in percent) below which a file is
 * stamped UNCOMPRESSIBLE and skipped by later sweeps until a newer codec
 * generation sniffs it. INVFS_MIN_GAIN_PCT, default 0.5. */
static double vol_min_gain_pct(void)
{
    const char *e = getenv("INVFS_MIN_GAIN_PCT");
    if (!e || !*e) return 0.5;
    char *endp = NULL;
    double pct = strtod(e, &endp);
    if (endp == e || pct < 0.0 || pct >= 100.0) {
        fprintf(stderr, "[vol] INVFS_MIN_GAIN_PCT=\"%s\" invalid; using 0.5\n", e);
        return 0.5;
    }
    return pct;
}

static uint16_t tz_codec_gen(uint32_t algo)
{
    const invfs_codec *c = invfs_codec_by_algo(algo);
    return c ? c->generation : 0;
}

/* WP10 §12.2: the JXL codec's decode working set from cheap JPEG headers,
 * never a trial decode. Walks the marker stream for SOF0/SOF1/SOF2
 * (0xFFC0-0xC2; baseline/extended/progressive) and returns ~w*h*3 -- the
 * pixel buffer djxl materializes before writing the JPEG back out.
 * 0 = geometry unknown (the caller admits the file and lets cjxl try). */
static uint64_t jpeg_raw_estimate(const uint8_t *j, size_t n)
{
    size_t p = 2;   /* past SOI (FF D8) */

    while (p + 4 <= n) {
        uint8_t m;
        unsigned seglen;
        if (j[p] != 0xFF) { p++; continue; }   /* tolerate garbage padding */
        m = j[p + 1];
        if (m == 0xFF) { p++; continue; }      /* fill byte */
        if (m == 0x00) { p += 2; continue; }   /* stuffed 0xFF */
        if (m == 0xD9) break;                  /* EOI */
        if (m == 0xDA) break;                  /* SOS: entropy data follows */
        if (m == 0xD8 || m == 0x01 ||
            (m >= 0xD0 && m <= 0xD7)) {        /* SOI/TEM/RSTn: no length */
            p += 2;
            continue;
        }
        seglen = ((unsigned)j[p + 2] << 8) | j[p + 3];
        if (seglen < 2 || p + 2 + seglen > n) break;
        if (m >= 0xC0 && m <= 0xC2) {
            unsigned h, w;
            if (p + 9 > n) break;
            h = ((unsigned)j[p + 5] << 8) | j[p + 6];
            w = ((unsigned)j[p + 7] << 8) | j[p + 8];
            if (!w || !h) return 0;
            return (uint64_t)w * h * 3;
        }
        p += 2 + seglen;
    }
    return 0;
}

int vol_sweep_file(invfs_volume *v, uint64_t inode_id)
{
    invfs_meta_pub keep;
    int have_keep;
    char name[256] = "";
    int rc;

    have_keep = vol_get_meta(v, inode_id, &keep) == 0;
    {
        uint64_t pos = idx_get_id(v, inode_id);
        if (pos >= v->inode_area_start * INVFS_BLOCK_SIZE &&
            pos + sizeof(invfs_inode_rec) <= v->inode_area_pos) {
            invfs_inode_rec h;
            if (io_seek(&v->io, pos) == 0 &&
                io_read(&v->io, &h, sizeof h) == 0 &&
                h.magic == INODE_REC_MAGIC && h.inode_id == inode_id) {
                size_t nl = h.name_len < sizeof(name) - 1
                          ? h.name_len : sizeof(name) - 1;
                memcpy(name, h.name, nl);
                name[nl] = 0;
            }
        }
    }

    rc = vol_sweep_file_inner(v, inode_id, 0);

    if (have_keep && name[0]) {
        uint64_t nid = vol_find(v, name);
        if (nid != 0 && nid != inode_id) {
            invfs_meta_pub chk;
            if (vol_get_meta(v, nid, &chk) != 0)
                vol_apply_meta(v, name, &keep);
        }
    }
    return rc;
}

static int vol_sweep_file_inner(invfs_volume *v, uint64_t inode_id,
                                int generic_only)
{
    uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    uint64_t end = v->inode_area_pos;
    uint64_t rec_pos = 0;
    invfs_inode_rec rec_h;
    uint8_t *rec = NULL;
    uint32_t crc_stored, crc_calc;
    invfs_ast_recipe_header ast_h;
    invfs_ast_block_entry *ents;
    uint16_t i;
    int swept_any = 0;
    /* Target of the new maps. Zero until the atomic path is chosen, and the
       whole file is written under it before anything references it. */
    uint64_t new_id = 0;
    int all_raw = 1;
    /* WP10: bytes the sweep actually stored (sum of csize+8 per segment),
       for the UNCOMPRESSIBLE gain check; a codec guard that refused the file
       (guard_algo) stamps GENERIC_GUARD instead of a gain-based class. A
       codec rejected by the decode-memory policy (memlimit_algo) stamps
       GENERIC_MEMLIMIT -- stamped on the NEW id at the end, like guard. */
    uint64_t new_bytes = 0;
    uint32_t guard_algo = 0;
    uint32_t memlimit_algo = 0;

    /* locate the inode record */
    {   /* jump straight to it; 0 = not indexed, keep the full scan */
        uint64_t ip = idx_get_id(v, inode_id);
        if (ip >= pos && ip + sizeof(invfs_inode_rec) <= end) pos = ip;
    }
    while (pos + sizeof(invfs_inode_rec) <= end) {
        if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &rec_h, sizeof(rec_h)) != 0)
            return -1;
        if (rec_h.magic != INODE_REC_MAGIC) {
            if (rec_h.magic == TOMBSTONE_MAGIC) {
                pos += rec_h.rec_len + 4;
                continue;
            }
            return -1;
        }
        if (rec_h.inode_id == inode_id) { rec_pos = pos; break; }
        pos += rec_h.rec_len + 4;
    }
    if (rec_pos == 0) return -1;

    rec = (uint8_t *)malloc(rec_h.rec_len);
    if (!rec) return -1;
    if (io_seek(&v->io, rec_pos) != 0 || io_read(&v->io, rec, rec_h.rec_len) != 0 ||
        io_read(&v->io, &crc_stored, 4) != 0) { free(rec); return -1; }
    crc_calc = invfs_crc32c(rec, rec_h.rec_len);
    if (crc_calc != crc_stored) { fprintf(stderr, "inode CRC mismatch\n"); free(rec); return -1; }

    memcpy(&ast_h, rec + sizeof(invfs_inode_rec), sizeof(ast_h));
    ents = (invfs_ast_block_entry *)(rec + sizeof(invfs_inode_rec) + sizeof(ast_h));

    /* already swept (Shadow/BINARY) — nothing to do */
    if (ents[0].zone != INVFS_ZONE_RAW) {
        if (getenv("INVFS_DEBUG"))
            printf("[sweep] %s: already in Shadow (zone %u), skip\n",
                   rec_h.name, ents[0].zone);
        free(rec);
        return 1;
    }

    /* JXL pass: if file is a RAW blob starting with JPEG magic,
     * transcode the whole file via cjxl and replace the inode. */
    if (!generic_only && ents[0].zone == INVFS_ZONE_RAW) {
        uint8_t *full = NULL;
        size_t full_len = 0;
        if (getenv("INVFS_DEBUG"))
            printf("[sweep] %s: reading full file\n", rec_h.name);
        if (vol_read_file(v, inode_id, &full, &full_len) == 0 && full_len >= 3 &&
            full[0] == 0xFF && full[1] == 0xD8 && full[2] == 0xFF) {
            const invfs_codec *jc = invfs_codec_by_algo(INVFS_ALGO_JXL);
            uint64_t raw;
            if (getenv("INVFS_DEBUG"))
                printf("[sweep] %s: JPEG detected (%zu bytes)\n",
                       rec_h.name, full_len);
            if (!jc || !jc->probe || !jc->probe()) {
                /* cjxl absent: leave the file RAW and UNSTAMPED. Falling
                 * into the generic path would be terminal for it -- zone!=RAW
                 * and a GENERIC/UNCOMPRESSIBLE stamp never re-arm when a tool
                 * appears -- so it waits, and the first sweep after cjxl is
                 * installed picks it up. */
                if (getenv("INVFS_DEBUG"))
                    printf("[sweep] %s: cjxl unavailable, deferred\n",
                           rec_h.name);
                free(full);
                free(rec);
                return 1;
            }
            /* admission from the SOF geometry (WP10 §12.2); past the
             * decode-memory policy the file goes generic, stamped so a
             * raised limit re-arms the JXL path */
            raw = jpeg_raw_estimate(full, full_len);
            if (raw && raw > vol_get_dec_mem_limit(v)) {
                if (getenv("INVFS_DEBUG"))
                    printf("[sweep] %s: JPEG raw ~%llu B > dec_mem limit\n",
                           rec_h.name, (unsigned long long)raw);
                memlimit_algo = INVFS_ALGO_JXL;
            } else {
                uint8_t *jxl = NULL;
                size_t jxl_len = 0;
                int crc_res = invfs_jxl_compress(full, full_len, &jxl, &jxl_len);
                if (getenv("INVFS_DEBUG"))
                    printf("[sweep] %s: compress rc=%d jxl_len=%zu (orig %zu)\n",
                           rec_h.name, crc_res, jxl_len, full_len);
                if (crc_res == 0 && jxl_len < full_len) {
                    /* guard: the blob must djxl back to the exact original
                     * JPEG bytes before anything is replaced */
                    uint8_t *back = NULL;
                    size_t back_len = 0;
                    int exact =
                        invfs_jxl_decompress(jxl, jxl_len, &back, &back_len) == 0 &&
                        back_len == full_len &&
                        memcmp(back, full, full_len) == 0;
                    free(back);
                    if (exact) {
                        /* atomic: create new inode first; old stays until then */
                        if (getenv("INVFS_DEBUG"))
                            printf("[sweep] %s: create JXL inode first\n", rec_h.name);
                        {
                            uint64_t newino = vol_create_jxl_file(v, rec_h.name, jxl, jxl_len, full_len);
                            if (newino == 0)
                                fprintf(stderr, "sweep: JXL create failed (%s)\n", rec_h.name);
                            else {
                                vol_delete_inode(v, inode_id, rec_h.name);
                                vol_stamp_class(v, newino, INVFS_CLASS_CODEC,
                                                INVFS_ALGO_JXL, tz_codec_gen(INVFS_ALGO_JXL));
                                swept_any = 1;
                            }
                        }
                        free(jxl);
                        free(full);
                        free(rec);
                        if (getenv("INVFS_DEBUG"))
                            printf("[sweep] %s: JXL %llu -> %zu bytes\n",
                                   rec_h.name, (unsigned long long)full_len, jxl_len);
                        /* 2, not 0: the generic ZSTD-19 path below also returns 0,
                           so the caller could not tell a JPEG that went through
                           cjxl from one that got shadow-compressed -- and every
                           cover in a sweep was reported as "Shadow (ZSTD-19)"
                           even when JXL had done the work. */
                        return swept_any ? 2 : 1;
                    }
                    if (getenv("INVFS_DEBUG"))
                        printf("[sweep] %s: JXL round-trip mismatch\n", rec_h.name);
                }
                /* JXL declined (tool failed, no gain, or the round-trip
                 * guard refused): guard-stamp so the next sweep skips the
                 * retry until cjxl's generation improves */
                guard_algo = INVFS_ALGO_JXL;
                free(jxl);
            }
        }
        free(full);
    }

    /* APE pass: FLAC magic -> transcode whole file via MAC.exe -c4000 */
    if (!generic_only && ents[0].zone == INVFS_ZONE_RAW && getenv("INVFS_APE")) {
        uint8_t *full = NULL;
        size_t full_len = 0;
        if (vol_read_file(v, inode_id, &full, &full_len) == 0 && full_len >= 4 &&
            full[0] == 'f' && full[1] == 'L' && full[2] == 'a' && full[3] == 'C') {
            uint8_t *ape = NULL;
            size_t ape_len = 0;
            int crc_res = invfs_ape_compress(full, full_len, &ape, &ape_len);
            if (getenv("INVFS_DEBUG"))
                printf("[sweep] %s: APE rc=%d ape_len=%zu (flac %zu)\n",
                       rec_h.name, crc_res, ape_len, full_len);
            if (crc_res == 0 && ape_len < full_len) {
                /* probe decode to learn exact re-encoded FLAC size
                 * (ffmpeg re-encode differs from original FLAC bytes) */
                uint8_t *probe = NULL;
                size_t probe_len = 0;
                uint64_t fsize = full_len;
                if (invfs_ape_decompress(ape, ape_len, &probe, &probe_len) == 0) {
                    fsize = probe_len;
                    free(probe);
                }
                {
                    uint64_t newino = vol_create_ape_file(v, rec_h.name, ape, ape_len, fsize);
                    if (newino == 0)
                        fprintf(stderr, "sweep: APE create failed (%s)\n", rec_h.name);
                    else {
                        vol_delete_inode(v, inode_id, rec_h.name);
                        vol_stamp_class(v, newino, INVFS_CLASS_CODEC,
                                        INVFS_ALGO_APE, tz_codec_gen(INVFS_ALGO_APE));
                        swept_any = 1;
                    }
                }
                free(ape); free(full); free(rec);
                return swept_any ? 0 : 1;
            }
            guard_algo = INVFS_ALGO_APE;
            free(ape);
        }
        free(full);
    }

    /* Which path can be taken. A file whose segments are ALL still RAW can be
       handed to a fresh inode id wholesale. A partially swept one cannot: the
       already-Shadow segments are mapped under the old id, and
       vol_delete_inode(old) frees every block the old id's maps name -- it
       would free blocks the new record still uses. Such a file can only come
       from a volume damaged by the pre-atomic code (a mid-file alloc failure);
       finish it the old way, in place, which is the behaviour it already had. */
    for (i = 0; i < ast_h.num_blocks; i++)
        if (ents[i].zone != INVFS_ZONE_RAW) { all_raw = 0; break; }
    if (all_raw && ast_h.num_blocks > 0) {
        if (vol_mark_dirty(v) != 0) { free(rec); return -1; }
        new_id = v->next_inode_id++;
    } else if (!all_raw) {
        fprintf(stderr, "sweep: %s is partially swept; finishing in place "
                        "(pre-atomic volume)\n", rec_h.name);
    }

    for (i = 0; i < ast_h.num_blocks; i++) {
        invfs_ast_block_entry *e = &ents[i];
        uint64_t pba_old = 0, phys_len_old = 0;
        uint32_t hdr_old, crc_old;
        uint8_t *blob_old = NULL, *orig = NULL;
        size_t orig_len = e->length;
        uint64_t pba_new, phys_blocks_new;
        uint32_t csize_new, crc_new;
        uint8_t *cbuf_new = NULL;
        int cbound;
        uint8_t hdr4[8];  /* [4B csize][4B crc] — was [4] = stack overflow */

        if (e->zone != INVFS_ZONE_RAW)
            continue;  /* already swept */
        /* 1. read old segment */
        if (vol_lookup_entry(v, inode_id, e->block_id, &pba_old, &phys_len_old) != 0) {
            fprintf(stderr, "sweep: L2P miss seg %u\n", e->block_id);
            sweep_unwind(v, new_id); free(rec); return -1;
        }
        {
            uint8_t hdrb[8];
            if (io_seek(&v->io, pba_old * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, hdrb, 8) != 0) { sweep_unwind(v, new_id); free(rec); return -1; }
            memcpy(&hdr_old, hdrb, 4);
            memcpy(&crc_old, hdrb + 4, 4);
        }
        blob_old = (uint8_t *)malloc(hdr_old);
        if (!blob_old) { sweep_unwind(v, new_id); free(rec); return -1; }
        if (io_seek(&v->io, pba_old * INVFS_BLOCK_SIZE + 8) != 0 ||
            io_read(&v->io, blob_old, hdr_old) != 0) { free(blob_old); sweep_unwind(v, new_id); free(rec); return -1; }
        /* deep protection: verify segment CRC32C */
        if (crc_old != 0 && invfs_crc32c(blob_old, hdr_old) != crc_old) {
            fprintf(stderr, "sweep: segment CRC mismatch inode %llu seg %u\n",
                    (unsigned long long)inode_id, e->block_id);
            free(blob_old); sweep_unwind(v, new_id); free(rec); return -1;
        }

        /* 2. decompress to original */
        orig = (uint8_t *)malloc(orig_len ? orig_len : 1);
        if (!orig) { free(blob_old); sweep_unwind(v, new_id); free(rec); return -1; }
        if (e->algo == INVFS_ALGO_LZ4) {
            int got = LZ4_decompress_safe((const char *)blob_old, (char *)orig,
                                          (int)hdr_old, (int)orig_len);
            if (got != (int)orig_len) { free(blob_old); free(orig); sweep_unwind(v, new_id); free(rec); return -1; }
        } else {  /* NONE */
            if (hdr_old != orig_len) { free(blob_old); free(orig); sweep_unwind(v, new_id); free(rec); return -1; }
            memcpy(orig, blob_old, orig_len);
        }
        free(blob_old);

        /* 3. recompress with ZSTD level 19 (fallback raw) */
        cbound = (int)ZSTD_compressBound(orig_len);
        cbuf_new = (uint8_t *)malloc((size_t)cbound + 8 + INVFS_BLOCK_SIZE);
        if (!cbuf_new) { free(orig); sweep_unwind(v, new_id); free(rec); return -1; }
        csize_new = (uint32_t)ZSTD_compress(cbuf_new + 8, cbound, orig, orig_len, 19);
        if (ZSTD_isError(csize_new) || csize_new >= orig_len) {
            csize_new = (uint32_t)orig_len;   /* store raw */
            memcpy(cbuf_new + 8, orig, orig_len);
            e->algo = INVFS_ALGO_NONE;
        } else {
            e->algo = INVFS_ALGO_ZSTD;
        }
        e->zone = INVFS_ZONE_BINARY;
        /* block_id stays the segment index (L2P key) */
        crc_new = invfs_crc32c(cbuf_new + 8, csize_new);

        /* 4. write to Shadow zone */
        phys_blocks_new = ((uint64_t)csize_new + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
        pba_new = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                               phys_blocks_new, 1);
        if (pba_new == 0) { free(cbuf_new); free(orig); sweep_unwind(v, new_id); free(rec); return -1; }
        hdr4[0] = (uint8_t)(csize_new & 0xFF);
        hdr4[1] = (uint8_t)((csize_new >> 8) & 0xFF);
        hdr4[2] = (uint8_t)((csize_new >> 16) & 0xFF);
        hdr4[3] = (uint8_t)((csize_new >> 24) & 0xFF);
        hdr4[4] = (uint8_t)(crc_new & 0xFF);
        hdr4[5] = (uint8_t)((crc_new >> 8) & 0xFF);
        hdr4[6] = (uint8_t)((crc_new >> 16) & 0xFF);
        hdr4[7] = (uint8_t)((crc_new >> 24) & 0xFF);
        memcpy(cbuf_new, hdr4, 8);
        new_bytes += (uint64_t)csize_new + 8;   /* WP10 gain accounting */
        if (write_segment_blocks(v, pba_new, cbuf_new, (size_t)csize_new + 8,
                                 phys_blocks_new) != 0) {
            free(cbuf_new); free(orig); sweep_unwind(v, new_id); free(rec); return -1;
        }
        free(cbuf_new);

        /* 5. L2P: map the new segment -- in memory only, same as vol_map.
         * The UNMAP record used to be written straight to the device here.
         * It was redundant twice over: vol_flush rebuilds the journal from
         * the in-memory table, which l2p_remove has already updated, and
         * that table holds only MAP entries, so a compacted journal never
         * contains UNMAP at all (replay applies them, flush never emits
         * them). It also landed on top of the terminator -- see vol_map.
         *
         * On the atomic path the new mapping goes under new_id and the old
         * inode keeps its own maps untouched, so the file stays fully
         * readable from its old record until the new one is durable. */
        if (new_id) {
            if (vol_map(v, new_id, e->block_id, pba_new, (uint32_t)phys_blocks_new) != 0) {
                free(orig); sweep_unwind(v, new_id); free(rec); return -1;
            }
        } else {
            l2p_remove(v, inode_id, e->block_id);
            if (vol_map(v, inode_id, e->block_id, pba_new, (uint32_t)phys_blocks_new) != 0) {
                free(orig); free(rec); return -1;
            }
            /* 6. free old RAW blocks (in-place path only; the atomic path
             * leaves them to vol_delete_inode of the old id, which is what
             * makes the old record readable until the new one lands).
             *
             * Free exactly what was allocated: the L2P entry's length is the
             * phys_blocks the write path asked alloc_blocks for. Recomputing
             * it from the payload used "hdr_old + 4" against a segment written
             * as csize + 8, so whenever those two straddled a block boundary
             * the last block was never returned to the bitmap -- allocated,
             * owned by no record, which is exactly what fsck calls an orphan.
             * A sweep of ~1900 segments leaked about one, matching the
             * ~4-in-4096 chance that the 4-byte discrepancy crosses a
             * boundary. */
            vol_free_blocks(v, pba_old, phys_len_old);
        }
        free(orig);
        swept_any = 1;

        if (getenv("INVFS_DEBUG"))
            printf("[sweep] inode %llu seg %u: RAW(%llu) -> SHADOW(%llu) %u bytes (%s)\n",
                   (unsigned long long)inode_id, e->block_id,
                   (unsigned long long)pba_old, (unsigned long long)pba_new,
                   csize_new, e->algo == INVFS_ALGO_ZSTD ? "zstd-19" : "raw");
    }

    /* 7. Publish. On the atomic path the record is appended under new_id and
     * the old one tombstoned -- the same delete+create shape every other
     * transcode uses, and the reason vol_pre_record's ordering is enough here:
     * new_id's maps are on disk before any record mentions new_id, and the old
     * record stays readable until the instant the new one lands. */
    if (swept_any && new_id) {
        invfs_inode_rec *nh = (invfs_inode_rec *)rec;
        char name[INVFS_MAX_NAME + 1];
        size_t nlen = rec_h.name_len > INVFS_MAX_NAME ? INVFS_MAX_NAME : rec_h.name_len;
        memcpy(name, rec_h.name, nlen);
        name[nlen] = 0;

        nh->inode_id = new_id;
        crc_calc = invfs_crc32c(rec, rec_h.rec_len);
        if (v->inode_area_pos + rec_h.rec_len + 4 +
            sizeof(invfs_inode_rec) + 4 > v->inode_area_end) {
            /* no room for the record AND the tombstone that must follow it */
            fprintf(stderr, "sweep: inode area full (%s)\n", name);
            sweep_unwind(v, new_id); free(rec); return -1;
        }
        if (vol_pre_record(v) != 0) { sweep_unwind(v, new_id); free(rec); return -1; }
        if (io_seek(&v->io, v->inode_area_pos) != 0 ||
            io_write(&v->io, rec, rec_h.rec_len) != 0 ||
            io_write(&v->io, &crc_calc, 4) != 0) {
            sweep_unwind(v, new_id); free(rec); return -1;
        }
        v->inode_area_pos += rec_h.rec_len + 4;
        idx_put(v, name, nlen, new_id, v->inode_area_pos - rec_h.rec_len - 4,
                nh->file_size, nh->ctime);
        idx_put_id(v, new_id, v->inode_area_pos - rec_h.rec_len - 4);
        /* frees the old RAW blocks: they are still mapped to the old id */
        if (vol_delete_inode(v, inode_id, name) != 0)
            fprintf(stderr, "sweep: %s transcoded but the old record survived; "
                            "invf-fsck -f will reclaim it\n", name);
    } else if (swept_any) {
        /* in-place, pre-atomic volumes only (see the note above the loop) */
        crc_calc = invfs_crc32c(rec, rec_h.rec_len);
        if (io_seek(&v->io, rec_pos) != 0 ||
            io_write(&v->io, rec, rec_h.rec_len) != 0 ||
            io_write(&v->io, &crc_calc, 4) != 0) { free(rec); return -1; }
    } else {
        sweep_unwind(v, new_id);   /* nothing swept: give the id's maps back */
    }
    /* WP10 §2: stamp the outcome class. A codec guard that refused the file
     * pins GENERIC_GUARD (retried when that codec's generation grows past the
     * stored one); otherwise the file-level gain decides UNCOMPRESSIBLE vs
     * GENERIC. A GUARD/MEMLIMIT stamp the caller set before we ran (it lives
     * in the copied ext) wins over the gain verdict -- it carries the retry
     * semantics the gain verdict knows nothing about. */
    if (swept_any && ast_h.file_size > 0) {
        uint64_t tid = new_id ? new_id : inode_id;
        uint8_t oc = 0, oa = 0;
        uint16_t og = 0;
        int havec = vol_get_class(v, tid, &oc, &oa, &og) == 0;
        if (guard_algo) {
            vol_stamp_class(v, tid, INVFS_CLASS_GENERIC_GUARD,
                            (uint8_t)guard_algo, tz_codec_gen(guard_algo));
        } else if (memlimit_algo) {
            vol_stamp_class(v, tid, INVFS_CLASS_GENERIC_MEMLIMIT,
                            (uint8_t)memlimit_algo, tz_codec_gen(memlimit_algo));
        } else if (!havec || oc == INVFS_CLASS_GENERIC ||
                   oc == INVFS_CLASS_UNCOMPRESSIBLE) {
            double pct = vol_min_gain_pct();
            if ((double)new_bytes >=
                (double)ast_h.file_size * (1.0 - pct / 100.0))
                vol_stamp_class(v, tid, INVFS_CLASS_UNCOMPRESSIBLE, 0,
                                invfs_registry_generation());
            else
                vol_stamp_class(v, tid, INVFS_CLASS_GENERIC, INVFS_ALGO_ZSTD,
                                tz_codec_gen(INVFS_ALGO_ZSTD));
        }
    }
    free(rec);
    return swept_any ? 0 : 1;
}

/* ---- on-demand sweep core (embedded daemon / CLI) ---- */

/* pending list lives in RAM (daemon holds the volume open with one L2P
   in memory); a crash leaves files unswept — invariant: nothing lost. */
void vol_mark_pending(invfs_volume *v, uint64_t inode_id)
{
    if (!v || inode_id == 0) return;
    for (size_t i = 0; i < v->n_pending; i++)
        if (v->pending[i] == inode_id) return;
    if (v->n_pending >= v->cap_pending) {
        size_t nc = v->cap_pending ? v->cap_pending * 2 : 64;
        uint64_t *np = (uint64_t *)realloc(v->pending, nc * sizeof(uint64_t));
        if (!np) return;
        v->pending = np;
        v->cap_pending = nc;
    }
    v->pending[v->n_pending++] = inode_id;
}

void vol_unmark_pending(invfs_volume *v, uint64_t inode_id)
{
    if (!v) return;
    for (size_t i = 0; i < v->n_pending; i++) {
        if (v->pending[i] == inode_id) {
            v->pending[i] = v->pending[v->n_pending - 1];
            v->n_pending--;
            return;
        }
    }
}

size_t vol_pending_count(invfs_volume *v)
{
    return v ? v->n_pending : 0;
}

/* Zone of an inode's first AST segment, or -1 if it cannot be read.
   RAW means "never swept". Needed because vol_read_file hands back
   DECODED bytes: a PMP inode still looks exactly like an MP3 to a magic
   test, so without this a re-sweep would transcode it again -- costing
   seconds per file and rewriting flash for no gain. The other container
   codecs dodge this by checking for their sibling "!recipe" inode; a PMP
   blob has no sibling, so it checks the zone directly. */
static int vol_inode_first_zone(invfs_volume *v, uint64_t inode_id)
{
    uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    uint64_t end = v->inode_area_pos;
    invfs_inode_rec rec_h;
    invfs_ast_recipe_header ast_h;
    invfs_ast_block_entry e0;
    uint64_t ip = idx_get_id(v, inode_id);

    if (ip >= pos && ip + sizeof(invfs_inode_rec) <= end) pos = ip;
    while (pos + sizeof(invfs_inode_rec) <= end) {
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &rec_h, sizeof rec_h) != 0) return -1;
        if (rec_h.magic == TOMBSTONE_MAGIC) { pos += rec_h.rec_len + 4; continue; }
        if (rec_h.magic != INODE_REC_MAGIC) return -1;
        if (rec_h.inode_id == inode_id) break;
        pos += rec_h.rec_len + 4;
    }
    if (pos + sizeof(invfs_inode_rec) > end) return -1;
    if (io_seek(&v->io, pos + sizeof(invfs_inode_rec)) != 0 ||
        io_read(&v->io, &ast_h, sizeof ast_h) != 0 ||
        io_read(&v->io, &e0, sizeof e0) != 0) return -1;
    if (ast_h.num_blocks == 0) return -1;
    return (int)e0.zone;
}

/* WP14b candidate shape (WP10 §12.7 v2): every AST entry is a per-segment
 * generic store (BINARY zone, NONE/LZ4/ZSTD algo). An extraction container's
 * part ("name!partN") looks exactly like this before batching; a part
 * holding anything else (a whole-file JXL/APE blob, a batch member) is not
 * a candidate. 1 = the shape matches. */
static int part_generic_segments(invfs_volume *v, uint64_t inode_id)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0;
    invfs_ast_recipe_header ah;
    const invfs_ast_block_entry *ents;
    size_t base = sizeof(invfs_inode_rec);
    uint16_t i;
    int ok = 0;

    if (meta_read_record_by_id(v, inode_id, &buf, &rl, NULL, 0, NULL) != 0)
        return 0;
    if (rl >= base + sizeof(ah)) {
        memcpy(&ah, buf + base, sizeof(ah));
        if (ah.num_blocks && ah.num_children == 0 &&
            rl >= base + sizeof(ah) +
                   (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
            ents = (const invfs_ast_block_entry *)(buf + base + sizeof(ah));
            ok = 1;
            for (i = 0; i < ah.num_blocks; i++) {
                if (ents[i].zone != INVFS_ZONE_BINARY ||
                    (ents[i].algo != INVFS_ALGO_NONE &&
                     ents[i].algo != INVFS_ALGO_LZ4 &&
                     ents[i].algo != INVFS_ALGO_ZSTD)) {
                    ok = 0;
                    break;
                }
            }
        }
    }
    free(buf);
    return ok;
}

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
static int tz_defer(invfs_volume *v, uint64_t inode_id, const char *name,
                    uint64_t size, uint32_t family);
static int bz_defer(invfs_volume *v, uint64_t inode_id, const char *name,
                    uint64_t size, uint32_t family);
static int vol_class_downgrade(invfs_volume *v, uint64_t inode_id,
                               const char *name, uint8_t calgo);
static int tz_sniff_any(invfs_volume *v, uint64_t inode_id, const char *name);
static int tz_member_oversized(invfs_volume *v, uint64_t inode_id,
                               uint64_t unit_limit);
static int vol_pack_sweep(invfs_volume *v, uint64_t inode_id,
                          const char *name, const invfs_codec *pc,
                          const uint8_t *full, size_t full_len);
/* WP14b M2 (definitions live with the container creators, below) */
static uint64_t vol_create_blob_file(invfs_volume *v, const char *name,
                                     const uint8_t *data, size_t len,
                                     uint64_t orig_size, uint32_t algo);
static int vol_delete_siblings(invfs_volume *v, const char *name);
static uint64_t vol_transcode_abort(invfs_volume *v, const char *name);

/* WP14b: defer the parts of a just-exploded extraction container
 * ("name!partN", N = 0..) into the batching accumulators, so a TAR swept in
 * this run has its members batched by THIS run's vol_tz_flush instead of
 * sitting one run in per-file ZSTD. The flush re-reads and re-sniffs each
 * part from its live record, so a head sniff is enough here; parts that
 * sniff as nothing stay per-file generic (the absent-stamp walk branch
 * reconsiders them next run). */
static void defer_container_parts(invfs_volume *v, const char *name)
{
    uint8_t head[8192];
    char pn[320];
    unsigned i;
    int n_bin = 0, n_text = 0;

    for (i = 0; ; i++) {
        uint64_t pino, fsz = 0;
        int got, bfam, tfam;

        snprintf(pn, sizeof pn, "%s!part%u", name, i);
        pino = vol_find(v, pn);
        if (!pino) break;
        got = vol_read_range(v, pino, 0, sizeof head, head);
        if (got <= 0 ||
            vol_stat_full(v, pn, NULL, &fsz, NULL) != 0 || !fsz)
            continue;
        bfam = invfs_binary_family(head, (size_t)got, pn);
        if (bfam > 0) {
            if (bz_defer(v, pino, pn, fsz, (uint32_t)bfam) == 0) n_bin++;
            continue;
        }
        tfam = invfs_text_family(pn, head, (size_t)got);
        if (tfam > 0) {
            const invfs_codec *pc = invfs_codec_by_algo(INVFS_ALGO_PPMD);
            if (pc && pc->dec_mem_bytes > vol_get_dec_mem_limit(v))
                vol_stamp_class(v, pino, INVFS_CLASS_GENERIC_MEMLIMIT,
                                INVFS_ALGO_PPMD, pc->generation);
            else if (tz_defer(v, pino, pn, fsz, (uint32_t)tfam) == 0)
                n_text++;
        }
    }
    /* one summary line per container, not one per part (a Silesia TAR has
     * ~1500 members); same format the sweep driver's part aggregator uses */
    if (n_bin)
        printf("  %s!*: %d parts -> ZSTD batch\n", name, n_bin);
    if (n_text)
        printf("  %s!*: %d parts -> PPMd batch\n", name, n_text);
}

/* WP12(b): JPEG upgrade retry for a file the class predicate just re-armed
 * (GENERIC_MEMLIMIT: the raised dec_mem limit admits it; GENERIC_GUARD: a
 * newer codec generation). By the time either stamp exists the file is
 * stored as generic BINARY segments, so the RAW-gated JXL branch in
 * vol_sweep_file_inner could never see it again -- this runs the SAME full
 * JPEG attempt on the current (decoded) content: cjxl, djxl decode-back
 * memcmp guard, and the RAW branch's publish shape (new blob inode first,
 * retire the old one after, meta carried across).
 * Outcomes: transcode -> CODEC{JXL,gen} on the new id, returns 7 (the
 * caller's JPEG->JXL code); still over the limit -> GENERIC_MEMLIMIT at
 * the current generation; guard refusal -> GENERIC_GUARD{JXL, current
 * gen} (a stale gen would refire the retry on every sweep); cjxl absent
 * or create failed -> stamp untouched, the file waits. 0/-1 otherwise. */
static int vol_jxl_retry(invfs_volume *v, uint64_t inode_id, const char *name)
{
    const invfs_codec *jc = invfs_codec_by_algo(INVFS_ALGO_JXL);
    uint8_t *full = NULL, *jxl = NULL, *back = NULL;
    size_t full_len = 0, jxl_len = 0, back_len = 0;
    uint64_t raw;
    int rc = 0;

    /* the tool vanished since the stamp was written: keep the stamp --
     * the first sweep after cjxl returns picks the file up again */
    if (!jc || !jc->probe || !jc->probe()) return 0;
    if (vol_read_file(v, inode_id, &full, &full_len) != 0) return -1;
    /* the stamp says "JPEG rejected earlier"; if the content is not one
     * any more the stamp is stale -- leave file and stamp alone */
    if (full_len < 3 || full[0] != 0xFF || full[1] != 0xD8 || full[2] != 0xFF)
        goto out;

    /* same admission gate as the RAW branch (WP10 §12.2: SOF geometry,
     * never a trial decode) */
    raw = jpeg_raw_estimate(full, full_len);
    if (raw && raw > vol_get_dec_mem_limit(v)) {
        vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                        INVFS_ALGO_JXL, tz_codec_gen(INVFS_ALGO_JXL));
        goto out;
    }

    if (invfs_jxl_compress(full, full_len, &jxl, &jxl_len) == 0 &&
        jxl_len < full_len) {
        /* guard: the blob must djxl back to the exact original bytes */
        int exact =
            invfs_jxl_decompress(jxl, jxl_len, &back, &back_len) == 0 &&
            back_len == full_len && memcmp(back, full, full_len) == 0;
        free(back);
        if (exact) {
            invfs_meta_pub keep;
            int have_keep = vol_get_meta(v, inode_id, &keep) == 0;
            uint64_t newino =
                vol_create_jxl_file(v, name, jxl, jxl_len, full_len);
            if (newino) {
                vol_delete_inode(v, inode_id, name);
                /* the fresh blob record has no ext; carry the old meta
                 * across, exactly like the vol_sweep_file wrapper does */
                if (have_keep) {
                    invfs_meta_pub chk;
                    if (vol_get_meta(v, newino, &chk) != 0)
                        vol_apply_meta(v, name, &keep);
                }
                vol_stamp_class(v, newino, INVFS_CLASS_CODEC,
                                INVFS_ALGO_JXL, tz_codec_gen(INVFS_ALGO_JXL));
                rc = 7;
            } else {
                /* no space for the blob: NOT a guard refusal -- keep the
                 * old stamp so the retry re-arms */
                fprintf(stderr, "sweep: JXL create failed (%s)\n", name);
            }
            goto out;
        }
    }
    /* tool failed / no gain / round-trip mismatch: guard-stamp at the
     * CURRENT generation so the predicate stops refiring every sweep */
    vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                    INVFS_ALGO_JXL, tz_codec_gen(INVFS_ALGO_JXL));
out:
    free(jxl);
    free(full);
    return rc;
}

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
static int vol_exer_carve(invfs_volume *v, uint64_t inode_id,
                          const char *name, const uint8_t *full,
                          size_t full_len, uint32_t *nparts_out)
{
    const invfs_codec *jc = invfs_codec_by_algo(INVFS_ALGO_JXL);
    exe_region reg[EXE_MAX_MEDIA];
    /* per-member build state: the stored blob plus its decode-back proof */
    struct { uint8_t *blob, *back; size_t blen; } pm[EXE_MAX_MEDIA];
    exer_row rows[EXE_MAX_MEDIA];
    invfs_meta_pub keep;
    int have_keep, rc = 0;
    size_t nr, i, kept = 0;
    uint64_t media_sum = 0, kept_sum = 0, blob_total;
    uint8_t *pay = NULL, *cblob = NULL;
    size_t pay_len, cblob_len = 0;

    /* cjxl absent: no carve at all -- the JPEG members are the point */
    if (!jc || !jc->probe || !jc->probe()) return 0;
    if (name_too_long_for_children(name)) return 0;
    {
        /* leftover siblings can only come from a carve killed mid-commit
         * (a finished one is CONTAINER-stamped and never reaches here):
         * purge and proceed rather than refusing the file forever */
        char p0[288];
        snprintf(p0, sizeof p0, "%s!exr0", name);
        if (vol_find(v, p0) != 0)
            vol_delete_siblings(v, name);
    }

    nr = exe_scan_media(full, full_len, reg, EXE_MAX_MEDIA);
    if (nr == 0) return 0;
    for (i = 0; i < nr; i++) media_sum += reg[i].len;
    if (media_sum < (64ull << 10) || media_sum * 20 < full_len)
        return 0;   /* not worth carving: binary batching is its home */
    {
        /* decode-time working set (WP10 §12.2, mirrored): the read holds
         * the decoded payload (table + glue) plus the member bytes */
        uint64_t usize = 8 + 17 * (uint64_t)nr + (full_len - media_sum);
        if (usize + media_sum > vol_get_dec_mem_limit(v)) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                            INVFS_ALGO_EXER, tz_codec_gen(INVFS_ALGO_EXER));
            return 2;
        }
    }

    have_keep = vol_get_meta(v, inode_id, &keep) == 0;
    memset(pm, 0, sizeof pm);
    for (i = 0; i < nr; i++) {
        const uint8_t *mem = full + reg[i].off;
        size_t ml = (size_t)reg[i].len;
        exer_row *r = &rows[kept];

        if (reg[i].kind == 1) {
            /* JPEG -> lossless JXL; SOF-geometry admission first (the
             * whole-file branch's rule, per member) */
            uint64_t raw = jpeg_raw_estimate(mem, ml);
            size_t bl = 0;
            if (raw && raw > vol_get_dec_mem_limit(v)) continue;
            if (invfs_jxl_compress(mem, ml, &pm[kept].blob,
                                   &pm[kept].blen) != 0 ||
                pm[kept].blen >= ml)
                goto skip;
            if (invfs_jxl_decompress(pm[kept].blob, pm[kept].blen,
                                     &pm[kept].back, &bl) != 0 ||
                bl != ml || memcmp(pm[kept].back, mem, ml) != 0)
                goto skip;   /* guard refused: the range stays glue */
            r->codec = INVFS_ALGO_JXL;
        } else {
            /* PNG: carved verbatim under ZSTD-19 (see the header note) */
            size_t cb = ZSTD_compressBound(ml), cl;
            pm[kept].blob = (uint8_t *)malloc(cb);
            pm[kept].back = (uint8_t *)malloc(ml);
            if (!pm[kept].blob || !pm[kept].back)
                goto skip;
            cl = ZSTD_compress(pm[kept].blob, cb, mem, ml, 19);
            if (ZSTD_isError(cl) || cl >= ml)
                goto skip;
            pm[kept].blen = cl;
            {
                size_t d = ZSTD_decompress(pm[kept].back, ml,
                                           pm[kept].blob, cl);
                if (ZSTD_isError(d) || d != ml)
                    goto skip;
            }
            r->codec = INVFS_ALGO_ZSTD;
        }
        r->off = reg[i].off;
        r->len = reg[i].len;
        kept_sum += r->len;
        kept++;
        continue;
    skip:
        free(pm[kept].blob); free(pm[kept].back);
        memset(&pm[kept], 0, sizeof pm[kept]);
    }
    if (kept == 0)
        goto out;   /* every member refused: binary batching is its home */

    /* payload = [EXER][u32 n][rows][glue (original minus carved ranges)] */
    pay_len = 8 + 17 * kept + (size_t)(full_len - kept_sum);
    pay = (uint8_t *)malloc(pay_len);
    cblob = (uint8_t *)malloc(ZSTD_compressBound(pay_len));
    if (!pay || !cblob) goto out;
    memcpy(pay, "EXER", 4);
    pay[4] = (uint8_t)kept;
    pay[5] = (uint8_t)(kept >> 8);
    pay[6] = (uint8_t)(kept >> 16);
    pay[7] = (uint8_t)(kept >> 24);
    {
        size_t gp = 8 + 17 * kept;
        uint64_t fp = 0;
        for (i = 0; i < kept; i++) {
            exer_wr64(pay + 8 + 17 * i, rows[i].off);
            exer_wr64(pay + 8 + 17 * i + 8, rows[i].len);
            pay[8 + 17 * i + 16] = rows[i].codec;
            memcpy(pay + gp, full + fp, (size_t)(rows[i].off - fp));
            gp += (size_t)(rows[i].off - fp);
            fp = rows[i].off + rows[i].len;
        }
        memcpy(pay + gp, full + fp, (size_t)(full_len - fp));
    }
    {
        size_t cl = ZSTD_compress(cblob, ZSTD_compressBound(pay_len),
                                  pay, pay_len, 19);
        if (ZSTD_isError(cl)) goto out;
        cblob_len = cl;
    }

    /* the house invariant, whole-file form: decode the FINAL blob, parse
     * the table back out of it, splice the decode-proven member bytes at
     * the table offsets, and memcmp the rebuild against the original */
    {
        uint8_t *dec = (uint8_t *)malloc(pay_len);
        uint8_t *reb = (uint8_t *)malloc(full_len ? full_len : 1);
        uint8_t *pb[EXE_MAX_MEDIA];
        exer_row *grows = NULL;
        size_t gn = 0;
        int okm = 0;

        if (dec && reb) {
            size_t d = ZSTD_decompress(dec, pay_len, cblob, cblob_len);
            grows = (exer_row *)malloc(EXE_MAX_MEDIA * sizeof *grows);
            if (grows && !ZSTD_isError(d) && d == pay_len &&
                exer_payload_parse(dec, d, full_len, grows,
                                   EXE_MAX_MEDIA, &gn) == 0 && gn == kept) {
                for (i = 0; i < kept; i++) pb[i] = pm[i].back;
                exer_splice(dec, grows, gn, pb, reb, full_len);
                okm = memcmp(reb, full, full_len) == 0;
            }
        }
        free(dec); free(reb); free(grows);
        if (!okm) {
            fprintf(stderr, "EXER: %s: rebuild guard refused, "
                            "carve abandoned\n", name);
            goto out;
        }
    }

    /* size guard (the TAR/GZ rule): the stored form must beat the file */
    blob_total = cblob_len;
    for (i = 0; i < kept; i++) blob_total += pm[i].blen;
    if (blob_total >= full_len) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[vol] %s: EXER blobs %llu >= exe %zu -- keep "
                            "original\n", name, (unsigned long long)blob_total,
                    full_len);
        goto out;
    }

    /* children first, the name-owning record last (the FLAC note) */
    for (i = 0; i < kept; i++) {
        char pn[288];
        uint64_t pino;
        snprintf(pn, sizeof pn, "%s!exr%zu", name, i);
        pino = vol_create_blob_file(v, pn, pm[i].blob, pm[i].blen,
                                    rows[i].len, rows[i].codec);
        if (!pino) {
            fprintf(stderr, "EXER: part inode failed for %s\n", pn);
            vol_transcode_abort(v, name);
            goto out;
        }
        if (rows[i].codec == INVFS_ALGO_JXL)
            vol_stamp_class(v, pino, INVFS_CLASS_CODEC, INVFS_ALGO_JXL,
                            tz_codec_gen(INVFS_ALGO_JXL));
        else
            vol_stamp_class(v, pino, INVFS_CLASS_GENERIC, INVFS_ALGO_ZSTD,
                            tz_codec_gen(INVFS_ALGO_ZSTD));
    }
    {
        uint64_t newino = vol_create_blob_file(v, name, cblob, cblob_len,
                                               (uint64_t)full_len,
                                               INVFS_ALGO_EXER);
        if (!newino) {
            /* no space for the main record: NOT a guard refusal (the
             * vol_jxl_retry convention) -- leave unstamped, the file is
             * untouched and a retry re-arms */
            fprintf(stderr, "sweep: EXER create failed (%s)\n", name);
            vol_transcode_abort(v, name);
            goto out;
        }
        vol_delete_inode(v, inode_id, name);
        /* the fresh blob record has no ext; carry the old meta across,
         * exactly like the vol_jxl_retry flow does */
        if (have_keep) {
            invfs_meta_pub chk;
            if (vol_get_meta(v, newino, &chk) != 0)
                vol_apply_meta(v, name, &keep);
        }
        vol_stamp_class(v, newino, INVFS_CLASS_CONTAINER, INVFS_ALGO_EXER,
                        tz_codec_gen(INVFS_ALGO_EXER));
    }
    *nparts_out = (uint32_t)kept;
    rc = 1;
out:
    for (i = 0; i < kept; i++) {
        free(pm[i].blob);
        free(pm[i].back);
    }
    free(pay);
    free(cblob);
    return rc;
}

/* process a single inode: container explode / transcode / shadow move.
   Returns 1 if the inode was replaced/transcoded, 0 if not applicable,
   -1 on hard error (caller keeps it pending or aborts). */
int vol_sweep_one(invfs_volume *v, uint64_t inode_id, const char *name)
{
    char rname[272], p0name[272], jn[272];
    uint8_t *full = NULL;
    size_t full_len = 0;

    if (!name || strlen(name) > 240 || inode_id == 0) return 0;
    /* internal control names (the "\x01tzb" batch owner) are never swept */
    if ((uint8_t)name[0] == 0x01) return 0;

    /* WP10 §2: class-aware walk predicate. A present class flag decides
     * skip/retry/downgrade without touching content; absent = the legacy
     * rule (RAW zone = full path below, anything else = already swept). */
    {
        uint8_t ccls = 0, calgo = 0;
        uint16_t cgen = 0;
        if (vol_get_class(v, inode_id, &ccls, &calgo, &cgen) == 0) {
            const invfs_codec *cc = invfs_codec_by_algo(calgo);
            switch (ccls) {
            case INVFS_CLASS_UNCOMPRESSIBLE:
                /* Retry only when the registry grew AND a codec actually
                 * sniffs the content now (the sniff is the cheap part --
                 * one head segment, no full read). */
                if (invfs_registry_generation() <= cgen) return 0;
                if (!tz_sniff_any(v, inode_id, name)) {
                    /* Nothing claims it even now: advance the snapshot so
                     * the next sweep skips the sniff until the registry
                     * grows again. */
                    vol_stamp_class(v, inode_id, INVFS_CLASS_UNCOMPRESSIBLE, 0,
                                    invfs_registry_generation());
                    return 0;
                }
                break;
            case INVFS_CLASS_GENERIC_GUARD:
                if (!cc || cc->generation <= cgen) return 0;
                /* WP12(b): a JXL retry lives behind the generic store
                 * now -- the RAW-gated branch can never see it again */
                if (calgo == INVFS_ALGO_JXL)
                    return vol_jxl_retry(v, inode_id, name);
                break;   /* new sub-encoder: retry via the full path */
            case INVFS_CLASS_GENERIC_MEMLIMIT:
                if (!cc || cc->dec_mem_bytes > vol_get_dec_mem_limit(v))
                    return 0;
                if (calgo == INVFS_ALGO_JXL)
                    return vol_jxl_retry(v, inode_id, name);
                break;   /* the policy now admits the codec: retry */
            default: {
                /* TEXT/BATCHED_BIN/CODEC/CONTAINER/GENERIC: policy
                 * compliance. The generic floor codecs (NONE/LZ4/ZSTD) are
                 * always compliant -- there is nothing cheaper left to
                 * downgrade INTO. (A BATCHED_BIN member stamped with the
                 * ZSTD_BCJ AST tag finds no registry entry -- the BCJ tag
                 * names a pipeline stage, not a codec -- and skips the
                 * codec-level checks entirely; the batch-size rule below is
                 * its compliance check.) */
                int violated = 0;
                if (cc && calgo != INVFS_ALGO_NONE &&
                    calgo != INVFS_ALGO_LZ4 && calgo != INVFS_ALGO_ZSTD) {
                    if (cc->dec_mem_bytes > vol_get_dec_mem_limit(v))
                        violated = 1;
                    if (!violated && (cc->caps & INVFS_CODEC_CAP_WHOLEFILE) &&
                        v->arc_budget) {
                        uint64_t fsz = 0;
                        vol_stat_full(v, name, NULL, &fsz, NULL);
                        if (fsz > v->arc_budget) violated = 1;
                    }
                }
                /* a batch that outgrew the cache budget un-batches: arc
                 * refuses entries over budget/2 (arc.c), so the policy
                 * guarantee is read-time, and the member re-stores generic.
                 * BATCHED_BIN (WP14a) obeys the same rule -- the batch
                 * payload's [4B usize] header is codec-independent, so
                 * tz_member_oversized reads zstd batches unchanged. */
                if (!violated && (ccls == INVFS_CLASS_TEXT ||
                                  ccls == INVFS_CLASS_BATCHED_BIN) &&
                    v->arc_budget &&
                    tz_member_oversized(v, inode_id, v->arc_budget / 2))
                    violated = 1;
                /* WP14a migration path: a GENERIC file (pre-WP14a that
                 * means per-segment ZSTD) whose head sniffs as an
                 * executable family re-enters the full path below and
                 * defers into the binary accumulator. ALWAYS on -- not
                 * generation-gated: binary batching shipped in the same
                 * build as this predicate, so a GENERIC stamp on a
                 * binary-family file can only predate it, and re-batching
                 * it is the upgrade the stamp exists to permit. (The
                 * UNCOMPRESSIBLE stamp stays generation-gated: such a file
                 * already proved the gain is not there.) */
                if (!violated && ccls == INVFS_CLASS_GENERIC &&
                    !strchr(name, '!')) {
                    uint8_t head[8192];
                    int got = vol_read_range(v, inode_id, 0, sizeof head,
                                             head);
                    if (got > 0 &&
                        invfs_binary_family(head, (size_t)got, name) > 0)
                        break;   /* -> full path: re-read + bz_defer */
                }
                if (!violated) return 0;
                {
                    /* the downgrade stamp names the batch PAYLOAD codec:
                     * BCJ is a pipeline stage with no registry entry, so a
                     * MEMLIMIT{ZSTD_BCJ} stamp could never re-arm (the
                     * predicate's by_algo lookup finds nothing). ZSTD is
                     * the codec the retry consults; it is always admitted,
                     * so the re-batch fires on the very next sweep and
                     * re-targets the CURRENT batch size. */
                    uint8_t dalgo = (ccls == INVFS_CLASS_BATCHED_BIN &&
                                     calgo == INVFS_ALGO_ZSTD_BCJ)
                                  ? (uint8_t)INVFS_ALGO_ZSTD : calgo;
                    return vol_class_downgrade(v, inode_id, name, dalgo) == 0
                           ? 6 : -1;
                }
            }
            }
        } else {
            int fz = vol_inode_first_zone(v, inode_id);
            if (fz != INVFS_ZONE_RAW) {
                /* WP14b (WP10 §12.7 v2): an extraction container's part
                 * ("name!partN") stored per-segment generic -- absent class
                 * stamp, BINARY zone, NONE/LZ4/ZSTD algos -- is a batching
                 * candidate: sniff the head, defer into the binary or text
                 * accumulator. Parts already in batches (zone TEXT) and
                 * whole-file blob siblings skip here as before. The flush
                 * re-validates from the live record. */
                if (fz == INVFS_ZONE_BINARY && strchr(name, '!') &&
                    part_generic_segments(v, inode_id)) {
                    uint8_t head[8192];
                    uint64_t fsz = 0;
                    int got = vol_read_range(v, inode_id, 0, sizeof head,
                                             head);
                    int bfam, tfam;
                    if (got > 0 &&
                        vol_stat_full(v, name, NULL, &fsz, NULL) == 0 &&
                        fsz) {
                        bfam = invfs_binary_family(head, (size_t)got, name);
                        if (bfam > 0 &&
                            bz_defer(v, inode_id, name, fsz,
                                     (uint32_t)bfam) == 0)
                            return 10;   /* part -> ZSTD batch */
                        tfam = invfs_text_family(name, head, (size_t)got);
                        if (tfam > 0) {
                            const invfs_codec *pc =
                                invfs_codec_by_algo(INVFS_ALGO_PPMD);
                            if (pc && pc->dec_mem_bytes >
                                      vol_get_dec_mem_limit(v)) {
                                vol_stamp_class(v, inode_id,
                                                INVFS_CLASS_GENERIC_MEMLIMIT,
                                                INVFS_ALGO_PPMD,
                                                pc->generation);
                            } else if (tz_defer(v, inode_id, name, fsz,
                                                (uint32_t)tfam) == 0) {
                                return 9;   /* part -> PPMd batch */
                            }
                        }
                    }
                }
                /* Cheap reject before the expensive read. vol_read_file
                   DECODES, so on an already-swept volume the old order paid
                   a full packMP3/cjxl/APE decode per file just to conclude
                   "nothing to do". Anything not in RAW has been swept; the
                   one caller that still needs the decoded bytes
                   (vol_sweep_file) re-checks the zone itself. (Legacy rule
                   for files with no class flag.) */
                return 0;
            }
        }
    }

    if (vol_read_file(v, inode_id, &full, &full_len) != 0 || full_len < 4) {
        free(full);
        return 0;
    }

    /* ZIP container: explode into AST children (keep original bytes) */
    if (full[0] == 'P' && full[1] == 'K' &&
        ((full[2] == 3 && full[3] == 4) || (full[2] == 5 && full[3] == 6))) {
        invfs_ast_child_entry *ch =
            (invfs_ast_child_entry *)calloc(MAX_AST_CHILDREN, sizeof(*ch));
        if (ch) {
            int n = vol_zip_parse_children(full, full_len, ch, MAX_AST_CHILDREN);
            if (n > 0) {
                uint64_t nino = vol_create_container_file(v, name, full, full_len,
                                                          ch, (size_t)n);
                if (nino) {
                    vol_delete_inode(v, inode_id, name);
                    vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                                    INVFS_ALGO_ZIPR, tz_codec_gen(INVFS_ALGO_ZIPR));
                    free(ch); free(full);
                    return 1;
                }
                /* recognized but refused: retry only on a new generation */
                vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                                INVFS_ALGO_ZIPR, tz_codec_gen(INVFS_ALGO_ZIPR));
            }
            free(ch);
        }
        free(full);
        return 0;
    }

    /* FLAC -> APE(PCM) + frame recipe (bit-exact) */
    if (full_len >= 4 && memcmp(full, "fLaC", 4) == 0) {
        snprintf(rname, sizeof rname, "%s!recipe", name);
        if (vol_find(v, rname) != 0) { free(full); return 0; }
        uint64_t nino = vol_create_flac_file(v, name, full, full_len);
        free(full);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_FLACR, tz_codec_gen(INVFS_ALGO_FLACR));
            return 0;
        }
        if (vol_delete_inode(v, inode_id, name) != 0) return -1;
        vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                        INVFS_ALGO_FLACR, tz_codec_gen(INVFS_ALGO_FLACR));
        return 2;   /* FLAC */
    }

    /* TAR -> members + IVFT recipe (bit-exact) */
    if (full_len >= 512 && full[0] != 0 && full[0] != 1 &&
        memcmp(full + 257, "ustar", 5) == 0) {
        snprintf(p0name, sizeof p0name, "%s!part0", name);
        if (vol_find(v, p0name) != 0) { free(full); return 0; }
        uint64_t nino = vol_create_tar_file(v, name, full, full_len);
        free(full);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_TARR, tz_codec_gen(INVFS_ALGO_TARR));
            return 0;
        }
        if (vol_delete_inode(v, inode_id, name) != 0) return -1;
        vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                        INVFS_ALGO_TARR, tz_codec_gen(INVFS_ALGO_TARR));
        defer_container_parts(v, name);   /* WP14b: batch parts this run */
        return 3;   /* TAR */
    }

    /* GZIP (tar.gz) -> members + IVGZ recipe (bit-exact deflate) */
    if (full_len >= 18 && full[0] != 0 && full[0] != 1 &&
        full[0] == 0x1F && full[1] == 0x8B) {
        snprintf(p0name, sizeof p0name, "%s!part0", name);
        if (vol_find(v, p0name) != 0) { free(full); return 0; }
        uint64_t nino = vol_create_gz_file(v, name, full, full_len);
        free(full);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_GZR, tz_codec_gen(INVFS_ALGO_GZR));
            return 0;
        }
        if (vol_delete_inode(v, inode_id, name) != 0) return -1;
        vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                        INVFS_ALGO_GZR, tz_codec_gen(INVFS_ALGO_GZR));
        defer_container_parts(v, name);   /* WP14b: batch parts this run */
        return 4;   /* GZIP */
    }

    /* PNG -> JXL lossless + IVPN recipe (bit-exact) */
    if (full_len >= 33 && memcmp(full, "\x89PNG\r\n\x1a\n", 8) == 0) {
        snprintf(jn, sizeof jn, "%s!jxl", name);
        if (vol_find(v, jn) != 0) { free(full); return 0; }
        /* WP10 §12.2: admission needs the DECODE working set, derivable from
         * IHDR without a trial decode: raw pixels ~= h * (1 + ceil(w*ch*bd/8))
         * (unfiltered rows + per-row filter byte). Beyond the limit the file
         * stays admissible to generic ZSTD but never to PNGR. */
        {
            uint32_t w = ((uint32_t)full[16] << 24) | ((uint32_t)full[17] << 16) |
                         ((uint32_t)full[18] << 8) | (uint32_t)full[19];
            uint32_t h = ((uint32_t)full[20] << 24) | ((uint32_t)full[21] << 16) |
                         ((uint32_t)full[22] << 8) | (uint32_t)full[23];
            unsigned bd = full[24], ct = full[25];
            static const uint8_t chans[7] = { 1, 0, 3, 1, 2, 0, 4 };
            unsigned ch = ct < sizeof chans ? chans[ct] : 0;
            if (w && h && ch) {
                uint64_t raw = (uint64_t)h *
                               (1 + (((uint64_t)w * ch * bd) + 7) / 8);
                if (raw > vol_get_dec_mem_limit(v)) {
                    vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                                    INVFS_ALGO_PNGR, tz_codec_gen(INVFS_ALGO_PNGR));
                    free(full);
                    return 0;
                }
            }
        }
        uint64_t nino = vol_create_png_file(v, name, full, full_len);
        free(full);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_PNGR, tz_codec_gen(INVFS_ALGO_PNGR));
            return 0;
        }
        if (vol_delete_inode(v, inode_id, name) != 0) return -1;
        vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                        INVFS_ALGO_PNGR, tz_codec_gen(INVFS_ALGO_PNGR));
        return 5;   /* PNG */
    }

    /* MP3 -> PMP (packMP3, bit-exact). Matches an ID3v2 tag or a bare
       frame sync; packMP3 re-checks the content itself and refuses
       MPEG-2/2.5 Layer III, which we detect by a missing blob rather
       than by exit code (it exits 0 on refusal). */
    if (full_len >= 4 &&
        ((full[0] == 'I' && full[1] == 'D' && full[2] == '3') ||
         (full[0] == 0xFF && (full[1] & 0xE0) == 0xE0))) {
        uint8_t *pmp = NULL;
        size_t pmp_len = 0;
        int prc = invfs_pmp_compress(full, full_len, &pmp, &pmp_len);
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[sweep] %s: PMP rc=%d pmp_len=%zu (mp3 %zu)\n",
                    name, prc, pmp_len, full_len);
        /* only if it pays: a refused MPEG-2 file leaves pmp NULL, and a
           rare expansion must not cost space either. Either way we fall
           through and the generic ZSTD-19 path still gets the file. */
        if (prc == 0 && pmp_len > 0 && pmp_len < full_len) {
            uint64_t nino = vol_create_pmp_file(v, name, pmp, pmp_len,
                                                (uint64_t)full_len);
            free(pmp); free(full);
            if (!nino) return 0;
            if (vol_delete_inode(v, inode_id, name) != 0) return -1;
            vol_stamp_class(v, nino, INVFS_CLASS_CODEC,
                            INVFS_ALGO_PMP, tz_codec_gen(INVFS_ALGO_PMP));
            return 8;   /* MP3 */
        }
        free(pmp);
        /* Refused (MPEG-2/2.5, or a rare expansion). The generic path below
         * still runs; the GUARD stamp survives it (inner re-reads the record
         * and its gain-stamp yields to GUARD), so a new packMP3 generation
         * is what re-arms this file. */
        vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                        INVFS_ALGO_PMP, tz_codec_gen(INVFS_ALGO_PMP));
    }

    /* WP13: codecpack codecs — the registry's dynamic EXTERNAL entries, the
     * only ones carrying encode/decode trampolines (builtin externals have
     * NULL fn pointers and their own branches above). First sniff hit wins;
     * a declined pack stamps and falls through to text/generic. */
    {
        size_t cn = 0, ci;
        const invfs_codec *all = invfs_codec_all(&cn);
        for (ci = 0; ci < cn; ci++) {
            const invfs_codec *pc = &all[ci];
            int prc;
            if (!(pc->caps & INVFS_CODEC_CAP_EXTERNAL) || !pc->encode ||
                !pc->decode || !pc->sniff)
                continue;
            if (pc->sniff(full, full_len, name) <= 0)
                continue;
            prc = vol_pack_sweep(v, inode_id, name, pc, full, full_len);
            if (prc == 1) { free(full); return 0; }   /* tool absent: defer */
            if (prc >= 100) { free(full); return prc; }   /* transcoded */
            break;   /* declined: stamps applied; text/generic still run */
        }
    }

    /* WP10 §4: every magic dispatch above declined -- classify text. Text
     * DEFERS into the sweep-run accumulator (sealed into shared PPMd batches
     * by vol_tz_flush at the end of the run); "!" sibling parts stay with
     * their container in v1 (WP10 §12.7). */
    if (!strchr(name, '!')) {
        int fam = invfs_text_family(name, full, full_len);
        if (fam > 0) {
            const invfs_codec *pc = invfs_codec_by_algo(INVFS_ALGO_PPMD);
            if (pc && pc->dec_mem_bytes > vol_get_dec_mem_limit(v)) {
                /* PPMd's model exceeds the decode-memory policy: store
                 * generic (below), stamped so a raised limit re-tries the
                 * text path without waiting for a generation bump. */
                vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                                INVFS_ALGO_PPMD, pc->generation);
            } else if (tz_defer(v, inode_id, name, full_len,
                                (uint32_t)fam) == 0) {
                free(full);
                return 9;   /* text -> PPMd batch (deferred to flush) */
            }
        }
    }

    /* WP14b M2: exe-as-container carving, BEFORE the binary-batch
     * deferral -- a carved exe is strictly better than a batched one (the
     * embedded media gets a real codec, the glue still gets ZSTD-19).
     * '!'-sibling parts are never carved (WP10 §12.7). A MEMLIMIT refusal
     * (rc 2) skips batching too: the retry must re-arm from generic
     * storage, not from inside a batch. */
    int exer_no_bz = 0;
    if (!strchr(name, '!') &&
        invfs_binary_family(full, full_len, name) > 0) {
        uint32_t nparts = 0;
        int erc = vol_exer_carve(v, inode_id, name, full, full_len, &nparts);
        if (erc == 1) {
            v->last_exer_parts = nparts;   /* invf-sweep reports the count */
            free(full);
            return 11;   /* exe media -> JXL container */
        }
        exer_no_bz = (erc == 2);
    }

    /* WP14a: not text either -- executable binaries (ELF/PE/Mach-O by
     * magic, >= 4 KB) defer into the BINARY accumulator and are sealed
     * into shared ZSTD batches (x86 members BCJ-prefiltered first) by the
     * same vol_tz_flush. No dec_mem gate on purpose: the batch payload
     * codec is ZSTD, the generic floor itself -- a policy tight enough to
     * reject it would reject the floor it falls back to, and the batch
     * unit is already bounded by arc_budget/2 at seal time. Same "!"
     * sibling exclusion as text. */
    if (!strchr(name, '!') && !exer_no_bz) {
        int bfam = invfs_binary_family(full, full_len, name);
        if (bfam > 0 &&
            bz_defer(v, inode_id, name, full_len, (uint32_t)bfam) == 0) {
            free(full);
            return 10;   /* binary -> ZSTD batch (deferred to flush) */
        }
    }

    free(full);

    /* generic: recompress RAW -> Shadow(ZSTD-19), JPEG -> JXL */
    {
        int rc = vol_sweep_file(v, inode_id);
        if (rc == 2) return 7;      /* JPEG -> JXL (lossless, bit-exact) */
        if (rc == 0) return 6;      /* swept to Shadow */
        if (rc < 0) return -1;      /* hard error */
        return 0;                   /* already in Shadow: nothing done */
    }
}

/* drain the pending list (daemon background): process each pending inode
   and unmark it. Called when the daemon holds the volume exclusively
   (no open handles). Returns number of processed inodes. */
int vol_sweep_pending(invfs_volume *v)
{
    size_t n = v->n_pending;
    int done = 0;
    while (n > 0) {
        uint64_t id = v->pending[0];
        /* find the current name for this inode (may have been deleted) */
        char nm[256];
        int found = 0;
        {
            uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
            uint64_t end = v->inode_area_pos;
            invfs_inode_rec rh;
            while (pos + sizeof(rh) <= end) {
                if (vol_read_raw(v, pos, &rh, sizeof(rh)) != 0) break;
                if (rh.magic != INODE_REC_MAGIC) {
                    if (rh.magic == TOMBSTONE_MAGIC) { pos += rh.rec_len + 4; continue; }
                    break;
                }
                if (rh.inode_id == id && rh.name_len < sizeof nm) {
                    memcpy(nm, rh.name, rh.name_len);
                    nm[rh.name_len] = 0;
                    found = 1;
                    break;
                }
                pos += rh.rec_len + 4;
            }
        }
        vol_unmark_pending(v, id);
        if (found) {
            int rc = vol_sweep_one(v, id, nm);
            if (rc != 0) done++;
        }
        n = v->n_pending;   /* re-read (list may shrink) */
    }
    /* seal the partial text batch the drain accumulated (WP10 §4) */
    vol_tz_flush(v);
    return done;
}

/*
 * Extract one container member (window) from an in-memory archive buffer.
 * member data is at ch->data_off, compressed with ch->method (0=stored,
 * 8=deflate). Returns 0 on success; *out malloc'd, caller frees.
 */
int vol_zip_extract_member(const uint8_t *z, size_t zlen,
                           const invfs_ast_child_entry *ch,
                           uint8_t **out, size_t *out_len)
{
    uint8_t *buf;
    if (ch->data_off == 0 || ch->usize == 0)
        return -1;
    if ((size_t)ch->data_off + ch->csize > zlen)
        return -1;
    if (ch->method == 0) {   /* stored */
        if (ch->csize != ch->usize)
            return -1;
        buf = (uint8_t *)malloc(ch->usize);
        if (!buf) return -1;
        memcpy(buf, z + ch->data_off, ch->usize);
        *out = buf; *out_len = ch->usize;
        return 0;
    }
    if (ch->method == 8) {   /* deflate */
        buf = (uint8_t *)malloc(ch->usize ? ch->usize : 1);
        if (!buf) return -1;
        {
            size_t got = tinfl_decompress_mem_to_mem(
                buf, ch->usize, z + ch->data_off, ch->csize,
                TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            if (got != ch->usize) { free(buf); return -1; }
        }
        *out = buf; *out_len = ch->usize;
        return 0;
    }
    return -1;  /* unsupported method */
}

/*
 * Read a file by name; if the name contains '!', it addresses a container
 * member (possibly nested: "a.zip!inner.zip!x.txt"). Members are extracted
 * on demand from the container's original bytes (1:1 container read-back).
 */
int vol_read_named(invfs_volume *v, const char *name, uint8_t **out, size_t *out_len)
{
    char buf[512], *comps[16];
    size_t ncomp = 0, total = 0;
    uint8_t *cur = NULL;
    size_t cur_len = 0;
    const char *p = name;
    uint64_t ino;
    int rc = -1;

    if (strlen(name) + 1 > sizeof buf)
        return -1;
    strcpy(buf, name);

    /* split on '!' */
    {
        char *q = buf;
        while (ncomp < 16 && *q) {
            comps[ncomp++] = q;
            q = strchr(q, '!');
            if (!q) break;
            *q = 0;
            q++;
        }
    }
    if (ncomp == 0) return -1;

    ino = vol_find(v, comps[0]);
    if (!ino) return -1;
    if (vol_read_file(v, ino, &cur, &cur_len) != 0) return -1;

    for (size_t i = 1; i < ncomp; i++) {
        invfs_ast_child_entry *ch = NULL;
        size_t nch = 0, j;
        uint8_t *next = NULL;
        size_t next_len = 0;
        ch = (invfs_ast_child_entry *)calloc(MAX_AST_CHILDREN, sizeof(*ch));
        if (!ch) { free(cur); return -1; }
        nch = (size_t)vol_zip_parse_children(cur, cur_len, ch, MAX_AST_CHILDREN);
        if ((int)nch <= 0) { free(ch); free(cur); return -1; }
        for (j = 0; j < nch; j++) {
            if (ch[j].name_len == strlen(comps[i]) &&
                strncmp(ch[j].name, comps[i], ch[j].name_len) == 0)
                break;
        }
        if (j == nch) { free(ch); free(cur); return -1; }
        if (vol_zip_extract_member(cur, cur_len, &ch[j], &next, &next_len) != 0) {
            free(ch); free(cur);
            return -1;
        }
        free(ch);
        free(cur);
        cur = next;
        cur_len = next_len;
        total += next_len;
    }
    *out = cur;
    *out_len = cur_len;
    return 0;
}

/* stat: get file size by name; returns 0 on success */
int vol_stat(invfs_volume *v, const char *name, uint64_t *size_out)
{
    uint64_t id, ctime;
    return vol_stat_full(v, name, &id, size_out, &ctime);
}

/* One index lookup + one header read: inode id, size and ctime together.
   The Dokan/FUSE layers used to keep their own shadow copy of the whole
   inode area and rebuild it after every close, which is O(area) per file
   operation; this replaces it. Returns 0 on success. */
int vol_stat_full(invfs_volume *v, const char *name, uint64_t *id_out,
                  uint64_t *size_out, uint64_t *ctime_out)
{
    const name_index_entry *e = idx_get(v, name, strlen(name));

    /* The old scan walked from the start of the area and returned -1 on the
       first record that was not an INOD, so a single deleted file made stat
       fail for every name stored after it. The index goes straight to the
       live record -- and carries size and ctime, so a stat costs no I/O at
       all. Both are copied from the record header the index was built from,
       and every writer updates them through idx_put. */
    if (!e) return -1;
    /* Every out-param is optional: a caller that only wants to know whether a
       name exists has no id or size to put anywhere, and passing NULL for the
       rest is the obvious way to ask that. Dereferencing unconditionally
       turned that question into a crash. */
    if (id_out)    *id_out    = e->inode_id;
    if (size_out)  *size_out  = e->size;
    if (ctime_out) *ctime_out = e->ctime;
    return 0;
}

/* live names in the index (files + directory anchors) */
uint64_t vol_name_count(invfs_volume *v) { return (uint64_t)v->ncount; }

/*
 * Range read: decode only the segments covering [offset, offset+len).
 * Returns bytes actually read (may be less near EOF), or -1 on error.
 */
/* Algos whose one segment IS the whole file: the blob decodes in a single
   piece, so there is no cheaper way to answer a window than to rebuild
   everything, and the result is worth keeping. LZ4 and ZSTD stay out on
   purpose -- they decode per 64 KB segment, which is already bounded, and
   caching them would spend the budget evicting the entries that cost a
   subprocess to produce. */
static int algo_is_whole_file(uint32_t algo)
{
    const invfs_codec *c;
    if (algo == INVFS_ALGO_FLACR || algo == INVFS_ALGO_TARR ||
        algo == INVFS_ALGO_GZR   || algo == INVFS_ALGO_PNGR ||
        algo == INVFS_ALGO_PMP   || algo == INVFS_ALGO_APE  ||
        algo == INVFS_ALGO_JXL   || algo == INVFS_ALGO_EXER)
        return 1;
    /* WP13: codecpack codecs declare WHOLEFILE in their manifest caps */
    c = invfs_codec_by_algo(algo);
    return c && (c->caps & INVFS_CODEC_CAP_WHOLEFILE) != 0;
}

int vol_read_range(invfs_volume *v, uint64_t inode_id, uint64_t offset,
                   size_t len, void *buf)
{
    uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    uint64_t end = v->inode_area_pos;
    invfs_ast_recipe_header ast_h;
    invfs_ast_block_entry *ents = NULL;
    uint8_t *rec = NULL;
    uint16_t i;
    size_t got = 0;

    {   /* jump straight to the record; 0 = not indexed, keep the full scan */
        uint64_t ip = idx_get_id(v, inode_id);
        if (ip >= pos && ip + sizeof(invfs_inode_rec) <= end) pos = ip;
    }

    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec rec_h;
        uint32_t crc_stored, crc_calc;
        if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &rec_h, sizeof(rec_h)) != 0)
            return -1;
        if (rec_h.magic != INODE_REC_MAGIC && rec_h.magic != TOMBSTONE_MAGIC) {
            return -1;
        }
        if (rec_h.magic == TOMBSTONE_MAGIC) { pos += rec_h.rec_len + 4; continue; }
        if (rec_h.inode_id != inode_id) { pos += rec_h.rec_len + 4; continue; }
        rec = (uint8_t *)malloc(rec_h.rec_len);
        if (!rec) return -1;
        if (io_seek(&v->io, pos) != 0 || io_read(&v->io, rec, rec_h.rec_len) != 0 ||
            io_read(&v->io, &crc_stored, 4) != 0) { free(rec); return -1; }
        crc_calc = invfs_crc32c(rec, rec_h.rec_len);
        if (crc_calc != crc_stored) { free(rec); return -1; }

        memcpy(&ast_h, rec + sizeof(invfs_inode_rec), sizeof(ast_h));
        ents = (invfs_ast_block_entry *)(rec + sizeof(invfs_inode_rec) + sizeof(ast_h));
        break;
    }
    if (!rec) { fprintf(stderr,"[rr] record not found id=%llu\n",
        (unsigned long long)inode_id); return -1; }
    if (ast_h.num_children > 0 ||
        (ast_h.num_blocks == 1 && algo_is_whole_file(ents[0].algo))) {
        /* Whole-file reconstruction, served out of the content cache.
         *
         * Two shapes land here. A container (num_children > 0) keeps the
         * original archive bytes and its members are windows into them, so a
         * window costs a full rebuild. A transcoded file is one segment whose
         * algo decodes in a single piece -- and for FLACR/TARR/GZR/PNGR there
         * is no segment path below at all: they fell through to the raw branch,
         * where the stored blob length never equals the reconstructed length,
         * so every ranged read of a swept file returned -1. That is why a
         * transcoded file was unreadable through a mount while invf-cat, which
         * goes through vol_read_file, still returned it byte for byte.
         *
         * Rebuilding per callback is what makes this need a cache rather than
         * just a fix: a mount asks in 64 KB pieces, so a sequential read of an
         * N-byte container did O(N^2) work, and a FLAC spawned MAC.exe once per
         * 64 KB. Copy the window out BEFORE handing the buffer to the cache --
         * arc_put either takes ownership or frees an oversized entry, and
         * after it returns the pointer is not ours to read. */
        const uint8_t *all = NULL;
        uint8_t *fresh = NULL;
        size_t all_len = 0, take = 0;
        free(rec);
        if (!arc_get(v->arc, inode_id, &all, &all_len)) {
            if (vol_read_file(v, inode_id, &fresh, &all_len) != 0) return -1;
            all = fresh;
        }
        if (offset < all_len) {
            take = (size_t)((offset + len > all_len) ? all_len - offset : len);
            memcpy(buf, all + offset, take);
        }
        if (fresh) arc_put(v->arc, inode_id, fresh, all_len);
        return (int)take;
    }
    if (offset >= ast_h.file_size) { free(rec); return 0; }

    for (i = 0; i < ast_h.num_blocks; i++) {
        const invfs_ast_block_entry *e = &ents[i];
        uint64_t seg_lo = e->file_offset;
        uint64_t seg_hi = e->file_offset + e->length;
        uint64_t req_lo = offset, req_hi = offset + len;
        uint64_t lo, hi;
        uint8_t tmp[SEGMENT_SIZE];
        /* A container part (vol_create_blob_file) is ONE segment holding the
         * whole part, and a part can far exceed the 64 KB segment size of
         * the regular path: decode those from the heap, not the stack
         * (WP14b reads part heads through here at sweep time). */
        uint8_t *segbuf = tmp;
        uint8_t *segheap = NULL;
        size_t want;

        if (seg_hi <= req_lo || seg_lo >= req_hi)
            continue;  /* no overlap */
        lo = (req_lo > seg_lo) ? req_lo : seg_lo;
        hi = (req_hi < seg_hi) ? req_hi : seg_hi;
        if (hi > ast_h.file_size) hi = ast_h.file_size;
        if (lo >= hi) continue;

        /* Batch member: slice of a shared PPMd/ZSTD(+BCJ) batch; heap +
         * pba-keyed ARC, never the stack tmp[] (batches reach 4 MB,
         * segments only 64 KB) */
        if (e->zone == INVFS_ZONE_TEXT && tz_batch_algo(e->algo)) {
            want = (size_t)(hi - lo);
            if (vol_read_text_slice(v, inode_id, e, lo - seg_lo,
                                    (uint8_t *)buf + (size_t)(lo - offset),
                                    want) != 0) {
                free(rec); return -1;
            }
            got += want;
            continue;
        }

        /* decode whole segment */
        {
            uint64_t pba = 0, plen = 0;
            uint32_t hdr, crc_hdr;
            uint8_t *blob;
            if (e->length > sizeof tmp) {
                segheap = (uint8_t *)malloc((size_t)e->length);
                if (!segheap) { free(rec); return -1; }
                segbuf = segheap;
            }
            if (vol_lookup_entry(v, inode_id, e->block_id, &pba, &plen) != 0) {
                free(segheap); free(rec); return -1;
            }
            {
                uint8_t hdrb[8];
                if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
                    io_read(&v->io, hdrb, 8) != 0) { free(segheap); free(rec); return -1; }
                memcpy(&hdr, hdrb, 4);
                memcpy(&crc_hdr, hdrb + 4, 4);
            }
            blob = (uint8_t *)malloc(hdr);
            if (!blob) { free(segheap); free(rec); return -1; }
            if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE + 8) != 0 ||
                io_read(&v->io, blob, hdr) != 0) { free(blob); free(segheap); free(rec); return -1; }
            /* deep protection: verify segment CRC32C */
            if (crc_hdr != 0 && invfs_crc32c(blob, hdr) != crc_hdr) {
                fprintf(stderr, "segment CRC mismatch: inode %llu seg %u\n",
                        (unsigned long long)inode_id, e->block_id);
                free(blob); free(segheap); free(rec); return -1;
            }

            if (e->algo == INVFS_ALGO_LZ4) {
                int got = LZ4_decompress_safe((const char *)blob, (char *)segbuf,
                                              (int)hdr, (int)e->length);
                if (got != (int)e->length) {
                    free(blob); free(segheap); free(rec); return -1; }
            } else if (e->algo == INVFS_ALGO_ZSTD) {
                size_t got = ZSTD_decompress(segbuf, e->length, blob, hdr);
                if (ZSTD_isError(got) || got != e->length) { free(blob); free(segheap); free(rec); return -1; }
            } else if (e->algo == INVFS_ALGO_JXL) {
                uint8_t *jpg = NULL;
                size_t jpg_len = 0;
                if (invfs_jxl_decompress(blob, hdr, &jpg, &jpg_len) != 0 ||
                    jpg_len != e->length) {
                    free(blob); free(segheap); free(rec); return -1;
                }
                want = (size_t)(hi - lo);
                memcpy((uint8_t *)buf + (size_t)(lo - offset), jpg + (size_t)(lo - seg_lo), want);
                got += want;
                free(jpg);
                free(blob);
                free(segheap);
                continue;   /* unreachable in practice: JXL blobs divert via
                             * algo_is_whole_file (single-segment records) --
                             * the continue matches APE/PMP for safety */
            } else if (e->algo == INVFS_ALGO_APE) {
                uint8_t *fl = NULL;
                size_t fl_len = 0;
                if (invfs_ape_decompress(blob, hdr, &fl, &fl_len) != 0 ||
                    fl_len < (size_t)(hi - lo) || (size_t)(lo - seg_lo) > fl_len) {
                    free(blob); free(segheap); free(rec); return -1;
                }
                want = (size_t)(hi - lo);
                memcpy((uint8_t *)buf + (size_t)(lo - offset),
                       fl + (size_t)(lo - seg_lo), want);
                got += want;
                free(fl);
                free(blob);
                free(segheap);
                continue;
            } else if (e->algo == INVFS_ALGO_PMP) {
                /* ranged read: decode the whole blob, hand back the window.
                   A media player seeking in an .mp3 hits this per read, and
                   packMP3 decodes at ~1.9 MB/s -- correct but slow. Worth a
                   cache if playback off the mount ever matters. */
                uint8_t *m = NULL;
                size_t m_len = 0;
                if (invfs_pmp_decompress(blob, hdr, &m, &m_len) != 0 ||
                    (size_t)(lo - seg_lo) > m_len ||
                    m_len - (size_t)(lo - seg_lo) < (size_t)(hi - lo)) {
                    free(m); free(blob); free(segheap); free(rec); return -1;
                }
                want = (size_t)(hi - lo);
                memcpy((uint8_t *)buf + (size_t)(lo - offset),
                       m + (size_t)(lo - seg_lo), want);
                got += want;
                free(m);
                free(blob);
                free(segheap);
                continue;
            } else {
                if (hdr != e->length) {
                    free(blob); free(segheap); free(rec); return -1; }
                memcpy(segbuf, blob, e->length);
            }
            free(blob);
        }

        want = (size_t)(hi - lo);
        memcpy((uint8_t *)buf + (size_t)(lo - offset), segbuf + (size_t)(lo - seg_lo), want);
        free(segheap);
        got += want;
    }
    free(rec);
    return (int)got;
}

/* Collect the block_ids of an inode's zone==TEXT AST entries (WP10 §7).
 * Those segments are shared PPMd batches owned by the internal "\x01tzb"
 * inode; the retiring inode holds only duplicate L2P mappings to them, so
 * the blocks must survive the retire. *out is malloc'd (NULL when 0). A
 * parse failure yields 0/NULL, which just disables the skip -- the same
 * behaviour the retire path always had for a record it cannot read. */
static size_t collect_text_lbas(invfs_volume *v, uint64_t inode_id,
                                uint32_t **out)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0;
    invfs_ast_recipe_header ah;
    const invfs_ast_block_entry *ents;
    uint32_t *ids = NULL;
    size_t n = 0, i;

    *out = NULL;
    if (meta_read_record_by_id(v, inode_id, &buf, &rl, NULL, 0, NULL) != 0)
        return 0;
    if (rl >= sizeof(invfs_inode_rec) + sizeof(ah)) {
        memcpy(&ah, buf + sizeof(invfs_inode_rec), sizeof(ah));
        if (rl >= sizeof(invfs_inode_rec) + sizeof(ah) +
                  (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
            ids = (uint32_t *)malloc(ah.num_blocks
                                     ? ah.num_blocks * sizeof(uint32_t) : 1);
            if (ids) {
                ents = (const invfs_ast_block_entry *)
                       (buf + sizeof(invfs_inode_rec) + sizeof(ah));
                for (i = 0; i < ah.num_blocks; i++)
                    if (ents[i].zone == INVFS_ZONE_TEXT)
                        ids[n++] = ents[i].block_id;
            }
        }
    }
    free(buf);
    *out = ids;
    return n;
}

/*
 * Delete a file: free its data blocks, unmap L2P, append tombstone.
 * Returns 0 on success, -1 if not found.
 */
/* delete the specific inode (NOT by name — safe for atomic sweeps:
 * create-new-first then delete-old; the new inode stays untouched). */
/* Retire an inode: tombstone it and drop its maps, with or without freeing
 * the blocks those maps name.
 *
 * `free_data = 0` exists for two callers that must NOT free:
 *
 *  - fsck's orphan reclaim. Dedupe remaps several inodes onto one canonical
 *    pba (sweep_dedupe), so "this inode's blocks" is not the same set as
 *    "blocks only this inode uses". Freeing directly would punch a hole in a
 *    live file that shares a deduplicated segment -- identical FLAC cover art
 *    across an album is exactly the case dedupe was built for. fsck instead
 *    rebuilds the bitmap from whatever records remain live, which frees
 *    precisely what nothing references any more.
 *  - rename, which hands the same blocks to a second inode id before
 *    retiring the first.
 *
 * Ordering note for the freeing case: the tombstone is written with io_write
 * while the bitmap is only dirtied in RAM, so the tombstone is durable before
 * the frees it authorizes. The reverse order would let a crash leave a live
 * record whose blocks are free and reusable.
 */
static int vol_retire_inode(invfs_volume *v, uint64_t inode_id,
                            const char *name, int free_data)
{
    uint64_t pos, end;
    size_t i;

    if (inode_id == 0)
        return -1;
    if (vol_mark_dirty(v) != 0)
        return -1;

    /* Give the cache's budget back. Not a safety measure: inode ids come from
       a monotonic counter and no path rewrites a record in place, so a stale
       entry could never be served for the wrong content -- it would just hold
       memory for a file that no longer exists. Dropping the ghost matters more
       than dropping the data: a ghost for an id that can never be inserted
       again is history the adaptation would learn nothing from. */
    arc_invalidate(v->arc, inode_id);

    /* free all blocks mapped to this inode.
     * L2P length is the PHYSICAL block count of each segment (written at
     * map time), so no header reads here — safe against stale/reallocated
     * blocks. */
    if (free_data) {
        /* WP10 §7 (anti-PB7): never free blocks a zone==TEXT AST entry
         * names. The member holds only an L2P dup into the shared batch,
         * which belongs to the hidden owner inode; freeing here would punch
         * a hole in every other member. GC reclaims dead batches. The dup
         * mappings themselves are dropped with the inode below, as usual. */
        uint32_t *text_lbas = NULL;
        size_t n_text = collect_text_lbas(v, inode_id, &text_lbas), t;
        for (i = 0; i < v->l2p_count; i++) {
            const invfs_l2p_entry *e = &v->l2p[i];
            if (e->type == INVFS_JRN_MAP && e->inode == inode_id) {
                uint64_t nblk = e->length;
                int shared = 0;
                for (t = 0; t < n_text; t++)
                    if (text_lbas[t] == e->lba) { shared = 1; break; }
                if (shared) continue;
                if (nblk == 0 || e->pba >= v->sb.total_blocks ||
                    nblk > v->sb.total_blocks - e->pba)
                    continue;  /* stale entry — never free out of bounds */
                vol_free_blocks(v, e->pba, nblk);
            }
        }
        free(text_lbas);
    }
    /* rewrite L2P in-memory: drop this inode's maps */
    {
        size_t w = 0;
        for (i = 0; i < v->l2p_count; i++) {
            if (v->l2p[i].inode == inode_id) {
                if (w < v->l2p_dirty) v->l2p_dirty = w;
                continue;
            }
            if (w != i) v->l2p[w] = v->l2p[i];
            w++;
        }
        v->l2p_count = w;
        if (v->l2p_dirty > v->l2p_count) v->l2p_dirty = v->l2p_count;
    }

    /* append tombstone record */
    pos = v->inode_area_pos;
    end = v->inode_area_end;
    if (pos + sizeof(invfs_inode_rec) + 4 > end)
        return -1;
    {
        invfs_inode_rec rec;
        uint32_t crc;
        memset(&rec, 0, sizeof(rec));
        rec.magic = TOMBSTONE_MAGIC;
    v->hot.tombstones++;
    v->hot.tombstones++;
        rec.rec_len = (uint32_t)sizeof(invfs_inode_rec);
        rec.inode_id = inode_id;
        rec.file_size = 0;
        rec_set_name(&rec, name);
        crc = invfs_crc32c(&rec, sizeof(rec));
        if (io_seek(&v->io, pos) != 0 ||
            io_write(&v->io, &rec, sizeof(rec)) != 0 ||
            io_write(&v->io, &crc, 4) != 0)
            return -1;
        v->inode_area_pos = pos + sizeof(rec) + 4;
        idx_del(v, name, strlen(name), inode_id);
    }
    return 0;
}

int vol_delete_inode(invfs_volume *v, uint64_t inode_id, const char *name)
{
    return vol_retire_inode(v, inode_id, name, 1);
}

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
static int vol_delete_siblings(invfs_volume *v, const char *name)
{
    char (*names)[256] = NULL;
    size_t n = 0, cap = 0, nlen = strlen(name), i;
    uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    uint64_t end = v->inode_area_pos;

    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec h;
        char nm[257];
        size_t nl;
        if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &h, sizeof h) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof(invfs_inode_rec) ||
            h.rec_len > INVFS_MAX_REC_LEN) break;
        pos += (uint64_t)h.rec_len + 4;
        if (h.magic != INODE_REC_MAGIC) continue;
        nl = h.name_len < 256 ? h.name_len : 256;
        memcpy(nm, h.name, nl);
        nm[nl] = 0;
        if (nl <= nlen + 1 || strncmp(nm, name, nlen) != 0 || nm[nlen] != '!')
            continue;
        if (vol_find(v, nm) == 0) continue;      /* already superseded */
        for (i = 0; i < n; i++)
            if (strcmp(names[i], nm) == 0) break;
        if (i < n) continue;                     /* older record, same name */
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            void *nn = realloc(names, ncap * sizeof(*names));
            if (!nn) break;                      /* purge what we gathered */
            names = (char (*)[256])nn;
            cap = ncap;
        }
        memcpy(names[n++], nm, nl + 1);
    }

    for (i = 0; i < n; i++)
        vol_delete_file(v, names[i]);
    free(names);
    return (int)n;
}

/* A transcode that gives up partway has already committed some of its
   children. Their names ("name!partN", "name!recipe", "name!coverN") stay live
   records that no pass can reach: sweep skips internal '!' names and fsck
   counts them as live files, so nothing will ever free them. Purge them on
   every abort -- including the "not smaller, keep the original" verdict, which
   for tar/gz is reached only after the parts are already down, so the cheapest
   possible outcome was silently the most expensive one. */
static uint64_t vol_transcode_abort(invfs_volume *v, const char *name)
{
    int n = vol_delete_siblings(v, name);
    if (n > 0 && getenv("INVFS_DEBUG"))
        fprintf(stderr, "[vol] %s: transcode aborted, purged %d orphan(s)\n",
                name, n);
    return 0;
}

/* ==================== format v2 metadata (INO2 ext block) ====================
 *
 * A v2 record is [base][AST blob]["INO2" ext][CRC32C]. The ext carries the
 * POSIX identity of the inode: type, mode, uid/gid, mtime/atime, nlink,
 * rdev, symlink target and xattr TLVs. Everything a Linux rootfs needs that
 * the v1 base record cannot hold.
 *
 * Rewrites NEVER change the inode id: the L2P keys stay valid, so metadata
 * updates are crash-safe without touching data blocks. The old version is
 * killed by appending a tombstone whose UNUSED file_size field holds the
 * byte offset of the exact INOD being retired ("position kill"). Legacy
 * tombstones leave file_size 0 and keep the old kill-by-id semantics, so
 * v1 volumes replay identically. Both records land in ONE io_write: a torn
 * tail fails the trailing CRC and the scan stops at the previous boundary.
 */

/* total AST blob length after the fixed header (recipe + children), or
 * (size_t)-1 if bounds are violated */
static size_t vol_ast_blob_len(const uint8_t *rec, size_t rec_len)
{
    invfs_ast_recipe_header h;
    size_t base = sizeof(invfs_inode_rec);
    size_t len;
    uint16_t i;
    const uint8_t *p, *end;
    if (rec_len < base + sizeof(h)) return (size_t)-1;
    memcpy(&h, rec + base, sizeof(h));
    len = sizeof(h) + (size_t)h.num_blocks * sizeof(invfs_ast_block_entry);
    if (len > rec_len - base) return (size_t)-1;
    p = rec + base + len;
    end = rec + rec_len;
    for (i = 0; i < h.num_children; i++) {
        uint16_t nl;
        if ((size_t)(end - p) < 2) return (size_t)-1;
        memcpy(&nl, p, 2); p += 2;
        /* wire child = [u16 nlen][name][u16 method][4x u32] */
        if (nl > MAX_AST_CHILD_NAME || (size_t)(end - p) < nl + 20)
            return (size_t)-1;
        p += nl + 20;
    }
    return (size_t)(p - (rec + base));
}

/* parse one xattr TLV at p; returns bytes consumed or 0 on corruption */
static size_t xattr_tlv_size(const uint8_t *p, size_t avail)
{
    uint16_t nl, vl;
    if (avail < 4) return 0;
    memcpy(&nl, p, 2);
    memcpy(&vl, p + 2 + nl, 2);
    if ((size_t)2 + nl + 2 + vl > avail || nl == 0) return 0;
    return (size_t)2 + nl + 2 + vl;
}

static int meta_parse_ext(const uint8_t *p, size_t n,
                          invfs_meta_pub *out,
                          uint8_t **xattrs_out, size_t *xlen_out)
{
    invfs_meta_ext_hdr h;
    if (n < sizeof(h)) return -1;
    memcpy(&h, p, sizeof(h));
    if (h.magic != INVFS_META_MAGIC || h.version != 2 ||
        h.ext_len > n || h.target_len >= sizeof(out->target))
        return -1;
    if ((size_t)sizeof(h) + h.target_len > h.ext_len) return -1;
    memset(out, 0, sizeof(*out));
    out->type = h.type;
    out->mode = h.mode;
    out->uid = h.uid;
    out->gid = h.gid;
    out->mtime = h.mtime;
    out->atime = h.atime;
    out->nlink = h.nlink;
    out->rdev = h.rdev;
    if (h.target_len) {
        memcpy(out->target, p + sizeof(h), h.target_len);
        out->target[h.target_len] = 0;
    } else {
        out->target[0] = 0;
    }
    if (xattrs_out) {
        *xattrs_out = (uint8_t *)(p + sizeof(h) + h.target_len);
        *xlen_out = h.ext_len - sizeof(h) - h.target_len;
    }
    return 0;
}

/* locate the ext inside a raw record; NULL when absent (v1 record) */
static const uint8_t *meta_locate_ext(const uint8_t *rec, size_t rec_len,
                                      size_t *ext_len_out)
{
    size_t ast = vol_ast_blob_len(rec, rec_len);
    size_t off;
    invfs_meta_ext_hdr h;
    if (ast == (size_t)-1) return NULL;
    off = sizeof(invfs_inode_rec) + ast;
    if (rec_len < off + sizeof(h)) return NULL;
    memcpy(&h, rec + off, sizeof(h));
    if (h.magic != INVFS_META_MAGIC || h.ext_len > rec_len - off)
        return NULL;
    *ext_len_out = h.ext_len;
    return rec + off;
}

/* read the latest live record for an inode id; returns malloc'd buffer and
 * optionally its name/position. Walks forward from the index hint so stale
 * hints degrade to a full-area scan instead of wrong answers. */
static int meta_read_record_by_id(invfs_volume *v, uint64_t inode_id,
                                  uint8_t **buf_out, uint32_t *rl_out,
                                  char *name_out, size_t name_cap,
                                  uint64_t *pos_out)
{
    uint64_t p, hint;

    hint = idx_get_id(v, inode_id);
    p = vol_inode_area_start(v);
    if (hint >= p && hint + sizeof(invfs_inode_rec) <= v->inode_area_pos)
        p = hint;
    /* vol_inode_next returns the NEXT scan position; the record it described
     * sits at p - rl - 4. Same pattern as vol_get_children. */
    while ((p = vol_inode_next(v, p, NULL, NULL, NULL, NULL, 0, rl_out)) != 0) {
        invfs_inode_rec rh;
        uint8_t *buf;
        if (vol_read_raw(v, p - *rl_out - 4, &rh, sizeof(rh)) != 0)
            break;
        if (rh.magic == TOMBSTONE_MAGIC) continue;
        if (rh.inode_id != inode_id) continue;
        buf = (uint8_t *)malloc(*rl_out);
        if (!buf) return -1;
        if (vol_read_raw(v, p - *rl_out - 4, buf, *rl_out) != 0) {
            free(buf);
            return -1;
        }
        if (name_out && name_cap) {
            size_t nl = rh.name_len < name_cap - 1 ? rh.name_len : name_cap - 1;
            memcpy(name_out, rh.name, nl);
            name_out[nl] = 0;
        }
        if (pos_out) *pos_out = p - *rl_out - 4;
        *buf_out = buf;
        return 0;
    }
    return -1;
}

static size_t meta_serialize(const invfs_meta_pub *m,
                             const uint8_t *xattrs, size_t xlen,
                             uint8_t *dst)
{
    invfs_meta_ext_hdr h;
    size_t tlen = m ? strlen(m->target) : 0;
    uint8_t *d = dst;
    memset(&h, 0, sizeof(h));
    h.magic = INVFS_META_MAGIC;
    h.version = 2;
    h.ext_len = (uint16_t)(sizeof(h) + tlen + xlen);
    if (m) {
        h.type = m->type;
        h.mode = m->mode;
        h.uid = m->uid;
        h.gid = m->gid;
        h.mtime = m->mtime;
        h.atime = m->atime;
        h.nlink = m->nlink;
        h.rdev = m->rdev;
        h.target_len = (uint16_t)tlen;
    }
    memcpy(d, &h, sizeof(h)); d += sizeof(h);
    if (tlen) { memcpy(d, m->target, tlen); d += tlen; }
    if (xlen) { memcpy(d, xattrs, xlen); d += xlen; }
    return (size_t)(d - dst);
}

static void meta_pub_from_hdr_defaults(invfs_meta_pub *m, uint8_t type)
{
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->mode = (type == INVFS_ITYP_DIR) ? 0755 :
              (type == INVFS_ITYP_LNK) ? 0777 : 0644;
    m->nlink = (type == INVFS_ITYP_DIR) ? 2 : 1;
}

int vol_get_meta(invfs_volume *v, uint64_t inode_id, invfs_meta_pub *out)
{
    uint8_t *buf = NULL;
    uint32_t rl;
    const uint8_t *ext;
    size_t elen = 0;
    if (!out) return -1;
    if (meta_read_record_by_id(v, inode_id, &buf, &rl, NULL, 0, NULL) != 0)
        return -1;
    ext = meta_locate_ext(buf, rl, &elen);
    free(buf);
    if (!ext) return -1;
    return meta_parse_ext(ext, elen, out, NULL, NULL);
}

/* append [INOD(same id, updated ext)][DELT(position-kill)] as one write */
static uint64_t meta_rewrite(invfs_volume *v, uint64_t inode_id,
                             const invfs_meta_pub *newpub,   /* NULL=keep */
                             const uint8_t *newx, size_t newxlen, /* NULL=keep */
                             uint64_t *new_pos_out)
{
    uint8_t *oldbuf = NULL, *nurec = NULL, *combo = NULL;
    uint32_t rl;
    char name[256];
    uint64_t old_pos = 0;
    invfs_inode_rec *oh, *nh;
    size_t ast, elen = 0, new_ext_size, nu_len, total, off;
    const uint8_t *extp;
    uint8_t *xattrs = NULL;
    size_t xlen = 0;
    invfs_meta_pub cur;
    invfs_inode_rec tomb;
    uint32_t crc_nu, crc_tb;

    if (meta_read_record_by_id(v, inode_id, &oldbuf, &rl, name, sizeof(name),
                               &old_pos) != 0)
        return 0;
    oh = (invfs_inode_rec *)oldbuf;
    ast = vol_ast_blob_len(oldbuf, rl);
    if (ast == (size_t)-1) { free(oldbuf); return 0; }

    extp = meta_locate_ext(oldbuf, rl, &elen);
    if (extp && meta_parse_ext(extp, elen, &cur, &xattrs, &xlen) != 0)
        extp = NULL;
    if (!extp) {
        meta_pub_from_hdr_defaults(&cur, oh->name_len &&
                                   name[oh->name_len - 1] == '/'
                                   ? INVFS_ITYP_DIR : INVFS_ITYP_REG);
        xattrs = NULL; xlen = 0;
    }

    {
        static const uint8_t no_x[1] = { 0 };
        if (!newpub) newpub = &cur;
        if (!newx) { newx = xattrs ? xattrs : no_x; newxlen = xattrs ? xlen : 0; }
        new_ext_size = sizeof(invfs_meta_ext_hdr) +
                       strlen(newpub->target) + newxlen;
        if (new_ext_size > INVFS_META_SLACK) { free(oldbuf); return 0; }
        nu_len = sizeof(invfs_inode_rec) + ast + new_ext_size;
        total = nu_len + 4 + sizeof(invfs_inode_rec) + 4;
        combo = (uint8_t *)calloc(1, total);
        if (!combo) { free(oldbuf); return 0; }

        nh = (invfs_inode_rec *)combo;
        memcpy(nh, oh, sizeof(*oh));
        nh->rec_len = (uint32_t)nu_len;   /* ext grows the record */
        memcpy(combo + sizeof(invfs_inode_rec),
               oldbuf + sizeof(invfs_inode_rec), ast);
        meta_serialize(newpub, newx, newxlen,
                       combo + sizeof(invfs_inode_rec) + ast);
        crc_nu = invfs_crc32c(combo, nu_len);
        memcpy(combo + nu_len, &crc_nu, 4);

        memset(&tomb, 0, sizeof(tomb));
        tomb.magic = TOMBSTONE_MAGIC;
    v->hot.tombstones++;
        tomb.rec_len = (uint32_t)sizeof(tomb);
        tomb.inode_id = inode_id;
        tomb.file_size = old_pos;          /* position kill (v2) */
        tomb.name_len = oh->name_len;
        memcpy(tomb.name, oh->name, sizeof(tomb.name));
        crc_tb = invfs_crc32c((uint8_t *)&tomb, sizeof(tomb));
        off = nu_len + 4;
        memcpy(combo + off, &tomb, sizeof(tomb));
        memcpy(combo + off + sizeof(tomb), &crc_tb, 4);
    }
    free(oldbuf);

    if (v->inode_area_pos + total > v->inode_area_end) { free(combo); return 0; }
    if (vol_mark_dirty(v) != 0) { free(combo); return 0; }
    {
        uint64_t rec_start = v->inode_area_pos;
        if (io_seek(&v->io, rec_start) != 0 ||
            io_write(&v->io, combo, total) != 0) {
            free(combo);
            return 0;
        }
        if (new_pos_out) *new_pos_out = rec_start;
        v->inode_area_pos = rec_start + total;
        idx_put(v, name, strlen(name), inode_id, rec_start,
                nh->file_size, nh->ctime);
        idx_put_id(v, inode_id, rec_start);
    }
    free(combo);
    return inode_id;
}

uint64_t vol_apply_meta(invfs_volume *v, const char *name,
                        const invfs_meta_pub *meta)
{
    uint64_t id;
    if (v->sb.vol_flags & VOLF_READONLY) return 0;
    id = vol_find(v, name);
    if (id == 0) return 0;
    return meta_rewrite(v, id, meta, NULL, 0, NULL);
}

uint64_t vol_create_symlink(invfs_volume *v, const char *name,
                            const char *target)
{
    invfs_meta_pub m;
    size_t tl;
    uint64_t nid;

    if (v->sb.vol_flags & VOLF_READONLY) return 0;
    if (name_too_long(name)) return 0;
    tl = strlen(target);
    if (tl == 0 || tl >= INVFS_META_TARGET_MAX) return 0;

    nid = vol_create_file(v, name, NULL, 0);
    if (nid == 0) return 0;
    meta_pub_from_hdr_defaults(&m, INVFS_ITYP_LNK);
    m.mode = 0777;
    memcpy(m.target, target, tl + 1);
    m.mtime = (int64_t)time(NULL);
    if (meta_rewrite(v, nid, &m, NULL, 0, NULL) == 0) {
        vol_delete_file(v, name);      /* roll back the empty placeholder */
        return 0;
    }
    return nid;
}

uint64_t vol_create_special(invfs_volume *v, const char *name,
                            uint8_t type, uint16_t mode, uint64_t rdev)
{
    invfs_meta_pub m;
    uint64_t nid;

    if (v->sb.vol_flags & VOLF_READONLY) return 0;
    if (name_too_long(name)) return 0;
    if (type != INVFS_ITYP_FIFO && type != INVFS_ITYP_SOCK &&
        type != INVFS_ITYP_CHR && type != INVFS_ITYP_BLK)
        return 0;

    nid = vol_create_file(v, name, NULL, 0);
    if (nid == 0) return 0;
    meta_pub_from_hdr_defaults(&m, type);
    m.mode = mode;
    m.rdev = rdev;
    m.mtime = (int64_t)time(NULL);
    if (meta_rewrite(v, nid, &m, NULL, 0, NULL) == 0) {
        vol_delete_file(v, name);
        return 0;
    }
    return nid;
}

/* ---- xattr TLV helpers ---- */

int vol_get_xattr(invfs_volume *v, uint64_t inode_id, const char *xn,
                  void *val, size_t *vlen)
{
    uint8_t *buf = NULL, *x = NULL;
    uint32_t rl;
    size_t xl = 0, nlen = strlen(xn), rem, got = 0;
    const uint8_t *p;
    if (!vlen) return -1;
    if (meta_read_record_by_id(v, inode_id, &buf, &rl, NULL, 0, NULL) != 0)
        return -1;
    if (!meta_locate_ext(buf, rl, &xl)) { free(buf); return -1; }
    /* re-walk via parse to get the TLV slice */
    {
        size_t ast = vol_ast_blob_len(buf, rl);
        const uint8_t *ext = buf + sizeof(invfs_inode_rec) + ast;
        if (meta_parse_ext(ext, xl, &(invfs_meta_pub){0}, &x, &xl) != 0) {
            free(buf);
            return -1;
        }
    }
    p = x; rem = xl;
    while (rem > 0) {
        size_t tsz = xattr_tlv_size(p, rem);
        uint16_t nl, vl;
        if (tsz == 0) break;
        memcpy(&nl, p, 2); memcpy(&vl, p + 2 + nl, 2);
        if (nl == nlen && memcmp(p + 2, xn, nlen) == 0) {
            got = vl;
            if (*vlen == 0) { *vlen = vl; free(buf); return 0; }
            if (*vlen < vl) { free(buf); return -2; }   /* ERANGE-ish */
            memcpy(val, p + 2 + nl + 2, vl);
            *vlen = vl;
            free(buf);
            return 0;
        }
        p += tsz; rem -= tsz;
    }
    free(buf);
    return -1;   /* ENODATA */
}

int vol_set_xattr(invfs_volume *v, uint64_t inode_id, const char *xn,
                  const void *val, size_t vlen)
{
    uint8_t *buf = NULL, *nb = NULL;
    uint32_t rl;
    size_t xl = 0, nlen = strlen(xn), rem;
    uint8_t *x = NULL;
    const uint8_t *p;
    size_t cap = INVFS_META_XATTR_MAX, used = 0, tsz;
    char name[256];
    int rc = -1;

    if (v->sb.vol_flags & VOLF_READONLY) return -1;
    if (nlen == 0 || nlen > 255) return -1;
    if (meta_read_record_by_id(v, inode_id, &buf, &rl, name, sizeof(name),
                               NULL) != 0)
        return -1;
    nb = (uint8_t *)calloc(1, cap);
    if (!nb) { free(buf); return -1; }

    if (meta_locate_ext(buf, rl, &xl)) {
        size_t ast = vol_ast_blob_len(buf, rl);
        const uint8_t *ext = buf + sizeof(invfs_inode_rec) + ast;
        if (meta_parse_ext(ext, xl, &(invfs_meta_pub){0}, &x, &xl) == 0) {
            /* copy existing TLVs except the one being replaced */
            p = x; rem = xl;
            while (rem > 0) {
                uint16_t nl;
                tsz = xattr_tlv_size(p, rem);
                if (tsz == 0) break;
                memcpy(&nl, p, 2);
                if (!(nl == nlen && memcmp(p + 2, xn, nlen) == 0)) {
                    if (used + tsz > cap) goto done;
                    memcpy(nb + used, p, tsz);
                    used += tsz;
                }
                p += tsz; rem -= tsz;
            }
        }
    }
    tsz = 2 + nlen + 2 + vlen;
    if (used + tsz > cap) { rc = -2; goto done; }
    {
        uint16_t nl = (uint16_t)nlen, vl = (uint16_t)vlen;
        memcpy(nb + used, &nl, 2);
        memcpy(nb + used + 2, xn, nlen);
        memcpy(nb + used + 2 + nlen, &vl, 2);
        if (vlen) memcpy(nb + used + 2 + nlen + 2, val, vlen);
    }
    used += tsz;
    rc = meta_rewrite(v, inode_id, NULL, nb, used, NULL) ? 0 : -1;
done:
    free(nb);
    free(buf);
    return rc;
}

int vol_remove_xattr(invfs_volume *v, uint64_t inode_id, const char *xn)
{
    uint8_t *buf = NULL, *nb = NULL;
    uint32_t rl;
    size_t xl = 0, nlen = strlen(xn), rem, used = 0, cap = INVFS_META_XATTR_MAX;
    uint8_t *x = NULL;
    const uint8_t *p;
    char name[256];
    int found = 0, rc;

    if (v->sb.vol_flags & VOLF_READONLY) return -1;
    if (meta_read_record_by_id(v, inode_id, &buf, &rl, name, sizeof(name),
                               NULL) != 0)
        return -1;
    if (!meta_locate_ext(buf, rl, &xl)) { free(buf); return -1; }
    nb = (uint8_t *)calloc(1, cap);
    if (!nb) { free(buf); return -1; }
    {
        size_t ast = vol_ast_blob_len(buf, rl);
        const uint8_t *ext = buf + sizeof(invfs_inode_rec) + ast;
        if (meta_parse_ext(ext, xl, &(invfs_meta_pub){0}, &x, &xl) != 0) {
            free(nb); free(buf); return -1;
        }
    }
    p = x; rem = xl;
    while (rem > 0) {
        uint16_t nl;
        size_t tsz = xattr_tlv_size(p, rem);
        if (tsz == 0) break;
        memcpy(&nl, p, 2);
        if (nl == nlen && memcmp(p + 2, xn, nlen) == 0) {
            found = 1;
        } else {
            if (used + tsz > cap) break;
            memcpy(nb + used, p, tsz);
            used += tsz;
        }
        p += tsz; rem -= tsz;
    }
    if (!found) { free(nb); free(buf); return -1; }
    rc = meta_rewrite(v, inode_id, NULL, nb, used, NULL) ? 0 : -1;
    free(nb);
    free(buf);
    return rc;
}

int vol_list_xattr(invfs_volume *v, uint64_t inode_id,
                   char *buf, size_t bcap)
{
    uint8_t *rb = NULL, *x = NULL;
    uint32_t rl;
    size_t xl = 0, rem, used = 0;
    const uint8_t *p;
    if (meta_read_record_by_id(v, inode_id, &rb, &rl, NULL, 0, NULL) != 0)
        return -1;
    if (!meta_locate_ext(rb, rl, &xl)) { free(rb); return 0; }  /* none */
    {
        size_t ast = vol_ast_blob_len(rb, rl);
        const uint8_t *ext = rb + sizeof(invfs_inode_rec) + ast;
        if (meta_parse_ext(ext, xl, &(invfs_meta_pub){0}, &x, &xl) != 0) {
            free(rb);
            return -1;
        }
    }
    p = x; rem = xl;
    while (rem > 0) {
        uint16_t nl;
        size_t tsz = xattr_tlv_size(p, rem);
        if (tsz == 0) break;
        memcpy(&nl, p, 2);
        if (buf) {
            if (used + nl + 1 > bcap) { free(rb); return -2; }
            memcpy(buf + used, p + 2, nl);
            buf[used + nl] = 0;
        }
        used += nl + 1;
        p += tsz; rem -= tsz;
    }
    free(rb);
    return (int)used;
}

/* ---- WP10 storage-class flag ("invfs.class" xattr, WP10 §2) ---- */

int vol_get_class(invfs_volume *v, uint64_t inode_id,
                  uint8_t *cls, uint8_t *algo, uint16_t *gen)
{
    invfs_class_tlv tlv;
    size_t vlen = sizeof(tlv);
    if (vol_get_xattr(v, inode_id, INVFS_XATTR_CLASS, &tlv, &vlen) != 0 ||
        vlen != sizeof(tlv))
        return 1;   /* absent (a malformed value reads as unclassified) */
    if (cls)  *cls  = tlv.cls;
    if (algo) *algo = tlv.algo;
    if (gen)  *gen  = tlv.gen;
    return 0;
}

int vol_stamp_class(invfs_volume *v, uint64_t inode_id,
                    uint8_t cls, uint8_t algo, uint16_t gen)
{
    invfs_class_tlv cur, want;
    size_t vlen = sizeof(cur);

    /* check-then-write: an unchanged stamp would still cost a meta_rewrite
     * (record append + position-kill tombstone) per file per sweep */
    if (vol_get_xattr(v, inode_id, INVFS_XATTR_CLASS, &cur, &vlen) == 0 &&
        vlen == sizeof(cur) &&
        cur.cls == cls && cur.algo == algo && cur.gen == gen)
        return 1;   /* unchanged */
    want.cls  = cls;
    want.algo = algo;
    want.gen  = gen;
    if (vol_set_xattr(v, inode_id, INVFS_XATTR_CLASS,
                      &want, sizeof(want)) != 0)
        return -1;
    return 0;
}

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
#define TZ_BATCH_MAX  (4ull << 20)    /* user-confirmed PPMd optimum (§3) */

/* one slice of one member file, inside one batch */
typedef struct {
    uint64_t batch_seq;
    uint64_t file_off;
    uint32_t batch_off;
    uint32_t len;
} tz_slice;

/* per-candidate build state while a flush runs */
typedef struct {
    tz_slice *slices;
    size_t n_slices, cap_slices;
    uint64_t file_size;      /* authoritative size when fed (fresh read) */
    uint64_t new_id;         /* allocated at commit (0 = not yet) */
    uint32_t algo;           /* batch algo its slices carry (PPMD / ZSTD /
                              * ZSTD_BCJ) -- WP14a */
    int bcj;                 /* member is x86-BCJ-prefiltered per slice */
    int complete;            /* all bytes of the candidate fed */
    int fallback;            /* its batch failed -> generic per-file path */
    int committed;           /* member record rewritten */
} tz_member;

/* a batch that made it to disk during this flush */
typedef struct {
    uint64_t seq, pba;
    uint32_t phys;
    uint32_t algo;           /* the batch payload codec tag (WP14a) */
} tz_sealed;

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

/* ---- accumulator (deferral side) ---- */

/* one accumulator is a (array, n, cap) triple on the volume: v->tz for
 * text candidates (WP10), v->bz for binary ones (WP14a). Same growth
 * and dedup rules for both. */
static int acc_defer(tz_candidate **arr, size_t *np, size_t *capp,
                     uint64_t inode_id, const char *name,
                     uint64_t size, uint32_t family)
{
    size_t i, nl = strlen(name);
    tz_candidate *nt;
    if (nl == 0 || nl > INVFS_MAX_NAME) return -1;
    for (i = 0; i < *np; i++)
        if ((*arr)[i].inode_id == inode_id) return 0;   /* already deferred */
    if (*np == *capp) {
        size_t nc = *capp ? *capp * 2 : 64;
        nt = (tz_candidate *)realloc(*arr, nc * sizeof(tz_candidate));
        if (!nt) return -1;
        *arr = nt;
        *capp = nc;
    }
    nt = &(*arr)[(*np)++];
    nt->inode_id = inode_id;
    nt->size = size;
    nt->family = family;
    memcpy(nt->name, name, nl + 1);
    return 0;
}

static int tz_defer(invfs_volume *v, uint64_t inode_id, const char *name,
                    uint64_t size, uint32_t family)
{
    return acc_defer(&v->tz, &v->tz_n, &v->tz_cap, inode_id, name, size,
                     family);
}

/* WP14a: defer an executable (binary-family) file into the binary
 * accumulator; vol_tz_flush seals it into shared ZSTD(+BCJ) batches. */
static int bz_defer(invfs_volume *v, uint64_t inode_id, const char *name,
                    uint64_t size, uint32_t family)
{
    return acc_defer(&v->bz, &v->bz_n, &v->bz_cap, inode_id, name, size,
                     family);
}

/* ---- owner inode ---- */

/* Load the owner record: entries, ext, position. Absent owner => *o zeroed
 * and o->pos = 0 (caller creates it). Returns 0 on success. */
static int tz_owner_load(invfs_volume *v, uint64_t owner, tz_owner *o)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0;
    invfs_ast_recipe_header ah;
    const invfs_ast_block_entry *ents;
    size_t base = sizeof(invfs_inode_rec);
    size_t elen = 0;
    uint32_t i;

    memset(o, 0, sizeof *o);
    if (meta_read_record_by_id(v, owner, &buf, &rl, NULL, 0, &o->pos) != 0 ||
        !o->pos) {
        free(buf);
        return -1;
    }
    if (rl < base + sizeof(ah)) { free(buf); return -1; }
    memcpy(&ah, buf + base, sizeof(ah));
    if (rl < base + sizeof(ah) +
             (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
        free(buf);
        return -1;
    }
    o->ctime = ((const invfs_inode_rec *)buf)->ctime;
    if (ah.num_blocks) {
        o->ents = (invfs_ast_block_entry *)malloc((size_t)ah.num_blocks *
                                                  sizeof(*o->ents));
        if (!o->ents) { free(buf); return -1; }
        ents = (const invfs_ast_block_entry *)(buf + base + sizeof(ah));
        for (i = 0; i < ah.num_blocks; i++) {
            o->ents[i] = ents[i];
            /* seq assignment is strictly monotone even after GC removed
             * middle entries: max surviving block_id + 1 */
            if ((uint64_t)ents[i].block_id + 1 > o->next_seq)
                o->next_seq = (uint64_t)ents[i].block_id + 1;
        }
        o->n = ah.num_blocks;
        o->cap = ah.num_blocks;
    }
    {
        const uint8_t *ext = meta_locate_ext(buf, rl, &elen);
        if (ext && elen && elen <= 0xFFFF) {
            o->ext = (uint8_t *)malloc(elen);
            if (o->ext) { memcpy(o->ext, ext, elen); o->ext_len = (uint32_t)elen; }
        }
    }
    free(buf);
    return 0;
}

static void tz_owner_free(tz_owner *o)
{
    free(o->ents);
    free(o->ext);
    memset(o, 0, sizeof *o);
}

/* Append [INOD(owner, entries)][DELT(position-kill previous)] as one write.
 * The owner keeps its inode id so the owner L2P maps stay valid. Entry
 * file_offsets are rebuilt as the cumulative concatenation of the given
 * entries (so the owner itself stays readable as the concatenation of its
 * live batches, and GC repacks the same way). */
static int tz_owner_write(invfs_volume *v, uint64_t owner, tz_owner *o)
{
    size_t rec_len, total, off;
    uint8_t *combo;
    invfs_inode_rec *rh, tomb;
    invfs_ast_recipe_header ah;
    uint32_t crc_rec, crc_tomb;
    uint64_t run = 0, new_pos;
    uint32_t i;

    for (i = 0; i < o->n; i++) {
        o->ents[i].file_offset = run;
        run += o->ents[i].length;
    }
    if (run > 0xFFFFFFFFu) return -1;   /* ast_h.file_size is u32 */

    rec_len = sizeof(invfs_inode_rec) + sizeof(ah) +
              (size_t)o->n * sizeof(invfs_ast_block_entry) + o->ext_len;
    total = rec_len + 4 + (o->pos ? sizeof(tomb) + 4 : 0);
    combo = (uint8_t *)calloc(1, total);
    if (!combo) return -1;

    memset(&ah, 0, sizeof ah);
    ah.version = 1;
    ah.file_size = (uint32_t)run;
    ah.num_blocks = (uint16_t)o->n;

    rh = (invfs_inode_rec *)combo;
    rh->magic = INODE_REC_MAGIC;
    rh->rec_len = (uint32_t)rec_len;
    rh->inode_id = owner;
    rh->file_size = run;
    rh->ctime = o->ctime;
    rec_set_name(rh, TZ_OWNER_NAME);
    memcpy(combo + sizeof(invfs_inode_rec), &ah, sizeof ah);
    if (o->n)
        memcpy(combo + sizeof(invfs_inode_rec) + sizeof(ah), o->ents,
               (size_t)o->n * sizeof(invfs_ast_block_entry));
    if (o->ext_len)
        memcpy(combo + rec_len - o->ext_len, o->ext, o->ext_len);
    crc_rec = invfs_crc32c(combo, rec_len);
    memcpy(combo + rec_len, &crc_rec, 4);

    off = rec_len + 4;
    if (o->pos) {
        memset(&tomb, 0, sizeof tomb);
        tomb.magic = TOMBSTONE_MAGIC;
        v->hot.tombstones++;
        tomb.rec_len = (uint32_t)sizeof(tomb);
        tomb.inode_id = owner;
        tomb.file_size = o->pos;        /* v2 position kill */
        tomb.name_len = (uint32_t)(sizeof(TZ_OWNER_NAME) - 1);
        memcpy(tomb.name, TZ_OWNER_NAME, sizeof(TZ_OWNER_NAME) - 1);
        crc_tomb = invfs_crc32c((uint8_t *)&tomb, sizeof tomb);
        memcpy(combo + off, &tomb, sizeof tomb);
        memcpy(combo + off + sizeof tomb, &crc_tomb, 4);
    }

    if (v->inode_area_pos + total > v->inode_area_end) { free(combo); return -1; }
    if (vol_mark_dirty(v) != 0) { free(combo); return -1; }
    new_pos = v->inode_area_pos;
    if (io_seek(&v->io, new_pos) != 0 ||
        io_write(&v->io, combo, total) != 0) { free(combo); return -1; }
    v->inode_area_pos = new_pos + total;
    free(combo);
    idx_put(v, TZ_OWNER_NAME, sizeof(TZ_OWNER_NAME) - 1, owner, new_pos,
            run, o->ctime);
    idx_put_id(v, owner, new_pos);
    o->pos = new_pos;
    return 0;
}

/* Find the batch owner, creating it lazily (empty record) on first use. */
static uint64_t tz_owner_id(invfs_volume *v)
{
    uint64_t id = vol_find(v, TZ_OWNER_NAME);
    if (id) return id;
    return vol_create_file(v, TZ_OWNER_NAME, NULL, 0);
}

/* ---- small policy helpers used by the walk predicate ---- */

/* Cheap head sniff across the registry (UNCOMPRESSIBLE retry gate): one
 * 8 KB window, registry order (= sniff priority), any positive answer
 * qualifies. */
static int tz_sniff_any(invfs_volume *v, uint64_t inode_id, const char *name)
{
    uint8_t head[8192];
    int got = vol_read_range(v, inode_id, 0, sizeof head, head);
    size_t n = 0, i;
    const invfs_codec *all;
    if (got <= 0) return 0;
    all = invfs_codec_all(&n);
    for (i = 0; i < n; i++)
        if (all[i].sniff && all[i].sniff(head, (size_t)got, name) > 0)
            return 1;
    return 0;
}

/* Read the usize of a TEXT member's batch segment without decoding it
 * (WP10 §6: "store decoded unit sizes to speed this up" -- the [4B usize]
 * sits right behind the 8-byte framing). 1 = batch exceeds unit_limit. */
static int tz_member_oversized(invfs_volume *v, uint64_t inode_id,
                               uint64_t unit_limit)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0;
    invfs_ast_recipe_header ah;
    const invfs_ast_block_entry *ents;
    size_t base = sizeof(invfs_inode_rec);
    uint16_t i;
    int over = 0;

    if (meta_read_record_by_id(v, inode_id, &buf, &rl, NULL, 0, NULL) != 0)
        return 0;   /* unreadable: not GC/policy business here */
    if (rl < base + sizeof(ah)) { free(buf); return 0; }
    memcpy(&ah, buf + base, sizeof(ah));
    if (rl < base + sizeof(ah) +
             (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
        free(buf);
        return 0;
    }
    ents = (const invfs_ast_block_entry *)(buf + base + sizeof(ah));
    for (i = 0; i < ah.num_blocks && !over; i++) {
        uint64_t pba = 0, plen = 0;
        uint8_t hb[12];
        uint32_t usize;
        if (ents[i].zone != INVFS_ZONE_TEXT) continue;
        if (vol_lookup_entry(v, inode_id, ents[i].block_id, &pba, &plen) != 0 ||
            !pba) continue;
        if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
            io_read(&v->io, hb, sizeof hb) != 0) continue;
        memcpy(&usize, hb + 8, 4);
        if (usize > unit_limit) over = 1;
    }
    free(buf);
    return over;
}

/* Policy downgrade (WP10 §6, both directions): decode and re-store through
 * the generic per-segment path, then stamp GENERIC_MEMLIMIT{codec} -- NOT
 * bare GENERIC, because the limit is the reason, and MEMLIMIT is what makes
 * the re-sweep after a RAISED limit find the file again (§2 table). The old
 * record's blocks retire as usual; TEXT members' batch blocks survive via
 * the retire gate and their L2P dups vanish with the old record (the batch
 * keeps a hole -- GC/compactor territory). */
static int vol_class_downgrade(invfs_volume *v, uint64_t inode_id,
                               const char *name, uint8_t calgo)
{
    uint8_t *data = NULL;
    size_t len = 0;
    invfs_meta_pub keep;
    int have_keep;
    uint64_t nid;

    have_keep = vol_get_meta(v, inode_id, &keep) == 0;
    if (vol_read_file(v, inode_id, &data, &len) != 0)
        return -1;
    nid = vol_replace_file(v, name, data, len);   /* fresh RAW record */
    free(data);
    if (!nid) return -1;
    if (have_keep) vol_apply_meta(v, name, &keep);
    /* generic_only: a downgraded JPEG must not loop straight back into JXL */
    if (vol_sweep_file_inner(v, nid, 1) < 0) return -1;
    /* the inner sweep re-ids the file (append-only store): stamp the LIVE
     * record, never the intermediate -- stamping the deleted id would
     * resurrect it and hijack the name */
    {
        uint64_t live = vol_find(v, name);
        vol_stamp_class(v, live ? live : nid, INVFS_CLASS_GENERIC_MEMLIMIT,
                        calgo, tz_codec_gen(calgo));
    }
    return 0;
}

/* ---- flush: seal batches, commit members ---- */

typedef struct {
    invfs_volume *v;
    tz_owner owner;
    uint64_t owner_id;
    uint64_t next_seq;
    /* the batch currently being filled */
    uint8_t *bbuf;
    size_t blen, bcap;          /* bcap = batch target */
    uint64_t open_seq;
    /* WP14a: 0 = text flush (PPMd), 1 = binary flush (ZSTD[+BCJ]).
     * open_algo/open_bcj describe the batch currently being filled:
     * the first member of a batch sets the tone, and a member of the
     * other BCJ kind seals the open batch first (binary batches are
     * prefilter-homogeneous -- the (family,size) sort makes BCJ families
     * adjacent, so this split only ever fires at family boundaries). */
    int binary;
    uint32_t open_algo;         /* PPMD / ZSTD / ZSTD_BCJ */
    int open_bcj;
    /* sealed batches of this flush (for member-commit pba lookup) */
    tz_sealed *sealed;
    size_t n_sealed, cap_sealed;
    int sealed_any;
    int skip_commit;            /* INVFS_TZ_SKIP_COMMIT fault-injection hook */
} tz_ctx;

static int tz_slice_push(tz_member *m, uint64_t seq, uint64_t file_off,
                         uint32_t batch_off, uint32_t len)
{
    if (m->n_slices == m->cap_slices) {
        size_t nc = m->cap_slices ? m->cap_slices * 2 : 4;
        tz_slice *ns = (tz_slice *)realloc(m->slices, nc * sizeof(tz_slice));
        if (!ns) return -1;
        m->slices = ns;
        m->cap_slices = nc;
    }
    m->slices[m->n_slices].batch_seq = seq;
    m->slices[m->n_slices].file_off = file_off;
    m->slices[m->n_slices].batch_off = batch_off;
    m->slices[m->n_slices].len = len;
    m->n_slices++;
    return 0;
}

static const tz_sealed *tz_sealed_find(const tz_ctx *c, uint64_t seq)
{
    size_t i;
    for (i = 0; i < c->n_sealed; i++)
        if (c->sealed[i].seq == seq) return &c->sealed[i];
    return NULL;
}

/* Rewrite one member's record under its pre-allocated new id (dup maps are
 * already written and flushed by tz_commit_ready) with TEXT-zone entries,
 * carrying the old record's ext (INO2 + xattrs) verbatim, then retire the
 * old id (its RAW blocks free normally -- the TEXT gate only covers batches)
 * and stamp the batching class (TEXT for PPMd batches, BATCHED_BIN for
 * WP14a binary batches). The entry algo comes from the sealed batch the
 * slice landed in: PPMD for text, ZSTD or ZSTD_BCJ for binary. */
static int tz_commit_member(tz_ctx *c, tz_member *m, const tz_candidate *cand)
{
    invfs_volume *v = c->v;
    uint8_t *old = NULL;
    uint32_t orl = 0;
    uint64_t old_pos = 0;
    char name[256];
    const uint8_t *ext;
    size_t ext_len = 0;
    uint64_t new_id, fsize, ctime;
    size_t rec_size, i;
    uint8_t *rec;
    invfs_inode_rec *rh;
    invfs_ast_recipe_header ah;
    invfs_ast_block_entry *ne;
    uint32_t crc;
    int rc = -1;

    if (meta_read_record_by_id(v, cand->inode_id, &old, &orl, name,
                               sizeof name, &old_pos) != 0)
        return -1;
    ext = meta_locate_ext(old, orl, &ext_len);   /* may be NULL (v1 record) */
    ctime = ((const invfs_inode_rec *)old)->ctime;
    fsize = m->file_size;

    new_id = m->new_id;
    rec_size = sizeof(invfs_inode_rec) + sizeof(ah) +
               m->n_slices * sizeof(invfs_ast_block_entry) + ext_len;
    rec = (uint8_t *)calloc(1, rec_size);
    if (!rec) { free(old); return -1; }

    memset(&ah, 0, sizeof ah);
    ah.version = 1;
    ah.file_size = (uint32_t)fsize;
    ah.num_blocks = (uint16_t)m->n_slices;

    rh = (invfs_inode_rec *)rec;
    rh->magic = INODE_REC_MAGIC;
    rh->rec_len = (uint32_t)rec_size;
    rh->inode_id = new_id;
    rh->file_size = fsize;
    rh->ctime = ctime;
    rec_set_name(rh, name);
    memcpy(rec + sizeof(invfs_inode_rec), &ah, sizeof ah);
    ne = (invfs_ast_block_entry *)(rec + sizeof(invfs_inode_rec) + sizeof ah);
    for (i = 0; i < m->n_slices; i++) {
        const tz_sealed *s = tz_sealed_find(c, m->slices[i].batch_seq);
        if (!s) goto out;   /* can only be a bug in the flush */
        memset(&ne[i], 0, sizeof ne[i]);
        ne[i].file_offset = m->slices[i].file_off;
        ne[i].length = m->slices[i].len;
        ne[i].zone = INVFS_ZONE_TEXT;      /* TEXT zone == "batched" (WP14a) */
        ne[i].algo = s->algo;              /* PPMD / ZSTD / ZSTD_BCJ */
        ne[i].block_id = (uint32_t)m->slices[i].batch_seq;
        ne[i].block_offset = m->slices[i].batch_off;
    }
    if (ext_len)
        memcpy(rec + rec_size - ext_len, ext, ext_len);
    crc = invfs_crc32c(rec, rec_size);

    /* room for the record AND the tombstone the retire appends */
    if (v->inode_area_pos + rec_size + 4 + sizeof(invfs_inode_rec) + 4 >
        v->inode_area_end)
        goto out;
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, rec, rec_size) != 0 ||
        io_write(&v->io, &crc, 4) != 0)
        goto out;
    idx_put(v, name, strlen(name), new_id, v->inode_area_pos, fsize, ctime);
    idx_put_id(v, new_id, v->inode_area_pos);
    v->inode_area_pos += rec_size + 4;

    /* the retire frees the old RAW/generic blocks; the record had no TEXT
     * entries, so the batch gate does not engage */
    if (vol_delete_inode(v, cand->inode_id, name) != 0)
        fprintf(stderr, "tz: %s committed but the old record survived; "
                        "invf-fsck -f will reclaim it\n", name);
    if (c->binary)
        vol_stamp_class(v, new_id, INVFS_CLASS_BATCHED_BIN,
                        (uint8_t)m->algo, invfs_registry_generation());
    else
        vol_stamp_class(v, new_id, INVFS_CLASS_TEXT, INVFS_ALGO_PPMD,
                        invfs_registry_generation());
    rc = 0;
out:
    free(rec);
    free(old);
    return rc;
}

/* Commit every complete, non-fallback member whose slices all live in
 * sealed batches. Two phases: allocate new ids + write the L2P dup maps
 * for ALL ready members, ONE journal flush so the dups are durable, then
 * the record rewrites (crash ordering, WP10 §4: batch segments and owner
 * maps are already durable from the seal). */
static int tz_commit_ready(tz_ctx *c, const tz_candidate *cands,
                           tz_member *members, size_t n)
{
    invfs_volume *v = c->v;
    size_t i, j;
    int any = 0, rc = 0;

    if (c->skip_commit) return 0;   /* crash-safety fault injection */
    for (i = 0; i < n; i++) {
        tz_member *m = &members[i];
        if (!m->complete || m->fallback || m->committed || m->new_id ||
            !m->n_slices)
            continue;
        for (j = 0; j < m->n_slices; j++)
            if (!tz_sealed_find(c, m->slices[j].batch_seq))
                break;
        if (j < m->n_slices) continue;   /* still feeding an open batch */
        m->new_id = v->next_inode_id++;
        for (j = 0; j < m->n_slices; j++) {
            const tz_sealed *s = tz_sealed_find(c, m->slices[j].batch_seq);
            if (vol_map(v, m->new_id, m->slices[j].batch_seq, s->pba,
                        s->phys) != 0)
                return -1;
        }
        any = 1;
    }
    if (any && vol_pre_record(v) != 0)
        return -1;
    for (i = 0; i < n; i++) {
        tz_member *m = &members[i];
        if (!m->new_id || m->committed) continue;
        if (tz_commit_member(c, m, &cands[i]) != 0) {
            fprintf(stderr, "tz: commit failed for %s\n", cands[i].name);
            rc = -1;   /* the old record is untouched; the member stays RAW */
        }
        m->committed = 1;
    }
    return rc;
}

/* cumulative decoded bytes the owner covers (= its readable size) */
static uint64_t tz_owner_total(const tz_owner *o)
{
    uint64_t t = 0;
    uint32_t i;
    for (i = 0; i < o->n; i++) t += o->ents[i].length;
    return t;
}

/* Seal the open batch: encode (PPMd for text, ZSTD-19 for binary -- the
 * registry's zstd entry, mirroring the generic sweep level), MANDATORY
 * decode+memcmp verify (doc/06 invariant), size guard, write the segment
 * in the shadow zone, map it under the owner, flush the journal, append
 * the owner AST entry. On ANY refusal (encode/verify/guard/ENOSPC) the
 * batch is dropped unwritten and every member with a slice in it falls
 * back to the generic per-file path -- the same "not smaller, keep the
 * original" answer every other path gives.
 * Returns 0 on seal/fallback, -1 on hard error after the segment landed. */
static int tz_seal(tz_ctx *c, const tz_candidate *cands,
                   tz_member *members, size_t n_members)
{
    invfs_volume *v = c->v;
    size_t cap = c->blen + c->blen / 2 + 4096;
    uint8_t *enc = NULL, *ver = NULL, *seg = NULL;
    size_t enc_len = 0;
    uint64_t pba = 0, phys = 0;
    uint32_t csize = 0, crc;
    int fail = 0;
    size_t i, j;
    int rc = 0;

    if (c->blen == 0) return 0;

    enc = (uint8_t *)malloc(cap);
    ver = (uint8_t *)malloc(c->blen);
    if (!enc || !ver) fail = 1;
    if (!fail) {
        if (c->binary) {
            /* WP14a: one zstd stream per batch. The payload codec is the
             * registry ZSTD entry; BCJ (algo tag 14 on the AST) already
             * ran per member slice before the bytes landed in bbuf. */
            const invfs_codec *zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
            if (!zc || !zc->encode ||
                zc->encode(c->bbuf, c->blen, enc, cap, &enc_len) != 0)
                fail = 1;
            /* invariant: the batch must round-trip byte-exactly before it
             * is stored */
            if (!fail &&
                (zc->decode(enc, enc_len, ver, c->blen) != 0 ||
                 memcmp(ver, c->bbuf, c->blen) != 0)) {
                fprintf(stderr, "tz: ZSTD round-trip mismatch -- binary "
                                "batch dropped, members stay generic\n");
                fail = 1;
            }
        } else {
            if (invfs_ppmd_encode(c->bbuf, c->blen, enc, cap, &enc_len) != 0)
                fail = 1;
            /* invariant: the batch must round-trip byte-exactly before it
             * is stored */
            if (!fail &&
                (invfs_ppmd_decode(enc, enc_len, ver, c->blen) != 0 ||
                 memcmp(ver, c->bbuf, c->blen) != 0)) {
                fprintf(stderr, "tz: PPMd round-trip mismatch -- batch dropped, "
                                "members stay generic\n");
                fail = 1;
            }
        }
    }
    /* size guard: the batch must beat storing the slices raw (§4: ppmd<raw,
     * no ZSTD comparison -- 3 s/4 MB is too expensive) */
    if (!fail && enc_len + 4 >= c->blen)
        fail = 1;
    /* the owner's ast_h.file_size is u32 */
    if (!fail && tz_owner_total(&c->owner) + c->blen > 0xFFFFFFFFu)
        fail = 1;

    if (!fail) {
        /* payload = [4B usize LE][PPMd wire]; standard [csize][crc] framing */
        uint32_t usize = (uint32_t)c->blen;
        uint8_t hdr[8];
        csize = (uint32_t)(4 + enc_len);
        phys = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
        pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                           phys, 1);
        if (!pba) {
            fail = 1;
        } else {
            seg = (uint8_t *)malloc((size_t)phys * INVFS_BLOCK_SIZE);
            if (!seg) { vol_free_blocks(v, pba, phys); pba = 0; fail = 1; }
        }
        if (!fail) {
            memcpy(seg + 8, &usize, 4);
            memcpy(seg + 12, enc, enc_len);
            crc = invfs_crc32c(seg + 8, csize);
            hdr[0] = (uint8_t)(csize & 0xFF);
            hdr[1] = (uint8_t)((csize >> 8) & 0xFF);
            hdr[2] = (uint8_t)((csize >> 16) & 0xFF);
            hdr[3] = (uint8_t)((csize >> 24) & 0xFF);
            hdr[4] = (uint8_t)(crc & 0xFF);
            hdr[5] = (uint8_t)((crc >> 8) & 0xFF);
            hdr[6] = (uint8_t)((crc >> 16) & 0xFF);
            hdr[7] = (uint8_t)((crc >> 24) & 0xFF);
            memcpy(seg, hdr, 8);
            if (write_segment_blocks(v, pba, seg, (size_t)csize + 8,
                                     phys) != 0) {
                vol_free_blocks(v, pba, phys);
                pba = 0;
                fail = 1;
            }
        }
        if (!fail && vol_map(v, c->owner_id, c->open_seq, pba,
                             (uint32_t)phys) != 0) {
            vol_free_blocks(v, pba, phys);
            pba = 0;
            fail = 1;
        }
    }

    if (fail) {
        /* nothing durable references the batch: every member with a slice
         * in it goes through the generic per-file path. A member that ALSO
         * has slices in earlier, sealed batches leaves those as holes in
         * otherwise-live batches -- GC/compactor territory, never a read
         * hazard (no record ever names them). */
        for (i = 0; i < n_members; i++) {
            tz_member *m = &members[i];
            if (m->committed || m->fallback) continue;
            for (j = 0; j < m->n_slices; j++)
                if (m->slices[j].batch_seq == c->open_seq) {
                    m->fallback = 1;
                    break;
                }
        }
        c->blen = 0;
        free(enc); free(ver); free(seg);
        return 0;
    }

    /* the owner map must be durable BEFORE the owner record names the pba */
    if (vol_pre_record(v) != 0) {
        free(enc); free(ver); free(seg);
        return -1;
    }
    /* owner AST += {cumulative offset, usize, TEXT, <batch algo>, seq, 0} */
    if (c->owner.n == c->owner.cap) {
        uint32_t nc = c->owner.cap ? c->owner.cap * 2 : 16;
        invfs_ast_block_entry *ne =
            (invfs_ast_block_entry *)realloc(c->owner.ents,
                                             nc * sizeof(*ne));
        if (!ne) { free(enc); free(ver); free(seg); return -1; }
        c->owner.ents = ne;
        c->owner.cap = nc;
    }
    {
        invfs_ast_block_entry *e = &c->owner.ents[c->owner.n];
        memset(e, 0, sizeof *e);
        e->length = c->blen;              /* file_offset rebuilt by write */
        e->zone = INVFS_ZONE_TEXT;
        e->algo = c->open_algo;           /* PPMD / ZSTD / ZSTD_BCJ */
        e->block_id = (uint32_t)c->open_seq;
        e->block_offset = 0;
        c->owner.n++;
    }
    if (tz_owner_write(v, c->owner_id, &c->owner) != 0) {
        free(enc); free(ver); free(seg);
        return -1;   /* segment + map landed; no record names them: orphan,
                        fsck/GC territory -- members stay RAW, nothing torn */
    }
    /* remember the seal for member-commit pba lookup */
    if (c->n_sealed == c->cap_sealed) {
        size_t nc = c->cap_sealed ? c->cap_sealed * 2 : 16;
        tz_sealed *ns = (tz_sealed *)realloc(c->sealed, nc * sizeof(*ns));
        if (!ns) { free(enc); free(ver); free(seg); return -1; }
        c->sealed = ns;
        c->cap_sealed = nc;
    }
    c->sealed[c->n_sealed].seq = c->open_seq;
    c->sealed[c->n_sealed].pba = pba;
    c->sealed[c->n_sealed].phys = (uint32_t)phys;
    c->sealed[c->n_sealed].algo = c->open_algo;
    c->n_sealed++;
    c->sealed_any = 1;
    if (getenv("INVFS_DEBUG"))
        fprintf(stderr, "[tz] sealed %s batch %llu: %zu -> %zu bytes (pba %llu)\n",
                c->open_algo == INVFS_ALGO_PPMD ? "ppmd" :
                c->open_algo == INVFS_ALGO_ZSTD_BCJ ? "zstd+bcj" : "zstd",
                (unsigned long long)c->open_seq, c->blen, enc_len,
                (unsigned long long)pba);
    c->open_seq++;
    c->blen = 0;
    /* members whose slices ALL live in sealed batches commit now */
    rc = tz_commit_ready(c, cands, members, n_members);
    free(enc); free(ver); free(seg);
    return rc;
}

/* stable ordering key: (family, size, original index) -- the doc/06 language
 * grouping; qsort is not stable, so the index breaks residual ties */
static const tz_candidate *g_sort_cands;
static int tz_order_cmp(const void *a, const void *b)
{
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    const tz_candidate *x = &g_sort_cands[ia], *y = &g_sort_cands[ib];
    if (x->family != y->family) return x->family < y->family ? -1 : 1;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    return ia < ib ? -1 : 1;
}

/* which stored classes may be re-batched at flush time (the candidate is
 * no longer RAW): the deferred retries (MEMLIMIT/UNCOMPRESSIBLE/GUARD
 * settled into generic storage) for both domains; plus, for the BINARY
 * accumulator, plain GENERIC -- the WP14a migration path re-batches
 * pre-existing per-segment-ZSTD binaries outright (binary batching shipped
 * with this build, so a GENERIC stamp on a binary-family file can only
 * predate it). Text keeps GENERIC terminal (WP10: no upgrade path). */
static int tz_flush_class_ok(int binary, uint8_t cc)
{
    if (cc == INVFS_CLASS_GENERIC_MEMLIMIT ||
        cc == INVFS_CLASS_UNCOMPRESSIBLE ||
        cc == INVFS_CLASS_GENERIC_GUARD)
        return 1;
    if (binary && cc == INVFS_CLASS_GENERIC)
        return 1;
    return 0;
}

/* Flush ONE accumulator (text: binary=0, WP10; binary: binary=1, WP14a).
 * The machinery is shared; what differs is exactly the codec (PPMd vs
 * ZSTD[+BCJ]), the class stamp, and the content re-validation. */
static int tz_flush_one(invfs_volume *v, int binary)
{
    tz_ctx c;
    size_t n, i;
    size_t *order = NULL;
    tz_candidate *sc = NULL;   /* candidates in sorted order */
    tz_member *members = NULL;
    tz_candidate *acc = binary ? v->bz : v->tz;
    size_t acc_n = binary ? v->bz_n : v->tz_n;
    int rc = 0;

    if (acc_n == 0) return 1;   /* nothing pending */
    if (!vol_write_enabled(v)) {
        if (binary) v->bz_n = 0;   /* read-only volume: nothing can land */
        else        v->tz_n = 0;
        return 1;
    }

    memset(&c, 0, sizeof c);
    c.v = v;
    c.binary = binary;
    c.open_algo = binary ? INVFS_ALGO_ZSTD : INVFS_ALGO_PPMD;
    c.skip_commit = getenv("INVFS_TZ_SKIP_COMMIT") != NULL;
    n = acc_n;

    /* batch target: min(4 MB, arc/2) (arc refuses larger units); a disabled
     * cache (budget 0) does not bound the batch -- reads still decode */
    c.bcap = (size_t)TZ_BATCH_MAX;
    if (v->arc_budget && v->arc_budget / 2 < (uint64_t)c.bcap)
        c.bcap = (size_t)(v->arc_budget / 2);
    if (c.bcap < 4096) c.bcap = 4096;

    c.bbuf = (uint8_t *)malloc(c.bcap);
    order = (size_t *)malloc(n * sizeof(size_t));
    sc = (tz_candidate *)malloc(n * sizeof(tz_candidate));
    members = (tz_member *)calloc(n, sizeof(tz_member));
    if (!c.bbuf || !order || !sc || !members) { rc = -1; goto out; }

    /* owner: find-or-create, then load its current AST state. ONE owner
     * ("\x01tzb") holds text and binary batches alike: the batch_seq space,
     * the L2P maps and the GC mark rule (zone==TEXT) are codec-agnostic. */
    c.owner_id = tz_owner_id(v);
    if (!c.owner_id || tz_owner_load(v, c.owner_id, &c.owner) != 0) {
        rc = -1;
        goto out;
    }
    c.open_seq = c.owner.next_seq;

    /* stable sort by (family, size) into the working copy */
    g_sort_cands = acc;
    for (i = 0; i < n; i++) order[i] = i;
    qsort(order, n, sizeof(size_t), tz_order_cmp);
    for (i = 0; i < n; i++) sc[i] = acc[order[i]];

    /* feed every candidate into the batch stream, sealing as batches fill */
    for (i = 0; i < n; i++) {
        tz_candidate *cand = &sc[i];
        tz_member *m = &members[i];
        uint8_t *data = NULL;
        size_t dlen = 0, off = 0;
        int z;

        /* re-validate against the LIVE name: the deferral is only a hint,
         * the content may have moved on (replaced => new id; swept by a
         * sibling path meanwhile => no longer ours to batch) */
        if (vol_find(v, cand->name) != cand->inode_id)
            continue;
        z = vol_inode_first_zone(v, cand->inode_id);
        if (z != INVFS_ZONE_RAW) {
            uint8_t cc = 0, ca = 0;
            uint16_t cg = 0;
            /* a deferred retry (MEMLIMIT/UNCOMPRESSIBLE/Guard settled into
             * generic storage) is fine to batch -- and, for the binary
             * accumulator, so is plain GENERIC (the WP14a migration);
             * anything else was swept meanwhile and is left alone */
            if (z < 0)
                continue;
            if (vol_get_class(v, cand->inode_id, &cc, &ca, &cg) != 0) {
                /* WP14b: a container part carries no class stamp; the walk
                 * deferred it out of the absent-stamp + BINARY zone +
                 * generic-algos shape, and the name->id pin above still
                 * holds the record that shape was checked against. */
                if (z != INVFS_ZONE_BINARY || !strchr(cand->name, '!'))
                    continue;
            } else if (!tz_flush_class_ok(binary, cc))
                continue;
        }
        if (vol_read_file(v, cand->inode_id, &data, &dlen) != 0 || !dlen) {
            free(data);
            continue;
        }
        if (binary) {
            /* the BCJ prefilter decision rides on the FRESH content's
             * family, not the defer-time sort key */
            int bfam = invfs_binary_family(data, dlen, cand->name);
            if (bfam == 0) {
                /* content changed class since the deferral: plain generic */
                vol_sweep_file(v, cand->inode_id);
                free(data);
                continue;
            }
            m->bcj = (bfam == INVFS_BIN_FAMILY_ELF_X64 ||
                      bfam == INVFS_BIN_FAMILY_ELF_X86);
            m->algo = m->bcj ? INVFS_ALGO_ZSTD_BCJ : INVFS_ALGO_ZSTD;
            /* batches are prefilter-homogeneous (a batch's algo is uniform
             * for all its members): a member of the other BCJ kind seals
             * the open batch first. The (family,size) sort keeps the BCJ
             * families (20,21) adjacent, so this split only ever fires at
             * a family boundary. */
            if (c.blen && c.open_bcj != m->bcj &&
                tz_seal(&c, sc, members, n) != 0) {
                rc = -1; free(data); goto out;
            }
        } else {
            if (invfs_text_family(cand->name, data, dlen) == 0) {
                /* content changed class since the deferral: plain generic */
                vol_sweep_file(v, cand->inode_id);
                free(data);
                continue;
            }
            m->algo = INVFS_ALGO_PPMD;
        }
        m->file_size = dlen;
        while (off < dlen) {
            size_t take;
            if (c.blen == c.bcap &&
                tz_seal(&c, sc, members, n) != 0) { rc = -1; free(data); goto out; }
            if (m->fallback) break;   /* its batch was dropped mid-file */
            if (c.blen == 0) {   /* first member of a batch sets the tone */
                c.open_bcj = m->bcj;
                c.open_algo = m->algo;
            }
            take = c.bcap - c.blen;
            if (take > dlen - off) take = dlen - off;
            if (tz_slice_push(m, c.open_seq, off, (uint32_t)c.blen,
                              (uint32_t)take) != 0) { rc = -1; free(data); goto out; }
            memcpy(c.bbuf + c.blen, data + off, take);
            /* WP14a: the BCJ prefilter runs on the member slice STANDALONE
             * (pc=0, state=0), before the batch exists as a whole. The read
             * path inverts exactly this window at pc=0 after the batch
             * decode -- enc and dec windows coincide, so the transform is
             * bijective regardless of where the slice sits in the batch. */
            if (m->bcj)
                invfs_bcj_x86_enc(c.bbuf + c.blen, take);
            c.blen += take;
            off += take;
        }
        free(data);
        m->complete = 1;
        if (c.blen == c.bcap &&
            tz_seal(&c, sc, members, n) != 0) { rc = -1; goto out; }
    }
    /* drain the partial batch, then commit whatever is left */
    if (rc == 0 && tz_seal(&c, sc, members, n) != 0) rc = -1;
    if (rc == 0 && tz_commit_ready(&c, sc, members, n) != 0) rc = -1;
    /* fallbacks go through the generic per-file path now; never-fed
     * candidates simply keep their files for the next run */
    for (i = 0; i < n; i++) {
        tz_member *m = &members[i];
        if (m->fallback && !m->committed)
            vol_sweep_file(v, sc[i].inode_id);
        free(m->slices);
    }
out:
    free(order);
    free(sc);
    free(members);
    free(c.bbuf);
    free(c.sealed);
    tz_owner_free(&c.owner);
    if (binary) v->bz_n = 0;   /* the accumulator is drained either way */
    else        v->tz_n = 0;
    if (rc != 0) return rc;
    return c.sealed_any ? 0 : 1;
}

int vol_tz_flush(invfs_volume *v)
{
    int rt, rb;

    if (!v) return -1;
    /* text first, then binary: two independent accumulators, one shared
     * owner inode (batch_seq space is handed out by tz_owner_load) */
    rt = tz_flush_one(v, 0);
    rb = tz_flush_one(v, 1);
    if (rt < 0 || rb < 0) return -1;
    return (rt == 0 || rb == 0) ? 0 : 1;   /* 0 = something sealed */
}

size_t vol_acc_pending(const invfs_volume *v, int binary)
{
    if (!v) return 0;
    return binary ? v->bz_n : v->tz_n;
}

/* WP14b M2: part count of the exe carve behind the last rc-11 answer */
unsigned vol_exer_last_parts(const invfs_volume *v)
{
    return v ? v->last_exer_parts : 0;
}

/* u32 comparator for the GC mark set */
static int tz_u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Text-zone GC (WP10 §7): mark-and-sweep over the owner AST. A batch is
 * live iff at least one LIVE member record has a zone==TEXT entry naming
 * its batch_seq (marking by block_id rather than pba keeps the mark set
 * correct even if a member's L2P dup is damaged -- the dup only maps the
 * name, it does not define liveness). Owner entries outside the mark set
 * are dead: invalidate the tagged ARC key, free the blocks (the retire-time
 * TEXT gate does not apply here -- freeing is the whole point), drop the
 * owner L2P map, and rewrite the owner record without them.
 * Returns the number of dead batches reclaimed, 0 = nothing, <0 = error. */
int vol_tz_gc(invfs_volume *v)
{
    tz_owner o;
    uint64_t owner;
    uint32_t *live = NULL;     /* sorted live batch_seqs */
    size_t n_live = 0, cap_live = 0;
    uint64_t pos, end;
    uint32_t i;
    size_t w;
    int freed = 0;
    int rc = 0;

    if (!v) return -1;
    owner = vol_find(v, TZ_OWNER_NAME);
    if (!owner) return 0;
    if (tz_owner_load(v, owner, &o) != 0) return -1;
    if (o.n == 0) { tz_owner_free(&o); return 0; }
    if (!vol_write_enabled(v)) { tz_owner_free(&o); return 0; }

    /* mark: walk live records, collect block_ids of zone==TEXT entries */
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    end = v->inode_area_pos;
    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec h;
        invfs_ast_recipe_header ah;
        invfs_ast_block_entry e;
        uint64_t apos;
        uint16_t nb, j;
        size_t nl;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &h, sizeof h) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof h || pos + h.rec_len + 4 > end) break;
        if (h.magic == TOMBSTONE_MAGIC) { pos += h.rec_len + 4; continue; }
        if (h.inode_id == owner) { pos += h.rec_len + 4; continue; }
        nl = h.name_len < sizeof(h.name) ? h.name_len : sizeof(h.name) - 1;
        /* only the LIVE record of a name marks anything (superseded ones
         * may still carry TEXT entries of an older batching generation) */
        {
            char nm[257];
            memcpy(nm, h.name, nl);
            nm[nl] = 0;
            if (vol_find(v, nm) != h.inode_id) { pos += h.rec_len + 4; continue; }
        }
        apos = pos + sizeof h;
        if (io_seek(&v->io, apos) != 0 ||
            io_read(&v->io, &ah, sizeof ah) != 0) break;
        nb = ah.num_blocks;
        for (j = 0; j < nb; j++) {
            if (io_seek(&v->io, apos + sizeof ah +
                        (uint64_t)j * sizeof e) != 0 ||
                io_read(&v->io, &e, sizeof e) != 0) { rc = -1; goto out; }
            if (e.zone != INVFS_ZONE_TEXT) continue;
            if (n_live == cap_live) {
                size_t nc = cap_live ? cap_live * 2 : 64;
                uint32_t *nl2 = (uint32_t *)realloc(live, nc * sizeof *nl2);
                if (!nl2) { rc = -1; goto out; }
                live = nl2;
                cap_live = nc;
            }
            live[n_live++] = e.block_id;
        }
        pos += h.rec_len + 4;
    }
    /* sort for the membership queries below */
    if (n_live > 1)
        qsort(live, n_live, sizeof *live, tz_u32_cmp);

    /* sweep: drop owner entries no live member names */
    if (vol_mark_dirty(v) != 0) { rc = -1; goto out; }
    w = 0;
    for (i = 0; i < o.n; i++) {
        uint32_t seq = o.ents[i].block_id;
        int keep = 0;
        if (live) {
            size_t lo = 0, hi = n_live;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (live[mid] < seq) lo = mid + 1; else hi = mid;
            }
            keep = lo < n_live && live[lo] == seq;
        }
        if (keep) {
            o.ents[w++] = o.ents[i];
            continue;
        }
        {
            uint64_t pba = 0, plen = 0;
            if (vol_lookup_entry(v, owner, seq, &pba, &plen) == 0 && pba) {
                arc_invalidate(v->arc, pba | TZ_ARC_TAG);
                vol_free_blocks(v, pba, plen);
                freed++;
            }
            l2p_remove(v, owner, seq);
        }
    }
    o.n = (uint32_t)w;
    if (freed > 0 && tz_owner_write(v, owner, &o) != 0) rc = -1;
out:
    free(live);
    tz_owner_free(&o);
    return rc ? rc : freed;
}

/* ---- WP12(h): offline per-segment dedupe ------------------------------
 *
 * Port of the Windows-prototype pass (src/sweep.c sweep_dedupe) into the
 * engine: BLAKE3 over the STORED bytes of every live segment ([8B header]
 * + csize payload -- a complete, deterministic function of what is on
 * disk), keep ONE physical copy per distinct hash, remap the losers'
 * (inode, lba) L2P entries onto the winner's pba -- the same L2P-dup
 * trick TEXT members use -- and return the duplicate blocks to the
 * bitmap. Runs between the sweep walk and vol_tz_gc: the walk's
 * transcodes are what create the duplicates worth finding (every album's
 * identical cover art lands in Shadow as identical segments).
 *
 * Skip rules (all three are load-bearing):
 *  - zone==INVFS_ZONE_TEXT entries (WP10 §11): shared PPMd batches belong
 *    to the owner inode and are never dedup candidates;
 *  - whole-file blobs (JXL/APE): unique by construction;
 *  - inodes sitting in the sweep run's batching accumulators (v->tz text,
 *    v->bz binary): their records are retired by the SAME run's
 *    vol_tz_flush, and retiring frees the id's L2P targets -- merging one
 *    first would free the canonical copy under the survivor the dup was
 *    remapped onto.
 *
 * Liveness uses the same rule as vol_compute_stats: the name must resolve
 * to this id (newest record wins) AND the id index must point at THIS
 * record position (position-kill tombstones leave older same-id versions
 * in the area).
 *
 * No arc_invalidate is needed for the freed pbas: the content cache holds
 * whole-file reconstructions keyed by inode id and decoded TEXT batches
 * keyed by pba|TZ_ARC_TAG (see vol_read_text_slice). Dedupe never touches
 * TEXT pbas, and per-segment RAW/BINARY payloads are decoded on read and
 * never cached (algo_is_whole_file), so no ARC entry can alias a freed
 * block. The merge changes only WHO points at the surviving copy, never
 * the bytes under a live mapping.
 *
 * The remap + free are in-memory until the caller's vol_flush (bitmap
 * slice + journal suffix land together), exactly like the sweep's own
 * RAW->Shadow moves. Returns the number of merged segments, <0 on error
 * (a failed merge restores the victim's mapping before bailing). */
typedef struct {
    uint8_t  hash[32];
    uint64_t inode, lba, pba;
    uint32_t phys;      /* physical blocks of this segment */
} dedup_seg;

static int dedup_cmp(const void *a, const void *b)
{
    const dedup_seg *x = (const dedup_seg *)a, *y = (const dedup_seg *)b;
    int c = memcmp(x->hash, y->hash, 32);
    if (c) return c;
    if (x->inode != y->inode) return x->inode < y->inode ? -1 : 1;
    if (x->lba  != y->lba)  return x->lba  < y->lba  ? -1 : 1;
    return 0;
}

/* is this inode deferred into one of the running sweep's accumulators
 * (text WP10 / binary WP14a)? */
static int dedup_is_deferred(const invfs_volume *v, uint64_t inode_id)
{
    size_t i;
    for (i = 0; i < v->tz_n; i++)
        if (v->tz[i].inode_id == inode_id) return 1;
    for (i = 0; i < v->bz_n; i++)
        if (v->bz[i].inode_id == inode_id) return 1;
    return 0;
}

int vol_sweep_dedupe(invfs_volume *v)
{
    uint64_t pos, end;
    size_t n = 0, cap = 1 << 16;
    dedup_seg *segs;
    blake3_hasher hx;
    uint8_t *blob = NULL;
    size_t blobcap = 0;
    size_t merged = 0;
    uint64_t freed_blocks = 0;
    uint8_t *freed = NULL;
    int marked = 0;             /* vol_mark_dirty done (first real merge) */
    int rc = 0;

    if (!v) return -1;
    if (!vol_write_enabled(v)) return 0;   /* read-only: nothing to merge */

    segs = (dedup_seg *)malloc(cap * sizeof(dedup_seg));
    if (!segs) return -1;

    /* pass 1: hash the stored bytes of every live, dedup-eligible segment */
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    end = v->inode_area_pos;
    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec h;
        invfs_ast_recipe_header ah;
        uint8_t *rec;
        uint32_t crc_stored, crc_calc;
        uint32_t i;
        size_t base;

        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &h, sizeof(h)) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof(h) || h.rec_len > INVFS_MAX_REC_LEN ||
            pos + h.rec_len + 4 > end) break;
        if (h.magic == TOMBSTONE_MAGIC) { pos += (uint64_t)h.rec_len + 4; continue; }
        if (h.name_len >= sizeof(h.name)) { pos += (uint64_t)h.rec_len + 4; continue; }

        rec = (uint8_t *)malloc(h.rec_len);
        if (!rec) { rc = -1; goto out; }
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, rec, h.rec_len) != 0 ||
            io_read(&v->io, &crc_stored, 4) != 0) { free(rec); rc = -1; goto out; }
        crc_calc = invfs_crc32c(rec, h.rec_len);
        if (crc_calc != crc_stored) {
            /* torn append: skip it, keep scanning (vol_open's rule) */
            free(rec);
            pos += (uint64_t)h.rec_len + 4;
            continue;
        }
        {
            /* newest-wins + position-kill: only the LIVE record version
             * describes segments that may be remapped */
            uint64_t ip = idx_get_id(v, h.inode_id);
            if (dedup_is_deferred(v, h.inode_id) ||
                vol_find(v, h.name) != h.inode_id || (ip && ip != pos)) {
                free(rec);
                pos += (uint64_t)h.rec_len + 4;
                continue;
            }
        }
        base = sizeof(invfs_inode_rec);
        if (h.rec_len < base + sizeof(ah)) { free(rec); break; }
        memcpy(&ah, rec + base, sizeof(ah));
        if (h.rec_len < base + sizeof(ah) +
                         (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
            free(rec); break;
        }
        for (i = 0; i < ah.num_blocks; i++) {
            invfs_ast_block_entry e;
            uint64_t pba = 0, phys = 0;
            uint8_t hdrb[8];
            uint32_t csize;

            memcpy(&e, rec + base + sizeof(ah) +
                   (size_t)i * sizeof(e), sizeof(e));
            if (e.zone == INVFS_ZONE_TEXT)
                continue;   /* WP10 §11: shared PPMd batches, owner-owned */
            if (e.algo == INVFS_ALGO_JXL || e.algo == INVFS_ALGO_APE ||
                e.algo == INVFS_ALGO_EXER)
                continue;   /* whole-file blobs: unique by construction */
            if (vol_lookup_entry(v, h.inode_id, e.block_id, &pba, &phys) != 0 ||
                pba == 0)
                continue;
            if (pba >= v->sb.total_blocks ||
                phys > v->sb.total_blocks - pba)
                continue;   /* stale map -- never hash out of bounds */
            if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, hdrb, 8) != 0)
                continue;
            memcpy(&csize, hdrb, 4);
            /* the payload must fit the blocks the L2P says this segment
             * owns; 0 or overlong means the header is not a segment */
            if (csize == 0 || (uint64_t)csize + 8 > phys * INVFS_BLOCK_SIZE)
                continue;
            if (csize > blobcap) {
                uint8_t *nb = (uint8_t *)realloc(blob, csize);
                if (!nb) { rc = -1; goto out; }
                blob = nb;
                blobcap = csize;
            }
            if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE + 8) != 0 ||
                io_read(&v->io, blob, csize) != 0)
                continue;
            blake3_hasher_init(&hx);
            blake3_hasher_update(&hx, hdrb, 8);
            blake3_hasher_update(&hx, blob, csize);
            blake3_hasher_finalize(&hx, segs[n].hash, 32);
            segs[n].inode = h.inode_id;
            segs[n].lba = e.block_id;
            segs[n].pba = pba;
            segs[n].phys = (uint32_t)phys;
            if (++n >= cap) {
                dedup_seg *ns;
                cap *= 2;
                ns = (dedup_seg *)realloc(segs, cap * sizeof(dedup_seg));
                if (!ns) { rc = -1; goto out; }
                segs = ns;
            }
        }
        free(rec);
        pos += (uint64_t)h.rec_len + 4;
    }
    printf("dedupe: hashed %zu live segments\n", n);

    /* pass 2: group by hash, remap duplicates onto the first copy */
    if (n > 1)
        qsort(segs, n, sizeof(dedup_seg), dedup_cmp);
    freed = (uint8_t *)calloc((size_t)(v->sb.total_blocks / 8 + 1), 1);
    if (!freed) { rc = -1; goto out; }
    {
        size_t i = 0;
        while (i < n) {
            size_t j = i + 1;
            while (j < n && memcmp(segs[i].hash, segs[j].hash, 32) == 0) j++;
            if (j - i > 1) {
                uint64_t canon_pba = segs[i].pba;
                size_t k;
                for (k = i + 1; k < j; k++) {
                    const dedup_seg *s = &segs[k];
                    uint64_t cur_pba = 0, cur_len = 0;
                    /* re-read the CURRENT map: superseded data is not ours
                     * to free, and an already-merged entry needs nothing */
                    if (vol_lookup_entry(v, s->inode, s->lba,
                                         &cur_pba, &cur_len) != 0)
                        continue;
                    if (cur_pba == canon_pba)
                        continue;   /* already shared */
                    if (!marked && vol_mark_dirty(v) != 0) { rc = -1; break; }
                    marked = 1;
                    l2p_remove(v, s->inode, s->lba);
                    if (vol_map(v, s->inode, s->lba, canon_pba,
                                (uint32_t)(cur_len ? cur_len : s->phys)) != 0) {
                        /* a failed merge must not strand the file without
                         * a mapping: put the old one back */
                        vol_map(v, s->inode, s->lba, cur_pba,
                                (uint32_t)(cur_len ? cur_len : s->phys));
                        rc = -1;
                        break;
                    }
                    /* free the duplicate copy once (3+ identical segments
                     * all point at the same loser pba after the first) */
                    if (cur_pba < v->sb.total_blocks &&
                        !(freed[cur_pba >> 3] & (1u << (cur_pba & 7)))) {
                        freed[cur_pba >> 3] |= (uint8_t)(1u << (cur_pba & 7));
                        vol_free_blocks(v, cur_pba,
                                        cur_len ? cur_len : s->phys);
                        freed_blocks += cur_len ? cur_len : s->phys;
                    }
                    merged++;
                }
                if (rc != 0) break;
            }
            i = j;
        }
    }
    printf("dedupe: merged %zu segments, freed %llu blocks\n",
           merged, (unsigned long long)freed_blocks);
out:
    free(freed);
    free(blob);
    free(segs);
    return rc ? rc : (int)merged;
}



/* Does this record own "name!..." siblings that must die with it? A
 * ZIP-style container lists AST children; the extraction containers
 * (TARR/GZR/PNGR/FLACR/EXER) carry num_children == 0 but keep their
 * payload in sibling inodes ("name!partN", "name!recipe", "name!jxl",
 * "name!coverN", "name!exrN") the read path resolves by name -- deleting
 * only the anchor strands them as live records nothing reaches (verified:
 * a TAR's parts survived vol_unlink). The sibling walk is O(area), so
 * plain files skip it.
 * 1 = siblings possible (unknown record -> 1: scan conservatively). */
static int record_owns_siblings(const uint8_t *rec, uint32_t rl)
{
    invfs_ast_recipe_header ah;
    size_t base = sizeof(invfs_inode_rec);
    uint32_t fl;

    if (!rec || rl < base + sizeof(ah) + sizeof(invfs_ast_block_entry))
        return 1;
    memcpy(&ah, rec + base, sizeof(ah));
    if (ah.num_children != 0)
        return 1;
    if (ah.num_blocks == 0)
        return 0;
    memcpy(&fl, rec + base + sizeof(ah) + 16, 4);   /* zone:2 | algo:6 LSB */
    switch ((fl >> 2) & 0x3F) {
    case INVFS_ALGO_TARR:
    case INVFS_ALGO_GZR:
    case INVFS_ALGO_PNGR:
    case INVFS_ALGO_FLACR:
    case INVFS_ALGO_EXER:
        return 1;
    }
    return 0;
}

/* Replace `name` with `data`, or create it if absent.
 *
 * The write commit path used to delete by name and then create, which loses
 * the user's existing file outright when the create fails: the old record is
 * already tombstoned and the new bytes are gone with it. ENOSPC on an
 * overwrite is a normal condition, not a corner case -- the whole point of
 * this filesystem is that data survives.
 *
 * Append the new record first. The inode area is append-only and readers are
 * version-aware, so the newer record wins from the moment it lands; only then
 * is the old inode tombstoned. idx_del ignores a mismatched id, so the
 * tombstone cannot take the replacement's name down with it (the sweep has
 * relied on exactly this ordering all along).
 *
 * Returns the new inode id, or 0 with the volume untouched. */
uint64_t vol_replace_file(invfs_volume *v, const char *name,
                          const uint8_t *data, size_t len)
{
    uint64_t old_id, nid;

    if (v->sb.vol_flags & VOLF_READONLY) return 0;   /* EROFS */
    old_id = vol_find(v, name);
    nid = vol_create_file(v, name, data, len);
    if (nid == 0) return 0;                          /* old file still there */
    if (old_id != 0) {
        /* the replacement is plain RAW, so the old file's recipe/parts/covers
           describe bytes that no longer exist under this name */
        int owns_siblings = 1;   /* unknown: scan conservatively */
        uint8_t *obuf = NULL;
        uint32_t orl = 0;
        if (meta_read_record_by_id(v, old_id, &obuf, &orl, NULL, 0, NULL) == 0) {
            owns_siblings = record_owns_siblings(obuf, orl);
            free(obuf);
        }
        vol_delete_inode(v, old_id, name);
        /* sibling deletion walks the WHOLE inode area (pread per record):
         * plain files -- everything written through FUSE -- can only have
         * '!' siblings when their AST lists children or a container algo,
         * so skip the walk */
        if (owns_siblings)
            vol_delete_siblings(v, name);
    }
    return nid;
}

int vol_delete_file(invfs_volume *v, const char *name)
{
    uint64_t inode_id;
    if (v->sb.vol_flags & VOLF_READONLY) return -1;   /* EROFS */
    inode_id = vol_find(v, name);
    if (inode_id == 0)
        return -1;
    return vol_delete_inode(v, inode_id, name);
}

/* Delete a file and the sibling records that belong to it. This is what an
   unlink means for a transcoded file; vol_delete_file alone strands them. */
/* remove ONE name of a multi-name (hardlinked) inode: tombstone only
 * this name's record; blocks stay alive for the surviving names.
 * nlink on surviving records is not rewritten (drift only delays block
 * retirement; fsck recount fixes it). */
int vol_unlink_name(invfs_volume *v, const char *name)
{
    uint64_t id, pos = 0;
    uint8_t *obuf = NULL;
    uint32_t orl = 0, crc;
    size_t nl;
    invfs_inode_rec rec;

    if (v->sb.vol_flags & VOLF_READONLY) return -1;
    id = vol_find(v, name);
    if (!id) return -1;
    if (meta_read_record_by_id(v, id, &obuf, &orl, NULL, 0, &pos) != 0 || !pos) {
        free(obuf);
        return -1;
    }
    free(obuf);
    vol_mark_dirty(v);
    nl = strlen(name);
    memset(&rec, 0, sizeof(rec));
    rec.magic = TOMBSTONE_MAGIC;
    v->hot.tombstones++;
    rec.rec_len = (uint32_t)sizeof(rec);
    rec.inode_id = id;
    rec.file_size = pos;                    /* v2 position-kill */
    rec_set_name(&rec, name);
    crc = invfs_crc32c((const uint8_t *)&rec, sizeof(rec));
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, &rec, sizeof(rec)) != 0 ||
        io_write(&v->io, &crc, 4) != 0)
        return -1;
    v->inode_area_pos += sizeof(rec) + 4;
    idx_del_at(v, name, nl, pos);
    idx_bump_dirs(v, name, nl, -1);
    return 0;
}

int vol_unlink(invfs_volume *v, const char *name)
{
    int rc;
    if (v->sb.vol_flags & VOLF_READONLY) return -1;   /* EROFS */
    if (strchr(name, '!')) return vol_delete_file(v, name);   /* a sibling */
    /* capture the record first: sibling deletion is a full-area walk and
     * plain files (no children, no container algo) never have '!' siblings */
    {
        int owns_siblings = 1;
        uint8_t *obuf = NULL;
        uint32_t orl = 0;
        uint64_t id = vol_find(v, name);
        if (id && meta_read_record_by_id(v, id, &obuf, &orl, NULL, 0, NULL) == 0) {
            owns_siblings = record_owns_siblings(obuf, orl);
            free(obuf);
        }
        rc = vol_delete_file(v, name);
        if (rc == 0 && owns_siblings) vol_delete_siblings(v, name);
    }
    return rc;
}

/* ---- rename / move ---- */

/* Move ONE inode record to a new name.
 *
 * The name lives inside the inode record and the inode area is append-only,
 * so a rename is: append a copy of the record with the AST payload verbatim
 * (the data blocks are never read, rewritten or recompressed), re-key that
 * inode's L2P mappings, then tombstone the old name.
 *
 * The copy deliberately gets a NEW inode id. vol_fsck_scan kills live records
 * by inode_id alone (pass 2), so if both names shared an id, the old name's
 * tombstone would make `invf-fsck --fix` free the renamed file's blocks. */
static int rename_one(invfs_volume *v, const char *from, const char *to)
{
    uint64_t old_id, new_id, rec_pos = 0;
    invfs_inode_rec rh, *nh;
    uint8_t *rec;
    uint32_t crc;
    size_t i, tolen = strlen(to), fromlen = strlen(from);
    const name_index_entry *ie;

    (void)fromlen;
    if (tolen >= sizeof(rh.name)) return -1;
    ie = idx_get(v, from, fromlen);
    if (!ie) return -1;
    old_id = ie->inode_id;
    rec_pos = ie->pos;          /* the live record — the index tracks it */
    if (rec_pos == 0) return -1;

    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, &rh, sizeof rh) != 0) return -1;
    if (rh.rec_len < sizeof(invfs_inode_rec)) return -1;

    /* room for the copy AND the tombstone that follows it — running out
       between the two would leave the file reachable under both names */
    if (v->inode_area_pos + rh.rec_len + 4 +
        sizeof(invfs_inode_rec) + 4 > v->inode_area_end)
        return -3;   /* inode area full */

    rec = (uint8_t *)malloc(rh.rec_len);
    if (!rec) return -1;
    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, rec, rh.rec_len) != 0) { free(rec); return -1; }

    new_id = v->next_inode_id++;
    nh = (invfs_inode_rec *)rec;
    nh->inode_id = new_id;
    nh->name_len = (uint32_t)tolen;
    memset(nh->name, 0, sizeof nh->name);
    memcpy(nh->name, to, tolen);
    crc = invfs_crc32c(rec, rh.rec_len);
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, rec, rh.rec_len) != 0 ||
        io_write(&v->io, &crc, 4) != 0) { free(rec); return -1; }
    v->inode_area_pos += rh.rec_len + 4;
    idx_put(v, to, tolen, new_id, v->inode_area_pos - rh.rec_len - 4,
            nh->file_size, nh->ctime);
    idx_put_id(v, new_id, v->inode_area_pos - rh.rec_len - 4);
    free(rec);

    /* hand the blocks over BEFORE tombstoning the old name: vol_delete_inode
       frees every block still mapped to its inode, so the re-key is what
       makes the tombstone a pure unlink instead of a data free */
    for (i = 0; i < v->l2p_count; i++)
        if (v->l2p[i].inode == old_id) {
            v->l2p[i].inode = new_id;
            if (i < v->l2p_dirty) v->l2p_dirty = i;
        }

    return vol_delete_inode(v, old_id, from);
}

/* Rename a file or directory.
 *
 * Transcoded files keep their payload in sibling records ("name!recipe",
 * "name!partN", "name!coverN", "name!jxl") that the read path resolves BY
 * NAME, so moving a file has to move its siblings too — otherwise the
 * bit-exact rebuild silently loses its recipe. Directories are virtual
 * (anchor "dir/" + prefix), so moving one rewrites every record under the
 * prefix, anchor included.
 *
 * Returns 0, -1 = ENOENT/EINVAL/IO, -2 = destination exists, -3 = ENOSPC.
 * The inode area is append-only, so a multi-record move that fails partway
 * leaves the already-moved records at their new names; re-running the rename
 * completes it. */
int vol_rename(invfs_volume *v, const char *from, const char *to)
{
    char (*names)[256] = NULL;
    char pre[300];
    size_t n = 0, cap = 0, pren = 0, flen, i;
    uint64_t pos, end;
    int dir, rc = 0;

    if (v->sb.vol_flags & VOLF_READONLY) return -1;   /* EROFS */
    if (!v || !from || !to || !from[0] || !to[0]) return -1;
    if (strcmp(from, to) == 0) return 0;
    flen = strlen(from);
    if (strlen(to) >= 240) return -1;
    /* "a" -> "a/b" would move a directory inside itself */
    if (strncmp(to, from, flen) == 0 && to[flen] == '/') return -1;

    dir = vol_is_dir(v, from);
    if (!dir && vol_find(v, from) == 0) return -1;            /* ENOENT */
    /* POSIX rename replaces an existing plain-file destination
     * silently; directory destinations stay unsupported (EEXIST). */
    if (vol_is_dir(v, to)) return -2;
    {
        uint64_t toid = vol_find(v, to);
        if (toid != 0) {
            if (dir) return -1;                    /* dir over file: ENOTDIR-ish */
            if (vol_delete_file(v, to) != 0) return -1;
        }
    }

    vol_ensure_path(v, to);   /* parents of the destination */

    /* FAST PATH: plain files without container siblings are renamed as
     * hardlink(new)+unlink_name(old) -- both are O(1) index-hint ops.
     * The legacy full-area sibling scan below only runs for directories
     * and container anchors ('!' siblings / num_children>0); on big
     * volumes it cost minutes per rename (dracut does hundreds). */
    if (!dir && !strchr(from, '!') && !strchr(to, '!')) {
        uint8_t *buf = NULL;
        uint32_t rl = 0;
        uint64_t fid = vol_find(v, from);
        int simple = 0;
        if (fid != 0 &&
            meta_read_record_by_id(v, fid, &buf, &rl, NULL, 0, NULL) == 0 &&
            rl >= sizeof(invfs_inode_rec) + 16) {
            uint16_t nch;
            memcpy(&nch, buf + sizeof(invfs_inode_rec) + 12, 2);
            simple = (nch == 0);
        }
        free(buf);
        if (simple) {
            if (vol_hardlink(v, from, to) != 0) return -1;
            if (vol_unlink_name(v, from) != 0) return -1;
            return 0;
        }
    }

    if (dir) { snprintf(pre, sizeof pre, "%s/", from); pren = strlen(pre); }

    /* Collect every record the move touches BEFORE appending anything:
       each rename_one extends the inode area, and a live scan would then
       walk into the records it had just written. */
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    end = v->inode_area_pos;
    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec h;
        char nm[257];
        size_t nl;
        if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &h, sizeof h) != 0) {
            rc = -1; break;
        }
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof(invfs_inode_rec)) break;
        pos += h.rec_len + 4;
        if (h.magic != INODE_REC_MAGIC) continue;
        nl = h.name_len < 256 ? h.name_len : 256;
        memcpy(nm, h.name, nl);
        nm[nl] = 0;
        if (dir) {
            if (nl < pren || strncmp(nm, pre, pren) != 0) continue;
        } else {
            /* the file itself, plus its "name!..." siblings */
            if (nl < flen || strncmp(nm, from, flen) != 0) continue;
            if (nm[flen] != 0 && nm[flen] != '!') continue;
        }
        if (vol_find(v, nm) == 0) continue;   /* superseded/tombstoned */
        for (i = 0; i < n; i++)
            if (strcmp(names[i], nm) == 0) break;
        if (i < n) continue;                  /* older record for same name */
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            void *nn = realloc(names, ncap * sizeof(*names));
            if (!nn) { free(names); return -1; }
            names = (char (*)[256])nn;
            cap = ncap;
        }
        memcpy(names[n++], nm, nl + 1);
    }

    if (rc == 0 && n == 0) rc = -1;   /* nothing matched */

    for (i = 0; i < n && rc == 0; i++) {
        char nn[300];
        if (dir) snprintf(nn, sizeof nn, "%s/%s", to, names[i] + pren);
        else     snprintf(nn, sizeof nn, "%s%s", to, names[i] + flen);
        if (strlen(nn) >= 256) { rc = -1; break; }
        rc = rename_one(v, names[i], nn);
    }
    free(names);
    return rc;
}

/* Hard link: a second NAME record sharing the same inode_id (and thus the
 * same AST/L2P blocks). LIMITATION: no block refcounts yet -- unlinking
 * EITHER name retires the shared blocks and dangles the survivor
 * (audit WP6). Portage lock files and similar transient links are fine. */
int vol_hardlink(invfs_volume *v, const char *from, const char *to)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0, crc;
    invfs_inode_rec *nh;
    uint64_t id, pos;
    size_t nl;

    if (!v || !from || !to || !from[0] || !to[0]) return -1;
    if (v->sb.vol_flags & VOLF_READONLY) return -1;
    if (strlen(to) >= 256) return -1;

    id = vol_find(v, from);
    if (!id) return -1;                              /* ENOENT */
    {
        uint64_t toid = vol_find(v, to);
        if (toid) {
            /* stale-index self-heal: killed emerges leave table entries
             * whose latest record is already a tombstone. The id-index
             * hint always points at the newest version we wrote, so
             * liveness = the header AT THE HINT is an INOD for this id.
             * (meta_read_record_by_id alone would false-positive: it
             * happily reads OLDER same-id versions still present in the
             * append-only area.) */
            int alive = 0;
            uint64_t hp = idx_get_id(v, toid);
            if (hp >= v->inode_area_start * INVFS_BLOCK_SIZE &&
                hp + sizeof(invfs_inode_rec) <= v->inode_area_pos &&
                io_seek(&v->io, hp) == 0) {
                invfs_inode_rec hh;
                if (io_read(&v->io, &hh, sizeof(hh)) == 0 &&
                    hh.magic == INODE_REC_MAGIC &&
                    hh.inode_id == toid)
                    alive = 1;
            }
            if (alive) {
                return -2;                           /* EEXIST */
            }
        }
    }
    if (vol_is_dir(v, from)) return -3;              /* dirs can't hardlink */

    if (meta_read_record_by_id(v, id, &buf, &rl, NULL, 0, NULL) != 0)
        return -1;
    if (rl < sizeof(invfs_inode_rec)) { free(buf); return -1; }

    /* name[] is a fixed 256-byte field: record size is unchanged */
    nh = (invfs_inode_rec *)buf;
    nl = strlen(to);
    memset(nh->name, 0, sizeof(nh->name));
    memcpy(nh->name, to, nl);
    nh->name_len = (uint32_t)nl;

    if (vol_mark_dirty(v) != 0) { free(buf); return -1; }
    crc = invfs_crc32c(buf, rl);
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, buf, rl) != 0 ||
        io_write(&v->io, &crc, 4) != 0) {
        free(buf);
        return -1;
    }
    pos = v->inode_area_pos;
    v->inode_area_pos += (uint64_t)rl + 4;
    idx_put(v, nh->name, nl, id, pos, nh->file_size, nh->ctime);
    idx_put_id(v, id, pos);
    /* the second name is a live child of its parent directories; without
     * this bump, unlinking it later drives parent counts negative and
     * rmdir starts refusing empty dirs ("Directory not empty") */
    idx_bump_dirs(v, to, nl, +1);
    free(buf);
    return 0;
}

/* count free blocks from in-memory bitmap */
uint64_t vol_count_free(invfs_volume *v)
{
    uint64_t i, free = 0;
    for (i = 0; i < v->sb.total_blocks; i++)
        if (!bit_get(v->bitmap, i))
            free++;
    return free;
}

/* Free blocks inside one zone. Called once per mount (and after fsck
 * rewrites the bitmap) to seed the per-zone counters that alloc_blocks
 * then maintains incrementally. */
static uint64_t zone_count_free(invfs_volume *v, uint64_t start, uint64_t len)
{
    uint64_t i, free = 0;
    for (i = start; i < start + len && i < v->sb.total_blocks; i++)
        if (!bit_get(v->bitmap, i))
            free++;
    return free;
}

static void alloc_state_reset(invfs_volume *v)
{
    v->raw_cursor    = v->sb.raw_zone_start;
    v->shadow_cursor = v->sb.shadow_zone_start;
    v->raw_free    = zone_count_free(v, v->sb.raw_zone_start,
                                    v->sb.raw_zone_blocks);
    v->shadow_free = zone_count_free(v, v->sb.shadow_zone_start,
                                    v->sb.shadow_zone_blocks);
    v->raw_fail_run = v->shadow_fail_run = 0;
    v->bm_lo = 1; v->bm_hi = 0;   /* on-disk bitmap matches memory */
}

/*
/*
 * JXL helpers: transcode JPEG -> JXL (lossless) and back via subprocesses.
 * Returns 0 on success. Out buffers are malloc'd.
 */
#ifndef _WIN32
/* ---- WP11: POSIX twin of the Windows tool plumbing (CreateProcessW) ----
 *
 * One exec layer serves every external codec (cjxl/djxl, MAC, packMP3,
 * ffmpeg): fixed argv arrays (no shell), a fresh mkdtemp scratch dir per
 * transcode (a leftover output from an earlier run can never be misread as
 * this run's), and a hard timeout so a wedged helper cannot hang a sweep.
 * Scratch lives in /dev/shm (tmpfs) first -- the intermediate WAV/JPEG can
 * be tens of MB and should not wear the flash the volume itself lives on --
 * with /tmp as fallback. */

/* read a whole file into a fresh buffer; returns 0 on success */
static int slurp_file(const char *path, uint8_t **out, size_t *out_len);

/* Tool search order: $INVFS_TOOLS/<name> -> /usr/lib/invfs/tools/<name> ->
 * the bare name (execvp's PATH search). No other absolute paths. */
static const char *tool_resolve(const char *name, char *buf, size_t cap)
{
    const char *dir = getenv("INVFS_TOOLS");
    int n;

    if (dir && *dir) {
        n = snprintf(buf, cap, "%s/%s", dir, name);
        if (n > 0 && (size_t)n < cap && access(buf, X_OK) == 0)
            return buf;
    }
    n = snprintf(buf, cap, "/usr/lib/invfs/tools/%s", name);
    if (n > 0 && (size_t)n < cap && access(buf, X_OK) == 0)
        return buf;
    return name;
}

static uint64_t tool_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

#define TOOL_TIMEOUT_MS (120ull * 1000ull)

/* fork/execvp, wait with a timeout. The child is muted (stdin/out/err to
 * /dev/null), matching the CREATE_NO_WINDOW processes on the Windows side.
 * Returns the child's exit code, or -1 on fork failure, a kill, or expiry. */
static int tool_exec(char *const argv[])
{
    pid_t pid;
    int st = 0;
    uint64_t t0;

    if (!argv || !argv[0]) return -1;
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    t0 = tool_now_ms();
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (tool_now_ms() - t0 > TOOL_TIMEOUT_MS) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
                ;
            fprintf(stderr, "tool_exec: %s killed after %llus\n", argv[0],
                    (unsigned long long)(TOOL_TIMEOUT_MS / 1000));
            return -1;
        }
        usleep(5000);
    }
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

static int tool_tmpdir(char *dir, size_t cap)
{
    static const char *roots[] = { "/dev/shm", "/tmp" };
    size_t r;

    for (r = 0; r < sizeof roots / sizeof roots[0]; r++) {
        int n = snprintf(dir, cap, "%s/invfs-tool-XXXXXX", roots[r]);
        if (n > 0 && (size_t)n < cap && mkdtemp(dir) != NULL)
            return 0;
    }
    return -1;
}

static void tool_rm(const char *dir, const char *name)
{
    char p[320];
    int n = snprintf(p, sizeof p, "%s/%s", dir, name);
    if (n > 0 && (size_t)n < sizeof p) unlink(p);
}

/* write a whole buffer, creating/truncating; 0 on success */
static int tool_write(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    return fclose(f);
}

/* slurp a tool's output file; 0 only if it exists and is non-empty (an
 * empty output is how several of these tools say "refused") */
static int tool_slurp_out(const char *path, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (slurp_file(path, out, out_len) != 0 || *out_len == 0) {
        free(*out);
        *out = NULL;
        *out_len = 0;
        return -1;
    }
    return 0;
}

/* like tool_exec, but the child's stdout lands in buf (NUL-terminated,
 * truncated at cap-1, overflow drained and discarded so the child never
 * blocks on a full pipe). Used by the codecpack estimate hook. */
static int tool_exec_out(char *const argv[], char *buf, size_t cap)
{
    int pfd[2], st = 0, exited = 0;
    pid_t pid;
    uint64_t t0;
    size_t got = 0;

    if (cap) buf[0] = '\0';
    if (pipe(pfd) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            dup2(dn, STDERR_FILENO);
        }
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL, 0) | O_NONBLOCK);
    t0 = tool_now_ms();
    for (;;) {
        pid_t w;
        for (;;) {   /* drain what there is; discard past cap */
            char junk[256];
            char *dst = got + 1 < cap ? buf + got : junk;
            size_t room = dst == junk ? sizeof junk : cap - 1 - got;
            ssize_t r = read(pfd[0], dst, room);
            if (r <= 0) break;
            if (dst != junk) got += (size_t)r;
        }
        if (exited) break;      /* reaped and drained */
        w = waitpid(pid, &st, WNOHANG);
        if (w == pid) { exited = 1; continue; }
        if (w < 0 && errno != EINTR) { close(pfd[0]); return -1; }
        if (tool_now_ms() - t0 > TOOL_TIMEOUT_MS) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
                ;
            close(pfd[0]);
            return -1;
        }
        usleep(5000);
    }
    close(pfd[0]);
    if (cap) buf[got < cap ? got : cap - 1] = '\0';
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}
#endif /* !_WIN32 */

/* ---- codecpack execution hooks (WP13; declared in codec.h, called by the
 * codec.c trampolines and the sweep's pack branch) ---- */

/* Substitute {in} {out} {pack} in one argv token. Returns 0 on overflow. */
static size_t pack_subst(char *dst, size_t cap, const char *tok,
                         const char *packdir, const char *in, const char *out)
{
    size_t w = 0;

    while (*tok) {
        const char *rep;
        size_t rl;
        if (strncmp(tok, "{in}", 4) == 0)         { rep = in;      tok += 4; }
        else if (strncmp(tok, "{out}", 5) == 0)   { rep = out;     tok += 5; }
        else if (strncmp(tok, "{pack}", 6) == 0)  { rep = packdir; tok += 6; }
        else { rep = tok++; rl = 1; goto emit; }
        rl = strlen(rep);
    emit:
        if (w + rl + 1 > cap) return 0;
        memcpy(dst + w, rep, rl);
        w += rl;
    }
    dst[w] = '\0';
    return w;
}

/* Fixed argv from the manifest template: split on whitespace, substitute
 * placeholders per token (no shell — WP10 §10). A bare argv[0] stays bare
 * (execvp does the PATH search); a relative path containing '/' is taken
 * relative to the pack dir (mirrors manifest_tool_ok). */
static int pack_argv_build(const invfs_pack_def *def, const char *tmpl,
                           const char *in, const char *out,
                           char *argv[], size_t maxa,
                           char *arena, size_t acap)
{
    size_t used = 0, argc = 0;

    while (*tmpl) {
        char tok[1024];
        size_t tl = 0, w;

        while (*tmpl == ' ' || *tmpl == '\t') tmpl++;
        if (!*tmpl) break;
        while (tmpl[tl] && tmpl[tl] != ' ' && tmpl[tl] != '\t') {
            if (tl + 1 >= sizeof tok) return -1;
            tok[tl] = tmpl[tl];
            tl++;
        }
        tok[tl] = '\0';
        tmpl += tl;
        if (argc + 1 >= maxa) return -1;
        w = pack_subst(arena + used, acap - used, tok,
                       def->dir, in ? in : "", out ? out : "");
        if (!w && tok[0]) return -1;    /* arena overflow */
        if (argc == 0 && strchr(arena + used, '/') && arena[used] != '/') {
            /* relative path: resolve against the pack dir */
            char joined[4096];
            int n = snprintf(joined, sizeof joined, "%s/%s",
                             def->dir, arena + used);
            if (n <= 0 || (size_t)n >= sizeof joined ||
                (size_t)n + 1 > acap - used) return -1;
            memcpy(arena + used, joined, (size_t)n + 1);
            w = (size_t)n;
        }
        argv[argc++] = arena + used;
        used += w + 1;
    }
    argv[argc] = NULL;
    return argc ? 0 : -1;
}

int invfs_codec_pack_exec(const invfs_codec *c, int is_encode,
                          const char *in_path, const char *out_path)
{
#ifdef _WIN32
    (void)c; (void)is_encode; (void)in_path; (void)out_path;
    return -1;   /* the POSIX tool layer does not exist on Windows */
#else
    const invfs_pack_def *def = invfs_codec_pack_def(c);
    const char *tmpl;
    char *argv[24];
    char arena[4096];

    if (!def) return -1;
    tmpl = is_encode ? def->encode : def->decode;
    if (!tmpl || !in_path || !out_path) return -1;
    if (pack_argv_build(def, tmpl, in_path, out_path,
                        argv, 24, arena, sizeof arena) != 0)
        return -1;
    return tool_exec(argv);
#endif
}

int invfs_codec_pack_estimate(const invfs_codec *c, const char *in_path,
                              uint64_t *out_bytes)
{
#ifdef _WIN32
    (void)c; (void)in_path; (void)out_bytes;
    return -1;
#else
    const invfs_pack_def *def = invfs_codec_pack_def(c);
    char *argv[24];
    char arena[4096];
    char out[256];
    char *endp = NULL;
    unsigned long long v;

    if (!def || !def->estimate || !in_path) return -1;
    if (pack_argv_build(def, def->estimate, in_path, NULL,
                        argv, 24, arena, sizeof arena) != 0)
        return -1;
    if (tool_exec_out(argv, out, sizeof out) != 0) return -1;
    errno = 0;
    v = strtoull(out, &endp, 10);
    if (errno || endp == out) return -1;
    while (*endp == ' ' || *endp == '\t' || *endp == '\n' || *endp == '\r')
        endp++;
    if (*endp) return -1;   /* trailing garbage: not a bare byte count */
    *out_bytes = (uint64_t)v;
    return 0;
#endif
}

static int run_tool(const char *exe, const char *a1, const char *a2, const char *opts)
{
#ifdef _WIN32
    WCHAR wexe[512], wcmd[4096];
    WCHAR wa1[512], wa2[512], wopts[256];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    MultiByteToWideChar(CP_UTF8, 0, exe, -1, wexe, 512);
    MultiByteToWideChar(CP_UTF8, 0, a1, -1, wa1, 512);
    MultiByteToWideChar(CP_UTF8, 0, a2, -1, wa2, 512);
    MultiByteToWideChar(CP_UTF8, 0, opts, -1, wopts, 256);
    swprintf_s(wcmd, 4096, L"\"%s\" \"%s\" \"%s\" %s", wexe, wa1, wa2, wopts);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(wexe, wcmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        fprintf(stderr, "run_tool: CreateProcess failed (%lu): %ls\n",
                (unsigned long)GetLastError(), wcmd);
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    /* "<tool> <in> <out> <opts...>" — opts is a compile-time literal at
     * every call site ("--lossless_jpeg=1", "-d 0 -e 7", ...), split on
     * whitespace into a fixed argv; no shell, no quoting. */
    char exeb[512], obuf[256];
    char *argv[16], *save = NULL, *tok;
    int ac = 0;

    argv[ac++] = (char *)tool_resolve(exe, exeb, sizeof exeb);
    argv[ac++] = (char *)a1;
    argv[ac++] = (char *)a2;
    snprintf(obuf, sizeof obuf, "%s", opts ? opts : "");
    for (tok = strtok_r(obuf, " \t", &save); tok && ac < 15;
         tok = strtok_r(NULL, " \t", &save))
        argv[ac++] = tok;
    argv[ac] = NULL;
    return tool_exec(argv);
#endif
}

/* ffmpeg needs -i before the input */
static int run_ffmpeg(const char *a1, const char *a2, const char *opts)
{
#ifdef _WIN32
    WCHAR wcmd[4096];
    WCHAR wa1[512], wa2[512], wopts[256];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    MultiByteToWideChar(CP_UTF8, 0, a1, -1, wa1, 512);
    MultiByteToWideChar(CP_UTF8, 0, a2, -1, wa2, 512);
    MultiByteToWideChar(CP_UTF8, 0, opts, -1, wopts, 256);
    /* options MUST come before the output file: ffmpeg 8.x ignores
       -c:a/-sample_fmt placed after the output (silently emits 16-bit) */
    swprintf_s(wcmd, 4096, L"ffmpeg -loglevel error -i \"%s\" %s \"%s\"", wa1, wopts, wa2);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        fprintf(stderr, "run_ffmpeg: CreateProcess failed (%lu)\n",
                (unsigned long)GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0)
        fprintf(stderr, "run_ffmpeg: exit code %lu: %ls\n", (unsigned long)code, wcmd);
    return (int)code;
#else
    /* ffmpeg -loglevel error -i <a1> <opts...> <a2> — options MUST come
     * before the output file: ffmpeg 8.x ignores -c:a/-sample_fmt placed
     * after the output (silently emits 16-bit) */
    char exeb[512], obuf[256];
    char *argv[20], *save = NULL, *tok;
    int ac = 0, rc;

    argv[ac++] = (char *)tool_resolve("ffmpeg", exeb, sizeof exeb);
    argv[ac++] = (char *)"-loglevel";
    argv[ac++] = (char *)"error";
    argv[ac++] = (char *)"-i";
    argv[ac++] = (char *)a1;
    snprintf(obuf, sizeof obuf, "%s", opts ? opts : "");
    for (tok = strtok_r(obuf, " \t", &save); tok && ac < 18;
         tok = strtok_r(NULL, " \t", &save))
        argv[ac++] = tok;
    argv[ac++] = (char *)a2;
    argv[ac] = NULL;
    rc = tool_exec(argv);
    if (rc != 0)
        fprintf(stderr, "run_ffmpeg: exit code %d\n", rc);
    return rc;
#endif
}

int invfs_jxl_compress(const uint8_t *jpeg, size_t jpeg_len,
                       uint8_t **jxl_out, size_t *jxl_len)
{
#ifdef _WIN32
    char jpg_tmp[256], jxl_tmp[256];
    static const char *cjxl = "D:\\VFS\\tools\\jxl\\x64-windows-static\\bin\\cjxl.exe";
    FILE *f;
    long sz;

    _snprintf_s(jpg_tmp, sizeof jpg_tmp, _TRUNCATE, "%s\\%d_tmp.jpg",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(jxl_tmp, sizeof jxl_tmp, _TRUNCATE, "%s\\%d_tmp.jxl",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    f = fopen(jpg_tmp, "wb");
    if (!f) return -1;
    fwrite(jpeg, 1, jpeg_len, f);
    fclose(f);

    /* lossless JPEG transcode: JXL stores the original JPEG bitstream
     * (boxes), djxl reconstructs the exact same JPEG bytes (1:1 invariant) */
    if (run_tool(cjxl, jpg_tmp, jxl_tmp, "--lossless_jpeg=1") != 0) {
        remove(jpg_tmp); remove(jxl_tmp);
        return -1;
    }
    f = fopen(jxl_tmp, "rb");
    if (!f) { remove(jpg_tmp); remove(jxl_tmp); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    *jxl_out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*jxl_out) { fclose(f); remove(jpg_tmp); remove(jxl_tmp); return -1; }
    if (fread(*jxl_out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*jxl_out); fclose(f); remove(jpg_tmp); remove(jxl_tmp); return -1;
    }
    fclose(f);
    *jxl_len = (size_t)sz;
    remove(jpg_tmp); remove(jxl_tmp);
    return 0;
#else
    char dir[64], in[128], out[128];
    int rc;

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(in, sizeof in, "%s/in.jpg", dir);
    snprintf(out, sizeof out, "%s/out.jxl", dir);
    if (tool_write(in, jpeg, jpeg_len) != 0) { rmdir(dir); return -1; }

    /* lossless JPEG transcode: JXL stores the original JPEG bitstream
     * (boxes), djxl reconstructs the exact same JPEG bytes (1:1 invariant) */
    rc = run_tool("cjxl", in, out, "--lossless_jpeg=1");
    if (rc != 0 || tool_slurp_out(out, jxl_out, jxl_len) != 0) {
        tool_rm(dir, "in.jpg"); tool_rm(dir, "out.jxl"); rmdir(dir);
        return -1;
    }
    tool_rm(dir, "in.jpg"); tool_rm(dir, "out.jxl"); rmdir(dir);
    return 0;
#endif
}

int invfs_jxl_decompress(const uint8_t *jxl, size_t jxl_len,
                         uint8_t **jpg_out, size_t *jpg_len)
{
#ifdef _WIN32
    char jxl_tmp[256], jpg_tmp[256];
    static const char *djxl = "D:\\VFS\\tools\\jxl\\x64-windows-static\\bin\\djxl.exe";
    FILE *f;
    long sz;

    _snprintf_s(jxl_tmp, sizeof jxl_tmp, _TRUNCATE, "%s\\%d_tmp2.jxl",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(jpg_tmp, sizeof jpg_tmp, _TRUNCATE, "%s\\%d_tmp2.jpg",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    f = fopen(jxl_tmp, "wb");
    if (!f) return -1;
    fwrite(jxl, 1, jxl_len, f);
    fclose(f);

    if (run_tool(djxl, jxl_tmp, jpg_tmp, "") != 0) {
        remove(jxl_tmp); remove(jpg_tmp);
        return -1;
    }
    f = fopen(jpg_tmp, "rb");
    if (!f) { remove(jxl_tmp); remove(jpg_tmp); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    *jpg_out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*jpg_out) { fclose(f); remove(jxl_tmp); remove(jpg_tmp); return -1; }
    if (fread(*jpg_out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*jpg_out); fclose(f); remove(jxl_tmp); remove(jpg_tmp); return -1;
    }
    fclose(f);
    *jpg_len = (size_t)sz;
    remove(jxl_tmp); remove(jpg_tmp);
    return 0;
#else
    /* djxl picks the output format by extension: a JXL holding a JPEG
     * reconstruction written to "*.jpg" reproduces the original bytes */
    char dir[64], in[128], out[128];
    int rc;

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(in, sizeof in, "%s/in.jxl", dir);
    snprintf(out, sizeof out, "%s/out.jpg", dir);
    if (tool_write(in, jxl, jxl_len) != 0) { rmdir(dir); return -1; }

    rc = run_tool("djxl", in, out, "");
    if (rc != 0 || tool_slurp_out(out, jpg_out, jpg_len) != 0) {
        tool_rm(dir, "in.jxl"); tool_rm(dir, "out.jpg"); rmdir(dir);
        return -1;
    }
    tool_rm(dir, "in.jxl"); tool_rm(dir, "out.jpg"); rmdir(dir);
    return 0;
#endif
}

/* MP3 -> PMP (packMP3, lossless and bit-exact).
 *
 * packMP3 differs from cjxl/MAC in three ways that shape this code:
 *   - it takes ONE path and derives the output name itself (foo.mp3 ->
 *     foo.pmp, next to the input), so run_tool's "in out" form is unusable
 *     and we must know the output name in advance;
 *   - it decides compress-vs-decompress by CONTENT, not extension, so the
 *     same binary and the same argument shape serve both directions;
 *   - it exits 0 on a refused file (readme: "For unrecognized file types no
 *     action is taken"), and MPEG-2/2.5 Layer III really is refused even
 *     though the extension says .mp3. So the exit code proves nothing --
 *     only the presence of the output file does. Verified: a MPEG-2 file
 *     printed "fatal error: file is MPEG-2 LAYER III, not supported" and
 *     still exited 0.
 * Both halves therefore check for the produced file, not the status. */
static int run_packmp3(const char *path)
{
#ifdef _WIN32
    static const char *pmp =
        "D:\\VFS\\packMP3-v1.0g\\packMP3.exe";
    WCHAR wexe[512], wcmd[4096], wpath[512];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    MultiByteToWideChar(CP_UTF8, 0, pmp, -1, wexe, 512);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512);
    /* -np: never block on "press any key" -- this runs with no console.
       -o: overwrite, else packMP3 invents foo_.pmp and we'd read a stale file.
       No -p: warnings must abort. -p relaxes them but the readme is explicit
       that reconstruction is then not guaranteed bit-exact, which would break
       the 1:1 invariant this filesystem exists to hold. */
    swprintf_s(wcmd, 4096, L"\"%s\" -np -o \"%s\"", wexe, wpath);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(wexe, wcmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        fprintf(stderr, "run_packmp3: CreateProcess failed (%lu)\n",
                (unsigned long)GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    char exeb[512];
    char *argv[5];

    argv[0] = (char *)tool_resolve("packMP3", exeb, sizeof exeb);
    argv[1] = (char *)"-np";   /* never block on "press any key" */
    argv[2] = (char *)"-o";    /* overwrite, else foo_.pmp is invented */
    argv[3] = (char *)path;
    argv[4] = NULL;
    return tool_exec(argv);
#endif
}

/* read a whole file into a fresh buffer; returns 0 on success */
static int slurp_file(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    if (!f) return -1;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    *out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*out) { fclose(f); return -1; }
    if (fread(*out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*out); *out = NULL; fclose(f); return -1;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return 0;
}

int invfs_pmp_compress(const uint8_t *mp3, size_t mp3_len,
                       uint8_t **pmp_out, size_t *pmp_len)
{
#ifdef _WIN32
    char mp3_tmp[256], pmp_tmp[256];
    FILE *f;
    int rc;

    _snprintf_s(mp3_tmp, sizeof mp3_tmp, _TRUNCATE, "%s\\%d_tmp.mp3",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(pmp_tmp, sizeof pmp_tmp, _TRUNCATE, "%s\\%d_tmp.pmp",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    /* a leftover .pmp from an earlier file would be read back as this
       file's output, silently storing the wrong audio */
    remove(pmp_tmp);

    f = fopen(mp3_tmp, "wb");
    if (!f) return -1;
    fwrite(mp3, 1, mp3_len, f);
    fclose(f);

    run_packmp3(mp3_tmp);
    rc = slurp_file(pmp_tmp, pmp_out, pmp_len);   /* absence == refused */
    remove(mp3_tmp); remove(pmp_tmp);
    return rc;
#else
    /* packMP3 derives the output name from the input (in.mp3 -> in.pmp);
     * the scratch dir is fresh per call, so a stale blob cannot be misread */
    char dir[64], in[128], out[128];
    int rc;

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(in, sizeof in, "%s/in.mp3", dir);
    snprintf(out, sizeof out, "%s/in.pmp", dir);
    if (tool_write(in, mp3, mp3_len) != 0) { rmdir(dir); return -1; }

    run_packmp3(in);   /* exit 0 even on refusal: only the blob proves it */
    rc = tool_slurp_out(out, pmp_out, pmp_len);
    tool_rm(dir, "in.mp3"); tool_rm(dir, "in.pmp"); rmdir(dir);
    return rc;
#endif
}

int invfs_pmp_decompress(const uint8_t *pmp, size_t pmp_len,
                         uint8_t **mp3_out, size_t *mp3_len)
{
#ifdef _WIN32
    char pmp_tmp[256], mp3_tmp[256];
    FILE *f;
    int rc;

    /* separate names from the compress side: a sweep and a read can run in
       the same process, and _tmp.mp3 is live there */
    _snprintf_s(pmp_tmp, sizeof pmp_tmp, _TRUNCATE, "%s\\%d_tmp2.pmp",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(mp3_tmp, sizeof mp3_tmp, _TRUNCATE, "%s\\%d_tmp2.mp3",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    remove(mp3_tmp);

    f = fopen(pmp_tmp, "wb");
    if (!f) return -1;
    fwrite(pmp, 1, pmp_len, f);
    fclose(f);

    run_packmp3(pmp_tmp);
    rc = slurp_file(mp3_tmp, mp3_out, mp3_len);
    remove(pmp_tmp); remove(mp3_tmp);
    return rc;
#else
    char dir[64], in[128], out[128];
    int rc;

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(in, sizeof in, "%s/in.pmp", dir);
    snprintf(out, sizeof out, "%s/in.mp3", dir);
    if (tool_write(in, pmp, pmp_len) != 0) { rmdir(dir); return -1; }

    run_packmp3(in);
    rc = tool_slurp_out(out, mp3_out, mp3_len);
    tool_rm(dir, "in.pmp"); tool_rm(dir, "in.mp3"); rmdir(dir);
    return rc;
#endif
}

/* FLAC bits-per-sample from STREAMINFO (ffmpeg 8.x silently downconverts
   24-bit FLAC to 16-bit WAV unless an explicit pcm_s*le codec is given). */
static int flac_bits_per_sample(const uint8_t *flac, size_t n)
{
    if (!flac || n < 42 || memcmp(flac, "fLaC", 4)) return 16;
    uint32_t blen = ((uint32_t)flac[5] << 16) | ((uint32_t)flac[6] << 8) | flac[7];
    if ((flac[4] & 0x7F) != 0 || blen < 34 || n < 8u + blen) return 16;
    const uint8_t *b = flac + 8;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[10 + i];
    int bps = (int)((v >> 36) & 0x1F) + 1;
    return (bps >= 8 && bps <= 32) ? bps : 16;
}

int invfs_ape_compress(const uint8_t *flac, size_t flac_len,
                       uint8_t **ape_out, size_t *ape_len)
{
#ifdef _WIN32
    char flac_tmp[256], wav_tmp[256], ape_tmp[256], fopts[64];
    static const char *mac = "D:\\bin\\MAC.exe";
    FILE *f;
    long sz;

    _snprintf_s(flac_tmp, sizeof flac_tmp, _TRUNCATE, "%s\\%d_tmp.flac",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(wav_tmp, sizeof wav_tmp, _TRUNCATE, "%s\\%d_tmp.wav",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(ape_tmp, sizeof ape_tmp, _TRUNCATE, "%s\\%d_tmp.ape",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    f = fopen(flac_tmp, "wb");
    if (!f) return -1;
    fwrite(flac, 1, flac_len, f);
    fclose(f);

    /* FLAC -> WAV (ffmpeg, explicit codec matching the source bit depth —
       ffmpeg 8.x otherwise downconverts 24-bit FLAC to 16-bit WAV),
       then WAV -> APE (MAC.exe -c4000, density profile) */
    int bps = flac_bits_per_sample(flac, flac_len);
    if (bps >= 25)      _snprintf_s(fopts, sizeof fopts, _TRUNCATE, "-y -c:a pcm_s32le");
    else if (bps >= 17) _snprintf_s(fopts, sizeof fopts, _TRUNCATE, "-y -c:a pcm_s24le");
    else if (bps >= 9)  _snprintf_s(fopts, sizeof fopts, _TRUNCATE, "-y -c:a pcm_s16le");
    else                _snprintf_s(fopts, sizeof fopts, _TRUNCATE, "-y -c:a pcm_u8");
    if (run_ffmpeg(flac_tmp, wav_tmp, fopts) != 0 ||
        run_tool(mac, wav_tmp, ape_tmp, "-c4000") != 0) {
        remove(flac_tmp); remove(wav_tmp); remove(ape_tmp);
        return -1;
    }
    f = fopen(ape_tmp, "rb");
    if (!f) { remove(flac_tmp); remove(wav_tmp); remove(ape_tmp); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    *ape_out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*ape_out) { fclose(f); remove(flac_tmp); remove(wav_tmp); remove(ape_tmp); return -1; }
    if (fread(*ape_out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*ape_out); fclose(f); remove(flac_tmp); remove(wav_tmp); remove(ape_tmp); return -1;
    }
    fclose(f);
    *ape_len = (size_t)sz;
    remove(flac_tmp); remove(wav_tmp); remove(ape_tmp);
    return 0;
#else
    /* FLAC -> WAV (ffmpeg, explicit codec matching the source bit depth),
     * WAV -> APE (mac -c4000) */
    char dir[64], fin[128], wmid[128], aout[128], fopts[64];
    int bps = flac_bits_per_sample(flac, flac_len);

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(fin, sizeof fin, "%s/in.flac", dir);
    snprintf(wmid, sizeof wmid, "%s/mid.wav", dir);
    snprintf(aout, sizeof aout, "%s/out.ape", dir);
    if (tool_write(fin, flac, flac_len) != 0) { rmdir(dir); return -1; }

    if (bps >= 25)      snprintf(fopts, sizeof fopts, "-y -c:a pcm_s32le");
    else if (bps >= 17) snprintf(fopts, sizeof fopts, "-y -c:a pcm_s24le");
    else if (bps >= 9)  snprintf(fopts, sizeof fopts, "-y -c:a pcm_s16le");
    else                snprintf(fopts, sizeof fopts, "-y -c:a pcm_u8");
    if (run_ffmpeg(fin, wmid, fopts) != 0 ||
        run_tool("mac", wmid, aout, "-c4000") != 0 ||
        tool_slurp_out(aout, ape_out, ape_len) != 0) {
        tool_rm(dir, "in.flac"); tool_rm(dir, "mid.wav");
        tool_rm(dir, "out.ape"); rmdir(dir);
        return -1;
    }
    tool_rm(dir, "in.flac"); tool_rm(dir, "mid.wav");
    tool_rm(dir, "out.ape"); rmdir(dir);
    return 0;
#endif
}

int invfs_ape_decompress(const uint8_t *ape, size_t ape_len,
                         uint8_t **flac_out, size_t *flac_len)
{
#ifdef _WIN32
    char ape_tmp[256], wav_tmp[256], flac_tmp[256];
    static const char *mac = "D:\\bin\\MAC.exe";
    FILE *f;
    long sz;

    _snprintf_s(ape_tmp, sizeof ape_tmp, _TRUNCATE, "%s\\%d_tmp2.ape",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(wav_tmp, sizeof wav_tmp, _TRUNCATE, "%s\\%d_tmp2.wav",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(flac_tmp, sizeof flac_tmp, _TRUNCATE, "%s\\%d_tmp2.flac",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    f = fopen(ape_tmp, "wb");
    if (!f) return -1;
    fwrite(ape, 1, ape_len, f);
    fclose(f);

    /* APE -> WAV (MAC.exe -d), WAV -> FLAC (ffmpeg) */
    if (run_tool(mac, ape_tmp, wav_tmp, "-d") != 0 ||
        run_ffmpeg(wav_tmp, flac_tmp, "-y") != 0) {
        remove(ape_tmp); remove(wav_tmp); remove(flac_tmp);
        return -1;
    }
    f = fopen(flac_tmp, "rb");
    if (!f) { remove(ape_tmp); remove(wav_tmp); remove(flac_tmp); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    *flac_out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*flac_out) { fclose(f); remove(ape_tmp); remove(wav_tmp); remove(flac_tmp); return -1; }
    if (fread(*flac_out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*flac_out); fclose(f); remove(ape_tmp); remove(wav_tmp); remove(flac_tmp); return -1;
    }
    fclose(f);
    *flac_len = (size_t)sz;
    remove(ape_tmp); remove(wav_tmp); remove(flac_tmp);
    return 0;
#else
    /* APE -> WAV (mac -d), WAV -> FLAC (ffmpeg) */
    char dir[64], ain[128], wmid[128], fout[128];

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(ain, sizeof ain, "%s/in.ape", dir);
    snprintf(wmid, sizeof wmid, "%s/mid.wav", dir);
    snprintf(fout, sizeof fout, "%s/out.flac", dir);
    if (tool_write(ain, ape, ape_len) != 0) { rmdir(dir); return -1; }

    if (run_tool("mac", ain, wmid, "-d") != 0 ||
        run_ffmpeg(wmid, fout, "-y") != 0 ||
        tool_slurp_out(fout, flac_out, flac_len) != 0) {
        tool_rm(dir, "in.ape"); tool_rm(dir, "mid.wav");
        tool_rm(dir, "out.flac"); rmdir(dir);
        return -1;
    }
    tool_rm(dir, "in.ape"); tool_rm(dir, "mid.wav");
    tool_rm(dir, "out.flac"); rmdir(dir);
    return 0;
#endif
}

/* APE -> WAV in memory (MAC.exe -d only; no ffmpeg re-compress step).
   Used by the FLAC-recipe path: the WAV feeds flacx_rebuild which
   reproduces the ORIGINAL FLAC bytes bit-exactly from the recipe. */
int invfs_ape_to_wav(const uint8_t *ape, size_t ape_len,
                     uint8_t **wav_out, size_t *wav_len)
{
#ifdef _WIN32
    char ape_tmp[256], wav_tmp[256];
    static const char *mac = "D:\\bin\\MAC.exe";
    FILE *f;
    long sz;

    _snprintf_s(ape_tmp, sizeof ape_tmp, _TRUNCATE, "%s\\%d_tmp3.ape",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(wav_tmp, sizeof wav_tmp, _TRUNCATE, "%s\\%d_tmp3.wav",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    f = fopen(ape_tmp, "wb");
    if (!f) return -1;
    fwrite(ape, 1, ape_len, f);
    fclose(f);

    /* APE -> WAV (MAC.exe -d) */
    if (run_tool(mac, ape_tmp, wav_tmp, "-d") != 0) {
        remove(ape_tmp); remove(wav_tmp);
        return -1;
    }
    f = fopen(wav_tmp, "rb");
    if (!f) { remove(ape_tmp); remove(wav_tmp); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    *wav_out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*wav_out) { fclose(f); remove(ape_tmp); remove(wav_tmp); return -1; }
    if (fread(*wav_out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*wav_out); fclose(f); remove(ape_tmp); remove(wav_tmp); return -1;
    }
    fclose(f);
    *wav_len = (size_t)sz;
    remove(ape_tmp); remove(wav_tmp);
    return 0;
#else
    /* APE -> WAV (mac -d only; the WAV feeds flacx_rebuild) */
    char dir[64], ain[128], wout[128];

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(ain, sizeof ain, "%s/in.ape", dir);
    snprintf(wout, sizeof wout, "%s/out.wav", dir);
    if (tool_write(ain, ape, ape_len) != 0) { rmdir(dir); return -1; }

    if (run_tool("mac", ain, wout, "-d") != 0 ||
        tool_slurp_out(wout, wav_out, wav_len) != 0) {
        tool_rm(dir, "in.ape"); tool_rm(dir, "out.wav"); rmdir(dir);
        return -1;
    }
    tool_rm(dir, "in.ape"); tool_rm(dir, "out.wav"); rmdir(dir);
    return 0;
#endif
}

/* FLAC transcode (density profile): store the PCM as an APE blob in inode
   `name` (algo=FLACR, file_size = original FLAC size) and the frame recipe
   in sibling inode `name!recipe`. Reading `name` reproduces the original
   FLAC bytes bit-exactly: APE->WAV->flacx_rebuild(recipe). */
uint64_t vol_create_flac_file(invfs_volume *v, const char *name,
                              const uint8_t *flac, size_t flac_len)
{
#ifdef _WIN32
    uint8_t *recipe = NULL, *ape = NULL;
    size_t rlen = 0, ape_len = 0;
    flacx_cover *covers = NULL;
    uint32_t ncv = 0, i;
    uint64_t a;

    if (name_too_long_for_children(name)) return 0;
    if (flacx_extract(flac, flac_len, &recipe, &rlen, &covers, &ncv) != 0) {
        fprintf(stderr, "[vol] flacx_extract failed for %s\n", name);
        return 0;
    }
    if (invfs_ape_compress(flac, flac_len, &ape, &ape_len) != 0) {
        fprintf(stderr, "[vol] APE compress failed for %s\n", name);
        for (i = 0; i < ncv; i++) free(covers[i].data);
        free(covers); free(recipe);
        return 0;
    }
    /* invariant: transcode ONLY if it actually pays off — otherwise the
       original FLAC is kept (git-safe). On synthetic/24-bit material APE
       -c4000 often loses to FLAC -8; on real 16-bit CD music it wins by
       ~2-6% (B-series host benchmarks: APE = 91.1% of FLAC size). */
    {
        size_t cover_bytes = 0;
        for (i = 0; i < ncv; i++) cover_bytes += covers[i].len;
        if (!getenv("INVFS_FORCE_FLACR") && ape_len + rlen + cover_bytes >= flac_len) {
            if (getenv("INVFS_DEBUG"))
                fprintf(stderr, "[vol] %s: APE+recipe+covers %zu+%zu+%zu >= FLAC %zu — keep original\n",
                        name, ape_len, rlen, cover_bytes, flac_len);
            for (i = 0; i < ncv; i++) free(covers[i].data);
            free(covers); free(recipe); free(ape);
            return 0;
        }
    }
    /* Children first, the name-owning record last.
     *
     * This used to write the APE inode under `name` and only then the recipe.
     * That order is not merely leaky, it is destructive: the sweep tombstones
     * the original FLAC as soon as this returns non-zero, so a failure after
     * the APE landed left `name` pointing at APE-compressed PCM with no
     * recipe to rebuild the FLAC container from -- the file could no longer
     * be reconstructed at all. Writing the payload children first means the
     * name only ever flips to the transcoded form once everything needed to
     * decode it is already durable. tar/gz/png always did it this way. */
    char rname[272];
    _snprintf_s(rname, sizeof rname, _TRUNCATE, "%s!recipe", name);
    /* store the recipe ZSTD-compressed (repetitive frame headers shrink
       ~2x); fall back to raw if it does not compress */
    uint64_t b = 0;
    {
        size_t cbound = ZSTD_compressBound(rlen);
        uint8_t *rc = (uint8_t *)malloc(cbound ? cbound : 1);
        if (rc) {
            size_t clen = ZSTD_compress(rc, cbound, recipe, rlen, 19);
            if (!ZSTD_isError(clen) && clen < rlen) {
                b = vol_create_blob_file(v, rname, rc, clen,
                                         (uint64_t)rlen, INVFS_ALGO_ZSTD);
                free(rc);
            } else {
                free(rc);
            }
        }
        if (!b)
            b = vol_create_blob_file(v, rname, recipe, rlen,
                                     (uint64_t)rlen, INVFS_ALGO_NONE);
    }
    if (!b) {
        fprintf(stderr, "[vol] recipe inode failed for %s\n", name);
        for (i = 0; i < ncv; i++) free(covers[i].data);
        free(covers); free(recipe); free(ape);
        return vol_transcode_abort(v, name);
    }
    /* covers as separate inodes "name!coverN" — identical covers across
       tracks become identical segments and are block-deduped. Only kind=0
       slots carry payloads (kind=1 zero-PADDING has no data). */
    uint32_t di = 0;
    for (i = 0; i < ncv; i++) {
        if (!covers[i].data) continue;   /* kind=1 zero-slot */
        char cn[288];
        _snprintf_s(cn, sizeof cn, _TRUNCATE, "%s!cover%u", name, di++);
        uint64_t ci = vol_create_blob_file(v, cn, covers[i].data, covers[i].len,
                                           (uint64_t)covers[i].len, INVFS_ALGO_NONE);
        if (!ci) {
            /* the recipe addresses this slot by name, so a dropped cover is a
               FLAC that cannot be rebuilt -- a failed transcode, not a warning
               to carry forward */
            fprintf(stderr, "[vol] cover inode failed for %s!cover%u\n", name, di - 1);
            for (i = 0; i < ncv; i++) free(covers[i].data);
            free(covers); free(recipe); free(ape);
            return vol_transcode_abort(v, name);
        }
    }
    a = vol_create_blob_file(v, name, ape, ape_len,
                             (uint64_t)flac_len, INVFS_ALGO_FLACR);
    for (i = 0; i < ncv; i++) free(covers[i].data);
    free(covers);
    free(recipe);
    free(ape);
    if (!a) {
        fprintf(stderr, "[vol] APE inode failed for %s\n", name);
        return vol_transcode_abort(v, name);
    }
    return a;
#else
    (void)v; (void)name; (void)flac; (void)flac_len;
    return 0;
#endif
}

/* TAR container (density profile): split the archive into members without
   interpreting them (header bytes kept verbatim in the IVFT recipe), store
   each payload in sibling "name!partN" compressed by its best algorithm,
   rebuild = byte-for-byte reassembly (invariant 1:1). */
#define TARX_MAX_PARTS 2048
uint64_t vol_create_tar_file(invfs_volume *v, const char *name,
                             const uint8_t *tar, size_t tar_len)
{
    tarx_member *members = NULL;
    size_t n = 0, trailer_len = 0, i;
    uint8_t *trailer = NULL, *recipe = NULL;
    size_t rlen = 0;
    uint64_t total = 0;

    if (name_too_long_for_children(name)) return 0;
    if (tarx_extract(tar, tar_len, &members, &n, &trailer, &trailer_len) != 0) {
        fprintf(stderr, "[vol] tarx_extract failed for %s\n", name);
        return 0;
    }
    if (n == 0 || n > TARX_MAX_PARTS) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[vol] %s: %zu members — keep original\n", name, n);
        free(members); free(trailer);
        return 0;
    }
    if (tarx_build_recipe(members, n, trailer, trailer_len, &recipe, &rlen) != 0) {
        free(members); free(trailer);
        return 0;
    }

    /* parts: "name!partN", each compressed by its best algorithm */
    total = (uint64_t)rlen;
    for (i = 0; i < n; i++) {
        size_t dlen = (size_t)members[i].data_len;
        size_t plen = dlen + (members[i].pad_kind ? 0 : (size_t)members[i].pad_len);
        uint8_t *pd = (uint8_t *)malloc(plen ? plen : 1);
        if (!pd) { free(recipe); free(members); free(trailer);
                   return vol_transcode_abort(v, name); }
        memcpy(pd, tar + members[i].data_off, dlen);
        if (!members[i].pad_kind && members[i].pad_len)
            memcpy(pd + dlen, tar + members[i].data_off + dlen, members[i].pad_len);
        size_t bound = ZSTD_compressBound(plen);
        uint8_t *c = (uint8_t *)malloc(bound);
        if (!c) { free(pd); free(recipe); free(members); free(trailer);
                  return vol_transcode_abort(v, name); }
        size_t cl = ZSTD_compress(c, bound, pd, plen, 19);
        uint32_t algo = INVFS_ALGO_ZSTD;
        if (ZSTD_isError(cl) || cl >= plen) {
            algo = INVFS_ALGO_NONE;
            memcpy(c, pd, plen);
            cl = plen;
        }
        char pn[320];
        snprintf(pn, sizeof pn, "%s!part%u", name, (unsigned)i);
        uint64_t pino = vol_create_blob_file(v, pn, c, cl, (uint64_t)plen, algo);
        if (!pino) {
            fprintf(stderr, "[vol] part inode failed for %s\n", pn);
            free(c); free(pd); free(recipe); free(members); free(trailer);
            return vol_transcode_abort(v, name);
        }
        total += (uint64_t)cl;
        free(c); free(pd);
    }

    /* guard: transcode only when smaller (invariant: never lose) */
    if (total >= tar_len) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[vol] %s: parts+recipe %llu >= tar %zu — keep original\n",
                    name, (unsigned long long)total, tar_len);
        free(recipe); free(members); free(trailer);
        return vol_transcode_abort(v, name);
    }

    /* recipe blob: [0x01][zstd] or [0x00][raw] */
    size_t bound = ZSTD_compressBound(rlen);
    uint8_t *rc = (uint8_t *)malloc(bound + 1);
    if (!rc) { free(recipe); free(members); free(trailer);
               return vol_transcode_abort(v, name); }
    size_t rbl = 0;
    size_t rcl = ZSTD_compress(rc + 1, bound, recipe, rlen, 19);
    if (!ZSTD_isError(rcl) && rcl < rlen) { rc[0] = 1; rbl = rcl + 1; }
    else { rc[0] = 0; memcpy(rc + 1, recipe, rlen); rbl = rlen + 1; }
    uint64_t ino = vol_create_blob_file(v, name, rc, rbl,
                                        (uint64_t)tar_len, INVFS_ALGO_TARR);
    if (!ino) {
        fprintf(stderr, "[vol] tar inode failed for %s\n", name);
        free(rc); free(recipe); free(members); free(trailer);
        return vol_transcode_abort(v, name);
    }
    free(rc); free(recipe); free(members); free(trailer);
    return ino;
}

/* GZIP container (density profile): split a .tar.gz into tar members
   (siblings "name!partN", each compressed by its best algorithm) plus a
   gzip-recipe: [gzip header bytes][deflate params (level,memLevel)][crc32]
   [isize] + IVFT (tar structure). Rebuild reproduces the deflate stream
   BIT-EXACTLY with a vanilla zlib replica (windowBits=-15) — verified at
   transcode time against the original stream; non-zlib encoders (7-Zip,
   java) fail the probe and keep the original (invariant). */
uint64_t vol_create_gz_file(invfs_volume *v, const char *name,
                            const uint8_t *gz, size_t gz_len)
{
    if (gz_len < 18 || gz[0] != 0x1F || gz[1] != 0x8B) return 0;
    if (name_too_long_for_children(name)) return 0;
    unsigned flg = gz[3];
    size_t hlen = 10;
    if (flg & 0x04) { unsigned xl = gz[hlen] | (gz[hlen + 1] << 8); hlen += 2 + xl; }
    if (flg & 0x08) { while (gz[hlen]) hlen++; hlen++; }
    if (flg & 0x10) { while (gz[hlen]) hlen++; hlen++; }
    if (flg & 0x02) hlen += 2;
    if (hlen + 8 >= gz_len) return 0;
    size_t stream_len = gz_len - hlen - 8;
    unsigned crc_stored = (unsigned)gz[gz_len - 8] | ((unsigned)gz[gz_len - 7] << 8) |
                          ((unsigned)gz[gz_len - 6] << 16) | ((unsigned)gz[gz_len - 5] << 24);
    unsigned isize = (unsigned)gz[gz_len - 4] | ((unsigned)gz[gz_len - 3] << 8) |
                     ((unsigned)gz[gz_len - 2] << 16) | ((unsigned)gz[gz_len - 1] << 24);

    /* inflate raw deflate (zlib) */
    z_stream in;
    memset(&in, 0, sizeof in);
    if (inflateInit2(&in, -15) != Z_OK) return 0;
    size_t cap = (size_t)isize + (size_t)isize / 2 + 64;
    if (cap < 1024) cap = 1024;
    uint8_t *tar = (uint8_t *)malloc(cap);
    if (!tar) { inflateEnd(&in); return 0; }
    in.next_in = (Bytef *)(gz + hlen);
    in.avail_in = (uInt)(stream_len > 0x7FFFFFFF ? 0x7FFFFFFF : stream_len);
    in.next_out = tar;
    in.avail_out = (uInt)(cap > 0x7FFFFFFF ? 0x7FFFFFFF : cap);
    int rr = inflate(&in, Z_FINISH);
    inflateEnd(&in);
    if (rr != Z_STREAM_END) { free(tar); return 0; }
    size_t tar_len = (size_t)in.total_out;

    /* members */
    tarx_member *members = NULL;
    size_t n = 0, trailer_len = 0;
    uint8_t *trailer = NULL, *recipe = NULL;
    size_t rlen = 0, i;
    if (n == 0 && (tar_len < 512 || memcmp(tar + 257, "ustar", 5) != 0)) {
        /* not a tar inside: keep original */
        free(tar); return 0;
    }
    if (tarx_extract(tar, tar_len, &members, &n, &trailer, &trailer_len) != 0 ||
        n == 0 || n > TARX_MAX_PARTS) {
        free(tar); free(members); free(trailer);
        return 0;
    }
    (void)tar_len;

    /* probe: find (level, memLevel) reproducing the original stream */
    int plevel = 0, pmem = 0, found = 0;
    for (int lv = 1; lv <= 9 && !found; lv++) {
        for (int mm = 7; mm <= 9 && !found; mm++) {
            z_stream s;
            memset(&s, 0, sizeof s);
            if (deflateInit2(&s, lv, Z_DEFLATED, -15, mm,
                             Z_DEFAULT_STRATEGY) != Z_OK) continue;
            size_t bound = deflateBound(&s, (uLong)tar_len);
            uint8_t *re = (uint8_t *)malloc(bound);
            if (!re) { deflateEnd(&s); continue; }
            s.next_in = tar;
            s.avail_in = (uInt)(tar_len > 0x7FFFFFFF ? 0x7FFFFFFF : tar_len);
            s.next_out = re;
            s.avail_out = (uInt)bound;
            int r2 = deflate(&s, Z_FINISH);
            size_t re_len = (size_t)s.total_out;
            deflateEnd(&s);
            if (r2 == Z_STREAM_END && re_len == stream_len &&
                memcmp(re, gz + hlen, stream_len) == 0) {
                plevel = lv; pmem = mm; found = 1;
            }
            free(re);
        }
    }
    if (!found) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[vol] %s: deflate not reproducible — keep original\n", name);
        free(tar); free(members); free(trailer);
        return 0;
    }

    /* parts */
    uint64_t total = (uint64_t)rlen;
    for (i = 0; i < n; i++) {
        size_t dlen = (size_t)members[i].data_len;
        size_t plen = dlen + (members[i].pad_kind ? 0 : (size_t)members[i].pad_len);
        uint8_t *pd = (uint8_t *)malloc(plen ? plen : 1);
        if (!pd) { free(tar); free(members); free(trailer); free(recipe);
                   return vol_transcode_abort(v, name); }
        memcpy(pd, tar + members[i].data_off, dlen);
        if (!members[i].pad_kind && members[i].pad_len)
            memcpy(pd + dlen, tar + members[i].data_off + dlen, members[i].pad_len);
        size_t bound = ZSTD_compressBound(plen);
        uint8_t *c = (uint8_t *)malloc(bound);
        if (!c) { free(pd); free(tar); free(members); free(trailer); free(recipe);
                  return vol_transcode_abort(v, name); }
        size_t cl = ZSTD_compress(c, bound, pd, plen, 19);
        uint32_t algo = INVFS_ALGO_ZSTD;
        if (ZSTD_isError(cl) || cl >= plen) {
            algo = INVFS_ALGO_NONE;
            memcpy(c, pd, plen); cl = plen;
        }
        char pn[320];
        snprintf(pn, sizeof pn, "%s!part%u", name, (unsigned)i);
        uint64_t pino = vol_create_blob_file(v, pn, c, cl, (uint64_t)plen, algo);
        if (!pino) { free(c); free(pd); free(tar); free(members); free(trailer); free(recipe);
                     return vol_transcode_abort(v, name); }
        total += (uint64_t)cl;
        free(c); free(pd);
    }

    /* gzip recipe: [IVGZ][ver][level][mem][crc32][isize][hlen(2)][header] + IVFT */
    if (tarx_build_recipe(members, n, trailer, trailer_len, &recipe, &rlen) != 0) {
        free(tar); free(members); free(trailer);
        return vol_transcode_abort(v, name);
    }
    total += (uint64_t)rlen;
    /* guard: transcode only when smaller */
    if (total >= gz_len) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[vol] %s: parts+recipe %llu >= gz %zu — keep original\n",
                    name, (unsigned long long)total, gz_len);
        free(tar); free(members); free(trailer); free(recipe);
        return vol_transcode_abort(v, name);
    }
    size_t pre = 4 + 1 + 1 + 1 + 4 + 4 + 2 + hlen;
    uint8_t *gzr = (uint8_t *)malloc(pre + rlen);
    if (!gzr) { free(tar); free(members); free(trailer); free(recipe);
                return vol_transcode_abort(v, name); }
    size_t o = 0;
    memcpy(gzr + o, "IVGZ", 4); o += 4;
    gzr[o++] = 1;
    gzr[o++] = (uint8_t)plevel;
    gzr[o++] = (uint8_t)pmem;
    gzr[o++] = (uint8_t)(crc_stored & 0xFF); gzr[o++] = (uint8_t)((crc_stored >> 8) & 0xFF);
    gzr[o++] = (uint8_t)((crc_stored >> 16) & 0xFF); gzr[o++] = (uint8_t)((crc_stored >> 24) & 0xFF);
    gzr[o++] = (uint8_t)(isize & 0xFF); gzr[o++] = (uint8_t)((isize >> 8) & 0xFF);
    gzr[o++] = (uint8_t)((isize >> 16) & 0xFF); gzr[o++] = (uint8_t)((isize >> 24) & 0xFF);
    gzr[o++] = (uint8_t)(hlen & 0xFF); gzr[o++] = (uint8_t)((hlen >> 8) & 0xFF);
    memcpy(gzr + o, gz, hlen); o += hlen;
    memcpy(gzr + o, recipe, rlen); o += rlen;

    /* recipe blob: [0x01][zstd] or [0x00][raw] */
    size_t bound = ZSTD_compressBound(o);
    uint8_t *rc = (uint8_t *)malloc(bound + 1);
    if (!rc) { free(tar); free(members); free(trailer); free(recipe); free(gzr);
               return vol_transcode_abort(v, name); }
    size_t rbl = 0;
    size_t rcl = ZSTD_compress(rc + 1, bound, gzr, o, 19);
    if (!ZSTD_isError(rcl) && rcl < o) { rc[0] = 1; rbl = rcl + 1; }
    else { rc[0] = 0; memcpy(rc + 1, gzr, o); rbl = o + 1; }
    uint64_t ino = vol_create_blob_file(v, name, rc, rbl,
                                        (uint64_t)gz_len, INVFS_ALGO_GZR);
    if (!ino) {
        fprintf(stderr, "[vol] gz inode failed for %s\n", name);
        free(rc); free(gzr); free(tar); free(members); free(trailer); free(recipe);
        return vol_transcode_abort(v, name);
    }
    free(rc); free(gzr); free(tar); free(members); free(trailer); free(recipe);
    return ino;
}

/* ---- PNG Repack ---- */

/* zlib inflate wrapper used by pngx (windowBits=15: zlib wrapper) */
static int png_inflate(const unsigned char *in, size_t in_len,
                       unsigned char **out, size_t *out_len);

/* djxl: JXL blob -> PNG -> parse -> unfilter -> pixels. Returns 0 on ok. */
static int invfs_png_from_jxl(invfs_volume *v, uint64_t jxl_inode,
                              uint8_t **rgb, size_t *rgb_len)
{
#ifdef _WIN32
    uint8_t *jxl = NULL; size_t jxl_len = 0;
    if (vol_read_inode(v, jxl_inode, 0, &jxl, &jxl_len) != 0) return -1;
    char jx_tmp[256], dn_tmp[256];
    static const char *djxl = "D:\\VFS\\tools\\jxl\\x64-windows-static\\bin\\djxl.exe";
    _snprintf_s(jx_tmp, sizeof jx_tmp, _TRUNCATE, "%s\\%d_pn.jxl",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(dn_tmp, sizeof dn_tmp, _TRUNCATE, "%s\\%d_pn.png",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    FILE *f = fopen(jx_tmp, "wb");
    if (!f) { free(jxl); return -1; }
    fwrite(jxl, 1, jxl_len, f);
    fclose(f);
    free(jxl);
    extern int run_tool(const char *exe, const char *in, const char *out,
                        const char *opts);
    if (run_tool(djxl, jx_tmp, dn_tmp, "") != 0) {
        remove(jx_tmp); remove(dn_tmp); return -1;
    }
    f = fopen(dn_tmp, "rb");
    uint8_t *dn = NULL; long dn_len = 0;
    if (f) {
        fseek(f, 0, SEEK_END); dn_len = ftell(f); fseek(f, 0, SEEK_SET);
        dn = (uint8_t *)malloc(dn_len ? (size_t)dn_len : 1);
        if (fread(dn, 1, (size_t)dn_len, f) != (size_t)dn_len) { free(dn); dn = NULL; }
        fclose(f);
    }
    remove(jx_tmp); remove(dn_tmp);
    if (!dn) return -1;
    pngx_info di;
    memset(&di, 0, sizeof di);
    int r = pngx_extract(dn, (size_t)dn_len, NULL, 0, png_inflate, &di);
    free(dn);
    if (r != 0) return -1;
    *rgb = di.rgb; *rgb_len = di.rgb_len;
    di.rgb = NULL; di.rgb_len = 0;
    pngx_free(&di);
    return 0;
#else
    (void)v; (void)jxl_inode; (void)rgb; (void)rgb_len;
    return -1;
#endif
}

/* miniz tdefl wrapper: zlib-wrapped deflate with level 1..10 */
static int mz_tdefl_compress(const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t out_cap, int level,
                             size_t *out_len)
{
    mz_uint flags = tdefl_create_comp_flags_from_zip_params(
        level < 0 ? 0 : (level > 10 ? 10 : level), 15, 0);
    flags |= TDEFL_WRITE_ZLIB_HEADER | TDEFL_COMPUTE_ADLER32;
    mz_uint n = tdefl_compress_mem_to_mem(out, out_cap, in, in_len, flags);
    if (!n) return -1;
    *out_len = n;
    return 0;
}

static int png_inflate(const unsigned char *in, size_t in_len,
                       unsigned char **out, size_t *out_len)
{
    z_stream s;
    memset(&s, 0, sizeof s);
    inflateInit2(&s, 15);
    size_t cap = in_len * 8 + 4096;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) { inflateEnd(&s); return -1; }
    s.next_in = (Bytef *)in;
    s.avail_in = (uInt)(in_len > 0x7FFFFFFF ? 0x7FFFFFFF : in_len);
    s.next_out = buf;
    s.avail_out = (uInt)(cap > 0x7FFFFFFF ? 0x7FFFFFFF : cap);
    int r = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    if (r != Z_STREAM_END) { free(buf); return -1; }
    *out = buf;
    *out_len = (size_t)s.total_out;
    return 0;
}

/* JXL CLI helper: run cjxl/djxl via temp files (same pattern as APE). */
static int jxl_tool(const char *exe, const char *in, const char *out,
                    const char *opts)
{
    extern int run_tool(const char *exe, const char *in, const char *out,
                        const char *opts);
    return run_tool(exe, in, out, opts);
}

/* PNG -> JXL lossless blob + IVPN recipe (bit-exact via deflate replica
   and refilter). Verified end-to-end at transcode time: the whole
   rebuild path (djxl -> parse -> refilter -> deflate) is executed and
   the resulting PNG compared byte-for-byte with the original. */
uint64_t vol_create_png_file(invfs_volume *v, const char *name,
                             const uint8_t *png, size_t png_len)
{
#ifdef _WIN32
    if (png_len < 33 || memcmp(png, "\x89PNG\r\n\x1a\n", 8) != 0) return 0;
    if (name_too_long_for_children(name)) return 0;
    pngx_info info;
    memset(&info, 0, sizeof info);
    if (pngx_extract(png, png_len, NULL, 0, png_inflate, &info) != 0) {
        return 0;
    }
    if (info.bitdepth != 8 || info.interlace != 0 || info.bpp == 0) {
        pngx_free(&info); return 0;   /* v1 guard: 8-bit non-interlaced */
    }
    if (info.nrows == 0 || info.rgb_len == 0) { pngx_free(&info); return 0; }

    /* spool.png: original filtered rows with STORED IDAT (level 0) */
    uint8_t *spool = NULL;
    size_t spool_len = 0;
    {
        z_stream s;
        memset(&s, 0, sizeof s);
        if (deflateInit2(&s, 0, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            pngx_free(&info); return 0;
        }
        size_t bound = deflateBound(&s, (uLong)info.filtered_len);
        uint8_t *stream = (uint8_t *)malloc(bound);
        s.next_in = info.filtered;
        s.avail_in = (uInt)(info.filtered_len > 0x7FFFFFFF ? 0x7FFFFFFF : info.filtered_len);
        s.next_out = stream;
        s.avail_out = (uInt)bound;
        int r = deflate(&s, Z_FINISH);
        size_t slen = (size_t)s.total_out;
        deflateEnd(&s);
        if (r != Z_STREAM_END) { free(stream); pngx_free(&info); return 0; }
        /* build spool png: sig + IHDR + IDAT(stored) + IEND */
        uint8_t ihdr[13];
        wr32v(ihdr, info.width);
        wr32v(ihdr + 4, info.height);
        ihdr[8] = info.bitdepth; ihdr[9] = info.colortype; ihdr[10] = 0;
        ihdr[11] = 0; ihdr[12] = info.interlace;
        size_t cap = 8 + 25 + slen + 12;
        spool = (uint8_t *)malloc(cap);
        if (!spool) { free(stream); pngx_free(&info); return 0; }
        size_t o = 0;
        memcpy(spool + o, "\x89PNG\r\n\x1a\n", 8); o += 8;
        if (pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IHDR", ihdr, 13) ||
            pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IDAT", stream, slen) ||
            pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IEND", NULL, 0)) {
            free(stream); free(spool); pngx_free(&info); return 0;
        }
        spool_len = o;
        free(stream);
    }

    /* cjxl: spool.png -> JXL (lossless, effort 7) */
    char sp_tmp[256], jx_tmp[256], dn_tmp[256];
    static const char *cjxl = "D:\\VFS\\tools\\jxl\\x64-windows-static\\bin\\cjxl.exe";
    static const char *djxl = "D:\\VFS\\tools\\jxl\\x64-windows-static\\bin\\djxl.exe";
    _snprintf_s(sp_tmp, sizeof sp_tmp, _TRUNCATE, "%s\\%d_spool.png",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(jx_tmp, sizeof jx_tmp, _TRUNCATE, "%s\\%d_tmp.jxl",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(dn_tmp, sizeof dn_tmp, _TRUNCATE, "%s\\%d_dn.png",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    FILE *f = fopen(sp_tmp, "wb");
    if (!f) { free(spool); pngx_free(&info); return 0; }
    fwrite(spool, 1, spool_len, f);
    fclose(f);
    free(spool);
    if (jxl_tool(cjxl, sp_tmp, jx_tmp, "-d 0 -e 7") != 0) {
        remove(sp_tmp); remove(jx_tmp); remove(dn_tmp);
        pngx_free(&info); return 0;
    }
    /* djxl: verify the round-trip produces identical pixels */
    if (jxl_tool(djxl, jx_tmp, dn_tmp, "") != 0) {
        remove(sp_tmp); remove(jx_tmp); remove(dn_tmp);
        pngx_free(&info); return 0;
    }
    f = fopen(dn_tmp, "rb");
    uint8_t *dn = NULL; long dn_len = 0;
    if (f) {
        fseek(f, 0, SEEK_END); dn_len = ftell(f); fseek(f, 0, SEEK_SET);
        dn = (uint8_t *)malloc(dn_len ? (size_t)dn_len : 1);
        if (fread(dn, 1, (size_t)dn_len, f) != (size_t)dn_len) { free(dn); dn = NULL; }
        fclose(f);
    }
    int roundtrip_ok = 0;
    if (dn) {
        pngx_info di;
        memset(&di, 0, sizeof di);
        if (pngx_extract(dn, (size_t)dn_len, NULL, 0, png_inflate, &di) == 0 &&
            di.rgb_len == info.rgb_len &&
            memcmp(di.rgb, info.rgb, info.rgb_len) == 0)
            roundtrip_ok = 1;
        pngx_free(&di);
        free(dn);
    }
    remove(sp_tmp); remove(dn_tmp);
    if (!roundtrip_ok) {
        remove(jx_tmp);
        pngx_free(&info); return 0;   /* JXL changed pixels: keep original */
    }

    /* brute-force deflate params reproducing the original IDAT */
    uint8_t enc = 0, level = 0, mem = 0;
    int found = 0;
    /* refilter first (pixels from JXL round-trip are info.rgb) */
    uint8_t *filt = NULL; size_t filt_len = 0;
    if (pngx_refilter(info.rgb, info.rgb_len, &info, &filt, &filt_len) != 0) {
        remove(jx_tmp); pngx_free(&info); return 0;
    }
    static const int prio[][2] = {
        {6,8},{7,9},{6,9},{9,8},{7,8},{9,9},{6,7},{8,9},{8,8},{5,8},
        {4,8},{3,8},{2,8},{1,8},{7,7},{8,7},{9,7},{1,9},{2,9},{3,9},{4,9},{5,9}
    };
    for (size_t i = 0; i < sizeof(prio) / sizeof(prio[0]) && !found; i++) {
        z_stream s;
        memset(&s, 0, sizeof s);
        if (deflateInit2(&s, prio[i][0], Z_DEFLATED, 15, prio[i][1],
                         Z_DEFAULT_STRATEGY) != Z_OK) continue;
        size_t bound = deflateBound(&s, (uLong)filt_len);
        uint8_t *re = (uint8_t *)malloc(bound);
        s.next_in = filt;
        s.avail_in = (uInt)(filt_len > 0x7FFFFFFF ? 0x7FFFFFFF : filt_len);
        s.next_out = re;
        s.avail_out = (uInt)bound;
        int r2 = deflate(&s, Z_FINISH);
        size_t re_len = (size_t)s.total_out;
        deflateEnd(&s);
        if (r2 == Z_STREAM_END && re_len == info.idat_len &&
            memcmp(re, info.idat, info.idat_len) == 0) {
            enc = 0; level = (uint8_t)prio[i][0]; mem = (uint8_t)prio[i][1];
            found = 1;
        }
        free(re);
    }
    if (!found) {
        /* miniz tdefl levels 1..10 */
        for (int lv = 1; lv <= 10 && !found; lv++) {
            size_t olen = 0;
            size_t bound = filt_len + filt_len / 4 + 4096;
            uint8_t *re = (uint8_t *)malloc(bound);
            if (mz_tdefl_compress(filt, filt_len, re, bound, lv, &olen) == 0 &&
                olen == info.idat_len && memcmp(re, info.idat, info.idat_len) == 0) {
                enc = 1; level = (uint8_t)lv; mem = 0;
                found = 1;
            }
            free(re);
        }
    }
    free(filt);
    if (!found) {
        remove(jx_tmp);
        pngx_free(&info); return 0;   /* unknown encoder: keep original */
    }

    /* recipe + JXL blob */
    uint8_t *recipe = NULL; size_t rlen = 0;
    if (pngx_build_recipe(&info, enc, level, mem, &recipe, &rlen) != 0) {
        remove(jx_tmp); pngx_free(&info); return 0;
    }
    f = fopen(jx_tmp, "rb");
    uint8_t *jxl = NULL; long jxl_len = 0;
    if (f) {
        fseek(f, 0, SEEK_END); jxl_len = ftell(f); fseek(f, 0, SEEK_SET);
        jxl = (uint8_t *)malloc(jxl_len ? (size_t)jxl_len : 1);
        if (fread(jxl, 1, (size_t)jxl_len, f) != (size_t)jxl_len) { free(jxl); jxl = NULL; }
        fclose(f);
    }
    remove(jx_tmp);
    if (!jxl) { free(recipe); pngx_free(&info); return 0; }

    /* guard: JXL + recipe must be smaller than the original */
    if ((size_t)jxl_len + rlen >= png_len) {
        free(jxl); free(recipe); pngx_free(&info); return 0;
    }
    /* create name!jxl first, then name (atomic) */
    char jn[320];
    snprintf(jn, sizeof jn, "%s!jxl", name);
    uint64_t jino = vol_create_blob_file(v, jn, jxl, (size_t)jxl_len,
                                         (uint64_t)jxl_len, INVFS_ALGO_NONE);
    free(jxl);
    if (!jino) { free(recipe); pngx_free(&info);
                 return vol_transcode_abort(v, name); }
    size_t bound = ZSTD_compressBound(rlen);
    uint8_t *rc = (uint8_t *)malloc(bound + 1);
    if (!rc) { free(recipe); pngx_free(&info);
               return vol_transcode_abort(v, name); }
    size_t rbl = 0;
    size_t rcl = ZSTD_compress(rc + 1, bound, recipe, rlen, 19);
    if (!ZSTD_isError(rcl) && rcl < rlen) { rc[0] = 1; rbl = rcl + 1; }
    else { rc[0] = 0; memcpy(rc + 1, recipe, rlen); rbl = rlen + 1; }
    uint64_t ino = vol_create_blob_file(v, name, rc, rbl,
                                        (uint64_t)png_len, INVFS_ALGO_PNGR);
    free(rc); free(recipe); pngx_free(&info);
    if (!ino) return vol_transcode_abort(v, name);
    return ino;
#else
    (void)v; (void)name; (void)png; (void)png_len;
    return 0;
#endif
}


/* create inode storing one blob (JXL/APE/...) as a single segment */
static uint64_t vol_create_blob_file(invfs_volume *v, const char *name,
                                     const uint8_t *blob, size_t blob_len,
                                     uint64_t orig_size, uint32_t algo)
{
    uint64_t inode_id;
    uint64_t phys_blocks;
    uint64_t pba;
    uint8_t hdr4[8];
    size_t rec_size;
    uint8_t *rec;
    invfs_inode_rec *rh;
    invfs_ast_recipe_header ast_h;
    invfs_ast_block_entry e;
    uint32_t crc;

    /* Fault injection: fail every blob write from the Nth of this process on.
     *
     * Every transcode child goes through here, so this is the one place that
     * can make a partial transcode happen on demand. Without it the abort
     * paths are only reachable by filling a volume to a precise byte -- the
     * sweep frees the original as it goes, so the exact failure point is not
     * controllable from outside -- and they are precisely the paths where a
     * bug costs the user a file instead of some space.
     *
     * From the Nth *onward*, not the Nth alone: that is what ENOSPC looks
     * like, and it is the only way to exercise the retry paths (the FLAC
     * recipe falls back to storing uncompressed, so failing one write just
     * takes the fallback and the transcode succeeds). Unset in normal runs. */
    {
        const char *fc = getenv("INVFS_FAIL_CHILD");
        if (fc && atoi(fc) > 0) {
            static int nth = 0;
            if (++nth >= atoi(fc)) {
                fprintf(stderr, "[vol] INVFS_FAIL_CHILD: failing blob #%d (%s)\n",
                        nth, name);
                return 0;
            }
        }
    }

    /* blob_len goes into a 4-byte on-disk header and orig_size into the
       uint32 ast_h.file_size — refuse rather than fold either silently */
    if (blob_len > 0xFFFFFFFFu || orig_size > 0xFFFFFFFFu) {
        fprintf(stderr, "invarifs: %s: blob %llu / original %llu bytes "
                "exceeds the 4 GB format limit\n", name,
                (unsigned long long)blob_len, (unsigned long long)orig_size);
        return 0;
    }
    if (name_too_long(name)) return 0;

    inode_id = v->next_inode_id++;
    phys_blocks = (blob_len + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                       phys_blocks, 1);

    if (pba == 0) return 0;
    hdr4[0] = (uint8_t)(blob_len & 0xFF);
    hdr4[1] = (uint8_t)((blob_len >> 8) & 0xFF);
    hdr4[2] = (uint8_t)((blob_len >> 16) & 0xFF);
    hdr4[3] = (uint8_t)((blob_len >> 24) & 0xFF);
    {
        uint32_t bcrc = invfs_crc32c(blob, blob_len);
        hdr4[4] = (uint8_t)(bcrc & 0xFF);
        hdr4[5] = (uint8_t)((bcrc >> 8) & 0xFF);
        hdr4[6] = (uint8_t)((bcrc >> 16) & 0xFF);
        hdr4[7] = (uint8_t)((bcrc >> 24) & 0xFF);
    }

    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, hdr4, 8) != 0 ||
        io_write(&v->io, blob, blob_len) != 0) {
        vol_free_blocks(v, pba, phys_blocks);
        return 0;
    }
    if (vol_map(v, inode_id, 0, pba, (uint32_t)phys_blocks) != 0) {
        vol_free_blocks(v, pba, phys_blocks);
        return 0;
    }

    memset(&ast_h, 0, sizeof ast_h);
    ast_h.version = 1;
    ast_h.file_size = (uint32_t)orig_size;
    ast_h.num_blocks = 1;
    memset(&e, 0, sizeof e);
    e.file_offset = 0;
    e.length = orig_size;
    e.zone = INVFS_ZONE_BINARY;
    e.algo = algo;
    e.block_id = 0;

    rec_size = sizeof(invfs_inode_rec) + sizeof(ast_h) + sizeof(e);
    rec = (uint8_t *)calloc(1, rec_size);
    if (!rec) return 0;
    rh = (invfs_inode_rec *)rec;
    rh->magic = INODE_REC_MAGIC;
    rh->rec_len = (uint32_t)rec_size;
    rh->inode_id = inode_id;
    rh->file_size = orig_size;
    rh->ctime = (uint64_t)time(NULL);
    rec_set_name(rh, name);
    memcpy(rec + sizeof(invfs_inode_rec), &ast_h, sizeof ast_h);
    memcpy(rec + sizeof(invfs_inode_rec) + sizeof ast_h, &e, sizeof e);
    crc = invfs_crc32c(rec, rec_size);

    if (v->inode_area_pos + rec_size + 4 > v->inode_area_end) { free(rec); return 0; }
    if (vol_pre_record(v) != 0) { free(rec); return 0; }
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, rec, rec_size) != 0 ||
        io_write(&v->io, &crc, 4) != 0) { free(rec); return 0; }
    v->inode_area_pos += rec_size + 4;
    idx_put(v, name, strlen(name), inode_id, v->inode_area_pos - rec_size - 4,
            rh->file_size, rh->ctime);
    idx_put_id(v, inode_id, v->inode_area_pos - rec_size - 4);
    free(rec);
    return inode_id;
}

uint64_t vol_create_jxl_file(invfs_volume *v, const char *name,
                             const uint8_t *jxl, size_t jxl_len,
                             uint64_t orig_size)
{
    return vol_create_blob_file(v, name, jxl, jxl_len, orig_size, INVFS_ALGO_JXL);
}

uint64_t vol_create_ape_file(invfs_volume *v, const char *name,
                             const uint8_t *ape, size_t ape_len,
                             uint64_t orig_size)
{
    return vol_create_blob_file(v, name, ape, ape_len, orig_size, INVFS_ALGO_APE);
}

uint64_t vol_create_pmp_file(invfs_volume *v, const char *name,
                             const uint8_t *pmp, size_t pmp_len,
                             uint64_t orig_size)
{
    return vol_create_blob_file(v, name, pmp, pmp_len, orig_size, INVFS_ALGO_PMP);
}

/* WP13: one codecpack transcode attempt on a RAW file (packs register
 * whole-file EXTERNAL codecs; the manifest argv runs as a subprocess via
 * the codec.c trampolines + the exec hooks above). Mirrors the JXL branch
 * of vol_sweep_file_inner: probe -> admission (WP10 §12.2: the pack's
 * estimate command when it has one, else the manifest dec_mem constant) ->
 * encode -> decode-back memcmp guard (the 1:1 invariant) -> size guard ->
 * new blob inode first, retire the old one after, CODEC{algo, pack
 * generation} stamp.
 * Returns 100+algo on success, 1 when the pack's tools are unavailable
 * (defer: leave the file RAW and unstamped -- like a missing cjxl, the
 * first sweep after the tools appear picks it up), 0 when the pack
 * declined (GUARD/MEMLIMIT stamped; the caller falls through to
 * text/generic, and the stamp carries the retry semantics). */
static int vol_pack_sweep(invfs_volume *v, uint64_t inode_id, const char *name,
                          const invfs_codec *pc, const uint8_t *full,
                          size_t full_len)
{
    const invfs_pack_def *def;
    uint8_t *enc = NULL, *back = NULL;
    size_t enc_cap, enc_len = 0;
    uint64_t ws;

    if (!pc->probe || !pc->probe()) return 1;    /* tools absent: wait */
    def = invfs_codec_pack_def(pc);
    if (!def) return 0;

    /* WP10 §12.2 admission: decode working set from the pack's estimate
     * (header-derived, never a trial decode), else the manifest constant */
    ws = pc->dec_mem_bytes;
    if (def->estimate) {
        char dir[64], in[128];
        int ok = 0;
        if (tool_tmpdir(dir, sizeof dir) != 0) return 0;
        snprintf(in, sizeof in, "%s/in", dir);
        if (tool_write(in, full, full_len) == 0 &&
            invfs_codec_pack_estimate(pc, in, &ws) == 0)
            ok = 1;
        tool_rm(dir, "in");
        rmdir(dir);
        /* the pack could not size the job — for an estimate command that
         * parses the container header this IS the refusal (e.g. an
         * encapsulated DICOM): record it as a guard refusal so a newer
         * pack generation re-arms the retry (WP10 §2 table) */
        if (!ok) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            (uint8_t)pc->algo, pc->generation);
            return 0;
        }
    }
    if (ws && ws > vol_get_dec_mem_limit(v)) {
        vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                        (uint8_t)pc->algo, pc->generation);
        return 0;
    }

    /* the blob may exceed the input (codec overhead); the size guard below
     * is what refuses it, but the trampoline needs room to produce it */
    enc_cap = full_len + full_len / 4 + 65536;
    enc = (uint8_t *)malloc(enc_cap);
    if (!enc) return 0;
    if (pc->encode(full, full_len, enc, enc_cap, &enc_len) == 0 &&
        enc_len < full_len) {
        /* guard: the blob must decode back to the exact original bytes
         * before anything is replaced */
        back = (uint8_t *)malloc(full_len ? full_len : 1);
        if (back &&
            pc->decode(enc, enc_len, back, full_len) == 0 &&
            memcmp(back, full, full_len) == 0) {
            invfs_meta_pub keep;
            int have_keep = vol_get_meta(v, inode_id, &keep) == 0;
            uint64_t newino = vol_create_blob_file(v, name, enc, enc_len,
                                                   full_len, pc->algo);
            if (newino) {
                vol_delete_inode(v, inode_id, name);
                /* the fresh blob record has no ext; carry the old meta
                 * across, like the vol_jxl_retry flow does */
                if (have_keep) {
                    invfs_meta_pub chk;
                    if (vol_get_meta(v, newino, &chk) != 0)
                        vol_apply_meta(v, name, &keep);
                }
                vol_stamp_class(v, newino, INVFS_CLASS_CODEC,
                                (uint8_t)pc->algo, pc->generation);
                free(back);
                free(enc);
                return 100 + (int)pc->algo;
            }
            /* no space for the blob: NOT a guard refusal (vol_jxl_retry's
             * convention) -- leave unstamped so a retry re-arms */
            fprintf(stderr, "sweep: %s create failed (%s)\n", pc->name, name);
            free(back);
            free(enc);
            return 0;
        }
        free(back);
    }
    free(enc);
    /* declined: tool failed, no gain, or the round-trip guard refused --
     * the file goes generic below and retries when the pack's generation
     * improves (WP10 §2 table) */
    vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                    (uint8_t)pc->algo, pc->generation);
    return 0;
}


/* Drop a name whose backing record is already gone (index ghost left by
 * a killed process). Returns 0 if the entry was forgotten, -1 if the
 * name looks live (caller should use the normal unlink path). */
int vol_forget_name(invfs_volume *v, const char *name)
{
    uint64_t id, pos = 0;
    invfs_inode_rec hh;

    if (v->sb.vol_flags & VOLF_READONLY) return -1;
    id = vol_find(v, name);
    if (!id) return 0;                       /* already forgotten */
    pos = idx_get_id(v, id);
    if (!pos ||
        pos < v->inode_area_start * INVFS_BLOCK_SIZE ||
        pos + sizeof(hh) > v->inode_area_pos)
        return -1;
    if (io_seek(&v->io, pos) != 0 ||
        io_read(&v->io, &hh, sizeof(hh)) != 0)
        return -1;
    if (hh.magic == INODE_REC_MAGIC && hh.inode_id == id)
        return -1;                           /* record is alive */
    /* dead: drop the index entry and undo the directory child count */
    idx_del_at(v, name, strlen(name), pos);
    idx_bump_dirs(v, name, strlen(name), -1);
    vol_mark_dirty(v);
    return 0;
}


/* ---- manual live sweep support --------------------------------------
 * Collect inode ids of every live regular file with data (last record
 * per name wins), for a caller-driven sweep pass with its own locking
 * and progress reporting. CRC-validated scan, same rules as open. */
typedef struct { char name[256]; uint64_t id; } sweep_seed;

size_t vol_collect_sweepables(invfs_volume *v, uint64_t *ids, size_t max)
{
    sweep_seed *seen = NULL;
    size_t seen_n = 0, seen_cap = 0;
    size_t out = 0;
    uint64_t pos, end;
    size_t s;

    if (!v || !ids || max == 0) return 0;
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    end = v->inode_area_pos;
    while (pos + sizeof(invfs_inode_rec) <= end && out < max) {
        invfs_inode_rec h;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &h, sizeof(h)) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof(h) || h.rec_len > INVFS_MAX_REC_LEN ||
            pos + h.rec_len + 4 > end) break;
        {
            uint8_t *rb = (uint8_t *)malloc((size_t)h.rec_len + 4);
            uint32_t stored, calc;
            int bad = 0;
            if (!rb) break;
            if (io_seek(&v->io, pos) != 0 ||
                io_read(&v->io, rb, (size_t)h.rec_len + 4) != 0) { free(rb); break; }
            memcpy(&stored, rb + h.rec_len, 4);
            calc = invfs_crc32c(rb, h.rec_len);
            free(rb);
            if (calc != stored) bad = 1;   /* torn: skip, keep scanning */
            if (bad) { pos += (uint64_t)h.rec_len + 4; continue; }
        }
        pos += (uint64_t)h.rec_len + 4;
        if (h.magic != INODE_REC_MAGIC) continue;
        if (h.file_size == 0) continue;               /* nothing to move */
        {
            size_t nl = h.name_len < sizeof(h.name) ? h.name_len : sizeof(h.name)-1;
            int dup = 0;
            for (s = 0; s < seen_n; s++)
                if (strncmp(seen[s].name, h.name, sizeof(seen[s].name)) == 0)
                    { seen[s].id = h.inode_id; dup = 1; break; }
            if (dup) continue;
            if (seen_n == seen_cap) {
                sweep_seed *ns;
                seen_cap = seen_cap ? seen_cap*2 : 4096;
                ns = realloc(seen, seen_cap * sizeof(*seen));
                if (!ns) break;
                seen = ns;
            }
            memset(seen[seen_n].name, 0, sizeof(seen[seen_n].name));
            memcpy(seen[seen_n].name, h.name, nl);
            seen[seen_n].id = h.inode_id;
            seen_n++;
        }
    }
    /* resolve through the live index: tombstoned seeds drop out here */
    for (s = 0; s < seen_n && out < max; s++) {
        uint64_t id = vol_find(v, seen[s].name);
        if (id != 0) ids[out++] = id;
    }
    free(seen);
    return out;
}


uint64_t vol_zone_used_bytes(invfs_volume *v, uint64_t start_blk, uint64_t end_blk)
{
    uint64_t b, used = 0;
    if (!v || end_blk <= start_blk) return 0;
    for (b = start_blk; b < end_blk; b++)
        if (v->bitmap[b >> 3] & (1u << (b & 7))) used++;
    return used * INVFS_BLOCK_SIZE;
}

int vol_compute_stats(invfs_volume *v, invfs_volume_stats *out)
{
    uint64_t pos, end;
    if (!v || !out) return -1;
    memset(out, 0, sizeof(*out));
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    end = v->inode_area_pos;
    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec h;
        uint8_t *rb;
        uint32_t stored, calc;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &h, sizeof(h)) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof(h) || h.rec_len > INVFS_MAX_REC_LEN ||
            pos + h.rec_len + 4 > end) { out->bad_records++; break; }
        rb = malloc((size_t)h.rec_len + 4);
        if (!rb) break;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, rb, (size_t)h.rec_len + 4) != 0) { free(rb); break; }
        memcpy(&stored, rb + h.rec_len, 4);
        calc = invfs_crc32c(rb, h.rec_len);
        if (calc != stored) { free(rb); out->bad_records++; pos += (uint64_t)h.rec_len + 4; continue; }
        pos += (uint64_t)h.rec_len + 4;
        if (h.magic == TOMBSTONE_MAGIC) { free(rb); out->tombstones++; continue; }
        /* count each file once, at its live version: the name resolves to
         * the current id and the id index points at the newest record.
         * Older same-id versions (meta rewrites, class stamps, batch-owner
         * growth) and replaced/deleted records would otherwise inflate every
         * counter below. */
        {
            uint64_t ip = idx_get_id(v, h.inode_id);
            if (vol_find(v, h.name) != h.inode_id || (ip && ip != pos - ((uint64_t)h.rec_len + 4))) {
                free(rb);
                continue;
            }
        }
        {
            invfs_meta_pub m;
            int type = (vol_get_meta(v, h.inode_id, &m) == 0) ? m.type : -1;
            switch (type) {
            case INVFS_ITYP_DIR:  out->dirs++; break;
            case INVFS_ITYP_LNK:  out->links++; break;
            case INVFS_ITYP_FIFO: case INVFS_ITYP_SOCK:
            case INVFS_ITYP_CHR:  case INVFS_ITYP_BLK: out->special++; break;
            default: {
                out->files++;
                /* WP12(a): internal owner records ("\x01tzb") carry
                 * file_size = the sum of their sealed batches, and every
                 * TEXT member counts its own slices -- counting the owner
                 * too doubles the text-zone logical bytes (the Silesia
                 * image showed 309 MiB logical vs a 202 MiB corpus). The
                 * members carry the logical truth, so 0x01-prefixed
                 * internal names contribute nothing to ANY logical field
                 * (zone attribution, logical_bytes, biggest). Physical
                 * used-bytes accounting is bitmap-based and untouched. */
                if (h.name_len && (uint8_t)h.name[0] == 0x01)
                    break;
                /* attribute logical size across the AST's zones */
                {
                    /* record layout: rec header | AST header | entries |
                     * children | INO2 ext (vol_create_file, ~line 1821) --
                     * num_blocks sits at +8 of the AST header, entries at
                     * +16. (An earlier version of this loop added the whole
                     * AST blob length to the base and read past it.) */
                    size_t base = sizeof(invfs_inode_rec);
                    uint16_t nb = 0;
                    if (h.rec_len >= base + 16 && h.file_size > 0) {
                        memcpy(&nb, rb + base + 8, 2);
                        if (h.rec_len < base + 16 + (size_t)nb * 24)
                            nb = 0;     /* truncated recipe: attribute nothing */
                        uint64_t remain = h.file_size;
                        int i;
                        for (i = 0; i < nb && remain > 0; i++) {
                            uint8_t *e = rb + base + 16 + (size_t)i * 24;
                            uint32_t zone = e[16] & 3;   /* zone:2 LSB */
                            uint64_t seg;
                            /* TEXT entries are arbitrary-length slices of a
                             * shared batch (not 64 KB segments): use the
                             * entry's own length */
                            if (zone == INVFS_ZONE_TEXT) {
                                memcpy(&seg, e + 8, 8);
                                if (seg > remain) seg = remain;
                            } else {
                                seg = remain > 65536 ? 65536 : remain;
                            }
                            remain -= seg;
                            if (zone == INVFS_ZONE_TEXT)
                                out->logic_text_bytes += seg;
                            else if (zone == INVFS_ZONE_BINARY)
                                out->logic_shadow_bytes += seg;
                            else
                                out->logic_raw_bytes += seg;
                        }
                    }
                }
                if (h.file_size && vol_find(v, h.name) == h.inode_id) {
                    out->logical_bytes += h.file_size;
                    if (h.file_size > out->biggest_size) {
                        out->biggest_size = h.file_size;
                        snprintf(out->biggest_name, sizeof(out->biggest_name),
                                 "%s", h.name);
                    }
                }
            }
            }
            free(rb);
        }
    }
    out->raw_used_bytes =
        vol_zone_used_bytes(v, v->sb.raw_zone_start, v->sb.shadow_zone_start);
    out->shadow_used_bytes =
        vol_zone_used_bytes(v, v->sb.shadow_zone_start, v->sb.total_blocks);
    return 0;
}

/* hot population counters for the user.invfs.stats virtual xattr */
void vol_hot_counters(invfs_volume *v, uint64_t *files, uint64_t *dirs,
                      uint64_t *tombstones, uint64_t *logical_bytes)
{
    if (files)       *files = v->hot.files;
    if (dirs)        *dirs = v->hot.dirs;
    if (tombstones)  *tombstones = v->hot.tombstones;
    if (logical_bytes) *logical_bytes = v->hot.logical_bytes;
}

/* test-only export of the static parser */
int meta_read_record_by_id_p(invfs_volume *v, uint64_t id, uint8_t **buf, uint32_t *rl)
{ return meta_read_record_by_id(v, id, buf, rl, NULL, 0, NULL); }
