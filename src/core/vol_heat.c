/* vol_heat.c — WP27 heat counters + decay + promotion.
 *
 * Heat lives in the record's INO2 ext as the "invfs.heat" xattr TLV
 * (invarifs.h): [u16 LE rheat][u8 wheat][u8 reserved]. It moved out of the
 * L2P journal pads with format v2 -- the journal is the owner-scoped WAL
 * now, and the read path never touches it.
 *
 * Semantics (unchanged from WP19):
 *  - read:  +1 the FIRST time an inode is touched by a vol_read_* path
 *           within this process (open-session == process lifetime, tracked
 *           in v->heat_tab). Saturates at 0xFFFF. Persistence: the accrual
 *           is RAM-only; it folds into the record's TLV at vol_close and
 *           at the sweep's decay pass. A crash loses pending touches --
 *           heat is advisory.
 *  - write: a fresh record is born wheat 0 (an absent TLV reads as
 *           (0,0) -- "no heat history"); a rewrite carries max(old)+1
 *           into the replacement record (vol_write_commit /
 *           vol_replace_file). Saturates at 0xFF. (v1's pads were born
 *           wheat 1; the carry is old+1 either way, so the observable
 *           chain is identical, and absent-as-zero means the decay pass
 *           never stamps a never-heated file -- zero churn on cold
 *           volumes, which matters now that a stamp is a record append.)
 *
 * Decay (vol_heat_sweep_begin, once per sweep RUN): the session's accrued
 * reads fold in, then rheat >>= 1, wheat -= 1 (floor 0), persisted into
 * the records (a sweep rewrites records anyway). Hysteresis math: the
 * promotion check runs AFTER the decay, so a single read burst of H
 * promotes iff H survives exactly one halving (H >= 2*HOT); sustained
 * reading of R touches-per-interval stabilises rheat at R. HOT=8: one hot
 * weekend (16+ opens) promotes once, casual 8..15-open bursts decay away.
 * Write-hot (wheat >= 2 post-decay = rewritten at least twice inside the
 * last interval) skips the heavy codec fan-out for one sweep.
 */

#include "volume_internal.h"

/* 64-bit mix (splitmix64 finalizer) for the per-inode tables */
static uint64_t idx_mix_heat(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}


/* per-inode accrued read touches this session. heat_tab doubles as the
 * once-per-session seen set: presence == counted. On allocation failure a
 * touch is silently dropped -- a colder file, never a wrong one. */
static int heat_tab_touch(invfs_volume *v, uint64_t inode)
{
    size_t mask, i, j;
    if (!v->heat_tab) {
        v->heat_tab = (uint64_t (*)[2])calloc(256, sizeof *v->heat_tab);
        if (!v->heat_tab) return 1;
        v->heat_tab_mask = 255;
    } else if ((v->heat_tab_n + 1) * 10 >= (v->heat_tab_mask + 1) * 7) {
        size_t nc = (v->heat_tab_mask + 1) * 2;
        uint64_t (*ns)[2] = (uint64_t (*)[2])calloc(nc, sizeof *ns);
        if (!ns) return 1;
        for (j = 0; j <= v->heat_tab_mask; j++) {
            if (v->heat_tab[j][0]) {
                size_t k = (size_t)(idx_mix_heat(v->heat_tab[j][0])) & (nc - 1);
                while (ns[k][0]) k = (k + 1) & (nc - 1);
                ns[k][0] = v->heat_tab[j][0];
                ns[k][1] = v->heat_tab[j][1];
            }
        }
        free(v->heat_tab);
        v->heat_tab = ns;
        v->heat_tab_mask = nc - 1;
    }
    mask = v->heat_tab_mask;
    i = (size_t)idx_mix_heat(inode) & mask;
    while (v->heat_tab[i][0]) {
        if (v->heat_tab[i][0] == inode)
            return 1;   /* already counted this session */
        i = (i + 1) & mask;
    }
    v->heat_tab[i][0] = inode;
    v->heat_tab[i][1] = 1;
    v->heat_tab_n++;
    return 0;
}

/* accrued reads of `inode` this session (0 when none) */
static uint16_t heat_tab_get(const invfs_volume *v, uint64_t inode)
{
    size_t i;
    if (!v->heat_tab) return 0;
    i = (size_t)idx_mix_heat(inode) & v->heat_tab_mask;
    while (v->heat_tab[i][0]) {
        if (v->heat_tab[i][0] == inode)
            return v->heat_tab[i][1] > 0xFFFF ? 0xFFFF
                                              : (uint16_t)v->heat_tab[i][1];
        i = (i + 1) & v->heat_tab_mask;
    }
    return 0;
}

/* remove + return the session's accrued reads of `inode` (the write-commit
 * carry transfers the old id's pending touches onto the replacement) */
static uint16_t heat_tab_take(invfs_volume *v, uint64_t inode)
{
    size_t i;
    uint16_t r;
    if (!v->heat_tab) return 0;
    i = (size_t)idx_mix_heat(inode) & v->heat_tab_mask;
    while (v->heat_tab[i][0]) {
        if (v->heat_tab[i][0] == inode) {
            r = v->heat_tab[i][1] > 0xFFFF ? 0xFFFF
                                           : (uint16_t)v->heat_tab[i][1];
            v->heat_tab[i][0] = 0;
            v->heat_tab[i][1] = 0;
            v->heat_tab_n--;
            /* no tombstone compaction: the map is rebuilt by the fold */
            return r;
        }
        i = (i + 1) & v->heat_tab_mask;
    }
    return 0;
}


/* +1 accrued read on the inode, once per session. Read-only sessions
 * accrue nothing. RAM-only: folds at close / sweep-decay time. */
void heat_touch_read(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    (void)lba;   /* WP27: per-file counters; the segment index is kept in
                  * the signature for the call sites' shape */
    if (!vol_write_enabled(v)) return;
    if (!inode) return;
    if (heat_tab_touch(v, inode)) return;   /* already counted */
    /* conservative summary: the promotion walk gates on it */
    v->heat_any_rhot = 1;
}

uint16_t heat_session_take(invfs_volume *v, uint64_t inode)
{
    return heat_tab_take(v, inode);
}


/* ---- the TLV ---- */

/* parse the "invfs.heat" TLV out of a raw record buffer. Fills *r/*w
 * (either may be NULL). 0 = found, -1 = absent/corrupt. */
int heat_read_tlv(const uint8_t *rec, uint32_t rec_len,
                  uint16_t *r, uint8_t *w)
{
    const uint8_t *ext, *p;
    size_t elen = 0, rem;
    size_t ast;
    invfs_meta_ext_hdr h;

    if (r) *r = 0;
    if (w) *w = 0;
    ext = meta_locate_ext(rec, rec_len, &elen);
    if (!ext) return -1;
    if (elen < sizeof(h)) return -1;
    memcpy(&h, ext, sizeof h);
    if (h.magic != INVFS_META_MAGIC || h.ext_len > elen ||
        (size_t)sizeof(h) + h.target_len > h.ext_len)
        return -1;
    p = ext + sizeof(h) + h.target_len;
    rem = h.ext_len - sizeof(h) - h.target_len;
    while (rem >= 4) {
        uint16_t nl, vl;
        memcpy(&nl, p, 2);
        memcpy(&vl, p + 2 + nl, 2);
        if ((size_t)2 + nl + 2 + vl > rem || nl == 0) break;
        if (nl == strlen(INVFS_XATTR_HEAT) &&
            memcmp(p + 2, INVFS_XATTR_HEAT, nl) == 0 && vl >= 3) {
            if (r) *r = (uint16_t)(p[2 + nl + 2] |
                                   ((uint16_t)p[2 + nl + 3] << 8));
            if (w) *w = p[2 + nl + 4];
            return 0;
        }
        p += 2 + nl + 2 + vl;
        rem -= 2 + nl + 2 + vl;
    }
    return -1;
}

/* stored heat of an inode's live record (0/0 when the TLV is absent) */
static int heat_get(invfs_volume *v, uint64_t inode, uint16_t *r, uint8_t *w)
{
    uint8_t *rec = NULL;
    uint32_t rl = 0;
    int rc;
    if (meta_read_record_by_id(v, inode, &rec, &rl, NULL, 0, NULL) != 0)
        return -1;
    rc = heat_read_tlv(rec, rl, r, w);
    free(rec);
    return rc;
}

/* effective read heat: stored + this session's accrued, saturated */
uint16_t heat_file_r(const invfs_volume *v, uint64_t inode)
{
    uint16_t r = 0;
    uint32_t s;
    invfs_volume *vv = (invfs_volume *)v;
    if (heat_get(vv, inode, &r, NULL) != 0)
        r = 0;
    s = (uint32_t)r + heat_tab_get(v, inode);
    return s > 0xFFFF ? 0xFFFF : (uint16_t)s;
}

/* write heat: the stored counter verbatim; an absent TLV reads as (0,0) --
 * "no heat history". (v1's pads were born wheat=1; under v2 the rewrite
 * carry is old+1, so born-0 keeps the observable chain identical: create
 * -> 0, first rewrite -> 1, ... and the absent rule means the decay pass
 * never has to stamp a never-heated file -- zero churn on cold volumes.) */
uint8_t heat_file_maxw(invfs_volume *v, uint64_t inode)
{
    uint8_t w = 0;
    if (heat_get(v, inode, NULL, &w) != 0)
        return 0;
    return w;
}

/* persist a write-heat value on the file's live record (no-op when
 * unchanged -- the stamp check-then-write rule) */
void heat_file_setw(invfs_volume *v, uint64_t inode, uint8_t w)
{
    uint16_t r = 0;
    uint8_t cur = 0;
    int have = heat_get(v, inode, &r, &cur) == 0;
    uint8_t val[4];
    if (have && cur == w)
        return;
    val[0] = (uint8_t)r;
    val[1] = (uint8_t)(r >> 8);
    val[2] = w;
    val[3] = 0;
    if (vol_set_xattr(v, inode, INVFS_XATTR_HEAT, val, sizeof val) != 0 &&
        getenv("INVFS_DEBUG"))
        fprintf(stderr, "[heat] inode %llu: heat stamp failed\n",
                (unsigned long long)inode);
}

/* persist read+write heat on the file's live record */
static void heat_write(invfs_volume *v, uint64_t inode, uint16_t r, uint8_t w)
{
    uint8_t val[4];
    val[0] = (uint8_t)r;
    val[1] = (uint8_t)(r >> 8);
    val[2] = w;
    val[3] = 0;
    if (vol_set_xattr(v, inode, INVFS_XATTR_HEAT, val, sizeof val) != 0 &&
        getenv("INVFS_DEBUG"))
        fprintf(stderr, "[heat] inode %llu: heat stamp failed\n",
                (unsigned long long)inode);
}


/* Build the INO2 ext blob for a fresh/rewritten record: old ext carried
 * verbatim with the heat TLV inserted/replaced. NULL old_ext fabricates a
 * minimal defaults ext (the WP27 rule: there is always something to carry
 * on a rewrite, and absent-ext + heat-init is the pre-warm create). The
 * blob is malloc'd (NULL = allocation failure; callers keep the old ext --
 * heat lost, never corrupt). */
uint8_t *heat_ext_merge(const invfs_volume *v, const uint8_t *old_ext,
                        uint32_t old_len, int is_dir, uint16_t rheat,
                        uint8_t wheat, uint32_t *len_out)
{
    static const char HN[] = INVFS_XATTR_HEAT;
    const size_t hn = sizeof(HN) - 1;
    invfs_meta_ext_hdr h;
    invfs_meta_pub pub;
    const uint8_t *xattrs = NULL;
    size_t xlen = 0;
    uint8_t *out, *w;
    size_t keep = 0, out_len, tlen;
    const uint8_t *p;
    size_t rem;

    memset(&pub, 0, sizeof pub);
    if (old_ext && old_len >= sizeof h) {
        memcpy(&h, old_ext, sizeof h);
        if (h.magic == INVFS_META_MAGIC && h.ext_len <= old_len &&
            (size_t)sizeof h + h.target_len <= h.ext_len) {
            pub.type = h.type;
            pub.mode = h.mode;
            pub.uid = h.uid;
            pub.gid = h.gid;
            pub.mtime = h.mtime;
            pub.atime = h.atime;
            pub.nlink = h.nlink;
            pub.rdev = h.rdev;
            if (h.target_len && h.target_len < sizeof pub.target)
                memcpy(pub.target, old_ext + sizeof h, h.target_len);
            xattrs = old_ext + sizeof h + h.target_len;
            xlen = h.ext_len - sizeof h - h.target_len;
        } else {
            old_ext = NULL;   /* unreadable ext: rebuild from defaults */
        }
    }
    if (!old_ext) {
        pub.type = is_dir ? INVFS_ITYP_DIR : INVFS_ITYP_REG;
        pub.mode = is_dir ? 0755 : 0644;
        pub.nlink = is_dir ? 2 : 1;
        xattrs = NULL;
        xlen = 0;
    }
    /* keep every TLV except the one being replaced */
    p = xattrs;
    rem = xlen;
    while (rem >= 4) {
        uint16_t nl, vl;
        size_t tsz;
        memcpy(&nl, p, 2);
        memcpy(&vl, p + 2 + nl, 2);
        tsz = (size_t)2 + nl + 2 + vl;
        if (tsz > rem || nl == 0) break;
        if (!(nl == hn && memcmp(p + 2, HN, hn) == 0))
            keep += tsz;
        p += tsz;
        rem -= tsz;
    }
    tlen = strlen(pub.target);
    out_len = sizeof h + tlen + keep + (2 + hn + 2 + 4);
    if (out_len > INVFS_META_SLACK) return NULL;
    out = (uint8_t *)malloc(out_len);
    if (!out) return NULL;
    memset(&h, 0, sizeof h);
    h.magic = INVFS_META_MAGIC;
    h.version = 2;
    h.ext_len = (uint16_t)out_len;
    h.type = pub.type;
    h.mode = pub.mode;
    h.uid = pub.uid;
    h.gid = pub.gid;
    h.mtime = pub.mtime;
    h.atime = pub.atime;
    h.nlink = pub.nlink;
    h.rdev = pub.rdev;
    h.target_len = (uint16_t)tlen;
    w = out;
    memcpy(w, &h, sizeof h); w += sizeof h;
    if (tlen) { memcpy(w, pub.target, tlen); w += tlen; }
    /* re-copy the kept TLVs (they were counted above) */
    p = xattrs;
    rem = xlen;
    while (rem >= 4) {
        uint16_t nl, vl;
        size_t tsz;
        memcpy(&nl, p, 2);
        memcpy(&vl, p + 2 + nl, 2);
        tsz = (size_t)2 + nl + 2 + vl;
        if (tsz > rem || nl == 0) break;
        if (!(nl == hn && memcmp(p + 2, HN, hn) == 0)) {
            memcpy(w, p, tsz);
            w += tsz;
        }
        p += tsz;
        rem -= tsz;
    }
    {
        uint16_t nl16 = (uint16_t)hn, vl16 = 4;
        memcpy(w, &nl16, 2); w += 2;
        memcpy(w, HN, hn); w += hn;
        memcpy(w, &vl16, 2); w += 2;
        *w++ = (uint8_t)rheat;
        *w++ = (uint8_t)(rheat >> 8);
        *w++ = wheat;
        *w++ = 0;
    }
    *len_out = (uint32_t)out_len;
    (void)v;
    return out;
}


/* v1 kept the WP19 hot summaries in the journal pads; v2 stores heat in
 * the records, so an open starts conservative (1/1 = "maybe hot") and the
 * sweep's decay pass recomputes the truth from the TLVs. */
void l2p_seed_heat(invfs_volume *v)
{
    v->heat_any_rhot = 1;
    v->heat_any_whot = 1;
}


/* Fold the session's accrued read touches into the records' TLVs.
 * Best-effort per file: a record that died mid-session (rewritten) is
 * skipped; heat is advisory, so a lost touch is a colder file, never a
 * wrong one. The liveness test is the NAME index (idx_id_live): the id
 * index is never pruned by design (vol_read_inode must still find
 * tombstoned records), so it cannot tell a retired id from a live one --
 * and meta-rewriting a retired id would re-add its name to the live
 * index (the fold-then-resurrect bug). */void heat_fold(invfs_volume *v)
{
    size_t i;
    if (!v->heat_tab) return;
    for (i = 0; i <= v->heat_tab_mask; i++) {
        uint64_t inode = v->heat_tab[i][0];
        uint64_t n;
        uint16_t r = 0, nr;
        uint8_t w = 0;
        if (!inode) continue;
        n = v->heat_tab[i][1];
        if (!n) continue;
        if (!idx_id_live(v, inode)) continue;   /* dead id (rewritten) */
        if (heat_get(v, inode, &r, &w) != 0) { r = 0; w = 0; }
        /* absent TLV == (0,0): "no heat history"; the fold writes only
         * when the accrual changes the stored value */
        nr = (uint64_t)r + n > 0xFFFF ? 0xFFFF : (uint16_t)(r + n);
        if (nr == r) continue;
        heat_write(v, inode, nr, w);
    }
    /* the touches are persisted: the table resets (a long-lived process
     * folding twice must not double-count) */
    memset(v->heat_tab, 0, (v->heat_tab_mask + 1) * sizeof *v->heat_tab);
    v->heat_tab_n = 0;
    v->heat_folded = 1;
}


/* WP27 heat persistence entry for drivers that want to persist read
 * touches WITHOUT a sweep run (the pump in the test suites, a daemon
 * flush point): the sweep's decay pass does this plus the halving; this
 * folds only. */
void vol_heat_persist(invfs_volume *v)
{
    if (!v || !vol_write_enabled(v)) return;
    if (!v->heat_tab_n) return;
    if (vol_mark_dirty(v) != 0) return;
    heat_fold(v);
}


/* ---- decay + promotion ------------------------------
 * One decay pass per sweep RUN: fold the session's accruals, then
 * rheat >>= 1 (exponential), wheat saturating-down by 1, persisted into
 * the records. The promotion check runs AFTER the decay (the hysteresis).
 * See the section comment at the top for the full rules. */

/* WP43: per-record body of the decay pass, fed by vol_records_walk().
 * `end` is the walk bound frozen before the pass started: heat_write()
 * stamps persist as NEW record versions (a stamp is a record append),
 * and each append bumps the active-extent cursor, so an unfrozen walk
 * would see its own stamps and decay them again -- a runaway append
 * loop. Records at or past the frozen end were appended by this pass;
 * aborting there keeps exactly one decay per stored value. */
typedef struct {
    invfs_volume *v;
    uint64_t      end;
    int           any_r;
    int           any_w;
} heat_decay_ctx;

static int heat_decay_cb(void *ctx_, uint64_t rec_pos,
                         const invfs_inode_rec *h, const uint8_t *rec)
{
    heat_decay_ctx *ctx = (heat_decay_ctx *)ctx_;
    invfs_volume *v = ctx->v;
    uint16_t r = 0;
    uint8_t w = 0;

    if (rec_pos >= ctx->end) return 1;   /* an append made during the pass */
    if (h->magic != INODE_REC_MAGIC || !h->name_len ||
        h->name_len > INVFS_MAX_NAME ||
        h->rec_len < INVFS_REC_HDR_LEN + h->name_len + 1 ||
        (uint8_t)((const invfs_inode_rec *)rec)->name[0] == 0x01 ||
        vol_find(v, ((const invfs_inode_rec *)rec)->name) != h->inode_id)
        return 0;
    if (idx_get_id(v, h->inode_id) != rec_pos)
        return 0;   /* the live version only (position-kill chains share the id) */
    /* absent TLV == (0,0) and decays to itself: the pass never stamps a
     * never-heated file (zero churn on cold volumes -- a format-v2
     * property, since a stamp is a record append now). The record came
     * off the walker CRC-verified, so heat_read_tlv sees the same bytes
     * the legacy loop's seek/read/CRC sequence did. */
    if (heat_read_tlv(rec, h->rec_len, &r, &w) != 0)
        return 0;
    {
        uint16_t nr = (uint16_t)(r >> 1);
        uint8_t nw = w ? (uint8_t)(w - 1) : 0;
        if (nr != r || nw != w)
            heat_write(v, h->inode_id, nr, nw);
        if (nr >= INVFS_HEAT_HOT) ctx->any_r = 1;
        if (nw >= INVFS_WHEAT_HOT) ctx->any_w = 1;
    }
    return 0;
}

void vol_heat_sweep_begin(invfs_volume *v)
{
    heat_decay_ctx ctx;
    /* the fold first: reads this process observed count into the decayed
     * totals exactly once */
    heat_fold(v);

    if (!v || !vol_write_enabled(v)) return;
    /* walk the live records (mapper extents via the shared walker on
     * v0.3.0+, the legacy area otherwise); collect warm files (stored
     * TLV != 0), then persist their decayed counters -- collect-in-cb,
     * stamps land past the frozen walk end */
    ctx.v = v;
    ctx.end = v->inode_area_pos;
    ctx.any_r = 0;
    ctx.any_w = 0;
    vol_records_walk(v, heat_decay_cb, &ctx);
    v->heat_any_rhot = ctx.any_r;
    v->heat_any_whot = ctx.any_w;
    (void)v;
}


/* promotion candidate: one live TEXT member at/above the heat threshold */
typedef struct {
    uint64_t inode;
    uint16_t r;
    char     name[256];
} heat_cand;


static int heat_cand_cmp(const void *a, const void *b)
{
    const heat_cand *x = (const heat_cand *)a, *y = (const heat_cand *)b;
    if (x->r != y->r) return x->r > y->r ? -1 : 1;
    return x->inode < y->inode ? -1 : x->inode > y->inode;
}


/* WP19 tiering, promotion direction (demotion has no in-tree backend since
 * zstd-22 was dropped): extract read-hot PPMd batch members to standalone
 * per-segment ZSTD (the generic sweep machinery on the decoded content),
 * stamp GENERIC{ZSTD}. BATCHED_BIN members never promote (already fast).
 * WP27: a member's dedupe-shared guard from v1 is gone by construction --
 * dedupe never merges TEXT entries, and the promotion abandons the batch
 * slice (a hole for the GC), it never frees it. Top-K by rheat within the
 * per-sweep budget: min(64, 10% of live TEXT members).
 * Runs between the sweep walk and vol_sweep_dedupe in the driver, after
 * the run's decay pass. Returns the number of promotions, <0 on error. */
/* WP43: per-record body of the promotion candidate collection, fed by
 * vol_records_walk(). Collection only reads (liveness, class, heat);
 * the records are rewritten by the promotion itself, after the walk. */
typedef struct {
    invfs_volume *v;
    heat_cand *cand;
    size_t n_cand, cap_cand;
    size_t text_members;
    int err;
} heat_cand_ctx;

static int heat_promote_cb(void *ctx_, uint64_t rec_pos,
                           const invfs_inode_rec *h, const uint8_t *rec)
{
    heat_cand_ctx *ctx = (heat_cand_ctx *)ctx_;
    invfs_volume *v = ctx->v;
    size_t nl;
    char nm[257];
    uint64_t ip;
    uint8_t cc = 0, ca = 0;
    uint16_t cg = 0, r;

    if (h->magic == TOMBSTONE_MAGIC) return 0;
    if (h->name_len > INVFS_MAX_NAME ||
        h->rec_len < INVFS_REC_HDR_LEN + h->name_len + 1) return 0;
    nl = h->name_len < INVFS_MAX_NAME ? h->name_len : INVFS_MAX_NAME;
    memcpy(nm, ((const invfs_inode_rec *)rec)->name, nl);
    nm[nl] = 0;
    ip = idx_get_id(v, h->inode_id);
    if (vol_find(v, nm) != h->inode_id || (ip && ip != rec_pos))
        return 0;   /* superseded version: not the live record */
    if (vol_get_class(v, h->inode_id, &cc, &ca, &cg) != 0)
        return 0;
    if (cc != INVFS_CLASS_TEXT)
        return 0;   /* BATCHED_BIN (fast already) never promotes */
    ctx->text_members++;
    r = heat_file_r(v, h->inode_id);
    if (r < INVFS_HEAT_HOT) return 0;
    if (ctx->n_cand == ctx->cap_cand) {
        size_t nc = ctx->cap_cand ? ctx->cap_cand * 2 : 16;
        heat_cand *nc2 = (heat_cand *)realloc(ctx->cand, nc * sizeof *nc2);
        if (!nc2) { ctx->err = 1; return 1; }
        ctx->cand = nc2;
        ctx->cap_cand = nc;
    }
    ctx->cand[ctx->n_cand].inode = h->inode_id;
    ctx->cand[ctx->n_cand].r = r;
    memcpy(ctx->cand[ctx->n_cand].name, nm, nl + 1);
    ctx->n_cand++;
    return 0;
}

int vol_heat_promote(invfs_volume *v)
{
    uint64_t owner;
    heat_cand_ctx ctx;
    size_t budget = 0, i;
    int promoted = 0;

    if (!v || !vol_write_enabled(v)) return 0;
    if (!v->heat_any_rhot) return 0;   /* cold volume: skip the walk */
    owner = vol_find(v, TZ_OWNER_NAME);
    if (!owner) return 0;

    /* walk live records (mapper extents via the shared walker on v0.3.0+,
     * the legacy area otherwise); TEXT class + hot -> candidate */
    memset(&ctx, 0, sizeof ctx);
    ctx.v = v;
    vol_records_walk(v, heat_promote_cb, &ctx);
    if (ctx.err) goto out;   /* realloc failed mid-collection (legacy: goto out) */
    /* NOTE: heat_any_rhot is NOT reset when the walk finds no TEXT
     * candidate: the summary means "some file is read-hot", and the tier
     * migration (vol_tier_migrate) keys on exactly that for
     * non-TEXT-classed segments. The decay pass recomputes the truth
     * every run, so a genuinely cold volume re-cools on its own. */

    if (ctx.text_members) {
        budget = ctx.text_members / 10;          /* 10% of live members */
        if (budget > INVFS_HEAT_PROMOTE_MAX) budget = INVFS_HEAT_PROMOTE_MAX;
    }
    if (ctx.n_cand > 1)
        qsort(ctx.cand, ctx.n_cand, sizeof *ctx.cand, heat_cand_cmp);

    for (i = 0; i < ctx.n_cand && (size_t)promoted < budget; i++) {
        uint64_t fsz = 0, nseg;
        const invfs_codec *zc;
        /* admission: same worst-case pricing as the generic sweep's
         * DEFER_ENOSPC path -- the promoted shape coexists with the batch
         * hole until GC, so promotion temporarily costs space */
        if (vol_stat_full(v, ctx.cand[i].name, NULL, &fsz, NULL) != 0 || !fsz)
            continue;
        nseg = (fsz + SEGMENT_SIZE - 1) / SEGMENT_SIZE;
        if (sweep_enospc(v, fsz + nseg * 8 + INVFS_ENOSPC_MARGIN)) {
            fprintf(stderr, "[heat] %s: promotion deferred (ENOSPC)\n",
                    ctx.cand[i].name);
            continue;
        }
        /* dec_mem: the generic floor is always admitted -- checked anyway,
         * so a policy that rejects ZSTD also refuses to extract into it */
        zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
        if (zc && zc->dec_mem_bytes > vol_get_dec_mem_limit(v))
            continue;
        if (vol_store_generic(v, ctx.cand[i].inode, ctx.cand[i].name,
                              INVFS_CLASS_GENERIC,
                              INVFS_ALGO_ZSTD) != 0) {
            fprintf(stderr, "[heat] %s: promotion failed (member left "
                            "batched, intact)\n", ctx.cand[i].name);
            continue;
        }
        promoted++;
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[heat] %s: rheat %u -> extracted to generic "
                            "ZSTD\n", ctx.cand[i].name, ctx.cand[i].r);
    }
out:
    printf("heat: %zu hot text member(s), %d promoted "
           "(budget %zu of %zu live)\n", ctx.n_cand, promoted, budget,
           ctx.text_members);
    free(ctx.cand);
    return promoted;
}
