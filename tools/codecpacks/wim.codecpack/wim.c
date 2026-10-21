/*
 * wim.c — WIM containerpack helper (InvariantFS WP637, algo 28).
 *
 * Decomposes a Windows Imaging Format file (the `sources/install.wim` of
 * every Windows install ISO, and wimlib's `.esd` solid form) into one
 * member per RESOURCE, stored VERBATIM. Single C11 file, libc only, fully
 * streaming (<= 8 MiB windows): a WIM resource can be gigabytes, so no
 * command ever loads one in RAM.
 *
 * WHY VERBATIM AND NEVER RE-ENCODED. install.wim is entropy-coded (LZX /
 * XPRESS per resource, LZMS in a solid .esd). Microsoft's encoder is
 * closed and wimlib's stream differs ("usually outperforms and
 * outcompresses their closed-source Microsoft equivalents",
 * wimlib.net/compression.html), so a bit-exact rebuild by re-encoding is
 * unachievable. The lane therefore never decodes anything: it copies the
 * compressed bytes and the recipe carries everything else, so bit
 * exactness holds BY CONSTRUCTION, with no encoder in the loop. Same
 * discipline as p7z.c:4-14 (compressed content is kept, never guessed).
 *
 * WHAT THAT BUYS. Position independence. The fixed 64 KiB segment dedupe
 * finds almost nothing across two WIM versions for two compounding
 * reasons: a size change in one file shifts every later resource's offset
 * (breaking offset-aligned matching) and a compressed stream diverges on
 * any content change. Once each resource is its own content-addressed
 * member, an unchanged resource is the same bytes at the same address no
 * matter where it sits in the file, so the engine's dedupe pass shares it
 * across versions.
 *
 * Commands (fixed argv, no shell; exit 0 = ok, 3 = decline, 1 = error,
 * 2 = usage; stdout/stderr are /dev/null for everything except estimate's
 * stdout):
 *
 *     enumerate <in> <out>           member table lines "idx<TAB>sname<TAB>usize"
 *     extract   <in> <idx> <out>     member idx's bytes (exactly usize)
 *     extract_batch <in> <table> <dir>  every member the sliced table names,
 *                                    written to <dir>/<idx> (WP140: one
 *                                    header parse per CALL, not per member --
 *                                    an install.wim has ~10^5 resources and
 *                                    the per-member path is O(n^2))
 *     strip     <in> <out>           the recipe (WIR1, below)
 *     rebuild   <recipe> <dir> <out> original file, bit-exact
 *     map       <in> <out>           the FS-owned MRMP member map (WP16b)
 *     estimate  <in>                 print sum(member usize) + 64 MiB
 *
 * ------------------------------------------------------------------------
 * FORMAT. Offsets and multi-byte fields are little-endian; the layout is
 * MS-WIM (and wimlib's, which mirrors it):
 *
 *   WIM header, 208 bytes at offset 0:
 *     0x00  8  magic "MSWIM\0\0\0"
 *     0x08  u32  cbSize            (208; the header's own length)
 *     0x0c  u32  dwVersion         (0x00010D00 normal, 0x00000E00 solid)
 *     0x10  u32  dwFlags           (0x40 = WRITE_IN_PROGRESS, see refuse)
 *     0x14  u32  dwCompressionSize (per-resource chunk size; 0 or 2^n)
 *     0x18  16   guid
 *     0x28  u16  usPartNumber      (1-based)
 *     0x2a  u16  usTotalParts
 *     0x2c  u32  dwImageCount
 *     0x30  reshdr  rhOffsetTable   \
 *     0x48  reshdr  rhXmlData        |  reshdr = { u64 size:56, u64 flags:8,
 *     0x60  reshdr  rhBootMetadata   |             u64 offset:56 } -- 16
 *     0x78  u32  dwBootIndex         |  bytes, one flag byte inside the
 *     0x7c  reshdr  rhIntegrity     /   first word and one padding byte
 *     0x94  60   reserved               after the offset
 *
 *   The OFFSET TABLE (a.k.a. resource table) is the resource named by
 *   rhOffsetTable; it is an array of 50-byte entries (SHA-1 WIMs) or
 *   62-byte entries (wimlib's SHA-256 extension):
 *     0x00  size_in_wim   56 bits (bytes of THIS resource in the file;
 *                          0 = the resource is not in this file)
 *     0x07  flags          8 bits (0x04 = compressed, 0x02 = metadata,
 *                          0x08 = spanned; advisory to this pack)
 *     0x08  offset_in_wim 56 bits, then one padding byte at 0x0f
 *     0x0f  20/32  hash           (SHA-1 or SHA-256; not used for naming)
 *     ..    u16  part_number
 *     ..    u32  refcnt
 *   A body shorter than a whole number of entries is tolerated exactly
 *   when there are 8 trailing bytes: those carry the compression type
 *   (a wimlib extension), which this pack never needs.
 *
 *   The 56/8/56 shape is not a choice: a real WIM packs `size:56` and
 *   `flags:8` into the same 8 bytes, so reading the size as a full u64
 *   lifts the flags byte into the top of the size (0x06 -> a 4.7e27-byte
 *   table) and a real Microsoft file is DECLINED by name. That is exactly
 *   what happened here: the fixtures were written from the same
 *   misreading, so the pack agreed with them and disagreed with every WIM
 *   ever shipped (found by tools/test-wim.sh's corpus leg on a real
 *   boot.wim, and by reading the header of the extracted file: the table
 *   resource reads size=415800 flags=0x02 off=255270697, while a full-u64
 *   read gives 0x0200000000065838).
 *
 * Every resource the table names becomes ONE member, holding the bytes
 * exactly as they lie in the file — compressed, uncompressed, or (in a
 * solid `.esd`) a slice of a solid block. Nothing is decoded. Resources
 * whose extents OVERLAP are unioned into a single member, which is what
 * turns a solid WIM's aliases of one solid block into "one verbatim
 * member per solid chunk" (no decomposition inside solid compression;
 * still bit-exact, still dedupable at chunk granularity). Adjacent-but-not
 * -overlapping extents stay separate members, so an ordinary LZX/XPRESS
 * WIM gets one member per resource, which is where the cross-version
 * dedupe comes from.
 *
 * Member indexes are 1..n in FILE ORDER (the resource order the encoder
 * wrote is not always offset order; the enumerate table, the recipe and
 * the map all use this one index, so the three can never disagree).
 *
 * ------------------------------------------------------------------------
 * RECIPE FORMAT ("WIR1", pack-owned; the FS never parses it). All integers
 * little-endian. The recipe carries the file size, the member table
 * (idx + extent — the idx<->extent binding, since rebuild gets only
 * "<dir>/<idx>" files and index order is not extent order), and every byte
 * OUTSIDE the member extents, verbatim, as ordered (off,len,bytes) ranges:
 * the 208-byte header, the offset table, the XML data, the integrity
 * table, every gap with whatever arbitrary content it holds, and any
 * trailing junk.
 *
 *   offset  size  field
 *   0       4     "WIR1"
 *   4       8     u64 wim_size
 *   12      4     u32 n_members
 *   16      20*n_members   members, sorted by off:
 *                        u32 idx, u64 off, u64 len        (len > 0)
 *   ..      4     u32 n_ranges
 *   ..      var   ranges, sorted by off, each:
 *                        u64 off, u64 len, u8 bytes[len]  (len > 0)
 *
 * members + ranges together EXACTLY partition [0, wim_size) (rebuild
 * validates this before writing anything; trailing garbage refuses).
 * rebuild: truncate {out} to wim_size, write the recipe ranges verbatim
 * at their offsets, splice each "<dir>/<idx>" at its member extent; a
 * missing/short/long member file or any recipe inconsistency exits 1.
 *
 * MAP (MRMP, FS-owned): one RECIPE entry per recipe range (src_off = the
 * range's payload offset INSIDE the recipe blob, i.e. the WIR1 layout
 * above) and one MEMBER entry per member extent (idx, src_off = 0).
 * Entries sorted by orig_off, partitioning [0, wim_size). strip and map
 * share the layout math, so the RECIPE src_off's always match the blob
 * strip wrote. A WIM's map is what makes reads local: no member read and
 * no rebuild ever execs this helper again.
 *
 * Refuse list (exit 3, "not ours / cannot decompose" — the file then
 * stays RAW and waits, or falls to the generic floor):
 *   no "MSWIM\0\0\0" magic, or a file shorter than the header
 *   cbSize outside [148,4096] or past EOF; unknown/invalid dwVersion
 *   usPartNumber/usTotalParts not 1/1 (a SPLIT WIM carries only part of
 *     the resources, and part 2+ carries no complete view of the set;
 *     decomposing one part of a spanned set is not this lane's job)
 *   dwImageCount outside [1,65535]; dwCompressionSize neither 0 nor 2^n
 *   dwFlags & 0x40 (WRITE_IN_PROGRESS): the file is a torn write
 *   rhBootMetadata present with dwBootIndex outside [1,dwImageCount]
 *   any header resource (offset table / XML / boot metadata / integrity)
 *     whose extent runs past EOF, or an empty offset table
 *   offset table whose body is not a whole number of 50/62-byte entries
 *     (or that, with 8 trailing compression-type bytes), or any entry
 *     whose extent runs past EOF / starts inside the header
 *   zero members (a WIM that names no bytes is not a container)
 *   > 1048575 members (the FS idx bound, CPACK_MAX_MEMBERS)
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


#define WIM_HDR_LEN      208u          /* MS-WIM WIMHEADER, bUnused[60] incl. */
#define WIM_HDR_LEN_MIN  148u          /* through rhIntegrity, no reserved */
#define WIM_HDR_LEN_MAX  4096u         /* sanity cap on a claimed cbSize */
#define WIM_MAGIC        "MSWIM\0\0\0"
#define WIM_MAGIC_LEN    8u
#define COPY_BUF         (8u << 20)    /* 8 MiB streaming window */
#define TABLE_MAX        (64ull << 20) /* offset-table sanity cap */
#define MAX_MEMBERS      1048575u       /* FS idx is 0..1048575; we start at 1 */
#define ESTIMATE_MARGIN  (64ull << 20)  /* estimate = sum(usize) + 64 MiB */
#define HDR_FLAG_WRITE_IN_PROGRESS 0x00000040u
#define RESHDR_FLAG_COMPRESSED 0x04u    /* bit in the reshdr/entry flag byte */

/* The streaming window. Lazily allocated and file-scope so every command
 * shares one 8 MiB buffer: a WIM resource is up to gigabytes, so nothing is
 * ever read whole. Lazily rather than in main() because the .so build
 * (IVPACK_SHARED_LIB) has no main() -- ivpack_impl.h's GLUE_NONE is then
 * honest, and the allocation happens on the first stream_copy instead of
 * being forgotten by a build that never runs main(). */
#define STREAM_BUF (8u << 20)

static uint8_t *stream_buf(void)
{
    static uint8_t *b;

    if (!b) b = (uint8_t *)malloc(STREAM_BUF);
    return b;
}

/* the pack's exit codes (every containerpack agrees, see ivpack_impl.h) */
#define EXIT_OK      0
#define EXIT_ERROR   1
#define EXIT_DECLINE 3

typedef struct {
    uint32_t idx;                       /* member index (the FS contract) */
    uint64_t off;                       /* byte offset in the WIM */
    uint64_t len;                       /* extent bytes (== usize) */
    char     sname[40];                 /* suggested name (advisory) */
} member_t;

typedef struct {
    uint64_t off;
    uint64_t len;
} range_t;

/* one resource-table extent, before the overlap merge */
typedef struct {
    uint64_t off;
    uint64_t len;
    uint8_t  hash[32];
    uint32_t hlen;
} extent_t;

typedef struct {
    uint64_t  wim_size;
    member_t *members;                  /* sorted by off */
    size_t    nmem;
    range_t  *ranges;                   /* sorted by off; member complement */
    size_t    nrng;
} plan_t;

/* ---------------- little-endian + I/O helpers ---------------- */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int wr(FILE *f, const void *b, size_t n)
{
    return fwrite(b, 1, n, f) == n ? 0 : -1;
}

static int wr_le32(FILE *f, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
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
{
    return fread(b, 1, n, f) == n ? 0 : -1;
}

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

static int read_at(FILE *f, uint64_t off, void *buf, size_t n)
{
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    return rd_exact(f, buf, n);
}

/* copy n bytes src -> dst (both already positioned), <= 8 MiB windows */
static int stream_copy(FILE *src, FILE *dst, uint64_t n)
{
    uint8_t *buf = stream_buf();

    if (!buf) return -1;
    while (n) {
        size_t c = n > COPY_BUF ? COPY_BUF : (size_t)n;
        if (fread(buf, 1, c, src) != c) return -1;
        if (fwrite(buf, 1, c, dst) != c) return -1;
        n -= c;
    }
    return 0;
}

static int seek_to(FILE *f, uint64_t off)
{
    return fseeko(f, (off_t)off, SEEK_SET);
}

/* ---------------- header + resource table parse ---------------- */

typedef struct {
    uint32_t version;
    uint32_t flags;
    uint32_t image_count;
    uint32_t boot_index;
    uint32_t n_entries;                 /* offset-table entries */
    uint32_t entry_size;                /* 50 (SHA-1) or 62 (SHA-256) */
    uint64_t hdr_len;                   /* cbSize */
    uint64_t table_off, table_cb;
    uint64_t xml_off, xml_cb;
    uint64_t boot_off, boot_cb;
    uint64_t integ_off, integ_cb;
} whdr_t;

/* A RESHDR is {u64 cb, u64 offset, u64 original_size} (24 bytes). */
static void rd_reshdr(const uint8_t *p, uint64_t *cb, uint64_t *off)
{
    *cb = le64(p) & 0x00FFFFFFFFFFFFFFull;      /* size:56 | flags:8 */
    *off = le64(p + 8) & 0x00FFFFFFFFFFFFFFull; /* offset:56 | pad:8 */
}

/* Parse the 208-byte (or shorter) header. Returns EXIT_OK, EXIT_DECLINE or
 * EXIT_ERROR; a decline means "not a WIM this lane can decompose". */
static int wim_header(FILE *f, uint64_t fsize, whdr_t *h)
{
    uint8_t b[WIM_HDR_LEN];
    uint32_t cb;

    memset(h, 0, sizeof *h);
    if (fsize < WIM_HDR_LEN_MIN) return EXIT_DECLINE;
    if (read_at(f, 0, b, WIM_HDR_LEN) != 0) return EXIT_ERROR;
    if (memcmp(b, WIM_MAGIC, WIM_MAGIC_LEN) != 0) return EXIT_DECLINE;

    cb = le32(b + 0x08);
    if (cb < WIM_HDR_LEN_MIN || cb > WIM_HDR_LEN_MAX || cb > fsize)
        return EXIT_DECLINE;
    h->hdr_len = cb;
    h->version = le32(b + 0x0c);
    h->flags = le32(b + 0x10);
    /* 0x00010D00 is every LZX/XPRESS WIM; 0x00000E00 is the solid/LZMS
     * (.esd) form. Anything else is a format this parser does not know. */
    if (h->version != 0x00010D00u && h->version != 0x00000E00u)
        return EXIT_DECLINE;
    /* WRITE_IN_PROGRESS: the file is a torn write. Same refusal qcow2's
     * "not fully written" check makes; nothing here can be bit-exact. */
    if (h->flags & HDR_FLAG_WRITE_IN_PROGRESS) return EXIT_DECLINE;
    {
        uint32_t compsz = le32(b + 0x14);
        /* 0, or a power of two chunk size (32 KiB .. 32 MiB in practice) */
        if (compsz && (compsz & (compsz - 1))) return EXIT_DECLINE;
    }
    if (le16(b + 0x28) != 1 || le16(b + 0x2a) != 1) return EXIT_DECLINE;
    h->image_count = le32(b + 0x2c);
    if (h->image_count == 0 || h->image_count > 65535u) return EXIT_DECLINE;

    rd_reshdr(b + 0x30, &h->table_cb, &h->table_off);
    rd_reshdr(b + 0x48, &h->xml_cb, &h->xml_off);
    rd_reshdr(b + 0x60, &h->boot_cb, &h->boot_off);
    h->boot_index = le32(b + 0x78);
    rd_reshdr(b + 0x7c, &h->integ_cb, &h->integ_off);

    if (h->boot_cb && (h->boot_index < 1 || h->boot_index > h->image_count))
        return EXIT_DECLINE;
    if (!h->table_cb) return EXIT_DECLINE;          /* no resource table */
    if (h->table_cb > TABLE_MAX) return EXIT_DECLINE;
    if (h->table_off < h->hdr_len ||
        h->table_off > fsize || h->table_cb > fsize - h->table_off)
        return EXIT_DECLINE;
    /* every other header resource must lie inside the file too */
    if ((h->xml_cb && (h->xml_off > fsize ||
                       h->xml_cb > fsize - h->xml_off)) ||
        (h->boot_cb && (h->boot_off > fsize ||
                        h->boot_cb > fsize - h->boot_off)) ||
        (h->integ_cb && (h->integ_off > fsize ||
                         h->integ_cb > fsize - h->integ_off)))
        return EXIT_DECLINE;

    /* entry stride: 50 bytes (SHA-1) or wimlib's 62 (SHA-256); a wimlib
     * table may carry 8 trailing bytes of compression-type metadata, which
     * this lane never needs and therefore only has to tolerate. */
    if (h->table_cb % 50u == 0) {
        h->entry_size = 50;
    } else if (h->table_cb % 62u == 0) {
        h->entry_size = 62;
    } else if (h->table_cb > 8 && (h->table_cb - 8) % 50u == 0) {
        h->entry_size = 50;
    } else if (h->table_cb > 8 && (h->table_cb - 8) % 62u == 0) {
        h->entry_size = 62;
    } else {
        return EXIT_DECLINE;
    }
    h->n_entries = (uint32_t)(h->table_cb / h->entry_size);
    if (!h->n_entries) return EXIT_DECLINE;
    return EXIT_OK;
}

/* qsort comparator: extents by file offset */
static int extent_cmp(const void *a, const void *b)
{
    const extent_t *x = (const extent_t *)a;
    const extent_t *y = (const extent_t *)b;

    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return 0;
}

static void plan_free(plan_t *pl)
{
    free(pl->members);
    free(pl->ranges);
    pl->members = NULL;
    pl->ranges = NULL;
    pl->nmem = pl->nrng = 0;
}

/* Read the offset table and turn it into the plan: members (one per
 * non-overlapping resource extent, in file order, idx 1..n) and ranges (the
 * member complement, i.e. every byte that is not a member). */
static int plan_build(FILE *f, uint64_t fsize, const whdr_t *h, plan_t *pl)
{
    uint8_t *tbl = NULL;
    extent_t *ex = NULL;
    size_t nex = 0, i;
    int rc = EXIT_ERROR;

    memset(pl, 0, sizeof *pl);
    pl->wim_size = fsize;

    tbl = (uint8_t *)malloc((size_t)h->table_cb);
    ex = (extent_t *)malloc(((size_t)h->n_entries + 1) * sizeof *ex);
    if (!tbl || !ex) { rc = EXIT_ERROR; goto out; }
    if (read_at(f, h->table_off, tbl, (size_t)h->table_cb) != 0) {
        rc = EXIT_ERROR;
        goto out;
    }
    for (i = 0; i < h->n_entries; i++) {
        const uint8_t *e = tbl + i * h->entry_size;
        uint64_t len = le64(e) & 0x00FFFFFFFFFFFFFFull;      /* size:56 */
        uint64_t off = le64(e + 8) & 0x00FFFFFFFFFFFFFFull;  /* offset:56 */

        if (!len) continue;                    /* resource not in this file */
        if (off < h->hdr_len || off > fsize || len > fsize - off) {
            rc = EXIT_DECLINE;                 /* extends past EOF */
            goto out;
        }
        ex[nex].off = off;
        ex[nex].len = len;
        ex[nex].hlen = (uint32_t)(h->entry_size - 30);   /* 20 or 32 */
        memcpy(ex[nex].hash, e + 0x0f, ex[nex].hlen);
        nex++;
    }
    if (!nex) { rc = EXIT_DECLINE; goto out; }  /* names no bytes */

    qsort(ex, nex, sizeof *ex, extent_cmp);
    /* union overlapping extents: in a solid WIM several resources alias the
     * same solid block, and a member covering the alias set == one verbatim
     * member per solid chunk (the WP's "no decomposition inside solid
     * compression"). Adjacent-but-disjoint extents stay separate members. */
    {
        size_t n = 0, j = 0;
        while (j < nex) {
            uint64_t off = ex[j].off, end = ex[j].off + ex[j].len;
            size_t group = 1;
            j++;
            while (j < nex && ex[j].off < end) {
                uint64_t e2 = ex[j].off + ex[j].len;
                if (e2 > end) end = e2;
                group++;
                j++;
            }
            ex[n] = ex[j - 1];
            ex[n].off = off;
            ex[n].len = end - off;
            ex[n].hlen = (uint32_t)(group > 1 ? 1 : 0);   /* 1 = unioned */
            n++;
        }
        nex = n;
    }
    if (nex > MAX_MEMBERS) { rc = EXIT_DECLINE; goto out; }

    pl->members = (member_t *)calloc(nex, sizeof *pl->members);
    if (!pl->members) { rc = EXIT_ERROR; goto out; }
    pl->nmem = nex;
    for (i = 0; i < nex; i++) {
        member_t *m = &pl->members[i];
        uint32_t ix = (uint32_t)(i + 1);        /* 1..n, file order */
        m->idx = ix;
        m->off = ex[i].off;
        m->len = ex[i].len;
        /* advisory only; the FS sanitizes and needs no meaning from us */
        if (ex[i].hlen == 1)
            snprintf(m->sname, sizeof m->sname, "solid%04u", ix);
        else if (h->xml_cb && ex[i].off == h->xml_off &&
                 ex[i].len == h->xml_cb)
            snprintf(m->sname, sizeof m->sname, "xmldata");
        else if (h->boot_cb && ex[i].off == h->boot_off &&
                 ex[i].len == h->boot_cb)
            snprintf(m->sname, sizeof m->sname, "bootmeta");
        else if (h->integ_cb && ex[i].off == h->integ_off &&
                 ex[i].len == h->integ_cb)
            snprintf(m->sname, sizeof m->sname, "integrity");
        else
            snprintf(m->sname, sizeof m->sname, "res%04u", ix);
    }

    /* ranges = the member complement; members + ranges must partition the
     * file exactly, which is what makes rebuild bit-exact by construction */
    pl->ranges = (range_t *)calloc(nex + 1, sizeof *pl->ranges);
    if (!pl->ranges) { rc = EXIT_ERROR; goto out; }
    {
        uint64_t pos = 0;
        size_t n = 0;
        for (i = 0; i < nex; i++) {
            if (pl->members[i].off > pos) {
                pl->ranges[n].off = pos;
                pl->ranges[n].len = pl->members[i].off - pos;
                n++;
            }
            pos = pl->members[i].off + pl->members[i].len;
        }
        if (pos < fsize) {
            pl->ranges[n].off = pos;
            pl->ranges[n].len = fsize - pos;
            n++;
        }
        if (pos > fsize) { rc = EXIT_DECLINE; goto out; }
        pl->nrng = n;
    }
    return EXIT_OK;
out:
    free(tbl);
    free(ex);
    if (rc != EXIT_OK) plan_free(pl);
    return rc;
}

/* Open + validate. Returns EXIT_OK (plan filled), EXIT_DECLINE or
 * EXIT_ERROR; *f_out is left open for the streaming commands. */
static int analyze(const char *path, plan_t *pl, FILE **f_out)
{
    whdr_t h;
    FILE *f;
    uint64_t fsize;
    int rc;

    memset(pl, 0, sizeof *pl);
    f = fopen(path, "rb");
    if (!f) return EXIT_ERROR;
    if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return EXIT_ERROR; }
    {
        off_t s = ftello(f);
        if (s < 0) { fclose(f); return EXIT_ERROR; }
        fsize = (uint64_t)s;
    }
    rc = wim_header(f, fsize, &h);
    if (rc == EXIT_OK) rc = plan_build(f, fsize, &h, pl);
    if (rc != EXIT_OK) { fclose(f); return rc; }
    if (f_out) *f_out = f; else fclose(f);
    return EXIT_OK;
}

/* ---------------- commands ---------------- */

static int cmd_enumerate(const char *in, const char *out)
{
    plan_t pl;
    FILE *o;
    size_t i;
    int rc = analyze(in, &pl, NULL);

    if (rc != EXIT_OK) return rc;
    o = fopen(out, "w");
    if (!o) { plan_free(&pl); return EXIT_ERROR; }
    for (i = 0; i < pl.nmem; i++)
        fprintf(o, "%u\t%s\t%" PRIu64 "\n", pl.members[i].idx,
                pl.members[i].sname, pl.members[i].len);
    rc = fclose(o) == 0 ? EXIT_OK : EXIT_ERROR;
    plan_free(&pl);
    return rc;
}

/* The member whose idx the FS asked for, or NULL. An idx the plan does not
 * announce is a HARD error (the FS only asks announced ones). */
static const member_t *find_member(const plan_t *pl, unsigned long idx)
{
    size_t i;

    for (i = 0; i < pl->nmem; i++)
        if (pl->members[i].idx == (uint32_t)idx) return &pl->members[i];
    return NULL;
}

static int cmd_extract(const char *in, const char *idx_s, const char *out)
{
    plan_t pl;
    FILE *f = NULL, *fo = NULL;
    const member_t *m;
    char *end = NULL;
    unsigned long idx;
    int rc;

    if (!idx_s) return EXIT_ERROR;
    idx = strtoul(idx_s, &end, 10);
    if (!end || *end || end == idx_s) return EXIT_ERROR;
    rc = analyze(in, &pl, &f);
    if (rc != EXIT_OK) return rc;
    m = find_member(&pl, idx);
    if (!m) { fclose(f); plan_free(&pl); return EXIT_ERROR; }
    fo = fopen(out, "wb");
    if (!fo) { fclose(f); plan_free(&pl); return EXIT_ERROR; }
    if (seek_to(f, m->off) != 0 || stream_copy(f, fo, m->len) != 0 ||
        fclose(fo) != 0) {
        fclose(f);
        plan_free(&pl);
        return EXIT_ERROR;
    }
    fclose(f);
    plan_free(&pl);
    return EXIT_OK;
}

#ifndef IVPACK_SHARED_LIB
/* WP140: ONE header parse for the whole member set. An install.wim carries
 * ~10^5 resources; the per-member `extract` path re-parses the offset table
 * (and re-reads it from disk) on every call, which is O(n^2) and measured in
 * days for a real install.wim. This command parses once and writes
 * "<dir>/<idx>" for every member the enumerate table at <tbl> announces.
 *
 * The set comes from the TABLE, not from an index range, so it is exactly
 * what the FS asked for. BIT-EXACTNESS is unchanged and unchanged by
 * construction: the same stream_copy over the same (off,len) extent that
 * cmd_extract does -- nothing is decoded, re-encoded or checksummed
 * differently here.
 *
 * CLI-only on purpose: the ADR-007 plugin ABI has five container commands
 * and the FS falls back to one exec per member for a pack without a `batch`
 * line, so nothing depends on this existing.
 */
static int cmd_extract_batch(const char *in, const char *tbl, const char *dir)
{
    plan_t pl;
    FILE *f = NULL, *ft = NULL, *fo = NULL;
    char *text = NULL;
    long tlen;
    uint8_t *seen = NULL;
    size_t pos = 0;
    int rc;

    rc = analyze(in, &pl, &f);
    if (rc != EXIT_OK) return rc;
    rc = EXIT_ERROR;

    ft = fopen(tbl, "rb");
    if (!ft) goto out;
    if (fseek(ft, 0, SEEK_END) != 0) goto out;
    tlen = ftell(ft);
    if (tlen < 0) goto out;
    if (fseek(ft, 0, SEEK_SET) != 0) goto out;
    text = (char *)malloc((size_t)tlen + 1);
    if (!text) goto out;
    if (tlen && fread(text, 1, (size_t)tlen, ft) != (size_t)tlen) goto out;
    text[tlen] = 0;
    fclose(ft);
    ft = NULL;

    seen = (uint8_t *)calloc(pl.nmem + 1, 1);
    if (!seen) goto out;

    while (pos < (size_t)tlen) {
        char *line = text + pos;
        char *nl = strchr(line, '\n');
        char *endp = NULL;
        unsigned long idx;
        unsigned long long usize;
        const member_t *m;
        char path[4096];
        int n;

        if (nl) { *nl = 0; pos = (size_t)(nl - text) + 1; }
        else pos = (size_t)tlen;
        if (!*line) continue;                   /* tolerate a blank line */

        /* "idx<TAB>sname<TAB>usize" — exactly what enumerate wrote */
        idx = strtoul(line, &endp, 10);
        if (!endp || endp == line || *endp != '\t') goto out;
        line = endp + 1;
        endp = strchr(line, '\t');
        if (!endp) goto out;
        *endp = 0;
        usize = strtoull(endp + 1, &endp, 10);
        if (!endp || endp[0] != 0) goto out;
        m = find_member(&pl, idx);
        if (!m) goto out;                       /* not a member of this WIM */
        if (seen[m->idx]) goto out;             /* the FS table never repeats */
        seen[m->idx] = 1;
        if (m->len != (uint64_t)usize) goto out;

        n = snprintf(path, sizeof path, "%s/%lu", dir, idx);
        if (n <= 0 || (size_t)n >= sizeof path) goto out;
        fo = fopen(path, "wb");
        if (!fo) goto out;
        if (seek_to(f, m->off) != 0 ||
            stream_copy(f, fo, m->len) != 0) {
            fclose(fo);
            fo = NULL;
            goto out;
        }
        if (fclose(fo) != 0) { fo = NULL; goto out; }
        fo = NULL;
    }
    rc = EXIT_OK;
out:
    if (fo) fclose(fo);
    if (ft) fclose(ft);
    if (f) fclose(f);
    free(text);
    free(seen);
    plan_free(&pl);
    return rc;
}
#endif /* !IVPACK_SHARED_LIB: the plugin ABI has no batch command */

/* the WIR1 recipe header size for n members (shared by strip and map, so
 * the two can never disagree about where a range's payload starts) */
static uint64_t recipe_hdr_size(size_t n_mem)
{
    return 4 + 8 + 4 + (uint64_t)n_mem * 20 + 4;
}

static int cmd_strip(const char *in, const char *out)
{
    plan_t pl;
    FILE *f = NULL, *fo = NULL;
    size_t i;
    int rc;

    rc = analyze(in, &pl, &f);
    if (rc != EXIT_OK) return rc;
    fo = fopen(out, "wb");
    if (!fo) { fclose(f); plan_free(&pl); return EXIT_ERROR; }
    if (wr(fo, "WIR1", 4) != 0 ||
        wr_le64(fo, pl.wim_size) != 0 ||
        wr_le32(fo, (uint32_t)pl.nmem) != 0)
        goto fail;
    for (i = 0; i < pl.nmem; i++) {
        if (wr_le32(fo, pl.members[i].idx) != 0 ||
            wr_le64(fo, pl.members[i].off) != 0 ||
            wr_le64(fo, pl.members[i].len) != 0)
            goto fail;
    }
    if (wr_le32(fo, (uint32_t)pl.nrng) != 0) goto fail;
    /* every byte outside the member extents, verbatim: the header, the
     * offset table, gaps (junk included), trailing junk */
    for (i = 0; i < pl.nrng; i++) {
        if (wr_le64(fo, pl.ranges[i].off) != 0 ||
            wr_le64(fo, pl.ranges[i].len) != 0) goto fail;
        if (seek_to(f, pl.ranges[i].off) != 0 ||
            stream_copy(f, fo, pl.ranges[i].len) != 0) goto fail;
    }
    rc = fclose(fo) == 0 ? EXIT_OK : EXIT_ERROR;
    fclose(f);
    plan_free(&pl);
    return rc;
fail:
    fclose(fo);
    fclose(f);
    plan_free(&pl);
    return EXIT_ERROR;
}

/* rebuild: splice the WIR1 ranges + the member files back into the original
 * WIM. Parses NO WIM at all (the recipe is self-contained), so it is
 * bit-exact by construction: every byte comes from either the recipe (as it
 * was) or a member file (as it was). No encoder, no decoder, no guess. */
static int cmd_rebuild(const char *recipe, const char *dir, const char *out)
{
    FILE *fr = NULL, *fo = NULL, *fm = NULL;
    member_t *mem = NULL;
    range_t *rng = NULL;
    uint64_t *rpos = NULL;                  /* payload offset per range */
    uint64_t size = 0, rsize = 0, payload = 0, ranges_start = 0;
    uint32_t nmem = 0, nrng = 0, i;
    int rc = EXIT_ERROR;

    fr = fopen(recipe, "rb");
    if (!fr) return EXIT_ERROR;
    if (fseeko(fr, 0, SEEK_END) != 0) goto out;
    {
        off_t s = ftello(fr);
        if (s < 0) goto out;
        rsize = (uint64_t)s;
    }
    if (seek_to(fr, 0) != 0) goto out;
    {
        uint8_t magic[4];
        if (rd_exact(fr, magic, 4) != 0 || memcmp(magic, "WIR1", 4) != 0)
            goto out;
    }
    if (rd_le64f(fr, &size) != 0 || !size) goto out;
    if (rd_le32f(fr, &nmem) != 0 || !nmem || nmem > MAX_MEMBERS) goto out;
    mem = (member_t *)calloc(nmem, sizeof *mem);
    if (!mem) goto out;
    {
        uint64_t pos = 0;
        for (i = 0; i < nmem; i++) {
            if (rd_le32f(fr, &mem[i].idx) != 0 ||
                rd_le64f(fr, &mem[i].off) != 0 ||
                rd_le64f(fr, &mem[i].len) != 0)
                goto out;
            if (!mem[i].idx || mem[i].idx > nmem) goto out;
            if (!mem[i].len || mem[i].len > size ||
                mem[i].off > size - mem[i].len)
                goto out;
            if (mem[i].off < pos) goto out;      /* sorted, disjoint */
            pos = mem[i].off + mem[i].len;
        }
    }
    if (rd_le32f(fr, &nrng) != 0 || nrng > nmem + 1) goto out;
    rng = (range_t *)calloc(nrng ? nrng : 1, sizeof *rng);
    rpos = (uint64_t *)calloc(nrng ? nrng : 1, sizeof *rpos);
    if (!rng || !rpos) goto out;
    {
        off_t s = ftello(fr);
        if (s < 0) goto out;
        ranges_start = (uint64_t)s;
    }
    /* pass 1: validate the WHOLE recipe before writing a single byte */
    payload = ranges_start;
    for (i = 0; i < nrng; i++) {
        if (rd_le64f(fr, &rng[i].off) != 0 || rd_le64f(fr, &rng[i].len) != 0)
            goto out;
        if (!rng[i].len || rng[i].len > size ||
            rng[i].off > size - rng[i].len)
            goto out;
        if (i && rng[i].off < rng[i - 1].off + rng[i - 1].len) goto out;
        rpos[i] = payload + 16;
        payload += 16 + rng[i].len;
        if (payload > rsize) goto out;            /* truncated recipe */
        if (fseeko(fr, (off_t)rng[i].len, SEEK_CUR) != 0) goto out;
    }
    if (payload != rsize) goto out;               /* trailing garbage */
    /* members and ranges must EXACTLY partition [0, size) */
    {
        uint32_t mi = 0;
        uint64_t pos = 0;
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
    /* pass 2: write */
    fo = fopen(out, "wb");
    if (!fo) goto out;
    if (ftruncate(fileno(fo), (off_t)size) != 0) goto out;
    for (i = 0; i < nrng; i++) {
        if (seek_to(fr, rpos[i]) != 0 ||
            seek_to(fo, rng[i].off) != 0 ||
            stream_copy(fr, fo, rng[i].len) != 0)
            goto out;
    }
    for (i = 0; i < nmem; i++) {
        char mp[4096];
        int n = snprintf(mp, sizeof mp, "%s/%u", dir, mem[i].idx);
        off_t msz;

        if (n <= 0 || (size_t)n >= sizeof mp) goto out;
        fm = fopen(mp, "rb");
        if (!fm) goto out;
        if (fseeko(fm, 0, SEEK_END) != 0) goto out;
        msz = ftello(fm);
        if (msz < 0 || (uint64_t)msz != mem[i].len) goto out;
        if (seek_to(fm, 0) != 0 || seek_to(fo, mem[i].off) != 0 ||
            stream_copy(fm, fo, mem[i].len) != 0)
            goto out;
        if (fclose(fm) != 0) { fm = NULL; goto out; }
        fm = NULL;
    }
    if (fflush(fo) != 0 || ftruncate(fileno(fo), (off_t)size) != 0) goto out;
    rc = fclose(fo) == 0 ? EXIT_OK : EXIT_ERROR;
    fo = NULL;
out:
    if (fm) fclose(fm);
    if (fo) fclose(fo);
    if (fr) fclose(fr);
    free(mem);
    free(rng);
    free(rpos);
    return rc;
}

/* map: the FS-owned MRMP blob. One RECIPE entry per recipe range (src_off =
 * where that range's payload sits inside the WIR1 blob strip wrote) and one
 * MEMBER entry per member extent (idx, src_off 0), merged in original-offset
 * order so the entries partition [0, wim_size). A WIM's map is what makes
 * reads local: no member read and no rebuild ever execs this helper again. */
static int cmd_map(const char *in, const char *out)
{
    plan_t pl;
    FILE *o = NULL;
    uint64_t src;
    size_t mi = 0, ri = 0;
    int rc;

    rc = analyze(in, &pl, NULL);
    if (rc != EXIT_OK) return rc;
    o = fopen(out, "wb");
    if (!o) { plan_free(&pl); return EXIT_ERROR; }
    src = recipe_hdr_size(pl.nmem);
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
            kind = 0;                              /* RECIPE */
            idx = 0;
            src_off = src + 16;                    /* past the range header */
            src += 16 + len;
            ri++;
        } else {
            orig_off = pl.members[mi].off;
            len = pl.members[mi].len;
            kind = 1;                              /* MEMBER */
            idx = pl.members[mi].idx;
            src_off = 0;
            mi++;
        }
        if (wr_le64(o, orig_off) != 0 || wr_le64(o, len) != 0 ||
            wr(o, &kind, 1) != 0 || wr_le32(o, idx) != 0 ||
            wr_le64(o, src_off) != 0)
            goto fail;
    }
    rc = fclose(o) == 0 ? EXIT_OK : EXIT_ERROR;
    plan_free(&pl);
    return rc;
fail:
    fclose(o);
    plan_free(&pl);
    return EXIT_ERROR;
}

static int cmd_estimate(const char *in)
{
    plan_t pl;
    uint64_t sum = 0;
    size_t i;
    int rc;

    rc = analyze(in, &pl, NULL);
    if (rc != EXIT_OK) return rc;
    for (i = 0; i < pl.nmem; i++) {
        if (UINT64_MAX - sum < pl.members[i].len) {
            plan_free(&pl);
            return EXIT_DECLINE;
        }
        sum += pl.members[i].len;
    }
    if (UINT64_MAX - sum < ESTIMATE_MARGIN) {
        plan_free(&pl);
        return EXIT_DECLINE;
    }
    printf("%" PRIu64 "\n", (uint64_t)(sum + ESTIMATE_MARGIN));
    plan_free(&pl);
    return EXIT_OK;
}

/* ---- ivpack plugin C ABI export (ADR-007) --------------------------------
 * Built as libwim.so with -DIVPACK_SHARED_LIB -fPIC -shared; the fork guard
 * in ivpack_impl.h keeps the CLI's exit(3)=decline / exit(1)=error contract
 * intact inside a long-lived worker. IVPACK_GLUE_NONE() is accurate here
 * because this pack keeps no per-call global state: the streaming window is
 * allocated lazily on first use (stream_buf()), not by main(). */
static const ivpack_desc s_wim_desc = {
    IVPACK_API_VERSION, "wim", "1.0.0", "containerpack", 0
};

const ivpack_desc *ivpack_get_desc(void) { return &s_wim_desc; }

IVPACK_CANON_CALLS(wim)

IVPACK_DEFINE_CONTAINER_CMD(wim, IVPACK_GLUE_NONE())

IVPACK_DEFINE_CONTAINER_ESTIMATE(wim, IVPACK_GLUE_NONE(),
                             rc = (int)cmd_estimate(a);)

#ifndef IVPACK_SHARED_LIB
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (strcmp(argv[1], "enumerate") == 0 && argc == 4)
        return cmd_enumerate(argv[2], argv[3]);
    if (strcmp(argv[1], "extract") == 0 && argc == 5)
        return cmd_extract(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "extract_batch") == 0 && argc == 5)
        return cmd_extract_batch(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "strip") == 0 && argc == 4)
        return cmd_strip(argv[2], argv[3]);
    if (strcmp(argv[1], "rebuild") == 0 && argc == 5)
        return cmd_rebuild(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "map") == 0 && argc == 4)
        return cmd_map(argv[2], argv[3]);
    if (strcmp(argv[1], "estimate") == 0 && argc == 3)
        return cmd_estimate(argv[2]);
    return 2;
}
#endif /* !IVPACK_SHARED_LIB */
