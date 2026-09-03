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


/* Give back everything the atomic sweep path allocated under `new_id` before
   it gave up. Safe to call with new_id == 0 (the in-place path) or with no
   maps yet. Nothing has been made durable at this point -- no record names
   new_id, no flush has happened -- so this is pure in-memory bookkeeping plus
   bitmap bits, and the file is left exactly as it was found. */
static void sweep_unwind(invfs_volume *v, uint64_t new_id)
{
    size_t i, w = 0;
    if (!new_id) return;
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->inode == new_id) {
            uint64_t nblk = e->length;
            if (nblk && e->pba < v->sb.total_blocks &&
                nblk <= v->sb.total_blocks - e->pba)
                vol_free_blocks(v, e->pba, nblk);
        }
    }
    for (i = 0; i < v->l2p_count; i++) {
        if (v->l2p[i].inode == new_id) {
            /* WP22d: queue the UNMAP op; an earlier flush of this same
             * session may already have made the MAP durable, so the cancel
             * must reach the journal too (append-only, never rewritten) */
            if (v->l2p[i].type == INVFS_JRN_MAP) {
                invfs_l2p_entry ue;
                memset(&ue, 0, sizeof ue);
                ue.type = INVFS_JRN_UNMAP;
                ue.inode = v->l2p[i].inode;
                ue.lba = v->l2p[i].lba;
                jrn_push_op(v, &ue);
            }
            continue;
        }
        if (w != i) v->l2p[w] = v->l2p[i];
        w++;
    }
    v->l2p_count = w;
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


int vol_sweep_file(invfs_volume *v, uint64_t inode_id)
{
    invfs_meta_pub keep;
    int have_keep;
    char name[256] = "";
    int rc;

    have_keep = vol_get_meta(v, inode_id, &keep) == 0;
    {
        uint64_t pos = idx_get_id(v, inode_id);
        if (pos >= v->inode_area_start * INVFS_BLOCK_SIZE &&
            pos + sizeof(invfs_inode_rec) <= v->inode_area_pos) {
            invfs_inode_rec h;
            if (io_seek(&v->io, pos) == 0 &&
                io_read(&v->io, &h, sizeof h) == 0 &&
                h.magic == INODE_REC_MAGIC && h.inode_id == inode_id) {
                size_t nl = h.name_len < sizeof(name) - 1
                          ? h.name_len : sizeof(name) - 1;
                memcpy(name, h.name, nl);
                name[nl] = 0;
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
    uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    uint64_t end = v->inode_area_pos;
    uint64_t rec_pos = 0;
    invfs_inode_rec rec_h;
    uint8_t *rec = NULL;
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

    /* locate the inode record */
    {   /* jump straight to it; 0 = not indexed, keep the full scan */
        uint64_t ip = idx_get_id(v, inode_id);
        if (ip >= pos && ip + sizeof(invfs_inode_rec) <= end) pos = ip;
    }
    while (pos + sizeof(invfs_inode_rec) <= end) {
        if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &rec_h, sizeof(rec_h)) != 0)
            return -1;
        if (rec_h.magic != INODE_REC_MAGIC) {
            if (rec_h.magic == TOMBSTONE_MAGIC) {
                pos += rec_h.rec_len + 4;
                continue;
            }
            return -1;
        }
        if (rec_h.inode_id == inode_id) { rec_pos = pos; break; }
        pos += rec_h.rec_len + 4;
    }
    if (rec_pos == 0) return -1;

    rec = (uint8_t *)malloc(rec_h.rec_len);
    if (!rec) return -1;
    if (io_seek(&v->io, rec_pos) != 0 || io_read(&v->io, rec, rec_h.rec_len) != 0 ||
        io_read(&v->io, &crc_stored, 4) != 0) { free(rec); return -1; }
    crc_calc = invfs_crc32c(rec, rec_h.rec_len);
    if (crc_calc != crc_stored) { fprintf(stderr, "inode CRC mismatch\n"); free(rec); return -1; }

    if (invfs_ast_hdr_parse(rec + sizeof(invfs_inode_rec),
                            rec_h.rec_len - sizeof(invfs_inode_rec),
                            &ast_h) != 0 ||
        (size_t)ast_h.num_blocks * sizeof(invfs_ast_block_entry) >
            rec_h.rec_len - sizeof(invfs_inode_rec) - ast_h.hdr_len) {
        fprintf(stderr, "sweep: %s: unsupported/corrupt AST recipe header\n",
                rec_h.name);
        free(rec);
        return -1;
    }
    ents = (invfs_ast_block_entry *)(rec + sizeof(invfs_inode_rec) +
                                     ast_h.hdr_len);

    /* already swept (Shadow/BINARY) — nothing to do */
    if (ents[0].zone != INVFS_ZONE_RAW) {
        if (getenv("INVFS_DEBUG"))
            printf("[sweep] %s: already in Shadow (zone %u), skip\n",
                   rec_h.name, ents[0].zone);
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
                       rec_h.name, crc_res, ape_len, full_len);
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
                    uint64_t newino = vol_create_ape_file(v, rec_h.name, ape, ape_len, fsize);
                    if (newino == 0)
                        fprintf(stderr, "sweep: APE create failed (%s)\n", rec_h.name);
                    else {
                        vol_delete_inode(v, inode_id, rec_h.name);
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
                        "(pre-atomic volume)\n", rec_h.name);
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
        /* 1. read old segment */
        if (vol_lookup_entry(v, inode_id, e->block_id, &pba_old, &phys_len_old) != 0) {
            fprintf(stderr, "sweep: L2P miss seg %u\n", e->block_id);
            sweep_unwind(v, new_id); free(rec); return -1;
        }
        {
            uint8_t hdrb[8];
            if (io_seek(&v->io, pba_old * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, hdrb, 8) != 0) { sweep_unwind(v, new_id); free(rec); return -1; }
            memcpy(&hdr_old, hdrb, 4);
            memcpy(&crc_old, hdrb + 4, 4);
        }
        blob_old = (uint8_t *)malloc(hdr_old);
        if (!blob_old) { sweep_unwind(v, new_id); free(rec); return -1; }
        if (io_seek(&v->io, pba_old * INVFS_BLOCK_SIZE + 8) != 0 ||
            io_read(&v->io, blob_old, hdr_old) != 0) { free(blob_old); sweep_unwind(v, new_id); free(rec); return -1; }
        /* deep protection: verify segment CRC32C */
        if (crc_old != 0 && invfs_crc32c(blob_old, hdr_old) != crc_old) {
            fprintf(stderr, "sweep: segment CRC mismatch inode %llu seg %u\n",
                    (unsigned long long)inode_id, e->block_id);
            free(blob_old); sweep_unwind(v, new_id); free(rec); return -1;
        }

        /* 2. decompress to original */
        orig = (uint8_t *)malloc(orig_len ? orig_len : 1);
        if (!orig) { free(blob_old); sweep_unwind(v, new_id); free(rec); return -1; }
        if (e->algo == INVFS_ALGO_LZ4) {
            int got = LZ4_decompress_safe((const char *)blob_old, (char *)orig,
                                          (int)hdr_old, (int)orig_len);
            if (got != (int)orig_len) { free(blob_old); free(orig); sweep_unwind(v, new_id); free(rec); return -1; }
        } else if (e->algo == INVFS_ALGO_ZSTD) {
            /* WP23 (cross-lane touch, flagged for the WP22e lane): a RAW
             * segment written under fill pressure is ZSTD -- decode it
             * with the same per-segment dispatch the read paths already
             * had, or the sweep would mis-decode it as verbatim NONE. */
            size_t got = ZSTD_decompress(orig, orig_len, blob_old, hdr_old);
            if (ZSTD_isError(got) || got != orig_len) { free(blob_old); free(orig); sweep_unwind(v, new_id); free(rec); return -1; }
        } else {  /* NONE */
            if (hdr_old != orig_len) { free(blob_old); free(orig); sweep_unwind(v, new_id); free(rec); return -1; }
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
        if (!cbuf_new) { free(orig); sweep_unwind(v, new_id); free(rec); return -1; }
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
                               phys_blocks_new, 1);
        if (pba_new == 0) { free(cbuf_new); free(orig); sweep_unwind(v, new_id); free(rec); return -1; }
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
            free(cbuf_new); free(orig); sweep_unwind(v, new_id); free(rec); return -1;
        }
        free(cbuf_new);

        /* 5. L2P: map the new segment -- in memory only, same as vol_map.
         * The UNMAP record used to be written straight to the device here.
         * It was redundant twice over: vol_flush rebuilds the journal from
         * the in-memory table, which l2p_remove has already updated, and
         * that table holds only MAP entries, so a compacted journal never
         * contains UNMAP at all (replay applies them, flush never emits
         * them). It also landed on top of the terminator -- see vol_map.
         *
         * On the atomic path the new mapping goes under new_id and the old
         * inode keeps its own maps untouched, so the file stays fully
         * readable from its old record until the new one is durable. */
        if (new_id) {
            if (vol_map(v, new_id, e->block_id, pba_new, (uint32_t)phys_blocks_new) != 0) {
                free(orig); sweep_unwind(v, new_id); free(rec); return -1;
            }
        } else {
            l2p_remove(v, inode_id, e->block_id);
            if (vol_map(v, inode_id, e->block_id, pba_new, (uint32_t)phys_blocks_new) != 0) {
                free(orig); free(rec); return -1;
            }
            /* 6. free old RAW blocks (in-place path only; the atomic path
             * leaves them to vol_delete_inode of the old id, which is what
             * makes the old record readable until the new one lands).
             *
             * Free exactly what was allocated: the L2P entry's length is the
             * phys_blocks the write path asked alloc_blocks for. Recomputing
             * it from the payload used "hdr_old + 4" against a segment written
             * as csize + 8, so whenever those two straddled a block boundary
             * the last block was never returned to the bitmap -- allocated,
             * owned by no record, which is exactly what fsck calls an orphan.
             * A sweep of ~1900 segments leaked about one, matching the
             * ~4-in-4096 chance that the 4-byte discrepancy crosses a
             * boundary. */
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
        memcpy(name, rec_h.name, nlen);
        name[nlen] = 0;

        nh->inode_id = new_id;
        crc_calc = invfs_crc32c(rec, rec_h.rec_len);
        if (v->inode_area_pos + rec_h.rec_len + 4 +
            sizeof(invfs_inode_rec) + 4 > v->inode_area_end) {
            /* no room for the record AND the tombstone that must follow it */
            fprintf(stderr, "sweep: inode area full (%s)\n", name);
            sweep_unwind(v, new_id); free(rec); return -1;
        }
        if (vol_pre_record(v) != 0) { sweep_unwind(v, new_id); free(rec); return -1; }
        if (io_seek(&v->io, v->inode_area_pos) != 0 ||
            io_write(&v->io, rec, rec_h.rec_len) != 0 ||
            io_write(&v->io, &crc_calc, 4) != 0) {
            sweep_unwind(v, new_id); free(rec); return -1;
        }
        v->inode_area_pos += rec_h.rec_len + 4;
        idx_put(v, name, nlen, new_id, v->inode_area_pos - rec_h.rec_len - 4,
                nh->file_size, nh->ctime);
        idx_put_id(v, new_id, v->inode_area_pos - rec_h.rec_len - 4);
        /* frees the old RAW blocks: they are still mapped to the old id */
        if (vol_delete_inode(v, inode_id, name) != 0)
            fprintf(stderr, "sweep: %s transcoded but the old record survived; "
                            "invf-fsck -f will reclaim it\n", name);
    } else if (swept_any) {
        /* in-place, pre-atomic volumes only (see the note above the loop) */
        crc_calc = invfs_crc32c(rec, rec_h.rec_len);
        if (io_seek(&v->io, rec_pos) != 0 ||
            io_write(&v->io, rec, rec_h.rec_len) != 0 ||
            io_write(&v->io, &crc_calc, 4) != 0) { free(rec); return -1; }
    } else {
        sweep_unwind(v, new_id);   /* nothing swept: give the id's maps back */
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
    uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    uint64_t end = v->inode_area_pos;
    invfs_inode_rec rec_h;
    uint8_t hb[INVFS_AST_HDR_V2_LEN];
    invfs_ast_hdr ast_h;
    invfs_ast_block_entry e0;
    uint64_t ip = idx_get_id(v, inode_id);
    uint32_t ver;

    if (ip >= pos && ip + sizeof(invfs_inode_rec) <= end) pos = ip;
    while (pos + sizeof(invfs_inode_rec) <= end) {
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &rec_h, sizeof rec_h) != 0) return -1;
        if (rec_h.magic == TOMBSTONE_MAGIC) { pos += rec_h.rec_len + 4; continue; }
        if (rec_h.magic != INODE_REC_MAGIC) return -1;
        if (rec_h.inode_id == inode_id) break;
        pos += rec_h.rec_len + 4;
    }
    if (pos + sizeof(invfs_inode_rec) > end) return -1;
    /* recipe header length is version-dependent (16 B v1 / 24 B v2):
     * read the version word first, then the full header, then entry 0
     * behind it */
    if (io_seek(&v->io, pos + sizeof(invfs_inode_rec)) != 0 ||
        io_read(&v->io, &ver, 4) != 0) return -1;
    if (ver != INVFS_AST_VERSION_V1 && ver != INVFS_AST_VERSION_V2)
        return -1;
    {
        size_t need = ver == INVFS_AST_VERSION_V1 ? INVFS_AST_HDR_V1_LEN
                                                  : INVFS_AST_HDR_V2_LEN;
        if (io_seek(&v->io, pos + sizeof(invfs_inode_rec)) != 0 ||
            io_read(&v->io, hb, need) != 0)
            return -1;
        if (invfs_ast_hdr_parse(hb, need, &ast_h) != 0)
            return -1;
        if (ast_h.num_blocks == 0) return -1;
        if (io_seek(&v->io, pos + sizeof(invfs_inode_rec) + need) != 0 ||
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
    size_t base = sizeof(invfs_inode_rec);
    uint32_t i;
    int ok = 0;

    if (meta_read_record_by_id(v, inode_id, &buf, &rl, NULL, 0, NULL) != 0)
        return 0;
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


/* process a single inode: container explode / transcode / shadow move.
   Returns 1 if the inode was replaced/transcoded, 0 if not applicable,
   -1 on hard error (caller keeps it pending or aborts). */
int vol_sweep_one(invfs_volume *v, uint64_t inode_id, const char *name)
{
    char rname[272], p0name[272], jn[272];
    uint8_t *full = NULL;
    size_t full_len = 0;

    if (!name || strlen(name) > 240 || inode_id == 0) return 0;
    /* internal control names (the "\x01tzb" batch owner) are never swept */
    if ((uint8_t)name[0] == 0x01) return 0;
    /* WP4ab: a live write session has forked this file's segment layout
     * (old-record blocks aliased under the session's new id). Transcoding
     * it now would retire the old record mid-session. Skip; the commit
     * re-marks the file pending, so the next drain picks it up. */
    if (vol_write_active_name(v, name)) return 0;

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
    return vol_sweep_file_inner(v, inode_id, 1);
}


/* Resolve the current name of an inode id for a sweep driver that has
 * only the id (vol_sweep_one wants the name: class policy, pack sniffing
 * and the live-session guard all key on it). 1 = found, 0 = deleted or
 * unreadable. Same scan the pending drain has always used. */
int vol_sweep_name_of(invfs_volume *v, uint64_t id, char *nm, size_t cap)
{
    uint64_t pos, end;
    invfs_inode_rec rh;

    if (!v || !nm || cap == 0) return 0;
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    end = v->inode_area_pos;
    while (pos + sizeof(rh) <= end) {
        if (vol_read_raw(v, pos, &rh, sizeof(rh)) != 0) break;
        if (rh.magic != INODE_REC_MAGIC) {
            if (rh.magic == TOMBSTONE_MAGIC) { pos += rh.rec_len + 4; continue; }
            break;
        }
        if (rh.inode_id == id && rh.name_len < cap) {
            memcpy(nm, rh.name, rh.name_len);
            nm[rh.name_len] = 0;
            return 1;
        }
        pos += rh.rec_len + 4;
    }
    return 0;
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
            if (rc != 0) done++;
        }
        n = v->n_pending;   /* re-read (list may shrink) */
    }
    /* WP19: extract any batch member the reads since the last run made
     * hot, then seal the partial text batch the drain accumulated */
    vol_heat_promote(v);
    vol_tz_flush(v);
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
            uint64_t newino = vol_create_blob_file(v, name, enc, enc_len,
                                                   full_len, pc->algo);
            if (newino) {
                vol_delete_inode(v, inode_id, name);
                /* the fresh blob record has no ext; carry the old meta
                 * across, like the vol_jxl_retry flow does */
                if (have_keep) {
                    invfs_meta_pub chk;
                    if (vol_get_meta(v, newino, &chk) != 0)
                        vol_apply_meta(v, name, &keep);
                }
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


size_t vol_collect_sweepables(invfs_volume *v, uint64_t *ids, size_t max)
{
    sweep_seed *seen = NULL;
    size_t seen_n = 0, seen_cap = 0;
    size_t out = 0;
    uint64_t pos, end;
    size_t s;

    if (!v || !ids || max == 0) return 0;
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    end = v->inode_area_pos;
    while (pos + sizeof(invfs_inode_rec) <= end && out < max) {
        invfs_inode_rec h;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &h, sizeof(h)) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof(h) || h.rec_len > INVFS_MAX_REC_LEN ||
            pos + h.rec_len + 4 > end) break;
        {
            uint8_t *rb = (uint8_t *)malloc((size_t)h.rec_len + 4);
            uint32_t stored, calc;
            int bad = 0;
            if (!rb) break;
            if (io_seek(&v->io, pos) != 0 ||
                io_read(&v->io, rb, (size_t)h.rec_len + 4) != 0) { free(rb); break; }
            memcpy(&stored, rb + h.rec_len, 4);
            calc = invfs_crc32c(rb, h.rec_len);
            free(rb);
            if (calc != stored) bad = 1;   /* torn: skip, keep scanning */
            if (bad) { pos += (uint64_t)h.rec_len + 4; continue; }
        }
        pos += (uint64_t)h.rec_len + 4;
        if (h.magic != INODE_REC_MAGIC) continue;
        if (h.file_size == 0) continue;               /* nothing to move */
        {
            size_t nl = h.name_len < sizeof(h.name) ? h.name_len : sizeof(h.name)-1;
            int dup = 0;
            for (s = 0; s < seen_n; s++)
                if (strncmp(seen[s].name, h.name, sizeof(seen[s].name)) == 0)
                    { seen[s].id = h.inode_id; dup = 1; break; }
            if (dup) continue;
            if (seen_n == seen_cap) {
                sweep_seed *ns;
                seen_cap = seen_cap ? seen_cap*2 : 4096;
                ns = realloc(seen, seen_cap * sizeof(*seen));
                if (!ns) break;
                seen = ns;
            }
            memset(seen[seen_n].name, 0, sizeof(seen[seen_n].name));
            memcpy(seen[seen_n].name, h.name, nl);
            seen[seen_n].id = h.inode_id;
            seen_n++;
        }
    }
    /* resolve through the live index: tombstoned seeds drop out here */
    for (s = 0; s < seen_n && out < max; s++) {
        uint64_t id = vol_find(v, seen[s].name);
        if (id != 0) ids[out++] = id;
    }
    free(seen);
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


int vol_compute_stats(invfs_volume *v, invfs_volume_stats *out)
{
    uint64_t pos, end;
    if (!v || !out) return -1;
    memset(out, 0, sizeof(*out));
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    end = v->inode_area_pos;
    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec h;
        uint8_t *rb;
        uint32_t stored, calc;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &h, sizeof(h)) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof(h) || h.rec_len > INVFS_MAX_REC_LEN ||
            pos + h.rec_len + 4 > end) { out->bad_records++; break; }
        rb = malloc((size_t)h.rec_len + 4);
        if (!rb) break;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, rb, (size_t)h.rec_len + 4) != 0) { free(rb); break; }
        memcpy(&stored, rb + h.rec_len, 4);
        calc = invfs_crc32c(rb, h.rec_len);
        if (calc != stored) { free(rb); out->bad_records++; pos += (uint64_t)h.rec_len + 4; continue; }
        pos += (uint64_t)h.rec_len + 4;
        if (h.magic == TOMBSTONE_MAGIC) { free(rb); out->tombstones++; continue; }
        /* count each file once, at its live version: the name resolves to
         * the current id and the id index points at the newest record.
         * Older same-id versions (meta rewrites, class stamps, batch-owner
         * growth) and replaced/deleted records would otherwise inflate every
         * counter below. */
        {
            uint64_t ip = idx_get_id(v, h.inode_id);
            if (vol_find(v, h.name) != h.inode_id || (ip && ip != pos - ((uint64_t)h.rec_len + 4))) {
                free(rb);
                continue;
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
                out->files++;
                /* WP12(a): internal owner records ("\x01tzb") carry
                 * file_size = the sum of their sealed batches, and every
                 * TEXT member counts its own slices -- counting the owner
                 * too doubles the text-zone logical bytes (the Silesia
                 * image showed 309 MiB logical vs a 202 MiB corpus). The
                 * members carry the logical truth, so 0x01-prefixed
                 * internal names contribute nothing to ANY logical field
                 * (zone attribution, logical_bytes, biggest). Physical
                 * used-bytes accounting is bitmap-based and untouched. */
                if (h.name_len && (uint8_t)h.name[0] == 0x01)
                    break;
                /* attribute logical size across the AST's zones */
                {
                    /* record layout: rec header | AST header (v1 16B /
                     * v2 24B -- WP22a; the parsed view carries the length)
                     * | entries | children | INO2 ext. (An earlier version
                     * of this loop added the whole AST blob length to the
                     * base and read past it.) */
                    size_t base = sizeof(invfs_inode_rec);
                    invfs_ast_hdr ah;
                    uint32_t nb = 0;
                    size_t hl = 0;
                    if (h.rec_len >= base + INVFS_AST_HDR_V1_LEN &&
                        h.file_size > 0 &&
                        invfs_ast_hdr_parse(rb + base, h.rec_len - base,
                                            &ah) == 0) {
                        hl = ah.hdr_len;
                        if (h.rec_len < base + hl + (size_t)ah.num_blocks * 24)
                            nb = 0;     /* truncated recipe: attribute nothing */
                        else
                            nb = ah.num_blocks;
                    }
                    {
                        uint64_t remain = h.file_size;
                        uint32_t i;
                        for (i = 0; i < nb && remain > 0; i++) {
                            uint8_t *e = rb + base + hl + (size_t)i * 24;
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
                if (h.file_size && vol_find(v, h.name) == h.inode_id) {
                    out->logical_bytes += h.file_size;
                    if (h.file_size > out->biggest_size) {
                        out->biggest_size = h.file_size;
                        snprintf(out->biggest_name, sizeof(out->biggest_name),
                                 "%s", h.name);
                    }
                }
            }
            }
            free(rb);
        }
    }
    out->raw_used_bytes =
        vol_zone_used_bytes(v, v->sb.raw_zone_start, v->sb.shadow_zone_start);
    out->shadow_used_bytes =
        vol_zone_used_bytes(v, v->sb.shadow_zone_start, v->sb.total_blocks);
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
