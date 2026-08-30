/* vol_png.c — PNG repack (JXL lossless + IVPN recipe) + blob-file
 * creation shared by the recipe-based codecs. Split from volume.c. */

#include "volume_internal.h"


/* ---- PNG Repack ---- */

/* zlib inflate wrapper used by pngx (windowBits=15: zlib wrapper) */
static int png_inflate(const unsigned char *in, size_t in_len,
                       unsigned char **out, size_t *out_len);


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
    (void)v; (void)jxl_inode; (void)rgb; (void)rgb_len;
    return -1;
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
        if (deflateInit2(&s, 0, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
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

    /* brute-force deflate params reproducing the original IDAT */
    uint8_t enc = 0, level = 0, mem = 0;
    int found = 0;
    /* refilter first (pixels from JXL round-trip are info.rgb) */
    uint8_t *filt = NULL; size_t filt_len = 0;
    if (pngx_refilter(info.rgb, info.rgb_len, &info, &filt, &filt_len) != 0) {
        remove(jx_tmp); pngx_free(&info); return 0;
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
            if (mz_tdefl_compress(filt, filt_len, re, bound, lv, &olen) == 0 &&
                olen == info.idat_len && memcmp(re, info.idat, info.idat_len) == 0) {
                enc = 1; level = (uint8_t)lv; mem = 0;
                found = 1;
            }
            free(re);
        }
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
    (void)v; (void)name; (void)png; (void)png_len;
    return 0;
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
        rec_size = sizeof(invfs_inode_rec) + INVFS_AST_HDR_V1_LEN;
        rec = (uint8_t *)calloc(1, rec_size);
        if (!rec) return 0;
        /* empty file, v1 header by definition (nothing overflows it) */
        if (invfs_ast_hdr_write(rec + sizeof(invfs_inode_rec), 0, 0, 0) == 0) {
            free(rec);
            return 0;
        }
        rh = (invfs_inode_rec *)rec;
        rh->magic = INODE_REC_MAGIC;
        rh->rec_len = (uint32_t)rec_size;
        rh->inode_id = inode_id;
        rh->file_size = 0;
        rh->ctime = (uint64_t)time(NULL);
        rec_set_name(rh, name);
        crc = invfs_crc32c(rec, rec_size);
        if (v->inode_area_pos + rec_size + 4 > v->inode_area_end) { free(rec); return 0; }
        if (vol_pre_record(v) != 0) { free(rec); return 0; }
        if (io_seek(&v->io, v->inode_area_pos) != 0 ||
            io_write(&v->io, rec, rec_size) != 0 ||
            io_write(&v->io, &crc, 4) != 0) { free(rec); return 0; }
        v->inode_area_pos += rec_size + 4;
        idx_put(v, name, strlen(name), inode_id, v->inode_area_pos - rec_size - 4,
                rh->file_size, rh->ctime);
        idx_put_id(v, inode_id, v->inode_area_pos - rec_size - 4);
        free(rec);
        return inode_id;
    }

    inode_id = v->next_inode_id++;
    phys_blocks = (blob_len + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                       phys_blocks, 1);

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

    if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, hdr4, 8) != 0 ||
        io_write(&v->io, blob, blob_len) != 0) {
        vol_free_blocks(v, pba, phys_blocks);
        return 0;
    }
    if (vol_map(v, inode_id, 0, pba, (uint32_t)phys_blocks) != 0) {
        vol_free_blocks(v, pba, phys_blocks);
        return 0;
    }

    memset(&e, 0, sizeof e);
    e.file_offset = 0;
    e.length = orig_size;
    e.zone = INVFS_ZONE_BINARY;
    e.algo = algo;
    e.block_id = 0;

    /* v2 recipe header only when orig_size overflows v1's u32 (WP22a) */
    ast_hlen = invfs_ast_hdr_write(ast_h, orig_size, 1, 0);
    if (!ast_hlen) return 0;
    rec_size = sizeof(invfs_inode_rec) + ast_hlen + sizeof(e);
    rec = (uint8_t *)calloc(1, rec_size);
    if (!rec) return 0;
    rh = (invfs_inode_rec *)rec;
    rh->magic = INODE_REC_MAGIC;
    rh->rec_len = (uint32_t)rec_size;
    rh->inode_id = inode_id;
    rh->file_size = orig_size;
    rh->ctime = (uint64_t)time(NULL);
    rec_set_name(rh, name);
    memcpy(rec + sizeof(invfs_inode_rec), ast_h, ast_hlen);
    memcpy(rec + sizeof(invfs_inode_rec) + ast_hlen, &e, sizeof e);
    crc = invfs_crc32c(rec, rec_size);

    if (v->inode_area_pos + rec_size + 4 > v->inode_area_end) { free(rec); return 0; }
    if (vol_pre_record(v) != 0) { free(rec); return 0; }
    if (io_seek(&v->io, v->inode_area_pos) != 0 ||
        io_write(&v->io, rec, rec_size) != 0 ||
        io_write(&v->io, &crc, 4) != 0) { free(rec); return 0; }
    v->inode_area_pos += rec_size + 4;
    idx_put(v, name, strlen(name), inode_id, v->inode_area_pos - rec_size - 4,
            rh->file_size, rh->ctime);
    idx_put_id(v, inode_id, v->inode_area_pos - rec_size - 4);
    free(rec);
    return inode_id;
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
