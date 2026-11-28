/* touched_domain_test -- the ENOSPC precheck must ask touched[] in the child
 * domain it lives in.
 *
 * touched[] is per child: wsession_write_seg_n stores it at j % child_segs, and
 * wsession_switch_child memsets it. The precheck counted
 * `!(j < touched_cap && touched[j])` with the ABSOLUTE segment index, so on a
 * multi-child file it read some other child's slot. Two directions, both real:
 *
 *   - a segment of ANOTHER child that shares a relative slot with a touched one
 *     is reported as already materialized -> the call UNDER-reserves;
 *   - a segment of the CURRENT child that is materialized is reported fresh when
 *     the absolute index falls outside the bitmap -> OVER-reserves 17 blocks.
 *
 * Why an accessor instead of an end-to-end volume: a wrong count only becomes a
 * wrong DECISION when the volume's headroom happens to sit between the two
 * numbers (a window of 17*N blocks on a device with a ~500-block reserve), so a
 * test built that way would be a story about fill arithmetic. This asserts the
 * count itself, which is what the decision is.
 *
 * The child_segs=2 fixture makes a 512 KiB file span four children, so every
 * boundary case is reachable on a 100 MiB image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "volume.h"
#include "invarifs.h"
#include "volume_internal.h"

#define SEG  (64u * 1024u)
#define NSEG 8u
#define IMG  "/dev/shm/touched_domain.img"

static int failures;

static void check(const char *what, uint64_t got, uint64_t want)
{
    if (got == want) {
        printf("  [ok] %s = %llu\n", what, (unsigned long long)got);
    } else {
        printf("  [FAIL] %s = %llu, expected %llu\n", what,
               (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

int main(void)
{
    invfs_volume *v;
    invfs_wsession *ws = NULL;
    uint8_t *buf;
    char cmd[256];
    uint64_t rc;
    int err = 0, i;

    /* A child every 2 segments: child c owns absolute segments 2c and 2c+1. */
    setenv("INVFS_CHILD_SEGS", "2", 1);
    remove(IMG);
    snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 100 >/dev/null 2>&1", IMG);
    if (system(cmd) != 0) {
        fprintf(stderr, "touched_domain_test: mkfs failed (run from the repo root)\n");
        return 2;
    }

    buf = malloc(NSEG * SEG);
    if (!buf) return 2;
    /* Incompressible: the fixture must not collapse into fewer segments. */
    {
        uint64_t x = 0x2545F4914F6CDD1DULL;
        for (i = 0; i < (int)(NSEG * SEG); i++) {
            if ((i & 7) == 0) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
            buf[i] = (uint8_t)(x >> ((i & 7) * 8));
        }
    }

    v = vol_open(IMG, &err);
    if (!v) { fprintf(stderr, "touched_domain_test: open err=%d\n", err); return 2; }

    /* Content that already exists: every untouched segment is an alias of this
     * record, so writing one needs a slot of its own. */
    if (vol_create_file(v, "base.bin", buf, NSEG * SEG) == 0) {
        fprintf(stderr, "touched_domain_test: create failed\n");
        vol_close(v);
        return 2;
    }

    rc = vol_write_begin(v, "base.bin", 0, &ws);
    if (!ws) {
        fprintf(stderr, "touched_domain_test: begin failed rc=%llu\n",
                (unsigned long long)rc);
        vol_close(v);
        return 2;
    }

    /* Segment 7 -> child 3, relative slot 1. The session is now ON child 3 and
     * its bitmap says "relative slot 1 is mine". */
    if (vol_write_range(ws, 7 * SEG, buf, SEG) != 0) {
        fprintf(stderr, "touched_domain_test: write seg 7 failed\n");
        vol_write_abort(ws);
        vol_close(v);
        return 2;
    }
    printf("fixture: child_segs=2, %u segments, session moved to child 3 by seg 7\n",
           NSEG);

    /* THE DEFECT: absolute segment 1 is child 0, relative slot 1 -- the same slot
     * number, a different child. It was never written by this session, so it
     * needs a slot. The absolute-index read reported 0 here. */
    check("fresh(1,1) [child 0 rel 1, NOT mine]", wsess_fresh_segments(ws, 1, 1), 1);

    /* The other direction: segment 7 is mine, in the child the session is on. */
    check("fresh(7,7) [child 3 rel 1, mine]", wsess_fresh_segments(ws, 7, 7), 0);

    /* A segment in no child's bitmap at all. */
    check("fresh(5,5) [child 2 rel 1, not mine]", wsess_fresh_segments(ws, 5, 5), 1);

    /* A range that crosses children: the two of child 3 (segments 6,7) are the
     * only ones this session can materialize, so 4..5 must still count. */
    check("fresh(4,7) [children 2 and 3]", wsess_fresh_segments(ws, 4, 7), 2);

    vol_write_abort(ws);
    vol_close(v);
    free(buf);
    remove(IMG);

    if (failures) {
        printf("RED: %d failure(s)\n", failures);
        return 1;
    }
    printf("GREEN: touched[] is asked in the child domain it lives in\n");
    return 0;
}
