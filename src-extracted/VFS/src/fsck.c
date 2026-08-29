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
        printf("  l2p misses:   %llu (AST segments without L2P - data lost)\n",
               (unsigned long long)rep.l2p_miss);
        printf("  orphans:      %llu%s\n", (unsigned long long)rep.orphans,
               rep.orphans ? (fix ? " -> freed" : " (use -f to free)") : "");
        printf("  missing:      %llu%s\n", (unsigned long long)rep.missing,
               rep.missing ? (fix ? " -> restored" : " (use -f)") : "");
        printf("  bad records:  %llu\n", (unsigned long long)rep.bad_recs);
        printf("  free blocks:  %llu\n",
               (unsigned long long)vol_free_blocks_cached(v));
        printf("%s\n", issues ? (fix || r2.stripes_repaired
                                 ? "REPAIRED" : "ISSUES FOUND")
                              : "OK");
    }

    vol_close(v);
    return issues ? 3 : 0;
}
