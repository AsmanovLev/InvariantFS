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
#include "vol_walk.h"

/* 64-bit mix (splitmix64 finalizer) for the per-inode tables */
static uint64_t idx_mix_heat(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}


/* per-inode accrued read touches this session. heat_tab doubles as the
 * once-per-session seen set: presence == counted. On allocation failure a
 * touch is silently dropped -- a colder file, never a wrong one.
 *
 * WP-heat-table-concurrent-safe: caller holds heat_mu. The grow below
 * free()s the old array and republishes the pointer, so an unlocked caller
 * is not merely writing the wrong count -- it is dereferencing an array
 * another thread has already freed. The read path reaches here with
 * g_io_lock RELEASED (src/cli/fuse_fs.c:1407 then :1414, under fuse_loop_mt
 * at :3353), so "the caller serializes" was never true. */
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

/* accrued reads of `inode` this session (0 when none).
 * WP-heat-table-concurrent-safe: this one TAKES heat_mu itself rather than
 * requiring the caller to, because its only caller (heat_file_r) is reached
 * from the sweep/tier walk and from vol_sweep.c, and a lock at every one of
 * those entry points would spread this over four files for nothing. It holds
 * the lock across nothing but the probe, so it stays a leaf. */
static uint16_t heat_tab_get(const invfs_volume *cv, uint64_t inode)
{
    invfs_volume *v = (invfs_volume *)cv;
    size_t i;
    uint16_t r = 0;
    pthread_mutex_lock(&v->heat_mu);
    if (!v->heat_tab) { pthread_mutex_unlock(&v->heat_mu); return 0; }
    i = (size_t)idx_mix_heat(inode) & v->heat_tab_mask;
    while (v->heat_tab[i][0]) {
        if (v->heat_tab[i][0] == inode) {
            r = v->heat_tab[i][1] > 0xFFFF ? 0xFFFF
                                            : (uint16_t)v->heat_tab[i][1];
            break;
        }
        i = (i + 1) & v->heat_tab_mask;
    }
    pthread_mutex_unlock(&v->heat_mu);
    return r;
}

/* remove + return the session's accrued reads of `inode` (the write-commit
 * carry transfers the old id's pending touches onto the replacement).
 * WP-heat-table-concurrent-safe: caller holds heat_mu -- it REMOVES an entry
 * and decrements heat_tab_n, so it is a writer, and the write path's
 * g_io_lock does not exclude the lock-free readers. */
static uint16_t heat_tab_take(invfs_volume *v, uint64_t inode)
{
    size_t i;
    uint16_t r = 0;
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
            break;
        }
        i = (i + 1) & v->heat_tab_mask;
    }
    return r;
}

/* WP-heat-table-concurrent-safe: the two summary flags are shared state on
 * the same footing as the table (heat_touch_read sets heat_any_rhot from a
 * lock-free reader), so they get the same lock and the same accessors. */
int heat_any_rhot(const invfs_volume *cv)
{
    invfs_volume *v = (invfs_volume *)cv;
    int r;
    pthread_mutex_lock(&v->heat_mu);
    r = v->heat_any_rhot;
    pthread_mutex_unlock(&v->heat_mu);
    return r;
}

int heat_any_whot(const invfs_volume *cv)
{
    invfs_volume *v = (invfs_volume *)cv;
    int w;
    pthread_mutex_lock(&v->heat_mu);
    w = v->heat_any_whot;
    pthread_mutex_unlock(&v->heat_mu);
    return w;
}

/* vol_open / vol_close. A zeroed pthread_mutex_t happens to be a valid
 * glibc normal mutex, but relying on that is not what "locked" means; the
 * test harness calls this too, which is why it is exported. */
void heat_locks_init(invfs_volume *v)
{
    pthread_mutex_init(&v->heat_mu, NULL);
}

void heat_locks_destroy(invfs_volume *v)
{
    pthread_mutex_destroy(&v->heat_mu);
}


/* +1 accrued read on the inode, once per session. Read-only sessions
 * accrue nothing. RAM-only: folds at close / sweep-decay time.
 *
 * WP-heat-table-concurrent-safe: this is the LOCK-FREE READ PATH. It is
 * reached from vol_read_range with g_io_lock already released
 * (src/cli/fuse_fs.c:1407 releases, :1414 calls, :3353 fuse_loop_mt), so the
 * only thing standing between N threads and one heat_tab is this lock. */
void heat_touch_read(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    (void)lba;   /* WP27: per-file counters; the segment index is kept in
                  * the signature for the call sites' shape */
    if (!vol_write_enabled(v)) return;
    if (!inode) return;
    pthread_mutex_lock(&v->heat_mu);
    if (!heat_tab_touch(v, inode)) {
        /* conservative summary: the promotion walk gates on it */
        v->heat_any_rhot = 1;
    }
    pthread_mutex_unlock(&v->heat_mu);
}

uint16_t heat_session_take(invfs_volume *v, uint64_t inode)
{
    uint16_t r;
    pthread_mutex_lock(&v->heat_mu);
    r = heat_tab_take(v, inode);
    pthread_mutex_unlock(&v->heat_mu);
    return r;
}


/* ---- the TLV ---- */

/* parse the "invfs.heat" TLV out of a raw record buffer. Fills *r / *w
 * (either may be NULL). 0 = found, -1 = absent/corrupt. */


/* WP78: is the id still live? The v2 name index it used to ask is empty
 * on this format, which marked every file dead and meant heat never
 * persisted anywhere. */
static int heat_id_live(invfs_volume *v, uint64_t inode)
{
    invfs_v3_inode in;
    return vol_v3_inode_get(v, inode, &in) == 1 && in.nlink > 0;
}

/* stored heat of an inode (0/0 when the TLV is absent). WP78: read it
 * through the xattr API so it works on both the v2 INO2 ext and the v3
 * named-xattr tree (meta_read_record_by_id is a v2-only lookup and made
 * every v3 file read as cold). */
static int heat_get(invfs_volume *v, uint64_t inode, uint16_t *r, uint8_t *w)
{
    uint8_t val[4];
    size_t vl = sizeof val;
    if (r) *r = 0;
    if (w) *w = 0;
    if (vol_get_xattr(v, inode, INVFS_XATTR_HEAT, val, &vl) != 0 || vl < 3)
        return -1;
    if (r) *r = (uint16_t)(val[0] | ((uint16_t)val[1] << 8));
    if (w) *w = val[2];
    return 0;
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


/* Fold the session's accrued read touches into the records' TLVs.
 * Best-effort per file: a record that died mid-session (rewritten) is
 * skipped; heat is advisory, so a lost touch is a colder file, never a
 * wrong one. The liveness test is heat_id_live(): the id
 * position hint is never pruned, so it cannot tell a retired id from a
 * live one, and rewriting a retired id would resurrect it (the
 * fold-then-resurrect bug).
 *
 * WP-heat-table-concurrent-safe: the walk below is now split in three, and
 * the reason is lock ordering, not style. The per-record work calls
 * heat_id_live -> vol_v3_inode_get and heat_write -> vol_set_xattr, and those
 * descend into the v3 tree, whose locks (vol_btree, vol_delta) sit ABOVE
 * heat_mu. So heat_mu is held only across (a) copying the pending
 * (inode, count) pairs out of the table and (b) the reset memset -- both
 * straight-line table work -- and is NOT held across the xattr I/O. A touch
 * that lands while the fold is doing I/O is simply not folded this pass; it
 * is in the next one. That is the same trade the fold already made for
 * crashed records ("a lost touch is a colder file, never a wrong one"), and
 * it is why heat_mu stays a strict leaf. */void heat_fold(invfs_volume *v)
{
    uint64_t *pend = NULL;
    size_t np = 0, cap = 0, i;

    /* (a) copy the pending touches out under the lock. */
    pthread_mutex_lock(&v->heat_mu);
    if (v->heat_tab && v->heat_tab_n) {
        cap = v->heat_tab_n;
        pend = (uint64_t *)calloc(2 * cap, sizeof *pend);
        if (pend) {
            for (i = 0; i <= v->heat_tab_mask; i++) {
                uint64_t inode = v->heat_tab[i][0];
                uint64_t n = v->heat_tab[i][1];
                if (!inode || !n) continue;
                if (np == cap) break;   /* cannot happen: cap == heat_tab_n */
                pend[2 * np] = inode;
                pend[2 * np + 1] = n;
                np++;
            }
        }
    }
    v->heat_folded = 1;   /* set on every fold attempt, as before */
    pthread_mutex_unlock(&v->heat_mu);
    if (!pend) return;

    /* (b) the per-record work, with heat_mu NOT held. */
    for (i = 0; i < np; i++) {
        uint64_t inode = pend[2 * i];
        uint64_t n = pend[2 * i + 1];
        uint16_t r = 0, nr;
        uint8_t w = 0;
        if (!heat_id_live(v, inode)) continue;   /* dead id (rewritten) */
        if (heat_get(v, inode, &r, &w) != 0) { r = 0; w = 0; }
        /* absent TLV == (0,0): "no heat history"; the fold writes only
         * when the accrual changes the stored value */
        nr = (uint64_t)r + n > 0xFFFF ? 0xFFFF : (uint16_t)(r + n);
        if (nr == r) continue;
        heat_write(v, inode, nr, w);
    }

    /* (c) reset under the lock. Everything copied out above has been folded;
     * anything a reader added since (a) is left for the next pass rather than
     * silently erased -- so only the pairs we actually folded are cleared. */
    pthread_mutex_lock(&v->heat_mu);
    for (i = 0; i < np; i++) {
        uint64_t inode = pend[2 * i], n = pend[2 * i + 1];
        size_t k = (size_t)idx_mix_heat(inode) & v->heat_tab_mask;
        while (v->heat_tab[k][0]) {
            if (v->heat_tab[k][0] == inode) {
                /* clear only what we folded; a concurrent touch cannot have
                 * raised this slot above the value we copied */
                v->heat_tab[k][0] = 0;
                v->heat_tab[k][1] = 0;
                v->heat_tab_n--;
                (void)n;
                break;
            }
            k = (k + 1) & v->heat_tab_mask;
        }
    }
    pthread_mutex_unlock(&v->heat_mu);
    free(pend);
}


/* WP27 heat persistence entry for drivers that want to persist read
 * touches WITHOUT a sweep run (the pump in the test suites, a daemon
 * flush point): the sweep's decay pass does this plus the halving; this
 * folds only. */
void vol_heat_persist(invfs_volume *v)
{
    int empty;
    if (!v || !vol_write_enabled(v)) return;
    /* WP-heat-table-concurrent-safe: read the count under heat_mu. A reader
     * that adds a touch between this test and heat_fold()'s copy-out simply
     * gets folded on the next call -- the fold is idempotent-by-reset either
     * way, and heat is advisory. */
    pthread_mutex_lock(&v->heat_mu);
    empty = !v->heat_tab_n;
    pthread_mutex_unlock(&v->heat_mu);
    if (empty) return;
    if (vol_mark_dirty(v) != 0) return;
    heat_fold(v);
}


/* ---- decay + promotion ------------------------------
 * One decay pass per sweep RUN: fold the session's accruals, then
 * rheat >>= 1 (exponential), wheat saturating-down by 1, persisted into
 * the records. The promotion check runs AFTER the decay (the hysteresis).
 * See the section comment at the top for the full rules. */

/* WP43: per-record body of the decay pass, fed by the namespace walk.
 *
 * WP145: the walk here is FALLIBLE, and a stopped walk always reports
 * "I saw exactly what I stored" -- so a pass that writes as it walks cannot
 * tell a whole live set from a prefix of one, and leaves every inode the
 * walk did not reach hot for good. That is placement, and it persists: the
 * next sweep stops on the same unreadable page.
 *
 * So the pass COLLECTS during the walk and APPLIES after the receipt is
 * committed. The old `end` field (the walk bound frozen before the pass, to
 * stop a heat stamp -- which is a record append, which bumps that bound --
 * from being decayed again by the same pass) goes with it: no write happens
 * while the walk is running, so there is nothing left to outrun. */
typedef struct {
    uint64_t inode;
    uint16_t r;
    uint8_t  w;
} heat_decay_rec;

typedef struct {
    invfs_volume *v;
    heat_decay_rec *rec;   /* the decayed values, NOT yet written */
    size_t   n_rec, cap_rec;
    size_t   n_seen;       /* every row the walk handed us, changed or not */
    int      oom;
    int      any_r;
    int      any_w;
} heat_decay_ctx;

static int heat_decay_push(heat_decay_ctx *c, uint64_t ino, uint16_t r,
                           uint8_t w)
{
    if (c->n_rec == c->cap_rec) {
        size_t nc = c->cap_rec ? c->cap_rec * 2 : 64;
        heat_decay_rec *n2 =
            (heat_decay_rec *)realloc(c->rec, nc * sizeof *n2);
        if (!n2) { c->oom = 1; return -1; }
        c->rec = n2;
        c->cap_rec = nc;
    }
    c->rec[c->n_rec].inode = ino;
    c->rec[c->n_rec].r = r;
    c->rec[c->n_rec].w = w;
    c->n_rec++;
    return 0;
}



/* WP78: v3 decay body -- one pass over the live inode set, reading the
 * heat xattr through the format-agnostic xattr API. Reads and RECORDS; it
 * does not write (see heat_decay_ctx above). */
static int heat_decay_v3_cb(invfs_volume *v, uint64_t inode_id,
                            const char *name, void *ctx_)
{
    heat_decay_ctx *ctx = (heat_decay_ctx *)ctx_;
    uint16_t r = 0, nr;
    uint8_t w = 0, nw;

    ctx->n_seen++;
    if (!name || (unsigned char)name[0] == 0x01)
        return 0;
    if (heat_get(v, inode_id, &r, &w) != 0)
        return 0;               /* never heated: nothing to decay */
    nr = (uint16_t)(r >> 1);
    nw = w ? (uint8_t)(w - 1) : 0;
    if (nr != r || nw != w)
        heat_decay_push(ctx, inode_id, nr, nw);
    if (nr >= INVFS_HEAT_HOT) ctx->any_r = 1;
    if (nw >= INVFS_WHEAT_HOT) ctx->any_w = 1;
    return 0;
}

void vol_heat_sweep_begin(invfs_volume *v)
{
    heat_decay_ctx ctx;
    vol_walk_t w;
    int rc;
    size_t i;

    /* the fold first: reads this process observed count into the decayed
     * totals exactly once */
    heat_fold(v);

    if (!v || !vol_write_enabled(v)) return;
    /* walk the live records (mapper extents via the shared walker on
     * v0.3.0+, the legacy area otherwise); collect the warm files (stored
     * TLV != 0) and their decayed counters -- the stamps land after the
     * walk, never during it. */
    memset(&ctx, 0, sizeof ctx);
    ctx.v = v;
    rc = vol_v3_iter_live_inodes(v, heat_decay_v3_cb, &ctx);

    /* WP145: the RECEIPT, and the only mechanism for it -- vol_walk_t is a
     * caller-side value, not a channel on the walk. found == n on purpose:
     * an iterator has no caller-imposed cap, so nothing but rc < 0 can make
     * this walk short, and catching that one thing is what the commit is
     * for. (An array-filling walk needs `found` too because the walk can
     * stop on the caller's own buffer; this one cannot.) */
    vol_walk_init(&w, v, "vol_heat_sweep_begin");
    vol_walk_result(&w, rc, ctx.n_seen, ctx.n_seen);
    if (vol_walk_commit(&w) != 0) {
        fprintf(stderr,
                "[heat] decay REFUSED: the live-inode walk did not complete "
                "(%zu inode(s) reached, and it stopped rather than finished). "
                "Decaying only those would leave every cold file the walk "
                "never reached hot in perpetuity -- and would republish "
                "\"is anything hot\" from a subset, which is the flag the "
                "promotion pass and the tier migration both gate on. Heat is "
                "UNCHANGED for this run; the next sweep decays all of it, "
                "because rheat only ever falls.\n", ctx.n_seen);
        free(ctx.rec);
        return;
    }
    if (ctx.oom) {
        fprintf(stderr, "[heat] decay REFUSED: out of memory collecting the "
                "decay set. Heat is UNCHANGED for this run.\n");
        free(ctx.rec);
        return;
    }

    for (i = 0; i < ctx.n_rec; i++)
        heat_write(v, ctx.rec[i].inode, ctx.rec[i].r, ctx.rec[i].w);
    pthread_mutex_lock(&v->heat_mu);   /* WP-heat-table-concurrent-safe */
    v->heat_any_rhot = ctx.any_r;
    v->heat_any_whot = ctx.any_w;
    pthread_mutex_unlock(&v->heat_mu);
    free(ctx.rec);
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
 * the namespace walk. Collection only reads (liveness, class, heat);
 * the records are rewritten by the promotion itself, after the walk. */
typedef struct {
    invfs_volume *v;
    heat_cand *cand;
    size_t n_cand, cap_cand;
    size_t text_members;
    size_t n_seen;       /* every row the walk handed us */
    int err;
} heat_cand_ctx;



/* WP78: v3 promotion candidate collection -- one pass over the live inode
 * set, class TEXT and read-hot. */
static int heat_promote_v3_cb(invfs_volume *v, uint64_t inode_id,
                              const char *name, void *ctx_)
{
    heat_cand_ctx *ctx = (heat_cand_ctx *)ctx_;
    uint8_t cc = 0, ca = 0;
    uint16_t cg = 0, r;
    size_t nl;

    ctx->n_seen++;
    if (!name || (unsigned char)name[0] == 0x01)
        return 0;
    if (vol_get_class(v, inode_id, &cc, &ca, &cg) != 0)
        return 0;
    if (cc != INVFS_CLASS_TEXT)
        return 0;               /* BATCHED_BIN (fast already) never promotes */
    ctx->text_members++;
    r = heat_file_r(v, inode_id);
    if (r < INVFS_HEAT_HOT) return 0;
    if (ctx->n_cand == ctx->cap_cand) {
        size_t nc = ctx->cap_cand ? ctx->cap_cand * 2 : 16;
        heat_cand *nc2 = (heat_cand *)realloc(ctx->cand, nc * sizeof *nc2);
        if (!nc2) { ctx->err = 1; return 1; }
        ctx->cand = nc2;
        ctx->cap_cand = nc;
    }
    nl = strlen(name);
    if (nl > sizeof ctx->cand[0].name - 1) nl = sizeof ctx->cand[0].name - 1;
    ctx->cand[ctx->n_cand].inode = inode_id;
    ctx->cand[ctx->n_cand].r = r;
    memcpy(ctx->cand[ctx->n_cand].name, name, nl);
    ctx->cand[ctx->n_cand].name[nl] = 0;
    ctx->n_cand++;
    return 0;
}

/* WP78: extract one read-hot v3 TEXT member to a standalone whole-file
 * ZSTD blob (the v3 analogue of vol_store_generic's promotion), with the
 * same decode+memcmp bit-exactness guard. The batch segment is left for
 * tz_v3_gc (vol_v3_free_recipe_blocks skips TEXT entries). 1 = promoted,
 * 0 = no gain/refused (member stays batched), -1 = hard error. */
static int heat_extract_v3(invfs_volume *v, uint64_t inode_id,
                           const char *name)
{
    uint8_t *data = NULL, *enc = NULL, *back = NULL;
    size_t dlen = 0, enc_cap, enc_len = 0;
    const invfs_codec *zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
    uint64_t newino;

    if (!zc || !zc->encode || !zc->decode) return -1;
    if (vol_read_file(v, inode_id, &data, &dlen) != 0 || !data) {
        free(data);
        return -1;
    }
    enc_cap = ZSTD_compressBound(dlen);
    enc = (uint8_t *)malloc(enc_cap);
    if (!enc) { free(data); return -1; }
    {
        size_t zrc = ZSTD_compress(enc, enc_cap, data, dlen,
                                   invfs_profile_zstd_level(v->profile));
        if (ZSTD_isError(zrc) || zrc == 0 || zrc >= dlen || zrc > 0xFFFFFFFFu) {
            free(enc); free(data);
            return 0;           /* no gain: leave the member batched */
        }
        enc_len = zrc;
    }
    back = (uint8_t *)malloc(dlen ? dlen : 1);
    if (!back || zc->decode(enc, enc_len, back, dlen) != 0 ||
        memcmp(back, data, dlen) != 0) {
        free(back); free(enc); free(data);
        return 0;
    }
    free(back);
    newino = vol_v3_publish_blob_inode(v, inode_id, enc, enc_len, dlen,
                                       INVFS_ALGO_ZSTD);
    free(enc);
    free(data);
    if (!newino) return -1;
    vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC, INVFS_ALGO_ZSTD,
                    invfs_registry_generation());
    return 1;
}

int vol_heat_promote(invfs_volume *v)
{
    uint64_t owner;
    heat_cand_ctx ctx;
    vol_walk_t w;
    size_t budget = 0, i;
    int promoted = 0, rc;

    if (!v || !vol_write_enabled(v)) return 0;
    if (!heat_any_rhot(v)) return 0;   /* cold volume: skip the walk */
    owner = vol_find(v, TZ_OWNER_NAME);
    (void)owner;

    /* walk the live inodes; TEXT class + hot -> candidate */
    memset(&ctx, 0, sizeof ctx);
    ctx.v = v;
    rc = vol_v3_iter_live_inodes(v, heat_promote_v3_cb, &ctx);

    /* WP145: the RECEIPT -- see vol_heat_sweep_begin for why found == n is
     * the right pair here. This pass only COLLECTS in its callback, so
     * unlike the decay pass nothing has been written yet and refusing costs
     * no undo. `ctx.err` needs no separate branch below: it is set only by
     * the callback returning non-zero, which is what makes the walk stop,
     * which is exactly what the commit below refuses on. */
    vol_walk_init(&w, v, "vol_heat_promote");
    vol_walk_result(&w, rc, ctx.n_seen, ctx.n_seen);
    if (vol_walk_commit(&w) != 0) {
        fprintf(stderr,
                "[heat] promotion REFUSED: the live-inode walk did not "
                "complete (%zu inode(s) reached, and it stopped rather than "
                "finished). The candidate list AND its budget (10%% of the "
                "live TEXT members) would both come from that subset, so "
                "this pass would extract data out of a partial view of the "
                "volume and print it as a whole one. Nothing was promoted; "
                "the next sweep retries with a complete scan.\n", ctx.n_seen);
        free(ctx.cand);
        return 0;
    }
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
        {
            int pr = heat_extract_v3(v, ctx.cand[i].inode, ctx.cand[i].name);
            if (pr <= 0) {
                if (pr < 0)
                    fprintf(stderr, "[heat] %s: promotion failed (member left "
                                    "batched, intact)\n", ctx.cand[i].name);
                continue;
            }
        }
        promoted++;
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[heat] %s: rheat %u -> extracted to generic "
                            "ZSTD\n", ctx.cand[i].name, ctx.cand[i].r);
    }
    if (!invfs_sweep_ui_active())
        printf("heat: %zu hot text member(s), %d promoted "
               "(budget %zu of %zu live)\n", ctx.n_cand, promoted, budget,
               ctx.text_members);
    free(ctx.cand);
    return promoted;
}
