/* nlink_v3_test.c — WP118: the nlink-vs-dirent-fan-in invariant.
 *
 * The bug this exists for (WP111b, commit 1771a0d): a rootfs with 7 names
 * reported "5 files ok, 0 corrupt" under `invf-verify --deep` and "OK"
 * under `invf-fsck`, because two pairs of names pointed at one inode id
 * each. Both tools walked live INODES, so a name that landed on an inode
 * it did not belong to was invisible: the walk visited the id once and
 * had no way to notice the second name.
 *
 * The invariant that catches it is not "inode ids must be unique" -- that
 * is false for every hardlink, which is why it was never the right check
 * and is why this test leads with the hardlink case. The right one is
 * ACCOUNTING: for every live inode, the number of names that resolve to it
 * (its fan-in) must equal its nlink.
 *
 * Legs:
 *   A  three plain files                       -> balances
 *   B  hardlinks: 3 names on one inode, plus a
 *      cross-directory link                   -> STILL BALANCES (the
 *      positive control: a uniqueness check fails here)
 *   C  the same volume after a remount        -> still balances
 *   D  ALIASING: a raw second name on an inode whose nlink was never
 *      bumped -- the exact on-disk shape of the WP111b loss -- detected,
 *      with the inode id and the discrepancy, by BOTH vol_v3_nlink_audit
 *      and vol_fsck_scan
 *   E  the same volume once the accounting is put right -> balances again
 *   F  the other direction: a name removed behind nlink's back (reported
 *      as an orphan row, NOT damage -- see leg F1 for why) and an inflated
 *      nlink on a named inode (fatal, detected the other way round)
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_delta.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  OK    %s\n", what);
    }
}

static invfs_volume *g_v;
static char g_img[512];

/* The audit as the check itself, with the numbers spelled out: a bare
 * "no mismatch" would pass just as well on a walk that visited nothing. */
static void expect_balanced(const char *what, uint64_t names, uint64_t inodes)
{
    invfs_nlink_audit a;
    char b[160];
    int rc = vol_v3_nlink_audit(g_v, &a);
    snprintf(b, sizeof b, "%s: audit completes", what);
    ok(rc == 0, b);
    snprintf(b, sizeof b, "%s: no fan-in mismatch (names=%llu inodes=%llu "
             "missing=%llu stale=%llu)%s%s", what,
             (unsigned long long)a.names, (unsigned long long)a.inodes,
             (unsigned long long)a.missing_names,
             (unsigned long long)a.stale_dirents,
             a.nfault ? " -- first offender: " : "",
             a.nfault ? a.fault[0].name : "");
    if (a.nfault)
        printf("          (id=%llu nlink=%u fanin=%u reason=%s)\n",
               (unsigned long long)a.fault[0].id, a.fault[0].nlink,
               a.fault[0].fanin, a.fault[0].reason);
    ok(!a.mismatch && a.nfault_total == 0, b);
    snprintf(b, sizeof b, "%s: counted %llu names over %llu inodes", what,
             (unsigned long long)a.names, (unsigned long long)a.inodes);
    ok(a.names == names && a.inodes == inodes, b);
}

static void expect_fault(const char *what, uint64_t id, uint64_t stale,
                         uint64_t missing)
{
    invfs_nlink_audit a;
    char b[192];
    size_t i;
    int found = 0;

    if (vol_v3_nlink_audit(g_v, &a) != 0) {
        snprintf(b, sizeof b, "%s: audit completes", what);
        ok(0, b);
        return;
    }
    snprintf(b, sizeof b, "%s: the imbalance is DETECTED", what);
    ok(a.mismatch && a.nfault_total >= 1, b);
    snprintf(b, sizeof b, "%s: %llu stale dirent(s), %llu missing name(s)", what,
             (unsigned long long)a.stale_dirents,
             (unsigned long long)a.missing_names);
    ok(a.stale_dirents == stale && a.missing_names == missing, b);
    for (i = 0; i < a.nfault; i++)
        if (a.fault[i].id == id)
            found = 1;
    snprintf(b, sizeof b, "%s: the message names inode %llu", what,
             (unsigned long long)id);
    ok(found, b);
}

/* The same verdict through the fsck entry point, which is where the
 * operator sees it. */
static void expect_fsck(const char *what, int bad, uint64_t stale)
{
    invfs_fsck_report rep;
    char b[192];
    if (vol_fsck_scan(g_v, &rep, 0) != 0) {
        snprintf(b, sizeof b, "%s: fsck scan completes", what);
        ok(0, b);
        return;
    }
    snprintf(b, sizeof b, "%s: fsck %s the nlink accounting", what,
             bad ? "reports" : "does not report");
    ok(!!rep.nlink_bad == !!bad, b);
    if (bad) {
        snprintf(b, sizeof b, "%s: fsck counts %llu stale dirent(s)", what,
                 (unsigned long long)rep.nlink_stale);
        ok(rep.nlink_stale == stale, b);
        /* damage is not optional: the verdict and the exit code follow it */
        snprintf(b, sizeof b, "%s: fsck marks the volume damaged", what);
        ok(rep.v3_damaged, b);
    }
}

static uint64_t write_file(const char *name, const char *body)
{
    invfs_meta_pub m;
    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode = 0644;
    return vol_v3_write_bulk(g_v, name, (const uint8_t *)body,
                             strlen(body), &m);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    uint64_t id_a, id_b, id_c, id_d, child = 0;
    invfs_v3_inode in;
    int err = 0;

    printf("nlink_v3_test (WP118): nlink vs dirent fan-in on v3\n");

    snprintf(g_img, sizeof g_img, "%s/invf-nlink-v3-test.img", dir);
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
        fprintf(stderr, "nlink_v3_test: vol_open failed: err=%d\n", err);
        return 2;
    }

    /* ---- leg A: three plain files balance ---------------------------- */
    id_a = write_file("a.txt", "alpha content");
    id_b = write_file("b.txt", "beta content");
    id_c = write_file("c.txt", "gamma content");
    ok(id_a && id_b && id_c && id_a != id_b && id_b != id_c,
       "leg A: three files on three inodes");
    expect_balanced("leg A (3 plain files)", 3, 3);
    expect_fsck("leg A", 0, 0);

    /* ---- leg B: hardlinks MUST stay clean ---------------------------- */
    /* This is the whole reason the check is an accounting check. A check
     * of the form "no two names share an inode id" fails right here, on
     * a volume that is perfectly correct: `ln` is supposed to do this. */
    ok(vol_hardlink(g_v, "a.txt", "a-link.txt") == 0, "leg B: ln a.txt a-link.txt");
    ok(vol_hardlink(g_v, "a.txt", "a-link2.txt") == 0, "leg B: ln a.txt a-link2.txt");
    ok(vol_v3_mkdir(g_v, "sub") != 0, "leg B: mkdir sub");
    ok(vol_hardlink(g_v, "a.txt", "sub/a-in-subdir.txt") == 0,
       "leg B: hardlink into a subdirectory");
    ok(vol_v3_inode_get(g_v, id_a, &in) == 1 && in.nlink == 4,
       "leg B: the shared inode's nlink is 4");
    /* 6 files + the "sub" directory = 7 names over 3 inodes: four of those
     * names are the SAME inode, which is exactly what a hardlink is. */
    expect_balanced("leg B (hardlinks: 4 names on one inode)", 7, 3);
    expect_fsck("leg B", 0, 0);

    /* ---- leg C: the accounting survives a remount --------------------- */
    vol_close(g_v);
    g_v = vol_open(g_img, &err);
    if (!g_v) {
        fprintf(stderr, "nlink_v3_test: reopen failed: err=%d\n", err);
        return 2;
    }
    expect_balanced("leg C (after remount)", 7, 3);
    expect_fsck("leg C", 0, 0);

    /* ---- leg D: the aliasing corruption ------------------------------- */
    /* Exactly the WP111b shape: a name lands on an inode that already has
     * one, and the row's nlink is not bumped. On the fixed write path the
     * allocator can no longer hand out a colliding id (that was 1771a0d),
     * so the raw dirent primitive builds the on-disk state the bug left. */
    ok(vol_v3_dirent_get(g_v, INVFS_V3_ROOT_INO, "intruder.txt", &child) == 0,
       "leg D: the intruder name does not exist yet");
    ok(vol_v3_dirent_delta_put(g_v, INVFS_V3_ROOT_INO, "intruder.txt", id_a) == 0,
       "leg D: a second name is put on inode a");
    expect_fault("leg D (stale dirent)", id_a, 1, 0);
    expect_fsck("leg D", 1, 1);
    /* and the two names really are the same inode: that is the data loss */
    {
        uint64_t r1 = 0, r2 = 0;
        ok(vol_v3_path_lookup(g_v, "a.txt", &r1) == 1 &&
           vol_v3_path_lookup(g_v, "intruder.txt", &r2) == 1 && r1 == r2,
           "leg D: both names resolve to one inode (the aliasing)");
    }

    /* ---- leg E: put the accounting right -> clean again -------------- */
    /* The engine's own unlink would decrement nlink for the intruder,
     * leaving 5 names against nlink 3; the volume's real state is "four
     * legitimate links plus one that never should have existed", so the
     * operator resolves it by declaring the intruder a link and then
     * removing it. Either way the invariant must come out balanced. */
    ok(vol_v3_inode_get(g_v, id_a, &in) == 1, "leg E: read the shared row");
    in.nlink = 5;
    /* the delta tier, not the base: on a fresh volume the shared row lives
     * in the delta (that is where vol_hardlink writes it) and the overlay
     * shadows a base write, which is why a base put here would look like it
     * took and then not be seen at all. */
    ok(vol_v3_inode_delta_put(g_v, id_a, &in) == 0, "leg E: declare the 5th link");
    ok(vol_v3_unlink(g_v, "intruder.txt") == 0, "leg E: unlink the intruder");
    expect_balanced("leg E (accounting restored)", 7, 3);
    expect_fsck("leg E", 0, 0);

    /* ---- leg F: the other direction -- a MISSING name ---------------- */
    id_d = write_file("d.txt", "delta content");
    ok(id_d != 0, "leg F: d.txt created");
    expect_balanced("leg F (8 names, 4 inodes)", 8, 4);

    /* F1: the name goes and the row's nlink is never decremented. A row
     * that NO name resolves to is an ORPHAN ROW: the v3 write order is row
     * then dirent, so a crash between the two legitimately produces one and
     * no repair removes it -- it is reported loudly and is deliberately not
     * counted as damage (see vol_v3_nlink_audit). */
    ok(vol_v3_dirent_delta_del(g_v, INVFS_V3_ROOT_INO, "d.txt") == 0,
       "leg F1: the dirent is removed behind nlink's back");
    {
        invfs_nlink_audit a;
        size_t i;
        int found = 0;
        ok(vol_v3_nlink_audit(g_v, &a) == 0, "leg F1: audit completes");
        for (i = 0; i < a.nfault; i++)
            if (a.fault[i].id == id_d &&
                !strcmp(a.fault[i].reason, "orphan-row"))
                found = 1;
        ok(found, "leg F1: the row nobody names is reported, with its id");
        ok(a.orphan_rows == 1, "leg F1: exactly one orphan row");
        ok(!a.mismatch, "leg F1: an orphan row is reported, not damage");
    }
    ok(vol_v3_dirent_delta_put(g_v, INVFS_V3_ROOT_INO, "d.txt", id_d) == 0,
       "leg F1: the name is restored");
    expect_balanced("leg F1 (name restored)", 8, 4);

    /* F2: the fatal direction of the same inequality -- an inode that HAS a
     * name and claims more links than any directory entry accounts for.
     * This is the "fan-in < nlink: a name is missing" case, and unlike the
     * orphan row it is unambiguous: a live, named inode cannot have three
     * links and one name. */
    ok(vol_v3_inode_get(g_v, id_d, &in) == 1, "leg F2: read d.txt's row");
    in.nlink = 3;
    ok(vol_v3_inode_delta_put(g_v, id_d, &in) == 0, "leg F2: nlink inflated to 3");
    expect_fault("leg F2 (missing name)", id_d, 0, 2);
    expect_fsck("leg F2", 1, 0);
    in.nlink = 1;
    ok(vol_v3_inode_delta_put(g_v, id_d, &in) == 0, "leg F2: nlink back to 1");
    expect_balanced("leg F2 (nlink corrected)", 8, 4);
    expect_fsck("leg F2", 0, 0);

    /* ---- leg G: a hardlink whose last name goes frees the row -------- */
    /* The end of a hardlink's life must not leave a dangling count
     * behind: unlinking one name keeps the row for the survivors, and the
     * last unlink takes the row with it. */
    ok(vol_v3_unlink(g_v, "a-link.txt") == 0, "leg G: unlink one hardlink name");
    ok(vol_v3_unlink(g_v, "a-link2.txt") == 0, "leg G: unlink the second");
    ok(vol_v3_inode_get(g_v, id_a, &in) == 1 && in.nlink == 2,
       "leg G: the survivor count is 2 (a.txt + sub/a-in-subdir.txt)");
    expect_balanced("leg G (one hardlink name unlinked)", 6, 4);
    expect_fsck("leg G", 0, 0);

    vol_close(g_v);
    unlink(g_img);
    printf("nlink_v3_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
