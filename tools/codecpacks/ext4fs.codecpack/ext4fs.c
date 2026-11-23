/*
 * ext4fs.c — ext4fs containerpack helper (InvariantFS WP16a/WP16b ABI v1.1).
 *
 * Decomposes an ext4 filesystem image into per-file members that flow
 * through the whole InvariantFS codec pipeline (text PPMd batching /
 * binary ZSTD+BCJ batching / generic ZSTD-19), with a seekable MRMP map
 * so reads of the original image splice locally from the recipe + member
 * siblings and never exec this pack.
 *
 * Single C11 file, libc/POSIX only. All image I/O streams in <= 8 MiB
 * windows; the image is never mmap'd or slurped whole.
 *
 * Commands (fixed argv, no shell; exit 0 = ok, 3 = decline (honest
 * refusal), anything else = error):
 *
 *   ext4fs enumerate <in> <out>            member table "idx<TAB>sname<TAB>usize"
 *   ext4fs extract   <in> <idx> <out>      member idx's logical bytes (usize)
 *   ext4fs strip     <in> <out>            recipe (below)
 *   ext4fs rebuild   <recipe> <dir> <out>  original image, bit-exact
 *   ext4fs map       <in> <out>            MRMP member map (WP16b)
 *   ext4fs estimate  <in>                  sum(member usize) + 64 MiB, bare
 *
 * MEMBERS
 *
 *   idx   = ext4 inode number (must fit the ABI range 0..ABI_MAX_IDX);
 *   set   = used inodes >= s_first_ino whose i_mode is S_IFREG — and only
 *           those. Directories, symlinks (fast targets ride inside the
 *           inode, slow targets sit in data blocks), device/fifo/socket
 *           nodes and the reserved inodes (< s_first_ino: journal, resize,
 *           ...) are all metadata: their blocks stay in the recipe
 *           verbatim. Hardlinks share one inode, hence ONE member; the
 *           duplicate directory entries stay in the recipe (dir blocks
 *           are metadata). sname = sanitized basename from the directory
 *           walk ("ino<N>" for unreachable inodes); collisions are fine.
 *   usize = i_size logical bytes. Holes and unwritten (fallocate'd)
 *           extents read as zeros on extract; unwritten extents' physical
 *           blocks are NOT member content (they hold bit-rot garbage) and
 *           stay in the recipe verbatim. A written extent's on-disk tail
 *           past i_size is likewise recipe, not member content.
 *
 * RECIPE FORMAT (pack-owned; the FS never parses it; all integers LE)
 *
 *    0   8   magic "E4RCP002"
 *    8   8   u64 image_size        (byte size of the original image file)
 *   16   4   u32 block_size
 *   20   4   u32 n_members
 *   24   4   u32 n_ranges         (recipe byte-range count)
 *   28   4   u32 n_extents        (total member extent entries)
 *   32  ..  member table: n_members x 24 B, idx ascending
 *             { u32 idx; u32 n_ext; u64 usize; u64 reserved0 }
 *    ..  ..  extent map: n_extents x 32 B, grouped by member in member
 *             table order, logical_off ascending within a member
 *             { u32 idx; u32 reserved0; u64 logical_off; u64 image_off;
 *               u64 len }
 *    ..  ..  range table: n_ranges x 16 B, off ascending, non-overlapping
 *             { u64 off; u64 len }
 *    ..  ..  range bytes: the image bytes of every range, concatenated in
 *             range-table order. data_base = 32 + 24*M + 32*E + 16*R.
 *
 *   E4RCP002 stores the NON-ZERO runs of the non-member bytes only. Every
 *   block of that region is READ and classified (gcfg_split_zero), and the
 *   all-zero runs are written down nowhere: their complement in
 *   [0, image_size) IS zeros, and rebuild reproduces them for free because
 *   it writes every range at its absolute image offset into a file it
 *   ftruncate()s -- the gaps become holes, and holes read as zeros. The
 *   proof is a MEASUREMENT, not an assumption: a free block holding bit-rot
 *   is not zero, so it stays in the range table (measured: 8 MiB of random
 *   bytes written into a 128 MiB image's free space moved that much coverage
 *   from the elided side to the stored side, and the recipe grew from 60 652
 *   B to 8 449 276 B). E4RCP001 stored every non-member byte verbatim, which
 *   is why the core's size guard saw "fixed = the whole partition" and
 *   declined every decomposition at every size tried.
 *
 *   WHAT THAT COSTS: because the gaps are not written down, image_size can no
 *   longer be PROVEN from the recipe body (E4RCP001's ranges+extents tiled
 *   [0, image_size) exactly; v2 only requires them to be disjoint, in bounds
 *   and ascending). A corrupted image_size that is merely TOO LARGE therefore
 *   rebuilds without complaint -- measured, 'ZZZZ' at header offset 8 gave
 *   exit 0 and a 1.4 GiB sparse file from a 64 MiB recipe. The wrong-size
 *   output is caught one layer up: pack_container_rebuild() requires the
 *   rebuilt file to be exactly the record's length (src/core/vol_cpack.c).
 *
 *   The ranges and the per-member extents still cover everything that is
 *   NOT elided: superblock, GDT, block/inode bitmaps, inode tables, directory
 *   blocks, extent-tree index nodes, journal, reserved-GDT blocks, unwritten
 *   extents, beyond-i_size tails and every non-zero unused block -- verbatim,
 *   so bit-rot is preserved. The per-member extent map says where each
 *   member's logical bytes live in the image: idx -> (logical_off,
 *   image_off, len) list. rebuild truncates the output to image_size,
 *   writes the ranges verbatim, then splices member bytes into their
 *   extents in order; a member file whose size != usize fails with
 *   exit 1. strip+map agree on data_base, so MRMP RECIPE src_off values
 *   point into the recipe blob's data section.
 *
 *   rebuild accepts E4RCP002 only. An E4RCP001 recipe is refused BY NAME
 *   (it is a real generation, not damage) with a message that names the way
 *   out: re-import the container from its source image.
 *
 * SUPPORTED (parse-proven; anything else declines, exit 3)
 *
 *   block sizes 1024/2048/4096; ext4 extent-mapped files (depth 0..5
 *   extent trees); htree directories (indirect_levels 0/1 — index blocks
 *   are metadata, recipe-verbatim, never parsed as dirents); 32- and
 *   64-bit group descriptors (64bit feature); flex_bg layout.
 *
 *   Feature policy (read-only bit-exact walk; checksums are never
 *   verified and nothing is ever written to the image):
 *     INCOMPAT allow : filetype extents 64bit flex_bg csum_seed
 *                      orphan_file
 *     RO_COMPAT allow: sparse_super large_file huge_file dir_nlink
 *                      extra_isize metadata_csum
 *     COMPAT         : any (read-compatible by definition; has_journal,
 *                      ext_attr, resize_inode, dir_index are the usual)
 *     REFUSE (exit 3): encrypt casefold verity meta_bg bigalloc
 *                      inline_data compression journal_dev recover(dirty
 *                      journal) mmp ea_inode dirdata largedir, unknown
 *                      INCOMPAT/RO_COMPAT bits, missing EXTENTS (ext2/3
 *                      block maps), un-clean fs state, 8K+ blocks, any
 *                      structural corruption the parse cannot prove
 *                      (bad extent trees, dirent chains, dx roots, shared
 *                      data blocks, member content overlapping metadata,
 *                      > ABI_MAX_MEMBERS members or an inode number >
 *                      ABI_MAX_IDX), and
 *                      images with no regular files (zero members).
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The plugin C ABI (ADR-007). Same pair of includes as fatfs/ntfs: the pack is
 * built twice from this one source -- once as the CLI (bin/ext4fs) and once as
 * libext4fs.so with -DIVPACK_SHARED_LIB -- so the CLI and the plugin can never
 * drift apart. */
#if __has_include("ivpack_impl.h")
#include "ivpack_impl.h"
#elif __has_include("../../../src/include/ivpack_impl.h")
#include "../../../src/include/ivpack_impl.h"
#endif
#include <sys/types.h>
#include <unistd.h>

#define EX_DECLINE 3        /* honest refusal: unsupported / unprovable */
#define EX_ERROR   1        /* content error (rebuild mismatch, junk)   */
#define EX_USAGE   2        /* argv error                              */
#define EX_IO      4        /* I/O or allocation failure               */

#define COPY_WINDOW (8u << 20)      /* stream members in <= 8 MiB chunks */

/* ---- ext4 on-disk constants ---- */
#define E4_SB_OFF        1024u
#define E4_SB_SIZE       1024u
#define E4_MAGIC         0xEF53u

#define E4_STATE_VALID   0x0001u
#define E4_STATE_ERROR   0x0002u

#define E4C_HAS_JOURNAL  0x0004u   /* COMPAT: informational only */
#define E4I_COMPRESSION  0x0001u
#define E4I_FILETYPE     0x0002u
#define E4I_RECOVER      0x0004u
#define E4I_JOURNAL_DEV  0x0008u
#define E4I_META_BG      0x0010u
#define E4I_EXTENTS      0x0040u
#define E4I_64BIT        0x0080u
#define E4I_MMP          0x0100u
#define E4I_FLEX_BG      0x0200u
#define E4I_EA_INODE     0x0400u
#define E4I_ORPHAN_FILE  0x1000u   /* (was DIRDATA; modern orphan file) */
#define E4I_CSUM_SEED    0x2000u
#define E4I_LARGEDIR     0x4000u
#define E4I_INLINE_DATA  0x8000u
#define E4I_ENCRYPT      0x10000u
#define E4I_CASEFOLD     0x20000u
#define E4R_SPARSE_SUPER 0x0001u
#define E4R_LARGE_FILE   0x0002u
#define E4R_BTREE_DIR    0x0004u
#define E4R_HUGE_FILE    0x0008u
#define E4R_UNINIT_BG    0x0010u
#define E4R_DIR_NLINK    0x0020u
#define E4R_EXTRA_ISIZE  0x0040u
#define E4R_HAS_SNAPSHOT 0x0080u
#define E4R_QUOTA        0x0100u
#define E4R_BIGALLOC     0x0200u
#define E4R_METADATA_CSUM 0x0400u
#define E4R_REPLICA      0x0800u
#define E4R_READONLY     0x1000u
#define E4R_PROJECT      0x2000u
#define E4R_SHARED_BLOCKS 0x4000u
#define E4R_VERITY       0x8000u

#define E4I_ALLOW (E4I_FILETYPE | E4I_EXTENTS | E4I_64BIT | E4I_FLEX_BG | \
                   E4I_CSUM_SEED | E4I_ORPHAN_FILE)
#define E4R_ALLOW (E4R_SPARSE_SUPER | E4R_LARGE_FILE | E4R_HUGE_FILE | \
                   E4R_DIR_NLINK | E4R_EXTRA_ISIZE | E4R_METADATA_CSUM)

#define E4_FL_INDEX      0x00001000u   /* htree-indexed directory */
#define E4_FL_EXTENTS    0x00080000u
#define E4_FL_INLINE_DATA 0x10000000u

#define E4_EXT_MAGIC     0xF30Au
#define E4_EXT_MAX_DEPTH 5u

#define E4_S_IFMT        0xF000u
#define E4_S_IFREG       0x8000u
#define E4_S_IFDIR       0x4000u

#define ABI_MAX_MEMBERS  1048576u
#define ABI_MAX_IDX      1048575u
#define ABI_MAP_MAX_ENTS (4u * ABI_MAX_MEMBERS + 4u)

/* `classify` is a CLI-only manifest command: the plugin C ABI carries five
 * (enumerate/extract/strip/rebuild/map). Under -DIVPACK_SHARED_LIB nothing
 * references it, so -Werror=unused-function would reject the .so. */
#if defined(__GNUC__)
#define E4_MAYBE_UNUSED __attribute__((unused))
#else
#define E4_MAYBE_UNUSED
#endif

#define RECIPE_MAGIC "E4RCP002"
#define RECIPE_HDR   32u
#define RECIPE_MENT  24u
#define RECIPE_EENT  32u
#define RECIPE_RENT  16u

#define ESTIMATE_MARGIN (64ull << 20)   /* estimate = sum(usize) + 64 MiB */

/* ---- little-endian readers ---- */
static uint16_t le16(const uint8_t *p)
{ return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v)
{ put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static int g_verbose;   /* EXT4FS_DEBUG=1: reasons also on decline paths */

static void note(const char *what, uint64_t a, uint64_t b)
{
    if (g_verbose)
        fprintf(stderr, "ext4fs: %s (%" PRIu64 ", %" PRIu64 ")\n", what, a, b);
}

/* ---- positional I/O helpers (streaming, never whole-file) ---- */
static int read_at(int fd, uint64_t off, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len) {
        ssize_t r = pread(fd, p, len, (off_t)off);
        if (r <= 0) return -1;
        p += (size_t)r;
        off += (uint64_t)r;
        len -= (size_t)r;
    }
    return 0;
}

static int write_at(int fd, uint64_t off, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len) {
        ssize_t w = pwrite(fd, p, len, (off_t)off);
        if (w <= 0) return -1;
        p += (size_t)w;
        off += (uint64_t)w;
        len -= (size_t)w;
    }
    return 0;
}

/* copy [in_off, +len) of infd to [out_off, +len) of outfd, <=8 MiB windows */
static int copy_range(int infd, uint64_t in_off, int outfd,
                      uint64_t out_off, uint64_t len)
{
    static uint8_t *win;
    if (!win) {
        win = (uint8_t *)malloc(COPY_WINDOW);
        if (!win) return -1;
    }
    while (len) {
        size_t n = len > COPY_WINDOW ? COPY_WINDOW : (size_t)len;
        if (read_at(infd, in_off, win, n) != 0) return -1;
        if (write_at(outfd, out_off, win, n) != 0) return -1;
        in_off += n;
        out_off += n;
        len -= n;
    }
    return 0;
}

/* ---- interval set (metadata proof) ---- */
typedef struct { uint64_t lo, hi; } ivl_t;

typedef struct {
    ivl_t  *v;
    size_t  n, cap;
} ivls_t;

static int ivl_add(ivls_t *s, uint64_t lo, uint64_t hi)
{
    if (hi <= lo) return 0;
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 128;
        ivl_t *nv = (ivl_t *)realloc(s->v, nc * sizeof *nv);
        if (!nv) return -1;
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n].lo = lo;
    s->v[s->n].hi = hi;
    s->n++;
    return 0;
}

static int ivl_cmp(const void *a, const void *b)
{
    uint64_t x = ((const ivl_t *)a)->lo, y = ((const ivl_t *)b)->lo;
    return x < y ? -1 : x > y;
}

static void ivl_sort_merge(ivls_t *s)
{
    size_t i, w = 0;
    qsort(s->v, s->n, sizeof *s->v, ivl_cmp);
    for (i = 0; i < s->n; i++) {
        if (w && s->v[i].lo <= s->v[w - 1].hi) {
            if (s->v[i].hi > s->v[w - 1].hi)
                s->v[w - 1].hi = s->v[i].hi;
        } else {
            s->v[w++] = s->v[i];
        }
    }
    s->n = w;
}

/* 1 if [lo,hi) intersects the (sorted, merged) set */
static int ivl_hits(const ivls_t *s, uint64_t lo, uint64_t hi)
{
    size_t a = 0, b = s->n;
    while (a < b) {                     /* first ivl with hi > lo */
        size_t m = (a + b) / 2;
        if (s->v[m].hi <= lo) a = m + 1; else b = m;
    }
    return a < s->n && s->v[a].lo < hi;
}

/* ---- parsed filesystem ---- */
typedef struct {
    uint64_t logical, image, len;   /* one member content run (bytes) */
} mext_t;

typedef struct {
    uint32_t  ino;
    uint64_t  usize;
    char      sname[25];
    mext_t   *ext;
    size_t    n_ext, cap_ext;
} member_t;

typedef struct {
    uint64_t image, len, logical;
    uint32_t idx;
} grange_t;                          /* member content, image-sorted view */

typedef struct {
    int      fd;
    uint64_t file_size;              /* actual image file size */
    uint32_t block_size;
    uint64_t blocks_count;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_count;
    uint32_t inodes_per_group;
    uint32_t first_ino;
    uint32_t inode_size;
    uint32_t desc_size;
    uint32_t ngroups;
    uint32_t feat_compat, feat_incompat, feat_ro;
    uint64_t fs_bytes;               /* blocks_count * block_size */
    uint64_t gdt_off;                /* byte offset of the GDT */

    uint8_t *ibmp;                   /* ngroups * block_size inode bitmaps */

    member_t *mem;
    size_t    n_mem, cap_mem;

    grange_t *gr;                    /* global member content ranges */
    size_t    n_gr;

    ivls_t    meta;                  /* parsed-metadata interval proof */

    char    (*names)[25];            /* inodes_count+1 sanitized basenames */
    uint8_t  *visited;               /* dir-walk visited bitmap */
} fs_t;

typedef struct {
    fs_t     *fs;
    int       is_dir;
    uint64_t  prev_lend;             /* previous logical end (blocks) */
    int       have_prev;
    member_t *m;                     /* REG: accumulate content runs */
    uint64_t *dblocks;               /* DIR: physical blocks, logical order */
    size_t    n_dblocks, cap_dblocks;
} walk_t;

/* ---- small vector push helpers ---- */
static int mext_push(member_t *m, uint64_t logical, uint64_t image,
                     uint64_t len)
{
    mext_t *e;
    if (m->n_ext && m->ext[m->n_ext - 1].logical +
        m->ext[m->n_ext - 1].len == logical &&
        m->ext[m->n_ext - 1].image + m->ext[m->n_ext - 1].len == image) {
        m->ext[m->n_ext - 1].len += len;   /* contiguous run: merge */
        return 0;
    }
    if (m->n_ext == m->cap_ext) {
        size_t nc = m->cap_ext ? m->cap_ext * 2 : 16;
        mext_t *ne = (mext_t *)realloc(m->ext, nc * sizeof *ne);
        if (!ne) return -1;
        m->ext = ne;
        m->cap_ext = nc;
    }
    e = &m->ext[m->n_ext++];
    e->logical = logical;
    e->image = image;
    e->len = len;
    return 0;
}

static int dblk_push(walk_t *w, uint64_t phys)
{
    if (w->n_dblocks == w->cap_dblocks) {
        size_t nc = w->cap_dblocks ? w->cap_dblocks * 2 : 32;
        uint64_t *nv = (uint64_t *)realloc(w->dblocks, nc * sizeof *nv);
        if (!nv) return -1;
        w->dblocks = nv;
        w->cap_dblocks = nc;
    }
    w->dblocks[w->n_dblocks++] = phys;
    return 0;
}

/* ---- superblock + GDT parse (validate everything we rely on) ---- */
static int parse_super(fs_t *fs)
{
    uint8_t sb[E4_SB_SIZE];
    uint32_t log_bs, rev;

    if (read_at(fs->fd, E4_SB_OFF, sb, sizeof sb) != 0) {
        note("superblock read", 0, 0);
        return EX_DECLINE;
    }
    if (le16(sb + 0x38) != E4_MAGIC) {
        note("bad magic", le16(sb + 0x38), 0);
        return EX_DECLINE;
    }
    fs->inodes_count     = le32(sb + 0x00);
    fs->blocks_count     = le32(sb + 0x04);
    fs->first_data_block = le32(sb + 0x14);
    log_bs               = le32(sb + 0x18);
    fs->blocks_per_group = le32(sb + 0x20);
    fs->inodes_per_group = le32(sb + 0x28);
    rev                  = le32(sb + 0x4C);
    fs->first_ino        = le32(sb + 0x54);
    fs->inode_size       = le16(sb + 0x58);
    fs->feat_compat      = le32(sb + 0x5C);
    fs->feat_incompat    = le32(sb + 0x60);
    fs->feat_ro          = le32(sb + 0x64);

    if (log_bs > 2) {          /* 1024/2048/4096 proven; 8K+ untestable */
        note("block size not supported", 1024u << log_bs, 0);
        return EX_DECLINE;
    }
    fs->block_size = 1024u << log_bs;

    if (!(le16(sb + 0x3A) & E4_STATE_VALID) ||
        (le16(sb + 0x3A) & E4_STATE_ERROR)) {
        note("fs state not clean", le16(sb + 0x3A), 0);
        return EX_DECLINE;     /* structures may be mid-write */
    }
    if (fs->feat_incompat & ~E4I_ALLOW) {
        note("unknown INCOMPAT feature", fs->feat_incompat & ~E4I_ALLOW, 0);
        return EX_DECLINE;
    }
    if (fs->feat_ro & ~E4R_ALLOW) {
        note("unknown RO_COMPAT feature", fs->feat_ro & ~E4R_ALLOW, 0);
        return EX_DECLINE;
    }
    if (!(fs->feat_incompat & E4I_EXTENTS)) {
        note("extents feature absent (ext2/3 block maps)", 0, 0);
        return EX_DECLINE;
    }
    if (rev == 0)
        fs->inode_size = 128;
    if (fs->first_ino == 0)
        fs->first_ino = 11;
    if (fs->inode_size < 128 || fs->inode_size > fs->block_size ||
        (fs->inode_size & (fs->inode_size - 1)) != 0) {
        note("inode size", fs->inode_size, 0);
        return EX_DECLINE;
    }
    if (fs->feat_incompat & E4I_64BIT) {
        fs->blocks_count |= (uint64_t)le32(sb + 0x150) << 32;
        fs->desc_size = le16(sb + 0xFE);
        if (fs->desc_size < 32 || fs->desc_size > 1024 ||
            fs->desc_size > fs->block_size ||
            (fs->desc_size & (fs->desc_size - 1)) != 0) {
            note("descriptor size", fs->desc_size, 0);
            return EX_DECLINE;
        }
    } else {
        fs->desc_size = 32;
    }
    if (!fs->blocks_per_group || !fs->inodes_per_group ||
        !fs->inodes_count || fs->first_ino > fs->inodes_count ||
        fs->first_data_block >= fs->blocks_count ||
        fs->inodes_per_group > fs->block_size * 8 ||
        ((uint64_t)fs->inodes_per_group * fs->inode_size) %
            fs->block_size != 0) {
        note("geometry", fs->blocks_per_group, fs->inodes_per_group);
        return EX_DECLINE;
    }
    fs->fs_bytes = fs->blocks_count * fs->block_size;
    if (fs->fs_bytes > fs->file_size) {
        note("fs larger than image file", fs->fs_bytes, fs->file_size);
        return EX_DECLINE;
    }
    fs->ngroups = (uint32_t)((fs->blocks_count - fs->first_data_block +
                              fs->blocks_per_group - 1) /
                             fs->blocks_per_group);
    if (!fs->ngroups ||
        (uint64_t)(fs->inodes_count + fs->inodes_per_group - 1) /
            fs->inodes_per_group > fs->ngroups) {
        note("group count", fs->ngroups, 0);
        return EX_DECLINE;
    }
    fs->gdt_off = ((uint64_t)fs->first_data_block + 1) * fs->block_size;
    if (fs->gdt_off + (uint64_t)fs->ngroups * fs->desc_size > fs->fs_bytes) {
        note("GDT out of range", fs->gdt_off, 0);
        return EX_DECLINE;
    }
    /* [0, GDT end): boot block + superblock + GDT are parsed metadata */
    if (ivl_add(&fs->meta, 0, fs->gdt_off +
                (uint64_t)fs->ngroups * fs->desc_size) != 0)
        return EX_IO;
    return 0;
}

static int parse_gdt(fs_t *fs)
{
    uint8_t *gdt;
    uint64_t gsz = (uint64_t)fs->ngroups * fs->desc_size;
    uint32_t g;

    gdt = (uint8_t *)malloc(gsz ? gsz : 1);
    if (!gdt) return EX_IO;
    if (read_at(fs->fd, fs->gdt_off, gdt, (size_t)gsz) != 0) {
        free(gdt);
        note("GDT read", 0, 0);
        return EX_DECLINE;
    }
    fs->ibmp = (uint8_t *)malloc((size_t)fs->ngroups * fs->block_size);
    if (!fs->ibmp) { free(gdt); return EX_IO; }
    for (g = 0; g < fs->ngroups; g++) {
        const uint8_t *d = gdt + (size_t)g * fs->desc_size;
        uint64_t bb = le32(d + 0x00), ib = le32(d + 0x04), it = le32(d + 0x08);
        uint64_t itsz;
        if (fs->feat_incompat & E4I_64BIT) {
            bb |= (uint64_t)le32(d + 0x20) << 32;
            ib |= (uint64_t)le32(d + 0x24) << 32;
            it |= (uint64_t)le32(d + 0x28) << 32;
        }
        itsz = (uint64_t)fs->inodes_per_group * fs->inode_size;
        if (bb >= fs->blocks_count || ib >= fs->blocks_count ||
            it >= fs->blocks_count ||
            it * fs->block_size + itsz > fs->fs_bytes) {
            free(gdt);
            note("group geometry", g, 0);
            return EX_DECLINE;
        }
        /* bitmaps + inode tables are parsed metadata */
        if (ivl_add(&fs->meta, bb * fs->block_size,
                    bb * fs->block_size + fs->block_size) != 0 ||
            ivl_add(&fs->meta, ib * fs->block_size,
                    ib * fs->block_size + fs->block_size) != 0 ||
            ivl_add(&fs->meta, it * fs->block_size,
                    it * fs->block_size + itsz) != 0) {
            free(gdt);
            return EX_IO;
        }
        if (read_at(fs->fd, ib * fs->block_size,
                    fs->ibmp + (size_t)g * fs->block_size,
                    fs->block_size) != 0) {
            free(gdt);
            note("inode bitmap read", g, 0);
            return EX_DECLINE;
        }
    }
    free(gdt);
    return 0;
}

static int inode_used(const fs_t *fs, uint32_t ino)
{
    uint32_t g, j;
    if (ino == 0 || ino > fs->inodes_count) return 0;
    g = (ino - 1) / fs->inodes_per_group;
    j = (ino - 1) % fs->inodes_per_group;
    return (fs->ibmp[(size_t)g * fs->block_size + j / 8] >> (j % 8)) & 1;
}

static int read_inode(fs_t *fs, uint32_t ino, uint8_t *buf)
{
    uint64_t it, off;
    uint32_t g, j;
    uint8_t dbuf[64];
    g = (ino - 1) / fs->inodes_per_group;
    j = (ino - 1) % fs->inodes_per_group;
    if (read_at(fs->fd, fs->gdt_off + (uint64_t)g * fs->desc_size,
                dbuf, fs->desc_size) != 0)
        return -1;
    it = le32(dbuf + 0x08);
    if (fs->feat_incompat & E4I_64BIT)
        it |= (uint64_t)le32(dbuf + 0x28) << 32;
    off = it * fs->block_size + (uint64_t)j * fs->inode_size;
    return read_at(fs->fd, off, buf, fs->inode_size);
}

/* ---- extent tree walk (depth 0..5); leaves feed the walk_t sink ---- */
static int walk_extents(fs_t *fs, walk_t *w, const uint8_t *node,
                        int in_inode, unsigned depth)
{
    uint16_t magic, entries, max;
    uint32_t i;

    magic   = le16(node + 0);
    entries = le16(node + 2);
    max     = le16(node + 4);
    if (magic != E4_EXT_MAGIC) {
        note("extent magic", magic, 0);
        return EX_DECLINE;
    }
    if (depth > E4_EXT_MAX_DEPTH) {
        note("extent depth", depth, 0);
        return EX_DECLINE;
    }
    {
        unsigned limit = in_inode ? 4u : (fs->block_size - 12u) / 12u;
        if (max > limit || entries > max) {
            note("extent counts", entries, max);
            return EX_DECLINE;
        }
    }
    if (le16(node + 6) != depth) {
        note("extent depth mismatch", le16(node + 6), depth);
        return EX_DECLINE;
    }
    if (depth == 0) {
        for (i = 0; i < entries; i++) {
            const uint8_t *e = node + 12 + (size_t)i * 12;
            uint32_t lb = le32(e + 0);
            uint16_t elen = le16(e + 4);
            uint64_t phys = le32(e + 8) | ((uint64_t)le16(e + 6) << 32);
            int unwritten = elen > 32768;
            uint32_t nblk = unwritten ? (elen & 0x7FFF) : elen;

            if (nblk == 0 || phys + nblk > fs->blocks_count) {
                note("extent range", phys, nblk);
                return EX_DECLINE;
            }
            if (w->have_prev && lb < w->prev_lend) {
                note("extent overlap", lb, w->prev_lend);
                return EX_DECLINE;   /* overlapping logical ranges */
            }
            if (w->is_dir && (unwritten ||
                              (w->have_prev && lb != w->prev_lend) ||
                              (!w->have_prev && lb != 0))) {
                note("directory extent gap/unwritten", lb, 0);
                return EX_DECLINE;
            }
            w->have_prev = 1;
            w->prev_lend = (uint64_t)lb + nblk;
            if (w->is_dir) {
                uint32_t k;
                for (k = 0; k < nblk; k++)
                    if (dblk_push(w, phys + k) != 0)
                        return EX_IO;
            } else if (!unwritten && w->m) {
                /* member content = written bytes inside [0, usize) */
                uint64_t lo = (uint64_t)lb * fs->block_size;
                uint64_t hi = lo + (uint64_t)nblk * fs->block_size;
                if (lo < w->m->usize) {
                    uint64_t chi = hi < w->m->usize ? hi : w->m->usize;
                    if (mext_push(w->m, lo, phys * fs->block_size,
                                  chi - lo) != 0)
                        return EX_IO;
                }
                /* tail past i_size (and every unwritten block) stays
                 * recipe: it is image bytes, not member content */
            }
        }
        return 0;
    }
    /* interior node: idx entries point at child nodes (metadata) */
    {
        uint32_t prev_ei = 0;
        for (i = 0; i < entries; i++) {
            const uint8_t *e = node + 12 + (size_t)i * 12;
            uint32_t eib = le32(e + 0);
            uint64_t leaf = le32(e + 4) | ((uint64_t)le16(e + 8) << 32);
            uint8_t *child;
            int rc;

            if (i && eib <= prev_ei) {
                note("idx ordering", eib, 0);
                return EX_DECLINE;
            }
            prev_ei = eib;
            if (leaf >= fs->blocks_count) {
                note("idx leaf block", leaf, 0);
                return EX_DECLINE;
            }
            if (ivl_add(&fs->meta, leaf * fs->block_size,
                        leaf * fs->block_size + fs->block_size) != 0)
                return EX_IO;
            child = (uint8_t *)malloc(fs->block_size);
            if (!child) return EX_IO;
            if (read_at(fs->fd, leaf * fs->block_size, child,
                        fs->block_size) != 0) {
                free(child);
                note("extent node read", leaf, 0);
                return EX_DECLINE;
            }
            rc = walk_extents(fs, w, child, 0, depth - 1);
            free(child);
            if (rc) return rc;
        }
    }
    return 0;
}

static int walk_inode_extents(fs_t *fs, walk_t *w, const uint8_t *inode)
{
    if (!(le32(inode + 0x20) & E4_FL_EXTENTS)) {
        note("inode without extents", 0, 0);
        return EX_DECLINE;   /* ext2/3-style block map: not provable */
    }
    w->have_prev = 0;
    w->prev_lend = 0;
    return walk_extents(fs, w, inode + 0x28, 1, le16(inode + 0x28 + 6));
}

/* ---- name table (sanitized basenames; first dirent wins) ---- */
static void name_record(fs_t *fs, uint32_t ino, const uint8_t *name,
                        size_t nlen)
{
    size_t i, w = 0;
    char *dst;
    if (ino > fs->inodes_count) return;
    dst = fs->names[ino];
    if (dst[0]) return;                 /* first path wins (hardlinks) */
    for (i = 0; i < nlen && w < 24; i++) {
        unsigned ch = name[i];
        int ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                 (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                 ch == '-';
        dst[w++] = (char)(ok ? ch : '_');
    }
    dst[w] = 0;
}

/* ---- directory walk (names only; every block is recipe metadata) ---- */
static int walk_dir(fs_t *fs, uint32_t ino, unsigned depth);

/* parse one leaf block linearly, recursing into child directories */
static int dir_leaf(fs_t *fs, const uint8_t *blk, unsigned depth,
                    uint8_t *visited)
{
    uint32_t off = 0;
    int v2 = (fs->feat_incompat & E4I_FILETYPE) != 0;

    while (off < fs->block_size) {
        const uint8_t *de = blk + off;
        uint32_t cino = le32(de + 0);
        uint16_t rlen = le16(de + 4);
        uint32_t nlen = v2 ? de[6] : le16(de + 6);

        if (rlen < 8 || (rlen & 3) || off + rlen > fs->block_size ||
            nlen > (uint32_t)rlen - 8u) {
            note("dirent chain", off, rlen);
            return EX_DECLINE;
        }
        if (cino) {
            uint8_t ib[4096];           /* inode_size <= block_size <= 4K */
            uint16_t mode;
            size_t k;

            if (cino > fs->inodes_count || !inode_used(fs, cino)) {
                note("dirent inode unused/out of range", cino, 0);
                return EX_DECLINE;
            }
            if (!nlen) {
                note("zero-length name with live inode", cino, 0);
                return EX_DECLINE;
            }
            for (k = 0; k < nlen; k++)
                if (de[8 + k] == 0 || de[8 + k] == '/') {
                    note("bad name bytes", cino, 0);
                    return EX_DECLINE;
                }
            if (!(nlen == 1 && de[8] == '.') &&
                !(nlen == 2 && de[8] == '.' && de[9] == '.')) {
                name_record(fs, cino, de + 8, nlen);
                if (read_inode(fs, cino, ib) != 0) {
                    note("child inode read", cino, 0);
                    return EX_DECLINE;
                }
                mode = le16(ib + 0) & E4_S_IFMT;
                if (mode == E4_S_IFDIR && cino <= fs->inodes_count &&
                    !((visited[cino / 8] >> (cino % 8)) & 1)) {
                    int rc;
                    visited[cino / 8] |= (uint8_t)(1u << (cino % 8));
                    rc = walk_dir(fs, cino, depth + 1);
                    if (rc) return rc;
                }
            }
        }
        off += rlen;
    }
    return off == fs->block_size ? 0 : EX_DECLINE;
}

/* parse the htree dx_root (block 0 of an indexed dir): the two dot
 * dirents, then the index. Returns the set of interior index blocks
 * (indirect_levels=1) as a bitmap over the dir's logical blocks. */
static int dir_index_root(fs_t *fs, const uint8_t *blk, size_t nblocks,
                          uint8_t **nodes_out, uint8_t *visited,
                          unsigned depth)
{
    int v2 = (fs->feat_incompat & E4I_FILETYPE) != 0;
    uint32_t nlen0, nlen1;
    uint16_t count, limit;
    uint8_t indirect;
    uint8_t *nodes;
    uint32_t i;

    *nodes_out = NULL;
    /* dot entries: "." occupies the first 12 bytes; ".." follows, with a
     * rec_len that may cover the rest of the block (the dx_root_info sits
     * at the fixed offset 24 either way) */
    nlen0 = v2 ? blk[6] : le16(blk + 6);
    if (le32(blk) == 0 || le16(blk + 4) != 12 || nlen0 != 1 || blk[8] != '.') {
        note("dx root dot", 0, 0);
        return EX_DECLINE;
    }
    nlen1 = v2 ? blk[12 + 6] : le16(blk + 12 + 6);
    if (le32(blk + 12) == 0 || le16(blk + 16) < 12 || (le16(blk + 16) & 3) ||
        12 + (uint32_t)le16(blk + 16) > fs->block_size || nlen1 != 2 ||
        blk[20] != '.' || blk[21] != '.') {
        note("dx root dotdot", 0, 0);
        return EX_DECLINE;
    }
    /* dx_root_info @24: reserved u32, hash_version u8, info_length u8,
     * indirect_levels u8, unused_flags u8 */
    if (blk[24 + 4 + 1] != 8) {
        note("dx info_length", blk[29], 0);
        return EX_DECLINE;
    }
    indirect = blk[24 + 4 + 2];
    if (indirect > 1) {
        note("dx indirect_levels", indirect, 0);
        return EX_DECLINE;
    }
    /* dx_countlimit overlaps dx_entry[0]: limit@32, count@34, and the
     * i-th entry's block number is the u32 at 36 + 8*i (the countlimit's
     * block member IS entry 0's block) */
    limit = le16(blk + 32);
    count = le16(blk + 34);
    if (!count || count > limit || 32u + (uint32_t)limit * 8u >
        fs->block_size) {
        note("dx countlimit", count, limit);
        return EX_DECLINE;
    }
    nodes = (uint8_t *)calloc((nblocks + 7) / 8, 1);
    if (!nodes) return EX_IO;
    for (i = 0; i < count; i++) {
        uint32_t b = le32(blk + 36 + (size_t)i * 8);
        if (b == 0 || b >= nblocks) {
            note("dx entry block", b, nblocks);
            free(nodes);
            return EX_DECLINE;
        }
        if (indirect == 1)
            nodes[b / 8] |= (uint8_t)(1u << (b % 8));
    }
    (void)visited; (void)depth;
    *nodes_out = nodes;
    return 0;
}

static int walk_dir(fs_t *fs, uint32_t ino, unsigned depth)
{
    uint8_t *inode, *blk;
    walk_t w;
    int rc = EX_DECLINE;
    size_t i;
    uint8_t *nodes = NULL;

    if (depth > 64) {
        note("dir depth", depth, 0);
        return EX_DECLINE;
    }
    inode = (uint8_t *)malloc(fs->inode_size);
    blk = (uint8_t *)malloc(fs->block_size);
    if (!inode || !blk) { rc = EX_IO; goto out; }
    if (read_inode(fs, ino, inode) != 0 ||
        (le16(inode + 0) & E4_S_IFMT) != E4_S_IFDIR) {
        note("walk_dir on non-directory", ino, 0);
        goto out;
    }
    memset(&w, 0, sizeof w);
    w.fs = fs;
    w.is_dir = 1;
    rc = walk_inode_extents(fs, &w, inode);
    if (rc) goto out;
    if (w.n_dblocks == 0) {             /* empty directory: fine */
        rc = 0;
        goto out;
    }
    /* every directory data block is parsed metadata */
    for (i = 0; i < w.n_dblocks; i++)
        if (ivl_add(&fs->meta, w.dblocks[i] * fs->block_size,
                    w.dblocks[i] * fs->block_size + fs->block_size) != 0) {
            rc = EX_IO;
            goto out;
        }
    if (le32(inode + 0x20) & E4_FL_INDEX) {
        if (w.n_dblocks < 2) {
            note("indexed dir with < 2 blocks", ino, 0);
            rc = EX_DECLINE;
            goto out;
        }
        if (read_at(fs->fd, w.dblocks[0] * fs->block_size, blk,
                    fs->block_size) != 0) {
            rc = EX_DECLINE;
            goto out;
        }
        rc = dir_index_root(fs, blk, w.n_dblocks, &nodes, fs->visited,
                            depth);
        if (rc) goto out;
        /* validate interior index blocks (metadata, never parsed) */
        for (i = 1; nodes && i < w.n_dblocks; i++) {
            uint16_t ncount, nlimit;
            uint32_t k;
            if (!((nodes[i / 8] >> (i % 8)) & 1)) continue;
            if (read_at(fs->fd, w.dblocks[i] * fs->block_size, blk,
                        fs->block_size) != 0) {
                rc = EX_DECLINE;
                goto out;
            }
            /* fake dirent: inode 0, empty name, rec_len covering 8..bs */
            if (le32(blk) != 0 || le16(blk + 4) < 8 || (le16(blk + 4) & 3) ||
                le16(blk + 4) > fs->block_size || blk[6] != 0) {
                note("dx node fake dirent", i, 0);
                rc = EX_DECLINE;
                goto out;
            }
            nlimit = le16(blk + 8);
            ncount = le16(blk + 10);
            if (!ncount || ncount > nlimit ||
                8u + (uint32_t)nlimit * 8u > fs->block_size) {
                note("dx node countlimit", ncount, nlimit);
                rc = EX_DECLINE;
                goto out;
            }
            for (k = 0; k < ncount; k++) {
                uint32_t b = le32(blk + 12 + (size_t)k * 8);
                if (b == 0 || b >= w.n_dblocks ||
                    ((nodes[b / 8] >> (b % 8)) & 1)) {
                    note("dx node leaf ref", b, 0);
                    rc = EX_DECLINE;
                    goto out;
                }
            }
        }
    }
    for (i = 0; i < w.n_dblocks; i++) {
        if (i == 0 && nodes) continue;   /* dx root: dot entries only */
        if (nodes && ((nodes[i / 8] >> (i % 8)) & 1)) continue;
        if (read_at(fs->fd, w.dblocks[i] * fs->block_size, blk,
                    fs->block_size) != 0) {
            rc = EX_DECLINE;
            goto out;
        }
        rc = dir_leaf(fs, blk, depth, fs->visited);
        if (rc) goto out;
    }
    rc = 0;
out:
    free(nodes);
    free(w.dblocks);
    free(inode);
    free(blk);
    return rc;
}

/* ---- member scan ---- */
static int member_push(fs_t *fs, uint32_t ino, uint64_t usize)
{
    member_t *m;
    if (fs->n_mem == fs->cap_mem) {
        size_t nc = fs->cap_mem ? fs->cap_mem * 2 : 64;
        member_t *nm = (member_t *)realloc(fs->mem, nc * sizeof *nm);
        if (!nm) return -1;
        fs->mem = nm;
        fs->cap_mem = nc;
    }
    m = &fs->mem[fs->n_mem++];
    memset(m, 0, sizeof *m);
    m->ino = ino;
    m->usize = usize;
    return 0;
}

static int scan_inodes(fs_t *fs)
{
    uint32_t ino;
    uint8_t *ib;
    int rc;

    ib = (uint8_t *)malloc(fs->inode_size);
    if (!ib) return EX_IO;

    /* root first (names), then every remaining used directory */
    fs->visited = (uint8_t *)calloc(fs->inodes_count / 8 + 1, 1);
    if (!fs->visited) { free(ib); return EX_IO; }
    if (!inode_used(fs, 2)) {
        note("root inode unused", 0, 0);
        free(ib);
        return EX_DECLINE;
    }
    fs->visited[2 / 8] |= (uint8_t)(1u << (2 % 8));
    rc = walk_dir(fs, 2, 0);
    if (rc) { free(ib); return rc; }

    for (ino = fs->first_ino; ino <= fs->inodes_count; ino++) {
        uint16_t mode;
        if (!inode_used(fs, ino)) continue;
        if (read_inode(fs, ino, ib) != 0) {
            note("inode read", ino, 0);
            free(ib);
            return EX_DECLINE;
        }
        mode = le16(ib + 0) & E4_S_IFMT;
        if (mode == E4_S_IFDIR) {
            if (!((fs->visited[ino / 8] >> (ino % 8)) & 1)) {
                fs->visited[ino / 8] |= (uint8_t)(1u << (ino % 8));
                rc = walk_dir(fs, ino, 0);
                if (rc) { free(ib); return rc; }
            }
        } else if (mode == E4_S_IFREG) {
            walk_t w;
            uint64_t usize;
            if (le32(ib + 0x20) & E4_FL_INLINE_DATA) {
                note("inline-data regular file", ino, 0);
                free(ib);
                return EX_DECLINE;
            }
            usize = le32(ib + 0x04) | ((uint64_t)le32(ib + 0x6C) << 32);
            if (ino > ABI_MAX_IDX || fs->n_mem >= ABI_MAX_MEMBERS) {
                note("member beyond ABI range", ino, fs->n_mem);
                free(ib);
                return EX_DECLINE;
            }
            if (member_push(fs, ino, usize) != 0) {
                free(ib);
                return EX_IO;
            }
            memset(&w, 0, sizeof w);
            w.fs = fs;
            w.is_dir = 0;
            w.m = &fs->mem[fs->n_mem - 1];
            rc = walk_inode_extents(fs, &w, ib);
            if (rc) { free(ib); return rc; }
        }
        /* symlinks / devices / fifos / sockets: metadata, recipe */
    }
    free(ib);
    return 0;
}

static int mem_cmp(const void *a, const void *b)
{
    uint32_t x = ((const member_t *)a)->ino, y = ((const member_t *)b)->ino;
    return x < y ? -1 : x > y;
}

static int gr_cmp(const void *a, const void *b)
{
    uint64_t x = ((const grange_t *)a)->image, y = ((const grange_t *)b)->image;
    return x < y ? -1 : x > y;
}

/* finish: names, member ordering, global ranges, overlap proof, recipe
 * ranges (returned via out params as a freshly allocated array) */
typedef struct { uint64_t off, len; } rrange_t;

static int finish_parse(fs_t *fs, rrange_t **ranges_out, size_t *n_ranges_out)
{
    size_t i, j, n_gr = 0;
    grange_t *gr;
    rrange_t *rr = NULL;
    size_t n_rr = 0, cap_rr = 0;
    uint64_t cur;

    for (i = 0; i < fs->n_mem; i++) {
        member_t *m = &fs->mem[i];
        if (fs->names[m->ino][0])
            memcpy(m->sname, fs->names[m->ino], sizeof m->sname);
        else
            snprintf(m->sname, sizeof m->sname, "ino%u", m->ino);
        n_gr += m->n_ext;
    }
    qsort(fs->mem, fs->n_mem, sizeof *fs->mem, mem_cmp);

    gr = (grange_t *)malloc((n_gr ? n_gr : 1) * sizeof *gr);
    if (!gr) return EX_IO;
    n_gr = 0;
    for (i = 0; i < fs->n_mem; i++) {
        member_t *m = &fs->mem[i];
        for (j = 0; j < m->n_ext; j++) {
            gr[n_gr].image = m->ext[j].image;
            gr[n_gr].len = m->ext[j].len;
            gr[n_gr].logical = m->ext[j].logical;
            gr[n_gr].idx = m->ino;
            n_gr++;
        }
    }
    qsort(gr, n_gr, sizeof *gr, gr_cmp);

    /* shared blocks: two members claiming the same image bytes */
    for (i = 1; i < n_gr; i++)
        if (gr[i].image < gr[i - 1].image + gr[i - 1].len) {
            note("shared data blocks", gr[i].image, 0);
            free(gr);
            return EX_DECLINE;
        }

    /* member content must never intersect parsed metadata */
    ivl_sort_merge(&fs->meta);
    for (i = 0; i < n_gr; i++)
        if (ivl_hits(&fs->meta, gr[i].image, gr[i].image + gr[i].len)) {
            note("member content overlaps metadata", gr[i].image, 0);
            free(gr);
            return EX_DECLINE;
        }

    /* recipe ranges = the complement of member content in [0, file_size) */
    cur = 0;
    for (i = 0; i <= n_gr; i++) {
        uint64_t end = i < n_gr ? gr[i].image : fs->file_size;
        if (end > cur) {
            if (n_rr == cap_rr) {
                size_t nc = cap_rr ? cap_rr * 2 : 256;
                rrange_t *nr = (rrange_t *)realloc(rr, nc * sizeof *nr);
                if (!nr) { free(rr); free(gr); return EX_IO; }
                rr = nr;
                cap_rr = nc;
            }
            rr[n_rr].off = cur;
            rr[n_rr].len = end - cur;
            n_rr++;
        }
        if (i < n_gr)
            cur = gr[i].image + gr[i].len;
    }
    fs->gr = gr;
    fs->n_gr = n_gr;
    *ranges_out = rr;
    *n_ranges_out = n_rr;
    return 0;
}

static void fs_free(fs_t *fs)
{
    size_t i;
    if (!fs) return;
    if (fs->fd >= 0) close(fs->fd);
    for (i = 0; i < fs->n_mem; i++)
        free(fs->mem[i].ext);
    free(fs->mem);
    free(fs->gr);
    free(fs->ibmp);
    free(fs->meta.v);
    free(fs->names);
    free(fs->visited);
    free(fs);
}

/* full parse: superblock -> GDT -> bitmaps -> inode scan -> dir walk ->
 * member content ranges -> overlap proof -> recipe ranges */
static int parse_all(const char *path, fs_t **fs_out, rrange_t **rr_out,
                     size_t *n_rr_out)
{
    fs_t *fs;
    struct stat st;
    int rc;

    *fs_out = NULL;
    *rr_out = NULL;
    *n_rr_out = 0;
    fs = (fs_t *)calloc(1, sizeof *fs);
    if (!fs) return EX_IO;
    fs->fd = -1;
    fs->fd = open(path, O_RDONLY);
    if (fs->fd < 0) { fs_free(fs); return EX_IO; }
    if (fstat(fs->fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size < E4_SB_OFF + E4_SB_SIZE) {
        fs_free(fs);
        note("not an ext4 image (size)", 0, 0);
        return EX_DECLINE;
    }
    fs->file_size = (uint64_t)st.st_size;
    rc = parse_super(fs);
    if (rc) goto out;
    rc = parse_gdt(fs);
    if (rc) goto out;
    fs->names = (char (*)[25])calloc((size_t)fs->inodes_count + 1,
                                     sizeof *fs->names);
    if (!fs->names) { rc = EX_IO; goto out; }
    rc = scan_inodes(fs);
    if (rc) goto out;
    if (fs->n_mem == 0) {
        note("no regular files (zero members)", 0, 0);
        rc = EX_DECLINE;
        goto out;
    }
    rc = finish_parse(fs, rr_out, n_rr_out);
out:
    if (rc) {
        fs_free(fs);
        *rr_out = NULL;
        *n_rr_out = 0;
        return rc;
    }
    *fs_out = fs;
    return 0;
}

/* ---- recipe header math shared by strip and map ---- */
static uint64_t recipe_data_base(size_t n_mem, size_t n_ext, size_t n_rr)
{
    return RECIPE_HDR + RECIPE_MENT * (uint64_t)n_mem +
           RECIPE_EENT * (uint64_t)n_ext + RECIPE_RENT * (uint64_t)n_rr;
}

static int write_recipe_header(int ofd, const fs_t *fs, const rrange_t *rr,
                               size_t n_rr, uint64_t image_size)
{
    uint8_t hdr[RECIPE_HDR];
    size_t i, j;
    uint64_t off = RECIPE_HDR;

    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, RECIPE_MAGIC, 8);
    put64(hdr + 8, image_size);
    put32(hdr + 16, fs->block_size);
    put32(hdr + 20, (uint32_t)fs->n_mem);
    put32(hdr + 24, (uint32_t)n_rr);
    put32(hdr + 28, (uint32_t)fs->n_gr);
    if (write_at(ofd, 0, hdr, sizeof hdr) != 0) return -1;
    for (i = 0; i < fs->n_mem; i++) {
        uint8_t e[RECIPE_MENT];
        memset(e, 0, sizeof e);
        put32(e + 0, fs->mem[i].ino);
        put32(e + 4, (uint32_t)fs->mem[i].n_ext);
        put64(e + 8, fs->mem[i].usize);
        if (write_at(ofd, off, e, sizeof e) != 0) return -1;
        off += sizeof e;
    }
    for (i = 0; i < fs->n_mem; i++) {
        const member_t *m = &fs->mem[i];
        for (j = 0; j < m->n_ext; j++) {
            uint8_t e[RECIPE_EENT];
            memset(e, 0, sizeof e);
            put32(e + 0, m->ino);
            put64(e + 8, m->ext[j].logical);
            put64(e + 16, m->ext[j].image);
            put64(e + 24, m->ext[j].len);
            if (write_at(ofd, off, e, sizeof e) != 0) return -1;
            off += sizeof e;
        }
    }
    for (i = 0; i < n_rr; i++) {
        uint8_t e[RECIPE_RENT];
        put64(e + 0, rr[i].off);
        put64(e + 8, rr[i].len);
        if (write_at(ofd, off, e, sizeof e) != 0) return -1;
        off += sizeof e;
    }
    return 0;
}

/* ---- commands ---- */
static int cmd_enumerate(const char *in, const char *out)
{
    fs_t *fs;
    rrange_t *rr;
    size_t n_rr, i;
    FILE *o;
    int rc = parse_all(in, &fs, &rr, &n_rr);
    if (rc) return rc;
    o = fopen(out, "w");
    if (!o) { rc = EX_IO; goto out; }
    for (i = 0; i < fs->n_mem; i++)
        fprintf(o, "%u\t%s\t%" PRIu64 "\n", fs->mem[i].ino, fs->mem[i].sname,
                fs->mem[i].usize);
    if (fclose(o) != 0) rc = EX_IO;
out:
    free(rr);
    fs_free(fs);
    return rc;
}

static int cmd_extract(const char *in, const char *idx_s, const char *out)
{
    fs_t *fs;
    rrange_t *rr;
    size_t n_rr, i;
    char *endp = NULL;
    unsigned long idx;
    member_t *m = NULL;
    int ofd, rc = parse_all(in, &fs, &rr, &n_rr);

    if (rc) return rc;
    idx = strtoul(idx_s, &endp, 10);
    if (!endp || *endp || idx > ABI_MAX_IDX) { rc = EX_USAGE; goto out; }
    for (i = 0; i < fs->n_mem; i++)
        if (fs->mem[i].ino == idx) { m = &fs->mem[i]; break; }
    if (!m) {
        note("extract: no such member", idx, 0);
        rc = EX_ERROR;
        goto out;
    }
    ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (ofd < 0) { rc = EX_IO; goto out; }
    for (i = 0; i < m->n_ext; i++)
        if (copy_range(fs->fd, m->ext[i].image, ofd, m->ext[i].logical,
                       m->ext[i].len) != 0) {
            close(ofd);
            rc = EX_IO;
            goto out;
        }
    if (ftruncate(ofd, (off_t)m->usize) != 0) rc = EX_IO;
    if (close(ofd) != 0) rc = EX_IO;
out:
    free(rr);
    fs_free(fs);
    return rc;
}

/* ---- classify: what every non-member byte range IS ----------------------
 *
 * ABI v1, additive: `ext4fs classify <image> <out>`. It answers the question
 * the core cannot answer for itself, because the core never parses this pack's
 * recipe: of the bytes that ride verbatim, which are STRUCTURE (superblock,
 * GDT, bitmaps, inode tables, directory blocks, journal -- a rebuild needs
 * them) and which are GARBAGE (free blocks and unwritten extents -- not needed
 * by any member file, needed only to reproduce the image bit-for-bit)?
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
 *                bit1 = reserved (compressible, when sampling lands)
 *
 * `zero_verified` is a MEASUREMENT, not an assumption from "the block is
 * unallocated": a free block may hold bit-rot, and this pack's whole point is
 * preserving it.
 */
#define GCFG_MAGIC "GCFG"
#define GCFG_VER   1
#define GCFG_ENT   24
#define GCFG_CLS_META 0
#define GCFG_CLS_GARB 1
#define GCFG_CLS_MEMBER 2
#define GCFG_F_ZERO 0x01

typedef struct { uint64_t off, len; uint8_t cls, flags; } gcfg_ent;

static int gcfg_cmp(const void *a, const void *b)
{
    const gcfg_ent *x = (const gcfg_ent *)a, *y = (const gcfg_ent *)b;
    return x->off < y->off ? -1 : x->off > y->off;
}

static int gcfg_push(gcfg_ent **v, size_t *n, size_t *cap,
                     uint64_t off, uint64_t len, int cls, int flags)
{
    if (!len)
        return 0;
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        gcfg_ent *nv = (gcfg_ent *)realloc(*v, nc * sizeof *nv);
        if (!nv) return -1;
        *v = nv; *cap = nc;
    }
    (*v)[*n].off = off; (*v)[*n].len = len;
    (*v)[*n].cls = (uint8_t)cls; (*v)[*n].flags = (uint8_t)flags;
    (*n)++;
    return 0;
}

/* Split [lo,hi) into block-granular runs of all-zero / not-all-zero. Whole
 * blocks only: a partial tail block can never be elided, so it is reported as
 * not-zero. Returns 0 on success, -1 on read error. */
static int gcfg_split_zero(int fd, uint64_t lo, uint64_t hi, uint64_t bs,
                           gcfg_ent **v, size_t *n, size_t *cap)
{
    uint8_t *buf = (uint8_t *)malloc((size_t)bs);
    uint64_t b, run_start;
    int run_zero = 0;
    if (!buf) return -1;
    run_start = lo;
    for (b = lo; b < hi; b += bs) {
        size_t got = (size_t)((hi - b) < bs ? (hi - b) : bs);
        int z = 0;
        if (got == bs) {
            size_t k;
            if (read_at(fd, b, buf, bs) != 0) { free(buf); return -1; }
            z = 1;
            for (k = 0; k < bs; k++)
                if (buf[k]) { z = 0; break; }
        }
        if (b == lo) {
            run_zero = z;
            run_start = b;
            continue;
        }
        if (z != run_zero) {
            if (gcfg_push(v, n, cap, run_start, b - run_start,
                          GCFG_CLS_GARB, run_zero ? GCFG_F_ZERO : 0) != 0) {
                free(buf); return -1;
            }
            run_zero = z;
            run_start = b;
        }
    }
    if (run_start < hi &&
        gcfg_push(v, n, cap, run_start, hi - run_start,
                  GCFG_CLS_GARB, run_zero ? GCFG_F_ZERO : 0) != 0) {
        free(buf); return -1;
    }
    free(buf);
    return 0;
}

/* Build the GCFG table for an already-parsed image. Shared by `classify` and
 * by `strip` (which writes it beside the recipe, so the core gets the
 * classification for free at decompose time instead of re-reading the image
 * later, when it is no longer a file at all). */
static int build_gcfg(fs_t *fs, rrange_t *rr, size_t n_rr,
                      gcfg_ent **tab_out, size_t *n_out)
{
    size_t i, j, n = 0, cap = 0;
    gcfg_ent *tab = NULL;

    for (i = 0; i < n_rr; i++) {
        uint64_t cur = rr[i].off, end = rr[i].off + rr[i].len;
        for (j = 0; j < fs->meta.n && cur < end; j++) {
            uint64_t mlo = fs->meta.v[j].lo, mhi = fs->meta.v[j].hi;
            if (mhi <= cur) continue;
            if (mlo >= end) break;
            if (mlo > cur &&
                gcfg_split_zero(fs->fd, cur, mlo, fs->block_size,
                                &tab, &n, &cap) != 0) goto fail;
            if (gcfg_push(&tab, &n, &cap, mlo < cur ? cur : mlo,
                          (mhi > end ? end : mhi) - (mlo < cur ? cur : mlo),
                          GCFG_CLS_META, 0) != 0) goto fail;
            cur = mhi > end ? end : mhi;
        }
        if (cur < end &&
            gcfg_split_zero(fs->fd, cur, end, fs->block_size,
                            &tab, &n, &cap) != 0) goto fail;
    }
    for (i = 0; i < fs->n_gr; i++)
        if (gcfg_push(&tab, &n, &cap, fs->gr[i].image, fs->gr[i].len,
                      GCFG_CLS_MEMBER, 0) != 0) goto fail;
    qsort(tab, n, sizeof *tab, gcfg_cmp);
    *tab_out = tab; *n_out = n;
    return 0;
fail:
    free(tab);
    return -1;
}

static int write_gcfg(const char *path, const gcfg_ent *tab, size_t n,
                      uint64_t block_size)
{
    uint8_t hdr[16];
    uint64_t off = 16;
    int ofd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600), i;

    if (ofd < 0) return EX_IO;
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, GCFG_MAGIC, 4);
    put32(hdr + 4, GCFG_VER);
    put32(hdr + 8, (uint32_t)n);
    put32(hdr + 12, (uint32_t)block_size);
    if (write_at(ofd, 0, hdr, sizeof hdr) != 0) { close(ofd); return EX_IO; }
    for (i = 0; i < (int)n; i++) {
        uint8_t e[GCFG_ENT];
        memset(e, 0, sizeof e);
        put64(e + 0, tab[i].off);
        put64(e + 8, tab[i].len);
        e[16] = tab[i].cls;
        e[17] = tab[i].flags;
        if (write_at(ofd, off, e, sizeof e) != 0) { close(ofd); return EX_IO; }
        off += sizeof e;
    }
    if (close(ofd) != 0) return EX_IO;
    return 0;
}

static int cmd_strip(const char *in, const char *out)
{
    fs_t *fs;
    rrange_t *rr;
    size_t n_rr, i;
    uint64_t woff;
    int ofd, rc = parse_all(in, &fs, &rr, &n_rr);

    if (rc) return rc;
    ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (ofd < 0) { rc = EX_IO; goto out; }
    /* E4RCP002: store the NON-ZERO runs only. Everything a range covered and
     * this filter drops is an all-zero block the rebuild reproduces for free
     * as a hole -- measured on a 128 MiB ext4 with one member: 119.3 of 119.5
     * MiB of the garbage was verified zero, and verify-by-reading is what
     * makes dropping it safe (a free block with bit-rot is not zero, so it
     * stays). E4RCP001 stored all of it verbatim, which is why the core's size
     * guard saw "fixed = the whole partition" and declined every decomposition
     * at every size tried. */
    {
        rrange_t *nz = NULL;
        size_t n_nz = 0, cap_nz = 0;
        for (i = 0; i < n_rr; i++) {
            gcfg_ent *tab = NULL;
            size_t n = 0, cap_tab = 0, k;   /* cap_tab: the split's OWN capacity.
                                             * Sharing nz's made it write into a
                                             * NULL tab -- segfault, caught by the
                                             * round-trip test, not by the build. */
            if (gcfg_split_zero(fs->fd, rr[i].off, rr[i].off + rr[i].len,
                                fs->block_size, &tab, &n, &cap_tab) != 0) {
                free(tab); free(nz); close(ofd); rc = EX_IO; goto out;
            }
            for (k = 0; k < n; k++) {
                if (tab[k].flags & GCFG_F_ZERO)
                    continue;                  /* a hole reproduces it */
                if (n_nz == cap_nz) {
                    size_t nc = cap_nz ? cap_nz * 2 : 256;
                    rrange_t *nn = (rrange_t *)realloc(nz, nc * sizeof *nn);
                    if (!nn) { free(tab); free(nz); close(ofd); rc = EX_IO; goto out; }
                    nz = nn; cap_nz = nc;
                }
                /* NO coalescing, deliberately: `map` builds its recipe entries
                 * from the same split, and both sides must agree on the range
                 * COUNT, because that count is what recipe_data_base() adds to
                 * find where the bytes start. Merging here made the map's
                 * src_off point 16 bytes per merged pair past the blob, and the
                 * core's guard caught it as "memcmp failed" -- silent-looking,
                 * but it was a real disagreement between two writers. */
                nz[n_nz].off = tab[k].off;
                nz[n_nz].len = tab[k].len;
                n_nz++;
            }
            free(tab);
        }
        if (write_recipe_header(ofd, fs, nz, n_nz, fs->file_size) != 0) {
            free(nz); close(ofd); rc = EX_IO; goto out;
        }
        woff = recipe_data_base(fs->n_mem, fs->n_gr, n_nz);
        for (i = 0; i < n_nz; i++) {
            if (copy_range(fs->fd, nz[i].off, ofd, woff, nz[i].len) != 0) {
                free(nz); close(ofd); rc = EX_IO; goto out;
            }
            woff += nz[i].len;
        }
        free(nz);
    }
    if (close(ofd) != 0) rc = EX_IO;
    /* The classification rides beside the recipe: `<out>.cls`. Written HERE
     * because this is the one moment the image still exists as a file and is
     * already open -- after decomposition the core has a recipe and members,
     * and asking the pack to classify then would mean materialising the image
     * again. Best-effort: a pack that cannot classify must not fail a strip
     * that already succeeded, so the return value is reported and ignored.
     * EXT4FS_CLS=0 turns the pass off for a caller that wants to skip the
     * extra sequential read of the non-member bytes. */
    {
        const char *off_env = getenv("EXT4FS_CLS");
        if (!(off_env && off_env[0] == '0' && off_env[1] == 0)) {
            gcfg_ent *tab = NULL;
            size_t n = 0;
            char clspath[4096];
            if (build_gcfg(fs, rr, n_rr, &tab, &n) == 0 && tab) {
                int wrote = 0;
                if (snprintf(clspath, sizeof clspath, "%s.cls", out) > 0 &&
                    write_gcfg(clspath, tab, n, fs->block_size) == 0)
                    wrote = 1;
                if (!wrote)
                    fprintf(stderr, "ext4fs: strip: could not write %s.cls\n",
                            out);
                free(tab);
            }
        }
    }
out:
    free(rr);
    fs_free(fs);
    return rc;
}

/* One MRMP segment. kind: 0 = recipe bytes (non-zero run), 1 = member
 * payload, 3 = ZEROS -- the image bytes here are all zero and the recipe
 * stores nothing for them.
 *
 * Kind 3 is the whole point of E4RCP002 and it is NOT ext4-specific: any pack
 * that can prove a region of its container is zeros (free blocks, a sparse
 * tail, an unformatted span) declares it here and the core stores nothing and
 * serves zeros. The proof is the pack's obligation: a region may be declared
 * zero only after READING it, because a free block may hold bit-rot. */
typedef struct { uint64_t off, len, src_off, logical; uint32_t idx; uint8_t kind; } map_seg;

static int map_seg_cmp(const void *a, const void *b)
{
    const map_seg *x = (const map_seg *)a, *y = (const map_seg *)b;
    return x->off < y->off ? -1 : x->off > y->off;
}

static int cmd_map(const char *in, const char *out)
{
    fs_t *fs;
    rrange_t *rr;
    size_t n_rr, i, k, n_seg = 0, cap_seg = 0;
    uint64_t dbase, rsrc, pos;
    map_seg *seg = NULL;
    uint8_t *blob;
    size_t blen, wp;
    int ofd, rc = parse_all(in, &fs, &rr, &n_rr);

    if (rc) return rc;
    /* Every byte of the image is one segment: non-zero run, member payload, or
     * zeros. Sorted, they must tile -- that is what the core's validator
     * checks, and it is the property that makes the map answer for every byte
     * exactly once.
     *
     * kind 3 (zeros) is emitted here, and it is what makes the decomposition
     * PAY: on a 128 MiB image with two small members the recipe falls from
     * the whole partition to 60 652 B, and the core stores nothing for the
     * 127 MiB that are classified zero. The read path through such a map is
     * proven, not assumed -- the core's sweep-time guard reads EVERY entry
     * back through the real read path (recipe segment + member siblings) and
     * memcmps it against the original image, and tools/test-ext4fs.sh takes
     * `cmp` of the whole container back (fsA 4K blocks, fsB 1K blocks, 2K/
     * htree/multi-extent/orphan-only fixtures, pack-absent reads through the
     * same map) plus three consecutive sweeps without drift. The map used to
     * REFUSE when a recipe had elided zeros, because the core could not serve
     * kind 3 yet; that gate is gone with the reader in place. */
#define SEG_PUSH(o, l, kd)                                                   \
    do {                                                                     \
        if (n_seg == cap_seg) {                                              \
            size_t nc = cap_seg ? cap_seg * 2 : 256;                         \
            map_seg *nv = (map_seg *)realloc(seg, nc * sizeof *nv);           \
            if (!nv) { free(seg); rc = EX_IO; goto out; }                     \
            seg = nv; cap_seg = nc;                                          \
        }                                                                    \
        seg[n_seg].off = (o); seg[n_seg].len = (l);                          \
        seg[n_seg].kind = (uint8_t)(kd); seg[n_seg].idx = 0;                 \
        seg[n_seg].src_off = 0; seg[n_seg].logical = 0; n_seg++;                                     \
    } while (0)

    for (i = 0; i < n_rr; i++) {
        gcfg_ent *tab = NULL;
        size_t n = 0, cap_tab = 0;
        if (gcfg_split_zero(fs->fd, rr[i].off, rr[i].off + rr[i].len,
                            fs->block_size, &tab, &n, &cap_tab) != 0) {
            free(tab); free(seg); rc = EX_IO; goto out;
        }
        for (k = 0; k < n; k++)
            SEG_PUSH(tab[k].off, tab[k].len,
                     (tab[k].flags & GCFG_F_ZERO) ? 3 : 0);
        free(tab);
    }
    for (i = 0; i < fs->n_gr; i++) {
        SEG_PUSH(fs->gr[i].image, fs->gr[i].len, 1);
        seg[n_seg - 1].idx = fs->gr[i].idx;
        /* A member is stored ONCE and named by several extents, so a member
         * entry must carry WHICH part of the member it serves: the logical
         * offset, which MRMP keeps in the src_off field. Dropping it (every
         * extent claiming logical 0) made the core compare the second
         * extent's image bytes against the member's FIRST bytes -- that was
         * the guard's "memcmp failed". */
        seg[n_seg - 1].logical = fs->gr[i].logical;
    }
    if (n_seg == 0 || n_seg > ABI_MAP_MAX_ENTS) {
        note("map entry count", n_seg, 0);
        free(seg);
        rc = EX_DECLINE;
        goto out;
    }
    qsort(seg, n_seg, sizeof *seg, map_seg_cmp);

    blen = 8 + n_seg * 29;
    blob = (uint8_t *)malloc(blen);
    if (!blob) { free(seg); rc = EX_IO; goto out; }
    memcpy(blob, "MRMP", 4);
    put32(blob + 4, (uint32_t)n_seg);
    wp = 8;
    /* The recipe blob is written by cmd_strip with the FILTERED range count
     * (only non-zero runs), so the map's src_off base must use the same count
     * -- not n_rr, which is the pre-filter complement of the members. Getting
     * this wrong is not a crash: it makes every recipe entry point past the
     * blob, and the core refuses the map. */
    {
        size_t n_kind0 = 0;
        for (i = 0; i < n_seg; i++)
            if (seg[i].kind == 0) n_kind0++;
        dbase = recipe_data_base(fs->n_mem, fs->n_gr, n_kind0);
    }
    rsrc = dbase;
    pos = 0;
    for (i = 0; i < n_seg; i++) {
        if (seg[i].off != pos) {          /* internal: must tile exactly */
            free(blob); free(seg);
            rc = EX_ERROR;
            goto out;
        }
        put64(blob + wp, seg[i].off);
        put64(blob + wp + 8, seg[i].len);
        blob[wp + 16] = seg[i].kind;
        put32(blob + wp + 17, seg[i].kind == 1 ? seg[i].idx : 0);
        put64(blob + wp + 21, seg[i].kind == 0 ? rsrc
                             : (seg[i].kind == 1 ? seg[i].logical : 0));
        if (seg[i].kind == 0) rsrc += seg[i].len;
        wp += 29;
        pos = seg[i].off + seg[i].len;
    }
    free(seg);
    ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (ofd < 0) { free(blob); rc = EX_IO; goto out; }
    if (write_at(ofd, 0, blob, blen) != 0 || close(ofd) != 0) {
        free(blob);
        rc = EX_IO;
        goto out;
    }
    free(blob);
out:
    free(rr);
    fs_free(fs);
    return rc;
}

static int cmd_estimate(const char *in)
{
    fs_t *fs;
    rrange_t *rr;
    size_t n_rr, i;
    uint64_t sum = 0;
    int rc = parse_all(in, &fs, &rr, &n_rr);

    if (rc) return rc;
    for (i = 0; i < fs->n_mem; i++)
        sum += fs->mem[i].usize;

    /* Classification summary on STDERR, so stdout stays the ONE number the
     * core parses: a second line there would be read as part of the first by
     * any consumer that does not expect it. A human gets the split, a parser
     * gets what it always got.
     *
     * This is the "will it fit, and where would the win come from" answer for
     * one container: meta_structure must be stored verbatim, garbage is the
     * part an operator could trade away, and member content is what dedupe and
     * the codec lane act on. */
    {
        uint64_t meta = 0, garb = 0, mem = 0;
        size_t j;
        for (i = 0; i < n_rr; i++) {
            uint64_t cur = rr[i].off, end = rr[i].off + rr[i].len;
            for (j = 0; j < fs->meta.n && cur < end; j++) {
                uint64_t mlo = fs->meta.v[j].lo, mhi = fs->meta.v[j].hi;
                if (mhi <= cur) continue;
                if (mlo >= end) break;
                if (mlo > cur) garb += mlo - cur;
                meta += (mhi > end ? end : mhi) - (mlo < cur ? cur : mlo);
                cur = mhi > end ? end : mhi;
            }
            if (cur < end) garb += end - cur;
        }
        for (i = 0; i < fs->n_gr; i++) mem += fs->gr[i].len;
        fprintf(stderr, "cls: meta %" PRIu64 " garbage %" PRIu64
                        " member %" PRIu64 " image %" PRIu64 "\n",
                meta, garb, mem, fs->file_size);
    }
    printf("%" PRIu64 "\n", (uint64_t)(sum + ESTIMATE_MARGIN));
    free(rr);
    fs_free(fs);
    return 0;
}



static E4_MAYBE_UNUSED int cmd_classify(const char *in, const char *out)
{
    fs_t *fs;
    rrange_t *rr;
    size_t n_rr, n = 0;
    gcfg_ent *tab = NULL;
    int rc = parse_all(in, &fs, &rr, &n_rr);

    if (rc) return rc;
    if (build_gcfg(fs, rr, n_rr, &tab, &n) != 0) { rc = EX_IO; goto done; }
    rc = write_gcfg(out, tab, n, fs->block_size);
done:
    free(tab);
    free(rr);
    fs_free(fs);
    return rc;

}

/* ---- rebuild: the recipe alone reproduces the original ---- */
typedef struct {
    uint32_t idx;
    uint32_t n_ext;
    uint64_t usize;
} rmem_t;

typedef struct {
    uint64_t logical, image, len;
} rext_t;

static int rext_img_cmp(const void *pa, const void *pb)
{
    uint64_t u = ((const rext_t *)pa)->image;
    uint64_t v = ((const rext_t *)pb)->image;
    return u < v ? -1 : u > v;
}

static int cmd_rebuild(const char *recipe, const char *dir, const char *out)
{
    int rfd = -1, ofd = -1, rc = EX_ERROR;
    uint8_t hdr[RECIPE_HDR];
    uint64_t image_size, dbase, expect_size, woff;
    uint32_t n_mem, n_rr, n_ext, i;
    rmem_t *mem = NULL;
    rext_t *ext = NULL;
    rrange_t *rr = NULL;

    rfd = open(recipe, O_RDONLY);
    if (rfd < 0) return EX_IO;
    {
        struct stat st;
        if (fstat(rfd, &st) != 0) { rc = EX_IO; goto out; }
        expect_size = (uint64_t)st.st_size;
    }
    if (read_at(rfd, 0, hdr, sizeof hdr) != 0) {
        note("rebuild: cannot read the recipe header", 0, 0);
        goto out;
    }
    if (memcmp(hdr, RECIPE_MAGIC, 8) != 0) {
        /* E4RCP001 stored every non-member byte verbatim; E4RCP002 stores only
         * the non-zero runs and lets the gaps be holes. The bytes still tile
         * the image, so a v1 recipe is not damage -- it is an older
         * generation, and this pack deliberately carries no second read path
         * for it (single-operator fleet, no volumes in the field; recorded in
         * docs/EXT4FS-ZERO-ELISION.md). The way back is the source image the
         * container was made from. */
        if (memcmp(hdr, "E4RCP001", 8) == 0)
            fprintf(stderr, "ext4fs: rebuild: this recipe is E4RCP001 (every "
                            "non-member byte stored verbatim) and this pack "
                            "writes and reads E4RCP002 only. Re-import the "
                            "container from its source image.\n");
        else
            note("rebuild: unknown recipe magic", 0, 0);
        goto out;
    }
    memcpy(&image_size, hdr + 8, 8);
    {
        uint32_t bs;
        memcpy(&bs, hdr + 16, 4);
        if (bs != 1024 && bs != 2048 && bs != 4096) {
            note("rebuild: bad block size", bs, 0);
            goto out;
        }
    }
    memcpy(&n_mem, hdr + 20, 4);
    memcpy(&n_rr, hdr + 24, 4);
    memcpy(&n_ext, hdr + 28, 4);
    if (!n_mem || n_mem > ABI_MAX_MEMBERS) {
        note("rebuild: member count", n_mem, 0);
        goto out;
    }
    dbase = recipe_data_base(n_mem, n_ext, n_rr);
    if (dbase > expect_size) {
        note("rebuild: truncated recipe tables", 0, 0);
        goto out;
    }
    mem = (rmem_t *)malloc((size_t)n_mem * sizeof *mem);
    ext = (rext_t *)malloc((size_t)(n_ext ? n_ext : 1) * sizeof *ext);
    rr = (rrange_t *)malloc((size_t)(n_rr ? n_rr : 1) * sizeof *rr);
    if (!mem || !ext || !rr) { rc = EX_IO; goto out; }
    {
        uint8_t buf[RECIPE_EENT];
        uint64_t off = RECIPE_HDR, prev_idx = 0;
        for (i = 0; i < n_mem; i++) {
            if (read_at(rfd, off, buf, RECIPE_MENT) != 0) goto out;
            memcpy(&mem[i].idx, buf + 0, 4);
            memcpy(&mem[i].n_ext, buf + 4, 4);
            memcpy(&mem[i].usize, buf + 8, 8);
            if (i && mem[i].idx <= prev_idx) goto out;   /* idx ascending */
            prev_idx = mem[i].idx;
            off += RECIPE_MENT;
        }
        {
            uint64_t tot = 0, k;
            for (k = 0; k < n_mem; k++) tot += mem[k].n_ext;
            if (tot != n_ext) {
                note("rebuild: extent count mismatch", tot, n_ext);
                goto out;
            }
        }
        {
            uint32_t mi = 0, left = mem[0].n_ext;
            uint64_t prev_log = 0;
            for (i = 0; i < n_ext; i++) {
                uint32_t x;
                if (read_at(rfd, off, buf, RECIPE_EENT) != 0) goto out;
                memcpy(&x, buf + 0, 4);
                while (left == 0) {         /* next member's group */
                    prev_log = 0;
                    if (++mi >= n_mem) goto out;
                    left = mem[mi].n_ext;
                }
                if (x != mem[mi].idx) goto out;   /* grouping matches table */
                memcpy(&ext[i].logical, buf + 8, 8);
                memcpy(&ext[i].image, buf + 16, 8);
                memcpy(&ext[i].len, buf + 24, 8);
                if (!ext[i].len ||
                    ext[i].logical < prev_log ||
                    ext[i].logical + ext[i].len > mem[mi].usize ||
                    ext[i].image + ext[i].len > image_size) {
                    note("rebuild: bad extent", i, 0);
                    goto out;
                }
                prev_log = ext[i].logical + ext[i].len;
                left--;
                off += RECIPE_EENT;
            }
        }
        {
            uint64_t prev_end = 0, tot = 0;
            for (i = 0; i < n_rr; i++) {
                if (read_at(rfd, off, buf, RECIPE_RENT) != 0) goto out;
                memcpy(&rr[i].off, buf + 0, 8);
                memcpy(&rr[i].len, buf + 8, 8);
                if (!rr[i].len || (i && rr[i].off < prev_end) ||
                    rr[i].off + rr[i].len > image_size) {
                    note("rebuild: bad range", i, 0);
                    goto out;
                }
                prev_end = rr[i].off + rr[i].len;
                tot += rr[i].len;
                off += RECIPE_RENT;
            }
            if (dbase + tot != expect_size) {
                note("rebuild: recipe size mismatch", dbase + tot,
                     expect_size);
                goto out;
            }
        }
    }
    /* partition proof: ranges and member extents must be in bounds, ascending
     * and DISJOINT -- they no longer have to TILE the image.
     *
     * In E4RCP001 they did tile it: every non-member byte was stored verbatim,
     * so a gap would have been a hole where the original had bytes. E4RCP002
     * stores only the NON-ZERO runs, so a gap is exactly what an all-zero
     * stretch of the image leaves behind -- and a hole in the output reads as
     * zeros, which is what those bytes were. Requiring tile would fail every
     * v2 recipe; requiring disjointness is the property that still has to
     * hold, because overlap would mean two sources for one image byte. */
    {
        rext_t *xs = (rext_t *)malloc((size_t)(n_ext ? n_ext : 1) *
                                      sizeof *xs);
        uint64_t p = 0;
        uint32_t ri = 0, xi = 0;
        if (!xs) { rc = EX_IO; goto out; }
        memcpy(xs, ext, (size_t)n_ext * sizeof *xs);
        qsort(xs, n_ext, sizeof *xs, rext_img_cmp);
        while (ri < n_rr || xi < n_ext) {
            if (ri < n_rr && (xi >= n_ext || rr[ri].off <= xs[xi].image)) {
                if (rr[ri].off < p || rr[ri].off + rr[ri].len > image_size) {
                    free(xs); goto out;          /* overlap or past the end */
                }
                p = rr[ri].off + rr[ri].len;
                ri++;
            } else {
                if (xs[xi].image < p || xs[xi].image + xs[xi].len > image_size) {
                    free(xs); goto out;          /* overlap or past the end */
                }
                p = xs[xi].image + xs[xi].len;
                xi++;
            }
        }
        free(xs);
    }
    /* write: recipe ranges verbatim, member bytes into their extents */
    ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (!ofd) { rc = EX_IO; goto out; }
    woff = dbase;
    for (i = 0; i < n_rr; i++) {
        if (copy_range(rfd, woff, ofd, rr[i].off, rr[i].len) != 0) {
            rc = EX_IO;
            goto out;
        }
        woff += rr[i].len;
    }
    for (i = 0; i < n_mem; i++) {
        char path[4096];
        int mfd;
        struct stat st;
        uint32_t j, base = 0, k;
        int n;

        n = snprintf(path, sizeof path, "%s/%u", dir, mem[i].idx);
        if (n <= 0 || (size_t)n >= sizeof path) goto out;
        mfd = open(path, O_RDONLY);
        if (mfd < 0) {
            note("rebuild: missing member", mem[i].idx, 0);
            goto out;
        }
        if (fstat(mfd, &st) != 0 || (uint64_t)st.st_size != mem[i].usize) {
            note("rebuild: usize mismatch", mem[i].idx,
                 (uint64_t)st.st_size);
            close(mfd);
            goto out;                    /* the house contract: exit 1 */
        }
        for (k = 0; k < i; k++) base += mem[k].n_ext;
        for (j = 0; j < mem[i].n_ext; j++) {
            const rext_t *e = &ext[base + j];
            if (copy_range(mfd, e->logical, ofd, e->image, e->len) != 0) {
                close(mfd);
                rc = EX_IO;
                goto out;
            }
        }
        close(mfd);
    }
    if (ftruncate(ofd, (off_t)image_size) != 0) { rc = EX_IO; goto out; }
    if (close(ofd) != 0) { ofd = -1; rc = EX_IO; goto out; }
    ofd = -1;
    rc = 0;
out:
    if (rfd >= 0) close(rfd);
    if (ofd >= 0) close(ofd);
    free(mem);
    free(ext);
    free(rr);
    return rc;
}

/* ---- ivpack plugin C ABI export (ADR-007) --------------------------------
 * The five cmd_* below already take the canonical signatures, so the whole
 * glue is one descriptor plus three macros; ivpack_impl.h fork-guards each call
 * so the CLI's exit(3)=decline / exit(1)=error contract survives inside a
 * long-lived worker. Without this block libext4fs.so exports nothing and
 * `invf-ivpack_packs_test` fails on "ext4fs: exports the three ABI symbols" --
 * which is how shard 4 of `make test` had been dying one line past the member
 * bound check. */
static const ivpack_desc s_ext4fs_desc = {
    IVPACK_API_VERSION, "ext4fs", "1.0.0", "containerpack", 0
};

const ivpack_desc *ivpack_get_desc(void) { return &s_ext4fs_desc; }

IVPACK_CANON_CALLS(ext4fs)

IVPACK_DEFINE_CONTAINER_CMD(ext4fs, IVPACK_GLUE_NONE())

IVPACK_DEFINE_CONTAINER_ESTIMATE(ext4fs, IVPACK_GLUE_NONE(),
                                rc = (int)cmd_estimate(a);)

#ifndef IVPACK_SHARED_LIB
int main(int argc, char **argv)
{
    g_verbose = getenv("EXT4FS_DEBUG") != NULL;
    if (argc < 2) return EX_USAGE;
    if (!strcmp(argv[1], "enumerate") && argc == 4)
        return cmd_enumerate(argv[2], argv[3]);
    if (!strcmp(argv[1], "extract") && argc == 5)
        return cmd_extract(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "strip") && argc == 4)
        return cmd_strip(argv[2], argv[3]);
    if (!strcmp(argv[1], "rebuild") && argc == 5)
        return cmd_rebuild(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "map") && argc == 4)
        return cmd_map(argv[2], argv[3]);
    if (!strcmp(argv[1], "estimate") && argc == 3)
        return cmd_estimate(argv[2]);
    if (!strcmp(argv[1], "classify") && argc == 4)
        return cmd_classify(argv[2], argv[3]);
    return EX_USAGE;
}
#endif /* !IVPACK_SHARED_LIB */
