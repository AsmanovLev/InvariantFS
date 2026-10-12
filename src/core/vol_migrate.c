/* vol_migrate.c — WP601 migration zone (see vol_migrate.h for the contract).
 *
 * House discipline (copied from heat_promote + heat_extract_v3 + WP145,
 * read those, this file does not alter them):
 *  - vol_walk receipt: the candidate list comes from vol_iter_live_inodes
 *    and is USED only when vol_walk_commit says the walk finished. A
 *    stopped walk refuses the whole pass (nothing moved, walk_refused=1):
 *    a candidate list AND its budget drawn from a subset would extract
 *    data out of a partial view of the volume and print it as a whole one.
 *  - ENOSPC admission: moves coexist with their sources until the publish
 *    lands, so each file prices its worst case (new extents + the flat
 *    margin, the same DEFER_ENOSPC shape heat_promote uses) and defers
 *    when the gate says wait. A deferred file is retried by the next pass,
 *    never failed.
 *  - Failure containment: a failed move frees what it allocated, leaves
 *    the source recipe intact, and moves on to the next file. Per-file
 *    failures are counted in the report, never fatal to the pass.
 *
 * Selection v1 (measure + justify, no gold-plate): live regular files,
 * smallest first (small files fill the holes dedupe/batches left behind
 * them; the allocator's first-fit does the hole-filling, the ordering only
 * decides who gets first pick). A segment is movable when it is
 * exclusively owned (pba_ref_count == 1: shared segments are dedupe's
 * canonical output and re-copying them per file would un-merge what
 * dedupe merged), RAW- or BINARY-zoned, and neither a TEXT shared batch
 * member nor a WINDOW reference (owned by the batch registry / the source
 * inode respectively). The publish gate is DOWNWARD-ONLY: a copied extent
 * is republished only when its new pba lies strictly below its old one;
 * an upward (or equal) allocation is freed unpublished. Every published
 * move therefore strictly decreases the sum of live segment pbas, which is
 * bounded below by zero — the pass converges by construction, with no
 * stamps, no markers, and no re-move churn, and each move fills a lower
 * hole with a higher segment (the freed tail extent merges upward). A
 * moved RAW entry is retagged BINARY: it was consolidated into shadow,
 * and RAW means "awaiting sweep disposition" (the generic floor retags
 * the same way). Budget-capped per run (see
 * the header).
 *
 * Test seam: INVFS_MIGRATE_SLOW=1 sleeps 500 ms per considered segment (moved or skipped). It
 * widens the kill -9 window for the crash-mid-move probe and does nothing
 * otherwise. Production never sets it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "invarifs.h"
#include "volume.h"
#include "volume_internal.h"
#include "vol_delta.h"   /* WP612 retire API */
#include "vol_walk.h"
#include "vol_migrate.h"
#include "codec.h"

void vol_migrate_frag(invfs_volume *v, uint64_t *runs_out, double *score_out)
{
    uint64_t start, end, b, runs = 0, free = 0, cur = 0, largest = 0;
    int in_run = 0;

    if (runs_out) *runs_out = 0;
    if (score_out) *score_out = 0;
    if (!v) return;
    start = v->sb.raw_zone_start;
    end = v->sb.total_blocks;
    if (end <= start) return;
    for (b = start; b < end; b++) {
        int used = (v->bitmap[b >> 3] >> (b & 7)) & 1;
        if (!used) {
            free++;
            cur++;
            if (!in_run) { in_run = 1; runs++; }
        } else {
            if (in_run) {
                in_run = 0;
                if (cur > largest) largest = cur;
                cur = 0;
            }
        }
    }
    if (in_run && cur > largest) largest = cur;
    if (getenv("INVFS_MIGRATE_DEBUG")) {
        uint64_t shown = 0;
        int in2 = 0;
        uint64_t rs = 0, rl = 0;
        fprintf(stderr, "[migrate-dbg] free-run map (first 40):");
        for (b = start; b < end && shown < 40; b++) {
            int used = (v->bitmap[b >> 3] >> (b & 7)) & 1;
            if (!used && !in2) { in2 = 1; rs = b; rl = 0; }
            if (!used) rl++;
            if (used && in2) {
                in2 = 0;
                fprintf(stderr, " [%llu+%llu]",
                        (unsigned long long)rs, (unsigned long long)rl);
                shown++;
            }
        }
        if (in2 && shown < 40)
            fprintf(stderr, " [%llu+%llu-tail]",
                    (unsigned long long)rs, (unsigned long long)rl);
        fprintf(stderr, "\n");
    }
    if (runs_out) *runs_out = runs;
    if (score_out) *score_out = free ? 1.0 - (double)largest / (double)free : 0.0;
}

/* ---- candidate collection (walk + receipt, the heat_promote shape) ---- */

typedef struct {
    uint64_t id;
    char name[256];
    uint64_t size;
} migrate_cand;

typedef struct {
    migrate_cand *cand;
    size_t n, cap;
    size_t n_seen;
    int oom;
} migrate_collect_ctx;

static int migrate_collect_cb(invfs_volume *v, uint64_t inode_id,
                              const char *name, void *ctx_)
{
    migrate_collect_ctx *ctx = (migrate_collect_ctx *)ctx_;
    invfs_inode in;
    size_t nl;

    ctx->n_seen++;
    if (!name || !name[0] || (unsigned char)name[0] == 0x01)
        return 0;                       /* engine bookkeeping, not a target */
    if (ctx->n == ctx->cap) {
        size_t nc = ctx->cap ? ctx->cap * 2 : 64;
        migrate_cand *nn = (migrate_cand *)realloc(ctx->cand, nc * sizeof *nn);
        if (!nn) { ctx->oom = 1; return 1; }
        ctx->cand = nn;
        ctx->cap = nc;
    }
    /* Size now: the pass sorts smallest-first, and a size fetched from a
     * partial walk is exactly what the receipt below refuses to trust. */
    if (vol_inode_get(v, inode_id, &in) != 1 || in.size == 0)
        return 0;
    ctx->cand[ctx->n].id = inode_id;
    nl = strlen(name);
    if (nl > sizeof ctx->cand[0].name - 1) nl = sizeof ctx->cand[0].name - 1;
    memcpy(ctx->cand[ctx->n].name, name, nl);
    ctx->cand[ctx->n].name[nl] = 0;
    ctx->cand[ctx->n].size = in.size;
    ctx->n++;
    return 0;
}

static int migrate_cand_cmp(const void *a_, const void *b_)
{
    const migrate_cand *a = (const migrate_cand *)a_;
    const migrate_cand *b = (const migrate_cand *)b_;
    if (a->size < b->size) return -1;
    if (a->size > b->size) return 1;
    if (a->id < b->id) return -1;
    if (a->id > b->id) return 1;
    return 0;
}

/* ---- per-file relocate (Phase A) ----
 * Returns 1 when >= 1 segment was republished, 0 when the file was left
 * intact (unmovable shape, deferred, or failed-contained). *budget_io is
 * decremented by every new block written; *moved_* / *seal_* accumulate. */

typedef struct { uint64_t old_pba, old_plen, new_pba, new_plen; } migrate_move;

/* Tier/raw-mirror coverage: freeing this segment would drop its dev0
 * acceleration copy (or dev1 mirror) via wp25_on_free -- measured: moved
 * canonicals orphan their copies, heat accounting collapses, and the
 * multidev demotion leg sees no pressure. Tier owns these placements;
 * v1 leaves them. Single-device volumes have empty maps (always miss). */
static int tier_covered(invfs_volume *v, uint64_t pba)
{
    uint64_t dp = 0, dl = 0;
    if (wp25_tier_lookup(v, pba, &dp, &dl) == 0)
        return 1;
    if (wp25_rawm_lookup(v, pba, &dp, &dl) == 0)
        return 1;
    return 0;
}

static int migrate_file(invfs_volume *v, uint64_t inode_id, const char *name,
                        uint64_t *budget_io, int *budget_hit,
                        size_t *moved_segs, uint64_t *moved_blocks,
                        uint64_t *seal_ranges, uint64_t *seal_blocks,
                        size_t *skipped_up, size_t *skipped_tier)
{
    invfs_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0, i;
    const invfs_ast_window_entry *wins = NULL;
    uint32_t n_wins = 0;
    invfs_ast_block_entry *ne = NULL;
    migrate_move *moves = NULL;
    size_t n_moves = 0, cap_moves = 0;
    uint64_t need_bytes = 0, need_segs = 0;
    size_t nmoved = 0;
    uint64_t spend = 0;
    int slow = getenv("INVFS_MIGRATE_SLOW") != NULL;
    int rc_out = 0;

    if (vol_inode_get(v, inode_id, &in) != 1)
        return 0;
    if (in.size == 0)
        return 0;
    if (vol_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob)
        return 0;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 ||
        !ents || n_ents == 0) {
        free(blob);
        return 0;
    }
    /* The window table (container lanes) is part of the recipe: a
     * serialize without it would drop every window and corrupt the file.
     * Carried verbatim; never interpreted here. */
    if (vol_ast_recipe_windows(blob, blen, &ah, &wins, &n_wins) != 0) {
        free(blob);
        return 0;
    }
    /* Windowed recipes (container members/siblings) are lane-owned: the
     * container lane manages their layout (maps, sibling sets, nested
     * decomposition), and republishing their segments here has corrupted
     * volume metadata in the field (freed bitmap bits under live roots).
     * v1 does not move them -- counted, never half-done. The window
     * interaction (republish + serialize_win) is a filed follow-up. */
    if (n_wins > 0) {
        free(blob);
        return 2;
    }

    ne = (invfs_ast_block_entry *)malloc(n_ents * sizeof *ne);
    if (!ne) { free(blob); return 0; }
    memcpy(ne, ents, n_ents * sizeof *ne);

    pba_ref_ensure(v);

    /* Price first: the new extents coexist with the sources until the
     * publish lands (the heat_promote DEFER_ENOSPC shape). RAW or BINARY
     * zone: the floor places everything it can, so RAW-only would starve
     * (measured: RAW zone empty after every full sweep); consolidation has
     * to compact shadow too. The publish gate below keeps every accepted
     * move convergent: downward moves decrease the pba potential,
     * RAW->shadow moves strictly drain RAW occupancy (nothing allocates
     * back into RAW here); shadow->shadow-upward is churn and refused. */
    for (i = 0; i < n_ents; i++) {
        uint64_t plen = 0;
        if (ne[i].pba == 0 ||
            (ne[i].zone != INVFS_ZONE_RAW &&
             ne[i].zone != INVFS_ZONE_BINARY) ||
            ne[i].algo == INVFS_ALGO_WINDOW_SRC)
            continue;
        if (pba_ref_count(v, ne[i].pba) != 1)
            continue;           /* shared: dedupe's output, leave it */
        if (tier_covered(v, ne[i].pba))
            continue;           /* tier copy or raw mirror: tier owns it */
        if (seg_extent_checked(v, ne[i].pba, &plen) != 0 || plen == 0)
            continue;
        need_bytes += plen * INVFS_BLOCK_SIZE;
        need_segs++;
    }
    if (need_segs == 0) goto done;      /* nothing exclusively owned */
    if (sweep_enospc(v, need_bytes + need_segs * 8 + INVFS_ENOSPC_MARGIN)) {
        fprintf(stderr, "[migrate] %s: relocation deferred (ENOSPC)\n",
                name ? name : "?");
        goto done;
    }

    for (i = 0; i < n_ents; i++) {
        uint64_t old_pba = ne[i].pba, old_plen = 0;
        uint32_t csize = 0;
        uint8_t *pay = NULL, *frame = NULL, *back = NULL;
        uint64_t new_plen, pba_new;
        uint32_t crc;
        size_t fr;
        migrate_move *nm;

        if (old_pba == 0 ||
            (ne[i].zone != INVFS_ZONE_RAW &&
             ne[i].zone != INVFS_ZONE_BINARY) ||
            ne[i].algo == INVFS_ALGO_WINDOW_SRC)
            continue;
        if (pba_ref_count(v, old_pba) != 1)
            continue;
        if (tier_covered(v, old_pba)) {
            (*skipped_tier)++;
            continue;
        }
        if (seg_extent_checked(v, old_pba, &old_plen) != 0 || old_plen == 0)
            continue;
        /* Checked read (CRC-verified, seal-recovery + tier failover
         * inside): a segment that does not read is left in place, and
         * the file keeps every segment it had. */
        if (seg_read_checked(v, old_pba, old_plen, 1, &csize, &pay) != 0 ||
            !pay)
            continue;
        new_plen = ((uint64_t)csize + 8 + INVFS_BLOCK_SIZE - 1) /
                   INVFS_BLOCK_SIZE;
        if (new_plen > *budget_io) {
            *budget_hit = 1;
            free(pay);
            continue;
        }
        frame = (uint8_t *)malloc((size_t)new_plen * INVFS_BLOCK_SIZE);
        if (!frame) { free(pay); continue; }
        /* Rebuild the frame deterministically: the bytes below are a
         * verbatim copy of what the checked read verified, so the move
         * is codec-agnostic and bit-exact by construction (no decode,
         * no re-encode, nothing to guard). */
        memcpy(frame, &csize, 4);
        crc = invfs_crc32c(pay, csize);
        memcpy(frame + 4, &crc, 4);
        if (csize) memcpy(frame + 8, pay, csize);
        fr = (size_t)new_plen * INVFS_BLOCK_SIZE - 8 - csize;
        if (fr) memset(frame + 8 + csize, 0, fr);
        free(pay);

        pba_new = alloc_blocks(v, v->sb.shadow_zone_start,
                               v->sb.shadow_zone_blocks, new_plen, 1,
                               INVFS_ALLOC_DATA);
        if (pba_new == 0) { free(frame); continue; }
        if (io_pwrite(&v->io, pba_new * INVFS_BLOCK_SIZE, frame,
                      (size_t)new_plen * INVFS_BLOCK_SIZE) != 0) {
            vol_free_blocks(v, pba_new, new_plen);
            free(frame);
            continue;
        }
        /* Copy verify: read the new bytes back off the device and memcmp
         * before anything names them. A torn write fails here, the new
         * extent is freed, and the source is untouched. */
        back = (uint8_t *)malloc((size_t)new_plen * INVFS_BLOCK_SIZE);
        if (!back ||
            io_pread(&v->io, pba_new * INVFS_BLOCK_SIZE, back,
                     (size_t)new_plen * INVFS_BLOCK_SIZE) != 0 ||
            memcmp(back, frame, (size_t)new_plen * INVFS_BLOCK_SIZE) != 0) {
            free(back);
            vol_free_blocks(v, pba_new, new_plen);
            free(frame);
            continue;
        }
        free(back);
        free(frame);
        /* Publish gate: DOWNWARD moves only (new strictly below old).
         * The pba potential strictly decreases per move and is bounded
         * below, so repeated passes terminate: no ping-pong, no churn.
         * A RAW->shadow move landing higher is refused even though it
         * would drain RAW, because the next pass moves it back into the
         * hole it left (measured oscillation on adjacent slots).
         * Upward copies are freed unpublished (budget refunded). */
        if (getenv("INVFS_MIGRATE_DEBUG"))
            fprintf(stderr,
                    "[migrate-dbg] %s seg pba=%llu plen=%llu zone=%u -> new=%llu "
                    "(%s)\n",
                    name ? name : "?",
                    (unsigned long long)old_pba,
                    (unsigned long long)old_plen,
                    (unsigned)ne[i].zone,
                    (unsigned long long)pba_new,
                    (pba_new < old_pba ? "DOWNWARD-MOVE" : "UPWARD-SKIP"));
        if (pba_new >= old_pba) {
            vol_free_blocks(v, pba_new, new_plen);
            *skipped_up += 1;
            if (slow) {
                /* Slow every considered segment, not just published
                 * moves: the kill window must exist even on fixtures
                 * where nothing is movable (fresh volumes), and per
                 * published move alone makes the window geometry-
                 * dependent. Skips touch nothing old, so slowing them
                 * is behavior-neutral. */
                struct timespec sts;
                sts.tv_sec = 0;
                sts.tv_nsec = 500 * 1000 * 1000;
                nanosleep(&sts, NULL);
            }
            continue;
        }
        if (slow) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 500 * 1000 * 1000;
            nanosleep(&ts, NULL);
        }

        /* TRANSACTION: record the pair, touch nothing old. Until the
         * recipe publish below lands, the on-disk recipe still names
         * old_pba, so its refcount and its blocks stay exactly as they
         * were (the sweep remap's rule, same reason). */
        if (n_moves == cap_moves) {
            size_t nc = cap_moves ? cap_moves * 2 : 16;
            nm = (migrate_move *)realloc(moves, nc * sizeof *nm);
            if (!nm) {
                vol_free_blocks(v, pba_new, new_plen);
                continue;
            }
            moves = nm;
            cap_moves = nc;
        }
        moves[n_moves].old_pba = old_pba;
        moves[n_moves].old_plen = old_plen;
        moves[n_moves].new_plen = new_plen;
        n_moves++;
        /* WP612: no +1 here any more (the publish-time validate retired
         * with it). The publish flags the map stale and the next free
         * gate -- the retire drain included -- rebuilds it from the live
         * set, which names pba_new through the new row. */
        ne[i].pba = pba_new;
        /* The zone TAG is deliberately left exactly as it was, even
         * when the bytes physically land in the shadow extent (RAW tag
         * in shadow placement is ordinary post-WP-DZ). RAW means
         * "awaiting sweep disposition" and disposition is
         * lane-specific: a container member awaiting (nested)
         * decomposition must stay visible to the container lane, and
         * retagging it BINARY hides it (BINARY reads as disposed --
         * measured: sweep #2 of test-containerpack declined a nested
         * member its own move had retagged). Placement never decides
         * what a block is, so the tag stays with the content, not the
         * address. algo UNCHANGED: the bytes are a verbatim copy. */
        *budget_io -= new_plen;
        spend += new_plen;
        nmoved++;
    }

    if (nmoved > 0) {
        uint8_t *new_blob = NULL;
        size_t new_blen = 0;
        uint8_t new_addr[INVFS_RECIPE_ADDR_LEN];
        int published = 0;
        int ser_ok;
        size_t r;
        if (n_wins > 0 && wins)
            ser_ok = vol_ast_recipe_serialize_win(in.size, ne,
                                                  (uint32_t)n_ents, wins,
                                                  n_wins, &new_blob,
                                                  &new_blen);
        else
            ser_ok = vol_ast_recipe_serialize(in.size, ne, (uint32_t)n_ents,
                                              &new_blob, &new_blen);
        /* ONE &&: serialize and store failures both land in the rollback
         * (the sweep-rollback-test-conflict shape: never a nested if
         * whose else binds to the wrong test). */
        if (ser_ok == 0 &&
            vol_recipe_store(v, new_blob, new_blen, new_addr) == 0) {
            memcpy(in.recipe_addr, new_addr, sizeof new_addr);
            /* Leg2: the row publish is the durability point. Its rc
             * decides published; a failure rolls back like a store
             * failure (freeing blocks the live row still names would
             * hand live bytes to the next allocator). */
            if (vol_inode_delta_put(v, inode_id, &in) == 0)
                published = 1;
            else
                fprintf(stderr,
                        "[migrate] %s: row publish failed; rolling back\n",
                        name ? name : "?");
            /* WP612: no hand-kept exactness across the publish any more.
             * The -1 per moved pba / +1 per new one used to run under a
             * staged row (same WP607 s35/s23 hazard as the sweep remap).
             * The publish flagged the map stale; invalidate explicitly so
             * the next free gate rebuilds from the live set. */
            pba_ref_invalidate(v);
        }
        free(new_blob);

        if (published) {
            /* WP612: the segments the new recipe replaced are RETIRED, not
             * freed. The row publish above is staged -- a fence-failure
             * doom restores the old recipe, which would then name freed
             * blocks. The entry carries the old set with the new set as
             * keep; the drain replays the share-aware loop against the
             * live map, and only while this row is still the live one.
             * The seal handoff stays inline: it marks ranges for parity
             * recompute, it frees nothing, and recomputing over
             * still-allocated blocks is the safe direction. */
            for (r = 0; r < n_moves; r++) {
                /* Seal handoff: the old range's stripes need a parity
                 * recompute at the next reseal. Consumed, never silent:
                 * a no-op when no seal session tracks (NULL bitmap), the
                 * load-bearing mark otherwise. */
                seal_dirty_mark(v, moves[r].old_pba, moves[r].old_plen);
                *seal_ranges += 1;
                *seal_blocks += moves[r].old_plen;
            }
            {
                uint64_t *oldp = NULL, *keepp = NULL;
                oldp = n_moves ?
                    (uint64_t *)malloc(n_moves * sizeof *oldp) : NULL;
                keepp = n_moves ?
                    (uint64_t *)malloc(n_moves * sizeof *keepp) : NULL;
                if (n_moves == 0 || (oldp && keepp)) {
                    for (r = 0; r < n_moves; r++) {
                        oldp[r] = moves[r].old_pba;
                        keepp[r] = moves[r].new_pba;
                    }
                    if (n_moves)
                        vol_retire_pbas(v, inode_id, oldp, n_moves,
                                        keepp, n_moves);
                } else {
                    fprintf(stderr,
                            "[migrate] %s: retire list OOM; superseded "
                            "segments stay allocated for the orphan "
                            "collector\n", name ? name : "?");
                }
                free(oldp);
                free(keepp);
            }
            *moved_segs += nmoved;
            *moved_blocks += spend;
            rc_out = 1;
        } else {
            /* Nothing reachable names the new extents; the old recipe is
             * intact. Give them all back (the sweep rollback's rule). No
             * refcount to un-count any more (WP612: the record-time +1
             * retired with the publish-time -1s). */
            for (r = 0; r < n_moves; r++) {
                vol_free_blocks(v, moves[r].new_pba, moves[r].new_plen);
            }
            *budget_io += spend;  /* rolled back: the budget is whole again */
            fprintf(stderr,
                    "[migrate] %s: new recipe unpublished; rolled back "
                    "%llu block(s) across %zu segment(s), file intact\n",
                    name ? name : "?", (unsigned long long)spend, n_moves);
        }
    }

done:
    free(moves);
    free(ne);
    free(blob);
    return rc_out;
}

/* ---- per-file explicit recompress (Phase B) ----
 * v1: single-entry, non-shared recipes holding NONE/LZ4 payloads are
 * re-encoded to the generic ZSTD floor. Every recompress goes through the
 * decode-back memcmp guard plus the strictly-smaller size guard (the pack
 * lane's rule); a recompress that cannot prove itself keeps the original,
 * stamps nothing, and retries on generation roll. Multi-entry recipes are
 * skipped and counted (TODO: per-segment floor). Files already GENERIC at
 * the current registry generation are current and skipped. */

static int migrate_recompress_file(invfs_volume *v, uint64_t inode_id,
                                   const char *name,
                                   size_t *n_recomp, uint64_t *saved)
{
    invfs_inode in;
    uint8_t *blob = NULL, *data = NULL;
    size_t blen = 0, dlen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;
    const invfs_codec *zc;
    uint8_t cc = 0, ca = 0;
    uint16_t cg = 0;
    uint32_t algo0;
    uint8_t *enc = NULL, *back = NULL;
    size_t enc_cap, enc_len = 0;
    uint64_t newino;

    if (vol_inode_get(v, inode_id, &in) != 1 || in.size == 0)
        return 0;
    if (vol_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob)
        return 0;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 ||
        !ents || n_ents == 0) {
        free(blob);
        return 0;
    }
    if (n_ents != 1) {
        free(blob);
        return 2;               /* multi-entry: counted, not half-done */
    }
    if (ents[0].zone == INVFS_ZONE_TEXT ||
        ents[0].algo == INVFS_ALGO_WINDOW_SRC) {
        free(blob);
        return 0;
    }
    algo0 = ents[0].algo;
    free(blob);
    blob = NULL;
    /* Current generation already? Generations keep gating retries as
     * today: a GENERIC file stamped at the live generation is current. */
    if (vol_get_class(v, inode_id, &cc, &ca, &cg) == 0 &&
        cc == INVFS_CLASS_GENERIC && ca == INVFS_ALGO_ZSTD &&
        cg == invfs_registry_generation())
        return 0;
    /* Only the floor's inputs: verbatim or fast-codec payloads. Anything
     * already densely coded (JXL/PPMD/EXE-carve/container recipes carry
     * their own algos) is not the floor's business. */
    if (algo0 != INVFS_ALGO_NONE && algo0 != INVFS_ALGO_LZ4)
        return 0;
    zc = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
    if (!zc || !zc->encode || !zc->decode)
        return 0;
    if (vol_read_file(v, inode_id, &data, &dlen) != 0 || !data)
        return 0;
    /* Admission (the pack lane's pricing): the blob coexists with the
     * original until publish, so price the encode scratch bound. */
    if (sweep_enospc(v, (uint64_t)dlen + dlen / 4 + 65536 +
                        INVFS_ENOSPC_MARGIN)) {
        fprintf(stderr, "[migrate] %s: recompress deferred (ENOSPC)\n",
                name ? name : "?");
        free(data);
        return 0;
    }
    if (zc->dec_mem_bytes > vol_get_dec_mem_limit(v)) {
        free(data);
        return 0;
    }
    enc_cap = dlen + dlen / 4 + 65536;
    enc = (uint8_t *)malloc(enc_cap ? enc_cap : 1);
    if (!enc) { free(data); return 0; }
    if (zc->encode(data, dlen, enc, enc_cap, &enc_len) != 0 ||
        enc_len == 0 || enc_len >= dlen) {
        /* No gain: not a refusal, just not the floor's file. */
        free(enc);
        free(data);
        return 0;
    }
    back = (uint8_t *)malloc(dlen ? dlen : 1);
    if (!back || zc->decode(enc, enc_len, back, dlen) != 0 ||
        memcmp(back, data, dlen) != 0) {
        free(back);
        free(enc);
        free(data);
        return 0;               /* guard refused: original kept, unstamped */
    }
    free(back);
    {
        uint64_t saved_now = (uint64_t)(dlen - enc_len);
        free(data);
        newino = vol_publish_blob_inode(v, inode_id, enc, enc_len, dlen,
                                        INVFS_ALGO_ZSTD);
        free(enc);
        if (!newino)
            return 0;           /* publish failed: retry re-arms, unstamped */
        vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC, INVFS_ALGO_ZSTD,
                        invfs_registry_generation());
        *n_recomp += 1;
        *saved += saved_now;
    }
    return 1;
}

/* ---- the stage ---- */

int vol_migrate_run(invfs_volume *v, int recompress, uint64_t max_blocks,
                    vol_migrate_report *rep)
{
    migrate_collect_ctx ctx;
    vol_walk_t w;
    vol_migrate_report lr;
    size_t i;
    uint64_t budget;
    int wrc, ret = 0;
    int budget_hit = 0;

    memset(&lr, 0, sizeof lr);
    if (!v)
        goto out;
    if (!vol_write_enabled(v))
        goto out;
    if (max_blocks == 0)
        max_blocks = VOL_MIGRATE_DEFAULT_MAX_BLOCKS;
    budget = max_blocks;

    vol_migrate_frag(v, &lr.free_runs_before, &lr.frag_before);

    memset(&ctx, 0, sizeof ctx);
    wrc = vol_iter_live_inodes(v, migrate_collect_cb, &ctx);
    /* The RECEIPT (WP145, the heat_promote shape): the callback stores
     * exactly the non-empty named inodes it reaches, so n_seen counts
     * every delivery and n counts what survived the filter — and the walk
     * is complete only when the iterator says so (wrc == 0) with no OOM
     * stop. Anything else refuses the pass: nothing moved. */
    vol_walk_init(&w, v, "vol_migrate_run");
    vol_walk_result(&w, (wrc != 0 || ctx.oom) ? -1 : 0, ctx.n_seen, ctx.n_seen);
    if (vol_walk_commit(&w) != 0) {
        fprintf(stderr,
                "[migrate] pass REFUSED: the live-inode walk did not "
                "complete (%zu inode(s) reached). The candidate list would "
                "come from that subset, so this pass would relocate out of "
                "a partial view of the volume. Nothing moved; the next "
                "sweep retries with a complete scan.\n", ctx.n_seen);
        lr.walk_refused = 1;
        free(ctx.cand);
        goto out_frag;
    }
    lr.files_seen = ctx.n;
    if (ctx.n > 1)
        qsort(ctx.cand, ctx.n, sizeof ctx.cand[0], migrate_cand_cmp);

    /* Hole-first allocation: the shadow allocator is next-fit from a
     * per-zone cursor, which after a full sweep sits near the top -- so
     * plain allocations land ABOVE every source and the downward-only
     * publish gate below rejects them all (measured: 94/94 upward
     * skips, zero moves). Reset the cursor to the zone head for this
     * pass so migrate allocations fill the lowest holes first (first
     * fit from head); successive allocs advance it past each filled
     * hole, so consolidation proceeds bottom-up and the gate's
     * potential (sum of pbas) strictly decreases. Left where it ends:
     * it is a valid next-fit position for whatever allocates next.
     * No other allocator state is touched (delta/metadata paths use
     * their own cursors), and the sweep stage otherwise allocates
     * nothing while migrate runs. */
    v->shadow_cursor = v->sb.shadow_zone_start;

    for (i = 0; i < ctx.n; i++) {
        size_t ms = 0, su = 0, st = 0;
        uint64_t mb = 0, sr = 0, sb = 0;
        int fr;
        if (budget == 0) {
            budget_hit = 1;
            lr.blocks_capped += 1;
            /* Recompress needs no move budget (it reuses the sweep's own
             * admission), so a capped run still recompresses. */
            if (!recompress)
                continue;
        } else {
            fr = migrate_file(v, ctx.cand[i].id, ctx.cand[i].name, &budget,
                              &budget_hit, &ms, &mb, &sr, &sb, &su, &st);
            if (fr == 2) {
                lr.files_skipped_windows += 1;
            } else if (fr > 0) {
                lr.files_moved += 1;
                lr.segments_moved += ms;
                lr.blocks_moved += mb;
                lr.seal_ranges += sr;
                lr.seal_blocks += sb;
            }
            lr.segments_skipped_upward += su;
            lr.segments_skipped_tier += st;
        }
        if (recompress) {
            size_t nr = 0;
            uint64_t sv = 0;
            int rr = migrate_recompress_file(v, ctx.cand[i].id,
                                             ctx.cand[i].name, &nr, &sv);
            if (rr == 2)
                lr.files_skipped_multi += 1;
            else if (rr > 0) {
                lr.files_recompressed += nr;
                lr.recompress_saved += sv;
            }
        }
    }
    free(ctx.cand);

out_frag:
    vol_migrate_frag(v, &lr.free_runs_after, &lr.frag_after);
    if (!invfs_sweep_ui_active())
        printf("migrate: %zu file(s) seen, %zu moved (%zu segments, "
               "%llu blocks, %zu upward-skipped, %zu window-skipped, %zu tier-skipped)%s; frag runs %llu->%llu score %.3f->%.3f; "
               "seal-dirty %llu range(s) (%llu blocks)%s\n",
               lr.files_seen, lr.files_moved, lr.segments_moved,
               (unsigned long long)lr.blocks_moved,
               lr.segments_skipped_upward, lr.files_skipped_windows,
               lr.segments_skipped_tier,
               budget_hit ? " [budget hit]" : "",
               (unsigned long long)lr.free_runs_before,
               (unsigned long long)lr.free_runs_after,
               lr.frag_before, lr.frag_after,
               (unsigned long long)lr.seal_ranges,
               (unsigned long long)lr.seal_blocks,
               recompress ? "; recompress: active" : "");
    if (recompress && !invfs_sweep_ui_active())
        printf("migrate-recompress: %zu file(s) re-encoded to ZSTD "
               "(%llu bytes saved), %zu multi-entry skipped\n",
               lr.files_recompressed,
               (unsigned long long)lr.recompress_saved,
               lr.files_skipped_multi);
    ret = (int)(lr.segments_moved + lr.files_recompressed);

out:
    if (rep)
        *rep = lr;
    return ret;
}
