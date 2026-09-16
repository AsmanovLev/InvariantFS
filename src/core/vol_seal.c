/* vol_seal.c — WP20 shadow-zone XOR parity seal + WP20b redundancy
 * configuration / layer-2 RS seal. Split from volume.c. */

#include "volume_internal.h"


/* (Re)allocate the dirty bitmap and mark every shadow block: the state
 * before that moment is simply not tracked, so the next reseal must be a
 * full pass. */
void seal_dirty_reset(invfs_volume *v)
{
    size_t bytes = (size_t)((v->sb.shadow_zone_blocks + 7) / 8);
    if (!v->seal_dirty)
        v->seal_dirty = (uint8_t *)malloc(bytes ? bytes : 1);
    if (v->seal_dirty)
        memset(v->seal_dirty, 0xFF, bytes);
}


/* Mark the shadow-zone blocks [pba, pba+n) dirty (their stripes need a
 * parity recompute at the next reseal). No-op outside the shadow zone or
 * when no seal config exists (the bitmap is NULL then). */
void seal_dirty_mark(invfs_volume *v, uint64_t pba, uint64_t n)
{
    uint64_t ss = v->sb.shadow_zone_start;
    uint64_t b, end;
    if (!v->seal_dirty || !n) return;
    if (pba + n <= ss || pba >= ss + v->sb.shadow_zone_blocks) return;
    if (pba < ss) { n -= ss - pba; pba = ss; }
    end = pba + n;
    if (end > ss + v->sb.shadow_zone_blocks)
        end = ss + v->sb.shadow_zone_blocks;
    for (b = pba; b < end; b++)
        bit_set(v->seal_dirty, b - ss);
}


/* 1 when any block of the stripe is dirty (or nothing is tracked, which
 * is the full-pass fallback); 0 = the stripe is provably unchanged since
 * the last successful reseal. */
static int seal_stripe_dirty(const invfs_volume *v, uint64_t stripe,
                             uint32_t k)
{
    uint64_t base = stripe * k, end = base + k, b;
    if (!v->seal_dirty) return 1;
    if (end > v->sb.shadow_zone_blocks) end = v->sb.shadow_zone_blocks;
    for (b = base; b < end; b++)
        if (bit_get(v->seal_dirty, b)) return 1;
    return 0;
}


/* ==================== WP20: --seal shadow-zone XOR parity ==================
 *
 * Optional parity over the shadow zone (the RAW zone is excluded: it is the
 * hot, transient staging area; everything valuable is swept into Shadow).
 * Stripe s covers the shadow-zone-relative block range
 * [s*k1, (s+1)*k1); its parity block holds the XOR of
 * the stripe's OCCUPIED blocks at seal time (absent blocks read as zero).
 *
 * Ownership mirrors the WP10 text-batch owner: parity blocks are ordinary
 * shadow-zone allocations mapped through the L2P of hidden internal inodes
 * "\x01parity", "\x01parity1", ... (0x01-prefixed names are filtered from
 * listings, swept never, and skipped by dedupe). One owner record (shard)
 * covers SEAL_SHARD stripes -- a policy cap per record, not a format one
 * (WP22a v2 recipe headers count past it; the shard rule stays so records
 * remain small and pre-WP22a readers still parse them) -- and maps
 * block_id = shard-local stripe number to the parity pba, which is what
 * keeps parity blocks live in fsck and what makes the seal survive
 * close/reopen. Shard owners exist contiguously from shard 0 (the state
 * probe stops at the first absent name).
 *
 * Parity blocks are themselves occupied shadow blocks but are NOT covered
 * by the XOR (excluded via the owner's L2P map set, at seal and recovery
 * alike). Covering them would make a re-seal's parity write invalidate an
 * earlier-computed stripe holding that block -- an ordering hazard with no
 * fix inside one pass -- and a corrupt parity block would then poison
 * recovery of its neighbours. RAID's rule: parity is the tool, not the
 * cargo. A corrupt parity block is detected by verify --deep (recompute
 * vs stored) and rewritten by the next re-seal.
 *
 * Re-seal is check-and-update, never delete-then-regenerate: each stripe's
 * parity is recomputed in memory and written back ONLY when it changed
 * (membership churn since the last seal is just recomputed -- the mapping
 * is positional, there is no persistent table beyond the owner AST/L2P).
 * A stripe whose content all went away loses its parity block; a stripe
 * that gained content gains one.
 *
 * Crash ordering follows the tz_seal rules: new maps are flushed durable
 * (step: vol_flush) BEFORE any owner record names them; removals rewrite
 * the owner record FIRST and only then unmap+free. A crash anywhere leaves
 * at worst orphan parity blocks (fsck reclaims them as usual), never a
 * live record pointing at a free block.
 *
 * Read-path recovery (seg_read_checked hooks every framed-segment read):
 * on a CRC/bounds failure of a shadow-zone segment, the stripe syndrome
 * (stored parity XOR the stripe's current occupied blocks) is the corrupt
 * block's XOR delta when exactly one block of the stripe went bad and
 * membership is unchanged. Candidates are spliced into a fresh image of
 * the segment and the segment's OWN framed CRC32C arbitrates -- a wrong
 * reconstruction can never pass it, so drift or double damage degrades to
 * the original EIO, never to garbage. On success the restored block is
 * written back (self-heal) and logged.
 *
 * WP20b: the layer-1 stripe size k1 is runtime-configurable (v->seal_k1,
 * persisted in the RDP0 descriptor; SEAL_STRIPE_K is only the default),
 * and a second, independent layer sits next to it: RS(32+m2, 32) over
 * GF(2^8) (rs.c), m2 parity blocks per 32-block stripe, owned by hidden
 * "\x01parity2", "\x01parity21", ... records (same owner pattern as
 * layer 1: AST entries algo=NONE length=4096, L2P maps; shard-local
 * block_id = stripe*m2 + slot). Layers are independent: a block may be
 * covered by both, and parity blocks of EITHER layer are excluded from
 * BOTH layers' data (parity is the tool, not the cargo). Layer-2
 * recovery is fsck-only (vol_seal2_repair, invoked by invf-fsck
 * --repair): the runtime read path is unchanged and still fails loudly
 * when layer 1 cannot help.
 *
 * (Name-space note: layer-1 shard N is "\x01parity<N>", layer-2 shard N
 * is "\x01parity2<N>": layer-1 shard 2 collides with layer-2 shard 0,
 * but a second shard needs > 65535 stripes -- an 8+ TB shadow zone.)
 */

/* SEAL_STRIPE_K/SEAL2_K live with the WP20b helpers ahead of vol_open */
#define SEAL_SHARD      65535u   /* stripes per owner record (policy cap;
                                  * keeps owner records v1-header sized) */


static void seal_shard_name(uint64_t shard, char *out, size_t cap)
{
    if (shard == 0)
        snprintf(out, cap, "\x01parity");
    else
        snprintf(out, cap, "\x01parity%llu", (unsigned long long)shard);
}


static void seal2_shard_name(uint64_t shard, char *out, size_t cap)
{
    if (shard == 0)
        snprintf(out, cap, "\x01parity2");
    else
        snprintf(out, cap, "\x01parity2%llu", (unsigned long long)shard);
}


/* layer-2 stripes per owner record: one AST entry per parity block */
uint64_t seal2_shard_stripes(uint32_t m2)
{
    return 65535 / m2;
}

void seal_view_free(seal_view *sv)
{
    free(sv->shard_id);
    free(sv->shard2_id);
    free(sv->is_par);
    free(sv->is_ret);
    memset(sv, 0, sizeof *sv);
}


/* the seal's data-membership rule: occupied in the bitmap, never in the
 * exclusion set (parity of both layers + the retention registry) */
int seal_excluded(const seal_view *sv, uint64_t b)
{
    return bit_get(sv->is_par, b) || (sv->is_ret && bit_get(sv->is_ret, b));
}

int seal_view_load(invfs_volume *v, seal_view *sv)
{
    uint64_t shard;
    size_t i;
    uint64_t *ret_id = NULL;
    size_t nret = 0;

    memset(sv, 0, sizeof *sv);
    sv->is_par = (uint8_t *)calloc((size_t)(v->sb.total_blocks + 7) / 8, 1);
    sv->is_ret = (uint8_t *)calloc((size_t)(v->sb.total_blocks + 7) / 8, 1);
    if (!sv->is_par || !sv->is_ret) { free(ret_id); return -1; }
    for (shard = 0;; shard++) {
        char nm[32];
        uint64_t id;
        uint64_t *ns;
        seal_shard_name(shard, nm, sizeof nm);
        id = vol_find(v, nm);
        if (!id) break;      /* shards exist contiguously from 0 */
        ns = (uint64_t *)realloc(sv->shard_id,
                                 (shard + 1) * sizeof *sv->shard_id);
        if (!ns) { free(ret_id); seal_view_free(sv); return -1; }
        sv->shard_id = ns;
        sv->shard_id[shard] = id;
    }
    sv->nshards = (size_t)shard;
    for (shard = 0;; shard++) {
        char nm[32];
        uint64_t id;
        uint64_t *ns;
        seal2_shard_name(shard, nm, sizeof nm);
        id = vol_find(v, nm);
        if (!id) break;      /* layer-2 shards exist contiguously from 0 */
        ns = (uint64_t *)realloc(sv->shard2_id,
                                 (shard + 1) * sizeof *sv->shard2_id);
        if (!ns) { free(ret_id); seal_view_free(sv); return -1; }
        sv->shard2_id = ns;
        sv->shard2_id[shard] = id;
    }
    sv->nshards2 = (size_t)shard;
    /* WP21: retention-registry shards ("\x01reten", "\x01reten1", ...) */
    for (shard = 0;; shard++) {
        char nm[32];
        uint64_t id;
        uint64_t *ns;
        ret_shard_name(shard, nm, sizeof nm);
        id = vol_find(v, nm);
        if (!id) break;
        ns = (uint64_t *)realloc(ret_id, (shard + 1) * sizeof *ret_id);
        if (!ns) { free(ret_id); seal_view_free(sv); return -1; }
        ret_id = ns;
        ret_id[shard] = id;
    }
    nret = (size_t)shard;
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        uint64_t b, bend;
        int is_owner = 0, is_ret = 0;
        if (e->type != INVFS_JRN_MAP) continue;
        for (shard = 0; (size_t)shard < sv->nshards; shard++)
            if (e->inode == sv->shard_id[shard]) { is_owner = 1; break; }
        if (!is_owner)
            for (shard = 0; (size_t)shard < sv->nshards2; shard++)
                if (e->inode == sv->shard2_id[shard]) { is_owner = 1; break; }
        if (!is_owner)
            for (shard = 0; (size_t)shard < nret; shard++)
                if (e->inode == ret_id[shard]) { is_ret = 1; break; }
        if (!is_owner && !is_ret) continue;
        if (e->pba >= v->sb.total_blocks) continue;
        bend = e->pba + (e->length ? e->length : 1);
        if (bend > v->sb.total_blocks) bend = v->sb.total_blocks;
        for (b = e->pba; b < bend; b++)
            bit_set(is_ret ? sv->is_ret : sv->is_par, b);
    }
    free(ret_id);
    return 0;
}


static void seal_xor_block(uint8_t *acc, const uint8_t *blk)
{
    uint64_t *a = (uint64_t *)acc;
    const uint64_t *b = (const uint64_t *)blk;
    size_t i;
    for (i = 0; i < INVFS_BLOCK_SIZE / sizeof(uint64_t); i++)
        a[i] ^= b[i];
}


/* XOR of the stripe's occupied non-parity blocks (absent = zero) into out
 * (out is the accumulator: callers memset or read the parity block into it
 * first). Returns 0, -1 on a device-level read failure. */
static int seal_stripe_xor(invfs_volume *v, const seal_view *sv,
                           uint64_t stripe, uint8_t *acc)
{
    uint64_t base = v->sb.shadow_zone_start + stripe * v->seal_k1;
    uint64_t end = base + v->seal_k1;
    uint64_t shadow_end = v->sb.shadow_zone_start + v->sb.shadow_zone_blocks;
    uint64_t b;
    uint8_t *tmp;

    if (end > shadow_end) end = shadow_end;
    tmp = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    if (!tmp) return -1;
    for (b = base; b < end; b++) {
        if (!bit_get(v->bitmap, b)) continue;
        if (seal_excluded(sv, b)) continue;
        if (io_seek(&v->io, b * INVFS_BLOCK_SIZE) != 0 ||
            io_read(&v->io, tmp, INVFS_BLOCK_SIZE) != 0) {
            free(tmp);
            return -1;
        }
        seal_xor_block(acc, tmp);
    }
    free(tmp);
    return 0;
}


/* The syndrome of one stripe: stored parity XOR the stripe's current
 * occupied content. With no membership drift and exactly one corrupt block
 * in the stripe, this is that block's XOR delta. Returns 1 when the stripe
 * has a parity block (out filled), 0 when it is not sealed / not readable. */
static int seal_syndrome(invfs_volume *v, const seal_view *sv,
                         uint64_t stripe, uint8_t *out)
{
    uint64_t shard = stripe / SEAL_SHARD;
    uint64_t local = stripe % SEAL_SHARD;
    uint64_t ppba = 0, plen = 0;

    if (shard >= sv->nshards || !sv->shard_id[shard]) return 0;
    if (vol_lookup_entry(v, sv->shard_id[shard], local, &ppba, &plen) != 0 ||
        !ppba || ppba >= v->sb.total_blocks)
        return 0;   /* no parity for this stripe (or a corrupt map): out */
    if (io_seek(&v->io, ppba * INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, out, INVFS_BLOCK_SIZE) != 0)
        return 0;   /* the parity block itself is unreadable */
    if (seal_stripe_xor(v, sv, stripe, out) != 0)
        return 0;   /* a stripe-mate is unreadable: cannot isolate */
    return 1;
}


/* A repaired segment image must re-prove itself against its own framing:
 * [4B csize][4B crc32c] and crc32c over the csize payload bytes. crc == 0
 * segments carry no check and can never be recovered-verified. */
int seal_seg_verify(const uint8_t *seg, uint64_t plen,
                           uint32_t *csize_out)
{
    uint32_t csize, crc;
    memcpy(&csize, seg, 4);
    memcpy(&crc, seg + 4, 4);
    if (csize == 0 || crc == 0 ||
        (uint64_t)csize + 8 > plen * INVFS_BLOCK_SIZE)
        return -1;
    if (invfs_crc32c(seg + 8, csize) != crc)
        return -1;
    *csize_out = csize;
    return 0;
}


/* One recovery attempt for a framed segment whose read just failed. Reads
 * the whole segment (plen blocks), tries the stripe-syndrome repairs and
 * returns the verified payload (malloc'd, *csize_out bytes). On success the
 * restored block is written back when the volume is writable and the
 * recovery is logged. -1 = no recovery: the caller's original error.
 *
 * WP27: callers no longer carry the extent (the 32B AST entry has pba but
 * no length field): plen == 0 derives it from the segment's own framed
 * header (csize at pba), capped by the shadow zone's end. When even the
 * header block is torn the derivation fails and the recovery refuses --
 * loud, like any unrecoverable read. */
int seal_recover_segment(invfs_volume *v, uint64_t pba, uint64_t plen,
                                uint32_t *csize_out, uint8_t **blob_out)
{
    seal_view sv;
    uint8_t *seg = NULL, *syn = NULL, *blob = NULL;
    uint64_t ss, i, cb, s0, s1;
    uint64_t healed[8];      /* corrupt blocks the repair spliced in */
    size_t healed_n = 0;
    size_t span;
    uint32_t csize = 0;
    /* one-stripe syndrome memo (blocks are walked in address order) */
    uint64_t syn_s = 0;
    int syn_have = 0, syn_live = 0;
    int rc = -1;

    ss = v->sb.shadow_zone_start;
    if (!plen) {
        /* WP27: derive from the framed header. When even the header block
         * is torn (format v2 stores no length anywhere), fall back to the
         * bitmap: the segment's blocks are one contiguous allocated run
         * starting at pba. An overestimate (adjacent live segments merge
         * into one run) is safe here: the segment's own framed CRC
         * arbitrates every repair attempt, and the write-back touches
         * only the healed blocks. */
        uint8_t hb[8];
        uint32_t hc;
        if (io_seek(&v->io, pba * INVFS_BLOCK_SIZE) != 0 ||
            io_read(&v->io, hb, 8) != 0)
            return -1;
        memcpy(&hc, hb, 4);
        if (hc)
            plen = ((uint64_t)hc + 8 + INVFS_BLOCK_SIZE - 1) /
                   INVFS_BLOCK_SIZE;
        if (!plen || plen > v->sb.total_blocks - pba ||
            pba + plen > ss + v->sb.shadow_zone_blocks) {
            /* torn header: bound the span by the contiguous allocated
             * run (never past the shadow zone's end) */
            uint64_t b, lim = ss + v->sb.shadow_zone_blocks;
            if (pba < ss || pba >= lim)
                return -1;
            plen = 0;
            for (b = pba; b < lim && bit_get(v->bitmap, b); b++)
                plen++;
            if (!plen)
                return -1;
        }
    }
    if (!plen || pba < ss ||
        plen > v->sb.total_blocks - pba)
        return -1;
    if (pba + plen > ss + v->sb.shadow_zone_blocks)
        return -1;   /* the seal covers the shadow zone only */
    if (seal_view_load(v, &sv) != 0)
        return -1;
    if (sv.nshards == 0) { seal_view_free(&sv); return -1; }  /* never sealed */
    if (plen > (uint64_t)SIZE_MAX / INVFS_BLOCK_SIZE) { seal_view_free(&sv); return -1; }
    span = (size_t)plen * INVFS_BLOCK_SIZE;
    seg = (uint8_t *)malloc(span);
    syn = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    if (!seg || !syn) goto out;
    /* per-block reads: a block that fails at the device level reads as
     * zeros here and is exactly what the parity math is asked to restore */
    for (i = 0; i < plen; i++) {
        if (io_seek(&v->io, (pba + i) * INVFS_BLOCK_SIZE) != 0 ||
            io_read(&v->io, seg + i * INVFS_BLOCK_SIZE,
                    INVFS_BLOCK_SIZE) != 0)
            memset(seg + i * INVFS_BLOCK_SIZE, 0, INVFS_BLOCK_SIZE);
    }

    /* pass A: a single corrupt block in its stripe (the common case, and
     * the only drift-tolerant one). With no membership drift since the
     * seal, the stripe's syndrome is exactly the block's XOR delta; the
     * segment's own framed CRC arbitrates, so drift or a second bad block
     * in the stripe can never surface as good bytes. */
    for (i = 0; i < plen; i++) {
        uint64_t s, z;
        cb = pba + i;
        s = (cb - ss) / v->seal_k1;
        if (!syn_have || syn_s != s) {
            syn_live = seal_syndrome(v, &sv, s, syn) == 1;
            if (syn_live) {     /* zero syndrome: nothing to repair with */
                syn_live = 0;
                for (z = 0; z < INVFS_BLOCK_SIZE; z++)
                    if (syn[z]) { syn_live = 1; break; }
            }
            syn_s = s;
            syn_have = 1;
        }
        if (!syn_live) continue;
        seal_xor_block(seg + i * INVFS_BLOCK_SIZE, syn);
        if (seal_seg_verify(seg, plen, &csize) == 0) {
            healed[healed_n++] = cb;
            break;
        }
        seal_xor_block(seg + i * INVFS_BLOCK_SIZE, syn);   /* undo */
    }

    /* pass B: corrupt blocks in SEVERAL of the stripes the segment spans
     * (a pass-A single repair can never verify then). One corrupt block per
     * stripe stays recoverable: odometer over the syndromed stripes'
     * candidate blocks, the framed CRC arbitrating each combination.
     * Bounded: <= 8 syndromed stripes and the combination count times the
     * segment size capped (a wider burst fails loud -- RS-grade repair is
     * the WP20 successor's job). */
    if (!healed_n) {
        uint64_t nspan, t, combos = 1;
        uint64_t st_lo[8], st_hi[8], cur[8];
        uint8_t (*st_syn)[INVFS_BLOCK_SIZE];
        size_t nsyn = 0;
        int ok = 0;

        s0 = (pba - ss) / v->seal_k1;
        s1 = (pba + plen - 1 - ss) / v->seal_k1;
        nspan = s1 - s0 + 1;
        st_syn = malloc(8 * sizeof *st_syn);
        if (st_syn) {
            for (t = 0; t < nspan && nsyn < 8; t++) {
                uint64_t s = s0 + t;
                uint64_t lo, hi, z;
                int nz = 0;
                if (seal_syndrome(v, &sv, s, st_syn[nsyn]) != 1)
                    continue;
                for (z = 0; z < INVFS_BLOCK_SIZE; z++)
                    if (st_syn[nsyn][z]) { nz = 1; break; }
                if (!nz) continue;   /* consistent stripe: no candidate */
                lo = pba > ss + s * v->seal_k1 ? pba
                                                  : ss + s * v->seal_k1;
                hi = ss + (s + 1) * v->seal_k1;
                if (hi > pba + plen) hi = pba + plen;
                if (hi <= lo) continue;
                st_lo[nsyn] = lo;
                st_hi[nsyn] = hi;
                combos *= hi - lo;
                nsyn++;
            }
            if (nsyn >= 2 &&
                combos <= 65536 &&
                combos * ((uint64_t)plen * INVFS_BLOCK_SIZE) <= (1ull << 32)) {
                uint64_t ci;
                for (ci = 0; ci < combos && !ok; ci++) {
                    uint64_t c = ci;
                    size_t j;
                    for (j = 0; j < nsyn; j++) {
                        cur[j] = st_lo[j] + c % (st_hi[j] - st_lo[j]);
                        c /= st_hi[j] - st_lo[j];
                    }
                    for (j = 0; j < nsyn; j++)
                        seal_xor_block(seg + (cur[j] - pba) * INVFS_BLOCK_SIZE,
                                       st_syn[j]);
                    if (seal_seg_verify(seg, plen, &csize) == 0) {
                        for (j = 0; j < nsyn && healed_n < 8; j++)
                            healed[healed_n++] = cur[j];
                        ok = 1;
                    } else {
                        for (j = 0; j < nsyn; j++)
                            seal_xor_block(seg + (cur[j] - pba) *
                                           INVFS_BLOCK_SIZE, st_syn[j]);
                    }
                }
            }
            free(st_syn);
        }
    }
    if (!healed_n) goto out;

    /* self-heal: write the restored block(s) back. A data-only repair --
     * bitmap, journal and superblock are untouched, so no dirty marking;
     * a failed write-back is not fatal (the next read recovers again). */
    if (vol_write_enabled(v)) {
        for (i = 0; i < healed_n; i++) {
            int wok = io_seek(&v->io, healed[i] * INVFS_BLOCK_SIZE) == 0 &&
                      io_write(&v->io,
                               seg + (healed[i] - pba) * INVFS_BLOCK_SIZE,
                               INVFS_BLOCK_SIZE) == 0;
            fprintf(stderr, "[seal] recovered block %llu via parity%s\n",
                    (unsigned long long)healed[i],
                    wok ? "" : " (write-back failed)");
        }
    } else {
        for (i = 0; i < healed_n; i++)
            fprintf(stderr, "[seal] recovered block %llu via parity "
                    "(read-only volume, not written back)\n",
                    (unsigned long long)healed[i]);
    }
    blob = (uint8_t *)malloc(csize);
    if (!blob) goto out;
    memcpy(blob, seg + 8, csize);
    *csize_out = csize;
    *blob_out = blob;
    rc = 0;
out:
    free(seg);
    free(syn);
    seal_view_free(&sv);
    return rc;
}


/* Rewrite one shard's owner record when its entry set changed. `par_pba`
 * is the desired stripe set (stripe -> parity pba, 0 = none). */
static int seal_shard_sync(invfs_volume *v, uint64_t owner, uint64_t shard,
                           tz_owner *o, const uint64_t *par_pba,
                           uint64_t n_stripes)
{
    uint64_t base = shard * SEAL_SHARD;
    uint64_t hi = base + SEAL_SHARD;
    uint32_t want_n = 0, i;
    uint64_t s;
    int same;
    char nm[32];

    if (hi > n_stripes) hi = n_stripes;
    for (s = base; s < hi; s++)
        if (par_pba[s]) want_n++;
    /* compare against the record's current block_id list */
    same = (o->n == want_n);
    if (same) {
        uint64_t w = base;
        for (i = 0; i < o->n && same; i++, w++) {
            while (w < hi && !par_pba[w]) w++;
            if (w >= hi || o->ents[i].block_id != (uint32_t)(w - base))
                same = 0;
        }
    }
    if (same) return 0;

    if (o->cap < want_n) {
        invfs_ast_block_entry *ne = (invfs_ast_block_entry *)
            realloc(o->ents, (want_n ? want_n : 1) * sizeof(*ne));
        if (!ne) return -1;
        o->ents = ne;
        o->cap = want_n;
    }
    o->n = 0;
    for (s = base; s < hi; s++) {
        invfs_ast_block_entry *e;
        if (!par_pba[s]) continue;
        e = &o->ents[o->n++];
        memset(e, 0, sizeof *e);
        e->length = INVFS_BLOCK_SIZE;   /* file_offset rebuilt by the write */
        e->zone = INVFS_ZONE_BINARY;    /* physical home; NOT TEXT: the WP10
                                         * GC marks zone==TEXT block_ids and
                                         * the retire path spares them */
        e->algo = INVFS_ALGO_NONE;
        e->block_id = (uint32_t)(s - base);
        e->block_offset = 0;
        e->pba = par_pba[s];   /* WP27: the owner record is self-describing
                                * (fsck derives the bitmap from records);
                                * the WAL map stays the WAL */
    }
    seal_shard_name(shard, nm, sizeof nm);
    return tz_owner_write(v, owner, nm, o);
}


/* ---- WP20b: redundancy configuration + layer 2 (RS) ------------------ */

void vol_redun_config(invfs_volume *v, uint32_t k1, int l2_algo, uint32_t m2)
{
    uint32_t ok1, om;
    int oa;
    uint32_t nk1, nm;
    int na;

    if (!v) return;
    ok1 = v->seal_k1;
    oa = v->rd_present ? v->rd.l2_algo : 0;
    om = v->rd_present ? v->rd.m2 : 0;
    nk1 = k1 ? k1 : ok1;
    na = l2_algo >= 0 ? l2_algo : oa;
    nm = m2 ? m2 : om;
    if (nk1 < 8) nk1 = 8;
    if (nk1 > 128) nk1 = 128;
    if (na && nm < SEAL2_M2_MIN) nm = SEAL2_M2_MIN;
    if (na && nm > SEAL2_M2_MAX) nm = SEAL2_M2_MAX;
    /* a geometry change invalidates the dirty tracking: full pass */
    if (nk1 != ok1 || na != oa || (na && nm != om))
        seal_dirty_reset(v);
    v->seal_k1 = nk1;
    v->rd.l1_algo = INVFS_RDP0_L1_XOR;
    v->rd.l2_algo = (uint8_t)na;
    v->rd.k1 = (uint16_t)nk1;
    v->rd.k2 = (uint16_t)(na ? SEAL2_K : 0);
    v->rd.m2 = (uint8_t)(na ? nm : 0);
    v->rd_present = 1;
    if (!v->seal_dirty)
        seal_dirty_reset(v);
}


int vol_redun_state(const invfs_volume *v, uint32_t *k1, int *l2_algo,
                    uint32_t *m2)
{
    if (!v) return 0;
    if (k1) *k1 = v->seal_k1;
    if (l2_algo) *l2_algo = v->rd_present ? v->rd.l2_algo : 0;
    if (m2) *m2 = v->rd_present ? v->rd.m2 : 0;
    return v->rd_present;
}


/* Rebuild the layer-2 stripe -> parity-pba map from the journal (newest
 * wins). par has n_stripes*m2 slots: stripe*s slot j at par[s*m2+j]. */
void seal2_map_load(const invfs_volume *v, const seal_view *sv,
                           uint64_t shard_stripes, uint32_t m2,
                           uint64_t n_stripes, uint64_t *par)
{
    size_t i;
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        size_t shard;
        uint64_t gs;
        if (e->type != INVFS_JRN_MAP) continue;
        for (shard = 0; shard < sv->nshards2; shard++)
            if (e->inode == sv->shard2_id[shard]) break;
        if (shard == sv->nshards2) continue;
        gs = shard * shard_stripes + e->lba / m2;
        if (gs < n_stripes && e->pba)
            par[gs * m2 + e->lba % m2] = e->pba;
    }
}


/* Rewrite one layer-2 shard's owner record when its entry set changed.
 * Entries are (stripe, slot) pairs in ascending shard-local block_id
 * order (block_id = local_stripe*m2 + slot). Same pattern as
 * seal_shard_sync. */
static int seal2_shard_sync(invfs_volume *v, uint64_t owner, uint64_t shard,
                            tz_owner *o, const uint64_t *par,
                            uint64_t n_stripes, uint32_t m2)
{
    uint64_t shard_stripes = seal2_shard_stripes(m2);
    uint64_t base = shard * shard_stripes;
    uint64_t hi = base + shard_stripes;
    uint32_t want_n = 0, i;
    uint64_t s, j;
    int same;
    char nm[32];

    if (hi > n_stripes) hi = n_stripes;
    for (s = base; s < hi; s++)
        for (j = 0; j < m2; j++)
            if (par[s * m2 + j]) want_n++;
    same = (o->n == want_n);
    if (same) {
        uint64_t idx = 0;
        for (s = base; s < hi && same; s++)
            for (j = 0; j < m2 && same; j++) {
                if (!par[s * m2 + j]) continue;
                if (idx >= o->n ||
                    o->ents[idx].block_id != (uint32_t)((s - base) * m2 + j))
                    same = 0;
                idx++;
            }
    }
    if (same) return 0;

    if (o->cap < want_n) {
        invfs_ast_block_entry *ne = (invfs_ast_block_entry *)
            realloc(o->ents, (want_n ? want_n : 1) * sizeof(*ne));
        if (!ne) return -1;
        o->ents = ne;
        o->cap = want_n;
    }
    o->n = 0;
    for (s = base; s < hi; s++) {
        for (j = 0; j < m2; j++) {
            invfs_ast_block_entry *e;
            if (!par[s * m2 + j]) continue;
            e = &o->ents[o->n++];
            memset(e, 0, sizeof *e);
            e->length = INVFS_BLOCK_SIZE;
            e->zone = INVFS_ZONE_BINARY;   /* see seal_shard_sync */
            e->algo = INVFS_ALGO_NONE;
            e->block_id = (uint32_t)((s - base) * m2 + j);
            e->block_offset = 0;
            e->pba = par[s * m2 + j];   /* WP27: self-describing */
        }
    }
    seal2_shard_name(shard, nm, sizeof nm);
    return tz_owner_write(v, owner, nm, o);
}


/* The layer-2 seal pass (called by vol_seal after the layer-1 pass, with
 * the same live seal_view): check-and-update RS(32+m2, 32) parity over
 * the shadow zone. Mirrors the layer-1 rules: absent blocks encode as
 * zero, parity blocks of both layers are excluded from the data, new maps
 * are flushed durable before any owner record names them, stripes are
 * recomputed only when their blocks dirtied since the last reseal. */
static int vol_seal_l2(invfs_volume *v, seal_view *sv, invfs_seal_report *rep)
{
    const uint32_t k2 = SEAL2_K;
    const uint32_t m2 = v->rd.m2;
    const int algo = v->rd.l2_algo;
    uint64_t shard_stripes = seal2_shard_stripes(m2);
    uint64_t ss = v->sb.shadow_zone_start, snb = v->sb.shadow_zone_blocks;
    uint64_t n_stripes = (snb + k2 - 1) / k2;
    uint64_t s, j, b;
    uint64_t occupied = 0;
    uint64_t *par = NULL;    /* (stripe*m2 + slot) -> parity pba (0 = none) */
    uint8_t *want = NULL;    /* stripe holds >=1 occupied non-parity block */
    uint8_t *data = NULL, *pacc = NULL, *stored = NULL;
    uint8_t **dp = NULL, **pp = NULL;
    tz_owner *own = NULL;
    size_t own_cap = 0;
    int rc = 0;
    size_t i;

    par = (uint64_t *)calloc((size_t)n_stripes * m2, sizeof *par);
    want = (uint8_t *)calloc((size_t)(n_stripes + 7) / 8, 1);
    data = (uint8_t *)malloc((size_t)k2 * INVFS_BLOCK_SIZE);
    pacc = (uint8_t *)malloc((size_t)m2 * INVFS_BLOCK_SIZE);
    stored = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    dp = (uint8_t **)malloc(k2 * sizeof *dp);
    pp = (uint8_t **)malloc(m2 * sizeof *pp);
    if (!par || !want || !data || !pacc || !stored || !dp || !pp) {
        rc = -1;
        goto out;
    }
    for (i = 0; i < k2; i++) dp[i] = data + i * INVFS_BLOCK_SIZE;
    for (i = 0; i < m2; i++) pp[i] = pacc + i * INVFS_BLOCK_SIZE;

    seal2_map_load(v, sv, shard_stripes, m2, n_stripes, par);
    if (sv->nshards2) {
        own = (tz_owner *)calloc(sv->nshards2, sizeof *own);
        if (!own) { rc = -1; goto out; }
        own_cap = sv->nshards2;
        for (i = 0; i < sv->nshards2; i++) {
            if (tz_owner_load(v, sv->shard2_id[i], &own[i]) != 0) {
                fprintf(stderr, "seal2: owner shard %zu unreadable\n", i);
                rc = -1;
                goto out;
            }
        }
    }

    /* membership from the live bitmap, parity blocks (both layers) out */
    for (s = 0; s < n_stripes; s++) {
        uint64_t base = ss + s * k2, end = base + k2, cnt = 0;
        if (end > ss + snb) end = ss + snb;
        for (b = base; b < end; b++)
            if (bit_get(v->bitmap, b) && !seal_excluded(sv, b)) cnt++;
        if (cnt) {
            want[s / 8] |= (uint8_t)(1u << (s % 8));
            occupied += cnt;
        }
    }

    /* allocate the parity slots a wanted stripe is missing */
    for (s = 0; s < n_stripes; s++) {
        uint64_t shard;
        int missing = 0;
        if (!(want[s / 8] & (1u << (s % 8)))) continue;
        for (j = 0; j < m2; j++)
            if (!par[s * m2 + j]) missing = 1;
        if (!missing) continue;
        shard = s / shard_stripes;
        /* shards exist contiguously: create the missing owners up to the
         * one this stripe needs (empty records; the sync below fills
         * them) */
        while (shard >= sv->nshards2) {
            char nm[32];
            uint64_t nid;
            uint64_t *ns;
            tz_owner *no;
            seal2_shard_name(sv->nshards2, nm, sizeof nm);
            nid = vol_create_file(v, nm, NULL, 0);
            if (!nid) { rc = -1; goto finalize; }
            ns = (uint64_t *)realloc(sv->shard2_id,
                                     (sv->nshards2 + 1) *
                                     sizeof *sv->shard2_id);
            if (!ns) { rc = -1; goto finalize; }
            sv->shard2_id = ns;
            sv->shard2_id[sv->nshards2] = nid;
            no = (tz_owner *)realloc(own, (sv->nshards2 + 1) * sizeof *own);
            if (!no) { rc = -1; goto finalize; }
            own = no;
            memset(&own[sv->nshards2], 0, sizeof *own);
            own_cap = sv->nshards2 + 1;
            /* load the fresh record's position so the sync's rewrite
             * position-kills it (see the layer-1 loop) */
            if (tz_owner_load(v, nid, &own[sv->nshards2]) != 0) {
                rc = -1;
                goto finalize;
            }
            sv->nshards2++;
        }
        for (j = 0; j < m2; j++) {
            uint64_t pba;
            if (par[s * m2 + j]) continue;
            pba = alloc_blocks(v, ss, snb, 1, 1, INVFS_ALLOC_DATA);
            if (!pba) {
                fprintf(stderr, "seal2: ENOSPC for parity %llu of stripe "
                        "%llu\n", (unsigned long long)j,
                        (unsigned long long)s);
                rep->l2_unprotected++;
                continue;
            }
            if (vol_map(v, sv->shard2_id[shard],
                        (s % shard_stripes) * m2 + j, pba, 1) != 0) {
                vol_free_blocks(v, pba, 1);
                rep->l2_unprotected++;
                continue;
            }
            par[s * m2 + j] = pba;
            bit_set(sv->is_par, pba);
            rep->l2_added++;
        }
    }
    /* maps durable BEFORE any owner record names them (the tz_seal rule) */
    if (rep->l2_added && vol_flush(v) != 0) { rc = -1; goto finalize; }

    /* check-and-update every wanted stripe with complete parity */
    for (s = 0; s < n_stripes; s++) {
        int full = 1;
        if (!(want[s / 8] & (1u << (s % 8)))) continue;
        for (j = 0; j < m2; j++)
            if (!par[s * m2 + j]) full = 0;
        if (!full) continue;
        if (!seal_stripe_dirty(v, s, k2)) {
            rep->l2_dirty_skipped++;
            continue;
        }
        /* data slots: occupied non-parity blocks, everything else zero */
        memset(data, 0, (size_t)k2 * INVFS_BLOCK_SIZE);
        for (i = 0; i < k2; i++) {
            b = ss + s * k2 + i;
            if (b >= ss + snb) break;
            if (!bit_get(v->bitmap, b) || seal_excluded(sv, b)) continue;
            if (io_seek(&v->io, b * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, data + i * INVFS_BLOCK_SIZE,
                        INVFS_BLOCK_SIZE) != 0) {
                rc = -1;
                goto finalize;
            }
        }
        if (rs_encode(algo, k2, m2, INVFS_BLOCK_SIZE, dp, pp) != 0) {
            rc = -1;
            goto finalize;
        }
        for (j = 0; j < m2; j++) {
            uint64_t pb = par[s * m2 + j];
            if (io_seek(&v->io, pb * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, stored, INVFS_BLOCK_SIZE) != 0) {
                rc = -1;
                goto finalize;
            }
            if (memcmp(pp[j], stored, INVFS_BLOCK_SIZE) == 0) {
                rep->l2_unchanged++;
            } else {
                if (io_seek(&v->io, pb * INVFS_BLOCK_SIZE) != 0 ||
                    io_write(&v->io, pp[j], INVFS_BLOCK_SIZE) != 0) {
                    rc = -1;
                    goto finalize;
                }
                rep->l2_updated++;
            }
        }
    }

finalize:
    /* live layer-2 state after this run */
    for (s = 0; s < n_stripes; s++) {
        int full = 1;
        if (!(want[s / 8] & (1u << (s % 8)))) continue;
        for (j = 0; j < m2; j++)
            if (!par[s * m2 + j]) full = 0;
        if (full) rep->l2_stripes++;
    }
    rep->l2_parity_blocks = rep->l2_stripes * m2;
    for (i = 0; i < sv->nshards2; i++) {
        if (i < own_cap &&
            seal2_shard_sync(v, sv->shard2_id[i], i, &own[i],
                             par, n_stripes, m2) != 0) {
            fprintf(stderr, "seal2: owner shard %zu write failed\n", i);
            rc = -1;
        }
    }
    /* removals: stripes whose content went away, and stray maps no record
     * names (an earlier interrupted seal's debris) */
    {
        size_t j2, w2;
        for (j2 = 0; j2 < v->l2p_count; j2++) {
            const invfs_l2p_entry *e = &v->l2p[j2];
            size_t shard;
            uint64_t gs, slot;
            if (e->type != INVFS_JRN_MAP) continue;
            for (shard = 0; shard < sv->nshards2; shard++)
                if (e->inode == sv->shard2_id[shard]) break;
            if (shard == sv->nshards2) continue;
            gs = shard * shard_stripes + e->lba / m2;
            slot = e->lba % m2;
            if (gs < n_stripes && par[gs * m2 + slot] == e->pba &&
                (want[gs / 8] & (1u << (gs % 8))))
                continue;   /* live seal member */
            if (e->pba < v->sb.total_blocks)
                vol_free_blocks(v, e->pba, e->length ? e->length : 1);
            rep->l2_freed++;
        }
        w2 = 0;
        for (j2 = 0; j2 < v->l2p_count; j2++) {
            const invfs_l2p_entry *e = &v->l2p[j2];
            size_t shard;
            uint64_t gs, slot;
            int keep = 1;
            if (e->type == INVFS_JRN_MAP) {
                for (shard = 0; shard < sv->nshards2; shard++)
                    if (e->inode == sv->shard2_id[shard]) break;
                if (shard < sv->nshards2) {
                    gs = shard * shard_stripes + e->lba / m2;
                    slot = e->lba % m2;
                    keep = gs < n_stripes && par[gs * m2 + slot] == e->pba &&
                           (want[gs / 8] & (1u << (gs % 8)));
                }
            }
            if (keep) {
                if (w2 != j2) v->l2p[w2] = v->l2p[j2];
                w2++;
            } else if (e->type == INVFS_JRN_MAP) {
                /* WP22d: the journal is append-only -- the stale map is
                 * cancelled by an UNMAP op, never rewritten in place */
                invfs_l2p_entry ue;
                memset(&ue, 0, sizeof ue);
                ue.type = INVFS_JRN_UNMAP;
                ue.inode = e->inode;
                ue.lba = e->lba;
                jrn_push_op(v, &ue);
            }
        }
        v->l2p_count = w2;
        /* WP-L2Q: the keep test is pba-dependent, so a key can keep one
         * duplicate while dropping another -- a by-key index delete is not
         * provably exact here. Rebuild the index from the final table. */
        l2p_idx_rebuild(v);
    }
    if (rep->l2_freed || rep->l2_added || rep->l2_updated) {
        if (vol_flush(v) != 0) rc = -1;
    }
    rep->l2_overhead_pct = occupied
        ? 100.0 * (double)rep->l2_parity_blocks / (double)occupied : 0.0;
out:
    free(par);
    free(want);
    free(data);
    free(pacc);
    free(stored);
    free(dp);
    free(pp);
    if (own)
        for (i = 0; i < own_cap; i++) tz_owner_free(&own[i]);
    free(own);
    return rc;
}


/* verify --deep layer-2 leg: recompute every layer-2 stripe's RS parity
 * from the current data and compare against the stored parity blocks.
 * Full scan (the dirty bitmap is a reseal optimization, not a verifier). */
static int vol_seal2_verify_leg(invfs_volume *v, const seal_view *sv,
                                invfs_seal_verify *out)
{
    const uint32_t k2 = SEAL2_K;
    const uint32_t m2 = v->rd.m2;
    const int algo = v->rd.l2_algo;
    uint64_t shard_stripes = seal2_shard_stripes(m2);
    uint64_t ss = v->sb.shadow_zone_start, snb = v->sb.shadow_zone_blocks;
    uint64_t n_stripes = (snb + k2 - 1) / k2;
    uint64_t s, j, b;
    uint64_t *par = NULL;
    uint8_t *data = NULL, *pacc = NULL, *stored = NULL;
    uint8_t **dp = NULL, **pp = NULL;
    int rc = 0;
    size_t i;

    par = (uint64_t *)calloc((size_t)n_stripes * m2, sizeof *par);
    data = (uint8_t *)malloc((size_t)k2 * INVFS_BLOCK_SIZE);
    pacc = (uint8_t *)malloc((size_t)m2 * INVFS_BLOCK_SIZE);
    stored = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    dp = (uint8_t **)malloc(k2 * sizeof *dp);
    pp = (uint8_t **)malloc(m2 * sizeof *pp);
    if (!par || !data || !pacc || !stored || !dp || !pp) { rc = -1; goto out; }
    for (i = 0; i < k2; i++) dp[i] = data + i * INVFS_BLOCK_SIZE;
    for (i = 0; i < m2; i++) pp[i] = pacc + i * INVFS_BLOCK_SIZE;

    seal2_map_load(v, sv, shard_stripes, m2, n_stripes, par);

    for (s = 0; s < n_stripes; s++) {
        uint64_t base = ss + s * k2, end = base + k2, cnt = 0, mapped = 0;
        if (end > ss + snb) end = ss + snb;
        for (b = base; b < end; b++)
            if (bit_get(v->bitmap, b) && !seal_excluded(sv, b)) cnt++;
        for (j = 0; j < m2; j++)
            if (par[s * m2 + j]) mapped++;
        if (cnt && !mapped) { out->missing2++; continue; }
        if (!cnt && mapped) { out->extra2++; continue; }
        if (!cnt) continue;
        if (mapped != m2) { out->mismatched2++; out->sealed2++; continue; }
        memset(data, 0, (size_t)k2 * INVFS_BLOCK_SIZE);
        for (i = 0; i < k2; i++) {
            b = base + i;
            if (!bit_get(v->bitmap, b) || seal_excluded(sv, b)) continue;
            if (io_seek(&v->io, b * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, data + i * INVFS_BLOCK_SIZE,
                        INVFS_BLOCK_SIZE) != 0) {
                out->mismatched2++;   /* unreadable while checking */
                out->sealed2++;
                goto next_stripe;
            }
        }
        if (rs_encode(algo, k2, m2, INVFS_BLOCK_SIZE, dp, pp) != 0) {
            rc = -1;
            goto out;
        }
        for (j = 0; j < m2; j++) {
            if (io_seek(&v->io, par[s * m2 + j] * INVFS_BLOCK_SIZE) != 0 ||
                io_read(&v->io, stored, INVFS_BLOCK_SIZE) != 0 ||
                memcmp(pp[j], stored, INVFS_BLOCK_SIZE) != 0) {
                out->mismatched2++;
                break;
            }
        }
        out->sealed2++;
    next_stripe:
        ;
    }
out:
    free(par);
    free(data);
    free(pacc);
    free(stored);
    free(dp);
    free(pp);
    return rc;
}


int vol_seal(invfs_volume *v, int unseal, invfs_seal_report *rep)
{
    seal_view sv;
    tz_owner *own = NULL;
    size_t own_cap = 0;
    uint64_t ss, snb, n_stripes, s, b;
    uint64_t occupied = 0;
    uint64_t *par_pba = NULL;    /* stripe -> parity pba (0 = none) */
    uint8_t *want = NULL;        /* stripe holds >=1 occupied non-parity blk */
    uint8_t *acc = NULL, *stored = NULL;
    int rc = 0;
    size_t i;

    if (!v || !rep) return -1;
    memset(rep, 0, sizeof *rep);
    if (!vol_write_enabled(v)) {
        fprintf(stderr, "seal: volume is read-only; cannot %s\n",
                unseal ? "unseal" : "seal");
        return -1;
    }
    if (seal_view_load(v, &sv) != 0) return -1;

    if (unseal) {
        /* free every parity block and remove the owners (BOTH layers).
         * The retire path frees the blocks the owner's maps name (entries
         * are zone!=TEXT, so the shared-batch gate does not hold them
         * back) and drops the maps; the tombstone lands in the same run.
         * The RDP0 descriptor goes too: nothing is configured any more. */
        if ((sv.nshards || sv.nshards2 || v->rd_present) &&
            vol_mark_dirty(v) != 0) { seal_view_free(&sv); return -1; }
        for (i = 0; i < sv.nshards + sv.nshards2; i++) {
            char nm[32];
            size_t j;
            uint64_t oid = i < sv.nshards ? sv.shard_id[i]
                                          : sv.shard2_id[i - sv.nshards];
            if (i < sv.nshards) seal_shard_name(i, nm, sizeof nm);
            else                seal2_shard_name(i - sv.nshards, nm, sizeof nm);
            for (j = 0; j < v->l2p_count; j++) {
                const invfs_l2p_entry *e = &v->l2p[j];
                if (e->type == INVFS_JRN_MAP && e->inode == oid)
                    rep->freed += e->length ? e->length : 1;
            }
            if (vol_delete_file(v, nm) != 0) {
                fprintf(stderr, "seal: could not remove owner shard %zu\n", i);
                rc = -1;
            }
        }
        if (vol_write_rdp0(v, NULL) != 0) {
            fprintf(stderr, "seal: could not clear the RDP0 descriptor\n");
            rc = -1;
        }
        free(v->seal_dirty);
        v->seal_dirty = NULL;
        if (vol_flush(v) != 0) rc = -1;
        seal_view_free(&sv);
        return rc;
    }

    /* configure/reseal: the dirty bitmap must exist (a NULL one = "no
     * tracking state", which has to mean a full pass) */
    if (!v->seal_dirty)
        seal_dirty_reset(v);
    if (!v->seal_dirty) { seal_view_free(&sv); return -1; }

    ss = v->sb.shadow_zone_start;
    snb = v->sb.shadow_zone_blocks;
    if (!snb) { seal_view_free(&sv); return 0; }
    n_stripes = (snb + v->seal_k1 - 1) / v->seal_k1;
    par_pba = (uint64_t *)calloc(n_stripes, sizeof *par_pba);
    want = (uint8_t *)calloc((size_t)(n_stripes + 7) / 8, 1);
    acc = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    stored = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    if (!par_pba || !want || !acc || !stored) { rc = -1; goto out; }

    /* current stripe -> parity pba map from the journal (newest wins, the
     * vol_lookup_entry rule); strays outside the zone are fsck's business */
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        size_t shard;
        if (e->type != INVFS_JRN_MAP) continue;
        for (shard = 0; shard < sv.nshards; shard++)
            if (e->inode == sv.shard_id[shard]) break;
        if (shard == sv.nshards) continue;
        s = shard * SEAL_SHARD + e->lba;
        if (s < n_stripes && e->pba) par_pba[s] = e->pba;
    }
    /* owner record state per shard (position-kill targets) */
    if (sv.nshards) {
        own = (tz_owner *)calloc(sv.nshards, sizeof *own);
        if (!own) { rc = -1; goto out; }
        own_cap = sv.nshards;
        for (i = 0; i < sv.nshards; i++) {
            if (tz_owner_load(v, sv.shard_id[i], &own[i]) != 0) {
                fprintf(stderr, "seal: owner shard %zu unreadable\n", i);
                rc = -1;
                goto out;
            }
        }
    }

    if (vol_mark_dirty(v) != 0) { rc = -1; goto out; }

    /* membership from the live bitmap, parity blocks excluded */
    for (s = 0; s < n_stripes; s++) {
        uint64_t base = ss + s * v->seal_k1;
        uint64_t end = base + v->seal_k1;
        uint64_t cnt = 0;
        if (end > ss + snb) end = ss + snb;
        for (b = base; b < end; b++)
            if (bit_get(v->bitmap, b) && !seal_excluded(&sv, b)) cnt++;
        if (cnt) {
            want[s / 8] |= (uint8_t)(1u << (s % 8));
            occupied += cnt;
        }
    }

    /* allocate parity where a stripe wants it but has none */
    for (s = 0; s < n_stripes; s++) {
        uint64_t shard, pba;
        if (!(want[s / 8] & (1u << (s % 8))) || par_pba[s]) continue;
        shard = s / SEAL_SHARD;
        /* shards exist contiguously: create the missing owners up to the
         * one this stripe needs (empty records; the sync below fills them) */
        while (shard >= sv.nshards) {
            char nm[32];
            uint64_t nid;
            uint64_t *ns;
            tz_owner *no;
            seal_shard_name(sv.nshards, nm, sizeof nm);
            nid = vol_create_file(v, nm, NULL, 0);
            if (!nid) { rc = -1; goto finalize; }
            ns = (uint64_t *)realloc(sv.shard_id,
                                     (sv.nshards + 1) * sizeof *sv.shard_id);
            if (!ns) { rc = -1; goto finalize; }
            sv.shard_id = ns;
            sv.shard_id[sv.nshards] = nid;
            no = (tz_owner *)realloc(own, (sv.nshards + 1) * sizeof *own);
            if (!no) { rc = -1; goto finalize; }
            own = no;
            memset(&own[sv.nshards], 0, sizeof *own);
            own_cap = sv.nshards + 1;
            /* load the fresh record's position so the sync's rewrite
             * position-kills it (a created-but-never-killed empty record
             * would stay live next to the full one) */
            if (tz_owner_load(v, nid, &own[sv.nshards]) != 0) {
                rc = -1;
                goto finalize;
            }
            sv.nshards++;
        }
        pba = alloc_blocks(v, ss, snb, 1, 1, INVFS_ALLOC_DATA);
        if (!pba) {
            fprintf(stderr, "seal: ENOSPC for parity of stripe %llu\n",
                    (unsigned long long)s);
            rep->unprotected++;
            continue;
        }
        if (vol_map(v, sv.shard_id[shard], s % SEAL_SHARD, pba, 1) != 0) {
            vol_free_blocks(v, pba, 1);
            rep->unprotected++;
            continue;
        }
        par_pba[s] = pba;
        bit_set(sv.is_par, pba);
        rep->added++;
    }
    /* maps durable BEFORE any owner record names them (the tz_seal rule) */
    if (rep->added && vol_flush(v) != 0) { rc = -1; goto finalize; }

    /* check-and-update every wanted stripe: recompute, write only on change.
     * WP20b: stripes whose blocks are all clean in the dirty bitmap cannot
     * have drifted (every write/alloc/free path marks them), so their
     * recompute is skipped; the first seal of a session is always a full
     * pass (the bitmap starts all-ones). */
    for (s = 0; s < n_stripes; s++) {
        if (!(want[s / 8] & (1u << (s % 8))) || !par_pba[s]) continue;
        if (!seal_stripe_dirty(v, s, v->seal_k1)) {
            rep->dirty_skipped++;
            continue;
        }
        memset(acc, 0, INVFS_BLOCK_SIZE);
        if (seal_stripe_xor(v, &sv, s, acc) != 0) { rc = -1; goto finalize; }
        if (io_seek(&v->io, par_pba[s] * INVFS_BLOCK_SIZE) != 0 ||
            io_read(&v->io, stored, INVFS_BLOCK_SIZE) != 0) {
            rc = -1;
            goto finalize;
        }
        if (memcmp(acc, stored, INVFS_BLOCK_SIZE) == 0) {
            rep->unchanged++;
        } else {
            if (io_seek(&v->io, par_pba[s] * INVFS_BLOCK_SIZE) != 0 ||
                io_write(&v->io, acc, INVFS_BLOCK_SIZE) != 0) {
                rc = -1;
                goto finalize;
            }
            rep->updated++;
        }
    }

finalize:
    /* live seal state after this run (also what a hard error leaves) */
    for (s = 0; s < n_stripes; s++)
        if (par_pba[s] && (want[s / 8] & (1u << (s % 8))))
            rep->stripes++;
    rep->parity_blocks = rep->stripes;
    /* owner records name exactly the final map set (additions landed above,
     * removals drop here BEFORE their maps/blocks go away) */
    for (i = 0; i < sv.nshards; i++) {
        if (i < own_cap &&
            seal_shard_sync(v, sv.shard_id[i], i, &own[i],
                            par_pba, n_stripes) != 0) {
            fprintf(stderr, "seal: owner shard %zu write failed\n", i);
            rc = -1;
        }
    }
    /* removals: stripes whose content went away (and stray maps no record
     * names -- an earlier interrupted seal's debris) */
    {
        size_t j, w2;
        for (j = 0; j < v->l2p_count; j++) {
            const invfs_l2p_entry *e = &v->l2p[j];
            size_t shard;
            uint64_t gs;
            if (e->type != INVFS_JRN_MAP) continue;
            for (shard = 0; shard < sv.nshards; shard++)
                if (e->inode == sv.shard_id[shard]) break;
            if (shard == sv.nshards) continue;
            gs = shard * SEAL_SHARD + e->lba;
            if (gs < n_stripes && par_pba[gs] == e->pba &&
                (want[gs / 8] & (1u << (gs % 8))))
                continue;   /* live seal member */
            if (e->pba < v->sb.total_blocks)
                vol_free_blocks(v, e->pba, e->length ? e->length : 1);
            rep->freed++;
        }
        /* drop the removed maps (collect-then-remove is unnecessary here:
         * the loop above only READS; this pass removes everything not in
         * the final set, matching the rewritten records) */
        w2 = 0;
        for (j = 0; j < v->l2p_count; j++) {
            const invfs_l2p_entry *e = &v->l2p[j];
            size_t shard;
            uint64_t gs;
            int keep = 1;
            if (e->type == INVFS_JRN_MAP) {
                for (shard = 0; shard < sv.nshards; shard++)
                    if (e->inode == sv.shard_id[shard]) break;
                if (shard < sv.nshards) {
                    gs = shard * SEAL_SHARD + e->lba;
                    keep = gs < n_stripes && par_pba[gs] == e->pba &&
                           (want[gs / 8] & (1u << (gs % 8)));
                }
            }
            if (keep) {
                if (w2 != j) v->l2p[w2] = v->l2p[j];
                w2++;
            } else if (e->type == INVFS_JRN_MAP) {
                invfs_l2p_entry ue;
                memset(&ue, 0, sizeof ue);
                ue.type = INVFS_JRN_UNMAP;
                ue.inode = e->inode;
                ue.lba = e->lba;
                jrn_push_op(v, &ue);
            }
        }
        v->l2p_count = w2;
        /* WP-L2Q: pba-dependent keep test -> rebuild the session index
         * from the final table (see the layer-2 pass above). */
        l2p_idx_rebuild(v);
    }
    if (rep->freed || rep->added || rep->updated) {
        if (vol_flush(v) != 0) rc = -1;
    }
    rep->overhead_pct = occupied
        ? 100.0 * (double)rep->parity_blocks / (double)occupied : 0.0;

    /* WP20b layer 2 (independent of layer 1, same occupancy model) */
    if (rc == 0 && v->rd_present && v->rd.l2_algo &&
        v->rd.m2 >= SEAL2_M2_MIN && v->rd.m2 <= SEAL2_M2_MAX &&
        vol_seal_l2(v, &sv, rep) != 0) {
        fprintf(stderr, "seal: layer-2 pass failed\n");
        rc = -1;
    }
    /* persist the redundancy configuration (the RDP0 descriptor): this is
     * what makes the seal findable after close/reopen (auto-reseal) and
     * what the geometry (k1/k2/m2) on disk is read back from */
    if (rc == 0) {
        invfs_rdp0 rd = v->rd;
        rd.l1_algo = INVFS_RDP0_L1_XOR;
        rd.k1 = (uint16_t)v->seal_k1;
        rd.k2 = (uint16_t)(rd.l2_algo ? SEAL2_K : 0);
        if (!rd.l2_algo) rd.m2 = 0;
        rd.parity_area_hint = rep->parity_blocks + rep->l2_parity_blocks;
        if (vol_write_rdp0(v, &rd) != 0) {
            fprintf(stderr, "seal: RDP0 descriptor write failed\n");
            rc = -1;
        } else if (vol_flush(v) != 0) {
            rc = -1;
        }
    }
    /* a fully successful reseal leaves nothing dirty */
    if (rc == 0 && v->seal_dirty)
        memset(v->seal_dirty, 0,
               (size_t)((v->sb.shadow_zone_blocks + 7) / 8));
out:
    free(par_pba);
    free(want);
    free(acc);
    free(stored);
    if (own)
        for (i = 0; i < own_cap; i++) tz_owner_free(&own[i]);
    free(own);
    seal_view_free(&sv);
    return rc;
}


/* verify --deep leg: recompute every sealed stripe against its stored
 * parity block and report the drift counters. */
int vol_seal_verify(invfs_volume *v, invfs_seal_verify *out)
{
    seal_view sv;
    uint64_t ss, snb, n_stripes, s, b;
    uint64_t *par_pba = NULL;
    uint8_t *acc = NULL, *stored = NULL;
    size_t i;
    int rc = 0;

    if (!v || !out) return -1;
    memset(out, 0, sizeof *out);
    if (seal_view_load(v, &sv) != 0) return -1;
    /* not sealed at all: neither layer has owners */
    if (sv.nshards == 0 && sv.nshards2 == 0) { seal_view_free(&sv); return 0; }

    ss = v->sb.shadow_zone_start;
    snb = v->sb.shadow_zone_blocks;
    n_stripes = (snb + v->seal_k1 - 1) / v->seal_k1;
    par_pba = (uint64_t *)calloc(n_stripes, sizeof *par_pba);
    acc = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    stored = (uint8_t *)malloc(INVFS_BLOCK_SIZE);
    if (!par_pba || !acc || !stored) { rc = -1; goto out; }

    if (sv.nshards) {
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        size_t shard;
        if (e->type != INVFS_JRN_MAP) continue;
        for (shard = 0; shard < sv.nshards; shard++)
            if (e->inode == sv.shard_id[shard]) break;
        if (shard == sv.nshards) continue;
        s = shard * SEAL_SHARD + e->lba;
        if (s < n_stripes && e->pba) par_pba[s] = e->pba;
    }

    for (s = 0; s < n_stripes; s++) {
        uint64_t base = ss + s * v->seal_k1;
        uint64_t end = base + v->seal_k1;
        uint64_t cnt = 0;
        if (end > ss + snb) end = ss + snb;
        for (b = base; b < end; b++)
            if (bit_get(v->bitmap, b) && !seal_excluded(&sv, b)) cnt++;
        if (cnt && !par_pba[s]) { out->missing++; continue; }
        if (!cnt && par_pba[s]) { out->extra++; continue; }
        if (!cnt) continue;
        memset(acc, 0, INVFS_BLOCK_SIZE);
        if (seal_stripe_xor(v, &sv, s, acc) != 0 ||
            io_seek(&v->io, par_pba[s] * INVFS_BLOCK_SIZE) != 0 ||
            io_read(&v->io, stored, INVFS_BLOCK_SIZE) != 0) {
            out->mismatched++;   /* unreadable while checking: not clean */
            out->sealed++;
            continue;
        }
        if (memcmp(acc, stored, INVFS_BLOCK_SIZE) != 0)
            out->mismatched++;
        out->sealed++;
    }
    }
    /* WP20b layer-2 leg (full scan, like layer 1): recompute every sealed
     * RS stripe against its stored parity blocks. Needs the live
     * descriptor (algo/m2 are not recoverable from the owners alone). */
    if (sv.nshards2 && v->rd_present && v->rd.l2_algo &&
        v->rd.m2 >= SEAL2_M2_MIN && v->rd.m2 <= SEAL2_M2_MAX)
        rc = vol_seal2_verify_leg(v, &sv, out);
    else if (sv.nshards2) {
        /* owners but no readable config: every mapped stripe is drift */
        fprintf(stderr, "verify: layer-2 owners present but no valid RDP0 "
                        "descriptor; layer 2 not verifiable\n");
        out->mismatched2 += sv.nshards2;   /* honest drift, not "clean" */
    }
out:
    free(par_pba);
    free(acc);
    free(stored);
    seal_view_free(&sv);
    return rc;
}
