/* sweep_bang_test.c -- WP136: a '!'-bearing USER name is excluded from every
 * sweep transform lane, so it silently loses batching (and with it the
 * batch's dedup) while the byte-identical file beside it gets the full
 * treatment.
 *
 * The observable here is the STATE, not the log. A sweep that skips a lane
 * says nothing -- it exits 0 and prints no complaint -- so a test that
 * asserts an exit code proves nothing about this defect. What proves it is
 * two volumes holding the same bytes under names that differ only by a '!',
 * swept the same way: the plain names reach a shared PPMd text batch, the
 * '!' names do not, and the divergence is the whole finding.
 *
 * STAGING. A '!' name cannot be created through any shipped user path --
 * WP135 refuses it at the name-introduction funnels, which is correct and is
 * not what this test is about. The tree already has the way round it:
 * vol_write_bulk is the underlying write call that vol_create_file and
 * vol_replace_file wrap with their own refusal, and it is what the tree
 * itself uses to put a name on a volume. That is what a PRE-WP135 build
 * left behind, so it is exactly the state this defect is about.
 *
 * The read-back oracle is vol_find + vol_read_inode, by EXACT name --
 * never vol_read_named, which splits the supplied name on '!' and walks it
 * as a container path (src/core/vol_read.c:1390) and would therefore report
 * a miss for a file that is present and intact. That is a separate finding
 * (reported, not fixed) and using it here would make this test lie.
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

static int checks = 0, failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", what); }
    else        { printf("  OK    %s\n", what); }
}

#define NCOPY   3            /* copies per volume -- enough to show sharing */
#define BUFSZ   (48 * 1024)

static uint8_t g_buf[BUFSZ];

/* Same bytes, every time: the ONLY difference between the two volumes below
 * is which characters are in the names. */
static void fill_buf(void)
{
    size_t i;
    for (i = 0; i < BUFSZ; i++)
        g_buf[i] = (uint8_t)("THE_QUICK_BROWN_FOX_JUMPS_OVER_THE_LAZY_DOG_"
                             "and_then_some_more_repeating_text_here\n"[i % 62]);
}

static invfs_volume *fresh(const char *dir, const char *tag)
{
    char img[512], cmd[1024];
    invfs_volume *v;
    int err = 0;
    snprintf(img, sizeof img, "%s/sweep-bang-%s.img", dir, tag);
    unlink(img);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 >/dev/null 2>&1",
             getenv("PWD") ? getenv("PWD") : ".", img);
    if (system(cmd) != 0) { printf("  cannot mkfs %s\n", img); return NULL; }
    v = vol_open(img, &err);
    if (!v) printf("  vol_open(%s) failed err=%d\n", img, err);
    return v;
}

static int reads_exact(invfs_volume *v, const char *name)
{
    uint64_t id = vol_find(v, name);
    uint8_t *d = NULL; size_t n = 0; int good;
    if (!id) return 0;
    if (vol_read_inode(v, id, 0, &d, &n) != 0 || !d) return 0;
    good = (n == BUFSZ) && memcmp(d, g_buf, BUFSZ) == 0;
    free(d);
    return good;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    invfs_volume *va, *vb;
    int i, all_text_a = 1, all_text_b = 1, all_exact = 1;
    const char *names_a[NCOPY] = { "notes0.txt", "notes1.txt", "notes2.txt" };
    const char *names_b[NCOPY] = { "no!tes0.txt", "no!tes1.txt", "no!tes2.txt" };

    printf("sweep_bang_test (WP136): a '!'-named file is excluded from the "
           "sweep's transform lanes\n");
    fill_buf();

    /* ---------- LEG A/B: the state divergence ------------------------ */
    va = fresh(dir, "a");
    vb = fresh(dir, "b");
    if (!va || !vb) { if (va) vol_close(va); if (vb) vol_close(vb); return 2; }

    for (i = 0; i < NCOPY; i++) {
        ok(vol_write_bulk(va, names_a[i], g_buf, BUFSZ, NULL) != 0,
           "plain name staged on volume A");
        ok(vol_write_bulk(vb, names_b[i], g_buf, BUFSZ, NULL) != 0,
           "'!'-bearing name staged on volume B");
    }
    for (i = 0; i < NCOPY; i++) {
        ok(vol_inode_first_zone(va, vol_find(va, names_a[i])) == INVFS_ZONE_RAW,
           "plain name starts in RAW");
        ok(vol_inode_first_zone(vb, vol_find(vb, names_b[i])) == INVFS_ZONE_RAW,
           "'!'-bearing name starts in RAW");
    }
    printf("  -- sweeping both volumes the same way --\n");
    for (i = 0; i < NCOPY; i++) {
        vol_mark_pending(va, vol_find(va, names_a[i]));
        vol_mark_pending(vb, vol_find(vb, names_b[i]));
    }
    vol_sweep_pending(va);
    vol_sweep_pending(vb);

    printf("  -- the divergence (RAW=%d TEXT=%d BINARY=%d) --\n",
           INVFS_ZONE_RAW, INVFS_ZONE_TEXT, INVFS_ZONE_BINARY);

    for (i = 0; i < NCOPY; i++) {
        int za = vol_inode_first_zone(va, vol_find(va, names_a[i]));
        int zb = vol_inode_first_zone(vb, vol_find(vb, names_b[i]));
        if (za != INVFS_ZONE_TEXT) all_text_a = 0;
        if (zb != INVFS_ZONE_TEXT) all_text_b = 0;
        if (!reads_exact(va, names_a[i]) || !reads_exact(vb, names_b[i]))
            all_exact = 0;
        printf("    %-12s z=%d      |  %-12s z=%d\n",
               names_a[i], za, names_b[i], zb);
    }

    /* THE CONTROL, in two halves. The first proves the harness is live --
     * a sweep on this tree DOES batch a plain text name, so the second
     * failing is the defect and not a broken fixture. The second is the red
     * control proper: byte-identical content, names differing by one
     * character, swept the same way, and the '!' name does not get the
     * shared batch. Nothing in the sweep reports this -- no warning, no log
     * line, exit 0 -- so asserting the STATE is the only way to see it. */
    ok(all_text_a,
       "CONTROL (harness live): every PLAIN name was batched into a shared "
       "PPMd text segment");
    ok(all_text_b,
       "RED CONTROL: every '!'-bearing name was batched too -- same bytes, "
       "one character of name apart");

    /* LEG C: bit-exactness is not what is being traded here. Whatever lane a
     * name takes, every byte must come back. */
    ok(all_exact,
       "every file reads back byte-exact after the sweep, on both volumes");

    vol_close(va);
    vol_close(vb);

    /* ---------- LEG E: the narrowing must not unprotect a real sibling --
     *
     * A name whose suffix IS a shape a lane mints, and whose prefix names a
     * live container, is genuinely one of ours: it must stay out of the
     * batching lanes (WP10 12.7 -- a member belongs to its container's
     * batch, not to a whole-file lane). This is the assertion that fails
     * first if the fix is "just drop the '!' test". */
    {
        invfs_volume *v = fresh(dir, "e");
        uint64_t box, part;
        int zbox, zpart;
        if (!v) return 2;
        box = vol_write_bulk(v, "box.txt", g_buf, BUFSZ, NULL);
        part = vol_write_bulk(v, "box.txt!part0", g_buf, BUFSZ, NULL);
        ok(box != 0 && part != 0, "container + a real '!part0' sibling staged");
        vol_mark_pending(v, box);
        vol_mark_pending(v, part);
        vol_sweep_pending(v);
        zbox = vol_inode_first_zone(v, box);
        zpart = vol_inode_first_zone(v, part);
        printf("  box.txt z=%d   box.txt!part0 z=%d\n", zbox, zpart);
        ok(zbox == INVFS_ZONE_TEXT,
           "the container itself was batched");
        ok(zpart != INVFS_ZONE_TEXT,
           "a REAL container sibling was NOT batched (WP10 12.7 intact)");
        ok(reads_exact(v, "box.txt") && reads_exact(v, "box.txt!part0"),
           "container and sibling both read back byte-exact");

        /* LEG F: the other half of the predicate. A name that MATCHES a
         * minted shape but has no live container is not one of ours, and
         * must be treated as the user name it is. */
        {
            uint64_t orphan = vol_write_bulk(v, "ghost.txt!mbr0000-x",
                                                g_buf, BUFSZ, NULL);
            int zo = 0;
            ok(orphan != 0, "a '!mbr0000-x' name with NO live container staged");
            vol_mark_pending(v, orphan);
            vol_sweep_pending(v);
            zo = vol_inode_first_zone(v, orphan);
            printf("  ghost.txt!mbr0000-x z=%d\n", zo);
            ok(zo == INVFS_ZONE_TEXT,
               "a shape-matching name with no live container was batched too");
            ok(reads_exact(v, "ghost.txt!mbr0000-x"),
               "and reads back byte-exact");
        }
        vol_close(v);
    }

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
