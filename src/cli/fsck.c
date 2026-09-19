/*
 * invf-fsck: check & repair an InvariantFS volume.
 *
 * Rebuilds the L2P table and used-bitmap from the inode area (AST
 * recipes are the source of truth for block ownership). Reports:
 *   - orphaned blocks (allocated but referenced by no live AST)
 *   - missing blocks  (referenced but free)
 *   - stale L2P       (journal out of sync with the inode area)
 *   - bad inode records (CRC/length)
 *
 * Usage: invf-fsck <image> [-f|--fix] [--repair] [-q]
 *   default: read-only report; -f applies fixes (rewrites bitmap,
 *   journal and superblock state=CLEAN).
 *   --repair: WP20b layer-2 RS recovery -- after the structural scan,
 *   reconstruct CRC-failed shadow segments from the RS(32+m2, 32) parity
 *   (vol_seal2_repair). Stripes damaged beyond m2 are reported and left
 *   untouched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"
#include "invarifs.h"

int main(int argc, char **argv)
{
    const char *img = NULL;
    int fix = 0, quiet = 0, repair = 0, i;
    int err = 0;
    invfs_volume *v;
    invfs_fsck_report rep;
    invfs_seal2_repair r2;
    int issues;

    memset(&r2, 0, sizeof r2);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-fsck <image> [-f|--fix] [--repair] [-q]\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fix") == 0)
            fix = 1;
        else if (strcmp(argv[i], "--repair") == 0)
            repair = 1;
        else if (strcmp(argv[i], "-q") == 0)
            quiet = 1;
        else
            img = argv[i];
    }
    if (!img) {
        fprintf(stderr, "usage: invf-fsck <image> [-f|--fix] [--repair] [-q]\n");
        return 2;
    }

    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "invf-fsck: cannot open %s (err %d)\n", img, err);
        return 1;
    }

    /* WP-M4: a format-v3 volume uses the metadata-v3 base tree, not the v2
     * inode-record stream. vol_fsck_scan dispatches to the v3 checker (RT30
     * root double-slot + base-tree walk). It detects and reports only -- v3
     * repair is a follow-up WP -- so -f changes nothing here; damage exits
     * nonzero. The v2 path below is untouched. */
    if (vol_sb(v)->vol_flags & VOLF_V3) {
        const invfs_superblock *sb = vol_sb(v);
        if (vol_fsck_scan(v, &rep, fix) != 0) {
            fprintf(stderr, "invf-fsck: v3 scan failed\n");
            vol_close(v);
            return 1;
        }
        if (!quiet) {
            printf("InvariantFS fsck: %s\n", img);
            printf("  state:        %s\n",
                   sb->state == INVFS_STATE_CLEAN ? "CLEAN" :
                   sb->state == INVFS_STATE_DIRTY ? "DIRTY" :
                   sb->state == INVFS_STATE_RECOVERY ? "RECOVERY" : "UNKNOWN");
            printf("  format:       v3 (metadata-v3 base tree)\n");
            if (rep.v3_rt30_bad)
                printf("  root desc:    TORN (magic/version/CRC)\n");
            printf("  root seq:     %llu\n",
                   (unsigned long long)rep.v3_root_seq);
            printf("  pages walked: %llu\n",
                   (unsigned long long)rep.v3_pages_walked);
            printf("  base keys:    %llu\n",
                   (unsigned long long)rep.v3_keys);
            printf("  torn slots:   %llu\n",
                   (unsigned long long)rep.v3_slots_torn);
            if (rep.v3_slots_ambiguous)
                printf("  ambiguous slots: %llu (both root slots valid at "
                       "the same gen)\n",
                       (unsigned long long)rep.v3_slots_ambiguous);
            printf("  bad pages:    %llu\n",
                   (unsigned long long)rep.v3_bad_pages);
            printf("  cycles/shared: %llu\n",
                   (unsigned long long)rep.v3_cycles);
            if (rep.v3_reachable_free)
                printf("  reachable-but-free pages: %llu (bitmap divergence)\n",
                       (unsigned long long)rep.v3_reachable_free);
            printf("%s\n", rep.v3_damaged ? "DAMAGED" : "OK");
        }
        vol_close(v);
        return rep.v3_damaged ? 3 : 0;
    }

    /* WP25: a degraded mount (dev0 absent) is read-only -- report mode
     * works (every structure reads from the dev1 mirror), but -f mutates:
     * the mirror would diverge with dev0 absent. Reattach dev0 first. */
    if (fix && vol_degraded(v)) {
        fprintf(stderr, "invf-fsck: %s: DEGRADED volume (dev0 absent) -- "
                "report mode only; reattach dev0 to repair\n", img);
        vol_close(v);
        return 1;
    }

    /* WP21: with a sweep checkpoint live, the rebuild's orphan reclaim is
     * the one pass that could free a not-yet-registered retained block
     * out from under a future rollback. Report mode is unaffected; -f is
     * refused until the checkpoint is resolved. (invf-rollback drives the
     * same engine with the checkpoint in place -- it is not blocked.) */
    if (fix && vol_ckp_armed(v)) {
        fprintf(stderr, "invf-fsck: %s: a sweep checkpoint is live; -f "
                "would break rollback. Resolve it first: invf-rollback %s "
                "(undo the sweep) or invf-sweep %s --realize (accept it)\n",
                img, img, img);
        vol_close(v);
        return 1;
    }

    /* WP22e: an interrupted inode-area compaction (CMP0 armed + the
     * read-only latch) must be completed BEFORE any scan -- the area may
     * be torn mid-copy and only the staging run holds the compacted
     * stream. -f rolls the staging in (idempotent; verified before
     * anything is overwritten) and clears descriptor+latch; the staging
     * run then reads as an ordinary orphan for the rebuild below, exactly
     * like a stranded one from a pre-arm crash. Report mode notes it and
     * scans what is there (inspection). */
    if (vol_compact_pending(v)) {
        if (!fix) {
            fprintf(stderr, "invf-fsck: %s: an interrupted inode-area "
                    "compaction is pending; the scan below may be partial "
                    "-- run invf-fsck -f %s to finish it\n", img, img);
        } else {
            if (vol_compact_recover(v) < 0) {
                fprintf(stderr, "invf-fsck: %s: compaction roll-forward "
                        "failed; the volume is left for review\n", img);
                vol_close(v);
                return 1;
            }
            if (!quiet)
                printf("  compaction: interrupted pass rolled forward\n");
        }
    }

    if (vol_fsck_scan(v, &rep, fix) != 0) {
        fprintf(stderr, "invf-fsck: scan failed\n");
        vol_close(v);
        return 1;
    }

    /* WP20b: layer-2 RS recovery runs after the structural scan, so the
     * bitmap/L2P the repair trusts are the just-verified ones. */
    if (repair) {
        if (vol_seal2_repair(v, &r2) != 0) {
            fprintf(stderr, "invf-fsck: seal2 repair pass failed\n");
            vol_close(v);
            return 1;
        }
        if (!quiet && (r2.stripes_scanned || r2.stripes_repaired ||
                       r2.unrecoverable))
            printf("  seal2 repair: %llu damaged stripes, %llu repaired "
                   "(%llu blocks rewritten), %llu unrecoverable "
                   "(%llu hypotheses)\n",
                   (unsigned long long)r2.stripes_scanned,
                   (unsigned long long)r2.stripes_repaired,
                   (unsigned long long)r2.blocks_rewritten,
                   (unsigned long long)r2.unrecoverable,
                   (unsigned long long)r2.hypotheses);
    }

    issues = (rep.orphans || rep.missing || rep.bad_recs || rep.l2p_miss ||
              rep.cut_records || rep.lost_files || rep.corrupt_files ||
              r2.unrecoverable);
    if (!quiet) {
        const invfs_superblock *sb = vol_sb(v);
        printf("InvariantFS fsck: %s\n", img);
        printf("  state:        %s\n",
               sb->state == INVFS_STATE_CLEAN ? "CLEAN" :
               sb->state == INVFS_STATE_DIRTY ? "DIRTY" :
               sb->state == INVFS_STATE_RECOVERY ? "RECOVERY" : "UNKNOWN");
        printf("  live files:   %llu\n", (unsigned long long)rep.live_files);
        printf("  l2p entries:  %llu\n", (unsigned long long)rep.l2p_entries);
        printf("  l2p misses:   %llu (AST segments with an invalid pba/extent - data lost)\n",
               (unsigned long long)rep.l2p_miss);
        printf("  cut records:  %llu (torn newest versions, fallback live)%s\n",
               (unsigned long long)rep.cut_records,
               rep.cut_records ? (fix ? " -> quarantined" :
                                  " (use -f to quarantine)") : "");
        printf("  lost files:   %llu (no readable version)%s\n",
               (unsigned long long)rep.lost_files,
               rep.lost_files ? (fix ? " -> quarantined" :
                                 " (use -f to quarantine)") : "");
        if (rep.corrupt_files)
            printf("  corrupt files: %llu (segment CRC failed)%s\n",
                   (unsigned long long)rep.corrupt_files,
                   fix ? " -> quarantined" : "");
        printf("  orphans:      %llu%s\n", (unsigned long long)rep.orphans,
               rep.orphans ? (fix ? " -> freed" : " (use -f to free)") : "");
        if (rep.held_ckpt)
            printf("  held for checkpoint: %llu (retained until "
                   "rollback/realize)\n", (unsigned long long)rep.held_ckpt);
        printf("  missing:      %llu%s\n", (unsigned long long)rep.missing,
               rep.missing ? (fix ? " -> restored" : " (use -f)") : "");
        printf("  bad records:  %llu\n", (unsigned long long)rep.bad_recs);
        printf("  free blocks:  %llu\n",
               (unsigned long long)vol_free_blocks_cached(v));
        {
            invfs_ckp0 ck;
            if (vol_ckp_info(v, &ck))
                printf("  checkpoint:   sweep #%llu live (undo: "
                       "invf-rollback; accept: invf-sweep --realize)\n",
                       (unsigned long long)ck.sweep_seq);
        }
        printf("%s\n", issues ? (fix || r2.stripes_repaired
                                 ? "REPAIRED" : "ISSUES FOUND")
                              : "OK");
    }

    vol_close(v);
    return issues ? 3 : 0;
}
