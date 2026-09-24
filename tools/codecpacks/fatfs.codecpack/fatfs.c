/*
 * fatfs.c — the fatfs containerpack (InvariantFS WP16a/WP16b ABI v1.1).
 *
 * Decomposes FAT12/16/32 and exFAT disk images into per-file members.
 * Single translation unit, C11, libc only, no mmap: every command streams
 * {in}/{out} in <= 1 MiB windows.
 *
 * Commands (fixed argv, no shell; exit 0 = ok, 3 = decline/refuse, 1 = I/O
 * or internal error, 2 = usage):
 *
 *     enumerate <in> <out>           member table: "idx<TAB>sname<TAB>usize"
 *     extract   <in> <idx> <out>     member idx's raw bytes (exactly usize)
 *     strip     <in> <out>           the recipe (see below)
 *     rebuild   <recipe> <dir> <out> the original image, bit-exact
 *     map       <in> <out>           FS-owned MRMP member map (WP16b)
 *     estimate  <in>                 stdout: decode working set in bytes
 *
 * MEMBERS
 *
 *   Regular files only. Directories, volume labels, LFN entries and files
 *   with the FAT SYSTEM attribute (and every deleted 0xE5 entry) are
 *   METADATA: their bytes stay in the recipe verbatim. A member's usize is
 *   the logical file size (FAT: dir-entry u32; exFAT: stream ValidData-
 *   Length); the slack bytes of its final cluster are NOT extracted.
 *
 *   idx assignment: synthetic, NOT the first cluster (first clusters exceed
 *   the ABI's 0..65535 idx range on large FAT32/exFAT images). idx is the
 *   enumeration sequence number: on-disk directory order, depth-first into
 *   subdirectories at the point the subdirectory entry appears. Stable for
 *   an unchanged image, which is all the ABI requires (the FS iterates the
 *   table, and the map/rebuild pin every run by absolute offset anyway).
 *
 *   Suggested names: the LFN (FAT) / 0xC1 name (exFAT) when present, else
 *   the 8.3 name, folded to [A-Za-z0-9._-] (anything else -> '_') and cut
 *   to 20 chars; empty results fall back to "f<idx>".
 *
 * RECIPE (pack-owned; the FS never parses it)
 *
 *   offset  size
 *   0       4     "FATR"
 *   4       4     u32 version = 1
 *   8       8     u64 orig_size
 *   16      4     u32 n_members
 *   20      4     u32 reserved (0)
 *   24      ..    per member: u32 idx, u64 usize, u32 n_runs, u32 pad,
 *                 then n_runs x { u64 orig_off, u64 len }   (runs in chain
 *                 order, sum(len) == usize, adjacent contiguous clusters
 *                 pre-merged)
 *   ..      ..    complement payload: every original byte NOT covered by a
 *                 member run, ascending original offset, to EOF.
 *
 *   member runs ∪ complement == [0, orig_size) by construction, so rebuild
 *   interleaves member bytes (from <dir>/<idx>) with the complement and is
 *   bit-exact. The map command renders the same partition to MRMP: RECIPE
 *   src_off = 24 + table_bytes + (running complement offset), MEMBER
 *   src_off = the member-internal offset.
 *
 * SUPPORTED / DECLINED (exit 3)
 *
 *   + FAT12/16/32: 512 bytes/sector ONLY (else decline), power-of-two
 *     sectors/cluster, FAT type by data-cluster count (the only correct
 *     rule), FAT12 12-bit entry packing, FAT32 root via root cluster,
 *     FAT12/16 fixed root region, 8.3 + LFN names, subdirectories
 *     (recursive), fragmented chains, chains longer than the file needs
 *     (tail clusters -> recipe).
 *   + exFAT: superfloppy only (PartitionOffset must be 0), 512-byte
 *     sectors, 1 or 2 FATs (only #1 is walked), NoFatChain contiguous
 *     files/dirs AND FAT-chain mode (fragmented) files/dirs, 85/83/C0/C1
 *     entry sets, allocation bitmap / upcase table / labels left in the
 *     recipe. Entry checksums are not validated (names are advisory).
 *   - declined: bytes/sector != 512, partitioned images (MBR), truncated
 *     images, corrupt/looping/cross-linked chains (a global per-cluster
 *     ownership map + walk bounds catch them), >65536 members, zero
 *     members (the FS refuses an empty table anyway), a map that would
 *     exceed the ABI's entry cap.
 */

#define _POSIX_C_SOURCE 200809L   /* fileno, fseeko, ftello under -std=c11 */
#define _FILE_OFFSET_BITS 64

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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


#define RC_OK      0
#define RC_ERR     1
#define RC_USAGE   2
#define RC_DECLINE 3

#define COPY_CHUNK     (1u << 20)          /* streaming window: 1 MiB */
#define MAX_FAT_BYTES  (256ull << 20)      /* one FAT copy, sanity cap */
#define MAX_CLUSTERS   (64ull << 20)       /* ownership map is 1 B/clu */
#define MAX_CLUS_BYTES (32ull << 20)       /* cluster size sanity cap */
#define MAX_DIR_BYTES  (64ull << 20)       /* one directory region cap */
#define MAX_MEMBERS    65536u              /* the ABI's member bound */
#define MAX_RUNS       (4u << 20)          /* total member runs sanity */
#define MAX_DEPTH      64                  /* subdirectory recursion */
#define MAP_MAX_ENTS   (4u * 65536u + 4u)  /* the ABI's map entry cap */

/* ------------------------------------------------------------------ */
/* little-endian scalars (the formats are LE; read via memcpy)         */

static uint16_t le16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = (uint16_t)((v >> 8) | (v << 8));
#endif
    return v;
}

static uint32_t le32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "fatfs: out of memory\n"); exit(RC_ERR); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "fatfs: out of memory\n"); exit(RC_ERR); }
    return q;
}

/* ------------------------------------------------------------------ */
/* parsed-image state                                                  */

typedef struct { uint64_t off, len; } run_t;

typedef struct {
    uint32_t  idx;
    uint64_t  usize;
    run_t    *runs;                 /* chain order; sum(len) == usize */
    uint32_t  n_runs, cap_runs;
    char      sname[24];
} member_t;

typedef struct {
    uint64_t off, len;              /* a run in the original image */
    uint64_t src;                   /* member-internal offset */
    uint32_t midx;
} seg_t;

typedef struct {
    int      exfat;
    int      bits;                  /* 12 / 16 / 32 (exFAT uses 32) */
    uint64_t img_size;
    uint64_t fat_off, fat_len;      /* first FAT copy */
    uint64_t data_off;              /* byte offset of cluster 2 */
    uint64_t clus;                  /* bytes per cluster */
    uint32_t clus_count;            /* valid clusters: 2..clus_count+1 */
    uint32_t root_clu;              /* FAT32 / exFAT root start */
    uint64_t root_off, root_len;    /* FAT12/16 fixed root region */
    uint8_t *fat;                   /* the first FAT, loaded */
    uint8_t *owner;                 /* clus_count+2 claim map */
    member_t *mem;
    uint32_t n_mem, cap_mem;
    uint32_t total_runs;
    FILE    *in;
} fs_t;

static void note(const char *why) { fprintf(stderr, "fatfs: decline: %s\n", why); }

/* ------------------------------------------------------------------ */
/* FAT access                                                          */

/* byte offset of cluster clu (clu >= 2) */
static uint64_t clu_off(const fs_t *f, uint32_t clu)
{
    return f->data_off + (uint64_t)(clu - 2) * f->clus;
}

/* claim a cluster (directory or member payload); -1 = cross-link/loop */
static int claim(fs_t *f, uint32_t clu)
{
    if (f->owner[clu]) return -1;
    f->owner[clu] = 1;
    return 0;
}

static int clu_valid(const fs_t *f, uint32_t clu)
{
    return clu >= 2 && (uint64_t)(clu - 2) < f->clus_count;
}

/* FAT entry value (masked for 32-bit); *ok = 0 when out of the FAT */
static uint32_t fat_next(const fs_t *f, uint32_t clu, int *ok)
{
    uint64_t off;
    *ok = 1;
    if (f->bits == 12) {
        uint16_t v;
        off = (uint64_t)clu + (clu >> 1);
        if (off + 2 > f->fat_len) { *ok = 0; return 0; }
        v = le16(f->fat + off);
        return (clu & 1) ? (uint32_t)(v >> 4) : (uint32_t)(v & 0x0FFF);
    }
    if (f->bits == 16) {
        off = (uint64_t)clu * 2;
        if (off + 2 > f->fat_len) { *ok = 0; return 0; }
        return le16(f->fat + off);
    }
    off = (uint64_t)clu * 4;
    if (off + 4 > f->fat_len) { *ok = 0; return 0; }
    return le32(f->fat + off) & 0x0FFFFFFF;
}

static uint32_t eoc_min(const fs_t *f)
{
    return f->bits == 12 ? 0x0FF8u : f->bits == 16 ? 0xFFF8u : 0x0FFFFFF8u;
}

static uint32_t bad_mark(const fs_t *f)
{
    return f->bits == 12 ? 0x0FF7u : f->bits == 16 ? 0xFFF7u : 0x0FFFFFF7u;
}

static int val_is_eoc(const fs_t *f, uint32_t v) { return v >= eoc_min(f); }
static int val_is_bad(const fs_t *f, uint32_t v) { return v == bad_mark(f); }

/* ------------------------------------------------------------------ */
/* members + runs                                                      */

static member_t *member_new(fs_t *f, uint64_t usize, const char *sname)
{
    member_t *m;
    if (f->n_mem == MAX_MEMBERS) return NULL;
    if (f->n_mem == f->cap_mem) {
        f->cap_mem = f->cap_mem ? f->cap_mem * 2 : 64;
        f->mem = (member_t *)xrealloc(f->mem, f->cap_mem * sizeof *f->mem);
    }
    m = &f->mem[f->n_mem];
    m->idx = f->n_mem;              /* synthetic sequence; see the header */
    m->usize = usize;
    m->runs = NULL;
    m->n_runs = m->cap_runs = 0;
    snprintf(m->sname, sizeof m->sname, "%s", sname);
    f->n_mem++;
    return m;
}

static int member_add_run(fs_t *f, member_t *m, uint64_t off, uint64_t len)
{
    run_t *r;
    if (!len) return 0;
    if (m->n_runs) {
        r = &m->runs[m->n_runs - 1];
        if (r->off + r->len == off) { r->len += len; return 0; } /* merge */
    }
    if (f->total_runs == MAX_RUNS) return -1;
    if (m->n_runs == m->cap_runs) {
        m->cap_runs = m->cap_runs ? m->cap_runs * 2 : 16;
        m->runs = (run_t *)xrealloc(m->runs, m->cap_runs * sizeof *m->runs);
    }
    m->runs[m->n_runs].off = off;
    m->runs[m->n_runs].len = len;
    m->n_runs++;
    f->total_runs++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* name assembly                                                       */

static void sanitize(char *out, size_t cap, const char *in, size_t n)
{
    size_t w = 0, i;
    for (i = 0; i < n && w + 1 < cap; i++) {
        unsigned ch = (unsigned char)in[i];
        int ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                 (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                 ch == '-';
        out[w++] = (char)(ok ? ch : '_');
    }
    out[w] = 0;
}

/* u16 chars -> ASCII sname (printable kept, else '_') */
static void sname_from_utf16(char *out, size_t cap, const uint16_t *cs, size_t n)
{
    char tmp[256];
    size_t i, w = 0;
    for (i = 0; i < n && w + 1 < sizeof tmp; i++) {
        uint16_t c = cs[i];
        if (c == 0x0000) break;
        if (c == 0xFFFF) continue;
        tmp[w++] = (c >= 0x20 && c < 0x7F) ? (char)c : '_';
    }
    sanitize(out, cap, tmp, w);
}

/* FAT 8.3 -> "NAME.EXT" (trailing spaces trimmed) */
static void sname_from_83(char *out, size_t cap, const uint8_t *e)
{
    char tmp[16];
    size_t w = 0;
    int i;
    for (i = 7; i >= 0 && e[i] == ' '; i--) ;
    int nl = i + 1;
    for (i = 0; i < nl; i++) {
        uint8_t c = e[i];
        if (i == 0 && c == 0x05) c = 0xE5;   /* escaped lead byte */
        tmp[w++] = (c >= 0x20 && c < 0x7F) ? (char)c : '_';
    }
    for (i = 10; i >= 8 && e[i] == ' '; i--) ;
    if (i >= 8) {
        int el = i - 8 + 1;
        tmp[w++] = '.';
        for (i = 0; i < el; i++) {
            uint8_t c = e[8 + i];
            tmp[w++] = (c >= 0x20 && c < 0x7F) ? (char)c : '_';
        }
    }
    sanitize(out, cap, tmp, w);
}

/* ------------------------------------------------------------------ */
/* file payload walks                                                  */

/* FAT-chain file: exactly ceil(usize/clus) clusters are claimed and
 * recorded; a longer chain's tail stays unclaimed (-> recipe). */
static int walk_file_chain(fs_t *f, member_t *m, uint32_t start)
{
    uint64_t need = (m->usize + f->clus - 1) / f->clus;
    uint64_t left = m->usize;
    uint32_t clu = start;
    uint64_t i;

    for (i = 0; i < need; i++) {
        uint64_t want;
        if (!clu_valid(f, clu)) { note("file chain out of range"); return -1; }
        if (claim(f, clu) != 0) { note("cross-linked or looping chain"); return -1; }
        want = left < f->clus ? left : f->clus;
        if (member_add_run(f, m, clu_off(f, clu), want) != 0) {
            note("run table overflow");
            return -1;
        }
        left -= want;
        if (i + 1 < need) {
            int ok;
            uint32_t v = fat_next(f, clu, &ok);
            if (!ok || v == 0 || val_is_bad(f, v) || val_is_eoc(f, v) ||
                !clu_valid(f, v)) {
                note("file chain shorter than the file");
                return -1;
            }
            clu = v;
        }
    }
    return 0;
}

/* exFAT NoFatChain file: the clusters start..start+need-1 contiguously */
static int walk_file_contig(fs_t *f, member_t *m, uint32_t start)
{
    uint64_t need = (m->usize + f->clus - 1) / f->clus;
    uint64_t left = m->usize;
    uint64_t i;

    for (i = 0; i < need; i++) {
        uint32_t clu = start + (uint32_t)i;
        uint64_t want;
        if ((uint64_t)i + start > 0xFFFFFFFFu || !clu_valid(f, clu)) {
            note("contiguous file out of range");
            return -1;
        }
        if (claim(f, clu) != 0) { note("cross-linked file"); return -1; }
        want = left < f->clus ? left : f->clus;
        if (member_add_run(f, m, clu_off(f, clu), want) != 0) {
            note("run table overflow");
            return -1;
        }
        left -= want;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* directory region acquisition (clusters claimed, loops caught)       */

/* read a whole FAT-chained directory into memory (bounded by
 * MAX_DIR_BYTES); every cluster is claimed. */
static int read_dir_chain(fs_t *f, uint32_t start, uint8_t **out, size_t *out_len)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    uint32_t clu = start;

    for (;;) {
        int ok;
        uint32_t v;
        if (!clu_valid(f, clu)) { note("directory chain out of range"); goto bad; }
        if (claim(f, clu) != 0) { note("looping/cross-linked directory"); goto bad; }
        if ((uint64_t)len + f->clus > MAX_DIR_BYTES) { note("directory too large"); goto bad; }
        buf = (uint8_t *)xrealloc(buf, len + (size_t)f->clus);
        if (fseeko(f->in, (off_t)clu_off(f, clu), SEEK_SET) != 0 ||
            fread(buf + len, 1, (size_t)f->clus, f->in) != (size_t)f->clus) {
            note("directory read failed");
            goto bad;
        }
        len += (size_t)f->clus;
        v = fat_next(f, clu, &ok);
        if (!ok || v == 0 || val_is_bad(f, v)) { note("bad directory chain"); goto bad; }
        if (val_is_eoc(f, v)) break;
        clu = v;
    }
    *out = buf;
    *out_len = len;
    return 0;
bad:
    free(buf);
    return -1;
}

/* read an exFAT contiguous (NoFatChain) directory of `bytes` bytes */
static int read_dir_contig(fs_t *f, uint32_t start, uint64_t bytes,
                           uint8_t **out, size_t *out_len)
{
    uint64_t nclu = (bytes + f->clus - 1) / f->clus;
    uint64_t total = nclu * f->clus;
    uint64_t i;
    uint8_t *buf;

    if (total > MAX_DIR_BYTES) { note("directory too large"); return -1; }
    for (i = 0; i < nclu; i++) {
        uint32_t clu = start + (uint32_t)i;
        if ((uint64_t)i + start > 0xFFFFFFFFu || !clu_valid(f, clu)) {
            note("contiguous directory out of range");
            return -1;
        }
        if (claim(f, clu) != 0) { note("cross-linked directory"); return -1; }
    }
    buf = (uint8_t *)xmalloc((size_t)total);
    if (fseeko(f->in, (off_t)clu_off(f, start), SEEK_SET) != 0 ||
        fread(buf, 1, (size_t)total, f->in) != (size_t)total) {
        note("directory read failed");
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = (size_t)total;
    return 0;
}

/* ------------------------------------------------------------------ */
/* FAT12/16/32 directory entries                                       */

static int fat_dir(fs_t *f, const uint8_t *d, size_t n, int depth);

static int fat_subdir(fs_t *f, uint32_t start, int depth)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    int rc;

    if (depth >= MAX_DEPTH) { note("directory nesting too deep"); return -1; }
    if (!clu_valid(f, start)) { note("subdirectory start cluster invalid"); return -1; }
    rc = read_dir_chain(f, start, &buf, &len);
    if (rc == 0) rc = fat_dir(f, buf, len, depth + 1);
    free(buf);
    return rc;
}

static int fat_dir(fs_t *f, const uint8_t *d, size_t n, int depth)
{
    uint16_t lfn[20 * 13];      /* chunk k holds name chars k*13..k*13+12 */
    uint8_t  have[20];
    int lfn_max = 0;            /* highest chunk seq seen (0 = no LFN) */
    size_t i;

    memset(have, 0, sizeof have);
    for (i = 0; i + 32 <= n; i += 32) {
        const uint8_t *e = d + i;
        uint8_t attr = e[11];

        if (e[0] == 0x00) break;                 /* end of directory */
        if (e[0] == 0xE5) {                      /* deleted: recipe bytes */
            lfn_max = 0;
            memset(have, 0, sizeof have);
            continue;
        }
        if ((attr & 0x3F) == 0x0F) {             /* LFN chunk */
            unsigned seq = e[0] & 0x1F;
            static const uint8_t coff[13] =
                { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            unsigned k;
            if (e[0] & 0x40) {                   /* the logically-last chunk */
                lfn_max = (int)seq;
                memset(have, 0, sizeof have);
            }
            if (seq >= 1 && seq <= 20) {
                if ((int)seq > lfn_max) lfn_max = (int)seq;
                for (k = 0; k < 13; k++)
                    lfn[(seq - 1) * 13 + k] = le16(e + coff[k]);
                have[seq - 1] = 1;
            }
            continue;
        }
        /* a short entry: label / directory / file */
        {
            char sname[24];
            uint32_t start;
            uint64_t usize;
            int is_label = (attr & 0x08) != 0;
            int is_dir = (attr & 0x10) != 0;
            int is_system = (attr & 0x04) != 0;

            sname[0] = 0;
            if (lfn_max > 0) {                   /* assemble the long name */
                uint16_t cs[20 * 13];
                size_t cn = 0;
                int p;
                for (p = 0; p < lfn_max && p < 20; p++) {
                    unsigned k;
                    if (!have[p]) continue;
                    for (k = 0; k < 13 && cn < sizeof cs / sizeof cs[0]; k++)
                        cs[cn++] = lfn[p * 13 + k];
                }
                sname_from_utf16(sname, sizeof sname, cs, cn);
            }
            if (!sname[0]) sname_from_83(sname, sizeof sname, e);
            lfn_max = 0;
            memset(have, 0, sizeof have);

            if (is_label) continue;              /* volume label: recipe */
            start = (uint32_t)le16(e + 26);
            if (f->bits == 32) start |= (uint32_t)le16(e + 20) << 16;
            usize = le32(e + 28);

            if (is_dir) {
                char raw83[13];
                size_t w = 0;
                int k;
                for (k = 0; k < 11; k++) {       /* "." / ".." by raw 8.3 */
                    uint8_t c = e[k];
                    if (c != ' ' && w + 1 < sizeof raw83) raw83[w++] = (char)c;
                }
                raw83[w] = 0;
                if (strcmp(raw83, ".") == 0 || strcmp(raw83, "..") == 0)
                    continue;
                if (fat_subdir(f, start, depth) != 0) return -1;
                continue;
            }
            if (is_system) continue;             /* SYSTEM files: recipe */
            /* a regular file: a member */
            {
                member_t *m = member_new(f, usize,
                                         sname[0] ? sname : "f");
                if (!m) { note("too many members"); return -1; }
                if (!m->sname[0] || strcmp(m->sname, "f") == 0)
                    snprintf(m->sname, sizeof m->sname, "f%u", m->idx);
                if (usize == 0) continue;        /* zero-length member */
                if (!clu_valid(f, start)) {
                    note("file start cluster invalid");
                    return -1;
                }
                if ((uint64_t)usize > (uint64_t)f->clus_count * f->clus) {
                    note("file larger than the data area");
                    return -1;
                }
                if (walk_file_chain(f, m, start) != 0) return -1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* exFAT directory entries                                             */

static int exfat_dir(fs_t *f, const uint8_t *d, size_t n, int depth);

static int exfat_subdir(fs_t *f, uint32_t start, uint64_t valid, int nofc,
                        int depth)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    int rc;
    uint64_t proc;

    if (depth >= MAX_DEPTH) { note("directory nesting too deep"); return -1; }
    if (nofc) rc = read_dir_contig(f, start, valid, &buf, &len);
    else      rc = read_dir_chain(f, start, &buf, &len);
    if (rc != 0) return -1;
    proc = valid < (uint64_t)len ? valid : (uint64_t)len;
    proc -= proc % 32;
    rc = exfat_dir(f, buf, (size_t)proc, depth + 1);
    free(buf);
    return rc;
}

static int exfat_dir(fs_t *f, const uint8_t *d, size_t n, int depth)
{
    size_t i;

    for (i = 0; i + 32 <= n; i += 32) {
        uint8_t type = d[i];

        if (type == 0x00) break;                 /* end of directory */
        if (!(type & 0x80)) continue;            /* inactive/deleted: recipe */
        if (type != 0x85) continue;              /* 81 bitmap / 82 upcase /
                                                    83 label / A0 GUID: recipe */
        /* a file entry set: 0x85 + SecondaryCount secondaries */
        {
            unsigned nsec = d[i + 1];
            uint16_t attr = le16(d + i + 4);
            const uint8_t *sx = NULL;
            uint16_t name[256];
            size_t name_n = 0;
            unsigned j;

            if (i + 32ull * (1 + nsec) > n) {
                note("file entry set truncated");
                return -1;
            }
            memset(name, 0, sizeof name);
            for (j = 1; j <= nsec; j++) {
                const uint8_t *s = d + i + 32 * j;
                if (s[0] == 0xC0 && !sx) {
                    sx = s;
                } else if (s[0] == 0xC1) {
                    unsigned k;
                    for (k = 0; k < 15 && name_n < sizeof name / sizeof name[0]; k++)
                        name[name_n++] = le16(s + 2 + 2 * k);
                }
                /* unknown secondary types are tolerated and skipped */
            }
            if (!sx) { note("file entry without a stream extension"); return -1; }
            {
                unsigned flags = sx[1];
                unsigned name_len = sx[3];
                uint64_t valid = le64(sx + 8);
                uint32_t start = le32(sx + 20);
                int nofc = (flags & 0x02) != 0;
                char sname[24];

                if (name_len && name_len < name_n) name_n = name_len;
                sname_from_utf16(sname, sizeof sname, name,
                                 name_len ? name_len : name_n);
                if (attr & 0x10) {               /* subdirectory */
                    if (valid == 0) { i += 32ull * nsec; continue; }
                    if (!clu_valid(f, start)) {
                        note("subdirectory start cluster invalid");
                        return -1;
                    }
                    if (exfat_subdir(f, start, valid, nofc, depth) != 0)
                        return -1;
                } else {                         /* a regular file: a member */
                    member_t *m;
                    if (!(flags & 0x01) && valid != 0) {
                        note("file with data but no allocation");
                        return -1;
                    }
                    m = member_new(f, valid, sname[0] ? sname : "f");
                    if (!m) { note("too many members"); return -1; }
                    if (!m->sname[0] || strcmp(m->sname, "f") == 0)
                        snprintf(m->sname, sizeof m->sname, "f%u", m->idx);
                    if (valid == 0) { i += 32ull * nsec; continue; }
                    if (!clu_valid(f, start)) {
                        note("file start cluster invalid");
                        return -1;
                    }
                    if (valid > (uint64_t)f->clus_count * f->clus) {
                        note("file larger than the data area");
                        return -1;
                    }
                    if (nofc) {
                        if (walk_file_contig(f, m, start) != 0) return -1;
                    } else {
                        if (walk_file_chain(f, m, start) != 0) return -1;
                    }
                }
            }
            i += 32ull * nsec;                   /* skip the secondaries */
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* boot parse                                                          */

static int load_fat(fs_t *f)
{
    if (f->fat_len == 0 || f->fat_len > MAX_FAT_BYTES) {
        note("FAT size out of bounds");
        return -1;
    }
    f->fat = (uint8_t *)xmalloc((size_t)f->fat_len);
    if (fseeko(f->in, (off_t)f->fat_off, SEEK_SET) != 0 ||
        fread(f->fat, 1, (size_t)f->fat_len, f->in) != (size_t)f->fat_len) {
        note("FAT read failed");
        return -1;
    }
    return 0;
}

static int parse_exfat(fs_t *f, const uint8_t *b)
{
    uint64_t part_off, vol_len, fat_off_s, fat_len_s, heap_s, cc;
    unsigned bps_shift, spc_shift;
    int k;

    for (k = 11; k < 64; k++)                    /* MustBeZero */
        if (b[k]) { note("exFAT MustBeZero violated"); return -1; }
    if (b[0] != 0xEB || b[1] != 0x76 || b[2] != 0x90) {
        note("exFAT jump boot");
        return -1;
    }
    part_off = le64(b + 64);
    vol_len = le64(b + 72);
    fat_off_s = le32(b + 80);
    fat_len_s = le32(b + 84);
    heap_s = le32(b + 88);
    cc = le32(b + 92);
    f->root_clu = le32(b + 96);
    bps_shift = b[108];
    spc_shift = b[109];
    /* NumberOfFats (b[110]) may be 1 or 2; only #1 is walked, both are
     * recipe bytes. */
    if (part_off != 0) { note("exFAT with a partition offset (MBR image)"); return -1; }
    if (bps_shift != 9) { note("exFAT bytes/sector != 512"); return -1; }
    if (spc_shift > 16) { note("exFAT cluster too large"); return -1; }
    f->clus = 512ull << spc_shift;
    if (f->clus > MAX_CLUS_BYTES) { note("exFAT cluster too large"); return -1; }
    if (vol_len == 0 || vol_len * 512 > f->img_size) {
        note("exFAT volume length exceeds the file (truncated?)");
        return -1;
    }
    if (cc == 0 || cc > MAX_CLUSTERS) { note("exFAT cluster count out of bounds"); return -1; }
    f->fat_off = fat_off_s * 512;
    f->fat_len = fat_len_s * 512;
    f->data_off = heap_s * 512;
    f->clus_count = (uint32_t)cc;
    if (f->fat_off + f->fat_len > vol_len * 512 ||
        f->data_off + (uint64_t)cc * f->clus > vol_len * 512) {
        note("exFAT regions exceed the volume");
        return -1;
    }
    if (!clu_valid(f, f->root_clu)) { note("exFAT root cluster invalid"); return -1; }
    return 0;
}

static int parse_fat(fs_t *f, const uint8_t *b)
{
    uint64_t spc, res, nfats, root_ent, tot, fatsz, root_secs, first_data, cc;
    uint64_t tot16, tot32, fsz16;

    if (b[0] != 0xEB && b[0] != 0xE9) { note("FAT jump boot"); return -1; }
    if (le16(b + 11) != 512) { note("FAT bytes/sector != 512"); return -1; }
    spc = b[13];
    if (spc == 0 || (spc & (spc - 1)) != 0) { note("sectors/cluster not a power of two"); return -1; }
    f->clus = spc * 512;
    if (f->clus > MAX_CLUS_BYTES) { note("cluster too large"); return -1; }
    res = le16(b + 14);
    nfats = b[16];
    root_ent = le16(b + 17);
    tot16 = le16(b + 19);
    tot32 = le32(b + 32);
    tot = tot16 ? tot16 : tot32;
    fsz16 = le16(b + 22);
    fatsz = fsz16 ? fsz16 : le32(b + 36);
    if (res == 0) { note("no reserved sectors"); return -1; }
    if (nfats == 0 || nfats > 4) { note("FAT count out of bounds"); return -1; }
    if (fatsz == 0) { note("zero FAT size"); return -1; }
    if (tot == 0) { note("zero total sectors"); return -1; }
    root_secs = (root_ent * 32 + 511) / 512;
    if ((uint64_t)res + nfats * fatsz + root_secs >= tot) {
        note("no data area");
        return -1;
    }
    first_data = res + nfats * fatsz + root_secs;
    cc = (tot - first_data) / spc;
    if (cc == 0 || cc > MAX_CLUSTERS) { note("cluster count out of bounds"); return -1; }
    if (tot * 512 > f->img_size) {
        note("FAT volume exceeds the file (truncated?)");
        return -1;
    }
    /* the type is decided by the data-cluster count, never by the label */
    f->bits = cc < 4085 ? 12 : cc < 65525 ? 16 : 32;
    f->clus_count = (uint32_t)cc;
    f->fat_off = res * 512;
    f->fat_len = fatsz * 512;
    f->data_off = first_data * 512;
    if (f->bits == 32) {
        f->root_clu = le32(b + 44);
        if (!clu_valid(f, f->root_clu)) { note("FAT32 root cluster invalid"); return -1; }
    } else {
        f->root_off = (res + nfats * fatsz) * 512;
        f->root_len = root_secs * 512;
    }
    return 0;
}

/* full parse: boot -> FAT -> the directory tree -> member run table */
static int fs_parse(fs_t *f, const char *path)
{
    uint8_t b[512];
    struct stat st;
    int rc = RC_DECLINE;

    memset(f, 0, sizeof *f);
    f->in = fopen(path, "rb");
    if (!f->in) { note("open failed"); return RC_ERR; }
    if (fstat(fileno(f->in), &st) != 0 || st.st_size < 512) {
        note("not a block image (too small)");
        goto out;
    }
    f->img_size = (uint64_t)st.st_size;
    if (fread(b, 1, 512, f->in) != 512) { note("boot sector read failed"); goto out; }
    if (b[510] != 0x55 || b[511] != 0xAA) { note("no boot signature"); goto out; }

    if (memcmp(b + 3, "EXFAT   ", 8) == 0) {
        f->exfat = 1;
        f->bits = 32;
        if (parse_exfat(f, b) != 0) goto out;
    } else {
        if (parse_fat(f, b) != 0) goto out;
    }
    if (load_fat(f) != 0) goto out;
    f->owner = (uint8_t *)calloc(f->clus_count + 2, 1);
    if (!f->owner) { note("out of memory"); rc = RC_ERR; goto out; }

    if (f->exfat) {
        uint8_t *buf = NULL;
        size_t len = 0;
        if (read_dir_chain(f, f->root_clu, &buf, &len) != 0) goto out;
        rc = exfat_dir(f, buf, len, 0) == 0 ? 0 : RC_DECLINE;
        free(buf);
    } else if (f->bits == 32) {
        uint8_t *buf = NULL;
        size_t len = 0;
        if (read_dir_chain(f, f->root_clu, &buf, &len) != 0) goto out;
        rc = fat_dir(f, buf, len, 0) == 0 ? 0 : RC_DECLINE;
        free(buf);
    } else {
        if (f->root_len == 0) { note("empty fixed root"); rc = RC_DECLINE; goto out; }
        {
            uint8_t *buf = (uint8_t *)xmalloc((size_t)f->root_len);
            size_t got;
            if (fseeko(f->in, (off_t)f->root_off, SEEK_SET) != 0 ||
                (got = fread(buf, 1, (size_t)f->root_len, f->in)) != (size_t)f->root_len) {
                (void)got;
                note("root directory read failed");
                free(buf);
                rc = RC_DECLINE;
                goto out;
            }
            rc = fat_dir(f, buf, (size_t)f->root_len, 0) == 0 ? 0 : RC_DECLINE;
            free(buf);
        }
    }
    if (rc != 0) goto out;
    if (f->n_mem == 0) { note("no member files"); rc = RC_DECLINE; goto out; }
    return 0;
out:
    return rc;
}

static void fs_free(fs_t *f)
{
    uint32_t i;
    for (i = 0; i < f->n_mem; i++) free(f->mem[i].runs);
    free(f->mem);
    free(f->fat);
    free(f->owner);
    if (f->in) fclose(f->in);
}

/* ------------------------------------------------------------------ */
/* the global segment list: every member run, sorted by original offset,
 * validated to be a non-overlapping in-image partition complement      */

static int seg_cmp(const void *a, const void *b)
{
    const seg_t *x = (const seg_t *)a, *y = (const seg_t *)b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    if (x->midx != y->midx) return x->midx < y->midx ? -1 : 1;
    return x->src < y->src ? -1 : x->src > y->src;
}

/* members/runs may come from the recipe table (rebuild) or a fresh parse */
static int build_segs(const member_t *mem, uint32_t n_mem, uint64_t img_size,
                      seg_t **out, uint32_t *n_out)
{
    seg_t *segs = NULL;
    uint32_t n = 0, i, r;
    uint64_t pos = 0;

    for (i = 0; i < n_mem; i++) {
        const member_t *m = &mem[i];
        uint64_t cur = 0;
        for (r = 0; r < m->n_runs; r++) {
            if (m->runs[r].len == 0 ||
                m->runs[r].off > img_size ||
                m->runs[r].len > img_size - m->runs[r].off) {
                note("member run outside the image");
                free(segs);
                return -1;
            }
            segs = (seg_t *)xrealloc(segs, (n + 1) * sizeof *segs);
            segs[n].off = m->runs[r].off;
            segs[n].len = m->runs[r].len;
            segs[n].src = cur;
            segs[n].midx = m->idx;
            n++;
            cur += m->runs[r].len;
        }
        if (cur != m->usize) {
            note("member runs do not add up to usize");
            free(segs);
            return -1;
        }
    }
    if (n) qsort(segs, n, sizeof *segs, seg_cmp);
    for (i = 0; i < n; i++) {
        if (segs[i].off < pos) {
            note("member runs overlap");
            free(segs);
            return -1;
        }
        pos = segs[i].off + segs[i].len;
    }
    *out = segs;
    *n_out = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* streaming helpers                                                   */

static int copy_from(FILE *dst, FILE *src, uint64_t off, uint64_t len,
                     uint8_t *buf)
{
    if (fseeko(src, (off_t)off, SEEK_SET) != 0) return -1;
    while (len) {
        size_t want = len > COPY_CHUNK ? COPY_CHUNK : (size_t)len;
        if (fread(buf, 1, want, src) != want) return -1;
        if (fwrite(buf, 1, want, dst) != want) return -1;
        len -= want;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* recipe header/table serialization (shared by strip and map)         */

static uint64_t table_bytes(const fs_t *f)
{
    uint64_t t = 24;
    uint32_t i;
    for (i = 0; i < f->n_mem; i++)
        t += 20 + 16ull * f->mem[i].n_runs;
    return t;
}

static int write_table(FILE *out, const fs_t *f)
{
    uint8_t h[24];
    uint32_t i, r;

    memcpy(h, "FATR", 4);
    put32(h + 4, 1);
    put64(h + 8, f->img_size);
    put32(h + 16, f->n_mem);
    put32(h + 20, 0);
    if (fwrite(h, 1, 24, out) != 24) return -1;
    for (i = 0; i < f->n_mem; i++) {
        const member_t *m = &f->mem[i];
        uint8_t mh[20];
        put32(mh, m->idx);
        put64(mh + 4, m->usize);
        put32(mh + 12, m->n_runs);
        put32(mh + 16, 0);
        if (fwrite(mh, 1, 20, out) != 20) return -1;
        for (r = 0; r < m->n_runs; r++) {
            uint8_t rb[16];
            put64(rb, m->runs[r].off);
            put64(rb + 8, m->runs[r].len);
            if (fwrite(rb, 1, 16, out) != 16) return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */

static int cmd_enumerate(const char *in, const char *out)
{
    fs_t f;
    FILE *o;
    uint32_t i;
    int rc = RC_OK;

    if (fs_parse(&f, in) != 0) { fs_free(&f); return RC_DECLINE; }
    o = fopen(out, "w");
    if (!o) { fs_free(&f); return RC_ERR; }
    for (i = 0; i < f.n_mem; i++)
        if (fprintf(o, "%u\t%s\t%" PRIu64 "\n", f.mem[i].idx, f.mem[i].sname,
                    f.mem[i].usize) < 0)
            rc = RC_ERR;
    if (fclose(o) != 0) rc = RC_ERR;
    fs_free(&f);
    return rc;
}

static int cmd_extract(const char *in, const char *idx_s, const char *out)
{
    fs_t f;
    FILE *o = NULL;
    uint8_t *buf = NULL;
    uint32_t idx, i, r;
    const member_t *m = NULL;
    uint64_t wrote = 0;
    int rc = RC_ERR;

    idx = (uint32_t)strtoul(idx_s, NULL, 10);
    if (fs_parse(&f, in) != 0) { fs_free(&f); return RC_DECLINE; }
    for (i = 0; i < f.n_mem; i++)
        if (f.mem[i].idx == idx) { m = &f.mem[i]; break; }
    if (!m) { note("no such member idx"); rc = RC_DECLINE; goto out; }
    o = fopen(out, "wb");
    if (!o) goto out;
    buf = (uint8_t *)xmalloc(COPY_CHUNK);
    for (r = 0; r < m->n_runs; r++) {
        if (copy_from(o, f.in, m->runs[r].off, m->runs[r].len, buf) != 0)
            goto out;
        wrote += m->runs[r].len;
    }
    if (wrote != m->usize) goto out;
    if (fclose(o) != 0) { o = NULL; goto out; }
    o = NULL;
    rc = RC_OK;
out:
    if (o) fclose(o);
    free(buf);
    fs_free(&f);
    return rc;
}

static int cmd_strip(const char *in, const char *out)
{
    fs_t f;
    FILE *o = NULL;
    uint8_t *buf = NULL;
    seg_t *segs = NULL;
    uint32_t n_segs = 0, i;
    uint64_t pos = 0;
    int rc = RC_ERR;

    if (fs_parse(&f, in) != 0) { fs_free(&f); return RC_DECLINE; }
    if (build_segs(f.mem, f.n_mem, f.img_size, &segs, &n_segs) != 0) {
        fs_free(&f);
        return RC_DECLINE;
    }
    o = fopen(out, "wb");
    if (!o) goto out;
    buf = (uint8_t *)xmalloc(COPY_CHUNK);
    if (write_table(o, &f) != 0) goto out;
    for (i = 0; i < n_segs; i++) {
        if (segs[i].off > pos &&
            copy_from(o, f.in, pos, segs[i].off - pos, buf) != 0)
            goto out;
        pos = segs[i].off + segs[i].len;
    }
    if (f.img_size > pos && copy_from(o, f.in, pos, f.img_size - pos, buf) != 0)
        goto out;
    if (fclose(o) != 0) { o = NULL; goto out; }
    o = NULL;
    rc = RC_OK;
out:
    if (o) fclose(o);
    free(buf);
    free(segs);
    fs_free(&f);
    return rc;
}

static int cmd_map(const char *in, const char *out)
{
    fs_t f;
    FILE *o = NULL;
    seg_t *segs = NULL;
    uint32_t n_segs = 0, i;
    uint64_t pos = 0, recipe_src, tbl;
    int rc = RC_ERR;

    if (fs_parse(&f, in) != 0) { fs_free(&f); return RC_DECLINE; }
    if (build_segs(f.mem, f.n_mem, f.img_size, &segs, &n_segs) != 0) {
        fs_free(&f);
        return RC_DECLINE;
    }
    /* entries: every member run plus the gaps between them */
    {
        uint64_t n_gaps = 0;
        for (i = 0; i < n_segs; i++) {
            if (segs[i].off > pos) n_gaps++;
            pos = segs[i].off + segs[i].len;
        }
        if (f.img_size > pos) n_gaps++;
        if (n_gaps + n_segs > MAP_MAX_ENTS) {
            note("map would exceed the ABI entry cap");
            free(segs);
            fs_free(&f);
            return RC_DECLINE;
        }
    }
    o = fopen(out, "wb");
    if (!o) goto out;
    tbl = table_bytes(&f);
    recipe_src = tbl;               /* complement bytes follow the table */
    {
        /* count first, then the entries */
        uint64_t n_gaps = 0, count;
        uint8_t hdr[8];
        pos = 0;
        for (i = 0; i < n_segs; i++) {
            if (segs[i].off > pos) n_gaps++;
            pos = segs[i].off + segs[i].len;
        }
        if (f.img_size > pos) n_gaps++;
        count = n_gaps + n_segs;
        memcpy(hdr, "MRMP", 4);
        put32(hdr + 4, (uint32_t)count);
        if (fwrite(hdr, 1, 8, o) != 8) goto out;
    }
    pos = 0;
    for (i = 0; i <= n_segs; i++) {
        uint64_t seg_off = i < n_segs ? segs[i].off : f.img_size;
        uint8_t en[29];
        if (seg_off > pos) {        /* RECIPE gap */
            put64(en, pos);
            put64(en + 8, seg_off - pos);
            en[16] = 0;
            put32(en + 17, 0);
            put64(en + 21, recipe_src);
            if (fwrite(en, 1, 29, o) != 29) goto out;
            recipe_src += seg_off - pos;
        }
        if (i < n_segs) {           /* MEMBER run */
            put64(en, segs[i].off);
            put64(en + 8, segs[i].len);
            en[16] = 1;
            put32(en + 17, segs[i].midx);
            put64(en + 21, segs[i].src);
            if (fwrite(en, 1, 29, o) != 29) goto out;
            pos = segs[i].off + segs[i].len;
        }
    }
    if (fclose(o) != 0) { o = NULL; goto out; }
    o = NULL;
    rc = RC_OK;
out:
    if (o) fclose(o);
    free(segs);
    fs_free(&f);
    return rc;
}

static int cmd_estimate(const char *in)
{
    fs_t f;
    uint64_t sum = 0;
    uint32_t i;

    if (fs_parse(&f, in) != 0) { fs_free(&f); return RC_DECLINE; }
    for (i = 0; i < f.n_mem; i++) sum += f.mem[i].usize;
    printf("%" PRIu64 "\n", (uint64_t)(sum + (64ull << 20)));
    fs_free(&f);
    return RC_OK;
}

/* rebuild: the recipe table + <dir>/<idx> member files -> the original */
static int cmd_rebuild(const char *recipe, const char *dir, const char *out)
{
    FILE *r = NULL, *o = NULL, *mf = NULL;
    uint8_t *buf = NULL;
    member_t *mem = NULL;
    seg_t *segs = NULL;
    uint32_t n_mem = 0, n_segs = 0, i, j;
    uint64_t orig_size, tbl = 24, rsize, pos, payload;
    uint8_t h[24];
    int rc = RC_ERR;
    uint32_t mf_idx = UINT32_MAX;

    r = fopen(recipe, "rb");
    if (!r) return RC_ERR;
    if (fread(h, 1, 24, r) != 24 || memcmp(h, "FATR", 4) != 0 ||
        le32(h + 4) != 1) {
        note("bad recipe header");
        goto out;
    }
    orig_size = le64(h + 8);
    n_mem = le32(h + 16);
    if (n_mem == 0 || n_mem > MAX_MEMBERS) { note("bad member count"); goto out; }
    mem = (member_t *)calloc(n_mem, sizeof *mem);
    if (!mem) goto out;
    for (i = 0; i < n_mem; i++) {
        uint8_t mh[20];
        member_t *m = &mem[i];
        if (fread(mh, 1, 20, r) != 20) { note("short member table"); goto out; }
        m->idx = le32(mh);
        m->usize = le64(mh + 4);
        m->n_runs = le32(mh + 12);
        if (m->n_runs > MAX_RUNS || (m->usize && m->n_runs == 0) ||
            (!m->usize && m->n_runs != 0)) {
            note("bad run count");
            goto out;
        }
        if ((uint64_t)m->n_runs > (orig_size / 512 + 1)) {
            note("implausible run count");
            goto out;
        }
        m->runs = (run_t *)xmalloc((size_t)m->n_runs * sizeof(run_t) + 1);
        for (j = 0; j < m->n_runs; j++) {
            uint8_t rb[16];
            if (fread(rb, 1, 16, r) != 16) { note("short run table"); goto out; }
            m->runs[j].off = le64(rb);
            m->runs[j].len = le64(rb + 8);
        }
        tbl += 20 + 16ull * m->n_runs;
    }
    if (build_segs(mem, n_mem, orig_size, &segs, &n_segs) != 0) goto out;
    /* the recipe payload must be exactly the complement */
    payload = orig_size;
    for (i = 0; i < n_segs; i++) payload -= segs[i].len;
    if (fseeko(r, 0, SEEK_END) != 0 || (rsize = (uint64_t)ftello(r)) == (uint64_t)-1)
        goto out;
    if (rsize != tbl + payload) { note("recipe size mismatch"); goto out; }

    /* every member file must exist with exactly usize bytes */
    for (i = 0; i < n_mem; i++) {
        char *path = (char *)xmalloc(strlen(dir) + 16);
        struct stat st;
        sprintf(path, "%s/%u", dir, mem[i].idx);
        if (stat(path, &st) != 0) {
            note("member file missing");
            free(path);
            goto out;
        }
        free(path);
        if ((uint64_t)st.st_size != mem[i].usize) {
            note("member file size mismatch");
            goto out;
        }
    }

    o = fopen(out, "wb");
    if (!o) goto out;
    buf = (uint8_t *)xmalloc(COPY_CHUNK);
    pos = 0;
    {
        uint64_t recipe_cur = tbl;
        for (i = 0; i <= n_segs; i++) {
            uint64_t seg_off = i < n_segs ? segs[i].off : orig_size;
            if (seg_off > pos) {
                if (copy_from(o, r, recipe_cur, seg_off - pos, buf) != 0)
                    goto out;
                recipe_cur += seg_off - pos;
            }
            if (i < n_segs) {
                uint64_t left = segs[i].len, off = segs[i].src;
                if (mf_idx != segs[i].midx) {
                    char *path = (char *)xmalloc(strlen(dir) + 16);
                    if (mf) fclose(mf);
                    sprintf(path, "%s/%u", dir, segs[i].midx);
                    mf = fopen(path, "rb");
                    free(path);
                    if (!mf) goto out;
                    mf_idx = segs[i].midx;
                }
                if (fseeko(mf, (off_t)off, SEEK_SET) != 0) goto out;
                while (left) {
                    size_t want = left > COPY_CHUNK ? COPY_CHUNK : (size_t)left;
                    if (fread(buf, 1, want, mf) != want) goto out;
                    if (fwrite(buf, 1, want, o) != want) goto out;
                    left -= want;
                }
                pos = segs[i].off + segs[i].len;
            }
        }
    }
    if (fclose(o) != 0) { o = NULL; goto out; }
    o = NULL;
    rc = RC_OK;
out:
    if (mf) fclose(mf);
    if (o) fclose(o);
    if (r) fclose(r);
    for (i = 0; i < n_mem; i++) free(mem[i].runs);
    free(mem);
    free(segs);
    free(buf);
    return rc;
}


/* ---- ivpack plugin C ABI export (ADR-007) --------------------------------
 * Built as libfatfs.so with -DIVPACK_SHARED_LIB -fPIC -shared; the fork
 * guard in ivpack_impl.h keeps the CLI's exit(3)=decline / exit(1)=error
 * contract intact inside a long-lived worker. See ivpack_impl.h. */
static const ivpack_desc s_fatfs_desc = {
    IVPACK_API_VERSION, "fatfs", "1.0.0", "containerpack", 0
};

const ivpack_desc *ivpack_get_desc(void) { return &s_fatfs_desc; }

IVPACK_CANON_CALLS(fatfs)

IVPACK_DEFINE_CONTAINER_CMD(fatfs, IVPACK_GLUE_NONE())

IVPACK_DEFINE_CONTAINER_ESTIMATE(fatfs, IVPACK_GLUE_NONE(),
                             rc = (int)cmd_estimate(a);)

#ifndef IVPACK_SHARED_LIB
int main(int argc, char **argv)
{
    if (argc < 3) return RC_USAGE;
    if (strcmp(argv[1], "enumerate") == 0 && argc == 4)
        return cmd_enumerate(argv[2], argv[3]);
    if (strcmp(argv[1], "extract") == 0 && argc == 5)
        return cmd_extract(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "strip") == 0 && argc == 4)
        return cmd_strip(argv[2], argv[3]);
    if (strcmp(argv[1], "rebuild") == 0 && argc == 5)
        return cmd_rebuild(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "map") == 0 && argc == 4)
        return cmd_map(argv[2], argv[3]);
    if (strcmp(argv[1], "estimate") == 0 && argc == 3)
        return cmd_estimate(argv[2]);
    return RC_USAGE;
}
#endif /* !IVPACK_SHARED_LIB */

