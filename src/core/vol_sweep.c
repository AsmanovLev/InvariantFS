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
                            const char *name,
                            invfs_sweep_file_progress_fn progress,
                            void *progress_user);


/* Give back everything the atomic sweep path allocated for the new record
 * before it gave up: the segments this run wrote sit in the entry table
 * (zone flipped to BINARY at write time); their extents derive from the
 * framed headers. Safe to call with ents == NULL (nothing written) or when
 * nothing was swept. Nothing has been made durable at this point -- no
 * record names the new pbas, no flush has happened -- so this is pure
 * in-memory bookkeeping plus bitmap bits, and the file is left exactly as
 * it was found. */



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
 * is an extent position, so a bound over the contiguous record region
 * rejected every hint and each lookup came back empty (the sweep saw
 * no files at all). The index hint is tried first, then the shared
 * extent-aware scan. Returns the record position and fills *h, or 0 if
 * absent. */
typedef struct {
    uint64_t want;
    uint64_t rec_pos;
    invfs_inode_rec h;
    char name[257];
    int found;
} sweep_locate_ctx;





int vol_sweep_file(invfs_volume *v, uint64_t inode_id)
{
    /* WP-M23: v3 volumes route directly to the id-keyed v3 sweep.
     * vol_sweep_one_v3 returns: 1 = swept, 0 = skipped/noop, -1 = err.
     * Returns: 0 = swept, 1 = skipped/noop, -1 = err. */
    int rc;
    if (!v || !inode_id) return -1;
    if (vol_write_active_id(v, inode_id))
        return 1;   /* skipped -- active write session */
    rc = vol_sweep_one_v3(v, inode_id, NULL, NULL, NULL);
    if (rc > 0) return 0;
    if (rc == 0) return 1;
    return -1;
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


/* WP14b candidate shape (WP10 §12.7 v2): every AST entry is a per-segment
 * generic store (BINARY zone, NONE/LZ4/ZSTD algo). An extraction container's
 * part ("name!partN") looks exactly like this before batching; a part
 * holding anything else (a whole-file JXL/APE blob, a batch member) is not
 * a candidate. 1 = the shape matches. */


/* `max_out` (WP103): 0 = the original rule -- the blob only has to be
 * smaller than the input, which is the right bar for a pack that SNIFFED the
 * file and is its only competitor on offer. Non-zero = the byte ceiling a
 * try-last pack must come in under (see sweep_lane_ref_size): a pack that
 * runs LAST still has to beat the engine lane, not merely the LZ4 the write
 * path left on disk. */
static int vol_pack_sweep(invfs_volume *v, uint64_t inode_id,
                          const char *name, const invfs_codec *pc,
                          const uint8_t *full, size_t full_len,
                          size_t max_out);


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
    if (!invfs_sweep_ui_active()) {
        if (n_bin)
            printf("  %s!*: %d parts -> ZSTD batch\n", name, n_bin);
        if (n_text)
            printf("  %s!*: %d parts -> PPMd batch\n", name, n_text);
    }
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

    rc = vol_pack_sweep(v, inode_id, name, jc, full, full_len, 0);
    free(full);
    return rc == 1 ? 0 : rc;   /* 1 = wait RAW (tools/space): keep waiting */
}


/* WP14b: exe-carve upgrade retry -- the EXER twin of vol_jxl_retry above,
 * for the same reason. vol_exer_carve stamps GENERIC_MEMLIMIT{EXER} on a
 * decode-policy refusal (vol_exer.c) and the caller then lets the generic
 * floor store the file, so by the next sweep the first zone is SHADOW and
 * the RAW-gated dispatch in vol_sweep_one_ex can never see it again: the
 * EXER entry is a builtin CONTAINER codec (codec.c), so the
 * "retry via the full path" break below is taken, the dispatch is skipped,
 * and the stamp is dead forever. This runs the SAME carve on the current
 * (reconstructed) bytes; the working-set admission inside vol_exer_carve
 * re-reads the live limit, so a raised limit admits the file and the carve
 * commits CONTAINER{EXER} + the exrN siblings. 11 = carved (the driver
 * reports the part count), 0 = still refused (vol_exer_carve re-stamped, or
 * nothing to carve: the next sweep retries), -1 only on a read failure. */
int vol_exer_retry(invfs_volume *v, uint64_t inode_id, const char *name)
{
    uint8_t *full = NULL;
    size_t full_len = 0;
    uint32_t nparts = 0;
    int rc;

    if (vol_read_file(v, inode_id, &full, &full_len) != 0) return -1;
    rc = vol_exer_carve(v, inode_id, name, full, full_len, &nparts);
    free(full);
    if (rc != 1) return 0;
    v->last_exer_parts = nparts;   /* invf-sweep reports the count */
    return 11;
}


/* WP78: shared per-file transcode decision. Both the v2 record path and the
 * v3 blob path read the whole file and hand it here; every bit-exactness
 * guard lives in the helpers this dispatches to (containerpack/pack sweeps
 * decode-back-and-memcmp, the builtin container builders carry their own
 * rebuild guards). `v3` selects v3-safe publication: on a v3 volume
 * vol_create_blob_file supersedes the inode row IN PLACE (same id), so the
 * caller must NOT vol_delete_inode() the old id afterwards. Returns
 * SWEEP_DECLINED when no lane claimed the content -- the caller then runs
 * its own generic floor. Never frees `full`; the caller owns it. */
#define SWEEP_DECLINED (-2)

/* WP103: the size the ENGINE's own lane would store this file at -- the bar
 * a try-last codecpack has to come in under, discounted by the gain guard.
 * Same codec the lanes use: the registry's ZSTD entry (zstdc_encode, level
 * 19) on BCJ-prefiltered bytes for the two x86 families, which is exactly
 * what tz_seal does to a binary member (vol_textzone.c:610, :937). The
 * comparison is therefore pack-whole-file vs lane-whole-file -- the same
 * comparison RESULTS.md measured (+6.8% for LZMA2+BCJ over BCJ+ZSTD-19 on
 * ELF, -13.9% for LZMA2 over PPMd8-z on text).
 *
 * Slightly strict for content that is neither text nor binary: those reach
 * the generic floor, which compresses PER SEGMENT, and a whole-file
 * reference can only be the smaller of the two. Strict is the safe side --
 * a pack that cannot clear the bar is declined and the file keeps the
 * builtin lane. Returns -1 when no reference could be produced (no ZSTD
 * entry, or the working buffers would not allocate): the caller declines
 * rather than admitting an unmeasured pack. */
static int sweep_lane_ref_size(const char *name, const uint8_t *full,
                               size_t full_len, size_t *out)
{
    const invfs_codec *zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
    const uint8_t *src = full;
    uint8_t *work = NULL, *enc = NULL;
    size_t cap, ref = 0;
    int bfam, rc = -1;

    if (!zc || !zc->encode || !full_len) return -1;
    bfam = invfs_binary_family(full, full_len, name);
    if (bfam == INVFS_BIN_FAMILY_ELF_X64 ||
        bfam == INVFS_BIN_FAMILY_ELF_X86) {
        work = (uint8_t *)malloc(full_len);
        if (!work) return -1;
        memcpy(work, full, full_len);
        invfs_bcj_x86_enc(work, full_len);
        src = work;
    }
    cap = ZSTD_compressBound(full_len);
    enc = (uint8_t *)malloc(cap);
    if (enc && zc->encode(src, full_len, enc, cap, &ref) == 0) {
        rc = 0;
        if (out) *out = ref;
    }
    free(enc);
    free(work);
    return rc;
}

static int sweep_dispatch(invfs_volume *v, uint64_t inode_id,
                          const char *name, const uint8_t *full,
                          size_t full_len)
{
    char rname[272], p0name[272], jn[272];
    int declined_algo = 0;   /* WP103: the pack the WP13 loop already tried */
    /* WP202: the address the row carries RIGHT NOW, captured before any lane
     * below can supersede it in place.
     *
     * Every builtin container lane ends in vol_create_*_file ->
     * vol_create_blob_file -> vol_v3_create_content_node, which reuses the
     * dirent's inode id and replaces the row: the moment the lane lands, the
     * recipe that named the ORIGINAL file's segments is unreachable, and
     * nothing frees it. vol_create_blob_file is handed the NEW address and
     * never the old one, so the capture has to happen here, at the call site
     * -- the same discipline (and the same reasoning) as the containerpack
     * commit at vol_cpack.c:3276-3282. */
    uint8_t old_addr[INVFS_V3_RECIPE_ADDR_LEN];
    memset(old_addr, 0, sizeof old_addr);

    invfs_v3_inode in;
    if (vol_v3_inode_get(v, inode_id, &in) == 1)
        memcpy(old_addr, in.recipe_addr, sizeof old_addr);

    if (!name || !name[0] || !full || full_len < 4)
        return SWEEP_DECLINED;

    /* WP59a: anchored files must never be claimed by a pack/container codec.
     * If the file reached the full path without a class stamp (first sweep
     * after creation), skip every codec/container branch and go straight to
     * the generic floor (builtin LZ4/ZSTD). The class stamp set by the
     * creation path prevents re-entry on subsequent sweeps. */
    if (invfs_inode_is_anchored(v, inode_id)) {
        vol_stamp_class(v, inode_id, INVFS_CLASS_ANCHORED, 0, 0);
        return SWEEP_DECLINED;
    }

    /* WP19: a write-hot file (rewritten at least twice inside the last
     * sweep interval -- wheat survives rewrites, see vol_replace_file)
     * churns too fast to amortize containers, transcodes or batching:
     * skip the heavy fan-out for this run and take the generic floor. */
    /* WP-heat-table-concurrent-safe: read the summary through its locked
     * accessor -- heat_any_whot is written by the lock-free read path. */
    if (heat_any_whot(v) && heat_file_maxw(v, inode_id) >= INVFS_WHEAT_HOT)
        return SWEEP_DECLINED;

    /* FLAC -> APE(PCM) + frame recipe (bit-exact) */
    if (full_len >= 4 && memcmp(full, "fLaC", 4) == 0) {
        snprintf(rname, sizeof rname, "%s!recipe", name);
        if (vol_find(v, rname) != 0) return 0;
        uint64_t nino = vol_create_flac_file(v, name, full, full_len);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_FLACR, tz_codec_gen(INVFS_ALGO_FLACR));
            return 0;
        }
        /* WP202: the lane SUPERSEDED the row in place, so the old recipe's
         * data segments are the lane's to release. */
        vol_v3_release_superseded_blob(v, inode_id, old_addr);
        vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                        INVFS_ALGO_FLACR, tz_codec_gen(INVFS_ALGO_FLACR));
        return 2;   /* FLAC */
    }

    /* TAR -> members + IVFT recipe (bit-exact) */
    if (full_len >= 512 && full[0] != 0 && full[0] != 1 &&
        memcmp(full + 257, "ustar", 5) == 0) {
        snprintf(p0name, sizeof p0name, "%s!part0", name);
        if (vol_find(v, p0name) != 0) return 0;
        uint64_t nino = vol_create_tar_file(v, name, full, full_len);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_TARR, tz_codec_gen(INVFS_ALGO_TARR));
            return 0;
        }
        /* WP202: the lane SUPERSEDED the row in place, so the old recipe's
         * data segments are the lane's to release. */
        vol_v3_release_superseded_blob(v, inode_id, old_addr);
        vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                        INVFS_ALGO_TARR, tz_codec_gen(INVFS_ALGO_TARR));
        defer_container_parts(v, name);   /* WP14b: batch parts this run */
        return 3;   /* TAR */
    }

    /* GZIP (tar.gz) -> members + IVGZ recipe (bit-exact deflate) */
    if (full_len >= 18 && full[0] != 0 && full[0] != 1 &&
        full[0] == 0x1F && full[1] == 0x8B) {
        snprintf(p0name, sizeof p0name, "%s!part0", name);
        if (vol_find(v, p0name) != 0) return 0;
        uint64_t nino = vol_create_gz_file(v, name, full, full_len);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_GZR, tz_codec_gen(INVFS_ALGO_GZR));
            return 0;
        }
        /* WP202: the lane SUPERSEDED the row in place, so the old recipe's
         * data segments are the lane's to release. */
        vol_v3_release_superseded_blob(v, inode_id, old_addr);
        vol_stamp_class(v, nino, INVFS_CLASS_CONTAINER,
                        INVFS_ALGO_GZR, tz_codec_gen(INVFS_ALGO_GZR));
        defer_container_parts(v, name);   /* WP14b: batch parts this run */
        return 4;   /* GZIP */
    }

    /* PNG -> JXL lossless + IVPN recipe (bit-exact) */
    if (full_len >= 33 && memcmp(full, "\x89PNG\r\n\x1a\n", 8) == 0) {
        snprintf(jn, sizeof jn, "%s!jxl", name);
        if (vol_find(v, jn) != 0) return 0;
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
                    return 0;
                }
            }
        }
        uint64_t nino = vol_create_png_file(v, name, full, full_len);
        if (!nino) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            INVFS_ALGO_PNGR, tz_codec_gen(INVFS_ALGO_PNGR));
            return 0;
        }
        /* WP202: the lane SUPERSEDED the row in place, so the old recipe's
         * data segments are the lane's to release. */
        vol_v3_release_superseded_blob(v, inode_id, old_addr);
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
        if (prc == 0 && pmp_len > 0 && pmp_len < full_len) {
            /* WP101: the round-trip guard. packMP3 is an EXTERNAL helper
             * (fork+exec) and nothing in its output format guarantees the
             * bytes decode back to the input: the read path can only check
             * that the length matches, so a packMP3 that mangles a frame
             * (or a swapped/hijacked helper binary) would be committed and
             * then served as if it were the original. Decode the blob back
             * and memcmp it against the original before anything is
             * replaced -- the same rule the LZ4/ZSTD/codecpack lanes apply.
             * A refusal is a DECLINE, not an error: the file stays RAW. */
            uint8_t *back = NULL;
            size_t back_len = 0;
            int exact = 0;
            if (invfs_pmp_decompress(pmp, pmp_len, &back, &back_len) == 0 &&
                back && back_len == full_len &&
                memcmp(back, full, full_len) == 0)
                exact = 1;
            free(back);
            if (!exact) {
                if (getenv("INVFS_DEBUG"))
                    fprintf(stderr, "[sweep] %s: PMP round-trip guard refused "
                                    "(decoded %zu of %zu) -- staying RAW\n",
                            name, back_len, full_len);
                free(pmp);
                vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                                INVFS_ALGO_PMP, tz_codec_gen(INVFS_ALGO_PMP));
                goto pmp_declined;
            }
            {
            uint64_t nino = vol_create_pmp_file(v, name, pmp, pmp_len,
                                                (uint64_t)full_len);
            free(pmp);
            if (!nino) return 0;
            /* WP202: on v3 the lane SUPERSEDED the row in place, so the old
             * recipe's data segments are the lane's to release; on v2 the row
             * still exists and the tombstone is the whole job. */
            vol_v3_release_superseded_blob(v, inode_id, old_addr);
                vol_stamp_class(v, nino, INVFS_CLASS_CODEC,
                            INVFS_ALGO_PMP, tz_codec_gen(INVFS_ALGO_PMP));
            return 8;   /* MP3 */
            }
        }
        free(pmp);
        vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                        INVFS_ALGO_PMP, tz_codec_gen(INVFS_ALGO_PMP));
    pmp_declined:
        /* fall through to the generic lanes: the MP3 stays RAW and is
         * retried when the codec generation rolls over */
        ;
    }

    /* WP16a: container codecpacks (manifest type=container) -- decompose a
     * container into "!mbrNNNN" member inodes that flow through the whole
     * normal pipeline. Placed AFTER every builtin container magic above
     * (ZIP/TAR/GZ/PNG/FLAC, and the MP3 codec branch) and BEFORE the WP13
     * whole-file codec-pack loop / text / generic. */
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
            if (prc >= 100) return prc;   /* decomposed */
            if (prc == 1) { deferred = 1; continue; }   /* defer: try next */
        }
        if (deferred) return 0;   /* a later sweep may claim */
    }

    /* WP13: codecpack codecs -- the registry's dynamic EXTERNAL entries, the
     * only ones carrying encode/decode trampolines (builtin externals have
     * NULL fn pointers and their own branches above). First sniff hit wins;
     * a declined pack stamps and falls through to text/generic.
     * WP103: `declined_algo` remembers which pack already had its turn on
     * this file, so the try-last loop further down (which runs after the
     * text and EXER branches) does not encode the same file with the same
     * pack twice. 0 = none declined. */
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
                    packless_claim = 1;
                continue;
            }
            prc = vol_pack_sweep(v, inode_id, name, pc, full, full_len, 0);
            if (prc == 1) return 0;   /* tool absent: defer */
            if (prc >= 100) return prc;   /* transcoded */
            packless_claim = 0;
            declined_algo = (int)pc->algo;
            break;   /* declined: stamps applied; text/generic still run */
        }
        if (packless_claim) return 0;   /* wait for the pack */
    }

    /* WP10 §4: every magic dispatch above declined -- classify text. Text
     * DEFERS into the sweep-run accumulator (sealed into shared PPMd batches
     * by vol_tz_flush at the end of the run); "!" sibling parts stay with
     * their container in v1 (WP10 §12.7).
     *
     * WP136: "stays with its container" is a narrower claim than "has a '!'",
     * and the narrow one is the true one. vol_name_is_container_sibling asks
     * whether this is a name a lane MINTED as part of a container's
     * decomposition, not whether the byte happens to occur in it -- so a user
     * file `a!b` that a pre-WP135 build left behind reaches this lane like
     * every other text file, instead of being excluded from every transform
     * the filesystem performs, silently and for ever. */
    if (!vol_name_is_container_sibling(v, name)) {
        int fam = invfs_text_family(name, full, full_len);
        if (fam > 0) {
            const invfs_codec *pc = invfs_codec_by_algo(INVFS_ALGO_PPMD);
            if (pc && pc->dec_mem_bytes > vol_get_dec_mem_limit(v)) {
                vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                                INVFS_ALGO_PPMD, pc->generation);
            } else if (tz_defer(v, inode_id, name, full_len,
                                (uint32_t)fam) == 0) {
                return 9;   /* text -> PPMd batch (deferred to flush) */
            }
        }
    }

    /* WP14b M2: exe-as-container carving, BEFORE the binary-batch
     * deferral -- a carved exe is strictly better than a batched one (the
     * embedded media gets a real codec, the glue still gets ZSTD-19).
     * '!'-sibling parts are never carved (WP10 §12.7) -- meaning the parts a
     * lane minted, which is what vol_name_is_container_sibling asks (WP136);
     * a MEMLIMIT refusal (rc 2) skips batching too. */
    int exer_no_bz = 0;
    if (!vol_name_is_container_sibling(v, name) &&
        invfs_binary_family(full, full_len, name) > 0) {
        uint32_t nparts = 0;
        int erc = vol_exer_carve(v, inode_id, name, full, full_len, &nparts);
        if (erc == 1) {
            v->last_exer_parts = nparts;   /* invf-sweep reports the count */
            return 11;   /* exe media -> JXL container */
        }
        exer_no_bz = (erc == 2);
    }

    /* ---- WP103: the try-last codecpack loop ---------------------------
     *
     * Placement is the whole point, so read it against the branches above:
     * a pack that scored 0 in the WP13 loop (no sniff.magic AND no
     * sniff.ext -- a `family = code` general codec) is offered a SECOND
     * chance here, which is AFTER the text branch (a text file has already
     * returned 9 to PPMd and never arrives) and AFTER the exe carve, and
     * BEFORE the binary ZSTD batch.
     *
     * Why after text: pack admission is claim-based. pack_sniff_impl
     * returns 100 on a magic, 50 on an extension, 0 otherwise, and the
     * WP13 loop breaks on the first hit, so a pack that sniffs `.c`/`.py`/
     * `.md` PRE-EMPTS the built-in text lane and PPMd is never tried. That
     * is a regression, not a lane: on the 12.2 MB text corpus measured in
     * /srv/bench/packs/RESULTS.md, PPMd8-z gets 4.684x against LZMA2's
     * 4.034x (-13.9%), and on web text 11.49x against 10.35x. Ranking by
     * claim can only ever say "packs win on text", which is the wrong
     * answer. Ordering by result says the right one.
     *
     * Why before binary, and not after: a general codec has no input magic
     * to claim (claiming the magic the pack itself emits would make the
     * sweep re-compress its own output), so without this block a pack is
     * simply DEAD on the class where it does win. Binary is that class --
     * LZMA2+BCJ 3.178x against the lane's BCJ+ZSTD-19 2.975x, +6.8% -- and
     * it is the only measured headroom left. Placing this after the binary
     * branch as well would leave the pack reachable only for content that
     * is neither text nor executable, and the change would prove nothing.
     *
     * Three rules make running last safe:
     *  - it must BEAT the engine lane by more than the gain guard, not
     *    merely beat the LZ4 sitting in RAW right now: the bar is
     *    sweep_lane_ref_size() discounted by INVFS_MIN_GAIN_PCT, checked
     *    in vol_pack_sweep's max_out. No reference, no admission.
     *  - being unavailable is NOT a claim. A pack with no trampolines, or
     *    whose tools do not resolve, is skipped, never deferred: a score-0
     *    pack matches EVERY file, so one under-built pack must not be able
     *    to freeze the whole binary lane on RAW.
     *  - nothing is ever half-applied. vol_pack_sweep encodes, decodes back
     *    and memcmps before it replaces anything, so a decline leaves the
     *    file exactly as found and the branch below still runs.
     *
     * '!'-sibling parts never get here (WP10 §12.7): a member belongs to
     * its container's batch, not to a whole-file pack. "Sibling" means one a
     * lane minted, not any name with a '!' in it (WP136) -- a user file whose
     * name happens to carry one gets this lane, and its bit-exactness guard
     * with it. */
    if (!vol_name_is_container_sibling(v, name)) {
        size_t cn = 0, ci;
        const invfs_codec *all = invfs_codec_all(&cn);
        size_t lane_ref = 0, bar = 0;
        int have_ref = 0;
        for (ci = 0; ci < cn; ci++) {
            const invfs_codec *pc = &all[ci];
            int prc;
            if (!(pc->caps & INVFS_CODEC_CAP_EXTERNAL) || !pc->sniff)
                continue;
            if (pc->sniff(full, full_len, name) != 0) continue;
            if ((int)pc->algo == declined_algo) continue;  /* had its turn */
            if (!pc->encode || !pc->decode) continue;      /* not a claim */
            if (!pc->probe || !pc->probe()) continue;     /* tools absent */
            /* A pack with claim rules that scored 0 above has ALREADY had
             * its turn: it looked at this file and said no. WP103's second
             * chance is for the claim-free general codecs, which score 0
             * because they have nothing to score with, not because they
             * recognised the file and declined. Without this, a pack can
             * reach a whole-file encode on a file it does not claim -- and
             * its refusal stamp then overwrites the one a lane that DID
             * claim the file wrote (see the stamp note at the call). */
            if (invfs_codec_pack_claims(pc)) continue;
            if (!have_ref) {
                /* the lane's own answer for this file, discounted by the
                 * gain guard -- measured once per file, not once per pack */
                double pct = vol_min_gain_pct();
                if (sweep_lane_ref_size(name, full, full_len, &lane_ref) != 0)
                    break;   /* unmeasurable: no pack can be admitted */
                bar = (size_t)((double)lane_ref * (1.0 - pct / 100.0));
                if (bar < 1) bar = 1;   /* 0 means "no ceiling" downstream */
                have_ref = 1;
            }
            prc = vol_pack_sweep(v, inode_id, name, pc, full, full_len, bar);
            if (getenv("INVFS_DEBUG_PACKS"))
                fprintf(stderr, "[packdbg] try-last %s on %s: lane=%zu "
                                "bar=%zu prc=%d\n", pc->name, name,
                        lane_ref, bar, prc);
            if (prc == 1) continue;   /* out of space: try the next one */
            if (prc >= 100) return prc;   /* transcoded, round-trip clean */
            /* declined: try the NEXT one. Unlike the claimed loop above
             * this must not break -- a score-0 pack that happens to be
             * registered first and LOSES (plain ZSTD-19 against the
             * BCJ+ZSTD-19 lane, say) would otherwise mask a pack that
             * wins, and registry order is not a quality ranking. The price
             * is one external encode per installed score-0 pack on a file
             * no pack claimed; if none of them can prove the win the file
             * falls through to the batch below, untouched. */
        }
    }

    /* WP14a: not text either -- executable binaries (ELF/PE/Mach-O by
     * magic, >= 4 KB) defer into the BINARY accumulator and are sealed
     * into shared ZSTD batches (x86 members BCJ-prefiltered first). As
     * above: a lane-minted sibling stays with its container (WP136). */
    if (!vol_name_is_container_sibling(v, name) && !exer_no_bz) {
        int bfam = invfs_binary_family(full, full_len, name);
        if (bfam > 0 &&
            bz_defer(v, inode_id, name, full_len, (uint32_t)bfam) == 0) {
            return 10;   /* binary -> ZSTD batch (deferred to flush) */
        }
    }

    return SWEEP_DECLINED;
}


/* WP78: the v3 generic floor stores the file as a whole-file ZSTD blob. A
 * class stamp a declined codec/container pack set during the dispatch
 * (GENERIC_MEMLIMIT / GENERIC_GUARD) carries retry semantics the gain
 * verdict knows nothing about, so it must win -- mirrors
 * vol_sweep_file_inner's rule. */
static void v3_stamp_generic(invfs_volume *v, uint64_t id, uint8_t cls,
                             uint8_t algo)
{
    uint8_t oc = 0, oa = 0;
    uint16_t og = 0;
    int havec = vol_get_class(v, id, &oc, &oa, &og) == 0;
    if (havec && (oc == INVFS_CLASS_GENERIC_MEMLIMIT ||
                  oc == INVFS_CLASS_GENERIC_GUARD))
        return;
    vol_stamp_class(v, id, cls, algo, invfs_registry_generation());
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
#define SWEEP_BATCH_SIZE 128
typedef struct {
    uint64_t old_pba;
    uint64_t old_plen;
    size_t orig_len;
    uint32_t csize_old;
    uint32_t old_algo;
    uint8_t *blob_old;

    /* Result */
    uint32_t csize_new;
    uint32_t algo_new;
    uint32_t crc_new;
    uint8_t cbuf[SEGMENT_SIZE + 128 + INVFS_BLOCK_SIZE];
    int valid;
} sweep_task_t;

struct sweep_worker_arg {
    sweep_task_t *tasks;
    size_t start;
    size_t end;
    int zlevel;
};

/* One superseded segment and the replacement written for it, held until the
 * new recipe is durable. See the transaction note at the remap loop. */
typedef struct {
    uint64_t old_pba;
    uint64_t old_plen;
    uint64_t new_pba;
    uint64_t new_plen;
} sweep_remap_t;

static void *sweep_thread_worker(void *arg_) {
    struct sweep_worker_arg *a = (struct sweep_worker_arg *)arg_;
    uint8_t orig[SEGMENT_SIZE];
    for (size_t k = a->start; k < a->end; k++) {
        if (!a->tasks[k].valid) continue;
        size_t orig_len = a->tasks[k].orig_len;

        /* WP200: the frame must say what the recipe says it says, BEFORE any
         * of the bytes are copied -- and the check is the read path's
         * ast_frame_ok(), not a fourth derivation of the same rule.
         *
         * This used to be a bare `memcpy(orig, blob_old, orig_len)` in the
         * else arm, where `blob_old` is a `csize_old`-byte allocation. When
         * the frame is shorter than the entry claims, that read runs off the
         * end of the heap block -- and unlike the read path's over-read,
         * whose damage ended at the caller's buffer, this one is WRITTEN
         * BACK: the over-read tail is recompressed, CRC'd, allocated and
         * stored as an ordinary segment. A corruption the read path refused
         * came back one sweep later as a file that reads successfully with
         * wrong bytes. Marking the task invalid is the same "leave this
         * segment exactly as it was found" answer the decompress-failure
         * arms below already give. */
        if (ast_frame_ok(a->tasks[k].old_algo, a->tasks[k].csize_old,
                         orig_len,
                         a->tasks[k].old_algo == INVFS_ALGO_ZSTD ? "zstd" : "lz4") != 0) {
            free(a->tasks[k].blob_old);
            a->tasks[k].blob_old = NULL;
            a->tasks[k].valid = 0;
            continue;
        }

        if (a->tasks[k].old_algo == INVFS_ALGO_LZ4) {
            int got = LZ4_decompress_safe((const char *)a->tasks[k].blob_old, (char *)orig,
                                          (int)a->tasks[k].csize_old, (int)orig_len);
            free(a->tasks[k].blob_old); a->tasks[k].blob_old = NULL;
            if (got != (int)orig_len) { a->tasks[k].valid = 0; continue; }
        } else if (a->tasks[k].old_algo == INVFS_ALGO_ZSTD) {
            size_t got = ZSTD_decompress(orig, orig_len, a->tasks[k].blob_old, a->tasks[k].csize_old);
            free(a->tasks[k].blob_old); a->tasks[k].blob_old = NULL;
            if (ZSTD_isError(got) || got != orig_len) { a->tasks[k].valid = 0; continue; }
        } else {
            memcpy(orig, a->tasks[k].blob_old, orig_len);
            free(a->tasks[k].blob_old); a->tasks[k].blob_old = NULL;
        }

        size_t zrc = ZSTD_compress(a->tasks[k].cbuf + 8, SEGMENT_SIZE + 64, orig, orig_len, a->zlevel);
        if (!ZSTD_isError(zrc) && zrc > 0 && zrc < orig_len) {
            a->tasks[k].csize_new = (uint32_t)zrc;
            a->tasks[k].algo_new = INVFS_ALGO_ZSTD;
        } else {
            a->tasks[k].csize_new = (uint32_t)orig_len;
            memcpy(a->tasks[k].cbuf + 8, orig, orig_len);
            a->tasks[k].algo_new = INVFS_ALGO_NONE;
        }
        a->tasks[k].crc_new = invfs_crc32c(a->tasks[k].cbuf + 8, a->tasks[k].csize_new);
        a->tasks[k].cbuf[0] = (uint8_t)(a->tasks[k].csize_new & 0xFF);
        a->tasks[k].cbuf[1] = (uint8_t)((a->tasks[k].csize_new >> 8) & 0xFF);
        a->tasks[k].cbuf[2] = (uint8_t)((a->tasks[k].csize_new >> 16) & 0xFF);
        a->tasks[k].cbuf[3] = (uint8_t)((a->tasks[k].csize_new >> 24) & 0xFF);
        a->tasks[k].cbuf[4] = (uint8_t)(a->tasks[k].crc_new & 0xFF);
        a->tasks[k].cbuf[5] = (uint8_t)((a->tasks[k].crc_new >> 8) & 0xFF);
        a->tasks[k].cbuf[6] = (uint8_t)((a->tasks[k].crc_new >> 16) & 0xFF);
        a->tasks[k].cbuf[7] = (uint8_t)((a->tasks[k].crc_new >> 24) & 0xFF);
    }
    return NULL;
}

static int vol_sweep_one_v3(invfs_volume *v, uint64_t inode_id,
                            const char *name,
                            invfs_sweep_file_progress_fn progress,
                            void *progress_user)
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
    if (!invfs_sweep_ui_active()) {
        fprintf(stderr, "[sweep-v3] checking inode %llu (%s)...\n",
                (unsigned long long)inode_id, name ? name : "NULL");
        fflush(stderr);
    }
    if (vol_write_active_id(v, inode_id))
        return 0;
    if (name && name[0] && vol_write_active_name(v, name))
        return 0;

    /* class policy: a stamp means drained-or-gated (subset of the WP10
     * table that needs no v2 record surgery) */
    if (vol_get_class(v, inode_id, &ccls, &calgo, &cgen) == 0 && ccls != 0) {
        switch (ccls) {
        case INVFS_CLASS_UNCOMPRESSIBLE:
            if (invfs_registry_generation() <= cgen)
                return 0;       /* retry only when the registry grew */
            break;
        case INVFS_CLASS_DEFER_ENOSPC:
            break;              /* space is a property of NOW: retry */
        case INVFS_CLASS_GENERIC_MEMLIMIT: {
            const invfs_codec *cc = invfs_codec_by_algo(calgo);
            /* WP14b: an EXER stamp is a carve refusal, and the file is
             * generic-stored behind it -- same trap as the JXL stamp below,
             * so take the retry before the container/map break. */
            if (calgo == INVFS_ALGO_EXER)
                return vol_exer_retry(v, inode_id, name);
            /* WP-M26 / WP16b: a seekable container with a map
             * (!mbrmap) doesn't need whole-file ARC buffering, so the
             * arc_budget gate never applied to it. If the stamp comes
             * from that path, retry unconditionally on the next sweep:
             * the new sweep just runs through the seekable MRMP path. */
            if (cc && cc->sniff && (cc->caps & INVFS_CODEC_CAP_CONTAINER)) {
                if (!invfs_codec_pack_def(cc) ||
                    invfs_codec_pack_def(cc)->map != NULL)
                    break;
            }
            if (!cc || (cc->dec_mem_bytes && cc->dec_mem_bytes > vol_get_dec_mem_limit(v)))
                return 0;
            /* WP78 / WP12(b): a JXL stamp lives behind generic storage now
             * -- the RAW-gated dispatch can never see it again, so retry
             * through the codecpack's own attempt on the current bytes. */
            if (calgo == INVFS_ALGO_JXL)
                return vol_jxl_retry(v, inode_id, name);
            break;              /* memory limit raised or now admits: retry */
        }
        case INVFS_CLASS_GENERIC_GUARD: {
            const invfs_codec *cc = invfs_codec_by_algo(calgo);
            if (!cc || cc->generation <= cgen)
                return 0;       /* retry only when the codec's gen grew */
            if (calgo == INVFS_ALGO_JXL)
                return vol_jxl_retry(v, inode_id, name);
            break;              /* new sub-encoder: retry via the dispatch */
        }
        case INVFS_CLASS_CONTAINER: {
            /* WP-Q2R3: a decomposition is stale when the pack that made it
             * has since bumped its generation. Migrate by re-deriving it
             * from the RECONSTRUCTED container stream (the stored map serves
             * the original bytes; the pack strips them again with its current
             * recipe format). Pack-agnostic -- see vol_cpack_migrate. */
            const invfs_codec *cc = invfs_codec_by_algo(calgo);
            const invfs_pack_def *cd = cc ? invfs_codec_pack_def(cc) : NULL;
            /* only packs that opt into the v2 map wire record a generation;
             * without the opt-in a v1 map means "no generations here" and the
             * decomposition is left alone (otherwise it would never settle) */
            if (name && name[0] && !vol_name_is_container_sibling(v, name) &&
                cc && cd &&
                cd->decomp_gen && cc->generation > cgen &&
                cpack_map_decomp_gen(v, name) < cc->generation) {
                int mrc = vol_cpack_migrate(v, inode_id, name, cc);
                if (mrc != 0) return mrc;
            }
            return 0;           /* current generation: already swept */
        }
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

    /* WP78: whole-file transcode dispatch -- the same decision the v2 path
     * runs (builtin containers, codec/container packs, text/binary batching,
     * EXER carve), so a v3 volume gets full sweep parity. Above the
     * whole-file memory cap only the container packs still get a head
     * sniff (their prior v3 behavior). */
    {
        uint64_t max_sweep_size = 256ULL * 1024ULL * 1024ULL;
        const char *env_max = getenv("INVFS_SWEEP_MAX_FILE");
        if (env_max && *env_max) {
            unsigned long long sz = strtoull(env_max, NULL, 10);
            if (sz > 0) max_sweep_size = sz;
        } else if (v->arc_budget && v->arc_budget < max_sweep_size) {
            max_sweep_size = v->arc_budget;
        }

        if (in.size <= max_sweep_size) {
            if (vol_read_inode(v, inode_id, 0, &full, &full_len) == 0 && full) {
                if (full_len == (size_t)in.size) {
                    int drc = sweep_dispatch(v, inode_id, name, full,
                                             full_len);
                    free(full);
                    full = NULL;
                    if (drc != SWEEP_DECLINED)
                        return drc;
                } else {
                    free(full);
                    full = NULL;
                }
            }
        } else if (name && name[0] &&
                   !vol_name_is_container_sibling(v, name)) {
            /* WP136, and the weaker of the two shapes this replaced. The old
             * test was `!strstr(name, "!mbrt") && !strstr(name, "!mbrmap")`:
             * a SUBSTRING match ANYWHERE in the name, which is not the
             * question at all. It says "internal" about `my!mbrt-backup.txt`
             * -- a user's file whose middle happens to spell a tag -- and it
             * says it without asking whether a container is there, so it also
             * says "internal" about `ghost.txt!mbrt` on a volume with no
             * `ghost.txt`. Both lose the containerpack head sniff on the
             * over-large path, silently, the same way the five lane guards
             * lost every lane.
             *
             * Own commit, deliberately. This is reachable only for a file
             * over INVFS_SWEEP_MAX_FILE, so it is not the same population as
             * the lane guards, and it can be reverted without touching them:
             * the two are not the same finding and are not the same blast
             * radius. */
            size_t cn = 0, ci;
            const invfs_codec *all = invfs_codec_all(&cn);
            uint8_t head[8192];
            int head_len = vol_read_range(v, inode_id, 0, sizeof head, head);
            if (head_len > 0) {
                for (ci = 0; ci < cn; ci++) {
                    const invfs_codec *pc = &all[ci];
                    const invfs_pack_def *pd;
                    int prc;
                    if (!pc->sniff || !(pc->caps & INVFS_CODEC_CAP_CONTAINER))
                        continue;
                    pd = invfs_codec_pack_def(pc);
                    if (!pd || !pd->is_container)
                        continue;
                    if (pc->sniff(head, (size_t)head_len, name) <= 0)
                        continue;
                    if (vol_read_inode(v, inode_id, 0, &full, &full_len) == 0 && full) {
                        prc = vol_containerpack_sweep(v, inode_id, name, pc, full, full_len);
                        free(full);
                        full = NULL;
                        if (prc >= 100) return prc;
                        uint8_t chk_cls = 0;
                        if (vol_get_class(v, inode_id, &chk_cls, NULL, NULL) == 0 &&
                            chk_cls == INVFS_CLASS_GENERIC_MEMLIMIT)
                            return 0;
                    }
                }
            }
        }
    }

    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;
    {
        /* A zero recipe address = a RAW inode whose segments have not been
         * published yet. A sibling re-created by THIS sweep (a container
         * decomposition commit, or a migration re-deriving one) lands exactly
         * here: there is nothing stored, so there is nothing to sweep. */
        static const uint8_t zero_addr[INVFS_V3_RECIPE_ADDR_LEN];
        if (memcmp(in.recipe_addr, zero_addr,
                   INVFS_V3_RECIPE_ADDR_LEN) == 0)
            return 0;
    }
    if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob)
        return -1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0) {
        free(blob);
        return -1;               /* a recipe that does not parse is corrupt */
    }
    if (!ents || n_ents == 0) {
        /* a RAW inode whose segments have not been written yet (a sibling
         * re-created by this same sweep, e.g. a decomposition migration):
         * nothing stored, nothing to sweep -- not an error */
        free(blob);
        return 0;
    }

    int zlevel = invfs_profile_zstd_level(v->profile);
    const char *env_zlvl = getenv("INVFS_SWEEP_ZSTD_LEVEL");
    if (env_zlvl && *env_zlvl) {
        int zl = atoi(env_zlvl);
        if (zl >= 1 && zl <= 22) zlevel = zl;
    }

    /* For multi-segment files: per-segment recompression preserves
     * segment boundaries for fast random I/O and enables intra- and
     * inter-file deduplication in vol_sweep_dedupe.
     * WP83: Parallel multi-threaded segment sweep with bounded batch size (constant O(1) RAM). */
    if (n_ents > 1) {
        invfs_ast_block_entry *new_ents = (invfs_ast_block_entry *)malloc(n_ents * sizeof(*new_ents));
        int any_swept = 0;
        uint64_t new_total = 0;   /* sum of rewritten segment csizes (+framing) */
        /* The remap is a TRANSACTION. Every replacement segment is written
         * before there is anywhere to name it -- vol_v3_recipe_store below is
         * the single point at which the file's new recipe becomes reachable,
         * and it is the one step here that can fail for want of space. So the
         * pairs are recorded, and the superseded segment is neither
         * un-refcounted nor freed until the publish has succeeded. */
        sweep_remap_t *remap = NULL;
        size_t remap_n = 0, remap_cap = 0;

        /* O(1) hash map for tracking remapped duplicate pbas within the file */
        size_t map_cap = 64;
        while (map_cap < n_ents * 2) map_cap *= 2;
        typedef struct { uint64_t old_pba; uint64_t new_pba; uint32_t algo; uint8_t zone; } pba_map_t;
        pba_map_t *pmap = (pba_map_t *)calloc(map_cap, sizeof(*pmap));

        if (!new_ents || !pmap) {
            free(blob); free(new_ents); free(pmap);
            return -1;
        }
        memcpy(new_ents, ents, n_ents * sizeof(*new_ents));
        pba_ref_ensure(v);

        /* Parallel worker pool context: process in bounded chunks (e.g. 128 segments = 8 MiB)
         * to strictly bound memory regardless of file size (handles 32GB files on 8GB RAM). */
        int num_threads = 6;
        const char *env_threads = getenv("INVFS_SWEEP_THREADS");
        if (env_threads && *env_threads) {
            int t = atoi(env_threads);
            if (t >= 1 && t <= 64) num_threads = t;
        }

        sweep_task_t *tasks = (sweep_task_t *)calloc(SWEEP_BATCH_SIZE, sizeof(*tasks));
        if (!tasks) {
            free(blob); free(new_ents); free(pmap);
            return -1;
        }

        for (size_t batch_start = 0; batch_start < n_ents; batch_start += SWEEP_BATCH_SIZE) {
            size_t batch_end = batch_start + SWEEP_BATCH_SIZE;
            if (batch_end > n_ents) batch_end = n_ents;
            size_t batch_len = batch_end - batch_start;

            /* 1. Filter and pre-read batch segments */
            for (size_t k = 0; k < batch_len; k++) {
                size_t i = batch_start + k;
                invfs_ast_block_entry *e = &new_ents[i];
                uint64_t old_pba = e->pba;
                tasks[k].valid = 0;

                if (e->zone != INVFS_ZONE_RAW || old_pba == 0)
                    continue;

                /* Check if already remapped */
                int already_mapped = 0;
                size_t h = (size_t)((old_pba * 11400714819323198485ull) & (map_cap - 1));
                while (pmap[h].old_pba) {
                    if (pmap[h].old_pba == old_pba) {
                        e->pba = pmap[h].new_pba;
                        e->zone = pmap[h].zone;
                        e->algo = pmap[h].algo;
                        already_mapped = 1;
                        any_swept = 1;
                        break;
                    }
                    h = (h + 1) & (map_cap - 1);
                }
                if (already_mapped)
                    continue;

                if (seg_extent_checked(v, old_pba, &tasks[k].old_plen) != 0)
                    continue;
                if (seg_read_checked(v, old_pba, tasks[k].old_plen, 1, &tasks[k].csize_old, &tasks[k].blob_old) != 0 || !tasks[k].blob_old)
                    continue;

                tasks[k].old_pba = old_pba;
                tasks[k].orig_len = (size_t)e->length;
                tasks[k].old_algo = e->algo;
                tasks[k].valid = 1;
            }

            /* 2. Parallel decompress and ZSTD recompress across threads using pthreads */
            if (num_threads <= 1 || batch_len < 4) {
                struct sweep_worker_arg single_arg = { tasks, 0, batch_len, zlevel };
                sweep_thread_worker(&single_arg);
            } else {
                int active_threads = num_threads;
                if ((size_t)active_threads > batch_len) active_threads = (int)batch_len;
                pthread_t th[64];
                struct sweep_worker_arg wargs[64];
                size_t per_th = (batch_len + active_threads - 1) / active_threads;

                for (int t = 0; t < active_threads; t++) {
                    wargs[t].tasks = tasks;
                    wargs[t].start = t * per_th;
                    wargs[t].end = (t + 1) * per_th;
                    if (wargs[t].end > batch_len) wargs[t].end = batch_len;
                    wargs[t].zlevel = zlevel;
                    /* th[t] MUST be initialised before pthread_create is
                     * called: on failure (EAGAIN under thread/memory
                     * pressure -- this box has OOMed) pthread_create does
                     * not write *th, so the join loop below would read
                     * stack garbage and pthread_join on it. That is UB
                     * and it faults: reproduced with pthread_create forced
                     * to EAGAIN, where the join consumed 0xdeadbeef...
                     * and the sweep died on SIGSEGV. */
                    th[t] = 0;
                    if (wargs[t].start < wargs[t].end &&
                        pthread_create(&th[t], NULL, sweep_thread_worker,
                                       &wargs[t]) != 0) {
                        /* No thread for this slice, so the work still has
                         * to happen -- run it on this one. Dropping it
                         * would silently leave those segments RAW, which is
                         * the same "a failure that does not surface" shape
                         * this branch is here to end. */
                        sweep_thread_worker(&wargs[t]);
                    }
                }
                for (int t = 0; t < active_threads; t++) {
                    if (th[t]) pthread_join(th[t], NULL);
                }
            }

            /* 3. Sequential allocate, write, refcount, and map update */
            if (progress)
                progress(progress_user, name, (uint64_t)batch_end,
                         (uint64_t)n_ents);
            if (!invfs_sweep_ui_active() &&
                (batch_start % 2560 == 0 || batch_end == n_ents)) {
                fprintf(stderr, "    [sweep] processed %zu / %zu segments (%.1f%%)...\n",
                        batch_end, n_ents, (batch_end * 100.0) / n_ents);
                fflush(stderr);
            }
            for (size_t k = 0; k < batch_len; k++) {
                if (!tasks[k].valid) continue;
                size_t i = batch_start + k;
                invfs_ast_block_entry *e = &new_ents[i];
                uint64_t old_pba = tasks[k].old_pba;
                uint64_t old_plen = tasks[k].old_plen;
                uint32_t csize_new = tasks[k].csize_new;

                uint64_t phys_blocks_new = ((uint64_t)csize_new + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
                uint64_t pba_new = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                                                phys_blocks_new, 1, INVFS_ALLOC_DATA);
                if (pba_new == 0)
                    continue;

                if (io_pwrite(&v->io, pba_new * INVFS_BLOCK_SIZE, tasks[k].cbuf, (size_t)csize_new + 8) != 0) {
                    vol_free_blocks(v, pba_new, phys_blocks_new);
                    continue;
                }

                /* TRANSACTION: record the pair, but do NOT touch old_pba's
                 * refcount and do NOT free it yet. Until
                 * vol_v3_recipe_store below succeeds the on-disk recipe
                 * still names old_pba, so freeing it here would leave the
                 * file reading from blocks the volume has already given
                 * away -- and a publish that then fails for want of space
                 * would strand every pba_new written so far as allocated,
                 * unreferenced garbage. Both are fixed by deferring the
                 * supersede to after the publish. */
                if (remap_n == remap_cap) {
                    size_t ncap = remap_cap ? remap_cap * 2 : 64;
                    sweep_remap_t *nr = (sweep_remap_t *)realloc(
                        remap, ncap * sizeof(*nr));
                    if (!nr) {
                        /* The +1 below never happened, so there is no
                         * refcount to undo -- only the block to give back. */
                        vol_free_blocks(v, pba_new, phys_blocks_new);
                        continue;   /* this segment stays RAW; the rest goes on */
                    }
                    remap = nr;
                    remap_cap = ncap;
                }
                remap[remap_n].old_pba = old_pba;
                remap[remap_n].old_plen = old_plen;
                remap[remap_n].new_pba = pba_new;
                remap[remap_n].new_plen = phys_blocks_new;
                remap_n++;

                pba_ref_modify(v, pba_new, +1);
                e->pba = pba_new;
                e->algo = tasks[k].algo_new;
                e->zone = INVFS_ZONE_BINARY;
                any_swept = 1;
                new_total += (uint64_t)csize_new + 8;

                size_t inh = (size_t)((old_pba * 11400714819323198485ull) & (map_cap - 1));
                while (pmap[inh].old_pba && pmap[inh].old_pba != old_pba)
                    inh = (inh + 1) & (map_cap - 1);
                pmap[inh].old_pba = old_pba;
                pmap[inh].new_pba = pba_new;
                pmap[inh].algo = e->algo;
                pmap[inh].zone = e->zone;
            }
        }

        free(tasks);
        free(pmap);
        free(blob);

        if (any_swept) {
            uint8_t *new_blob = NULL;
            size_t new_blen = 0;
            uint8_t new_addr[INVFS_V3_RECIPE_ADDR_LEN];
            int published = 0;
            /* WP sweep-rollback-test-conflict: this MUST stay ONE `&&`
             * condition, never a nested `if` whose `else` binds to the wrong
             * test. A merge bound the rollback branch to the SERIALIZE call
             * (which essentially never fails) instead of the STORE call, so a
             * failed publish fell out of the outer `if` having done nothing:
             * any_swept stayed 1, the function returned "swept", and every
             * block remap[] had recorded stayed allocated and named by
             * nothing -- d7ad082's defect, reintroduced, measured at 163
             * blocks on tools/test-sweep-publish-rollback.sh. `&&`
             * short-circuits, so a serialize failure AND a store failure both
             * land in the rollback below, which is what its comment has
             * always claimed. */
            if (vol_ast_recipe_serialize(in.size, new_ents, (uint32_t)n_ents, &new_blob, &new_blen) == 0 &&
                vol_v3_recipe_store(v, new_blob, new_blen, new_addr) == 0) {
                memcpy(in.recipe_addr, new_addr, sizeof(new_addr));
                vol_v3_inode_delta_put(v, inode_id, &in);
                published = 1;
                /* WP pba-ref-v3-incremental: the remap loop above moved
                 * the counts itself (-1 per retired pba, +1 per new
                 * one), so the map is exact again and the staleness
                 * vol_v3_inode_delta_put just flagged does not apply. */
                pba_ref_validate(v);
                /* WP78: the same gain verdict the v2 generic floor uses
                 * (the per-segment sum vs the file size), so a wholly
                 * incompressible multi-segment file is stamped
                 * UNCOMPRESSIBLE rather than GENERIC. */
                if ((double)new_total >=
                    (double)in.size * (1.0 - vol_min_gain_pct() / 100.0))
                    v3_stamp_generic(v, inode_id,
                                     INVFS_CLASS_UNCOMPRESSIBLE, 0);
                else
                    v3_stamp_generic(v, inode_id,
                                     INVFS_CLASS_GENERIC, INVFS_ALGO_ZSTD);
            }
            free(new_blob);

            if (published) {
                /* Structure before reference, in the only direction that is
                 * safe: the new recipe is durable, so the segments it
                 * replaced may go. The refcount test is the pre-existing
                 * share guard (a pba two live recipes still name is not
                 * freed), unchanged -- only its POSITION moved.
                 * WP sweep-rollback-test-conflict: this loop was dropped by
                 * the same merge, so a SUCCESSFUL publish stranded the
                 * superseded segments too -- the second half of the unclaimed
                 * growth that suite measures. */
                for (size_t r = 0; r < remap_n; r++) {
                    pba_ref_modify(v, remap[r].old_pba, -1);
                    if (pba_ref_count(v, remap[r].old_pba) == 0)
                        vol_free_blocks(v, remap[r].old_pba, remap[r].old_plen);
                }
            } else {
                /* The pass wrote %llu blocks and could not name a single one
                 * of them. Give them all back: nothing reachable points at
                 * them, the old recipe is intact, and leaving them allocated
                 * is a permanent leak -- measured at 152.2 MiB on a 0.5 GiB
                 * volume, unrecoverable by any later sweep or by
                 * invf-fsck -f, which is what wedged that volume into a
                 * user-visible ENOSPC on a write that needed 46 blocks. */
                uint64_t gave = 0;
                for (size_t r = 0; r < remap_n; r++) {
                    pba_ref_modify(v, remap[r].new_pba, -1);
                    vol_free_blocks(v, remap[r].new_pba, remap[r].new_plen);
                    gave += remap[r].new_plen;
                }
                fprintf(stderr,
                        "sweep: %s: the new recipe could not be published "
                        "(no room for the recipe blob or its metadata); "
                        "rolled back %llu rewritten block(s) across %llu "
                        "segment(s) and left the file RAW\n",
                        name ? name : "?", (unsigned long long)gave,
                        (unsigned long long)remap_n);
                any_swept = 0;   /* NOT swept: the caller must count it skipped */
            }
        }
        free(remap);
        free(new_ents);
        return any_swept ? 1 : 0;
    }

    free(blob);

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

    /* WP-B1: the profile ladder. invfs_profile_generic_algo() already states
     * the intent (turbo = store verbatim, fastest = LZ4, else ZSTD) and the v2
     * record writer honours it -- but this v3 blob path compressed with ZSTD
     * unconditionally, so on the default format INVFS_PROFILE=turbo wrote
     * LESS than balanced: the exact opposite of what the knob promises, and
     * silently so. The test that caught it is test-heat leg 3. */
    {
    uint32_t algo = (uint32_t)invfs_profile_generic_algo(v->profile);

    if (algo == INVFS_ALGO_NONE) {
        /* turbo: identity. No codec, no round-trip guard needed -- what goes
         * in is what comes out, which is the whole point of the profile. */
        newino = vol_v3_publish_blob_inode(v, inode_id, full, full_len,
                                           full_len, INVFS_ALGO_NONE);
        free(full);
        if (!newino)
            return 0;       /* store failed (ENOSPC): stay RAW */
        v3_stamp_generic(v, inode_id, INVFS_CLASS_GENERIC, INVFS_ALGO_NONE);
        return 1;
    }

    if (algo == INVFS_ALGO_LZ4) {
        int l4cap = LZ4_compressBound((int)full_len);
        uint8_t *dec = NULL;
        const invfs_codec *lc = invfs_codec_by_algo(INVFS_ALGO_LZ4);
        int l4n;
        if (l4cap <= 0 || !lc || !lc->decode) { free(full); return 0; }
        enc = (uint8_t *)malloc((size_t)l4cap);
        if (!enc) { free(full); return -1; }
        l4n = LZ4_compress_default((const char *)full, (char *)enc,
                                    (int)full_len, l4cap);
        if (l4n <= 0 || (size_t)l4n >= full_len) {
            free(enc);
            free(full);
            v3_stamp_generic(v, inode_id, INVFS_CLASS_UNCOMPRESSIBLE, 0);
            return 0;
        }
        dec = (uint8_t *)malloc(full_len ? full_len : 1);
        if (!dec || lc->decode(enc, (size_t)l4n, dec, full_len) != 0 ||
            memcmp(dec, full, full_len) != 0) {
            free(dec); free(enc); free(full);
            return 0;       /* the round-trip guard refused: stay RAW */
        }
        free(dec);
        newino = vol_v3_publish_blob_inode(v, inode_id, enc, (size_t)l4n,
                                           full_len, INVFS_ALGO_LZ4);
        free(enc);
        free(full);
        if (!newino)
            return 0;
        v3_stamp_generic(v, inode_id, INVFS_CLASS_GENERIC, INVFS_ALGO_LZ4);
        return 1;
    }

    zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
    enc_cap = ZSTD_compressBound(full_len);
    enc = (uint8_t *)malloc(enc_cap);
    if (!enc) { free(full); return -1; }
    size_t zrc = ZSTD_compress(enc, enc_cap, full, full_len, zlevel);
    if (ZSTD_isError(zrc) || zrc == 0 || zrc >= full_len || zrc > 0xFFFFFFFFu) {
        /* no gain or exceeds blob cap: generation-gated skip until the registry grows */
        free(enc);
        free(full);
        v3_stamp_generic(v, inode_id, INVFS_CLASS_UNCOMPRESSIBLE, 0);
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

    v3_stamp_generic(v, inode_id, INVFS_CLASS_GENERIC, INVFS_ALGO_ZSTD);
    return 1;
    }
}

/* process a single inode: container explode / transcode / shadow move.
   Returns 1 if the inode was replaced/transcoded, 0 if not applicable,
   -1 on hard error (caller keeps it pending or aborts). */
int vol_sweep_one_ex(invfs_volume *v, uint64_t inode_id, const char *name,
                      invfs_sweep_file_progress_fn progress, void *progress_user)
{
    uint64_t live = inode_id;
    /* A record can be REPLACED between the walk that collected the id
     * and this call: a container commit -- or a decomposition migration
     * re-deriving one, which re-creates every "!mbr*" sibling -- retires
     * the old inode under the same name. A stale id is not a hard error,
     * it is a name that now resolves elsewhere: re-resolve and sweep the
     * live record (which the next run would have done anyway). */
    if (name && name[0]) {
        uint64_t n = vol_find(v, name);
        if (n) {
            if (n != live && getenv("INVFS_DEBUG_PACKS"))
                fprintf(stderr, "sweep: %s: record replaced mid-sweep "
                                "(id %llu -> %llu), sweeping the live "
                                "one\n", name,
                        (unsigned long long)live,
                        (unsigned long long)n);
            live = n;
        }
    }
    return vol_sweep_one_v3(v, live, name, progress, progress_user);
}

int vol_sweep_one(invfs_volume *v, uint64_t inode_id, const char *name)
{
    return vol_sweep_one_ex(v, inode_id, name, NULL, NULL);
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
    return vol_sweep_file(v, inode_id);
}


/* Resolve the current name of an inode id for a sweep driver that has
 * only the id (vol_sweep_one wants the name: class policy, pack sniffing
 * and the live-session guard all key on it). 1 = found, 0 = deleted, -1 =
 * the name could not be resolved because the namespace walk STOPPED.
 * Same scan the pending drain has always used.
 *
 * WP135: `(rc > 0) ? 1 : 0` flattened that -1 into the "deleted" answer, so
 * an inode the walk simply could not reach was skipped with a comment that
 * said "deleted between collect and walk" -- which the operator then
 * believed, on a volume that was not being deleted from. */
int vol_sweep_name_of(invfs_volume *v, uint64_t id, char *nm, size_t cap)
{
    uint64_t parent;
    int rc;
    if (!v || !nm || cap == 0) return 0;
    /* WP-M18: the dirent tree, delta-first. The v2 alternative walked the
     * append-only inode area to find the record carrying the name. */
    rc = vol_v3_name_of(v, id, nm, cap, &parent);
    if (rc < 0) { nm[0] = 0; return -1; }
    return (rc > 0) ? 1 : 0;
}


/* drain the pending list (daemon background): process each pending inode
   and unmark it. Called when the daemon holds the volume exclusively
   (no open handles). Returns number of processed inodes. */
int vol_sweep_pending(invfs_volume *v)
{
    size_t n = v->n_pending;
    int done = 0;
    vol_heat_sweep_begin(v);   /* WP19: one decay pass per sweep run */
    while (n > 0) {
        uint64_t id = v->pending[0];
        /* find the current name for this inode (may have been deleted) */
        char nm[256];
        int found = vol_sweep_name_of(v, id, nm, sizeof nm);
        vol_unmark_pending(v, id);
        /* WP135: == 1, not "non-zero". vol_sweep_name_of propagates the -1
         * that a stopped v3 walk now produces, and a truthiness test would
         * have fed that -1 straight into vol_sweep_one with an EMPTY name --
         * a rewrite keyed on a name the volume could not supply. */
        if (found == 1) {
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
    if (!(v->sb.vol_flags & VOLF_READONLY)) {
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
 * text/generic, and the stamp carries the retry semantics). `max_out`
 * is the WP103 lane bar -- 0 keeps the "smaller than the input" rule. */
static int vol_pack_sweep(invfs_volume *v, uint64_t inode_id, const char *name,
                          const invfs_codec *pc, const uint8_t *full,
                          size_t full_len, size_t max_out)
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
        if (tool_tmpdir(dir, sizeof dir, (uint64_t)full_len) != 0) return 0;
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
        enc_len < full_len &&
        (!max_out || enc_len <= max_out)) {
        /* guard: the blob must decode back to the exact original bytes
         * before anything is replaced */
        back = (uint8_t *)malloc(full_len ? full_len : 1);
        if (back &&
            pc->decode(enc, enc_len, back, full_len) == 0 &&
            memcmp(back, full, full_len) == 0) {
            /* vol_v3_publish_blob_inode carries the inode row forward in
             * place; the v2 shape had to snapshot the ext and re-apply it
             * over a freshly created blob record. */
            uint64_t newino = vol_v3_publish_blob_inode(v, inode_id, enc,
                                                       enc_len, full_len,
                                                       pc->algo);
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
 * extent-aware record scan. Last record per name wins; torn records are
 * already skipped by the walker. An OOM aborts the walk and keeps what was
 * collected so far. */
typedef struct {
    sweep_seed *seen;
    size_t seen_n, seen_cap;
} sweep_seen_ctx;




/* WP-M21b: collector callback for the v3 live-set iterator. Internal
 * (\x01) rows and nameless rows are not sweep targets; the iterator has
 * already dropped deleted rows and de-duplicated nlink-shared inodes. */
struct v3_sweep_ids { uint64_t *ids; size_t max, n; size_t found; int full; };

static int collect_sweepables_v3_cb(invfs_volume *v, uint64_t id,
                                    const char *name, void *ctx)
{
    struct v3_sweep_ids *c = (struct v3_sweep_ids *)ctx;
    (void)v;
    if (!name || !name[0] || (unsigned char)name[0] == 0x01)
        return 0;
    /* Counted BEFORE the full test, so a collect that ran out of room leaves
     * found == n + 1: the excess is the proof that the list is a strict
     * subset of the volume. Counted after, a truncating call would be
     * indistinguishable from an exactly-full one -- and that difference is
     * the whole of the "swept a prefix and said nothing" defect. */
    c->found++;
    if (c->n >= c->max) {
        /* The callback's non-zero return is how the iteration STOPS, and
         * vol_v3_iter_live_inodes propagates a non-zero callback result as
         * its own (-1). So a buffer that simply filled up arrives at the
         * caller indistinguishable from a walk that failed -- which is
         * exactly the confusion this WP exists to end, one level down. The
         * flag says which it was, and the caller resolves it: a full buffer
         * is the normal state of the first pass of a growing collect. */
        c->full = 1;
        return 1;
    }
    c->ids[c->n++] = id;
    return 0;
}

/* WP135: the live-set collect, and the walk's STATUS.
 *
 * `*rc_out` is vol_v3_iter_live_inodes' own return value. It is a separate
 * out-param rather than being folded into the return because the return is a
 * count and callers legitimately want the count they got even on a short
 * walk -- what they must not do is mistake it for the whole set, and only
 * they can decide what a short list is worth. */
size_t vol_collect_sweepables_ex(invfs_volume *v, uint64_t *ids, size_t max,
                                 size_t *found_out, int *rc_out)
{
    struct v3_sweep_ids vc;
    int rc;

    vc.ids = ids; vc.max = max; vc.n = 0; vc.found = 0; vc.full = 0;
    /* WP135: this status was discarded, and the discard is the sharpest of
     * the five. vol_v3_iter_live_inodes returns -1 when the base scan
     * fails, and the two things that follow from that were both silent:
     *
     *   1. the DELTA pass is skipped entirely (vol_btree.c:5300 only runs
     *      it when the base scan came back 0), so the collect loses every
     *      inode created since the last fold; and
     *   2. vol_collect_sweepables_grow below sees found == n -- the walk
     *      stopped at the first failure, so it SAW exactly what it STORED
     *      -- and reads that as "the walk reached the end of the live set:
     *      COMPLETE", returning 0.
     *
     * The caller then prints an ordinary successful DONE line and rewrites
     * the subset it enumerated. AGENTS.md 2.5: the sweep is what drains RAW
     * and reclaims, so this is unbounded dead-segment accumulation behind a
     * clean log -- the exact failure 2.5 warns the operator about, reached
     * by a damaged volume rather than by a disabled worker. */
    rc = vol_v3_iter_live_inodes(v, collect_sweepables_v3_cb, &vc);
    /* *rc_out answers ONE question: did the walk fail for a reason of its
     * own? A stop this callback asked for (the buffer filled) is not that,
     * and reporting it as one would make every growing collect give up
     * after its first pass. Truncation is reported the other way, by
     * found > n, which is what the whole found/n contract is for. */
    if (rc_out) *rc_out = (rc != 0 && !vc.full) ? rc : 0;
    if (found_out) *found_out = vc.found;
    return vc.n;
}

size_t vol_collect_sweepables(invfs_volume *v, uint64_t *ids, size_t max)
{
    return vol_collect_sweepables_ex(v, ids, max, NULL, NULL);
}

int vol_collect_sweepables_grow(invfs_volume *v, uint64_t **ids_io, size_t *cap_io,
                                size_t *n_out, size_t *found_out)
{
    uint64_t *ids;
    size_t cap, n = 0, found = 0;
    vol_walk_t w;
    int rc = 0;

    if (!v || !ids_io || !cap_io || !n_out || !found_out) return -1;
    ids = *ids_io; cap = *cap_io;

    if (cap == 0) {
        cap = 4096;
        ids = (uint64_t *)malloc(cap * sizeof *ids);
        if (!ids) { *n_out = 0; *found_out = 0; return -1; }
    }
    vol_walk_init(&w, v, "vol_collect_sweepables");
    for (;;) {
        n = vol_collect_sweepables_ex(v, ids, cap, &found, &rc);
        /* rc, NOT the receipt's "complete". The two are not the same
         * question and conflating them is a bug this loop had once: on a
         * seed of one slot, found > n is the EXPECTED state of the first
         * pass -- that is what makes the buffer double -- and a receipt
         * that folds truncation into "complete" would read that as a walk
         * that stopped and give up after one entry. Truncation is handled
         * by the doubling below; only the walk's own status can stop it.
         *
         * The receipt is recorded ONCE, after the loop. A collect that
         * grows takes several passes and every one of the early ones is
         * truncated by construction; recording each would latch the volume
         * once per pass and discharge it once, and vol_close would then
         * report N unclaimed walks for a collect that was answered
         * correctly. The receipt is about the OUTCOME, and there is one
         * outcome. */
        if (rc != 0)
            break;                 /* the walk STOPPED: growing cannot help */
        if (found <= n) break;     /* reached the end: this is the live set */
        /* found > n: the buffer is full and the volume has more. Double
         * and walk again -- the same shape tz_v3_gc already uses
         * (vol_textzone.c:1569) and the same shape the offline sweep's own
         * collector uses (tools/invf-sweep.c:1203). */
        if (cap > (size_t)-1 / (2 * sizeof *ids)) { break; }
        {
            uint64_t *ni = (uint64_t *)realloc(ids, cap * 2 * sizeof *ni);
            if (!ni) break;            /* OOM: leave the subset we have */
            ids = ni; cap *= 2;
        }
    }
    *ids_io = ids; *cap_io = cap;
    *n_out = n; *found_out = found;
    vol_walk_result(&w, rc, n, found);
    /* WP135: -1 for a walk that did not finish, exactly as it already
     * returned -1 for a list it could not grow. Both are "this is a strict
     * subset of a set we cannot enumerate", and the caller has to say so
     * either way. Return 0 only for a list that IS the live set. */
    return vol_walk_commit(&w);
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
 * the WP40 extent-aware walker so statistics cover volumes whose records
 * live in dynamic meta extents. ctx carries the
 * claimed bitmap and the stats accumulator; rb INCLUDES the trailing
 * CRC and is owned by the walker (never freed here). */
typedef struct {
    invfs_volume *v;
    invfs_volume_stats *out;
    uint8_t *claimed;
    int have_ms;
} wstats_ctx;



/* WP-M21b: per-inode body of the v3 stats pass -- type straight from the
 * row (invfs_v3_inode.type), logical bytes from the row size. Internal
 * \x01 owners are excluded from the file count (the v2 body's rule).
 * WP78: a live recipe's zone==TEXT entries also feed the TEXT accounting
 * (logic = slice lengths, physical = each shared batch extent once), so the
 * text/binary batch zones show up in invf-stats like they do on v2. */
typedef struct {
    invfs_volume *v;
    invfs_volume_stats *out;
    uint8_t *claimed;
} wstats_v3_ctx;

/* Attribute one segment extent to a content class. Each physical block is
 * counted once no matter how many live recipes name it (a shared TEXT batch
 * is referenced by the owner and by every member), so the claimed bitmap --
 * not the zone tag -- is what makes the per-class totals add up. */
static void wstats_claim(invfs_volume *v, uint8_t *claimed, uint64_t pba,
                         uint64_t *phys)
{
    uint64_t plen = 0, b, bend;

    if (!claimed || !pba || pba >= v->sb.total_blocks)
        return;
    if (seg_extent(v, pba, NULL, &plen) != 0)
        return;
    bend = pba + plen;
    if (bend > v->sb.total_blocks)
        bend = v->sb.total_blocks;
    for (b = pba; b < bend; b++) {
        if (bit_get(claimed, b)) continue;
        bit_set(claimed, b);
        *phys += INVFS_BLOCK_SIZE;
    }
}

static int wstats_v3_cb(invfs_volume *v, uint64_t inode_id,
                        const char *name, void *ctx_)
{
    wstats_v3_ctx *c = (wstats_v3_ctx *)ctx_;
    invfs_volume_stats *out = c->out;
    invfs_v3_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t ne = 0, i;

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
    if (in.size == 0)
        return 0;
    if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob) {
        free(blob);
        return 0;
    }
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &ne) == 0 && ents) {
        for (i = 0; i < ne; i++) {
            if (ents[i].zone == INVFS_ZONE_TEXT) {
                out->logic_text_bytes += ents[i].length;
                wstats_claim(v, c->claimed, ents[i].pba,
                             &out->text_used_bytes);
            } else if (ents[i].zone == INVFS_ZONE_BINARY) {
                out->logic_shadow_bytes += ents[i].length;
                wstats_claim(v, c->claimed, ents[i].pba,
                             &out->shadow_used_bytes);
            } else {
                out->logic_raw_bytes += ents[i].length;
                wstats_claim(v, c->claimed, ents[i].pba,
                             &out->raw_used_bytes);
            }
        }
    }
    free(blob);
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
    wstats_v3_ctx vc;
    if (!v || !out) return -1;
    memset(out, 0, sizeof(*out));
    claimed = (uint8_t *)calloc((size_t)(v->sb.total_blocks + 7) / 8, 1);
    if (claimed)
        have_ms = 1;   /* degrade: physical zeros, not a lie */

    /* WP-M21b: population from the live-set iterator (type comes from the
     * inode row itself), physical from the zone bitmaps: the v3 data plane
     * still allocates RAW segments into the RAW zone and blob/drain segments
     * into Shadow, so region attribution is exact. The per-class physical
     * breakdown comes from the same recipe parse (each entry's zone tag over
     * its own pba, each block claimed once), so it is a CONTENT-CLASS measure
     * here exactly as the volume.h contract requires. */
    vc.v = v; vc.out = out; vc.claimed = claimed;
    (void)vol_v3_iter_live_inodes(v, wstats_v3_cb, &vc);
    /* raw/shadow/text_used_bytes are already CONTENT-CLASS measures,
     * filled by the callback above. Do NOT recompute them as a scan of
     * the raw/shadow REGIONS: zone boundaries are advisory and raw-class
     * blocks legitimately live past raw_zone_blocks (the contract in
     * volume.h says so). unclaimed is the one field that IS a region
     * measure by definition -- data-region blocks no live recipe names. */
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
