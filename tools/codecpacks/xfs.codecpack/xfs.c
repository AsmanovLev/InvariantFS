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
/* v4 features2 bits we accept; FTYPE lives here on v4 */
#define FEAT2_OK     0x2bbu
#define FEAT2_FTYPE  0x200u

#define NULLSTARTBLOCK  ((1ULL << 52) - 1)  /* bmbt hole marker */
#define DIR_LEAF_DBLOCK (1ULL << 23)        /* leaf/free area: file blocks
                                             * >= this are not dir data */
#define MAX_MEMBERS    65536u
#define MAX_IDX        65535u
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
 * refusal. */
static img_t *parse_image(const char *path)
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
    if (!im->nmem)
        decline("no regular files (a zero-member container goes generic)");
    build_partition(im);
    return im;
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
    else if (!strcmp(argv[1], "estimate") && argc == 3)
        cmd_estimate(argv[2]);
    else
        return 2;
    return 0;
}
#endif /* !IVPACK_SHARED_LIB */

