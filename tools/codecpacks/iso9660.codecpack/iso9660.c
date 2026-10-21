/*
 * iso9660.c — iso9660/udf containerpack helper (InvariantFS WP636).
 *
 * Decomposes OPTICAL IMAGES (".iso") into one member per RECORDED FILE DATA
 * EXTENT, so the members flow through the whole normal pipeline (text PPMd
 * batching / binary ZSTD(+BCJ) batching / generic ZSTD-19, nested containers
 * included — a Windows ISO's install.wim is a member and can be decomposed
 * by the WIM lane, WP637, without this pack knowing about WIM).
 *
 * Bit-exactness is BY CONSTRUCTION, the same contract qcow2/rawdisk/p7z
 * follow: the recipe (strip) is the image size + the member table + every
 * byte OUTSIDE the member extents, verbatim, in file order. Filesystem
 * metadata (system area, volume descriptors, path tables, directory records,
 * UDF file entries, boot catalogs, any gap or junk) is NEVER regenerated --
 * it rides in the recipe untouched, so rebuild is a splice and there is
 * nothing to re-encode and nothing to get subtly wrong. The descriptor sets
 * are parsed ONLY to LOCATE member extents.
 *
 * That shape also makes the FS's own guard decisive: the sweep extracts the
 * members, splices them with the recipe and memcmps the original BEFORE it
 * commits anything (src/core/vol_cpack.c, the full-house guard + the
 * seekable map guard). A wrong extent table is therefore a REFUSAL (the file
 * stays RAW/generic), never corruption.
 *
 * Commands (fixed argv, no shell; exit 0 = ok, 3 = decline, 1 = error,
 * 2 = usage; stdout/stderr are /dev/null except estimate's stdout):
 *
 *     enumerate <in> <out>           member table lines "idx<TAB>sname<TAB>usize"
 *     extract   <in> <idx> <out>     member idx's raw bytes (exactly usize)
 *     strip     <in> <out>           the recipe (I9R1, below)
 *     rebuild   <recipe> <dir> <out> original image, bit-exact
 *     map       <in> <out>           the FS-owned MRMP member map (WP16b)
 *     estimate  <in>                 print size + sum(usize) + 64 MiB
 *
 * SNIFF ORDER (the manifest declares both magics; the helper's strict parse
 * is the real gate — a sniff hit whose parse fails is a DECLINE, exit 3):
 *   1. UDF: "BEA01"/"NSR02"/"NSR03" in the volume recognition sequence at
 *      sector 16, and an Anchor Volume Descriptor Pointer (tag 2) at sector
 *      256, N-1 or N-256.
 *   2. ISO9660: "CD001" at byte 16*2048+1.
 * A UDF-bridge disc carries BOTH; the bridge trees name the same extents, so
 * the walker prefers UDF when it parses and falls back to ISO9660, and
 * prefers the Joliet supplementary tree over the primary tree for names
 * (both name the same file extents; only the names differ).
 *
 * RECIPE FORMAT ("I9R1", pack-owned; the FS never parses it). All integers
 * little-endian, the same shape as rawdisk's RDR1/qcow2's Q2R3:
 *
 *   offset  size  field
 *   0       4     "I9R1"
 *   4       8     u64 image_size
 *   12      4     u32 n_members
 *   16      20*n_members   members, sorted by off:
 *                               u32 idx, u64 off, u64 len        (len > 0)
 *   ..      4     u32 n_ranges
 *   ..      var   ranges, sorted by off, each:
 *                               u64 off, u64 len, u8 bytes[len]  (len > 0)
 *
 * members + ranges together EXACTLY partition [0, image_size) (rebuild
 * validates this before writing anything). rebuild: truncate {out} to
 * image_size, write the recipe ranges verbatim at their offsets, splice each
 * "<dir>/<idx>" at its member extent; a missing/short/long member file or
 * any recipe inconsistency exits 1.
 *
 * MAP (MRMP, FS-owned): one RECIPE entry per recipe range (src_off = the
 * range's payload offset INSIDE the I9R1 blob) and one MEMBER entry per
 * member extent (idx, src_off = 0) — every member is one contiguous extent,
 * which is what makes the map trivial. Entries sorted by orig_off,
 * partitioning [0, image_size). strip and map share the layout math.
 *
 * MEMBER GRANULARITY, and why it is per-extent rather than per-file: an
 * optical image stores a file as one or more recorded extents plus holes
 * (extent type 1/2 = allocated-not-recorded / not-allocated). A hole's
 * on-disk bytes are NOT required to be zeros, so turning a hole into a
 * zero-filled member would lose them. Extents only are members; holes and
 * every other byte are recipe. A file split over several extents (ISO9660
 * multi-extent records, UDF multi-AD files) therefore appears as several
 * members, each a contiguous original range — exactly how rawdisk treats a
 * partitioned disk. Reads and rebuild are unaffected: members are opaque
 * byte ranges spliced back in file order.
 *
 * Refuse list (exit 3, "not ours / cannot decompose bit-exactly"):
 *   neither an ISO9660 PVD (CD001 @16*2048+1) nor a valid UDF anchor
 *   truncated image, descriptor past EOF, logical block size not 2048/1024/512
 *   ISO9660: bad both-endian copies, record crossing a block, record length
 *     past the directory extent, extended-attribute records (ext_attr_len
 *     != 0 — the data does not start at the extent), extent outside the
 *     image, zero members, > 1M members, directory depth > 32, symlinked/
 *     duplicate extent sets that OVERLAP (decline, never guess)
 *   UDF: tag checksum/CRC/tagLocation mismatch, no PVD/LVD/PD/FSD, a
 *     partition map that is not type 1 (metadata/virtual/sparable — UDF 2.0+
 *     Windows media would need the metadata-partition walk), missing root
 *     ICB, an Allocation Descriptor type this pack does not read (2 =
 *     extended_ad, 3 = embedded), an ICB that is not a File Entry /
 *     Extended File Entry (indirect/terminal entries decline), a FID chain
 *     that runs past its directory extent
 * extract on an unannounced idx and all rebuild inconsistencies are hard
 * errors (exit 1), not declines.
 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ADR-007: this file doubles as a .so plugin. See rawdisk.c for the shape. */
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

#define ISO_SECTOR     2048ull          /* the only sector size in practice */
#define COPY_BUF       (8u << 20)       /* 8 MiB streaming window */
#define MAX_MEMBERS    1048575u         /* FS idx is 0..1048575; we start at 1 */
#define DIR_MAX_DEPTH  32               /* ISO9660 spec says 8; be generous */
#define DIR_MAX_BYTES  (64ull << 20)    /* one directory's data extent cap */
#define DIR_MAX_COUNT  200000u          /* directories visited cap */
#define ESTIMATE_MARGIN (64ull << 20)
#define SNAME_MAX      40

static uint8_t *g_buf;                  /* the streaming window (main) */

typedef struct {
    uint32_t idx;                       /* member index (the FS contract) */
    uint64_t off;                       /* byte offset in the image */
    uint64_t len;                       /* extent bytes (== usize) */
    char     sname[SNAME_MAX];          /* suggested name (advisory) */
} member_t;

typedef struct { uint64_t off, len; } range_t;

typedef struct {
    uint64_t  img_size;
    member_t *members;                  /* sorted by off in plan_finish */
    size_t    nmem;
    range_t  *ranges;                   /* member complement */
    size_t    nrng;
    uint32_t  ndirs;                    /* walk caps */
} plan_t;

/* ---------------- integer + I/O helpers ---------------- */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p)
{ return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ISO9660 "both byte order" fields: the little-endian copy first, then the
 * big-endian one. Both must agree; a disagreement is a structural violation
 * (ECMA-119 7.3.3), so it declines. */
static int both32(const uint8_t *p, uint32_t *out)
{
    uint32_t l = le32(p), b = be32(p + 4);
    if (l != b) return -1;
    *out = l;
    return 0;
}
static int both16(const uint8_t *p, uint16_t *out)
{
    uint16_t l = le16(p), b = be16(p + 2);
    if (l != b) return -1;
    *out = l;
    return 0;
}

static int wr(FILE *f, const void *b, size_t n)
{ return fwrite(b, 1, n, f) == n ? 0 : -1; }

static int wr_le32(FILE *f, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
    return wr(f, b, 4);
}

static int wr_le64(FILE *f, uint64_t v)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    return wr(f, b, 8);
}

static int rd_exact(FILE *f, void *b, size_t n)
{ return fread(b, 1, n, f) == n ? 0 : -1; }

static int rd_le32f(FILE *f, uint32_t *v)
{
    uint8_t b[4];
    if (rd_exact(f, b, 4) != 0) return -1;
    *v = le32(b);
    return 0;
}

static int rd_le64f(FILE *f, uint64_t *v)
{
    uint8_t b[8];
    if (rd_exact(f, b, 8) != 0) return -1;
    *v = le64(b);
    return 0;
}

static int seek_to(FILE *f, uint64_t off)
{ return fseeko(f, (off_t)off, SEEK_SET); }

static int read_at(FILE *f, uint64_t off, void *buf, size_t n)
{
    if (seek_to(f, off) != 0) return -1;
    return rd_exact(f, buf, n);
}

/* copy n bytes src -> dst (both already positioned), <= 8 MiB windows */
static int stream_copy(FILE *src, FILE *dst, uint64_t n)
{
    while (n) {
        size_t c = n > COPY_BUF ? COPY_BUF : (size_t)n;
        if (fread(g_buf, 1, c, src) != c) return -1;
        if (fwrite(g_buf, 1, c, dst) != c) return -1;
        n -= c;
    }
    return 0;
}

static uint64_t file_size_of(FILE *f)
{
    off_t sz;
    if (fseeko(f, 0, SEEK_END) != 0) return 0;
    sz = ftello(f);
    return sz < 0 ? 0 : (uint64_t)sz;
}

/* ---------------- plan (shared by both walkers) ---------------- */

static void plan_free(plan_t *p)
{
    free(p->members);
    free(p->ranges);
    memset(p, 0, sizeof *p);
}

static int plan_add(plan_t *p, uint64_t off, uint64_t len, const char *sname)
{
    member_t *m;
    if (len == 0) return 0;                     /* empty extent: recipe's job */
    if (p->nmem >= MAX_MEMBERS) return -1;
    if (off > p->img_size || len > p->img_size - off) return -1;
    m = (member_t *)realloc(p->members, (p->nmem + 1) * sizeof *m);
    if (!m) return -1;
    p->members = m;
    m = &p->members[p->nmem++];
    memset(m, 0, sizeof *m);
    m->off = off;
    m->len = len;
    snprintf(m->sname, sizeof m->sname, "%s", sname ? sname : "");
    return 0;
}

/* sort by offset, then length, so IDENTICAL extents end up adjacent and
 * can be coalesced deterministically */
static int member_off_cmp(const void *a, const void *b)
{
    const member_t *x = (const member_t *)a, *y = (const member_t *)b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    if (x->len != y->len) return x->len < y->len ? -1 : 1;
    return 0;
}

/* sort by offset, refuse overlaps, assign dense idx 1..n, build the
 * complement ranges. 0 = ok, -1 = refuse (decline). */
static int plan_finish(plan_t *p)
{
    uint64_t pos = 0;
    size_t i, n = 0;

    if (!p->nmem) return -1;                    /* nothing to decompose */
    qsort(p->members, p->nmem, sizeof *p->members, member_off_cmp);
    /* COALESCE hard links: two directories naming ONE extent is not a
     * conflict -- it is the same bytes twice, and optical images ship that
     * way in quantity (measured on the Win7 corpus: 21 such pairs). One
     * member per distinct extent; the recipe keeps both names, because the
     * directory records that name them ride there verbatim. */
    for (i = 0; i < p->nmem; i++) {
        if (n && p->members[i].off == p->members[n - 1].off &&
            p->members[i].len == p->members[n - 1].len)
            continue;
        if (n != i) p->members[n] = p->members[i];
        n++;
    }
    p->nmem = n;
    for (i = 1; i < p->nmem; i++)
        if (p->members[i].off < p->members[i - 1].off + p->members[i - 1].len)
            return -1;                  /* partially overlapping: refuse */
    for (i = 0; i < p->nmem; i++) p->members[i].idx = (uint32_t)(i + 1);
    p->ranges = (range_t *)malloc((p->nmem + 1) * sizeof *p->ranges);
    if (!p->ranges) return -1;
    n = 0;                                      /* ranges are counted afresh */
    for (i = 0; i < p->nmem; i++) {
        if (p->members[i].off > pos) {
            p->ranges[n].off = pos;
            p->ranges[n].len = p->members[i].off - pos;
            n++;
        }
        pos = p->members[i].off + p->members[i].len;
    }
    if (pos < p->img_size) {
        p->ranges[n].off = pos;
        p->ranges[n].len = p->img_size - pos;
        n++;
    }
    p->nrng = n;
    return 0;
}

/* ---------------- ISO9660 (ECMA-119) ---------------- */

/* A parsed directory record. */
typedef struct {
    uint64_t ext_lba;                       /* logical block of the extent */
    uint64_t ext_len;                       /* bytes */
    uint8_t  flags;
    int      dot;                           /* "." / ".." (id 0x00 / 0x01) */
    char     name[64];
} isorec_t;

/* UCS-2BE (Joliet) or ASCII (primary) identifier -> ASCII sname, with the
 * ECMA-119 ";version" suffix and a trailing '.' dropped. */
static void iso_name(isorec_t *o, const uint8_t *nm, unsigned nlen, int jol)
{
    size_t w = 0;
    unsigned i;
    if (jol) {
        for (i = 0; i + 1 < nlen && w + 1 < sizeof o->name; i += 2) {
            unsigned u = (unsigned)((nm[i] << 8) | nm[i + 1]);
            o->name[w++] = (u >= 0x20 && u < 0x7F && u != '/' && u != '\\') ?
                           (char)u : '_';
        }
    } else {
        for (i = 0; i < nlen && w + 1 < sizeof o->name; i++) {
            unsigned ch = nm[i];
            o->name[w++] = (ch >= 0x20 && ch < 0x7F && ch != '/' && ch != '\\') ?
                           (char)ch : '_';
        }
    }
    o->name[w] = 0;
    while (w && o->name[w - 1] == '.') o->name[--w] = 0;   /* "FILE." case */
    for (i = 0; i < w; i++)
        if (o->name[i] == ';') { o->name[i] = 0; break; }  /* ";1" version */
}

static int iso_record(const uint8_t *r, unsigned rl, int jol, isorec_t *o)
{
    uint32_t ext, dlen;
    unsigned nlen;
    if (rl < 34) return -1;
    if (r[1] != 0) return -1;      /* extended attribute record: data does
                                    * not start at the extent */
    if (both32(r + 2, &ext) != 0 || both32(r + 10, &dlen) != 0) return -1;
    nlen = r[32];
    if (nlen == 0 || 33u + nlen > rl) return -1;
    memset(o, 0, sizeof *o);
    o->ext_lba = ext;
    o->ext_len = dlen;
    o->flags = r[25];
    o->dot = (nlen == 1 && (r[33] == 0 || r[33] == 1)) ? 1 : 0;
    iso_name(o, r + 33, nlen, jol);
    return 0;
}

/* Walk one directory extent, recursing into subdirectories. */
static int iso_dir(plan_t *p, FILE *f, uint64_t blk, uint64_t lba,
                   uint64_t dlen, int depth, int jol)
{
    uint8_t *d;
    uint64_t i, base;
    int rc = 0;

    if (depth > DIR_MAX_DEPTH) return -1;
    if (dlen == 0 || dlen > DIR_MAX_BYTES) return -1;
    base = lba * blk;
    if (base > p->img_size || dlen > p->img_size - base) return -1;
    if (++p->ndirs > DIR_MAX_COUNT) return -1;
    d = (uint8_t *)malloc((size_t)dlen);
    if (!d) return -1;
    if (read_at(f, base, d, (size_t)dlen) != 0) { free(d); return -1; }
    for (i = 0; i < dlen; ) {
        unsigned rl = d[i];
        isorec_t r;
        if (rl == 0) {                          /* pad to the next block */
            i = (i / blk + 1) * blk;
            continue;
        }
        if (i + rl > dlen) { rc = -1; break; }  /* record crosses the extent */
        if (iso_record(d + i, rl, jol, &r) != 0) { rc = -1; break; }
        i += rl;
        if (r.dot) continue;                    /* "." and ".." */
        if (r.ext_len == 0) continue;           /* empty file */
        if (r.flags & 0x02) {
            if (iso_dir(p, f, blk, r.ext_lba, r.ext_len, depth + 1, jol) != 0) {
                rc = -1;
                break;
            }
        } else if (plan_add(p, r.ext_lba * blk, r.ext_len, r.name) != 0) {
            rc = -1;
            break;
        }
    }
    free(d);
    return rc;
}

/* Scan the volume descriptor sequence (sector 16 forward) for a Primary and
 * a Joliet Supplementary descriptor, then walk the tree they point at.
 * Joliet wins for NAMES when both exist — it names the same file extents
 * with the long name instead of the 8.3 one. 0 = ok, 3 = decline. */
static int iso_parse(FILE *f, plan_t *p)
{
    uint8_t desc[ISO_SECTOR], pvd[ISO_SECTOR], svd[ISO_SECTOR];
    int have_pvd = 0, have_svd = 0;
    unsigned sec;
    uint16_t bs16 = 0;
    uint32_t vss = 0;
    uint64_t blk, base, dlen;
    uint8_t *root;
    isorec_t r;
    int jol;

    memset(pvd, 0, sizeof pvd);
    memset(svd, 0, sizeof svd);
    for (sec = 16; sec < 16 + 64; sec++) {
        if (read_at(f, (uint64_t)sec * ISO_SECTOR, desc, sizeof desc) != 0)
            break;
        /* A UDF bridge disc puts the volume recognition sequence
         * (BEA01/NSR02/NSR03/TEA01) at sector 16 and starts the ISO
         * descriptors after it. Skip those blocks instead of stopping at
         * the first non-CD001 one -- otherwise the ISO fallback can never
         * read the tree of a disc whose UDF side declined (fixture kind
         * "vrs-iso", mk-fixtures.py in this pack dir). */
        if (memcmp(desc + 1, "CD001", 5) != 0) {
            if (memcmp(desc + 1, "BEA01", 5) == 0 ||
                memcmp(desc + 1, "NSR02", 5) == 0 ||
                memcmp(desc + 1, "NSR03", 5) == 0 ||
                memcmp(desc + 1, "TEA01", 5) == 0)
                continue;
            break;
        }
        if (desc[0] == 1 && !have_pvd) {
            memcpy(pvd, desc, sizeof pvd);
            have_pvd = 1;
        } else if (desc[0] == 2 && !have_svd) {
            if (memcmp(desc + 88, "%/@", 3) == 0 ||
                memcmp(desc + 88, "%/C", 3) == 0 ||
                memcmp(desc + 88, "%/E", 3) == 0) {
                memcpy(svd, desc, sizeof svd);
                have_svd = 1;
            }
        }
        if (desc[0] == 255) break;               /* terminator */
    }
    if (!have_pvd) return 3;
    if (both16(pvd + 128, &bs16) != 0) return 3;
    if (bs16 < 512 || bs16 > 4096 || (bs16 & (bs16 - 1)) != 0) return 3;
    if (both32(pvd + 80, &vss) != 0) return 3;
    blk = bs16;
    if (!vss) return 3;
    if ((uint64_t)vss * blk > p->img_size) return 3;   /* truncated image */
    root = have_svd ? svd + 156 : pvd + 156;
    jol = have_svd;
    if (iso_record(root, (unsigned)root[0], jol, &r) != 0) return 3;
    if (!(r.flags & 0x02) || r.ext_len == 0) return 3;
    base = r.ext_lba * blk;
    dlen = r.ext_len;
    if (base > p->img_size || dlen > p->img_size - base) return 3;
    if (iso_dir(p, f, blk, r.ext_lba, dlen, 0, jol) != 0) return 3;
    if (plan_finish(p) != 0) return 3;
    return 0;
}

/* Drop a walker's partial plan but keep the image size, so the fallback
 * walker starts clean (UDF declined after adding members, say). */
static void plan_reset(plan_t *p)
{
    uint64_t keep = p->img_size;
    plan_free(p);
    p->img_size = keep;
}

static int udf_parse(FILE *f, plan_t *p);      /* below */

#define I9_MAGIC "I9R1"

/* 0 = parsed, 3 = decline, 1 = I/O error. UDF first (bridge discs carry
 * both, and the UDF tree is the authoritative one), ISO9660 second. */
static int analyze(const char *path, plan_t *p)
{
    FILE *f;
    uint64_t size;
    int rc;

    memset(p, 0, sizeof *p);
    f = fopen(path, "rb");
    if (!f) return 1;
    size = file_size_of(f);
    if (!size) { fclose(f); return 3; }
    p->img_size = size;
    rc = udf_parse(f, p);
    if (rc != 0) {
        plan_reset(p);
        rc = iso_parse(f, p);
    }
    if (rc != 0) plan_free(p);
    fclose(f);
    return rc;
}

/* ---------------- commands ---------------- */

static int cmd_enumerate(const char *in, const char *out)
{
    plan_t pl;
    FILE *o;
    size_t i;
    int rc = analyze(in, &pl);

    if (rc != 0) return rc;
    o = fopen(out, "w");
    if (!o) { plan_free(&pl); return 1; }
    for (i = 0; i < pl.nmem; i++)
        fprintf(o, "%u\t%s\t%" PRIu64 "\n", pl.members[i].idx,
                pl.members[i].sname, pl.members[i].len);
    rc = fclose(o) == 0 ? 0 : 1;
    plan_free(&pl);
    return rc;
}

static int cmd_extract(const char *in, const char *idx_s, const char *out)
{
    plan_t pl;
    FILE *fi = NULL, *fo = NULL;
    const member_t *m = NULL;
    char *end = NULL;
    unsigned long idx;
    size_t i;
    int rc;

    idx = strtoul(idx_s, &end, 10);
    if (!end || *end) return 1;
    rc = analyze(in, &pl);
    if (rc != 0) return rc;
    for (i = 0; i < pl.nmem; i++)
        if (pl.members[i].idx == (uint32_t)idx) { m = &pl.members[i]; break; }
    if (!m) { plan_free(&pl); return 1; }       /* the FS asks announced idx */
    fi = fopen(in, "rb");
    fo = fopen(out, "wb");
    if (fi && fo && seek_to(fi, m->off) == 0 &&
        stream_copy(fi, fo, m->len) == 0 && fclose(fo) == 0)
        rc = 0;
    else
        rc = 1;
    if (fi) fclose(fi);
    plan_free(&pl);
    return rc;
}

static int cmd_strip(const char *in, const char *out)
{
    plan_t pl;
    FILE *fi = NULL, *fo = NULL;
    size_t i;
    int rc = analyze(in, &pl);

    if (rc != 0) return rc;
    fi = fopen(in, "rb");
    fo = fopen(out, "wb");
    if (!fi || !fo) goto fail;
    if (wr(fo, I9_MAGIC, 4) != 0 ||
        wr_le64(fo, pl.img_size) != 0 ||
        wr_le32(fo, (uint32_t)pl.nmem) != 0)
        goto fail;
    for (i = 0; i < pl.nmem; i++)
        if (wr_le32(fo, pl.members[i].idx) != 0 ||
            wr_le64(fo, pl.members[i].off) != 0 ||
            wr_le64(fo, pl.members[i].len) != 0)
            goto fail;
    if (wr_le32(fo, (uint32_t)pl.nrng) != 0) goto fail;
    for (i = 0; i < pl.nrng; i++) {
        if (wr_le64(fo, pl.ranges[i].off) != 0 ||
            wr_le64(fo, pl.ranges[i].len) != 0)
            goto fail;
        if (seek_to(fi, pl.ranges[i].off) != 0 ||
            stream_copy(fi, fo, pl.ranges[i].len) != 0)
            goto fail;
    }
    rc = fclose(fo) == 0 ? 0 : 1;
    if (fi) fclose(fi);
    plan_free(&pl);
    return rc;
fail:
    if (fi) fclose(fi);
    if (fo) fclose(fo);
    plan_free(&pl);
    return 1;
}

/* I9R1 parse state (rebuild) */
typedef struct { uint64_t off, len, payload_pos; } rrange_t;

static int cmd_rebuild(const char *recipe, const char *dir, const char *out)
{
    FILE *fr = NULL, *fo = NULL, *fm = NULL;
    member_t *mem = NULL;
    rrange_t *rng = NULL;
    uint64_t size, rsize, pos, payload;
    uint32_t nmem = 0, nrng = 0, i;
    int rc = 1;

    fr = fopen(recipe, "rb");
    if (!fr) return 1;
    rsize = file_size_of(fr);
    if (!rsize || seek_to(fr, 0) != 0) goto out;
    {
        uint8_t magic[4];
        if (rd_exact(fr, magic, 4) != 0 || memcmp(magic, I9_MAGIC, 4) != 0)
            goto out;
    }
    if (rd_le64f(fr, &size) != 0 || !size || size > (1ull << 48)) goto out;
    if (rd_le32f(fr, &nmem) != 0 || !nmem || nmem > MAX_MEMBERS) goto out;
    mem = (member_t *)malloc((size_t)nmem * sizeof *mem);
    if (!mem) goto out;
    pos = 0;
    for (i = 0; i < nmem; i++) {
        if (rd_le32f(fr, &mem[i].idx) != 0 ||
            rd_le64f(fr, &mem[i].off) != 0 ||
            rd_le64f(fr, &mem[i].len) != 0)
            goto out;
        if (!mem[i].len || mem[i].len > size || mem[i].off > size - mem[i].len)
            goto out;
        if (mem[i].off < pos) goto out;          /* sorted, non-overlapping */
        pos = mem[i].off + mem[i].len;
    }
    if (rd_le32f(fr, &nrng) != 0 || nrng > nmem + 1) goto out;
    rng = (rrange_t *)malloc((nrng ? nrng : 1) * sizeof *rng);
    if (!rng) goto out;
    {
        off_t rs = ftello(fr);
        if (rs < 0) goto out;
        payload = (uint64_t)rs;
    }
    /* pass 1: validate the WHOLE recipe before writing anything */
    for (i = 0; i < nrng; i++) {
        if (rd_le64f(fr, &rng[i].off) != 0 || rd_le64f(fr, &rng[i].len) != 0)
            goto out;
        if (!rng[i].len || rng[i].len > size || rng[i].off > size - rng[i].len)
            goto out;
        rng[i].payload_pos = payload + 16;
        payload += 16 + rng[i].len;
        if (payload > rsize) goto out;           /* truncated recipe */
        if (fseeko(fr, (off_t)rng[i].len, SEEK_CUR) != 0) goto out;
    }
    if (payload != rsize) goto out;              /* trailing garbage */
    {   /* members + ranges must exactly partition [0, size) */
        uint32_t mi = 0;
        pos = 0;
        for (i = 0; i < nrng; i++) {
            while (mi < nmem && mem[mi].off < rng[i].off) {
                if (mem[mi].off != pos) goto out;
                pos += mem[mi].len;
                mi++;
            }
            if (rng[i].off != pos) goto out;
            pos += rng[i].len;
        }
        while (mi < nmem) {
            if (mem[mi].off != pos) goto out;
            pos += mem[mi].len;
            mi++;
        }
        if (pos != size) goto out;
    }
    /* pass 2: write — truncate to image_size, recipe ranges verbatim */
    fo = fopen(out, "wb");
    if (!fo) goto out;
    if (ftruncate(fileno(fo), (off_t)size) != 0) goto out;
    for (i = 0; i < nrng; i++) {
        if (seek_to(fr, rng[i].payload_pos) != 0 ||
            seek_to(fo, rng[i].off) != 0 ||
            stream_copy(fr, fo, rng[i].len) != 0)
            goto out;
    }
    for (i = 0; i < nmem; i++) {
        char mp[4096];
        uint64_t msz;
        int n = snprintf(mp, sizeof mp, "%s/%u", dir, mem[i].idx);
        if (n <= 0 || (size_t)n >= sizeof mp) goto out;
        fm = fopen(mp, "rb");
        if (!fm) goto out;
        msz = file_size_of(fm);
        if (msz != mem[i].len) goto out;         /* size mismatch: exit 1 */
        if (seek_to(fm, 0) != 0 || seek_to(fo, mem[i].off) != 0 ||
            stream_copy(fm, fo, mem[i].len) != 0)
            goto out;
        fclose(fm);
        fm = NULL;
    }
    if (fflush(fo) != 0 || ftruncate(fileno(fo), (off_t)size) != 0) goto out;
    rc = fclose(fo) == 0 ? 0 : 1;
    fo = NULL;
out:
    if (fm) fclose(fm);
    if (fo) fclose(fo);
    if (fr) fclose(fr);
    free(mem);
    free(rng);
    return rc;
}

static int cmd_map(const char *in, const char *out)
{
    plan_t pl;
    FILE *o = NULL;
    uint64_t src;
    size_t mi = 0, ri = 0;
    int rc = analyze(in, &pl);

    if (rc != 0) return rc;
    o = fopen(out, "wb");
    if (!o) { plan_free(&pl); return 1; }
    /* the layout strip writes: header, then (16B range hdr + payload) per
     * range in offset order — RECIPE src_off's index that blob */
    src = 4 + 8 + 4 + (uint64_t)pl.nmem * 20 + 4;
    if (wr(o, "MRMP", 4) != 0 ||
        wr_le32(o, (uint32_t)(pl.nmem + pl.nrng)) != 0)
        goto fail;
    while (mi < pl.nmem || ri < pl.nrng) {
        uint64_t orig_off, len, src_off;
        uint32_t idx;
        uint8_t kind;
        if (ri < pl.nrng &&
            (mi == pl.nmem || pl.ranges[ri].off < pl.members[mi].off)) {
            orig_off = pl.ranges[ri].off;
            len = pl.ranges[ri].len;
            kind = 0;                            /* RECIPE */
            idx = 0;
            src_off = src + 16;                  /* past the range header */
            src += 16 + len;
            ri++;
        } else {
            orig_off = pl.members[mi].off;
            len = pl.members[mi].len;
            kind = 1;                            /* MEMBER */
            idx = pl.members[mi].idx;
            src_off = 0;
            mi++;
        }
        if (wr_le64(o, orig_off) != 0 || wr_le64(o, len) != 0 ||
            wr(o, &kind, 1) != 0 || wr_le32(o, idx) != 0 ||
            wr_le64(o, src_off) != 0)
            goto fail;
    }
    rc = fclose(o) == 0 ? 0 : 1;
    plan_free(&pl);
    return rc;
fail:
    fclose(o);
    plan_free(&pl);
    return 1;
}

static int cmd_estimate(const char *in)
{
    plan_t pl;
    uint64_t sum = ESTIMATE_MARGIN;
    size_t i;
    int rc = analyze(in, &pl);

    if (rc != 0) return rc;
    for (i = 0; i < pl.nmem; i++) sum += pl.members[i].len;
    printf("%" PRIu64 "\n", sum);
    plan_free(&pl);
    return 0;
}

/* ---------------- ivpack plugin glue (ADR-007) ---------------- */

static int ivpack_glue_iso9660(void)
{
    if (!g_buf) g_buf = (uint8_t *)malloc(COPY_BUF);
    return g_buf ? 0 : IVPACK_RC_ERROR;
}

static const ivpack_desc s_iso9660_desc = {
    IVPACK_API_VERSION, "iso9660", "1.0.0", "containerpack", 0
};

const ivpack_desc *ivpack_get_desc(void) { return &s_iso9660_desc; }

IVPACK_CANON_CALLS(iso9660)

IVPACK_DEFINE_CONTAINER_CMD(iso9660, ivpack_glue_iso9660())

IVPACK_DEFINE_CONTAINER_ESTIMATE(iso9660, ivpack_glue_iso9660(),
                             rc = (int)cmd_estimate(a);)

#ifndef IVPACK_SHARED_LIB
int main(int argc, char **argv)
{
    const char *cmd;
    int rc;

    if (argc < 2) return 2;
    cmd = argv[1];
    g_buf = (uint8_t *)malloc(COPY_BUF);
    if (!g_buf) return 1;
    rc = 2;
    if (!strcmp(cmd, "enumerate") && argc == 4)
        rc = cmd_enumerate(argv[2], argv[3]);
    else if (!strcmp(cmd, "extract") && argc == 5)
        rc = cmd_extract(argv[2], argv[3], argv[4]);
    else if (!strcmp(cmd, "strip") && argc == 4)
        rc = cmd_strip(argv[2], argv[3]);
    else if (!strcmp(cmd, "rebuild") && argc == 5)
        rc = cmd_rebuild(argv[2], argv[3], argv[4]);
    else if (!strcmp(cmd, "map") && argc == 4)
        rc = cmd_map(argv[2], argv[3]);
    else if (!strcmp(cmd, "estimate") && argc == 3)
        rc = cmd_estimate(argv[2]);
    free(g_buf);
    return rc;
}
#endif

/* ---------------- UDF (ECMA-167 / OSTA UDF) ---------------- */

#define UDF_BLK      2048u              /* the AVDP sits at sector 256 of 2048 */
#define UDF_VDS_MAX  (1u << 20)         /* VDS byte-length sanity cap */
#define UDF_MAX_MAPS 8u

typedef struct { uint32_t len, lb; uint16_t part; } lad_t;

static uint16_t crc16_ccitt(const uint8_t *d, size_t n)
{
    uint16_t crc = 0;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for (k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

/* Validate a descriptor tag: tagIdentifier, descriptorVersion (2 = ECMA-167
 * rev 2 / UDF 1.x, 3 = rev 3 / UDF 2.x), the 8-bit tagChecksum and the CRC.
 * tagLocation is checked separately (structures inside a partition are
 * recorded with the partition-relative block by some writers and with the
 * absolute sector by others; both are accepted, nothing else is). */
static int udf_tag_ok(const uint8_t *b, size_t blen, uint16_t want_id)
{
    unsigned ck = 0, i;
    uint16_t crc, crclen;
    if (blen < 16) return 0;
    if (le16(b) != want_id) return 0;
    if (le16(b + 2) != 2 && le16(b + 2) != 3) return 0;
    for (i = 0; i < 16; i++)
        if (i != 4) ck += b[i];
    if ((ck & 0xFF) != b[4]) return 0;
    crc = le16(b + 8);
    crclen = le16(b + 10);
    if ((size_t)crclen + 16 > blen) return 0;
    if (crc16_ccitt(b + 16, crclen) != crc) return 0;
    return 1;
}

static void udf_lad(const uint8_t *b, lad_t *o)
{
    o->len = le32(b);
    o->lb = le32(b + 4);
    o->part = le16(b + 8);
}

typedef struct {
    uint64_t img_size;
    uint32_t part_start;                /* partition start, in 2048 sectors */
    uint32_t part_len;
    uint16_t part_num;                  /* the Partition Descriptor's NUMBER */
    uint16_t part_index;                /* the INDEX in the LVD partition-map
                                         * array that names part_num: what a
                                         * long_ad's partition reference is */
    uint32_t ndirs;                     /* walk caps */
    uint8_t  root[UDF_BLK];             /* scratch for a descriptor read */
} udf_t;

/* Absolute byte offset of a partition-relative logical block, or -1 when the
 * block is outside the partition (or the partition outside the image). */
static int64_t udf_abs(const udf_t *u, uint32_t lb)
{
    uint64_t s;
    if (u->part_len && lb >= u->part_len) return -1;
    s = (uint64_t)u->part_start + lb;
    if (s * UDF_BLK > u->img_size) return -1;
    return (int64_t)(s * UDF_BLK);
}

/* The volume recognition sequence: "BEA01"/"NSR02"/"NSR03" at sector 16.
 * Used only as a cheap gate before the (still strict) anchor parse. */
static int udf_vrs_ok(FILE *f)
{
    uint8_t b[8];
    if (read_at(f, 16 * UDF_BLK, b, sizeof b) != 0) return 0;
    return memcmp(b + 1, "BEA0", 4) == 0 || memcmp(b + 1, "NSR0", 4) == 0;
}

/* AVDP -> main volume descriptor sequence -> PD + LVD. Fills u; on success
 * *fsd_off receives the byte offset of the File Set Descriptor. */
static int udf_volume(FILE *f, udf_t *u, uint64_t *fsd_off)
{
    static const int64_t anchors[3] = { 0, -1, -256 };  /* 256, N-1, N-256 */
    uint8_t av[512], lvd[UDF_BLK];
    uint64_t nsec = u->img_size / UDF_BLK;
    int64_t asec = -1;
    uint32_t vds_len = 0, vds_loc = 0, off;
    int have_pd = 0, have_lvd = 0, i;
    lad_t fsd;

    if (!udf_vrs_ok(f)) return 3;
    for (i = 0; i < 3; i++) {
        int64_t s = anchors[i] < 0 ? (int64_t)nsec + anchors[i]
                                  : 256 + anchors[i];
        if (s < 0 || (uint64_t)s >= nsec || s * 2048ull + 512 > u->img_size)
            continue;
        if (read_at(f, (uint64_t)s * UDF_BLK, av, sizeof av) != 0) continue;
        if (!udf_tag_ok(av, sizeof av, 2)) continue;
        if ((uint64_t)le32(av + 12) != (uint64_t)s) continue;
        asec = s;
        break;
    }
    if (asec < 0) return 3;
    vds_len = le32(av + 16);
    vds_loc = le32(av + 20);
    if (!vds_len || vds_len > UDF_VDS_MAX) return 3;
    if (vds_len % UDF_BLK) return 3;
    if ((uint64_t)vds_loc * UDF_BLK + vds_len > u->img_size) return 3;
    for (off = 0; off < vds_len; off += UDF_BLK) {
        uint8_t d[UDF_BLK];
        uint32_t loc = vds_loc + off / UDF_BLK;
        if (read_at(f, (uint64_t)loc * UDF_BLK, d, sizeof d) != 0) return 3;
        if (le16(d) == 8) break;                       /* terminating desc */
        if (!udf_tag_ok(d, sizeof d, le16(d))) return 3;
        if ((uint64_t)le32(d + 12) != (uint64_t)loc) return 3;
        if (le16(d) == 5 && !have_pd) {                /* Partition */
            u->part_num = le16(d + 22);
            u->part_start = le32(d + 188);
            u->part_len = le32(d + 192);
            have_pd = 1;
        } else if (le16(d) == 6 && !have_lvd) {        /* Logical Volume */
            memcpy(lvd, d, sizeof lvd);
            have_lvd = 1;
        } else if (le16(d) == 3) {
            return 3;                /* Volume Descriptor Pointer: the
                                      * sequence continues elsewhere */
        }
    }
    if (!have_pd || !have_lvd) return 3;
    if (le32(lvd + 212) != UDF_BLK) return 3;          /* not optical sizing */
    if (!u->part_len) return 3;
    if ((uint64_t)u->part_start + u->part_len >
        u->img_size / UDF_BLK) return 3;
    {   /* the partition map must be a single type-1 (physical) map naming
         * the partition we just read. Type 2 = UDF 2.0+ metadata partition
         * (a second indirection this pack does not walk): decline. */
        uint32_t mtl = le32(lvd + 264), nm = le32(lvd + 268), mo = 440, k;
        int found = 0;
        if (nm == 0 || nm > UDF_MAX_MAPS) return 3;
        if (mtl > UDF_BLK - 440) return 3;
        for (k = 0; k < nm; k++) {
            uint8_t type, len;
            if (mo + 2 > UDF_BLK) return 3;
            type = lvd[mo];
            len = lvd[mo + 1];
            if (len < 2 || mo + len > UDF_BLK) return 3;
            if (type != 1) return 3;
            if (len != 6 || le16(lvd + mo + 4) != u->part_num) return 3;
            /* ECMA-167 3/7.3, 3/10.5: a long_ad carries a partition
             * REFERENCE NUMBER, which is the INDEX of the map in this
             * array -- NOT the Partition Descriptor's partition number.
             * Real Windows media numbers the partition 2989 and refers to
             * it as 0; comparing the two declined every such disc. */
            u->part_index = (uint16_t)k;
            found = 1;
            mo += len;
        }
        if (!found) return 3;
    }
    udf_lad(lvd + 248, &fsd);
    if (fsd.part != u->part_index) return 3;
    {
        int64_t o = udf_abs(u, fsd.lb);
        if (o < 0) return 3;
        *fsd_off = (uint64_t)o;
    }
    return 0;
}

typedef struct {
    uint64_t info_len;
    uint32_t hdr;                       /* first byte of the EA area */
    uint32_t ealen, adlen;
    uint8_t  adtype;                    /* icbTag flags & 7 */
    uint8_t  ftype;                     /* icbTag fileType (4 dir, 5 file) */
    uint8_t  buf[UDF_BLK];
} fe_t;

/* Read and validate a File Entry (261) or Extended File Entry (266) at
 * partition-relative block lb. 0 = ok, 3 = decline. */
static int udf_fe(FILE *f, const udf_t *u, uint32_t lb, fe_t *fe)
{
    int64_t off = udf_abs(u, lb);
    uint16_t flags, tid;
    uint32_t tagloc;

    if (off < 0) return 3;
    if (read_at(f, (uint64_t)off, fe->buf, UDF_BLK) != 0) return 3;
    tid = le16(fe->buf);
    if (tid != 261 && tid != 266) return 3;  /* 259/260 indirect/terminal */
    if (!udf_tag_ok(fe->buf, UDF_BLK, tid)) return 3;
    tagloc = le32(fe->buf + 12);
    if (tagloc != lb && (uint64_t)tagloc != (uint64_t)u->part_start + lb)
        return 3;
    fe->hdr = tid == 261 ? 176u : 216u;
    fe->info_len = le64(fe->buf + 56);
    flags = le16(fe->buf + 34);
    fe->adtype = (uint8_t)(flags & 7);
    fe->ftype = fe->buf[27];
    fe->ealen = le32(fe->buf + fe->hdr - 8);
    fe->adlen = le32(fe->buf + fe->hdr - 4);
    if ((uint64_t)fe->hdr + fe->ealen + fe->adlen > UDF_BLK) return 3;
    return 0;
}

/* A directory's data as one contiguous buffer (holes read as zeros, which is
 * what a hole means). *out is malloc'd. 0 = ok, 3 = decline. */
static int udf_dir_data(FILE *f, const udf_t *u, const fe_t *fe,
                        uint8_t **out, uint64_t *out_len)
{
    uint64_t total = fe->info_len, pos = 0;
    unsigned esz, i;
    uint8_t *d;

    if (total > DIR_MAX_BYTES) return 3;
    d = (uint8_t *)calloc(1, total ? (size_t)total : 1);
    if (!d) return 3;
    if (fe->adtype == 3) {                       /* data embedded in the FE */
        if (total > UDF_BLK - fe->hdr - fe->ealen) { free(d); return 3; }
        memcpy(d, fe->buf + fe->hdr + fe->ealen, (size_t)total);
        *out = d;
        *out_len = total;
        return 0;
    }
    if (fe->adtype != 0 && fe->adtype != 1) {     /* 2 = extended_ad */
        free(d);
        return 3;
    }
    esz = fe->adtype == 0 ? 8u : 16u;
    for (i = 0; i + esz <= fe->adlen; i += esz) {
        uint32_t v = le32(fe->buf + fe->hdr + fe->ealen + i);
        uint32_t type = v >> 30, ln = v & 0x3FFFFFFFu;
        if (type == 3) {                          /* AE chain: decline */
            free(d);
            return 3;
        }
        if (pos + ln > total) { free(d); return 3; }
        if (type == 0 && ln) {
            int64_t off = udf_abs(u, le32(fe->buf + fe->hdr + fe->ealen + i + 4));
            if (off < 0 || (uint64_t)off + ln > u->img_size ||
                read_at(f, (uint64_t)off, d + pos, ln) != 0) {
                free(d);
                return 3;
            }
        }
        pos += ln;
    }
    if (pos != total) { free(d); return 3; }       /* extent sum != info_len */
    *out = d;
    *out_len = total;
    return 0;
}

/* Every RECORDED extent of a file entry becomes a member; holes stay recipe
 * (their on-disk bytes need not be zeros, so they must be kept verbatim).
 * A file whose data is embedded in its own FE has no extent at all: the FE
 * bytes ride in the recipe, so there is nothing to extract. */
static int udf_file_members(plan_t *p, FILE *f, const udf_t *u, uint32_t lb,
                            const char *sname)
{
    fe_t fe;
    unsigned esz, i;

    if (udf_fe(f, u, lb, &fe) != 0) return 3;
    if (fe.adtype == 3) return 0;
    if (fe.adtype != 0 && fe.adtype != 1) return 3;
    esz = fe.adtype == 0 ? 8u : 16u;
    for (i = 0; i + esz <= fe.adlen; i += esz) {
        const uint8_t *e = fe.buf + fe.hdr + fe.ealen + i;
        uint32_t v = le32(e), type = v >> 30, ln = v & 0x3FFFFFFFu;
        int64_t off;
        if (type == 3) return 3;                 /* AE chain: decline */
        if (type != 0 || !ln) continue;          /* hole: recipe keeps it */
        off = udf_abs(u, le32(e + 4));
        if (off < 0) return 3;
        if (plan_add(p, (uint64_t)off, ln, sname) != 0) return 3;
    }
    return 0;
}

/* Walk one UDF directory: parse its FID chain, recurse into directories,
 * collect file extents. 0 = ok, 3 = decline. */
static int udf_walk(plan_t *p, FILE *f, udf_t *u, uint32_t lb, int depth)
{
    fe_t fe;
    uint8_t *d = NULL;
    uint64_t len = 0, i = 0;

    if (depth > DIR_MAX_DEPTH) return 3;
    if (++u->ndirs > DIR_MAX_COUNT) return 3;
    if (udf_fe(f, u, lb, &fe) != 0) return 3;
    if (udf_dir_data(f, u, &fe, &d, &len) != 0) return 3;
    while (i + 38 <= len) {
        const uint8_t *fid;
        uint8_t ch;
        unsigned lifi;
        uint16_t liu;
        uint64_t tl;
        lad_t icb;
        char sname[SNAME_MAX];

        if (le16(d + i) != 257) {                /* zero padding to a block */
            i = (i / UDF_BLK + 1) * UDF_BLK;
            continue;
        }
        if (!udf_tag_ok(d + i, (size_t)(len - i), 257)) { free(d); return 3; }
        fid = d + i;
        ch = fid[18];
        lifi = fid[19];
        liu = le16(fid + 36);
        udf_lad(fid + 20, &icb);
        tl = 38ull + lifi + liu;
        tl = (tl + 3) & ~3ull;                   /* 4-byte aligned FIDs */
        if (i + tl > len) { free(d); return 3; }
        sname[0] = 0;
        if (lifi) {
            const uint8_t *nm = fid + 38;
            size_t w = 0;
            unsigned k;
            if (nm[0] == 8) {
                for (k = 1; k < lifi && w + 1 < sizeof sname; k++) {
                    unsigned c = nm[k];
                    sname[w++] = (c >= 0x20 && c < 0x7F && c != '/') ?
                                 (char)c : '_';
                }
            } else if (nm[0] == 16) {
                for (k = 1; k + 1 < lifi && w + 1 < sizeof sname; k += 2) {
                    unsigned c = (unsigned)((nm[k] << 8) | nm[k + 1]);
                    sname[w++] = (c >= 0x20 && c < 0x7F && c != '/') ?
                                 (char)c : '_';
                }
            } else {
                free(d);
                return 3;                        /* unknown compression id */
            }
            sname[w] = 0;
        }
        i += tl;
        /* parent (0x08), deleted (0x04) and metadata (0x10) FIDs are not
         * members; a zero-length identifier is the parent entry */
        if (!lifi || (ch & 0x0C) || (ch & 0x10)) continue;
        if (icb.part != u->part_index) { free(d); return 3; }
        if (ch & 0x02) {
            if (udf_walk(p, f, u, icb.lb, depth + 1) != 0) { free(d); return 3; }
        } else if (udf_file_members(p, f, u, icb.lb, sname) != 0) {
            free(d);
            return 3;
        }
    }
    free(d);
    return 0;
}

static int udf_parse(FILE *f, plan_t *p)
{
    udf_t u;
    uint64_t fsd_off = 0;
    fe_t fe;
    lad_t root;
    uint32_t tagloc;

    memset(&u, 0, sizeof u);
    u.img_size = p->img_size;
    if (u.img_size < 257ull * UDF_BLK) return 3;
    if (udf_volume(f, &u, &fsd_off) != 0) return 3;
    if (read_at(f, fsd_off, fe.buf, UDF_BLK) != 0) return 3;
    if (!udf_tag_ok(fe.buf, UDF_BLK, 256)) return 3;      /* File Set Desc */
    tagloc = le32(fe.buf + 12);
    if ((uint64_t)tagloc != fsd_off / UDF_BLK &&
        (uint64_t)tagloc != (uint64_t)u.part_start + fsd_off / UDF_BLK)
        return 3;
    udf_lad(fe.buf + 400, &root);                         /* rootDirectoryICB */
    if (root.part != u.part_index) return 3;
    if (udf_walk(p, f, &u, root.lb, 0) != 0) return 3;
    if (plan_finish(p) != 0) return 3;
    return 0;
}
