/*
 * invf-rollback — roll a volume back to its last sweep checkpoint (WP21)
 * or save point (WP-M16 for v3 volumes).
 *
 *   invf-rollback <image>
 *
 * For v2 volumes: restores the checkpoint's staged journal prefix,
 * decapitates the inode area at the checkpoint append pointer (append-only
 * soundness: everything past it — post-sweep records, tombstones, the
 * retention registry — vanishes wholesale), and lets the ordinary fsck
 * rebuild machinery reconcile bitmap+L2P: the pre-sweep record versions
 * resurrect with their retained blocks, the sweep's allocations are
 * reclaimed as orphans, and CKP0 is cleared last. See the WP21 section
 * comment in volume.c.
 *
 * For v3 volumes: restores the save point's {base_root, delta_end} by
 * publishing base_root via RT30 double-slot, truncating the delta chain
 * to delta_end, and replaying. See the WP-M16 section comment.
 *
 * Exit codes: 0 = rolled back; 1 = no checkpoint/save point (nothing
 * to roll back); 2 = refused (a live redundancy seal would be invalidated
 * — free it first: invf-sweep <img> --free-redundant); 3 = the
 * checkpoint descriptor or its journal staging failed verification (the
 * post-sweep state is untouched); 5 = io/internal error (re-run: the
 * phases are idempotent, a killed run simply continues).
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume_internal.h"
#include "invarifs.h"
#include "vol_spt0.h"

int main(int argc, char **argv)
{
    const char *img;
    int err = 0, rc;
    invfs_volume *v;
    invfs_ckp0 ck;
    uint64_t reclaimed = 0;
    int is_v3 = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-rollback <image>\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

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

    is_v3 = (v->sb.vol_flags & VOLF_V3) != 0;

    if (is_v3) {
        invfs_spt0 sp;
        if (!spt0_info(v, &sp)) {
            fprintf(stderr, "invf-rollback: %s: no save point\n", img);
            vol_close(v);
            return 1;
        }
        printf("invf-rollback: %s: save point (v3), base_root=%llu, "
               "delta_end=%llu\n", img,
               (unsigned long long)sp.base_root,
               (unsigned long long)sp.delta_end);
        if (vol_needs_recovery(v))
            fprintf(stderr, "invf-rollback: volume was not closed cleanly; "
                    "rollback proceeds as the recovery\n");

        rc = spt0_restore(v);
        switch (rc) {
        case 0:
            printf("invf-rollback: %s: rolled back to save point "
                   "(base_root=%llu, delta_end=%llu), save point cleared\n",
                   img,
                   (unsigned long long)sp.base_root,
                   (unsigned long long)sp.delta_end);
            break;
        default:
            fprintf(stderr, "invf-rollback: %s: rollback failed (rc %d); "
                    "re-run is safe\n", img, rc);
            break;
        }
        vol_close(v);
        return rc == 0 ? 0 : 5;
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
