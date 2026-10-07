/* spn_walk_tear_test.c -- the savepoint walk fails closed on a torn delta
 * chain (F8).
 *
 * WHAT THIS IS
 * ------------
 * spn_reclaim frees old_map \ new_map, where new_map is built by walking
 * the pinned generation (vol_iter_inodes_at over base + delta replay). The
 * replay used to BREAK SILENTLY on a torn/unreadable segment header (and on
 * a torn record tail inside a segment): every older segment vanished from
 * the walk, rows whose only record lay below the tear were never visited,
 * no indeterminacy was counted, and the reclaim freed their blocks under
 * live recipes. In leg 5 this killed one quiescent file per run (s18.bin,
 * r00.bin): recipe still naming the blocks, blocks reallocated and
 * overwritten, final verify red -- while the daemon kept serving the
 * pre-corruption bytes from ARC, so every op readback stayed green.
 *
 * WHY THIS IS NOT A PROBABILITY TEST
 * ----------------------------------
 * The interleave is PLANNED: a real volume, a real victim file buried in an
 * older delta segment, then surgical sabotage of a NEWER segment's header
 * (direct pwrite, the torn-write shape dm-flakey produces). The collection
 * loop reads the intact head, then hits the torn header:
 *
 *   pre-fix:  break, replay the head alone, return 0 with the victim
 *             never visited. The test reports that as FAIL (red).
 *   post-fix: return -1. The capture fails, the sweep refuses. GREEN.
 *
 * THE CONTROL ON THE CONTROL. Before the sabotage the same walk must
 * return 0 AND visit the victim: if the setup cannot even see the file on
 * an intact chain, a refusal after sabotage proves nothing. That is a FAIL
 * too, not a pass. Filler records use 16-byte keys so they consume chain
 * space without ever entering the walk (it only takes 8-byte inode rows).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "volume_internal.h"   /* pulls in volume.h + invarifs.h */
#include "vol_delta.h"
#include "vol_spt0.h"

static int checks = 0;
static int failures = 0;
static char g_img[512];
static invfs_volume *g_v = NULL;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
}

static uint64_t g_victim = 0;
static int g_visited = 0;
static int g_victim_seen = 0;

static int walk_cb(invfs_volume *v, uint64_t id, const invfs_inode *in,
                   void *ctx)
{
    (void)v;
    (void)in;
    (void)ctx;
    g_visited++;
    if (id == g_victim)
        g_victim_seen = 1;
    return 0;
}

/* snapshot the live chain the way spt0_capture does: current base root +
 * full current delta prefix. Returns 0 with *segs > 0, or -1. */
static int snap(uint64_t *root_pba, uint64_t *end, uint64_t *segs,
                uint64_t *head)
{
    invfs_blkptr root;
    uint64_t cur, n = 0, bytes = 0;
    if (vol_base_root(g_v, &root) != 0)
        return -1;
    cur = g_v->delta_seg_pba;
    while (cur && n < 64) {
        invfs_delta_seg_hdr h;
        n++;
        if (delta_read_hdr(g_v, cur, &h) != 0)
            return -1;
        if (h.prev_pba == cur)
            break;
        cur = h.prev_pba;
    }
    if (n == 0)
        return -1;
    bytes = (n - 1) * (uint64_t)INVFS_DELTA_SEG_BYTES + g_v->delta_bump;
    *root_pba = root.pba;
    *end = bytes;
    *segs = n;
    *head = g_v->delta_seg_pba;
    return 0;
}

static int walk_now(int *rc_out, int *visited_out, int *victim_out)
{
    uint64_t root_pba, end, segs, head;
    int rc;
    if (snap(&root_pba, &end, &segs, &head) != 0)
        return -1;
    g_visited = 0;
    g_victim_seen = 0;
    rc = vol_iter_inodes_at(g_v, root_pba, end, segs, head, walk_cb, NULL);
    *rc_out = rc;
    *visited_out = g_visited;
    *victim_out = g_victim_seen;
    return 0;
}

/* walk with a snapshot taken BEFORE sabotage: the sabotage tears a chain
 * segment header, so re-snapshotting afterwards cannot even count the
 * chain. A real capture snapshots-then-walks back-to-back on a static
 * image, which is exactly this. */
static uint64_t g_snap_root, g_snap_end, g_snap_segs, g_snap_head;
static int walk_saved(int *rc_out, int *visited_out, int *victim_out)
{
    int rc;
    g_visited = 0;
    g_victim_seen = 0;
    rc = vol_iter_inodes_at(g_v, g_snap_root, g_snap_end, g_snap_segs,
                            g_snap_head, walk_cb, NULL);
    *rc_out = rc;
    *visited_out = g_visited;
    *victim_out = g_victim_seen;
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    static const char victim_content[] = "F8 victim payload, small";
    uint8_t filler[12 * 1024];
    uint8_t key[16];
    int err = 0, i, rc, visited, seen;
    uint64_t seg_head, seg_older = 0;
    int fd;

    printf("spn_walk_tear_test: torn delta chain fails the generation walk\n");
    memset(filler, 0xA5, sizeof filler);

    snprintf(g_img, sizeof g_img, "%s/invf-spn-walk-tear-test.img", dir);
    unlink(g_img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 root, g_img);
        if (system(cmd) != 0) {
            printf("  cannot create volume with invf-mkfs\n");
            return 2;
        }
    }
    g_v = vol_open(g_img, &err);
    if (!g_v) {
        printf("  vol_open failed: err=%d\n", err);
        return 2;
    }

    /* the victim, then enough filler to roll the chain at least twice, so
     * the victim sits two segments deep and sabotage lands in front of it */
    ok(vol_replace_file(g_v, "victim.bin",
                        (const uint8_t *)victim_content,
                        sizeof victim_content - 1) != 0,
       "victim.bin written");
    g_victim = vol_find(g_v, "victim.bin");
    ok(g_victim != 0, "victim.bin resolves");
    for (i = 0; i < 28; i++) {
        memset(key, 0, sizeof key);
        key[0] = (uint8_t)(0xF0 + (i & 0x0F));
        key[1] = (uint8_t)i;
        if (vol_delta_append(g_v, key, sizeof key, filler, sizeof filler,
                             0) != 0)
            break;
    }
    ok(i == 28, "28 filler records appended (chain rolled)");

    /* CONTROL: intact chain -- walk succeeds and sees the victim */
    ok(walk_now(&rc, &visited, &seen) == 0, "control walk ran");
    ok(rc == 0, "control walk returns 0 on the intact chain");
    ok(seen, "control walk visits victim.bin");
    printf("        (control: %d rows visited)\n", visited);

    /* snapshot BEFORE sabotage (see walk_saved), then tear the header of
     * the segment right after the head, i.e. strictly newer than the
     * victim's record. Collection reads the head, then hits this. */
    ok(snap(&g_snap_root, &g_snap_end, &g_snap_segs, &g_snap_head) == 0 &&
       g_snap_segs >= 2,
       "snapshot taken (chain rolled, victim buried)");
    {
        invfs_delta_seg_hdr h;
        uint64_t cur = g_v->delta_seg_pba;
        if (delta_read_hdr(g_v, cur, &h) == 0 && h.prev_pba != cur)
            seg_older = h.prev_pba;
    }
    ok(seg_older != 0, "a second chain segment exists to tear");
    fd = open(g_img, O_RDWR);
    ok(fd >= 0, "sabotage fd opened");
    if (fd >= 0) {
        static const uint8_t garbage[44] = {
            0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33,
        };
        ssize_t w = pwrite(fd, garbage, sizeof garbage,
                            (off_t)(seg_older * INVFS_BLOCK_SIZE));
        ok(w == (ssize_t)sizeof garbage, "segment header torn");
        close(fd);
    }

    /* RED/GREEN: the walk must refuse, not silently skip the victim */
    ok(walk_saved(&rc, &visited, &seen) == 0, "torn walk ran");
    ok(rc != 0, "walk FAILS on the torn chain (no silent amputation)");
    printf("        (torn: rc=%d, %d rows visited, victim %s)\n",
           rc, visited, seen ? "seen (unexpected)" : "not seen");

    vol_close(g_v);
    printf("spn_walk_tear_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
