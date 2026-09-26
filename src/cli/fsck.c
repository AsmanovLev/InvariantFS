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
#include "vol_spt0.h"

int main(int argc, char **argv)
{
    const char *img = NULL;
    int fix = 0, quiet = 0, repair = 0, i;
    int err = 0;
    invfs_volume *v;
    invfs_fsck_report rep;
    invfs_seal2_repair r2;
    int issues;

    memset(&rep, 0, sizeof rep);
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

    /* WP25: a degraded mount (dev0 absent) is read-only -- report mode
     * works (every structure reads from the dev1 mirror), but -f mutates:
     * the mirror would diverge with dev0 absent. Reattach dev0 first.
     *
     * WP98: this check sat BELOW the v3 branch, so on a Meta-v3 volume the
     * v3 return skipped it and `invf-fsck -f` happily ran its repair pass
     * against a read-only, degraded mount -- writing to the dev1 mirror
     * alone, which on v3 nothing detects or repairs (no DEVT sync_seq bump
     * and no mirror_resync: vol_flush returns before that tail for VOLF_V3,
     * volume.c:2435). Hoisted above both branches, so the refusal is format
     * independent. Report mode is unaffected. */
    if (fix && vol_degraded(v)) {
        fprintf(stderr, "invf-fsck: %s: DEGRADED volume (dev0 absent) -- "
                "report mode only; reattach dev0 to repair\n", img);
        vol_close(v);
        return 1;
    }

    /* WP-M4: a format-v3 volume uses the metadata-v3 base tree, not the v2
     * inode-record stream. vol_fsck_scan dispatches to the v3 checker (RT30
     * root double-slot + base-tree walk).
     *
     * WP86: that walk CONTAINS an unreadable base page instead of stopping at
     * it -- the page's key range is quarantined, every other subtree stays
     * readable, and a key inside a quarantined range reads EIO. `-f` excises
     * the quarantined ranges, folds the delta back in (which restores every
     * quarantined key the delta still holds) and names what is left, which is
     * gone for good.
     *
     * The exit code never softens the alarm: any damage found makes this pass
     * exit 3, repair or not (the v2 -f contract -- `issues` is computed from
     * what was found, not from what was fixed). A volume that is still
     * damaged after -f is reported as DEGRADED with the reason, never as OK,
     * and a damage -f cannot repair at all says so instead of doing nothing.
     * The v2 path below is untouched.
     *
     * WP99: the walk only ever sees ONE copy of the metadata, so on a
     * two-device volume it could not see the failure that actually loses
     * data. It walks whatever the mux serves -- the dev1 mirror once dev0 is
     * known to be stale -- and prints OK, while the tree reachable from the
     * OTHER device is a rolled-back generation whose next publish drops
     * every delta record past it. So the verdict now also asks the two
     * devices directly (vol_mirror_compare, the same call the mount path
     * makes) and a stale mirror is named, never folded into a clean OK. A
     * resync the pass itself performed is reported as such. */
    if (vol_sb(v)->vol_flags & VOLF_V3) {
        const invfs_superblock *sb = vol_sb(v);
        int degraded = 0;
        int mstale = -1, mstale_after = -1, mresynced = 0;
        const char *mwhy = NULL, *mwhy_after = NULL;
        (void)vol_mirror_compare(v, &mstale, &mwhy);
        if (vol_fsck_scan(v, &rep, fix) != 0) {
            fprintf(stderr, "invf-fsck: v3 scan failed\n");
            vol_close(v);
            return 1;
        }
        /* -f writes, so a resync pending from the open may have just run
         * (vol_commit_mirror). Ask again rather than reporting a mirror this
         * pass already put right -- but only after making sure it did: the
         * resync is a flush-time action and a report leaves the volume
         * untouched, so on a tree with no structural damage nothing would
         * have flushed and the "-f" this report points the operator at would
         * be a lie. */
        if (mstale >= 0 && fix) {
            if (vol_flush(v) != 0)
                fprintf(stderr, "invf-fsck: %s: the mirror resync flush "
                        "failed; the devices are still out of sync\n", img);
            (void)vol_mirror_compare(v, &mstale_after, &mwhy_after);
            if (mstale_after < 0)
                mresynced = 1;
        }
        if (rep.v3_rt30_bad || rep.v3_root_lost) {
            degraded = 1;
            fprintf(stderr, "invf-fsck: %s: CANNOT REPAIR: the base tree is "
                            "unreachable (no valid RT30 root). Only the delta "
                            "-- the writes since the last fold -- is readable; "
                            "rebuilding the base from the delta alone would "
                            "silently drop every key folded before it, so this "
                            "pass refuses. The volume is intact but unreadable: "
                            "restore it from a backup or from the original "
                            "image.\n", img);
        } else if (fix && rep.v3_quarantined && !rep.v3_repaired) {
            degraded = 1;
            fprintf(stderr, "invf-fsck: %s: CANNOT REPAIR: %llu unreadable base "
                            "page(s) could not be quarantined away; the volume "
                            "is unchanged and still degraded\n", img,
                    (unsigned long long)rep.v3_bad_pages);
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
            if (rep.v3_quarantined)
                printf("  quarantined:  %llu key range(s) -- a key inside one "
                       "reads EIO, everything else is readable\n",
                       (unsigned long long)rep.v3_quarantined);
            printf("  cycles/shared: %llu\n",
                   (unsigned long long)rep.v3_cycles);
            /* WP75: the v3 branch used to return before the v2 free-block
             * line, so free-block accounting was invisible on v3 volumes. */
            printf("  free blocks:  %llu\n",
                   (unsigned long long)vol_free_blocks_cached(v));
            if (rep.v3_reachable_free)
                printf("  reachable-but-free pages: %llu (bitmap divergence)\n",
                       (unsigned long long)rep.v3_reachable_free);
            if (spt0_info(v, NULL))
                printf("  save point:   %s\n",
                       rep.v3_savepoint_bad
                       ? "live, pinning a DAMAGED base tree -- invf-rollback "
                         "refuses it; invf-fsck -f drops it"
                       : "live");
            if (rep.v3_repaired) {
                printf("  repaired:     %llu quarantined range(s) excised; "
                       "%llu key(s) recovered from the delta\n",
                       (unsigned long long)rep.v3_quarantined,
                       (unsigned long long)rep.v3_keys_quarantined);
                if (rep.v3_lost_names)
                    printf("  LOST:         %llu name(s) -- their base page is "
                           "unreadable and the data is NOT recoverable (each "
                           "one is named above)\n",
                           (unsigned long long)rep.v3_lost_names);
                else
                    printf("  LOST:         unrecoverable, and not "
                           "attributable by name: the directory entries that "
                           "named the lost files were inside a quarantined "
                           "range too (see above)\n");
            }
            /* WP99: the two-device mirror verdict, asked of the devices
             * rather than of whatever the mux served the walk. A stale
             * device is named with the signal that saw it, because the
             * consequence is specific: a dev0 that is behind holds a
             * rolled-back root descriptor, and mounting it read-write
             * adopts that generation and drops the delta records past it. */
            if (mresynced)
                printf("  mirror:       dev%d was stale (%s); RESYNCED by this "
                       "pass, both devices now carry the same generation\n",
                       mstale, mwhy ? mwhy : "block 0");
            else if (mstale >= 0)
                printf("  mirror:       dev%d is STALE (%s) -- its block 0 is "
                       "behind the other device's. The tree walked above is "
                       "the newer copy and reads fine, but this volume is NOT "
                       "in sync: a mount that serves dev%d would adopt a "
                       "rolled-back root and drop the writes since. Reattach "
                       "both devices and run `invf-fsck -f %s` to resync "
                       "(newest state wins).\n",
                       mstale, mwhy ? mwhy : "block 0", mstale, img);
            else if (vol_ndev(v) == 2)
                printf("  mirror:       in sync\n");
            /* The verdict line keeps the v2 vocabulary (and the exact strings
             * the e2e gates grep for): OK / DAMAGED / REPAIRED. The exit code
             * is 3 whenever damage was found, REPAIRED or not -- the pass that
             * finds the damage is the one that raises the alarm. What was
             * repaired, and what is gone, is spelled out above.
             * WP99 adds MIRROR STALE, a verdict of its own for a volume whose
             * one device is behind: the tree is walkable, so DAMAGED would be
             * wrong, and OK would be the silent lie this pass exists to stop. */
            if (!rep.v3_damaged && mstale >= 0 && !mresynced)
                printf("MIRROR STALE\n");
            else if (!rep.v3_damaged)
                printf("OK\n");
            else if (rep.v3_repaired)
                printf("REPAIRED\n");
            else
                printf("DAMAGED\n");
            (void)degraded;
        }
        vol_close(v);
        return (rep.v3_damaged || (mstale >= 0 && !mresynced)) ? 3 : 0;
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

    /* WP-M21: CMP0/CMPS retired with online compaction. No descriptor is
     * ever armed, no staging run can be stranded. The fall-through scan
     * (below) is the only thing needed. */

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
