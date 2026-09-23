/*
 * rawdisk.c — rawdisk containerpack helper (InvariantFS WP16a/WP16b).
 *
 * Decomposes raw disk images (512-byte sectors) with GPT or MBR/EBR
 * partition tables into their partition members. Single C11 file, no
 * dependencies beyond libc, fully streaming (<= 8 MiB windows): members
 * are partitions and can be huge, so no command ever loads one in RAM.
 *
 * Commands (fixed argv, no shell; exit 0 = ok, 3 = decline, else error;
 * stdout/stderr are /dev/null for everything except estimate's stdout):
 *
 *     enumerate <in> <out>           member table lines "idx<TAB>sname<TAB>usize"
 *     extract   <in> <idx> <out>     member idx's raw bytes (exactly usize)
 *     strip     <in> <out>           the recipe (RDR1, below)
 *     rebuild   <recipe> <dir> <out> original image, bit-exact
 *     map       <in> <out>           the FS-owned MRMP member map (WP16b)
 *     estimate  <in>                 print sum(member usize) + 64 MiB
 *
 * Parse rules (STRICT — a weak sniff needs a strict parse; any structural
 * violation declines the whole image, which then stays RAW / goes generic;
 * bit-rot INSIDE the structures is preserved verbatim by rebuild instead):
 *
 *   GPT: "EFI PART" at LBA1. header fields: hdr_size in [92,512],
 *        current_lba == 1, entries_lba/num_entries/entry_size sane (entry
 *        size a power of two in [128,4096], table within the file, <= 64
 *        MiB, num_entries <= 65535 so idx = slot fits the FS's u16 range).
 *        An entry is used iff its type GUID is nonzero; the member extent
 *        is [first_lba,last_lba]*512 and must lie within the file. Member
 *        extents must not overlap. sname = the UTF-16LE name folded to
 *        ASCII (units >= 128 fold to '_'), else "p%04d" (the slot).
 *        A GPT signature present but structurally invalid declines the
 *        image outright — there is NO fall-through to the MBR (a
 *        protective/hybrid MBR would only whole-disk the image).
 *   MBR: 0x55AA at 510. 4 primary slots @446 (16 B each: boot, chs, type,
 *        chs, u32 lba, u32 count). Used iff type != 0 and count != 0; type
 *        0xEE (GPT protective) is skipped. Extents must lie within the
 *        file. idx = slot 1..4, sname "t%02x" (the type byte).
 *        Types 0x05/0x0F/0x85 are extended partitions: NOT members; their
 *        EBR chain yields logical partitions (idx 11+, sname "t%02x").
 *        Each EBR must carry 0x55AA; entry 0 = the logical partition (lba
 *        relative to this EBR, must lie inside the extended region), entry
 *        1 = the next EBR (lba relative to the extended region start) or
 *        0. The walk is loop-guarded (a visited-LBA set + step cap); any
 *        anomaly declines the image.
 *   Both fail -> decline (3). Zero members -> decline (3) (the FS refuses
 *   empty tables anyway; a "disk" with no partitions is not a container).
 *
 * Member indexes: MBR primary slots 1-4, logical partitions 11+ in chain
 * order, GPT slots 1..num_entries. Indexes are NOT dense (an unused MBR
 * slot leaves a hole); the FS iterates the table, not 0..n-1.
 *
 * ------------------------------------------------------------------------
 * RECIPE FORMAT ("RDR1", pack-owned; the FS never parses it). All integers
 * little-endian. The recipe carries the disk size, the member table
 * (idx + extent — the idx<->extent binding, since rebuild gets only
 * "<dir>/<idx>" files and slot order is not extent order), and every byte
 * OUTSIDE the member extents, verbatim, as ordered (off,len,bytes) ranges:
 * boot sectors, partition tables (GPT primary AND backup, EBRs), and all
 * gaps with whatever arbitrary content they hold.
 *
 *   offset  size  field
 *   0       4     "RDR1"
 *   4       8     u64 disk_size
 *   12      4     u32 n_members
 *   16      20*n_members   members, sorted by off:
 *                        u32 idx, u64 off, u64 len        (len > 0)
 *   ..      4     u32 n_ranges
 *   ..      var   ranges, sorted by off, each:
 *                        u64 off, u64 len, u8 bytes[len]  (len > 0)
 *
 * members + ranges together EXACTLY partition [0, disk_size) (rebuild
 * validates this before writing anything; trailing garbage refuses).
 * rebuild: truncate {out} to disk_size, write the recipe ranges verbatim
 * at their offsets, splice each "<dir>/<idx>" at its member extent; a
 * missing/short/long member file or any recipe inconsistency exits 1.
 *
 * MAP (MRMP, FS-owned): one RECIPE entry per recipe range (src_off = the
 * range's payload offset INSIDE the recipe blob, i.e. the RDR1 layout
 * above) and one MEMBER entry per member extent (idx, src_off = 0) —
 * trivial because partitions are contiguous. Entries sorted by orig_off,
 * partitioning [0, disk_size). strip and map share the layout math, so
 * the RECIPE src_off's always match the blob strip wrote.
 *
 * Refuse list (exit 3, "not ours / cannot decompose"):
 *   not GPT nor MBR (incl. < 512 B, no 0x55AA, text named .img)
 *   GPT signature with any invalid header field or table geometry
 *   GPT/MBR used entry extent outside the file, or overlapping extents
 *   MBR primary type 0xEE (protective), zero members, EBR anomaly
 *   > 65535 GPT slots / > 65535 members total (FS idx bound)
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


#define SECTOR_SIZE 512ull
#define COPY_BUF (8u << 20)            /* 8 MiB streaming window */
#define GPT_MAX_TABLE (64ull << 20)    /* GPT entries-table sanity cap */
#define MAX_MEMBERS 65535u             /* FS idx is 0..65535; we start at 1 */
#define EBR_STEP_CAP 4096u             /* EBR chain loop guard */
#define ESTIMATE_MARGIN (64ull << 20)  /* estimate = sum(usize) + 64 MiB */

static uint8_t *g_buf;                 /* the streaming window (main) */

typedef struct {
    uint32_t idx;                      /* member index (the FS contract) */
    uint64_t off;                      /* byte offset in the disk image */
    uint64_t len;                      /* extent bytes (== usize) */
    char     sname[40];                /* suggested name (advisory) */
} member_t;

typedef struct {
    uint64_t off;
    uint64_t len;
} range_t;

typedef struct {
    uint64_t  disk_size;
    member_t *members;                 /* sorted by off */
    size_t    nmem;
    range_t  *ranges;                  /* sorted by off; member complement */
    size_t    nrng;
} plan_t;

/* ---------------- little-endian + I/O helpers ---------------- */

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
    while (n) {
        size_t c = n > COPY_BUF ? COPY_BUF : (size_t)n;
        if (fread(g_buf, 1, c, src) != c) return -1;
        if (fwrite(g_buf, 1, c, dst) != c) return -1;
        n -= c;
    }
    return 0;
}

static int seek_to(FILE *f, uint64_t off)
{
    return fseeko(f, (off_t)off, SEEK_SET);
}

/* ---------------- plan (shared analysis) ---------------- */

static void plan_free(plan_t *p)
{
    free(p->members);
    free(p->ranges);
    memset(p, 0, sizeof *p);
}

static int plan_add_member(plan_t *p, uint32_t idx, uint64_t off,
                           uint64_t len, const char *sname)
{
    member_t *m;
    if (p->nmem >= MAX_MEMBERS) return -1;
    m = (member_t *)realloc(p->members, (p->nmem + 1) * sizeof *m);
    if (!m) return -1;
    p->members = m;
    m = &p->members[p->nmem++];
    m->idx = idx;
    m->off = off;
    m->len = len;
    snprintf(m->sname, sizeof m->sname, "%s", sname);
    return 0;
}

static int member_off_cmp(const void *a, const void *b)
{
    uint64_t x = ((const member_t *)a)->off;
    uint64_t y = ((const member_t *)b)->off;
    return x < y ? -1 : x > y;
}

/* sort members by offset, refuse overlaps, build the complement ranges */
static int plan_finish(plan_t *p)
{
    uint64_t pos = 0;
    size_t i, n = 0;

    if (!p->nmem) return -1;
    qsort(p->members, p->nmem, sizeof *p->members, member_off_cmp);
    for (i = 1; i < p->nmem; i++)
        if (p->members[i].off < p->members[i - 1].off + p->members[i - 1].len)
            return -1;                          /* overlapping extents */
    p->ranges = (range_t *)malloc((p->nmem + 1) * sizeof *p->ranges);
    if (!p->ranges) return -1;
    for (i = 0; i < p->nmem; i++) {
        if (p->members[i].off > pos) {
            p->ranges[n].off = pos;
            p->ranges[n].len = p->members[i].off - pos;
            n++;
        }
        pos = p->members[i].off + p->members[i].len;
    }
    if (pos < p->disk_size) {
        p->ranges[n].off = pos;
        p->ranges[n].len = p->disk_size - pos;
        n++;
    }
    p->nrng = n;
    return 0;
}

/* ---------------- GPT ---------------- */

static int guid_is_zero(const uint8_t *g)
{
    int i;
    for (i = 0; i < 16; i++)
        if (g[i]) return 0;
    return 1;
}

static void gpt_sname(char *dst, size_t cap, const uint8_t *nm, uint32_t slot)
{
    size_t w = 0;
    int i;
    for (i = 0; i < 36 && w + 1 < cap; i++) {
        unsigned u = nm[2 * i] | ((unsigned)nm[2 * i + 1] << 8);
        if (!u) break;
        dst[w++] = u < 128 ? (char)u : '_';
    }
    dst[w] = 0;
    if (!w) snprintf(dst, cap, "p%04u", slot);
}

/* 0 = parsed, 3 = decline. *is_gpt set to 1 iff the LBA1 signature is
 * present (a present-but-invalid GPT declines; it never falls to MBR). */
static int parse_gpt(FILE *f, uint64_t size, plan_t *p, int *is_gpt)
{
    uint8_t hdr[SECTOR_SIZE];
    uint64_t hdr_size, current_lba, entries_lba, num, esz, table, limit;
    uint64_t done = 0, epc;
    int rc = 3;

    *is_gpt = 0;
    if (size < 2 * SECTOR_SIZE) return 3;
    if (read_at(f, SECTOR_SIZE, hdr, sizeof hdr) != 0) return 3;
    if (memcmp(hdr, "EFI PART", 8) != 0) return 3;
    *is_gpt = 1;

    hdr_size = le32(hdr + 12);
    current_lba = le64(hdr + 24);
    entries_lba = le64(hdr + 72);
    num = le32(hdr + 80);
    esz = le32(hdr + 84);
    if (hdr_size < 92 || hdr_size > SECTOR_SIZE) return 3;
    if (current_lba != 1) return 3;
    if (!num || num > MAX_MEMBERS) return 3;
    if (esz < 128 || esz > 4096 || (esz & (esz - 1)) != 0) return 3;
    table = num * esz;
    if (table > GPT_MAX_TABLE) return 3;
    limit = size / SECTOR_SIZE;                          /* whole sectors */
    if (entries_lba < 2 || entries_lba >= limit) return 3;
    if (entries_lba * SECTOR_SIZE + table > size) return 3;

    epc = COPY_BUF / esz;                                /* entries/window */
    while (done < num) {
        uint64_t take = num - done < epc ? num - done : epc;
        uint64_t i;
        if (read_at(f, entries_lba * SECTOR_SIZE + done * esz,
                    g_buf, (size_t)(take * esz)) != 0)
            return 3;
        for (i = 0; i < take; i++) {
            const uint8_t *e = g_buf + i * esz;
            uint64_t first, last, off, len;
            char sname[40];
            if (guid_is_zero(e)) continue;               /* unused entry */
            first = le64(e + 32);
            last = le64(e + 40);
            if (first > last || last >= limit) return 3; /* outside disk */
            off = first * SECTOR_SIZE;
            len = (last - first + 1) * SECTOR_SIZE;
            gpt_sname(sname, sizeof sname, e + 56, (uint32_t)(done + i + 1));
            if (plan_add_member(p, (uint32_t)(done + i + 1), off, len,
                                sname) != 0)
                return 3;
        }
        done += take;
    }
    if (plan_finish(p) != 0) return 3;
    rc = 0;
    return rc;
}

/* ---------------- MBR / EBR ---------------- */

static int type_is_extended(uint8_t t)
{
    return t == 0x05 || t == 0x0F || t == 0x85;
}

static int mbr_sig_ok(const uint8_t *sec)
{
    return sec[510] == 0x55 && sec[511] == 0xAA;
}

/* walk one extended partition's EBR chain, adding logical members */
static int parse_ebr_chain(FILE *f, uint64_t size, plan_t *p,
                           uint64_t ext_start, uint64_t ext_cnt,
                           uint32_t *next_logical)
{
    uint8_t sec[SECTOR_SIZE];
    uint64_t *visited = NULL;
    size_t nvis = 0;
    uint64_t ebr = ext_start, limit = size / SECTOR_SIZE;
    unsigned steps = 0;
    int rc = 3;

    visited = (uint64_t *)malloc(EBR_STEP_CAP * sizeof *visited);
    if (!visited) return 1;
    for (;;) {
        const uint8_t *e0, *e1;
        uint8_t t0, t1;
        uint32_t s0, c0, s1, c1;
        uint64_t lba, next;
        char sname[16];
        size_t i;

        if (++steps > EBR_STEP_CAP) goto out;            /* loop guard */
        for (i = 0; i < nvis; i++)
            if (visited[i] == ebr) goto out;             /* chain loops */
        visited[nvis++] = ebr;
        if (read_at(f, ebr * SECTOR_SIZE, sec, sizeof sec) != 0) goto out;
        if (!mbr_sig_ok(sec)) goto out;                  /* rotten link */

        e0 = sec + 446;
        t0 = e0[4];
        s0 = le32(e0 + 8);
        c0 = le32(e0 + 12);
        if (t0 && t0 != 0xEE && !type_is_extended(t0) && c0) {
            lba = ebr + s0;
            if (lba < ext_start || lba >= ext_start + ext_cnt ||
                c0 > ext_start + ext_cnt - lba)
                goto out;                                /* outside region */
            snprintf(sname, sizeof sname, "t%02x", t0);
            if (*next_logical > MAX_MEMBERS) goto out;
            if (plan_add_member(p, (*next_logical)++, lba * SECTOR_SIZE,
                                (uint64_t)c0 * SECTOR_SIZE, sname) != 0)
                goto out;
        } else if (t0 && (type_is_extended(t0) || !c0)) {
            goto out;    /* entry 0 must be a normal, sized partition */
        }

        e1 = sec + 462;
        t1 = e1[4];
        s1 = le32(e1 + 8);
        c1 = le32(e1 + 12);
        if (!t1) break;                                  /* chain ends */
        if (!type_is_extended(t1) || !c1) goto out;      /* malformed link */
        next = ext_start + s1;
        if (next < ext_start || next >= ext_start + ext_cnt ||
            next >= limit)
            goto out;                                    /* bad link */
        ebr = next;
    }
    rc = 0;
out:
    free(visited);
    return rc;
}

/* 0 = parsed, 3 = decline, 1 = hard error */
static int parse_mbr(FILE *f, uint64_t size, plan_t *p)
{
    uint8_t sec[SECTOR_SIZE];
    uint64_t limit = size / SECTOR_SIZE;
    uint64_t ext_start[4], ext_cnt[4];
    size_t next = 0, i;
    uint32_t next_logical = 11;

    if (size < SECTOR_SIZE) return 3;
    if (read_at(f, 0, sec, sizeof sec) != 0) return 3;
    if (!mbr_sig_ok(sec)) return 3;

    for (i = 0; i < 4; i++) {
        const uint8_t *e = sec + 446 + 16 * i;
        uint8_t t = e[4];
        uint32_t start = le32(e + 8), cnt = le32(e + 12);
        char sname[16];

        if (!t || t == 0xEE) continue;     /* unused / GPT protective */
        if (!cnt) continue;                /* zero-sized: treat as unused */
        if (!start || (uint64_t)start + cnt > limit)
            return 3;                      /* claims space past EOF */
        if (type_is_extended(t)) {
            if (next < 4) {
                ext_start[next] = start;
                ext_cnt[next] = cnt;
                next++;
            }
            continue;                      /* the container, not a member */
        }
        snprintf(sname, sizeof sname, "t%02x", t);
        if (plan_add_member(p, (uint32_t)i + 1, (uint64_t)start * SECTOR_SIZE,
                            (uint64_t)cnt * SECTOR_SIZE, sname) != 0)
            return 3;
    }
    for (i = 0; i < next; i++) {
        int r = parse_ebr_chain(f, size, p, ext_start[i], ext_cnt[i],
                                &next_logical);
        if (r != 0) return r;
    }
    if (plan_finish(p) != 0) return 3;
    return 0;
}

/* analyze the image into a plan; 0 ok, 3 decline, 1 error */
static int analyze(const char *path, plan_t *p)
{
    FILE *f;
    uint64_t size;
    int is_gpt = 0, rc;

    memset(p, 0, sizeof *p);
    f = fopen(path, "rb");
    if (!f) return 1;
    if (seek_to(f, 0) != 0 || fseeko(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 1;
    }
    {
        off_t sz = ftello(f);
        if (sz < 0) { fclose(f); return 1; }
        size = (uint64_t)sz;
    }
    if (!size) { fclose(f); return 3; }
    p->disk_size = size;

    rc = parse_gpt(f, size, p, &is_gpt);
    if (rc == 0 || is_gpt) {               /* GPT parsed, or GPT declined */
        fclose(f);
        if (rc != 0) plan_free(p);
        return rc;
    }
    rc = parse_mbr(f, size, p);
    fclose(f);
    if (rc != 0) plan_free(p);
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
    int rc = 1;

    idx = strtoul(idx_s, &end, 10);
    if (!end || *end) return 1;
    rc = analyze(in, &pl);
    if (rc != 0) return rc;
    for (i = 0; i < pl.nmem; i++)
        if (pl.members[i].idx == (uint32_t)idx) { m = &pl.members[i]; break; }
    if (!m) { plan_free(&pl); return 1; }   /* the FS only asks announced */
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
    if (wr(fo, "RDR1", 4) != 0 ||
        wr_le64(fo, pl.disk_size) != 0 ||
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

/* RDR1 parse state (rebuild) */
typedef struct {
    uint64_t off, len, payload_pos;      /* payload_pos: inside the recipe */
} rrange_t;

static int cmd_rebuild(const char *recipe, const char *dir, const char *out)
{
    FILE *fr = NULL, *fo = NULL, *fm = NULL;
    member_t *mem = NULL;
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
        if (rd_exact(fr, magic, 4) != 0 || memcmp(magic, "RDR1", 4) != 0)
            goto out;
    }
    if (rd_le64f(fr, &size) != 0 || !size || size > (1ull << 48)) goto out;
    if (rd_le32f(fr, &nmem) != 0 || !nmem || nmem > 65536u) goto out;
    mem = (member_t *)malloc(nmem * sizeof *mem);
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
        if (payload > rsize) goto out;            /* truncated recipe */
        if (fseeko(fr, (off_t)rng[i].len, SEEK_CUR) != 0) goto out;
    }
    if (payload != rsize) goto out;               /* trailing garbage */
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
    /* pass 2: write — truncate to disk_size, recipe ranges verbatim */
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
            goto out;                             /* size mismatch: exit 1 */
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
    /* the recipe layout strip writes: header, then (16B hdr + payload)
     * per range in offset order — RECIPE src_off's index that blob */
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
            kind = 0;                             /* RECIPE */
            idx = 0;
            src_off = src + 16;                   /* past the range header */
            src += 16 + len;
            ri++;
        } else {
            orig_off = pl.members[mi].off;
            len = pl.members[mi].len;
            kind = 1;                             /* MEMBER */
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


/* rawdisk's main() allocates the 8 MiB streaming window once per process; the
 * forked worker child needs the same. It leaves through _exit(), so there is
 * nothing to free -- the window dies with the child. */
static int ivpack_glue_rawdisk(void)
{
    if (!g_buf) g_buf = (uint8_t *)malloc(COPY_BUF);
    return g_buf ? 0 : IVPACK_RC_ERROR;
}

/* ---- ivpack plugin C ABI export (ADR-007) --------------------------------
 * Built as librawdisk.so with -DIVPACK_SHARED_LIB -fPIC -shared; the fork
 * guard in ivpack_impl.h keeps the CLI's exit(3)=decline / exit(1)=error
 * contract intact inside a long-lived worker. See ivpack_impl.h. */
static const ivpack_desc s_rawdisk_desc = {
    IVPACK_API_VERSION, "rawdisk", "1.0.0", "containerpack", 0
};

const ivpack_desc *ivpack_get_desc(void) { return &s_rawdisk_desc; }

IVPACK_CANON_CALLS(rawdisk)

IVPACK_DEFINE_CONTAINER_CMD(rawdisk, ivpack_glue_rawdisk())

IVPACK_DEFINE_CONTAINER_ESTIMATE(rawdisk, ivpack_glue_rawdisk(),
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
#endif /* !IVPACK_SHARED_LIB */

