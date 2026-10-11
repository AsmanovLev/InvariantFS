/* vol_migrate.h — WP601 migration zone: defrag relocation + policy
 * recompression (Phases A and B).
 *
 * WHAT THIS IS. The sweep's `migrate` stage, run between `batches` and
 * `finalize` (seal runs after, so seal sees the compacted layout). Two
 * halves, one protocol each:
 *
 *  Phase A (relocate + defrag): per-segment canonical relocate —
 *  alloc new -> copy framed bytes -> pread-back memcmp verify ->
 *  single-recipe publish (the leg2 discipline: vol_inode_delta_put's rc
 *  decides published, never assumed) -> seal-dirty-mark the old pba
 *  ranges -> free the old extents (the savepoint pins them for one
 *  generation: the standard sweep debt, measured not feared).
 *  Crash-mid-move: pre-repoint death leaves garbage new segments
 *  (unclaimed; the orphan collector reclaims them); post-repoint death
 *  leaves new-live + old-freed-or-pinned. NEVER a splice: one delta_put
 *  publishes every entry of a file at once, so each move lands
 *  complete-old or complete-new (the leg7 assertion shape, per move).
 *
 *  Phase B (explicit recompress): `invf-sweep --recompress` re-runs a
 *  generic-ZSTD encode over eligible files on demand, each through the
 *  lane's own bit-exactness guard (decode-back memcmp + strictly-smaller
 *  size guard). A recompress that cannot prove itself keeps the
 *  original, stamps nothing, and retries on generation roll.
 *  Heat-tiered recompression is a PROPOSAL with numbers (see the sweep
 *  report): if the measurements say tiering buys nothing, explicit-only
 *  ships with the negative result quoted.
 *
 * WHAT THIS IS NOT. Not a transform/dedupe/tier-copy/batch change (those
 * are read, never altered, here). Not seal math (seal_dirty_mark is
 * consumed, never redesigned). v1 selection moves only exclusively-owned
 * (pba-refcount == 1), RAW-zone segments into shadow holes, retagging
 * them BINARY so the pass converges: dedupe-shared segments are already
 * consolidated, and copying them per file would undo dedupe.
 * v1 recompress handles single-entry recipes only (the common small-file
 * shape); multi-entry files are skipped and counted, not half-done.
 */
#ifndef INVFS_VOL_MIGRATE_H
#define INVFS_VOL_MIGRATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct invfs_volume invfs_volume;

/* Budget cap per run (blocks). 1024 blocks == 4 MiB of moved payload per
 * pass: moves coexist with their sources until the publish lands, so an
 * unbounded run could wedge a nearly-full volume the migrate was meant
 * to relieve. max_blocks == 0 selects this default. */
#define VOL_MIGRATE_DEFAULT_MAX_BLOCKS 1024u

typedef struct {
    size_t files_seen;          /* live candidates walked */
    size_t files_moved;         /* files with >= 1 segment relocated */
    size_t segments_moved;      /* relocated segment count */
    size_t segments_skipped_upward; /* copies freed unpublished: new pba not below old */
    uint64_t blocks_moved;      /* new-extent blocks written */
    uint64_t blocks_capped;     /* moves skipped by the per-run budget */
    size_t files_recompressed;  /* Phase B explicit recompress publishes */
    uint64_t recompress_saved;  /* payload bytes saved by recompress */
    size_t files_skipped_multi; /* recompress-eligible shape but multi-entry */
    uint64_t seal_ranges;       /* old-pba ranges passed to seal_dirty_mark */
    uint64_t seal_blocks;       /* blocks covered by those ranges */
    uint64_t free_runs_before;  /* fragmentation metric, before ... */
    double frag_before;         /* ... and after (see vol_migrate_frag) */
    uint64_t free_runs_after;
    double frag_after;
    int walk_refused;           /* 1: the live-set walk stopped; nothing moved */
} vol_migrate_report;

/* Fragmentation metric: over the data region [raw_zone_start,
 * total_blocks), R = number of contiguous free runs, F = free blocks,
 * L = largest free run; score = F ? 1 - L/F : 0. 0 == one contiguous run
 * (or full), -> 1 == maximally scattered. Cheap (one bitmap scan), and
 * the thing the selection exists to move. */
void vol_migrate_frag(invfs_volume *v, uint64_t *runs_out, double *score_out);

/* Run the migrate stage. recompress != 0 arms the Phase B explicit
 * recompress alongside relocation. max_blocks == 0 selects
 * VOL_MIGRATE_DEFAULT_MAX_BLOCKS. rep may be NULL. Returns relocated
 * segments + recompressed files (>= 0); 0 when read-only, refused, or
 * nothing was movable. Never a splice: see the header contract. */
int vol_migrate_run(invfs_volume *v, int recompress, uint64_t max_blocks,
                    vol_migrate_report *rep);

#endif /* INVFS_VOL_MIGRATE_H */
