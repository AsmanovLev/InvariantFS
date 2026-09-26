/* vol_png.c — PNG repack (JXL lossless + IVPN recipe) + blob-file
 * creation shared by the recipe-based codecs. Split from volume.c. */

#include "volume_internal.h"
#include "../codecs/deflate_repro.h"   /* INVFS_DEFLATE_ENGINE_* for a window transform */


/* ---- PNG Repack ---- */

/* zlib inflate wrapper used by pngx (windowBits=15: zlib wrapper) */
static int png_inflate(const unsigned char *in, size_t in_len,
                       unsigned char **out, size_t *out_len);

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
    if (tool_tmpdir(dir, sizeof dir) != 0) { free(jxl); return -1; }
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


static int png_inflate(const unsigned char *in, size_t in_len,
                       unsigned char **out, size_t *out_len)
{
    z_stream s;
    memset(&s, 0, sizeof s);
    inflateInit2(&s, 15);
    size_t cap = in_len * 8 + 4096;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) { inflateEnd(&s); return -1; }
    s.next_in = (Bytef *)in;
    s.avail_in = (uInt)(in_len > 0x7FFFFFFF ? 0x7FFFFFFF : in_len);
    s.next_out = buf;
    s.avail_out = (uInt)(cap > 0x7FFFFFFF ? 0x7FFFFFFF : cap);
    int r = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    if (r != Z_STREAM_END) { free(buf); return -1; }
    *out = buf;
    *out_len = (size_t)s.total_out;
    return 0;
}


/* JXL CLI helper: run cjxl/djxl via temp files (same pattern as APE). */
static int jxl_tool(const char *exe, const char *in, const char *out,
                    const char *opts)
{
    extern int run_tool(const char *exe, const char *in, const char *out,
                        const char *opts);
    return run_tool(exe, in, out, opts);
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
    {
        z_stream s;
        memset(&s, 0, sizeof s);
        /* -15: a PNG IDAT is a RAW deflate stream, not a zlib-wrapped one.
         * With windowBits 15 the spool carried a 2-byte header + adler32
         * that no PNG reader accepts. */
        if (deflateInit2(&s, 0, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            pngx_free(&info); return 0;
        }
        size_t bound = deflateBound(&s, (uLong)info.filtered_len);
        uint8_t *stream = (uint8_t *)malloc(bound);
        s.next_in = info.filtered;
        s.avail_in = (uInt)(info.filtered_len > 0x7FFFFFFF ? 0x7FFFFFFF : info.filtered_len);
        s.next_out = stream;
        s.avail_out = (uInt)bound;
        int r = deflate(&s, Z_FINISH);
        size_t slen = (size_t)s.total_out;
        deflateEnd(&s);
        if (r != Z_STREAM_END) { free(stream); pngx_free(&info); return 0; }
        /* build spool png: sig + IHDR + IDAT(stored) + IEND */
        uint8_t ihdr[13];
        wr32v(ihdr, info.width);
        wr32v(ihdr + 4, info.height);
        ihdr[8] = info.bitdepth; ihdr[9] = info.colortype; ihdr[10] = 0;
        ihdr[11] = 0; ihdr[12] = info.interlace;
        size_t cap = 8 + 25 + slen + 12;
        spool = (uint8_t *)malloc(cap);
        if (!spool) { free(stream); pngx_free(&info); return 0; }
        size_t o = 0;
        memcpy(spool + o, "\x89PNG\r\n\x1a\n", 8); o += 8;
        if (pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IHDR", ihdr, 13) ||
            pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IDAT", stream, slen) ||
            pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IEND", NULL, 0)) {
            free(stream); free(spool); pngx_free(&info); return 0;
        }
        spool_len = o;
        free(stream);
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
    invfs_deflate_params dp;
    memset(&dp, 0, sizeof dp);
    if (invfs_deflate_repro_find(filt, filt_len, info.idat, info.idat_len,
                                 -15, &dp) == 0) {
        enc = dp.engine;
        level = (uint8_t)dp.level;
        mem = (uint8_t)dp.mem_level;
        found = 1;
    }
    free(filt);
    if (!found) {
        remove(jx_tmp);
        pngx_free(&info); return 0;   /* unknown encoder: keep original */
    }

    /* recipe + JXL blob */
    uint8_t *recipe = NULL; size_t rlen = 0;
    if (pngx_build_recipe(&info, enc, level, mem, &recipe, &rlen) != 0) {
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
    if (png_len < 33 || memcmp(png, "\x89PNG\r\n\x1a\n", 8) != 0) return 0;
    if (name_too_long_for_children(name)) return 0;
    pngx_info info;
    memset(&info, 0, sizeof info);
    if (pngx_extract(png, png_len, NULL, 0, png_inflate, &info) != 0) {
        pngx_free(&info);   /* a failed extract still owns idat/idat_crc */
        return 0;
    }
    if (info.bitdepth != 8 || info.interlace != 0 || info.bpp == 0) {
        pngx_free(&info); return 0;   /* v1 guard: 8-bit non-interlaced */
    }
    if (info.nrows == 0 || info.rgb_len == 0) { pngx_free(&info); return 0; }

    /* spool.png: original filtered rows with STORED IDAT (level 0) */
    uint8_t *spool = NULL;
    size_t spool_len = 0;
    {
        z_stream s;
        memset(&s, 0, sizeof s);
        if (deflateInit2(&s, 0, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            pngx_free(&info); return 0;
        }
        size_t bound = deflateBound(&s, (uLong)info.filtered_len);
        uint8_t *stream = (uint8_t *)malloc(bound);
        if (!stream) { deflateEnd(&s); pngx_free(&info); return 0; }
        s.next_in = info.filtered;
        s.avail_in = (uInt)(info.filtered_len > 0x7FFFFFFF ? 0x7FFFFFFF : info.filtered_len);
        s.next_out = stream;
        s.avail_out = (uInt)bound;
        int r = deflate(&s, Z_FINISH);
        size_t slen = (size_t)s.total_out;
        deflateEnd(&s);
        if (r != Z_STREAM_END) { free(stream); pngx_free(&info); return 0; }
        /* build spool png: sig + IHDR + IDAT(stored) + IEND */
        uint8_t ihdr[13];
        wr32v(ihdr, info.width);
        wr32v(ihdr + 4, info.height);
        ihdr[8] = info.bitdepth; ihdr[9] = info.colortype; ihdr[10] = 0;
        ihdr[11] = 0; ihdr[12] = info.interlace;
        size_t cap = 8 + 25 + slen + 12;
        spool = (uint8_t *)malloc(cap);
        if (!spool) { free(stream); pngx_free(&info); return 0; }
        size_t o = 0;
        memcpy(spool + o, "\x89PNG\r\n\x1a\n", 8); o += 8;
        if (pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IHDR", ihdr, 13) ||
            pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IDAT", stream, slen) ||
            pngx_chunk_append(&spool, &o, &cap, (const uint8_t *)"IEND", NULL, 0)) {
            free(stream); free(spool); pngx_free(&info); return 0;
        }
        spool_len = o;
        free(stream);
    }

    char dir[64], sp[320], jx[320], dn[320];
    if (tool_tmpdir(dir, sizeof dir) != 0) { free(spool); pngx_free(&info); return 0; }
    snprintf(sp, sizeof sp, "%s/spool.png", dir);
    snprintf(jx, sizeof jx, "%s/tmp.jxl", dir);
    snprintf(dn, sizeof dn, "%s/dn.png", dir);
    if (tool_write(sp, spool, spool_len) != 0) {
        free(spool); pngx_free(&info);
        rmdir(dir);
        return 0;
    }
    free(spool);
    /* cjxl: spool.png -> JXL (lossless, effort 7) */
    if (jxl_tool("cjxl", sp, jx, "-d 0 -e 7") != 0) {
        tool_rm(dir, "spool.png"); tool_rm(dir, "tmp.jxl"); rmdir(dir);
        pngx_free(&info); return 0;
    }
    /* djxl: verify the round-trip produces identical pixels. The matched
     * pixels are kept for the read-path replay guard below. */
    uint8_t *rt_rgb = NULL; size_t rt_rgb_len = 0;
    {
        uint8_t *dnb = NULL; size_t dn_len = 0;
        if (jxl_tool("djxl", jx, dn, "") == 0 &&
            png_slurp(dn, &dnb, &dn_len) == 0) {
            pngx_info di;
            memset(&di, 0, sizeof di);
            if (pngx_extract(dnb, dn_len, NULL, 0, png_inflate, &di) == 0 &&
                di.rgb_len == info.rgb_len &&
                memcmp(di.rgb, info.rgb, info.rgb_len) == 0) {
                rt_rgb = di.rgb; rt_rgb_len = di.rgb_len;
                di.rgb = NULL; di.rgb_len = 0;
            }
            pngx_free(&di);
        }
        free(dnb);
    }
    tool_rm(dir, "spool.png"); tool_rm(dir, "dn.png");
    if (!rt_rgb) {
        tool_rm(dir, "tmp.jxl"); rmdir(dir);
        pngx_free(&info); return 0;   /* JXL changed pixels: keep original */
    }

    /* brute-force deflate params reproducing the original IDAT */
    uint8_t enc = 0, level = 0, mem = 0;
    int found = 0;
    /* refilter first (the JXL round-trip proved info.rgb is the pixels) */
    uint8_t *filt = NULL; size_t filt_len = 0;
    if (pngx_refilter(info.rgb, info.rgb_len, &info, &filt, &filt_len) != 0) {
        free(rt_rgb); tool_rm(dir, "tmp.jxl"); rmdir(dir);
        pngx_free(&info); return 0;
    }
    static const int prio[][2] = {
        {6,8},{7,9},{6,9},{9,8},{7,8},{9,9},{6,7},{8,9},{8,8},{5,8},
        {4,8},{3,8},{2,8},{1,8},{7,7},{8,7},{9,7},{1,9},{2,9},{3,9},{4,9},{5,9}
    };
    for (size_t i = 0; i < sizeof(prio) / sizeof(prio[0]) && !found; i++) {
        z_stream s;
        memset(&s, 0, sizeof s);
        if (deflateInit2(&s, prio[i][0], Z_DEFLATED, 15, prio[i][1],
                         Z_DEFAULT_STRATEGY) != Z_OK) continue;
        size_t bound = deflateBound(&s, (uLong)filt_len);
        uint8_t *re = (uint8_t *)malloc(bound);
        if (!re) { deflateEnd(&s); continue; }
        s.next_in = filt;
        s.avail_in = (uInt)(filt_len > 0x7FFFFFFF ? 0x7FFFFFFF : filt_len);
        s.next_out = re;
        s.avail_out = (uInt)bound;
        int r2 = deflate(&s, Z_FINISH);
        size_t re_len = (size_t)s.total_out;
        deflateEnd(&s);
        if (r2 == Z_STREAM_END && re_len == info.idat_len &&
            memcmp(re, info.idat, info.idat_len) == 0) {
            enc = 0; level = (uint8_t)prio[i][0]; mem = (uint8_t)prio[i][1];
            found = 1;
        }
        free(re);
    }
    if (!found) {
        /* miniz tdefl levels 1..10 */
        for (int lv = 1; lv <= 10 && !found; lv++) {
            size_t olen = 0;
            size_t bound = filt_len + filt_len / 4 + 4096;
            uint8_t *re = (uint8_t *)malloc(bound);
            if (re &&
                mz_tdefl_compress(filt, filt_len, re, bound, lv, &olen) == 0 &&
                olen == info.idat_len && memcmp(re, info.idat, info.idat_len) == 0) {
                enc = 1; level = (uint8_t)lv; mem = 0;
                found = 1;
            }
            free(re);
        }
    }
    free(filt);
    if (!found) {
        free(rt_rgb); tool_rm(dir, "tmp.jxl"); rmdir(dir);
        pngx_free(&info); return 0;   /* unknown encoder: keep original */
    }

    /* recipe + JXL blob */
    uint8_t *recipe = NULL; size_t rlen = 0;
    if (pngx_build_recipe(&info, enc, level, mem, &recipe, &rlen) != 0) {
        free(rt_rgb); tool_rm(dir, "tmp.jxl"); rmdir(dir);
        pngx_free(&info); return 0;
    }
    uint8_t *jxl = NULL; size_t jxl_len = 0;
    if (png_slurp(jx, &jxl, &jxl_len) != 0) {
        free(rt_rgb); free(recipe);
        tool_rm(dir, "tmp.jxl"); rmdir(dir);
        pngx_free(&info); return 0;
    }
    tool_rm(dir, "tmp.jxl"); rmdir(dir);

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
        int vok = 0;

        memset(&pi2, 0, sizeof pi2);
        if (pngx_parse_recipe(recipe, rlen, &pi2) == 0 &&
            pngx_refilter(rt_rgb, rt_rgb_len, &pi2, &f2, &f2_len) == 0) {
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
                dp.strategy = 0;
                dp.window_bits = -15;   /* PNG IDAT = raw deflate */
                if (invfs_deflate_repro_encode(f2, f2_len, &dp, &s2,
                                               &s2_len) == 0)
                    vok = 1;
            } else {
                size_t bound = f2_len + f2_len / 4 + 4096;
                s2 = (uint8_t *)malloc(bound);
                if (s2 &&
                    mz_tdefl_compress(f2, f2_len, s2, bound,
                                      pi2.level, &s2_len) == 0)
                    vok = 1;
            }
            if (vok) {
                vok = (pngx_rebuild(&pi2, s2, s2_len, &rb, &rb_len) == 0 &&
                       rb_len == png_len && memcmp(rb, png, png_len) == 0);
            }
        }
        free(f2); free(s2); free(rb);
        pngx_free(&pi2);
        free(rt_rgb);
        if (!vok) {
            free(jxl); free(recipe); pngx_free(&info);
            return 0;   /* the stored shape would not read back: refuse */
        }
    }

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
#endif
}



/* create inode storing one blob (JXL/APE/...) as a single segment */
uint64_t vol_create_blob_file(invfs_volume *v, const char *name,
                                     const uint8_t *blob, size_t blob_len,
                                     uint64_t orig_size, uint32_t algo)
{
    uint64_t inode_id;
    uint64_t phys_blocks;
    uint64_t pba;
    uint8_t hdr4[8];
    size_t rec_size;
    uint8_t *rec;
    invfs_inode_rec *rh;
    uint8_t ast_h[INVFS_AST_HDR_V2_LEN];
    size_t ast_hlen;
    invfs_ast_block_entry e;
    uint32_t crc;

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

    /* blob_len goes into a 4-byte on-disk segment header, so the blob stays
       u32-capped; orig_size rides the v2 recipe header past 4 GB (WP22a) --
       only MAX_FILE_SIZE is refused, never silently folded */
    if (blob_len > 0xFFFFFFFFu || orig_size > MAX_FILE_SIZE) {
        fprintf(stderr, "invarifs: %s: blob %llu / original %llu bytes "
                "exceeds the format limit (blob 4 GB / file 1 TB)\n", name,
                (unsigned long long)blob_len, (unsigned long long)orig_size);
        return 0;
    }
    if (name_too_long(name)) return 0;

    /* WP-M21b: v3 volumes publish blobs as a content-addressed recipe
     * blob + inode row (same segment layout, same AST entries, same
     * reader -- only the metadata publication differs). The v2 record
     * append below is refused on v3 (vol_records.c guards), and nothing
     * would read the record stream anyway. An existing name's row is
     * superseded in place (create_content_node reuses the id, delta-first
     * resolution), so v3 callers must NOT vol_delete_inode() the old id
     * afterwards -- that would destroy the fresh blob. The dropped
     * recipe/segments become unreachable and are reclaimed by the WP-M15
     * reachability reclaim after a fold. */
    if (v->sb.vol_flags & VOLF_V3) {
        uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN];
        uint8_t *rblob = NULL;
        size_t rlen = 0;

        if (blob_len == 0 && orig_size != 0) {
            fprintf(stderr, "invarifs: %s: empty blob for %llu-byte "
                    "original -- refused\n",
                    name, (unsigned long long)orig_size);
            return 0;
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
            if (vol_v3_recipe_store(v, rblob, rlen, addr) != 0) {
                free(rblob);
                vol_free_blocks(v, pba, phys_blocks);
                return 0;
            }
            free(rblob);
        }
        return vol_v3_create_content_node(v, name, orig_size, addr);
    }

    /* Empty member (e.g. a zero-length TAR part): the generic guard stores
     * such a blob verbatim as csize=0, and the read path (seg_read_checked,
     * min_csize=1) would then reject our own segment forever. An empty
     * original needs no segment at all — write the same num_blocks=0 record
     * shape vol_create_file uses for empty files. blob_len==0 with a
     * nonzero orig_size is an upstream bug: refuse rather than store
     * something no decoder could satisfy. */
    if (blob_len == 0) {
        if (orig_size != 0) {
            fprintf(stderr, "invarifs: %s: empty blob for %llu-byte original\n",
                    name, (unsigned long long)orig_size);
            return 0;
        }
        inode_id = v->next_inode_id++;
        rec_size = INVFS_REC_HDR_LEN + strlen(name) + 1 + INVFS_AST_HDR_V1_LEN;
        rec = (uint8_t *)calloc(1, rec_size);
        if (!rec) return 0;
        rh = (invfs_inode_rec *)rec;
        rec_set_name(rh, name);
        /* empty file, v1 header by definition (nothing overflows it) */
        if (invfs_ast_hdr_write(invfs_rec_body(rh), 0, 0, 0) == 0) {
            free(rec);
            return 0;
        }
        rh->magic = INODE_REC_MAGIC;
        rh->rec_len = (uint32_t)rec_size;
        rh->inode_id = inode_id;
        rh->file_size = 0;
        rh->ctime = (uint64_t)time(NULL);
        crc = invfs_crc32c(rec, rec_size);
        if (inode_area_make_room(v, (uint64_t)rec_size + 4) != 0) { free(rec); return 0; }
        if (vol_pre_record(v) != 0) { free(rec); return 0; }
        /* Bug J: route the append through the mapper */
        {
            uint64_t npos;
            int rc2 = vol_append_slot(v, (uint64_t)rec_size + 4, &npos);
            if (rc2 != 0) { free(rec); return 0; }
            if (io_seek(&v->io, npos) != 0 ||
                io_write(&v->io, rec, rec_size) != 0 ||
                io_write(&v->io, &crc, 4) != 0) { free(rec); return 0; }
            idx_put(v, name, strlen(name), inode_id, npos,
                    rh->file_size, rh->ctime);
            idx_put_id(v, inode_id, npos);
        }
        free(rec);
        return inode_id;
    }

    inode_id = v->next_inode_id++;
    phys_blocks = (blob_len + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                       phys_blocks, 1, INVFS_ALLOC_DATA);

    if (pba == 0) return 0;
    hdr4[0] = (uint8_t)(blob_len & 0xFF);
    hdr4[1] = (uint8_t)((blob_len >> 8) & 0xFF);
    hdr4[2] = (uint8_t)((blob_len >> 16) & 0xFF);
    hdr4[3] = (uint8_t)((blob_len >> 24) & 0xFF);
    {
        uint32_t bcrc = invfs_crc32c(blob, blob_len);
        hdr4[4] = (uint8_t)(bcrc & 0xFF);
        hdr4[5] = (uint8_t)((bcrc >> 8) & 0xFF);
        hdr4[6] = (uint8_t)((bcrc >> 16) & 0xFF);
        hdr4[7] = (uint8_t)((bcrc >> 24) & 0xFF);
    }

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

    /* v2 recipe header only when orig_size overflows v1's u32 (WP22a) */
    ast_hlen = invfs_ast_hdr_write(ast_h, orig_size, 1, 0);
    if (!ast_hlen) return 0;
    rec_size = INVFS_REC_HDR_LEN + strlen(name) + 1 + ast_hlen + sizeof(e);
    rec = (uint8_t *)calloc(1, rec_size);
    if (!rec) return 0;
    rh = (invfs_inode_rec *)rec;
    rh->magic = INODE_REC_MAGIC;
    rh->rec_len = (uint32_t)rec_size;
    rh->inode_id = inode_id;
    rh->file_size = orig_size;
    rh->ctime = (uint64_t)time(NULL);
    rec_set_name(rh, name);
    memcpy(invfs_rec_body(rh), ast_h, ast_hlen);
    memcpy(invfs_rec_body(rh) + ast_hlen, &e, sizeof e);
    crc = invfs_crc32c(rec, rec_size);

    if (inode_area_make_room(v, (uint64_t)rec_size + 4) != 0) { free(rec); return 0; }
    if (vol_pre_record(v) != 0) { free(rec); return 0; }
    /* Bug J: route the append through the mapper */
    {
        uint64_t npos;
        int rc2 = vol_append_slot(v, (uint64_t)rec_size + 4, &npos);
        if (rc2 != 0) { free(rec); return 0; }
        if (io_seek(&v->io, npos) != 0 ||
            io_write(&v->io, rec, rec_size) != 0 ||
            io_write(&v->io, &crc, 4) != 0) { free(rec); return 0; }
        idx_put(v, name, strlen(name), inode_id, npos,
                rh->file_size, rh->ctime);
        idx_put_id(v, inode_id, npos);
    }
    pba_ref_apply(v, rec, (uint32_t)rec_size, +1);
    free(rec);
    return inode_id;
}


/* WP-M23: supersede an existing v3 inode's recipe address with a newly
 * packed blob in Shadow, completely by inode id. Dirents, attributes, and
 * hardlinks are untouched. */
uint64_t vol_v3_publish_blob_inode(invfs_volume *v, uint64_t inode_id,
                                   const uint8_t *blob, size_t blob_len,
                                   uint64_t orig_size, uint32_t algo)
{
    /* pba doubles as vol_v3_free_recipe_blocks' keep_pba ("do not free this
     * one"), and the empty-blob branch below never allocates one -- so it must
     * start at 0, like the explicit 0 the unlink paths pass. Uninitialised,
     * a stack value that happens to equal an old recipe pba silently keeps
     * that block alive forever (a leak), and reading it is UB besides. */
    uint64_t phys_blocks = 0, pba = 0;
    uint8_t hdr4[8];
    invfs_ast_block_entry e;
    invfs_v3_inode in;
    uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN];
    uint8_t old_addr[INVFS_V3_RECIPE_ADDR_LEN];
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

    if (vol_v3_inode_get(v, inode_id, &in) != 1)
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
        if (vol_v3_recipe_store(v, rblob, rlen, addr) != 0) {
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
    memcpy(in.recipe_addr, addr, INVFS_V3_RECIPE_ADDR_LEN);
    if (vol_v3_inode_delta_put(v, inode_id, &in) != 0)
        return 0;

    /* WP-N1: targeted free of old RAW/shadow data blocks superseded by the new blob */
    vol_v3_free_recipe_blocks(v, old_addr, pba);

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
uint64_t vol_v3_publish_window_inode(invfs_volume *v, const char *name,
                                     uint64_t src_inode, uint64_t src_off,
                                     uint64_t length, uint64_t src_len,
                                     uint32_t transform,
                                     uint8_t engine, uint8_t level,
                                     uint8_t mem_level, uint8_t strategy,
                                     int8_t window_bits)
{
    invfs_ast_block_entry e;
    invfs_ast_window_entry w;
    invfs_v3_inode in;
    uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN];
    uint8_t old_addr[INVFS_V3_RECIPE_ADDR_LEN];
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
    if (vol_v3_inode_get(v, src_inode, &in) != 1) {
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
        if (vol_v3_inode_get(v, id, &in) != 1) return 0;
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
    if (vol_v3_recipe_store(v, rblob, rlen, addr) != 0) { free(rblob); return 0; }
    free(rblob);

    if (vol_v3_inode_get(v, id, &in) != 1) return 0;
    in.size = length;
    memset(&in.recipe, 0, sizeof in.recipe);
    memcpy(in.recipe_addr, addr, INVFS_V3_RECIPE_ADDR_LEN);
    if (vol_v3_inode_delta_put(v, id, &in) != 0) return 0;
    /* the window holds no blocks, so keep_pba is 0: free whatever the old
     * recipe of this inode used to own */
    vol_v3_free_recipe_blocks(v, old_addr, 0);
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
