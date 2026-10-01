/*
 * invf-rollback — roll a volume back to its last save point.
 *
 *   invf-rollback <image>
 *
 * Restores the save point's {base_root, delta_end} by publishing base_root
 * via the RT30 double-slot, truncating the delta chain to delta_end, and
 * replaying. See the WP-M16 section comment in vol_spt0.c.
 *
 * There is no second engine here. The coarse WP21 sweep checkpoint this
 * replaced had no writer once the v2 metadata machinery went, so there was
 * never a v3 volume carrying one and nothing to fall back to.
 *
 * Exit codes: 0 = rolled back; 1 = no save point (nothing to roll back);
 * 5 = io/internal error (re-run: the phases are idempotent, a killed run
 * simply continues). SPT0_RC_DAMAGED is reported in place and refuses.
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
    invfs_spt0 sp;

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

    if (!spt0_info(v, &sp)) {
        fprintf(stderr, "invf-rollback: %s: no save point\n", img);
        vol_close(v);
        return 1;
    }
    printf("invf-rollback: %s: save point, base_root=%llu, "
           "delta_end=%llu\n", img,
           (unsigned long long)sp.base_root,
           (unsigned long long)sp.delta_end);
    if (vol_needs_recovery(v)) {
        /* WP101: vol_open sets needs_recovery unconditionally for the whole
         * session (src/core/volume.c), so this test is true on EVERY volume,
         * clean or not -- the old message fired on every successful rollback
         * and told the operator that a clean volume needed crash recovery.
         * The real on-disk signal is sb.state, so report that, and name what
         * this pass actually does. */
        fprintf(stderr, "invf-rollback: %s: volume state=0x%02X%s%s%s; "
                "restoring the SPT0 save point (undo the last sweep, not "
                "a crash recovery)\n", img,
                (unsigned)v->sb.state,
                (v->sb.state & INVFS_STATE_DIRTY) ? " DIRTY" : "",
                (v->sb.state & INVFS_STATE_CLEAN) ? " CLEAN" : "",
                (v->sb.state & INVFS_STATE_RECOVERY) ? " RECOVERY" : "");
    }

    rc = spt0_restore(v);
    switch (rc) {
    case 0:
        printf("invf-rollback: %s: rolled back to save point "
               "(base_root=%llu, delta_end=%llu), save point cleared\n",
               img,
               (unsigned long long)sp.base_root,
               (unsigned long long)sp.delta_end);
        break;
    case SPT0_RC_DAMAGED:
        /* WP86: a save point whose base tree does not walk is not a
         * rollback target -- rolling back onto it would trade a degraded
         * volume for an unreadable one. Say so instead of "rc 3".
         * WP96: the same refusal covers the save point's DATA: if a
         * segment the pinned recipes name no longer matches its own CRC
         * (or the pinned log prefix a fold reset is gone), the rollback
         * is refused too, and invf-spt0 has already said which. */
        fprintf(stderr, "invf-rollback: %s: the save point is DAMAGED "
                "(base_root=%llu, see the invf-spt0 diagnostic above: the "
                "pinned base tree does not walk, or the data its recipes "
                "address is no longer there); refusing to roll back onto "
                "it -- nothing was written. Quarantine the unreadable "
                "pages first (invf-fsck %s -f), then re-run.\n",
                img, (unsigned long long)sp.base_root, img);
        break;
    default:
        fprintf(stderr, "invf-rollback: %s: rollback failed (rc %d); "
                "re-run is safe\n", img, rc);
        break;
    }
    vol_close(v);
    return rc == 0 ? 0 : 5;
}
