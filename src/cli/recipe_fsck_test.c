/* recipe_fsck_test.c — the recipe resolvability invariant, and the fsck
 * check that enforces it.
 *
 * The bug this exists for: a volume that had lost a file was reported OK
 * by `invf-fsck` and CORRUPT by `invf-verify --deep` /
 * `vol_read_inode`. The read path said
 *
 *     vol_read_inode: v3 inode 24: recipe blob missing/corrupt
 *
 * and fsck said OK, exit 0, because nothing in the fsck path ever
 * resolved the content address an inode row carries. A v3 inode row does
 * not hold its content: it holds a 32-byte BLAKE3 address, and the recipe
 * blob lives under its own key (0x04 || addr) in the SAME base tree the
 * page walk already verified. A key that is gone leaves no trace in any
 * page's own CRC, and the address is not a pointer, so no walk can follow
 * it. The page walk checked pages, the nlink audit checked names against
 * rows -- and the file itself was never asked for.
 *
 * Legs:
 *   A  three files, all recipes load      -> clean (the positive control:
 *                                           a check that always fired would
 *                                           pass every "clean" assertion
 *                                           here and be useless)
 *   B  one file's recipe blob lost        -> DETECTED by the audit AND by
 *                                           vol_fsck_scan, which marks the
 *                                           volume damaged (verdict DAMAGED,
 *                                           exit 3) and names the inode
 *   B2 ... and the read path agrees       -> the same inode really is
 *                                           unreadable, and its neighbours
 *                                           are NOT (no collateral)
 *   C  the address restored               -> clean again (a detection, not
 *                                           a sticky verdict)
 *   D  a second file loses its recipe     -> two offenders, BOTH named
 *   E  an empty file and a directory      -> never flagged (no false
 *                                           positives on the two shapes that
 *                                           have no recipe at all)
 *   F  a symlink                          -> never flagged either, and its
 *                                           target still reads back
 *                                           byte-exact. It is content (a
 *                                           real recipe_addr, size = the
 *                                           target length) but it is not an
 *                                           AST, so the audit must not parse
 *                                           it as one. F also pins the
 *                                           other half: the blob must still
 *                                           LOAD, so a symlink whose
 *                                           address is broken is still
 *                                           reported. The fix skips the
 *                                           parse, never the load.
 *
 * What the test does NOT establish: it plants the damage by rewriting the
 * row's address, so it covers "the address does not resolve". It does not
 * cover a blob whose bytes survive but no longer hash to the address --
 * that path exists in vol_v3_recipe_load and is folded into the same
 * finding, but nothing here produces it. It also does not cover a recipe
 * that loads and parses yet names a data segment that is gone; that is a
 * different check (invf-verify --deep reads the bytes) and is out of
 * scope here.
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

static uint64_t write_file(const char *name, const char *body)
{
    invfs_meta_pub m;
    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode = 0644;
    return vol_v3_write_bulk(g_v, name, (const uint8_t *)body,
                             strlen(body), &m);
}

/* Point the row's content address at a blob that does not exist. This is
 * the volume state the fuzz image reached: the row is intact, the tree is
 * intact, every page CRC verifies, and the file cannot be read. */
static int lose_recipe(uint64_t id)
{
    invfs_v3_inode in;
    if (vol_v3_inode_get(g_v, id, &in) != 1)
        return -1;
    in.recipe_addr[0] ^= 0x5Au;
    return vol_v3_inode_delta_put(g_v, id, &in);
}

static int restore_recipe(uint64_t id)
{
    invfs_v3_inode in;
    if (vol_v3_inode_get(g_v, id, &in) != 1)
        return -1;
    in.recipe_addr[0] ^= 0x5Au;
    return vol_v3_inode_delta_put(g_v, id, &in);
}

/* The audit as the check, with the numbers spelled out: a bare "no
 * offenders" would pass just as well on a walk that visited nothing. */
static void expect_clean(const char *what, uint64_t checked)
{
    invfs_recipe_audit a;
    char b[192];
    int rc = vol_v3_recipe_audit(g_v, &a);
    snprintf(b, sizeof b, "%s: audit completes", what);
    ok(rc == 0, b);
    if (rc != 0)
        return;
    snprintf(b, sizeof b, "%s: every live recipe loads (checked=%llu bad=%llu)%s",
             what, (unsigned long long)a.checked,
             (unsigned long long)a.nfault_total,
             a.nfault ? " -- first offender: " : "");
    if (a.nfault)
        printf("          (id=%llu kind=%s size=%llu name=%s)\n",
               (unsigned long long)a.fault[0].id,
               a.fault[0].kind == INVFS_RECIPE_BAD_CORRUPT ? "corrupt"
                                                           : "missing",
               (unsigned long long)a.fault[0].size, a.fault[0].name);
    ok(a.nfault_total == 0, b);
    snprintf(b, sizeof b, "%s: %llu live inode(s) with content were checked",
             what, (unsigned long long)a.checked);
    ok(a.checked == checked, b);

    {
        invfs_fsck_report rep;
        memset(&rep, 0, sizeof rep);
        if (vol_fsck_scan(g_v, &rep, 0) != 0) {
            snprintf(b, sizeof b, "%s: fsck scan completes", what);
            ok(0, b);
            return;
        }
        snprintf(b, sizeof b, "%s: fsck reports no unreadable recipe", what);
        ok(rep.v3_recipe_bad == 0 && !rep.v3_recipe_partial, b);
        snprintf(b, sizeof b, "%s: fsck does not call the volume damaged", what);
        ok(!rep.v3_damaged, b);
    }
}

static void expect_lost(const char *what, uint64_t id, const char *name,
                        uint64_t offenders)
{
    invfs_recipe_audit a;
    invfs_fsck_report rep;
    char b[224];
    size_t i;
    int found = 0, named = 0;

    if (vol_v3_recipe_audit(g_v, &a) != 0) {
        snprintf(b, sizeof b, "%s: audit completes", what);
        ok(0, b);
        return;
    }
    snprintf(b, sizeof b, "%s: the unreadable recipe is DETECTED "
             "(bad=%llu of checked=%llu)", what,
             (unsigned long long)a.nfault_total,
             (unsigned long long)a.checked);
    ok(a.nfault_total == offenders, b);
    for (i = 0; i < a.nfault; i++) {
        if (a.fault[i].id == id) {
            found = 1;
            if (!strcmp(a.fault[i].name, name))
                named = 1;
        }
    }
    snprintf(b, sizeof b, "%s: the message names inode %llu", what,
             (unsigned long long)id);
    ok(found, b);
    snprintf(b, sizeof b, "%s: the message names it as %s", what, name);
    ok(named, b);
    {
        int kind_ok = 1;
        for (i = 0; i < a.nfault; i++)
            if (a.fault[i].id == id &&
                a.fault[i].kind != INVFS_RECIPE_BAD_MISSING)
                kind_ok = 0;
        snprintf(b, sizeof b, "%s: it is classified MISSING, not CORRUPT", what);
        ok(kind_ok, b);
    }

    /* The entry point the operator runs. */
    memset(&rep, 0, sizeof rep);
    if (vol_fsck_scan(g_v, &rep, 0) != 0) {
        snprintf(b, sizeof b, "%s: fsck scan completes", what);
        ok(0, b);
        return;
    }
    snprintf(b, sizeof b, "%s: fsck counts %llu unreadable recipe(s)", what,
             (unsigned long long)offenders);
    ok(rep.v3_recipe_bad == offenders, b);
    /* THE point of the check: the verdict is not OK and the exit code is 3 */
    snprintf(b, sizeof b, "%s: fsck marks the volume DAMAGED", what);
    ok(rep.v3_damaged, b);
    snprintf(b, sizeof b, "%s: fsck still balances the nlink accounting "
             "(the damage is not visible to that check)", what);
    ok(!rep.nlink_bad, b);
}

static int reads_back(uint64_t id)
{
    uint8_t *d = NULL;
    size_t n = 0;
    int rc = vol_read_inode(g_v, id, 0, &d, &n);
    free(d);
    return rc == 0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    uint64_t id_a, id_b, id_c, id_empty, id_link;
    int err = 0;

    printf("recipe_fsck_test: fsck must see a lost recipe blob\n");

    snprintf(g_img, sizeof g_img, "%s/invf-recipe-fsck-test.img", dir);
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
        fprintf(stderr, "recipe_fsck_test: vol_open failed: err=%d\n", err);
        return 2;
    }

    /* ---- leg A: a sound volume --------------------------------------- */
    id_a = write_file("a.txt", "alpha content");
    id_b = write_file("b.txt", "beta content");
    id_c = write_file("c.txt", "gamma content");
    ok(id_a && id_b && id_c && id_a != id_b && id_b != id_c,
       "leg A: three files on three inodes");
    expect_clean("leg A (3 sound files)", 3);
    ok(reads_back(id_a) && reads_back(id_b) && reads_back(id_c),
       "leg A: all three read back");

    /* ---- leg B: one recipe blob is gone ------------------------------ */
    ok(lose_recipe(id_b) == 0, "leg B: b.txt's recipe address is broken");
    expect_lost("leg B (b.txt's recipe is lost)", id_b, "b.txt", 1);

    /* B2: the audit must be tracking the SAME thing the read path sees.
     * Without this the test would only prove the audit agrees with itself. */
    ok(!reads_back(id_b), "leg B2: the read path cannot read b.txt either");
    ok(reads_back(id_a) && reads_back(id_c),
       "leg B2: its neighbours are untouched (no collateral)");

    /* ---- leg C: put the address back -> clean again ------------------ */
    ok(restore_recipe(id_b) == 0, "leg C: b.txt's recipe address is restored");
    expect_clean("leg C (address restored)", 3);
    ok(reads_back(id_b), "leg C: b.txt reads back again");

    /* ---- leg D: two offenders, both named ---------------------------- */
    ok(lose_recipe(id_b) == 0 && lose_recipe(id_c) == 0,
       "leg D: two recipe blobs are lost");
    expect_lost("leg D (b.txt and c.txt both lost)", id_b, "b.txt", 2);
    {
        invfs_recipe_audit a;
        size_t i;
        int found = 0;
        if (vol_v3_recipe_audit(g_v, &a) == 0) {
            for (i = 0; i < a.nfault; i++)
                if (a.fault[i].id == id_c)
                    found = 1;
        }
        ok(found, "leg D: the SECOND offender is named too, not just the first");
    }

    /* ---- leg E: the two shapes that have no recipe at all ------------- */
    ok(restore_recipe(id_b) == 0 && restore_recipe(id_c) == 0,
       "leg E: both addresses are restored");
    id_empty = write_file("empty.txt", "");
    ok(id_empty != 0, "leg E: an empty file is created");
    ok(vol_v3_mkdir(g_v, "sub") != 0, "leg E: mkdir sub");
    /* 3 files with content + 1 empty + 1 directory. Only the three have a
     * recipe, and only those three are counted or checked. */
    expect_clean("leg E (empty file + directory are not false positives)", 3);

    /* ---- leg F: a symlink is content, and it is not an AST recipe ----
     *
     * A v3 symlink row carries size = target length and a REAL, non-zero
     * recipe_addr: the target string is stored content-addressed exactly
     * like a file's recipe (vol_v3_create_node). It is not an AST -- the
     * read path hands the blob back verbatim and never parses it -- but
     * this audit used to walk it into vol_ast_recipe_parse anyway and
     * report the inode as lost content.
     *
     * Why that mattered: a booted Linux root filesystem cannot exist
     * without symlinks (/bin, /sbin, /lib, /lib64, /usr/bin/sh,
     * /usr/lib64/ld-*.so*), so invf-fsck reported DAMAGED, exit 3, on
     * every healthy rootfs image while `invf-verify --deep` on the very
     * same volume said "0 corrupt" and exited 0. A checker stricter than
     * the read path invents damage.
     *
     * One symlink and one regular file is the whole fixture: the smallest
     * volume that reproduces it. */
    id_link = vol_create_symlink(g_v, "link", "usr/lib");
    ok(id_link != 0, "leg F: a symlink is created");
    /* 3 files with content + 1 empty + 1 dir + 1 symlink: all four
     * addressed inodes are checked, none of them is an offender. */
    expect_clean("leg F (a symlink is not lost content)", 4);

    /* The read path's contract, asserted HERE so the audit can never be
     * made permissive by quietly changing what the reader does. Byte
     * comparison, not a length: invf-verify --deep only checks readability
     * and length, because invfs_ast_block_entry carries a pba and no
     * content hash, so it is not an oracle for this. */
    {
        uint8_t *d = NULL;
        size_t n = 0;
        int rc = vol_read_inode(g_v, id_link, 0, &d, &n);
        ok(rc == 0 && n == 7 && d != NULL && memcmp(d, "usr/lib", 7) == 0,
           "leg F: the target reads back byte-exact through vol_read_inode");
        free(d);
    }

    /* And the check the fix must NOT have weakened: a raw blob still has to
     * LOAD. Dropping the address is real damage and is still reported, by
     * name, with the volume marked DAMAGED. Only the AST parse is skipped. */
    ok(lose_recipe(id_link) == 0, "leg F: the symlink's blob address is broken");
    expect_lost("leg F (the symlink's blob really is gone)", id_link, "link", 1);
    ok(restore_recipe(id_link) == 0, "leg F: the symlink's blob is restored");
    expect_clean("leg F (restored)", 4);

    vol_close(g_v);
    unlink(g_img);
    printf("recipe_fsck_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
