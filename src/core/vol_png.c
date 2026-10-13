/* vol_png.c — PNG repack (JXL lossless + IVPN recipe) + blob-file
 * creation shared by the recipe-based codecs. Split from volume.c. */

#include "volume_internal.h"
#include "vol_delta.h"   /* WP612 retire API */
#include "../codecs/deflate_repro.h"   /* INVFS_DEFLATE_ENGINE_* for a window transform */


/* ---- PNG Repack ---- */

/* WP91: say WHY the lane refused, under INVFS_DEBUG.
 *
 * Every refusal here is a `return 0`, and a `return 0` from this function
 * reaches the caller as one indistinguishable GENERIC_GUARD{PNGR} stamp. The
 * lane therefore has a dozen distinct refusal reasons and one visible
 * symptom, which is how "the decode-back pixel check never ran" (a parse
 * failure) got reported for a long time as "the deflate search found
 * nothing" -- a search that was never reached. The helper's own stderr
 * cannot cover this half: cjxl exiting 0 and djxl exiting 0 is exactly the
 * case where the refusal is ours, not the tool's.
 *
 * INVFS_DEBUG is the existing convention (vol_sweep.c, vol_tier.c, ...), so
 * this adds no new knob and no behaviour change: unset, it is a single
 * getenv() the compiler already had to emit for the rest of the file. */
static void png_refuse(const char *lane, const char *name, const char *why)
{
    if (getenv("INVFS_DEBUG"))
        fprintf(stderr, "[vol] %s %s: refused (%s); original kept RAW\n",
                lane ? lane : "PNGR", name ? name : "?", why);
}

/* zlib inflate wrapper used by pngx (windowBits=-15: a PNG IDAT is a RAW
 * deflate stream -- see png_inflate() for why the framing is negotiated) */
static int png_inflate(const unsigned char *in, size_t in_len,
                       unsigned char **out, size_t *out_len);

/* Build spool.png: the ORIGINAL filtered rows, deflated at level 0
 * (STORED), so cjxl sees the pixels with no second compressor in between.
 *
 * Shared by both halves of vol_create_png_file() on purpose. The two copies
 * this replaces had already drifted once: dfc2b24 fixed the windowBits in
 * the _WIN32 copy and not the POSIX one, which is the whole reason the lane
 * still refused every file afterwards. One copy cannot drift.
 *
 * wbits chooses the IDAT framing (see the cjxl/djxl loop in the POSIX half
 * for why it is a parameter and not a constant). 0 on failure. */
static int png_spool_build(const pngx_info *info, int wbits,
                           uint8_t **out, size_t *out_len)
{
    z_stream s;
    uint8_t *stream, *spool, ihdr[13];
    size_t bound, cap, o = 0, slen;
    int r;

    *out = NULL;
    *out_len = 0;
    if (!info || !info->filtered || !info->filtered_len) return -1;
    memset(&s, 0, sizeof s);
    if (deflateInit2(&s, 0, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return -1;
    bound = deflateBound(&s, (uLong)info->filtered_len);
    stream = (uint8_t *)malloc(bound);
    if (!stream) { deflateEnd(&s); return -1; }
    s.next_in = info->filtered;
    s.avail_in = (uInt)(info->filtered_len > 0x7FFFFFFF ? 0x7FFFFFFF
                                                        : info->filtered_len);
    s.next_out = stream;
    s.avail_out = (uInt)bound;
    r = deflate(&s, Z_FINISH);
    slen = (size_t)s.total_out;
    deflateEnd(&s);
    if (r != Z_STREAM_END) { free(stream); return -1; }

    wr32v(ihdr, info->width);
    wr32v(ihdr + 4, info->height);
    ihdr[8] = info->bitdepth; ihdr[9] = info->colortype; ihdr[10] = 0;
    ihdr[11] = 0; ihdr[12] = info->interlace;
    cap = 8 + 25 + slen + 12;
    spool = (uint8_t *)malloc(cap);
    if (!spool) { free(stream); return -1; }
    memcpy(spool + o, "\x89PNG\r\n\x1a\n", 8); o += 8;
    if (pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IHDR", ihdr, 13) ||
        pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IDAT", stream, slen) ||
        pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IEND", NULL, 0)) {
        free(stream); free(spool); return -1;
    }
    free(stream);
    *out = spool;
    *out_len = o;
    return 0;
}

#ifndef _WIN32
/* read a whole tool-output file into a fresh buffer; 0 on success.
 * (vol_cpack.c's slurp_file is static to that TU; the PNG lane keeps its
 * own.) An empty/unreadable file is a failure: tools say "refused" by not
 * producing output. */
static int png_slurp(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;

    if (!f) return -1;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    *out = (uint8_t *)malloc((size_t)sz);
    if (!*out) { fclose(f); return -1; }
    if (fread(*out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*out); *out = NULL; fclose(f); return -1;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return 0;
}
#endif


/* djxl: JXL blob -> PNG -> parse -> unfilter -> pixels. Returns 0 on ok. */
int invfs_png_from_jxl(invfs_volume *v, uint64_t jxl_inode,
                              uint8_t **rgb, size_t *rgb_len)
{
#ifdef _WIN32
    uint8_t *jxl = NULL; size_t jxl_len = 0;
    if (vol_read_inode(v, jxl_inode, 0, &jxl, &jxl_len) != 0) return -1;
    char jx_tmp[256], dn_tmp[256];
    static const char *djxl = "D:\\VFS\\tools\\jxl\\x64-windows-static\\bin\\djxl.exe";
    _snprintf_s(jx_tmp, sizeof jx_tmp, _TRUNCATE, "%s\\%d_pn.jxl",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(dn_tmp, sizeof dn_tmp, _TRUNCATE, "%s\\%d_pn.png",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    FILE *f = fopen(jx_tmp, "wb");
    if (!f) { free(jxl); return -1; }
    fwrite(jxl, 1, jxl_len, f);
    fclose(f);
    free(jxl);
    extern int run_tool(const char *exe, const char *in, const char *out,
                        const char *opts);
    if (run_tool(djxl, jx_tmp, dn_tmp, "") != 0) {
        remove(jx_tmp); remove(dn_tmp); return -1;
    }
    f = fopen(dn_tmp, "rb");
    uint8_t *dn = NULL; long dn_len = 0;
    if (f) {
        fseek(f, 0, SEEK_END); dn_len = ftell(f); fseek(f, 0, SEEK_SET);
        dn = (uint8_t *)malloc(dn_len ? (size_t)dn_len : 1);
        if (fread(dn, 1, (size_t)dn_len, f) != (size_t)dn_len) { free(dn); dn = NULL; }
        fclose(f);
    }
    remove(jx_tmp); remove(dn_tmp);
    if (!dn) return -1;
    pngx_info di;
    memset(&di, 0, sizeof di);
    int r = pngx_extract(dn, (size_t)dn_len, NULL, 0, png_inflate, &di);
    free(dn);
    if (r != 0) return -1;
    *rgb = di.rgb; *rgb_len = di.rgb_len;
    di.rgb = NULL; di.rgb_len = 0;
    pngx_free(&di);
    return 0;
#else
    /* POSIX: djxl via the WP11 tool layer (vol_cpack.c) -- the blob lands
     * in a fresh /dev/shm scratch dir, the child runs under an RLIMIT_AS
     * ceiling with a 120 s timeout, and "djxl" resolves through
     * $INVFS_TOOLS -> /usr/lib/invfs/tools -> PATH. */
    uint8_t *jxl = NULL, *dn = NULL;
    size_t jxl_len = 0, dn_len = 0;
    char dir[64], in[128], out[128];
    pngx_info di;
    int ok = 0;

    if (vol_read_inode(v, jxl_inode, 0, &jxl, &jxl_len) != 0) return -1;
    /* jxl in, png out: both are the size of the image being re-encoded */
    if (tool_tmpdir(dir, sizeof dir, (uint64_t)jxl_len * 3 + (1u<<20)) != 0) { free(jxl); return -1; }
    snprintf(in, sizeof in, "%s/in.jxl", dir);
    snprintf(out, sizeof out, "%s/out.png", dir);
    if (tool_write(in, jxl, jxl_len) == 0 &&
        run_tool("djxl", in, out, "") == 0 &&
        png_slurp(out, &dn, &dn_len) == 0) {
        memset(&di, 0, sizeof di);
        if (pngx_extract(dn, dn_len, NULL, 0, png_inflate, &di) == 0) {
            *rgb = di.rgb; *rgb_len = di.rgb_len;
            di.rgb = NULL; di.rgb_len = 0;
            pngx_free(&di);
            ok = 1;
        }
    }
    free(jxl);
    free(dn);
    tool_rm(dir, "in.jxl");
    tool_rm(dir, "out.png");
    rmdir(dir);
    return ok ? 0 : -1;
#endif
}


/* miniz tdefl wrapper: zlib-wrapped deflate with level 1..10 */
int mz_tdefl_compress(const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t out_cap, int level,
                             size_t *out_len)
{
    mz_uint flags = tdefl_create_comp_flags_from_zip_params(
        level < 0 ? 0 : (level > 10 ? 10 : level), 15, 0);
    flags |= TDEFL_WRITE_ZLIB_HEADER | TDEFL_COMPUTE_ADLER32;
    mz_uint n = tdefl_compress_mem_to_mem(out, out_cap, in, in_len, flags);
    if (!n) return -1;
    *out_len = n;
    return 0;
}


/* True when `in` starts with a valid zlib (RFC 1950) header, i.e. the stream
 * is zlib-WRAPPED rather than a bare deflate stream. */
static int png_idat_is_zlib_wrapped(const unsigned char *in, size_t in_len)
{
    unsigned cmf, flg;
    if (!in || in_len < 2) return 0;
    cmf = in[0]; flg = in[1];
    return (cmf & 0x0F) == 8 && ((cmf << 8) | flg) % 31 == 0 && !(flg & 0x20);
}

static int png_inflate(const unsigned char *in, size_t in_len,
                       unsigned char **out, size_t *out_len)
{
    z_stream s;
    /* A PNG IDAT is a RAW deflate stream: the PNG spec's compression method 0
     * is DEFLATE with no zlib wrapper, and the recipe rebuilds the file at
     * windowBits -15, so -15 is the framing this lane stores.
     *
     * But the stream we are handed is not always the file's own IDAT: it is
     * also djxl's PNG, read back for the decode-back pixel guard, and a
     * given host's tools do not agree on the framing (libjxl writes a
     * zlib-wrapped IDAT with a 32 KB window, i.e. header 0x78 0x9c, or an
     * 8 KB one, 0x68 0x05). Refusing the wrapped case here took the lane's
     * own round-trip guard away -- the guard was never comparing pixels, and
     * the refusal it produced read as "JXL changed the pixels".
     *
     * So: try the spec framing first, then zlib. This is a parse tolerance
     * only. A wrapped IDAT on the INPUT is still refused, by name, further
     * up -- the recipe has no room to record which framing it must rebuild
     * with, so the stored shape is the raw one only. */
    int wbits[2];
    int nw = 0, i;

    wbits[nw++] = -15;
    if (png_idat_is_zlib_wrapped(in, in_len)) wbits[nw++] = 15;

    for (i = 0; i < nw; i++) {
        int r;
        size_t cap;
        uint8_t *buf;

        memset(&s, 0, sizeof s);
        if (inflateInit2(&s, wbits[i]) != Z_OK) return -1;
        /* The output has to be able to GROW. Sizing the buffer from in_len
         * (the old in_len * 8 + 4096) silently capped how well an IDAT was
         * allowed to compress: djxl's decode of a smooth gradient produced a
         * 31-byte IDAT for 9264 bytes of filtered rows, which is 299:1, and
         * the guess was short by 5 KB -- so the guard read djxl's PNG as
         * unparseable and the lane blamed the pixels. Same bound, same
         * symptom, for any highly compressible PNG. */
        cap = in_len * 8 + 4096;
        buf = (uint8_t *)malloc(cap);
        if (!buf) { inflateEnd(&s); return -1; }
        s.next_in = (Bytef *)in;
        s.avail_in = (uInt)(in_len > 0x7FFFFFFF ? 0x7FFFFFFF : in_len);
        for (;;) {
            s.next_out = buf + s.total_out;
            s.avail_out = (uInt)(cap - s.total_out);
            r = inflate(&s, Z_NO_FLUSH);
            if (r == Z_STREAM_END) {
                *out = buf;
                *out_len = (size_t)s.total_out;
                inflateEnd(&s);
                return 0;
            }
            if (r != Z_OK) break;              /* wrong framing, or corrupt */
            if (s.avail_out != 0) break;       /* input exhausted, no EOB */
            if (cap > (size_t)1 << 31) break;  /* refuse an absurd image */
            {
                size_t ncap = cap * 2;
                uint8_t *nb = (uint8_t *)realloc(buf, ncap);
                if (!nb) { free(buf); inflateEnd(&s); return -1; }
                buf = nb;
                cap = ncap;
            }
        }
        free(buf);
        inflateEnd(&s);
    }
    return -1;
}


/* JXL CLI helper: run cjxl/djxl via temp files (same pattern as APE). */
static int jxl_tool(const char *exe, const char *in, const char *out,
                    const char *opts)
{
    extern int run_tool(const char *exe, const char *in, const char *out,
                        const char *opts);
    return run_tool(exe, in, out, opts);
}

/* WP620: shared PNG-lane front half (both pixel codecs). Parse + the v1
 * guards + the zlib-wrapped refusal. 0 = admitted with info filled, -1 =
 * refused (the reason logged via png_refuse). Decode-working-set admission
 * stays in the sweep_dispatch branch, which owns the MEMLIMIT stamp. */
static int png_parse_admit(const char *lane, const char *name, const uint8_t *png,
                           size_t png_len, pngx_info *info)
{
    if (png_len < 33 || memcmp(png, "\x89PNG\r\n\x1a\n", 8) != 0) {
        png_refuse(lane, name, "no PNG signature"); return -1;
    }
    if (name_too_long_for_children(name)) {
        png_refuse(lane, name, "name too long for the codec sibling"); return -1;
    }
    memset(info, 0, sizeof *info);
    if (pngx_extract(png, png_len, NULL, 0, png_inflate, info) != 0) {
        /* A zlib-wrapped IDAT lands here: it is not a PNG. */
        png_refuse(lane, name, "IDAT is not a raw-deflate PNG stream, or the "
                         "chunks do not describe an image");
        pngx_free(info);   /* a failed extract still owns idat/idat_crc */
        return -1;
    }
    if (info->bitdepth != 8 || info->interlace != 0 || info->bpp == 0) {
        png_refuse(lane, name, "v1 guard: 8-bit non-interlaced only");
        pngx_free(info); return -1;
    }
    if (info->nrows == 0 || info->rgb_len == 0) {
        png_refuse(lane, name, "empty image"); pngx_free(info); return -1;
    }
    /* The recipe rebuilds the IDAT at windowBits -15 (a PNG IDAT is a raw
     * deflate stream) and the recipe has no field in which to record another
     * framing -- its enc byte is the deflate ENGINE, per dfc2b24, not a
     * window. So a zlib-wrapped IDAT is refused here, by name, instead of
     * being stored under a shape the read path could not reproduce. It is
     * not a PNG: no conforming decoder reads it. Some toolschains' PNG
     * writers do emit one, which is why pngx_extract() tolerates it and the
     * refusal lives here rather than in the parser. */
    if (png_idat_is_zlib_wrapped(info->idat, info->idat_len)) {
        png_refuse(lane, name, "IDAT is zlib-wrapped, not a raw deflate stream");
        pngx_free(info); return -1;
    }
    return 0;
}

/* WP620: shared PNG-lane commit half. refilter -> deflate-replica search ->
 * recipe -> full-house rebuild guard -> size guard -> sibling + name commit.
 * blob = the pixel codec's whole-file encoding (JXL or NBLI blob),
 * rt_rgb = the pixels that encoding decoded back to (the lane proved them
 * before calling). lane/sib_tag/algo name the lane ("PNGR"/"NBLI") for
 * diagnostics, the sibling suffix ("!jxl"/"!nbli") and the stamped algo.
 * Caller-owned buffers (info, rt_rgb, blob) are never freed here; the
 * recipe and all intermediates are. Returns the new inode id, 0 on any
 * refusal (the reason logged; the caller stamps). */
static uint64_t png_commit_repack(invfs_volume *v, const char *name,
                                  const uint8_t *png, size_t png_len,
                                  pngx_info *info,
                                  const uint8_t *rt_rgb, size_t rt_rgb_len,
                                  const uint8_t *blob, size_t blob_len,
                                  const char *lane, const char *blob_what,
                                  const char *sib_tag, uint32_t algo)
{
    /* Find the deflate parameters that reproduce the original IDAT
     * byte-for-byte. This has to search ACROSS ENGINES, not just across
     * (level, memLevel): the encoder that wrote the PNG may have been a
     * different zlib implementation than the one this build links. (See the
     * pre-refactor comment: on a zlib-ng host the old level/mem loop could
     * never match a PNG PIL wrote with stock zlib.) invfs_deflate_repro_find()
     * probes the bundled stock zlib and the system zlib and reports which
     * one matched, and `enc` carries that engine id, so the recipe format
     * is unchanged. */
    uint8_t enc = 0, level = 0, mem = 0, strategy = 0;
    int found = 0;
    /* refilter first (the round-trip proved rt_rgb is the pixels) */
    uint8_t *filt = NULL; size_t filt_len = 0;
    uint8_t *recipe = NULL; size_t rlen = 0;
    if (pngx_refilter(rt_rgb, rt_rgb_len, info, &filt, &filt_len) != 0) {
        png_refuse(lane, name, "refilter failed");
        return 0;
    }
    {
        invfs_deflate_params dp;
        memset(&dp, 0, sizeof dp);
        if (invfs_deflate_repro_find(filt, filt_len, info->idat,
                                     info->idat_len, -15, &dp) == 0) {
            enc = dp.engine;
            level = (uint8_t)dp.level;
            mem = (uint8_t)dp.mem_level;
            strategy = (uint8_t)dp.strategy;
            found = 1;
            if (getenv("INVFS_DEBUG"))
                fprintf(stderr, "[vol] %s %s: IDAT reproduced by %s "
                        "level=%d memLevel=%d strategy=%d\n",
                        lane, name ? name : "?",
                        invfs_deflate_engine_name(dp.engine),
                        (int)dp.level, (int)dp.mem_level, (int)dp.strategy);
        }
    }
    free(filt);
    if (!found) {
        png_refuse(lane, name, "no deflate parameters reproduce the IDAT "
                         "(windowBits/level/memLevel/strategy outside the grid)");
        return 0;   /* unknown encoder: keep original */
    }

    /* recipe + pixel-codec blob */
    if (pngx_build_recipe(info, enc, level, mem, strategy,
                          &recipe, &rlen) != 0) {
        png_refuse(lane, name, "recipe build failed");
        return 0;
    }

    /* Full-house guard: replay the READ path end-to-end before committing
     * anything -- recipe parse -> refilter(round-trip pixels) -> deflate
     * replica -> pngx_rebuild -- and demand the original PNG bytes back.
     * (The pixel and IDAT checks above verify the halves; this verifies the
     * serialized recipe round-trips and the parts assemble to the exact
     * file, which is what a reader will do with what we are about to
     * store.) */
    {
        pngx_info pi2;
        uint8_t *f2 = NULL, *s2 = NULL, *rb = NULL;
        size_t f2_len = 0, s2_len = 0, rb_len = 0;
        size_t want_split = 0, i;
        int vok = 0;
        const char *vwhy = "recipe does not parse";

        for (i = 0; i < info->idat_n; i++) want_split += info->idat_split[i];
        memset(&pi2, 0, sizeof pi2);
        if (pngx_parse_recipe(recipe, rlen, &pi2) != 0) {
            vwhy = "recipe does not parse";
        } else if (pngx_refilter(rt_rgb, rt_rgb_len, &pi2, &f2, &f2_len) != 0) {
            vwhy = "refilter of the round-tripped pixels failed";
        } else {
            if (pi2.enc == INVFS_DEFLATE_ENGINE_ZLIB_SYSTEM ||
                pi2.enc == INVFS_DEFLATE_ENGINE_ZLIB_STOCK) {
                /* Rebuild through the engine the sweep recorded, not through
                 * whatever deflateInit2 happens to map to now: the IDAT is
                 * reproduced bit-for-bit only by the same implementation that
                 * wrote it. invfs_deflate_repro_encode() selects it. */
                invfs_deflate_params dp;
                memset(&dp, 0, sizeof dp);
                dp.engine = pi2.enc;
                dp.level = (int8_t)pi2.level;
                dp.mem_level = (int8_t)pi2.mem;
                dp.strategy = (int8_t)pi2.strategy;
                dp.window_bits = -15;   /* PNG IDAT = raw deflate */
                if (invfs_deflate_repro_encode(f2, f2_len, &dp, &s2,
                                               &s2_len) != 0) {
                    vwhy = "deflate replica failed";
                } else if (s2_len != want_split) {
                    vwhy = "deflate replica is not the recorded IDAT length";
                } else {
                    vok = 1;
                }
            } else {
                size_t bound = f2_len + f2_len / 4 + 4096;
                s2 = (uint8_t *)malloc(bound);
                if (s2 &&
                    mz_tdefl_compress(f2, f2_len, s2, bound,
                                      pi2.level, &s2_len) == 0)
                    vok = 1;
                else
                    vwhy = "miniz replica failed";
            }
            if (vok && pngx_rebuild(&pi2, s2, s2_len, &rb, &rb_len) != 0) {
                vok = 0;
                vwhy = "pngx_rebuild rejected the replica";
            } else if (vok && rb_len != png_len) {
                vok = 0;
                vwhy = "rebuilt PNG is the wrong length";
            } else if (vok && memcmp(rb, png, png_len) != 0) {
                vok = 0;
                vwhy = "rebuilt PNG differs from the original";
            }
        }
        free(f2); free(s2); free(rb);
        pngx_free(&pi2);
        if (!vok) {
            png_refuse(lane, name, vwhy);
            free(recipe);
            return 0;   /* the stored shape would not read back: refuse */
        }
    }

    /* guard: pixel-codec blob + recipe must be smaller than the original */
    if (blob_len + rlen >= png_len) {
        char why[128];
        snprintf(why, sizeof why, "%s + recipe is not smaller than the PNG",
                 blob_what);
        png_refuse(lane, name, why);
        free(recipe); return 0;
    }
    /* create name<sib_tag> first, then name (atomic) */
    {
        char sn[320];
        uint64_t sino, ino;
        size_t bound, rbl = 0;
        uint8_t *rc;
        size_t rcl;
        snprintf(sn, sizeof sn, "%s%s", name, sib_tag);
        sino = vol_create_blob_file(v, sn, blob, blob_len,
                                    (uint64_t)blob_len, INVFS_ALGO_NONE);
        if (!sino) { png_refuse(lane, name, "blob write failed");
                     free(recipe);
                     return vol_transcode_abort(v, name); }
        bound = ZSTD_compressBound(rlen);
        rc = (uint8_t *)malloc(bound + 1);
        if (!rc) { png_refuse(lane, name, "out of memory");
                   free(recipe);
                   return vol_transcode_abort(v, name); }
        rcl = ZSTD_compress(rc + 1, bound, recipe, rlen, 19);
        if (!ZSTD_isError(rcl) && rcl < rlen) { rc[0] = 1; rbl = rcl + 1; }
        else { rc[0] = 0; memcpy(rc + 1, recipe, rlen); rbl = rlen + 1; }
        free(recipe);
        ino = vol_create_blob_file(v, name, rc, rbl,
                                   (uint64_t)png_len, algo);
        free(rc);
        if (!ino) return vol_transcode_abort(v, name);
        return ino;
    }
}

/* WP620: decode an NBLI blob through the fat pack (file-level argv, staged
 * as in.nbli/out.png -- the CLI sniffs the direction off the names). 0 on
 * ok with the decoded PNG slurped; -1 when the pack is absent, the tool
 * failed, or the output is missing/empty. Shared by the sweep-side
 * decode-back guard and the read path. */
static int nbli_decode_blob(const invfs_codec *nc, const uint8_t *nbli,
                            size_t nbli_len,
                            uint8_t **png_out, size_t *png_len)
{
    char dir[64], in[128], out[128];
    int rc = -1;
    *png_out = NULL; *png_len = 0;
    if (!nc) return -1;
    if (tool_tmpdir(dir, sizeof dir,
                    (uint64_t)nbli_len * 4 + (4u<<20)) != 0) return -1;
    snprintf(in, sizeof in, "%s/in.nbli", dir);
    snprintf(out, sizeof out, "%s/out.png", dir);
    if (tool_write(in, nbli, nbli_len) == 0 &&
        invfs_codec_pack_exec(nc, 0, in, out) == 0 &&
        png_slurp(out, png_out, png_len) == 0)
        rc = 0;
    tool_rm(dir, "in.nbli"); tool_rm(dir, "out.png");
    rmdir(dir);
    return rc;
}


/* PNG -> JXL lossless blob + IVPN recipe (bit-exact via deflate replica
   and refilter). Verified end-to-end at transcode time: the whole
   rebuild path (djxl -> parse -> refilter -> deflate) is executed and
   the resulting PNG compared byte-for-byte with the original. */
uint64_t vol_create_png_file(invfs_volume *v, const char *name,
                             const uint8_t *png, size_t png_len)
{
#ifdef _WIN32
    if (png_len < 33 || memcmp(png, "\x89PNG\r\n\x1a\n", 8) != 0) return 0;
    if (name_too_long_for_children(name)) return 0;
    pngx_info info;
    memset(&info, 0, sizeof info);
    if (pngx_extract(png, png_len, NULL, 0, png_inflate, &info) != 0) {
        return 0;
    }
    if (info.bitdepth != 8 || info.interlace != 0 || info.bpp == 0) {
        pngx_free(&info); return 0;   /* v1 guard: 8-bit non-interlaced */
    }
    if (info.nrows == 0 || info.rgb_len == 0) { pngx_free(&info); return 0; }

    /* spool.png: original filtered rows with STORED IDAT (level 0) */
    uint8_t *spool = NULL;
    size_t spool_len = 0;
    if (png_spool_build(&info, -15, &spool, &spool_len) != 0) {
        pngx_free(&info); return 0;
    }

    /* cjxl: spool.png -> JXL (lossless, effort 7) */
    char sp_tmp[256], jx_tmp[256], dn_tmp[256];
    static const char *cjxl = "D:\\VFS\\tools\\jxl\\x64-windows-static\\bin\\cjxl.exe";
    static const char *djxl = "D:\\VFS\\tools\\jxl\\x64-windows-static\\bin\\djxl.exe";
    _snprintf_s(sp_tmp, sizeof sp_tmp, _TRUNCATE, "%s\\%d_spool.png",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(jx_tmp, sizeof jx_tmp, _TRUNCATE, "%s\\%d_tmp.jxl",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(dn_tmp, sizeof dn_tmp, _TRUNCATE, "%s\\%d_dn.png",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    FILE *f = fopen(sp_tmp, "wb");
    if (!f) { free(spool); pngx_free(&info); return 0; }
    fwrite(spool, 1, spool_len, f);
    fclose(f);
    free(spool);
    if (jxl_tool(cjxl, sp_tmp, jx_tmp, "-d 0 -e 7") != 0) {
        remove(sp_tmp); remove(jx_tmp); remove(dn_tmp);
        pngx_free(&info); return 0;
    }
    /* djxl: verify the round-trip produces identical pixels */
    if (jxl_tool(djxl, jx_tmp, dn_tmp, "") != 0) {
        remove(sp_tmp); remove(jx_tmp); remove(dn_tmp);
        pngx_free(&info); return 0;
    }
    f = fopen(dn_tmp, "rb");
    uint8_t *dn = NULL; long dn_len = 0;
    if (f) {
        fseek(f, 0, SEEK_END); dn_len = ftell(f); fseek(f, 0, SEEK_SET);
        dn = (uint8_t *)malloc(dn_len ? (size_t)dn_len : 1);
        if (fread(dn, 1, (size_t)dn_len, f) != (size_t)dn_len) { free(dn); dn = NULL; }
        fclose(f);
    }
    int roundtrip_ok = 0;
    if (dn) {
        pngx_info di;
        memset(&di, 0, sizeof di);
        if (pngx_extract(dn, (size_t)dn_len, NULL, 0, png_inflate, &di) == 0 &&
            di.rgb_len == info.rgb_len &&
            memcmp(di.rgb, info.rgb, info.rgb_len) == 0)
            roundtrip_ok = 1;
        pngx_free(&di);
        free(dn);
    }
    remove(sp_tmp); remove(dn_tmp);
    if (!roundtrip_ok) {
        remove(jx_tmp);
        pngx_free(&info); return 0;   /* JXL changed pixels: keep original */
    }

    /* Find the deflate parameters that reproduce the original IDAT
     * byte-for-byte. This has to search ACROSS ENGINES, not just across
     * (level, memLevel): the encoder that wrote the PNG may have been a
     * different zlib implementation than the one this build links. On a
     * zlib-ng host the old level/mem loop could never match a PNG PIL wrote
     * with stock zlib -- all 225 (level, memLevel, strategy) combinations
     * missed -- so every PNG landed in GENERIC_GUARD{PNGR} instead of being
     * transcoded. invfs_deflate_repro_find() already probes the bundled
     * stock zlib and the system zlib and reports which one matched, and
     * `enc` carries that engine id (0 system zlib, 1 miniz, 2 stock zlib),
     * so the recipe format is unchanged.
     *
     * Compat: recipes written before this change used enc=0 to mean "the
     * build's default zlib", which was stock zlib. They now decode as
     * "system zlib". Nothing wrote one while the lane was refusing every
     * file, so the exposure is limited to volumes built with an older
     * binary that DID transcode PNGs. */
    uint8_t enc = 0, level = 0, mem = 0, strategy = 0;
    int found = 0;
    /* refilter first (the JXL round-trip proved info.rgb is the pixels) */
    uint8_t *filt = NULL; size_t filt_len = 0;
    if (pngx_refilter(info.rgb, info.rgb_len, &info, &filt, &filt_len) != 0) {
        remove(jx_tmp); pngx_free(&info); return 0;
    }
    invfs_deflate_params dp;
    memset(&dp, 0, sizeof dp);
    if (invfs_deflate_repro_find(filt, filt_len, info.idat, info.idat_len,
                                 -15, &dp) == 0) {
        enc = dp.engine;
        level = (uint8_t)dp.level;
        mem = (uint8_t)dp.mem_level;
        strategy = (uint8_t)dp.strategy;
        found = 1;
    }
    free(filt);
    if (!found) {
        remove(jx_tmp);
        pngx_free(&info); return 0;   /* unknown encoder: keep original */
    }

    /* recipe + JXL blob */
    uint8_t *recipe = NULL; size_t rlen = 0;
    if (pngx_build_recipe(&info, enc, level, mem, strategy, &recipe, &rlen) != 0) {
        remove(jx_tmp); pngx_free(&info); return 0;
    }
    f = fopen(jx_tmp, "rb");
    uint8_t *jxl = NULL; long jxl_len = 0;
    if (f) {
        fseek(f, 0, SEEK_END); jxl_len = ftell(f); fseek(f, 0, SEEK_SET);
        jxl = (uint8_t *)malloc(jxl_len ? (size_t)jxl_len : 1);
        if (fread(jxl, 1, (size_t)jxl_len, f) != (size_t)jxl_len) { free(jxl); jxl = NULL; }
        fclose(f);
    }
    remove(jx_tmp);
    if (!jxl) { free(recipe); pngx_free(&info); return 0; }

    /* guard: JXL + recipe must be smaller than the original */
    if ((size_t)jxl_len + rlen >= png_len) {
        free(jxl); free(recipe); pngx_free(&info); return 0;
    }
    /* create name!jxl first, then name (atomic) */
    char jn[320];
    snprintf(jn, sizeof jn, "%s!jxl", name);
    uint64_t jino = vol_create_blob_file(v, jn, jxl, (size_t)jxl_len,
                                         (uint64_t)jxl_len, INVFS_ALGO_NONE);
    free(jxl);
    if (!jino) { free(recipe); pngx_free(&info);
                 return vol_transcode_abort(v, name); }
    size_t bound = ZSTD_compressBound(rlen);
    uint8_t *rc = (uint8_t *)malloc(bound + 1);
    if (!rc) { free(recipe); pngx_free(&info);
               return vol_transcode_abort(v, name); }
    size_t rbl = 0;
    size_t rcl = ZSTD_compress(rc + 1, bound, recipe, rlen, 19);
    if (!ZSTD_isError(rcl) && rcl < rlen) { rc[0] = 1; rbl = rcl + 1; }
    else { rc[0] = 0; memcpy(rc + 1, recipe, rlen); rbl = rlen + 1; }
    uint64_t ino = vol_create_blob_file(v, name, rc, rbl,
                                        (uint64_t)png_len, INVFS_ALGO_PNGR);
    free(rc); free(recipe); pngx_free(&info);
    if (!ino) return vol_transcode_abort(v, name);
    return ino;
#else
    /* POSIX twin of the Windows body above, on the WP11 tool layer
     * (vol_cpack.c): cjxl/djxl resolve via $INVFS_TOOLS ->
     * /usr/lib/invfs/tools -> PATH and run in a fresh /dev/shm scratch dir
     * under an RLIMIT_AS ceiling with a 120 s timeout. Same contract: the
     * JXL round-trip must reproduce the pixels and the deflate replica the
     * original IDAT before anything is committed; refused files keep their
     * original RAW bytes. */
    pngx_info info;
    if (png_parse_admit("PNGR", name, png, png_len, &info) != 0) return 0;

    char dir[64], sp[320], jx[320], dn[320];
    /* the spool, the intermediate jxl and the decoded png are all in the
     * same scratch dir at once */
    if (tool_tmpdir(dir, sizeof dir, (uint64_t)png_len * 4 + (4u<<20)) != 0) { pngx_free(&info); return 0; }
    snprintf(sp, sizeof sp, "%s/spool.png", dir);
    snprintf(jx, sizeof jx, "%s/tmp.jxl", dir);
    snprintf(dn, sizeof dn, "%s/dn.png", dir);

    /* cjxl/djxl: prove the pixels survive the round-trip. The SPOOL's IDAT
     * framing is ours to choose -- the recipe's framing is not (see the
     * zlib-wrapped refusal above), and the spool never leaves this loop.
     *
     * The spec framing (-15, a bare deflate stream) is tried first. Only if
     * the decode-back guard rejects it do we retry zlib-wrapped, because a
     * toolchain whose PNG reader expects a wrapped IDAT does not fail on a
     * raw one -- it warns and hands back the wrong pixels, and only the
     * pixel memcmp can tell. That is a property of the host, not of the
     * file, and it must not decide the file's fate. Nothing is weakened
     * for the retry: the same pixel comparison and the same byte-exact
     * rebuild guard below still have to pass. */
    static const int spool_wbits[2] = { -15, 15 };
    uint8_t *rt_rgb = NULL; size_t rt_rgb_len = 0;
    const char *rt_why = "djxl failed (see INVFS_HELPER_VERBOSE)";
    int spi;
    for (spi = 0; spi < 2 && !rt_rgb; spi++) {
        uint8_t *spool = NULL;
        size_t spool_len = 0;
        uint8_t *dnb = NULL; size_t dn_len = 0;

        if (png_spool_build(&info, spool_wbits[spi], &spool, &spool_len) != 0)
            continue;
        if (tool_write(sp, spool, spool_len) != 0) {
            png_refuse("PNGR", name, "cannot write the spool PNG");
            free(spool);
            tool_rm(dir, "tmp.jxl"); rmdir(dir);
            pngx_free(&info); return 0;
        }
        free(spool);
        /* cjxl: spool.png -> JXL (lossless, effort 7) */
        if (jxl_tool("cjxl", sp, jx, "-d 0 -e 7") != 0) {
            rt_why = "cjxl failed on the spool (see INVFS_HELPER_VERBOSE)";
            tool_rm(dir, "spool.png"); tool_rm(dir, "tmp.jxl");
            continue;
        }
        /* djxl: verify the round-trip produces identical pixels. The
         * matched pixels are kept for the read-path replay guard below. */
        rt_why = "djxl failed (see INVFS_HELPER_VERBOSE)";
        if (jxl_tool("djxl", jx, dn, "") != 0) {
            tool_rm(dir, "spool.png"); tool_rm(dir, "tmp.jxl");
            continue;
        }
        if (png_slurp(dn, &dnb, &dn_len) != 0) {
            rt_why = "djxl produced no readable PNG";
            tool_rm(dir, "spool.png"); tool_rm(dir, "tmp.jxl");
            continue;
        }
        {
            pngx_info di;
            memset(&di, 0, sizeof di);
            if (pngx_extract(dnb, dn_len, NULL, 0, png_inflate, &di) != 0) {
                rt_why = "djxl's PNG does not parse";
            } else if (di.rgb_len != info.rgb_len) {
                rt_why = "djxl returned a different pixel count";
            } else if (memcmp(di.rgb, info.rgb, info.rgb_len) != 0) {
                rt_why = "JXL round-trip changed the pixels";
            } else {
                rt_rgb = di.rgb; rt_rgb_len = di.rgb_len;
                di.rgb = NULL; di.rgb_len = 0;
            }
            pngx_free(&di);
        }
        free(dnb);
        tool_rm(dir, "spool.png"); tool_rm(dir, "dn.png");
        /* tmp.jxl is left in place on the winning pass: the blob is slurped
         * after the search, and removing it here would have made the size
         * guard and the commit below read an empty file. */
        if (!rt_rgb) tool_rm(dir, "tmp.jxl");
    }
    if (!rt_rgb) {
        rmdir(dir);
        png_refuse("PNGR", name, rt_why);
        pngx_free(&info); return 0;   /* JXL changed pixels: keep original */
    }

    /* The pixel proof above left tmp.jxl on disk; slurp it now. The
     * deflate search, the full-house guard and the commit below live in
     * the shared png_commit_repack half and no longer touch scratch. */
    {
        uint8_t *jxl = NULL; size_t jxl_len = 0;
        uint64_t ino;
        if (png_slurp(jx, &jxl, &jxl_len) != 0) {
            png_refuse("PNGR", name, "cjxl left no JXL blob behind");
            free(rt_rgb);
            tool_rm(dir, "tmp.jxl"); rmdir(dir);
            pngx_free(&info); return 0;
        }
        tool_rm(dir, "tmp.jxl"); rmdir(dir);
        ino = png_commit_repack(v, name, png, png_len, &info,
                                rt_rgb, rt_rgb_len, jxl, jxl_len,
                                "PNGR", "JXL blob", "!jxl",
                                INVFS_ALGO_PNGR);
        free(rt_rgb); free(jxl); pngx_free(&info);
        return ino;
    }
#endif
}

/* PNG -> NBLI lossless blob + IVPN recipe (bit-exact via decode-back pixel
 * guard, deflate replica and whole-rebuild guard). The PNGR shape with the
 * pixel codec swapped: the spool PNG (canonical minimal re-emit of the
 * original filtered rows) goes through the nbli fat pack's buffer
 * trampolines (staged as in.png/out.nbli by pack_buffer_io), the decoded
 * PNG comes back through the file-level argv (in.nbli/out.png -- exact,
 * no size guess), and everything downstream (deflate search, recipe,
 * full-house guard, commit) is the shared png_commit_repack half.
 * 0 = refused (the file stays RAW; the caller stamps). Pack/tool absence
 * inside is a refusal like any other; the dispatch branch pre-gates the
 * common case into WAITING (RAW, unstamped) instead. */
uint64_t vol_create_nbli_file(invfs_volume *v, const char *name,
                              const uint8_t *png, size_t png_len)
{
#ifdef _WIN32
    (void)v; (void)name; (void)png; (void)png_len;
    return 0;   /* no POSIX pack tool layer on Windows: always refuse */
#else
    const invfs_codec *nc = invfs_codec_by_algo(INVFS_ALGO_NBLI);
    pngx_info info;
    uint8_t *spool = NULL, *enc = NULL, *dnb = NULL;
    size_t spool_len = 0, enc_cap, enc_len = 0, dn_len = 0;
    uint8_t *rt_rgb = NULL; size_t rt_rgb_len = 0;
    uint64_t ino;
    if (!nc || !nc->encode || !nc->decode ||
        (nc->probe && !nc->probe())) {
        png_refuse("NBLI", name, "nbli pack not loaded");
        return 0;
    }
    if (png_parse_admit("NBLI", name, png, png_len, &info) != 0) return 0;
    /* spool: the original filtered rows re-emitted minimal (STORED IDAT).
     * Framing is zlib-WRAPPED here, not the spec raw: NBLI's minimal PNG
     * reader only parses wrapped IDATs (a raw spool fails its decode).
     * The framing is ours to choose and changes nothing downstream -- NBLI
     * decodes to pixels before coding, and the recipe is built from the
     * ORIGINAL rows and IDAT, never from the spool. */
    if (png_spool_build(&info, 15, &spool, &spool_len) != 0) {
        png_refuse("NBLI", name, "spool build failed");
        pngx_free(&info); return 0;
    }
    /* encode through the pack (in.png -> out.nbli staging inside). The
     * blob may exceed the input on pathological input; the size guard in
     * the commit half is what refuses that, but the trampoline needs room
     * to produce it. */
    enc_cap = spool_len + spool_len / 4 + 65536;
    enc = (uint8_t *)malloc(enc_cap ? enc_cap : 1);
    if (!enc || nc->encode(spool, spool_len, enc, enc_cap, &enc_len) != 0 ||
        enc_len == 0) {
        png_refuse("NBLI", name, "nbli encode failed (see INVFS_HELPER_VERBOSE)");
        free(spool); free(enc);
        pngx_free(&info); return 0;
    }
    free(spool);
    /* decode-back pixel guard: the blob must render the exact original
     * pixels. RGBA (alpha dropped with a warning), 16-bit and anything
     * else NBLI cannot represent fails HERE, by pixel count or by memcmp
     * -- never silently. */
    if (nbli_decode_blob(nc, enc, enc_len, &dnb, &dn_len) != 0) {
        png_refuse("NBLI", name, "nbli decode failed (see INVFS_HELPER_VERBOSE)");
        free(enc);
        pngx_free(&info); return 0;
    }
    {
        pngx_info di;
        memset(&di, 0, sizeof di);
        if (pngx_extract(dnb, dn_len, NULL, 0, png_inflate, &di) != 0) {
            png_refuse("NBLI", name, "nbli's PNG does not parse");
        } else if (di.rgb_len != info.rgb_len) {
            png_refuse("NBLI", name, "nbli returned a different pixel count");
        } else if (memcmp(di.rgb, info.rgb, info.rgb_len) != 0) {
            png_refuse("NBLI", name, "NBLI round-trip changed the pixels");
        } else {
            rt_rgb = di.rgb; rt_rgb_len = di.rgb_len;
            di.rgb = NULL; di.rgb_len = 0;
        }
        pngx_free(&di);
    }
    free(dnb);
    if (!rt_rgb) {
        free(enc);
        pngx_free(&info); return 0;   /* pixels differ: keep original */
    }
    ino = png_commit_repack(v, name, png, png_len, &info,
                            rt_rgb, rt_rgb_len, enc, enc_len,
                            "NBLI", "NBLI blob", "!nbli",
                            INVFS_ALGO_NBLI);
    free(enc); free(rt_rgb); pngx_free(&info);
    return ino;
#endif
}

/* NBLI blob -> pixels (pack decode of the "name!nbli" sibling, parsed to
 * RGB). 0 on ok -- mirrors invfs_png_from_jxl's contract (POSIX half).
 * Pack absent or tool failed reads EIO upstream, exactly like a missing
 * djxl does for PNGR. */
int invfs_png_from_nbli(invfs_volume *v, uint64_t nbli_inode,
                        uint8_t **rgb, size_t *rgb_len)
{
#ifdef _WIN32
    (void)v; (void)nbli_inode; (void)rgb; (void)rgb_len;
    return -1;
#else
    const invfs_codec *nc = invfs_codec_by_algo(INVFS_ALGO_NBLI);
    uint8_t *nbli = NULL, *dn = NULL;
    size_t nbli_len = 0, dn_len = 0;
    pngx_info di;
    int ok = 0;
    if (!nc || !nc->encode || !nc->decode) return -1;
    if (vol_read_inode(v, nbli_inode, 0, &nbli, &nbli_len) != 0) return -1;
    if (nbli_decode_blob(nc, nbli, nbli_len, &dn, &dn_len) == 0) {
        memset(&di, 0, sizeof di);
        if (pngx_extract(dn, dn_len, NULL, 0, png_inflate, &di) == 0) {
            *rgb = di.rgb; *rgb_len = di.rgb_len;
            di.rgb = NULL; di.rgb_len = 0;
            pngx_free(&di);
            ok = 1;
        }
    }
    free(nbli);
    free(dn);
    return ok ? 0 : -1;
#endif
}



/* create inode storing one blob (JXL/APE/...) as a single segment */
uint64_t vol_create_blob_file(invfs_volume *v, const char *name,
                                     const uint8_t *blob, size_t blob_len,
                                     uint64_t orig_size, uint32_t algo)
{    uint64_t phys_blocks;
    uint64_t pba;
    uint8_t hdr4[8];
    invfs_ast_block_entry e;


    uint8_t addr[INVFS_RECIPE_ADDR_LEN];
    uint8_t *rblob = NULL;
    size_t rlen = 0;

    if (blob_len == 0 && orig_size != 0) {
        fprintf(stderr, "invarifs: %s: empty blob for %llu-byte "
                "original -- refused\n",
                name, (unsigned long long)orig_size);
        return 0;
    }
    /* Fault injection: fail every blob write from the Nth of this process on.
     *
     * Every transcode child goes through here, so this is the one place that
     * can make a partial transcode happen on demand. Without it the abort
     * paths are only reachable by filling a volume to a precise byte -- the
     * sweep frees the original as it goes, so the exact failure point is not
     * controllable from outside -- and they are precisely the paths where a
     * bug costs the user a file instead of some space.
     *
     * From the Nth *onward*, not the Nth alone: that is what ENOSPC looks
     * like, and it is the only way to exercise the retry paths (the FLAC
     * recipe falls back to storing uncompressed, so failing one write just
     * takes the fallback and the transcode succeeds). Unset in normal runs. */
    {
        const char *fc = getenv("INVFS_FAIL_CHILD");
        if (fc && atoi(fc) > 0) {
            static int nth = 0;
            if (++nth >= atoi(fc)) {
                fprintf(stderr, "[vol] INVFS_FAIL_CHILD: failing blob #%d (%s)\n",
                        nth, name);
                return 0;
            }
        }
    }

    if (blob_len == 0) {
        /* empty file: zero recipe addr; the read path keys off size 0 */
        memset(addr, 0, sizeof addr);
    } else {
        uint32_t bcrc;
        phys_blocks = (blob_len + 8 + INVFS_BLOCK_SIZE - 1) /
                      INVFS_BLOCK_SIZE;
        pba = alloc_blocks(v, v->sb.shadow_zone_start,
                           v->sb.shadow_zone_blocks, phys_blocks, 1,
                           INVFS_ALLOC_DATA);
        if (pba == 0) return 0;
        hdr4[0] = (uint8_t)(blob_len & 0xFF);
        hdr4[1] = (uint8_t)((blob_len >> 8) & 0xFF);
        hdr4[2] = (uint8_t)((blob_len >> 16) & 0xFF);
        hdr4[3] = (uint8_t)((blob_len >> 24) & 0xFF);
        bcrc = invfs_crc32c(blob, blob_len);
        hdr4[4] = (uint8_t)(bcrc & 0xFF);
        hdr4[5] = (uint8_t)((bcrc >> 8) & 0xFF);
        hdr4[6] = (uint8_t)((bcrc >> 16) & 0xFF);
        hdr4[7] = (uint8_t)((bcrc >> 24) & 0xFF);
        if (io_pwrite(&v->io, pba * INVFS_BLOCK_SIZE, hdr4, 8) != 0 ||
            io_pwrite(&v->io, pba * INVFS_BLOCK_SIZE + 8, blob, blob_len) != 0) {
            vol_free_blocks(v, pba, phys_blocks);
            return 0;
        }
        /* WP27: no L2P map -- the entry itself carries the address */
        memset(&e, 0, sizeof e);
        e.file_offset = 0;
        e.length = orig_size;
        e.zone = INVFS_ZONE_BINARY;
        e.algo = algo;
        e.block_id = 0;
        e.pba = pba;
        if (vol_ast_recipe_serialize(orig_size, &e, 1, &rblob,
                                     &rlen) != 0) {
            vol_free_blocks(v, pba, phys_blocks);
            return 0;
        }
        if (vol_recipe_store(v, rblob, rlen, addr) != 0) {
            free(rblob);
            vol_free_blocks(v, pba, phys_blocks);
            return 0;
        }
        free(rblob);
    }
    return vol_create_content_node(v, name, orig_size, addr);
}


/* WP-M23: supersede an existing v3 inode's recipe address with a newly
 * packed blob in Shadow, completely by inode id. Dirents, attributes, and
 * hardlinks are untouched. */
uint64_t vol_publish_blob_inode(invfs_volume *v, uint64_t inode_id,
                                   const uint8_t *blob, size_t blob_len,
                                   uint64_t orig_size, uint32_t algo)
{
    /* pba doubles as vol_free_recipe_blocks' keep_pba ("do not free this
     * one"), and the empty-blob branch below never allocates one -- so it must
     * start at 0, like the explicit 0 the unlink paths pass. Uninitialised,
     * a stack value that happens to equal an old recipe pba silently keeps
     * that block alive forever (a leak), and reading it is UB besides. */
    uint64_t phys_blocks = 0, pba = 0;
    uint8_t hdr4[8];
    invfs_ast_block_entry e;
    invfs_inode in;
    uint8_t addr[INVFS_RECIPE_ADDR_LEN];
    uint8_t old_addr[INVFS_RECIPE_ADDR_LEN];
    uint8_t *rblob = NULL;
    size_t rlen = 0;

    if (!v || !inode_id) return 0;
    if (v->sb.vol_flags & VOLF_READONLY) return 0;
    if (blob_len > 0xFFFFFFFFu || orig_size > MAX_FILE_SIZE) {
        fprintf(stderr, "invarifs: inode %llu: blob %llu / original %llu bytes "
                "exceeds the format limit\n", (unsigned long long)inode_id,
                (unsigned long long)blob_len, (unsigned long long)orig_size);
        return 0;
    }
    if (blob_len == 0 && orig_size != 0) {
        fprintf(stderr, "invarifs: inode %llu: empty blob for %llu-byte original\n",
                (unsigned long long)inode_id, (unsigned long long)orig_size);
        return 0;
    }

    if (vol_inode_get(v, inode_id, &in) != 1)
        return 0;
    memcpy(old_addr, in.recipe_addr, sizeof old_addr);

    if (blob_len == 0) {
        memset(addr, 0, sizeof addr);
    } else {
        uint32_t bcrc;
        phys_blocks = (blob_len + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
        pba = alloc_blocks(v, v->sb.shadow_zone_start,
                           v->sb.shadow_zone_blocks, phys_blocks, 1,
                           INVFS_ALLOC_DATA);
        if (pba == 0) return 0;
        hdr4[0] = (uint8_t)(blob_len & 0xFF);
        hdr4[1] = (uint8_t)((blob_len >> 8) & 0xFF);
        hdr4[2] = (uint8_t)((blob_len >> 16) & 0xFF);
        hdr4[3] = (uint8_t)((blob_len >> 24) & 0xFF);
        bcrc = invfs_crc32c(blob, blob_len);
        hdr4[4] = (uint8_t)(bcrc & 0xFF);
        hdr4[5] = (uint8_t)((bcrc >> 8) & 0xFF);
        hdr4[6] = (uint8_t)((bcrc >> 16) & 0xFF);
        hdr4[7] = (uint8_t)((bcrc >> 24) & 0xFF);
        if (io_pwrite(&v->io, pba * INVFS_BLOCK_SIZE, hdr4, 8) != 0 ||
            io_pwrite(&v->io, pba * INVFS_BLOCK_SIZE + 8, blob, blob_len) != 0) {
            vol_free_blocks(v, pba, phys_blocks);
            return 0;
        }
        memset(&e, 0, sizeof e);
        e.file_offset = 0;
        e.length = orig_size;
        e.zone = INVFS_ZONE_BINARY;
        e.algo = algo;
        e.block_id = 0;
        e.pba = pba;
        if (vol_ast_recipe_serialize(orig_size, &e, 1, &rblob, &rlen) != 0) {
            vol_free_blocks(v, pba, phys_blocks);
            return 0;
        }
        if (vol_recipe_store(v, rblob, rlen, addr) != 0) {
            free(rblob);
            vol_free_blocks(v, pba, phys_blocks);
            return 0;
        }
        free(rblob);
    }

    /* Supersede the recipe in place without touching dirents, type, mode,
     * uid, gid, nlink, or timestamps */
    in.size = orig_size;
    memset(&in.recipe, 0, sizeof in.recipe);
    memcpy(in.recipe_addr, addr, INVFS_RECIPE_ADDR_LEN);
    if (vol_inode_delta_put(v, inode_id, &in) != 0)
        return 0;

    /* WP-N1/WP612: the old RAW/shadow data blocks superseded by the new
     * blob are RETIRED, not freed: the row publish above is staged, not
     * durable, and a fence-failure doom restores the old recipe, which
     * would then name freed blocks (the WP607 s35/s23 class, sweep form).
     * The old keep_pba (the just-written pba) is vacuous here and is not
     * carried: it came from alloc_blocks -- the free pool -- while every
     * pba the retired recipe names is marked allocated, and the allocator
     * never hands out an allocated block, so the retired recipe cannot
     * name it. The drain replays the same share-aware resolver. */
    vol_retire_recipe(v, inode_id, old_addr, 0);

    return inode_id;
}


/* ADR-010 amendment 2: publish a file whose bytes are a window into another
 * inode. The window owns NO data blocks -- the source's own retention keeps
 * them alive -- so this is a metadata-only publication: a one-entry recipe
 * (algo = WINDOW_SRC, pba carrying src_inode) plus a one-entry window table.
 *
 * The source is expected to be a `!`-sibling of the same parent (a QCOW2
 * rankimg), which is what lets the existing sibling cascade drop the source
 * and its windows together: no windows_to refcount counter is needed for the
 * MVP. A window whose source is gone reads EIO (see cpack_map_read's
 * behaviour), never zeros.
 *
 * `length` is the number of bytes THIS file exposes; `src_len` is what is
 * read from the source, so a verbatim window has length == src_len and an
 * inflating one has src_len = the compressed cluster and length = the
 * decompressed size. */
uint64_t vol_publish_window_inode(invfs_volume *v, const char *name,
                                     uint64_t src_inode, uint64_t src_off,
                                     uint64_t length, uint64_t src_len,
                                     uint32_t transform,
                                     uint8_t engine, uint8_t level,
                                     uint8_t mem_level, uint8_t strategy,
                                     int8_t window_bits)
{
    invfs_ast_block_entry e;
    invfs_ast_window_entry w;
    invfs_inode in;
    uint8_t addr[INVFS_RECIPE_ADDR_LEN];
    uint8_t old_addr[INVFS_RECIPE_ADDR_LEN];
    uint8_t *rblob = NULL;
    size_t rlen = 0;
    uint64_t id;

    if (!v || !name || !name[0] || !src_inode || !length || !src_len)
        return 0;
    if (v->sb.vol_flags & VOLF_READONLY)
        return 0;
    if (length > MAX_FILE_SIZE)
        return 0;
    if (transform > 1) {           /* 2/3 are reserved, not implemented */
        fprintf(stderr, "invarifs: window %s: unsupported transform %u\n",
                name, transform);
        return 0;
    }
    if (transform == 1) {
        if (engine != INVFS_DEFLATE_ENGINE_ZLIB_SYSTEM &&
            engine != INVFS_DEFLATE_ENGINE_ZLIB_STOCK) return 0;
        if (!level || level > 9 || !mem_level || mem_level > 9 ||
            strategy > 4) return 0;
    }
    if (src_inode == 0)
        return 0;
    /* the source must exist right now: a window into a missing inode would
     * publish a file that can only ever read EIO */
    if (vol_inode_get(v, src_inode, &in) != 1) {
        fprintf(stderr, "invarifs: window %s: source inode %llu is not live\n",
                name, (unsigned long long)src_inode);
        return 0;
    }

    /* old_addr is the WINDOW inode's superseded recipe -- never the source's.
     * Confusing the two frees the source's data blocks: the window owns none,
     * so the only thing to release is what THIS inode used to own. */
    memset(old_addr, 0, sizeof old_addr);
    id = vol_find(v, name);
    if (id) {
        if (vol_inode_get(v, id, &in) != 1) return 0;
        memcpy(old_addr, in.recipe_addr, sizeof old_addr);
    } else {
        id = vol_create_file(v, name, NULL, 0);
        if (!id) return 0;
    }

    memset(&e, 0, sizeof e);
    e.file_offset = 0;
    e.length = length;
    e.zone = INVFS_ZONE_BINARY;
    e.algo = INVFS_ALGO_WINDOW_SRC;
    e.block_id = 0;                /* index into the window table */
    e.pba = src_inode;             /* the entry's pba slot carries the source */

    memset(&w, 0, sizeof w);
    w.src_off = src_off;
    w.src_len = src_len;
    w.transform = transform;
    w.engine = engine;
    w.level = level;
    w.mem_level = mem_level;
    w.strategy = strategy;
    w.window_bits = window_bits;

    if (vol_ast_recipe_serialize_win(length, &e, 1, &w, 1, &rblob, &rlen) != 0)
        return 0;
    if (vol_recipe_store(v, rblob, rlen, addr) != 0) { free(rblob); return 0; }
    free(rblob);

    if (vol_inode_get(v, id, &in) != 1) return 0;
    in.size = length;
    memset(&in.recipe, 0, sizeof in.recipe);
    memcpy(in.recipe_addr, addr, INVFS_RECIPE_ADDR_LEN);
    if (vol_inode_delta_put(v, id, &in) != 0) return 0;
    /* the window holds no blocks, so there is no keep_pba: whatever the
     * old recipe of this inode used to own is RETIRED (WP612), not freed
     * -- the row publish above is staged, and a doomed row restored to
     * the old recipe must keep its blocks. */
    vol_retire_recipe(v, id, old_addr, 0);
    return id;
}


uint64_t vol_create_jxl_file(invfs_volume *v, const char *name,
                             const uint8_t *jxl, size_t jxl_len,
                             uint64_t orig_size)
{
    return vol_create_blob_file(v, name, jxl, jxl_len, orig_size, INVFS_ALGO_JXL);
}


uint64_t vol_create_ape_file(invfs_volume *v, const char *name,
                             const uint8_t *ape, size_t ape_len,
                             uint64_t orig_size)
{
    return vol_create_blob_file(v, name, ape, ape_len, orig_size, INVFS_ALGO_APE);
}


uint64_t vol_create_pmp_file(invfs_volume *v, const char *name,
                             const uint8_t *pmp, size_t pmp_len,
                             uint64_t orig_size)
{
    return vol_create_blob_file(v, name, pmp, pmp_len, orig_size, INVFS_ALGO_PMP);
}
