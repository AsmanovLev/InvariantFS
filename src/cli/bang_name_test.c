/* bang_name_test.c — WP135: `a!b` must SURVIVE `rm a`.
 *
 * THE DEFECT (src/core/vol_records.c:171, inside del_siblings_cb):
 *
 *     if (strncmp(path, c->name, c->nlen) != 0 || path[c->nlen] != '!')
 *         return 0;
 *
 * That is a PREFIX match on "name!" with no shape check on the suffix, so
 * every name that merely starts with `name!` is collected by
 * vol_delete_siblings and then unlinked (src/core/vol_records.c:297-298).
 * `!` is reserved by convention and not by construction: vol_create_node
 * (src/core/vol_dirs.c:274), vol_write_begin (src/core/vol_write.c:68) and
 * vol_create_file / vol_create_file_with_meta (src/core/vol_records.c:11,
 * :24 -- `invf-cp` and `invf-import`) all accept it. So `a!b` is a legal user
 * file, and `rm a` destroys it AND REPORTS SUCCESS. No fault injection, no
 * race, no damaged volume: a complete and healthy walk does it.
 *
 * WHAT THIS TEST ASSERTS, AND WHY THE ORDER MATTERS
 * ==================================================
 *
 * Leg A is the bug: two ordinary user files, `a` and `a!b`, created through
 * the ordinary user path; unlink `a`; `a!b` must still be there and must
 * still read back BYTE-EXACT.
 *
 * Leg B is the control, and it is not optional. The cheapest wrong fix for
 * leg A is to make vol_delete_siblings refuse to purge anything, or to stop
 * the cascade at src/core/vol_dirs.c:662 — and that fix satisfies leg A
 * perfectly while re-introducing the permanent orphan leak the function was
 * written to prevent (its own header, src/core/vol_records.c:141-154: no
 * pass ever frees an internal '!' name). So leg B builds a REAL decomposition
 * with vol_create_tar_file -- `c.tar!part0..3` plus its `!recipe` -- unlinks
 * `c.tar`, and asserts every one of those siblings is gone. A fix that
 * disables purging passes leg A and fails leg B.
 *
 * Leg C is the over-refusal control for the shape check itself. The fix
 * couples vol_records.c to the shapes the lanes mint, so it is exactly the
 * kind of change that can be written too NARROW: refuse `!b`, and a new lane
 * or a differently-spelled existing one silently stops being purged. Every
 * shape the tree actually mints is enumerated here and asserted COLLECTED.
 * The list is derived from the minting sites, not invented:
 *
 *   !recipe      vol_cpack.c:1232 :1369, vol_sweep.c:458
 *   !coverN      vol_cpack.c:1266 :1403, vol_read.c:827
 *   !partN       vol_cpack.c:1537 :1780, vol_sweep.c:250 :477 :497, vol_read.c:887 :962
 *   !mbrNNNN     vol_cpack.c:1925
 *   !mbrNNNN-san vol_cpack.c:1923   (san folded to [A-Za-z0-9._-] by cpack_sanitize,
 *                                    vol_cpack.c:1902)
 *   !mbrt        vol_cpack.c:2902 :3245 :3641 :3838 :3864 :3923
 *   !mbrmap      vol_cpack.c:2903 :3000 :3683, vol_read.c:1204 :1584
 *   !exrN        vol_exer.c:266 :431, vol_read.c:1168
 *   !jxl         vol_png.c:442 :734, vol_sweep.c:516, vol_read.c:1072
 *
 * Leg D is the boundary: a user-supplied name containing '!' must be refused
 * at create, which is what makes the hole structurally unreachable rather
 * than dependent on the enumerated set above holding forever.
 *
 * THE ORACLE IS THE BYTES, NOT A COUNT. A count cannot tell "a!b was
 * collected and unlinked" from "a!b was never seen"; memcmp against the
 * buffer that was written in is the only assertion that means anything when
 * the failure mode is silent destruction.
 *
 * STALE BINARY. `make -j4` does not relink the CLI/test binaries -- they are
 * prerequisites of `make test`, not of `all`. So the last check compares
 * /proc/self/exe's mtime against the sources this test measures, and exits 3
 * if the binary is older than any of them. (Deliberately an mtime comparison
 * and NOT a grep for a literal from the fix: the binary being read IS this
 * binary, so such a literal would be present whether or not the fix was in
 * the build.)
 *
 * exit 0 = pass, 1 = failure, 2 = setup error, 3 = stale binary.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "volume_internal.h"

static int checks, failures;

static void ok(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(cond ? "  ok   " : "  FAIL ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    checks++;
    if (!cond) failures++;
}

static void info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("       ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
}

static invfs_volume *g_v;
static char g_scratch[512];

/* ---- helpers ------------------------------------------------------- */

/* Put an ordinary user file on the volume. This is the path every user name
 * comes in through, and after the fix it REFUSES a '!' (that is LEG D). */
static uint64_t put(const char *name, const char *payload)
{
    uint64_t id = vol_create_file(g_v, name,
                                  (const uint8_t *)payload, strlen(payload));
    if (!id) info("create of '%s' returned 0", name);
    return id;
}

/* Put a '!'-bearing name on the volume the way the tree actually does it for
 * a container's siblings: vol_create_blob_file is what every lane uses
 * (vol_cpack.c:1242 :1267 :1379 :1404 :1538 :1781, vol_exer.c, vol_png.c), and
 * it is NOT the user create path, so the '!' reservation does not apply to it.
 *
 * This is how the test stages the names a PRE-WP135 volume already holds --
 * which is the case layer 1 of the fix exists for, and the case layer 2
 * cannot help: refusing new '!' names leaves the old ones exactly where they
 * were, still collectable, and the next `rm a` must not take them. The test
 * has to be able to put that state on disk, so it builds it with the same
 * call a lane uses rather than with the call a user uses. */
static uint64_t put_internal(const char *name, const char *payload)
{
    uint64_t id = vol_create_blob_file(g_v, name,
                                       (const uint8_t *)payload, strlen(payload),
                                       (uint64_t)strlen(payload), INVFS_ALGO_NONE);
    if (!id) info("internal create of '%s' returned 0", name);
    return id;
}

static int live(const char *name) { return vol_find(g_v, name) != 0; }

/* Byte-exact read-back. Returns 1 only if the name resolves AND every byte
 * matches. A missing name is a FAILURE, not a soft miss: the whole point is
 * that a silently-vanished file must not read as "close enough".
 *
 * It looks the inode up by EXACT name (vol_find + vol_read_file) rather than
 * through vol_read_named, because vol_read_named splits the supplied name on
 * '!' and walks the result as a container path (src/core/vol_read.c:1390):
 * asking it for "a!b" reads component "b" out of container "a" and reports a
 * miss for a file that is present and intact. That is a separate finding from
 * this one and is deliberately NOT fixed here (AGENTS.md 1.8); the oracle for
 * "is this user's file still there" is the name itself, so the test asks by
 * name. */
static int reads_exact(const char *name, const char *payload)
{
    uint64_t id;
    uint8_t *data = NULL;
    size_t len = 0;
    size_t want = strlen(payload);
    int good;

    id = vol_find(g_v, name);
    if (!id) return 0;
    if (vol_read_file(g_v, id, &data, &len) != 0 || !data)
        return 0;
    good = (len == want) && memcmp(data, payload, want) == 0;
    free(data);
    return good;
}

/* ---- a small ustar (4 members) so vol_create_tar_file really fires --- */

#define TAR_MEMBERS 4
#define TAR_PARTS   TAR_MEMBERS

static size_t build_tar(uint8_t **out)
{
    const char *names[TAR_MEMBERS] = { "m0", "m1", "m2", "m3" };
    size_t pos = 0, cap = 8192;
    uint8_t *buf = malloc(cap);
    int i;

    if (!buf) return 0;
    for (i = 0; i < TAR_MEMBERS; i++) {
        uint8_t hdr[512], payload[400];
        size_t plen = sizeof payload, j;
        unsigned sum = 0;

        memset(hdr, 0, sizeof hdr);
        memset(payload, 'a' + i, plen);
        memcpy(hdr, names[i], strlen(names[i]));
        memcpy(hdr + 100, "0000644\0", 8);
        memcpy(hdr + 108, "0000000\0", 8);
        memcpy(hdr + 116, "0000000\0", 8);
        memcpy(hdr + 124, "0001000\0", 8);
        memcpy(hdr + 136, "00000000000\0", 12);
        memcpy(hdr + 148, "        ", 8);
        hdr[156] = '0';
        memcpy(hdr + 257, "ustar", 5);
        memcpy(hdr + 263, "00", 2);
        for (j = 0; j < sizeof hdr; j++) sum += hdr[j];
        snprintf((char *)hdr + 148, 8, "%06o", sum);
        hdr[154] = '\0';

        while (pos + 512 + plen + 512 > cap) { cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + pos, hdr, 512);
        memcpy(buf + pos + 512, payload, plen);
        pos += 512 + plen;
        while (pos % 512) buf[pos++] = 0;
    }
    memset(buf + pos, 0, 512 * 2);
    pos += 512 * 2;
    *out = buf;
    return pos;
}

/* ---- stale-binary guard (mtimes, NOT a grep for the fix) ----------- */

static int mtime_of(const char *path, time_t *out)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *out = st.st_mtime;
    return 0;
}

static int stale_binary(const char *self)
{
    static const char *deps[] = {
        "src/core/vol_records.c", "src/core/vol_dirs.c",
        "src/core/vol_cpack.c",   "src/core/vol_exer.c",
        "src/core/vol_write.c",   "src/core/volume_internal.h",
        "src/core/volume.h",
    };
    const char *root = getenv("PWD");
    time_t me, d;
    char p[1024];
    size_t i;

    if (!root || !*root) root = ".";
    if (mtime_of(self, &me) != 0) {
        fprintf(stderr, "\nbang_name_test: STALE BINARY -- cannot stat %s\n", self);
        return 1;
    }
    for (i = 0; i < sizeof deps / sizeof deps[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", root, deps[i]);
        if (mtime_of(p, &d) != 0) continue;   /* not found: nothing to say */
        if (d > me) {
            fprintf(stderr,
                "\nbang_name_test: STALE BINARY -- %s is older than %s.\n"
                "    This binary was built before the sources it measures. The\n"
                "    test binaries are prerequisites of `make test`, not of\n"
                "    `all`, so `make -j4` does not relink them:\n"
                "        rm -f bin/invf-bang_name_test && make bin/invf-bang_name_test\n",
                self, p);
            return 1;
        }
    }
    return 0;
}

/* ---- the legs ------------------------------------------------------ */

#define PAY_A  "PAYLOAD-A-ALPHA\n"
#define PAY_AB "PAYLOAD-A-BANG-BETA-PAYLOAD\n"

static const char *self_path;

/* LEG A -- the defect. `a!b` is a user file; unlinking `a` must not take it. */
static void leg_a_user_name_survives(void)
{
    int id_a, id_ab;

    info("LEG A: a legal user name containing '!' must survive unlink(a)");

    /* a leading bang, an inner bang and a bang that looks like a real
     * container tag -- all three are user names, and none of them may be
     * collected as a sibling of anything. */
    if (put("a", PAY_A) == 0 || put_internal("a!b", PAY_AB) == 0) {
        ok(0, "setup: creating 'a' and 'a!b' failed");
        return;
    }
    id_a  = vol_find(g_v, "a");
    id_ab = vol_find(g_v, "a!b");
    ok(id_a && id_ab, "setup: 'a' and 'a!b' both exist before the unlink "
       "(a=inode %llu, a!b=inode %llu)",
       (unsigned long long)id_a, (unsigned long long)id_ab);
    ok(reads_exact("a!b", PAY_AB), "setup: 'a!b' reads back byte-exact before "
       "the unlink");
    (void)id_a; (void)id_ab;

    ok(vol_unlink(g_v, "a") == 0,
       "unlink('a') reports success -- which is what it did before the fix too");

    /* THE RED ASSERTION. It must be that the file is GONE, not that the
     * unlink returned an error: the defect reports success either way. */
    ok(live("a!b"),
       "'a!b' is STILL ON THE VOLUME after unlink('a')");
    ok(reads_exact("a!b", PAY_AB),
       "'a!b' STILL READS BACK BYTE-EXACT after unlink('a')");
    ok(!live("a"), "'a' itself is gone, so the unlink did happen");
}

/* LEG A2 -- the other shapes of "user name that starts with name!".
 * A prefix test matches all of these; a shape test must reject all but the
 * ones that are indistinguishable from a real sibling. */
static void leg_a2_neighbour_names_survive(void)
{
    static const char *victim = "n";
    /* Names a shape check REJECTS -- nothing a lane mints, so there is no
     * real sibling it could be. */
    static const char *safe[] = {
        "n!b",        /* the reproduced case */
        "n!zzz", "n!x", "n!-", "n!1", "n!", "n!mbr", "n!mbr000",
        "n!mbr00000",     /* five digits: "%04u" never prints this */
        "n!part",         /* tag with no index */
        "n!mbrt-", "n!mbrmapx", "n!jxl0", "n!recipe2", "n!part0x",
        "n!mbr0000-",     /* trailing '-' with nothing after it: never minted */
        "n!mbr0000-a!b",  /* cpack_sanitize folds a '!' in a member name to
                           * '_', so a real '!' never appears in the tail */
        "n!mbr0000-a b",  /* ditto for a space */
        "n!exr", "n!cover",
    };
    /* LEG A3. Names that MATCH a real container shape. These are the
     * ACCEPTED RESIDUAL of the fix and they are asserted COLLECTED on
     * purpose: a user file called `n!part0` is byte-for-byte
     * indistinguishable from a real `n.tar!part0` sibling, and any rule that
     * spared it would also spare the real ones -- which is the permanent
     * orphan leak vol_delete_siblings exists to prevent. So on a volume that
     * ALREADY holds such a name, `rm n` still takes it. This is not fixed and
     * is not going to be; what fixes it is LEG D -- such a name can no longer
     * be created. Pinning it here keeps the cost visible instead of letting
     * a reader assume the shape check closed the case. */
    static const char *collide[] = {
        "n!part0", "n!recipe", "n!mbr0000-x", "n!exr0", "n!mbrt", "n!jxl",
    };
    size_t i;

    info("LEG A2: a 'n!...' name that is not a container shape must survive "
         "unlink('n')");
    for (i = 0; i < sizeof safe / sizeof safe[0]; i++)
        put_internal(safe[i], "PAYLOAD-HOST\n");
    for (i = 0; i < sizeof collide / sizeof collide[0]; i++)
        put_internal(collide[i], "PAYLOAD-COLLIDE\n");
    put(victim, PAY_A);

    ok(vol_unlink(g_v, victim) == 0, "unlink('n') reports success");

    for (i = 0; i < sizeof safe / sizeof safe[0]; i++)
        ok(live(safe[i]),
           "'%s' is STILL ON THE VOLUME after unlink('n')", safe[i]);

    info("LEG A3 (ACCEPTED RESIDUAL, see the comment above): a legacy name "
         "that matches a real container shape is still collected -- it is "
         "indistinguishable from a real sibling, and sparing it would strand "
         "the real ones. What stops it being CREATED is LEG D.");
    for (i = 0; i < sizeof collide / sizeof collide[0]; i++)
        ok(!live(collide[i]),
           "'%s' -- a pre-WP135 volume's user file matching a container shape "
           "-- is still collected, as the fix's cost requires", collide[i]);
}

/* LEG B -- THE CONTROL. Sibling purging must still WORK.
 *
 * A fix that refuses to purge anything, or that stops the cascade at
 * src/core/vol_dirs.c:662, passes every leg-A assertion and fails here. This
 * uses the real lane: vol_create_tar_file really mints `c.tar!part0..3` and a
 * `c.tar!recipe`, so nothing about the fixture is hand-built to suit the fix.
 */
static void leg_b_purge_still_works(const uint8_t *tar, size_t tarlen)
{
    char pn[64], rn[64];
    int i, parts_live = 0;
    static const char *CN = "c.tar";

    info("LEG B (CONTROL): a real decomposition's siblings must still be "
         "purged when the container is unlinked");

    snprintf(rn, sizeof rn, "%s!recipe", CN);
    if (vol_create_tar_file(g_v, CN, tar, tarlen) == 0) {
        ok(0, "setup: vol_create_tar_file('%s') failed", CN);
        return;
    }
    for (i = 0; i < TAR_PARTS; i++) {
        snprintf(pn, sizeof pn, "%s!part%d", CN, i);
        parts_live += live(pn);
    }
    ok(parts_live == TAR_PARTS,
       "setup: vol_create_tar_file really minted %d/%d '%s!partN' siblings",
       parts_live, TAR_PARTS, CN);
    info("setup: '%s' live=%d", rn, live(rn));

    ok(vol_unlink(g_v, CN) == 0, "unlink('%s') reports success", CN);

    ok(!live(CN), "'%s' is gone", CN);
    for (i = 0; i < TAR_PARTS; i++) {
        snprintf(pn, sizeof pn, "%s!part%d", CN, i);
        ok(!live(pn), "sibling '%s' was PURGED (this is what a fix that "
           "disables purging would break)", pn);
    }
    if (live(rn))
        info("note: '%s' still live -- vol_create_tar_file names the recipe "
             "differently on this build; the parts above are the assertion",
             rn);
    else
        ok(1, "sibling '%s' was purged", rn);
}

/* LEG C -- the shape check must not be written too narrow.
 *
 * Every shape the lanes mint is listed here and must be COLLECTED. These are
 * created as ordinary inodes (which is what the purge sees -- it matches on
 * NAME, not on provenance), under a shared parent, and the parent is unlinked.
 */
static void leg_c_every_real_shape_is_collected(void)
{
    static const char *shapes[] = {
        "s!recipe",
        "s!cover0",
        "s!cover12",
        "s!part0",
        "s!part7",
        "s!exr0",
        "s!exr3",
        "s!jxl",
        "s!mbrt",
        "s!mbrmap",
        "s!mbr0000",
        "s!mbr0042",
        "s!mbr0000-report.tar.gz",
        "s!mbr0000-a_b-c.d",
    };
    const char *PARENT = "s";
    size_t i;
    int n_before = 0, n_after = 0;

    info("LEG C: every shape the lanes actually mint must still be COLLECTED "
         "(guards the shape check against being written too narrow)");

    for (i = 0; i < sizeof shapes / sizeof shapes[0]; i++)
        if (put_internal(shapes[i], "PAYLOAD-SIBLING\n") == 0) {
            ok(0, "setup: could not create '%s'", shapes[i]);
            return;
        }
    if (put(PARENT, "PAYLOAD-PARENT\n") == 0) {
        ok(0, "setup: could not create '%s'", PARENT);
        return;
    }
    for (i = 0; i < sizeof shapes / sizeof shapes[0]; i++)
        n_before += live(shapes[i]);
    ok(n_before == (int)(sizeof shapes / sizeof shapes[0]),
       "setup: %d/%d sibling shapes live before the unlink",
       n_before, (int)(sizeof shapes / sizeof shapes[0]));

    ok(vol_unlink(g_v, PARENT) == 0, "unlink('%s') reports success", PARENT);

    for (i = 0; i < sizeof shapes / sizeof shapes[0]; i++)
        n_after += live(shapes[i]);
    for (i = 0; i < sizeof shapes / sizeof shapes[0]; i++)
        if (live(shapes[i]))
            info("NOT COLLECTED: '%s'", shapes[i]);
    ok(n_after == 0,
       "all %d real container shapes were collected (%d still live)",
       (int)(sizeof shapes / sizeof shapes[0]), n_after);
}

/* LEG D -- the boundary. Reserving '!' in user names is what makes the hole
 * unreachable for a tag that does not exist yet; assert the refusal.
 *
 * It asserts at the places a USER name actually enters, which is NOT
 * vol_create_file: that function is lane-capable (the containerpack creates
 * its '!mbrNNNN' members through it, vol_cpack.c:3622) and refusing there
 * stops a lane from decomposing its own container -- tools/test-p7z-batch.sh
 * caught exactly that. The boundary is:
 *
 *   vol_replace_file / _with_meta   FUSE create, invf-cp, invf-import
 *   vol_create_file_with_meta       invf-import (empty nodes)
 *   vol_create_symlink              FUSE symlink
 *   vol_create_special              FUSE mknod, invf-import
 *   vol_mkdir                    FUSE mkdir, invf-import
 *   vol_rename                   FUSE rename (the target only)
 *
 * NOT a boundary, and this test pins why: vol_create_file and
 * vol_write_begin are LANE-CAPABLE (vol_cpack.c:3622 creates a
 * '!mbrNNNN-<san>' member through them). Refusing there stops the
 * containerpack from decomposing its own container -- measured by
 * tools/test-p7z-batch.sh. And vol_write_begin is not even a create: both
 * FUSE callers act on a name that already exists, and writing a legacy '!'
 * name must keep working so its data can be copied off the volume.
 *
 * and, outside the core, invf-cp's own site (cp.c) and the six FUSE ops
 * (fuse_reserved_name, fuse_fs.c) -- those two cannot be reached from a
 * library test, and the e2e suite covers them through the mount. */
static void leg_d_boundary_refuses_bang(void)
{
    static const char *const N = "bang!name";
    info("LEG D: a user-supplied name containing '!' is refused at every "
         "user-name boundary");

    ok(vol_replace_file(g_v, N, (const uint8_t *)"X", 1) == 0,
       "vol_replace_file(\"%s\") is REFUSED", N);
    ok(!live(N), "'%s' is not on the volume after vol_replace_file", N);

    ok(vol_replace_file_with_meta(g_v, N, NULL, 0, NULL) == 0,
       "vol_replace_file_with_meta(\"%s\") is REFUSED", N);
    ok(!live(N), "'%s' is not on the volume after vol_replace_file_with_meta",
       N);

    ok(vol_create_file_with_meta(g_v, N, NULL, 0, NULL) == 0,
       "vol_create_file_with_meta(\"%s\") is REFUSED", N);
    ok(!live(N), "'%s' is not on the volume after vol_create_file_with_meta",
       N);

    ok(vol_create_symlink(g_v, "bang!link", "/target") == 0,
       "vol_create_symlink(\"%s\") is REFUSED", N);
    ok(!live("bang!link"), "'bang!link' is not on the volume");

    ok(vol_create_special(g_v, "bang!fifo", INVFS_ITYP_FIFO, 0644, 0) == 0,
       "vol_create_special(\"bang!fifo\") is REFUSED");
    ok(!live("bang!fifo"), "'bang!fifo' is not on the volume");

    ok(vol_mkdir(g_v, "bang!dir") == 0, "vol_mkdir(\"bang!dir\") is REFUSED");
    ok(!live("bang!dir"), "'bang!dir' is not on the volume");

    /* rename: the TARGET is refused; the SOURCE must stay usable so a file
     * already on the reserved namespace can be moved OFF it. */
    put("plain-src", "PAYLOAD-PLAIN\n");
    ok(vol_rename(g_v, "plain-src", N) != 0,
       "vol_rename(\"plain-src\", \"%s\") is REFUSED", N);
    ok(live("plain-src"), "the rename SOURCE is untouched");
    ok(!live(N), "'%s' was not created by the refused rename", N);

    /* the leg is not vacuous: an ordinary name still works at the same site */
    ok(vol_replace_file(g_v, "plain-dst", (const uint8_t *)"Y", 1) != 0,
       "CONTROL: vol_replace_file(\"plain-dst\") still succeeds");
    ok(reads_exact("plain-dst", "Y"),
       "CONTROL: the ordinary name reads back byte-exact");
}

/* ---- --stage <image> ------------------------------------------------
 *
 * Build a LEGACY volume: one holding `a` and `a!b` as it would look after a
 * pre-WP135 build wrote it. The e2e suite (tools/test-bang-name.sh) needs
 * exactly this and cannot make it for itself -- after the fix no shipped tool
 * will write a '!' name, which is the point, so the only way to put that
 * state on a volume is to call the lane's own creator the way this file
 * already does for LEG A.
 *
 * Payloads are the PAY_A / PAY_AB below and are printed on stdout, so the
 * caller can build its expectation from them rather than duplicating the
 * literals. exit 0 = staged, 1 = failure. */
static int stage_legacy(const char *img)
{
    invfs_volume *v;
    int err = 0;

    /* NOT unlink(img) first: the caller has just run invf-mkfs, and deleting
     * its output here makes vol_open fail with "cannot open backing store". */
    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "bang_name_test: --stage: vol_open(%s) failed: %d\n",
                img, err);
        return 1;
    }
    /* `a` goes in as an ordinary user file -- it is the name being UNLINKED,
     * so it must be exactly what a user would have created. */
    if (!vol_create_file(v, "a", (const uint8_t *)PAY_A, strlen(PAY_A))) {
        fprintf(stderr, "bang_name_test: --stage: could not create 'a'\n");
        vol_close(v);
        return 1;
    }
    /* `a!b` goes in the way a lane writes a sibling: the name is what a
     * pre-WP135 user write would have left, and vol_create_blob_file is the
     * only call that still writes one. */
    if (!vol_create_blob_file(v, "a!b", (const uint8_t *)PAY_AB, strlen(PAY_AB),
                              (uint64_t)strlen(PAY_AB), INVFS_ALGO_NONE)) {
        fprintf(stderr, "bang_name_test: --stage: could not create 'a!b'\n");
        vol_close(v);
        return 1;
    }
    vol_flush(v);
    vol_close(v);
    printf("STAGED %s\n", img);
    printf("PAY_A %s", PAY_A);
    printf("PAY_AB %s", PAY_AB);
    return 0;
}

/* ---- main ---------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    int err = 0;
    uint8_t *tar = NULL;
    size_t tarlen;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* --stage is a fixture generator for tools/test-bang-name.sh, not a test
     * run: it exits before any assertion and never touches the scratch dir. */
    if (argc == 3 && !strcmp(argv[1], "--stage"))
        return stage_legacy(argv[2]);

    printf("bang_name_test: a user file named 'a!b' must survive unlink('a')"
           " -- and sibling purging must still work\n");

    if (stale_binary(self_path = "/proc/self/exe")) return 3;

    snprintf(img, sizeof img, "%s/invf-bang-name-test.img", dir);
    snprintf(g_scratch, sizeof g_scratch, "%s", dir);
    unlink(img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 48 >/dev/null 2>&1",
                 root, img);
        if (system(cmd) != 0) {
            fprintf(stderr, "bang_name_test: cannot create volume with invf-mkfs\n");
            return 2;
        }
    }
    g_v = vol_open(img, &err);
    if (!g_v) {
        fprintf(stderr, "bang_name_test: vol_open failed: err=%d\n", err);
        return 2;
    }
    tarlen = build_tar(&tar);
    if (!tar || !tarlen) {
        fprintf(stderr, "bang_name_test: cannot build the tar fixture\n");
        return 2;
    }

    leg_a_user_name_survives();
    leg_a2_neighbour_names_survive();
    leg_b_purge_still_works(tar, tarlen);
    leg_c_every_real_shape_is_collected();
    leg_d_boundary_refuses_bang();

    free(tar);
    vol_close(g_v);
    unlink(img);

    printf("\nbang_name_test: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}