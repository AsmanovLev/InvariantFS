/*
 * qcow2.c — the qcow2 containerpack helper (InvariantFS WP16a + WP16b ABI
 * v1.1).
 *
 * Decomposes QEMU qcow2 disk images (v2/v3, unencrypted, no backing file,
 * no internal snapshots, no compressed clusters) into:
 *   - member idx 1 ("diskimg"): the concatenation of all ALLOCATED guest
 *     data clusters in L1/L2 table order (= guest address order), full
 *     cluster_size bytes each — i.e. the raw disk image up to the last
 *     allocated cluster, ready for the rawdisk pack to decompose further
 *     (nested containers);
 *   - recipe: a small pack-owned layout header (Q2R1, below) + every other
 *     byte of the file verbatim (header + extensions, L1 table, L2 tables,
 *     refcount table + blocks, free-cluster junk, trailing junk), in file
 *     order.
 *
 * QCOW2 layout (offsets verified empirically against qemu-img 10.2.2
 * output; ALL multi-byte integers in the file are BIG-ENDIAN):
 *
 *   0x00  4   magic "QFI\xFB" (51 46 49 FB) — the sniff
 *   0x04 u32  version: 2 or 3 accepted (1 = qcow1, different format: refuse)
 *   0x08 u64  backing_file_offset — any nonzero = DECLINED (the guest bytes
 *             are meaningless without the parent; decomposition would
 *             silently serve a lie)
 *   0x10 u32  backing_file_size   — likewise must be 0
 *   0x14 u32  cluster_bits, accepted 9..21 (cluster_size 512 B .. 2 MiB)
 *   0x18 u64  virtual disk size (informational; the tables, not this field,
 *             decide which clusters exist)
 *   0x20 u32  crypt_method — must be 0 (1 = AES, 2 = LUKS: refuse)
 *   0x24 u32  l1_size (L1 entries)
 *   0x28 u64  l1_table_offset (cluster-aligned)
 *   0x30 u64  refcount_table_offset (cluster-aligned)
 *   0x38 u32  refcount_table_clusters (>= 1)
 *   0x3C u32  nb_snapshots — must be 0 (internal snapshots pin clusters in
 *             ways this decomposition does not model: refuse)
 *   0x40 u64  snapshots_offset (ignored when nb_snapshots == 0)
 *   --- v3 only (version 2 header ends at 0x48, refcount_bits implied 16):
 *   0x48 u64  incompatible_features — must be 0, refuse ANY set bit (dirty
 *             (0), corrupt (1), external data file (2), compression type
 *             (3), extended L2 (4) all change the meaning or the layout of
 *             what follows; "refuse non-zero to be safe")
 *   0x50 u64  compatible_features — accepted unconditionally (layout-
 *             neutral by definition; e.g. lazy_refcounts bit 0)
 *   0x58 u64  autoclear_features  — accepted unconditionally (same)
 *   0x60 u32  refcount_order — must be 4, i.e. refcount_bits = 16 (the task
 *             matrix; v2 implies 16 by construction)
 *   0x64 u32  header_length — must be >= 104 and <= file size; header
 *             extensions (e.g. qemu's feature-name table, verified at 0x70
 *             with header_length=0x70) live in [header_length, ...), are
 *             NOT parsed, and ride in the recipe verbatim
 *
 * L1 table: l1_size x u64 at l1_table_offset. Entry 0 = no L2 table for the
 * guest range (all-unallocated). Otherwise bit 63 = QCOW_OFLAG_COPIED
 * (layout-neutral, ignored) and bits 0..62 = the L2 table's cluster-aligned
 * offset.
 *
 * L2 table: cluster_size/8 x u64 at that offset; one entry per guest
 * cluster. Entry 0 = unallocated (NO bytes in the file — qcow2 is sparse
 * by construction). Otherwise:
 *   bit 62 (QCOW_OFLAG_COMPRESSED) — DECLINE the image (compressed clusters
 *     use a different sector-count layout this pack does not model);
 *   bit 0 (QCOW_OFLAG_ZERO, v3+) — a known-zero cluster: NOT a member, its
 *     in-file bytes (if any — preallocated zero clusters keep their extent,
 *     verified: write -z over an allocated cluster yields entry
 *     0x8000000000050001, offset kept, ZERO set, stale bytes still in the
 *     file) stay RECIPE bytes verbatim. In a v2 image the ZERO bit is
 *     reserved: a set bit is declined as unprovable;
 *   else bits 1..61 (mask 0x3FFFFFFFFFFFFFFE) = the data cluster's offset,
 *     which must be nonzero, cluster-aligned, and fully inside the file.
 *
 * Refcount structures are validated for EXTENT sanity only (table covers
 * its clusters, every nonzero table entry points at a full in-file
 * cluster); their contents are qemu's consistency business, not this
 * pack's — every refcount byte rides in the recipe verbatim regardless.
 *
 * Validation (any failure = decline, exit 3): file >= 104 bytes; magic;
 * version 2|3; no backing file; cluster_bits 9..21; crypt_method 0;
 * nb_snapshots 0; v3: incompat 0 / refcount_order 4 / 104 <= header_length
 * <= file_size; l1_size >= 1 with l1_table_offset nonzero, cluster-aligned
 * and [off, off + 8*l1_size) inside the file; refcount_table_clusters >= 1
 * with the table nonzero/aligned/in-file; every nonzero refcount-table
 * entry aligned and its block fully in-file; every present L2 table
 * nonzero/aligned and fully in-file; every data cluster nonzero/aligned
 * and fully in-file; no QCOW_OFLAG_COMPRESSED anywhere; no ZERO bit in v2;
 * data-cluster offsets pairwise distinct (a cluster referenced twice is
 * corrupt, and would overlap in the map); n_alloc >= 1 (an image with zero
 * allocated clusters is declined — nothing to decompose); n_alloc <= 2^21
 * (2M clusters — parse-table memory cap; FS admission binds real images
 * far lower); and the exact coalesced MRMP entry count stays under the FS
 * cap (4*65536+4 = 262148).
 *
 * Recipe format (pack-owned; the FS never parses it) — "Q2R1":
 *   [0]   4B   "Q2R1"
 *   [4]  u32LE n_ext          (== n_alloc, one extent per allocated cluster)
 *   [8]  u64LE file_size      (original container size)
 *   [16] u64LE member_size    (== n_ext * cluster_size)
 *   [24] u32LE cluster_bits
 *   [28] u32LE reserved (0)
 *   [32] n_ext x 16B { u64LE file_off, u64LE mem_off }  — FILE order;
 *        mem_off is the cluster's rank in the guest-ordered member stream
 *        times cluster_size
 *   [32 + 16*n_ext ...]  the verbatim gap bytes, in file order (every byte
 *        of the original that is not a member extent: header + extensions,
 *        L1/L2 tables, refcount structures, free-cluster junk, tail)
 * rebuild re-derives the layout from this table alone (no qcow2 parsing at
 * rebuild time), validates it strictly (sizes, disjoint sorted extents,
 * mem_off a permutation of the member stream), then splices member bytes
 * at the cluster slots. Bit-exact by construction, junk-in-a-hole included
 * (holes are never seek-skipped; the recipe carries their bytes).
 *
 * Map (MRMP, FS-owned): RECIPE runs for the gaps (src_off into the recipe
 * blob at 32 + 16*n_ext + accumulated gap bytes) + MEMBER runs for the
 * data clusters, coalesced when consecutive in the file AND in the member
 * stream (a sequential qemu-img convert coalesces to a handful of entries;
 * a scrambled layout costs up to 2*n_alloc+1, bounded by the parse-time
 * cap check). The entries exactly partition [0, file_size).
 *
 * Commands (fixed argv, no shell; exit 0 = ok, 3 = decline/fail):
 *   enumerate <in> <out>           "1<TAB>diskimg<TAB><n_alloc*cs>\n"
 *   extract   <in> <idx> <out>     the concatenated guest-order cluster stream
 *   strip     <in> <out>           the Q2R1 recipe
 *   rebuild   <recipe> <dir> <out> original, bit-exact
 *   map       <in> <out>           MRMP: RECIPE gaps + MEMBER cluster runs
 *   estimate  <in>                 prints member bytes + 64 MiB
 *
 * Everything streams through a 4 MiB window; no member ever needs to fit in
 * memory here (the parse tables are 24 B per allocated cluster, capped).
 *
 * C11, libc only. No stdout/stderr chatter (the FS pipes them to /dev/null;
 * estimate's stdout is the single sanctioned exception).
 */

#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L   /* pread, fstat, fileno under -std=c11 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>
/* ADR-007: this file doubles as a .so plugin (libqcow2.so, built with
 * -fPIC -shared -DIVPACK_SHARED_LIB). The plugin glue is compiled in BOTH
 * builds -- the CLI build simply never calls it, and main() is what
 * -DIVPACK_SHARED_LIB drops -- so the .so and the CLI can never drift apart. */
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
#if __has_include("deflate_repro.h")
#include "deflate_repro.h"
#elif __has_include("../../../src/codecs/deflate_repro.h")
#include "../../../src/codecs/deflate_repro.h"
#endif


/* ---- qcow2 header field offsets (all values in the file are BE) -------- */
#define QC_VERSION_OFF    0x04u
#define QC_BACKING_OFF    0x08u   /* u64 backing_file_offset */
#define QC_BACKINGSZ_OFF  0x10u   /* u32 backing_file_size */
#define QC_CLUSBITS_OFF   0x14u
#define QC_CRYPT_OFF      0x20u
#define QC_L1SIZE_OFF     0x24u
#define QC_L1OFF_OFF      0x28u
#define QC_RTOFF_OFF      0x30u
#define QC_RTCLUS_OFF     0x38u
#define QC_NSNAP_OFF      0x3Cu
#define QC_INCOMPAT_OFF   0x48u   /* v3 only */
#define QC_REFCORD_OFF    0x60u   /* v3 only */
#define QC_HDRLEN_OFF     0x64u   /* v3 only */
#define QC_HDR_NEED       104u    /* every field we read lives below this */

#define QC_MIN_CLUSBITS   9u                      /* 512 B */
#define QC_MAX_CLUSBITS   21u                     /* 2 MiB */
#define QC_REFC_ORDER_16  4u                      /* refcount_bits == 16 */

#define QCOW_OFLAG_COPIED     0x8000000000000000ull
#define QCOW_OFLAG_COMPRESSED 0x4000000000000000ull
#define QCOW_OFLAG_ZERO       0x0000000000000001ull
#define QC_L1_OFFMASK         0x3fffffffffffffffull   /* drop COPIED */
#define QC_L2_OFFMASK         0x3ffffffffffffffeull   /* drop COPIED|ZERO */

#define QC_MAX_ALLOC     (1u << 21)      /* parse-table cap: 2M clusters */
#define MAP_MAX_ENTS     262148u         /* FS: 4 * CPACK_MAX_MEMBERS + 4 */
#define EST_MARGIN       (64ull << 20)   /* the estimate's flat margin */
#define COPY_CAP         (4u << 20)      /* streaming window (<= 8 MiB) */
#define L1_WIN           4096u           /* L1 entries per read window */

#define MEMBER_IDX       1u
#define MEMBER_SNAME     "diskimg"

/* recipe (Q2R1/Q2R2/Q2R3) constants */
#define QR_MAGIC         "Q2R1"
#define QR2_MAGIC        "Q2R2"
#define QR3_MAGIC        "Q2R3"
#define QR_HDR_LEN       32u             /* fixed header before the table */
#define QR_ENT_LEN       16u             /* { u64 file_off, u64 mem_off } */
#define QR2_ENT_LEN      20u             /* { u64 file_off, u64 mem_off, u32 csize } */
#define QR3_ENT_LEN      24u             /* { u64 file_off, u64 mem_off, u32 csize, u8 repro, u8 level, u8 mem, u8 strat } */

typedef struct {
    uint64_t off;      /* file offset of the data cluster */
    uint32_t rank;     /* guest-stream rank: mem_off = rank * cluster_size */
    uint32_t csize;    /* 0 = uncompressed cluster of cs bytes; >0 = compressed stream byte length */
    uint8_t  repro;    /* 1 = reproducible via deflate_repro; 0 = verbatim in recipe gap */
    uint8_t  level;    /* deflate level */
    uint8_t  mem;      /* deflate memLevel */
    uint8_t  strat;    /* deflate strategy */
} qc_ext;

typedef struct {
    uint64_t off;      /* file offset of cluster */
    uint32_t csize;    /* 0 = uncompressed, >0 = compressed */
} qc_entry;

typedef struct {
    uint64_t file_size;
    uint64_t cs;            /* cluster size (1 << cluster_bits) */
    uint64_t member_size;   /* n_alloc * cs */
    uint32_t cluster_bits;
    uint32_t version;
    uint32_t n_alloc;
    qc_entry *alloc;        /* n_alloc data clusters, GUEST order */
    qc_ext   *fext;         /* n_alloc extents, FILE order (sorted by off) */
} qc_img;

static uint32_t get32be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint64_t get64be(const uint8_t *p)
{
    return (uint64_t)get32be(p) << 32 | (uint64_t)get32be(p + 4);
}

static uint32_t get32le(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;            /* the FS's own convention: pack formats are LE */
}

static uint64_t get64le(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void put32le(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64le(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static int pread_full(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = (uint8_t *)buf;
    while (len) {
        ssize_t r = pread(fd, p, len, (off_t)off);
        if (r <= 0) return -1;
        p += (size_t)r;
        len -= (size_t)r;
        off += (uint64_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w <= 0) return -1;
        p += (size_t)w;
        len -= (size_t)w;
    }
    return 0;
}

static void qc_free(qc_img *v)
{
    free(v->alloc);
    free(v->fext);
    memset(v, 0, sizeof *v);
}

static int ext_cmp(const void *a, const void *b)
{
    uint64_t x = ((const qc_ext *)a)->off, y = ((const qc_ext *)b)->off;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int u64_cmp(const void *a, const void *b)
{
    uint64_t x, y;
    memcpy(&x, a, 8);
    memcpy(&y, b, 8);
    return x < y ? -1 : x > y ? 1 : 0;
}

/*
 * The refcount-structure extent check (task rule: "L1/L2/refcount extents
 * past EOF" = decline). Contents are NOT interpreted: every refcount byte
 * is recipe either way. Table: rt_clusters full clusters at rt_off; every
 * nonzero entry must be an aligned, fully in-file refcount-block cluster.
 */
static int qc_check_refcounts(int fd, uint64_t file_size, uint64_t cs,
                              uint64_t rt_off, uint32_t rt_clusters)
{
    uint8_t *win;
    uint64_t total, base;
    int rc = -1;

    if (!rt_off || (rt_off & (cs - 1)) != 0) return -1;
    if (rt_clusters < 1) return -1;
    if (rt_off > file_size ||
        (uint64_t)rt_clusters * cs > file_size - rt_off) return -1;
    win = (uint8_t *)malloc(COPY_CAP);
    if (!win) return -1;
    total = (uint64_t)rt_clusters * cs;
    for (base = 0; base < total; base += COPY_CAP) {
        size_t want = (total - base) > COPY_CAP ? COPY_CAP
                                                : (size_t)(total - base);
        size_t i;
        if (pread_full(fd, win, want, rt_off + base) != 0) goto out;
        for (i = 0; i + 8 <= want; i += 8) {
            uint64_t e = get64be(win + i);
            if (!e) continue;
            if ((e & (cs - 1)) != 0 || e > file_size || cs > file_size - e)
                goto out;
        }
    }
    rc = 0;
out:
    free(win);
    return rc;
}

/*
 * Parse + fully validate a qcow2 image from an fd. On success v owns the
 * alloc table (guest order) and the fext table (file order, duplicates
 * refused, the exact coalesced map-entry count within the FS cap).
 * 0 = ok, -1 = decline (exit 3 at the command layer).
 */
static int qc_parse_fd(int fd, uint64_t file_size, qc_img *v)
{
    uint8_t hdr[QC_HDR_NEED];
    uint8_t *l1w = NULL, *l2t = NULL;
    uint64_t l1_off, rt_off, l2_per, i64;
    uint32_t version, cluster_bits, l1_size, rt_clusters, cap = 0;
    uint32_t i;
    int rc = -1;

    memset(v, 0, sizeof *v);
    if (file_size < QC_HDR_NEED) return -1;
    v->file_size = file_size;
    if (pread_full(fd, hdr, sizeof hdr, 0) != 0) return -1;

    if (memcmp(hdr, "QFI\xFB", 4) != 0) return -1;
    version = get32be(hdr + QC_VERSION_OFF);
    if (version != 2 && version != 3) return -1;
    v->version = version;
    if (get64be(hdr + QC_BACKING_OFF) != 0) return -1;   /* backing file */
    if (get32be(hdr + QC_BACKINGSZ_OFF) != 0) return -1;
    if (get32be(hdr + QC_CRYPT_OFF) != 0) return -1;     /* encryption */
    if (get32be(hdr + QC_NSNAP_OFF) != 0) return -1;     /* snapshots */

    cluster_bits = get32be(hdr + QC_CLUSBITS_OFF);
    if (cluster_bits < QC_MIN_CLUSBITS || cluster_bits > QC_MAX_CLUSBITS)
        return -1;
    v->cluster_bits = cluster_bits;
    v->cs = 1ull << cluster_bits;

    if (version == 3) {
        uint64_t hdr_len;
        if (get64be(hdr + QC_INCOMPAT_OFF) != 0) return -1;  /* unknown
            incompatible features (dirty/corrupt/external-data-file/
            compression-type/extended-L2): refuse non-zero to be safe */
        if (get32be(hdr + QC_REFCORD_OFF) != QC_REFC_ORDER_16) return -1;
        hdr_len = get32be(hdr + QC_HDRLEN_OFF);
        if (hdr_len < QC_HDR_NEED || hdr_len > file_size) return -1;
    }
    /* v2: refcount_bits is 16 by construction (no refcount_order field) */

    l1_size = get32be(hdr + QC_L1SIZE_OFF);
    l1_off = get64be(hdr + QC_L1OFF_OFF);
    if (l1_size < 1) return -1;                 /* empty by construction */
    if (!l1_off || (l1_off & (v->cs - 1)) != 0) return -1;
    if (l1_off > file_size || 8ull * l1_size > file_size - l1_off)
        return -1;

    rt_off = get64be(hdr + QC_RTOFF_OFF);
    rt_clusters = get32be(hdr + QC_RTCLUS_OFF);
    if (qc_check_refcounts(fd, file_size, v->cs, rt_off, rt_clusters) != 0)
        return -1;

    /* L1/L2 walk, guest order (L1 index order, L2 index order within each
     * table) = the member stream order. alloc[] grows geometrically. */
    l2_per = v->cs / 8;
    l1w = (uint8_t *)malloc(L1_WIN * 8);
    l2t = (uint8_t *)malloc(v->cs);
    if (!l1w || !l2t) goto out;
    for (i64 = 0; i64 < l1_size; i64 += L1_WIN) {
        uint32_t n = (l1_size - i64) > L1_WIN ? L1_WIN
                                              : (uint32_t)(l1_size - i64);
        uint32_t k;
        if (pread_full(fd, l1w, (size_t)n * 8, l1_off + i64 * 8) != 0)
            goto out;
        for (k = 0; k < n; k++) {
            uint64_t l1e = get64be(l1w + (size_t)k * 8);
            uint64_t l2_off, j;
            if (!l1e) continue;                 /* absent L2: all-unallocated */
            l2_off = l1e & QC_L1_OFFMASK;
            if (!l2_off || (l2_off & (v->cs - 1)) != 0 ||
                l2_off > file_size || v->cs > file_size - l2_off)
                goto out;
            if (pread_full(fd, l2t, v->cs, l2_off) != 0) goto out;
            for (j = 0; j < l2_per; j++) {
                uint64_t le = get64be(l2t + (size_t)j * 8);
                uint64_t off;
                uint32_t csize = 0;
                if (!le) continue;              /* unallocated: no bytes */
                if (le & QCOW_OFLAG_COMPRESSED) {
                    /* QCOW2 compressed cluster: sector count occupies (cluster_bits - 8) bits */
                    unsigned csize_shift = 62u - (v->cluster_bits - 8u);
                    uint64_t csize_mask = (1ull << (v->cluster_bits - 8u)) - 1ull;
                    uint64_t offset_mask = (1ull << csize_shift) - 1ull;
                    size_t dataSize = (size_t)(((le >> csize_shift) & csize_mask) + 1u) * 512u;
                    off = le & offset_mask;
                    if (off > file_size) {
                        fprintf(stderr, "qcow2: compressed off out of bounds\n");
                        goto out;
                    }
                    if (dataSize > file_size - off)
                        dataSize = (size_t)(file_size - off);
                    /* Read compressed stream to determine exact consumed bytes */
                    uint8_t *comp_peek = (uint8_t *)malloc(dataSize);
                    if (!comp_peek) goto out;
                    if (pread_full(fd, comp_peek, dataSize, off) != 0) {
                        fprintf(stderr, "qcow2: pread comp_peek failed\n");
                        free(comp_peek);
                        goto out;
                    }
                    z_stream zst;
                    memset(&zst, 0, sizeof zst);
                    if (inflateInit2(&zst, -12) != Z_OK) {
                        fprintf(stderr, "qcow2: inflateInit2 failed\n");
                        free(comp_peek);
                        goto out;
                    }
                    uint8_t dummy_out[512];
                    zst.next_in = comp_peek;
                    zst.avail_in = (uInt)dataSize;
                    zst.next_out = dummy_out;
                    zst.avail_out = sizeof dummy_out;
                    /* inflate until stream ends to discover exact compressed length */
                    while (zst.avail_in) {
                        int zr = inflate(&zst, Z_NO_FLUSH);
                        if (zr == Z_STREAM_END) break;
                        if (zr != Z_OK && zr != Z_BUF_ERROR) {
                            fprintf(stderr, "qcow2: inflate failed with %d\n", zr);
                            inflateEnd(&zst);
                            free(comp_peek);
                            goto out;
                        }
                        zst.next_out = dummy_out;
                        zst.avail_out = sizeof dummy_out;
                    }
                    csize = (uint32_t)(dataSize - zst.avail_in);
                    inflateEnd(&zst);
                    free(comp_peek);
                    if (csize == 0) {
                        fprintf(stderr, "qcow2: csize is 0\n");
                        goto out;
                    }
                } else if (le & QCOW_OFLAG_ZERO) {     /* known-zero cluster */
                    if (version == 2) goto out; /* reserved bit in v2 */
                    continue;                   /* stays RECIPE bytes */
                } else {
                    off = le & QC_L2_OFFMASK;
                    if (!off || (off & (v->cs - 1)) != 0 ||
                        off > file_size || v->cs > file_size - off)
                        goto out;
                    csize = 0;
                }
                if (v->n_alloc == cap) {
                    qc_entry *na;
                    if (cap >= QC_MAX_ALLOC) goto out;
                    cap = cap ? cap * 2 : 4096;
                    if (cap > QC_MAX_ALLOC) cap = QC_MAX_ALLOC;
                    na = (qc_entry *)realloc(v->alloc,
                                             (size_t)cap * sizeof *na);
                    if (!na) goto out;
                    v->alloc = na;
                }
                v->alloc[v->n_alloc].off = off;
                v->alloc[v->n_alloc].csize = csize;
                v->n_alloc++;
            }
        }
    }
    if (v->n_alloc < 1) goto out;   /* nothing to decompose: decline */
    v->member_size = (uint64_t)v->n_alloc * v->cs;

    /* FILE order: sort {off, rank} by offset; a duplicate offset is a
     * cluster referenced twice = corrupt (and unpartitionable in the map) */
    v->fext = (qc_ext *)malloc((size_t)v->n_alloc * sizeof *v->fext);
    if (!v->fext) goto out;
    for (i = 0; i < v->n_alloc; i++) {
        v->fext[i].off = v->alloc[i].off;
        v->fext[i].csize = v->alloc[i].csize;
        v->fext[i].rank = i;
    }
    qsort(v->fext, v->n_alloc, sizeof *v->fext, ext_cmp);
    for (i = 1; i < v->n_alloc; i++)
        if (v->fext[i].off == v->fext[i - 1].off) goto out;

    /* the exact coalesced MRMP entry count must stay under the FS cap */
    {
        uint64_t n_ents = 0, pos = 0;
        i = 0;
        while (i < v->n_alloc) {
            uint64_t run_off = v->fext[i].off;
            uint64_t run_len = v->fext[i].csize ? (uint64_t)v->fext[i].csize : v->cs;
            uint64_t run_mem = (uint64_t)v->fext[i].rank * v->cs;
            i++;
            while (i < v->n_alloc && !v->fext[i - 1].csize && !v->fext[i].csize &&
                   v->fext[i].off == run_off + run_len &&
                   (uint64_t)v->fext[i].rank * v->cs == run_mem + run_len) {
                run_len += v->cs;
                i++;
            }
            if (run_off > pos) n_ents++;        /* RECIPE gap */
            n_ents++;                           /* MEMBER run */
            pos = run_off + run_len;
        }
        if (pos < file_size) n_ents++;          /* trailing RECIPE */
        if (n_ents > MAP_MAX_ENTS) goto out;
    }
    rc = 0;
out:
    free(l1w);
    free(l2t);
    if (rc) qc_free(v);
    return rc;
}

static int qc_parse(const char *path, qc_img *v)
{
    struct stat st;
    int fd, rc;

    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    rc = qc_parse_fd(fd, (uint64_t)st.st_size, v);
    close(fd);
    return rc;
}

/* copy [off, off+len) of in_fd to out_fd, streaming through buf */
static int copy_range(int in_fd, int out_fd, uint64_t off, uint64_t len,
                      uint8_t *buf)
{
    while (len) {
        size_t want = len > COPY_CAP ? COPY_CAP : (size_t)len;
        if (pread_full(in_fd, buf, want, off) != 0) return -1;
        if (write_full(out_fd, buf, want) != 0) return -1;
        off += want;
        len -= want;
    }
    return 0;
}

static int open_out(const char *path)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
}

/* ---- enumerate ---------------------------------------------------------- */

static int cmd_enumerate(const char *in, const char *out)
{
    qc_img v;
    FILE *f;
    int rc = 3;

    if (qc_parse(in, &v) != 0) return 3;
    f = fopen(out, "w");
    if (f) {
        fprintf(f, "%u\t%s\t%llu\n", MEMBER_IDX, MEMBER_SNAME,
                (unsigned long long)v.member_size);
        if (fclose(f) == 0) rc = 0;
    }
    qc_free(&v);
    return rc;
}

/* ---- extract ------------------------------------------------------------ */

static int cmd_extract(const char *in, const char *idx_s, const char *out)
{
    qc_img v;
    uint8_t *buf = NULL;
    char *endp = NULL;
    uint32_t i;
    int fd_in = -1, fd_out = -1, rc = 3;

    if (strtoul(idx_s, &endp, 10) != MEMBER_IDX || !endp || *endp)
        return 3;
    if (qc_parse(in, &v) != 0) return 3;
    fd_in = open(in, O_RDONLY);
    fd_out = open_out(out);
    buf = (uint8_t *)malloc(COPY_CAP);
    if (fd_in < 0 || fd_out < 0 || !buf) goto out;
    /* the member stream = allocated clusters in GUEST (table walk) order */
    for (i = 0; i < v.n_alloc; i++) {
        if (v.alloc[i].csize) {
            /* Decompress zlib cluster into full cluster_size member bytes */
            uint8_t *comp_buf = (uint8_t *)malloc(v.alloc[i].csize);
            uint8_t *dec_buf = (uint8_t *)malloc(v.cs);
            if (!comp_buf || !dec_buf) {
                free(comp_buf); free(dec_buf); goto out;
            }
            if (pread_full(fd_in, comp_buf, v.alloc[i].csize, v.alloc[i].off) != 0) {
                free(comp_buf); free(dec_buf); goto out;
            }
            z_stream zst;
            memset(&zst, 0, sizeof zst);
            if (inflateInit2(&zst, -12) != Z_OK) {
                free(comp_buf); free(dec_buf); goto out;
            }
            zst.next_in = comp_buf;
            zst.avail_in = (uInt)v.alloc[i].csize;
            zst.next_out = dec_buf;
            zst.avail_out = (uInt)v.cs;
            int zr = inflate(&zst, Z_FINISH);
            inflateEnd(&zst);
            free(comp_buf);
            if (zr != Z_STREAM_END && zr != Z_OK) {
                free(dec_buf); goto out;
            }
            if (write_full(fd_out, dec_buf, v.cs) != 0) {
                free(dec_buf); goto out;
            }
            free(dec_buf);
        } else {
            if (copy_range(fd_in, fd_out, v.alloc[i].off, v.cs, buf) != 0) goto out;
        }
    }
    rc = 0;
out:
    if (fd_in >= 0) close(fd_in);
    if (fd_out >= 0 && close(fd_out) != 0) rc = 3;
    free(buf);
    qc_free(&v);
    return rc;
}

/* ---- strip (Q2R1 header + file-order extent table + the gap bytes) ----- */

static int cmd_strip(const char *in, const char *out)
{
    qc_img v;
    uint8_t hdr[QR_HDR_LEN];
    uint8_t *tab = NULL, *buf = NULL;
    uint64_t pos = 0;
    uint32_t i;
    int fd_in = -1, fd_out = -1, rc = 3;

    if (qc_parse(in, &v) != 0) return 3;
    tab = (uint8_t *)malloc((size_t)v.n_alloc * QR2_ENT_LEN);
    buf = (uint8_t *)malloc(COPY_CAP);
    if (!tab || !buf) goto out_free;
    for (i = 0; i < v.n_alloc; i++) {
        put64le(tab + (size_t)i * QR2_ENT_LEN, v.fext[i].off);
        put64le(tab + (size_t)i * QR2_ENT_LEN + 8,
                (uint64_t)v.fext[i].rank * v.cs);
        put32le(tab + (size_t)i * QR2_ENT_LEN + 16, v.fext[i].csize);
    }
    fd_in = open(in, O_RDONLY);
    fd_out = open_out(out);
    if (fd_in < 0 || fd_out < 0) goto out;
    memcpy(hdr, QR2_MAGIC, 4);
    put32le(hdr + 4, v.n_alloc);
    put64le(hdr + 8, v.file_size);
    put64le(hdr + 16, v.member_size);
    put32le(hdr + 24, v.cluster_bits);
    put32le(hdr + 28, 0);
    if (write_full(fd_out, hdr, sizeof hdr) != 0 ||
        write_full(fd_out, tab, (size_t)v.n_alloc * QR2_ENT_LEN) != 0)
        goto out;
    /* the gap bytes: every non-member byte, in file order (junk-filled
     * holes and compressed cluster bytes included) */
    for (i = 0; i < v.n_alloc; i++) {
        uint64_t ext = v.fext[i].off;
        uint64_t elen = v.fext[i].csize ? (uint64_t)v.fext[i].csize : v.cs;
        if (ext > pos && copy_range(fd_in, fd_out, pos, ext - pos, buf) != 0)
            goto out;
        if (v.fext[i].csize) {
            /* compressed cluster: written into recipe gap so rebuild reproduces it verbatim */
            if (copy_range(fd_in, fd_out, ext, elen, buf) != 0)
                goto out;
        }
        pos = ext + elen;
    }
    if (v.file_size > pos &&
        copy_range(fd_in, fd_out, pos, v.file_size - pos, buf) != 0)
        goto out;
    rc = 0;
out:
    if (fd_in >= 0) close(fd_in);
    if (fd_out >= 0 && close(fd_out) != 0) rc = 3;
out_free:
    free(tab);
    free(buf);
    qc_free(&v);
    return rc;
}

/* ---- map (MRMP: RECIPE gaps + coalesced MEMBER runs partition the file) - */

/* In-memory buffer version of map: reads recipe file or buffer, writes MRMP directly to out_buf */
int cmd_map_mem(const char *in_path, uint8_t *out_buf, size_t out_cap, size_t *out_len)
{
    qc_img v;
    uint8_t *ent = NULL;
    uint64_t pos = 0, recipe_off, n_ents = 0;
    uint32_t i;
    int rc = 3;
    uint8_t hdr[8];

    if (!out_buf || out_cap < 8 || !out_len) return 3;
    if (qc_parse(in_path, &v) != 0) return 3;

    /* count the exact entry number first (the parse already capped it) */
    i = 0;
    while (i < v.n_alloc) {
        uint64_t run_off = v.fext[i].off;
        uint64_t run_len = v.fext[i].csize ? (uint64_t)v.fext[i].csize : v.cs;
        uint64_t run_mem = (uint64_t)v.fext[i].rank * v.cs;
        int is_comp = (v.fext[i].csize != 0);
        i++;
        while (i < v.n_alloc && !is_comp && !v.fext[i].csize &&
               v.fext[i].off == run_off + run_len &&
               (uint64_t)v.fext[i].rank * v.cs == run_mem + run_len) {
            run_len += v.cs;
            i++;
        }
        if (run_off > pos) n_ents++;
        n_ents++;
        pos = run_off + run_len;
    }
    if (pos < v.file_size) n_ents++;

    size_t total_needed = 8 + (size_t)n_ents * 29;
    if (out_cap < total_needed) {
        qc_free(&v);
        return 3; /* buffer too small */
    }

    ent = (uint8_t *)malloc((size_t)n_ents * 29);
    if (!ent) { qc_free(&v); return 3; }

    pos = 0;
    n_ents = 0;
    recipe_off = QR_HDR_LEN + (uint64_t)v.n_alloc * QR2_ENT_LEN;
    i = 0;
    while (i < v.n_alloc) {
        uint64_t run_off = v.fext[i].off;
        uint64_t run_len = v.fext[i].csize ? (uint64_t)v.fext[i].csize : v.cs;
        uint64_t run_mem = (uint64_t)v.fext[i].rank * v.cs;
        uint8_t *p;
        int is_comp = (v.fext[i].csize != 0);
        i++;
        while (i < v.n_alloc && !is_comp && !v.fext[i].csize &&
               v.fext[i].off == run_off + run_len &&
               (uint64_t)v.fext[i].rank * v.cs == run_mem + run_len) {
            run_len += v.cs;
            i++;
        }
        if (run_off > pos) {                    /* RECIPE gap */
            p = ent + (size_t)n_ents * 29;
            put64le(p, pos);
            put64le(p + 8, run_off - pos);
            p[16] = 0;                          /* kind RECIPE */
            put32le(p + 17, 0);
            put64le(p + 21, recipe_off);
            recipe_off += run_off - pos;
            n_ents++;
        }
        p = ent + (size_t)n_ents * 29;
        put64le(p, run_off);
        put64le(p + 8, run_len);
        if (is_comp) {
            p[16] = 0;
            put32le(p + 17, 0);
            put64le(p + 21, recipe_off);
            recipe_off += run_len;
        } else {
            p[16] = 1;
            put32le(p + 17, MEMBER_IDX);
            put64le(p + 21, run_mem);
        }
        n_ents++;
        pos = run_off + run_len;
    }
    if (v.file_size > pos) {                    /* trailing RECIPE gap */
        uint8_t *p = ent + (size_t)n_ents * 29;
        put64le(p, pos);
        put64le(p + 8, v.file_size - pos);
        p[16] = 0;
        put32le(p + 17, 0);
        put64le(p + 21, recipe_off);
        n_ents++;
    }

    memcpy(hdr, "MRMP", 4);
    put32le(hdr + 4, (uint32_t)n_ents);
    memcpy(out_buf, hdr, 8);
    memcpy(out_buf + 8, ent, (size_t)n_ents * 29);
    *out_len = total_needed;
    rc = 0;

    free(ent);
    qc_free(&v);
    return rc;
}

static int cmd_map(const char *in, const char *out)
{
    qc_img v;
    uint8_t *ent = NULL;
    uint64_t pos = 0, recipe_off, n_ents = 0;
    uint32_t i;
    int fd_out = -1, rc = 3;
    uint8_t hdr[8];

    if (qc_parse(in, &v) != 0) return 3;
    /* count the exact entry number first (the parse already capped it) */
    i = 0;
    while (i < v.n_alloc) {
        uint64_t run_off = v.fext[i].off;
        uint64_t run_len = v.fext[i].csize ? (uint64_t)v.fext[i].csize : v.cs;
        uint64_t run_mem = (uint64_t)v.fext[i].rank * v.cs;
        int is_comp = (v.fext[i].csize != 0);
        i++;
        while (i < v.n_alloc && !is_comp && !v.fext[i].csize &&
               v.fext[i].off == run_off + run_len &&
               (uint64_t)v.fext[i].rank * v.cs == run_mem + run_len) {
            run_len += v.cs;
            i++;
        }
        if (run_off > pos) n_ents++;
        n_ents++;
        pos = run_off + run_len;
    }
    if (pos < v.file_size) n_ents++;
    ent = (uint8_t *)malloc((size_t)n_ents * 29);
    if (!ent) goto out_free;

    pos = 0;
    n_ents = 0;
    recipe_off = QR_HDR_LEN + (uint64_t)v.n_alloc * QR2_ENT_LEN;
    i = 0;
    while (i < v.n_alloc) {
        uint64_t run_off = v.fext[i].off;
        uint64_t run_len = v.fext[i].csize ? (uint64_t)v.fext[i].csize : v.cs;
        uint64_t run_mem = (uint64_t)v.fext[i].rank * v.cs;
        uint8_t *p;
        int is_comp = (v.fext[i].csize != 0);
        i++;
        while (i < v.n_alloc && !is_comp && !v.fext[i].csize &&
               v.fext[i].off == run_off + run_len &&
               (uint64_t)v.fext[i].rank * v.cs == run_mem + run_len) {
            run_len += v.cs;
            i++;
        }
        if (run_off > pos) {                    /* RECIPE gap */
            p = ent + (size_t)n_ents * 29;
            put64le(p, pos);
            put64le(p + 8, run_off - pos);
            p[16] = 0;                          /* kind RECIPE */
            put32le(p + 17, 0);
            put64le(p + 21, recipe_off);
            recipe_off += run_off - pos;
            n_ents++;
        }
        p = ent + (size_t)n_ents * 29;
        put64le(p, run_off);
        put64le(p + 8, run_len);
        if (is_comp) {
            /* Compressed cluster: raw bytes ride in RECIPE gap for seekable read */
            p[16] = 0;                          /* kind RECIPE */
            put32le(p + 17, 0);
            put64le(p + 21, recipe_off);
            recipe_off += run_len;
        } else {
            /* Uncompressed cluster: member stream */
            p[16] = 1;                          /* kind MEMBER */
            put32le(p + 17, MEMBER_IDX);
            put64le(p + 21, run_mem);
        }
        n_ents++;
        pos = run_off + run_len;
    }
    if (v.file_size > pos) {                    /* trailing RECIPE gap */
        uint8_t *p = ent + (size_t)n_ents * 29;
        put64le(p, pos);
        put64le(p + 8, v.file_size - pos);
        p[16] = 0;
        put32le(p + 17, 0);
        put64le(p + 21, recipe_off);
        n_ents++;
    }
    fd_out = open_out(out);
    if (fd_out < 0) { free(ent); qc_free(&v); return 3; }
    memcpy(hdr, "MRMP", 4);
    put32le(hdr + 4, (uint32_t)n_ents);
    if (write_full(fd_out, hdr, 8) != 0 ||
        write_full(fd_out, ent, (size_t)n_ents * 29) != 0) {
        close(fd_out);
        free(ent);
        qc_free(&v);
        return 3;
    }
    rc = (close(fd_out) == 0) ? 0 : 3;
    free(ent);
    qc_free(&v);
    return rc;
out_free:
    free(ent);
    qc_free(&v);
    return 3;
}

/* ---- rebuild ------------------------------------------------------------ */

/*
 * rebuild re-derives the layout from the Q2R1 recipe alone (no qcow2
 * parsing): the recipe is self-describing, so a future qcow2 version bump
 * cannot break already-stored decompositions. The table is validated
 * strictly; ANY inconsistency (or a member whose size disagrees) fails
 * loudly.
 */
static int cmd_rebuild(const char *recipe, const char *dir, const char *out)
{
    struct stat rst, mst;
    uint8_t hdr[QR_HDR_LEN];
    uint8_t *tab = NULL, *chk = NULL, *buf = NULL;
    uint64_t file_size, member_size, cs, gap_off, pos = 0;
    uint32_t n_ext, cluster_bits, i;
    char mpath[4096];
    int fd_r = -1, fd_m = -1, fd_out = -1, rc = 3;
    int is_q2r2 = 0;
    size_t ent_sz;

    if (stat(recipe, &rst) != 0) return 3;
    if (snprintf(mpath, sizeof mpath, "%s/%u", dir, MEMBER_IDX) >=
        (int)sizeof mpath)
        return 3;
    if (stat(mpath, &mst) != 0) return 3;
    if ((uint64_t)rst.st_size < QR_HDR_LEN) return 3;
    fd_r = open(recipe, O_RDONLY);
    if (fd_r < 0) return 3;
    if (pread_full(fd_r, hdr, sizeof hdr, 0) != 0) goto out;
    if (memcmp(hdr, QR_MAGIC, 4) == 0) {
        is_q2r2 = 0;
        ent_sz = QR_ENT_LEN;
    } else if (memcmp(hdr, QR2_MAGIC, 4) == 0) {
        is_q2r2 = 1;
        ent_sz = QR2_ENT_LEN;
    } else {
        goto out;
    }
    n_ext = get32le(hdr + 4);
    file_size = get64le(hdr + 8);
    member_size = get64le(hdr + 16);
    cluster_bits = get32le(hdr + 24);
    if (get32le(hdr + 28) != 0) goto out;
    if (cluster_bits < QC_MIN_CLUSBITS || cluster_bits > QC_MAX_CLUSBITS)
        goto out;
    cs = 1ull << cluster_bits;
    if (n_ext < 1) goto out;
    if (member_size / cs != n_ext || member_size % cs != 0) goto out;
    if (file_size < QC_HDR_NEED) goto out;
    if ((uint64_t)mst.st_size != member_size) goto out;

    tab = (uint8_t *)malloc((size_t)n_ext * ent_sz);
    chk = (uint8_t *)malloc((size_t)n_ext * 8);
    buf = (uint8_t *)malloc(COPY_CAP);
    if (!tab || !chk || !buf) goto out;
    if (pread_full(fd_r, tab, (size_t)n_ext * ent_sz, QR_HDR_LEN) != 0)
        goto out;

    uint64_t uncomp_extent_bytes = 0;
    for (i = 0; i < n_ext; i++) {
        uint64_t foff = get64le(tab + (size_t)i * ent_sz);
        uint64_t moff = get64le(tab + (size_t)i * ent_sz + 8);
        uint32_t csize = is_q2r2 ? get32le(tab + (size_t)i * ent_sz + 16) : 0;
        uint64_t elen = csize ? (uint64_t)csize : cs;
        if (foff > file_size || elen > file_size - foff) goto out;
        if (i && foff < get64le(tab + (size_t)(i - 1) * ent_sz))
            goto out;
        if (moff % cs != 0 || moff >= member_size) goto out;
        put64le(chk + (size_t)i * 8, moff);
        if (!csize)
            uncomp_extent_bytes += cs;
    }
    if ((uint64_t)rst.st_size !=
        QR_HDR_LEN + (uint64_t)n_ext * ent_sz + (file_size - uncomp_extent_bytes))
        goto out;

    /* permutation check */
    qsort(chk, n_ext, 8, u64_cmp);
    for (i = 0; i < n_ext; i++)
        if (get64le(chk + (size_t)i * 8) != (uint64_t)i * cs) goto out;

    fd_m = open(mpath, O_RDONLY);
    fd_out = open_out(out);
    if (fd_m < 0 || fd_out < 0) goto out;
    gap_off = QR_HDR_LEN + (uint64_t)n_ext * ent_sz;
    for (i = 0; i < n_ext; i++) {
        uint64_t foff = get64le(tab + (size_t)i * ent_sz);
        uint64_t moff = get64le(tab + (size_t)i * ent_sz + 8);
        uint32_t csize = is_q2r2 ? get32le(tab + (size_t)i * ent_sz + 16) : 0;
        uint64_t elen = csize ? (uint64_t)csize : cs;
        if (foff > pos) {                /* gap: from the recipe */
            if (copy_range(fd_r, fd_out, gap_off, foff - pos, buf) != 0) {
                fprintf(stderr, "qcow2: rebuild copy gap failed\n");
                goto out;
            }
            gap_off += foff - pos;
        }
        if (csize) {
            /* compressed cluster: verbatim from recipe gap */
            if (copy_range(fd_r, fd_out, gap_off, elen, buf) != 0) {
                fprintf(stderr, "qcow2: rebuild copy comp cluster failed\n");
                goto out;
            }
            gap_off += elen;
        } else {
            /* uncompressed cluster: from member stream */
            if (copy_range(fd_m, fd_out, moff, cs, buf) != 0) {
                fprintf(stderr, "qcow2: rebuild copy member cluster failed\n");
                goto out;
            }
        }
        pos = foff + elen;
    }
    if (file_size > pos &&
        copy_range(fd_r, fd_out, gap_off, file_size - pos, buf) != 0) {
        fprintf(stderr, "qcow2: rebuild copy tail gap failed\n");
        goto out;
    }
    rc = 0;
out:
    if (fd_r >= 0) close(fd_r);
    if (fd_m >= 0) close(fd_m);
    if (fd_out >= 0 && close(fd_out) != 0) rc = 3;
    free(tab);
    free(chk);
    free(buf);
    return rc;
}

/* ---- estimate ------------------------------------------------------------ */

static int cmd_estimate(const char *in)
{
    qc_img v;

    if (qc_parse(in, &v) != 0) return 3;
    printf("%llu\n", (unsigned long long)(v.member_size + EST_MARGIN));
    qc_free(&v);
    return 0;
}

/* ---- ivpack plugin C ABI export (ADR-007) --------------------------------
 * Built as libqcow2.so with -DIVPACK_SHARED_LIB -fPIC -shared; the fork guard
 * in ivpack_impl.h keeps the CLI's exit(3)=decline / exit(1)=error contract
 * intact inside a long-lived worker, and routes the estimate through the same
 * stdout the CLI prints. Shared with the other containerpacks: see
 * src/include/ivpack_impl.h. */
IVPACK_DEFINE_DESC(s_qcow2_desc, "qcow2", "1.1.0")

static int qcow2_iv_call_enumerate(const ivpack_container_args *a)
{ return (int)cmd_enumerate(a->in_path, a->out_path); }
static int qcow2_iv_call_extract(const ivpack_container_args *a)
{ return (int)cmd_extract(a->in_path, a->extract_idx, a->out_path); }
static int qcow2_iv_call_strip(const ivpack_container_args *a)
{ return (int)cmd_strip(a->in_path, a->out_path); }
static int qcow2_iv_call_rebuild(const ivpack_container_args *a)
{ return (int)cmd_rebuild(a->recipe_path, a->mbr_dir, a->out_path); }
static int qcow2_iv_call_map(const ivpack_container_args *a)
{
    if (a->out_buf && a->out_cap > 0 && a->out_len)
        return (int)cmd_map_mem(a->in_path ? a->in_path : a->recipe_path, a->out_buf, a->out_cap, a->out_len);
    return (int)cmd_map(a->in_path ? a->in_path : a->recipe_path, a->out_path);
}

IVPACK_DEFINE_CONTAINER_CMD(qcow2, IVPACK_GLUE_NONE())

IVPACK_DEFINE_CONTAINER_ESTIMATE(qcow2, IVPACK_GLUE_NONE(),
                                 rc = (int)cmd_estimate(a);)

/* ---- main ---------------------------------------------------------------- */

#ifndef IVPACK_SHARED_LIB
int main(int argc, char **argv)
{
    const char *cmd;

    if (argc < 2) return 2;
    cmd = argv[1];
    if (strcmp(cmd, "enumerate") == 0 && argc == 4)
        return cmd_enumerate(argv[2], argv[3]);
    if (strcmp(cmd, "extract") == 0 && argc == 5)
        return cmd_extract(argv[2], argv[3], argv[4]);
    if (strcmp(cmd, "strip") == 0 && argc == 4)
        return cmd_strip(argv[2], argv[3]);
    if (strcmp(cmd, "rebuild") == 0 && argc == 5)
        return cmd_rebuild(argv[2], argv[3], argv[4]);
    if (strcmp(cmd, "map") == 0 && argc == 4)
        return cmd_map(argv[2], argv[3]);
    if (strcmp(cmd, "estimate") == 0 && argc == 3)
        return cmd_estimate(argv[2]);
    return 2;
}
#endif
