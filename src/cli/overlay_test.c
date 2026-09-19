/* overlay_test.c — WP-M11 e2e driver for the metadata-v3 overlay reads.
 *
 * The delta overlay (delta first, then base; the delta wins) is exercised
 * end to end on a real VOLF_V3 image through the public engine surface:
 *   scenario <img>   write base inode rows and dirents, then append delta
 *                    records through the WP-M10 API and assert that
 *                    vol_v3_inode_get / vol_v3_dirent_get / vol_v3_dirent_scan
 *                    prefer the delta, that a delta delete hides a base
 *                    entry, that a delta update wins, and that the base root
 *                    page is byte-identical (untouched) throughout;
 *   verify   <img>   remount and assert every overlay result survived.
 *
 * This is the offline counterpart of the FUSE lookup/getattr/readdir/read
 * path; it never invents its own on-disk format (the key/value encodings are
 * the frozen WP-M5/M6 encodings the delta reuses).
 *
 * exit 0 = all checks passed, 1 = a check failed, 2 = usage/open error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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
    }
}

/* ---- key / value encodings (frozen WP-M5/M6 forms) -------------------- */

static void ino_key(uint64_t id, uint8_t k[8])
{
    int i;
    for (i = 0; i < 8; i++)
        k[i] = (uint8_t)(id >> (56 - 8 * i));
}

static uint16_t dirent_key(uint8_t *k, uint64_t parent, const char *name)
{
    size_t nlen = name ? strlen(name) : 0;
    int i;
    for (i = 0; i < 8; i++)
        k[i] = (uint8_t)(parent >> (56 - 8 * i));
    k[8] = (uint8_t)(nlen >> 8);
    k[9] = (uint8_t)(nlen & 0xFF);
    if (nlen)
        memcpy(k + 10, name, nlen);
    return (uint16_t)(10 + nlen);
}

static void child_val(uint8_t v[8], uint64_t child)
{
    int i;
    for (i = 0; i < 8; i++)
        v[i] = (uint8_t)(child >> (56 - 8 * i));
}

/* The delta value for an inode key is the frozen invfs_v3_inode_row. */
static uint16_t ino_row(uint8_t *buf, uint16_t mode, uint64_t size)
{
    invfs_v3_inode_row r;
    memset(&r, 0, sizeof r);
    r.row_version = INVFS_V3_INODE_ROW_VERSION;
    r.type = INVFS_ITYP_REG;
    r.mode = mode;
    r.uid = 1000;
    r.gid = 1000;
    r.nlink = 1;
    r.size = size;
    r.mtime = (int64_t)time(NULL);
    r.atime = r.mtime;
    memcpy(buf, &r, sizeof r);
    return (uint16_t)sizeof r;
}

/* ---- engine access ---------------------------------------------------- */

static invfs_volume *g_v;

static int open_v3(const char *img)
{
    int err = 0;
    g_v = vol_open(img, &err);
    if (!g_v) {
        fprintf(stderr, "overlay_test: vol_open(%s) failed: err=%d\n", img, err);
        return -1;
    }
    if (!(vol_sb(g_v)->vol_flags & VOLF_V3)) {
        fprintf(stderr, "overlay_test: %s is not a v3 volume\n", img);
        vol_close(g_v);
        g_v = NULL;
        return -1;
    }
    return 0;
}

static int base_put(uint64_t id, uint16_t mode, uint64_t size)
{
    invfs_v3_inode in;
    memset(&in, 0, sizeof in);
    in.type = INVFS_ITYP_REG;
    in.mode = mode;
    in.uid = in.gid = 1000;
    in.nlink = 1;
    in.size = size;
    in.mtime = in.atime = (int64_t)time(NULL);
    return vol_v3_inode_put(g_v, id, &in);
}

static int base_dirent_put(uint64_t parent, const char *name, uint64_t child)
{
    return vol_v3_dirent_put(g_v, parent, name, child);
}

/* Base-only reads: bypass the overlay so a test can prove the base tier is
 * untouched. Uses the public base root + btree_search. */
static int base_ino_mode(uint64_t id, uint16_t *mode_out)
{
    uint8_t k[8];
    invfs_blkptr root;
    bt_val val;
    invfs_v3_inode_row r;
    int found = 0;

    ino_key(id, k);
    if (vol_v3_base_root(g_v, &root) != 0)
        return -1;
    if (btree_search(g_v, root, (bt_key){k, 8}, &val, &found) != 0)
        return -1;
    if (!found)
        return 0;
    if (val.n < INVFS_V3_INODE_ROW_FIXED)
        return -1;
    memcpy(&r, val.p, sizeof r);
    if (mode_out)
        *mode_out = r.mode;
    return 1;
}

static int base_dirent_get(uint64_t parent, const char *name, uint64_t *child)
{
    uint8_t k[10 + INVFS_MAX_NAME];
    invfs_blkptr root;
    bt_val val;
    uint16_t kn = dirent_key(k, parent, name);
    int found = 0;

    if (vol_v3_base_root(g_v, &root) != 0)
        return -1;
    if (btree_search(g_v, root, (bt_key){k, kn}, &val, &found) != 0)
        return -1;
    if (!found)
        return 0;
    if (val.n < 8)
        return -1;
    {
        uint64_t id = 0;
        int i;
        for (i = 0; i < 8; i++)
            id = (id << 8) | val.p[i];
        if (child)
            *child = id;
    }
    return 1;
}

static int delta_put_inode(uint64_t id, uint16_t mode, uint64_t size)
{
    uint8_t k[8], v[INVFS_V3_INODE_ROW_FIXED];
    uint16_t vl;
    ino_key(id, k);
    vl = ino_row(v, mode, size);
    return vol_delta_append(g_v, k, sizeof k, v, vl, 0);
}

static int delta_del_inode(uint64_t id)
{
    uint8_t k[8];
    ino_key(id, k);
    return vol_delta_append(g_v, k, sizeof k, NULL, 0,
                            INVFS_DELTA_FLAG_DELETE);
}

static int delta_put_dirent(uint64_t parent, const char *name, uint64_t child)
{
    uint8_t k[10 + INVFS_MAX_NAME], v[8];
    uint16_t kn = dirent_key(k, parent, name);
    child_val(v, child);
    return vol_delta_append(g_v, k, kn, v, sizeof v, 0);
}

static int delta_del_dirent(uint64_t parent, const char *name)
{
    uint8_t k[10 + INVFS_MAX_NAME];
    uint16_t kn = dirent_key(k, parent, name);
    return vol_delta_append(g_v, k, kn, NULL, 0, INVFS_DELTA_FLAG_DELETE);
}

/* ---- readdir merge capture ------------------------------------------- */

typedef struct {
    int      n;
    char     name[16][INVFS_MAX_NAME + 1];
    uint64_t child[16];
} dir_capture;

static int dir_cb(void *ctx, const char *name, size_t nlen, uint64_t child)
{
    dir_capture *c = (dir_capture *)ctx;
    if (c->n >= 16)
        return 1;
    if (nlen > INVFS_MAX_NAME)
        return 0;
    memcpy(c->name[c->n], name, nlen);
    c->name[c->n][nlen] = 0;
    c->child[c->n] = child;
    c->n++;
    return 0;
}

static void dir_reset(dir_capture *c)
{
    memset(c, 0, sizeof *c);
}

/* ---- base snapshot (byte-level "base untouched" proof) ---------------- */

typedef struct {
    uint64_t pba, gen;
    uint8_t  page[INVFS_BLOCK_SIZE];
} base_snap;

static int base_snap_take(base_snap *s)
{
    invfs_blkptr root;
    memset(s, 0, sizeof *s);
    if (vol_v3_base_root(g_v, &root) != 0)
        return -1;
    s->pba = root.pba;
    s->gen = root.gen;
    if (root.pba) {
        if (io_seek(&g_v->io, root.pba * (uint64_t)INVFS_BLOCK_SIZE) != 0 ||
            io_read(&g_v->io, s->page, sizeof s->page) != 0)
            return -1;
    }
    return 0;
}

static int base_snap_equal(const base_snap *s)
{
    invfs_blkptr root;
    uint8_t page[INVFS_BLOCK_SIZE];
    if (vol_v3_base_root(g_v, &root) != 0)
        return 0;
    if (root.pba != s->pba || root.gen != s->gen)
        return 0;
    if (root.pba) {
        if (io_seek(&g_v->io, root.pba * (uint64_t)INVFS_BLOCK_SIZE) != 0 ||
            io_read(&g_v->io, page, sizeof page) != 0)
            return 0;
        if (memcmp(page, s->page, sizeof page) != 0)
            return 0;
    }
    return 1;
}

/* ---- scenarios -------------------------------------------------------- */

static const uint64_t PARENT = 77;

static void fix_base(void)
{
    /* all base writes happen before the snapshot, so the delta appends that
     * follow must leave the base byte-identical */
    ok(base_put(42, 0640, 10) == 0, "base put inode 42");
    ok(base_put(43, 0644, 11) == 0, "base put inode 43");
    ok(base_put(44, 0700, 12) == 0, "base put inode 44");
    ok(base_dirent_put(PARENT, "a", 101) == 0, "base dirent a");
    ok(base_dirent_put(PARENT, "b", 102) == 0, "base dirent b");
    ok(base_dirent_put(PARENT, "d", 104) == 0, "base dirent d");
}

static void check_base_only(void)
{
    uint16_t mode = 0;
    uint64_t child = 0;

    ok(base_ino_mode(42, &mode) == 1 && mode == 0640,
       "base read of inode 42 == 0640");
    ok(base_ino_mode(43, &mode) == 1 && mode == 0644,
       "base read of inode 43 == 0644");
    ok(base_ino_mode(44, &mode) == 1 && mode == 0700,
       "base read of inode 44 == 0700");
    ok(base_ino_mode(42, NULL) == 1, "base inode 42 present");
    ok(base_ino_mode(45, NULL) == 0, "base inode 45 absent");
    ok(base_dirent_get(PARENT, "b", &child) == 1 && child == 102,
       "base dirent b == 102");
    ok(base_dirent_get(PARENT, "d", &child) == 1 && child == 104,
       "base dirent d == 104");
}

/* Overlay reads -------------------------------------------------------- */

static void check_inode_overlay(void)
{
    invfs_v3_inode in;
    int rc;

    /* 42: delta value shadows the base row */
    rc = vol_v3_inode_get(g_v, 42, &in);
    ok(rc == 1 && in.mode == 0640, "overlay inode 42 base-only == 0640");
    ok(delta_put_inode(42, 0600, 20) == 0, "delta put inode 42 == 0600");
    rc = vol_v3_inode_get(g_v, 42, &in);
    ok(rc == 1 && in.mode == 0600 && in.size == 20,
       "delta value shadows base (mode 0600 size 20)");
    ok(base_ino_mode(42, NULL) == 1, "base inode 42 still present after delta put");

    /* delta update wins over the older delta value */
    ok(delta_put_inode(42, 0666, 30) == 0, "delta update inode 42 == 0666");
    rc = vol_v3_inode_get(g_v, 42, &in);
    ok(rc == 1 && in.mode == 0666 && in.size == 30,
       "delta update wins (mode 0666 size 30)");

    /* delta-only inode (no base row) */
    ok(delta_put_inode(45, 0644, 5) == 0, "delta-only put inode 45");
    rc = vol_v3_inode_get(g_v, 45, &in);
    ok(rc == 1 && in.mode == 0644, "delta-only inode 45 visible");
    ok(base_ino_mode(45, NULL) == 0, "inode 45 absent from the base");

    /* base-only inode still resolves through the overlay */
    rc = vol_v3_inode_get(g_v, 44, &in);
    ok(rc == 1 && in.mode == 0700, "base-only inode 44 visible");

    /* delta delete shadows a base row */
    ok(delta_del_inode(43) == 0, "delta delete inode 43");
    rc = vol_v3_inode_get(g_v, 43, &in);
    ok(rc == 0, "delta delete hides base inode 43");
    ok(base_ino_mode(43, NULL) == 1, "base inode 43 intact after delta delete");
}

static void check_dirent_overlay(void)
{
    uint64_t child = 0;
    dir_capture cap;
    int rc;

    /* base reads before the delta */
    ok(vol_v3_dirent_get(g_v, PARENT, "b", &child) == 1 && child == 102,
       "overlay dirent b base-only == 102");

    ok(delta_put_dirent(PARENT, "b", 202) == 0, "delta dirent b -> 202");
    ok(delta_put_dirent(PARENT, "c", 103) == 0, "delta dirent c -> 103");
    ok(delta_del_dirent(PARENT, "d") == 0, "delta delete dirent d");

    /* point lookup: delta wins */
    ok(vol_v3_dirent_get(g_v, PARENT, "b", &child) == 1 && child == 202,
       "delta dirent b shadows base (202)");
    ok(vol_v3_dirent_get(g_v, PARENT, "d", &child) == 0,
       "delta delete hides base dirent d");
    ok(vol_v3_dirent_get(g_v, PARENT, "c", &child) == 1 && child == 103,
       "delta-only dirent c visible");
    ok(vol_v3_dirent_get(g_v, PARENT, "a", &child) == 1 && child == 101,
       "base-only dirent a visible");

    /* ordered merge: base a,b,d; delta b->202, c=103, delete d */
    dir_reset(&cap);
    rc = vol_v3_dirent_scan(g_v, PARENT, dir_cb, &cap);
    ok(rc == 0 && cap.n == 3, "readdir merge yields 3 entries");
    if (cap.n == 3) {
        ok(strcmp(cap.name[0], "a") == 0 && cap.child[0] == 101,
           "merge[0] a -> 101 (base)");
        ok(strcmp(cap.name[1], "b") == 0 && cap.child[1] == 202,
           "merge[1] b -> 202 (delta wins)");
        ok(strcmp(cap.name[2], "c") == 0 && cap.child[2] == 103,
           "merge[2] c -> 103 (delta-only)");
    }
    ok(base_dirent_get(PARENT, "b", &child) == 1 && child == 102,
       "base dirent b intact after delta update");
    ok(base_dirent_get(PARENT, "d", &child) == 1 && child == 104,
       "base dirent d intact after delta delete");
}

/* add-before-remove staging: with no fold (WP-M14) this proves the two
 * observable endpoints -- the delta still owning the key, and the base
 * already owning it after the delta dropped it -- both resolve, so a reader
 * sees no transient ENOENT. */
static void check_add_before_remove(void)
{
    invfs_v3_inode in;
    int rc;

    /* delta still owns 42 (fold has not removed it): delta value */
    rc = vol_v3_inode_get(g_v, 42, &in);
    ok(rc == 1 && in.mode == 0666, "pre-remove stage resolves via the delta");

    /* delta no longer owns 44 (fold already published it): base value */
    rc = vol_v3_inode_get(g_v, 44, &in);
    ok(rc == 1 && in.mode == 0700, "post-remove stage resolves via the base");
}

static void scenario(const char *img)
{
    base_snap before;
    if (open_v3(img) != 0)
        exit(2);
    printf("overlay scenario (WP-M11)\n");

    fix_base();
    check_base_only();

    if (base_snap_take(&before) != 0) {
        ok(0, "snapshot the base root");
        vol_close(g_v);
        exit(1);
    }

    /* every mutation below goes through the delta; the base must not move */
    check_inode_overlay();
    check_dirent_overlay();
    check_add_before_remove();

    ok(base_snap_equal(&before),
       "base root page byte-identical after all delta appends");
    if (base_snap_equal(&before))
        printf("base root page byte-identical after all delta appends\n");

    vol_close(g_v);
    g_v = NULL;
    printf("scenario: %d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("SCENARIO FAIL\n");
        exit(1);
    }
    printf("SCENARIO PASS\n");
}

static void verify(const char *img)
{
    invfs_v3_inode in;
    uint64_t child = 0;
    dir_capture cap;

    if (open_v3(img) != 0)
        exit(2);
    printf("overlay verify (WP-M11 remount)\n");

    /* delta rows replay */
    ok(vol_v3_inode_get(g_v, 42, &in) == 1 && in.mode == 0666 && in.size == 30,
       "remount: delta update inode 42 == 0666");
    ok(vol_v3_inode_get(g_v, 45, &in) == 1 && in.mode == 0644,
       "remount: delta-only inode 45 visible");
    ok(vol_v3_inode_get(g_v, 43, &in) == 0,
       "remount: delta delete still hides inode 43");
    ok(vol_v3_inode_get(g_v, 44, &in) == 1 && in.mode == 0700,
       "remount: base-only inode 44 visible");

    /* base rows replay untouched */
    ok(base_ino_mode(42, NULL) == 1, "remount: base inode 42 intact");
    ok(base_ino_mode(43, NULL) == 1, "remount: base inode 43 intact");

    /* the readdir merge replays identically */
    ok(vol_v3_dirent_get(g_v, PARENT, "b", &child) == 1 && child == 202,
       "remount: delta dirent b shadows base (202)");
    ok(vol_v3_dirent_get(g_v, PARENT, "d", &child) == 0,
       "remount: delta delete still hides dirent d");
    dir_reset(&cap);
    ok(vol_v3_dirent_scan(g_v, PARENT, dir_cb, &cap) == 0 && cap.n == 3 &&
       strcmp(cap.name[0], "a") == 0 && cap.child[0] == 101 &&
       strcmp(cap.name[1], "b") == 0 && cap.child[1] == 202 &&
       strcmp(cap.name[2], "c") == 0 && cap.child[2] == 103,
       "remount: readdir merge identical (a,b,c)");

    vol_close(g_v);
    g_v = NULL;
    printf("verify: %d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("VERIFY FAIL\n");
        exit(1);
    }
    printf("VERIFY PASS\n");
}

static void usage(const char *a0)
{
    fprintf(stderr, "usage: %s scenario <img>\n       %s verify <img>\n",
            a0, a0);
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("%s version %s (build %s)\n", argv[0],
                   INVFS_VERSION_STRING, INVFS_BUILD_DATE);
            return 0;
        }
    }
    if (argc == 3 && strcmp(argv[1], "scenario") == 0) {
        scenario(argv[2]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "verify") == 0) {
        verify(argv[2]);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
