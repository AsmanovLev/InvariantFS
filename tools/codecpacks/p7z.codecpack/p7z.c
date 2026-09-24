/*
 * p7z.c — p7z containerpack helper (InvariantFS WP16a/WP16b).
 *
 * Decomposes 7z archives whose file payloads are all STORED (7z method
 * "Copy", coder id {00}) into member inodes. This is the ONLY 7z shape the
 * pack touches, and the reason is bit-exactness: a Copy member's bytes sit
 * verbatim in the archive, so the recipe (strip) is "everything except the
 * member payloads" and rebuild is a splice — no recompression anywhere, so
 * the FS sweep GUARD (rebuild memcmp) and the WP16b map guard hold
 * byte-exactly. A compressed (LZMA/LZMA2/PPMd/BZip2/Deflate/...), filtered
 * (BCJ+x), or encrypted (7zAES) member could only be rebuilt by
 * re-encoding, which is NOT bit-exact across encoder versions/settings, or
 * by storing the compressed bytes in the recipe, which stores the data
 * twice (worse than generic ZSTD-19 on the whole archive). Both are
 * dishonest "decomposition"; the pack DECLINES (exit 3) instead. See the
 * refuse list at the bottom.
 *
 * The 7z header (metadata: names, times, attributes, CRCs, folder layout)
 * is NEVER regenerated — it rides verbatim inside the recipe. The header is
 * parsed only to LOCATE member payload extents. The format is documented in
 * tools/7-Zip-zstd/DOC/7zFormat.txt and cross-checked against the reference
 * reader (tools/7-Zip-zstd/CPP/7zip/Archive/7z/7zIn.cpp).
 *
 * Header wrinkle: 7zz compresses the header itself by default
 * (kEncodedHeader 0x17): the tail blob is a HeaderInfo describing a small
 * LZMA stream (the packed header). The 7z LZMA coder properties are exactly
 * the LZMA-Alone 5-byte properties, so the helper fabricates an LZMA-Alone
 * stream (props + u64 unpack size + packed bytes) into a temp file and
 * decodes it with the real 7zz (`7zz x -tlzma -so`), fork/execvp with fixed
 * argv, no shell. The decoded blob is then parsed as a raw header. 7zz is
 * resolved from $P7Z_7ZZ, then a sibling of this binary named "7zz", then
 * PATH. 7zz is ONLY needed for this header decode;
 * enumerate/extract/strip/map/estimate are otherwise pure C, and rebuild
 * never needs 7zz (the recipe is self-contained).
 *
 * Commands (fixed argv, no shell; exit 0 = ok, 3 = decline, 1 = error,
 * 2 = usage; stdout/stderr are /dev/null except estimate's stdout):
 *
 *     enumerate <in> <out>           member table lines "idx<TAB>sname<TAB>usize"
 *     extract   <in> <idx> <out>     member idx's raw bytes (exactly usize)
 *     strip     <in> <out>           the recipe (P7R1, below)
 *     rebuild   <recipe> <dir> <out> original archive, bit-exact
 *     map       <in> <out>           the FS-owned MRMP member map (WP16b)
 *     estimate  <in>                 print size + sum(usize) + 64 MiB
 *
 * Member indexes = the archive's file order (0..n-1, dense). Every
 * FilesInfo entry is a member: directories and empty files are 0-length
 * members (their metadata lives in the verbatim header inside the recipe).
 * A member with data occupies exactly one extent: its own Copy folder, or a
 * substream of a multi-file Copy folder (a "solid" stored archive, where
 * the folder stream is just the concatenation of its files).
 *
 * ------------------------------------------------------------------------
 * RECIPE FORMAT ("P7R1", pack-owned; the FS never parses it). All integers
 * little-endian. Same shape as rawdisk's RDR1: the archive size, the data
 * member table (idx + extent), and every byte OUTSIDE the member extents
 * verbatim as ordered (off,len,bytes) ranges — the signature header, the
 * (packed) header, any gaps, and any trailing junk.
 *
 *   offset  size  field
 *   0       4     "P7R1"
 *   4       8     u64 archive_size
 *   12      4     u32 n_data_members
 *   16      20*n_data_members   members, sorted by off:
 *                               u32 idx, u64 off, u64 len        (len > 0)
 *   ..      4     u32 n_ranges
 *   ..      var   ranges, sorted by off, each:
 *                               u64 off, u64 len, u8 bytes[len]  (len > 0)
 *
 * members + ranges together EXACTLY partition [0, archive_size) (rebuild
 * validates this before writing anything; trailing garbage refuses).
 * rebuild: truncate {out} to archive_size, write the recipe ranges verbatim
 * at their offsets, splice each "<dir>/<idx>" at its member extent; a
 * missing/short/long member file or any recipe inconsistency exits 1.
 *
 * MAP (MRMP, FS-owned): one RECIPE entry per recipe range (src_off = the
 * range's payload offset INSIDE the P7R1 blob) and one MEMBER entry per
 * data member (idx, src_off = 0). Zero-length members have no entries.
 * strip and map share the layout math, so the RECIPE src_off's always match
 * the blob strip wrote.
 *
 * Refuse list (exit 3, "not ours / cannot decompose bit-exactly"):
 *   not a 7z signature, truncated, bad header CRCs, header past EOF,
 *     version major != 0
 *   encoded header that is not exactly 1 folder / 1 plain LZMA coder /
 *     5-byte props (LZMA2/BCJ-wrapped/AES-encrypted headers, -mhe=on), or
 *     7zz unavailable/failing to decode it
 *   any data folder with != 1 coder, or a coder id != {00} (Copy):
 *     LZMA 03 01 01, LZMA2 21, PPMd 03 04 01, BZip2, Deflate, 7zAES
 *     06 F1 07 01, BCJ filters, ... — this single rule covers every
 *     compressed/filtered/encrypted member, solid or not
 *   complex coders (bind pairs), multi-packed-stream folders, pack size !=
 *     unpack size for a Copy folder (not stored 1:1), 0-substream folders
 *     with payload
 *   anti files (differential update artifacts), kStartPos (multivolume),
 *     external name/folder streams, any structural inconsistency
 *   > 65536 files (FS member bound), 0 files (the FS refuses empty tables)
 * extract on an unannounced idx and all rebuild inconsistencies are hard
 * errors (exit 1), not declines. Bit-rot INSIDE member payloads is NOT
 * detected and not refused: the rotten bytes are extracted and spliced back
 * verbatim, so a bit-rotten archive still round-trips byte-exactly (the
 * archive-internal CRC mismatch is preserved, not healed — the same policy
 * rawdisk applies to partition contents).
 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
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


#define COPY_BUF_SZ   (8u << 20)        /* 8 MiB streaming window */
#define MAX_FILES     65536u            /* FS member bound (table rows) */
#define CPACK_MAX_IDX_PLUS1 65536u      /* recipe member record cap */
#define MAX_HEADER    (256ull << 20)    /* header blob sanity cap */
#define MAX_SUBSTREAMS (1u << 21)       /* >> FS bound; alloc guard */
#define ESTIMATE_MARGIN (64ull << 20)   /* estimate slack */

static uint8_t *g_buf;                  /* the streaming window */

/* ---------------- CRC32 (zlib polynomial, as 7z uses) ---------------- */

static uint32_t g_crc_tab[256];
static int g_crc_ready = 0;

static void crc32_init(void)
{
    uint32_t i, j, c;
    if (g_crc_ready) return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        g_crc_tab[i] = c;
    }
    g_crc_ready = 1;
}

static uint32_t crc32_calc(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    crc32_init();
    for (i = 0; i < n; i++)
        c = g_crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* ---------------- little-endian + file I/O helpers ---------------- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
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
    put_le64(b, v);
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

static int seek_to(FILE *f, uint64_t off)
{
    return fseeko(f, (off_t)off, SEEK_SET);
}

static int read_at(FILE *f, uint64_t off, void *buf, size_t n)
{
    if (seek_to(f, off) != 0) return -1;
    return rd_exact(f, buf, n);
}

/* copy n bytes src -> dst (both already positioned), <= 8 MiB windows */
static int stream_copy(FILE *src, FILE *dst, uint64_t n)
{
    while (n) {
        size_t c = n > COPY_BUF_SZ ? COPY_BUF_SZ : (size_t)n;
        if (fread(g_buf, 1, c, src) != c) return -1;
        if (fwrite(g_buf, 1, c, dst) != c) return -1;
        n -= c;
    }
    return 0;
}

/* ---------------- header cursor (bounded buffer parse) ---------------- */

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
} cur_t;

static int cur_byte(cur_t *c, uint8_t *v)
{
    if (c->pos >= c->len) return -1;
    *v = c->p[c->pos++];
    return 0;
}

/* 7z variable-length UINT64 (7zFormat.txt "Notes about Notation"):
 * k leading 1 bits in the first byte, then k extra bytes LITTLE-ENDIAN,
 * and the remaining low bits of the first byte as the high part. */
static int cur_vu(cur_t *c, uint64_t *v)
{
    uint8_t b;
    unsigned k, i;
    uint64_t val = 0;
    if (cur_byte(c, &b) != 0) return -1;
    if (!(b & 0x80)) { *v = b; return 0; }
    for (k = 0; k < 8 && (b & (0x80u >> k)); k++) ;
    /* k = number of leading 1 bits, 1..8 */
    if (k >= 8) {
        if (c->len - c->pos < 8) return -1;
        *v = le64(c->p + c->pos);
        c->pos += 8;
        return 0;
    }
    if (c->len - c->pos < k) return -1;
    for (i = 0; i < k; i++)
        val |= (uint64_t)c->p[c->pos++] << (8 * i);
    *v = val | ((uint64_t)(b & (uint8_t)(0x7Fu >> k)) << (8 * k));
    return 0;
}

static int cur_skip(cur_t *c, uint64_t n)
{
    if (n > c->len - c->pos) return -1;
    c->pos += (size_t)n;
    return 0;
}

/* 7z bool vector: MSB-first packed bits, num items */
static int cur_bv(cur_t *c, uint8_t *bits, uint64_t num)
{
    uint64_t i;
    uint8_t b = 0;
    for (i = 0; i < num; i++) {
        if ((i & 7) == 0 && cur_byte(c, &b) != 0) return -1;
        bits[i] = (uint8_t)((b >> (7 - (i & 7))) & 1);
    }
    return 0;
}

/* Digests(num): AllAreDefined byte / bitmask + u32 per defined. */
static int skip_digests(cur_t *c, uint64_t num)
{
    uint8_t all;
    uint64_t cnt = num, i;
    if (cur_byte(c, &all) != 0) return -1;
    if (!all) {
        uint64_t nbytes = (num + 7) / 8;
        if (c->len - c->pos < nbytes) return -1;
        cnt = 0;
        for (i = 0; i < num; i++)
            if (c->p[c->pos + (i >> 3)] & (0x80u >> (i & 7))) cnt++;
        c->pos += (size_t)nbytes;
    }
    return cur_skip(c, cnt * 4);
}

static int read_digests(cur_t *c, uint64_t num, uint8_t *defs, uint32_t *vals)
{
    uint8_t all;
    uint64_t i;
    if (cur_byte(c, &all) != 0) return -1;
    if (all) {
        for (i = 0; i < num; i++) defs[i] = 1;
    } else if (cur_bv(c, defs, num) != 0) {
        return -1;
    }
    for (i = 0; i < num; i++) {
        if (!defs[i]) { vals[i] = 0; continue; }
        if (c->len - c->pos < 4) return -1;
        vals[i] = le32(c->p + c->pos);
        c->pos += 4;
    }
    return 0;
}

/* ---------------- StreamsInfo parse ---------------- */

typedef struct {
    uint64_t num_coders;
    uint64_t in_total, out_total;
    uint8_t  id0[8];         /* coder 0 id bytes */
    uint8_t  id0_len;
    uint8_t  complex0, attrs0;
    uint8_t  props0[16];     /* coder 0 properties (capped) */
    uint8_t  props0_len;
} fcoder_t;

typedef struct {
    int      has_pack;       /* PackInfo seen */
    uint64_t pack_pos;       /* relative to byte 32 of the archive */
    uint64_t num_pack_streams;
    uint64_t *pack_sizes;
    int      has_unpack;     /* UnPackInfo seen */
    uint64_t num_folders;
    fcoder_t *fcoders;       /* per folder */
    uint64_t *folder_unpack; /* per folder (first out stream size) */
    uint8_t  *folder_crc_def;
    uint32_t *folder_crc;
    uint64_t *num_unpack;    /* per folder substream count (default 1) */
    uint64_t *sub_sizes;     /* resolved per-substream unpack sizes */
    uint64_t total_subs;
} sinfo_t;

static void sinfo_free(sinfo_t *s)
{
    free(s->pack_sizes);
    free(s->fcoders);
    free(s->folder_unpack);
    free(s->folder_crc_def);
    free(s->folder_crc);
    free(s->num_unpack);
    free(s->sub_sizes);
    memset(s, 0, sizeof *s);
}

/* one folder's coder chain; structural parse only (policy lives higher) */
static int parse_folder(cur_t *c, fcoder_t *fc)
{
    uint64_t ci;
    memset(fc, 0, sizeof *fc);
    if (cur_vu(c, &fc->num_coders) != 0) return -1;
    if (fc->num_coders == 0 || fc->num_coders > 64) return -1;
    for (ci = 0; ci < fc->num_coders; ci++) {
        uint8_t mainb, idlen, complexf, attrs;
        uint64_t id = 0, nin = 1, nout = 1, psz = 0, j;
        if (cur_byte(c, &mainb) != 0) return -1;
        if (mainb & 0xC0) return -1;        /* reserved/alt bits: refuse */
        idlen = mainb & 0x0F;
        complexf = (mainb & 0x10) != 0;
        attrs = (mainb & 0x20) != 0;
        if (idlen > 8) return -1;
        if (c->len - c->pos < idlen) return -1;
        for (j = 0; j < idlen; j++) id = (id << 8) | c->p[c->pos++];
        if (ci == 0) {
            fc->id0_len = idlen;
            memset(fc->id0, 0, sizeof fc->id0);
            if (idlen)
                memcpy(fc->id0, c->p + c->pos - idlen, idlen);
            fc->complex0 = complexf;
            fc->attrs0 = attrs;
        }
        if (complexf) {
            if (cur_vu(c, &nin) != 0 || cur_vu(c, &nout) != 0) return -1;
            if (nin == 0 || nout == 0 || nin > 64 || nout > 64) return -1;
        }
        if (attrs) {
            if (cur_vu(c, &psz) != 0 || psz > (1u << 20)) return -1;
            if (c->len - c->pos < psz) return -1;
            if (ci == 0) {
                fc->props0_len = (uint8_t)(psz > 16 ? 16 : psz);
                memset(fc->props0, 0, sizeof fc->props0);
                if (psz)
                    memcpy(fc->props0, c->p + c->pos, fc->props0_len);
            }
            c->pos += (size_t)psz;
        }
        fc->in_total += nin;
        fc->out_total += nout;
    }
    /* bind pairs: out_total - 1, then the multi-packed-stream index list */
    if (fc->out_total == 0) return -1;
    {
        uint64_t nbind = fc->out_total - 1, i;
        for (i = 0; i < nbind; i++) {
            uint64_t a, b;
            if (cur_vu(c, &a) != 0 || cur_vu(c, &b) != 0) return -1;
        }
        if (fc->in_total < nbind) return -1;
        if (fc->in_total - nbind > 1) {
            uint64_t n = fc->in_total - nbind;
            for (i = 0; i < n; i++) {
                uint64_t t;
                if (cur_vu(c, &t) != 0) return -1;
            }
        }
    }
    return 0;
}

/* StreamsInfo body; 0 ok, -1 malformed. Always collects into *si. */
static int parse_streams_info(cur_t *c, sinfo_t *si)
{
    uint64_t id, i, f;

    memset(si, 0, sizeof *si);
    for (;;) {
        if (cur_vu(c, &id) != 0) return -1;
        if (id == 0x00) return 0;                       /* kEnd */
        if (id == 0x06) {                               /* kPackInfo */
            if (si->has_pack) return -1;
            si->has_pack = 1;
            if (cur_vu(c, &si->pack_pos) != 0) return -1;
            if (cur_vu(c, &si->num_pack_streams) != 0) return -1;
            if (si->num_pack_streams > MAX_SUBSTREAMS) return -1;
            si->pack_sizes = (uint64_t *)calloc(si->num_pack_streams + 1,
                                                sizeof(uint64_t));
            if (!si->pack_sizes) return -1;
            for (;;) {
                if (cur_vu(c, &id) != 0) return -1;
                if (id == 0x00) break;
                if (id == 0x09) {                       /* kSize */
                    uint64_t sum = 0;
                    for (i = 0; i < si->num_pack_streams; i++) {
                        if (cur_vu(c, &si->pack_sizes[i]) != 0) return -1;
                        sum += si->pack_sizes[i];
                        if (sum < si->pack_sizes[i]) return -1;
                    }
                    continue;
                }
                if (id == 0x0A) {                       /* kCRC */
                    if (skip_digests(c, si->num_pack_streams) != 0)
                        return -1;
                    continue;
                }
                {
                    uint64_t sz;                        /* unknown: skip */
                    if (cur_vu(c, &sz) != 0 || cur_skip(c, sz) != 0)
                        return -1;
                }
            }
        } else if (id == 0x07) {                        /* kUnPackInfo */
            uint8_t ext8;
            if (si->has_unpack) return -1;
            si->has_unpack = 1;
            if (cur_vu(c, &id) != 0 || id != 0x0B) return -1;   /* kFolder */
            if (cur_vu(c, &si->num_folders) != 0) return -1;
            if (si->num_folders > MAX_SUBSTREAMS) return -1;
            if (cur_byte(c, &ext8) != 0) return -1;             /* External */
            if (ext8 != 0) return -1;                   /* external folders */
            si->fcoders = (fcoder_t *)calloc(si->num_folders + 1,
                                             sizeof(fcoder_t));
            si->folder_unpack = (uint64_t *)calloc(si->num_folders + 1,
                                                   sizeof(uint64_t));
            si->num_unpack = (uint64_t *)calloc(si->num_folders + 1,
                                                sizeof(uint64_t));
            si->folder_crc_def = (uint8_t *)calloc(si->num_folders + 1, 1);
            si->folder_crc = (uint32_t *)calloc(si->num_folders + 1, 4);
            if (!si->fcoders || !si->folder_unpack || !si->num_unpack ||
                !si->folder_crc_def || !si->folder_crc)
                return -1;
            for (f = 0; f < si->num_folders; f++) {
                if (parse_folder(c, &si->fcoders[f]) != 0) return -1;
                si->num_unpack[f] = 1;                  /* default */
            }
            /* kCodersUnPackSize (mandatory per reference reader) */
            if (cur_vu(c, &id) != 0 || id != 0x0C) return -1;
            for (f = 0; f < si->num_folders; f++) {
                uint64_t o;
                for (o = 0; o < si->fcoders[f].out_total; o++) {
                    uint64_t v;
                    if (cur_vu(c, &v) != 0) return -1;
                    if (o == 0) si->folder_unpack[f] = v;
                }
            }
            for (;;) {
                if (cur_vu(c, &id) != 0) return -1;
                if (id == 0x00) break;
                if (id == 0x0A) {                       /* folder CRCs */
                    if (read_digests(c, si->num_folders,
                                     si->folder_crc_def,
                                     si->folder_crc) != 0)
                        return -1;
                    continue;
                }
                {
                    uint64_t sz;
                    if (cur_vu(c, &sz) != 0 || cur_skip(c, sz) != 0)
                        return -1;
                }
            }
        } else if (id == 0x08) {                        /* kSubStreamsInfo */
            int have_counts = 0, have_sizes = 0;
            if (!si->has_unpack) return -1;
            for (;;) {
                if (cur_vu(c, &id) != 0) return -1;
                if (id == 0x00) break;
                if (id == 0x0D) {                       /* kNumUnpackStream */
                    uint64_t total = 0;
                    if (have_counts || have_sizes) return -1;
                    have_counts = 1;
                    for (f = 0; f < si->num_folders; f++) {
                        uint64_t v;
                        if (cur_vu(c, &v) != 0) return -1;
                        if (v > MAX_SUBSTREAMS) return -1;
                        si->num_unpack[f] = v;
                        total += v;
                        if (total > MAX_SUBSTREAMS) return -1;
                    }
                    continue;
                }
                if (id == 0x09) {                       /* kSize */
                    uint64_t spos = 0, total = 0;
                    if (have_sizes) return -1;
                    have_sizes = 1;
                    for (f = 0; f < si->num_folders; f++)
                        total += si->num_unpack[f];
                    if (total > MAX_SUBSTREAMS) return -1;
                    si->total_subs = total;
                    si->sub_sizes = (uint64_t *)calloc(total + 1,
                                                       sizeof(uint64_t));
                    if (!si->sub_sizes) return -1;
                    for (f = 0; f < si->num_folders; f++) {
                        uint64_t nu = si->num_unpack[f], j, sum = 0;
                        if (!nu) continue;
                        for (j = 0; j + 1 < nu; j++) {
                            uint64_t v;
                            if (cur_vu(c, &v) != 0) return -1;
                            sum += v;
                            if (sum < v) return -1;
                            si->sub_sizes[spos++] = v;
                        }
                        if (sum > si->folder_unpack[f]) return -1;
                        si->sub_sizes[spos++] = si->folder_unpack[f] - sum;
                    }
                    continue;
                }
                if (id == 0x0A) {                       /* substream CRCs */
                    uint64_t ndig = 0;
                    for (f = 0; f < si->num_folders; f++) {
                        uint64_t nu = si->num_unpack[f];
                        if (!(nu == 1 && si->folder_crc_def[f])) ndig += nu;
                    }
                    if (skip_digests(c, ndig) != 0) return -1;
                    continue;
                }
                {
                    uint64_t sz;
                    if (cur_vu(c, &sz) != 0 || cur_skip(c, sz) != 0)
                        return -1;
                }
            }
            if (!have_sizes) {
                /* no kSize: every folder must have exactly 1 substream */
                uint64_t spos = 0;
                si->sub_sizes = (uint64_t *)calloc(si->num_folders + 1,
                                                   sizeof(uint64_t));
                if (!si->sub_sizes) return -1;
                for (f = 0; f < si->num_folders; f++) {
                    if (si->num_unpack[f] != 1) return -1;
                    si->sub_sizes[spos++] = si->folder_unpack[f];
                }
                si->total_subs = spos;
            }
        } else {
            /* unknown StreamsInfo id: size-prefixed skip */
            uint64_t sz;
            if (cur_vu(c, &sz) != 0 || cur_skip(c, sz) != 0) return -1;
        }
    }
    return 0;
}

/* ---------------- FilesInfo parse ---------------- */

typedef struct {
    uint64_t num_files;
    uint8_t *empty_stream;      /* per file bit */
    int      has_empty_stream;
    int      has_anti;          /* any anti bit set -> caller declines */
    uint8_t *names;             /* kName blob (UTF-16LE, NUL-separated) */
    size_t   names_len;
} files_t;

static void files_free(files_t *fi)
{
    free(fi->empty_stream);
    free(fi->names);
    memset(fi, 0, sizeof *fi);
}

static int parse_files_info(cur_t *c, files_t *fi)
{
    uint64_t id, num_empty = 0;

    memset(fi, 0, sizeof *fi);
    if (cur_vu(c, &fi->num_files) != 0) return -1;
    if (fi->num_files == 0 || fi->num_files > MAX_FILES) return -1;
    for (;;) {
        uint64_t size;
        cur_t sub;
        if (cur_vu(c, &id) != 0) return -1;
        if (id == 0x00) return 0;                       /* kEnd */
        if (cur_vu(c, &size) != 0) return -1;
        if (size > c->len - c->pos) return -1;
        sub.p = c->p + c->pos;
        sub.len = (size_t)size;
        sub.pos = 0;
        if (id == 0x0E) {                               /* kEmptyStream */
            if (fi->has_empty_stream) return -1;
            fi->has_empty_stream = 1;
            fi->empty_stream = (uint8_t *)calloc(fi->num_files + 1, 1);
            if (!fi->empty_stream) return -1;
            if (cur_bv(&sub, fi->empty_stream, fi->num_files) != 0)
                return -1;
            {
                uint64_t i;
                for (i = 0; i < fi->num_files; i++)
                    num_empty += fi->empty_stream[i];
            }
        } else if (id == 0x0F) {                        /* kEmptyFile */
            if (cur_skip(&sub, size) != 0) return -1;   /* dirs vs empty */
        } else if (id == 0x10) {                        /* kAnti */
            uint8_t *bits;
            uint64_t i;
            if (!fi->has_empty_stream) return -1;
            bits = (uint8_t *)calloc(num_empty + 1, 1);
            if (!bits) return -1;
            if (cur_bv(&sub, bits, num_empty) != 0) { free(bits); return -1; }
            for (i = 0; i < num_empty; i++)
                if (bits[i]) fi->has_anti = 1;
            free(bits);
        } else if (id == 0x11) {                        /* kName */
            uint8_t ext;
            if (fi->names) return -1;
            if (cur_byte(&sub, &ext) != 0) return -1;
            if (ext != 0) return -1;                    /* external names */
            fi->names_len = sub.len - sub.pos;
            fi->names = (uint8_t *)malloc(fi->names_len + 2);
            if (!fi->names) return -1;
            memcpy(fi->names, sub.p + sub.pos, fi->names_len);
            fi->names[fi->names_len] = 0;
            fi->names[fi->names_len + 1] = 0;
            if (cur_skip(&sub, fi->names_len) != 0) return -1;
        } else if (id == 0x18) {                        /* kStartPos */
            return -1;                                  /* multivolume */
        } else {
            if (cur_skip(&sub, size) != 0) return -1;   /* times/attrs/... */
        }
        if (sub.pos != sub.len) return -1;              /* strict consume */
        if (cur_skip(c, size) != 0) return -1;
    }
    return 0;
}

/* the UTF-16LE name of file i -> ASCII basename sname (advisory) */
static void make_sname(const files_t *fi, uint64_t i, char *out, size_t cap)
{
    size_t pos = 0, start = 0, end = 0, p;
    uint64_t idx;
    char tmp[128];
    size_t w = 0;

    out[0] = 0;
    if (!fi->names) goto fallback;
    /* walk to the i-th NUL-terminated UTF-16LE string */
    for (idx = 0; idx <= i; idx++) {
        size_t s = pos;
        for (;;) {
            unsigned u;
            if (pos + 1 >= fi->names_len + 2) goto fallback;  /* unterminated */
            u = fi->names[pos] | ((unsigned)fi->names[pos + 1] << 8);
            pos += 2;
            if (pos > fi->names_len) goto fallback;
            if (!u) break;
        }
        if (idx == i) { start = s; end = pos - 2; break; }
    }
    /* basename: after the last '/' or '\\' */
    {
        size_t b = start, q;
        for (q = start; q + 1 < end + 2; q += 2) {
            unsigned u = fi->names[q] | ((unsigned)fi->names[q + 1] << 8);
            if (u == '/' || u == '\\') b = q + 2;
        }
        for (p = b; p < end && w + 1 < sizeof tmp; p += 2) {
            unsigned u = fi->names[p] | ((unsigned)fi->names[p + 1] << 8);
            char ch = (u < 128) ? (char)u : '_';
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                  ch == '-'))
                ch = '_';
            tmp[w++] = ch;
        }
    }
    while (w && tmp[w - 1] == '.') w--;             /* avoid trailing dots */
    if (w > 24) w = 24;                             /* FS sname budget */
    tmp[w] = 0;
    if (w && strcmp(tmp, ".") && strcmp(tmp, "..")) {
        size_t z;
        for (z = 0; z < w && z + 1 < cap; z++) out[z] = tmp[z];
        out[z] = 0;
        return;
    }
fallback:
    snprintf(out, cap, "mbr%llu", (unsigned long long)i);
}

/* ---------------- 7zz LZMA header decode ---------------- */

/* resolve the 7zz binary: $P7Z_7ZZ, then a sibling of this binary named
 * "7zz" (via /proc/self/exe), then PATH (matching the manifest comment) */
static const char *resolve_7zz(char *buf, size_t cap, const char *argv0)
{
    const char *env = getenv("P7Z_7ZZ");
    if (env && *env) return env;
    if (argv0) {
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0 && (size_t)n < sizeof exe - 1) {
            char *slash;
            exe[n] = 0;
            slash = strrchr(exe, '/');
            if (slash) {
                int m;
                *slash = 0;
                m = snprintf(buf, cap, "%s/7zz", exe);
                if (m > 0 && (size_t)m < cap && access(buf, X_OK) == 0)
                    return buf;
            }
        }
    }
    (void)argv0;
    return "7zz";
}

/*
 * Decode the packed header stream through 7zz's LZMA-Alone reader:
 * fabricate props(5) + le64(usize) + packed into a temp file, exec
 * `7zz x -tlzma -so <tmp>`, capture stdout. Exactly usize bytes with a
 * zero exit status or it did not happen. Returns 0 ok, 3 decline.
 */
static int lzma_decode_7zz(const char *argv0, FILE *in,
                           uint64_t pack_off, uint64_t pack_size,
                           const uint8_t props[5], uint64_t usize,
                           uint8_t **out_buf)
{
    char tmp[4096];
    char bin[4096];
    const char *path;
    int fd = -1, pfd[2];
    uint8_t hdr[13];
    uint8_t *buf = NULL, *packed = NULL;
    size_t got = 0;
    pid_t pid;
    int status, rc = 3;

    if (usize == 0 || usize > MAX_HEADER) return 3;
    if (pack_size == 0 || pack_size > MAX_HEADER) return 3;
    packed = (uint8_t *)malloc((size_t)pack_size);
    if (!packed) return 1;
    if (read_at(in, pack_off, packed, (size_t)pack_size) != 0) {
        free(packed);
        return 3;
    }
    /* temp file next to nothing in particular: TMPDIR, else /tmp */
    {
        const char *td = getenv("TMPDIR");
        int n;
        if (!td || !*td) td = "/tmp";
        n = snprintf(tmp, sizeof tmp, "%s/.p7zhdrXXXXXX", td);
        if (n <= 0 || (size_t)n >= sizeof tmp) { free(packed); return 1; }
    }
    fd = mkstemp(tmp);
    if (fd < 0) {
        int n = snprintf(tmp, sizeof tmp, "/tmp/.p7zhdrXXXXXX");
        if (n <= 0 || (size_t)n >= sizeof tmp) { free(packed); return 1; }
        fd = mkstemp(tmp);
        if (fd < 0) { free(packed); return 1; }
    }
    memcpy(hdr, props, 5);
    put_le64(hdr + 5, usize);
    if (write(fd, hdr, 13) != 13 ||
        write(fd, packed, (size_t)pack_size) != (ssize_t)pack_size) {
        close(fd);
        unlink(tmp);
        free(packed);
        return 1;
    }
    close(fd);
    free(packed);

    if (pipe(pfd) != 0) { unlink(tmp); return 1; }
    path = resolve_7zz(bin, sizeof bin, argv0);
    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        unlink(tmp);
        return 1;
    }
    if (pid == 0) {
        int devnull;
        close(pfd[0]);
        if (dup2(pfd[1], STDOUT_FILENO) < 0) _exit(127);
        close(pfd[1]);
        devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        execlp(path, path, "x", "-tlzma", "-so", tmp, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    buf = (uint8_t *)malloc((size_t)usize + 1);
    if (!buf) {
        close(pfd[0]);
        unlink(tmp);
        (void)waitpid(pid, &status, 0);
        return 1;
    }
    for (;;) {
        ssize_t r = read(pfd[0], buf + got, (size_t)usize + 1 - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        got += (size_t)r;
        if (got > (size_t)usize) break;             /* too much: refuse */
    }
    close(pfd[0]);
    unlink(tmp);
    if (waitpid(pid, &status, 0) < 0) { free(buf); return 1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { free(buf); return 3; }
    if (got != (size_t)usize) { free(buf); return 3; }
    *out_buf = buf;
    rc = 0;
    return rc;
}

/* ---------------- plan (shared analysis) ---------------- */

typedef struct {
    uint32_t idx;                       /* member index (== file order) */
    uint64_t usize;                     /* announced member size */
    uint64_t off;                       /* extent offset (valid iff usize) */
    char     sname[40];                 /* suggested name (advisory) */
} pmember_t;

typedef struct {
    uint64_t off;
    uint64_t len;
} prange_t;

typedef struct {
    uint64_t   archive_size;
    pmember_t *members;                 /* ALL FilesInfo entries */
    size_t     nmem;
    prange_t  *ranges;                  /* complement of the data extents */
    size_t     nrng;
} plan_t;

static void plan_free(plan_t *p)
{
    free(p->members);
    free(p->ranges);
    memset(p, 0, sizeof *p);
}

/* a folder is "plain stored": exactly one coder, id {00} Copy, not
 * complex, no attributes (Copy has none), 1 in / 1 out, 1 packed stream */
static int folder_is_copy(const fcoder_t *fc)
{
    return fc->num_coders == 1 && !fc->complex0 && !fc->attrs0 &&
           fc->id0_len == 1 && fc->id0[0] == 0x00 &&
           fc->in_total == 1 && fc->out_total == 1;
}

/* a folder is "plain LZMA": exactly one coder, id {03 01 01}, 5 props */
static int folder_is_lzma1(const fcoder_t *fc)
{
    return fc->num_coders == 1 && !fc->complex0 && fc->attrs0 &&
           fc->id0_len == 3 && fc->id0[0] == 0x03 && fc->id0[1] == 0x01 &&
           fc->id0[2] == 0x01 && fc->props0_len == 5 && fc->in_total == 1 &&
           fc->out_total == 1;
}

/*
 * Parse the archive into a plan: the member table (idx/usize/extent) and
 * the complement ranges. 0 = ok, 3 = decline, 1 = hard error.
 */
static int analyze(const char *argv0, const char *path, plan_t *pl)
{
    FILE *f = NULL;
    uint8_t sh[32];
    uint8_t *hblob = NULL, *decoded = NULL;
    uint64_t size, nho, nhs, hdr_off;
    uint32_t shc, nhc;
    sinfo_t si;
    files_t fi;
    int rc = 3;
    cur_t c;
    uint64_t id;

    memset(pl, 0, sizeof *pl);
    memset(&si, 0, sizeof si);
    memset(&fi, 0, sizeof fi);

    f = fopen(path, "rb");
    if (!f) return 1;
    if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    {
        off_t sz = ftello(f);
        if (sz < 0) { fclose(f); return 1; }
        size = (uint64_t)sz;
    }
    pl->archive_size = size;
    if (size < 32 || size > (1ull << 48)) { fclose(f); return 3; }
    if (read_at(f, 0, sh, 32) != 0) { fclose(f); return 3; }
    if (memcmp(sh, "7z\xBC\xAF\x27\x1C", 6) != 0) { fclose(f); return 3; }
    if (sh[6] != 0) { fclose(f); return 3; }        /* major version */
    shc = le32(sh + 8);
    if (crc32_calc(sh + 12, 20) != shc) { fclose(f); return 3; }
    nho = le64(sh + 12);
    nhs = le64(sh + 20);
    nhc = le32(sh + 28);
    if (nhs == 0 || nhs > MAX_HEADER) { fclose(f); return 3; }
    if (nho > size - 32 || nhs > size - 32 - nho) { fclose(f); return 3; }
    hdr_off = 32 + nho;
    hblob = (uint8_t *)malloc((size_t)nhs);
    if (!hblob) { fclose(f); return 1; }
    if (read_at(f, hdr_off, hblob, (size_t)nhs) != 0) goto out;
    if (crc32_calc(hblob, (size_t)nhs) != nhc) goto out;

    if (hblob[0] == 0x17) {                         /* kEncodedHeader */
        /* HeaderInfo: kEncodedHeader StreamsInfo kEnd — the StreamsInfo
         * must describe exactly one folder with one plain LZMA coder. */
        cur_t hc;
        sinfo_t hi;
        uint64_t pid, data_base, pack_size;
        memset(&hi, 0, sizeof hi);
        hc.p = hblob;
        hc.len = (size_t)nhs;
        hc.pos = 1;
        if (parse_streams_info(&hc, &hi) != 0) { sinfo_free(&hi); goto out; }
        /* tolerate a trailing kEnd */
        if (hc.pos < hc.len) {
            if (cur_vu(&hc, &pid) != 0 || pid != 0) { sinfo_free(&hi); goto out; }
        }
        if (hc.pos != hc.len || !hi.has_pack || !hi.has_unpack ||
            hi.num_folders != 1 || hi.num_pack_streams != 1 ||
            !folder_is_lzma1(&hi.fcoders[0])) {
            sinfo_free(&hi);
            goto out;
        }
        data_base = 32 + hi.pack_pos;
        if (hi.pack_pos > size - 32 ||
            hi.pack_sizes[0] > size - 32 - hi.pack_pos) {
            sinfo_free(&hi);
            goto out;
        }
        pack_size = hi.pack_sizes[0];
        /* sanity: LZMA props byte < 225, dictionary <= 256 MiB */
        if (hi.fcoders[0].props0[0] >= 225 ||
            le32(hi.fcoders[0].props0 + 1) > (256u << 20)) {
            sinfo_free(&hi);
            goto out;
        }
        rc = lzma_decode_7zz(argv0, f, data_base, pack_size,
                             hi.fcoders[0].props0, hi.folder_unpack[0],
                             &decoded);
        if (rc != 0) { sinfo_free(&hi); goto out; }
        /* verify the decoded header CRC when the header folder has one */
        if (hi.folder_crc_def[0] &&
            crc32_calc(decoded, (size_t)hi.folder_unpack[0]) !=
            hi.folder_crc[0]) {
            sinfo_free(&hi);
            goto out;
        }
        free(hblob);
        hblob = decoded;
        decoded = NULL;
        nhs = hi.folder_unpack[0];
        sinfo_free(&hi);
        rc = 3;
    }
    if (hblob[0] != 0x01) goto out;                 /* kHeader */

    c.p = hblob;
    c.len = (size_t)nhs;
    c.pos = 1;
    for (;;) {
        if (cur_vu(&c, &id) != 0) goto out;
        if (id == 0x00) break;                      /* kEnd */
        if (id == 0x02) {                           /* kArchiveProperties */
            for (;;) {
                uint64_t sz;
                if (cur_vu(&c, &id) != 0) goto out;
                if (id == 0x00) break;
                if (cur_vu(&c, &sz) != 0 || cur_skip(&c, sz) != 0) goto out;
            }
        } else if (id == 0x03) {                    /* kAdditionalStreamsInfo */
            sinfo_t ai;
            if (parse_streams_info(&c, &ai) != 0) { sinfo_free(&ai); goto out; }
            sinfo_free(&ai);
        } else if (id == 0x04) {                    /* kMainStreamsInfo */
            if (si.has_unpack || si.has_pack) goto out;
            if (parse_streams_info(&c, &si) != 0) goto out;
        } else if (id == 0x05) {                    /* kFilesInfo */
            if (fi.num_files) goto out;
            if (parse_files_info(&c, &fi) != 0) goto out;
        } else {
            goto out;                               /* unknown top-level */
        }
    }
    if (c.pos != c.len) goto out;                   /* trailing garbage */
    if (fi.num_files == 0) goto out;                /* no FilesInfo */
    if (fi.has_anti) goto out;                      /* differential junk */

    /* ---- build the plan (the strict support matrix) ---- */
    pl->members = (pmember_t *)calloc(fi.num_files, sizeof(pmember_t));
    if (!pl->members) { rc = 1; goto out; }
    pl->nmem = (size_t)fi.num_files;

    if (si.num_folders) {
        uint64_t *foff = NULL;
        uint64_t total_subs = 0, want_files = 0, data_base;
        uint64_t spos = 0, fidx = 0, within = 0, in_folder = 0, fk;

        /* every folder: plain Copy, pack == unpack, 1 pack stream each */
        if (!si.has_pack || !si.has_unpack) goto out;
        if (si.num_pack_streams != si.num_folders) goto out;
        if (!si.sub_sizes) goto out;
        for (fk = 0; fk < si.num_folders; fk++) {
            if (!folder_is_copy(&si.fcoders[fk])) goto out;
            if (si.pack_sizes[fk] != si.folder_unpack[fk]) goto out;
            if (si.num_unpack[fk] == 0 && si.folder_unpack[fk] != 0)
                goto out;
            total_subs += si.num_unpack[fk];
        }
        /* pack_pos + all pack bytes must lie inside the file */
        if (si.pack_pos > size - 32) goto out;
        data_base = 32 + si.pack_pos;
        {
            uint64_t t = 0;
            for (fk = 0; fk < si.num_folders; fk++) {
                t += si.pack_sizes[fk];
                if (t < si.pack_sizes[fk]) goto out;
            }
            if (t > size - data_base) goto out;
        }
        foff = (uint64_t *)calloc(si.num_folders, sizeof(uint64_t));
        if (!foff) { rc = 1; goto out; }
        {
            uint64_t acc = data_base;
            for (fk = 0; fk < si.num_folders; fk++) {
                foff[fk] = acc;
                acc += si.pack_sizes[fk];
            }
        }
        /* count non-empty files; must equal total substreams */
        for (fk = 0; fk < fi.num_files; fk++)
            if (!fi.has_empty_stream || !fi.empty_stream[fk]) want_files++;
        if (want_files != total_subs) { free(foff); goto out; }
        /* FillLinks (7zIn.cpp): non-empty files consume substreams in
         * folder order, skipping 0-stream folders */
        for (fk = 0; fk < fi.num_files; fk++) {
            pmember_t *m = &pl->members[fk];
            m->idx = (uint32_t)fk;
            make_sname(&fi, fk, m->sname, sizeof m->sname);
            if (fi.has_empty_stream && fi.empty_stream[fk]) {
                m->usize = 0;
                m->off = 0;
                continue;
            }
            while (fidx < si.num_folders && si.num_unpack[fidx] == 0) {
                fidx++;
                within = 0;
                in_folder = 0;
            }
            if (fidx >= si.num_folders) { free(foff); goto out; }
            m->usize = si.sub_sizes[spos];
            if (m->usize > si.folder_unpack[fidx] - within) {
                free(foff);
                goto out;
            }
            m->off = foff[fidx] + within;
            if (m->usize && (m->off >= size || m->usize > size - m->off)) {
                free(foff);
                goto out;
            }
            within += m->usize;
            spos++;
            if (++in_folder >= si.num_unpack[fidx]) {  /* folder exhausted */
                if (within != si.folder_unpack[fidx]) {
                    free(foff);
                    goto out;
                }
                fidx++;
                within = 0;
                in_folder = 0;
            }
        }
        /* leftover folders must be 0-stream (else: unmapped substreams) */
        while (fidx < si.num_folders) {
            if (si.num_unpack[fidx] != 0) { free(foff); goto out; }
            fidx++;
        }
        if (spos != total_subs) { free(foff); goto out; }
        free(foff);
    } else {
        /* no folders: every file must be empty */
        uint64_t i2;
        for (i2 = 0; i2 < fi.num_files; i2++) {
            pmember_t *m = &pl->members[i2];
            m->idx = (uint32_t)i2;
            make_sname(&fi, i2, m->sname, sizeof m->sname);
            if (!fi.has_empty_stream || !fi.empty_stream[i2]) goto out;
            m->usize = 0;
            m->off = 0;
        }
    }

    /* data extents are produced in ascending order by construction
     * (folders in order, substreams in order); verify, then complement */
    {
        uint64_t pos = 0;
        size_t i2, n = 0;
        pl->ranges = (prange_t *)malloc((pl->nmem + 1) * sizeof(prange_t));
        if (!pl->ranges) { rc = 1; goto out; }
        for (i2 = 0; i2 < pl->nmem; i2++) {
            pmember_t *m = &pl->members[i2];
            if (!m->usize) continue;
            if (m->off < pos) goto out;             /* overlap/disorder */
            if (m->off > pos) {
                pl->ranges[n].off = pos;
                pl->ranges[n].len = m->off - pos;
                n++;
            }
            pos = m->off + m->usize;
        }
        if (pos < size) {
            pl->ranges[n].off = pos;
            pl->ranges[n].len = size - pos;
            n++;
        }
        pl->nrng = n;
    }
    rc = 0;
out:
    if (rc != 0) plan_free(pl);
    sinfo_free(&si);
    files_free(&fi);
    free(hblob);
    free(decoded);
    fclose(f);
    return rc;
}

/* ---------------- commands ---------------- */

static int cmd_enumerate(const char *argv0, const char *in, const char *out)
{
    plan_t pl;
    FILE *o;
    size_t i;
    int rc = analyze(argv0, in, &pl);

    if (rc != 0) return rc;
    o = fopen(out, "w");
    if (!o) { plan_free(&pl); return 1; }
    for (i = 0; i < pl.nmem; i++)
        fprintf(o, "%u\t%s\t%" PRIu64 "\n", pl.members[i].idx,
                pl.members[i].sname, pl.members[i].usize);
    rc = fclose(o) == 0 ? 0 : 1;
    plan_free(&pl);
    return rc;
}

static int cmd_extract(const char *argv0, const char *in, const char *idx_s,
                       const char *out)
{
    plan_t pl;
    FILE *fi = NULL, *fo = NULL;
    const pmember_t *m = NULL;
    char *end = NULL;
    unsigned long idx;
    size_t i;
    int rc = 1;

    idx = strtoul(idx_s, &end, 10);
    if (!end || *end) return 1;
    rc = analyze(argv0, in, &pl);
    if (rc != 0) return rc;
    for (i = 0; i < pl.nmem; i++)
        if (pl.members[i].idx == (uint32_t)idx) { m = &pl.members[i]; break; }
    if (!m) { plan_free(&pl); return 1; }   /* the FS only asks announced */
    fo = fopen(out, "wb");
    if (!fo) { plan_free(&pl); return 1; }
    if (m->usize == 0) {
        rc = fclose(fo) == 0 ? 0 : 1;
        plan_free(&pl);
        return rc;
    }
    fi = fopen(in, "rb");
    if (fi && seek_to(fi, m->off) == 0 &&
        stream_copy(fi, fo, m->usize) == 0 && fclose(fo) == 0)
        rc = 0;
    else
        rc = 1;
    if (fi) fclose(fi);
    plan_free(&pl);
    return rc;
}

/* the recipe header size for n data members (shared by strip and map) */
static uint64_t recipe_hdr_size(size_t n_data)
{
    return 4 + 8 + 4 + (uint64_t)n_data * 20 + 4;
}

static int cmd_strip(const char *argv0, const char *in, const char *out)
{
    plan_t pl;
    FILE *fi = NULL, *fo = NULL;
    size_t i, ndata = 0;
    int rc = analyze(argv0, in, &pl);

    if (rc != 0) return rc;
    for (i = 0; i < pl.nmem; i++)
        if (pl.members[i].usize) ndata++;
    fi = fopen(in, "rb");
    fo = fopen(out, "wb");
    if (!fi || !fo) goto fail;
    if (wr(fo, "P7R1", 4) != 0 ||
        wr_le64(fo, pl.archive_size) != 0 ||
        wr_le32(fo, (uint32_t)ndata) != 0)
        goto fail;
    for (i = 0; i < pl.nmem; i++) {
        if (!pl.members[i].usize) continue;
        if (wr_le32(fo, pl.members[i].idx) != 0 ||
            wr_le64(fo, pl.members[i].off) != 0 ||
            wr_le64(fo, pl.members[i].usize) != 0)
            goto fail;
    }
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

/* P7R1 parse state (rebuild) */
typedef struct {
    uint32_t idx;
    uint64_t off, len;
} rmember_t;

typedef struct {
    uint64_t off, len, payload_pos;     /* payload_pos: inside the recipe */
} rrange_t;

static int cmd_rebuild(const char *recipe, const char *dir, const char *out)
{
    FILE *fr = NULL, *fo = NULL, *fm = NULL;
    rmember_t *mem = NULL;
    rrange_t *rng = NULL;
    uint64_t size, rsize, pos, ranges_start, payload;
    uint32_t nmem, nrng, i;
    int rc = 1;

    fr = fopen(recipe, "rb");
    if (!fr) return 1;
    if (fseeko(fr, 0, SEEK_END) != 0) goto out;
    {
        off_t rs = ftello(fr);
        if (rs < 0) goto out;
        rsize = (uint64_t)rs;
    }
    if (seek_to(fr, 0) != 0) goto out;
    {
        uint8_t magic[4];
        if (rd_exact(fr, magic, 4) != 0 || memcmp(magic, "P7R1", 4) != 0)
            goto out;
    }
    if (rd_le64f(fr, &size) != 0 || !size || size > (1ull << 48)) goto out;
    if (rd_le32f(fr, &nmem) != 0) goto out;
    if (nmem > CPACK_MAX_IDX_PLUS1) goto out;
    mem = (rmember_t *)malloc((nmem ? nmem : 1) * sizeof *mem);
    if (!mem) goto out;
    pos = 0;
    for (i = 0; i < nmem; i++) {
        if (rd_le32f(fr, &mem[i].idx) != 0 ||
            rd_le64f(fr, &mem[i].off) != 0 ||
            rd_le64f(fr, &mem[i].len) != 0)
            goto out;
        if (!mem[i].len || mem[i].len > size || mem[i].off > size - mem[i].len)
            goto out;
        if (mem[i].off < pos) goto out;         /* sorted, non-overlapping */
        pos = mem[i].off + mem[i].len;
    }
    if (rd_le32f(fr, &nrng) != 0 || nrng > nmem + 1) goto out;
    rng = (rrange_t *)malloc((nrng ? nrng : 1) * sizeof *rng);
    if (!rng) goto out;
    {
        off_t rs = ftello(fr);
        if (rs < 0) goto out;
        ranges_start = (uint64_t)rs;
    }
    /* pass 1: validate the whole recipe BEFORE writing anything */
    payload = ranges_start;
    for (i = 0; i < nrng; i++) {
        if (rd_le64f(fr, &rng[i].off) != 0 || rd_le64f(fr, &rng[i].len) != 0)
            goto out;
        if (!rng[i].len || rng[i].len > size || rng[i].off > size - rng[i].len)
            goto out;
        rng[i].payload_pos = payload + 16;
        payload += 16 + rng[i].len;
        if (payload > rsize) goto out;          /* truncated recipe */
        if (fseeko(fr, (off_t)rng[i].len, SEEK_CUR) != 0) goto out;
    }
    if (payload != rsize) goto out;             /* trailing garbage */
    /* members + ranges must exactly partition [0, size) */
    {
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
    /* pass 2: write — truncate to size, recipe ranges verbatim */
    fo = fopen(out, "wb");
    if (!fo) goto out;
    if (ftruncate(fileno(fo), (off_t)size) != 0) goto out;
    for (i = 0; i < nrng; i++) {
        if (seek_to(fr, rng[i].payload_pos) != 0 ||
            seek_to(fo, rng[i].off) != 0 ||
            stream_copy(fr, fo, rng[i].len) != 0)
            goto out;
    }
    /* members at their extents, from "<dir>/<idx>" */
    for (i = 0; i < nmem; i++) {
        char mp[4096];
        off_t msz;
        int n = snprintf(mp, sizeof mp, "%s/%u", dir, mem[i].idx);
        if (n <= 0 || (size_t)n >= sizeof mp) goto out;
        fm = fopen(mp, "rb");
        if (!fm) goto out;
        if (fseeko(fm, 0, SEEK_END) != 0) { fclose(fm); fm = NULL; goto out; }
        msz = ftello(fm);
        if (msz < 0 || (uint64_t)msz != mem[i].len) {
            fclose(fm);
            fm = NULL;
            goto out;                           /* size mismatch: exit 1 */
        }
        if (seek_to(fm, 0) != 0 || seek_to(fo, mem[i].off) != 0 ||
            stream_copy(fm, fo, mem[i].len) != 0) {
            fclose(fm);
            fm = NULL;
            goto out;
        }
        fclose(fm);
        fm = NULL;
    }
    if (fflush(fo) != 0 || ftruncate(fileno(fo), (off_t)size) != 0)
        goto out;
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

static int cmd_map(const char *argv0, const char *in, const char *out)
{
    plan_t pl;
    FILE *o = NULL;
    uint64_t src;
    size_t mi = 0, ri = 0, ndata = 0, i;
    int rc = analyze(argv0, in, &pl);

    if (rc != 0) return rc;
    for (i = 0; i < pl.nmem; i++)
        if (pl.members[i].usize) ndata++;
    o = fopen(out, "wb");
    if (!o) { plan_free(&pl); return 1; }
    /* the recipe layout strip writes: header, then (16B hdr + payload)
     * per range in offset order — RECIPE src_off's index that blob */
    src = recipe_hdr_size(ndata);
    if (wr(o, "MRMP", 4) != 0 ||
        wr_le32(o, (uint32_t)(ndata + pl.nrng)) != 0)
        goto fail;
    while (mi < pl.nmem || ri < pl.nrng) {
        uint64_t orig_off, len, src_off;
        uint32_t idx;
        uint8_t kind;
        while (mi < pl.nmem && !pl.members[mi].usize) mi++;
        if (ri < pl.nrng &&
            (mi == pl.nmem || pl.ranges[ri].off < pl.members[mi].off)) {
            orig_off = pl.ranges[ri].off;
            len = pl.ranges[ri].len;
            kind = 0;                           /* RECIPE */
            idx = 0;
            src_off = src + 16;                 /* past the range header */
            src += 16 + len;
            ri++;
        } else {
            orig_off = pl.members[mi].off;
            len = pl.members[mi].usize;
            kind = 1;                           /* MEMBER */
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

static int cmd_estimate(const char *argv0, const char *in)
{
    plan_t pl;
    uint64_t sum;
    size_t i;
    int rc = analyze(argv0, in, &pl);

    if (rc != 0) return rc;
    sum = pl.archive_size + ESTIMATE_MARGIN;
    for (i = 0; i < pl.nmem; i++) sum += pl.members[i].usize;
    printf("%" PRIu64 "\n", sum);
    plan_free(&pl);
    return 0;
}


/* Where is this .so? The newest plugin host hands the loaded object's path
 * down in args->self_path (ABI v2); older hosts do not, so fall back to the
 * first lib*.so mapping in /proc/self/maps. Both are best-effort: resolve_7zz
 * still has $P7Z_7ZZ and PATH, so a NULL argv0 only loses the pack-dir
 * sibling lookup. C11 + POSIX only (the pack builds with -std=c11 -Werror). */
static int p7z_self_path(const ivpack_container_args *args,
                         char *buf, size_t cap)
{
    char line[1024];
    char path[900];
    FILE *fp;

    if (args && args->self_path && args->self_path[0]) {
        snprintf(buf, cap, "%s", args->self_path);
        return 0;
    }
    fp = fopen("/proc/self/maps", "r");
    if (!fp) return -1;
    while (fgets(line, sizeof line, fp)) {
        const char *sl = strrchr(line, '/');
        const char *e;
        size_t dlen;
        if (!sl) continue;
        if (strncmp(sl + 1, "lib", 3) != 0) continue;
        e = strstr(sl, ".so");
        if (!e) continue;
        dlen = (size_t)(sl - line);              /* the directory part */
        if (dlen == 0 || dlen >= sizeof path) continue;
        memcpy(path, line, dlen);
        path[dlen] = '\0';
        snprintf(buf, cap, "%s%s", path, sl + 1);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return -1;
}

static const char *p7z_self_argv0(const ivpack_container_args *args,
                                  char *buf, size_t cap)
{
    if (p7z_self_path(args, buf, cap) != 0) return NULL;
    return buf;
}

/* p7z's main() allocates the 8 MiB streaming window; the forked worker child
 * needs the same (it leaves through _exit(), so nothing to free). */
static int ivpack_glue_p7z(void)
{
    if (!g_buf) g_buf = (uint8_t *)malloc(COPY_BUF_SZ);
    return g_buf ? 0 : IVPACK_RC_ERROR;
}

/* The estimate macro hands the body the image path as `a`, so argv0 has to be
 * resolved here rather than off ivpack_container_args. */
static int p7z_iv_estimate(const char *in)
{
    char a0[1024];
    return (int)cmd_estimate(p7z_self_argv0(NULL, a0, sizeof a0), in);
}

/* ---- ivpack plugin C ABI export (ADR-007) --------------------------------
 * Built as libp7z.so with -DIVPACK_SHARED_LIB -fPIC -shared; the fork
 * guard in ivpack_impl.h keeps the CLI's exit(3)=decline / exit(1)=error
 * contract intact inside a long-lived worker. See ivpack_impl.h. */
static const ivpack_desc s_p7z_desc = {
    IVPACK_API_VERSION, "p7z", "1.0.0", "containerpack", 0
};

const ivpack_desc *ivpack_get_desc(void) { return &s_p7z_desc; }

/* p7z's cmd_* take argv0 first: the CLI resolves a sibling "7zz" off
 * /proc/self/exe. Inside a worker /proc/self/exe is the daemon, so the glue
 * hands down the loaded .so path instead (host ABI v2) and falls back to
 * scanning /proc/self/maps. argv0 NULL is fine: resolve_7zz() then uses
 * $P7Z_7ZZ or PATH. */
static int p7z_iv_call_enumerate(const ivpack_container_args *a)
{ char a0[1024]; return (int)cmd_enumerate(p7z_self_argv0(a, a0, sizeof a0), a->in_path, a->out_path); }
static int p7z_iv_call_extract(const ivpack_container_args *a)
{ char a0[1024]; return (int)cmd_extract(p7z_self_argv0(a, a0, sizeof a0), a->in_path, a->extract_idx, a->out_path); }
static int p7z_iv_call_strip(const ivpack_container_args *a)
{ char a0[1024]; return (int)cmd_strip(p7z_self_argv0(a, a0, sizeof a0), a->in_path, a->out_path); }
static int p7z_iv_call_rebuild(const ivpack_container_args *a)
{ return (int)cmd_rebuild(a->recipe_path, a->mbr_dir, a->out_path); }
static int p7z_iv_call_map(const ivpack_container_args *a)
{ char a0[1024]; return (int)cmd_map(p7z_self_argv0(a, a0, sizeof a0), a->in_path, a->out_path); }

IVPACK_DEFINE_CONTAINER_CMD(p7z, ivpack_glue_p7z())

IVPACK_DEFINE_CONTAINER_ESTIMATE(p7z, ivpack_glue_p7z(),
                             rc = p7z_iv_estimate(a);)

#ifndef IVPACK_SHARED_LIB
int main(int argc, char **argv)
{
    const char *cmd;
    int rc;

    if (argc < 2) return 2;
    cmd = argv[1];
    g_buf = (uint8_t *)malloc(COPY_BUF_SZ);
    if (!g_buf) return 1;
    rc = 2;
    if (!strcmp(cmd, "enumerate") && argc == 4)
        rc = cmd_enumerate(argv[0], argv[2], argv[3]);
    else if (!strcmp(cmd, "extract") && argc == 5)
        rc = cmd_extract(argv[0], argv[2], argv[3], argv[4]);
    else if (!strcmp(cmd, "strip") && argc == 4)
        rc = cmd_strip(argv[0], argv[2], argv[3]);
    else if (!strcmp(cmd, "rebuild") && argc == 5)
        rc = cmd_rebuild(argv[2], argv[3], argv[4]);
    else if (!strcmp(cmd, "map") && argc == 4)
        rc = cmd_map(argv[0], argv[2], argv[3]);
    else if (!strcmp(cmd, "estimate") && argc == 3)
        rc = cmd_estimate(argv[0], argv[2]);
    free(g_buf);
    return rc;
}
#endif /* !IVPACK_SHARED_LIB */

