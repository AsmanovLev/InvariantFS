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
 * members and must never be mutated).
 *
 * WP-arc-concurrent-safe: the cached batch is read through a heap WINDOW, not
 * through a borrowed pointer. This function runs under invf_read with
 * g_io_lock released (fuse_fs.c:1407, :1414) and the daemon is fuse_loop_mt,
 * so another thread can be inside arc_replace -- free(victim->data) -- while
 * this one is still working off the cached batch. arc_get_copy copies under
 * the cache lock, so the window below is already private by the time
 * anything touches it; that is also what keeps the "cached batch stays in
 * encoded form" rule above true, because the BCJ inverse runs on the copy
 * and never on the shared batch. */
static int vol_read_text_slice(invfs_volume *v, uint64_t inode_id,
                               const invfs_ast_block_entry *e,
                               uint64_t slice_off, uint8_t *dst, size_t want)
{
    uint64_t pba = e->pba;
    uint8_t *win = NULL;        /* this slice's window, private to us */
    size_t   win_off;           /* where `win` starts inside the batch */
    size_t   need;              /* bytes of the batch the slice needs */
    uint8_t *fresh = NULL;
    int      bcj = (e->algo == INVFS_ALGO_ZSTD_BCJ);
    int      cached;

    if (want == 0) return 0;
    if (slice_off + want > e->length)
        return -1;   /* window runs past the member's slice */
    if (pba == 0 || pba >= v->sb.total_blocks) {
        fprintf(stderr, "pba invalid: inode %llu text seg %u\n",
                (unsigned long long)inode_id, e->block_id);
        return -1;
    }
    heat_touch_read(v, inode_id, e->block_id);   /* WP19: member heat */

    /* How much of the batch this member needs. The BCJ inverse is only
       bijective over the WHOLE member window (it was prefiltered standalone,
       pc=0), so it needs all of it; every other batch algo needs only the
       bytes the caller asked for. */
    if (bcj) { win_off = e->block_offset;          need = (size_t)e->length; }
    else     { win_off = e->block_offset + slice_off; need = want; }

    win = (uint8_t *)malloc(need ? need : 1);
    if (!win) return -1;

    cached = 0;
    {
        size_t got = 0;
        /* a short entry is not a usable window: fall through and decode */
        if (arc_get_copy(v->arc, pba | TZ_ARC_TAG, (size_t)win_off, need,
                         win, &got) && got == need)
            cached = 1;
    }

    if (!cached) {
        uint32_t csize, usize;
        uint8_t *blob;

        /* framed read, CRC-verified inside (payload holds at least
         * [4B usize][2B props]); on failure the WP20 seal parity gets one
         * recovery attempt before the error propagates. WP27: plen is not
         * stored in the entry -- 0 = derived from the segment header. */
        if (seg_read_checked(v, pba, 0, 4 + 2, &csize, &blob) != 0) {
            fprintf(stderr, "segment CRC mismatch: text batch pba %llu\n",
                    (unsigned long long)pba);
            free(win);
            return -1;
        }
        memcpy(&usize, blob, 4);
        if (usize == 0 || usize > (64u << 20)) {   /* batches are <= 4 MB */
            free(blob); free(win); return -1;
        }
        fresh = (uint8_t *)malloc(usize);
        if (!fresh) { free(blob); free(win); return -1; }
        /* the decoder's wire blob starts past the usize LE */
        if (e->algo == INVFS_ALGO_PPMD) {
            if (invfs_ppmd_decode(blob + 4, csize - 4, fresh, usize) != 0) {
                fprintf(stderr, "ppmd decode failed: text batch pba %llu\n",
                        (unsigned long long)pba);
                free(fresh); free(blob); free(win); return -1;
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
                free(fresh); free(blob); free(win); return -1;
            }
        } else {
            fprintf(stderr, "text slice: unknown batch algo %u\n",
                    (unsigned)e->algo);
            free(fresh); free(blob); free(win); return -1;
        }
        free(blob);
        if ((uint64_t)win_off + need > usize) {   /* recipe claims more than
                                                     * the batch holds */
            free(fresh); free(win); return -1;
        }
        memcpy(win, fresh + win_off, need);
        /* arc_put takes ownership (or frees an oversized entry), so the slice
         * is copied out of `fresh` BEFORE the buffer is handed over -- and the
         * window is already private, so the entry may be evicted immediately
         * afterwards without affecting us. NULL cache frees. */
        arc_put(v->arc, pba | TZ_ARC_TAG, fresh, usize);
    }

    if (bcj) {
        /* invert the per-member prefilter over the whole slice window, at
         * pc=0 -- the same window the encoder ran over (flush feed loop) */
        invfs_bcj_x86_dec(win, need);
        memcpy(dst, win + slice_off, want);
    } else {
        memcpy(dst, win, want);
    }
    free(win);
    return 0;
}

/* WP47: is `ip` a plausible record position? On a v0.3.0+ mapper volume
 * idx_get_id returns an absolute offset inside a dynamic metadata extent,
 * which a bound over the contiguous record region rejected. Accept any
 * position inside any mapper extent (mirroring vol_inode_next); anything
 * else is not a record position. */


typedef struct {
    uint64_t want;
    uint64_t pos;
    int found;
} read_locate_ctx;



/* Locate the live INOD for inode_id: the O(1) index hint when it is valid
 * and names a record for this id, else a scan of the metadata extents.
 * Returns the record position, or 0 when absent. */


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

/* The framing contract for ONE decoded segment, in one place.
 *
 * `hdr` is the csize the frame header on disk claims; `len` is the length the
 * recipe entry claims. For an in-place codec these are two different numbers
 * and both are load-bearing:
 *
 *   NONE   the payload IS the data, so they must be equal. If hdr < len the
 *          payload is shorter than the destination wants, and copying `len`
 *          bytes out of a `hdr`-byte buffer is a heap over-read whose tail is
 *          whatever happened to be allocated next -- not an error the codec
 *          can raise, because there is no codec involved.
 *   LZ4/   the frame must be non-empty and strictly smaller than the
 *   ZSTD    destination, or it is not a compressed-in-place segment.
 *
 * Whole-file blobs (JXL/APE/FLACR/...) keep csize unrelated to the logical
 * size -- the transcoder may be smaller OR larger -- so they are NOT checked
 * here; the serial loop applies that rule per-algo below.
 *
 * WP-read-parallel: this used to live ONLY in the serial loop, which made the
 * WP94 parallel fast path a second, weaker implementation of the same
 * contract -- it validated nothing and copied `e->length` for ALGO_NONE. Both
 * loops call this now, so "the fast path is a different implementation" cannot
 * rot back into "the fast path is a worse implementation".
 *
 * WP200: the SWEEP decodes stored segments too -- sweep_thread_worker() in
 * vol_sweep.c, one per segment, across a pool of threads -- and it had its own
 * copy of this contract, which was to have none of it. It is declared in
 * volume_internal.h and called from there for exactly the same reason: a
 * decoder that cannot see the check is free to skip it. */
int ast_frame_ok(uint8_t algo, uint32_t hdr, uint64_t len,
                 const char *algo_name)
{
    if (algo == INVFS_ALGO_NONE) {
        if (hdr != len) {
            fprintf(stderr, "raw segment header corrupt (csize %u, want %llu)\n",
                    hdr, (unsigned long long)len);
            return -1;
        }
    } else if (algo == INVFS_ALGO_LZ4 || algo == INVFS_ALGO_ZSTD) {
        if (hdr >= len || hdr == 0) {
            fprintf(stderr, "%s segment header corrupt (csize %u)\n",
                    algo_name, hdr);
            return -1;
        }
    }
    return 0;
}

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

        /* The frame must say what the recipe says it says, BEFORE the bytes
           are copied anywhere. Without this an ALGO_NONE entry whose frame is
           shorter than its declared length reads past the end of `blob`. */
        if (ast_frame_ok(e->algo, hdr, e->length,
                         e->algo == INVFS_ALGO_ZSTD ? "zstd" : "lz4") != 0) {
            free(blob);
            a->err = -1;
            return NULL;
        }

        if (e->algo == INVFS_ALGO_NONE) {
            memcpy(a->data + dst_off, blob, hdr);
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

/* WP120: the record name, resolved on first use.
 *
 * `rec_name` is read by exactly one family of lanes -- FLACR, TARR, GZR,
 * PNGR, EXR and the codecpack lane -- and only for two things: their
 * "name!sibling" lookups (name!recipe, name!coverN, name!partN, name!jxl,
 * name!exrN, name!mbrmap) and their error text. NONE/LZ4/ZSTD, PPMd text
 * batches and ZSTD binary batches -- the lanes that carry essentially every
 * byte a sweep or a read touches -- never look at it.
 *
 * On a v3 volume, though, OBTAINING it is not free: vol_v3_path_of() is a
 * reverse walk of the whole dirent tree, one vol_v3_dirent_scan() per
 * directory, each of which runs a vol_delta_range() and one
 * vol_delta_read_value() pread per delta-resident child (see the note on
 * vol_v3_path_of in vol_btree.c). Resolving it eagerly therefore made every
 * whole-file read cost O(number of inodes in the volume) -- and the sweep
 * reads every file whole exactly once (vol_sweep_one_v3), so its transform
 * stage grew superlinearly. On one content mix held constant, that stage
 * took 0.47 s at 1,017 inodes, 113 s at 4,270 and 2,086 s at 21,342 -- a
 * three-point log-log fit of T = 4.6e-9 * N^2.74, dominated by pread(2)
 * syscalls in vol_delta_read_value, ~S*G of them (S files transformed, G
 * delta records). With the name resolved on first use instead, the same
 * three sets transform in 0.065 s, 0.822 s and 0.785 s -- the stage is
 * flat in inode count and what remains is byte-proportional codec work.
 * The 46,245-inode reproducer, which had not finished in 40 minutes, then
 * ran transform to completion (46,438/46,438, 34m44s, swept=9109
 * skipped=12997 failed=0) and went on through dedupe.
 *
 * Two things these numbers do NOT say, recorded so they are not over-read.
 * The 2.74 exponent is quadratic-plus-cache-degradation, fitted to three
 * points it does not describe well (it over-predicts its own 21k point by
 * 1.6x), and it is an exponent on the pread COUNT -- the wall clock is worse
 * than the mechanism predicts, because each pread also gets dearer as the
 * working set outgrows the cache. And the 21k set is stride-sampled and
 * dominated by .pyc, which stage 3 defers to stage 6, so its collapse to
 * 0.785 s is not what a real rootfs does. The 46k volume is the honest case:
 * transform there is 34m44s, and 78% of that stage is now ZSTD compressing
 * and bit-exact-verifying 1.3 GiB of real files.
 *
 * So resolve it lazily. `pre` is a name the caller already has (the v2
 * record name, or a name handed down by a caller that resolved it); when it
 * is NULL the walk runs on first use, exactly as before -- the string, its
 * bytes and the truncated-prefix-on-overflow behaviour are unchanged, only
 * the timing moves. Single-threaded by construction: every recname_of()
 * caller is in the serial entry loop of vol_decode_ast_entries(), never in
 * decode_thread_worker(). */
typedef struct {
    invfs_volume *v;
    uint64_t      inode_id;
    const char   *pre;        /* caller-supplied name, or NULL to walk */
    char          buf[600];
    int           resolved;
} vol_recname;

static const char *recname_of(vol_recname *rn)
{
    if (!rn->resolved) {
        rn->resolved = 1;
        rn->buf[0] = 0;
        if (rn->pre && rn->pre[0]) {
            snprintf(rn->buf, sizeof rn->buf, "%s", rn->pre);
        } else if (rn->v) {
            /* return value ignored on purpose: vol_v3_path_of() leaves a
             * partial prefix in the buffer when the path does not fit, and
             * that is what the eager call handed the lanes. */
            (void)vol_v3_path_of(rn->v, rn->inode_id, rn->buf,
                                 sizeof rn->buf);
        }
    }
    return rn->buf;
}

/* ADR-010 amendment 2: `wins`/`n_wins` are the recipe's trailing window
 * table (NULL/0 for every recipe written before the change). Passing the
 * table in rather than re-parsing it per entry keeps the hot loop's cost at
 * one extra branch per entry. */
static int vol_decode_ast_entries(invfs_volume *v, uint64_t inode_id,
                                  vol_recname *rn,
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
                 * inode, so the serial loop's N touches net to exactly one).
                 *
                 * WP-heat-table-concurrent-safe: this comment used to end
                 * "and heat_tab_touch's table insert is not thread-safe --
                 * decode_thread_worker() must not call it". That was true
                 * about the WORKERS and wrong about everything it implied
                 * for the caller. "The calling thread" is NOT a
                 * serialization point: this runs inside vol_read_range,
                 * which invf_read calls with g_io_lock ALREADY RELEASED
                 * (src/cli/fuse_fs.c:1407 releases, :1414 calls, :3353
                 * fuse_loop_mt), so N FUSE read threads are in here at once
                 * and every one of them reaches this line. heat_tab_touch
                 * now takes heat_mu itself (src/core/vol_heat.c), so the
                 * hoist is about the DEDUPE (one touch, not N) and no longer
                 * about thread safety -- but the workers still must not call
                 * it, because the dedupe is by presence and a worker racing
                 * the caller's touch would fold one inode's N segments into
                 * whichever touch landed first. Skipping it left every read
                 * of a >=16-segment NONE/LZ4/ZSTD recipe at rheat=0, which
                 * starved heat promotion AND the WP25 tier migration
                 * (tier_heat_cb reads heat_file_r). */
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
                if (ast_frame_ok(e->algo, hdr, e->length,
                                 e->algo == INVFS_ALGO_ZSTD ? "zstd" : "lz4") != 0) {
                    free(blob); return -1;
                }
                /* whole-file blobs (JXL/APE/FLACR) keep csize unrelated to the
                   logical size: the transcoder may be smaller OR larger, so
                   ast_frame_ok deliberately does not judge them. */
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
                    /* whole file is one APE blob -> decode to flac.
                       WP101: the length must MATCH, it is not clamped. A
                       short decode used to be memcpy'd in and the tail of
                       the caller's buffer left at whatever it held, so a
                       truncated/corrupt APE blob was served as a partly
                       valid FLAC instead of failing. A long decode is an
                       overflow of `data + dst_off` and is refused too. */
                    uint8_t *fl = NULL;
                    size_t fl_len = 0;
                    if (invfs_ape_decompress(blob, hdr, &fl, &fl_len) != 0) {
                        fprintf(stderr, "APE decompress error\n");
                        free(blob); return -1;
                    }
                    if (fl_len != e->length) {
                        fprintf(stderr, "APE length mismatch (%s: got %zu, want %llu)\n",
                                recname_of(rn), fl_len, (unsigned long long)e->length);
                        free(fl); free(blob); return -1;
                    }
                    memcpy(data + dst_off, fl, fl_len);
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
                                recname_of(rn), m_len, (unsigned long long)e->length);
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
                        fprintf(stderr, "FLACR: APE->WAV failed for %s\n", recname_of(rn));
                        free(blob); return -1;
                    }
                    snprintf(rname, sizeof rname, "%s!recipe", recname_of(rn));
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
                        snprintf(cn, sizeof cn, "%s!cover%d", recname_of(rn), ci);
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
                        fprintf(stderr, "TARR: bad recipe for %s\n", recname_of(rn));
                        free(rcp_own); free(blob); return -1;
                    }
                    int np = tarx_recipe_num_parts(rcp, rcp_len);
                    uint8_t **parts = (uint8_t **)calloc(np > 0 ? (size_t)np : 1, sizeof(void *));
                    size_t *plens = (size_t *)calloc(np > 0 ? (size_t)np : 1, sizeof(size_t));
                    int ok = 1;
                    for (int pi = 0; pi < np; pi++) {
                        char pn[320];
                        snprintf(pn, sizeof pn, "%s!part%u", recname_of(rn), pi);
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
                        fprintf(stderr, "GZR: bad recipe for %s\n", recname_of(rn));
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
                        fprintf(stderr, "GZR: bad IVFT for %s\n", recname_of(rn));
                        free(rcp_own); free(blob); return -1;
                    }
                    int np = tarx_recipe_num_parts(ivft, ivft_len);
                    uint8_t **parts = (uint8_t **)calloc(np > 0 ? (size_t)np : 1, sizeof(void *));
                    size_t *plens = (size_t *)calloc(np > 0 ? (size_t)np : 1, sizeof(size_t));
                    int ok = 1;
                    for (int pi = 0; pi < np; pi++) {
                        char pn[320];
                        snprintf(pn, sizeof pn, "%s!part%u", recname_of(rn), pi);
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
                                        fprintf(stderr, "GZR: crc mismatch for %s\n", recname_of(rn));
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
                                recname_of(rn), rcp_len);
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
                        snprintf(jn, sizeof jn, "%s!jxl", recname_of(rn));
                        uint64_t jino = vol_find(v, jn);
                        if (!jino) {
                            fprintf(stderr, "PNGR: jxl '%s' missing\n", jn);
                            ok = 0;
                        } else if (invfs_png_from_jxl(v, jino, &rgb, &rgb_len) != 0) {
                            fprintf(stderr, "PNGR: djxl failed for %s\n", recname_of(rn));
                            ok = 0;
                        }
                    }
                    uint8_t *filt = NULL; size_t filt_len = 0;
                    uint8_t *stream = NULL; size_t stream_len = 0;
                    uint8_t *fl = NULL; size_t fl_len = 0;
                    if (ok && pngx_refilter(rgb, rgb_len, &pi, &filt, &filt_len) != 0)
                        ok = 0;
                    if (ok) {
                        /* deflate replica. Replay through the engine the
                         * sweep RECORDED (pi.enc), not through whatever
                         * deflateInit2 maps to now: the IDAT comes back
                         * bit-for-bit only from the same implementation
                         * that wrote it, and a PNG IDAT is a raw deflate
                         * stream (windowBits -15). dfc2b24 taught the
                         * _WIN32 transcode half this; the read path and the
                         * POSIX transcode half kept the old "pi.enc == 0
                         * means whatever this build links" assumption, so
                         * a PNG stamped on a zlib-ng host would have read
                         * back as EIO. The strategy comes from the recipe
                         * too: invfs_deflate_repro_find() may legitimately
                         * match a stream that needs Z_FILTERED, and a replay
                         * that assumed Z_DEFAULT_STRATEGY would produce a
                         * different stream and fail the length check. */
                        if (pi.enc == INVFS_DEFLATE_ENGINE_ZLIB_SYSTEM ||
                            pi.enc == INVFS_DEFLATE_ENGINE_ZLIB_STOCK) {
                            invfs_deflate_params dp;
                            memset(&dp, 0, sizeof dp);
                            dp.engine = pi.enc;
                            dp.level = (int8_t)pi.level;
                            dp.mem_level = (int8_t)pi.mem;
                            dp.strategy = (int8_t)pi.strategy;
                            dp.window_bits = -15;  /* PNG IDAT = raw deflate */
                            if (invfs_deflate_repro_encode(filt, filt_len, &dp,
                                                           &stream,
                                                           &stream_len) != 0)
                                ok = 0;
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
                        snprintf(pn, sizeof pn, "%s!exr%zu", recname_of(rn), pi);
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
                                recname_of(rn));
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
                        snprintf(mbn, sizeof mbn, "%s!mbrmap", recname_of(rn));
                        mapped = vol_find(v, mbn) != 0;
                    }
                    if (mapped) {
                        /* cpack_map_read returns the byte count (the
                         * vol_read_range convention): < 0 is the failure */
                        if (cpack_map_read(v, recname_of(rn), inode_id,
                                           (uint64_t)e->length,
                                           e->file_offset, data + dst_off,
                                           (size_t)e->length) < 0) {
                            fprintf(stderr, "%s: mapped container read "
                                    "failed for %s\n",
                                    pc ? pc->name : "codecpack", recname_of(rn));
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
                        if (pack_container_rebuild(v, pc, recname_of(rn),
                                                   blob, hdr,
                                                   data + dst_off,
                                                   (size_t)e->length) != 0) {
                            fprintf(stderr, "%s: container rebuild failed "
                                    "for %s\n", pc->name, recname_of(rn));
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
    invfs_v3_inode in;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;
    uint8_t *blob = NULL;
    size_t blen = 0;
    uint8_t *data = NULL;
    size_t len = 0;
    int rc;
    static const uint8_t zero_addr[INVFS_V3_RECIPE_ADDR_LEN];

    /* WP-M8: a v3 volume has no append-only record stream. The inode row in
     * the base tree carries a content-addressed recipe *reference*; fetch
     * and BLAKE3-verify the immutable blob, then decode its segments with
     * the exact same codec code (vol_decode_ast_entries).
     * An empty file (size 0 / no address) is the "no content" case.
     * WP-M11: vol_v3_inode_get and vol_v3_recipe_load resolve through the
     * delta overlay (delta first, then base), so a read observes the recent
     * tier without this function knowing about it. */
    rc = vol_v3_inode_get(v, inode_id, &in);
    if (rc != 1)
        return -1;
    /* The TYPE decides the shape of the content, before the address is
     * even looked at: a raw-blob type (a symlink -- the blob IS the
     * target string) is returned verbatim and never parsed as an AST.
     * The predicate is shared with the checkers (volume.h) precisely so
     * that neither side can drift from the other. */
    if (invfs_inode_content_is_raw_blob(in.type)) {
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
    const invfs_ast_window_entry *wins = NULL;
    uint32_t n_wins = 0;
    if (vol_ast_recipe_windows(blob, blen, &ah, &wins, &n_wins) != 0) {
        fprintf(stderr, "inode %llu: corrupt window table\n",
                (unsigned long long)inode_id);
        free(data); free(blob);
        return -1;
    }
    /* WP120: the name is resolved by the lanes that need it, not here --
     * resolving it here walked the whole dirent tree on every read. */
    {
        vol_recname rn;
        memset(&rn, 0, sizeof rn);
        rn.v = v;
        rn.inode_id = inode_id;
        if (vol_decode_ast_entries(v, inode_id, &rn, &ah, ents,
                                   wins, n_wins, data) != 0) {
            free(data); free(blob);
            return -1;
        }
    }
    free(blob);
    *out = data;
    *out_len = len;
    return 0;
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
    /* WP-M6: names resolve through the dirent tree / inode rows. The v2
     * alternative was one idx_get lookup -- and that index has been a no-op
     * returning NULL since WP-M21 retired it, so the v2 stat reported every
     * name as absent. */
    return vol_v3_path_stat(v, name, id_out, size_out, ctime_out);
}


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

    /* A raw-blob type -- a symlink -- has no segments to range over: its
     * content IS the target string, and the row's size is that string's
     * length. Answer it the way the authority does (vol_read_inode,
     * :1338: dispatch on the TYPE before recipe_addr is even looked at,
     * load, hand the blob back verbatim) rather than letting it fall
     * through to the AST parse below, which cannot succeed and returns
     * EIO. One file holding two answers for the same inode is how the
     * NEXT divergence starts.
     *
     * Unreachable through FUSE today -- the kernel resolves a symlink and
     * never routes read(2) on one to the daemon -- but every core caller
     * (the ranged-write fork in vol_write.c, the container lanes'
     * member source in vol_cpack.c, the sweep's header peek) reaches
     * this function by inode id alone, and none of them filters on type.
     *
     * The LOAD is the content here, so it is performed, not skipped. The
     * window is bounded by min(blen, in.size): the row's size is the
     * authority on how long the object is, and a blob longer than that
     * must not become readable past it. */
    if (invfs_inode_content_is_raw_blob(in.type)) {
        size_t avail;
        if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0)
            return -1;
        avail = (blen < in.size) ? blen : (size_t)in.size;
        if (offset >= avail) {
            free(blob);
            return 0;
        }
        if (len > avail - offset)
            len = (size_t)(avail - offset);
        memcpy(buf, blob + offset, len);
        free(blob);
        return (int)len;
    }

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
    /* WP-M9: a v3 file resolves through its content-addressed recipe blob
     * and decodes only the segments the window needs. The v2 alternative
     * located the inode record in the append-only inode area and walked it;
     * vol_open no longer admits a volume that has one. */
    return v3_read_range(v, inode_id, offset, len, (uint8_t *)buf);
}


