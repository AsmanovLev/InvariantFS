/* vol_read.c — framed-segment read with seal recovery, recursive
 * inode/container read paths, ranged reads. Split from volume.c. */

#include "volume_internal.h"
#include "../codecs/deflate_repro.h"   /* the window transform inverse */


/* WP10 §5 / WP14a: is this AST entry a member slice of a shared batch?
 * zone=TEXT means "batched"; the payload codec is PPMD (text batches,
 * WP10) or ZSTD / ZSTD_BCJ (binary batches, WP14a). */
static int tz_batch_algo(uint32_t algo)
{
    return algo == INVFS_ALGO_PPMD || algo == INVFS_ALGO_ZSTD ||
           algo == INVFS_ALGO_ZSTD_BCJ;
}


/* ---- WP20: framed-segment read with seal recovery ------------------------
 * Every shadow-zone segment read funnels through here: [4B csize LE]
 * [4B crc32c(payload)] at pba, payload right behind the header, plen = the
 * segment's physical block count from the L2P (0 = unknown: bounds check
 * skipped, the historical behaviour). min_csize is the smallest legal
 * payload (a batch's [usize][props] head needs 6, a plain segment 1, an
 * empty recipe 0). A segment whose csize is out of bounds is corrupt by
 * construction (writers always allocate ceil((csize+8)/4096) blocks), so
 * the bounds check costs the happy path nothing.
 *
 * A shadow-zone segment that fails the bounds/CRC check gets ONE recovery
 * attempt from the WP20 seal parity (seal_recover_segment) before the
 * original failure propagates; the recovered payload is verified against
 * the segment's own framed CRC, so a failed parity reconstruction can
 * never surface as good bytes. RAW-zone segments are not sealed and fail
 * straight away. 0 = ok (*blob_out malloc'd, *csize_out bytes), -1 =
 * unreadable (parity could not help or absent). */
/* One framed-segment read attempt at pba (the pre-WP25 body of
 * seg_read_checked): verifies [4B csize LE][4B crc32c(payload)] against
 * plen and the payload CRC. *bad_out: 1 = the bytes failed bounds/CRC
 * (seal-eligible), 0 = a device-level io failure. */
static int seg_read_once(invfs_volume *v, uint64_t pba, uint64_t plen,
                         uint32_t min_csize, uint32_t *csize_out,
                         uint8_t **blob_out, int *bad_out)
{
    uint8_t hdrb[8];
    uint32_t csize;
    uint8_t *blob = NULL;
    int bad = 0;

    if (io_pread(&v->io, pba * INVFS_BLOCK_SIZE, hdrb, 8) != 0)
        { *bad_out = 0; return -1; }
    memcpy(&csize, hdrb, 4);
    if (csize < min_csize ||
        (plen && (uint64_t)csize + 8 > plen * INVFS_BLOCK_SIZE) ||
        (!plen && ((uint64_t)csize + 8 > (v->sb.total_blocks - pba) * (uint64_t)INVFS_BLOCK_SIZE))) {
        bad = 1;            /* header out of bounds: never a legit segment */
    } else {
        uint32_t crc_hdr;
        memcpy(&crc_hdr, hdrb + 4, 4);
        blob = (uint8_t *)malloc(csize ? (size_t)csize : 1);
        if (!blob) return -1;
        if (csize &&
            io_pread(&v->io, pba * INVFS_BLOCK_SIZE + 8, blob, csize) != 0) {
            bad = 1;
        } else if (crc_hdr != 0 && invfs_crc32c(blob, csize) != crc_hdr) {
            bad = 1;        /* deep protection: payload CRC32C mismatch */
        }
    }
    *bad_out = bad;
    if (bad) { free(blob); return -1; }
    *csize_out = csize;
    *blob_out = blob;
    return 0;
}

int seg_read_checked(invfs_volume *v, uint64_t pba, uint64_t plen,
                            uint32_t min_csize, uint32_t *csize_out,
                            uint8_t **blob_out)
{
    uint8_t *blob = NULL;
    int bad = 0;

    /* WP25: reads prefer dev0 -- a read-hot canonical segment keeps an
     * acceleration copy in the dev0 tier arena (vol_tier_migrate). The
     * copy is byte-identical, so the framed CRC proves it; any failure
     * falls back to the canonical dev1 segment (the copy is then stale
     * or dev0 is gone -- either way the canonical read is the truth). */
    if (v->ndev == 2 && v->dev0_present && !v->dev_skip[0] &&
        pba >= v->sb.shadow_zone_start) {
        uint64_t dpba = 0, dlen = 0;
        if (wp25_tier_lookup(v, pba, &dpba, &dlen) == 0) {
            if (seg_read_once(v, dpba, dlen, min_csize, csize_out,
                              blob_out, &bad) == 0)
                return 0;
            if (getenv("INVFS_DEBUG"))
                fprintf(stderr, "[tier] dev0 copy of pba %llu failed; "
                        "reading canonical\n", (unsigned long long)pba);
            /* fall through to the canonical read */
        }
    }
    if (seg_read_once(v, pba, plen, min_csize, csize_out, &blob,
                      &bad) == 0) {
        *blob_out = blob;
        return 0;
    }
    /* WP25: a raw-zone segment whose dev0 copy is unreadable (degraded
     * mount, io error, or a CRC-failed torn write) is served by its dev1
     * mirror -- same framed bytes, the CRC governs. */
    if (v->ndev == 2 &&
        pba >= v->sb.raw_zone_start &&
        pba < v->sb.raw_zone_start + v->sb.raw_zone_blocks) {
        uint64_t mpba = 0, mlen = 0;
        if (wp25_rawm_lookup(v, pba, &mpba, &mlen) == 0) {
            int mbad = 0;
            if (seg_read_once(v, mpba, mlen, min_csize, csize_out,
                              blob_out, &mbad) == 0)
                return 0;
        }
        return -1;   /* no mirror or mirror unreadable: fail loudly */
    }
    if (bad) {
        if (seal_recover_segment(v, pba, plen, csize_out, &blob) != 0)
            return -1;      /* original error stands */
        if (*csize_out < min_csize) { free(blob); return -1; }
        *blob_out = blob;
        return 0;
    }
    return -1;
}


/* WP10 §5 + WP14a: read a slice of a shared batch. A member AST entry
 * (zone=TEXT) names its batch by block_id (= the owner's batch_seq, the
 * WAL key) and carries the batch's pba directly (WP27); block_offset is
 * the slice's offset in the DECODED batch. The batch segment carries a
 * [4B usize LE] sub-header in front of the codec blob (PPMd: [2B props]
 * [stream]; binary: one zstd frame), usize = decoded batch size, under
 * the usual [4B csize][4B crc32c] framing. The decoded batch is cached in
 * the ARC keyed by the TAGGED pba (pba | TZ_ARC_TAG), not inode id: one
 * batch is shared by many member inodes, and the segment is freed only by
 * GC (which invalidates the tagged pba key first). Batches reach 4 MB, so
 * this is heap-only -- never the callers' stack segment buffer.
 *
 * WP14a BCJ: an algo==ZSTD_BCJ batch holds member slices that were each
 * x86-BCJ-prefiltered STANDALONE (pc=0, state=0) before concatenation. The
 * inverse is only bijective over exactly that member window, so a partial
 * read of such a slice first decodes the slice [block_offset, +length) as
 * a whole, inverts it, and only then serves the requested sub-window. The
 * cached batch stays in encoded form (the cache is shared with the other
 * members and must never be mutated). */
static int vol_read_text_slice(invfs_volume *v, uint64_t inode_id,
                               const invfs_ast_block_entry *e,
                               uint64_t slice_off, uint8_t *dst, size_t want)
{
    uint64_t pba = e->pba;
    const uint8_t *batch = NULL;
    uint8_t *fresh = NULL;
    size_t batch_len = 0;
    int rc = -1;

    if (want == 0) return 0;
    if (slice_off + want > e->length)
        return -1;   /* window runs past the member's slice */
    if (pba == 0 || pba >= v->sb.total_blocks) {
        fprintf(stderr, "pba invalid: inode %llu text seg %u\n",
                (unsigned long long)inode_id, e->block_id);
        return -1;
    }
    heat_touch_read(v, inode_id, e->block_id);   /* WP19: member heat */
    if (!arc_get(v->arc, pba | TZ_ARC_TAG, &batch, &batch_len)) {
        uint32_t csize, usize;
        uint8_t *blob;

        /* framed read, CRC-verified inside (payload holds at least
         * [4B usize][2B props]); on failure the WP20 seal parity gets one
         * recovery attempt before the error propagates. WP27: plen is not
         * stored in the entry -- 0 = derived from the segment header. */
        if (seg_read_checked(v, pba, 0, 4 + 2, &csize, &blob) != 0) {
            fprintf(stderr, "segment CRC mismatch: text batch pba %llu\n",
                    (unsigned long long)pba);
            return -1;
        }
        memcpy(&usize, blob, 4);
        if (usize == 0 || usize > (64u << 20)) {   /* batches are <= 4 MB */
            free(blob); return -1;
        }
        fresh = (uint8_t *)malloc(usize);
        if (!fresh) { free(blob); return -1; }
        /* the decoder's wire blob starts past the usize LE */
        if (e->algo == INVFS_ALGO_PPMD) {
            if (invfs_ppmd_decode(blob + 4, csize - 4, fresh, usize) != 0) {
                fprintf(stderr, "ppmd decode failed: text batch pba %llu\n",
                        (unsigned long long)pba);
                free(fresh); free(blob); return -1;
            }
        } else if (e->algo == INVFS_ALGO_ZSTD ||
                   e->algo == INVFS_ALGO_ZSTD_BCJ) {
            /* WP14a: one zstd stream for the whole batch (decode-whole is
             * fine at GB/s); the BCJ tag only names the prefilter, which
             * is undone per member slice below, not here */
            const invfs_codec *zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
            if (!zc || !zc->decode ||
                zc->decode(blob + 4, csize - 4, fresh, usize) != 0) {
                fprintf(stderr, "zstd decode failed: binary batch pba %llu\n",
                        (unsigned long long)pba);
                free(fresh); free(blob); return -1;
            }
        } else {
            fprintf(stderr, "text slice: unknown batch algo %u\n",
                    (unsigned)e->algo);
            free(fresh); free(blob); return -1;
        }
        free(blob);
        batch = fresh;
        batch_len = usize;
    }
    if (e->algo == INVFS_ALGO_ZSTD_BCJ) {
        /* invert the per-member prefilter: the window is exactly the
         * member slice, decoded whole, at pc=0 -- the same window the
         * encoder ran over (see the flush feed loop) */
        if ((uint64_t)e->block_offset + e->length <= batch_len) {
            if (slice_off == 0 && want == (size_t)e->length) {
                /* the common case (whole-slice read): no extra copy */
                memcpy(dst, batch + e->block_offset, want);
                invfs_bcj_x86_dec(dst, want);
                rc = 0;
            } else {
                uint8_t *sl = (uint8_t *)malloc(e->length ? e->length : 1);
                if (sl) {
                    memcpy(sl, batch + e->block_offset, e->length);
                    invfs_bcj_x86_dec(sl, e->length);
                    memcpy(dst, sl + slice_off, want);
                    free(sl);
                    rc = 0;
                }
            }
        }
    } else if ((uint64_t)e->block_offset + slice_off + want <= batch_len) {
        memcpy(dst, batch + e->block_offset + slice_off, want);
        rc = 0;
    }
    /* arc_put takes ownership (or frees an oversized entry), so the slice
     * is copied out BEFORE the buffer is handed over. NULL cache frees. */
    if (fresh) arc_put(v->arc, pba | TZ_ARC_TAG, fresh, batch_len);
    return rc;
}

/* WP47: is `ip` a plausible record position? On a v0.3.0+ mapper volume
 * idx_get_id returns an absolute offset inside a dynamic metadata extent,
 * which the legacy [inode_area_start, inode_area_pos) bound rejected. Accept
 * any position inside any mapper extent (mirroring vol_inode_next), or the
 * legacy contiguous region; anything else falls back to vol_records_walk(). */
static int read_hint_valid(invfs_volume *v, uint64_t ip)
{
    const uint64_t rs = INVFS_REC_HDR_LEN;
    if (!ip) return 0;
    if (v->met0_present && v->meta_mapper) {
        size_t ei;
        int ok = 0;
        pthread_rwlock_rdlock(&v->meta_lock);
        for (ei = 0; ei < (size_t)v->met0.extent_count; ei++) {
            uint64_t entry = meta_mapper_get(v, ei);
            uint64_t start, stop;
            if (!entry) break;
            start = invfs_meta_ext_pba(entry) * INVFS_BLOCK_SIZE;
            stop = start + invfs_meta_ext_size(entry);
            if ((int)ei == (int)v->met0.active_extent &&
                stop > v->inode_area_pos)
                stop = v->inode_area_pos;
            if (ip >= start && ip + rs <= stop) { ok = 1; break; }
        }
        pthread_rwlock_unlock(&v->meta_lock);
        return ok;
    }
    return ip >= v->inode_area_start * INVFS_BLOCK_SIZE &&
           ip + rs <= v->inode_area_pos;
}

typedef struct {
    uint64_t want;
    uint64_t pos;
    int found;
} read_locate_ctx;

static int read_locate_cb(void *ctx_, uint64_t rec_pos,
                          const invfs_inode_rec *h, const uint8_t *rec)
{
    read_locate_ctx *c = (read_locate_ctx *)ctx_;
    (void)rec;
    if (h->magic != INODE_REC_MAGIC || h->inode_id != c->want) return 0;
    c->pos = rec_pos;
    c->found = 1;
    return 1;   /* found: stop the walk */
}

/* Locate the live INOD for inode_id: the O(1) index hint when it is valid
 * and names a record for this id, else the mapper-aware vol_records_walk().
 * Returns the record position, or 0 when absent. */
static uint64_t read_locate_record(invfs_volume *v, uint64_t inode_id)
{
    uint64_t ip = idx_get_id(v, inode_id);
    if (read_hint_valid(v, ip)) {
        invfs_inode_rec h;
        if (vol_read_raw(v, ip, &h, sizeof h) == 0 &&
            h.magic == INODE_REC_MAGIC && h.inode_id == inode_id)
            return ip;
    }
    {
        read_locate_ctx lc;
        memset(&lc, 0, sizeof lc);
        lc.want = inode_id;
        vol_records_walk(v, read_locate_cb, &lc);
        return lc.found ? lc.pos : 0;
    }
}

/* WP-M8: decode a parsed AST recipe (header + entries) into a caller-
 * allocated `data` of ast_h->file_size bytes. Shared verbatim by the v2
 * record path and the v3 content-addressed recipe-blob path so both
 * reconstruct with exactly the same segment codecs (bit-exactness).
 * 0 = complete, -1 = corrupt/unreadable. `data` is caller-owned; on
 * error its contents are undefined and the caller frees it. */
typedef struct {
    invfs_volume *v;
    uint64_t inode_id;
    const invfs_ast_block_entry *ents;
    uint8_t *data;
    size_t start;
    size_t end;
    int err;
} decode_worker_arg;

static void *decode_thread_worker(void *arg_) {
    decode_worker_arg *a = (decode_worker_arg *)arg_;
    for (size_t i = a->start; i < a->end; i++) {
        const invfs_ast_block_entry *e = &a->ents[i];
        uint32_t hdr;
        uint8_t *blob;
        size_t dst_off = (size_t)e->file_offset;

        uint64_t pba = e->pba;
        if (pba == 0 || pba >= a->v->sb.total_blocks) {
            a->err = -1;
            return NULL;
        }

        if (seg_read_checked(a->v, pba, 0, 1, &hdr, &blob) != 0) {
            a->err = -1;
            return NULL;
        }

        if (e->algo == INVFS_ALGO_NONE) {
            memcpy(a->data + dst_off, blob, (size_t)e->length);
            free(blob);
        } else if (e->algo == INVFS_ALGO_LZ4) {
            int got = LZ4_decompress_safe((const char *)blob,
                                          (char *)(a->data + dst_off),
                                          (int)hdr, (int)e->length);
            free(blob);
            if (got != (int)e->length) { a->err = -1; return NULL; }
        } else if (e->algo == INVFS_ALGO_ZSTD) {
            size_t got = ZSTD_decompress(a->data + dst_off, e->length, blob, hdr);
            free(blob);
            if (ZSTD_isError(got) || got != e->length) { a->err = -1; return NULL; }
        } else {
            free(blob);
            a->err = -1;
            return NULL;
        }
    }
    return NULL;
}

/* ADR-010 amendment 2: `wins`/`n_wins` are the recipe's trailing window
 * table (NULL/0 for every recipe written before the change). Passing the
 * table in rather than re-parsing it per entry keeps the hot loop's cost at
 * one extra branch per entry. */
static int vol_decode_ast_entries(invfs_volume *v, uint64_t inode_id,
                                  const char *rec_name,
                                  const invfs_ast_hdr *ast_h,
                                  const invfs_ast_block_entry *ents,
                                  const invfs_ast_window_entry *wins,
                                  uint32_t n_wins,
                                  uint8_t *data)
{
    uint32_t i;

            for (i = 0; i < ast_h->num_blocks; i++) {
                const invfs_ast_block_entry *e = &ents[i];
                uint64_t end = e->file_offset + e->length;
                if (e->file_offset > ast_h->file_size ||
                    end > ast_h->file_size ||
                    end < e->file_offset) {
                    fprintf(stderr, "inode %llu seg %u: entry out of bounds "
                            "(off=%llu len=%llu size=%llu)\n",
                            (unsigned long long)inode_id, e->block_id,
                            (unsigned long long)e->file_offset,
                            (unsigned long long)e->length,
                            (unsigned long long)ast_h->file_size);
                    return -1;
                }
                if (i > 0 && e->file_offset < ents[i-1].file_offset +
                                          ents[i-1].length) {
                    fprintf(stderr, "inode %llu seg %u: overlapping entry\n",
                            (unsigned long long)inode_id, e->block_id);
                    return -1;
                }
            }

            /* Fast path: multi-threaded parallel segment decode for large multi-block files */
            uint32_t nblocks = ast_h->num_blocks;
            int has_complex_codecs = 0;
            for (i = 0; i < nblocks; i++) {
                if (ents[i].zone != INVFS_ZONE_RAW && ents[i].zone != INVFS_ZONE_BINARY) {
                    has_complex_codecs = 1;
                    break;
                }
                if (ents[i].algo != INVFS_ALGO_NONE &&
                    ents[i].algo != INVFS_ALGO_LZ4 &&
                    ents[i].algo != INVFS_ALGO_ZSTD) {
                    has_complex_codecs = 1;
                    break;
                }
            }

            if (!has_complex_codecs && nblocks >= 16) {
                int n_threads = 6;
                /* WP94: the serial loop below touches read heat once per
                 * entry; this fast path replaces it, so it owes the same
                 * touch. It is made ONCE here, from the calling thread,
                 * because heat is per-file (heat_tab_touch dedupes per
                 * inode, so the serial loop's N touches net to exactly one)
                 * and heat_tab_touch's table insert is not thread-safe --
                 * decode_thread_worker() must not call it. Skipping it left
                 * every read of a >=16-segment NONE/LZ4/ZSTD recipe at
                 * rheat=0, which starved heat promotion AND the WP25 tier
                 * migration (tier_heat_cb reads heat_file_r). */
                heat_touch_read(v, inode_id, 0);
                const char *et = getenv("INVFS_READ_THREADS");
                if (et && *et) {
                    int t = atoi(et);
                    if (t >= 1 && t <= 32) n_threads = t;
                }
                if ((uint32_t)n_threads > nblocks) n_threads = (int)nblocks;

                pthread_t th[32];
                decode_worker_arg args[32];
                size_t per_th = (nblocks + n_threads - 1) / n_threads;

                for (int t = 0; t < n_threads; t++) {
                    args[t].v = v;
                    args[t].inode_id = inode_id;
                    args[t].ents = ents;
                    args[t].data = data;
                    args[t].start = t * per_th;
                    args[t].end = (t + 1) * per_th;
                    if (args[t].end > nblocks) args[t].end = nblocks;
                    args[t].err = 0;
                    if (args[t].start < args[t].end)
                        pthread_create(&th[t], NULL, decode_thread_worker, &args[t]);
                    else
                        th[t] = 0;
                }
                int any_err = 0;
                for (int t = 0; t < n_threads; t++) {
                    if (th[t]) {
                        pthread_join(th[t], NULL);
                        if (args[t].err != 0) any_err = -1;
                    }
                }
                if (any_err == 0) return 0;
            }

            for (i = 0; i < ast_h->num_blocks; i++) {
                const invfs_ast_block_entry *e = &ents[i];
                uint64_t pba = 0;
                uint32_t hdr;
                uint8_t *blob;
                size_t dst_off = (size_t)e->file_offset;

                /* Batch member: a slice of a shared batch (PPMd text,
                 * ZSTD/ZSTD_BCJ binary), decoded and cached by pba (heap
                 * only -- see vol_read_text_slice) */
                if (e->zone == INVFS_ZONE_TEXT && tz_batch_algo(e->algo)) {
                    if (vol_read_text_slice(v, inode_id, e, 0,
                                            data + dst_off,
                                            (size_t)e->length) != 0) {
                        return -1;
                    }
                    continue;
                }

                /* ADR-010 amendment 2: a WINDOW_SRC entry is a reference, not
                 * a payload -- it owns no blocks, so it must be handled before
                 * the pba check below (its pba slot holds the SOURCE inode).
                 * Read the source range through its own read path, invert the
                 * transform, and copy the slice. A missing or unreadable
                 * source is EIO for the caller, never zeros. */
                if (e->algo == INVFS_ALGO_WINDOW_SRC) {
                    const invfs_ast_window_entry *w;
                    uint8_t *srcbuf = NULL, *outbuf = NULL;
                    size_t outlen = 0;
                    int wrc = -1;

                    if (!wins || e->block_id >= n_wins) {
                        fprintf(stderr, "inode %llu: window entry %u has no "
                                "descriptor\n", (unsigned long long)inode_id,
                                e->block_id);
                        return -1;
                    }
                    w = &wins[e->block_id];
                    {
                        invfs_v3_inode sin;
                        if (!e->pba || vol_v3_inode_get(v, e->pba, &sin) != 1) {
                            fprintf(stderr, "inode %llu: window source inode "
                                    "%llu is gone\n",
                                    (unsigned long long)inode_id,
                                    (unsigned long long)e->pba);
                            return -1;
                        }
                    }
                    srcbuf = (uint8_t *)malloc(w->src_len ? w->src_len : 1);
                    if (!srcbuf) return -1;
                    outlen = (size_t)vol_read_range(v, e->pba, w->src_off,
                                                    w->src_len, srcbuf);
                    if ((int64_t)outlen != (int64_t)w->src_len) {
                        fprintf(stderr, "inode %llu: window source read failed "
                                "(off=%llu len=%llu got=%zu)\n",
                                (unsigned long long)inode_id,
                                (unsigned long long)w->src_off,
                                (unsigned long long)w->src_len, outlen);
                        free(srcbuf);
                        return -1;
                    }
                    if (w->transform == 0) {
                        if (w->src_len < e->length) { free(srcbuf); return -1; }
                        memcpy(data + dst_off, srcbuf, (size_t)e->length);
                        free(srcbuf);
                        heat_touch_read(v, e->pba, e->block_id);
                        continue;
                    }
                    if (w->transform == 1) {
                        invfs_deflate_params dp;
                        memset(&dp, 0, sizeof dp);
                        dp.engine = w->engine;
                        dp.level = (int8_t)w->level;
                        dp.mem_level = (int8_t)w->mem_level;
                        dp.strategy = (int8_t)w->strategy;
                        dp.window_bits = w->window_bits;
                        if (invfs_deflate_decompress(srcbuf, w->src_len,
                                                     w->window_bits,
                                                     &outbuf, &outlen) == 0 &&
                            outbuf && outlen == e->length) {
                            memcpy(data + dst_off, outbuf, outlen);
                            wrc = 0;
                        }
                        free(outbuf);
                        free(srcbuf);
                        if (wrc != 0) {
                            fprintf(stderr, "inode %llu: window inflate failed "
                                    "(%llu -> %llu, want %llu)\n",
                                    (unsigned long long)inode_id,
                                    (unsigned long long)w->src_len,
                                    (unsigned long long)outlen,
                                    (unsigned long long)e->length);
                            return -1;
                        }
                        heat_touch_read(v, e->pba, e->block_id);
                        continue;
                    }
                    free(srcbuf);
                    fprintf(stderr, "inode %llu: window transform %u is "
                            "reserved, not implemented\n",
                            (unsigned long long)inode_id, w->transform);
                    return -1;
                }

                /* segment physical location: the entry's own pba (WP27);
                 * plen derives from the segment header (0 = unknown) */
                pba = e->pba;
                if (pba == 0 || pba >= v->sb.total_blocks) {
                    fprintf(stderr, "pba invalid: inode %llu seg %u\n",
                            (unsigned long long)inode_id, e->block_id);
                    return -1;
                }
                heat_touch_read(v, inode_id, e->block_id);   /* WP19 */
                /* framed segment [4B csize][4B crc32c][payload]: CRC-verified
                 * read; a shadow-zone failure gets one WP20 seal-parity
                 * recovery attempt inside before the error propagates */
                if (seg_read_checked(v, pba, 0, 1, &hdr, &blob) != 0) {
                    fprintf(stderr, "segment CRC mismatch: inode %llu seg %u (corrupt)\n",
                            (unsigned long long)inode_id, e->block_id);
                    return -1;
                }
                if (e->algo == INVFS_ALGO_NONE) {
                    if (hdr != e->length) {
                        fprintf(stderr, "raw segment header corrupt (csize %u, want %llu)\n",
                                hdr, (unsigned long long)e->length);
                        free(blob); return -1;
                    }
                } else if (e->algo == INVFS_ALGO_LZ4 || e->algo == INVFS_ALGO_ZSTD) {
                    /* compressed-in-place segments: csize < usize is required */
                    if (hdr >= e->length || hdr == 0) {
                        fprintf(stderr, "lz4 segment header corrupt (csize %u)\n", hdr);
                        free(blob); return -1;
                    }
                }
                /* whole-file blobs (JXL/APE/FLACR) keep csize unrelated to the
                   logical size: the transcoder may be smaller OR larger */
                if (e->algo == INVFS_ALGO_LZ4) {
                    int got = LZ4_decompress_safe((const char *)blob,
                                                  (char *)(data + dst_off),
                                                  (int)hdr, (int)e->length);
                    if (got != (int)e->length) {
                        fprintf(stderr, "LZ4 decompress error: got %d, want %llu\n",
                                got, (unsigned long long)e->length);
                        free(blob); return -1;
                    }
                } else if (e->algo == INVFS_ALGO_ZSTD) {
                    size_t got = ZSTD_decompress(data + dst_off, e->length, blob, hdr);
                    if (ZSTD_isError(got) || got != e->length) {
                        fprintf(stderr, "ZSTD decompress error: %s\n",
                                ZSTD_isError(got) ? ZSTD_getErrorName(got) : "size mismatch");
                        free(blob); return -1;
                    }
                } else if (e->algo == INVFS_ALGO_JXL) {
                    /* whole file is one JXL blob -> decode to jpeg.
                     * WP16e: the lane is pack-owned -- with the jxl
                     * codecpack loaded the registry entry for algo 4 IS the
                     * pack, so decode routes through the pack trampoline
                     * exactly like any other pack algo. With no pack loaded
                     * the builtin placeholder has no decode: the builtin
                     * djxl wrapper answers, so pre-migration blobs and
                     * EXER-carved JXL parts (which never needed a pack)
                     * stay readable. */
                    const invfs_codec *jc = invfs_codec_by_algo(INVFS_ALGO_JXL);
                    if (jc && jc->decode) {
                        if (jc->decode(blob, hdr, data + dst_off,
                                       (size_t)e->length) != 0) {
                            fprintf(stderr, "JXL pack decode error\n");
                            free(blob); return -1;
                        }
                    } else {
                        uint8_t *jpg = NULL;
                        size_t jpg_len = 0;
                        if (invfs_jxl_decompress(blob, hdr, &jpg, &jpg_len) != 0 ||
                            jpg_len != e->length) {
                            fprintf(stderr, "JXL decompress error\n");
                            free(blob); return -1;
                        }
                        memcpy(data + dst_off, jpg, jpg_len);
                        free(jpg);
                    }
                } else if (e->algo == INVFS_ALGO_APE) {
                    /* whole file is one APE blob -> decode to flac */
                    uint8_t *fl = NULL;
                    size_t fl_len = 0;
                    if (invfs_ape_decompress(blob, hdr, &fl, &fl_len) != 0) {
                        fprintf(stderr, "APE decompress error\n");
                        free(blob); return -1;
                    }
                    memcpy(data + dst_off, fl, fl_len < e->length ? fl_len : e->length);
                    free(fl);
                } else if (e->algo == INVFS_ALGO_PMP) {
                    /* whole file is one PMP blob -> decode back to mp3.
                       Length is checked, not clamped: packMP3 is bit-exact
                       or it is nothing, so a short/long result means the
                       blob is corrupt and returning partial audio would
                       break the 1:1 invariant silently. */
                    uint8_t *m = NULL;
                    size_t m_len = 0;
                    if (invfs_pmp_decompress(blob, hdr, &m, &m_len) != 0 ||
                        m_len != e->length) {
                        fprintf(stderr, "PMP decompress error (%s: got %zu, want %llu)\n",
                                rec_name, m_len, (unsigned long long)e->length);
                        free(m); free(blob); return -1;
                    }
                    memcpy(data + dst_off, m, m_len);
                    free(m);
                } else if (e->algo == INVFS_ALGO_FLACR) {
                    /* whole file is an APE(PCM) blob; sibling inode
                       "name!recipe" holds the frame recipe. Rebuild the
                       ORIGINAL FLAC bit-exactly: APE->WAV->flacx_rebuild. */
                    uint8_t *wav = NULL, *rcp = NULL, *fl = NULL;
                    size_t wav_len = 0, rcp_len = 0, fl_len = 0;
                    char rname[272];
                    if (invfs_ape_to_wav(blob, hdr, &wav, &wav_len) != 0) {
                        fprintf(stderr, "FLACR: APE->WAV failed for %s\n", rec_name);
                        free(blob); return -1;
                    }
                    snprintf(rname, sizeof rname, "%s!recipe", rec_name);
                    uint64_t rino = vol_find(v, rname);
                    if (rino == 0) {
                        fprintf(stderr, "FLACR: recipe inode '%s' not found\n", rname);
                        free(wav); free(blob); return -1;
                    }
                    if (vol_read_inode(v, rino, 0, &rcp, &rcp_len) != 0) {
                        fprintf(stderr, "FLACR: cannot read recipe '%s'\n", rname);
                        free(wav); free(blob); return -1;
                    }
                    /* cover payloads: "name!coverN" (v2 recipes) */
                    int ncv = flacx_recipe_num_covers(rcp, rcp_len);
                    flacx_cover covers[16];
                    uint8_t *cdata[16];
                    int ok = 1;
                    for (int ci = 0; ci < ncv && ci < 16; ci++) {
                        char cn[288];
                        snprintf(cn, sizeof cn, "%s!cover%d", rec_name, ci);
                        uint64_t cino = vol_find(v, cn);
                        size_t clen = 0;
                        cdata[ci] = NULL;
                        if (cino == 0 ||
                            vol_read_inode(v, cino, 0, &cdata[ci], &clen) != 0 ||
                            clen > 0x7FFFFFFF) {
                            fprintf(stderr, "FLACR: cover '%s' missing\n", cn);
                            ok = 0;
                            break;
                        }
                        covers[ci].data = cdata[ci];
                        covers[ci].len = (uint32_t)clen;
                        covers[ci].offset = 0;
                    }
                    if (ok && flacx_rebuild(wav, wav_len, rcp, rcp_len, covers,
                                            (uint32_t)ncv, &fl, &fl_len) != 0)
                        ok = 0;
                    for (int ci = 0; ci < ncv && ci < 16; ci++) free(cdata[ci]);
                    if (!ok || fl_len != e->length) {
                        fprintf(stderr, "FLACR: rebuild error (got %zu want %llu)\n",
                                fl_len, (unsigned long long)e->length);
                        free(wav); free(rcp); free(blob); return -1;
                    }
                    memcpy(data + dst_off, fl, fl_len);
                    free(fl); free(wav); free(rcp);
                } else if (e->algo == INVFS_ALGO_TARR) {
                    /* blob = recipe: [0x01][zstd...] or [0x00][raw IVFT] */
                    const uint8_t *rcp; size_t rcp_len;
                    uint8_t *rcp_own = NULL;
                    if (hdr > 1 && blob[0] == 1) {
                        size_t rsize = ZSTD_getFrameContentSize(blob + 1, hdr - 1);
                        if (rsize == ZSTD_CONTENTSIZE_ERROR ||
                            rsize == ZSTD_CONTENTSIZE_UNKNOWN || rsize > (1u << 28)) {
                            fprintf(stderr, "TARR: bad recipe frame\n");
                            free(blob); return -1;
                        }
                        rcp_own = (uint8_t *)malloc(rsize ? rsize : 1);
                        if (!rcp_own) { free(blob); return -1; }
                        size_t rr = ZSTD_decompress(rcp_own, rsize, blob + 1, hdr - 1);
                        if (ZSTD_isError(rr)) {
                            fprintf(stderr, "TARR: recipe decompress fail\n");
                            free(rcp_own); free(blob); return -1;
                        }
                        rcp = rcp_own; rcp_len = rr;
                    } else {
                        rcp = blob + 1; rcp_len = hdr - 1;
                    }
                    tarx_member *members = NULL; size_t n = 0;
                    uint8_t *trailer = NULL; size_t tlen = 0;
                    if (tarx_parse_recipe(rcp, rcp_len, &members, &n, &trailer, &tlen) != 0) {
                        fprintf(stderr, "TARR: bad recipe for %s\n", rec_name);
                        free(rcp_own); free(blob); return -1;
                    }
                    int np = tarx_recipe_num_parts(rcp, rcp_len);
                    uint8_t **parts = (uint8_t **)calloc(np > 0 ? (size_t)np : 1, sizeof(void *));
                    size_t *plens = (size_t *)calloc(np > 0 ? (size_t)np : 1, sizeof(size_t));
                    int ok = 1;
                    for (int pi = 0; pi < np; pi++) {
                        char pn[320];
                        snprintf(pn, sizeof pn, "%s!part%u", rec_name, pi);
                        uint64_t pino = vol_find(v, pn);
                        if (!pino) {
                            fprintf(stderr, "TARR: part '%s' missing\n", pn);
                            ok = 0; break;
                        }
                        if (vol_read_inode(v, pino, 0, &parts[pi], &plens[pi]) != 0) { ok = 0; break; }
                    }
                    uint8_t *fl = NULL; size_t fl_len = 0;
                    if (ok && tarx_rebuild(members, n, trailer, tlen,
                                           (const uint8_t *const *)parts, plens,
                                           &fl, &fl_len) != 0)
                        ok = 0;
                    for (int pi = 0; pi < np; pi++) free(parts[pi]);
                    free(parts); free(plens); free(members); free(trailer);
                    if (!ok || fl_len != e->length) {
                        fprintf(stderr, "TARR: rebuild error (got %zu want %llu)\n",
                                fl_len, (unsigned long long)e->length);
                        free(rcp_own); free(blob); free(fl); return -1;
                    }
                    memcpy(data + dst_off, fl, fl_len);
                    free(fl); free(rcp_own);
                } else if (e->algo == INVFS_ALGO_GZR) {
                    /* blob = recipe: [0x01][zstd] or [0x00][raw]; recipe =
                       [IVGZ][ver][level][mem][crc32][isize][hlen(2)][header] + IVFT */
                    const uint8_t *rcp; size_t rcp_len;
                    uint8_t *rcp_own = NULL;
                    if (hdr > 1 && blob[0] == 1) {
                        size_t rsize = ZSTD_getFrameContentSize(blob + 1, hdr - 1);
                        if (rsize == ZSTD_CONTENTSIZE_ERROR ||
                            rsize == ZSTD_CONTENTSIZE_UNKNOWN || rsize > (1u << 28)) {
                            fprintf(stderr, "GZR: bad recipe frame\n");
                            free(blob); return -1;
                        }
                        rcp_own = (uint8_t *)malloc(rsize ? rsize : 1);
                        if (!rcp_own) { free(blob); return -1; }
                        size_t rr = ZSTD_decompress(rcp_own, rsize, blob + 1, hdr - 1);
                        if (ZSTD_isError(rr)) {
                            fprintf(stderr, "GZR: recipe decompress fail\n");
                            free(rcp_own); free(blob); return -1;
                        }
                        rcp = rcp_own; rcp_len = rr;
                    } else {
                        rcp = blob + 1; rcp_len = hdr - 1;
                    }
                    if (rcp_len < 20 || memcmp(rcp, "IVGZ", 4) != 0 || rcp[4] != 1) {
                        fprintf(stderr, "GZR: bad recipe for %s\n", rec_name);
                        free(rcp_own); free(blob); return -1;
                    }
                    int glevel = rcp[5];
                    int gmem = rcp[6];
                    unsigned gcrc = (unsigned)rcp[7] | ((unsigned)rcp[8] << 8) |
                                    ((unsigned)rcp[9] << 16) | ((unsigned)rcp[10] << 24);
                    unsigned gisz = (unsigned)rcp[11] | ((unsigned)rcp[12] << 8) |
                                     ((unsigned)rcp[13] << 16) | ((unsigned)rcp[14] << 24);
                    unsigned ghl = (unsigned)rcp[15] | ((unsigned)rcp[16] << 8);
                    if (17 + ghl + 17 > rcp_len) {
                        fprintf(stderr, "GZR: short recipe\n");
                        free(rcp_own); free(blob); return -1;
                    }
                    const uint8_t *ghdr = rcp + 17;
                    const uint8_t *ivft = rcp + 17 + ghl;
                    size_t ivft_len = rcp_len - 17 - ghl;
                    tarx_member *members = NULL; size_t n = 0;
                    uint8_t *trailer = NULL; size_t tlen = 0;
                    if (tarx_parse_recipe(ivft, ivft_len, &members, &n, &trailer, &tlen) != 0) {
                        fprintf(stderr, "GZR: bad IVFT for %s\n", rec_name);
                        free(rcp_own); free(blob); return -1;
                    }
                    int np = tarx_recipe_num_parts(ivft, ivft_len);
                    uint8_t **parts = (uint8_t **)calloc(np > 0 ? (size_t)np : 1, sizeof(void *));
                    size_t *plens = (size_t *)calloc(np > 0 ? (size_t)np : 1, sizeof(size_t));
                    int ok = 1;
                    for (int pi = 0; pi < np; pi++) {
                        char pn[320];
                        snprintf(pn, sizeof pn, "%s!part%u", rec_name, pi);
                        uint64_t pino = vol_find(v, pn);
                        if (!pino) {
                            fprintf(stderr, "GZR: part '%s' missing\n", pn);
                            ok = 0; break;
                        }
                        if (vol_read_inode(v, pino, 0, &parts[pi], &plens[pi]) != 0) { ok = 0; break; }
                    }
                    uint8_t *tar = NULL; size_t tar_len = 0;
                    if (ok && tarx_rebuild(members, n, trailer, tlen,
                                           (const uint8_t *const *)parts, plens,
                                           &tar, &tar_len) != 0)
                        ok = 0;
                    for (int pi = 0; pi < np; pi++) free(parts[pi]);
                    free(parts); free(plens); free(members); free(trailer);
                    if (tar_len > UINT32_MAX) {
                        fprintf(stderr, "GZR rebuild: tar too large (%llu > UINT32_MAX)\n",
                                (unsigned long long)tar_len);
                        free(tar);
                        free(rcp_own); free(blob); return -1;
                    }
                    uint8_t *fl = NULL; size_t fl_len = 0;
                    if (ok) {
                        /* reproduce deflate stream bit-exactly, wrap gzip */
                        z_stream s;
                        memset(&s, 0, sizeof s);
                        if (deflateInit2(&s, glevel, Z_DEFLATED, -15, gmem,
                                         Z_DEFAULT_STRATEGY) == Z_OK) {
                            size_t bound = deflateBound(&s, (uLong)tar_len);
                            uint8_t *stream = (uint8_t *)malloc(bound);
                            s.next_in = tar;
                            s.avail_in = (uInt)(tar_len > 0x7FFFFFFF ? 0x7FFFFFFF : tar_len);
                            s.next_out = stream;
                            s.avail_out = (uInt)bound;
                            int r2 = deflate(&s, Z_FINISH);
                            size_t stream_len = (size_t)s.total_out;
                            deflateEnd(&s);
                            if (r2 == Z_STREAM_END) {
                                fl = (uint8_t *)malloc(ghl + stream_len + 8);
                                if (fl) {
                                    memcpy(fl, ghdr, ghl);
                                    memcpy(fl + ghl, stream, stream_len);
                                    uLong c = crc32(0L, Z_NULL, 0);
                                    c = crc32(c, tar, (uInt)(tar_len > 0x7FFFFFFF ? 0x7FFFFFFF : tar_len));
                                    fl[ghl + stream_len + 0] = (uint8_t)(c & 0xFF);
                                    fl[ghl + stream_len + 1] = (uint8_t)((c >> 8) & 0xFF);
                                    fl[ghl + stream_len + 2] = (uint8_t)((c >> 16) & 0xFF);
                                    fl[ghl + stream_len + 3] = (uint8_t)((c >> 24) & 0xFF);
                                    fl[ghl + stream_len + 4] = (uint8_t)(gisz & 0xFF);
                                    fl[ghl + stream_len + 5] = (uint8_t)((gisz >> 8) & 0xFF);
                                    fl[ghl + stream_len + 6] = (uint8_t)((gisz >> 16) & 0xFF);
                                    fl[ghl + stream_len + 7] = (uint8_t)((gisz >> 24) & 0xFF);
                                    fl_len = ghl + stream_len + 8;
                                    if ((unsigned)(c & 0xFFFFFFFFu) != gcrc) {
                                        fprintf(stderr, "GZR: crc mismatch for %s\n", rec_name);
                                        ok = 0;
                                    }
                                } else ok = 0;
                            } else ok = 0;
                            free(stream);
                        } else ok = 0;
                    }
                    free(tar);
                    if (!ok || fl_len != e->length) {
                        fprintf(stderr, "GZR: rebuild error (got %zu want %llu)\n",
                                fl_len, (unsigned long long)e->length);
                        free(fl); free(rcp_own); free(blob); return -1;
                    }
                    memcpy(data + dst_off, fl, fl_len);
                    free(fl); free(rcp_own);
                } else if (e->algo == INVFS_ALGO_PNGR) {
                    /* blob = recipe [0x01][zstd] or [0x00][raw IVPN];
                       pixels from sibling "name!jxl" (djxl -> PNG). */
                    const uint8_t *rcp; size_t rcp_len;
                    uint8_t *rcp_own = NULL;
                    if (hdr > 1 && blob[0] == 1) {
                        size_t rsize = ZSTD_getFrameContentSize(blob + 1, hdr - 1);
                        if (rsize == ZSTD_CONTENTSIZE_ERROR ||
                            rsize == ZSTD_CONTENTSIZE_UNKNOWN || rsize > (1u << 28)) {
                            fprintf(stderr, "PNGR: bad recipe frame\n");
                            free(blob); return -1;
                        }
                        rcp_own = (uint8_t *)malloc(rsize ? rsize : 1);
                        if (!rcp_own) { free(blob); return -1; }
                        size_t rr = ZSTD_decompress(rcp_own, rsize, blob + 1, hdr - 1);
                        if (ZSTD_isError(rr)) {
                            fprintf(stderr, "PNGR: recipe decompress fail\n");
                            free(rcp_own); free(blob); return -1;
                        }
                        rcp = rcp_own; rcp_len = rr;
                    } else {
                        rcp = blob + 1; rcp_len = hdr - 1;
                    }
                    pngx_info pi;
                    memset(&pi, 0, sizeof pi);
                    if (pngx_parse_recipe(rcp, rcp_len, &pi) != 0) {
                        fprintf(stderr, "PNGR: bad recipe for %s (len %zu)\n",
                                rec_name, rcp_len);
                        fprintf(stderr, "PNGR: recipe head: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                rcp_len > 0 ? rcp[0] : 0, rcp_len > 1 ? rcp[1] : 0,
                                rcp_len > 2 ? rcp[2] : 0, rcp_len > 3 ? rcp[3] : 0,
                                rcp_len > 4 ? rcp[4] : 0, rcp_len > 5 ? rcp[5] : 0,
                                rcp_len > 6 ? rcp[6] : 0, rcp_len > 7 ? rcp[7] : 0,
                                rcp_len > 8 ? rcp[8] : 0, rcp_len > 9 ? rcp[9] : 0);
                        free(rcp_own); free(blob); return -1;
                    }
                    int ok = 1;
                    uint8_t *rgb = NULL; size_t rgb_len = 0;
                    {
                        char jn[320];
                        snprintf(jn, sizeof jn, "%s!jxl", rec_name);
                        uint64_t jino = vol_find(v, jn);
                        if (!jino) {
                            fprintf(stderr, "PNGR: jxl '%s' missing\n", jn);
                            ok = 0;
                        } else if (invfs_png_from_jxl(v, jino, &rgb, &rgb_len) != 0) {
                            fprintf(stderr, "PNGR: djxl failed for %s\n", rec_name);
                            ok = 0;
                        }
                    }
                    uint8_t *filt = NULL; size_t filt_len = 0;
                    uint8_t *stream = NULL; size_t stream_len = 0;
                    uint8_t *fl = NULL; size_t fl_len = 0;
                    if (ok && pngx_refilter(rgb, rgb_len, &pi, &filt, &filt_len) != 0)
                        ok = 0;
                    if (ok) {
                        /* deflate replica (zlib or miniz) */
                        if (pi.enc == 0) {
                            z_stream s;
                            memset(&s, 0, sizeof s);
                            if (deflateInit2(&s, pi.level, Z_DEFLATED, 15, pi.mem,
                                             Z_DEFAULT_STRATEGY) == Z_OK) {
                                size_t bound = deflateBound(&s, (uLong)filt_len);
                                stream = (uint8_t *)malloc(bound);
                                s.next_in = filt;
                                s.avail_in = (uInt)(filt_len > 0x7FFFFFFF ? 0x7FFFFFFF : filt_len);
                                s.next_out = stream;
                                s.avail_out = (uInt)bound;
                                int r2 = deflate(&s, Z_FINISH);
                                stream_len = (size_t)s.total_out;
                                deflateEnd(&s);
                                if (r2 != Z_STREAM_END) ok = 0;
                            } else ok = 0;
                        } else {
                            size_t bound = filt_len + filt_len / 4 + 4096;
                            stream = (uint8_t *)malloc(bound);
                            if (mz_tdefl_compress(filt, filt_len, stream, bound,
                                                  pi.level, &stream_len) != 0)
                                ok = 0;
                        }
                    }
                    if (ok && pngx_rebuild(&pi, stream, stream_len, &fl, &fl_len) != 0)
                        ok = 0;
                    free(rgb); free(filt); free(stream);
                    if (!ok || fl_len != e->length) {
                        fprintf(stderr, "PNGR: rebuild error (got %zu want %llu)\n",
                                fl_len, (unsigned long long)e->length);
                        pngx_free(&pi); free(fl); free(rcp_own); free(blob); return -1;
                    }
                    memcpy(data + dst_off, fl, fl_len);
                    pngx_free(&pi); free(fl); free(rcp_own);
                } else if (e->algo == INVFS_ALGO_EXER) {
                    /* WP14b M2: exe-as-container. The blob is ZSTD-19 of
                     * the EXER payload (header + part table + glue); each
                     * member's bytes live in a "name!exrN" sibling read
                     * through the normal path (a JXL sibling decodes to the
                     * member, a ZSTD one just inflates). Splice the members
                     * back over the glue at the table offsets. The payload
                     * is disk input: exer_payload_parse validates every
                     * bound before anything is copied. */
                    uint8_t *pay = NULL;
                    exer_row *rows = NULL;
                    uint8_t *parts[EXE_MAX_MEDIA];
                    size_t n = 0, pi;
                    int ok = 0;
                    size_t dsize = ZSTD_getFrameContentSize(blob, hdr);

                    memset(parts, 0, sizeof parts);
                    if (dsize != ZSTD_CONTENTSIZE_ERROR &&
                        dsize != ZSTD_CONTENTSIZE_UNKNOWN &&
                        dsize >= 8 + 17 &&
                        dsize <= (uint64_t)e->length + 8 + 17 * EXE_MAX_MEDIA)
                        pay = (uint8_t *)malloc(dsize);
                    rows = (exer_row *)malloc(EXE_MAX_MEDIA * sizeof *rows);
                    if (pay && rows) {
                        size_t d = ZSTD_decompress(pay, dsize, blob, hdr);
                        if (!ZSTD_isError(d) && d == dsize &&
                            exer_payload_parse(pay, dsize, e->length,
                                               rows, EXE_MAX_MEDIA, &n) == 0)
                            ok = 1;
                    }
                    for (pi = 0; ok && pi < n; pi++) {
                        char pn[288];
                        uint64_t pino;
                        size_t plen = 0;
                        snprintf(pn, sizeof pn, "%s!exr%zu", rec_name, pi);
                        pino = vol_find(v, pn);
                        if (!pino ||
                            vol_read_file(v, pino, &parts[pi], &plen) != 0 ||
                            plen != (size_t)rows[pi].len) {
                            fprintf(stderr, "EXER: part '%s' unreadable\n", pn);
                            ok = 0;
                            break;
                        }
                    }
                    if (ok)
                        exer_splice(pay, rows, n, parts, data, e->length);
                    for (pi = 0; pi < n; pi++) free(parts[pi]);
                    free(pay);
                    free(rows);
                    if (!ok) {
                        fprintf(stderr, "EXER: rebuild failed for %s\n",
                                rec_name);
                        free(blob); return -1;
                    }
                } else if (e->algo == INVFS_ALGO_NONE) {
                    memcpy(data + dst_off, blob, hdr);
                } else {
                    const invfs_codec *pc = invfs_codec_by_algo(e->algo);
                    const invfs_pack_def *pd =
                        pc ? invfs_codec_pack_def(pc) : NULL;
                    /* WP16b: a seekable pack container (map command ->
                     * CAP_SEEK) with its !mbrmap sibling splices locally --
                     * no pack exec, ever. The same read works with the pack
                     * ABSENT (pc == NULL): the map is self-describing, the
                     * sibling alone decides. Missing map: the pack's rebuild
                     * exec answers, as before. */
                    int mapped = 0;
                    if ((pd && pd->is_container &&
                         (pc->caps & INVFS_CODEC_CAP_SEEK)) || !pc) {
                        char mbn[288];
                        snprintf(mbn, sizeof mbn, "%s!mbrmap", rec_name);
                        mapped = vol_find(v, mbn) != 0;
                    }
                    if (mapped) {
                        /* cpack_map_read returns the byte count (the
                         * vol_read_range convention): < 0 is the failure */
                        if (cpack_map_read(v, rec_name, inode_id,
                                           (uint64_t)e->length,
                                           e->file_offset, data + dst_off,
                                           (size_t)e->length) < 0) {
                            fprintf(stderr, "%s: mapped container read "
                                    "failed for %s\n",
                                    pc ? pc->name : "codecpack", rec_name);
                            free(blob); return -1;
                        }
                    } else if (pd && pd->is_container) {
                        /* WP16a: the blob is the pack's recipe; the members
                         * live in "!mbrNNNN" sibling inodes and are read
                         * through their CURRENT stored form (vol_read_file
                         * -- the pack's extract cannot help, the original
                         * container no longer exists), then spliced by the
                         * pack's rebuild command. A missing/corrupt member
                         * fails the read LOUDLY (the 1:1 invariant). */
                        if (pack_container_rebuild(v, pc, rec_name,
                                                   blob, hdr,
                                                   data + dst_off,
                                                   (size_t)e->length) != 0) {
                            fprintf(stderr, "%s: container rebuild failed "
                                    "for %s\n", pc->name, rec_name);
                            free(blob); return -1;
                        }
                    } else if (!pc || !pc->decode) {
                        /* WP13: an algo this build cannot decode (pack not
                         * loaded) fails LOUDLY -- the historical raw copy
                         * would serve the blob as if it were the file,
                         * silently breaking the 1:1 invariant. */
                        fprintf(stderr, "inode %llu: algo %u requires a "
                                "codecpack that is not loaded\n",
                                (unsigned long long)inode_id, e->algo);
                        free(blob); return -1;
                    } else if (pc->decode(blob, hdr, data + dst_off,
                                          (size_t)e->length) != 0) {
                        fprintf(stderr, "%s: pack decode error\n", pc->name);
                        free(blob); return -1;
                    }
                }
                free(blob);
            }
    return 0;
}


int vol_read_inode(invfs_volume *v, uint64_t inode_id, unsigned depth,
                          uint8_t **out, size_t *out_len)
{
    uint64_t pos, end;
    uint8_t *data = NULL;
    size_t len = 0;
    char rec_name[INVFS_MAX_NAME + 1];

    /* WP-M8: a v3 volume has no append-only record stream. The inode row in
     * the base tree carries a content-addressed recipe *reference*; fetch
     * and BLAKE3-verify the immutable blob, then decode its segments with
     * the exact same codec code as the v2 path (vol_decode_ast_entries).
     * An empty file (size 0 / no address) is the "no content" case.
     * WP-M11: vol_v3_inode_get and vol_v3_recipe_load resolve through the
     * delta overlay (delta first, then base), so a read observes the recent
     * tier without this function knowing about it. */
    if (v->sb.vol_flags & VOLF_V3) {
        invfs_v3_inode in;
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents = NULL;
        size_t n_ents = 0;
        uint8_t *blob = NULL;
        size_t blen = 0;
        int rc = vol_v3_inode_get(v, inode_id, &in);
        static const uint8_t zero_addr[INVFS_V3_RECIPE_ADDR_LEN];

        if (rc != 1)
            return -1;
        if (in.type == INVFS_ITYP_LNK) {
            if (in.size == 0 ||
                memcmp(in.recipe_addr, zero_addr, INVFS_V3_RECIPE_ADDR_LEN) == 0) {
                *out = (uint8_t *)calloc(1, 1);
                if (!*out) return -1;
                *out_len = 0;
                return 0;
            }
            if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0)
                return -1;
            *out = blob;
            *out_len = blen;
            return 0;
        }
        if (in.size == 0 ||
            memcmp(in.recipe_addr, zero_addr, INVFS_V3_RECIPE_ADDR_LEN) == 0) {
            *out = (uint8_t *)malloc(1);
            if (!*out)
                return -1;
            *out_len = 0;
            return 0;
        }
        if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0) {
            fprintf(stderr, "vol_read_inode: v3 inode %llu: recipe blob "
                    "missing/corrupt\n", (unsigned long long)inode_id);
            return -1;
        }
        if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0) {
            fprintf(stderr, "vol_read_inode: v3 inode %llu: corrupt recipe "
                    "blob\n", (unsigned long long)inode_id);
            free(blob);
            return -1;
        }
        /* the row's size and the blob's header must agree; a disagreement
         * is corruption, not something to paper over with a clamp */
        if (ah.file_size != in.size) {
            fprintf(stderr, "vol_read_inode: v3 inode %llu: row size %llu != "
                    "recipe size %llu\n", (unsigned long long)inode_id,
                    (unsigned long long)in.size,
                    (unsigned long long)ah.file_size);
            free(blob);
            return -1;
        }
        len = (size_t)ah.file_size;
        data = (uint8_t *)malloc(len ? len : 1);
        if (!data) { free(blob); return -1; }
        char iname[600];
        const invfs_ast_window_entry *wins = NULL;
        uint32_t n_wins = 0;
        iname[0] = 0;
        if (vol_ast_recipe_windows(blob, blen, &ah, &wins, &n_wins) != 0) {
            fprintf(stderr, "inode %llu: corrupt window table\n",
                    (unsigned long long)inode_id);
            free(data); free(blob);
            return -1;
        }
        vol_v3_path_of(v, inode_id, iname, sizeof iname);
        if (vol_decode_ast_entries(v, inode_id, iname, &ah, ents,
                                   wins, n_wins, data) != 0) {
            free(data); free(blob);
            return -1;
        }
        free(blob);
        *out = data;
        *out_len = len;
        return 0;
    }

    /* The id index knows where this record is; without it every read of every
       file re-read the whole inode area, which is what kept reads quadratic
       after the name index landed. 0 means "not indexed" -- fall back to the
       scan below, which is still the authority. WP47: the hint is valid in any
       mapper extent, and the fallback is the shared mapper-aware walker. */
    pos = read_locate_record(v, inode_id);
    if (!pos) return -1;
    end = pos + INVFS_REC_HDR_LEN;

    while (pos + INVFS_REC_HDR_LEN <= end) {
        invfs_inode_rec rec_h;
        uint32_t crc_stored, crc_calc;
        uint8_t *rec;

        if (io_pread(&v->io, pos, &rec_h, sizeof(rec_h)) != 0)
            return -1;
        if (rec_h.magic != INODE_REC_MAGIC && rec_h.magic != TOMBSTONE_MAGIC)
            return -1;
        if (rec_h.magic == TOMBSTONE_MAGIC) { pos += rec_h.rec_len + 4; continue; }
        if (rec_h.inode_id != inode_id) { pos += rec_h.rec_len + 4; continue; }

        /* read full record + crc, verify */
        if (rec_h.rec_len < INVFS_REC_HDR_LEN + 1 ||
            rec_h.rec_len > INVFS_MAX_REC_LEN) {
            fprintf(stderr, "inode %llu: rec_len %u outside valid range\n",
                    (unsigned long long)inode_id, rec_h.rec_len);
            return -1;
        }
        rec = (uint8_t *)malloc(rec_h.rec_len);
        if (!rec) return -1;
        if (io_pread(&v->io, pos, rec, rec_h.rec_len) != 0 ||
            io_pread(&v->io, pos + rec_h.rec_len, &crc_stored, 4) != 0) {
            free(rec);
            return -1;
        }
        crc_calc = invfs_crc32c(rec, rec_h.rec_len);
        if (crc_calc != crc_stored) {
            fprintf(stderr, "inode record CRC mismatch (inode %llu)\n",
                    (unsigned long long)inode_id);
            free(rec);
            return -1;
        }

        /* the name lives in the full CRC-verified record, not in the
         * 36-byte prefix copy rec_h; materialize a bounded NUL-terminated
         * copy so every "%s" sibling-name build below is safe. */
        {
            const invfs_inode_rec *rr = (const invfs_inode_rec *)rec;
            size_t n = rr->name_len;
            size_t present = (size_t)rec_h.rec_len - INVFS_REC_HDR_LEN - 1;
            if (n > INVFS_MAX_NAME) n = INVFS_MAX_NAME;
            if (n > present) n = present;
            memcpy(rec_name, rr->name, n);
            rec_name[n] = 0;
        }

        /* parse AST: header + entries after rec header */
        {
            invfs_ast_hdr ast_h;
            const invfs_ast_block_entry *ents;
            size_t off = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)rec)
                                  - rec);
            if (invfs_ast_hdr_parse(rec + off, rec_h.rec_len - off,
                                    &ast_h) != 0) {
                fprintf(stderr, "inode %llu: unsupported/corrupt AST recipe "
                        "header\n", (unsigned long long)inode_id);
                free(rec);
                return -1;
            }
            off += ast_h.hdr_len;
            if ((size_t)ast_h.num_blocks * sizeof(invfs_ast_block_entry) >
                rec_h.rec_len - off) {
                free(rec);
                return -1;
            }
            ents = (const invfs_ast_block_entry *)(rec + off);

            len = (size_t)ast_h.file_size;
            data = (uint8_t *)malloc(len ? len : 1);
            if (!data) { free(rec); return -1; }

            const invfs_ast_window_entry *wins = NULL;
            uint32_t n_wins = 0;
            if (vol_ast_recipe_windows(rec + off - ast_h.hdr_len,
                                       rec_h.rec_len - off + ast_h.hdr_len,
                                       &ast_h, &wins, &n_wins) != 0) {
                fprintf(stderr, "inode %llu: corrupt window table\n",
                        (unsigned long long)inode_id);
                free(rec); free(data);
                return -1;
            }
            if (vol_decode_ast_entries(v, inode_id, rec_name, &ast_h,
                                       ents, wins, n_wins, data) != 0) {
                free(data); free(rec); return -1;
            }
        }
        free(rec);
        *out = data;
        *out_len = len;
        return 0;
    }
    return -1;  /* not found */
}


/* public entry: read file, reconstructing containers from children */
int vol_read_file(invfs_volume *v, uint64_t inode_id, uint8_t **out, size_t *out_len)
{
    return vol_read_inode(v, inode_id, 0, out, out_len);
}


/*
 * Read a file by name; if the name contains '!', it addresses a container
 * member (possibly nested: "a.zip!inner.zip!x.txt"). Members are extracted
 * on demand from the container's original bytes (1:1 container read-back).
 */
int vol_read_named(invfs_volume *v, const char *name, uint8_t **out, size_t *out_len)
{
    char buf[512], *comps[16];
    size_t ncomp = 0;
    uint8_t *cur = NULL;
    size_t cur_len = 0;
    uint64_t ino;

    if (strlen(name) + 1 > sizeof buf)
        return -1;
    strcpy(buf, name);

    /* split on '!' */
    {
        char *q = buf;
        while (ncomp < 16 && *q) {
            comps[ncomp++] = q;
            q = strchr(q, '!');
            if (!q) break;
            *q = 0;
            q++;
        }
    }
    if (ncomp == 0) return -1;

    ino = vol_find(v, comps[0]);
    if (!ino) return -1;
    if (vol_read_file(v, ino, &cur, &cur_len) != 0) return -1;

    for (size_t i = 1; i < ncomp; i++) {
        invfs_ast_child_entry *ch = NULL;
        size_t nch = 0, j;
        uint8_t *next = NULL;
        size_t next_len = 0;
        ch = (invfs_ast_child_entry *)calloc(MAX_AST_CHILDREN, sizeof(*ch));
        if (!ch) { free(cur); return -1; }
        nch = (size_t)vol_zip_parse_children(cur, cur_len, ch, MAX_AST_CHILDREN);
        if ((int)nch <= 0) { free(ch); free(cur); return -1; }
        for (j = 0; j < nch; j++) {
            if (ch[j].name_len == strlen(comps[i]) &&
                strncmp(ch[j].name, comps[i], ch[j].name_len) == 0)
                break;
        }
        if (j == nch) { free(ch); free(cur); return -1; }
        if (vol_zip_extract_member(cur, cur_len, &ch[j], &next, &next_len) != 0) {
            free(ch); free(cur);
            return -1;
        }
        free(ch);
        free(cur);
        cur = next;
        cur_len = next_len;
    }
    *out = cur;
    *out_len = cur_len;
    return 0;
}


/* stat: get file size by name; returns 0 on success */
int vol_stat(invfs_volume *v, const char *name, uint64_t *size_out)
{
    uint64_t id, ctime;
    return vol_stat_full(v, name, &id, size_out, &ctime);
}


/* One index lookup + one header read: inode id, size and ctime together.
   The Dokan/FUSE layers used to keep their own shadow copy of the whole
   inode area and rebuild it after every close, which is O(area) per file
   operation; this replaces it. Returns 0 on success. */
int vol_stat_full(invfs_volume *v, const char *name, uint64_t *id_out,
                  uint64_t *size_out, uint64_t *ctime_out)
{
    const name_index_entry *e;
    /* WP-M6: v3 resolves names through the dirent tree / inode rows. */
    if (v->sb.vol_flags & VOLF_V3)
        return vol_v3_path_stat(v, name, id_out, size_out, ctime_out);
    e = idx_get(v, name, strlen(name));

    /* The old scan walked from the start of the area and returned -1 on the
       first record that was not an INOD, so a single deleted file made stat
       fail for every name stored after it. The index goes straight to the
       live record -- and carries size and ctime, so a stat costs no I/O at
       all. Both are copied from the record header the index was built from,
       and every writer updates them through idx_put. */
    if (!e) return -1;
    /* Every out-param is optional: a caller that only wants to know whether a
       name exists has no id or size to put anywhere, and passing NULL for the
       rest is the obvious way to ask that. Dereferencing unconditionally
       turned that question into a crash. */
    if (id_out)    *id_out    = e->inode_id;
    if (size_out)  *size_out  = e->size;
    if (ctime_out) *ctime_out = e->ctime;
    return 0;
}


/* live names in the index (files + directory anchors) */
uint64_t vol_name_count(invfs_volume *v) { return (uint64_t)v->ncount; }


/*
 * Range read: decode only the segments covering [offset, offset+len).
 * Returns bytes actually read (may be less near EOF), or -1 on error.
 */
/* Algos whose one segment IS the whole file: the blob decodes in a single
   piece, so there is no cheaper way to answer a window than to rebuild
   everything, and the result is worth keeping. LZ4 and ZSTD stay out on
   purpose -- they decode per 64 KB segment, which is already bounded, and
   caching them would spend the budget evicting the entries that cost a
   subprocess to produce. */
static int algo_is_whole_file(uint32_t algo)
{
    const invfs_codec *c;
    if (algo == INVFS_ALGO_FLACR || algo == INVFS_ALGO_TARR ||
        algo == INVFS_ALGO_GZR   || algo == INVFS_ALGO_PNGR ||
        algo == INVFS_ALGO_PMP   || algo == INVFS_ALGO_APE  ||
        algo == INVFS_ALGO_JXL   || algo == INVFS_ALGO_EXER)
        return 1;
    /* WP13: codecpack codecs declare WHOLEFILE in their manifest caps */
    c = invfs_codec_by_algo(algo);
    if (!c || !(c->caps & INVFS_CODEC_CAP_WHOLEFILE))
        return 0;
    /* WP16b: a container pack carrying a map command (CAP_SEEK) has NO
     * whole-file read unit: ranged reads splice locally through the
     * !mbrmap sibling and never exec the pack. */
    if (c->caps & INVFS_CODEC_CAP_SEEK) {
        const invfs_pack_def *pd = invfs_codec_pack_def(c);
        if (pd && pd->is_container)
            return 0;
    }
    return 1;
}


/* WP-M9: ranged read of a v3 file. Fetch + BLAKE3-verify the recipe blob
 * once (O(segments), never O(filesize)), then decode only the entries that
 * overlap [offset, offset+len). A plain per-segment codec (NONE/LZ4/ZSTD)
 * decodes one bounded segment; a shared text batch is sliced through
 * vol_read_text_slice; a whole-file unit (codecpack/container) has no
 * cheaper read than the full reconstruction, so it falls back to
 * vol_read_inode once and slices the window. Returns bytes read or -1. */
static int v3_read_range(invfs_volume *v, uint64_t inode_id, uint64_t offset,
                         size_t len, void *buf)
{
    static const uint8_t zero_addr[INVFS_V3_RECIPE_ADDR_LEN];
    invfs_v3_inode in;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0, i, got = 0;
    uint8_t *blob = NULL, *all = NULL;
    size_t blen = 0, all_len = 0;
    int container_no_map = 0;
    int rc;

    rc = vol_v3_inode_get(v, inode_id, &in);
    if (rc != 1)
        return -1;
    if (in.size == 0 ||
        memcmp(in.recipe_addr, zero_addr, INVFS_V3_RECIPE_ADDR_LEN) == 0)
        return 0;
    if (offset >= in.size)
        return 0;
    if ((uint64_t)len > in.size - offset)
        len = (size_t)(in.size - offset);
    if (len == 0)
        return 0;

    if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0)
        return -1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 ||
        ah.file_size != in.size) {
        free(blob);
        return -1;
    }
    /* bound every entry against the row's size before allocating for it */
    for (i = 0; i < n_ents; i++) {
        uint64_t end = ents[i].file_offset + ents[i].length;
        if (ents[i].file_offset > in.size || end > in.size ||
            end < ents[i].file_offset) {
            free(blob);
            return -1;
        }
    }

    /* WP16b: check for seekable container with !mbrmap sibling */
    if (n_ents >= 1) {
        const invfs_codec *pc = invfs_codec_by_algo(ents[0].algo);
        const invfs_pack_def *pd = pc ? invfs_codec_pack_def(pc) : NULL;
        int seek = pd && pd->is_container && (pc->caps & INVFS_CODEC_CAP_SEEK) != 0;
        if (seek || !pc) {
            char iname[600];
            iname[0] = 0;
            if (vol_v3_path_of(v, inode_id, iname, sizeof iname) > 0 && iname[0]) {
                char mbn[640];
                snprintf(mbn, sizeof mbn, "%s!mbrmap", iname);
                if (vol_find(v, mbn) != 0) {
                    free(blob);
                    return cpack_map_read(v, iname, inode_id, in.size, offset, (uint8_t *)buf, len);
                }
                /* The map is GONE (deleted, or a pre-v1.1 sweep never wrote
                 * one): there is no local splice unit left, so this ranged
                 * read has to fall back to the pack's whole-file rebuild
                 * exec. algo_is_whole_file() deliberately says NO for a
                 * seekable container, so without this the entry falls
                 * through to the per-segment decoder and dies on
                 * "unexpected algo <container id>". */
                if (pd && pd->is_container)
                    container_no_map = 1;
            }
        }
    }

    /* Find starting entry. Entries are sorted by file_offset.
     * Use binary search to find the first entry whose seg_hi > offset. */
    size_t start_idx = 0;
    if (n_ents > 8) {
        size_t l = 0, r = n_ents;
        while (l < r) {
            size_t m = l + (r - l) / 2;
            uint64_t seg_hi = ents[m].file_offset + ents[m].length;
            if (seg_hi <= offset)
                l = m + 1;
            else
                r = m;
        }
        start_idx = l;
    }

    for (i = start_idx; i < n_ents; i++) {
        const invfs_ast_block_entry *e = &ents[i];
        uint64_t seg_lo = e->file_offset;
        uint64_t seg_hi = e->file_offset + e->length;
        uint64_t req_lo = offset, req_hi = offset + len;
        uint64_t lo, hi;
        uint8_t tmp[SEGMENT_SIZE];
        uint8_t *segbuf = tmp, *segheap = NULL;
        size_t want;

        if (seg_lo >= req_hi)
            break;  /* past the requested window: stop early */
        if (seg_hi <= req_lo)
            continue;
        lo = req_lo > seg_lo ? req_lo : seg_lo;
        hi = req_hi < seg_hi ? req_hi : seg_hi;
        if (lo >= hi)
            continue;

        /* shared batch member: slice it (heap + pba-keyed ARC) */
        if (e->zone == INVFS_ZONE_TEXT && tz_batch_algo(e->algo)) {
            want = (size_t)(hi - lo);
            if (vol_read_text_slice(v, inode_id, e, lo - seg_lo,
                                    (uint8_t *)buf + (size_t)(lo - offset),
                                    want) != 0) {
                free(blob);
                return -1;
            }
            got += want;
            continue;
        }

        /* whole-file unit (codecpack / container): no bounded segment
         * decode exists -- rebuild once and slice the requested window.
         * container_no_map: a seekable container whose !mbrmap is gone has
         * the same shape (vol_read_inode runs the pack's rebuild exec). */
        if (container_no_map || algo_is_whole_file(e->algo)) {
            if (!all) {
                if (vol_read_inode(v, inode_id, 0, &all, &all_len) != 0) {
                    free(blob);
                    return -1;
                }
            }
            want = (size_t)(hi - lo);
            if (lo < all_len) {
                size_t take = want;
                if (lo + take > all_len)
                    take = (size_t)(all_len - lo);
                memcpy((uint8_t *)buf + (size_t)(lo - offset), all + lo, take);
                got += take;
            }
            continue;
        }

        /* plain per-segment codec: decode the one segment, copy the window */
        if (e->length > sizeof tmp) {
            segheap = (uint8_t *)malloc((size_t)e->length);
            if (!segheap) { free(all); free(blob); return -1; }
            segbuf = segheap;
        }
        {
            uint64_t pba = e->pba;
            uint32_t hdr;
            uint8_t *sblob;
            if (pba == 0 || pba >= v->sb.total_blocks) {
                free(segheap); free(all); free(blob);
                return -1;
            }
            heat_touch_read(v, inode_id, e->block_id);
            if (seg_read_checked(v, pba, 0, 1, &hdr, &sblob) != 0) {
                fprintf(stderr, "segment CRC mismatch: inode %llu seg %u\n",
                        (unsigned long long)inode_id, e->block_id);
                free(segheap); free(all); free(blob);
                return -1;
            }
            if (e->algo == INVFS_ALGO_LZ4) {
                int d = LZ4_decompress_safe((const char *)sblob,
                                            (char *)segbuf, (int)hdr,
                                            (int)e->length);
                if (d != (int)e->length) {
                    free(sblob); free(segheap); free(all); free(blob);
                    return -1;
                }
            } else if (e->algo == INVFS_ALGO_ZSTD) {
                size_t d = ZSTD_decompress(segbuf, e->length, sblob, hdr);
                if (ZSTD_isError(d) || d != e->length) {
                    free(sblob); free(segheap); free(all); free(blob);
                    return -1;
                }
            } else if (e->algo == INVFS_ALGO_NONE) {
                if (hdr != e->length) {
                    free(sblob); free(segheap); free(all); free(blob);
                    return -1;
                }
                memcpy(segbuf, sblob, e->length);
            } else {
                /* the v3 recipe writer emits only the plain codecs; any
                 * other algo here is corruption -- fail loudly, never serve
                 * unverified bytes */
                fprintf(stderr, "v3 ranged read: inode %llu seg %u: "
                        "unexpected algo %u\n",
                        (unsigned long long)inode_id, e->block_id,
                        (unsigned)e->algo);
                free(sblob); free(segheap); free(all); free(blob);
                return -1;
            }
            free(sblob);
        }
        want = (size_t)(hi - lo);
        memcpy((uint8_t *)buf + (size_t)(lo - offset),
               segbuf + (size_t)(lo - seg_lo), want);
        free(segheap);
        got += want;
    }
    free(all);
    free(blob);
    return (int)got;
}

int vol_read_range(invfs_volume *v, uint64_t inode_id, uint64_t offset,
                   size_t len, void *buf)
{
    uint64_t pos, end;
    invfs_ast_hdr ast_h;
    invfs_ast_block_entry *ents = NULL;
    uint8_t *rec = NULL;
    uint32_t i;
    size_t got = 0;

    /* WP-M9: a v3 file resolves through its content-addressed recipe blob
     * and decodes only the segments the window needs. */
    if (v->sb.vol_flags & VOLF_V3)
        return v3_read_range(v, inode_id, offset, len, buf);

    /* WP47: hint jump when the index position is valid (mapper extent or
     * legacy region); otherwise locate through the mapper-aware walker. */
    pos = read_locate_record(v, inode_id);
    end = pos ? pos + INVFS_REC_HDR_LEN : 0;

    while (pos + INVFS_REC_HDR_LEN <= end) {
        invfs_inode_rec rec_h;
        uint32_t crc_stored, crc_calc;
        if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &rec_h, sizeof(rec_h)) != 0)
            return -1;
        if (rec_h.magic != INODE_REC_MAGIC && rec_h.magic != TOMBSTONE_MAGIC) {
            return -1;
        }
        if (rec_h.magic == TOMBSTONE_MAGIC) { pos += rec_h.rec_len + 4; continue; }
        if (rec_h.inode_id != inode_id) { pos += rec_h.rec_len + 4; continue; }
        if (rec_h.rec_len < INVFS_REC_HDR_LEN + 1 ||
            rec_h.rec_len > INVFS_MAX_REC_LEN) {
            fprintf(stderr, "[rr] inode %llu: rec_len %u outside valid range\n",
                    (unsigned long long)inode_id, rec_h.rec_len);
            return -1;
        }
        rec = (uint8_t *)malloc(rec_h.rec_len);
        if (!rec) return -1;
        if (io_seek(&v->io, pos) != 0 || io_read(&v->io, rec, rec_h.rec_len) != 0 ||
            io_read(&v->io, &crc_stored, 4) != 0) { free(rec); return -1; }
        crc_calc = invfs_crc32c(rec, rec_h.rec_len);
        if (crc_calc != crc_stored) { free(rec); return -1; }

        {
            size_t base = (size_t)(invfs_rec_cbody((const invfs_inode_rec *)rec)
                                   - rec);
            if (invfs_ast_hdr_parse(rec + base, rec_h.rec_len - base,
                                    &ast_h) != 0 ||
                (size_t)ast_h.num_blocks * sizeof(invfs_ast_block_entry) >
                    rec_h.rec_len - base - ast_h.hdr_len) {
                fprintf(stderr, "[rr] inode %llu: unsupported/corrupt AST recipe "
                        "header\n", (unsigned long long)inode_id);
                free(rec);
                return -1;
            }
            ents = (invfs_ast_block_entry *)(rec + base + ast_h.hdr_len);
        }
        break;
    }
    if (!rec) { fprintf(stderr,"[rr] record not found id=%llu\n",
        (unsigned long long)inode_id); return -1; }
    {
        /* the record's own name (rec_h was loop-local): sibling names
         * ("<name>!mbrmap") resolve by it */
        const invfs_inode_rec *rrh = (const invfs_inode_rec *)rec;
        size_t rpresent = (size_t)rrh->rec_len - INVFS_REC_HDR_LEN - 1;
        size_t rnl = rrh->name_len < INVFS_MAX_NAME
                   ? rrh->name_len : INVFS_MAX_NAME;
        char rname[INVFS_MAX_NAME + 1];
        if (rnl > rpresent) rnl = rpresent;
        memcpy(rname, rrh->name, rnl);
        rname[rnl] = 0;
        /* WP16b: a seekable containerpack record (the pack's algo + a map
         * command, i.e. CAP_SEEK) -- or an algo NO pack resolves (the pack
         * is absent) backed by a !mbrmap sibling -- serves ranged reads by
         * LOCAL SPLICE: the map translates ranges into recipe-segment reads
         * and member-sibling reads. No pack exec, no whole-file rebuild,
         * no ARC unit. A seekable pack whose map is missing (a pre-v1.1
         * sweep) falls back to the whole-file rebuild exec below. */
        int whole = ast_h.num_children > 0 ||
            (ast_h.num_blocks == 1 && algo_is_whole_file(ents[0].algo));
        if (!whole && ast_h.num_children == 0 && ast_h.num_blocks == 1 &&
            ents[0].zone == INVFS_ZONE_BINARY) {
            const invfs_codec *pc = invfs_codec_by_algo(ents[0].algo);
            const invfs_pack_def *pd = pc ? invfs_codec_pack_def(pc) : NULL;
            int seek = pd && pd->is_container &&
                       (pc->caps & INVFS_CODEC_CAP_SEEK) != 0;
            if (seek || !pc) {
                char mbn[288];
                snprintf(mbn, sizeof mbn, "%s!mbrmap", rname);
                if (vol_find(v, mbn) != 0) {
                    int64_t mrc;
                    if (offset >= ast_h.file_size) { free(rec); return 0; }
                    mrc = cpack_map_read(v, rname, inode_id,
                                         (uint64_t)ast_h.file_size, offset,
                                         (uint8_t *)buf, len);
                    free(rec);
                    return (int)mrc;
                }
                whole = seek;   /* map missing: the exec rebuild answers */
            }
        }
        if (whole) {
        /* Whole-file reconstruction, served out of the content cache.
         *
         * Two shapes land here. A container (num_children > 0) keeps the
         * original archive bytes and its members are windows into them, so a
         * window costs a full rebuild. A transcoded file is one segment whose
         * algo decodes in a single piece -- and for FLACR/TARR/GZR/PNGR there
         * is no segment path below at all: they fell through to the raw branch,
         * where the stored blob length never equals the reconstructed length,
         * so every ranged read of a swept file returned -1. That is why a
         * transcoded file was unreadable through a mount while invf-cat, which
         * goes through vol_read_file, still returned it byte for byte.
         *
         * Rebuilding per callback is what makes this need a cache rather than
         * just a fix: a mount asks in 64 KB pieces, so a sequential read of an
         * N-byte container did O(N^2) work, and a FLAC spawned MAC.exe once per
         * 64 KB. Copy the window out BEFORE handing the buffer to the cache --
         * arc_put either takes ownership or frees an oversized entry, and
         * after it returns the pointer is not ours to read. */
        const uint8_t *all = NULL;
        uint8_t *fresh = NULL;
        size_t all_len = 0, take = 0;
        free(rec);
        if (!arc_get(v->arc, inode_id, &all, &all_len)) {
            if (vol_read_file(v, inode_id, &fresh, &all_len) != 0) return -1;
            all = fresh;
        }
        if (offset < all_len) {
            take = (size_t)((offset + len > all_len) ? all_len - offset : len);
            memcpy(buf, all + offset, take);
        }
        if (fresh) arc_put(v->arc, inode_id, fresh, all_len);
        return (int)take;
        }
    }
    if (offset >= ast_h.file_size) { free(rec); return 0; }

    for (i = 0; i < ast_h.num_blocks; i++) {
        const invfs_ast_block_entry *e = &ents[i];
        uint64_t seg_lo = e->file_offset;
        uint64_t seg_hi = e->file_offset + e->length;
        uint64_t req_lo = offset, req_hi = offset + len;
        uint64_t lo, hi;
        uint8_t tmp[SEGMENT_SIZE];
        /* A container part (vol_create_blob_file) is ONE segment holding the
         * whole part, and a part can far exceed the 64 KB segment size of
         * the regular path: decode those from the heap, not the stack
         * (WP14b reads part heads through here at sweep time). */
        uint8_t *segbuf = tmp;
        uint8_t *segheap = NULL;
        size_t want;

        if (seg_hi <= req_lo || seg_lo >= req_hi)
            continue;  /* no overlap */
        lo = (req_lo > seg_lo) ? req_lo : seg_lo;
        hi = (req_hi < seg_hi) ? req_hi : seg_hi;
        if (hi > ast_h.file_size) hi = ast_h.file_size;
        if (lo >= hi) continue;

        /* Batch member: slice of a shared PPMd/ZSTD(+BCJ) batch; heap +
         * pba-keyed ARC, never the stack tmp[] (batches reach 4 MB,
         * segments only 64 KB) */
        if (e->zone == INVFS_ZONE_TEXT && tz_batch_algo(e->algo)) {
            want = (size_t)(hi - lo);
            if (vol_read_text_slice(v, inode_id, e, lo - seg_lo,
                                    (uint8_t *)buf + (size_t)(lo - offset),
                                    want) != 0) {
                free(rec); return -1;
            }
            got += want;
            continue;
        }

        /* decode whole segment */
        {
            uint64_t pba;
            uint32_t hdr;
            uint8_t *blob;
            if (e->length > sizeof tmp) {
                segheap = (uint8_t *)malloc((size_t)e->length);
                if (!segheap) { free(rec); return -1; }
                segbuf = segheap;
            }
            /* WP27: the entry carries the pba; plen derives from the
             * segment header (0 = unknown) */
            pba = e->pba;
            if (pba == 0 || pba >= v->sb.total_blocks) {
                free(segheap); free(rec); return -1;
            }
            heat_touch_read(v, inode_id, e->block_id);   /* WP19 */
            /* framed segment read, CRC-verified inside; a shadow-zone
             * failure gets one WP20 seal-parity recovery attempt before
             * the error propagates */
            if (seg_read_checked(v, pba, 0, 1, &hdr, &blob) != 0) {
                fprintf(stderr, "segment CRC mismatch: inode %llu seg %u\n",
                        (unsigned long long)inode_id, e->block_id);
                free(segheap); free(rec); return -1;
            }

            if (e->algo == INVFS_ALGO_LZ4) {
                int got = LZ4_decompress_safe((const char *)blob, (char *)segbuf,
                                              (int)hdr, (int)e->length);
                if (got != (int)e->length) {
                    free(blob); free(segheap); free(rec); return -1; }
            } else if (e->algo == INVFS_ALGO_ZSTD) {
                size_t got = ZSTD_decompress(segbuf, e->length, blob, hdr);
                if (ZSTD_isError(got) || got != e->length) { free(blob); free(segheap); free(rec); return -1; }
            } else if (e->algo == INVFS_ALGO_APE) {
                uint8_t *fl = NULL;
                size_t fl_len = 0;
                if (invfs_ape_decompress(blob, hdr, &fl, &fl_len) != 0 ||
                    fl_len < (size_t)(hi - lo) || (size_t)(lo - seg_lo) > fl_len) {
                    free(blob); free(segheap); free(rec); return -1;
                }
                want = (size_t)(hi - lo);
                memcpy((uint8_t *)buf + (size_t)(lo - offset),
                       fl + (size_t)(lo - seg_lo), want);
                got += want;
                free(fl);
                free(blob);
                free(segheap);
                continue;
            } else if (e->algo == INVFS_ALGO_PMP) {
                /* ranged read: decode the whole blob, hand back the window.
                   A media player seeking in an .mp3 hits this per read, and
                   packMP3 decodes at ~1.9 MB/s -- correct but slow. Worth a
                   cache if playback off the mount ever matters. */
                uint8_t *m = NULL;
                size_t m_len = 0;
                if (invfs_pmp_decompress(blob, hdr, &m, &m_len) != 0 ||
                    (size_t)(lo - seg_lo) > m_len ||
                    m_len - (size_t)(lo - seg_lo) < (size_t)(hi - lo)) {
                    free(m); free(blob); free(segheap); free(rec); return -1;
                }
                want = (size_t)(hi - lo);
                memcpy((uint8_t *)buf + (size_t)(lo - offset),
                       m + (size_t)(lo - seg_lo), want);
                got += want;
                free(m);
                free(blob);
                free(segheap);
                continue;
            } else {
                if (hdr != e->length) {
                    free(blob); free(segheap); free(rec); return -1; }
                memcpy(segbuf, blob, e->length);
            }
            free(blob);
        }

        want = (size_t)(hi - lo);
        memcpy((uint8_t *)buf + (size_t)(lo - offset), segbuf + (size_t)(lo - seg_lo), want);
        free(segheap);
        got += want;
    }
    free(rec);
    return (int)got;
}


