/* vol_exer.c — WP14b M2 exe-as-container carve: shared helpers +
 * sweep-side carve. Split from volume.c. */

#include "volume_internal.h"


/* ---- WP14b M2: exe-as-container shared helpers ----
 * The main record's segment is ZSTD-19 of the EXER payload (see
 * invarifs.h): [4B "EXER"][u32 LE num_parts][per part u64 file_offset +
 * u64 member_len + u8 codec][glue bytes]. codec = INVFS_ALGO_JXL for JPEG
 * members (the sibling "name!exrN" holds the lossless JXL blob),
 * INVFS_ALGO_ZSTD for PNG members (recognized and carved, but stored
 * recompressed-only: djxl's PNG output is a fresh encoding, so a
 * pixel-transcode could never pass the bit-exact guard -- the PNGR
 * machinery that could rebuild one is Windows-only, WP12(c)). */

static void exer_wr64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) { p[i] = (uint8_t)v; v >>= 8; }
}


static uint64_t exer_rd64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}


#define EXE_MEDIA_MIN (16 * 1024)   /* smaller finds are not worth carving */


typedef struct {
    uint64_t off, len;
    int kind;                       /* 1 = JPEG, 2 = PNG */
} exe_region;


/* End of the JPEG stream that starts at p (FF D8 FF validated by the
 * caller): walk the marker stream to EOI. Segments carry their length, so
 * thumbnails inside APPn cannot truncate the walk; after SOS the
 * entropy-coded run ends at the first FF NOT followed by 00 (stuffing),
 * D0-D7 (RSTn) or FF (fill) -- that is the next marker. Valid only if a
 * SOFn appeared before EOI. Returns the offset just past FF D9, 0 on
 * truncation/malformation. */
static size_t jpeg_scan_end(const uint8_t *b, size_t n, size_t p)
{
    int saw_sof = 0;

    p += 2;   /* past SOI */
    while (p + 1 < n) {
        uint8_t m;
        if (b[p] != 0xFF) return 0;
        m = b[p + 1];
        if (m == 0xFF) { p++; continue; }      /* fill bytes */
        if (m == 0x00) return 0;               /* stuffed byte: not a marker */
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            p += 2;                            /* SOI/TEM/RSTn: no length */
            continue;
        }
        if (m == 0xD9)
            return saw_sof ? p + 2 : 0;        /* EOI */
        if (p + 4 > n) return 0;
        {
            unsigned sl = ((unsigned)b[p + 2] << 8) | b[p + 3];
            if (sl < 2 || p + 2 + sl > n) return 0;
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 &&
                m != 0xCC)
                saw_sof = 1;                   /* SOFn (any encoding) */
            p += 2 + sl;
            if (m == 0xDA) {
                /* SOS: entropy run follows the header */
                for (;;) {
                    if (p + 1 >= n) return 0;
                    if (b[p] == 0xFF && b[p + 1] != 0x00 &&
                        !(b[p + 1] >= 0xD0 && b[p + 1] <= 0xD7) &&
                        b[p + 1] != 0xFF)
                        break;
                    p++;
                }
            }
        }
    }
    return 0;
}


/* End of the PNG stream starting at p (8-byte signature validated by the
 * caller): walk [u32 BE len][4B type][data][4B crc] chunks to IEND; IHDR
 * must come first. Returns the offset just past IEND, 0 on truncation or
 * structural garbage. */
static size_t png_scan_end(const uint8_t *b, size_t n, size_t p)
{
    int first = 1;

    p += 8;
    while (p + 12 <= n) {           /* shortest chunk: len+type+crc */
        uint32_t cl = ((uint32_t)b[p] << 24) | ((uint32_t)b[p + 1] << 16) |
                      ((uint32_t)b[p + 2] << 8) | b[p + 3];
        if ((uint64_t)cl + 12 > n - p) return 0;
        if (first && memcmp(b + p + 4, "IHDR", 4) != 0) return 0;
        first = 0;
        if (memcmp(b + p + 4, "IEND", 4) == 0)
            return p + 12 + cl;
        p += 12 + cl;
    }
    return 0;
}


/* Scan an executable image for embedded media worth carving (>= 16 KiB).
 * Regions are returned in file order; a validated stream's bytes are never
 * rescanned (no false hits inside an accepted JPEG/PNG). A random FF D8 FF
 * in code dies in jpeg_scan_end's marker walk long before the cjxl guard
 * would ever see it. */
static size_t exe_scan_media(const uint8_t *b, size_t n,
                             exe_region *out, size_t cap)
{
    size_t p = 0, cnt = 0;

    while (p + EXE_MEDIA_MIN <= n && cnt < cap) {
        size_t end = 0;
        int kind = 0;
        if (b[p] == 0xFF && p + 3 <= n &&
            b[p + 1] == 0xD8 && b[p + 2] == 0xFF) {
            end = jpeg_scan_end(b, n, p);
            kind = 1;
        } else if (b[p] == 0x89 && p + 8 <= n &&
                   memcmp(b + p, "\x89PNG\r\n\x1a\n", 8) == 0) {
            end = png_scan_end(b, n, p);
            kind = 2;
        }
        if (end && end - p >= EXE_MEDIA_MIN) {
            out[cnt].off = p;
            out[cnt].len = end - p;
            out[cnt].kind = kind;
            cnt++;
            p = end;
        } else {
            p++;
        }
    }
    return cnt;
}


/* Parse + validate an EXER payload. Disk input, never trusted: magic, part
 * count (1..EXE_MAX_MEDIA), ascending non-overlapping ranges inside
 * [0, file_size), and the glue must account for every byte the parts do
 * not cover. codecid is INVFS_ALGO_JXL or INVFS_ALGO_ZSTD; anything else
 * fails loudly (a newer encoder wrote it). Returns 0 and fills rows[] or
 * -1 on any violation. */
int exer_payload_parse(const uint8_t *pay, size_t pay_len,
                              uint64_t file_size,
                              exer_row *rows, size_t cap, size_t *n_out)
{
    uint32_t n, i;
    uint64_t prev_end = 0, member_sum = 0;
    size_t glue_len;

    if (pay_len < 8 || memcmp(pay, "EXER", 4) != 0) return -1;
    n = (uint32_t)pay[4] | ((uint32_t)pay[5] << 8) |
        ((uint32_t)pay[6] << 16) | ((uint32_t)pay[7] << 24);
    if (n == 0 || n > EXE_MAX_MEDIA || (size_t)n > cap) return -1;
    if ((uint64_t)8 + 17ull * n > pay_len) return -1;
    glue_len = pay_len - 8 - 17 * (size_t)n;
    for (i = 0; i < n; i++) {
        const uint8_t *r = pay + 8 + 17 * (size_t)i;
        rows[i].off = exer_rd64(r);
        rows[i].len = exer_rd64(r + 8);
        rows[i].codec = r[16];
        if (rows[i].codec != INVFS_ALGO_JXL &&
            rows[i].codec != INVFS_ALGO_ZSTD)
            return -1;
        if (rows[i].len == 0 || rows[i].len > file_size ||
            rows[i].off > file_size - rows[i].len)
            return -1;
        if (rows[i].off < prev_end) return -1;   /* ascending, no overlap */
        prev_end = rows[i].off + rows[i].len;
        member_sum += rows[i].len;
    }
    if (member_sum + glue_len != file_size) return -1;
    *n_out = n;
    return 0;
}


/* Splice an EXER payload's glue + the decoded part buffers into dst
 * (file_size bytes). Rows come pre-validated from exer_payload_parse, so
 * the glue arithmetic cannot overrun: parts are ascending, inside the
 * file, and member_sum + glue_len == file_size. */
void exer_splice(const uint8_t *pay, const exer_row *rows, size_t n,
                        uint8_t *const *parts, uint8_t *dst, uint64_t file_size)
{
    size_t gp = 8 + 17 * n;   /* glue cursor: past header + table */
    uint64_t fp = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        memcpy(dst + fp, pay + gp, (size_t)(rows[i].off - fp));
        gp += (size_t)(rows[i].off - fp);
        memcpy(dst + rows[i].off, parts[i], (size_t)rows[i].len);
        fp = rows[i].off + rows[i].len;
    }
    memcpy(dst + fp, pay + gp, (size_t)(file_size - fp));
}


/* ---- WP14b M2: exe-as-container carve (sweep side) ----
 *
 * A binary-family file (ELF/PE/Mach-O per invfs_binary_family) is scanned
 * for embedded media (exe_scan_media: validated JPEG/PNG streams, each
 * >= 16 KiB, at most EXE_MAX_MEDIA). Admission: the media must be worth
 * carving (sum of member lengths >= 64 KiB AND >= 5% of the file) and the
 * read-time working set (decoded payload + member bytes, ~usize + parts)
 * must fit the decode-memory policy. Then per member: JPEG transcodes via
 * the WP11 machinery (cjxl --lossless_jpeg=1, djxl decode-back memcmp
 * guard; a refusal skips just that member -- its bytes stay in the glue),
 * PNG is recompressed ZSTD-19 only (no pixel transcode can be bit-exact
 * while PNGR is Windows-only). The house invariant is checked as a whole
 * before anything reaches disk: ZSTD-decompress the final blob, splice
 * the decode-proven members at the table offsets, memcmp against the
 * original -- any mismatch abandons the carve.
 *
 * Storage: parts land first as "name!exrN" whole-file blob inodes
 * (algo=JXL / algo=ZSTD), then the main record replaces the file as one
 * EXER segment (children-first, the FLAC note), then the old record is
 * tombstoned and the classes stamped (main CONTAINER{EXER}, JXL parts
 * CODEC{JXL}, ZSTD parts GENERIC{ZSTD}).
 *
 * Returns 1 = carved (the caller reports rc 11), 0 = declined (caller
 * falls through to binary batching), 2 = decode-memory refusal
 * (GENERIC_MEMLIMIT{EXER} stamped; the caller must NOT batch -- the retry
 * re-arms from generic storage when the limit rises, the JXL pattern). */
int vol_exer_carve(invfs_volume *v, uint64_t inode_id,
                          const char *name, const uint8_t *full,
                          size_t full_len, uint32_t *nparts_out)
{
    const invfs_codec *jc = invfs_codec_by_algo(INVFS_ALGO_JXL);
    exe_region reg[EXE_MAX_MEDIA];
    /* per-member build state: the stored blob plus its decode-back proof */
    struct { uint8_t *blob, *back; size_t blen; } pm[EXE_MAX_MEDIA];
    exer_row rows[EXE_MAX_MEDIA];
    invfs_meta_pub keep;
    int have_keep, rc = 0;
    size_t nr, i, kept = 0;
    uint64_t media_sum = 0, kept_sum = 0, blob_total;
    uint8_t *pay = NULL, *cblob = NULL;
    size_t pay_len, cblob_len = 0;

    /* cjxl absent: no carve at all -- the JPEG members are the point */
    if (!jc || !jc->probe || !jc->probe()) return 0;
    if (name_too_long_for_children(name)) return 0;
    {
        /* leftover siblings can only come from a carve killed mid-commit
         * (a finished one is CONTAINER-stamped and never reaches here):
         * purge and proceed rather than refusing the file forever */
        char p0[288];
        snprintf(p0, sizeof p0, "%s!exr0", name);
        if (vol_find(v, p0) != 0)
            vol_delete_siblings(v, name);
    }

    nr = exe_scan_media(full, full_len, reg, EXE_MAX_MEDIA);
    if (nr == 0) return 0;
    for (i = 0; i < nr; i++) media_sum += reg[i].len;
    if (media_sum < (64ull << 10) || media_sum * 20 < full_len)
        return 0;   /* not worth carving: binary batching is its home */
    {
        /* decode-time working set (WP10 §12.2, mirrored): the read holds
         * the decoded payload (table + glue) plus the member bytes */
        uint64_t usize = 8 + 17 * (uint64_t)nr + (full_len - media_sum);
        if (usize + media_sum > vol_get_dec_mem_limit(v)) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                            INVFS_ALGO_EXER, tz_codec_gen(INVFS_ALGO_EXER));
            return 2;
        }
    }

    have_keep = vol_get_meta(v, inode_id, &keep) == 0;
    memset(pm, 0, sizeof pm);
    for (i = 0; i < nr; i++) {
        const uint8_t *mem = full + reg[i].off;
        size_t ml = (size_t)reg[i].len;
        exer_row *r = &rows[kept];

        if (reg[i].kind == 1) {
            /* JPEG -> lossless JXL; SOF-geometry admission first (the
             * whole-file branch's rule, per member) */
            uint64_t raw = jpeg_raw_estimate(mem, ml);
            size_t bl = 0;
            if (raw && raw > vol_get_dec_mem_limit(v)) continue;
            if (invfs_jxl_compress(mem, ml, &pm[kept].blob,
                                   &pm[kept].blen) != 0 ||
                pm[kept].blen >= ml)
                goto skip;
            if (invfs_jxl_decompress(pm[kept].blob, pm[kept].blen,
                                     &pm[kept].back, &bl) != 0 ||
                bl != ml || memcmp(pm[kept].back, mem, ml) != 0)
                goto skip;   /* guard refused: the range stays glue */
            r->codec = INVFS_ALGO_JXL;
        } else {
            /* PNG: carved verbatim under ZSTD-19 (see the header note) */
            size_t cb = ZSTD_compressBound(ml), cl;
            pm[kept].blob = (uint8_t *)malloc(cb);
            pm[kept].back = (uint8_t *)malloc(ml);
            if (!pm[kept].blob || !pm[kept].back)
                goto skip;
            cl = ZSTD_compress(pm[kept].blob, cb, mem, ml, 19);
            if (ZSTD_isError(cl) || cl >= ml)
                goto skip;
            pm[kept].blen = cl;
            {
                size_t d = ZSTD_decompress(pm[kept].back, ml,
                                           pm[kept].blob, cl);
                if (ZSTD_isError(d) || d != ml)
                    goto skip;
            }
            r->codec = INVFS_ALGO_ZSTD;
        }
        r->off = reg[i].off;
        r->len = reg[i].len;
        kept_sum += r->len;
        kept++;
        continue;
    skip:
        free(pm[kept].blob); free(pm[kept].back);
        memset(&pm[kept], 0, sizeof pm[kept]);
    }
    if (kept == 0)
        goto out;   /* every member refused: binary batching is its home */

    /* payload = [EXER][u32 n][rows][glue (original minus carved ranges)] */
    pay_len = 8 + 17 * kept + (size_t)(full_len - kept_sum);
    pay = (uint8_t *)malloc(pay_len);
    cblob = (uint8_t *)malloc(ZSTD_compressBound(pay_len));
    if (!pay || !cblob) goto out;
    memcpy(pay, "EXER", 4);
    pay[4] = (uint8_t)kept;
    pay[5] = (uint8_t)(kept >> 8);
    pay[6] = (uint8_t)(kept >> 16);
    pay[7] = (uint8_t)(kept >> 24);
    {
        size_t gp = 8 + 17 * kept;
        uint64_t fp = 0;
        for (i = 0; i < kept; i++) {
            exer_wr64(pay + 8 + 17 * i, rows[i].off);
            exer_wr64(pay + 8 + 17 * i + 8, rows[i].len);
            pay[8 + 17 * i + 16] = rows[i].codec;
            memcpy(pay + gp, full + fp, (size_t)(rows[i].off - fp));
            gp += (size_t)(rows[i].off - fp);
            fp = rows[i].off + rows[i].len;
        }
        memcpy(pay + gp, full + fp, (size_t)(full_len - fp));
    }
    {
        size_t cl = ZSTD_compress(cblob, ZSTD_compressBound(pay_len),
                                  pay, pay_len, 19);
        if (ZSTD_isError(cl)) goto out;
        cblob_len = cl;
    }

    /* the house invariant, whole-file form: decode the FINAL blob, parse
     * the table back out of it, splice the decode-proven member bytes at
     * the table offsets, and memcmp the rebuild against the original */
    {
        uint8_t *dec = (uint8_t *)malloc(pay_len);
        uint8_t *reb = (uint8_t *)malloc(full_len ? full_len : 1);
        uint8_t *pb[EXE_MAX_MEDIA];
        exer_row *grows = NULL;
        size_t gn = 0;
        int okm = 0;

        if (dec && reb) {
            size_t d = ZSTD_decompress(dec, pay_len, cblob, cblob_len);
            grows = (exer_row *)malloc(EXE_MAX_MEDIA * sizeof *grows);
            if (grows && !ZSTD_isError(d) && d == pay_len &&
                exer_payload_parse(dec, d, full_len, grows,
                                   EXE_MAX_MEDIA, &gn) == 0 && gn == kept) {
                for (i = 0; i < kept; i++) pb[i] = pm[i].back;
                exer_splice(dec, grows, gn, pb, reb, full_len);
                okm = memcmp(reb, full, full_len) == 0;
            }
        }
        free(dec); free(reb); free(grows);
        if (!okm) {
            fprintf(stderr, "EXER: %s: rebuild guard refused, "
                            "carve abandoned\n", name);
            goto out;
        }
    }

    /* size guard (the TAR/GZ rule): the stored form must beat the file */
    blob_total = cblob_len;
    for (i = 0; i < kept; i++) blob_total += pm[i].blen;
    if (blob_total >= full_len) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[vol] %s: EXER blobs %llu >= exe %zu -- keep "
                            "original\n", name, (unsigned long long)blob_total,
                    full_len);
        goto out;
    }

    /* children first, the name-owning record last (the FLAC note) */
    for (i = 0; i < kept; i++) {
        char pn[288];
        uint64_t pino;
        snprintf(pn, sizeof pn, "%s!exr%zu", name, i);
        pino = vol_create_blob_file(v, pn, pm[i].blob, pm[i].blen,
                                    rows[i].len, rows[i].codec);
        if (!pino) {
            fprintf(stderr, "EXER: part inode failed for %s\n", pn);
            vol_transcode_abort(v, name);
            goto out;
        }
        if (rows[i].codec == INVFS_ALGO_JXL)
            vol_stamp_class(v, pino, INVFS_CLASS_CODEC, INVFS_ALGO_JXL,
                            tz_codec_gen(INVFS_ALGO_JXL));
        else
            vol_stamp_class(v, pino, INVFS_CLASS_GENERIC, INVFS_ALGO_ZSTD,
                            tz_codec_gen(INVFS_ALGO_ZSTD));
    }
    {
        uint64_t newino = vol_create_blob_file(v, name, cblob, cblob_len,
                                               (uint64_t)full_len,
                                               INVFS_ALGO_EXER);
        if (!newino) {
            /* no space for the main record: NOT a guard refusal (the
             * vol_jxl_retry convention) -- leave unstamped, the file is
             * untouched and a retry re-arms */
            fprintf(stderr, "sweep: EXER create failed (%s)\n", name);
            vol_transcode_abort(v, name);
            goto out;
        }
        vol_delete_inode(v, inode_id, name);
        /* the fresh blob record has no ext; carry the old meta across,
         * exactly like the vol_jxl_retry flow does */
        if (have_keep) {
            invfs_meta_pub chk;
            if (vol_get_meta(v, newino, &chk) != 0)
                vol_apply_meta(v, name, &keep);
        }
        vol_stamp_class(v, newino, INVFS_CLASS_CONTAINER, INVFS_ALGO_EXER,
                        tz_codec_gen(INVFS_ALGO_EXER));
    }
    *nparts_out = (uint32_t)kept;
    rc = 1;
out:
    for (i = 0; i < kept; i++) {
        free(pm[i].blob);
        free(pm[i].back);
    }
    free(pay);
    free(cblob);
    return rc;
}


/* WP14b M2: part count of the exe carve behind the last rc-11 answer */
unsigned vol_exer_last_parts(const invfs_volume *v)
{
    return v ? v->last_exer_parts : 0;
}
