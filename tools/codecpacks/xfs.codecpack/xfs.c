/*
 * xfs.c — the "xfs" containerpack for InvariantFS (WP16, container ABI
 * v1.1): decomposes an XFS filesystem image into per-file members.
 *
 * Single translation unit, C11, libc only. All XFS on-disk integers are
 * big-endian; the pack-owned recipe and the FS-owned MRMP map are
 * little-endian (the invarifs host convention).
 *
 * Commands (fixed argv; exit 0 = ok, 3 = decline the image, 1 = error):
 *   enumerate <in> <out>            member table "idx<TAB>sname<TAB>usize"
 *   extract   <in> <idx> <out>      member idx's raw bytes (exactly usize)
 *   strip     <in> <out>            the recipe (everything but member bytes)
 *   rebuild   <recipe> <dir> <out>  the original image, bit-exact
 *   map       <in> <out>            MRMP partition (ABI v1.1, seekable)
 *   classify  <in> <out>            GCFG table: what every byte range IS
 *                                   (CLI-only manifest command, not ABI)
 *   estimate  <in>                  decode working set: sum(usize) + 64 MiB
 *
 * Members are the REGULAR FILES reachable from the root directory; idx is
 * the inode number (the FS caps idx at 65535, so an image holding a member
 * with a larger inode number is declined). Directories, symlinks, device
 * nodes and unlinked inodes stay in the recipe. The recipe carries every
 * non-member-content byte verbatim -- superblocks, AG headers, B+trees,
 * whole inode chunks (inode bytes are NOT member content), directory
 * blocks, the log, free space -- plus the range table rebuild needs.
 *
 * The reader is a namespace walker: it never trusts the inode B+trees
 * (agi/inobt are recipe bytes, not parse input), so only files reachable
 * from "/" become members and stale/unlinked inode data can never leak
 * into a member (and thus never get written back over live blocks at
 * rebuild time).
 *
 * Supported (v1):
 *   - superblock v4 and v5 (crc=0/1), blocksize 4096, inode size 256/512
 *   - file data forks in EXTENTS format and in BTREE format with a
 *     single-level root (bb_level == 1 -> leaf blocks "BMAP"/"BMA3")
 *   - directories in shortform (LOCAL), single-block (XD2B/XDB3) and
 *     multi-block leaf format (XD2D/XDD3 data blocks); ftype on or off
 *   - holes read as zeros; unwritten extents: their physical bytes stay
 *     in the RECIPE (they are not the member's logical zeros -- the
 *     member reads zeros, the disk bytes are undefined), holes have no
 *     disk bytes at all
 * Declined (exit 3, the FS stores the image whole):
 *   - blocksize != 4096, inode size not 256/512, dirblklog != 0
 *   - realtime sections (sb_rblocks != 0), quota flags set
 *   - rmapbt / reflink ro-compat features, nrext64, unknown feature bits
 *     in any v5 feature word, v5 log-incompat features
 *   - any regular file whose data fork is a B+tree deeper than one level
 *     (bb_level > 1), any directory in node (BTREE) format
 *   - any member inode number > 65535 (the FS idx cap) or > 65536 members
 *   - structural garbage anywhere on the walk
 * CRCs are not verified anywhere (read-only parse; the map guard at sweep
 * time proves every emitted byte range against the original anyway).
 *
 * classify additionally declines (see cmd_classify): an external log, a log
 * that does not fit sb_dblocks, and any contradiction between the free-space
 * B+trees, the inode B+trees and the AG headers -- because a GCFG table that
 * calls a needed block garbage is data loss for whoever converts with it,
 * while a refusal costs nothing.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ADR-007: this file doubles as a .so plugin (lib<name>.so, built with
 * -fPIC -shared -DIVPACK_SHARED_LIB). The plugin glue below is compiled in
 * BOTH builds -- the CLI build simply never calls it, and main() is what
 * -DIVPACK_SHARED_LIB drops -- so the .so and the CLI can never drift apart.
 * The headers are declarations + macros only: no new dependencies, no -ldl,
 * and the pack still builds with plain `cc -std=c11 -Wall -Wextra -Werror`. */
#if __has_include("ivpack_api.h")
#include "ivpack_api.h"
#elif __has_include("../../../src/include/ivpack_api.h")
#include "../../../src/include/ivpack_api.h"
#endif
#if __has_include("ivpack_impl.h")
#include "ivpack_impl.h"
#elif __has_include("../../../src/include/ivpack_impl.h")
#include "../../../src/include/ivpack_impl.h"
#endif


/* ------------------------------ constants ------------------------------ */

#define XFS_SB_MAGIC    0x58465342u   /* "XFSB" */
#define XFS_DINODE_MAGIC 0x494eu      /* "IN"  */
#define XFS_BMAP_MAGIC  0x424d4150u   /* "BMAP" (v4 bmap leaf) */
#define XFS_BMAP3_MAGIC 0x424d4133u   /* "BMA3" (v5 bmap leaf) */
#define XFS_DIR2_DATA_MAGIC  0x58443244u /* "XD2D" (v4 data block) */
#define XFS_DIR3_DATA_MAGIC  0x58444433u /* "XDD3" (v5 data block) */
#define XFS_DIR2_BLOCK_MAGIC 0x58443242u /* "XD2B" (v4 single-block dir) */
#define XFS_DIR3_BLOCK_MAGIC 0x58444233u /* "XDB3" (v5 single-block dir) */

/* dinode core field offsets (shared by v1/v2/v3 cores) */
#define DI_MODE       2
#define DI_VERSION    4
#define DI_FORMAT     5
#define DI_SIZE       56
#define DI_NEXTENTS   76
#define DI_FORKOFF    82
#define DI_FORK_V3    176      /* data fork offset, version-3 inode */
#define DI_FORK_V12   100      /* data fork offset, version-1/2 inode */

/* xfs_dinode_fmt values (zero-based enum on disk) */
#define FMT_DEV      0
#define FMT_LOCAL    1
#define FMT_EXTENTS  2
#define FMT_BTREE    3

/* v5 superblock feature bits we accept (all others -> decline) */
#define FEAT_RO_OK   0x09u    /* finobt (bit0), inobtcount (bit3) */
#define FEAT_RO_NO   0x06u    /* rmapbt (bit1), reflink (bit2): named below */
#define FEAT_INC_OK  0xcfu    /* ftype|spinodes|meta_uuid|bigtime|exchange|parent */
#define FEAT_INC_FTYPE 0x01u
#define FEAT_INC_SPINODES 0x02u  /* sparse inode chunks: inobt rec layout */
#define FEAT_RO_FINOBT 0x01u     /* v5 ro-compat: the free-inode btree */
/* v4 features2 bits we accept; FTYPE lives here on v4 */
#define FEAT2_OK     0x2bbu
#define FEAT2_FTYPE  0x200u

#define NULLSTARTBLOCK  ((1ULL << 52) - 1)  /* bmbt hole marker */
#define DIR_LEAF_DBLOCK (1ULL << 23)        /* leaf/free area: file blocks
                                             * >= this are not dir data */
#define MAX_MEMBERS    1048576u
#define MAX_IDX        1048575u
#define MAX_DEPTH      128
#define CHUNK          (8u << 20)           /* streaming window, <= 8 MiB */

/* recipe format ("XFSRCP01"): all integers little-endian
 *   [8B magic][u64 image_size][u32 n_members][u32 n_ranges][u64 reserved]
 *   n_members x {u64 ino, u64 usize}                      (16 B each)
 *   n_ranges  x {u64 orig, u64 len, u64 kind, u64 idx, u64 src} (40 B each)
 *   payload: the RECIPE ranges' bytes, concatenated in range order
 * kind 0 = RECIPE (bytes at the running payload offset), 1 = MEMBER
 * (bytes of member idx at logical offset src). */
#define RECIPE_MAGIC "XFSRCP01"
#define RECIPE_HDR   32
#define RECIPE_MENT  16
#define RECIPE_RENT  40

#define KIND_RECIPE 0
#define KIND_MEMBER 1

/* ------------------------------ diagnostics ---------------------------- */

static void vmsg(const char *kind, const char *fmt, va_list ap)
{
    fprintf(stderr, "xfs: %s: ", kind);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

/* decline the image (exit 3): the FS falls through to generic storage */
static void decline(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmsg("decline", fmt, ap);
    va_end(ap);
    exit(3);
}

/* hard error (exit 1): I/O failure, a lying member at rebuild, ... */
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmsg("error", fmt, ap);
    va_end(ap);
    exit(1);
}

/* ------------------------------ primitives ----------------------------- */

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void le64(uint8_t *p, uint64_t v)
{
    le32(p, (uint32_t)v); le32(p + 4, (uint32_t)(v >> 32));
}

/* LE readers for the recipe parser (kept separate from the writers for
 * symmetry with the BE side) */
static uint64_t le64v(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}

static uint32_t le32v(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

/* ------------------------------ the image ------------------------------ */

typedef struct {
    uint64_t startoff;    /* logical offset, filesystem blocks */
    uint64_t startblock;  /* raw fsblock (agno<<agblklog)|agbno, or hole */
    uint32_t count;       /* length, filesystem blocks */
    uint8_t  unwritten;
} ext_t;

typedef struct {
    uint64_t ino;
    uint64_t size;
    char     sname[32];
    ext_t   *exts;
    size_t   nexts;
} member_t;

typedef struct {
    uint64_t orig, len, src, idx;
    uint8_t  kind;
} range_t;

typedef struct {
    int      fd;
    uint64_t fsize;
    /* superblock */
    uint32_t blocksize;
    int      blocklog;
    uint64_t dblocks;
    uint32_t agblocks, agcount;
    int      agblklog;
    uint32_t inodesize, inopblock;
    int      inopblog;
    uint64_t rootino;
    int      v5;
    int      ftype;       /* directory entries carry a file-type byte */
    /* kept for classify (the GCFG pass needs the header geometry, the log
     * placement and two feature bits; the decomposition path does not) */
    uint32_t sectsize;
    uint64_t logstart, logblocks;
    int      finobt;      /* v5 ro-compat finobt bit */
    int      spinodes;    /* v5 incompat sparse-inode bit */
    /* walk results */
    member_t *mem;
    size_t   nmem, acap;
    uint64_t sum_usize;
    /* visited inode numbers (dirs and files): open-addressing hash */
    uint64_t *htab;
    size_t   hcap;        /* power of two; 0 = not allocated */
    size_t   hcount;
    /* the image partition (built after the walk) */
    range_t *ran;
    size_t   nran, rcap;
} img_t;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu)", n);
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory (%zu)", n);
    return q;
}

/* pread with bounds; short reads are corruption -> decline */
static void read_at(img_t *im, uint64_t off, void *buf, size_t len)
{
    uint8_t *p = buf;
    if (off > im->fsize || len > im->fsize - off)
        decline("read past end of image (off=%" PRIu64 " len=%zu)", off, len);
    while (len) {
        ssize_t r = pread(im->fd, p, len, (off_t)off);
        if (r < 0) die("pread: %s", strerror(errno));
        if (r == 0) decline("short read at %" PRIu64, off);
        p += r; off += (uint64_t)r; len -= (size_t)r;
    }
}

/* raw fsblock -> byte offset in the image; validates the AG addressing */
static uint64_t fsb_off(img_t *im, uint64_t fsb)
{
    uint64_t agno = fsb >> im->agblklog;
    uint64_t agbno = fsb & ((1ULL << im->agblklog) - 1);
    if (agno >= im->agcount || agbno >= im->agblocks)
        decline("fsblock %" PRIu64 " out of range (ag %" PRIu64
                " block %" PRIu64 ")", fsb, agno, agbno);
    return (agno * im->agblocks + agbno) << im->blocklog;
}

/* inode number -> byte offset; validates like fsb_off */
static uint64_t ino_off(img_t *im, uint64_t ino)
{
    int      ibits = im->inopblog + im->agblklog;
    uint64_t agno = ino >> ibits;
    uint64_t agino = ino & ((1ULL << ibits) - 1);
    uint64_t agbno = agino >> im->inopblog;
    uint64_t slot = agino & (im->inopblock - 1);
    if (agno >= im->agcount || agbno >= im->agblocks)
        decline("inode %" PRIu64 " out of range", ino);
    return ((agno * im->agblocks + agbno) << im->blocklog) +
           slot * im->inodesize;
}

/* ------------------------- visited-inode hash -------------------------- */

static void hput(img_t *im, uint64_t ino);   /* fwd */

static void hgrow(img_t *im)
{
    size_t ncap = im->hcap ? im->hcap * 2 : 1024, i;
    uint64_t *old = im->htab;
    size_t ocap = im->hcap;
    im->htab = xmalloc(ncap * sizeof *im->htab);
    memset(im->htab, 0, ncap * sizeof *im->htab);
    im->hcap = ncap;
    im->hcount = 0;
    for (i = 0; i < ocap; i++)
        if (old[i]) hput(im, old[i]);
    free(old);
}

static uint64_t hmix(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    return x ^ (x >> 33);
}

/* insert; returns 1 if newly inserted, 0 if already present */
static int hadd(img_t *im, uint64_t ino)
{
    size_t m;
    if (!im->hcap) hgrow(im);
    if ((im->hcount + 1) * 10 >= im->hcap * 7) hgrow(im);
    m = (size_t)hmix(ino) & (im->hcap - 1);
    while (im->htab[m]) {
        if (im->htab[m] == ino) return 0;
        m = (m + 1) & (im->hcap - 1);
    }
    im->htab[m] = ino;
    im->hcount++;
    return 1;
}

static void hput(img_t *im, uint64_t ino)
{
    (void)hadd(im, ino);
}

/* --------------------------- superblock parse -------------------------- */

static void parse_sb(img_t *im)
{
    uint8_t  sb[512];
    uint16_t vnum, sectsize;
    uint32_t features2, fcompat, fro, finc, flog;

    read_at(im, 0, sb, sizeof sb);
    if (be32(sb + 0) != XFS_SB_MAGIC)
        decline("not XFS (bad superblock magic)");
    im->blocksize = be32(sb + 4);
    im->dblocks   = be64(sb + 8);
    if (be64(sb + 16) != 0 || be64(sb + 24) != 0)
        decline("realtime sections are not supported");
    im->agblocks  = be32(sb + 84);
    im->agcount   = be32(sb + 88);
    vnum          = be16(sb + 100);
    sectsize      = be16(sb + 102);
    im->inodesize = be16(sb + 104);
    im->inopblock = be16(sb + 106);
    im->blocklog  = sb[120];
    im->inopblog  = sb[123];
    im->agblklog  = sb[124];
    im->rootino   = be64(sb + 56);
    im->sectsize  = sectsize;
    im->logstart  = be64(sb + 48);
    im->logblocks = be32(sb + 96);

    if (im->blocksize != 4096 || im->blocklog != 12)
        decline("blocksize %u not supported (v1: 4096 only)", im->blocksize);
    if (sectsize != 512 && sectsize != 4096)
        decline("sector size %u not supported", sectsize);
    if (im->inodesize != 256 && im->inodesize != 512)
        decline("inode size %u not supported (256/512 only)", im->inodesize);
    if (im->inodesize * im->inopblock != im->blocksize ||
        (1u << im->inopblog) != im->inopblock)
        decline("inconsistent inode geometry");
    if (!im->agcount || im->agcount > 1024 || im->agblocks < 64 ||
        im->agblklog >= 32 || (1ULL << im->agblklog) < im->agblocks)
        decline("inconsistent AG geometry");
    if (im->dblocks > (uint64_t)im->agcount * im->agblocks ||
        im->dblocks * im->blocksize > im->fsize)
        decline("superblock dblocks exceeds the image size");
    if (sb[126]) /* sb_inprogress */
        decline("filesystem creation never finished");
    if (sb[192]) /* sb_dirblklog */
        decline("directory blocks larger than fs blocks are not supported");
    if (be16(sb + 176)) /* sb_qflags */
        decline("quota accounting is not supported");

    im->v5 = ((vnum & 0x000f) == 5);
    if (!im->v5 && (vnum & 0x000f) != 4)
        decline("superblock version %u not supported", vnum & 0x000f);
    if (!(vnum & 0x2000))
        decline("dir v1 layout is not supported");
    if (vnum & 0x0040)
        decline("quota version bit set");
    if (vnum & 0x0200)
        decline("shared version numbers are not supported");

    features2 = (vnum & 0x1000) ? be32(sb + 200) : 0;
    if (features2 != be32(sb + 204))
        decline("features2/bad_features2 mismatch");
    if (im->v5) {
        fcompat = be32(sb + 208);
        fro     = be32(sb + 212);
        finc    = be32(sb + 216);
        flog    = be32(sb + 220);
        if (fcompat & ~0x7u)
            decline("unknown v5 compat features 0x%x", fcompat);
        if (fro & FEAT_RO_NO)
            decline("rmapbt/reflink filesystems are not supported");
        if (fro & ~0x0fu)
            decline("unknown v5 ro-compat features 0x%x", fro);
        if (finc & ~FEAT_INC_OK)
            decline("unsupported v5 incompat features 0x%x", finc);
        if (flog)
            decline("unsupported v5 log-incompat features 0x%x", flog);
        im->ftype = (finc & FEAT_INC_FTYPE) != 0;
        im->spinodes = (finc & FEAT_INC_SPINODES) != 0;
        im->finobt = (fro & FEAT_RO_FINOBT) != 0;
    } else {
        if (features2 & ~FEAT2_OK)
            decline("unsupported v4 features2 0x%x", features2);
        if (sb[179]) /* sb_shared_vn */
            decline("shared version numbers are not supported");
        im->ftype = (features2 & FEAT2_FTYPE) != 0;
    }
}

/* ----------------------------- inode parse ----------------------------- */

/* read the inode into a malloc'd buffer of inodesize bytes */
static uint8_t *read_inode(img_t *im, uint64_t ino)
{
    uint8_t *b = xmalloc(im->inodesize);
    read_at(im, ino_off(im, ino), b, im->inodesize);
    if (be16(b) != XFS_DINODE_MAGIC)
        decline("inode %" PRIu64 ": bad magic", ino);
    return b;
}

static int fork_base(const img_t *im, const uint8_t *di)
{
    (void)im;
    return di[DI_VERSION] == 3 ? DI_FORK_V3 : DI_FORK_V12;
}

/* decode one 16-byte bmbt record */
static void bmbt_rec(const uint8_t *p, ext_t *e)
{
    uint64_t l0 = be64(p), l1 = be64(p + 8);
    e->unwritten  = (uint8_t)(l0 >> 63);
    e->startoff   = (l0 & 0x7fffffffffffffffULL) >> 9;
    e->startblock = ((l0 & 0x1ff) << 43) | (l1 >> 21);
    e->count      = (uint32_t)(l1 & 0x1fffff);
}

/* collect a data fork's extent records (EXTENTS inline, or a single-level
 * BTREE root pointing at BMAP/BMA3 leaves). Records come back sorted by
 * startoff; that order is validated. */
static ext_t *get_extents(img_t *im, uint64_t ino, const uint8_t *di,
                          size_t *n_out)
{
    int      fmt = di[DI_FORMAT];
    uint32_t nextents = be32(di + DI_NEXTENTS);
    int      fb = fork_base(im, di);
    uint32_t fork_space = di[DI_FORKOFF] ? (uint32_t)di[DI_FORKOFF] * 8
                                         : im->inodesize - (uint32_t)fb;
    ext_t   *ex;
    size_t   n = 0, i;
    uint64_t prev_end = 0;

    *n_out = 0;
    if (nextents == 0)
        return NULL;
    if ((uint64_t)nextents * 16 > (64ULL << 20))
        decline("inode %" PRIu64 ": implausible extent count %u",
                ino, nextents);
    ex = xmalloc((size_t)nextents * sizeof *ex);

    if (fmt == FMT_EXTENTS) {
        if ((uint64_t)nextents * 16 > fork_space)
            decline("inode %" PRIu64 ": extents overflow the fork", ino);
        for (i = 0; i < nextents; i++)
            bmbt_rec(di + fb + i * 16, &ex[n++]);
    } else if (fmt == FMT_BTREE) {
        uint16_t level = be16(di + fb), nrecs = be16(di + fb + 2);
        uint32_t maxrecs = (fork_space - 4) / 16;
        uint32_t lbhdr = im->v5 ? 72 : 24;
        uint32_t lbmagic = im->v5 ? XFS_BMAP3_MAGIC : XFS_BMAP_MAGIC;
        uint8_t *blk;
        if (level != 1)
            decline("inode %" PRIu64 ": bmap btree depth %u > 1", ino, level);
        if (!nrecs || nrecs > maxrecs)
            decline("inode %" PRIu64 ": bad btree root records %u", ino, nrecs);
        blk = xmalloc(im->blocksize);
        for (i = 0; i < nrecs; i++) {
            uint64_t ptr = be64(di + fb + 4 + (size_t)maxrecs * 8 + i * 8);
            uint32_t nm, j;
            read_at(im, fsb_off(im, ptr), blk, im->blocksize);
            if (be32(blk) != lbmagic || be16(blk + 4) != 0)
                decline("inode %" PRIu64 ": bad bmap leaf block", ino);
            nm = be16(blk + 6);
            if (!nm || (uint64_t)lbhdr + (uint64_t)nm * 16 > im->blocksize)
                decline("inode %" PRIu64 ": bad bmap leaf record count", ino);
            if (n + nm > nextents)
                decline("inode %" PRIu64 ": extent count overflow", ino);
            for (j = 0; j < nm; j++)
                bmbt_rec(blk + lbhdr + (size_t)j * 16, &ex[n++]);
        }
        free(blk);
        if (n != nextents)
            decline("inode %" PRIu64 ": btree holds %zu extents, header "
                    "says %u", ino, n, nextents);
    } else {
        decline("inode %" PRIu64 ": unsupported data fork format %d",
                ino, fmt);
    }

    /* sorted, non-overlapping, in-range */
    for (i = 0; i < n; i++) {
        if (!ex[i].count)
            decline("inode %" PRIu64 ": zero-length extent", ino);
        if (i && ex[i].startoff < prev_end)
            decline("inode %" PRIu64 ": overlapping extents", ino);
        prev_end = ex[i].startoff + ex[i].count;
        if (ex[i].startblock != NULLSTARTBLOCK) {
            uint64_t byte0 = fsb_off(im, ex[i].startblock);
            uint64_t bytes = (uint64_t)ex[i].count << im->blocklog;
            if (byte0 > im->fsize || bytes > im->fsize - byte0)
                decline("inode %" PRIu64 ": extent past end of image", ino);
        }
    }
    *n_out = n;
    return ex;
}

/* --------------------------- directory walk ---------------------------- */

static void sanitize_sname(char out[32], const uint8_t *name, uint32_t len)
{
    size_t o = 0, i;
    for (i = 0; i < len && o < 24; i++) {
        uint8_t c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            out[o++] = (char)c;
        else
            out[o++] = '_';
    }
    if (!o) out[o++] = 'f';
    out[o] = '\0';
}

static void add_member(img_t *im, uint64_t ino,
                       const uint8_t *name, uint32_t namelen)
{
    uint8_t  *di;
    member_t *m;

    if (!hadd(im, ino)) return;             /* hardlink: already a member */
    if (ino > MAX_IDX)
        decline("member inode %" PRIu64 " exceeds the FS idx cap %u",
                ino, MAX_IDX);
    if (im->nmem >= MAX_MEMBERS)
        decline("more than %u members", MAX_MEMBERS);
    di = read_inode(im, ino);
    if ((be16(di + DI_MODE) & 0xf000) != 0x8000) {
        free(di);
        decline("inode %" PRIu64 ": expected a regular file", ino);
    }
    if (im->nmem == im->acap) {
        im->acap = im->acap ? im->acap * 2 : 64;
        im->mem = xrealloc(im->mem, im->acap * sizeof *im->mem);
    }
    m = &im->mem[im->nmem++];
    m->ino  = ino;
    m->size = be64(di + DI_SIZE);
    sanitize_sname(m->sname, name, namelen);
    m->exts = get_extents(im, ino, di, &m->nexts);
    im->sum_usize += m->size;
    free(di);
}

/* classify one directory entry and act on it */
static void dir_entry(img_t *im, uint64_t ino, uint32_t ftype,
                      const uint8_t *name, uint32_t namelen, unsigned depth);

static void walk_dir(img_t *im, uint64_t ino, unsigned depth)
{
    uint8_t *di;
    int      fmt;

    if (depth > MAX_DEPTH)
        decline("directory nesting deeper than %u", MAX_DEPTH);
    if (!hadd(im, ino)) return;             /* already walked (or a cycle) */
    di = read_inode(im, ino);
    if ((be16(di + DI_MODE) & 0xf000) != 0x4000) {
        free(di);
        decline("inode %" PRIu64 ": expected a directory", ino);
    }
    fmt = di[DI_FORMAT];

    if (fmt == FMT_LOCAL) {
        /* shortform: entries live in the fork, inumbers are 4 or 8 bytes
         * depending on the header's i8count */
        int      fb = fork_base(im, di);
        uint64_t dsz = be64(di + DI_SIZE);
        uint32_t fork_space = di[DI_FORKOFF] ? (uint32_t)di[DI_FORKOFF] * 8
                                             : im->inodesize - (uint32_t)fb;
        const uint8_t *p, *end;
        uint32_t count, isz, i;
        if (dsz > fork_space || dsz < 3) {
            free(di);
            decline("inode %" PRIu64 ": bad shortform size", ino);
        }
        p = di + fb;
        end = p + dsz;
        count = p[0];
        isz = p[1] ? 8 : 4;
        p += 2 + isz;                       /* count, i8count, parent */
        for (i = 0; i < count; i++) {
            uint32_t nl;
            uint64_t eino;
            uint32_t ft = 0;
            if (p + 3 > end || p + 3 + p[0] + (im->ftype ? 1 : 0) + isz > end)
                { free(di); decline("inode %" PRIu64 ": shortform entry "
                                    "overflow", ino); }
            nl = p[0];
            eino = 0;                       /* p[1..2] = readdir offset */
            if (im->ftype) ft = p[3 + nl];
            if (isz == 8) eino = be64(p + 3 + nl + (im->ftype ? 1 : 0));
            else          eino = be32(p + 3 + nl + (im->ftype ? 1 : 0));
            dir_entry(im, eino, ft, p + 3, nl, depth);
            p += 3 + nl + (im->ftype ? 1 : 0) + isz;
        }
        free(di);
        return;
    }

    if (fmt == FMT_EXTENTS) {
        /* block / leaf format: parse every data block found in the data
         * fork's extents; blocks at file offsets >= DIR_LEAF_DBLOCK are
         * the leaf/free index area (recipe bytes, never parsed) */
        ext_t   *ex;
        size_t   nex, i;
        uint8_t *blk;
        ex = get_extents(im, ino, di, &nex);
        free(di);
        blk = xmalloc(im->blocksize);
        for (i = 0; i < nex; i++) {
            uint32_t j;
            if (ex[i].startblock == NULLSTARTBLOCK || ex[i].unwritten)
                decline("inode %" PRIu64 ": hole/unwritten block in a "
                        "directory", ino);
            if (ex[i].startoff >= DIR_LEAF_DBLOCK)
                continue;                   /* leaf/free index blocks */
            for (j = 0; j < ex[i].count; j++) {
                uint64_t off = fsb_off(im, ex[i].startblock + j);
                uint32_t magic, pos, end;
                read_at(im, off, blk, im->blocksize);
                magic = be32(blk);
                if (magic == (im->v5 ? XFS_DIR3_DATA_MAGIC
                                     : XFS_DIR2_DATA_MAGIC)) {
                    pos = im->v5 ? 64 : 16; /* crc/compat header + bestfree */
                    end = im->blocksize;
                } else if (magic == (im->v5 ? XFS_DIR3_BLOCK_MAGIC
                                            : XFS_DIR2_BLOCK_MAGIC)) {
                    /* single-block dir: the hash leaf array + tail sit at
                     * the end; the data area ends before them */
                    uint32_t tcnt = be32(blk + im->blocksize - 8);
                    if ((uint64_t)tcnt * 8 + 8 > im->blocksize - 64)
                        decline("inode %" PRIu64 ": bad dir block tail",
                                ino);
                    pos = im->v5 ? 64 : 16;
                    end = im->blocksize - 8 - tcnt * 8;
                } else {
                    decline("inode %" PRIu64 ": unknown dir block magic "
                            "0x%08x", ino, magic);
                    continue;               /* unreachable */
                }
                while (pos + 4 <= end) {
                    if (be16(blk + pos) == 0xffff) {   /* free space */
                        uint32_t flen = be16(blk + pos + 2);
                        if (flen < 8 || pos + flen > end || (flen & 7))
                            decline("inode %" PRIu64 ": bad dir free entry",
                                    ino);
                        pos += flen;
                        continue;
                    }
                    {
                        uint64_t eino = be64(blk + pos);
                        uint32_t nl, esz, ft = 0;
                        if (pos + 12 > end)
                            decline("inode %" PRIu64 ": dir entry "
                                    "overflow", ino);
                        nl = blk[pos + 8];
                        if (!nl)
                            decline("inode %" PRIu64 ": zero namelen", ino);
                        esz = 8 + 1 + nl + (im->ftype ? 1 : 0) + 2;
                        esz = (esz + 7) & ~7u;
                        if (pos + esz > end)
                            decline("inode %" PRIu64 ": dir entry past "
                                    "block end", ino);
                        if (be16(blk + pos + esz - 2) != pos)
                            decline("inode %" PRIu64 ": dir entry tag "
                                    "mismatch", ino);
                        if (im->ftype) ft = blk[pos + 9 + nl];
                        if (eino)
                            dir_entry(im, eino, ft, blk + pos + 9, nl, depth);
                        pos += esz;
                    }
                }
                if (pos != end)
                    decline("inode %" PRIu64 ": dir block parse stopped "
                            "early (%u/%u)", ino, pos, end);
            }
        }
        free(blk);
        free(ex);
        return;
    }

    if (fmt == FMT_BTREE) {
        free(di);
        decline("inode %" PRIu64 ": node-format directory (not supported "
                "in v1)", ino);
    }
    free(di);
    decline("inode %" PRIu64 ": unsupported directory format %d", ino, fmt);
}

static void dir_entry(img_t *im, uint64_t ino, uint32_t ftype,
                      const uint8_t *name, uint32_t namelen, unsigned depth)
{
    if (namelen == 1 && name[0] == '.') return;
    if (namelen == 2 && name[0] == '.' && name[1] == '.') return;
    if (!ino) return;
    if (ftype == 1) {                       /* XFS_DIR3_FT_REG_FILE */
        add_member(im, ino, name, namelen);
        return;
    }
    if (ftype == 2) {                       /* XFS_DIR3_FT_DIR */
        walk_dir(im, ino, depth + 1);
        return;
    }
    if (ftype != 0) return;                 /* symlink/dev/...: recipe */
    /* ftype unknown (or the filesystem has no ftype bytes): classify by
     * the inode's mode. Unvisited inodes only -- the visited set tells us
     * a hardlink's type for free. */
    {
        uint8_t *di;
        uint16_t mode;
        int fresh;
        /* peek without consuming the visited slot twice: add_member and
         * walk_dir both insert, so probe the mode first */
        di = read_inode(im, ino);
        mode = be16(di + DI_MODE) & 0xf000;
        free(di);
        fresh = 1;  /* both callees tolerate a re-visit */
        (void)fresh;
        if (mode == 0x8000) add_member(im, ino, name, namelen);
        else if (mode == 0x4000) walk_dir(im, ino, depth + 1);
        /* anything else: recipe */
    }
}

/* ------------------------- the image partition ------------------------- */

static void ran_push(img_t *im, uint64_t orig, uint64_t len, uint8_t kind,
                     uint64_t idx, uint64_t src)
{
    if (im->nran == im->rcap) {
        im->rcap = im->rcap ? im->rcap * 2 : 256;
        im->ran = xrealloc(im->ran, im->rcap * sizeof *im->ran);
    }
    im->ran[im->nran].orig = orig;
    im->ran[im->nran].len  = len;
    im->ran[im->nran].kind = kind;
    im->ran[im->nran].idx  = idx;
    im->ran[im->nran].src  = src;
    im->nran++;
}

static int range_cmp(const void *a, const void *b)
{
    uint64_t x = ((const range_t *)a)->orig, y = ((const range_t *)b)->orig;
    return x < y ? -1 : x > y;
}

/* turn the members' extents into MEMBER ranges, then complete the
 * partition of [0, fsize) with RECIPE ranges. */
static void build_partition(img_t *im)
{
    size_t   i, j;
    uint64_t pos;

    for (i = 0; i < im->nmem; i++) {
        member_t *m = &im->mem[i];
        for (j = 0; j < m->nexts; j++) {
            ext_t   *e = &m->exts[j];
            uint64_t lstart, lbytes, take;
            if (e->startblock == NULLSTARTBLOCK || e->unwritten)
                continue;               /* holes: no bytes; unwritten:
                                           physical bytes stay RECIPE */
            lstart = e->startoff << im->blocklog;
            lbytes = (uint64_t)e->count << im->blocklog;
            take = (m->size > lstart) ? m->size - lstart : 0;
            if (take > lbytes) take = lbytes;
            if (!take) continue;        /* tail of the last block: RECIPE */
            ran_push(im, fsb_off(im, e->startblock), take, KIND_MEMBER,
                     m->ino, lstart);
        }
    }
    qsort(im->ran, im->nran, sizeof *im->ran, range_cmp);
    for (i = 1; i < im->nran; i++)
        if (im->ran[i].orig < im->ran[i - 1].orig + im->ran[i - 1].len)
            decline("two members claim the same blocks (offset %" PRIu64
                    ")", im->ran[i].orig);

    /* interleave with the recipe complement, in place behind the array */
    {
        range_t *mr = im->ran;
        size_t   nmr = im->nran;
        im->ran = NULL; im->nran = im->rcap = 0;
        pos = 0;
        for (i = 0; i < nmr; i++) {
            if (mr[i].orig > pos)
                ran_push(im, pos, mr[i].orig - pos, KIND_RECIPE, 0, 0);
            ran_push(im, mr[i].orig, mr[i].len, KIND_MEMBER, mr[i].idx,
                     mr[i].src);
            pos = mr[i].orig + mr[i].len;
        }
        if (pos < im->fsize)
            ran_push(im, pos, im->fsize - pos, KIND_RECIPE, 0, 0);
        free(mr);
    }
}

/* full parse: superblock + namespace walk + partition. exit 3 on any
 * refusal.
 *
 * `need_members` is 0 for classify only. `strip`/`map`/`estimate` need a
 * non-empty member set (a zero-member container goes generic, and the recipe
 * carries the whole image either way), but the classification of a freshly
 * mkfs'd image is exactly the interesting case for a caller that wants to know
 * what a blank filesystem looks like -- refusing it would be a refusal about
 * the caller's intent, not about the bytes. */
static img_t *parse_image_ex(const char *path, int need_members)
{
    img_t *im = xmalloc(sizeof *im);
    struct stat st;
    memset(im, 0, sizeof *im);
    im->fd = open(path, O_RDONLY);
    if (im->fd < 0) die("open %s: %s", path, strerror(errno));
    if (fstat(im->fd, &st) != 0) die("stat %s: %s", path, strerror(errno));
    if (!S_ISREG(st.st_mode) || st.st_size < 512)
        decline("not a plausible XFS image file");
    im->fsize = (uint64_t)st.st_size;
    parse_sb(im);
    walk_dir(im, im->rootino, 0);
    if (!im->nmem && need_members)
        decline("no regular files (a zero-member container goes generic)");
    build_partition(im);
    return im;
}

static img_t *parse_image(const char *path)
{
    return parse_image_ex(path, 1);
}

/* ------------------------------ streaming ------------------------------ */

/* copy len bytes from one fd to another in <= CHUNK windows */
static void fd_copy(int sfd, uint64_t soff, int dfd, uint64_t len,
                    uint8_t *buf)
{
    while (len) {
        size_t want = len > CHUNK ? CHUNK : (size_t)len;
        size_t done = 0;
        while (done < want) {
            ssize_t r = pread(sfd, buf + done, want - done,
                              (off_t)(soff + done));
            if (r < 0) die("pread: %s", strerror(errno));
            if (r == 0) die("short source read");
            done += (size_t)r;
        }
        done = 0;
        while (done < want) {
            ssize_t r = write(dfd, buf + done, want - done);
            if (r < 0) die("write: %s", strerror(errno));
            done += (size_t)r;
        }
        soff += want;
        len -= want;
    }
}

static void fd_zeros(int dfd, uint64_t len, uint8_t *buf)
{
    memset(buf, 0, CHUNK);
    while (len) {
        size_t want = len > CHUNK ? CHUNK : (size_t)len;
        size_t done = 0;
        while (done < want) {
            ssize_t r = write(dfd, buf + done, want - done);
            if (r < 0) die("write: %s", strerror(errno));
            done += (size_t)r;
        }
        len -= want;
    }
}

static int open_out(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) die("create %s: %s", path, strerror(errno));
    return fd;
}

/* ------------------------------ commands ------------------------------- */

static void cmd_enumerate(const char *in, const char *out)
{
    img_t *im = parse_image(in);
    FILE  *f = fopen(out, "w");
    size_t i;
    if (!f) die("create %s: %s", out, strerror(errno));
    for (i = 0; i < im->nmem; i++)
        if (fprintf(f, "%" PRIu64 "\t%s\t%" PRIu64 "\n",
                    im->mem[i].ino, im->mem[i].sname, im->mem[i].size) < 0)
            die("write table: %s", strerror(errno));
    if (fclose(f) != 0) die("close table: %s", strerror(errno));
}

static void cmd_extract(const char *in, const char *idx_s, const char *out)
{
    img_t   *im = parse_image(in);
    uint64_t want, pos = 0;
    member_t *m = NULL;
    size_t   i;
    int      ofd;
    uint8_t *buf;

    want = strtoull(idx_s, NULL, 10);
    for (i = 0; i < im->nmem; i++)
        if (im->mem[i].ino == want) { m = &im->mem[i]; break; }
    if (!m) decline("no member with idx %s", idx_s);
    ofd = open_out(out);
    buf = xmalloc(CHUNK);
    for (i = 0; i < m->nexts && pos < m->size; i++) {
        ext_t   *e = &m->exts[i];
        uint64_t lstart = e->startoff << im->blocklog;
        uint64_t lbytes = (uint64_t)e->count << im->blocklog;
        uint64_t take;
        if (lstart > pos) {
            uint64_t z = lstart - pos;
            if (z > m->size - pos) z = m->size - pos;
            fd_zeros(ofd, z, buf);
            pos += z;
            if (pos >= m->size) break;
        }
        take = m->size - pos;
        if (take > lbytes) take = lbytes;
        if (e->startblock == NULLSTARTBLOCK || e->unwritten)
            fd_zeros(ofd, take, buf);
        else
            fd_copy(im->fd, fsb_off(im, e->startblock) + (pos - lstart),
                    ofd, take, buf);
        pos += take;
    }
    if (pos < m->size) fd_zeros(ofd, m->size - pos, buf);
    free(buf);
    if (close(ofd) != 0) die("close %s: %s", out, strerror(errno));
}

/* recipe layout arithmetic, shared by strip and map so the two always
 * agree on the payload offsets */
static uint64_t payload_base(const img_t *im)
{
    return RECIPE_HDR + (uint64_t)im->nmem * RECIPE_MENT +
           (uint64_t)im->nran * RECIPE_RENT;
}

static void write_recipe_head(int fd, const img_t *im)
{
    uint8_t  hdr[RECIPE_HDR];
    uint8_t *tab;
    size_t   i, n;
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, RECIPE_MAGIC, 8);
    le64(hdr + 8, im->fsize);
    le32(hdr + 16, (uint32_t)im->nmem);
    le32(hdr + 20, (uint32_t)im->nran);
    tab = xmalloc((size_t)im->nmem * RECIPE_MENT +
                  (size_t)im->nran * RECIPE_RENT);
    n = 0;
    for (i = 0; i < im->nmem; i++) {
        le64(tab + n, im->mem[i].ino);  n += 8;
        le64(tab + n, im->mem[i].size); n += 8;
    }
    for (i = 0; i < im->nran; i++) {
        le64(tab + n, im->ran[i].orig); n += 8;
        le64(tab + n, im->ran[i].len);  n += 8;
        le64(tab + n, im->ran[i].kind); n += 8;
        le64(tab + n, im->ran[i].idx);  n += 8;
        le64(tab + n, im->ran[i].src);  n += 8;
    }
    if (write(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr)
        die("write recipe head: %s", strerror(errno));
    if (write(fd, tab, n) != (ssize_t)n)
        die("write recipe tables: %s", strerror(errno));
    free(tab);
}

static void cmd_strip(const char *in, const char *out)
{
    img_t   *im = parse_image(in);
    int      ofd = open_out(out);
    uint8_t *buf = xmalloc(CHUNK);
    size_t   i;
    write_recipe_head(ofd, im);
    for (i = 0; i < im->nran; i++)
        if (im->ran[i].kind == KIND_RECIPE)
            fd_copy(im->fd, im->ran[i].orig, ofd, im->ran[i].len, buf);
    free(buf);
    if (close(ofd) != 0) die("close %s: %s", out, strerror(errno));
}

static void cmd_map(const char *in, const char *out)
{
    img_t   *im = parse_image(in);
    FILE    *f = fopen(out, "wb");
    uint64_t running = payload_base(im);
    size_t   i;
    uint8_t  hdr[8], ent[29];
    if (!f) die("create %s: %s", out, strerror(errno));
    memcpy(hdr, "MRMP", 4);
    le32(hdr + 4, (uint32_t)im->nran);
    if (fwrite(hdr, 1, 8, f) != 8) die("write map: %s", strerror(errno));
    for (i = 0; i < im->nran; i++) {
        const range_t *r = &im->ran[i];
        le64(ent, r->orig);
        le64(ent + 8, r->len);
        if (r->kind == KIND_RECIPE) {
            ent[16] = 0;
            le32(ent + 17, 0);
            le64(ent + 21, running);
            running += r->len;
        } else {
            ent[16] = 1;
            le32(ent + 17, (uint32_t)r->idx);
            le64(ent + 21, r->src);
        }
        if (fwrite(ent, 1, 29, f) != 29) die("write map: %s", strerror(errno));
    }
    if (fclose(f) != 0) die("close map: %s", strerror(errno));
}

/* ---- classify: what every byte range IS (GCFG) --------------------------
 *
 * CLI-only manifest command, exactly like ext4fs's: `xfs classify <image>
 * <out>`. It is NOT one of the five plugin ABI commands and the ABI is not
 * touched; the core probes the manifest and falls back to "no classification"
 * when a pack lacks it.
 *
 * The question it answers is the one the core cannot answer itself, because
 * the core never parses this pack's recipe: of the bytes that ride verbatim,
 * which are STRUCTURE (superblocks, AG headers, the free-space and inode
 * B+trees, inode chunks, the log -- a rebuild needs them) and which are
 * GARBAGE (free space, AGFL blocks, unwritten extents, padding past the last
 * filesystem block -- bytes no member file needs and the filesystem does not
 * need to rebuild itself)?
 *
 * Why the core wants that distinction: garbage is the only verbatim byte an
 * operator can decide about. Structure cannot be dropped without losing the
 * filesystem; garbage can -- at the price of image bit-exactness, which then
 * has to be DECLARED, never silently repaired.
 *
 * Table (little-endian), written to <out>:
 *
 *   0   4   magic "GCFG"
 *   4   4   u32 version = 1
 *   8   4   u32 n_entries
 *  12   4   u32 block_size
 *  16  ..  n_entries x 24 B, off ascending, tiling [0, image_size)
 *            { u64 off; u64 len; u8 cls; u8 flags; u16 rsvd }
 *          cls   0 = meta_structure   1 = garbage   2 = member content
 *          flags bit0 = zero_verified (the pack READ these bytes and they are
 *                       all zero, so they can be reproduced without storing)
 *
 * `zero_verified` is a MEASUREMENT, never an assumption from "the block is
 * unallocated": a free block may hold bit-rot, and preserving it is this
 * pack's whole point. Garbage runs are therefore read back block by block and
 * split into all-zero / not-all-zero runs; a partial tail block can never be
 * elided and is reported as not-zero.
 *
 * Where each class comes from -- the classification is MEASURED, not guessed:
 *
 *   meta_structure  the AG header sectors (sb, AGF, AGI, AGFL); every block
 *                   of the by-block, by-count, inode and free-inode B+trees
 *                   (every level, not just the roots); every inode chunk an
 *                   inobt record names (sparse-chunk holes excepted, those
 *                   blocks are free space); the internal log. Everything else
 *                   that is allocated -- directory blocks, symlink targets,
 *                   attribute forks, member extent-btree blocks, unlinked
 *                   inodes -- falls into meta by being neither member nor
 *                   free, which is the conservative direction: calling a
 *                   needed block garbage is data loss, calling a free block
 *                   structure only costs compression.
 *   garbage         free space exactly as the by-block B+tree records it, plus
 *                   the blocks on the AGFL (they are removed from the btree),
 *                   plus a member's unwritten extents (its logical content is
 *                   zeros; the physical bytes are undefined) and anything
 *                   past sb_dblocks (padding in an oversized image file).
 *   member          the member partition this pack already builds for strip
 *                   and map (KIND_MEMBER ranges), byte-exact.
 *
 * Refusals (exit 3, no table written) rather than a guess: an external log
 * (sb_logstart == 0), a log that does not fit the image, a btree block whose
 * magic/level/count contradicts its AG header, a bnobt and cntbt that disagree
 * about the free set, a member range that lands in structure or in free space,
 * an AGFL block that the free-space btree also claims. A wrong `garbage` range
 * is data loss for whoever converts an image with it, so every contradiction
 * is fatal.
 */
#define GCFG_MAGIC "GCFG"
#define GCFG_VER   1
#define GCFG_ENT   24
#define GCFG_CLS_META   0
#define GCFG_CLS_GARB   1
#define GCFG_CLS_MEMBER 2
#define GCFG_F_ZERO     0x01

/* an inode chunk is 64 inodes; the sparse-inode hole mask carries one bit per
 * XFS_INODES_PER_HOLEMASK_BIT (4) inodes */
#define XFS_INODES_PER_CHUNK        64u
#define XFS_INODES_PER_HOLEMASK_BIT 4u

/* AG header field offsets. MEASURED, not recalled: xfs_db 6.19 on this box
 * prints the same layout for a crc=0 and a crc=1 image, and for 512B and 4K
 * sectors, because these fields are not part of the v5 tail. The header
 * POSITIONS inside the AG are sector-indexed (sb at sector 0, agf at 1,
 * agi at 2, agfl at 3), which is why the four are contiguous and fit inside
 * [0, 4 * sectsize). */
#define AGF_ROOTS_OFF       16   /* __be32[3]: bno, cnt, rmap */
#define AGF_LEVELS_OFF      28   /* __be32[3] */
#define AGF_FLFIRST_OFF     40
#define AGF_FLLAST_OFF      44
#define AGF_FLCOUNT_OFF     48
#define AGI_ROOT_OFF        20
#define AGI_LEVEL_OFF       24
#define AGI_FREE_ROOT_OFF   328
#define AGI_FREE_LEVEL_OFF  332
#define AGFL_CRC_HDR        36   /* v5: magic, seqno, uuid, lsn, crc */
#define AG_HEADER_SECTORS   4    /* sb, agf, agi, agfl: sectors 0..3 */
#define AG_NULLBLOCK        0xffffffffu
#define MAX_BT_LEVEL        8u   /* measured trees are 1-3 deep; 8 is a cap */

/* free-space / inode B+tree block magics, big-endian on disk */
#define BT_ABTB 0x41425442u      /* 'ABTB' v4 by-block */
#define BT_ABTC 0x41425443u      /* 'ABTC' v4 by-count */
#define BT_IABT 0x49414254u      /* 'IABT' v4 inode */
#define BT_AB3B 0x41423342u      /* 'AB3B' v5 by-block */
#define BT_AB3C 0x41423343u      /* 'AB3C' v5 by-count */
#define BT_IAB3 0x49414233u      /* 'IAB3' v5 inode */
#define BT_FIB3 0x46494233u      /* 'FIB3' v5 free-inode */

enum { CLS_TREE_BNO, CLS_TREE_CNT, CLS_TREE_INO, CLS_TREE_FINO };

typedef struct { uint64_t off, len; uint8_t cls, flags; } cls_ent;
typedef struct { uint64_t off, len; } cls_range;
typedef struct { cls_range *v; size_t n, cap; } cls_list;

typedef struct {
    img_t    *im;
    uint8_t  *meta;      /* one byte per fs block: 1 = structure */
    uint64_t  nblk;
    cls_list  garb;      /* garbage candidates, byte ranges, block aligned */
} cls_ctx;

typedef struct { uint64_t off, len, cls; } cls_evt;

static void cls_push(cls_list *l, uint64_t off, uint64_t len)
{
    if (!len) return;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 256;
        l->v = xrealloc(l->v, nc * sizeof *l->v);
        l->cap = nc;
    }
    l->v[l->n].off = off;
    l->v[l->n].len = len;
    l->n++;
}

/* AG-relative block number -> byte offset, validating the AG addressing. The
 * last AG may be shorter than sb_agblocks; a block past sb_dblocks is not part
 * of the filesystem even when it is inside the image file. */
static uint64_t ag_block_off(const img_t *im, uint64_t agno, uint64_t agbno)
{
    uint64_t aglen;
    if (agno >= im->agcount)
        decline("AG %" PRIu64 " is outside the filesystem", agno);
    aglen = im->dblocks - agno * (uint64_t)im->agblocks;
    if (agno * (uint64_t)im->agblocks >= im->dblocks)
        decline("AG %" PRIu64 " starts past sb_dblocks", agno);
    if (agbno >= im->agblocks || agbno >= aglen)
        decline("AG %" PRIu64 " block %" PRIu64 " is outside the "
                "filesystem (aglen %" PRIu64 ")", agno, agbno, aglen);
    return (agno * (uint64_t)im->agblocks + agbno) << im->blocklog;
}

static void cls_mark_meta(cls_ctx *c, uint64_t off, uint64_t len)
{
    const img_t *im = c->im;
    uint64_t b, b0, b1;
    if (!len) return;
    if (off >= im->fsize || len > im->fsize - off)
        decline("classify: structure range past end of image "
                "(off %" PRIu64 " len %" PRIu64 ")", off, len);
    b0 = off >> im->blocklog;
    b1 = (off + len - 1) >> im->blocklog;
    for (b = b0; b <= b1 && b < c->nblk; b++)
        c->meta[b] = 1;
}

static int cls_is_meta(const cls_ctx *c, uint64_t off)
{
    uint64_t b = off >> c->im->blocklog;
    return b < c->nblk && c->meta[b];
}

static void cls_push_garb(cls_ctx *c, uint64_t off, uint64_t len)
{
    if (!len) return;
    if (off >= c->im->fsize || len > c->im->fsize - off)
        decline("classify: garbage range past end of image "
                "(off %" PRIu64 " len %" PRIu64 ")", off, len);
    cls_push(&c->garb, off, len);
}

static uint32_t cls_bt_magic(const img_t *im, int kind)
{
    switch (kind) {
    case CLS_TREE_BNO: return im->v5 ? BT_AB3B : BT_ABTB;
    case CLS_TREE_CNT: return im->v5 ? BT_AB3C : BT_ABTC;
    case CLS_TREE_INO: return im->v5 ? BT_IAB3 : BT_IABT;
    default:           return BT_FIB3;      /* v5 only */
    }
}

static uint32_t cls_bt_keylen(int kind)
{
    return (kind == CLS_TREE_BNO || kind == CLS_TREE_CNT) ? 8u : 4u;
}

/* the inobt record, in both its shapes: with sparse inode chunks the record
 * carries holemask/ir_count (xfs_inobt_rec::sp), without them the classic
 * freecount. Both are 16 bytes, so the record stride never changes.
 *
 * ir_startino is an AG-RELATIVE inode number (xfs_inobt_rec.ir_startino), not
 * the global ino -- the kernel converts it with XFS_AGINO_TO_INO(mp, agno, ..).
 * Reading it as a global number refuses every AG but the first. */
static void cls_ino_rec(cls_ctx *c, uint64_t agno, const uint8_t *p)
{
    img_t *im = c->im;
    uint32_t startino = be32(p);
    uint16_t holemask = 0;
    uint32_t count = XFS_INODES_PER_CHUNK;
    uint64_t agino, agbno, j, nblk;
    int ibits = im->inopblog + im->agblklog;

    if (im->spinodes) {
        holemask = be16(p + 4);
        count = p[6];
    }
    if ((startino >> ibits) != 0)
        decline("AG %" PRIu64 ": inode chunk agino %u does not fit the AG "
                "(agino bits %d)", agno, startino, ibits);
    if (!count || count > XFS_INODES_PER_CHUNK)
        decline("AG %" PRIu64 ": inode chunk %u holds %u inodes",
                agno, startino, count);
    agino = startino;
    agbno = agino >> im->inopblog;
    nblk = XFS_INODES_PER_CHUNK / im->inopblock;
    /* The chunk does NOT have to start at a multiple of 64 aginos, and
     * requiring it refuses every filesystem made with -i sparse=0: measured on
     * this box, such an image's inobt record is startino=96 (block 12), which
     * the kernel is free to allocate because xfs_ialloc_ag_alloc only asks for
     * chunk alignment when sparse inodes are enabled. What must hold is that
     * the chunk's nblk blocks are INSIDE the AG. */
    {
        uint64_t aglen = im->dblocks - agno * (uint64_t)im->agblocks;
        if (aglen > im->agblocks)
            aglen = im->agblocks;
        if (agbno + nblk > aglen)
            decline("AG %" PRIu64 ": inode chunk at agino %u (block %" PRIu64
                    ", %" PRIu64 " blocks) runs past the end of the AG "
                    "(%" PRIu64 " blocks)", agno, startino, agbno, nblk,
                    aglen);
    }
    for (j = 0; j < nblk; j++) {
        int hole = 0;
        if (im->spinodes) {
            uint32_t k, nb = im->inopblock / XFS_INODES_PER_HOLEMASK_BIT;
            uint32_t k0 = (uint32_t)(j * im->inopblock /
                                     XFS_INODES_PER_HOLEMASK_BIT);
            for (k = 0; k < nb; k++)
                if (holemask & (1u << (k0 + k))) { hole = 1; break; }
        }
        if (!hole)
            cls_mark_meta(c, ag_block_off(im, agno, agbno + j), im->blocksize);
    }
}

/* Walk one AG B+tree, marking every block it occupies as structure and (for
 * the by-block tree) collecting its leaf records as free extents. `level` is
 * the BLOCK level: 0 is a leaf, and the AGF/AGI's "1 level" tree is one leaf
 * block, so the caller passes (agf_levels[i] - 1). An internal node's pointer
 * array starts after maxrecs keys. */
static void cls_bt_walk(cls_ctx *c, uint64_t agno, uint64_t agbno,
                        uint32_t level, int kind, cls_list *freev)
{
    img_t *im = c->im;
    uint32_t hdr = im->v5 ? 56u : 16u;
    uint32_t klen = cls_bt_keylen(kind);
    uint32_t rlen = (kind == CLS_TREE_BNO || kind == CLS_TREE_CNT) ? 8u : 16u;
    uint32_t maxrecs, nrecs, i, magic = cls_bt_magic(im, kind);
    uint64_t off = ag_block_off(im, agno, agbno);
    uint8_t *blk;

    if (level > MAX_BT_LEVEL)
        decline("AG %" PRIu64 " block %" PRIu64 ": btree level %u out of "
                "range", agno, agbno, level);
    cls_mark_meta(c, off, im->blocksize);
    blk = xmalloc(im->blocksize);
    read_at(im, off, blk, im->blocksize);
    if (be32(blk) != magic)
        decline("AG %" PRIu64 " block %" PRIu64 ": btree magic 0x%08x, "
                "expected 0x%08x -- structural garbage where a tree block "
                "belongs", agno, agbno, be32(blk), magic);
    if (be16(blk + 4) != level)
        decline("AG %" PRIu64 " block %" PRIu64 ": block says level %u, the "
                "AG header says %u", agno, agbno, be16(blk + 4), level);
    nrecs = be16(blk + 6);
    if (level == 0) {
        maxrecs = (im->blocksize - hdr) / rlen;
        /* nrecs == 0 is LEGAL here and must not be a refusal: a fresh AG's
         * inode btree root is an EMPTY leaf (measured on a mkfs.xfs 6.19
         * 4-AG image: AG2 block 3 = the free-inode btree root with 0
         * records), and an AG whose free space sits entirely on its AGFL has
         * an empty by-block leaf. The consequence is safe in both cases -- no
         * inode chunk to mark, no free extent to call garbage, so the blocks
         * land in `meta_structure`, which is the conservative direction. */
        if (nrecs > maxrecs)
            decline("AG %" PRIu64 " block %" PRIu64 ": %u leaf records, "
                    "max %u", agno, agbno, nrecs, maxrecs);
        for (i = 0; i < nrecs; i++) {
            const uint8_t *p = blk + hdr + (size_t)i * rlen;
            if (kind == CLS_TREE_INO || kind == CLS_TREE_FINO) {
                cls_ino_rec(c, agno, p);
            } else if (freev) {
                uint32_t start = be32(p), len = be32(p + 4);
                if (!len || (uint64_t)start + len > im->agblocks)
                    decline("AG %" PRIu64 " block %" PRIu64 ": free extent "
                            "%u+%u does not fit the AG", agno, agbno,
                            start, len);
                cls_push(freev, ag_block_off(im, agno, start),
                         (uint64_t)len << im->blocklog);
            }
        }
    } else {
        maxrecs = (im->blocksize - hdr) / (klen + 4u);
        if (!nrecs || nrecs > maxrecs)
            decline("AG %" PRIu64 " block %" PRIu64 ": %u node records, "
                    "max %u", agno, agbno, nrecs, maxrecs);
        for (i = 0; i < nrecs; i++) {
            uint32_t ptr = be32(blk + hdr + (size_t)maxrecs * klen +
                                (size_t)i * 4u);
            cls_bt_walk(c, agno, ptr, level - 1, kind, freev);
        }
    }
    free(blk);
}

/* The AGFL blocks are free space that is NOT in the free-space btree: XFS
 * takes them out of the btree when it puts them on the list. They carry no
 * content a rebuild needs, so they are garbage -- but only after proving the
 * window (flfirst/flcount) is parseable, because reading the wrong four bytes
 * here would name an arbitrary allocated block as garbage. */
static void cls_agfl(cls_ctx *c, uint64_t agno, uint64_t flfirst,
                     uint64_t flcount, cls_list *btree_free)
{
    img_t *im = c->im;
    uint32_t entsz = im->v5 ? AGFL_CRC_HDR : 0u;
    uint32_t nent = (im->sectsize - entsz) / 4u;
    uint64_t agb0 = ag_block_off(im, agno, 0);
    uint8_t *buf = xmalloc(im->sectsize);
    uint64_t i;

    read_at(im, agb0 + 3ull * im->sectsize, buf, im->sectsize);
    if (im->v5) {
        if (be32(buf) != 0x5841464cu)   /* 'XAFL' */
            decline("AG %" PRIu64 ": AGFL magic 0x%08x", agno, be32(buf));
        if (be32(buf + 4) != agno)
            decline("AG %" PRIu64 ": AGFL belongs to AG %u", agno,
                    be32(buf + 4));
    }
    if (!nent || flfirst >= nent || flcount > nent)
        decline("AG %" PRIu64 ": AGFL window %" PRIu64 "+%" PRIu64
                " does not fit %u entries", agno, flfirst, flcount, nent);
    for (i = 0; i < flcount; i++) {
        uint32_t slot = (uint32_t)((flfirst + i) % nent);
        uint32_t bno = be32(buf + entsz + 4u * slot);
        uint64_t off, k;
        if (bno == AG_NULLBLOCK)
            decline("AG %" PRIu64 ": AGFL slot %u is null inside the used "
                    "window", agno, slot);
        off = ag_block_off(im, agno, bno);
        /* a block on the AGFL is out of the btree by construction; an
         * overlap means one of the two structures was mis-read */
        for (k = 0; k < btree_free->n; k++)
            if (off >= btree_free->v[k].off &&
                off < btree_free->v[k].off + btree_free->v[k].len)
                decline("AG %" PRIu64 ": AGFL block %u is also in the "
                        "free-space btree", agno, bno);
        if (cls_is_meta(c, off))
            decline("AG %" PRIu64 ": AGFL block %u is also structure",
                    agno, bno);
        cls_push_garb(c, off, im->blocksize);
    }
    free(buf);
}

static int cls_range_cmp(const void *a, const void *b)
{
    const cls_range *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

static int cls_evt_cmp(const void *a, const void *b)
{
    const cls_evt *x = a, *y = b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return x->cls < y->cls ? -1 : x->cls > y->cls;
}

static void cls_collect_ag(cls_ctx *c, uint64_t agno)
{
    img_t *im = c->im;
    uint64_t agb0 = ag_block_off(im, agno, 0);
    uint8_t *buf = xmalloc(im->blocksize);
    cls_list bno = { 0, 0, 0 }, cnt = { 0, 0, 0 };
    uint64_t agf, agi;
    uint32_t broot, croot, blevel, clevel, iroot, ilevel, froot = 0, flevel = 0;
    uint64_t flfirst, flcount;
    size_t i;

    /* headers: [0, 4 * sectsize) is sb, AGF, AGI, AGFL, in sector steps */
    cls_mark_meta(c, agb0, 4ull * im->sectsize);

    agf = agb0 + 1ull * im->sectsize;
    read_at(im, agf, buf, im->blocksize);
    if (be32(buf) != 0x58414746u)          /* 'XAGF' */
        decline("AG %" PRIu64 ": AGF magic 0x%08x", agno, be32(buf));
    if (be32(buf + 4) != 1u)
        decline("AG %" PRIu64 ": AGF version %u", agno, be32(buf + 4));
    if (be32(buf + 8) != agno)
        decline("AG %" PRIu64 ": AGF says AG %u", agno, be32(buf + 8));
    if (be32(buf + 12) != im->agblocks &&
        be32(buf + 12) != im->dblocks - agno * (uint64_t)im->agblocks)
        decline("AG %" PRIu64 ": AGF length %u", agno, be32(buf + 12));
    broot  = be32(buf + AGF_ROOTS_OFF + 0);
    croot  = be32(buf + AGF_ROOTS_OFF + 4);
    blevel = be32(buf + AGF_LEVELS_OFF + 0);
    clevel = be32(buf + AGF_LEVELS_OFF + 4);
    flfirst = be32(buf + AGF_FLFIRST_OFF);
    flcount = be32(buf + AGF_FLCOUNT_OFF);

    agi = agb0 + 2ull * im->sectsize;
    read_at(im, agi, buf, im->blocksize);
    if (be32(buf) != 0x58414749u)          /* 'XAGI' */
        decline("AG %" PRIu64 ": AGI magic 0x%08x", agno, be32(buf));
    if (be32(buf + 4) != 1u)
        decline("AG %" PRIu64 ": AGI version %u", agno, be32(buf + 4));
    if (be32(buf + 8) != agno)
        decline("AG %" PRIu64 ": AGI says AG %u", agno, be32(buf + 8));
    iroot  = be32(buf + AGI_ROOT_OFF);
    ilevel = be32(buf + AGI_LEVEL_OFF);
    if (im->v5 && im->finobt) {
        froot  = be32(buf + AGI_FREE_ROOT_OFF);
        flevel = be32(buf + AGI_FREE_LEVEL_OFF);
    }
    free(buf);

    if (!blevel || !clevel || !ilevel)
        decline("AG %" PRIu64 ": AGF/AGI declare a zero-level btree "
                "(bno %u cnt %u ino %u)", agno, blevel, clevel, ilevel);
    cls_bt_walk(c, agno, broot, blevel - 1, CLS_TREE_BNO, &bno);
    cls_bt_walk(c, agno, croot, clevel - 1, CLS_TREE_CNT, &cnt);
    if (bno.n != cnt.n)
        decline("AG %" PRIu64 ": bnobt has %zu free extents, cntbt has %zu",
                agno, bno.n, cnt.n);
    qsort(bno.v, bno.n, sizeof *bno.v, cls_range_cmp);
    qsort(cnt.v, cnt.n, sizeof *cnt.v, cls_range_cmp);
    for (i = 0; i < bno.n; i++)
        if (bno.v[i].off != cnt.v[i].off || bno.v[i].len != cnt.v[i].len)
            decline("AG %" PRIu64 ": bnobt and cntbt disagree about the "
                    "free set at %" PRIu64, agno, bno.v[i].off);
    for (i = 0; i < bno.n; i++)
        cls_push(&c->garb, bno.v[i].off, bno.v[i].len);

    cls_bt_walk(c, agno, iroot, ilevel - 1, CLS_TREE_INO, NULL);
    if (flevel)
        cls_bt_walk(c, agno, froot, flevel - 1, CLS_TREE_FINO, NULL);

    cls_agfl(c, agno, flfirst, flcount, &bno);
    free(bno.v);
    free(cnt.v);
}

/* Split a block-aligned garbage range into runs of all-zero / not-all-zero
 * blocks. The zero test is a READ of the image: a free block may hold bit-rot,
 * and a hole is only reproducible when it is proved to be zeros. A partial
 * tail block cannot be elided by a block-granular consumer, so it is reported
 * as not-zero. */
static void cls_emit_garb(cls_ctx *c, cls_ent **v, size_t *n, size_t *cap,
                          uint64_t lo, uint64_t hi)
{
    const img_t *im = c->im;
    uint64_t bs = im->blocksize, b = lo, run_start;
    uint8_t *buf = xmalloc(bs);
    int run_zero = -1;

    run_start = lo;
    for (; b + bs <= hi; b += bs) {
        uint64_t k;
        int z = 1;
        read_at(c->im, b, buf, bs);
        for (k = 0; k < bs; k++)
            if (buf[k]) { z = 0; break; }
        if (run_zero < 0) {
            run_zero = z;
            run_start = b;
        } else if (z != run_zero) {
            if (*n == *cap) {
                size_t nc = *cap ? *cap * 2 : 256;
                *v = xrealloc(*v, nc * sizeof **v);
                *cap = nc;
            }
            (*v)[*n].off = run_start;
            (*v)[*n].len = b - run_start;
            (*v)[*n].cls = GCFG_CLS_GARB;
            (*v)[*n].flags = run_zero ? GCFG_F_ZERO : 0;
            (*n)++;
            run_start = b;
            run_zero = z;
        }
    }
    if (run_zero >= 0 && run_start < b) {
        if (*n == *cap) {
            size_t nc = *cap ? *cap * 2 : 256;
            *v = xrealloc(*v, nc * sizeof **v);
            *cap = nc;
        }
        (*v)[*n].off = run_start;
        (*v)[*n].len = b - run_start;
        (*v)[*n].cls = GCFG_CLS_GARB;
        (*v)[*n].flags = run_zero ? GCFG_F_ZERO : 0;
        (*n)++;
    }
    if (b < hi) {
        if (*n == *cap) {
            size_t nc = *cap ? *cap * 2 : 256;
            *v = xrealloc(*v, nc * sizeof **v);
            *cap = nc;
        }
        (*v)[*n].off = b;
        (*v)[*n].len = hi - b;
        (*v)[*n].cls = GCFG_CLS_GARB;
        (*v)[*n].flags = 0;             /* partial block: not verifiable */
        (*n)++;
    }
    free(buf);
}

static void cls_write(const char *path, const cls_ent *tab, size_t n,
                      uint32_t blocksize, uint64_t fsize)
{
    uint8_t hdr[16], ent[GCFG_ENT];
    uint64_t pos = 0;
    size_t i;
    FILE *f;

    /* The tiling is proved BEFORE the file is opened. A table that does not
     * cover the image exactly is a bug, not a warning, and handing it to the
     * core would be worse than refusing. */
    if (!n)
        die("classify: empty table");
    if (n > 0xffffffffu)
        die("classify: %zu entries do not fit the u32 header", n);
    for (i = 0; i < n; i++) {
        if (tab[i].off != pos)
            die("classify: table does not tile the image (want %" PRIu64
                ", got %" PRIu64 ")", pos, tab[i].off);
        if (!tab[i].len || tab[i].len > fsize - pos)
            die("classify: entry %zu has len %" PRIu64 " at %" PRIu64,
                i, tab[i].len, pos);
        pos += tab[i].len;
    }
    if (pos != fsize)
        die("classify: table covers %" PRIu64 " of %" PRIu64
            " image bytes", pos, fsize);

    f = fopen(path, "wb");
    if (!f) die("create %s: %s", path, strerror(errno));
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, GCFG_MAGIC, 4);
    le32(hdr + 4, GCFG_VER);
    le32(hdr + 8, (uint32_t)n);
    le32(hdr + 12, blocksize);
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr)
        die("write %s: %s", path, strerror(errno));
    for (i = 0; i < n; i++) {
        memset(ent, 0, sizeof ent);
        le64(ent + 0, tab[i].off);
        le64(ent + 8, tab[i].len);
        ent[16] = tab[i].cls;
        ent[17] = tab[i].flags;
        if (fwrite(ent, 1, sizeof ent, f) != sizeof ent)
            die("write %s: %s", path, strerror(errno));
    }
    if (fclose(f) != 0)
        die("close %s: %s", path, strerror(errno));
}

static void cmd_classify(const char *in, const char *out)
{
    img_t *im = parse_image_ex(in, 0);
    cls_ctx c;
    cls_evt *ev = NULL;
    cls_ent *tab = NULL;
    size_t nev = 0, cev = 0, ntab = 0, ctab = 0, i;
    uint64_t nb, b, meta_b = 0, garb_b = 0, mem_b = 0, zero_b = 0;

    if (!im->logstart)
        decline("an external log (sb_logstart == 0) is not supported");
    if (!im->logblocks)
        decline("the superblock declares a zero-length log");

    memset(&c, 0, sizeof c);
    c.im = im;
    c.nblk = (im->fsize + im->blocksize - 1) >> im->blocklog;
    c.meta = xmalloc((size_t)c.nblk);
    memset(c.meta, 0, (size_t)c.nblk);

    /* the log is one contiguous run of filesystem blocks, and sb_logstart is
     * an XFS block NUMBER in the PACKED form (agno << agblklog) | agbno -- the
     * same form fsb_off() takes, NOT a linear block index. MEASURED, because
     * dividing by sb_agblocks is the plausible wrong answer and it is wrong:
     * mkfs.xfs 6.19 on a 320 MiB / 4-AG image (agblklog 15) writes
     * logstart = 65541 = (2 << 15) | 5, and the log head (0xfeedbabe) is at
     * AG2 block 5. `65541 / 20480 = 3` and `% 20480 = 4101` would name AG3
     * block 4101 -- 64 MiB away from the log and inside live free space, so
     * that mistake reports the real log as garbage and 64 MiB of free space as
     * structure. */
    {
        uint64_t lagno  = im->logstart >> im->agblklog;
        uint64_t lagbno = im->logstart & ((1ULL << im->agblklog) - 1);
        uint64_t aglen;
        if (lagno >= im->agcount)
            decline("the internal log starts in AG %" PRIu64 ", outside the "
                    "filesystem (%u AGs)", lagno, im->agcount);
        aglen = im->dblocks - lagno * (uint64_t)im->agblocks;
        if (aglen > im->agblocks)
            aglen = im->agblocks;
        if (lagbno < AG_HEADER_SECTORS)
            decline("the internal log starts inside the AG headers "
                    "(AG %" PRIu64 " block %" PRIu64 ")", lagno, lagbno);
        if (im->logblocks > aglen - lagbno)
            decline("the internal log (AG %" PRIu64 " block %" PRIu64
                    " + %" PRIu64 " blocks) does not fit its AG (%" PRIu64
                    " blocks)", lagno, lagbno, (uint64_t)im->logblocks,
                    aglen);
        cls_mark_meta(&c, ag_block_off(im, lagno, lagbno),
                      (uint64_t)im->logblocks << im->blocklog);
    }

    for (nb = 0; nb < im->agcount; nb++)
        cls_collect_ag(&c, nb);

    /* padding past the last filesystem block (an image file may be larger
     * than the filesystem it holds); no member and no structure lives there */
    {
        uint64_t fsbytes = im->dblocks << im->blocklog;
        if (im->fsize > fsbytes)
            cls_push_garb(&c, fsbytes, im->fsize - fsbytes);
    }
    /* a member's unwritten extents: logical zeros, undefined physical bytes,
     * and they are NOT part of the member partition (see build_partition) */
    for (i = 0; i < im->nmem; i++) {
        member_t *m = &im->mem[i];
        size_t j;
        for (j = 0; j < m->nexts; j++) {
            ext_t *e = &m->exts[j];
            if (!e->unwritten || e->startblock == NULLSTARTBLOCK)
                continue;
            cls_push_garb(&c, fsb_off(im, e->startblock),
                          (uint64_t)e->count << im->blocklog);
        }
    }

    /* events: the member partition byte-exact, and every garbage block that
     * is not also structure. Sorting them once gives the tiling. */
    for (i = 0; i < im->nran; i++) {
        if (im->ran[i].kind != KIND_MEMBER)
            continue;
        if (nev == cev) {
            cev = cev ? cev * 2 : 256;
            ev = xrealloc(ev, cev * sizeof *ev);
        }
        ev[nev].off = im->ran[i].orig;
        ev[nev].len = im->ran[i].len;
        ev[nev].cls = GCFG_CLS_MEMBER;
        nev++;
    }
    b = 0;
    qsort(c.garb.v, c.garb.n, sizeof *c.garb.v, cls_range_cmp);
    for (i = 0; i < c.garb.n; i++) {
        uint64_t lo = c.garb.v[i].off, hi = lo + c.garb.v[i].len, run = 0;
        if (lo < b)
            lo = b;                 /* an overlapping free record: skip the
                                     * part already emitted */
        for (; lo < hi; lo += im->blocksize) {
            if (cls_is_meta(&c, lo)) {
                if (run && lo > run) {
                    if (nev == cev) {
                        cev = cev ? cev * 2 : 256;
                        ev = xrealloc(ev, cev * sizeof *ev);
                    }
                    ev[nev].off = run; ev[nev].len = lo - run;
                    ev[nev].cls = GCFG_CLS_GARB; nev++;
                    run = 0;
                }
                continue;
            }
            if (!run) run = lo;
        }
        if (run && hi > run) {
            if (nev == cev) {
                cev = cev ? cev * 2 : 256;
                ev = xrealloc(ev, cev * sizeof *ev);
            }
            ev[nev].off = run; ev[nev].len = hi - run;
            ev[nev].cls = GCFG_CLS_GARB; nev++;
        }
        if (hi > b) b = hi;
    }
    qsort(ev, nev, sizeof *ev, cls_evt_cmp);

    /* emit, filling every gap with structure */
    b = 0;
    for (i = 0; i < nev; i++) {
        uint64_t off = ev[i].off, len = ev[i].len, k;
        if (len > im->fsize - off)
            die("classify: event past end of image");
        if (off < b)
            die("classify: overlapping events at %" PRIu64, off);
        if (off > b) {
            if (ntab == ctab) {
                ctab = ctab ? ctab * 2 : 256;
                tab = xrealloc(tab, ctab * sizeof *tab);
            }
            tab[ntab].off = b; tab[ntab].len = off - b;
            tab[ntab].cls = GCFG_CLS_META; tab[ntab].flags = 0;
            ntab++;
            meta_b += off - b;
        }
        if (ev[i].cls == GCFG_CLS_MEMBER) {
            for (k = 0; k < len; k += im->blocksize) {
                uint64_t bb = off + k;
                if (bb + im->blocksize > im->fsize)
                    break;
                if (cls_is_meta(&c, bb))
                    decline("classify: member range at %" PRIu64 " lands in "
                            "structure", bb);
            }
            if (ntab == ctab) {
                ctab = ctab ? ctab * 2 : 256;
                tab = xrealloc(tab, ctab * sizeof *tab);
            }
            tab[ntab].off = off; tab[ntab].len = len;
            tab[ntab].cls = GCFG_CLS_MEMBER; tab[ntab].flags = 0;
            ntab++;
            mem_b += len;
        } else {
            size_t before = ntab;
            cls_emit_garb(&c, &tab, &ntab, &ctab, off, off + len);
            for (k = before; k < ntab; k++) {
                garb_b += tab[k].len;
                if (tab[k].flags & GCFG_F_ZERO) zero_b += tab[k].len;
            }
        }
        b = off + len;
    }
    if (b < im->fsize) {
        if (ntab == ctab) {
            ctab = ctab ? ctab * 2 : 256;
            tab = xrealloc(tab, ctab * sizeof *tab);
        }
        tab[ntab].off = b; tab[ntab].len = im->fsize - b;
        tab[ntab].cls = GCFG_CLS_META; tab[ntab].flags = 0;
        ntab++;
        meta_b += im->fsize - b;
    }

    cls_write(out, tab, ntab, im->blocksize, im->fsize);
    fprintf(stderr, "cls: meta %" PRIu64 " garbage %" PRIu64
                    " (zero %" PRIu64 ") member %" PRIu64
                    " entries %zu image %" PRIu64 "\n",
            meta_b, garb_b, zero_b, mem_b, ntab, im->fsize);
    free(tab);
    free(ev);
    free(c.garb.v);
    free(c.meta);
}

static void cmd_estimate(const char *in)
{
    img_t *im = parse_image(in);
    printf("%" PRIu64 "\n", (uint64_t)(im->sum_usize + (64ULL << 20)));
}

/* rebuild: the recipe's tables drive everything; member payload comes
 * from <dir>/<idx>, checked against the announced usize. */
static void cmd_rebuild(const char *recipe, const char *dir, const char *out)
{
    int      rfd, ofd;
    uint8_t  hdr[RECIPE_HDR];
    uint64_t image_size, nmem64, nran64, m, r, pos;
    uint8_t *mtab, *rtab, *buf;
    struct   { uint64_t ino, size; int fd; } *mbs;
    uint64_t payload;

    rfd = open(recipe, O_RDONLY);
    if (rfd < 0) die("open %s: %s", recipe, strerror(errno));
    {
        size_t done = 0;
        while (done < sizeof hdr) {
            ssize_t rr = read(rfd, hdr + done, sizeof hdr - done);
            if (rr < 0) die("read recipe: %s", strerror(errno));
            if (rr == 0) die("short recipe");
            done += (size_t)rr;
        }
    }
    if (memcmp(hdr, RECIPE_MAGIC, 8) != 0) die("bad recipe magic");
    image_size = le64v(hdr + 8);
    nmem64 = le32v(hdr + 16);
    nran64 = le32v(hdr + 20);
    if (nmem64 > MAX_MEMBERS || nran64 > 4 * MAX_MEMBERS + 4)
        die("implausible recipe table sizes");
    mtab = xmalloc((size_t)nmem64 * RECIPE_MENT);
    rtab = xmalloc((size_t)nran64 * RECIPE_RENT);
    {
        size_t need = (size_t)nmem64 * RECIPE_MENT +
                      (size_t)nran64 * RECIPE_RENT, done = 0;
        uint8_t *tmp = xmalloc(need);
        while (done < need) {
            ssize_t rr = read(rfd, tmp + done, need - done);
            if (rr < 0) die("read recipe tables: %s", strerror(errno));
            if (rr == 0) die("short recipe tables");
            done += (size_t)rr;
        }
        memcpy(mtab, tmp, (size_t)nmem64 * RECIPE_MENT);
        memcpy(rtab, tmp + (size_t)nmem64 * RECIPE_MENT,
               (size_t)nran64 * RECIPE_RENT);
        free(tmp);
    }
    mbs = xmalloc((size_t)nmem64 * sizeof *mbs);
    for (m = 0; m < nmem64; m++) {
        mbs[m].ino = le64v(mtab + m * RECIPE_MENT);
        mbs[m].size = le64v(mtab + m * RECIPE_MENT + 8);
        mbs[m].fd = -1;
    }
    ofd = open_out(out);
    buf = xmalloc(CHUNK);
    payload = RECIPE_HDR + nmem64 * RECIPE_MENT + nran64 * RECIPE_RENT;
    pos = 0;
    for (r = 0; r < nran64; r++) {
        uint64_t orig = le64v(rtab + r * RECIPE_RENT);
        uint64_t len  = le64v(rtab + r * RECIPE_RENT + 8);
        uint64_t kind = le64v(rtab + r * RECIPE_RENT + 16);
        uint64_t idx  = le64v(rtab + r * RECIPE_RENT + 24);
        uint64_t src  = le64v(rtab + r * RECIPE_RENT + 32);
        if (orig != pos || !len || len > image_size - pos)
            die("recipe ranges do not partition the image");
        if (kind == KIND_RECIPE) {
            fd_copy(rfd, payload, ofd, len, buf);
            payload += len;
        } else if (kind == KIND_MEMBER) {
            for (m = 0; m < nmem64; m++)
                if (mbs[m].ino == idx) break;
            if (m == nmem64) die("member %" PRIu64 " not in the recipe", idx);
            if (src > mbs[m].size || len > mbs[m].size - src)
                die("member %" PRIu64 " range past its usize", idx);
            if (mbs[m].fd < 0) {
                char path[1024];
                struct stat st;
                snprintf(path, sizeof path, "%s/%" PRIu64, dir, idx);
                mbs[m].fd = open(path, O_RDONLY);
                if (mbs[m].fd < 0)
                    die("open member %s: %s", path, strerror(errno));
                if (fstat(mbs[m].fd, &st) != 0 ||
                    (uint64_t)st.st_size != mbs[m].size)
                    die("member %" PRIu64 ": size mismatch (got %llu, "
                        "want %llu)", idx,
                        (unsigned long long)(st.st_size),
                        (unsigned long long)mbs[m].size);
            }
            fd_copy(mbs[m].fd, src, ofd, len, buf);
        } else {
            die("bad range kind %llu", (unsigned long long)kind);
        }
        pos += len;
    }
    if (pos != image_size) die("recipe under-covers the image");
    if (ftruncate(ofd, (off_t)image_size) != 0)
        die("truncate: %s", strerror(errno));
    for (m = 0; m < nmem64; m++)
        if (mbs[m].fd >= 0) close(mbs[m].fd);
    free(buf); free(mtab); free(rtab); free(mbs);
    if (close(ofd) != 0) die("close %s: %s", out, strerror(errno));
    close(rfd);
}

/* -------------------------------- main --------------------------------- */


/* ---- ivpack plugin C ABI export (ADR-007) --------------------------------
 * Built as libxfs.so with -DIVPACK_SHARED_LIB -fPIC -shared; the fork
 * guard in ivpack_impl.h keeps the CLI's exit(3)=decline / exit(1)=error
 * contract intact inside a long-lived worker. See ivpack_impl.h. */
static const ivpack_desc s_xfs_desc = {
    IVPACK_API_VERSION, "xfs", "1.0.0", "containerpack", 0
};

const ivpack_desc *ivpack_get_desc(void) { return &s_xfs_desc; }

/* xfs's cmd_* are void and signal every refusal with exit(3)/die(), which is
 * exactly why the call runs in the forked child (see ivpack_impl.h). */
static int xfs_iv_call_enumerate(const ivpack_container_args *a)
{ cmd_enumerate(a->in_path, a->out_path); return 0; }
static int xfs_iv_call_extract(const ivpack_container_args *a)
{ cmd_extract(a->in_path, a->extract_idx, a->out_path); return 0; }
static int xfs_iv_call_strip(const ivpack_container_args *a)
{ cmd_strip(a->in_path, a->out_path); return 0; }
static int xfs_iv_call_rebuild(const ivpack_container_args *a)
{ cmd_rebuild(a->recipe_path, a->mbr_dir, a->out_path); return 0; }
static int xfs_iv_call_map(const ivpack_container_args *a)
{ cmd_map(a->in_path, a->out_path); return 0; }

IVPACK_DEFINE_CONTAINER_CMD(xfs, IVPACK_GLUE_NONE())

IVPACK_DEFINE_CONTAINER_ESTIMATE(xfs, IVPACK_GLUE_NONE(),
                             cmd_estimate(a); rc = 0;)

#ifndef IVPACK_SHARED_LIB
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "enumerate") && argc == 4)
        cmd_enumerate(argv[2], argv[3]);
    else if (!strcmp(argv[1], "extract") && argc == 5)
        cmd_extract(argv[2], argv[3], argv[4]);
    else if (!strcmp(argv[1], "strip") && argc == 4)
        cmd_strip(argv[2], argv[3]);
    else if (!strcmp(argv[1], "rebuild") && argc == 5)
        cmd_rebuild(argv[2], argv[3], argv[4]);
    else if (!strcmp(argv[1], "map") && argc == 4)
        cmd_map(argv[2], argv[3]);
    else if (!strcmp(argv[1], "classify") && argc == 4)
        cmd_classify(argv[2], argv[3]);
    else if (!strcmp(argv[1], "estimate") && argc == 3)
        cmd_estimate(argv[2]);
    else
        return 2;
    return 0;
}
#endif /* !IVPACK_SHARED_LIB */

