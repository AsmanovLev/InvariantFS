/*
 * vdi.c — the vdi containerpack helper (InvariantFS WP16a + WP16b ABI v1.1).
 *
 * Decomposes VirtualBox VDI disk images (dynamic type only) into:
 *   - member idx 1 ("diskimg"): the concatenation of all ALLOCATED blocks
 *     in block-table order, full cbBlock bytes each — i.e. the virtual disk
 *     image padded up to a block multiple, ready for the rawdisk/ext4 packs
 *     to decompose further (nested containers);
 *   - recipe: every other byte of the file verbatim (header, block
 *     allocation map, bmap padding, holes left by unallocated blocks,
 *     trailing junk), in file order.
 *
 * VDI v1.1 layout (offsets verified empirically against qemu-img 10.x
 * output; all integers little-endian):
 *
 *   0x000 64   preheader comment text ("<<< Oracle VM VirtualBox Disk
 *              Image >>>\n" — VirtualBox; qemu writes "<<< QEMU VM Virtual
 *              Disk Image >>>\n": vendor text VARIES, useless for sniffing)
 *   0x040 u32  signature 0xBEDA107F (file bytes 7F 10 DA BE) — THE reliable
 *              sniff (manifest: sniff.magic=7F10DABE, sniff.offset=64)
 *   0x044 u32  version; only 0x00010001 (v1.1) is accepted (v1.0 has a
 *              different header layout)
 *   0x048 u32  header size (0x180 qemu / 0x190 VBox; informational, unused)
 *   0x04C u32  image type: 1 = dynamic (accepted), 2 = static (DECLINED —
 *              a static VDI is header + fixed block area, i.e. a raw disk
 *              image with a preamble: rawdisk pack territory, not ours),
 *              anything else (undo/diff/...) declined as unprovable
 *   0x050 u32  image flags (ignored; layout-neutral)
 *   0x054 256  image comment (ignored)
 *   0x154 u32  offBlocks — offset of the block allocation map
 *   0x158 u32  offData — offset of the block data area
 *   0x15C u32  cylinders / 0x160 heads / 0x164 sectors (legacy CHS, ignored)
 *   0x168 u32  sector size (512; ignored — irrelevant to the block layout)
 *   0x16C u32  unused
 *   0x170 u64  cbDisk — virtual disk size in bytes
 *   0x178 u32  cbBlock — block size (1 MiB typical)
 *   0x17C u32  cbBlockExtra — per-block extra bytes; must be 0 (nothing
 *              real sets it; declined as unprovable)
 *   0x180 u32  cBlocks — number of blocks covering the disk
 *   0x184 u32  cBlocksAllocated (informational; we count the bmap ourselves)
 *   0x188 16   uuidCreate / 0x198 uuidModify (ignored)
 *   0x1A8 16   uuidLinkage — parent UUID; any nonzero byte = differencing
 *              image = DECLINED (its bytes are meaningless without the
 *              parent; decomposition would silently serve a lie)
 *   0x1B8 16   uuidParentModify (ignored)
 *
 * Block map: cBlocks x u32 at offBlocks. Entry 0xFFFFFFFF = unallocated
 * (the block has NO bytes in the file — a dynamic VDI is sparse by
 * construction). Otherwise entry k means the block's cbBlock data bytes
 * live at file offset offData + k*cbBlock (verified: qemu-img reports the
 * same offsets, and vdi->raw conversion round-trips bit-exactly). Slots are
 * allocated in write order, so the map is generally NOT monotonic in the
 * block index. Every allocated block occupies a FULL cbBlock extent in the
 * file — including the last block when cbDisk is not a block multiple
 * (verified with qemu-io: the virtual tail padding is stored).
 *
 * Validation (any failure = decline, exit 3): file >= 0x1C8 bytes;
 * signature; version 1.1; type 1; uuidLink == 0; cbBlockExtra == 0;
 * cbBlock a power of two in [512, 2^28]; cbDisk >= 1; cBlocks >= 1 and
 * cBlocks == ceil(cbDisk/cbBlock) and <= 2^24 (the bmap slurp stays
 * <= 64 MiB); offBlocks >= 0x1C8 and both offBlocks/offData 512-byte
 * sector aligned (qemu refuses the unaligned as "unsupported"; so do we);
 * offBlocks + 4*cBlocks <= offData <= file_size; every allocated extent
 * [offData + slot*cbBlock, +cbBlock) inside the file; slots pairwise
 * distinct; and the MRMP map would stay under the FS entry cap
 * (4*65536+4): entries = n_alloc MEMBER runs + gap RECIPE runs, at most
 * 2*n_alloc + 1.
 *
 * Commands (fixed argv, no shell; exit 0 = ok, 3 = decline/fail):
 *   enumerate <in> <out>           "1<TAB>diskimg<TAB><n_alloc*cbBlock>\n"
 *   extract   <in> <idx> <out>     the concatenated allocated-block stream
 *   strip     <in> <out>           the recipe (gaps, file order)
 *   rebuild   <recipe> <dir> <out> original, bit-exact
 *   map       <in> <out>           MRMP: RECIPE gaps + MEMBER block extents
 *   estimate  <in>                 prints member bytes + 64 MiB
 *
 * rebuild re-derives the layout from the recipe alone: the recipe's first
 * range is the file's [0, first_extent) which covers [0, offData) (all
 * extents are >= offData by construction), so the header and the bmap are
 * read back out of the recipe at their original file offsets; the original
 * file size is gaps + member bytes (recipe_len + member_len), and the
 * member's size must be exactly n_alloc*cbBlock or the inputs are corrupt.
 * Holes are NOT seek-skipped: the recipe carries the unallocated regions'
 * original bytes verbatim, so junk-in-a-hole images rebuild bit-exactly
 * too (a sparse fresh output would only match zero-filled holes).
 * Everything streams through a 4 MiB window; no member ever needs to fit
 * in memory here.
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


/* ---- VDI v1.1 header field offsets ------------------------------------ */
#define VDI_SIG_OFF       0x40u
#define VDI_SIGNATURE     0xBEDA107Fu
#define VDI_VERSION_OFF   0x44u
#define VDI_VERSION_1_1   0x00010001u
#define VDI_TYPE_OFF      0x4Cu
#define VDI_TYPE_DYNAMIC  1u
#define VDI_OFFBLOCKS_OFF 0x154u
#define VDI_OFFDATA_OFF   0x158u
#define VDI_CBDISK_OFF    0x170u
#define VDI_CBBLOCK_OFF   0x178u
#define VDI_CBEXTRA_OFF   0x17Cu
#define VDI_CBLOCKS_OFF   0x180u
#define VDI_UUIDLINK_OFF  0x1A8u
#define VDI_HDR_NEED      0x1C8u   /* every field we read lives below this */

#define VDI_UNALLOCATED   0xFFFFFFFFu

#define VDI_MAX_CBLOCKS   (1u << 24)     /* bmap slurp cap: 64 MiB of u32 */
#define VDI_MIN_CBBLOCK   512u
#define VDI_MAX_CBBLOCK   (1u << 28)     /* 256 MiB blocks: generous */
#define MAP_MAX_ENTS      262148u        /* FS: 4 * CPACK_MAX_MEMBERS + 4 */
#define EST_MARGIN        (64ull << 20)  /* the estimate's flat margin */
#define COPY_CAP          (4u << 20)     /* streaming window (<= 8 MiB) */

#define MEMBER_IDX        1u
#define MEMBER_SNAME      "diskimg"

typedef struct {
    uint32_t blk;    /* virtual block index (member stream = blk order) */
    uint32_t slot;   /* data-area slot: extent at offData + slot*cbBlock */
} vdi_alloc;

typedef struct {
    uint64_t file_size;
    uint64_t cb_disk;
    uint32_t off_blocks, off_data, cb_block, c_blocks;
    uint32_t n_alloc;
    uint32_t *bmap;      /* c_blocks entries (malloc'd) */
    vdi_alloc *alloc;    /* n_alloc entries, in blk (table) order */
} vdi_img;

static uint32_t get32le(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;            /* the FS's own convention: on-disk is LE */
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

static void vdi_free(vdi_img *v)
{
    free(v->bmap);
    free(v->alloc);
    memset(v, 0, sizeof *v);
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/*
 * Parse + fully validate a VDI v1.1 dynamic image from an fd; file_size is
 * the size of the COMPLETE image (for rebuild: recipe_len + member_len —
 * the recipe alone is shorter). On success v owns the malloc'd bmap and
 * the alloc table (allocated blocks in blk/table order).
 * 0 = ok, -1 = decline (exit 3 at the command layer).
 */
static int vdi_parse_fd(int fd, uint64_t file_size, vdi_img *v)
{
    uint8_t hdr[VDI_HDR_NEED];
    uint32_t i, n;
    int rc = -1;

    memset(v, 0, sizeof *v);
    if (file_size < VDI_HDR_NEED) return -1;
    v->file_size = file_size;
    if (pread_full(fd, hdr, sizeof hdr, 0) != 0) return -1;

    if (get32le(hdr + VDI_SIG_OFF) != VDI_SIGNATURE) return -1;
    if (get32le(hdr + VDI_VERSION_OFF) != VDI_VERSION_1_1) return -1;
    if (get32le(hdr + VDI_TYPE_OFF) != VDI_TYPE_DYNAMIC) return -1;
    for (i = 0; i < 16; i++)
        if (hdr[VDI_UUIDLINK_OFF + i]) return -1;  /* differencing: refuse */
    if (get32le(hdr + VDI_CBEXTRA_OFF) != 0) return -1;

    v->cb_block   = get32le(hdr + VDI_CBBLOCK_OFF);
    v->cb_disk    = get64le(hdr + VDI_CBDISK_OFF);
    v->c_blocks   = get32le(hdr + VDI_CBLOCKS_OFF);
    v->off_blocks = get32le(hdr + VDI_OFFBLOCKS_OFF);
    v->off_data   = get32le(hdr + VDI_OFFDATA_OFF);

    if (v->cb_block < VDI_MIN_CBBLOCK || v->cb_block > VDI_MAX_CBBLOCK ||
        (v->cb_block & (v->cb_block - 1)) != 0)
        return -1;
    if (!v->cb_disk || !v->c_blocks || v->c_blocks > VDI_MAX_CBLOCKS)
        return -1;
    if ((v->cb_disk + v->cb_block - 1) / v->cb_block != v->c_blocks)
        return -1;
    if (v->off_blocks < VDI_HDR_NEED) return -1;
    /* sector alignment (qemu refuses anything else as "unaligned"): both
     * offsets are multiples of the 512-byte VDI sector */
    if ((v->off_blocks & 511) != 0 || (v->off_data & 511) != 0) return -1;
    if ((uint64_t)v->off_blocks + 4ull * v->c_blocks > v->off_data)
        return -1;
    if ((uint64_t)v->off_data > v->file_size) return -1;

    v->bmap = (uint32_t *)malloc(4ull * v->c_blocks);
    if (!v->bmap) return -1;
    if (pread_full(fd, v->bmap, 4ull * v->c_blocks, v->off_blocks) != 0)
        goto out;

    n = 0;
    for (i = 0; i < v->c_blocks; i++)
        if (v->bmap[i] != VDI_UNALLOCATED) n++;
    /* the MRMP map: n MEMBER runs + at most n+1 gap RECIPE runs; stay
     * strictly under the FS entry cap */
    if (2ull * n + 1 > MAP_MAX_ENTS) goto out;
    v->alloc = (vdi_alloc *)malloc((n ? n : 1) * sizeof(vdi_alloc));
    if (!v->alloc) goto out;
    n = 0;
    for (i = 0; i < v->c_blocks; i++) {
        uint32_t slot = v->bmap[i];
        uint64_t end;
        if (slot == VDI_UNALLOCATED) continue;
        /* slot < 2^32 and cb_block <= 2^28: no u64 overflow possible */
        end = (uint64_t)v->off_data + (uint64_t)slot * v->cb_block +
              v->cb_block;
        if (end > v->file_size) goto out;
        v->alloc[n].blk = i;
        v->alloc[n].slot = slot;
        n++;
    }
    v->n_alloc = n;
    /* slots must be pairwise distinct (two blocks sharing one extent is a
     * corrupt file, and would overlap in the map anyway) */
    if (n > 1) {
        uint32_t *srt = (uint32_t *)malloc(n * sizeof *srt);
        uint32_t k;
        int dup = 0;
        if (!srt) goto out;
        for (k = 0; k < n; k++) srt[k] = v->alloc[k].slot;
        qsort(srt, n, sizeof *srt, u32_cmp);
        for (k = 1; k < n; k++)
            if (srt[k] == srt[k - 1]) { dup = 1; break; }
        free(srt);
        if (dup) goto out;
    }
    rc = 0;
out:
    if (rc) vdi_free(v);
    return rc;
}

static int vdi_parse(const char *path, vdi_img *v)
{
    struct stat st;
    int fd, rc;

    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    rc = vdi_parse_fd(fd, (uint64_t)st.st_size, v);
    close(fd);
    return rc;
}

/*
 * The file-order walk: order[] = indices into v->alloc sorted by slot
 * (= by file offset, since extent = offData + slot*cbBlock). NULL when
 * n_alloc == 0 (a zero-length walk) or on allocation failure.
 */
static uint32_t *vdi_file_order(const vdi_img *v)
{
    uint32_t *order, i;
    if (!v->n_alloc) return NULL;
    order = (uint32_t *)malloc(v->n_alloc * sizeof *order);
    if (!order) return NULL;
    for (i = 0; i < v->n_alloc; i++) order[i] = i;
    /* insertion sort is plenty: allocation-order slots are near-sorted in
     * practice, and n_alloc is bounded by the map cap (~131k) */
    for (i = 1; i < v->n_alloc; i++) {
        uint32_t t = order[i];
        uint32_t j = i;
        while (j > 0 && v->alloc[order[j - 1]].slot > v->alloc[t].slot) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = t;
    }
    return order;
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
    vdi_img v;
    FILE *f;
    int rc = 3;

    if (vdi_parse(in, &v) != 0) return 3;
    f = fopen(out, "w");
    if (f) {
        fprintf(f, "%u\t%s\t%llu\n", MEMBER_IDX, MEMBER_SNAME,
                (unsigned long long)v.n_alloc * v.cb_block);
        if (fclose(f) == 0) rc = 0;
    }
    vdi_free(&v);
    return rc;
}

/* ---- extract ------------------------------------------------------------ */

static int cmd_extract(const char *in, const char *idx_s, const char *out)
{
    vdi_img v;
    uint8_t *buf = NULL;
    char *endp = NULL;
    uint32_t i;
    int fd_in = -1, fd_out = -1, rc = 3;

    if (strtoul(idx_s, &endp, 10) != MEMBER_IDX || !endp || *endp)
        return 3;
    if (vdi_parse(in, &v) != 0) return 3;
    fd_in = open(in, O_RDONLY);
    fd_out = open_out(out);
    buf = (uint8_t *)malloc(COPY_CAP);
    if (fd_in < 0 || fd_out < 0 || !buf) goto out;
    /* member stream = allocated blocks in TABLE (blk) order */
    for (i = 0; i < v.n_alloc; i++) {
        uint64_t off = (uint64_t)v.off_data +
                       (uint64_t)v.alloc[i].slot * v.cb_block;
        if (copy_range(fd_in, fd_out, off, v.cb_block, buf) != 0) goto out;
    }
    rc = 0;
out:
    if (fd_in >= 0) close(fd_in);
    if (fd_out >= 0 && close(fd_out) != 0) rc = 3;
    free(buf);
    vdi_free(&v);
    return rc;
}

/* ---- strip (the recipe = every byte that is not an allocated extent) ---- */

static int cmd_strip(const char *in, const char *out)
{
    vdi_img v;
    uint32_t *order = NULL;
    uint8_t *buf = NULL;
    uint64_t pos = 0;
    uint32_t i;
    int fd_in = -1, fd_out = -1, rc = 3;

    if (vdi_parse(in, &v) != 0) return 3;
    order = vdi_file_order(&v);
    if (v.n_alloc && !order) { vdi_free(&v); return 3; }
    fd_in = open(in, O_RDONLY);
    fd_out = open_out(out);
    buf = (uint8_t *)malloc(COPY_CAP);
    if (fd_in < 0 || fd_out < 0 || !buf) goto out;
    for (i = 0; i < v.n_alloc; i++) {
        uint64_t ext = (uint64_t)v.off_data +
                       (uint64_t)v.alloc[order[i]].slot * v.cb_block;
        if (ext > pos && copy_range(fd_in, fd_out, pos, ext - pos, buf) != 0)
            goto out;
        pos = ext + v.cb_block;                 /* skip the member bytes */
    }
    if (v.file_size > pos &&
        copy_range(fd_in, fd_out, pos, v.file_size - pos, buf) != 0)
        goto out;
    rc = 0;
out:
    if (fd_in >= 0) close(fd_in);
    if (fd_out >= 0 && close(fd_out) != 0) rc = 3;
    free(buf);
    free(order);
    vdi_free(&v);
    return rc;
}

/* ---- map (MRMP: RECIPE gaps + MEMBER extents, partitioning the file) ---- */

static int cmd_map(const char *in, const char *out)
{
    vdi_img v;
    uint32_t *order = NULL;
    uint8_t *ent = NULL;
    uint64_t pos = 0, recipe_off = 0;
    uint32_t i, n_ent = 0;
    int fd_out = -1, rc = 3;
    uint8_t hdr[8];

    if (vdi_parse(in, &v) != 0) return 3;
    order = vdi_file_order(&v);
    if (v.n_alloc && !order) { vdi_free(&v); return 3; }
    /* entries <= 2*n_alloc + 1, bounded at parse time */
    ent = (uint8_t *)malloc((2ull * v.n_alloc + 1) * 29);
    if (!ent) goto out;
    for (i = 0; i < v.n_alloc; i++) {
        const vdi_alloc *a = &v.alloc[order[i]];
        uint64_t ext = (uint64_t)v.off_data + (uint64_t)a->slot * v.cb_block;
        uint8_t *p;
        if (ext > pos) {                        /* RECIPE gap */
            p = ent + (size_t)n_ent * 29;
            put64le(p, pos);
            put64le(p + 8, ext - pos);
            p[16] = 0;                          /* kind RECIPE */
            put32le(p + 17, 0);
            put64le(p + 21, recipe_off);
            recipe_off += ext - pos;
            n_ent++;
        }
        p = ent + (size_t)n_ent * 29;           /* MEMBER extent */
        put64le(p, ext);
        put64le(p + 8, v.cb_block);
        p[16] = 1;                              /* kind MEMBER */
        put32le(p + 17, MEMBER_IDX);
        /* src_off = the extent's rank in the TABLE-ORDERED member stream;
         * order[i] is an index into the blk-ordered alloc[], so it IS the
         * rank */
        put64le(p + 21, (uint64_t)order[i] * v.cb_block);
        n_ent++;
        pos = ext + v.cb_block;
    }
    if (v.file_size > pos) {                    /* trailing RECIPE gap */
        uint8_t *p = ent + (size_t)n_ent * 29;
        put64le(p, pos);
        put64le(p + 8, v.file_size - pos);
        p[16] = 0;
        put32le(p + 17, 0);
        put64le(p + 21, recipe_off);
        n_ent++;
    }
    fd_out = open_out(out);
    if (fd_out < 0) goto out;
    memcpy(hdr, "MRMP", 4);
    put32le(hdr + 4, n_ent);
    if (write_full(fd_out, hdr, 8) != 0 ||
        write_full(fd_out, ent, (size_t)n_ent * 29) != 0)
        goto out;
    rc = 0;
out:
    if (fd_out >= 0 && close(fd_out) != 0) rc = 3;
    free(ent);
    free(order);
    vdi_free(&v);
    return rc;
}

/* ---- rebuild ------------------------------------------------------------ */

static int cmd_rebuild(const char *recipe, const char *dir, const char *out)
{
    vdi_img v;
    struct stat rst, mst;
    uint32_t *order = NULL;
    uint8_t *buf = NULL;
    uint64_t pos = 0, recipe_off = 0;
    uint32_t i;
    char mpath[4096];
    int fd_r = -1, fd_m = -1, fd_out = -1, rc = 3;

    if (stat(recipe, &rst) != 0) return 3;
    if (snprintf(mpath, sizeof mpath, "%s/%u", dir, MEMBER_IDX) >=
        (int)sizeof mpath)
        return 3;
    if (stat(mpath, &mst) != 0) return 3;
    fd_r = open(recipe, O_RDONLY);
    if (fd_r < 0) return 3;
    /* the synthesized original size: the recipe holds the gaps, the member
     * holds the extents */
    if (vdi_parse_fd(fd_r, (uint64_t)rst.st_size + (uint64_t)mst.st_size,
                     &v) != 0)
        goto out;
    /* ... and the member must be exactly the announced stream (the FS feeds
     * it from the table; a size mismatch = corrupt input, fail loudly) */
    if (mst.st_size != (off_t)((uint64_t)v.n_alloc * v.cb_block))
        goto out;
    order = vdi_file_order(&v);
    if (v.n_alloc && !order) goto out;
    fd_m = open(mpath, O_RDONLY);
    fd_out = open_out(out);
    buf = (uint8_t *)malloc(COPY_CAP);
    if (fd_m < 0 || fd_out < 0 || !buf) goto out;
    for (i = 0; i < v.n_alloc; i++) {
        const vdi_alloc *a = &v.alloc[order[i]];
        uint64_t ext = (uint64_t)v.off_data + (uint64_t)a->slot * v.cb_block;
        if (ext > pos) {                        /* gap: from the recipe */
            if (copy_range(fd_r, fd_out, recipe_off, ext - pos, buf) != 0)
                goto out;
            recipe_off += ext - pos;
        }
        /* extent: from the member stream at rank*cbBlock (order[i] is the
         * rank in the blk-ordered alloc[]) */
        if (copy_range(fd_m, fd_out, (uint64_t)order[i] * v.cb_block,
                       v.cb_block, buf) != 0)
            goto out;
        pos = ext + v.cb_block;
    }
    if (v.file_size > pos &&
        copy_range(fd_r, fd_out, recipe_off, v.file_size - pos, buf) != 0)
        goto out;
    rc = 0;
out:
    if (fd_r >= 0) close(fd_r);
    if (fd_m >= 0) close(fd_m);
    if (fd_out >= 0 && close(fd_out) != 0) rc = 3;
    free(buf);
    free(order);
    vdi_free(&v);
    return rc;
}

/* ---- estimate ------------------------------------------------------------ */

static int cmd_estimate(const char *in)
{
    vdi_img v;
    uint64_t ws;

    if (vdi_parse(in, &v) != 0) return 3;
    ws = (uint64_t)v.n_alloc * v.cb_block + EST_MARGIN;
    printf("%llu\n", (unsigned long long)ws);
    vdi_free(&v);
    return 0;
}

/* ---- main ---------------------------------------------------------------- */


/* ---- ivpack plugin C ABI export (ADR-007) --------------------------------
 * Built as libvdi.so with -DIVPACK_SHARED_LIB -fPIC -shared; the fork
 * guard in ivpack_impl.h keeps the CLI's exit(3)=decline / exit(1)=error
 * contract intact inside a long-lived worker. See ivpack_impl.h. */
static const ivpack_desc s_vdi_desc = {
    IVPACK_API_VERSION, "vdi", "1.0.0", "containerpack", 0
};

const ivpack_desc *ivpack_get_desc(void) { return &s_vdi_desc; }

IVPACK_CANON_CALLS(vdi)

IVPACK_DEFINE_CONTAINER_CMD(vdi, IVPACK_GLUE_NONE())

IVPACK_DEFINE_CONTAINER_ESTIMATE(vdi, IVPACK_GLUE_NONE(),
                             rc = (int)cmd_estimate(a);)

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
#endif /* !IVPACK_SHARED_LIB */

