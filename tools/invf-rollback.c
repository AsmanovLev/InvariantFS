/*
 * invf-rollback — roll a volume back to its last sweep checkpoint (WP21).
 *
 *   invf-rollback <image>
 *
 * Offline tool (same single-opener assumption as invf-fsck). Restores the
 * checkpoint's staged journal prefix, decapitates the inode area at the
 * checkpoint append pointer (append-only soundness: everything past it —
 * post-sweep records, tombstones, the retention registry — vanishes
 * wholesale), and lets the ordinary fsck rebuild machinery reconcile
 * bitmap+L2P: the pre-sweep record versions resurrect with their retained
 * blocks, the sweep's allocations are reclaimed as orphans, and CKP0 is
 * cleared last. See the WP21 section comment in volume.c.
 *
 * Exit codes: 0 = rolled back; 1 = no checkpoint (nothing to roll back);
 * 2 = refused (a live redundancy seal would be invalidated — free it
 * first: invf-sweep <img> --free-redundant); 3 = the checkpoint
 * descriptor or its journal staging failed verification (the post-sweep
 * state is untouched); 5 = io/internal error (re-run: the phases are
 * idempotent, a killed run simply continues).
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"
#include "invarifs.h"

int main(int argc, char **argv)
{
    const char *img;
    int err = 0, rc;
    invfs_volume *v;
    invfs_ckp0 ck;
    uint64_t reclaimed = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: invf-rollback <image>\n");
        return 2;
    }
    img = argv[1];

    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "invf-rollback: cannot open %s (err %d)\n",
                img, err);
        return 5;
    }
    if (!vol_ckp_info(v, &ck)) {
        fprintf(stderr, "invf-rollback: %s: no checkpoint\n", img);
        vol_close(v);
        return 1;
    }
    printf("invf-rollback: %s: checkpoint #%llu (unix %llu), inode area "
           "-> %llu, journal -> %llu\n", img,
           (unsigned long long)ck.sweep_seq,
           (unsigned long long)ck.time_unix,
           (unsigned long long)ck.inode_area_pos,
           (unsigned long long)ck.journal_pos);
    if (vol_needs_recovery(v))
        fprintf(stderr, "invf-rollback: volume was not closed cleanly; "
                "rollback proceeds as the recovery\n");

    rc = vol_rollback(v, &reclaimed);
    switch (rc) {
    case 0:
        printf("invf-rollback: rolled back to checkpoint #%llu; "
               "%llu post-sweep blocks reclaimed, checkpoint cleared\n",
               (unsigned long long)ck.sweep_seq,
               (unsigned long long)reclaimed);
        break;
    case -2:
        fprintf(stderr, "invf-rollback: %s: a redundancy seal is live; "
                "rollback would invalidate the parity stripes -- free it "
                "first: invf-sweep %s --free-redundant\n", img, img);
        break;
    case -3:
        fprintf(stderr, "invf-rollback: %s: checkpoint #%llu failed "
                "verification (descriptor bounds or journal staging); the "
                "post-sweep state is untouched\n", img,
                (unsigned long long)ck.sweep_seq);
        break;
    default:
        fprintf(stderr, "invf-rollback: %s: rollback failed (rc %d); "
                "re-run is safe (the phases are idempotent)\n", img, rc);
        break;
    }
    vol_close(v);
    return rc == 0 ? 0 : rc == -2 ? 2 : rc == -3 ? 3 : 5;
}
