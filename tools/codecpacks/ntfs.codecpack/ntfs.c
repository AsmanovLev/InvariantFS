/*
 * ntfs.c — the ntfs containerpack helper (InvariantFS WP16a/WP16b).
 *
 * Decomposes an NTFS filesystem image into per-file members: every
 * non-resident unnamed $DATA stream of a plain user file becomes one
 * member (idx = the MFT record number); everything else — boot sectors,
 * the whole $MFT (all FILE records, verbatim, fixup trailers included),
 * $Bitmap/$LogFile/$Boot/$BadClus/$Secure/$Upcase/$Extend payloads,
 * directory indexes, resident $DATA, file-content slack tails and all
 * unallocated clusters — lands in the recipe byte-exactly.
 *
 * Build:  cc -std=c11 -O2 -Wall -Wextra -Werror -o bin/ntfs ntfs.c
 * libc only; no third-party code.
 *
 * Commands (fixed argv, no shell; exit 0 = ok, 3 = decline this image,
 * 1 = hard error, 2 = usage):
 *   enumerate <in> <out>            "idx<TAB>sname<TAB>usize" per member
 *   extract   <in> <idx> <out>      member idx's logical content (usize B)
 *   strip     <in> <out>            the recipe (NTRC format, below)
 *   rebuild   <recipe> <dir> <out>  original image, bit-exact
 *   map       <in> <out>            MRMP member map (WP16b)
 *   estimate  <in>                  prints sum(member usize) + 64 MiB
 *
 * All commands stream: the image is never mapped or slurped; copies run
 * in <= 4 MiB windows (the ABI asks for <= 8 MiB).
 *
 * --------------------------------------------------------------------
 * SUPPORT MATRIX (v1)
 * --------------------------------------------------------------------
 *   bytes/sector            512 only            (others: DECLINE)
 *   sectors/cluster         1..128 power of two (else DECLINE)
 *   MFT record size         512..65536, multiple of the sector size
 *   $MFT $DATA              non-resident, any run count, in record 0 only
 *   $DATA resident          content stays in the record -> recipe (never
 *                           a member)
 *   $DATA non-resident      member (idx = MFT record number, usize = the
 *                           attribute's real size)
 *   sparse $DATA            SUPPORTED: hole runs (LCN delta 0) extract as
 *                           zeros and have no disk bytes, so the map has
 *                           no entries for them
 *   hardlinks (n x $FILE_NAME) one member; sname = the best name,
 *                           Win32(1) > Win32&DOS(3) > POSIX(0) > DOS(2)
 *   directories             never members; their $INDEX_ROOT/$INDEX_
 *                           ALLOCATION bytes are recipe verbatim (the
 *                           pack never parses INDX blocks — it scans FILE
 *                           records instead of walking trees)
 *   system files            records 0..15 and any name starting with '$'
 *                           are never members (their non-resident streams
 *                           — $LogFile, $Bitmap, $Secure:$SDS, $BadClus:
 *                           $Bad, $Extend\$UsnJrnl:$J ... — are recipe)
 *   image size              <= 4 GiB (the FS blob format is u32-sized)
 *   members                 <= 65536 and every idx <= 65535 (FS ABI caps)
 *
 * --------------------------------------------------------------------
 * HONEST DECLINES (exit 3; the FS then stores the image generically)
 * --------------------------------------------------------------------
 *   - bad boot sector (magic/jump), sector size != 512, bad cluster
 *     factor, bad MFT record size, image > 4 GiB, truncated image
 *   - fixup (update-sequence-array) mismatch on any IN-USE FILE record
 *   - $MFT described by an $ATTRIBUTE_LIST (multi-record $MFT)
 *   - ANY $ATTRIBUTE_LIST anywhere (attribute extents in other records)
 *   - ANY extension FILE record (nonzero base-record reference)
 *   - a user file (record >= 16, name not '$'-prefixed) with:
 *       * a NAMED $DATA stream (an ADS — v1 enumerates unnamed only)
 *       * a compressed $DATA (attribute flag 0x0001, LZNT1/NTFS-Xpress)
 *       * an encrypted $DATA (flag 0x4000) or a $LOGGED_UTILITY_STREAM
 *         named "$EFS"
 *       * more than one unnamed $DATA, or a runlist that cannot cover
 *         the announced real size, or runs outside the image
 *   - two members whose content clusters overlap (corrupt image)
 *
 * An in-use record >= 16 with non-resident data but NO $FILE_NAME at all
 * is NOT a decline: its bytes are provably recipe (it simply never
 * becomes a member — a member needs a name to be useful). Records 12..15
 * on a fresh mkntfs image are exactly this shape (nameless, in use).
 *
 * --------------------------------------------------------------------
 * FIXUP HANDLING (why rebuild needs no fixup logic at all)
 * --------------------------------------------------------------------
 * NTFS protects every multi-sector record (FILE, INDX, RSTR/RCRD log
 * pages) with an update-sequence array: the last two bytes of each
 * sector are replaced on disk by a sequence number; the originals live
 * in the USA. The READ side here (parse for enumerate/extract/strip/
 * map/estimate) verifies each sector trailer against the USN and
 * restores the original bytes IN MEMORY before interpreting a record.
 * NOTHING is ever written back into an NTFS structure: the recipe stores
 * ON-DISK bytes verbatim, i.e. with the USN trailers already in place,
 * and rebuild splices those same bytes back at their original offsets —
 * the fixups are correct by construction. Member content clusters carry
 * no fixups: fixups exist only inside record structures ($MFT FILE
 * records, INDX index blocks, $LogFile pages), and every one of those
 * ranges is recipe, never a member. Hence rebuild is a pure byte splice
 * of recipe ranges and member payloads; it does not parse NTFS at all.
 *
 * --------------------------------------------------------------------
 * RECIPE FORMAT ("NTRC", pack-owned; the FS never parses it)
 * --------------------------------------------------------------------
 * All integers little-endian.
 *    0   4   "NTRC"
 *    4   4   u32 version (= 1)
 *    8   8   u64 image_size
 *   16   8   u64 recipe_blob_size (sum of the range lengths)
 *   24   4   u32 cluster_size       (informational)
 *   28   4   u32 mft_record_size    (informational)
 *   32   4   u32 n_members
 *   36   4   u32 n_recipe_ranges
 *   40  24   reserved (zero)
 *   64  ..   member table: n_members x
 *              { u32 idx, u32 nruns, u64 usize,
 *                nruns x { u64 lcn, u64 len_clusters, u64 content_off } }
 *              (non-sparse runs only; content_off counts the sparse
 *              holes too, so rebuild can slice the member file directly)
 *        ..   recipe range table: n_recipe_ranges x
 *              { u64 orig_off, u64 len }   (sorted, disjoint)
 *        ..   recipe blob: the concatenated original bytes of every
 *              recipe range, in range-table order
 *
 * member ranges (the map's MEMBER entries) are, per member, per
 * non-sparse run: orig_off = lcn*cs, src_off = content_off, len =
 * min(len_clusters*cs, usize - content_off) — the real-size clip means
 * the last cluster's slack tail is NOT member bytes; it is recipe like
 * any other non-content byte. Recipe ranges are the exact complement:
 * [0, image_size) minus all member ranges. Union = the whole image, no
 * overlaps (checked at build time; the FS re-validates the same
 * partition on the map).
 *
 * The map command renders the same partition as MRMP:
 *   [4B "MRMP"][u32 n] n x { u64 orig_off, u64 len, u8 kind
 *                            (0=RECIPE/1=MEMBER), u32 idx, u64 src_off }
 * RECIPE src_off = the range's offset inside the WHOLE NTRC file (the FS
 * stores the strip output as-is and reads RECIPE sources from it);
 * MEMBER src_off = content_off. strip and map run the same deterministic
 * builder over the same frozen input, so the layouts agree.
 *
 * Diagnostics: the ABI swallows stdout/stderr (except estimate's
 * stdout); NTFS_PACK_DEBUG=1 in the environment re-enables stderr
 * reasons for hand debugging.
 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
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


#define EXIT_DECLINE 3

#define COPY_BUF_SZ   (4u << 20)     /* streaming window: <= 8 MiB (ABI) */
#define MAX_IMAGE     (4ull << 30)   /* the FS blob format is u32-sized */
#define MAX_MEMBERS   65536u         /* FS ABI: CPACK_MAX_MEMBERS */
#define MAX_IDX       65535u         /* FS ABI: CPACK_MAX_IDX */
#define MAX_MAP_ENTS  (4u * MAX_MEMBERS + 4u)   /* FS ABI cap */
#define ESTIMATE_SLACK (64ull << 20) /* estimate = sum(usize) + 64 MiB */

/* attribute types */
#define AT_END        0xFFFFFFFFu
#define AT_STANDINFO  0x10u
#define AT_ATTRLIST   0x20u
#define AT_FILENAME   0x30u
#define AT_DATA       0x80u
#define AT_LOGGED     0x100u

/* attribute header flags */
#define AF_COMPRESSED 0x0001u
#define AF_ENCRYPTED  0x4000u
#define AF_SPARSE     0x8000u

/* FILE record flags */
#define FF_INUSE      0x0001u
#define FF_DIR        0x0002u

static int dbg;   /* NTFS_PACK_DEBUG */

#define DBG(...) do { if (dbg) fprintf(stderr, "ntfs: " __VA_ARGS__); } while (0)

/* ---- little-endian field reads (byte assembly: endian-safe, no
 * alignment assumptions) ---- */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32));
}

/* ---- I/O helpers ---- */
static int pread_full(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *b = (uint8_t *)buf;
    while (len) {
        ssize_t r = pread(fd, b, len, (off_t)off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;   /* short read */
        b += r; len -= (size_t)r; off += (uint64_t)r;
    }
    return 0;
}

static int pwrite_full(int fd, const void *buf, size_t len, uint64_t off)
{
    const uint8_t *b = (const uint8_t *)buf;
    while (len) {
        ssize_t w = pwrite(fd, b, len, (off_t)off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        b += w; len -= (size_t)w; off += (uint64_t)w;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *b = (const uint8_t *)buf;
    while (len) {
        ssize_t w = write(fd, b, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        b += w; len -= (size_t)w;
    }
    return 0;
}

/* ---- run lists ---- */
typedef struct {
    uint64_t lcn;            /* absolute LCN (meaningless when sparse) */
    uint64_t len;            /* run length in clusters */
    int      sparse;         /* LCN delta 0: a hole — content is zeros */
} run;

/* Parse a runlist from buf[off .. end). On success *runs_out/n_out are
 * malloc'd (empty runlist -> *runs_out NULL, n 0). Returns 0 or -1. */
static int runlist_parse(const uint8_t *buf, size_t off, size_t end,
                         run **runs_out, size_t *n_out)
{
    run *runs = NULL;
    size_t n = 0, cap = 0, pos = off;
    uint64_t lcn = 0;

    *runs_out = NULL;
    *n_out = 0;
    for (;;) {
        uint8_t h;
        uint64_t rlen = 0;
        int64_t dlcn = 0;
        size_t llen, olen, i;

        if (pos >= end) goto fail;               /* unterminated */
        h = buf[pos++];
        if (!h) break;                           /* terminator */
        llen = h & 0x0F;
        olen = h >> 4;
        if (!llen || llen > 8 || olen > 8) goto fail;
        if (pos + llen + olen > end) goto fail;
        for (i = 0; i < llen; i++)
            rlen |= (uint64_t)buf[pos + i] << (8 * i);
        pos += llen;
        for (i = 0; i < olen; i++)
            dlcn |= (int64_t)((uint64_t)buf[pos + i] << (8 * i));
        pos += olen;
        if (olen && (buf[pos - 1] & 0x80))       /* sign-extend */
            dlcn -= (int64_t)1 << (8 * olen);
        if (!rlen) goto fail;
        if (olen == 0) {
            /* sparse hole: the delta is absent (=0), the accumulator
             * stays at the previous LCN by the encoding's rules */
            if (n == cap) {
                size_t nc = cap ? cap * 2 : 8;
                run *nr = (run *)realloc(runs, nc * sizeof *nr);
                if (!nr) goto fail;
                runs = nr; cap = nc;
            }
            runs[n].lcn = lcn;   /* unused */
            runs[n].len = rlen;
            runs[n].sparse = 1;
            n++;
            continue;
        }
        if (dlcn < 0 && (uint64_t)-dlcn > lcn) goto fail;   /* negative LCN */
        lcn = (uint64_t)((int64_t)lcn + dlcn);
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 8;
            run *nr = (run *)realloc(runs, nc * sizeof *nr);
            if (!nr) goto fail;
            runs = nr; cap = nc;
        }
        runs[n].lcn = lcn;
        runs[n].len = rlen;
        runs[n].sparse = 0;
        n++;
        if (UINT64_MAX - lcn < rlen) goto fail;  /* lcn+len overflow */
    }
    *runs_out = runs;
    *n_out = n;
    return 0;
fail:
    free(runs);
    return -1;
}

/* ---- the parsed volume ---- */
typedef struct {
    uint32_t idx;            /* MFT record number */
    uint64_t usize;          /* $DATA real size */
    char    *name;           /* malloc'd UTF-8 basename (never NULL) */
    run     *runs;           /* non-resident $DATA runlist (holes incl.) */
    size_t   nruns;
} member;

typedef struct {
    int      fd;
    uint64_t file_size;      /* the image file's size */
    uint32_t bps;            /* bytes per sector (512 only) */
    uint64_t cs;             /* cluster size in bytes */
    uint64_t recsz;          /* MFT record size in bytes */
    uint64_t mft_lcn;
    run     *mft_runs;
    size_t   mft_nruns;
    uint64_t mft_real;       /* $MFT $DATA real size */
    uint64_t nrec;           /* mft_real / recsz */
    member  *mem;
    size_t   nmem;
    uint8_t *recbuf;         /* recsz scratch */
} vol;

static void vol_close(vol *v)
{
    size_t i;
    if (v->fd >= 0) close(v->fd);
    free(v->mft_runs);
    for (i = 0; i < v->nmem; i++) {
        free(v->mem[i].name);
        free(v->mem[i].runs);
    }
    free(v->mem);
    free(v->recbuf);
    memset(v, 0, sizeof *v);
    v->fd = -1;
}

/* VCN -> disk offset inside the $MFT data stream. */
static int mft_disk_off(const vol *v, uint64_t content_off, uint64_t *out)
{
    uint64_t vcn = content_off / v->cs;
    uint64_t within = content_off % v->cs;
    uint64_t pos = 0;
    size_t i;

    for (i = 0; i < v->mft_nruns; i++) {
        const run *r = &v->mft_runs[i];
        if (vcn - pos < r->len) {   /* pos <= vcn (guarded below) */
            if (r->sparse) return -1;    /* a hole in $MFT: corrupt */
            if (r->lcn + (vcn - pos) > UINT64_MAX / v->cs) return -1;
            *out = (r->lcn + (vcn - pos)) * v->cs + within;
            return *out < v->file_size ? 0 : -1;
        }
        pos += r->len;
    }
    return -1;
}

/* Read MFT record rn and apply its fixup (verify USN, restore sector
 * tails in the buffer). Returns 1 ok, 0 = not a live FILE record (bad
 * magic), -1 = corrupt (decline-worthy). */
static int record_read(vol *v, uint64_t rn)
{
    uint64_t off;
    uint8_t *r = v->recbuf;
    uint16_t usa_off, usa_n, usn;
    size_t nsec, i;

    if (mft_disk_off(v, rn * v->recsz, &off) != 0) {
        DBG("record %llu: no disk offset\n", (unsigned long long)rn);
        return -1;
    }
    if (off + v->recsz > v->file_size) {
        DBG("record %llu: past the image end\n", (unsigned long long)rn);
        return -1;
    }
    if (pread_full(v->fd, r, (size_t)v->recsz, off) != 0) {
        DBG("record %llu: read failed\n", (unsigned long long)rn);
        return -1;
    }
    if (memcmp(r, "FILE", 4) != 0)
        return 0;                      /* BAAD/scratch: not a live record */
    usa_off = rd16(r + 4);
    usa_n = rd16(r + 6);
    nsec = (size_t)(v->recsz / v->bps);
    if ((size_t)usa_n != nsec + 1 ||
        (uint64_t)usa_off + (uint64_t)usa_n * 2 > v->recsz) {
        /* a FREE record with a stale/torn USA is just dead bytes (recipe);
         * in-use => the image is damaged */
        if (!(rd16(r + 22) & FF_INUSE)) return 0;
        DBG("record %llu: bad USA geometry (in use)\n",
            (unsigned long long)rn);
        return -1;
    }
    usn = rd16(r + usa_off);
    for (i = 0; i < nsec; i++) {
        size_t tail = (i + 1) * v->bps - 2;
        if (rd16(r + tail) != usn) {
            /* a stale trailer on a FREE record (a deleted file's record
             * keeps whatever bytes it died with on some media) is not
             * corruption the pack must prove against: the record's bytes
             * are recipe verbatim either way. In-use + bad fixup = the
             * image is damaged: decline. The flags word sits below the
             * first trailer, so the raw bytes are readable. */
            if (rd16(r + 22) & FF_INUSE) {
                DBG("record %llu: fixup mismatch sector %zu (in use)\n",
                    (unsigned long long)rn, i);
                return -1;
            }
            return 0;
        }
        r[tail] = r[usa_off + 2 + 2 * i];
        r[tail + 1] = r[usa_off + 3 + 2 * i];
    }
    return 1;
}

/* ---- attribute walking ---- */
typedef struct {
    const uint8_t *base;     /* attribute header start */
    uint32_t type;
    uint32_t len;            /* total attribute length */
    int      nonres;
    uint8_t  namelen;        /* in UTF-16 chars */
    uint16_t nameoff;
    uint16_t aflags;
} attr;

/* Advance *aoff to the next attribute; fills *a. Returns 1 ok, 0 = the
 * END marker, -1 = corrupt. recno is for diagnostics only. */
static uint64_t g_attr_rec;      /* current record, for DBG */
static int attr_next(const vol *v, uint32_t *aoff, attr *a)
{
    const uint8_t *r = v->recbuf;
    uint32_t o = *aoff;

    if ((uint64_t)o + 4 > v->recsz) goto bad;
    a->type = rd32(r + o);
    if (a->type == AT_END) return 0;   /* the END marker is 4 bytes only;
                                        * it may live in the record tail */
    if ((uint64_t)o + 16 > v->recsz) goto bad;
    a->base = r + o;
    a->len = rd32(r + o + 4);
    if (a->len < 16 || (uint64_t)o + a->len > v->recsz) goto bad;
    a->nonres = r[o + 8] ? 1 : 0;
    a->namelen = r[o + 9];
    a->nameoff = rd16(r + o + 10);
    a->aflags = rd16(r + o + 12);
    if ((uint32_t)a->nameoff + (uint32_t)a->namelen * 2 > a->len) goto bad;
    if (a->nonres) {
        if (a->len < 64) goto bad;
    } else {
        uint32_t clen;
        if (a->len < 24) goto bad;
        clen = rd32(r + o + 16);
        if ((uint64_t)rd16(r + o + 20) + clen > a->len) goto bad;
    }
    *aoff = o + a->len;
    return 1;
bad:
    DBG("record %llu: bad attribute bounds (off %u)\n",
        (unsigned long long)g_attr_rec, o);
    return -1;
}

/* UTF-16LE -> UTF-8 (BMP + surrogate pairs), sanitized for the member
 * table: bytes < 0x20, 0x7F and '\t' fold to '_' (the FS sanitizes to
 * [A-Za-z0-9._-] on top of this; the pack's own job is only to keep the
 * table line-shaped). Returns NULL on garbage. */
static char *name_utf8(const uint8_t *p, size_t nchars)
{
    size_t cap = nchars * 3 + 1, w = 0, i;
    char *s = (char *)malloc(cap);

    if (!s) return NULL;
    for (i = 0; i < nchars; i++) {
        uint32_t cp = rd16(p + 2 * i);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo;
            if (i + 1 >= nchars) { free(s); return NULL; }
            lo = rd16(p + 2 * i + 2);
            if (lo < 0xDC00 || lo > 0xDFFF) { free(s); return NULL; }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i++;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            free(s); return NULL;              /* lone low surrogate */
        }
        if (cp < 0x20 || cp == 0x7F) cp = '_';
        if (cp < 0x80) {
            if (w + 1 >= cap) break;
            s[w++] = (char)cp;
        } else if (cp < 0x800) {
            if (w + 2 >= cap) break;
            s[w++] = (char)(0xC0 | (cp >> 6));
            s[w++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (w + 3 >= cap) break;
            s[w++] = (char)(0xE0 | (cp >> 12));
            s[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[w++] = (char)(0x80 | (cp & 0x3F));
        } else {
            /* cap is nchars*3+1: a 4-byte UTF-8 sequence comes from a
             * surrogate PAIR (2 chars -> 6 bytes of budget), so w+4
             * always fits; keep the guard for defense in depth */
            if (w + 4 >= cap) break;
            s[w++] = (char)(0xF0 | (cp >> 18));
            s[w++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            s[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[w++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    s[w] = 0;
    return s;
}

/* $FILE_NAME namespace preference: Win32(1) > Win32&DOS(3) > POSIX(0) >
 * DOS(2); lower score wins. */
static int ns_score(uint8_t ns)
{
    switch (ns) {
    case 1: return 0;
    case 3: return 1;
    case 0: return 2;
    default: return 3;   /* 2 = DOS, anything else: last */
    }
}

/* Add a member; 0 ok, -1 decline (caps). */
static int member_add(vol *v, uint32_t idx, uint64_t usize, char *name,
                      run *runs, size_t nruns)
{
    member *nm;
    if (idx > MAX_IDX) {
        DBG("record %u: idx exceeds the FS ABI cap\n", idx);
        return -1;
    }
    if (v->nmem == MAX_MEMBERS) {
        DBG("member count exceeds the FS ABI cap\n");
        return -1;
    }
    nm = (member *)realloc(v->mem, (v->nmem + 1) * sizeof *nm);
    if (!nm) return -1;
    v->mem = nm;
    v->mem[v->nmem].idx = idx;
    v->mem[v->nmem].usize = usize;
    v->mem[v->nmem].name = name ? name : strdup("noname");
    v->mem[v->nmem].runs = runs;
    v->mem[v->nmem].nruns = nruns;
    if (!v->mem[v->nmem].name) return -1;
    v->nmem++;
    return 0;
}

/* Inspect one live FILE record; on decline return -1. */
static int record_scan(vol *v, uint64_t rn64)
{
    const uint8_t *r = v->recbuf;
    uint16_t flags = rd16(r + 22);
    uint32_t aoff;
    attr a;
    int arc;
    /* pass-1 collects */
    char *best = NULL;
    int best_score = 99;
    int have_name = 0, have_attrlist = 0, have_efs = 0;
    /* pass-2 collects */
    int n_undata = 0;              /* unnamed $DATA count */
    int undata_nonres = 0;
    uint64_t undata_real = 0;
    run *undata_runs = NULL;
    size_t undata_nruns = 0;
    int rc = -1;

    g_attr_rec = rn64;
    if (!(flags & FF_INUSE)) return 0;               /* free record */
    if (rd64(r + 32) != 0) {
        DBG("record %llu: extension record (attribute extents)\n",
            (unsigned long long)rn64);
        return -1;                                    /* attribute list */
    }

    /* pass 1: names, attribute lists, EFS */
    aoff = rd16(r + 20);
    while ((arc = attr_next(v, &aoff, &a)) == 1) {
        if (a.type == AT_ATTRLIST) {
            have_attrlist = 1;
        } else if (a.type == AT_FILENAME && !a.nonres) {
            const uint8_t *c = a.base + rd16(a.base + 20);
            uint32_t clen = rd32(a.base + 16);
            uint8_t nl, ns;
            char *utf;
            if (clen < 66) {
                DBG("record %llu: $FILE_NAME content %u < 66\n",
                    (unsigned long long)rn64, clen);
                return -1;
            }
            nl = c[64];
            ns = c[65];
            if ((uint64_t)66 + (uint64_t)nl * 2 > clen) {
                DBG("record %llu: $FILE_NAME name overruns content\n",
                    (unsigned long long)rn64);
                return -1;
            }
            utf = name_utf8(c + 66, nl);
            if (!utf) {
                DBG("record %llu: $FILE_NAME not valid UTF-16\n",
                    (unsigned long long)rn64);
                return -1;
            }
            have_name = 1;
            if (ns_score(ns) < best_score) {
                free(best);
                best = utf;
                best_score = ns_score(ns);
            } else {
                free(utf);
            }
        } else if (a.type == AT_LOGGED && a.namelen == 4) {
            const uint8_t *n = a.base + a.nameoff;
            if (rd16(n) == '$' && rd16(n + 2) == 'E' &&
                rd16(n + 4) == 'F' && rd16(n + 6) == 'S')
                have_efs = 1;
        }
    }
    if (arc < 0) return -1;

    /* pass 2: the unnamed $DATA. System streams (records < 16, '$'-named
     * files, directories) are never interpreted — their bytes are recipe
     * verbatim either way, so their flags/runlists can't refuse anything. */
    {
        int is_user = rn64 >= 16 && !(flags & FF_DIR) &&
                      have_name && best && best[0] != '$';
        aoff = rd16(r + 20);
        while ((arc = attr_next(v, &aoff, &a)) == 1) {
            if (a.type != AT_DATA) continue;
            if (a.namelen) {
                /* a named stream (an ADS): system files carry them
                 * legitimately ($Secure:$SDS, $BadClus:$Bad,
                 * $UsnJrnl:$J, $UpCase:$Info) — recipe anyway. On a
                 * user file an ADS is a v1 decline: enumerate covers
                 * the unnamed stream only and a named one must not
                 * slip past the bit-exact accounting half-understood. */
                if (is_user) {
                    DBG("record %llu: named $DATA (ADS) on a user file\n",
                        (unsigned long long)rn64);
                    goto out;
                }
                continue;
            }
            if (!is_user) continue;         /* system/resident: recipe */
            n_undata++;
            if (!a.nonres) continue;        /* resident: content is recipe */
            undata_nonres = 1;
            undata_real = rd64(a.base + 48);
            {
                uint16_t roff = rd16(a.base + 32);
                if ((uint32_t)roff >= a.len) goto out;
                if (runlist_parse(a.base, roff, a.len,
                                  &undata_runs, &undata_nruns) != 0) {
                    DBG("record %llu: bad runlist\n", (unsigned long long)rn64);
                    goto out;
                }
            }
            if (a.aflags & AF_COMPRESSED) {
                DBG("record %llu: compressed $DATA (LZNT1)\n",
                    (unsigned long long)rn64);
                goto out;
            }
            if (a.aflags & AF_ENCRYPTED) {
                DBG("record %llu: encrypted $DATA\n",
                    (unsigned long long)rn64);
                goto out;
            }
        }
        if (arc < 0) goto out;
    }

    if (have_attrlist) {
        DBG("record %llu: $ATTRIBUTE_LIST\n", (unsigned long long)rn64);
        goto out;
    }
    if (have_efs && rn64 >= 16) {
        DBG("record %llu: $EFS logged utility stream\n",
            (unsigned long long)rn64);
        goto out;
    }
    if (n_undata > 1) {
        DBG("record %llu: multiple unnamed $DATA\n",
            (unsigned long long)rn64);
        goto out;
    }
    if (undata_nonres) {
        /* a plain user file with cluster-backed content: a member */
        uint64_t cover = 0;
        size_t i;
        for (i = 0; i < undata_nruns; i++) {
            if (UINT64_MAX - cover < undata_runs[i].len) goto out;
            cover += undata_runs[i].len;
            if (!undata_runs[i].sparse) {
                uint64_t b0 = undata_runs[i].lcn, b1;
                if (b0 > UINT64_MAX / v->cs) goto out;
                b1 = b0 + undata_runs[i].len;
                if (b1 < b0 || b1 > UINT64_MAX / v->cs) goto out;
                if (b1 * v->cs > v->file_size) {
                    DBG("record %llu: run past the image end\n",
                        (unsigned long long)rn64);
                    goto out;
                }
            }
        }
        if (cover && undata_real > cover * v->cs) {
            DBG("record %llu: runlist cannot cover real size\n",
                (unsigned long long)rn64);
            goto out;
        }
        if (!cover && undata_real) goto out;
        if (member_add(v, (uint32_t)rn64, undata_real, best,
                       undata_runs, undata_nruns) != 0)
            goto out;
        best = NULL;             /* owned by the member now */
        undata_runs = NULL;
    }
    rc = 0;
out:
    free(best);
    free(undata_runs);
    return rc;
}

/* Full image parse: boot sector, record 0 ($MFT runlist), every FILE
 * record. Returns 0 ok; the member list is in v. */
static int vol_parse(vol *v)
{
    uint8_t bs[512];
    uint64_t rn;
    int8_t rec_shift;
    uint32_t spc;

    /* boot sector */
    if (pread_full(v->fd, bs, sizeof bs, 0) != 0) {
        DBG("short boot sector\n");
        return -1;
    }
    if (memcmp(bs + 3, "NTFS    ", 8) != 0) {
        DBG("bad magic\n");
        return -1;
    }
    v->bps = rd16(bs + 11);
    if (v->bps != 512) {
        DBG("bytes/sector %u: v1 supports 512 only\n", v->bps);
        return -1;
    }
    spc = bs[13];
    if (!spc || (spc & (spc - 1)) || spc > 128) {
        DBG("sectors/cluster %u: not a small power of two\n", spc);
        return -1;
    }
    v->cs = (uint64_t)v->bps * spc;
    v->mft_lcn = rd64(bs + 48);
    rec_shift = (int8_t)bs[64];
    if (rec_shift > 0)
        v->recsz = (uint64_t)rec_shift * v->cs;
    else if (rec_shift < 0 && rec_shift > -31)
        v->recsz = 1ull << (-rec_shift);
    else {
        DBG("bad MFT record shift %d\n", rec_shift);
        return -1;
    }
    if (v->recsz < 512 || v->recsz > 65536 || v->recsz % v->bps) {
        DBG("MFT record size %llu unsupported\n",
            (unsigned long long)v->recsz);
        return -1;
    }
    if (v->mft_lcn > UINT64_MAX / v->cs ||
        v->mft_lcn * v->cs + v->recsz > v->file_size) {
        DBG("MFT LCN past the image end\n");
        return -1;
    }
    v->recbuf = (uint8_t *)malloc((size_t)v->recsz);
    if (!v->recbuf) return -1;

    /* record 0 ($MFT): the runlist that maps every other record */
    if (pread_full(v->fd, v->recbuf, (size_t)v->recsz,
                   v->mft_lcn * v->cs) != 0) {
        DBG("record 0 unreadable\n");
        return -1;
    }
    if (memcmp(v->recbuf, "FILE", 4) != 0) {
        DBG("record 0 is not a FILE record\n");
        return -1;
    }
    {
        /* fixup on record 0 (same rules as record_read) */
        uint16_t usa_off = rd16(v->recbuf + 4), usa_n = rd16(v->recbuf + 6);
        size_t nsec = (size_t)(v->recsz / v->bps), i;
        uint16_t usn;
        if ((size_t)usa_n != nsec + 1 ||
            (uint64_t)usa_off + (uint64_t)usa_n * 2 > v->recsz) {
            DBG("record 0: bad USA\n");
            return -1;
        }
        usn = rd16(v->recbuf + usa_off);
        for (i = 0; i < nsec; i++) {
            size_t tail = (i + 1) * v->bps - 2;
            if (rd16(v->recbuf + tail) != usn) {
                DBG("record 0: fixup mismatch\n");
                return -1;
            }
            v->recbuf[tail] = v->recbuf[usa_off + 2 + 2 * i];
            v->recbuf[tail + 1] = v->recbuf[usa_off + 3 + 2 * i];
        }
    }
    {
        uint32_t aoff = rd16(v->recbuf + 20);
        attr a;
        int arc, found = 0;
        while ((arc = attr_next(v, &aoff, &a)) == 1) {
            if (a.type == AT_ATTRLIST) {
                DBG("$MFT uses an $ATTRIBUTE_LIST (multi-record $MFT)\n");
                return -1;
            }
            if (a.type != AT_DATA || a.namelen) continue;
            if (!a.nonres) {
                DBG("$MFT $DATA is resident: impossible\n");
                return -1;
            }
            if (a.aflags & (AF_COMPRESSED | AF_ENCRYPTED)) {
                DBG("$MFT $DATA compressed/encrypted\n");
                return -1;
            }
            if (found) {
                DBG("$MFT has multiple $DATA\n");
                return -1;
            }
            found = 1;
            v->mft_real = rd64(a.base + 48);
            {
                uint16_t roff = rd16(a.base + 32);
                if ((uint32_t)roff >= a.len ||
                    runlist_parse(a.base, roff, a.len,
                                  &v->mft_runs, &v->mft_nruns) != 0) {
                    DBG("$MFT runlist unparsable\n");
                    return -1;
                }
            }
        }
        if (arc < 0 || !found) {
            DBG("$MFT $DATA not found\n");
            return -1;
        }
    }
    {
        uint64_t cover = 0;
        size_t i;
        for (i = 0; i < v->mft_nruns; i++) {
            if (v->mft_runs[i].sparse) {
                DBG("$MFT is sparse: impossible\n");
                return -1;
            }
            if (UINT64_MAX - cover < v->mft_runs[i].len) return -1;
            cover += v->mft_runs[i].len;
            if (v->mft_runs[i].lcn > UINT64_MAX / v->cs ||
                v->mft_runs[i].len > UINT64_MAX / v->cs - v->mft_runs[i].lcn)
                return -1;
            if ((v->mft_runs[i].lcn + v->mft_runs[i].len) * v->cs >
                v->file_size) {
                DBG("$MFT run past the image end\n");
                return -1;
            }
        }
        if (!cover || v->mft_real > cover * v->cs) {
            DBG("$MFT runlist cannot cover its real size\n");
            return -1;
        }
    }
    v->nrec = v->mft_real / v->recsz;
    if (v->nrec == 0) {
        DBG("empty $MFT\n");
        return -1;
    }

    /* every FILE record */
    for (rn = 0; rn < v->nrec; rn++) {
        int rrc = record_read(v, rn);
        if (rrc < 0) {
            DBG("record %llu: unreadable/corrupt\n", (unsigned long long)rn);
            return -1;
        }
        if (rrc == 0) continue;         /* not a live FILE record */
        if (record_scan(v, rn) != 0) return -1;
    }
    return 0;
}

static int vol_open(const char *path, vol *v)
{
    struct stat st;
    memset(v, 0, sizeof *v);
    v->fd = open(path, O_RDONLY);
    if (v->fd < 0) return -1;
    if (fstat(v->fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 1024 || (uint64_t)st.st_size > MAX_IMAGE) {
        vol_close(v);
        return -1;
    }
    v->file_size = (uint64_t)st.st_size;
    if (vol_parse(v) != 0) {
        vol_close(v);
        return -1;
    }
    return 0;
}

static int member_find(const vol *v, uint32_t idx)
{
    size_t lo = 0, hi = v->nmem;
    while (lo < hi) {                   /* members are idx-ascending */
        size_t mid = (lo + hi) / 2;
        if (v->mem[mid].idx == idx) return (int)mid;
        if (v->mem[mid].idx < idx) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* ---- the disk-range partition (member ranges + recipe complement) ---- */
typedef struct {
    uint64_t off, len;
    uint32_t idx;        /* member idx (MEMBER) or 0 (RECIPE) */
    uint64_t src_off;    /* MEMBER: content offset in the member */
    uint8_t  kind;       /* 0 = RECIPE, 1 = MEMBER */
} range;

typedef struct {
    range  *r;
    size_t  n, cap;
    uint64_t recipe_blob_size;
} partition;

static int part_push(partition *p, uint64_t off, uint64_t len,
                     uint8_t kind, uint32_t idx, uint64_t src_off)
{
    range *nr;
    if (!len) return 0;
    if (p->n == MAX_MAP_ENTS) return -1;
    if (p->n == p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 64;
        nr = (range *)realloc(p->r, nc * sizeof *nr);
        if (!nr) return -1;
        p->r = nr;
        p->cap = nc;
    }
    p->r[p->n].off = off;
    p->r[p->n].len = len;
    p->r[p->n].kind = kind;
    p->r[p->n].idx = idx;
    p->r[p->n].src_off = src_off;
    p->n++;
    return 0;
}

static int range_cmp(const void *a, const void *b)
{
    uint64_t x = ((const range *)a)->off, y = ((const range *)b)->off;
    return x < y ? -1 : x > y;
}

/* Build the exact partition of [0, file_size): one MEMBER range per
 * non-sparse member run (clipped at usize), RECIPE ranges = the sorted
 * complement. Returns 0; -1 = overlapping member ranges (corrupt) or
 * the map entry cap exceeded. */
static int part_build(const vol *v, partition *p)
{
    size_t i, j;
    uint64_t pos = 0;

    memset(p, 0, sizeof *p);
    for (i = 0; i < v->nmem; i++) {
        const member *m = &v->mem[i];
        uint64_t coff = 0;
        for (j = 0; j < m->nruns; j++) {
            const run *rn = &m->runs[j];
            uint64_t bytes;
            if (rn->len > UINT64_MAX / v->cs) goto fail;
            bytes = rn->len * v->cs;
            if (!rn->sparse && coff < m->usize) {
                uint64_t slice = bytes;
                if (slice > m->usize - coff) slice = m->usize - coff;
                if (slice && rn->lcn <= UINT64_MAX / v->cs &&
                    rn->lcn * v->cs <= v->file_size &&
                    slice <= v->file_size - rn->lcn * v->cs) {
                    if (part_push(p, rn->lcn * v->cs, slice, 1,
                                  m->idx, coff) != 0)
                        goto fail;
                } else if (slice) {
                    goto fail;          /* run outside the image */
                }
            }
            if (UINT64_MAX - coff < bytes) goto fail;
            coff += bytes;
        }
    }
    /* complement: sort the member ranges, verify disjoint, fill gaps */
    if (p->n) {
        qsort(p->r, p->n, sizeof *p->r, range_cmp);
        for (i = 1; i < p->n; i++)
            if (p->r[i].off < p->r[i - 1].off + p->r[i - 1].len)
                goto fail;              /* overlapping members: corrupt */
    }
    {
        size_t nmem_ranges = p->n;
        uint64_t rpos = 0;
        /* weave the recipe gaps in (the member ranges are already
         * sorted; recipe ranges come out sorted too) */
        partition tmp = *p;
        range *mem_ranges = tmp.r;
        p->r = NULL; p->n = 0; p->cap = 0;
        for (i = 0; i < nmem_ranges; i++) {
            if (mem_ranges[i].off > rpos) {
                if (part_push(p, rpos, mem_ranges[i].off - rpos,
                              0, 0, 0) != 0) {
                    free(mem_ranges);
                    goto fail;
                }
            }
            rpos = mem_ranges[i].off + mem_ranges[i].len;
            if (part_push(p, mem_ranges[i].off, mem_ranges[i].len, 1,
                          mem_ranges[i].idx,
                          mem_ranges[i].src_off) != 0) {
                free(mem_ranges);
                goto fail;
            }
        }
        if (rpos < v->file_size)
            if (part_push(p, rpos, v->file_size - rpos, 0, 0, 0) != 0) {
                free(mem_ranges);
                goto fail;
            }
        free(mem_ranges);
    }
    /* the partition is sorted and disjoint by construction; assign the
     * recipe blob offsets (blob = recipe ranges concatenated in order) */
    pos = 0;
    for (i = 0; i < p->n; i++) {
        if (p->r[i].kind == 0) {
            p->r[i].src_off = pos;
            if (UINT64_MAX - pos < p->r[i].len) goto fail;
            pos += p->r[i].len;
        }
    }
    p->recipe_blob_size = pos;
    return 0;
fail:
    free(p->r);
    memset(p, 0, sizeof *p);
    return -1;
}

/* ---- commands ---- */

static int cmd_enumerate(const char *in, const char *out)
{
    vol v;
    FILE *f;
    size_t i;
    int rc = EXIT_DECLINE;

    if (vol_open(in, &v) != 0) return EXIT_DECLINE;
    f = fopen(out, "w");
    if (!f) { vol_close(&v); return 1; }
    for (i = 0; i < v.nmem; i++) {
        if (fprintf(f, "%u\t%s\t%llu\n", v.mem[i].idx, v.mem[i].name,
                    (unsigned long long)v.mem[i].usize) < 0) {
            fclose(f);
            vol_close(&v);
            return 1;
        }
    }
    if (fclose(f) != 0) { vol_close(&v); return 1; }
    rc = 0;
    vol_close(&v);
    return rc;
}

static int cmd_extract(const char *in, const char *idx_s, const char *out)
{
    vol v;
    char *endp = NULL;
    unsigned long idx_ul;
    int mi, ofd = -1, rc = EXIT_DECLINE;
    const member *m;
    uint8_t *buf = NULL;
    uint8_t *zeros = NULL;
    uint64_t written = 0, coff = 0;
    size_t j;

    errno = 0;
    idx_ul = strtoul(idx_s, &endp, 10);
    if (errno || !endp || *endp || idx_ul > MAX_IDX) return EXIT_DECLINE;
    if (vol_open(in, &v) != 0) return EXIT_DECLINE;
    mi = member_find(&v, (uint32_t)idx_ul);
    if (mi < 0) { DBG("extract: idx %lu unknown\n", idx_ul); goto out; }
    m = &v.mem[mi];
    ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (ofd < 0) { rc = 1; goto out; }
    buf = (uint8_t *)malloc(COPY_BUF_SZ);
    zeros = (uint8_t *)calloc(1, COPY_BUF_SZ);
    if (!buf || !zeros) { rc = 1; goto out; }
    for (j = 0; j < m->nruns && written < m->usize; j++) {
        const run *rn = &m->runs[j];
        uint64_t bytes = rn->len * v.cs;
        uint64_t slice = bytes, done = 0;
        if (slice > m->usize - written) slice = m->usize - written;
        if (!rn->sparse) {
            uint64_t doff = rn->lcn * v.cs;
            while (done < slice) {
                size_t want = (size_t)((slice - done) > COPY_BUF_SZ
                                       ? COPY_BUF_SZ : (slice - done));
                if (pread_full(v.fd, buf, want, doff + done) != 0) {
                    rc = 1;
                    goto out;
                }
                if (write_full(ofd, buf, want) != 0) { rc = 1; goto out; }
                done += want;
            }
        } else {
            while (done < slice) {
                size_t want = (size_t)((slice - done) > COPY_BUF_SZ
                                       ? COPY_BUF_SZ : (slice - done));
                if (write_full(ofd, zeros, want) != 0) { rc = 1; goto out; }
                done += want;
            }
        }
        written += slice;
        coff += bytes;
    }
    (void)coff;
    if (written != m->usize) goto out;   /* runlist under-covers */
    if (close(ofd) != 0) { ofd = -1; rc = 1; goto out; }
    ofd = -1;
    rc = 0;
out:
    if (ofd >= 0) close(ofd);
    free(buf);
    free(zeros);
    vol_close(&v);
    return rc;
}

/* Offset of the blob section inside the NTRC recipe file: 64-byte header
 * + member table (16 B + 24 B per non-sparse run) + range table (16 B per
 * recipe range). strip and map MUST agree on it: the FS stores the strip
 * output whole and reads RECIPE map sources at src_off inside that whole
 * file, so map's RECIPE src_off = ntrc_blob_off + the range's cumulative
 * blob offset. */
static uint64_t ntrc_blob_off(const vol *v, const partition *p)
{
    uint64_t off = 64;
    size_t i, j;
    for (i = 0; i < v->nmem; i++) {
        off += 16;
        for (j = 0; j < v->mem[i].nruns; j++)
            if (!v->mem[i].runs[j].sparse) off += 24;
    }
    for (i = 0; i < p->n; i++)
        if (p->r[i].kind == 0) off += 16;
    return off;
}

/* Write the NTRC header + tables; returns the blob offset. */
static int strip_write_tables(int ofd, const vol *v, const partition *p,
                              uint64_t *blob_off_out)
{
    uint8_t hdr[64];
    uint64_t blob_off;
    size_t i, j;

    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "NTRC", 4);
    wr32(hdr + 4, 1);                              /* version */
    wr64(hdr + 8, v->file_size);
    wr64(hdr + 16, p->recipe_blob_size);
    wr32(hdr + 24, (uint32_t)v->cs);
    wr32(hdr + 28, (uint32_t)v->recsz);
    wr32(hdr + 32, (uint32_t)v->nmem);
    wr32(hdr + 36, (uint32_t)0);                   /* n_ranges: patched */
    /* count recipe ranges */
    {
        uint32_t nrec_ranges = 0;
        for (i = 0; i < p->n; i++)
            if (p->r[i].kind == 0) nrec_ranges++;
        wr32(hdr + 36, nrec_ranges);
    }
    if (write_full(ofd, hdr, sizeof hdr) != 0) return -1;
    blob_off = sizeof hdr;
    for (i = 0; i < v->nmem; i++) {
        const member *m = &v->mem[i];
        uint8_t rec[16];
        uint64_t coff = 0;
        uint32_t nreal = 0;
        for (j = 0; j < m->nruns; j++)
            if (!m->runs[j].sparse) nreal++;
        wr32(rec, m->idx);
        wr32(rec + 4, nreal);
        wr64(rec + 8, m->usize);
        if (write_full(ofd, rec, sizeof rec) != 0) return -1;
        blob_off += sizeof rec;
        for (j = 0; j < m->nruns; j++) {
            uint8_t rb[24];
            uint64_t bytes = m->runs[j].len * v->cs;
            if (!m->runs[j].sparse) {
                wr64(rb, m->runs[j].lcn);
                wr64(rb + 8, m->runs[j].len);
                wr64(rb + 16, coff);
                if (write_full(ofd, rb, sizeof rb) != 0) return -1;
                blob_off += sizeof rb;
            }
            coff += bytes;
        }
    }
    for (i = 0; i < p->n; i++) {
        if (p->r[i].kind == 0) {
            uint8_t rb[16];
            wr64(rb, p->r[i].off);
            wr64(rb + 8, p->r[i].len);
            if (write_full(ofd, rb, sizeof rb) != 0) return -1;
            blob_off += sizeof rb;
        }
    }
    *blob_off_out = blob_off;
    return 0;
}

static int cmd_strip(const char *in, const char *out)
{
    vol v;
    partition p;
    int ofd = -1, rc = EXIT_DECLINE;
    uint8_t *buf = NULL;
    uint64_t blob_off = 0;
    size_t i;

    if (vol_open(in, &v) != 0) return EXIT_DECLINE;
    if (part_build(&v, &p) != 0) {
        DBG("strip: cannot partition the image\n");
        vol_close(&v);
        return EXIT_DECLINE;
    }
    ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (!ofd) { rc = 1; goto out; }
    if (strip_write_tables(ofd, &v, &p, &blob_off) != 0) { rc = 1; goto out; }
    buf = (uint8_t *)malloc(COPY_BUF_SZ);
    if (!buf) { rc = 1; goto out; }
    for (i = 0; i < p.n; i++) {
        if (p.r[i].kind != 0) continue;
        uint64_t done = 0;
        while (done < p.r[i].len) {
            size_t want = (size_t)((p.r[i].len - done) > COPY_BUF_SZ
                                   ? COPY_BUF_SZ : (p.r[i].len - done));
            if (pread_full(v.fd, buf, want, p.r[i].off + done) != 0 ||
                write_full(ofd, buf, want) != 0) {
                rc = 1;
                goto out;
            }
            done += want;
        }
    }
    if (close(ofd) != 0) { ofd = -1; rc = 1; goto out; }
    ofd = -1;
    rc = 0;
out:
    if (ofd >= 0) close(ofd);
    free(buf);
    free(p.r);
    vol_close(&v);
    return rc;
}

static int cmd_map(const char *in, const char *out)
{
    vol v;
    partition p;
    int ofd = -1, rc = EXIT_DECLINE;
    uint8_t hdr[8];
    size_t i;

    if (vol_open(in, &v) != 0) return EXIT_DECLINE;
    if (part_build(&v, &p) != 0) {
        DBG("map: cannot partition the image\n");
        vol_close(&v);
        return EXIT_DECLINE;
    }
    if (!p.n) goto out;                 /* cannot happen: size > 0 */
    {
        uint64_t blob_base = ntrc_blob_off(&v, &p);
        ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (!ofd) { rc = 1; goto out; }
        memcpy(hdr, "MRMP", 4);
        wr32(hdr + 4, (uint32_t)p.n);
        if (write_full(ofd, hdr, sizeof hdr) != 0) { rc = 1; goto out; }
        for (i = 0; i < p.n; i++) {
            uint8_t e[29];
            /* RECIPE src_off is inside the WHOLE strip file (header +
             * tables + blob): the FS reads the stored recipe segment */
            uint64_t src = p.r[i].src_off +
                           (p.r[i].kind == 0 ? blob_base : 0);
            wr64(e, p.r[i].off);
            wr64(e + 8, p.r[i].len);
            e[16] = p.r[i].kind;
            wr32(e + 17, p.r[i].idx);
            wr64(e + 21, src);
            if (write_full(ofd, e, sizeof e) != 0) { rc = 1; goto out; }
        }
    }
    if (close(ofd) != 0) { ofd = -1; rc = 1; goto out; }
    ofd = -1;
    rc = 0;
out:
    if (ofd >= 0) close(ofd);
    free(p.r);
    vol_close(&v);
    return rc;
}

static int cmd_estimate(const char *in)
{
    vol v;
    uint64_t sum = 0;
    size_t i;

    if (vol_open(in, &v) != 0) return EXIT_DECLINE;
    for (i = 0; i < v.nmem; i++) {
        if (UINT64_MAX - sum < v.mem[i].usize) {
            vol_close(&v);
            return EXIT_DECLINE;
        }
        sum += v.mem[i].usize;
    }
    if (UINT64_MAX - sum < ESTIMATE_SLACK) {
        vol_close(&v);
        return EXIT_DECLINE;
    }
    printf("%llu\n", (unsigned long long)(sum + ESTIMATE_SLACK));
    vol_close(&v);
    return 0;
}

/* ---- rebuild: splice the NTRC recipe + the member files back into the
 * original image. Parses NO NTFS at all (the recipe is self-contained);
 * fixup bytes are inside the recipe's verbatim ranges (see the header
 * comment). ---- */
typedef struct {
    uint32_t idx;
    uint32_t nruns;
    uint64_t usize;
    uint8_t *runs;         /* nruns x 24: {lcn, len_clusters, content_off} */
} rmember;

static int rread(int fd, uint64_t *pos, void *buf, size_t len)
{
    if (pread_full(fd, buf, len, *pos) != 0) return -1;
    *pos += len;
    return 0;
}

static int cmd_rebuild(const char *recipe, const char *dir, const char *out)
{
    int rfd = -1, ofd = -1, rc = EXIT_DECLINE;
    uint8_t hdr[64];
    uint64_t pos = 0, image_size, blob_size, blob_off;
    uint32_t version, n_members, n_ranges;
    rmember *mem = NULL;
    uint8_t *ranges = NULL;            /* n_ranges x 16: {orig_off, len} */
    uint8_t *buf = NULL;
    size_t i;
    uint64_t expect_size;

    rfd = open(recipe, O_RDONLY);
    if (rfd < 0) return EXIT_DECLINE;
    if (rread(rfd, &pos, hdr, sizeof hdr) != 0) goto out;
    if (memcmp(hdr, "NTRC", 4) != 0) { DBG("rebuild: bad magic\n"); goto out; }
    version = rd32(hdr + 4);
    if (version != 1) { DBG("rebuild: version %u\n", version); goto out; }
    image_size = rd64(hdr + 8);
    blob_size = rd64(hdr + 16);
    n_members = rd32(hdr + 32);
    n_ranges = rd32(hdr + 36);
    if (!image_size || image_size > MAX_IMAGE) goto out;
    if (n_members > MAX_MEMBERS || n_ranges > MAX_MAP_ENTS) goto out;

    mem = (rmember *)calloc(n_members ? n_members : 1, sizeof *mem);
    if (!mem) { rc = 1; goto out; }
    for (i = 0; i < n_members; i++) {
        uint8_t rec[16];
        if (rread(rfd, &pos, rec, sizeof rec) != 0) goto out;
        mem[i].idx = rd32(rec);
        mem[i].nruns = rd32(rec + 4);
        mem[i].usize = rd64(rec + 8);
        if (mem[i].nruns > (MAX_MAP_ENTS + 1u)) goto out;
        mem[i].runs = (uint8_t *)malloc((size_t)mem[i].nruns * 24 + 1);
        if (!mem[i].runs) { rc = 1; goto out; }
        if (rread(rfd, &pos, mem[i].runs, (size_t)mem[i].nruns * 24) != 0)
            goto out;
    }
    ranges = (uint8_t *)malloc((size_t)n_ranges * 16 + 1);
    if (!ranges) { rc = 1; goto out; }
    if (rread(rfd, &pos, ranges, (size_t)n_ranges * 16) != 0) goto out;
    blob_off = pos;
    /* the recipe file must be exactly header + tables + blob */
    {
        struct stat st;
        if (fstat(rfd, &st) != 0) { rc = 1; goto out; }
        if ((uint64_t)st.st_size != blob_off + blob_size) {
            DBG("rebuild: recipe size mismatch\n");
            goto out;
        }
    }
    /* range sanity: sorted, disjoint, inside the image, blob-consistent */
    {
        uint64_t rpos = 0, bpos = 0;
        for (i = 0; i < n_ranges; i++) {
            uint64_t off = rd64(ranges + i * 16);
            uint64_t len = rd64(ranges + i * 16 + 8);
            if (!len || off < rpos || len > image_size - off) goto out;
            rpos = off + len;
            if (UINT64_MAX - bpos < len) goto out;
            bpos += len;
        }
        if (bpos != blob_size) goto out;
    }
    buf = (uint8_t *)malloc(COPY_BUF_SZ);
    if (!buf) { rc = 1; goto out; }

    ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (!ofd) { rc = 1; goto out; }
    if (ftruncate(ofd, (off_t)image_size) != 0) { rc = 1; goto out; }

    /* recipe ranges verbatim (they carry the on-disk bytes, fixup
     * trailers included — nothing is recomputed) */
    {
        uint64_t bpos = 0;
        for (i = 0; i < n_ranges; i++) {
            uint64_t off = rd64(ranges + i * 16);
            uint64_t len = rd64(ranges + i * 16 + 8);
            uint64_t done = 0;
            while (done < len) {
                size_t want = (size_t)((len - done) > COPY_BUF_SZ
                                       ? COPY_BUF_SZ : (len - done));
                if (pread_full(rfd, buf, want, blob_off + bpos + done) != 0 ||
                    pwrite_full(ofd, buf, want, off + done) != 0) {
                    rc = 1;
                    goto out;
                }
                done += want;
            }
            bpos += len;
        }
    }
    /* members: slice each member file into its run clusters */
    for (i = 0; i < n_members; i++) {
        char path[4096];
        int mfd;
        struct stat st;
        uint32_t j;
        int n = snprintf(path, sizeof path, "%s/%u", dir, mem[i].idx);
        if (n <= 0 || (size_t)n >= sizeof path) goto out;
        mfd = open(path, O_RDONLY);
        if (mfd < 0) { DBG("rebuild: member %u missing\n", mem[i].idx); goto out; }
        if (fstat(mfd, &st) != 0 || (uint64_t)st.st_size != mem[i].usize) {
            DBG("rebuild: member %u size mismatch\n", mem[i].idx);
            close(mfd);
            goto out;
        }
        for (j = 0; j < mem[i].nruns; j++) {
            uint64_t lcn = rd64(mem[i].runs + j * 24);
            uint64_t lencl = rd64(mem[i].runs + j * 24 + 8);
            uint64_t coff = rd64(mem[i].runs + j * 24 + 16);
            uint64_t slice, done = 0, cs = rd32(hdr + 24);
            /* the writer's slice rule (part_build): min(len*cs,
             * usize - coff); cs rides in the header and is validated
             * here — the FS's byte-exact guard proves the result */
            if (!cs || (cs & (cs - 1)) || cs > (64u << 20)) {
                close(mfd); goto out;
            }
            if (lencl > UINT64_MAX / cs) { close(mfd); goto out; }
            if (coff > mem[i].usize) { close(mfd); goto out; }
            slice = lencl * cs;
            if (slice > mem[i].usize - coff) slice = mem[i].usize - coff;
            if (lcn > UINT64_MAX / cs) { close(mfd); goto out; }
            {
                uint64_t doff = lcn * cs;
                if (doff > image_size || slice > image_size - doff) {
                    close(mfd); goto out;
                }
                while (done < slice) {
                    size_t want = (size_t)((slice - done) > COPY_BUF_SZ
                                           ? COPY_BUF_SZ : (slice - done));
                    if (pread_full(mfd, buf, want, coff + done) != 0 ||
                        pwrite_full(ofd, buf, want, doff + done) != 0) {
                        close(mfd);
                        rc = 1;
                        goto out;
                    }
                    done += want;
                }
            }
        }
        close(mfd);
    }
    /* every byte must have been written: recipe union member slices is
     * the whole image by construction, but verify the cheap invariant —
     * the output size — and let the FS's memcmp guard be the proof */
    expect_size = image_size;
    {
        struct stat st;
        if (fstat(ofd, &st) != 0 || (uint64_t)st.st_size != expect_size) {
            rc = 1;
            goto out;
        }
    }
    if (close(ofd) != 0) { ofd = -1; rc = 1; goto out; }
    ofd = -1;
    rc = 0;
out:
    if (ofd >= 0) close(ofd);
    if (rfd >= 0) close(rfd);
    if (mem) {
        for (i = 0; i < n_members; i++) free(mem[i].runs);
        free(mem);
    }
    free(ranges);
    free(buf);
    return rc;
}


/* ---- ivpack plugin C ABI export (ADR-007) --------------------------------
 * Built as libntfs.so with -DIVPACK_SHARED_LIB -fPIC -shared; the fork
 * guard in ivpack_impl.h keeps the CLI's exit(3)=decline / exit(1)=error
 * contract intact inside a long-lived worker. See ivpack_impl.h. */
static const ivpack_desc s_ntfs_desc = {
    IVPACK_API_VERSION, "ntfs", "1.0.0", "containerpack", 0
};

const ivpack_desc *ivpack_get_desc(void) { return &s_ntfs_desc; }

IVPACK_CANON_CALLS(ntfs)

IVPACK_DEFINE_CONTAINER_CMD(ntfs, IVPACK_GLUE_NONE())

IVPACK_DEFINE_CONTAINER_ESTIMATE(ntfs, IVPACK_GLUE_NONE(),
                             rc = (int)cmd_estimate(a);)

#ifndef IVPACK_SHARED_LIB
int main(int argc, char **argv)
{
    dbg = getenv("NTFS_PACK_DEBUG") != NULL;
    if (argc < 2) return 2;
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
    return 2;
}
#endif /* !IVPACK_SHARED_LIB */

