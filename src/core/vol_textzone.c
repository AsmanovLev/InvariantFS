/* vol_textzone.c — WP10 text-zone write path: accumulator, owner
 * inode, batch seal/commit/flush, text-zone GC (+ WP14a binary batches).
 * Split from volume.c. */

#include "volume_internal.h"
#include "vol_fault.h"   /* test-only seams (P0-2 red control) */

/* Test-only reload door for this TU's fault sites (same shape as the
 * btree/dirs doors). Inert in production: nothing calls it but a test. */
void invfs_vol_textzone_fault_reload(void)
{
    invfs_vol_fault_reload();
}

#define TZ_BATCH_MAX  (4ull << 20)    /* user-confirmed PPMd optimum (§3) */


/* one slice of one member file, inside one batch */
typedef struct {
    uint64_t batch_seq;
    uint64_t file_off;
    uint32_t batch_off;
    uint32_t len;
} tz_slice;


/* per-candidate build state while a flush runs */
typedef struct {
    tz_slice *slices;
    size_t n_slices, cap_slices;
    uint64_t file_size;      /* authoritative size when fed (fresh read) */
    uint64_t new_id;         /* allocated at commit (0 = not yet) */
    uint32_t algo;           /* batch algo its slices carry (PPMD / ZSTD /
                              * ZSTD_BCJ) -- WP14a */
    int bcj;                 /* member is x86-BCJ-prefiltered per slice */
    int complete;            /* all bytes of the candidate fed */
    int fallback;            /* its batch failed -> generic per-file path */
    int committed;           /* member record rewritten */
} tz_member;


/* a batch that made it to disk during this flush */
typedef struct {
    uint64_t seq, pba;
    uint32_t phys;
    uint32_t algo;           /* the batch payload codec tag (WP14a) */
} tz_sealed;


/* ---- accumulator (deferral side) ---- */

/* one accumulator is a (array, n, cap) triple on the volume: v->tz for
 * text candidates (WP10), v->bz for binary ones (WP14a). Same growth
 * and dedup rules for both. */
static int acc_defer(tz_candidate **arr, size_t *np, size_t *capp,
                     uint64_t inode_id, const char *name,
                     uint64_t size, uint32_t family)
{
    size_t i, nl = strlen(name);
    tz_candidate *nt;
    if (nl == 0 || nl > INVFS_MAX_NAME) return -1;
    for (i = 0; i < *np; i++)
        if ((*arr)[i].inode_id == inode_id) return 0;   /* already deferred */
    if (*np == *capp) {
        size_t nc = *capp ? *capp * 2 : 64;
        nt = (tz_candidate *)realloc(*arr, nc * sizeof(tz_candidate));
        if (!nt) return -1;
        *arr = nt;
        *capp = nc;
    }
    nt = &(*arr)[(*np)++];
    nt->inode_id = inode_id;
    nt->size = size;
    nt->family = family;
    memcpy(nt->name, name, nl + 1);
    return 0;
}

int tz_defer(invfs_volume *v, uint64_t inode_id, const char *name,
                    uint64_t size, uint32_t family)
{
    return acc_defer(&v->tz, &v->tz_n, &v->tz_cap, inode_id, name, size,
                     family);
}


/* WP14a: defer an executable (binary-family) file into the binary
 * accumulator; vol_tz_flush seals it into shared ZSTD(+BCJ) batches. */
int bz_defer(invfs_volume *v, uint64_t inode_id, const char *name,
                    uint64_t size, uint32_t family)
{
    return acc_defer(&v->bz, &v->bz_n, &v->bz_cap, inode_id, name, size,
                     family);
}


/* ---- owner inode ---- */

/* Load the owner record: entries, ext, position. Absent owner => *o zeroed
 * and o->pos = 0 (caller creates it). Returns 0 on success. */





/* Append [INOD(owner, entries)][DELT(position-kill previous)] as one write.
 * The owner keeps its inode id so the owner L2P maps stay valid. Entry
 * file_offsets are rebuilt as the cumulative concatenation of the given
 * entries (so the owner itself stays readable as the concatenation of its
 * live batches, and GC repacks the same way). The name is a parameter so
 * the WP20 seal owners ("\x01parity*") reuse the exact same pattern. */



/* Find the batch owner, creating it lazily (empty record) on first use. */



/* ---- small policy helpers used by the walk predicate ---- */

/* Cheap head sniff across the registry (UNCOMPRESSIBLE retry gate): one
 * 8 KB window, registry order (= sniff priority), any positive answer
 * qualifies. */
int tz_sniff_any(invfs_volume *v, uint64_t inode_id, const char *name)
{
    uint8_t head[8192];
    int got = vol_read_range(v, inode_id, 0, sizeof head, head);
    size_t n = 0, i;
    const invfs_codec *all;
    if (got <= 0) return 0;
    all = invfs_codec_all(&n);
    for (i = 0; i < n; i++)
        if (all[i].sniff && all[i].sniff(head, (size_t)got, name) > 0)
            return 1;
    return 0;
}


/* Read the usize of a TEXT member's batch segment without decoding it
 * (WP10 §6: "store decoded unit sizes to speed this up" -- the [4B usize]
 * sits right behind the 8-byte framing). 1 = batch exceeds unit_limit. */



/* Decode and re-store through the generic per-segment path, then stamp
 * cls{calgo}. Two callers:
 *  - policy downgrade (WP10 §6, both directions): stamps GENERIC_MEMLIMIT
 *    {codec} -- NOT bare GENERIC, because the limit is the reason, and
 *    MEMLIMIT is what makes the re-sweep after a RAISED limit find the
 *    file again (§2 table);
 *  - WP19 heat promotion: a read-hot PPMd batch member is extracted to
 *    standalone per-segment ZSTD and stamped GENERIC{ZSTD} (its terminal
 *    form -- a latency upgrade out of the shared batch, not a policy
 *    retry candidate).
 * The old record's blocks retire as usual; TEXT members' batch blocks
 * survive via the retire gate and their L2P dups vanish with the old
 * record (the batch keeps a hole -- GC/compactor territory). */



/* ---- flush: seal batches, commit members ---- */

typedef struct {
    invfs_volume *v;
    tz_owner owner;
    uint64_t owner_id;
    uint64_t next_seq;
    /* the batch currently being filled */
    uint8_t *bbuf;
    size_t blen, bcap;          /* bcap = batch target */
    uint64_t open_seq;
    /* WP14a: 0 = text flush (PPMd), 1 = binary flush (ZSTD[+BCJ]).
     * open_algo/open_bcj describe the batch currently being filled:
     * the first member of a batch sets the tone, and a member of the
     * other BCJ kind seals the open batch first (binary batches are
     * prefilter-homogeneous -- the (family,size) sort makes BCJ families
     * adjacent, so this split only ever fires at family boundaries). */
    int binary;
    uint32_t open_algo;         /* PPMD / ZSTD / ZSTD_BCJ */
    int open_bcj;
    /* sealed batches of this flush (for member-commit pba lookup) */
    tz_sealed *sealed;
    size_t n_sealed, cap_sealed;
    int sealed_any;
    int skip_commit;            /* INVFS_TZ_SKIP_COMMIT fault-injection hook */
} tz_ctx;


static int tz_slice_push(tz_member *m, uint64_t seq, uint64_t file_off,
                         uint32_t batch_off, uint32_t len)
{
    if (m->n_slices == m->cap_slices) {
        size_t nc = m->cap_slices ? m->cap_slices * 2 : 4;
        tz_slice *ns = (tz_slice *)realloc(m->slices, nc * sizeof(tz_slice));
        if (!ns) return -1;
        m->slices = ns;
        m->cap_slices = nc;
    }
    m->slices[m->n_slices].batch_seq = seq;
    m->slices[m->n_slices].file_off = file_off;
    m->slices[m->n_slices].batch_off = batch_off;
    m->slices[m->n_slices].len = len;
    m->n_slices++;
    return 0;
}





/* Rewrite one member's record under its pre-allocated new id (dup maps are
 * already written and flushed by tz_commit_ready) with TEXT-zone entries,
 * carrying the old record's ext (INO2 + xattrs) verbatim, then retire the
 * old id (its RAW blocks free normally -- the TEXT gate only covers batches)
 * and stamp the batching class (TEXT for PPMd batches, BATCHED_BIN for
 * WP14a binary batches). The entry algo comes from the sealed batch the
 * slice landed in: PPMD for text, ZSTD or ZSTD_BCJ for binary. */



/* Commit every complete, non-fallback member whose slices all live in
 * sealed batches. WP27: the member record carries the batch pba in each
 * entry, so there are no dup maps to write: the batch segment and the
 * owner record landed durable at seal time (vol_pre_record there), and
 * the bitmap was flushed before the member record names the batch (the
 * pre-commit flush below). */



/* cumulative decoded bytes the owner covers (= its readable size) */



/* Seal the open batch: encode (PPMd for text, ZSTD-19 for binary -- the
 * registry's zstd entry, mirroring the generic sweep level), MANDATORY
 * decode+memcmp verify (doc/06 invariant), size guard, write the segment
 * in the shadow zone, map it under the owner, flush the journal, append
 * the owner AST entry. On ANY refusal (encode/verify/guard/ENOSPC) the
 * batch is dropped unwritten and every member with a slice in it falls
 * back to the generic per-file path -- the same "not smaller, keep the
 * original" answer every other path gives.
 * Returns 0 on seal/fallback, -1 on hard error after the segment landed. */



/* stable ordering key: (family, size, original index) -- the doc/06 language
 * grouping; qsort is not stable, so the index breaks residual ties */
static const tz_candidate *g_sort_cands;

static int tz_order_cmp(const void *a, const void *b)
{
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    const tz_candidate *x = &g_sort_cands[ia], *y = &g_sort_cands[ib];
    if (x->family != y->family) return x->family < y->family ? -1 : 1;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    return ia < ib ? -1 : 1;
}


/* which stored classes may be re-batched at flush time (the candidate is
 * no longer RAW): the deferred retries (MEMLIMIT/UNCOMPRESSIBLE/GUARD
 * settled into generic storage) for both domains; plus, for the BINARY
 * accumulator, plain GENERIC -- the WP14a migration path re-batches
 * pre-existing per-segment-ZSTD binaries outright (binary batching shipped
 * with this build, so a GENERIC stamp on a binary-family file can only
 * predate it). Text keeps GENERIC terminal (WP10: no upgrade path). */
static int tz_flush_class_ok(int binary, uint8_t cc)
{
    if (cc == INVFS_CLASS_GENERIC_MEMLIMIT ||
        cc == INVFS_CLASS_UNCOMPRESSIBLE ||
        cc == INVFS_CLASS_GENERIC_GUARD)
        return 1;
    if (binary && cc == INVFS_CLASS_GENERIC)
        return 1;
    return 0;
}


/* Flush ONE accumulator (text: binary=0, WP10; binary: binary=1, WP14a).
 * The machinery is shared; what differs is exactly the codec (PPMd vs
 * ZSTD[+BCJ]), the class stamp, and the content re-validation. */



/* ===================================================================
 * WP78: v3 text/binary batching.
 *
 * A v3 volume has no v2 record stream and no owner-L2P map, but the read
 * path already serves a shared batch through an AST entry with
 * zone==INVFS_ZONE_TEXT that names the batch segment by pba (WP27), exactly
 * as on v2. So the v3 flush only has to (a) build the batch segments, (b)
 * rewrite each member's recipe with TEXT entries pointing at them, and
 * (c) keep a registry of live batches so a later GC can free the dead ones.
 * The registry is a hidden 0x01 file (TZ_OWNER_NAME) holding a flat array of
 * {seq, algo, pba, phys}; GC marks every pba referenced by a live recipe's
 * TEXT entries and frees the unmarked registry rows.
 * =================================================================== */

#define TZ_REG_MAGIC 0x33565a54u   /* "TZV3" */

static int tz_u64_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

typedef struct {
    uint32_t seq;
    uint32_t algo;
    uint64_t pba;
    uint32_t phys;
} tz_reg_ent;

static int tz_extent_cmp(const void *a, const void *b)
{
    uint64_t x = ((const tz_extent *)a)->pba,
             y = ((const tz_extent *)b)->pba;
    return x < y ? -1 : x > y ? 1 : 0;
}

typedef struct {
    tz_reg_ent *ents;
    size_t n, cap;
    uint32_t next_seq;
    uint64_t owner_id;
} tz_reg;

/* THREE ANSWERS, and the third one used to be the second one.
 *
 * `reg.n == 0` is a legitimate answer -- it means this volume has never
 * batched anything -- and 0 is the SUCCESS return. What 0 also used to mean
 * is "the lookup did not COMPLETE": vol_find returns a uint64_t, so "there is
 * no registry" and "the dirent row could not be read" are both the value 0.
 *
 * That collapse is not a bookkeeping slip here. A registry ROW is the only
 * record that a batch segment exists and who owns it (:270-291), and
 * tz_reg_store rewrites the WHOLE blob from the in-memory array. So a load
 * that answered "empty" on a failed read is handed straight to a flush that
 * seals this run's batches, appends them to the empty array and publishes a
 * blob containing THIS RUN'S ENTRIES ONLY. Every earlier row is gone, the
 * segments they owned have no record anywhere, tz_gc can never see them and
 * can never free them, and tz_reg_owned_blocks (:385 -- the owner set
 * spn_reclaim consults before freeing into the shared pool) stops claiming
 * their blocks too. Irreversible: the old blob is overwritten, not shadowed.
 *
 * A failed read is not entitled to assert a negative about the volume, so the
 * third answer now REFUSES. Every caller already handles a non-zero return as
 * failure -- :385, :601 and :824 are all `if (tz_reg_load(v, &reg) != 0)
 * return -1;` -- so refusing here aborts the flush BEFORE anything is sealed,
 * which is also the cheap place to refuse: a refused run orphans nothing.
 *
 * The twin site is tz_reg_store's own lookup (:422). It has to move with
 * this one: leaving it alone keeps the same loss reachable by the path where
 * the load succeeded and the later lookup did not. */
static int tz_reg_load(invfs_volume *v, tz_reg *r)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    int frc;

    memset(r, 0, sizeof *r);
    r->next_seq = 1;
    frc = vol_find_rc(v, TZ_OWNER_NAME, &r->owner_id);
    if (frc < 0) return -1;              /* did not COMPLETE: not "empty" */
    if (frc == 0) return 0;              /* genuinely absent: empty, fine */
    if (!r->owner_id) return 0;
    /* P0-2: a failed read is not entitled to assert a negative about the
     * volume. Every caller already treats nonzero as failure, so -1 here
     * aborts the flush before anything is sealed (see the header note).
     * The fault site is the red control (tz_reg_collapse_test). */
    if (invfs_vol_fault("tz_reg_blob_read")) return -1;
    if (vol_read_file(v, r->owner_id, &buf, &len) != 0 || !buf) {
        free(buf);
        return -1;
    }
    if (len >= 8) {
        uint32_t magic = 0, n = 0;
        memcpy(&magic, buf, 4);
        memcpy(&n, buf + 4, 4);
        if (magic != TZ_REG_MAGIC) {
            /* torn blob wearing a readable length: not a registry, not
             * empty -- refuse, same rule (P0-2). */
            free(buf);
            return -1;
        }
        {
            size_t avail = (len - 8) / sizeof(tz_reg_ent);
            if ((size_t)n > avail) {
                /* entry count overruns the blob: torn tail. Clamping
                 * would silently drop rows the store then forgets --
                 * refuse instead (P0-2). */
                free(buf);
                return -1;
            }
            if (n) {
                r->ents = (tz_reg_ent *)malloc((size_t)n * sizeof *r->ents);
                if (!r->ents) {
                    /* OOM masquerading as empty would orphan the same
                     * way (P0-2). */
                    free(buf);
                    return -1;
                }
                memcpy(r->ents, buf + 8, (size_t)n * sizeof *r->ents);
                r->n = r->cap = n;
            }
            for (size_t i = 0; i < r->n; i++)
                if (r->ents[i].seq >= r->next_seq)
                    r->next_seq = r->ents[i].seq + 1;
        }
    }
    free(buf);
    return 0;
}

/* The registry's block extents, ascending, for a caller outside this file
 * that has to decide whether a block is still claimed.
 *
 * WHY THIS EXISTS. A batch is dead the moment no live recipe names its pba --
 * but the registry ROW that owns the block is only retired later, by
 * tz_gc at sweep stage 6. Between those two moments the block is garbage
 * AND still named, and the free pool is shared with the metadata zone
 * (§2.3: one pool, no hard regions), so the very next mbuf_alloc can hand
 * the same block out as a base B+-tree page. A free path that consults only
 * recipes then frees a live metadata page through a row that has been stale
 * for the whole run: that is the v3 recipe-blob loss this closes.
 *
 * So the registry is published here as an owner set, and spn_reclaim
 * (vol_spt0.c) treats these blocks as claimed. tz_gc still frees the dead
 * batches -- it is the one owner that drops the row and the blocks together,
 * and it runs in the same sweep, so nothing leaks: the reclaim's debt on a
 * dead batch is simply paid one stage later instead of never. */
int tz_reg_owned_blocks(invfs_volume *v, tz_extent **out, size_t *n)
{
    tz_reg reg;
    tz_extent *arr = NULL;
    size_t i;

    if (!out || !n)
        return -1;
    *out = NULL;
    *n = 0;
    if (!v)
        return 0;
    if (tz_reg_load(v, &reg) != 0) {
        free(reg.ents);
        return -1;
    }
    if (reg.n) {
        arr = (tz_extent *)malloc(reg.n * sizeof *arr);
        if (!arr) {
            free(reg.ents);
            return -1;
        }
        for (i = 0; i < reg.n; i++) {
            arr[i].pba = reg.ents[i].pba;
            /* phys == 0 would be a row that names no block at all. Give it
             * the head block anyway: claiming one block we may not own is a
             * bounded leak, claiming none for a row that does own blocks is
             * the data loss this function exists to prevent. */
            arr[i].phys = reg.ents[i].phys ? reg.ents[i].phys : 1;
        }
        qsort(arr, reg.n, sizeof *arr, tz_extent_cmp);
    }
    free(reg.ents);
    *out = arr;
    *n = reg.n;
    return 0;
}

/* THE SAME THREE ANSWERS, on the write side, and it matters just as much.
 *
 * This is the second of the two sites, and it is the more dangerous of the
 * pair, because it runs LATER: by the time this executes the flush has already
 * sealed this run's batches and rewritten every member's recipe to point at
 * them. So the branch below is not "which of two equivalent ways to write the
 * blob" -- on the else arm it is `vol_create_blob_file`, which on v3 installs
 * an EMPTY node over the existing one (vol_dirs.c:299 keeps the inode id,
 * :340-344 zeroes the recipe address, :365-367 frees the old blocks) and then
 * publishes the new content. The earlier rows are not merged; they are
 * destroyed by a lookup that never completed.
 *
 * Refusing costs this run's registry write and reports an error to the sweep,
 * which is the honest outcome: the run sealed segments and committed members
 * against a registry it could not confirm, and the operator gets a message
 * instead of a silent, irreversible loss of every row written before. */
static int tz_reg_store(invfs_volume *v, tz_reg *r)
{
    size_t len = 8 + r->n * sizeof(tz_reg_ent);
    uint8_t *buf = (uint8_t *)calloc(1, len);
    uint32_t magic = TZ_REG_MAGIC, n = (uint32_t)r->n;
    uint64_t id = 0;
    int frc;

    if (!buf) return -1;
    memcpy(buf, &magic, 4);
    memcpy(buf + 4, &n, 4);
    if (r->n) memcpy(buf + 8, r->ents, r->n * sizeof *r->ents);
    frc = vol_find_rc(v, TZ_OWNER_NAME, &id);
    if (frc < 0) {
        free(buf);
        return -1;          /* the registry's own row could not be resolved */
    }
    if (frc == 1 && id) {
        if (!vol_publish_blob_inode(v, id, buf, len, len, INVFS_ALGO_NONE)) {
            free(buf);
            return -1;
        }
    } else {
        id = vol_create_blob_file(v, TZ_OWNER_NAME, buf, len, len,
                                  INVFS_ALGO_NONE);
        if (!id) { free(buf); return -1; }
    }
    free(buf);
    return 0;
}

/* Seal one batch: encode (PPMd for text, ZSTD for binary -- the BCJ
 * prefilter already ran per member slice), mandatory decode+memcmp guard,
 * size guard, write the segment in Shadow. 0 = sealed, 1 = refused
 * (fallback; no segment written), -1 = hard error. */
static int tz_seal(invfs_volume *v, int binary, const uint8_t *bbuf,
                      size_t blen, uint32_t algo, uint64_t seq,
                      tz_reg_ent *out)
{
    size_t cap = blen + blen / 2 + 4096;
    uint8_t *enc = NULL, *ver = NULL, *seg = NULL;
    size_t enc_len = 0;
    uint64_t pba = 0, phys = 0;
    uint32_t csize, crc, usize;
    uint8_t hdr[8];
    int fail = 0;

    if (blen == 0) return 1;
    enc = (uint8_t *)malloc(cap);
    ver = (uint8_t *)malloc(blen);
    if (!enc || !ver) fail = 1;
    if (!fail) {
        if (binary) {
            const invfs_codec *zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
            if (!zc || !zc->encode ||
                zc->encode(bbuf, blen, enc, cap, &enc_len) != 0)
                fail = 1;
            if (!fail && (zc->decode(enc, enc_len, ver, blen) != 0 ||
                          memcmp(ver, bbuf, blen) != 0))
                fail = 1;
        } else {
            if (invfs_ppmd_encode(bbuf, blen, enc, cap, &enc_len) != 0)
                fail = 1;
            if (!fail && (invfs_ppmd_decode(enc, enc_len, ver, blen) != 0 ||
                          memcmp(ver, bbuf, blen) != 0))
                fail = 1;
        }
    }
    /* the batch must beat storing the slices raw */
    if (!fail && enc_len + 4 >= blen) fail = 1;
    if (fail) { free(enc); free(ver); return 1; }

    /* payload = [4B usize LE][encoded]; standard [csize][crc] framing */
    usize = (uint32_t)blen;
    csize = (uint32_t)(4 + enc_len);
    phys = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    pba = alloc_blocks(v, v->sb.shadow_zone_start, v->sb.shadow_zone_blocks,
                       phys, 1, INVFS_ALLOC_DATA);
    if (!pba) { free(enc); free(ver); return 1; }
    seg = (uint8_t *)malloc((size_t)phys * INVFS_BLOCK_SIZE);
    if (!seg) { vol_free_blocks(v, pba, phys); free(enc); free(ver); return 1; }
    memcpy(seg + 8, &usize, 4);
    memcpy(seg + 12, enc, enc_len);
    crc = invfs_crc32c(seg + 8, csize);
    hdr[0] = (uint8_t)(csize & 0xFF);
    hdr[1] = (uint8_t)((csize >> 8) & 0xFF);
    hdr[2] = (uint8_t)((csize >> 16) & 0xFF);
    hdr[3] = (uint8_t)((csize >> 24) & 0xFF);
    hdr[4] = (uint8_t)(crc & 0xFF);
    hdr[5] = (uint8_t)((crc >> 8) & 0xFF);
    hdr[6] = (uint8_t)((crc >> 16) & 0xFF);
    hdr[7] = (uint8_t)((crc >> 24) & 0xFF);
    memcpy(seg, hdr, 8);
    if (write_segment_blocks(v, pba, seg, (size_t)csize + 8, phys) != 0) {
        vol_free_blocks(v, pba, phys);
        free(seg); free(enc); free(ver);
        return 1;
    }
    free(seg); free(enc); free(ver);
    out->seq = (uint32_t)seq;
    out->algo = algo;
    out->pba = pba;
    out->phys = (uint32_t)phys;
    if (getenv("INVFS_DEBUG"))
        fprintf(stderr, "[tz-v3] sealed %s batch %llu: %zu -> %zu bytes "
                        "(pba %llu)\n",
                binary ? "binary" : "ppmd", (unsigned long long)seq, blen,
                enc_len, (unsigned long long)pba);
    return 0;
}

/* Rewrite one member's v3 recipe with TEXT entries naming the sealed
 * batches. Returns 0 on success/soft-skip, -1 on hard error. */
static int tz_commit_member(invfs_volume *v, int binary,
                               const tz_candidate *cand, tz_member *m,
                               const tz_sealed *sealed, size_t n_sealed)
{
    invfs_ast_block_entry *ents;
    uint8_t *rblob = NULL;
    size_t rlen = 0, i, j;
    uint8_t addr[INVFS_RECIPE_ADDR_LEN];
    invfs_inode in;
    uint8_t old_addr[INVFS_RECIPE_ADDR_LEN];

    if (!m->complete || m->fallback || m->committed || !m->n_slices)
        return 0;
    ents = (invfs_ast_block_entry *)calloc(m->n_slices, sizeof *ents);
    if (!ents) return -1;
    for (i = 0; i < m->n_slices; i++) {
        const tz_sealed *s = NULL;
        for (j = 0; j < n_sealed; j++)
            if (sealed[j].seq == m->slices[i].batch_seq) { s = &sealed[j]; break; }
        if (!s) { free(ents); return 0; }   /* still feeding an open batch */
        memset(&ents[i], 0, sizeof ents[i]);
        ents[i].file_offset = m->slices[i].file_off;
        ents[i].length = m->slices[i].len;
        ents[i].zone = INVFS_ZONE_TEXT;
        ents[i].algo = s->algo;
        ents[i].block_id = (uint32_t)m->slices[i].batch_seq;
        ents[i].block_offset = m->slices[i].batch_off;
        ents[i].pba = s->pba;
    }
    if (vol_ast_recipe_serialize(m->file_size, ents, (uint32_t)m->n_slices,
                                 &rblob, &rlen) != 0) {
        free(ents);
        return -1;
    }
    free(ents);
    if (vol_recipe_store(v, rblob, rlen, addr) != 0) {
        free(rblob);
        return -1;
    }
    free(rblob);
    if (vol_inode_get(v, cand->inode_id, &in) != 1)
        return -1;
    memcpy(old_addr, in.recipe_addr, sizeof old_addr);
    in.size = m->file_size;
    memset(&in.recipe, 0, sizeof in.recipe);
    memcpy(in.recipe_addr, addr, sizeof addr);
    if (vol_inode_delta_put(v, cand->inode_id, &in) != 0)
        return -1;
    vol_free_recipe_blocks(v, old_addr, 0);
    if (binary)
        vol_stamp_class(v, cand->inode_id, INVFS_CLASS_BATCHED_BIN,
                        (uint8_t)m->algo, invfs_registry_generation());
    else
        vol_stamp_class(v, cand->inode_id, INVFS_CLASS_TEXT,
                        INVFS_ALGO_PPMD, invfs_registry_generation());
    m->committed = 1;
    return 0;
}

/* Flush ONE v3 accumulator. Same shape as tz_flush_one (sort by
 * (family,size), feed into batches, BCJ-homogeneous binary batches), but
 * publication is v3 recipe deltas + the batch registry. */
static int tz_flush_one_v3(invfs_volume *v, int binary)
{
    tz_candidate *acc = binary ? v->bz : v->tz;
    size_t n = binary ? v->bz_n : v->tz_n;
    tz_reg reg;
    tz_sealed *sealed = NULL;
    size_t n_sealed = 0, cap_sealed = 0;
    uint8_t *bbuf = NULL;
    size_t bcap, i, j, blen = 0;
    size_t *order = NULL;
    tz_candidate *sc = NULL;
    tz_member *members = NULL;
    uint32_t open_seq, open_algo;
    int open_bcj = 0, rc = 0, sealed_any = 0;

    if (n == 0) return 1;
    if (!vol_write_enabled(v)) {
        if (binary) v->bz_n = 0; else v->tz_n = 0;
        return 1;
    }
    if (tz_reg_load(v, &reg) != 0) return -1;
    open_seq = reg.next_seq;
    open_algo = binary ? INVFS_ALGO_ZSTD : INVFS_ALGO_PPMD;

    bcap = (size_t)TZ_BATCH_MAX;
    if (v->arc_budget && v->arc_budget / 2 < (uint64_t)bcap)
        bcap = (size_t)(v->arc_budget / 2);
    if (bcap < 4096) bcap = 4096;

    bbuf = (uint8_t *)malloc(bcap);
    order = (size_t *)malloc(n * sizeof *order);
    sc = (tz_candidate *)malloc(n * sizeof *sc);
    members = (tz_member *)calloc(n, sizeof *members);
    if (!bbuf || !order || !sc || !members) { rc = -1; goto out; }

    g_sort_cands = acc;
    for (i = 0; i < n; i++) order[i] = i;
    qsort(order, n, sizeof(size_t), tz_order_cmp);
    for (i = 0; i < n; i++) sc[i] = acc[order[i]];

    for (i = 0; i < n; i++) {
        tz_candidate *cand = &sc[i];
        tz_member *m = &members[i];
        uint8_t *data = NULL;
        size_t dlen = 0, off = 0;
        int z;

        if (vol_find(v, cand->name) != cand->inode_id) continue;
        z = vol_inode_first_zone(v, cand->inode_id);
        if (z != INVFS_ZONE_RAW) {
            uint8_t cc = 0, ca = 0;
            uint16_t cg = 0;
            if (z < 0) continue;
            if (vol_get_class(v, cand->inode_id, &cc, &ca, &cg) != 0) {
                if (z != INVFS_ZONE_BINARY || !strchr(cand->name, '!'))
                    continue;
            } else if (!tz_flush_class_ok(binary, cc))
                continue;
        }
        if (vol_read_file(v, cand->inode_id, &data, &dlen) != 0 || !dlen) {
            free(data);
            continue;
        }
        if (binary) {
            int bfam = invfs_binary_family(data, dlen, cand->name);
            if (bfam == 0) {
                vol_sweep_file(v, cand->inode_id);
                free(data);
                continue;
            }
            m->bcj = (bfam == INVFS_BIN_FAMILY_ELF_X64 ||
                      bfam == INVFS_BIN_FAMILY_ELF_X86);
            m->algo = m->bcj ? INVFS_ALGO_ZSTD_BCJ : INVFS_ALGO_ZSTD;
            if (blen && open_bcj != m->bcj) {
                tz_reg_ent re;
                int sr = tz_seal(v, binary, bbuf, blen, open_algo,
                                    open_seq, &re);
                if (sr < 0) { rc = -1; free(data); goto out; }
                if (sr == 0) {
                    if (n_sealed == cap_sealed) {
                        size_t nc = cap_sealed ? cap_sealed * 2 : 16;
                        tz_sealed *ns = (tz_sealed *)realloc(sealed,
                                            nc * sizeof *ns);
                        if (!ns) { rc = -1; free(data); goto out; }
                        sealed = ns; cap_sealed = nc;
                    }
                    sealed[n_sealed].seq = re.seq;
                    sealed[n_sealed].pba = re.pba;
                    sealed[n_sealed].phys = re.phys;
                    sealed[n_sealed].algo = re.algo;
                    n_sealed++;
                    sealed_any = 1;
                } else {
                    for (j = 0; j < n; j++)
                        for (size_t k = 0; k < members[j].n_slices; k++)
                            if (members[j].slices[k].batch_seq == open_seq)
                                members[j].fallback = 1;
                }
                open_seq++;
                blen = 0;
            }
        } else {
            if (invfs_text_family(cand->name, data, dlen) == 0) {
                vol_sweep_file(v, cand->inode_id);
                free(data);
                continue;
            }
            m->algo = INVFS_ALGO_PPMD;
        }
        m->file_size = dlen;
        while (off < dlen) {
            size_t take;
            if (blen == bcap) {
                tz_reg_ent re;
                int sr = tz_seal(v, binary, bbuf, blen, open_algo,
                                    open_seq, &re);
                if (sr < 0) { rc = -1; free(data); goto out; }
                if (sr == 0) {
                    if (n_sealed == cap_sealed) {
                        size_t nc = cap_sealed ? cap_sealed * 2 : 16;
                        tz_sealed *ns = (tz_sealed *)realloc(sealed,
                                            nc * sizeof *ns);
                        if (!ns) { rc = -1; free(data); goto out; }
                        sealed = ns; cap_sealed = nc;
                    }
                    sealed[n_sealed].seq = re.seq;
                    sealed[n_sealed].pba = re.pba;
                    sealed[n_sealed].phys = re.phys;
                    sealed[n_sealed].algo = re.algo;
                    n_sealed++;
                    sealed_any = 1;
                } else {
                    for (j = 0; j < n; j++)
                        for (size_t k = 0; k < members[j].n_slices; k++)
                            if (members[j].slices[k].batch_seq == open_seq)
                                members[j].fallback = 1;
                }
                open_seq++;
                blen = 0;
            }
            if (m->fallback) break;
            if (blen == 0) {
                open_bcj = m->bcj;
                open_algo = m->algo;
            }
            take = bcap - blen;
            if (take > dlen - off) take = dlen - off;
            if (tz_slice_push(m, open_seq, off, (uint32_t)blen,
                              (uint32_t)take) != 0) {
                rc = -1; free(data); goto out;
            }
            memcpy(bbuf + blen, data + off, take);
            if (m->bcj)
                invfs_bcj_x86_enc(bbuf + blen, take);
            blen += take;
            off += take;
        }
        free(data);
        m->complete = 1;
    }
    /* drain the partial batch */
    if (blen) {
        tz_reg_ent re;
        int sr = tz_seal(v, binary, bbuf, blen, open_algo, open_seq, &re);
        if (sr < 0) { rc = -1; goto out; }
        if (sr == 0) {
            if (n_sealed == cap_sealed) {
                size_t nc = cap_sealed ? cap_sealed * 2 : 16;
                tz_sealed *ns = (tz_sealed *)realloc(sealed, nc * sizeof *ns);
                if (!ns) { rc = -1; goto out; }
                sealed = ns; cap_sealed = nc;
            }
            sealed[n_sealed].seq = re.seq;
            sealed[n_sealed].pba = re.pba;
            sealed[n_sealed].phys = re.phys;
            sealed[n_sealed].algo = re.algo;
            n_sealed++;
            sealed_any = 1;
        } else {
            for (j = 0; j < n; j++)
                for (size_t k = 0; k < members[j].n_slices; k++)
                    if (members[j].slices[k].batch_seq == open_seq)
                        members[j].fallback = 1;
        }
        open_seq++;
        blen = 0;
    }
    /* commit every complete member whose slices all reference sealed batches */
    for (i = 0; i < n; i++) {
        if (tz_commit_member(v, binary, &sc[i], &members[i], sealed,
                                n_sealed) != 0) {
            rc = -1;
            goto out;
        }
    }
    /* persist the registry (append this run's sealed batches) */
    if (sealed_any) {
        for (i = 0; i < n_sealed; i++) {
            if (reg.n == reg.cap) {
                size_t nc = reg.cap ? reg.cap * 2 : 16;
                tz_reg_ent *ne = (tz_reg_ent *)realloc(reg.ents,
                                        nc * sizeof *ne);
                if (!ne) { rc = -1; goto out; }
                reg.ents = ne; reg.cap = nc;
            }
            reg.ents[reg.n].seq = (uint32_t)sealed[i].seq;
            reg.ents[reg.n].algo = sealed[i].algo;
            reg.ents[reg.n].pba = sealed[i].pba;
            reg.ents[reg.n].phys = sealed[i].phys;
            reg.n++;
        }
        if (tz_reg_store(v, &reg) != 0) rc = -1;
    }
out:
    for (i = 0; i < n; i++) {
        tz_member *m = &members[i];
        if (m->fallback && !m->committed)
            vol_sweep_file(v, sc[i].inode_id);
        free(m->slices);
    }
    free(order);
    free(sc);
    free(members);
    free(bbuf);
    free(sealed);
    free(reg.ents);
    if (binary) v->bz_n = 0;
    else        v->tz_n = 0;
    if (rc != 0) return rc;
    return sealed_any ? 0 : 1;
}

/* WP78: v3 batch GC. A batch is live iff some live recipe has a zone==TEXT
 * entry naming its pba; unmarked registry rows are freed. */
static int tz_gc(invfs_volume *v)
{
    tz_reg reg;
    uint64_t *ids = NULL;
    size_t cap = 4096, n_ids = 0, i, j;
    uint64_t *live = NULL;
    size_t n_live = 0, cap_live = 0;
    int freed = 0;

    if (tz_reg_load(v, &reg) != 0) return -1;
    if (reg.n == 0) { free(reg.ents); return 0; }
    if (!vol_write_enabled(v)) { free(reg.ents); return 0; }

    ids = (uint64_t *)malloc(cap * sizeof *ids);
    if (!ids) { free(reg.ents); return -1; }
    for (;;) {
        size_t got = vol_collect_sweepables(v, ids, cap);
        if (got < cap) { n_ids = got; break; }
        cap *= 2;
        uint64_t *ni = (uint64_t *)realloc(ids, cap * sizeof *ni);
        if (!ni) { free(ids); free(reg.ents); return -1; }
        ids = ni;
    }
    for (i = 0; i < n_ids; i++) {
        invfs_inode in;
        uint8_t *blob = NULL;
        size_t blen = 0;
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents = NULL;
        size_t ne = 0;
        if (vol_inode_get(v, ids[i], &in) != 1) continue;
        /* A symlink's blob is its target string, not an AST. What this
         * loop collects is ents[j].pba for zone==TEXT entries, and a
         * symlink has no entries at all -- it is a member id in
         * `ids` (it got there through the batch registry, which does
         * not discriminate by type), so "listed as a possible member"
         * and "names a shared batch segment" are different questions.
         * Stated with the predicate the read path dispatches on
         * (src/core/volume.h) rather than left to the parse failing on a
         * strlen-delimited target: this list feeds tz_gc's liveness
         * test, and a fabricated pba admitted here would keep an
         * unrelated batch segment alive -- or let a real one be freed
         * while it is still referenced. */
        if (invfs_inode_content_is_raw_blob(in.type)) continue;
        if (vol_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob) {
            free(blob);
            continue;
        }
        if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &ne) == 0 && ents) {
            for (j = 0; j < ne; j++) {
                if (ents[j].zone != INVFS_ZONE_TEXT || !ents[j].pba) continue;
                if (n_live == cap_live) {
                    size_t nc = cap_live ? cap_live * 2 : 64;
                    uint64_t *nl = (uint64_t *)realloc(live, nc * sizeof *nl);
                    if (!nl) { free(blob); free(ids); free(live); free(reg.ents); return -1; }
                    live = nl; cap_live = nc;
                }
                live[n_live++] = ents[j].pba;
            }
        }
        free(blob);
    }
    free(ids);
    if (n_live > 1)
        qsort(live, n_live, sizeof *live, tz_u64_cmp);
    {
        size_t w = 0;
        for (i = 0; i < reg.n; i++) {
            int keep = 0;
            if (live) {
                size_t lo = 0, hi = n_live;
                while (lo < hi) {
                    size_t mid = (lo + hi) / 2;
                    if (live[mid] < reg.ents[i].pba) lo = mid + 1;
                    else hi = mid;
                }
                keep = lo < n_live && live[lo] == reg.ents[i].pba;
            }
            if (keep) { reg.ents[w++] = reg.ents[i]; continue; }
            arc_invalidate(v->arc, reg.ents[i].pba | TZ_ARC_TAG);
            vol_free_blocks(v, reg.ents[i].pba, reg.ents[i].phys);
            freed++;
        }
        reg.n = w;
    }
    if (freed > 0 && tz_reg_store(v, &reg) != 0) freed = -1;
    free(live);
    free(reg.ents);
    return freed;
}

int vol_tz_flush(invfs_volume *v)
{
    int rt, rb;

    if (!v) return -1;
    /* WP78: there are no owner records, but the read path serves a
     * TEXT-zone AST entry identically, so the same accumulators flush into
     * recipe deltas + a hidden batch registry. */
    rt = tz_flush_one_v3(v, 0);
    rb = tz_flush_one_v3(v, 1);
    if (rt < 0 || rb < 0) return -1;
    return (rt == 0 || rb == 0) ? 0 : 1;   /* 0 = something sealed */
}


size_t vol_acc_pending(const invfs_volume *v, int binary)
{
    if (!v) return 0;
    return binary ? v->bz_n : v->tz_n;
}


/* u32 comparator for the GC mark set */



/* WP47: GC mark pass over the shared mapper-aware walker. The legacy
 * contiguous loop saw no records on a v0.3.0+ mapper volume, so the mark
 * set came back empty and GC would free every live batch. The callback
 * keeps the exact legacy policy: skip tombstones and the owner record,
 * only the LIVE record of a name marks, and TEXT-zone entries contribute
 * their batch_seq (block_id). */
typedef struct {
    invfs_volume *v;
    uint64_t owner;
    uint32_t *live;
    size_t n_live, cap_live;
    int oom;
} tz_gc_mark_ctx;



/* Text-zone GC (WP10 §7): mark-and-sweep over the owner AST. A batch is
 * live iff at least one LIVE member record has a zone==TEXT entry naming
 * its batch_seq (marking by block_id rather than pba keeps the mark set
 * correct even if a member's L2P dup is damaged -- the dup only maps the
 * name, it does not define liveness). Owner entries outside the mark set
 * are dead: invalidate the tagged ARC key, free the blocks (the retire-time
 * TEXT gate does not apply here -- freeing is the whole point), drop the
 * owner L2P map, and rewrite the owner record without them.
 * Returns the number of dead batches reclaimed, 0 = nothing, <0 = error. */
int vol_tz_gc(invfs_volume *v)
{
    if (!v) return -1;
    return tz_gc(v);
}
