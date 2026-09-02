/* vol_heat.c — WP19 heat counters + decay + promotion.
 * Split from volume.c. */

#include "volume_internal.h"


/* public decode helpers for tools (meta_probe) */
uint16_t vol_heat_r(const invfs_l2p_entry *e) { return e ? l2p_rheat(e) : 0; }

uint8_t  vol_heat_w(const invfs_l2p_entry *e) { return e ? e->pad[2] : 0; }


/* ==================== WP19: heat counters ====================
 *
 * Heat lives in invfs_l2p_entry.pad (invarifs.h): pad[0..1] = u16 LE
 * read-heat, pad[2] = u8 write-heat. Keyed by (inode,lba), so it follows
 * the mapping across pba remaps (dedupe re-keys carry it over); new
 * entries (rewrites, transcodes, batch commits) start cold -- read-heat
 * RESETS on rewrite by design, write-heat is the one counter carried
 * across rewrites (old+1), because "rewritten often" is exactly the
 * history a rewrite destroys.
 *
 * Increment rules:
 *  - read:  +1 the FIRST time a (inode,lba) is touched by a vol_read_*
 *           path within this process (the engine has no per-open hook a
 *           workstream-free file can reach -- fuse_fs.c owns the FUSE
 *           open callback -- so "open-session" = process lifetime here,
 *           tracked in v->heat_seen). Saturates at 0xFFFF.
 *  - write: entries born in vol_create_file get wheat=1; vol_replace_file
 *           carries max(old)+1 onto the replacement's entries. Saturates
 *           at 0xFF.
 *
 * Decay (vol_heat_sweep_begin, once per sweep RUN): rheat >>= 1,
 * wheat -= 1 (floor 0). Hysteresis math: the promotion check runs AFTER
 * the decay, so a single read burst of H promotes only if H survives
 * exactly one halving (H >= 2*HOT); sustained reading of R files-per-
 * interval stabilises rheat at R (h' = (h+R)/2 -> R). HOT=8: one hot
 * weekend (16+ opens) promotes once, casual 8..15-open bursts decay away.
 * Write-hot (wheat >= 2 post-decay = rewritten at least twice inside the
 * last interval) skips the heavy codec fan-out for one sweep.
 *
 * Persistence (WP22d): the journal is append-only, so heat rides it in
 * three ways -- a MAP op carries its entry's current pad (re-read at flush
 * time); a pure read touch queues (inode,lba) into v->j_heat and the next
 * flush appends a refresh MAP with the live pad (one entry per pair per
 * session at most: heat_seen gates the touch, so reads never churn the
 * journal); a bulk change (the sweep's decay, or >64k pending refreshes)
 * sets j_heat_all and the next flush compacts, the image carrying every
 * pad. An fsck -f REBUILD reconstructs mappings from records and resets
 * heat to cold (documented: the rebuild has no history to be faithful
 * to). A crash loses only the pending touches -- heat is advisory. */

static uint64_t heat_hash(uint64_t inode, uint64_t lba)
{
    uint64_t h = inode * 0x9E3779B97F4A7C15ull ^ lba;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return h;
}


/* add (inode,lba) to the session set; 1 = already counted. On allocation
 * failure it answers "already counted" -- a missed increment is a colder
 * file, never a wrong one. */
static int heat_seen_add(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    size_t mask, i, j;
    if (!v->heat_seen_cap) {
        v->heat_seen = (uint64_t (*)[2])calloc(256, sizeof *v->heat_seen);
        if (!v->heat_seen) return 1;
        v->heat_seen_cap = 256;
    } else if ((v->heat_seen_n + 1) * 10 >= v->heat_seen_cap * 7) {
        size_t nc = v->heat_seen_cap * 2;
        uint64_t (*ns)[2] = (uint64_t (*)[2])calloc(nc, sizeof *ns);
        if (!ns) return 1;
        for (j = 0; j < v->heat_seen_cap; j++) {
            if (v->heat_seen[j][0]) {
                size_t k = (size_t)heat_hash(v->heat_seen[j][0],
                                             v->heat_seen[j][1]) & (nc - 1);
                while (ns[k][0]) k = (k + 1) & (nc - 1);
                ns[k][0] = v->heat_seen[j][0];
                ns[k][1] = v->heat_seen[j][1];
            }
        }
        free(v->heat_seen);
        v->heat_seen = ns;
        v->heat_seen_cap = nc;
    }
    mask = v->heat_seen_cap - 1;
    i = (size_t)heat_hash(inode, lba) & mask;
    while (v->heat_seen[i][0]) {
        if (v->heat_seen[i][0] == inode && v->heat_seen[i][1] == lba)
            return 1;
        i = (i + 1) & mask;
    }
    v->heat_seen[i][0] = inode;
    v->heat_seen[i][1] = lba;
    v->heat_seen_n++;
    return 0;
}


/* +1 read-heat on the live mapping for (inode,lba), once per session.
 * Read-only sessions (READONLY flag / awaiting recovery) accrue nothing:
 * the increment must be flushable to be honest, and vol_mark_dirty is what
 * makes vol_close persist it. The newest-wins scan mirrors
 * vol_lookup_entry; each first touch costs one scan, every later touch of
 * the pair in this session is absorbed by the session set. */
void heat_touch_read(invfs_volume *v, uint64_t inode, uint64_t lba)
{
    size_t i;
    if (!vol_write_enabled(v)) return;
    if (heat_seen_add(v, inode, lba)) return;
    for (i = v->l2p_count; i-- > 0; ) {
        invfs_l2p_entry *e = &v->l2p[i];
        uint16_t r;
        if (e->type != INVFS_JRN_MAP || e->inode != inode || e->lba != lba)
            continue;
        r = l2p_rheat(e);
        if (r == 0xFFFFu) return;   /* saturated: nothing new to persist */
        l2p_set_rheat(e, (uint16_t)(r + 1));
        /* WP22d: RAM-only until the flush; the flush re-appends a refresh
         * MAP carrying the current pad -- no rewrite of durable entries */
        jrn_heat_touch(v, inode, lba);
        if ((uint32_t)r + 1 >= INVFS_HEAT_HOT) v->heat_any_rhot = 1;
        vol_mark_dirty(v);
        return;
    }
}


/* max write-heat over an inode's live mappings (0 = cold/none) */
uint8_t heat_file_maxw(invfs_volume *v, uint64_t inode)
{
    uint8_t w = 0;
    size_t i;
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->inode == inode && e->pad[2] > w)
            w = e->pad[2];
    }
    return w;
}


/* set write-heat on every live mapping of an inode (the rewrite carry) */
void heat_file_setw(invfs_volume *v, uint64_t inode, uint8_t w)
{
    size_t i;
    for (i = 0; i < v->l2p_count; i++) {
        invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->inode == inode) {
            e->pad[2] = w;
            jrn_heat_touch(v, e->inode, e->lba);
        }
    }
}


/* grab/restamp one mapping's heat around an l2p_remove+vol_map remap
 * (dedupe): heat keys on (inode,lba), not on the physical slot, so a pba
 * remap must carry it over rather than rebirth the entry cold */
void heat_grab(invfs_volume *v, uint64_t inode, uint64_t lba,
                      uint16_t *r, uint8_t *w)
{
    size_t i;
    *r = 0; *w = 0;
    for (i = v->l2p_count; i-- > 0; ) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->inode == inode && e->lba == lba) {
            *r = l2p_rheat(e);
            *w = e->pad[2];
            return;
        }
    }
}

void heat_stamp(invfs_volume *v, uint64_t inode, uint64_t lba,
                       uint16_t r, uint8_t w)
{
    size_t i;
    for (i = v->l2p_count; i-- > 0; ) {
        invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->inode == inode && e->lba == lba) {
            l2p_set_rheat(e, r);
            e->pad[2] = w;
            jrn_heat_touch(v, inode, lba);
            return;
        }
    }
}


/* ---- WP19: heat decay + promotion ------------------------------
 *
 * One decay pass per sweep RUN (vol_heat_sweep_begin at run start):
 * rheat >>= 1 (exponential), wheat saturating-down by 1. The promotion
 * check runs AFTER the decay, which is the hysteresis: a single read
 * burst of H promotes only when H >= 2*INVFS_HEAT_HOT (it survives
 * exactly one halving); sustained reading at rate R/interval stabilises
 * rheat near R, so genuinely hot files re-qualify every sweep. See the
 * WP19 block comment by the L2P helpers for the full rules. */
void vol_heat_sweep_begin(invfs_volume *v)
{
    size_t i;
    int changed = 0, any_r = 0, any_w = 0;

    if (!v) return;
    for (i = 0; i < v->l2p_count; i++) {
        invfs_l2p_entry *e = &v->l2p[i];
        uint16_t r;
        uint8_t w;
        if (e->type != INVFS_JRN_MAP) continue;
        r = l2p_rheat(e);
        w = e->pad[2];
        if (r) {
            l2p_set_rheat(e, (uint16_t)(r >> 1));
            changed = 1;
        }
        if (w) {
            e->pad[2] = (uint8_t)(w - 1);
            changed = 1;
        }
        if ((uint32_t)(r >> 1) >= INVFS_HEAT_HOT) any_r = 1;
        if (w && (uint32_t)(w - 1) >= INVFS_WHEAT_HOT) any_w = 1;
    }
    v->heat_any_rhot = any_r;
    v->heat_any_whot = any_w;
    if (changed) {
        /* WP22d: the decay touched every entry at once -- persisting it is
         * a bulk change, so the next flush compacts (the image carries the
         * whole table's pads) instead of appending per-entry refreshes */
        v->j_heat_all = 1;
        if (vol_write_enabled(v))
            vol_mark_dirty(v);   /* the caller's vol_flush persists the decay */
    }
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
    if (x->r != y->r) return x->r > y->r ? -1 : 1;   /* hottest first */
    return x->inode < y->inode ? -1 : x->inode > y->inode;
}


/* per-inode max read-heat, open addressing (inode 0 = empty slot; real
 * inode ids start at 1). Built in ONE L2P scan so the record walk below
 * costs O(1) per member instead of a scan. */
typedef struct { uint64_t inode; uint16_t r; } heat_max_ent;


static int heat_max_put(heat_max_ent **tp, size_t *capp, size_t *np,
                        uint64_t inode, uint16_t r)
{
    size_t mask, i, j;
    heat_max_ent *t = *tp;
    if (!*capp) {
        *capp = 256;
        t = (heat_max_ent *)calloc(*capp, sizeof *t);
        if (!t) { *capp = 0; return -1; }
        *tp = t;
    } else if ((*np + 1) * 10 >= *capp * 7) {
        size_t nc = *capp * 2;
        heat_max_ent *nt = (heat_max_ent *)calloc(nc, sizeof *nt);
        if (!nt) return -1;
        for (j = 0; j < *capp; j++) {
            if (t[j].inode) {
                size_t k = (size_t)heat_hash(t[j].inode, 0) & (nc - 1);
                while (nt[k].inode) k = (k + 1) & (nc - 1);
                nt[k] = t[j];
            }
        }
        free(t);
        t = *tp = nt;
        *capp = nc;
    }
    mask = *capp - 1;
    i = (size_t)heat_hash(inode, 0) & mask;
    while (t[i].inode) {
        if (t[i].inode == inode) {
            if (r > t[i].r) t[i].r = r;   /* keep the max */
            return 0;
        }
        i = (i + 1) & mask;
    }
    t[i].inode = inode;
    t[i].r = r;
    (*np)++;
    return 0;
}


static uint16_t heat_max_get(const heat_max_ent *t, size_t cap,
                             uint64_t inode)
{
    size_t mask, i;
    if (!cap) return 0;
    mask = cap - 1;
    i = (size_t)heat_hash(inode, 0) & mask;
    while (t[i].inode) {
        if (t[i].inode == inode) return t[i].r;
        i = (i + 1) & mask;
    }
    return 0;
}


/* Dedup-shared guard (WP19: NEVER promote a pba referenced by >1 live MAP
 * entry). Two-pass L2P scan per candidate: for every block range the
 * member maps, any OTHER live entry overlapping it must be a batch-sibling
 * dup -- proven by the owner mapping that entry's lba to exactly the same
 * pba (that is what a member dup IS). Batch blocks are never dedupe
 * canon/loser candidates (dedupe skips zone==TEXT outright), so a foreign
 * overlap "cannot happen" -- this check is the belt-and-braces the spec
 * asks for, and it also refuses on any L2P corruption. */
static int heat_member_shared(invfs_volume *v, uint64_t member, uint64_t owner)
{
    size_t i, j;
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *m = &v->l2p[i];
        uint64_t mp, ml;
        if (m->type != INVFS_JRN_MAP || m->inode != member) continue;
        mp = m->pba;
        ml = m->length ? m->length : 1;
        for (j = 0; j < v->l2p_count; j++) {
            const invfs_l2p_entry *e = &v->l2p[j];
            uint64_t el, op = 0, ol = 0;
            if (e->type != INVFS_JRN_MAP) continue;
            if (e->inode == member || e->inode == owner) continue;
            el = e->length ? e->length : 1;
            if (e->pba + el <= mp || e->pba >= mp + ml) continue;
            if (vol_lookup_entry(v, owner, e->lba, &op, &ol) == 0 &&
                op == e->pba)
                continue;   /* a batch sibling's dup: hole semantics, fine */
            return 1;       /* foreign reference: shared (or corrupt) */
        }
    }
    return 0;
}


/* WP19 tiering, promotion direction (demotion has no in-tree backend since
 * zstd-22 was dropped): extract read-hot PPMd batch members to standalone
 * per-segment ZSTD (the generic sweep machinery on the decoded content),
 * stamp GENERIC{ZSTD}. BATCHED_BIN members never promote (already fast);
 * dedup-shared members never promote; space/dec_mem admission mirrors the
 * generic sweep's (DEFER_ENOSPC pricing). Top-K by rheat within the
 * per-sweep budget: min(64, 10% of live TEXT members).
 * Runs between the sweep walk and vol_sweep_dedupe in the driver, after
 * the run's decay pass. Returns the number of promotions, <0 on error. */
int vol_heat_promote(invfs_volume *v)
{
    uint64_t owner, pos, end;
    heat_max_ent *hmax = NULL;
    size_t hmax_cap = 0, hmax_n = 0;
    heat_cand *cand = NULL;
    size_t n_cand = 0, cap_cand = 0, text_members = 0, budget = 0, i;
    int promoted = 0;

    if (!v || !vol_write_enabled(v)) return 0;
    if (!v->heat_any_rhot) return 0;   /* cold volume: skip the walk */
    owner = vol_find(v, TZ_OWNER_NAME);
    if (!owner) return 0;

    /* pass 1: inode -> max read-heat, one L2P scan */
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        uint16_t r;
        if (e->type != INVFS_JRN_MAP) continue;
        r = l2p_rheat(e);
        if (r >= INVFS_HEAT_HOT &&
            heat_max_put(&hmax, &hmax_cap, &hmax_n, e->inode, r) != 0)
            goto out;
    }
    if (!hmax_n) { v->heat_any_rhot = 0; goto out; }  /* decayed below HOT */

    /* pass 2: walk live records; TEXT class + hot -> candidate. The walk
     * mirrors vol_tz_gc's mark pass (newest-wins + position check). */
    pos = v->inode_area_start * INVFS_BLOCK_SIZE;
    end = v->inode_area_pos;
    while (pos + sizeof(invfs_inode_rec) <= end) {
        invfs_inode_rec h;
        size_t nl;
        char nm[257];
        uint64_t ip, rec_pos = pos;
        uint8_t cc = 0, ca = 0;
        uint16_t cg = 0, r;
        if (io_seek(&v->io, pos) != 0 ||
            io_read(&v->io, &h, sizeof h) != 0) break;
        if (h.magic != INODE_REC_MAGIC && h.magic != TOMBSTONE_MAGIC) break;
        if (h.rec_len < sizeof h || h.rec_len > INVFS_MAX_REC_LEN ||
            pos + h.rec_len + 4 > end) break;
        pos += (uint64_t)h.rec_len + 4;
        if (h.magic == TOMBSTONE_MAGIC) continue;
        nl = h.name_len < sizeof(h.name) ? h.name_len : sizeof(h.name) - 1;
        memcpy(nm, h.name, nl);
        nm[nl] = 0;
        ip = idx_get_id(v, h.inode_id);
        if (vol_find(v, nm) != h.inode_id || (ip && ip != rec_pos))
            continue;   /* superseded version: not the live record */
        if (vol_get_class(v, h.inode_id, &cc, &ca, &cg) != 0)
            continue;
        if (cc != INVFS_CLASS_TEXT)
            continue;   /* BATCHED_BIN (fast already) never promotes */
        text_members++;
        r = heat_max_get(hmax, hmax_cap, h.inode_id);
        if (r < INVFS_HEAT_HOT) continue;
        if (n_cand == cap_cand) {
            size_t nc = cap_cand ? cap_cand * 2 : 16;
            heat_cand *nc2 = (heat_cand *)realloc(cand, nc * sizeof *nc2);
            if (!nc2) goto out;
            cand = nc2;
            cap_cand = nc;
        }
        cand[n_cand].inode = h.inode_id;
        cand[n_cand].r = r;
        memcpy(cand[n_cand].name, nm, nl + 1);
        n_cand++;
    }

    if (text_members) {
        budget = text_members / 10;              /* 10% of live members */
        if (budget > INVFS_HEAT_PROMOTE_MAX) budget = INVFS_HEAT_PROMOTE_MAX;
    }
    if (n_cand > 1)
        qsort(cand, n_cand, sizeof *cand, heat_cand_cmp);

    for (i = 0; i < n_cand && (size_t)promoted < budget; i++) {
        uint64_t fsz = 0, nseg;
        const invfs_codec *zc;
        if (heat_member_shared(v, cand[i].inode, owner)) {
            if (getenv("INVFS_DEBUG"))
                fprintf(stderr, "[heat] %s: dedup-shared, not promoted\n",
                        cand[i].name);
            continue;
        }
        /* admission: same worst-case pricing as the generic sweep's
         * DEFER_ENOSPC path -- the promoted shape coexists with the batch
         * hole until GC, so promotion temporarily costs space */
        if (vol_stat_full(v, cand[i].name, NULL, &fsz, NULL) != 0 || !fsz)
            continue;
        nseg = (fsz + SEGMENT_SIZE - 1) / SEGMENT_SIZE;
        if (sweep_enospc(v, fsz + nseg * 8 + INVFS_ENOSPC_MARGIN)) {
            fprintf(stderr, "[heat] %s: promotion deferred (ENOSPC)\n",
                    cand[i].name);
            continue;
        }
        /* dec_mem: the generic floor is always admitted -- checked anyway,
         * so a policy that rejects ZSTD also refuses to extract into it */
        zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
        if (zc && zc->dec_mem_bytes > vol_get_dec_mem_limit(v))
            continue;
        if (vol_store_generic(v, cand[i].inode, cand[i].name,
                              INVFS_CLASS_GENERIC,
                              INVFS_ALGO_ZSTD) != 0) {
            fprintf(stderr, "[heat] %s: promotion failed (member left "
                            "batched, intact)\n", cand[i].name);
            continue;
        }
        promoted++;
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[heat] %s: rheat %u -> extracted to generic "
                            "ZSTD\n", cand[i].name, cand[i].r);
    }
out:
    printf("heat: %zu hot text member(s), %d promoted "
           "(budget %zu of %zu live)\n", n_cand, promoted, budget,
           text_members);
    free(hmax);
    free(cand);
    return promoted;
}
