/* vol_tier.c — WP25: two-device heat tiering + the RAW mirror.
 *
 * Split from the WP25 work in volume.c. Two small L2P-visible second-copy
 * indexes, both persisted through hidden owner records (the WP20
 * seal-owner pattern: ordinary L2P maps keyed by the owner inode id, one
 * AST entry per copy so fsck keeps the blocks live):
 *
 *   "\x01rawm"  — the RAW mirror (raw_mirror=1, the default): every
 *     raw-zone segment written lands twice, once on dev0 (the RAW zone
 *     proper) and once in the canonical shadow on dev1. Key = the raw-zone
 *     pba; the map's L2P lba = the raw-zone-RELATIVE block index (fits the
 *     AST entry's 24-bit block_id because dev0 is capped at 2^24 blocks).
 *     Writes are part of write_segment_blocks (writethrough); a dev0 write
 *     failure logs + continues on the dev1 mirror (WP22c rule applied
 *     per-device), a dev1 failure latches. Reads use the mirror only as a
 *     failover (dev0 absent / io error / torn frame -- the segment's own
 *     CRC32C governs).
 *
 *   "\x01tier0" — the dev0 tier-arena copies: heat tiering is
 *     canonical-on-dev1 + acceleration-copies-on-dev0, NEVER a move. A
 *     sweep's migration pass (vol_tier_migrate, run after the WP19 decay +
 *     promotion) copies read-hot canonical segments (rheat >= INVFS_HEAT_HOT
 *     post-decay) into the dev0 arena and demotes (frees) the coldest
 *     copies while the arena's free share is under 20%. Key = the
 *     canonical dev1 pba; the map's L2P lba = a free-list ordinal.
 *     seg_read_checked prefers the dev0 copy and falls back to canonical.
 *     Demote/free only ever drops the dev0 copy -- canonical stays.
 *
 * Crash/ordering rules mirror tz_seal: copy data blocks are written and
 * their maps journaled BEFORE the owner record names them (the owner
 * sync runs at the end of vol_flush, after jrn_flush); removals rewrite
 * the owner record first (same flush) and only then unmap+free. A crash
 * anywhere leaves at worst an orphan block fsck reclaims.
 *
 * v1 limits (loud when hit, never silent): at most 65535 mirrored raw
 * segments and 65535 live tier copies (one owner record each), and dev0
 * must be < 2^24 blocks (the rawm map keys are 24-bit). Both are format
 * limits of the owner-record pattern, not of the device table. */
#include "volume_internal.h"


/* ---------------- sorted index (bsearch by key) ---------------- */

static size_t wp25_lower(const wp25_ent *t, size_t n, uint64_t key,
                         int *found)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    *found = (lo < n && t[lo].key == key);
    return lo;
}


/* insert or replace the entry for key; keeps the array sorted */
static int wp25_put(wp25_ent **tp, size_t *np, size_t *cp,
                    uint64_t key, uint64_t pba, uint32_t plen, uint32_t ord)
{
    int found;
    size_t pos = wp25_lower(*tp, *np, key, &found);
    if (found) {
        (*tp)[pos].pba = pba;
        (*tp)[pos].plen = plen;
        (*tp)[pos].ord = ord;
        return 0;
    }
    if (*np == *cp) {
        size_t nc = *cp ? *cp * 2 : 64;
        wp25_ent *nt = (wp25_ent *)realloc(*tp, nc * sizeof *nt);
        if (!nt) return -1;
        *tp = nt;
        *cp = nc;
    }
    memmove(*tp + pos + 1, *tp + pos, (*np - pos) * sizeof **tp);
    (*tp)[pos].key = key;
    (*tp)[pos].pba = pba;
    (*tp)[pos].plen = plen;
    (*tp)[pos].ord = ord;
    (*np)++;
    return 0;
}


/* remove key if present; 1 = removed */
static int wp25_del(wp25_ent *t, size_t *np, uint64_t key)
{
    int found;
    size_t pos = wp25_lower(t, *np, key, &found);
    if (!found) return 0;
    memmove(t + pos, t + pos + 1, (*np - pos - 1) * sizeof *t);
    (*np)--;
    return 1;
}


static const wp25_ent *wp25_get(const wp25_ent *t, size_t n, uint64_t key)
{
    int found;
    size_t pos = wp25_lower(t, n, key, &found);
    return found ? &t[pos] : NULL;
}


int wp25_tier_lookup(invfs_volume *v, uint64_t cpba,
                     uint64_t *dpba_out, uint64_t *plen_out)
{
    const wp25_ent *e = wp25_get(v->tier, v->tier_n, cpba);
    if (!e) return -1;
    *dpba_out = e->pba;
    *plen_out = e->plen;
    return 0;
}


int wp25_rawm_lookup(invfs_volume *v, uint64_t raw_pba,
                     uint64_t *mpba_out, uint64_t *plen_out)
{
    const wp25_ent *e = wp25_get(v->rawm, v->rawm_n, raw_pba);
    if (!e) return -1;
    *mpba_out = e->pba;
    *plen_out = e->plen;
    return 0;
}


uint64_t vol_tier_count(invfs_volume *v, uint64_t *blocks_out,
                        uint64_t *first_cpba, uint64_t *first_dpba)
{
    size_t i;
    uint64_t b = 0;
    if (blocks_out) *blocks_out = 0;
    if (first_cpba) *first_cpba = 0;
    if (first_dpba) *first_dpba = 0;
    if (!v) return 0;
    for (i = 0; i < v->tier_n; i++)
        b += v->tier[i].plen;
    if (blocks_out) *blocks_out = b;
    if (v->tier_n && first_cpba) *first_cpba = v->tier[0].key;
    if (v->tier_n && first_dpba) *first_dpba = v->tier[0].pba;
    return v->tier_n;
}


uint64_t vol_rawm_count(invfs_volume *v, uint64_t *blocks_out)
{
    size_t i;
    uint64_t b = 0;
    if (!v) return 0;
    for (i = 0; i < v->rawm_n; i++)
        b += v->rawm[i].plen;
    if (blocks_out) *blocks_out = b;
    return v->rawm_n;
}


/* ---------------- owner-record persistence ---------------- */

/* The tz_owner_write variant the WP25 owners need: the entries' keys ride
 * in file_offset (tz_owner_write would rebuild those as the cumulative
 * concatenation). Same ordering contract otherwise: append [INOD][DELT
 * position-kill of the previous version] as one write; the owner keeps
 * its inode id so the L2P maps stay valid. */
static int wp25_owner_write(invfs_volume *v, uint64_t owner,
                            const char *name, const wp25_ent *ents,
                            size_t n)
{
    invfs_ast_block_entry *ae = NULL;
    uint8_t ah[INVFS_AST_HDR_V2_LEN];
    size_t ahlen, rec_len, total;
    uint64_t run = 0, old_pos, new_pos;
    uint8_t *combo;
    invfs_inode_rec *rh, tomb;
    uint32_t crc_rec, crc_tomb;
    size_t i;

    if (n > 65535) return -1;   /* one owner record, v1 header */
    ae = (invfs_ast_block_entry *)calloc(n ? n : 1, sizeof *ae);
    if (!ae) return -1;
    for (i = 0; i < n; i++) {
        ae[i].file_offset = ents[i].key;   /* the canonical pba (record) */
        ae[i].length = (uint64_t)ents[i].plen * INVFS_BLOCK_SIZE;
        ae[i].zone = INVFS_ZONE_BINARY;    /* physical home; NOT TEXT */
        ae[i].algo = INVFS_ALGO_NONE;
        ae[i].block_id = ents[i].ord;      /* the L2P map key */
        ae[i].block_offset = 0;
        run += ae[i].length;
    }
    ahlen = invfs_ast_hdr_write(ah, run, (uint32_t)n, 0);
    if (!ahlen) { free(ae); return -1; }

    rec_len = sizeof(invfs_inode_rec) + ahlen + n * sizeof(*ae);
    old_pos = idx_get_id(v, owner);
    total = rec_len + 4 + (old_pos ? sizeof(tomb) + 4 : 0);
    combo = (uint8_t *)calloc(1, total);
    if (!combo) { free(ae); return -1; }

    rh = (invfs_inode_rec *)combo;
    rh->magic = INODE_REC_MAGIC;
    rh->rec_len = (uint32_t)rec_len;
    rh->inode_id = owner;
    rh->file_size = run;
    rh->ctime = (uint64_t)time(NULL);
    rec_set_name(rh, name);
    memcpy(combo + sizeof(invfs_inode_rec), ah, ahlen);
    if (n)
        memcpy(combo + sizeof(invfs_inode_rec) + ahlen, ae,
               n * sizeof(*ae));
    free(ae);
    crc_rec = invfs_crc32c(combo, rec_len);
    memcpy(combo + rec_len, &crc_rec, 4);
    if (old_pos) {
        memset(&tomb, 0, sizeof tomb);
        tomb.magic = TOMBSTONE_MAGIC;
        v->hot.tombstones++;
        tomb.rec_len = (uint32_t)sizeof tomb;
        tomb.inode_id = owner;
        tomb.file_size = old_pos;       /* v2 position kill */
        rec_set_name(&tomb, name);
        crc_tomb = invfs_crc32c(&tomb, sizeof tomb);
        memcpy(combo + rec_len + 4, &tomb, sizeof tomb);
        memcpy(combo + rec_len + 4 + sizeof tomb, &crc_tomb, 4);
    }

    if (v->inode_area_pos + total > v->inode_area_end) { free(combo); return -1; }
    if (vol_mark_dirty(v) != 0) { free(combo); return -1; }
    new_pos = v->inode_area_pos;
    if (io_seek(&v->io, new_pos) != 0 ||
        io_write(&v->io, combo, total) != 0) { free(combo); return -1; }
    v->inode_area_pos = new_pos + total;
    free(combo);
    idx_put(v, name, strlen(name), owner, new_pos, run,
            (uint64_t)time(NULL));
    idx_put_id(v, owner, new_pos);
    return 0;
}


/* Rebuild one in-RAM index from its owner record: AST entry file_offset =
 * key, block_id = map key; the map gives the copy's pba (+plen). Entries
 * whose map is missing are skipped (a torn window: the copy's blocks are
 * then orphans for fsck -- never wrong reads). */
static void wp25_index_load_one(invfs_volume *v, uint64_t owner,
                                wp25_ent **tp, size_t *np, size_t *cp,
                                int is_rawm)
{
    uint8_t *buf = NULL;
    uint32_t rl = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents;
    size_t base = sizeof(invfs_inode_rec);
    uint32_t i;

    *np = 0;
    if (!owner) return;
    if (meta_read_record_by_id(v, owner, &buf, &rl, NULL, 0, NULL) != 0)
        return;
    if (rl < base + INVFS_AST_HDR_V1_LEN ||
        invfs_ast_hdr_parse(buf + base, rl - base, &ah) != 0 ||
        rl < base + ah.hdr_len +
             (size_t)ah.num_blocks * sizeof(invfs_ast_block_entry)) {
        free(buf);
        return;
    }
    ents = (const invfs_ast_block_entry *)(buf + base + ah.hdr_len);
    for (i = 0; i < ah.num_blocks; i++) {
        uint64_t pba = 0, plen = 0, key;
        if (vol_lookup_entry(v, owner, ents[i].block_id, &pba, &plen) != 0 ||
            !pba)
            continue;
        key = is_rawm ? v->sb.raw_zone_start + ents[i].block_id
                      : ents[i].file_offset;
        if (wp25_put(tp, np, cp, key, pba, (uint32_t)plen,
                     ents[i].block_id) != 0)
            break;   /* OOM: a partial index only costs redundancy */
    }
    free(buf);
}


void wp25_index_load(invfs_volume *v)
{
    wp25_index_load_one(v, v->rawm_owner, &v->rawm, &v->rawm_n,
                        &v->rawm_cap, 1);
    wp25_index_load_one(v, v->tier_owner, &v->tier, &v->tier_n,
                        &v->tier_cap, 0);
    if (getenv("INVFS_DEBUG") && (v->rawm_n || v->tier_n))
        fprintf(stderr, "[wp25] index: %zu raw mirrors, %zu tier copies\n",
                v->rawm_n, v->tier_n);
}


/* vol_flush hook (after jrn_flush): rewrite dirty owner records so they
 * name exactly the current index entries. The maps/unmaps are already
 * durable in this same flush. */
int wp25_owner_sync(invfs_volume *v)
{
    if (v->rawm_dirty) {
        if (wp25_owner_write(v, v->rawm_owner, "\x01rawm", v->rawm,
                             v->rawm_n) != 0)
            return -1;
        v->rawm_dirty = 0;
    }
    if (v->tier_dirty) {
        if (wp25_owner_write(v, v->tier_owner, "\x01tier0", v->tier,
                             v->tier_n) != 0)
            return -1;
        v->tier_dirty = 0;
    }
    return 0;
}


/* ---------------- write/free hooks ---------------- */

/* Mirror one freshly written raw-zone segment onto dev1. The buffer is
 * the segment's full padded span (phys_blocks blocks). 0 = mirrored (or
 * gracefully unmirrored, logged); -1 = dev1 io failure (caller latches). */
int wp25_rawm_write(invfs_volume *v, uint64_t pba, const uint8_t *buf,
                    uint64_t phys_blocks)
{
    uint64_t rel, mpba;
    const wp25_ent *old;
    size_t span = (size_t)phys_blocks * INVFS_BLOCK_SIZE;

    if (!v->rawm_owner) {
        if (!v->rawio_logged) {
            v->rawio_logged = 1;
            fprintf(stderr, "vol: no \\x01rawm owner record; RAW segments "
                    "are NOT mirrored (pre-WP25 volume?)\n");
        }
        return 0;
    }
    rel = pba - v->sb.raw_zone_start;
    if (rel >= (1u << 24)) {
        fprintf(stderr, "vol: raw pba past the 24-bit mirror key limit; "
                "segment unmirrored\n");
        return 0;
    }
    old = wp25_get(v->rawm, v->rawm_n, pba);
    if (old) {
        /* rewrite of the same raw slot: retire the old mirror first
         * (unmap + free), then take a fresh allocation */
        l2p_remove(v, v->rawm_owner, old->ord);
        vol_free_blocks(v, old->pba, old->plen);
        wp25_del(v->rawm, &v->rawm_n, pba);
    }
    if (v->rawm_n >= 65535) {
        fprintf(stderr, "vol: RAW mirror table full (65535 live mirrors); "
                "segment unmirrored\n");
        return 0;
    }
    mpba = alloc_blocks(v, v->sb.shadow_zone_start,
                        v->sb.shadow_zone_blocks, phys_blocks, 0);
    if (!mpba) {
        fprintf(stderr, "vol: no shadow space for the RAW mirror of pba "
                "%llu; segment unmirrored (ENOSPC policy)\n",
                (unsigned long long)pba);
        return 0;
    }
    /* the caller's buffer is the padded span; write through the mux */
    if (io_seek(&v->io, mpba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, buf, span) != 0) {
        vol_free_blocks(v, mpba, phys_blocks);
        return -1;   /* dev1 failed: the caller latches */
    }
    if (vol_map(v, v->rawm_owner, rel, mpba, (uint32_t)phys_blocks) != 0) {
        vol_free_blocks(v, mpba, phys_blocks);
        return -1;
    }
    if (wp25_put(&v->rawm, &v->rawm_n, &v->rawm_cap, pba, mpba,
                 (uint32_t)phys_blocks, (uint32_t)rel) != 0) {
        l2p_remove(v, v->rawm_owner, rel);
        vol_free_blocks(v, mpba, phys_blocks);
        return -1;
    }
    v->rawm_dirty = 1;
    return 0;
}


/* vol_free_blocks hook (real frees only; retention has already returned).
 * A freed raw-zone run drops its dev1 mirror; a freed canonical
 * dev1-shadow run drops its dev0 tier copy. */
void wp25_on_free(invfs_volume *v, uint64_t pba, uint64_t nblocks)
{
    uint64_t end = pba + nblocks;
    size_t i;

    if (v->rawm_n && pba >= v->sb.raw_zone_start &&
        pba < v->sb.raw_zone_start + v->sb.raw_zone_blocks) {
        for (i = 0; i < v->rawm_n; ) {
            wp25_ent e = v->rawm[i];
            if (e.key >= pba && e.key < end) {
                l2p_remove(v, v->rawm_owner, e.ord);
                vol_free_blocks(v, e.pba, e.plen);
                wp25_del(v->rawm, &v->rawm_n, e.key);
                v->rawm_dirty = 1;
            } else {
                i++;
            }
        }
    }
    if (v->tier_n && pba >= v->sb.shadow_zone_start) {
        for (i = 0; i < v->tier_n; ) {
            wp25_ent e = v->tier[i];
            if (e.key >= pba && e.key < end) {
                l2p_remove(v, v->tier_owner, e.ord);
                vol_free_blocks(v, e.pba, e.plen);
                wp25_del(v->tier, &v->tier_n, e.key);
                v->tier_dirty = 1;
            } else {
                i++;
            }
        }
    }
}


/* fsck -f rebuild hook: drop entries whose canonical key or whose copy
 * blocks are no longer allocated (the rebuild replaces the bitmap
 * wholesale; nothing else re-validates the copies against it). */
void wp25_fsck_prune(invfs_volume *v)
{
    size_t i;
    if (v->ndev != 2) return;
    for (i = 0; i < v->rawm_n; ) {
        wp25_ent e = v->rawm[i];
        if (!bit_get(v->bitmap, e.key) || !bit_get(v->bitmap, e.pba)) {
            if (bit_get(v->bitmap, e.pba))
                vol_free_blocks(v, e.pba, e.plen);
            wp25_del(v->rawm, &v->rawm_n, e.key);
            v->rawm_dirty = 1;
        } else {
            i++;
        }
    }
    for (i = 0; i < v->tier_n; ) {
        wp25_ent e = v->tier[i];
        if (!bit_get(v->bitmap, e.key) || !bit_get(v->bitmap, e.pba)) {
            if (bit_get(v->bitmap, e.pba))
                vol_free_blocks(v, e.pba, e.plen);
            wp25_del(v->tier, &v->tier_n, e.key);
            v->tier_dirty = 1;
        } else {
            i++;
        }
    }
}


/* ---------------- the sweep migration pass (rule 9) ----------------
 * After the WP19 decay + promotion: copy read-hot canonical segments to
 * the dev0 arena (promotion), then while the arena free share is under
 * the 20% watermark evict the coldest copies (demotion). Canonical
 * placement never changes (NEVER move). */

/* current rheat of the hottest live map pointing at pba (the copy's
 * justification); 0 when nothing maps it any more */
static uint16_t tier_heat_of(const invfs_volume *v, uint64_t pba)
{
    size_t i;
    uint16_t r = 0;
    for (i = 0; i < v->l2p_count; i++) {
        const invfs_l2p_entry *e = &v->l2p[i];
        if (e->type == INVFS_JRN_MAP && e->pba == pba) {
            uint16_t er = l2p_rheat(e);
            if (er > r) r = er;
        }
    }
    return r;
}


/* copy one canonical segment into the dev0 arena. 0 = copied, 1 = skipped
 * (already copied / no owner / no arena space), -1 = error. */
static int tier_promote_one(invfs_volume *v, uint64_t cpba, uint32_t plen)
{
    uint64_t dpba;
    uint32_t ord;
    uint8_t *buf;
    size_t span = (size_t)plen * INVFS_BLOCK_SIZE;
    size_t i;

    if (wp25_get(v->tier, v->tier_n, cpba))
        return 1;
    if (!v->tier_owner || !v->arena_blocks || !plen)
        return 1;
    if (v->tier_n >= 65535)
        return 1;   /* v1 cap, reported by the caller's summary */
    if (span > (size_t)64 * 1024 * 1024)
        return 1;   /* sanity: a segment never spans 64 MB */
    /* lowest free ordinal */
    for (ord = 0; ord < 65535; ord++) {
        int used = 0;
        for (i = 0; i < v->tier_n; i++)
            if (v->tier[i].ord == ord) { used = 1; break; }
        if (!used) break;
    }
    if (ord == 65535) return 1;
    dpba = alloc_blocks(v, v->arena_start, v->arena_blocks, plen, 0);
    if (!dpba)
        return 1;   /* arena pressure: the demote pass below manages */
    buf = (uint8_t *)malloc(span);
    if (!buf) { vol_free_blocks(v, dpba, plen); return -1; }
    if (io_seek(&v->io, cpba * INVFS_BLOCK_SIZE) != 0 ||
        io_read(&v->io, buf, span) != 0 ||
        io_seek(&v->io, dpba * INVFS_BLOCK_SIZE) != 0 ||
        io_write(&v->io, buf, span) != 0) {
        free(buf);
        vol_free_blocks(v, dpba, plen);
        return -1;
    }
    free(buf);
    if (vol_map(v, v->tier_owner, ord, dpba, plen) != 0) {
        vol_free_blocks(v, dpba, plen);
        return -1;
    }
    if (wp25_put(&v->tier, &v->tier_n, &v->tier_cap, cpba, dpba, plen,
                 ord) != 0) {
        l2p_remove(v, v->tier_owner, ord);
        vol_free_blocks(v, dpba, plen);
        return -1;
    }
    v->tier_dirty = 1;
    return 0;
}


/* drop one copy (demotion): the copy is cache, so this is free + unmap;
 * the owner record rewrite at the next flush makes it durable. */
static void tier_demote_idx(invfs_volume *v, size_t idx)
{
    wp25_ent e = v->tier[idx];
    l2p_remove(v, v->tier_owner, e.ord);
    vol_free_blocks(v, e.pba, e.plen);
    wp25_del(v->tier, &v->tier_n, e.key);
    v->tier_dirty = 1;
    v->tier_demoted++;
}


int vol_tier_migrate(invfs_volume *v)
{
    uint64_t watermark;
    size_t i;
    int rc = 0;

    v->tier_promoted = v->tier_demoted = v->tier_blocks = 0;
    if (v->ndev != 2 || v->degraded || !vol_write_enabled(v))
        return 0;
    if (!v->arena_blocks || !v->tier_owner)
        return 0;
    watermark = v->arena_blocks / 5;   /* demote below 20% free */

    /* ---- promotion: read-hot canonical (dev1) segments -> dev0 copy --
     * the WP19 hysteresis applies: the sweep's decay ran first, so a
     * burst promotes only when it survives exactly one halving. */
    if (v->heat_any_rhot) {
        size_t n = v->l2p_count;
        for (i = 0; i < n; i++) {
            const invfs_l2p_entry *e = &v->l2p[i];
            int prc;
            if (e->type != INVFS_JRN_MAP) continue;
            if (l2p_rheat(e) < INVFS_HEAT_HOT) continue;
            if (e->inode == v->tier_owner || e->inode == v->rawm_owner)
                continue;   /* the engine's own second copies */
            if (!e->length || e->pba < v->sb.shadow_zone_start)
                continue;   /* only canonical (dev1) shadow segments */
            if (e->pba + e->length > v->sb.total_blocks)
                continue;
            prc = tier_promote_one(v, e->pba, e->length);
            if (prc < 0) { rc = -1; goto out; }
            if (prc == 0) {
                v->tier_promoted++;
                v->tier_blocks += e->length;
            }
        }
    }

    /* ---- demotion: arena pressure evicts the coldest copies ---- */
    while (v->arena_free < watermark && v->tier_n) {
        size_t coldest = 0;
        uint16_t minr = 0xFFFF;
        for (i = 0; i < v->tier_n; i++) {
            uint16_t r = tier_heat_of(v, v->tier[i].key);
            if (r < minr) { minr = r; coldest = i; }
        }
        tier_demote_idx(v, coldest);
    }
out:
    if (v->tier_promoted || v->tier_demoted || v->tier_n)
        printf("tier: %llu hot segment(s) copied to dev0 (%llu blocks), "
               "%llu demoted; %zu copies live\n",
               (unsigned long long)v->tier_promoted,
               (unsigned long long)v->tier_blocks,
               (unsigned long long)v->tier_demoted, v->tier_n);
    return rc;
}
