/* vol_repair.c — WP20b layer-2 (RS) repair, invf-fsck --repair.
 * Split from volume.c. */

#include "volume_internal.h"


/* ---- WP20b layer-2 repair (invf-fsck --repair) -------------------------
 *
 * Layer-2 recovery is fsck-ONLY: the runtime read path never touches RS
 * parity (it fails loudly past one bad block per layer-1 stripe). The
 * repair pass:
 *
 *   1. walks the live records (the verify --deep name walk) and checks the
 *      framing CRC32C of every distinct shadow-zone segment -> the failed
 *      segment list (a segment's bad blocks are among its plen blocks; a
 *      device-unreadable block is known-bad outright);
 *   2. groups the failures by layer-2 stripe and, per stripe, searches the
 *      erasure set: the forced set E0 (device-missing blocks + unmapped
 *      parity slots) plus hypotheses of 1..m2-|E0| blocks drawn from the
 *      failed segments' blocks, decoded with rs_decode and ARBITRATED:
 *        (a) the parity slots outside E recomputed from the decoded data
 *            must equal the stored parity blocks (deterministic: two
 *            distinct codewords of an MDS(k+m, k) code that agree on k
 *            surviving slots are equal, so a decode poisoned by a
 *            still-bad survivor always mismatches a compared parity slot);
 *        (b) every failed segment fully inside the stripe must pass its
 *            own framing CRC32C with the decoded blocks spliced in.
 *      A hypothesis that passes both is the true damage with overwhelming
 *      margin; its blocks are written back and re-read from the device.
 *   3. after all stripes, every failed segment is re-verified from the
 *      device; a segment that still fails flips its stripe to
 *      "unrecoverable" in the report (the blocks written are the
 *      parity-consistent reconstruction, but the report stays honest).
 *
 * The search is bounded (SEAL2_REPAIR_MAX_HYP hypotheses per stripe): a
 * stripe with damage beyond m2 -- or a hypothesis space too wide -- is
 * reported and left untouched, never written with garbage.
 */
#define SEAL2_REPAIR_MAX_HYP 4096u


typedef struct {
    uint64_t pba;
    uint32_t plen;
    int checkable;          /* framing crc present (bounds+crc sane) */
    uint8_t *img;           /* the segment's current device image */
} seal2_segref;


static unsigned pop64(uint64_t x)
{
    unsigned n = 0;
    while (x) { n += (unsigned)(x & 1); x >>= 1; }
    return n;
}


static void seal2_free_failed(seal2_segref *f, size_t n)
{
    size_t i;
    if (!f) return;
    for (i = 0; i < n; i++) free(f[i].img);
    free(f);
}


/* context for one stripe's repair attempts */
typedef struct {
    invfs_volume *v;
    const seal_view *sv;
    int algo;
    uint32_t m2;
    uint64_t base;            /* first block of the stripe */
    uint64_t zone_end;        /* shadow zone end (block) */
    const uint64_t *par;      /* the stripe's m2 parity pbas (0 = unmapped) */
    const uint8_t *orig;      /* (32+m2) blocks: the current slot content */
    uint8_t *work;            /* per-attempt decode target */
    uint8_t *reenc;           /* per-attempt re-encoded parity */
    uint8_t **blocks;
    uint8_t *present;
    uint8_t **dd, **pp;
    const seal2_segref *fail; /* all failed segments */
    const size_t *fidx;       /* indices of this stripe's failed segments */
    size_t nfail;
} seal2_ctx;


/* One hypothesis: erase the slots in E (bits 0..31 data, 32..32+m2-1
 * parity), decode, and arbitrate. The parity-recompute check alone is NOT
 * sufficient: with all parity slots among the survivors, any k-survivor
 * solve reproduces them by construction, so a decode poisoned by a
 * still-bad survivor would pass it (two distinct codewords of an
 * MDS(32+m2, 32) code may agree on any k slots). The framing CRC32C of
 * every failed segment is the real arbiter:
 *   (a) fast pre-filter -- recompute the parity from the decoded data:
 *       every parity slot NOT in E must equal its current content;
 *   (b) every checkable failed segment touching the stripe must pass its
 *       own framing CRC32C with the stripe's decoded blocks spliced in;
 *       blocks beyond the stripe (a segment spanning a stripe boundary)
 *       are re-read from the device NOW, so a clean or already-repaired
 *       neighbour arbitrates honestly. At least one checkable segment must
 *       arbitrate, or the hypothesis is unprovable. Returns 1 = proved
 *       (work holds the true stripe). */
static int seal2_try(seal2_ctx *c, uint64_t E)
{
    const uint32_t k2 = SEAL2_K;
    uint32_t nslots = k2 + c->m2;
    size_t i, j, f;
    unsigned arbitrated = 0;

    memcpy(c->work, c->orig, (size_t)nslots * INVFS_BLOCK_SIZE);
    for (i = 0; i < nslots; i++)
        c->present[i] = (uint8_t)!((E >> i) & 1);
    if (rs_decode(c->algo, k2, c->m2, INVFS_BLOCK_SIZE,
                  c->blocks, c->present) != 0)
        return 0;
    /* (a) fast pre-filter: parity consistency */
    if (rs_encode(c->algo, k2, c->m2, INVFS_BLOCK_SIZE, c->dd, c->pp) != 0)
        return 0;
    for (j = 0; j < c->m2; j++) {
        if ((E >> (k2 + j)) & 1) continue;
        if (memcmp(c->pp[j], c->work + (k2 + j) * INVFS_BLOCK_SIZE,
                   INVFS_BLOCK_SIZE) != 0)
            return 0;
    }
    /* (b) the framing CRC of every failed segment. The spliced image is
     * verified as a whole, so a segment whose HEADER block is corrupt
     * (uncheckable at scan time) arbitrates fine once the hypothesis
     * restores the header. */
    for (f = 0; f < c->nfail; f++) {
        const seal2_segref *g = &c->fail[c->fidx[f]];
        uint8_t *tmp;
        uint64_t b, lo, hi;
        uint32_t cs;
        int vok;
        tmp = (uint8_t *)malloc((size_t)g->plen * INVFS_BLOCK_SIZE);
        if (!tmp) return 0;
        memcpy(tmp, g->img, (size_t)g->plen * INVFS_BLOCK_SIZE);
        /* splice the stripe's decoded blocks */
        lo = g->pba > c->base ? g->pba : c->base;
        hi = g->pba + g->plen;
        if (hi > c->base + k2) hi = c->base + k2;
        if (hi > c->zone_end) hi = c->zone_end;
        for (b = lo; b < hi; b++)
            memcpy(tmp + (b - g->pba) * INVFS_BLOCK_SIZE,
                   c->work + (b - c->base) * INVFS_BLOCK_SIZE,
                   INVFS_BLOCK_SIZE);
        /* re-read any blocks beyond the stripe from the device (clean or
         * already-repaired neighbour: the current truth) */
        for (b = g->pba; b < g->pba + g->plen; b++) {
            if (b >= lo && b < hi) continue;
            if (io_seek(&c->v->io, b * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&c->v->io, tmp + (b - g->pba) * INVFS_BLOCK_SIZE,
                        INVFS_BLOCK_SIZE) != 0) {
                free(tmp);
                return 0;   /* cannot arbitrate now */
            }
        }
        vok = seal_seg_verify(tmp, g->plen, &cs);
        free(tmp);
        if (vok != 0) return 0;
        arbitrated++;
    }
    return arbitrated > 0;
}


/* Write back every erased slot that is a real block (occupied data slot /
 * mapped parity slot) and re-read it from the device. 0 = ok. */
static int seal2_commit(seal2_ctx *c, uint64_t E, uint64_t *written)
{
    const uint32_t k2 = SEAL2_K;
    uint8_t *chk = NULL;
    size_t i;
    int rc = -1;

    chk = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    if (!chk) return -1;
    for (i = 0; i < k2 + c->m2; i++) {
        uint64_t b;
        if (!((E >> i) & 1)) continue;
        if (i < k2) {
            b = c->base + i;
            if (b >= c->zone_end) continue;
            if (!bit_get(c->v->bitmap, b) || seal_excluded(c->sv, b))
                continue;   /* absent slot: zero content, nothing allocated */
        } else {
            b = c->par[i - k2];
            if (!b) continue;   /* unmapped parity slot: stays unmapped */
        }
        if (io_seek(&c->v->io, b * INVFS_BLOCK_SIZE) != 0 ||
            io_write(&c->v->io, c->work + i * INVFS_BLOCK_SIZE,
                     INVFS_BLOCK_SIZE) != 0 ||
            io_seek(&c->v->io, b * INVFS_BLOCK_SIZE) != 0 ||
            io_read(&c->v->io, chk, INVFS_BLOCK_SIZE) != 0 ||
            memcmp(chk, c->work + i * INVFS_BLOCK_SIZE,
                   INVFS_BLOCK_SIZE) != 0) {
            fprintf(stderr, "seal2 repair: write-back of block %llu failed "
                    "to stick\n", (unsigned long long)b);
            goto out;
        }
        (*written)++;
        fprintf(stderr, "[seal2] repaired block %llu via RS parity\n",
                (unsigned long long)b);
    }
    rc = 0;
out:
    free(chk);
    return rc;
}


/* Repair one stripe: enumerate erasure hypotheses E0 + 0..budget candidate
 * blocks (drawn from the failed segments' blocks) until one proves itself.
 * Returns 1 = repaired (*written bumped), 0 = unrecoverable. */
static int seal2_repair_stripe(seal2_ctx *c, uint64_t E0, uint32_t pool,
                               invfs_seal2_repair *rep, uint64_t *written)
{
    unsigned e0n = pop64(E0);
    unsigned budget, t, np = 0, att = 0;
    unsigned pslots[32];
    unsigned i;
    uint32_t p = pool;

    if (e0n > c->m2) return 0;   /* forced erasures alone exceed the code */
    budget = c->m2 - e0n;
    while (p) {                  /* candidate slots: pool's set bits */
        unsigned bit = 0;
        while (!((p >> bit) & 1)) bit++;
        pslots[np++] = bit;
        p &= p - 1;
    }
    for (t = 0; t <= budget; t++) {
        unsigned idx[SEAL2_M2_MAX];
        if (t > np) break;
        if (t == 0) {
            att++;
            if (seal2_try(c, E0) && seal2_commit(c, E0, written) == 0) {
                rep->hypotheses += att;
                return 1;
            }
            continue;
        }
        for (i = 0; i < t; i++) idx[i] = i;
        for (;;) {
            uint64_t E = E0;
            if (att >= SEAL2_REPAIR_MAX_HYP) {
                rep->hypotheses += att;
                return 0;
            }
            for (i = 0; i < t; i++) E |= 1ull << pslots[idx[i]];
            att++;
            if (seal2_try(c, E) && seal2_commit(c, E, written) == 0) {
                rep->hypotheses += att;
                return 1;
            }
            /* next t-combination (odometer) */
            {
                int q = (int)t - 1;
                while (q >= 0 && idx[q] == np - t + (unsigned)q) q--;
                if (q < 0) break;
                idx[q]++;
                for (i = (unsigned)q + 1; i < t; i++)
                    idx[i] = idx[i - 1] + 1;
            }
        }
    }
    rep->hypotheses += att;
    return 0;
}


int vol_seal2_repair(invfs_volume *v, invfs_seal2_repair *rep)
{
    const uint32_t k2 = SEAL2_K;
    seal_view sv;
    uint64_t ss, snb, n2, shard_stripes, zone_end;
    uint32_t m2;
    int algo;
    int rc = 0;
    /* live-record walk (the verify --deep pattern: newest id per name) */
    struct lr_ent { uint64_t id; } *ents = NULL;
    size_t nents = 0, capents = 0;
    /* candidate segments (deduped by pba) and the failed ones */
    typedef struct { uint64_t pba; uint32_t plen; } segcand;
    segcand *cand = NULL;
    size_t ncand = 0, capcand = 0;
    seal2_segref *fail = NULL;
    size_t nfail = 0, capfail = 0;
    /* per-stripe failure grouping */
    uint32_t *st_cnt = NULL;
    size_t *st_off = NULL, *st_fill = NULL;
    size_t *st_idx = NULL;
    uint8_t *st_status = NULL;    /* 1 = repaired, 2 = unrecoverable */
    /* device-missing occupied blocks (allocated on first failure) */
    uint8_t *devmiss = NULL;
    /* per-stripe decode context */
    uint8_t *orig = NULL, *work = NULL, *reenc = NULL;
    uint8_t **blocks = NULL, *present = NULL, **dd = NULL, **pp = NULL;
    uint64_t *par = NULL;
    uint32_t nslots;
    size_t i;

    if (!v || !rep) return -1;
    memset(rep, 0, sizeof *rep);
    if (!vol_write_enabled(v)) {
        fprintf(stderr, "seal2 repair: volume is read-only\n");
        return -1;
    }
    if (!v->rd_present || !v->rd.l2_algo) {
        fprintf(stderr, "seal2 repair: no layer-2 seal configured\n");
        return 0;
    }
    algo = v->rd.l2_algo;
    m2 = v->rd.m2;
    nslots = k2 + m2;
    if (m2 < SEAL2_M2_MIN || m2 > SEAL2_M2_MAX) return -1;
    if (seal_view_load(v, &sv) != 0) return -1;
    if (!sv.nshards2) {
        fprintf(stderr, "seal2 repair: layer 2 configured but never sealed\n");
        seal_view_free(&sv);
        return 0;
    }
    ss = v->sb.shadow_zone_start;
    snb = v->sb.shadow_zone_blocks;
    zone_end = ss + snb;
    n2 = (snb + k2 - 1) / k2;
    shard_stripes = seal2_shard_stripes(m2);

    par = (uint64_t *)calloc((size_t)n2 * m2, sizeof *par);
    st_cnt = (uint32_t *)calloc((size_t)n2, sizeof *st_cnt);
    st_status = (uint8_t *)calloc((size_t)n2, 1);
    orig = (uint8_t *)malloc((size_t)nslots * INVFS_BLOCK_SIZE);
    work = (uint8_t *)malloc((size_t)nslots * INVFS_BLOCK_SIZE);
    reenc = (uint8_t *)malloc((size_t)m2 * INVFS_BLOCK_SIZE);
    blocks = (uint8_t **)malloc(nslots * sizeof *blocks);
    present = (uint8_t *)malloc(nslots);
    dd = (uint8_t **)malloc(k2 * sizeof *dd);
    pp = (uint8_t **)malloc(m2 * sizeof *pp);
    if (!par || !st_cnt || !st_status || !orig || !work || !reenc ||
        !blocks || !present || !dd || !pp) { rc = -1; goto out; }
    for (i = 0; i < nslots; i++) blocks[i] = work + i * INVFS_BLOCK_SIZE;
    for (i = 0; i < k2; i++) dd[i] = work + i * INVFS_BLOCK_SIZE;
    for (i = 0; i < m2; i++) pp[i] = reenc + i * INVFS_BLOCK_SIZE;
    seal2_map_load(v, &sv, shard_stripes, m2, n2, par);

    /* ---- pass 1: live files' shadow segments -> framing-CRC check ---- */
    {
        uint64_t pos = v->inode_area_start * INVFS_BLOCK_SIZE;
        while (pos) {
            uint32_t magic, rl;
            uint64_t ino, fsz, np;
            char nm[256];
            np = vol_inode_next(v, pos, &magic, &ino, &fsz, nm, sizeof nm,
                                &rl);
            if (!np) break;
            pos = np;
            if (magic != INODE_REC_MAGIC) continue;
            if ((uint8_t)nm[0] == 0x01) continue;   /* internal owners */
            if (vol_find(v, nm) != ino) continue;   /* superseded */
            for (i = 0; i < nents; i++)
                if (ents[i].id == ino) break;
            if (i < nents) continue;               /* seen (name chain) */
            if (nents == capents) {
                size_t nc = capents ? capents * 2 : 256;
                void *ne = realloc(ents, nc * sizeof *ents);
                if (!ne) { rc = -1; goto out; }
                ents = (struct lr_ent *)ne;
                capents = nc;
            }
            ents[nents++].id = ino;
        }
    }
    for (i = 0; i < nents; i++) {
        uint8_t *buf = NULL;
        uint32_t rl = 0;
        uint64_t rpos = 0;
        uint32_t crc_stored;
        invfs_ast_recipe_header ah;
        const invfs_ast_block_entry *be;
        size_t base = sizeof(invfs_inode_rec);
        uint32_t bi;

        if (meta_read_record_by_id(v, ents[i].id, &buf, &rl, NULL, 0,
                                   &rpos) != 0)
            continue;
        /* the structural scan reports bad records; here a record whose
         * CRC fails is simply not a trustworthy segment source */
        if (io_seek(&v->io, rpos + rl) != 0 ||
            io_read(&v->io, &crc_stored, 4) != 0 ||
            invfs_crc32c(buf, rl) != crc_stored ||
            rl < base + sizeof(ah)) {
            free(buf);
            continue;
        }
        memcpy(&ah, buf + base, sizeof(ah));
        if (rl < base + sizeof(ah) +
                 (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
            free(buf);
            continue;
        }
        be = (const invfs_ast_block_entry *)(buf + base + sizeof(ah));
        for (bi = 0; bi < ah.num_blocks; bi++) {
            uint64_t pba = 0, plen = 0;
            if (vol_lookup_entry(v, ents[i].id, be[bi].block_id,
                                 &pba, &plen) != 0 || !pba)
                continue;
            if (!plen) plen = 1;
            if (pba < ss || pba + plen > zone_end) continue;
            if (plen > 8192) continue;   /* beyond any legit segment */
            if (ncand == capcand) {
                size_t nc = capcand ? capcand * 2 : 512;
                void *ncp = realloc(cand, nc * sizeof *cand);
                if (!ncp) { free(buf); rc = -1; goto out; }
                cand = (segcand *)ncp;
                capcand = nc;
            }
            cand[ncand].pba = pba;
            cand[ncand].plen = (uint32_t)plen;
            ncand++;
        }
        free(buf);
    }
    /* dedup by pba (batch members share the batch's segment) */
    if (ncand) {
        size_t a, w;
        /* insertion sort is fine for the counts fsck-scale volumes have */
        for (a = 1; a < ncand; a++) {
            segcand t = cand[a];
            size_t h = a;
            while (h > 0 && cand[h - 1].pba > t.pba) {
                cand[h] = cand[h - 1];
                h--;
            }
            cand[h] = t;
        }
        w = 0;
        for (a = 0; a < ncand; a++)
            if (!w || cand[a].pba != cand[w - 1].pba)
                cand[w++] = cand[a];
        ncand = w;
    }
    /* framing-CRC check each distinct segment */
    for (i = 0; i < ncand; i++) {
        uint64_t pba = cand[i].pba;
        uint32_t plen = cand[i].plen;
        uint8_t *img = (uint8_t *)malloc((size_t)plen * INVFS_BLOCK_SIZE);
        uint32_t csize, crc;
        int devbad = 0, failed = 0, checkable = 0;
        uint64_t b2;
        if (!img) { rc = -1; goto out; }
        for (b2 = 0; b2 < plen; b2++) {
            if (io_seek(&v->io, (pba + b2) * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, img + b2 * INVFS_BLOCK_SIZE,
                        INVFS_BLOCK_SIZE) != 0) {
                memset(img + b2 * INVFS_BLOCK_SIZE, 0, INVFS_BLOCK_SIZE);
                devbad = 1;
                if (!devmiss)
                    devmiss = (uint8_t *)calloc((size_t)(snb + 7) / 8, 1);
                if (devmiss) bit_set(devmiss, pba + b2 - ss);
            }
        }
        memcpy(&csize, img, 4);
        memcpy(&crc, img + 4, 4);
        if (csize && crc && (uint64_t)csize + 8 <=
            (uint64_t)plen * INVFS_BLOCK_SIZE) {
            checkable = 1;
            if (invfs_crc32c(img + 8, csize) != crc) failed = 1;
        } else if (csize || crc) {
            failed = 1;   /* header out of bounds: corrupt by construction */
        }
        /* devbad with a readable framing: the CRC is the truth */
        if (devbad && !checkable) failed = 1;
        if (!failed) { free(img); continue; }
        if (nfail == capfail) {
            size_t nc = capfail ? capfail * 2 : 32;
            void *nf = realloc(fail, nc * sizeof *fail);
            if (!nf) { free(img); rc = -1; goto out; }
            fail = (seal2_segref *)nf;
            capfail = nc;
        }
        fail[nfail].pba = pba;
        fail[nfail].plen = plen;
        fail[nfail].checkable = checkable;
        fail[nfail].img = img;
        nfail++;
        /* group by stripe */
        {
            uint64_t s0 = (pba - ss) / k2;
            uint64_t s1 = (pba + plen - 1 - ss) / k2;
            uint64_t sx;
            for (sx = s0; sx <= s1 && sx < n2; sx++)
                st_cnt[sx]++;
        }
    }
    if (!nfail) goto out;   /* nothing to repair: all counters zero */

    /* stripe -> failed-segment index lists */
    st_off = (size_t *)calloc((size_t)n2, sizeof *st_off);
    st_fill = (size_t *)calloc((size_t)n2, sizeof *st_fill);
    if (!st_off || !st_fill) { rc = -1; goto out; }
    {
        uint64_t sx;
        size_t tot = 0;
        for (sx = 0; sx < n2; sx++) { st_off[sx] = tot; tot += st_cnt[sx]; }
        st_idx = (size_t *)malloc((tot ? tot : 1) * sizeof *st_idx);
        if (!st_idx) { rc = -1; goto out; }
    }
    for (i = 0; i < nfail; i++) {
        uint64_t s0 = (fail[i].pba - ss) / k2;
        uint64_t s1 = (fail[i].pba + fail[i].plen - 1 - ss) / k2;
        uint64_t sx;
        for (sx = s0; sx <= s1 && sx < n2; sx++)
            st_idx[st_off[sx] + st_fill[sx]++] = i;
    }

    {
        uint64_t sx;
        for (sx = 0; sx < n2; sx++)
            if (st_cnt[sx]) rep->stripes_scanned++;
    }

    /* ---- pass 2: per-stripe erasure search + writeback ----
     * A stripe whose failed segments all span into another damaged stripe
     * can only be arbitrated after the neighbour is repaired, so the pass
     * repeats while it keeps repairing stripes (each success retires one;
     * a stripe that proves nothing in a no-progress round is honestly
     * unrecoverable). */
    {
        uint64_t sx;
        int progress;
        do {
            progress = 0;
            for (sx = 0; sx < n2; sx++) {
                uint64_t base, E0 = 0;
                uint32_t pool = 0;
                size_t f;
                int repaired;
                uint64_t written = 0;
                seal2_ctx c;
                if (!st_cnt[sx] || st_status[sx]) continue;
                base = ss + sx * k2;

                /* slot content: occupied non-parity data blocks read from
                 * the device (device-missing -> zero + forced erasure),
                 * parity slots from their map (unmapped -> forced) */
                memset(orig, 0, (size_t)nslots * INVFS_BLOCK_SIZE);
                for (i = 0; i < k2; i++) {
                    uint64_t b = base + i;
                    if (b >= zone_end) break;
                    if (!bit_get(v->bitmap, b) || seal_excluded(&sv, b))
                        continue;
                    if (devmiss && bit_get(devmiss, b - ss)) {
                        E0 |= 1ull << i;
                        continue;
                    }
                    if (io_seek(&v->io, b * INVFS_BLOCK_SIZE) != 0 ||
                        io_read(&v->io, orig + i * INVFS_BLOCK_SIZE,
                                INVFS_BLOCK_SIZE) != 0)
                        E0 |= 1ull << i;
                }
                for (i = 0; i < m2; i++) {
                    uint64_t pb = par[sx * m2 + i];
                    if (!pb || pb >= v->sb.total_blocks) {
                        E0 |= 1ull << (k2 + i);
                        continue;
                    }
                    if (io_seek(&v->io, pb * INVFS_BLOCK_SIZE) != 0 ||
                        io_read(&v->io, orig + (k2 + i) * INVFS_BLOCK_SIZE,
                                INVFS_BLOCK_SIZE) != 0)
                        E0 |= 1ull << (k2 + i);
                }
                /* the candidate pool: blocks of the failed segments that
                 * fall in this stripe (occupied, non-parity, not forced) */
                for (f = 0; f < st_cnt[sx]; f++) {
                    const seal2_segref *g = &fail[st_idx[st_off[sx] + f]];
                    uint64_t lo = g->pba > base ? g->pba : base;
                    uint64_t hi = g->pba + g->plen;
                    uint64_t b;
                    if (hi > base + k2) hi = base + k2;
                    if (hi > zone_end) hi = zone_end;
                    for (b = lo; b < hi; b++) {
                        if (!bit_get(v->bitmap, b) || seal_excluded(&sv, b))
                            continue;
                        if ((E0 >> (b - base)) & 1) continue;
                        pool |= 1u << (b - base);
                    }
                }

                memset(&c, 0, sizeof c);
                c.v = v;
                c.sv = &sv;
                c.algo = algo;
                c.m2 = m2;
                c.base = base;
                c.zone_end = zone_end;
                c.par = &par[sx * m2];
                c.orig = orig;
                c.work = work;
                c.reenc = reenc;
                c.blocks = blocks;
                c.present = present;
                c.dd = dd;
                c.pp = pp;
                c.fail = fail;
                c.fidx = &st_idx[st_off[sx]];
                c.nfail = st_cnt[sx];

                repaired = seal2_repair_stripe(&c, E0, pool, rep, &written);
                if (repaired) {
                    rep->stripes_repaired++;
                    rep->blocks_rewritten += written;
                    st_status[sx] = 1;
                    progress = 1;
                    fprintf(stderr, "[seal2] stripe %llu repaired "
                            "(%llu blocks rewritten)\n",
                            (unsigned long long)sx,
                            (unsigned long long)written);
                }
            }
        } while (progress);
        for (sx = 0; sx < n2; sx++) {
            if (!st_cnt[sx] || st_status[sx]) continue;
            st_status[sx] = 2;
            rep->unrecoverable++;
            fprintf(stderr, "[seal2] stripe %llu UNRECOVERABLE: "
                    "%u failed segment(s), damage beyond m2=%u or "
                    "unprovable -- left untouched\n",
                    (unsigned long long)sx, (unsigned)st_cnt[sx],
                    (unsigned)m2);
        }
    }

    /* ---- final gate: every failed segment re-verified from the device ---- */
    for (i = 0; i < nfail; i++) {
        uint8_t *img2;
        uint64_t b2, s0, s1, sx;
        uint32_t cs2;
        int vok = 0;
        /* no scan-time checkable filter here: a segment whose header was
         * corrupt verifies fine once its stripe was repaired */
        img2 = (uint8_t *)malloc((size_t)fail[i].plen * INVFS_BLOCK_SIZE);
        if (!img2) { rc = -1; goto out; }
        for (b2 = 0; b2 < fail[i].plen; b2++) {
            if (io_seek(&v->io, (fail[i].pba + b2) * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, img2 + b2 * INVFS_BLOCK_SIZE,
                        INVFS_BLOCK_SIZE) != 0)
                memset(img2 + b2 * INVFS_BLOCK_SIZE, 0, INVFS_BLOCK_SIZE);
        }
        vok = seal_seg_verify(img2, fail[i].plen, &cs2) == 0;
        free(img2);
        if (vok) continue;
        s0 = (fail[i].pba - ss) / k2;
        s1 = (fail[i].pba + fail[i].plen - 1 - ss) / k2;
        for (sx = s0; sx <= s1 && sx < n2; sx++) {
            if (st_status[sx] == 1) {
                st_status[sx] = 2;
                rep->stripes_repaired--;
                rep->unrecoverable++;
                fprintf(stderr, "[seal2] stripe %llu: segment at %llu "
                        "still fails CRC after repair -- honest failure\n",
                        (unsigned long long)sx,
                        (unsigned long long)fail[i].pba);
            }
        }
    }

out:
    free(ents);
    free(cand);
    seal2_free_failed(fail, nfail);
    free(st_cnt);
    free(st_off);
    free(st_fill);
    free(st_idx);
    free(st_status);
    free(devmiss);
    free(orig);
    free(work);
    free(reenc);
    free(blocks);
    free(present);
    free(dd);
    free(pp);
    free(par);
    seal_view_free(&sv);
    return rc;
}
