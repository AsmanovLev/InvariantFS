/*
 * pngx.c — PNG container extraction/rebuild for InvariantFS (PNG Repack).
 *
 * PNG = signature + chunks. IDAT (one or more consecutive chunks) holds a
 * single RAW DEFLATE stream -- a PNG IDAT is not zlib-wrapped, and the
 * recipe rebuilds it at windowBits -15 (WP91; this line said "zlib stream"
 * and was the belief behind the bug). Plan:
 *   transcode:  inflate IDAT -> filtered rows -> spool.png (stored IDAT)
 *               -> cjxl -d 0 -> name!jxl ; brute-force deflate params
 *               (both zlib engines x levels x memLevel x strategy, miniz)
 *               reproducing the original
 *               IDAT byte-for-byte; recipe = non-IDAT chunks + IDAT chunk
 *               split (sizes! part of the 1:1 file) + per-row filters
 *               (1 byte/row — NOT recoverable from RGB!) + encoder params.
 *   rebuild:    djxl name!jxl -> PNG -> parse -> unfilter -> pixels
 *               -> refilter (saved filters) -> deflate(engine,level,mem,
 *               strategy) -> IDAT
 *               -> original PNG byte-for-byte.
 *
 * Recipe IVPN v2:
 *   [4B "IVPN"][1B ver=2][4B width][4B height][1B bitdepth][1B colortype]
 *   [1B interlace][2B pre_n][pre chunks: (4B type, 4B len, data)]
 *   [2B idat_n][idat split: 4B len each]
 *   [2B post_n][post chunks]
 *   [1B enc: 0=zlib-system 1=miniz 2=zlib-stock][1B level][1B mem][1B strategy]
 *   [4B nrows][nrows bytes of filters]
 *
 * v1 is the same layout without the trailing strategy byte, and v1 could
 * only ever be replayed at Z_DEFAULT_STRATEGY -- that is what the v1 readers
 * (vol_png.c's full-house guard, vol_read.c's PNGR branch) hardcoded. So v1
 * parses as strategy 0, which is exactly what a v1 recipe meant. WP91 added
 * the byte because the cross-engine search (dfc2b24) can legitimately match
 * a stream that needs Z_FILTERED or Z_FIXED, and a recipe that cannot
 * reproduce its own IDAT is a shape the read path would fail on.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "pngx.h"

uint32_t wr32v(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return v; }
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3]; }
static void wr32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t rd16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static void wr16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* append a chunk to a growing buffer */
int pngx_chunk_append(uint8_t **buf, size_t *len, size_t *cap,
                        const uint8_t *type, const uint8_t *data, size_t dlen)
{
    if (*len + 12 + dlen > *cap) {
        size_t nc = *cap ? *cap * 2 : 4096;
        while (nc < *len + 12 + dlen) nc *= 2;
        uint8_t *nb = (uint8_t *)realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb; *cap = nc;
    }
    uint8_t *p = *buf + *len;
    wr32(p, (uint32_t)dlen);
    memcpy(p + 4, type, 4);
    if (dlen) memcpy(p + 8, data, dlen);
    /* CRC32 (IEEE) over type+data */
    extern unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned len);
    unsigned long c = crc32(0L, NULL, 0);
    c = crc32(c, type, 4);
    if (dlen) c = crc32(c, data, (unsigned)dlen);
    p[8 + dlen] = (uint8_t)(c >> 24);
    p[9 + dlen] = (uint8_t)(c >> 16);
    p[10 + dlen] = (uint8_t)(c >> 8);
    p[11 + dlen] = (uint8_t)c;
    *len += 12 + dlen;
    return 0;
}


/* PNG filter application: out[i] = f[i] + pred (mod 256). Used both for
   unfilter (f = filtered, out = pixels) and refilter (f = pixels, out =
   filtered). pred depends on previously computed bytes in cur/prev. */
static void apply_filters(const uint8_t *f, uint8_t bpp, size_t width,
                          const uint8_t *prev, uint8_t *out)
{
    uint8_t filter = f[0];
    const uint8_t *src = f + 1;
    uint8_t *dst = out;
    size_t w = width;
    if (filter == 0) {
        memcpy(dst, src, w);
    } else if (filter == 1) { /* Sub */
        for (size_t i = 0; i < w; i++) {
            uint8_t left = i >= bpp ? dst[i - bpp] : 0;
            dst[i] = (uint8_t)(src[i] + left);
        }
    } else if (filter == 2) { /* Up */
        for (size_t i = 0; i < w; i++)
            dst[i] = (uint8_t)(src[i] + prev[i]);
    } else if (filter == 3) { /* Average */
        for (size_t i = 0; i < w; i++) {
            uint8_t left = i >= bpp ? dst[i - bpp] : 0;
            dst[i] = (uint8_t)(src[i] + ((left + prev[i]) >> 1));
        }
    } else { /* 4 Paeth */
        for (size_t i = 0; i < w; i++) {
            uint8_t a = i >= bpp ? dst[i - bpp] : 0;
            uint8_t b = prev[i];
            uint8_t c = i >= bpp ? prev[i - bpp] : 0;
            int p = (int)a + (int)b - (int)c;
            int pa = p > (int)a ? p - (int)a : (int)a - p;
            int pb = p > (int)b ? p - (int)b : (int)b - p;
            int pc = p > (int)c ? p - (int)c : (int)c - p;
            uint8_t pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            dst[i] = (uint8_t)(src[i] + pred);
        }
    }
}

/* parse PNG: walk chunks, inflate IDAT (zlib, windowBits=15). Returns 0
   on success. *filtered = raw filtered rows (from IDAT); *rgb = pixels
   (after unfilter). Requires external inflate (zlib). */
int pngx_extract(const uint8_t *png, size_t png_len,
                 const unsigned char *idat_extra, size_t idat_extra_len,
                 int (*inflate_fn)(const unsigned char *in, size_t in_len,
                                   unsigned char **out, size_t *out_len),
                 pngx_info *info)
{
    if (png_len < 33 || memcmp(png, "\x89PNG\r\n\x1a\n", 8) != 0) return -1;
    memset(info, 0, sizeof *info);
    size_t pos = 8;
    int have_ihdr = 0;
    size_t idat_cap = 1 << 20, idat_len = 0;
    uint8_t *idat = (uint8_t *)malloc(idat_cap);
    size_t pre_cap = 4096, post_cap = 4096;
    uint8_t *pre = (uint8_t *)malloc(pre_cap), *post = (uint8_t *)malloc(post_cap);
    size_t pre_len = 0, post_len = 0;
    size_t *split = NULL; size_t split_n = 0, split_cap = 64;
    split = (size_t *)malloc(split_cap * sizeof(size_t));
    int in_idat = 0, done = 0;
    size_t first_idat = 0;

    while (pos + 12 <= png_len && !done) {
        uint32_t clen = rd32(png + pos);
        const uint8_t *type = png + pos + 4;
        const uint8_t *data = png + pos + 8;
        if (clen > png_len - pos - 12) break;
        if (memcmp(type, "IHDR", 4) == 0 && clen >= 13) {
            info->width = rd32(data);
            info->height = rd32(data + 4);
            info->bitdepth = data[8];
            info->colortype = data[9];
            info->interlace = data[12];
            have_ihdr = 1;
            if (info->bitdepth != 8 || info->interlace != 0) { /* v1 guard */ }
            int ch = info->colortype == 2 ? 3 : info->colortype == 6 ? 4 :
                     info->colortype == 4 ? 2 : info->colortype == 0 ? 1 :
                     info->colortype == 3 ? 1 : 0;
            info->bpp = (uint8_t)ch;
            info->row_bytes = (size_t)info->width * ch + 1;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (!in_idat) { in_idat = 1; first_idat = pre_len; }
            if (split_n == split_cap) {
                split_cap *= 2;
                split = (size_t *)realloc(split, split_cap * sizeof(size_t));
            }
            split[split_n++] = clen;
            /* keep original chunk CRC (may be broken — part of the file) */
            uint8_t *ncrc = (uint8_t *)realloc(info->idat_crc, split_n * 4);
            if (!ncrc) goto oom;
            info->idat_crc = ncrc;
            memcpy(info->idat_crc + (split_n - 1) * 4, png + pos + 8 + clen, 4);
            if (idat_len + clen > idat_cap) {
                while (idat_cap < idat_len + clen) idat_cap *= 2;
                idat = (uint8_t *)realloc(idat, idat_cap);
            }
            memcpy(idat + idat_len, data, clen);
            idat_len += clen;
        } else if (memcmp(type, "IEND", 4) == 0) {
            done = 1;
            memcpy(info->iend_crc, png + pos + 8 + clen, 4);
        } else {
            if (in_idat) {
                if (pngx_chunk_append(&post, &post_len, &post_cap, type, data, clen)) goto oom;
            } else {
                if (pngx_chunk_append(&pre, &pre_len, &pre_cap, type, data, clen)) goto oom;
            }
        }
        pos += 12 + clen;
    }
    if (!have_ihdr || idat_len == 0) goto fail;
    if (info->bitdepth != 8 || info->interlace != 0 ||
        info->colortype == 3) {
        /* palette or non-8-bit or interlaced: guard at caller */
    }
    /* extra (e.g. trailing data) — ignore */
    (void)idat_extra; (void)idat_extra_len;

    /* inflate */
    /* keep the original IDAT stream for brute-force verification */
    info->idat = (uint8_t *)malloc(idat_len ? idat_len : 1);
    if (!info->idat) goto oom;
    memcpy(info->idat, idat, idat_len);
    info->idat_len = idat_len;
    if (inflate_fn(idat, idat_len, &info->filtered, &info->filtered_len) != 0)
        goto fail;
    free(idat);
    idat = NULL;   /* the fail path below frees idat too (16-bit/interlaced
                      PNGs land here: filtered_len then mismatches) */
    if (info->filtered_len != (size_t)info->row_bytes * info->height)
        goto fail;

    /* unfilter rows -> rgb */
    info->rgb_len = (size_t)info->width * info->bpp * info->height;
    info->rgb = (uint8_t *)malloc(info->rgb_len ? info->rgb_len : 1);
    info->filters = (uint8_t *)malloc(info->height ? info->height : 1);
    if (!info->rgb || !info->filters) goto oom;
    info->nrows = info->height;
    uint8_t *prev = (uint8_t *)calloc(info->row_bytes ? info->row_bytes : 1, 1);
    if (!prev) goto oom;
    size_t row_px0 = (size_t)info->width * info->bpp;
    uint8_t *prev_px = (uint8_t *)calloc(row_px0 ? row_px0 : 1, 1);
    if (!prev_px) goto oom;
    for (size_t r = 0; r < info->height; r++) {
        const uint8_t *row = info->filtered + r * info->row_bytes;
        info->filters[r] = row[0];
        uint8_t *px = info->rgb + r * (size_t)info->width * info->bpp;
        apply_filters(row, info->bpp, (size_t)info->width * info->bpp, prev_px, px);
        memcpy(prev_px, px, (size_t)info->width * info->bpp);
    }
    /* move chunk buffers into info (drop leading position marker) */
    (void)first_idat;
    info->pre = pre; info->pre_len = pre_len;
    info->post = post; info->post_len = post_len;
    info->idat_split = split; info->idat_n = split_n;
    free(prev); free(prev_px);
    return 0;
oom:
    free(pre); free(post); free(split);
    free(info->filtered); info->filtered = NULL;
    return -1;
fail:
    free(pre); free(post); free(split); free(idat);
    free(info->filtered); info->filtered = NULL;
    return -1;
}

/* build IVPN recipe from parsed info + encoder params */
int pngx_build_recipe(const pngx_info *info, uint8_t enc, uint8_t level,
                      uint8_t mem, uint8_t strategy,
                      uint8_t **recipe_out, size_t *rlen_out)
{
    size_t rl = 4 + 1 + 4 + 4 + 1 + 1 + 1 + 2 + info->pre_len +
                2 + info->idat_n * 4 + 2 + info->post_len +
                1 + 1 + 1 + 1 + 4 + info->nrows + info->idat_n * 4 + 4;
    uint8_t *r = (uint8_t *)malloc(rl);
    if (!r) return -1;
    size_t o = 0;
    memcpy(r + o, "IVPN", 4); o += 4;
    r[o++] = 2;
    wr32(r + o, info->width); o += 4;
    wr32(r + o, info->height); o += 4;
    r[o++] = info->bitdepth;
    r[o++] = info->colortype;
    r[o++] = info->interlace;
    wr16(r + o, (uint16_t)(info->pre_len / (12 + 0))); o += 2; /* pre_n (unused, zero) */
    if (info->pre_len) { memcpy(r + o, info->pre, info->pre_len); o += info->pre_len; }
    wr16(r + o, (uint16_t)info->idat_n); o += 2;
    for (size_t i = 0; i < info->idat_n; i++) { wr32(r + o, (uint32_t)info->idat_split[i]); o += 4; }
    wr16(r + o, 0); o += 2; /* post_n (unused, zero) */
    if (info->post_len) { memcpy(r + o, info->post, info->post_len); o += info->post_len; }
    r[o++] = enc;
    r[o++] = level;
    r[o++] = mem;
    r[o++] = strategy;
    wr32(r + o, (uint32_t)info->nrows); o += 4;
    memcpy(r + o, info->filters, info->nrows); o += info->nrows;
    for (size_t i = 0; i < info->idat_n; i++) {
        memcpy(r + o, info->idat_crc + i * 4, 4); o += 4;
    }
    memcpy(r + o, info->iend_crc, 4); o += 4;
    *recipe_out = r;
    *rlen_out = o;
    return 0;
}

/* parse recipe back */
int pngx_parse_recipe(const uint8_t *r, size_t rlen, pngx_info *info)
{
    int ver;
    if (rlen < 33 || memcmp(r, "IVPN", 4) != 0) return -1;
    ver = r[4];
    if (ver != 1 && ver != 2) return -1;
    memset(info, 0, sizeof *info);
    size_t o = 5;
    info->width = rd32(r + o); o += 4;
    info->height = rd32(r + o); o += 4;
    info->bitdepth = r[o++];
    info->colortype = r[o++];
    info->interlace = r[o++];
    /* derive bpp/row_bytes from IHDR fields (8-bit only, v1) */
    info->bpp = (uint8_t)(info->colortype == 2 ? 3 : info->colortype == 6 ? 4 :
                 info->colortype == 4 ? 2 : info->colortype == 0 ? 1 :
                 info->colortype == 3 ? 1 : 0);
    info->row_bytes = (size_t)info->width * info->bpp + 1;
    uint16_t pre_n = rd16(r + o); o += 2;
    (void)pre_n;
    /* pre chunks: need lengths -> walk 12+len */
    size_t pre_end = o;
    size_t pre_cap = 4096; uint8_t *pre = (uint8_t *)malloc(pre_cap);
    size_t pre_len = 0;
    while (pre_end + 8 <= rlen) {
        uint32_t clen = rd32(r + pre_end);
        if (clen > 0x7FFFFFFF) break;
        /* chunk header present? assume pre_n chunks */
        if (pre_len == 0 && pre_n == 0) break; /* degenerate */
        if (pre_len >= pre_n * (12 + 4096)) break;
        break; /* not walking: pre stored as raw blob after a 2B count that
                  we set to zero; the blob is self-describing (12+len per
                  chunk) but we need its size -> store as raw bytes and
                  just copy everything up to the idat_n field */
    }
    free(pre);
    /* The pre/post blobs are self-describing (12+len each). To keep the
       recipe simple, the blob length is implicit: we store them as raw
       byte blobs right after the count fields (counts set to 0). For
       parsing, re-walk chunk headers. */
    /* --- reimplementation: chunk blobs are raw; walk them --- */
    o = 5 + 4 + 4 + 1 + 1 + 1 + 2;
    /* walk pre chunks */
    {
        uint8_t *pre2 = NULL; size_t pre2_len = 0, pre2_cap = 0;
        while (o + 12 <= rlen) {
            uint32_t clen = rd32(r + o);
            if (clen > 0x7FFFFFFF) break;
            if (o + 12 + clen > rlen) break;
            if (pngx_chunk_append(&pre2, &pre2_len, &pre2_cap, r + o + 4, r + o + 8, clen))
                { free(pre2); return -1; }
            o += 12 + clen;
        }
        info->pre = pre2; info->pre_len = pre2_len;
    }
    if (o + 2 > rlen) return -1;
    info->idat_n = rd16(r + o); o += 2;
    info->idat_split = (size_t *)malloc((info->idat_n ? info->idat_n : 1) * sizeof(size_t));
    for (size_t i = 0; i < info->idat_n; i++) {
        if (o + 4 > rlen) return -1;
        info->idat_split[i] = rd32(r + o); o += 4;
    }
    if (o + 2 > rlen) return -1;
    uint16_t post_n = rd16(r + o); o += 2;
    (void)post_n;
    {
        uint8_t *post2 = NULL; size_t post2_len = 0, post2_cap = 0;
        while (o + 12 <= rlen) {
            uint32_t clen = rd32(r + o);
            if (clen > 0x7FFFFFFF) break;
            if (o + 12 + clen > rlen) break;
            if (pngx_chunk_append(&post2, &post2_len, &post2_cap, r + o + 4, r + o + 8, clen))
                { free(post2); return -1; }
            o += 12 + clen;
        }
        info->post = post2; info->post_len = post2_len;
    }
    if (o + 3 > rlen) return -1;
    info->enc = r[o++];
    info->level = r[o++];
    info->mem = r[o++];
    if (ver >= 2) {
        if (o + 1 > rlen) return -1;
        info->strategy = r[o++];
    }
    /* v1 carried no strategy and every v1 reader assumed Z_DEFAULT_STRATEGY */
    if (o + 4 > rlen) return -1;
    info->nrows = rd32(r + o); o += 4;
    if (o + info->nrows > rlen) return -1;
    info->filters = (uint8_t *)malloc(info->nrows ? info->nrows : 1);
    memcpy(info->filters, r + o, info->nrows); o += info->nrows;
    /* original chunk CRCs (idat_n * 4 + IEND 4) */
    info->idat_crc = (uint8_t *)malloc(info->idat_n * 4 + 4);
    if (!info->idat_crc) return -1;
    if (o + info->idat_n * 4 + 4 > rlen) return -1;
    memcpy(info->idat_crc, r + o, info->idat_n * 4); o += info->idat_n * 4;
    memcpy(info->iend_crc, r + o, 4); o += 4;
    return 0;
}

/* refilter pixels -> filtered rows. Forward PNG filtering: filt[i] =
   px[i] - pred(px), pred computed from CURRENT (left) and PREVIOUS row
   pixels — NOT from filtered bytes (that is the inverse, unfilter). */
int pngx_refilter(const uint8_t *rgb, size_t rgb_len, const pngx_info *info,
                  uint8_t **out, size_t *out_len)
{
    size_t row_px = (size_t)info->width * info->bpp;
    if (rgb_len != row_px * info->height) return -1;
    size_t raw_len = info->row_bytes * info->height;
    uint8_t *f = (uint8_t *)malloc(raw_len ? raw_len : 1);
    if (!f) return -1;
    uint8_t *prev = (uint8_t *)calloc(row_px ? row_px : 1, 1);
    if (!prev) { free(f); return -1; }
    for (size_t r = 0; r < info->height; r++) {
        uint8_t *row = f + r * info->row_bytes;
        const uint8_t *px = rgb + r * row_px;
        row[0] = info->filters[r];
        uint8_t filter = info->filters[r];
        uint8_t *dst = row + 1;
        if (filter == 0) {
            memcpy(dst, px, row_px);
        } else if (filter == 1) { /* Sub */
            for (size_t i = 0; i < row_px; i++) {
                uint8_t left = i >= info->bpp ? px[i - info->bpp] : 0;
                dst[i] = (uint8_t)(px[i] - left);
            }
        } else if (filter == 2) { /* Up */
            for (size_t i = 0; i < row_px; i++)
                dst[i] = (uint8_t)(px[i] - prev[i]);
        } else if (filter == 3) { /* Average */
            for (size_t i = 0; i < row_px; i++) {
                uint8_t left = i >= info->bpp ? px[i - info->bpp] : 0;
                dst[i] = (uint8_t)(px[i] - ((left + prev[i]) >> 1));
            }
        } else { /* 4 Paeth */
            for (size_t i = 0; i < row_px; i++) {
                uint8_t a = i >= info->bpp ? px[i - info->bpp] : 0;
                uint8_t b = prev[i];
                uint8_t c = i >= info->bpp ? prev[i - info->bpp] : 0;
                int p = (int)a + (int)b - (int)c;
                int pa = p > (int)a ? p - (int)a : (int)a - p;
                int pb = p > (int)b ? p - (int)b : (int)b - p;
                int pc = p > (int)c ? p - (int)c : (int)c - p;
                uint8_t pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                dst[i] = (uint8_t)(px[i] - pred);
            }
        }
        memcpy(prev, px, row_px);
    }
    free(prev);
    *out = f;
    *out_len = info->row_bytes * info->height;
    return 0;
}

/* assemble the original PNG: pre chunks + IDAT (split) + post + IEND */
int pngx_rebuild(const pngx_info *info, const uint8_t *idat_stream,
                 size_t idat_len, uint8_t **out, size_t *out_len)
{
    /* verify split sums to stream */
    size_t total = 0;
    for (size_t i = 0; i < info->idat_n; i++) total += info->idat_split[i];
    if (total != idat_len) return -1;
    size_t cap = 8 + 25 + info->pre_len + info->post_len + idat_len + info->idat_n * 12 + 12;
    uint8_t *b = (uint8_t *)malloc(cap);
    if (!b) return -1;
    size_t o = 0;
    memcpy(b + o, "\x89PNG\r\n\x1a\n", 8); o += 8;
    /* IHDR */
    uint8_t ihdr[13];
    wr32(ihdr, info->width);
    wr32(ihdr + 4, info->height);
    ihdr[8] = info->bitdepth;
    ihdr[9] = info->colortype;
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = info->interlace;
    if (pngx_chunk_append(&b, &o, &cap, (const uint8_t *)"IHDR", ihdr, 13)) { free(b); return -1; }
    if (info->pre_len) { memcpy(b + o, info->pre, info->pre_len); o += info->pre_len; }
    /* IDAT chunks with split — preserve ORIGINAL CRCs (may be broken) */
    size_t off = 0;
    for (size_t i = 0; i < info->idat_n; i++) {
        uint8_t hdr2[8];
        wr32(hdr2, (uint32_t)info->idat_split[i]);
        memcpy(hdr2 + 4, "IDAT", 4);
        if (o + 12 + info->idat_split[i] > cap) {
            size_t nc = cap * 2;
            while (nc < o + 12 + info->idat_split[i]) nc *= 2;
            uint8_t *nb = (uint8_t *)realloc(b, nc);
            if (!nb) { free(b); return -1; }
            b = nb; cap = nc;
        }
        memcpy(b + o, hdr2, 8); o += 8;
        memcpy(b + o, idat_stream + off, info->idat_split[i]); o += info->idat_split[i];
        memcpy(b + o, info->idat_crc + i * 4, 4); o += 4;
        off += info->idat_split[i];
    }
    if (info->post_len) { memcpy(b + o, info->post, info->post_len); o += info->post_len; }
    {
        uint8_t hdr2[12];
        wr32(hdr2, 0);
        memcpy(hdr2 + 4, "IEND", 4);
        memcpy(hdr2 + 8, info->iend_crc, 4);
        if (o + 12 > cap) {
            size_t nc = cap * 2;
            uint8_t *nb = (uint8_t *)realloc(b, nc);
            if (!nb) { free(b); return -1; }
            b = nb; cap = nc;
        }
        memcpy(b + o, hdr2, 12); o += 12;
    }
    *out = b;
    *out_len = o;
    return 0;
}

void pngx_free(pngx_info *info)
{
    if (!info) return;
    free(info->pre); free(info->post);
    free(info->idat_split); free(info->filters); free(info->idat_crc);
    free(info->filtered); free(info->rgb); free(info->idat);
    memset(info, 0, sizeof *info);
}
