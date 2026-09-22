/* vol_sweep.c — on-demand sweep core (embedded daemon / CLI),
 * per-inode transcode driver, manual live sweep support + volume stats.
 * Split from volume.c. */

#include "volume_internal.h"

int sweep_enospc(invfs_volume *v, uint64_t need_bytes)
{
    uint64_t need_blocks =
        (need_bytes + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    return vol_free_blocks_cached(v) < need_blocks;
}


/* WP52: the WP42 gate that kept text/binary batch deferrals off mapper
 * volumes is gone -- vol_textzone's tz_owner_write (and the WP25 owner
 * appends) now go through the extent-sized, flush-safe vol_append_slot, so
 * a sealed batch's owner record can land in a dynamic metadata extent of
 * the right size. Batch deferral is once again unconditional; only the
 * PPMd decode-memory policy keeps a file on the generic floor. Legacy
 * format_version=0 behaviour is unchanged. */

static int vol_sweep_one_v3(invfs_volume *v, uint64_t inode_id,
                            const char *name);


/* Give back everything the atomic sweep path allocated for the new record
 * before it gave up: the segments this run wrote sit in the entry table
 * (zone flipped to BINARY at write time); their extents derive from the
 * framed headers. Safe to call with ents == NULL (nothing written) or when
 * nothing was swept. Nothing has been made durable at this point -- no
 * record names the new pbas, no flush has happened -- so this is pure
 * in-memory bookkeeping plus bitmap bits, and the file is left exactly as
 * it was found. */
static void sweep_unwind(invfs_volume *v, invfs_ast_block_entry *ents,
                         uint32_t nents)
{
    uint32_t i;
    if (!ents) return;
    for (i = 0; i < nents; i++) {
        uint64_t plen = 0;
        if (ents[i].zone != INVFS_ZONE_BINARY || !ents[i].pba)
            continue;
        if (seg_extent_checked(v, ents[i].pba, &plen) == 0)
            vol_free_blocks(v, ents[i].pba, plen);
        ents[i].pba = 0;
        ents[i].zone = INVFS_ZONE_RAW;
    }
}


/* WP10 §2: the minimum compression gain (in percent) below which a file is
 * stamped UNCOMPRESSIBLE and skipped by later sweeps until a newer codec
 * generation sniffs it. INVFS_MIN_GAIN_PCT, default 0.5. */
static double vol_min_gain_pct(void)
{
    const char *e = getenv("INVFS_MIN_GAIN_PCT");
    if (!e || !*e) return 0.5;
    char *endp = NULL;
    double pct = strtod(e, &endp);
    if (endp == e || pct < 0.0 || pct >= 100.0) {
        fprintf(stderr, "[vol] INVFS_MIN_GAIN_PCT=\"%s\" invalid; using 0.5\n", e);
        return 0.5;
    }
    return pct;
}

uint16_t tz_codec_gen(uint32_t algo)
{
    const invfs_codec *c = invfs_codec_by_algo(algo);
    return c ? c->generation : 0;
}


/* WP10 §12.2: the JXL codec's decode working set from cheap JPEG headers,
 * never a trial decode. Walks the marker stream for SOF0/SOF1/SOF2
 * (0xFFC0-0xC2; baseline/extended/progressive) and returns ~w*h*3 -- the
 * pixel buffer djxl materializes before writing the JPEG back out.
 * 0 = geometry unknown (the caller admits the file and lets cjxl try). */
uint64_t jpeg_raw_estimate(const uint8_t *j, size_t n)
{
    size_t p = 2;   /* past SOI (FF D8) */

    while (p + 4 <= n) {
        uint8_t m;
        unsigned seglen;
        if (j[p] != 0xFF) { p++; continue; }   /* tolerate garbage padding */
        m = j[p + 1];
        if (m == 0xFF) { p++; continue; }      /* fill byte */
        if (m == 0x00) { p += 2; continue; }   /* stuffed 0xFF */
        if (m == 0xD9) break;                  /* EOI */
        if (m == 0xDA) break;                  /* SOS: entropy data follows */
        if (m == 0xD8 || m == 0x01 ||
            (m >= 0xD0 && m <= 0xD7)) {        /* SOI/TEM/RSTn: no length */
            p += 2;
            continue;
        }
        seglen = ((unsigned)j[p + 2] << 8) | j[p + 3];
        if (seglen < 2 || p + 2 + seglen > n) break;
        if (m >= 0xC0 && m <= 0xC2) {
            unsigned h, w;
            if (p + 9 > n) break;
            h = ((unsigned)j[p + 5] << 8) | j[p + 6];
            w = ((unsigned)j[p + 7] << 8) | j[p + 8];
            if (!w || !h) return 0;
            return (uint64_t)w * h * 3;
        }
        p += 2 + seglen;
    }
    return 0;
}


/* WP42: per-id record locator for the sweep engine. idx_get_id stores the
 * absolute offset of an id's newest record; on a v0.3.0+ mapper volume that
 * is an extent position, so the legacy [inode_area_start, inode_area_pos)
 * bound rejected every hint and each lookup came back empty (the sweep saw
 * no files at all). Mapper volumes try the index hint first, then fall back
 * to the shared vol_records_walk(); legacy volumes keep the contiguous scan
 * byte-for-byte. Returns the record position and fills *h, or 0 if absent. */
typedef struct {
    uint64_t want;
    uint64_t rec_pos;
    invfs_inode_rec h;
    char name[257];
    int found;
} sweep_locate_ctx;

static int sweep_locate_cb(void *ctx_, uint64_t rec_pos,
                           const invfs_inode_rec *h, const uint8_t *rec)
{
    sweep_locate_ctx *c = (sweep_locate_ctx *)ctx_;
    size_t nl;
    (void)rec;
    if (h->magic != INODE_REC_MAGIC || h->inode_id != c->want)
        return 0;
    if (h->name_len > INVFS_MAX_NAME ||
        h->rec_len < INVFS_REC_HDR_LEN + h->name_len + 1)
        return 0;
    c->h = *h;
    c->rec_pos = rec_pos;
    nl = h->name_len < sizeof(c->name) - 1
       ? h->name_len : sizeof(c->name) - 1;
    memcpy(c->name, h->name, nl);
    c->name[nl] = 0;
    c->found = 1;
    return 1;   /* found: stop the walk */
}

static uint64_t sweep_locate_record(invfs_volume *v, uint64_t inode_id,
                                    invfs_inode_rec *h)
{
    if (v->met0_present && v->meta_mapper && v->met0.extent_count > 0) {
        uint64_t ip = idx_get_id(v, inode_id);
        if (ip && vol_read_raw(v, ip, h, sizeof(*h)) == 0 &&
            h->magic == INODE_REC_MAGIC && h->inode_id == inode_id)
            return ip;
        {
            sweep_locate_ctx lc;
            memset(&lc, 0, sizeof lc);
            lc.want = inode_id;
            vol_records_walk(v, sweep_locate_cb, &lc);
            if (lc.found) { *h = lc.h; return lc.rec_pos; }
        }
        return 0;
    }
    {
        uint64_t start = v->inode_area_start * INVFS_BLOCK_SIZE;
        uint64_t end = v->inode_area_pos;
        uint64_t pos = start;
        uint64_t ip = idx_get_id(v, inode_id);
        if (ip >= start && ip + INVFS_REC_HDR_LEN + 1 <= end) pos = ip;
        while (pos + INVFS_REC_HDR_LEN + 1 <= end) {
            invfs_inode_rec rh;
            if (io_seek(&v->io, pos) != 0 ||
                io_read(&v->io, &rh, sizeof rh) != 0)
                return 0;
            if (rh.magic == TOMBSTONE_MAGIC) {
                pos += (uint64_t)rh.rec_len + 4;
                continue;
            }
            if (rh.magic != INODE_REC_MAGIC) return 0;
            if (rh.inode_id == inode_id) { *h = rh; return pos; }
            pos += (uint64_t)rh.rec_len + 4;
        }
    }
    return 0;
}

int vol_sweep_file(invfs_volume *v, uint64_t inode_id)
{
    invfs_meta_pub keep;
    int have_keep;
    char name[256] = "";
    int rc;

    if (!v || !inode_id) return -1;

    /* WP-M23: v3 volumes route directly to the id-keyed v3 sweep.
     * vol_sweep_one_v3 returns: 1 = swept, 0 = skipped/noop, -1 = err.
     * vol_sweep_file returns: 0 = swept, 1 = skipped/noop, -1 = err. */
    if (v->sb.vol_flags & VOLF_V3) {
        if (vol_write_active_id(v, inode_id))
            return 1;   /* skipped -- active write session */
        rc = vol_sweep_one_v3(v, inode_id, NULL);
        if (rc > 0) return 0;
        if (rc == 0) return 1;
        return -1;
    }

    have_keep = vol_get_meta(v, inode_id, &keep) == 0;
    {
        uint64_t pos = idx_get_id(v, inode_id);
        int in_area;
        /* WP42: mapper volumes locate records by the absolute index hint;
         * only legacy volumes are bounded by the contiguous area. */
        if (v->met0_present && v->meta_mapper)
            in_area = (pos != 0);
        else
            in_area = pos >= v->inode_area_start * INVFS_BLOCK_SIZE &&
                      pos + INVFS_REC_HDR_LEN + 1 <= v->inode_area_pos;
        if (in_area) {
            invfs_inode_rec h;
            if (vol_read_raw(v, pos, &h, sizeof h) == 0 &&
                h.magic == INODE_REC_MAGIC && h.inode_id == inode_id &&
                h.name_len <= INVFS_MAX_NAME &&
                h.rec_len >= INVFS_REC_HDR_LEN + h.name_len + 1 &&
                h.rec_len <= INVFS_MAX_REC_LEN) {
                uint8_t *rb = (uint8_t *)malloc(h.rec_len);
                if (rb && vol_read_raw(v, pos, rb, h.rec_len) == 0) {
                    const invfs_inode_rec *fr = (const invfs_inode_rec *)rb;
                    size_t nl = fr->name_len < sizeof(name) - 1
                              ? fr->name_len : sizeof(name) - 1;
                    memcpy(name, fr->name, nl);
                    name[nl] = 0;
                }
                free(rb);
            }
        }
    }

    /* WP4ab: a live write session aliases this file's blocks under its own
     * id; sweeping (worst case the pre-atomic in-place path, which frees
     * old segments directly) would pull them from under the session. */
    if (name[0] && vol_write_active_name(v, name))
        return 1;   /* skipped -- the caller counts it, the file stays RAW */

    rc = vol_sweep_file_inner(v, inode_id, 0);

    if (have_keep && name[0]) {
        uint64_t nid = vol_find(v, name);
        if (nid != 0 && nid != inode_id) {
            invfs_meta_pub chk;
            if (vol_get_meta(v, nid, &chk) != 0)
                vol_apply_meta(v, name, &keep);
        }
    }
    return rc;
}

int vol_sweep_file_inner(invfs_volume *v, uint64_t inode_id,
                                int generic_only)
{
    uint64_t rec_pos = 0;
    invfs_inode_rec rec_h;
    uint8_t *rec = NULL;
    uint8_t *body = NULL;
    const char *rname = NULL;
    uint32_t crc_stored, crc_calc;
    invfs_ast_hdr ast_h;
    invfs_ast_block_entry *ents;
    uint32_t i;
    int swept_any = 0;
    /* Target of the new maps. Zero until the atomic path is chosen, and the
       whole file is written under it before anything references it. */
    uint64_t new_id = 0;
    int all_raw = 1;
    /* WP10: bytes the sweep actually stored (sum of csize+8 per segment),
       for the UNCOMPRESSIBLE gain check; a codec guard that refused the file
       (guard_algo) stamps GENERIC_GUARD instead of a gain-based class --
       stamped on the NEW id at the end. (A GENERIC_MEMLIMIT/GUARD stamp a
       pack branch set BEFORE we ran lives in the copied ext and wins over
       the gain verdict below.) */
    uint64_t new_bytes = 0;
    uint32_t guard_algo = 0;

    /* locate the inode record (mapper-aware, WP42) */
    {
        rec_pos = sweep_locate_record(v, inode_id, &rec_h);
        if (rec_pos == 0) return -1;
    }

    rec = (uint8_t *)malloc(rec_h.rec_len);
    if (!rec) return -1;
    if (io_seek(&v->io, rec_pos) != 0 || io_read(&v->io, rec, rec_h.rec_len) != 0 ||
        io_read(&v->io, &crc_stored, 4) != 0) { free(rec); return -1; }
    crc_calc = invfs_crc32c(rec, rec_h.rec_len);
    if (crc_calc != crc_stored) { fprintf(stderr, "inode CRC mismatch\n"); free(rec); return -1; }
    if (rec_h.rec_len < INVFS_REC_HDR_LEN + 1) { free(rec); return -1; }

    body = invfs_rec_body((invfs_inode_rec *)rec);
    rname = ((invfs_inode_rec *)rec)->name;
    if (((invfs_inode_rec *)rec)->name_len > INVFS_MAX_NAME ||
        (size_t)rec_h.rec_len <
            INVFS_REC_HDR_LEN + ((invfs_inode_rec *)rec)->name_len + 1) {
        fprintf(stderr, "sweep: invalid record name length\n");
        free(rec);
        return -1;
    }
    if (invfs_ast_hdr_parse(body, rec_h.rec_len - (uint32_t)(body - rec),
                            &ast_h) != 0 ||
        (size_t)ast_h.num_blocks * sizeof(invfs_ast_block_entry) >
            rec_h.rec_len - (uint32_t)(body - rec) - ast_h.hdr_len) {
        fprintf(stderr, "sweep: %s: unsupported/corrupt AST recipe header\n",
                rname);
        free(rec);
        return -1;
    }
    ents = (invfs_ast_block_entry *)(body + ast_h.hdr_len);

    /* already swept (Shadow/BINARY) — nothing to do */
    if (ents[0].zone != INVFS_ZONE_RAW) {
        if (getenv("INVFS_DEBUG"))
            printf("[sweep] %s: already in Shadow (zone %u), skip\n",
                   rname, ents[0].zone);
        free(rec);
        return 1;
    }

    /* WP16e: the JPEG->JXL lane is pack-owned now (the jxl codecpack claims
     * FF D8 FF content in vol_sweep_one's WP13 pack loop -- probe ->
     * estimate-based admission -> encode -> decode-back memcmp guard ->
     * stamp -> blob, all in vol_pack_sweep). No builtin branch remains here. */

    /* APE pass: FLAC magic -> transcode whole file via MAC.exe -c4000 */
    if (!generic_only && ents[0].zone == INVFS_ZONE_RAW && getenv("INVFS_APE")) {
        uint8_t *full = NULL;
        size_t full_len = 0;
        if (vol_read_file(v, inode_id, &full, &full_len) == 0 && full_len >= 4 &&
            full[0] == 'f' && full[1] == 'L' && full[2] == 'a' && full[3] == 'C') {
            uint8_t *ape = NULL;
            size_t ape_len = 0;
            int crc_res = invfs_ape_compress(full, full_len, &ape, &ape_len);
            if (getenv("INVFS_DEBUG"))
                printf("[sweep] %s: APE rc=%d ape_len=%zu (flac %zu)\n",
                       rname, crc_res, ape_len, full_len);
            if (crc_res == 0 && ape_len < full_len) {
                /* probe decode to learn exact re-encoded FLAC size
                 * (ffmpeg re-encode differs from original FLAC bytes) */
                uint8_t *probe = NULL;
                size_t probe_len = 0;
                uint64_t fsize = full_len;
                if (invfs_ape_decompress(ape, ape_len, &probe, &probe_len) == 0) {
                    fsize = probe_len;
                    free(probe);
                }
                {
                    uint64_t newino = vol_create_ape_file(v, rname, ape, ape_len, fsize);
                    if (newino == 0)
                        fprintf(stderr, "sweep: APE create failed (%s)\n", rname);
                    else {
                        vol_delete_inode(v, inode_id, rname);
                        vol_stamp_class(v, newino, INVFS_CLASS_CODEC,
                                        INVFS_ALGO_APE, tz_codec_gen(INVFS_ALGO_APE));
                        swept_any = 1;
                    }
                }
                free(ape); free(full); free(rec);
                return swept_any ? 0 : 1;
            }
            guard_algo = INVFS_ALGO_APE;
            free(ape);
        }
        free(full);
    }

    /* Which path can be taken. A file whose segments are ALL still RAW can be
       handed to a fresh inode id wholesale. A partially swept one cannot: the
       already-Shadow segments are mapped under the old id, and
       vol_delete_inode(old) frees every block the old id's maps name -- it
       would free blocks the new record still uses. Such a file can only come
       from a volume damaged by the pre-atomic code (a mid-file alloc failure);
       finish it the old way, in place, which is the behaviour it already had. */
    for (i = 0; i < ast_h.num_blocks; i++)
        if (ents[i].zone != INVFS_ZONE_RAW) { all_raw = 0; break; }
    /* WP16b DEFER_ENOSPC: the atomic path stores the whole new shape before
     * the old blocks retire, so price the worst case up front -- the
     * original's own size raw (an incompressible segment stores as-is) plus
     * per-segment framing plus the flat margin -- and defer cheaply instead
     * of reading + recompressing every segment just to unwind at the first
     * failed alloc. The file waits RAW; the DEFER_ENOSPC stamp re-enters it
     * on every later sweep (the class predicate's case). */
    if (all_raw && ast_h.num_blocks > 0 &&
        sweep_enospc(v, (uint64_t)ast_h.file_size +
                        (uint64_t)ast_h.num_blocks * 8 + INVFS_ENOSPC_MARGIN)) {
        vol_stamp_class(v, inode_id, INVFS_CLASS_DEFER_ENOSPC,
                        INVFS_ALGO_ZSTD, tz_codec_gen(INVFS_ALGO_ZSTD));
        free(rec);
        return 1;
    }
    if (all_raw && ast_h.num_blocks > 0) {
        if (vol_mark_dirty(v) != 0) { free(rec); return -1; }
        new_id = v->next_inode_id++;
    } else if (!all_raw) {
        fprintf(stderr, "sweep: %s is partially swept; finishing in place "
                        "(pre-atomic volume)\n", rname);
    }

    for (i = 0; i < ast_h.num_blocks; i++) {
        invfs_ast_block_entry *e = &ents[i];
        uint64_t pba_old = 0, phys_len_old = 0;
        uint32_t hdr_old, crc_old;
        uint8_t *blob_old = NULL, *orig = NULL;
        size_t orig_len = e->length;
        uint64_t pba_new, phys_blocks_new;
        uint32_t csize_new, crc_new;
        uint8_t *cbuf_new = NULL;
        int cbound;
        uint8_t hdr4[8];  /* [4B csize][4B crc] — was [4] = stack overflow */

        if (e->zone != INVFS_ZONE_RAW)
            continue;  /* already swept */
        /* 1. read old segment. WP27: the entry carries the pba; the
         * physical length derives from the segment's framed header. */
        pba_old = e->pba;
        if (pba_old == 0 || pba_old >= v->sb.total_blocks) {
            fprintf(stderr, "sweep: invalid pba seg %u\n", e->block_id);
            sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1;
        }
        {
            uint8_t hdrb[8];
            if (io_seek(&v->io, pba_old * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, hdrb, 8) != 0) { sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1; }
            memcpy(&hdr_old, hdrb, 4);
            memcpy(&crc_old, hdrb + 4, 4);
        }
        phys_len_old = ((uint64_t)hdr_old + 8 + INVFS_BLOCK_SIZE - 1) /
                       INVFS_BLOCK_SIZE;
        blob_old = (uint8_t *)malloc(hdr_old);
        if (!blob_old) { sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1; }
        if (io_seek(&v->io, pba_old * INVFS_BLOCK_SIZE + 8) != 0 ||
            io_read(&v->io, blob_old, hdr_old) != 0) { free(blob_old); sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1; }
        /* deep protection: verify segment CRC32C */
        if (crc_old != 0 && invfs_crc32c(blob_old, hdr_old) != crc_old) {
            fprintf(stderr, "sweep: segment CRC mismatch inode %llu seg %u\n",
                    (unsigned long long)inode_id, e->block_id);
            free(blob_old); sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1;
        }

        /* 2. decompress to original */
        orig = (uint8_t *)malloc(orig_len ? orig_len : 1);
        if (!orig) { free(blob_old); sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1; }
        if (e->algo == INVFS_ALGO_LZ4) {
            int got = LZ4_decompress_safe((const char *)blob_old, (char *)orig,
                                          (int)hdr_old, (int)orig_len);
            if (got != (int)orig_len) { free(blob_old); free(orig); sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1; }
        } else if (e->algo == INVFS_ALGO_ZSTD) {
            /* WP23 (cross-lane touch, flagged for the WP22e lane): a RAW
             * segment written under fill pressure is ZSTD -- decode it
             * with the same per-segment dispatch the read paths already
             * had, or the sweep would mis-decode it as verbatim NONE. */
            size_t got = ZSTD_decompress(orig, orig_len, blob_old, hdr_old);
            if (ZSTD_isError(got) || got != orig_len) { free(blob_old); free(orig); sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1; }
        } else {  /* NONE */
            if (hdr_old != orig_len) { free(blob_old); free(orig); sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1; }
            memcpy(orig, blob_old, orig_len);
        }
        free(blob_old);

        /* 3. recompress at the volume's profile (WP16b/WP19): the levelled
         * profiles re-encode ZSTD (default balanced = the historical 19);
         * the meta-profiles substitute the floor codec at admission --
         * fastest = LZ4, turbo = verbatim store. Fallback raw as always. */
        cbound = (v->profile == INVFS_PROFILE_FASTEST)
               ? LZ4_compressBound((int)orig_len)
               : (int)ZSTD_compressBound(orig_len);
        cbuf_new = (uint8_t *)malloc((size_t)cbound + 8 + INVFS_BLOCK_SIZE);
        if (!cbuf_new) { free(orig); sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1; }
        switch (invfs_profile_generic_algo(v->profile)) {
        case INVFS_ALGO_NONE:      /* turbo: verbatim, segment-aligned */
            csize_new = (uint32_t)orig_len;
            memcpy(cbuf_new + 8, orig, orig_len);
            e->algo = INVFS_ALGO_NONE;
            break;
        case INVFS_ALGO_LZ4:       /* fastest */
            csize_new = (uint32_t)LZ4_compress_default((const char *)orig,
                                                       (char *)(cbuf_new + 8),
                                                       (int)orig_len, cbound);
            if (csize_new == 0 || csize_new >= orig_len) {
                csize_new = (uint32_t)orig_len;
                memcpy(cbuf_new + 8, orig, orig_len);
                e->algo = INVFS_ALGO_NONE;
            } else {
                e->algo = INVFS_ALGO_LZ4;
            }
            break;
        default:                   /* the levelled ZSTD profiles */
            csize_new = (uint32_t)ZSTD_compress(cbuf_new + 8, cbound, orig,
                                                orig_len,
                                                invfs_profile_zstd_level(v->profile));
            if (ZSTD_isError(csize_new) || csize_new >= orig_len) {
                csize_new = (uint32_t)orig_len;   /* store raw */
                memcpy(cbuf_new + 8, orig, orig_len);
                e->algo = INVFS_ALGO_NONE;
            } else {
                e->algo = INVFS_ALGO_ZSTD;
            }
            break;
        }
        e->zone = INVFS_ZONE_BINARY;
        /* block_id stays the segment index (L2P key) */
        crc_new = invfs_crc32c(cbuf_new + 8, csize_new);

        /* 4. write to Shadow zone */
        phys_blocks_new = ((uint64_t)csize_new + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
        pba_new = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                               phys_blocks_new, 1, INVFS_ALLOC_DATA);
        if (pba_new == 0) { free(cbuf_new); free(orig); sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1; }
        hdr4[0] = (uint8_t)(csize_new & 0xFF);
        hdr4[1] = (uint8_t)((csize_new >> 8) & 0xFF);
        hdr4[2] = (uint8_t)((csize_new >> 16) & 0xFF);
        hdr4[3] = (uint8_t)((csize_new >> 24) & 0xFF);
        hdr4[4] = (uint8_t)(crc_new & 0xFF);
        hdr4[5] = (uint8_t)((crc_new >> 8) & 0xFF);
        hdr4[6] = (uint8_t)((crc_new >> 16) & 0xFF);
        hdr4[7] = (uint8_t)((crc_new >> 24) & 0xFF);
        memcpy(cbuf_new, hdr4, 8);
        new_bytes += (uint64_t)csize_new + 8;   /* WP10 gain accounting */
        if (write_segment_blocks(v, pba_new, cbuf_new, (size_t)csize_new + 8,
                                 phys_blocks_new) != 0) {
            free(cbuf_new); free(orig); sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1;
        }
        free(cbuf_new);

        /* 5. WP27: the entry carries the new segment's address -- no map,
         * no journal traffic. On the atomic path the new pba lands in the
         * in-memory copy of the record and the old record stays readable
         * until the new one is published. */
        e->pba = pba_new;
        if (!new_id) {
            /* 6. free old RAW blocks (in-place path only; the atomic path
             * leaves them to vol_delete_inode of the old id, which is what
             * makes the old record readable until the new one lands).
             *
             * Free exactly what was allocated: the physical block count
             * derives from the segment's framed header (csize+8, padded to
             * whole blocks -- the write_segment_blocks contract). */
            vol_free_blocks(v, pba_old, phys_len_old);
        }
        free(orig);
        swept_any = 1;

        if (getenv("INVFS_DEBUG"))
            printf("[sweep] inode %llu seg %u: RAW(%llu) -> SHADOW(%llu) %u bytes (%s)\n",
                   (unsigned long long)inode_id, e->block_id,
                   (unsigned long long)pba_old, (unsigned long long)pba_new,
                   csize_new, e->algo == INVFS_ALGO_ZSTD ? "zstd" :
                              e->algo == INVFS_ALGO_LZ4 ? "lz4" : "raw");
    }

    /* 7. Publish. On the atomic path the record is appended under new_id and
     * the old one tombstoned -- the same delete+create shape every other
     * transcode uses, and the reason vol_pre_record's ordering is enough here:
     * new_id's maps are on disk before any record mentions new_id, and the old
     * record stays readable until the instant the new one lands. */
    if (swept_any && new_id) {
        invfs_inode_rec *nh = (invfs_inode_rec *)rec;
        char name[INVFS_MAX_NAME + 1];
        size_t nlen = rec_h.name_len > INVFS_MAX_NAME ? INVFS_MAX_NAME : rec_h.name_len;
        memcpy(name, rname, nlen);
        name[nlen] = 0;

        nh->inode_id = new_id;
        crc_calc = invfs_crc32c(rec, rec_h.rec_len);
        if (!(v->met0_present && v->meta_mapper) &&
            inode_area_make_room(v, rec_h.rec_len + 4 +
                INVFS_REC_HDR_LEN + nlen + 1 + 4) != 0) {
            /* no room for the record AND the tombstone that must follow it */
            fprintf(stderr, "sweep: inode area full (%s)\n", name);
            sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1;
        }
        if (vol_pre_record(v) != 0) { sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1; }
        /* Bug J: route the append through the mapper */
        {
            uint64_t npos;
            int rc2 = vol_append_slot(v, (uint64_t)rec_h.rec_len + 4, &npos);
            if (rc2 != 0) {
                sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1;
            }
            if (io_seek(&v->io, npos) != 0 ||
                io_write(&v->io, rec, rec_h.rec_len) != 0 ||
                io_write(&v->io, &crc_calc, 4) != 0) {
                sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks); free(rec); return -1;
            }
            idx_put(v, name, nlen, new_id, npos, nh->file_size, nh->ctime);
            idx_put_id(v, new_id, npos);
        }
        pba_ref_apply(v, rec, rec_h.rec_len, +1);
        /* frees the old RAW blocks: they are still referenced by the old
         * record until the retire drops it */
        if (vol_delete_inode(v, inode_id, name) != 0)
            fprintf(stderr, "sweep: %s transcoded but the old record survived; "
                            "invf-fsck -f will reclaim it\n", name);
    } else if (swept_any) {
        /* in-place, pre-atomic volumes only (see the note above the loop) */
        crc_calc = invfs_crc32c(rec, rec_h.rec_len);
        if (io_seek(&v->io, rec_pos) != 0 ||
            io_write(&v->io, rec, rec_h.rec_len) != 0 ||
            io_write(&v->io, &crc_calc, 4) != 0) { free(rec); return -1; }
        pba_ref_reset(v);   /* the record mutated in place; the pba map
                             * rebuilds lazily from the live set */
    } else {
        sweep_unwind(v, new_id ? ents : NULL, ast_h.num_blocks);   /* nothing swept: give the id's maps back */
    }
    /* WP10 §2: stamp the outcome class. A codec guard that refused the file
     * pins GENERIC_GUARD (retried when that codec's generation grows past the
     * stored one); otherwise the file-level gain decides UNCOMPRESSIBLE vs
     * GENERIC. A GUARD/MEMLIMIT stamp the caller set before we ran (it lives
     * in the copied ext) wins over the gain verdict -- it carries the retry
     * semantics the gain verdict knows nothing about. */
    if (swept_any && ast_h.file_size > 0) {
        uint64_t tid = new_id ? new_id : inode_id;
        uint8_t oc = 0, oa = 0;
        uint16_t og = 0;
        int havec = vol_get_class(v, tid, &oc, &oa, &og) == 0;
        if (guard_algo) {
            vol_stamp_class(v, tid, INVFS_CLASS_GENERIC_GUARD,
                            (uint8_t)guard_algo, tz_codec_gen(guard_algo));
        } else if (!havec || oc == INVFS_CLASS_GENERIC ||
                   oc == INVFS_CLASS_UNCOMPRESSIBLE) {
            double pct = vol_min_gain_pct();
            if ((double)new_bytes >=
                (double)ast_h.file_size * (1.0 - pct / 100.0))
                vol_stamp_class(v, tid, INVFS_CLASS_UNCOMPRESSIBLE, 0,
                                invfs_registry_generation());
            else
                vol_stamp_class(v, tid, INVFS_CLASS_GENERIC, INVFS_ALGO_ZSTD,
                                tz_codec_gen(INVFS_ALGO_ZSTD));
        }
    }
    free(rec);
    return swept_any ? 0 : 1;
}


/* ---- on-demand sweep core (embedded daemon / CLI) ---- */

/* pending list lives in RAM (daemon holds the volume open with one L2P
   in memory); a crash leaves files unswept — invariant: nothing lost. */
void vol_mark_pending(invfs_volume *v, uint64_t inode_id)
{
    if (!v || inode_id == 0) return;
    for (size_t i = 0; i < v->n_pending; i++)
        if (v->pending[i] == inode_id) return;
    if (v->n_pending >= v->cap_pending) {
        size_t nc = v->cap_pending ? v->cap_pending * 2 : 64;
        uint64_t *np = (uint64_t *)realloc(v->pending, nc * sizeof(uint64_t));
        if (!np) return;
        v->pending = np;
        v->cap_pending = nc;
    }
    v->pending[v->n_pending++] = inode_id;
}


void vol_unmark_pending(invfs_volume *v, uint64_t inode_id)
{
    if (!v) return;
    for (size_t i = 0; i < v->n_pending; i++) {
        if (v->pending[i] == inode_id) {
            v->pending[i] = v->pending[v->n_pending - 1];
            v->n_pending--;
            return;
        }
    }
}


size_t vol_pending_count(invfs_volume *v)
{
    return v ? v->n_pending : 0;
}


/* Zone of an inode's first AST segment, or -1 if it cannot be read.
   RAW means "never swept". Needed because vol_read_file hands back
   DECODED bytes: a PMP inode still looks exactly like an MP3 to a magic
   test, so without this a re-sweep would transcode it again -- costing
   seconds per file and rewriting flash for no gain. The other container
   codecs dodge this by checking for their sibling "!recipe" inode; a PMP
   blob has no sibling, so it checks the zone directly. */
int vol_inode_first_zone(invfs_volume *v, uint64_t inode_id)
{
    uint64_t pos, bofs;
    invfs_inode_rec rec_h;
    uint8_t hb[INVFS_AST_HDR_V2_LEN];
    invfs_ast_hdr ast_h;
    invfs_ast_block_entry e0;
    uint32_t ver;

    /* WP-M21b: v3 -- no record stream to locate; the zone of entry 0
     * comes from the content-addressed recipe blob (same parse the read
     * path uses). Without this the absent-class branch of vol_sweep_one
     * saw fz == -1 != RAW for EVERY v3 file and skipped the whole live
     * set ("sweep done: swept=0 skipped=N"). */
    if (v->sb.vol_flags & VOLF_V3) {
        invfs_v3_inode in;
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents = NULL;
        size_t n_ents = 0;
        uint8_t *blob = NULL;
        size_t blen = 0;
        int zone = -1;
        static const uint8_t zero_addr[INVFS_V3_RECIPE_ADDR_LEN];

        if (vol_v3_inode_get(v, inode_id, &in) != 1)
            return -1;
        if (in.size == 0 ||
            memcmp(in.recipe_addr, zero_addr,
                   INVFS_V3_RECIPE_ADDR_LEN) == 0)
            return -1;                  /* empty file: no zone */
        if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0)
            return -1;
        if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) == 0 &&
            n_ents > 0)
            zone = ents[0].zone;
        free(blob);
        return zone;
    }

    /* WP42: mapper-aware locate; legacy volumes keep the contiguous scan */
    pos = sweep_locate_record(v, inode_id, &rec_h);
    if (pos == 0) return -1;
    /* WP58a: the body offset depends on the variable-length name; rec_h is
     * the 36-byte prefix, so derive it from rec_len-independent fields. */
    bofs = (uint64_t)INVFS_REC_HDR_LEN + rec_h.name_len + 1;
    /* recipe header length is version-dependent (16 B v1 / 24 B v2):
     * read the version word first, then the full header, then entry 0
     * behind it */
    if (io_seek(&v->io, pos + bofs) != 0 ||
        io_read(&v->io, &ver, 4) != 0) return -1;
    if (ver != INVFS_AST_VERSION_V1 && ver != INVFS_AST_VERSION_V2)
        return -1;
    {
        size_t need = ver == INVFS_AST_VERSION_V1 ? INVFS_AST_HDR_V1_LEN
                                                  : INVFS_AST_HDR_V2_LEN;
        if (io_seek(&v->io, pos + bofs) != 0 ||
            io_read(&v->io, hb, need) != 0)
            return -1;
        if (invfs_ast_hdr_parse(hb, need, &ast_h) != 0)
            return -1;
        if (ast_h.num_blocks == 0) return -1;
        if (io_seek(&v->io, pos + bofs + need) != 0 ||
            io_read(&v->io, &e0, sizeof e0) != 0) return -1;
    }
    return (int)e0.zone;
}


/* WP14b candidate shape (WP10 §12.7 v2): every AST entry is a per-segment
 * generic store (BINARY zone, NONE/LZ4/ZSTD algo). An extraction container's
 * part ("name!partN") looks exactly like this before batching; a part
 * holding anything else (a whole-file JXL/APE blob, a batch member) is not
 * a candidate. 1 = the shape matches. */
static int part_generic_segments(invfs_volume *v, uint64_t inode_id)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents;
    size_t base;
    uint32_t i;
    int ok = 0;

    if (meta_read_record_by_id(v, inode_id, &buf, &rl, NULL, 0, NULL) != 0)
        return 0;
    base = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)buf) - buf);
    if (rl >= base + INVFS_AST_HDR_V1_LEN &&
        invfs_ast_hdr_parse(buf + base, rl - base, &ah) == 0 &&
        ah.num_blocks && ah.num_children == 0 &&
        rl >= base + ah.hdr_len +
               (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
        ents = (const invfs_ast_block_entry *)(buf + base + ah.hdr_len);
        ok = 1;
        for (i = 0; i < ah.num_blocks; i++) {
            if (ents[i].zone != INVFS_ZONE_BINARY ||
                (ents[i].algo != INVFS_ALGO_NONE &&
                 ents[i].algo != INVFS_ALGO_LZ4 &&
                 ents[i].algo != INVFS_ALGO_ZSTD)) {
                ok = 0;
                break;
            }
        }
    }
    free(buf);
    return ok;
}

static int vol_pack_sweep(invfs_volume *v, uint64_t inode_id,
                          const char *name, const invfs_codec *pc,
                          const uint8_t *full, size_t full_len);


/* WP14b: defer the parts of a just-exploded extraction container
 * ("name!partN", N = 0..) into the batching accumulators, so a TAR swept in
 * this run has its members batched by THIS run's vol_tz_flush instead of
 * sitting one run in per-file ZSTD. The flush re-reads and re-sniffs each
 * part from its live record, so a head sniff is enough here; parts that
 * sniff as nothing stay per-file generic (the absent-stamp walk branch
 * reconsiders them next run). */
void defer_container_parts(invfs_volume *v, const char *name)
{
    uint8_t head[8192];
    char pn[320];
    unsigned i;
    int n_bin = 0, n_text = 0;

    for (i = 0; ; i++) {
        uint64_t pino, fsz = 0;
        int got, bfam, tfam;

        snprintf(pn, sizeof pn, "%s!part%u", name, i);
        pino = vol_find(v, pn);
        if (!pino) break;
        got = vol_read_range(v, pino, 0, sizeof head, head);
        if (got <= 0 ||
            vol_stat_full(v, pn, NULL, &fsz, NULL) != 0 || !fsz)
            continue;
        bfam = invfs_binary_family(head, (size_t)got, pn);
        if (bfam > 0) {
            if (bz_defer(v, pino, pn, fsz, (uint32_t)bfam) == 0) n_bin++;
            continue;
        }
        tfam = invfs_text_family(pn, head, (size_t)got);
        if (tfam > 0) {
            const invfs_codec *pc = invfs_codec_by_algo(INVFS_ALGO_PPMD);
            if (pc && pc->dec_mem_bytes > vol_get_dec_mem_limit(v))
                vol_stamp_class(v, pino, INVFS_CLASS_GENERIC_MEMLIMIT,
                                INVFS_ALGO_PPMD, pc->generation);
            else if (tz_defer(v, pino, pn, fsz, (uint32_t)tfam) == 0)
                n_text++;
        }
    }
    /* one summary line per container, not one per part (a Silesia TAR has
     * ~1500 members); same format the sweep driver's part aggregator uses */
    if (n_bin)
        printf("  %s!*: %d parts -> ZSTD batch\n", name, n_bin);
    if (n_text)
        printf("  %s!*: %d parts -> PPMd batch\n", name, n_text);
}


/* WP12(b): JPEG upgrade retry for a file the class predicate just re-armed
 * (GENERIC_MEMLIMIT: the raised dec_mem limit admits it; GENERIC_GUARD: a
 * newer codec generation). By the time either stamp exists the file is
 * stored as generic BINARY segments, so the RAW-gated pack loop in
 * vol_sweep_one could never see it again -- this runs the SAME attempt on
 * the current (decoded) content. WP16e: the attempt is the jxl codecpack's
 * (vol_pack_sweep over the registry entry for INVFS_ALGO_JXL, which IS the
 * pack once it is loaded): probe -> estimate admission -> cjxl encode ->
 * djxl decode-back memcmp guard -> new blob inode first, retire the old one
 * after, meta carried across.
 * Outcomes (vol_pack_sweep's): transcode -> CODEC{JXL, pack gen} on the new
 * id, returns 100+algo; still over the limit -> GENERIC_MEMLIMIT re-stamped
 * at the current generation, 0; guard refusal -> GENERIC_GUARD{JXL, current
 * gen}, 0 (a stale gen would refire the retry on every sweep); pack not
 * loaded, its tools absent, or the volume full (DEFER_ENOSPC) -> stamp
 * untouched, the file waits, 0. -1 only on a read failure. */
int vol_jxl_retry(invfs_volume *v, uint64_t inode_id, const char *name)
{
    const invfs_codec *jc = invfs_codec_by_algo(INVFS_ALGO_JXL);
    uint8_t *full = NULL;
    size_t full_len = 0;
    int rc;

    /* the pack (or its tools) vanished since the stamp was written: keep
     * the stamp -- the first sweep after it returns picks the file up
     * again. A placeholder entry (no pack override) has no trampolines. */
    if (!jc || !jc->encode || !jc->decode) return 0;
    if (vol_read_file(v, inode_id, &full, &full_len) != 0) return -1;
    /* the stamp says "JPEG rejected earlier"; if the content is not one
     * any more the stamp is stale -- leave file and stamp alone */
    if (full_len < 3 || full[0] != 0xFF || full[1] != 0xD8 || full[2] != 0xFF) {
        free(full);
        return 0;
    }

    rc = vol_pack_sweep(v, inode_id, name, jc, full, full_len);
    free(full);
    return rc == 1 ? 0 : rc;   /* 1 = wait RAW (tools/space): keep waiting */
}


/* WP-M21b: the v3 sweep path. The v2 full path below operates on the
 * record stream (sweep_locate_record, L2P maps, atomic new-id records)
 * which does not exist on a v3 volume -- it fails every file with -1
 * (measured: "sweep done: failed=5" on a 5-file v3 volume). The v3
 * drain with the same invariants: read the whole file (M8 recipe read)
 * -> ZSTD encode -> decode round-trip guard (publish only bit-exact)
 * -> publish via vol_create_blob_file's v3 branch (content-addressed
 * recipe blob + in-place row supersede; the superseded RAW data segments
 * are reclaimed immediately via targeted free vol_v3_free_recipe_blocks).
 * Class stamps (v3 xattrs, WP-M7) keep the WP10 policy: drained files
 * skip, uncompressible files are generation-gated, ENOSPC defers.
 * Deliberate M18-remainder gaps (v2 owner-record machinery, not yet
 * re-expressed on the v3 trees): per-segment Shadow clustering (the
 * whole-file entry is arc-budget bounded instead), text PPMd / binary
 * ZSTD batching, and container-pack transcode dispatch. Text and
 * binary both take generic ZSTD -- a weaker ratio than batching,
 * invariant-preserving. Returns follow vol_sweep_one's convention. */
static int vol_sweep_one_v3(invfs_volume *v, uint64_t inode_id,
                            const char *name)
{
    uint8_t ccls = 0, calgo = 0;
    uint16_t cgen = 0;
    invfs_v3_inode in;
    uint8_t *full = NULL, *enc = NULL, *back = NULL;
    size_t full_len = 0, enc_cap, enc_len = 0;
    const invfs_codec *zc;
    uint64_t newino;
    int fz;

    if (!v || !inode_id)
        return 0;

    /* WP-M23: active write session guard by inode_id and name */
    if (vol_write_active_id(v, inode_id))
        return 0;
    if (name && name[0] && vol_write_active_name(v, name))
        return 0;

    /* class policy: a stamp means drained-or-gated (subset of the WP10
     * table that needs no v2 record surgery) */
    if (vol_get_class(v, inode_id, &ccls, &calgo, &cgen) == 0) {
        switch (ccls) {
        case INVFS_CLASS_UNCOMPRESSIBLE:
            if (invfs_registry_generation() <= cgen)
                return 0;       /* retry only when the registry grew */
            break;
        case INVFS_CLASS_DEFER_ENOSPC:
            break;              /* space is a property of NOW: retry */
        default:
            return 0;           /* GENERIC/CODEC/TEXT/...: already swept */
        }
    }

    if (vol_v3_inode_get(v, inode_id, &in) != 1)
        return -1;
    if (in.size == 0)
        return 0;               /* nothing to drain: skip */
    if (in.type != 0 && in.type != INVFS_ITYP_REG)
        return 0;               /* only regular files are drained: skip */
    fz = vol_inode_first_zone(v, inode_id);
    if (fz != INVFS_ZONE_RAW)
        return 0;               /* already blob-stored; unreadable: skip */
    {
        uint64_t max_sweep_size = 256ULL * 1024ULL * 1024ULL; /* 256 MiB default */
        const char *env_max = getenv("INVFS_SWEEP_MAX_FILE");
        if (env_max && *env_max) {
            unsigned long long sz = strtoull(env_max, NULL, 10);
            if (sz > 0)
                max_sweep_size = sz;
        } else if (v->arc_budget && v->arc_budget < max_sweep_size) {
            max_sweep_size = v->arc_budget;
        }
        if (in.size > max_sweep_size)
            return 0;           /* whole-file buffering exceeds memory safety cap: skip */
    }

    zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
    if (!zc || !zc->encode || !zc->decode)
        return -1;

    if (vol_read_inode(v, inode_id, 0, &full, &full_len) != 0 || !full)
        return -1;
    if (full_len != (size_t)in.size) {
        free(full);
        return -1;              /* row/recipe disagree: do not touch */
    }

    if (sweep_enospc(v, (uint64_t)full_len + full_len / 4 + 65536 +
                        INVFS_ENOSPC_MARGIN)) {
        free(full);
        vol_stamp_class(v, inode_id, INVFS_CLASS_DEFER_ENOSPC,
                        INVFS_ALGO_ZSTD, invfs_registry_generation());
        return 0;
    }

    int zlevel = invfs_profile_zstd_level(v->profile);
    enc_cap = ZSTD_compressBound(full_len);
    enc = (uint8_t *)malloc(enc_cap);
    if (!enc) { free(full); return -1; }
    size_t zrc = ZSTD_compress(enc, enc_cap, full, full_len, zlevel);
    if (ZSTD_isError(zrc) || zrc == 0 || zrc >= full_len || zrc > 0xFFFFFFFFu) {
        /* no gain or exceeds blob cap: generation-gated skip until the registry grows */
        free(enc);
        free(full);
        vol_stamp_class(v, inode_id, INVFS_CLASS_UNCOMPRESSIBLE, 0,
                        invfs_registry_generation());
        return 0;
    }
    enc_len = zrc;
    back = (uint8_t *)malloc(full_len ? full_len : 1);
    if (!back || zc->decode(enc, enc_len, back, full_len) != 0 ||
        memcmp(back, full, full_len) != 0) {
        /* the round-trip guard refused: stay RAW, unstamped, retry */
        free(back);
        free(enc);
        free(full);
        return 0;
    }
    free(back);

    /* WP-M23: id-keyed publication directly supersedes the inode's recipe address.
     * All metadata (mode, uid, gid, mtime, atime) and all dirents (nested,
     * root, hardlinks) remain completely preserved without path resolution. */
    newino = vol_v3_publish_blob_inode(v, inode_id, enc, enc_len, full_len,
                                       INVFS_ALGO_ZSTD);
    free(enc);
    free(full);
    if (!newino)
        return 0;           /* store failed (ENOSPC): stay RAW */

    vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC, INVFS_ALGO_ZSTD,
                    invfs_registry_generation());
    return 1;
}

/* process a single inode: container explode / transcode / shadow move.
   Returns 1 if the inode was replaced/transcoded, 0 if not applicable,
   -1 on hard error (caller keeps it pending or aborts). */
int vol_sweep_one(invfs_volume *v, uint64_t inode_id, const char *name)
{
    char rname[272], p0name[272], jn[272];
    uint8_t *full = NULL;
    size_t full_len = 0;

    if (!v || inode_id == 0) return 0;

    /* WP-M23: active session guard by id and name */
    if (vol_write_active_id(v, inode_id)) return 0;
    if (name && name[0] && vol_write_active_name(v, name)) return 0;

    /* WP-M21b: v3 volumes take the blob-store path; everything below
     * this line is v2 record surgery. */
    if (v->sb.vol_flags & VOLF_V3)
        return vol_sweep_one_v3(v, inode_id, name);

    if (!name || strlen(name) > 240) return 0;
    /* internal control names (the "\x01tzb" batch owner) are never swept */
    if ((uint8_t)name[0] == 0x01) return 0;

    /* WP10 §2: class-aware walk predicate. A present class flag decides
     * skip/retry/downgrade without touching content; absent = the legacy
     * rule (RAW zone = full path below, anything else = already swept). */
    {
        uint8_t ccls = 0, calgo = 0;
        uint16_t cgen = 0;
        if (vol_get_class(v, inode_id, &ccls, &calgo, &cgen) == 0) {
            const invfs_codec *cc = invfs_codec_by_algo(calgo);
            switch (ccls) {
            case INVFS_CLASS_UNCOMPRESSIBLE:
                /* Retry only when the registry grew AND a codec actually
                 * sniffs the content now (the sniff is the cheap part --
                 * one head segment, no full read). */
                if (invfs_registry_generation() <= cgen) return 0;
                if (!tz_sniff_any(v, inode_id, name)) {
                    /* Nothing claims it even now: advance the snapshot so
                     * the next sweep skips the sniff until the registry
                     * grows again. */
                    vol_stamp_class(v, inode_id, INVFS_CLASS_UNCOMPRESSIBLE, 0,
                                    invfs_registry_generation());
                    return 0;
                }
                break;
            case INVFS_CLASS_GENERIC_GUARD:
                if (!cc || cc->generation <= cgen) return 0;
                /* WP12(b): a JXL retry lives behind the generic store
                 * now -- the RAW-gated branch can never see it again */
                if (calgo == INVFS_ALGO_JXL)
                    return vol_jxl_retry(v, inode_id, name);
                break;   /* new sub-encoder: retry via the full path */
            case INVFS_CLASS_GENERIC_MEMLIMIT:
                if (!cc || cc->dec_mem_bytes > vol_get_dec_mem_limit(v))
                    return 0;
                if (calgo == INVFS_ALGO_JXL)
                    return vol_jxl_retry(v, inode_id, name);
                break;   /* the policy now admits the codec: retry */
            case INVFS_CLASS_DEFER_ENOSPC:
                /* WP16b: the sweep deferred this file for free space. Space
                 * is a property of NOW, not of the file or the codec, so the
                 * file re-enters the full path on EVERY sweep (like an
                 * absent stamp): admitted when the free blocks suffice,
                 * re-stamped (a check-then-write no-op) when they do not. */
                break;
            case INVFS_CLASS_ANCHORED:
                /* WP59a: anchored files must never be transcoded by a
                 * pack/container codec (the chicken-and-egg: you need packs
                 * to read pack-coded files, but the packs themselves must
                 * be readable without packs).  Leave the file alone;
                 * builtin LZ4/ZSTD/PPMd-via-batch remain admissible if the
                 * file enters the full path through the absent-stamp branch
                 * (a fresh sweep), but this class stamp prevents the sweep
                 * from re-entering the full path on subsequent runs. */
                return 0;
            default: {
                /* TEXT/BATCHED_BIN/CODEC/CONTAINER/GENERIC: policy
                 * compliance. The generic floor codecs (NONE/LZ4/ZSTD) are
                 * always compliant -- there is nothing cheaper left to
                 * downgrade INTO. (A BATCHED_BIN member stamped with the
                 * ZSTD_BCJ AST tag finds no registry entry -- the BCJ tag
                 * names a pipeline stage, not a codec -- and skips the
                 * codec-level checks entirely; the batch-size rule below is
                 * its compliance check.) */
                int violated = 0;
                if (cc && calgo != INVFS_ALGO_NONE &&
                    calgo != INVFS_ALGO_LZ4 && calgo != INVFS_ALGO_ZSTD) {
                    if (cc->dec_mem_bytes > vol_get_dec_mem_limit(v))
                        violated = 1;
                    if (!violated && (cc->caps & INVFS_CODEC_CAP_WHOLEFILE) &&
                        v->arc_budget) {
                        uint64_t fsz = 0;
                        vol_stat_full(v, name, NULL, &fsz, NULL);
                        if (fsz > v->arc_budget) violated = 1;
                    }
                }
                /* a batch that outgrew the cache budget un-batches: arc
                 * refuses entries over budget/2 (arc.c), so the policy
                 * guarantee is read-time, and the member re-stores generic.
                 * BATCHED_BIN (WP14a) obeys the same rule -- the batch
                 * payload's [4B usize] header is codec-independent, so
                 * tz_member_oversized reads zstd batches unchanged. */
                if (!violated && (ccls == INVFS_CLASS_TEXT ||
                                  ccls == INVFS_CLASS_BATCHED_BIN) &&
                    v->arc_budget &&
                    tz_member_oversized(v, inode_id, v->arc_budget / 2))
                    violated = 1;
                /* WP14a migration path: a GENERIC file (pre-WP14a that
                 * means per-segment ZSTD) whose head sniffs as an
                 * executable family re-enters the full path below and
                 * defers into the binary accumulator. ALWAYS on -- not
                 * generation-gated: binary batching shipped in the same
                 * build as this predicate, so a GENERIC stamp on a
                 * binary-family file can only predate it, and re-batching
                 * it is the upgrade the stamp exists to permit. (The
                 * UNCOMPRESSIBLE stamp stays generation-gated: such a file
                 * already proved the gain is not there.) */
                if (!violated && ccls == INVFS_CLASS_GENERIC &&
                    !strchr(name, '!')) {
                    uint8_t head[8192];
                    int got = vol_read_range(v, inode_id, 0, sizeof head,
                                             head);
                    if (got > 0 &&
                        invfs_binary_family(head, (size_t)got, name) > 0)
                        break;   /* -> full path: re-read + bz_defer */
                }
                if (!violated) return 0;
                {
                    /* the downgrade stamp names the batch PAYLOAD codec:
                     * BCJ is a pipeline stage with no registry entry, so a
                     * MEMLIMIT{ZSTD_BCJ} stamp could never re-arm (the
                     * predicate's by_algo lookup finds nothing). ZSTD is
                     * the codec the retry consults; it is always admitted,
                     * so the re-batch fires on the very next sweep and
                     * re-targets the CURRENT batch size. */
                    uint8_t dalgo = (ccls == INVFS_CLASS_BATCHED_BIN &&
                                     calgo == INVFS_ALGO_ZSTD_BCJ)
                                  ? (uint8_t)INVFS_ALGO_ZSTD : calgo;
                    return vol_store_generic(v, inode_id, name,
                                             INVFS_CLASS_GENERIC_MEMLIMIT,
                                             dalgo) == 0 ? 6 : -1;
                }
            }
            }
        } else {
            int fz = vol_inode_first_zone(v, inode_id);
            if (fz != INVFS_ZONE_RAW) {
                /* WP14b (WP10 §12.7 v2): an extraction container's part
                 * ("name!partN") stored per-segment generic -- absent class
                 * stamp, BINARY zone, NONE/LZ4/ZSTD algos -- is a batching
                 * candidate: sniff the head, defer into the binary or text
                 * accumulator. Parts already in batches (zone TEXT) and
                 * whole-file blob siblings skip here as before. The flush
                 * re-validates from the live record. */
                if (fz == INVFS_ZONE_BINARY && strchr(name, '!') &&
                    part_generic_segments(v, inode_id)) {
                    uint8_t head[8192];
                    uint64_t fsz = 0;
                    int got = vol_read_range(v, inode_id, 0, sizeof head,
                                             head);
                    int bfam, tfam;
                    if (got > 0 &&
                        vol_stat_full(v, name, NULL, &fsz, NULL) == 0 &&
                        fsz) {
                        bfam = invfs_binary_family(head, (size_t)got, name);
                        if (bfam > 0 &&
                            bz_defer(v, inode_id, name, fsz,
                                     (uint32_t)bfam) == 0)
                            return 10;   /* part -> ZSTD batch */
                        tfam = invfs_text_family(name, head, (size_t)got);
                        if (tfam > 0) {
                            const invfs_codec *pc =
                                invfs_codec_by_algo(INVFS_ALGO_PPMD);
                            if (pc && pc->dec_mem_bytes >
                                      vol_get_dec_mem_limit(v)) {
                                vol_stamp_class(v, inode_id,
                                                INVFS_CLASS_GENERIC_MEMLIMIT,
                                                INVFS_ALGO_PPMD,
                                                pc->generation);
                            } else if (tz_defer(v, inode_id, name, fsz,
                                                (uint32_t)tfam) == 0) {
                                return 9;   /* part -> PPMd batch */
                            }
                        }
                    }
                }
                /* Cheap reject before the expensive read. vol_read_file
                   DECODES, so on an already-swept volume the old order paid
                   a full packMP3/cjxl/APE decode per file just to conclude
                   "nothing to do". Anything not in RAW has been swept; the
                   one caller that still needs the decoded bytes
                   (vol_sweep_file) re-checks the zone itself. (Legacy rule
                   for files with no class flag.) */
                return 0;
            }
        }
    }

    if (vol_read_file(v, inode_id, &full, &full_len) != 0 || full_len < 4) {
        free(full);
        return 0;
    }

    /* WP59a: anchored files must never be claimed by a pack/container codec.
     * If the file reached the full path without a class stamp (first sweep
     * after creation), skip every codec/container branch and go straight to
     * the generic floor (builtin LZ4/ZSTD).  The class stamp set by the
     * creation path prevents re-entry on subsequent sweeps. */
    if (invfs_inode_is_anchored(v, inode_id)) {
        vol_stamp_class(v, inode_id, INVFS_CLASS_ANCHORED, 0, 0);
        goto generic_floor;
    }

    /* WP19: a write-hot file (rewritten at least twice inside the last
     * sweep interval -- wheat survives rewrites, see vol_replace_file)
     * churns too fast to amortize containers, transcodes or batching:
     * skip the heavy fan-out for this run and take the generic floor.
     * The volume-wide summary keeps cold volumes from paying the scan. */
    if (v->heat_any_whot && heat_file_maxw(v, inode_id) >= INVFS_WHEAT_HOT) {
        free(full);
        full = NULL;
        goto generic_floor;
    }

    /* ZIP container: explode into AST children (keep original bytes) */
    if (full[0] == 'P' && full[1] == 'K' &&
        ((full[2] == 3 && full[3] == 4) || (full[2] == 5 && full[3] == 6))) {
        invfs_ast_child_entry *ch =
            (invfs_ast_child_entry *)calloc(MAX_AST_CHILDREN, sizeof(*ch));
        if (ch) {
            int n = vol_zip_parse_children(full, full_len, ch, MAX_AST_CHILDREN);
            if (n > 0) {
                uint64_t nino = vol_create_container_file(v, name, full, full_len,
                                                          ch, (size_t)n);
                if (nino) {
                    vol_delete_inode(v, inode_id, name);
                    vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                                    INVFS_ALGO_ZIPR, tz_codec_gen(INVFS_ALGO_ZIPR));
                    free(ch); free(full);
                    return 1;
                }
                /* recognized but refused: retry only on a new generation */
                vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                                INVFS_ALGO_ZIPR, tz_codec_gen(INVFS_ALGO_ZIPR));
            }
            free(ch);
        }
        free(full);
        return 0;
    }

    /* FLAC -> APE(PCM) + frame recipe (bit-exact) */
    if (full_len >= 4 && memcmp(full, "fLaC", 4) == 0) {
        snprintf(rname, sizeof rname, "%s!recipe", name);
        if (vol_find(v, rname) != 0) { free(full); return 0; }
        uint64_t nino = vol_create_flac_file(v, name, full, full_len);
        free(full);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_FLACR, tz_codec_gen(INVFS_ALGO_FLACR));
            return 0;
        }
        if (vol_delete_inode(v, inode_id, name) != 0) return -1;
        vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                        INVFS_ALGO_FLACR, tz_codec_gen(INVFS_ALGO_FLACR));
        return 2;   /* FLAC */
    }

    /* TAR -> members + IVFT recipe (bit-exact) */
    if (full_len >= 512 && full[0] != 0 && full[0] != 1 &&
        memcmp(full + 257, "ustar", 5) == 0) {
        snprintf(p0name, sizeof p0name, "%s!part0", name);
        if (vol_find(v, p0name) != 0) { free(full); return 0; }
        uint64_t nino = vol_create_tar_file(v, name, full, full_len);
        free(full);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_TARR, tz_codec_gen(INVFS_ALGO_TARR));
            return 0;
        }
        if (vol_delete_inode(v, inode_id, name) != 0) return -1;
        vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                        INVFS_ALGO_TARR, tz_codec_gen(INVFS_ALGO_TARR));
        defer_container_parts(v, name);   /* WP14b: batch parts this run */
        return 3;   /* TAR */
    }

    /* GZIP (tar.gz) -> members + IVGZ recipe (bit-exact deflate) */
    if (full_len >= 18 && full[0] != 0 && full[0] != 1 &&
        full[0] == 0x1F && full[1] == 0x8B) {
        snprintf(p0name, sizeof p0name, "%s!part0", name);
        if (vol_find(v, p0name) != 0) { free(full); return 0; }
        uint64_t nino = vol_create_gz_file(v, name, full, full_len);
        free(full);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_GZR, tz_codec_gen(INVFS_ALGO_GZR));
            return 0;
        }
        if (vol_delete_inode(v, inode_id, name) != 0) return -1;
        vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                        INVFS_ALGO_GZR, tz_codec_gen(INVFS_ALGO_GZR));
        defer_container_parts(v, name);   /* WP14b: batch parts this run */
        return 4;   /* GZIP */
    }

    /* PNG -> JXL lossless + IVPN recipe (bit-exact) */
    if (full_len >= 33 && memcmp(full, "\x89PNG\r\n\x1a\n", 8) == 0) {
        snprintf(jn, sizeof jn, "%s!jxl", name);
        if (vol_find(v, jn) != 0) { free(full); return 0; }
        /* WP10 §12.2: admission needs the DECODE working set, derivable from
         * IHDR without a trial decode: raw pixels ~= h * (1 + ceil(w*ch*bd/8))
         * (unfiltered rows + per-row filter byte). Beyond the limit the file
         * stays admissible to generic ZSTD but never to PNGR. */
        {
            uint32_t w = ((uint32_t)full[16] << 24) | ((uint32_t)full[17] << 16) |
                         ((uint32_t)full[18] << 8) | (uint32_t)full[19];
            uint32_t h = ((uint32_t)full[20] << 24) | ((uint32_t)full[21] << 16) |
                         ((uint32_t)full[22] << 8) | (uint32_t)full[23];
            unsigned bd = full[24], ct = full[25];
            static const uint8_t chans[7] = { 1, 0, 3, 1, 2, 0, 4 };
            unsigned ch = ct < sizeof chans ? chans[ct] : 0;
            if (w && h && ch) {
                uint64_t raw = (uint64_t)h *
                               (1 + (((uint64_t)w * ch * bd) + 7) / 8);
                if (raw > vol_get_dec_mem_limit(v)) {
                    vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                                    INVFS_ALGO_PNGR, tz_codec_gen(INVFS_ALGO_PNGR));
                    free(full);
                    return 0;
                }
            }
        }
        uint64_t nino = vol_create_png_file(v, name, full, full_len);
        free(full);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_PNGR, tz_codec_gen(INVFS_ALGO_PNGR));
            return 0;
        }
        if (vol_delete_inode(v, inode_id, name) != 0) return -1;
        vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                        INVFS_ALGO_PNGR, tz_codec_gen(INVFS_ALGO_PNGR));
        return 5;   /* PNG */
    }

    /* MP3 -> PMP (packMP3, bit-exact). Matches an ID3v2 tag or a bare
       frame sync; packMP3 re-checks the content itself and refuses
       MPEG-2/2.5 Layer III, which we detect by a missing blob rather
       than by exit code (it exits 0 on refusal). */
    if (full_len >= 4 &&
        ((full[0] == 'I' && full[1] == 'D' && full[2] == '3') ||
         (full[0] == 0xFF && (full[1] & 0xE0) == 0xE0))) {
        uint8_t *pmp = NULL;
        size_t pmp_len = 0;
        int prc = invfs_pmp_compress(full, full_len, &pmp, &pmp_len);
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[sweep] %s: PMP rc=%d pmp_len=%zu (mp3 %zu)\n",
                    name, prc, pmp_len, full_len);
        /* only if it pays: a refused MPEG-2 file leaves pmp NULL, and a
           rare expansion must not cost space either. Either way we fall
           through and the generic ZSTD-19 path still gets the file. */
        if (prc == 0 && pmp_len > 0 && pmp_len < full_len) {
            uint64_t nino = vol_create_pmp_file(v, name, pmp, pmp_len,
                                                (uint64_t)full_len);
            free(pmp); free(full);
            if (!nino) return 0;
            if (vol_delete_inode(v, inode_id, name) != 0) return -1;
            vol_stamp_class(v, nino, INVFS_CLASS_CODEC,
                            INVFS_ALGO_PMP, tz_codec_gen(INVFS_ALGO_PMP));
            return 8;   /* MP3 */
        }
        free(pmp);
        /* Refused (MPEG-2/2.5, or a rare expansion). The generic path below
         * still runs; the GUARD stamp survives it (inner re-reads the record
         * and its gain-stamp yields to GUARD), so a new packMP3 generation
         * is what re-arms this file. */
        vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                        INVFS_ALGO_PMP, tz_codec_gen(INVFS_ALGO_PMP));
    }

    /* WP16a: container codecpacks (manifest type=container) -- decompose a
     * container into "!mbrNNNN" member inodes that flow through the whole
     * normal pipeline. Placed AFTER every builtin container magic above
     * (ZIP/TAR/GZ/PNG/FLAC, and the MP3 codec branch) and BEFORE the WP13
     * whole-file codec-pack loop / text / generic. Every sniff-positive pack
     * gets its chance in registry order: a decline tries the NEXT pack (weak
     * magics overlap — e.g. rawdisk vs fatfs both sniff 55AA@510); a defer
     * (tools absent / DEFER_ENOSPC) is remembered and wins over declines. */
    {
        size_t cn = 0, ci;
        const invfs_codec *all = invfs_codec_all(&cn);
        int deferred = 0;
        for (ci = 0; ci < cn; ci++) {
            const invfs_codec *pc = &all[ci];
            const invfs_pack_def *pd;
            int prc;
            if (!pc->sniff || !(pc->caps & INVFS_CODEC_CAP_CONTAINER))
                continue;
            pd = invfs_codec_pack_def(pc);
            if (!pd || !pd->is_container)
                continue;
            if (pc->sniff(full, full_len, name) <= 0)
                continue;
            prc = vol_containerpack_sweep(v, inode_id, name, pc, full,
                                          full_len);
            if (getenv("INVFS_DEBUG_PACKS"))
                fprintf(stderr, "[packdbg] %s on %s -> prc=%d\n",
                        pc->name, name, prc);
            if (prc >= 100) { free(full); return prc; }   /* decomposed */
            if (prc == 1) { deferred = 1; continue; }     /* defer: try next */
            /* declined: stamps carry the retry semantics; try next pack */
        }
        if (deferred) { free(full); return 0; }   /* a later sweep may claim */
    }

    /* WP13: codecpack codecs — the registry's dynamic EXTERNAL entries, the
     * only ones carrying encode/decode trampolines (builtin externals have
     * NULL fn pointers and their own branches above). First sniff hit wins;
     * a declined pack stamps and falls through to text/generic.
     * WP16e: a builtin EXTERNAL entry whose transcode was retired to a pack
     * (CAP_PACKONLY, today: JXL) and which no loaded pack has overridden
     * still OWNS its content's sniff -- but builtins sit BEFORE the packs
     * in registry order, so the hit is only remembered here and the defer
     * fires when NO later pack claimed the content: the file waits RAW and
     * unstamped rather than falling to the generic floor, whose stamp would
     * be terminal (the class predicate never re-arms plain GENERIC for a
     * codec). The first sweep with the pack installed picks it up; this is
     * the same "wait RAW" semantics the tool-absent case has below. */
    {
        size_t cn = 0, ci;
        const invfs_codec *all = invfs_codec_all(&cn);
        int packless_claim = 0;
        for (ci = 0; ci < cn; ci++) {
            const invfs_codec *pc = &all[ci];
            int prc;
            if (!(pc->caps & INVFS_CODEC_CAP_EXTERNAL) || !pc->sniff)
                continue;
            if (pc->sniff(full, full_len, name) <= 0)
                continue;
            if (!pc->encode || !pc->decode) {
                if (pc->caps & INVFS_CODEC_CAP_PACKONLY)
                    packless_claim = 1;   /* placeholder: a later pack may
                                           * still claim -- decide below */
                continue;       /* other placeholders keep builtin branches */
            }
            prc = vol_pack_sweep(v, inode_id, name, pc, full, full_len);
            if (prc == 1) { free(full); return 0; }   /* tool absent: defer */
            if (prc >= 100) { free(full); return prc; }   /* transcoded */
            packless_claim = 0;   /* a real pack tried: its stamps rule */
            break;   /* declined: stamps applied; text/generic still run */
        }
        if (packless_claim) { free(full); return 0; }   /* wait for the pack */
    }

    /* WP10 §4: every magic dispatch above declined -- classify text. Text
     * DEFERS into the sweep-run accumulator (sealed into shared PPMd batches
     * by vol_tz_flush at the end of the run); "!" sibling parts stay with
     * their container in v1 (WP10 §12.7). */
    if (!strchr(name, '!')) {
        int fam = invfs_text_family(name, full, full_len);
        if (fam > 0) {
            const invfs_codec *pc = invfs_codec_by_algo(INVFS_ALGO_PPMD);
            if (pc && pc->dec_mem_bytes > vol_get_dec_mem_limit(v)) {
                /* PPMd's model exceeds the decode-memory policy: store
                 * generic (below), stamped so a raised limit re-tries the
                 * text path without waiting for a generation bump. */
                vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                                INVFS_ALGO_PPMD, pc->generation);
            } else if (tz_defer(v, inode_id, name, full_len,
                                (uint32_t)fam) == 0) {
                free(full);
                return 9;   /* text -> PPMd batch (deferred to flush) */
            }
        }
    }

    /* WP14b M2: exe-as-container carving, BEFORE the binary-batch
     * deferral -- a carved exe is strictly better than a batched one (the
     * embedded media gets a real codec, the glue still gets ZSTD-19).
     * '!'-sibling parts are never carved (WP10 §12.7). A MEMLIMIT refusal
     * (rc 2) skips batching too: the retry must re-arm from generic
     * storage, not from inside a batch. */
    int exer_no_bz = 0;
    if (!strchr(name, '!') &&
        invfs_binary_family(full, full_len, name) > 0) {
        uint32_t nparts = 0;
        int erc = vol_exer_carve(v, inode_id, name, full, full_len, &nparts);
        if (erc == 1) {
            v->last_exer_parts = nparts;   /* invf-sweep reports the count */
            free(full);
            return 11;   /* exe media -> JXL container */
        }
        exer_no_bz = (erc == 2);
    }

    /* WP14a: not text either -- executable binaries (ELF/PE/Mach-O by
     * magic, >= 4 KB) defer into the BINARY accumulator and are sealed
     * into shared ZSTD batches (x86 members BCJ-prefiltered first) by the
     * same vol_tz_flush. No dec_mem gate on purpose: the batch payload
     * codec is ZSTD, the generic floor itself -- a policy tight enough to
     * reject it would reject the floor it falls back to, and the batch
     * unit is already bounded by arc_budget/2 at seal time. Same "!"
     * sibling exclusion as text. */
    if (!strchr(name, '!') && !exer_no_bz) {
        int bfam = invfs_binary_family(full, full_len, name);
        if (bfam > 0 &&
            bz_defer(v, inode_id, name, full_len, (uint32_t)bfam) == 0) {
            free(full);
            return 10;   /* binary -> ZSTD batch (deferred to flush) */
        }
    }

generic_floor:
    free(full);

    /* generic: recompress RAW -> Shadow (profile codec). (WP16e: JPEG -> JXL
     * no longer lands here -- the jxl codecpack's WP13 branch above owns it
     * and reports 100+algo directly.) */
    {
        int rc = vol_sweep_file(v, inode_id);
        if (rc == 0) return 6;      /* swept to Shadow */
        if (rc < 0) return -1;      /* hard error */
        return 0;                   /* already in Shadow: nothing done */
    }
}


/* WP22e --fast: the sweep narrowed to "generic or nothing" -- only the
 * per-segment profile-level recompress of a RAW file (the generic floor).
 * The class predicate, container decomposition, codec transcodes and the
 * batching accumulators never run; a file already past RAW (batched,
 * container, generic, codec) is reported as "nothing to do". The record
 * (with its INO2 ext) is copied verbatim by the inner path, so no metadata
 * carry is needed -- unlike the transcode branches vol_sweep_file exists
 * for. */
int vol_sweep_file_generic(invfs_volume *v, uint64_t inode_id)
{
    if (!v || !inode_id) return -1;
    if (v->sb.vol_flags & VOLF_V3)
        return vol_sweep_file(v, inode_id);
    return vol_sweep_file_inner(v, inode_id, 1);
}


/* Resolve the current name of an inode id for a sweep driver that has
 * only the id (vol_sweep_one wants the name: class policy, pack sniffing
 * and the live-session guard all key on it). 1 = found, 0 = deleted or
 * unreadable. Same scan the pending drain has always used. */
int vol_sweep_name_of(invfs_volume *v, uint64_t id, char *nm, size_t cap)
{
    if (!v || !nm || cap == 0) return 0;

    /* WP-M18: v3 volumes use the dirent tree (delta-first) */
    if (v->sb.vol_flags & VOLF_V3) {
        uint64_t parent;
        int rc = vol_v3_name_of(v, id, nm, cap, &parent);
        return (rc > 0) ? 1 : 0;
    }

    /* v2: shared mapper-aware walker */
    {
        sweep_locate_ctx lc;
        memset(&lc, 0, sizeof lc);
        lc.want = id;
        vol_records_walk(v, sweep_locate_cb, &lc);
        if (!lc.found) return 0;
        {
            size_t nl = strlen(lc.name);
            if (nl >= cap) return 0;
            memcpy(nm, lc.name, nl + 1);
        }
        return 1;
    }
}


/* drain the pending list (daemon background): process each pending inode
   and unmark it. Called when the daemon holds the volume exclusively
   (no open handles). Returns number of processed inodes. */
int vol_sweep_pending(invfs_volume *v)
{
    size_t n = v->n_pending;
    int done = 0;
    /* WP21: a live checkpoint (CKP0) holds every sweep-time free in the
     * retention registry -- but only the offline invf-sweep runs
     * checkpoint-armed. The on-demand drain has no registry of its own,
     * so it defers the whole queue until the checkpoint is resolved
     * (invf-rollback / invf-sweep --realize): an unregistered free here
     * is exactly the block a rollback would have resurrected. */
    if (v->ck_present) return 0;
    vol_heat_sweep_begin(v);   /* WP19: one decay pass per sweep run */
    while (n > 0) {
        uint64_t id = v->pending[0];
        /* find the current name for this inode (may have been deleted) */
        char nm[256];
        int found = vol_sweep_name_of(v, id, nm, sizeof nm);
        vol_unmark_pending(v, id);
        if (found) {
            int rc = vol_sweep_one(v, id, nm);
            if (rc > 0) done++;
        }
        n = v->n_pending;   /* re-read (list may shrink) */
    }
    /* WP19: extract any batch member the reads since the last run made
     * hot, then seal the partial text batch the drain accumulated */
    vol_heat_promote(v);
    /* WP25 rule 9: the two-device tier migration follows decay+promotion
     * (self-guards: single-device / degraded / read-only -> no-op) */
    vol_tier_migrate(v);
    vol_tz_flush(v);
    /* WP-M21: extent shrink/merge run was retired (the mapper is pre-allocated
     * at mkfs; fold is the reclaim path now). */
    /* WP-M18: after sweep walk + meta merge, try to fold the delta and then
     * schedule reclaim. Both are idempotent stubs in this WP; M15 fills them. */
    if ((v->sb.vol_flags & VOLF_V3) && !(v->sb.vol_flags & VOLF_READONLY)) {
        (void)vol_v3_fold_request(v);
        (void)vol_reclaim_schedule(v);
    }
    return done;
}


/* WP13: one codecpack transcode attempt on a RAW file (packs register
 * whole-file EXTERNAL codecs; the manifest argv runs as a subprocess via
 * the codec.c trampolines + the exec hooks above). Mirrors the JXL branch
 * of vol_sweep_file_inner: probe -> admission (WP10 §12.2: the pack's
 * estimate command when it has one, else the manifest dec_mem constant) ->
 * encode -> decode-back memcmp guard (the 1:1 invariant) -> size guard ->
 * new blob inode first, retire the old one after, CODEC{algo, pack
 * generation} stamp.
 * Returns 100+algo on success, 1 when the pack's tools are unavailable
 * (defer: leave the file RAW and unstamped -- like a missing cjxl, the
 * first sweep after the tools appear picks it up), 0 when the pack
 * declined (GUARD/MEMLIMIT stamped; the caller falls through to
 * text/generic, and the stamp carries the retry semantics). */
static int vol_pack_sweep(invfs_volume *v, uint64_t inode_id, const char *name,
                          const invfs_codec *pc, const uint8_t *full,
                          size_t full_len)
{
    const invfs_pack_def *def;
    uint8_t *enc = NULL, *back = NULL;
    size_t enc_cap, enc_len = 0;
    uint64_t ws;

    if (!pc->probe || !pc->probe()) return 1;    /* tools absent: wait */
    def = invfs_codec_pack_def(pc);
    if (!def) return 0;

    /* WP10 §12.2 admission: decode working set from the pack's estimate
     * (header-derived, never a trial decode), else the manifest constant */
    ws = pc->dec_mem_bytes;
    if (def->estimate) {
        char dir[64], in[128];
        int ok = 0;
        if (tool_tmpdir(dir, sizeof dir) != 0) return 0;
        snprintf(in, sizeof in, "%s/in", dir);
        if (tool_write(in, full, full_len) == 0 &&
            invfs_codec_pack_estimate(pc, in, &ws) == 0)
            ok = 1;
        tool_rm(dir, "in");
        rmdir(dir);
        /* the pack could not size the job — for an estimate command that
         * parses the container header this IS the refusal (e.g. an
         * encapsulated DICOM): record it as a guard refusal so a newer
         * pack generation re-arms the retry (WP10 §2 table) */
        if (!ok) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            (uint8_t)pc->algo, pc->generation);
            return 0;
        }
    }
    if (ws && ws > vol_get_dec_mem_limit(v)) {
        vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                        (uint8_t)pc->algo, pc->generation);
        return 0;
    }

    /* WP16b DEFER_ENOSPC: the blob lands while the original still occupies
     * its RAW blocks, so price the worst case (the encode scratch bound:
     * the blob may exceed the input, the size guard below is what refuses
     * it) plus the flat margin. Under that, wait RAW instead of paying the
     * encode just to fail the create; every later sweep re-evaluates (the
     * class predicate's DEFER_ENOSPC case). Returns 1 -- the same "wait
     * RAW, unstamped elsewhere" the tools-absent case uses. */
    if (sweep_enospc(v, (uint64_t)full_len + full_len / 4 + 65536 +
                        INVFS_ENOSPC_MARGIN)) {
        vol_stamp_class(v, inode_id, INVFS_CLASS_DEFER_ENOSPC,
                        (uint8_t)pc->algo, pc->generation);
        return 1;
    }

    /* the blob may exceed the input (codec overhead); the size guard below
     * is what refuses it, but the trampoline needs room to produce it */
    enc_cap = full_len + full_len / 4 + 65536;
    enc = (uint8_t *)malloc(enc_cap);
    if (!enc) return 0;
    if (pc->encode(full, full_len, enc, enc_cap, &enc_len) == 0 &&
        enc_len < full_len) {
        /* guard: the blob must decode back to the exact original bytes
         * before anything is replaced */
        back = (uint8_t *)malloc(full_len ? full_len : 1);
        if (back &&
            pc->decode(enc, enc_len, back, full_len) == 0 &&
            memcmp(back, full, full_len) == 0) {
            invfs_meta_pub keep;
            int have_keep = vol_get_meta(v, inode_id, &keep) == 0;
            uint64_t newino;
            if (v->sb.vol_flags & VOLF_V3) {
                newino = vol_v3_publish_blob_inode(v, inode_id, enc, enc_len,
                                                   full_len, pc->algo);
            } else {
                newino = vol_create_blob_file(v, name, enc, enc_len,
                                               full_len, pc->algo);
                if (newino)
                    vol_delete_inode(v, inode_id, name);
                if (have_keep) {
                    invfs_meta_pub chk;
                    if (vol_get_meta(v, newino, &chk) != 0)
                        vol_apply_meta(v, name, &keep);
                }
            }
            if (newino) {
                vol_stamp_class(v, newino, INVFS_CLASS_CODEC,
                                (uint8_t)pc->algo, pc->generation);
                free(back);
                free(enc);
                return 100 + (int)pc->algo;
            }
            /* no space for the blob: NOT a guard refusal (vol_jxl_retry's
             * convention) -- leave unstamped so a retry re-arms */
            fprintf(stderr, "sweep: %s create failed (%s)\n", pc->name, name);
            free(back);
            free(enc);
            return 0;
        }
        free(back);
    }
    free(enc);
    /* declined: tool failed, no gain, or the round-trip guard refused --
     * the file goes generic below and retries when the pack's generation
     * improves (WP10 §2 table) */
    vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                    (uint8_t)pc->algo, pc->generation);
    return 0;
}



/* ---- manual live sweep support --------------------------------------
 * Collect inode ids of every live regular file with data (last record
 * per name wins), for a caller-driven sweep pass with its own locking
 * and progress reporting. CRC-validated scan, same rules as open. */
typedef struct { char name[256]; uint64_t id; } sweep_seed;

/* WP42: per-record body of the sweepable collector, fed by the shared
 * mapper-aware vol_records_walk(). Last record per name wins, exactly the
 * legacy contiguous scan's rule; torn records are already skipped by the
 * walker. An OOM aborts the walk and keeps what was collected so far. */
typedef struct {
    sweep_seed *seen;
    size_t seen_n, seen_cap;
} sweep_seen_ctx;

static int sweep_seen_cb(void *ctx_, uint64_t rec_pos,
                         const invfs_inode_rec *h, const uint8_t *rec)
{
    sweep_seen_ctx *c = (sweep_seen_ctx *)ctx_;
    size_t nl, s;
    (void)rec_pos; (void)rec;
    if (h->magic != INODE_REC_MAGIC) return 0;   /* tombstone */
    if (h->file_size == 0) return 0;             /* nothing to move */
    if (h->name_len > INVFS_MAX_NAME ||
        h->rec_len < INVFS_REC_HDR_LEN + h->name_len + 1)
        return 0;
    nl = h->name_len;
    for (s = 0; s < c->seen_n; s++)
        if (strncmp(c->seen[s].name, h->name, sizeof(c->seen[s].name)) == 0) {
            c->seen[s].id = h->inode_id;   /* last record wins */
            return 0;
        }
    if (c->seen_n == c->seen_cap) {
        sweep_seed *ns;
        c->seen_cap = c->seen_cap ? c->seen_cap * 2 : 4096;
        ns = realloc(c->seen, c->seen_cap * sizeof(*ns));
        if (!ns) return 1;   /* OOM: stop, resolve what we have */
        c->seen = ns;
    }
    memset(c->seen[c->seen_n].name, 0, sizeof(c->seen[c->seen_n].name));
    memcpy(c->seen[c->seen_n].name, h->name, nl);
    c->seen[c->seen_n].id = h->inode_id;
    c->seen_n++;
    return 0;
}


/* WP-M21b: collector callback for the v3 live-set iterator. Internal
 * (\x01) rows and nameless rows are not sweep targets; the iterator has
 * already dropped deleted rows and de-duplicated nlink-shared inodes. */
struct v3_sweep_ids { uint64_t *ids; size_t max, n; };

static int collect_sweepables_v3_cb(invfs_volume *v, uint64_t id,
                                    const char *name, void *ctx)
{
    struct v3_sweep_ids *c = (struct v3_sweep_ids *)ctx;
    (void)v;
    if (!name || !name[0] || (unsigned char)name[0] == 0x01)
        return 0;
    if (c->n >= c->max)
        return 1;                       /* full: stop the iteration */
    c->ids[c->n++] = id;
    return 0;
}

size_t vol_collect_sweepables(invfs_volume *v, uint64_t *ids, size_t max)
{
    sweep_seen_ctx c;
    size_t out = 0, s;

    if (!v || !ids || max == 0) return 0;

    /* WP-M21b: on v3 the live set is the base inode tree + delta overlay
     * (M18 iterator) -- the v2 record walk below finds nothing there. */
    if (v->sb.vol_flags & VOLF_V3) {
        struct v3_sweep_ids vc;
        vc.ids = ids; vc.max = max; vc.n = 0;
        (void)vol_v3_iter_live_inodes(v, collect_sweepables_v3_cb, &vc);
        return vc.n;
    }

    memset(&c, 0, sizeof c);
    /* WP42: the walker hops every mapper extent on v0.3.0+ volumes (the
     * legacy loop saw only the empty contiguous area) and still bounds
     * itself to [inode_area_start, inode_area_pos) on legacy volumes */
    vol_records_walk(v, sweep_seen_cb, &c);
    /* resolve through the live index: tombstoned seeds drop out here */
    for (s = 0; s < c.seen_n && out < max; s++) {
        uint64_t id = vol_find(v, c.seen[s].name);
        if (id != 0) ids[out++] = id;
    }
    free(c.seen);
    return out;
}



uint64_t vol_zone_used_bytes(invfs_volume *v, uint64_t start_blk, uint64_t end_blk)
{
    uint64_t b, used = 0;
    if (!v || end_blk <= start_blk) return 0;
    for (b = start_blk; b < end_blk; b++)
        if (v->bitmap[b >> 3] & (1u << (b & 7))) used++;
    return used * INVFS_BLOCK_SIZE;
}


/* WP41: shared per-record analysis body for vol_compute_stats, driven by
 * the WP40 mapper-aware walker vol_records_walk() so statistics cover
 * v0.3.0+ volumes whose inode records live in dynamic meta extents (the
 * legacy contiguous walk saw zero records there). ctx carries the
 * claimed bitmap and the stats accumulator; rb INCLUDES the trailing
 * CRC and is owned by the walker (never freed here). */
typedef struct {
    invfs_volume *v;
    invfs_volume_stats *out;
    uint8_t *claimed;
    int have_ms;
} wstats_ctx;

static int wstats_cb(void *ctx_, uint64_t rec_pos,
                     const invfs_inode_rec *hp, const uint8_t *rb)
{
    wstats_ctx *ctx = (wstats_ctx *)ctx_;
    invfs_volume *v = ctx->v;
    invfs_volume_stats *out = ctx->out;
    uint8_t *claimed = ctx->claimed;
    int have_ms = ctx->have_ms;
    invfs_inode_rec h = *hp;
    const invfs_inode_rec *fr = (const invfs_inode_rec *)rb;

    if (h.magic == TOMBSTONE_MAGIC) { out->tombstones++; return 0; }
    if (h.name_len > INVFS_MAX_NAME ||
        h.rec_len < INVFS_REC_HDR_LEN + h.name_len + 1)
        return 0;
    /* count each file once, at its live version: the name resolves to
     * the current id and the id index points at the newest record.
     * Older same-id versions (meta rewrites, class stamps, batch-owner
     * growth) and replaced/deleted records would otherwise inflate every
     * counter below. The position check uses rec_pos -- this record's
     * own offset -- exactly what the legacy loop compared with
     * pos - rec_len - 4 after advancing. */
    {
        uint64_t ip = idx_get_id(v, h.inode_id);
        if (vol_find(v, fr->name) != h.inode_id || (ip && ip != rec_pos))
            return 0;
    }
    /* WP-DZ: physical attribution by content class, for EVERY live
     * record including the 0x01 internal owners (their blocks -- text
     * batches, seal parity, the retention registry -- are real used
     * bytes; the 0x01 exclusion below stays logical-only).
     * WP27: the extent comes from the entry's pba; the physical
     * length derives from the segment's framed header (owner classes
     * carry it in the entry: reten length = blocks, tier/rawm length
     * = bytes, parity = one block). */
    if (have_ms) {
        size_t pbase = (size_t)(invfs_rec_cbody(fr) - rb);
        invfs_ast_hdr pah;
        if (h.rec_len >= pbase + INVFS_AST_HDR_V1_LEN &&
            invfs_ast_hdr_parse(rb + pbase, h.rec_len - pbase, &pah) == 0 &&
            h.rec_len >= pbase + pah.hdr_len +
                          (size_t)pah.num_blocks *
                              sizeof(invfs_ast_block_entry)) {
            const uint8_t *ep = rb + pbase + pah.hdr_len;
            uint32_t pi;
            int is_ret = h.name_len && (uint8_t)fr->name[0] == 0x01 &&
                         h.name_len >= 6 &&
                         memcmp(fr->name + 1, "reten", 5) == 0;
            for (pi = 0; pi < pah.num_blocks; pi++) {
                const uint8_t *e = ep + (size_t)pi *
                                        sizeof(invfs_ast_block_entry);
                uint32_t zab, zone;
                uint64_t pba, plen = 0, b, bend;
                memcpy(&zab, e + 16, 4);   /* zone:2 | algo:6 | bid:24 */
                zone = zab & 3;
                memcpy(&pba, e + 24, 8);
                if (!pba || pba >= v->sb.total_blocks) continue;
                if (is_ret) {
                    uint64_t l;
                    memcpy(&l, e + 8, 8);
                    plen = l;              /* reten: BLOCKS */
                } else if (h.name_len && (uint8_t)fr->name[0] == 0x01) {
                    /* seal parity: one block; tier/rawm: the copy's
                     * span in bytes; tzb: length is the DECODED size
                     * -- the batch's extent derives from its frame */
                    uint64_t l;
                    memcpy(&l, e + 8, 8);
                    if (!(h.name_len >= 4 &&
                          memcmp(fr->name + 1, "tzb", 3) == 0) &&
                        l && l % INVFS_BLOCK_SIZE == 0)
                        plen = l / INVFS_BLOCK_SIZE;   /* parity/tier/rawm */
                    else if (seg_extent(v, pba, NULL, &plen) != 0)
                        continue;                        /* tzb batch */
                } else if (seg_extent(v, pba, NULL, &plen) != 0)
                    continue;   /* torn header: attribute nothing */
                bend = pba + plen;
                if (bend > v->sb.total_blocks) bend = v->sb.total_blocks;
                for (b = pba; b < bend; b++) {
                    if (bit_get(claimed, b)) continue;
                    bit_set(claimed, b);
                    if (zone == INVFS_ZONE_TEXT)
                        out->text_used_bytes += INVFS_BLOCK_SIZE;
                    else if (zone == INVFS_ZONE_BINARY)
                        out->shadow_used_bytes += INVFS_BLOCK_SIZE;
                    else
                        out->raw_used_bytes += INVFS_BLOCK_SIZE;
                }
            }
        }
    }
    {
        invfs_meta_pub m;
        int type = (vol_get_meta(v, h.inode_id, &m) == 0) ? m.type : -1;
        switch (type) {
        case INVFS_ITYP_DIR:  out->dirs++; break;
        case INVFS_ITYP_LNK:  out->links++; break;
        case INVFS_ITYP_FIFO: case INVFS_ITYP_SOCK:
        case INVFS_ITYP_CHR:  case INVFS_ITYP_BLK: out->special++; break;
        default: {
            /* WP12(a)/WP52: internal owner records ("\x01tzb", "\x01rawm",
             * "\x01tier0", "\x01parityN", "\x01reten") are engine
             * bookkeeping, not regular files. They carry file_size = the
             * sum of their sealed batches / mirror spans, so counting the
             * owner as well as its members doubles the logical bytes (the
             * Silesia image showed 309 MiB logical vs a 202 MiB corpus).
             * With WP52's deferral re-enabled on mapper volumes these
             * owners are now visible to the mapper-aware walk, so the
             * exclusion must cover the file COUNT too -- otherwise the
             * population grows by the owner records across a sweep.
             * Physical used-bytes accounting is the class-based pass above
             * and DOES include the internal owners. */
            if (h.name_len && (uint8_t)fr->name[0] == 0x01)
                break;
            out->files++;
            /* attribute logical size across the AST's zones */
            {
                /* record layout: rec header | AST header (v1 16B /
                 * v2 24B -- WP22a; the parsed view carries the length)
                 * | entries | children | INO2 ext. (An earlier version
                 * of this loop added the whole AST blob length to the
                 * base and read past it.) */
                size_t base = (size_t)(invfs_rec_cbody(fr) - rb);
                invfs_ast_hdr ah;
                uint32_t nb = 0;
                size_t hl = 0;
                if (h.rec_len >= base + INVFS_AST_HDR_V1_LEN &&
                    h.file_size > 0 &&
                    invfs_ast_hdr_parse(rb + base, h.rec_len - base,
                                        &ah) == 0) {
                    hl = ah.hdr_len;
                    if (h.rec_len < base + hl + (size_t)ah.num_blocks *
                                              sizeof(invfs_ast_block_entry))
                        nb = 0;     /* truncated recipe: attribute nothing */
                    else
                        nb = ah.num_blocks;
                }
                {
                    uint64_t remain = h.file_size;
                    uint32_t i;
                    for (i = 0; i < nb && remain > 0; i++) {
                        const uint8_t *e = rb + base + hl +
                            (size_t)i * sizeof(invfs_ast_block_entry);
                        uint32_t zone = e[16] & 3;   /* zone:2 LSB */
                        uint64_t seg;
                        /* TEXT entries are arbitrary-length slices of a
                         * shared batch (not 64 KB segments): use the
                         * entry's own length */
                        if (zone == INVFS_ZONE_TEXT) {
                            memcpy(&seg, e + 8, 8);
                            if (seg > remain) seg = remain;
                        } else {
                            seg = remain > 65536 ? 65536 : remain;
                        }
                        remain -= seg;
                        if (zone == INVFS_ZONE_TEXT)
                            out->logic_text_bytes += seg;
                        else if (zone == INVFS_ZONE_BINARY)
                            out->logic_shadow_bytes += seg;
                        else
                            out->logic_raw_bytes += seg;
                    }
                }
            }
            if (h.file_size && vol_find(v, fr->name) == h.inode_id) {
                out->logical_bytes += h.file_size;
                if (h.file_size > out->biggest_size) {
                    out->biggest_size = h.file_size;
                    snprintf(out->biggest_name, sizeof(out->biggest_name),
                             "%s", fr->name);
                }
            }
        }
        }
    }
    return 0;
}

/* WP-M21b: per-inode body of the v3 stats pass -- type straight from the
 * row (invfs_v3_inode.type), logical bytes from the row size. Internal
 * \x01 owners are excluded from the file count (the v2 body's rule). */
static int wstats_v3_cb(invfs_volume *v, uint64_t inode_id,
                        const char *name, void *ctx_)
{
    invfs_volume_stats *out = (invfs_volume_stats *)ctx_;
    invfs_v3_inode in;

    if (vol_v3_inode_get(v, inode_id, &in) != 1)
        return 0;
    switch (in.type) {
    case INVFS_ITYP_DIR:
        out->dirs++;
        return 0;
    case INVFS_ITYP_LNK:
        out->links++;
        return 0;
    case INVFS_ITYP_FIFO: case INVFS_ITYP_SOCK:
    case INVFS_ITYP_CHR:  case INVFS_ITYP_BLK:
        out->special++;
        return 0;
    default:
        break;
    }
    if (name && (unsigned char)name[0] == 0x01)
        return 0;               /* engine bookkeeping, not a user file */
    out->files++;
    out->logical_bytes += in.size;
    return 0;
}

int vol_compute_stats(invfs_volume *v, invfs_volume_stats *out)
{
    /* WP-DZ: physical per-zone attribution is by CONTENT CLASS (the AST
     * entry's zone tag over its own pba), never by pba region -- zone
     * boundaries are advisory, so raw-class blocks legitimately live in
     * the shadow extent. `claimed` (one bit per block) makes each physical
     * block count exactly once no matter how many live records reference
     * it (dedupe shares, the TEXT owner/member double references). */
    uint8_t *claimed = NULL;
    int have_ms = 0;
    if (!v || !out) return -1;
    memset(out, 0, sizeof(*out));
    claimed = (uint8_t *)calloc((size_t)(v->sb.total_blocks + 7) / 8, 1);
    if (claimed)
        have_ms = 1;   /* degrade: physical zeros, not a lie */

    /* WP-M21b: v3 -- population from the live-set iterator (type comes
     * from the inode row itself), physical from the zone bitmaps: the v3
     * data plane still allocates RAW segments into the RAW zone and
     * blob/drain segments into Shadow, so region attribution is exact.
     * The per-class physical breakdown and the per-zone logical split
     * would need a recipe parse per live inode -- reported as zeros
     * rather than guessed at (M18-remainder). */
    if (v->sb.vol_flags & VOLF_V3) {
        (void)vol_v3_iter_live_inodes(v, wstats_v3_cb, out);
        out->raw_used_bytes = vol_zone_used_bytes(v,
            v->sb.raw_zone_start,
            v->sb.raw_zone_start + v->sb.raw_zone_blocks);
        out->shadow_used_bytes = vol_zone_used_bytes(v,
            v->sb.shadow_zone_start,
            v->sb.shadow_zone_start + v->sb.shadow_zone_blocks);
        free(claimed);
        return 0;
    }

    {
        wstats_ctx c;
        c.v = v; c.out = out; c.claimed = claimed; c.have_ms = have_ms;
        if (v->met0_present && v->meta_mapper && v->met0.extent_count > 0) {
            /* WP41: mapper volume -- records live in dynamic meta
             * extents (MET0 + mapper table). vol_records_walk() hops
             * across every extent via vol_inode_next; the legacy
             * contiguous walk saw zero records here (66182 files on the
             * 15 GiB stage3 volume reported as 0). The walker
             * CRC-verifies each record and skips torn ones silently. */
            vol_records_walk(v, wstats_cb, &c);
        } else {
            /* legacy format_version=0: direct contiguous loop, kept
             * byte-for-byte so regressions in the shared branch cannot
             * leak into legacy handling. The per-record body is shared
             * with the walker callback above. */
            uint64_t pos, end;
            pos = v->inode_area_start * INVFS_BLOCK_SIZE;
            end = v->inode_area_pos;
            while (pos + INVFS_REC_HDR_LEN + 1 <= end) {
                invfs_inode_rec h;
                uint8_t *rb;
                uint32_t stored, calc;
                if (io_seek(&v->io, pos) != 0 ||
                    io_read(&v->io, &h, sizeof(h)) != 0) break;
                if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
                if (h.rec_len < INVFS_REC_HDR_LEN + 1 || h.rec_len > INVFS_MAX_REC_LEN ||
                    pos + h.rec_len + 4 > end) { out->bad_records++; break; }
                rb = malloc((size_t)h.rec_len + 4);
                if (!rb) break;
                if (io_seek(&v->io, pos) != 0 ||
                    io_read(&v->io, rb, (size_t)h.rec_len + 4) != 0) { free(rb); break; }
                memcpy(&stored, rb + h.rec_len, 4);
                calc = invfs_crc32c(rb, h.rec_len);
                if (calc != stored) { free(rb); out->bad_records++; pos += (uint64_t)h.rec_len + 4; continue; }
                pos += (uint64_t)h.rec_len + 4;
                (void)wstats_cb(&c, pos - ((uint64_t)h.rec_len + 4), &h, rb);
                free(rb);
            }
        }
    }
    /* unattributed data-region blocks: used in the bitmap but claimed by
     * no live record above (orphans awaiting fsck, crash debris). On a
     * two-device volume the dev1 reserved span (metadata mirror + the
     * RAW-width gap) is allocated by construction -- it is metadata, not
     * content, so it never counts as unclaimed. */
    if (have_ms) {
        uint64_t used = vol_zone_used_bytes(v, v->sb.raw_zone_start,
                                            v->sb.total_blocks);
        uint64_t cl = out->raw_used_bytes + out->shadow_used_bytes +
                      out->text_used_bytes;
        if (v->ndev == 2) {
            uint64_t span = (v->sb.shadow_zone_start - v->dev0_blocks) *
                            INVFS_BLOCK_SIZE;
            used = span < used ? used - span : 0;
        }
        out->unclaimed_used_bytes = used > cl ? used - cl : 0;
    }
    free(claimed);
    return 0;
}


/* hot population counters for the user.invfs.stats virtual xattr */
void vol_hot_counters(invfs_volume *v, uint64_t *files, uint64_t *dirs,
                      uint64_t *tombstones, uint64_t *logical_bytes)
{
    if (files)       *files = v->hot.files;
    if (dirs)        *dirs = v->hot.dirs;
    if (tombstones)  *tombstones = v->hot.tombstones;
    if (logical_bytes) *logical_bytes = v->hot.logical_bytes;
}
