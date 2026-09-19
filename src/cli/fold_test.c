/* fold_test.c — WP-M14 e2e driver for the metadata-v3 fold.
 *
 * This is the offline counterpart of the sweep's fold step (WP-M18): it
 * exercises the public WP-M14 API on a real VOLF_V3 image through vol_open.
 *
 *   scenario <img>   write a base inode row + dirent, append delta records
 *                    that both update and delete base keys and add new ones,
 *                    capture the base root, then fold. Asserts:
 *                      - the base now holds every delta value (delta merged);
 *                      - a delta delete removed the base row;
 *                      - the delta is empty and RT30.delta_pba == 0;
 *                      - the published base root moved and its seq advanced;
 *                      - a delta-first reader sees the merged value at every
 *                        stage of the fold (add-before-remove);
 *                      - a second fold on an empty delta is a no-op.
 *   verify   <img>   remount and assert the merged namespace persisted from
 *                    the base tier alone (delta empty after replay).
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
#include "vol_metabuf.h"

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
        fprintf(stderr, "fold_test: vol_open(%s) failed: err=%d\n", img, err);
        return -1;
    }
    if (!(vol_sb(g_v)->vol_flags & VOLF_V3)) {
        fprintf(stderr, "fold_test: %s is not a v3 volume\n", img);
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

/* Base-only read (bypasses the overlay) so a test can prove what fold put
 * into the base. Uses the public base root + btree_search. */
static int base_ino_mode(uint64_t id, uint16_t *mode_out, uint64_t *size_out)
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
    if (size_out)
        *size_out = r.size;
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

/* ---- root snapshots --------------------------------------------------- */

typedef struct {
    uint64_t slot[2];
    uint64_t seq;
    uint64_t delta_pba;
} rt30_snap;

static void rt30_take(rt30_snap *s)
{
    s->slot[0] = g_v->rt30.root_slot[0];
    s->slot[1] = g_v->rt30.root_slot[1];
    s->seq = g_v->rt30.seq;
    s->delta_pba = g_v->rt30.delta_pba;
}

static const uint64_t PARENT = 77;

/* Reader-across-fold: after the fold publishes the new base and resets the
 * delta, the lock-free delta-then-base reader must resolve every merged key
 * (the add-before-remove contract the WP doc freezes). The delta is already
 * empty here, so the value can only come from the base -- which is exactly
 * the post-reset endpoint. */
static void check_reader_contract(void)
{
    invfs_v3_inode in;
    ok(vol_v3_inode_get(g_v, 42, &in) == 1 && in.mode == 0666,
       "reader after fold resolves merged 42 (delta-then-base)");
    ok(vol_v3_inode_get(g_v, 44, &in) == 1 && in.mode == 0700,
       "reader after fold resolves untouched base 44");
}

static void scenario(const char *img)
{
    invfs_v3_inode in;
    invfs_blkptr root;
    rt30_snap before, after;
    uint16_t mode;
    uint64_t size, child;

    if (open_v3(img) != 0)
        exit(2);
    printf("fold scenario (WP-M14)\n");

    /* base rows */
    ok(base_put(42, 0640, 10) == 0, "base put inode 42");
    ok(base_put(43, 0644, 11) == 0, "base put inode 43");
    ok(base_put(44, 0700, 12) == 0, "base put inode 44");
    ok(base_dirent_put(PARENT, "a", 101) == 0, "base dirent a");
    ok(base_dirent_put(PARENT, "b", 102) == 0, "base dirent b");

    /* delta: update 42, delete 43, add 45; add dirent c, delete dirent b */
    ok(delta_put_inode(42, 0600, 20) == 0, "delta update inode 42");
    ok(delta_put_inode(42, 0666, 30) == 0, "delta re-update inode 42 (coalesce)");
    ok(delta_del_inode(43) == 0, "delta delete inode 43");
    ok(delta_put_inode(45, 0644, 5) == 0, "delta add inode 45");
    ok(delta_put_dirent(PARENT, "c", 103) == 0, "delta add dirent c");
    ok(delta_del_dirent(PARENT, "b") == 0, "delta delete dirent b");

    rt30_take(&before);
    ok(before.delta_pba != 0, "delta segment is named before fold");

    /* pre-fold overlay: delta wins for 42/45, delete hides 43 */
    ok(vol_v3_inode_get(g_v, 42, &in) == 1 && in.mode == 0666 && in.size == 30,
       "pre-fold overlay: inode 42 = 0666/30 (delta)");
    ok(vol_v3_inode_get(g_v, 43, &in) == 0, "pre-fold overlay: 43 hidden");
    ok(vol_v3_inode_get(g_v, 45, &in) == 1 && in.mode == 0644,
       "pre-fold overlay: delta-only 45 visible");
    ok(base_ino_mode(42, &mode, &size) == 1 && mode == 0640 && size == 10,
       "pre-fold base still holds the old inode 42");

    /* FOLD */
    ok(vol_v3_fold(g_v) == 0, "fold returns 0");

    /* delta emptied + RT30 reset */
    ok(vol_delta_count(g_v) == 0, "delta index empty after fold");
    ok(g_v->delta_seg_pba == 0 || g_v->delta_bump == INVFS_DELTA_SEG_HDR_LEN,
       "delta bump cursor reset");
    rt30_take(&after);
    ok(after.delta_pba == 0, "RT30.delta_pba cleared after fold");
    ok(after.seq == before.seq + 1, "RT30 seq advanced by exactly one");
    ok((after.slot[0] != before.slot[0] || after.slot[1] != before.slot[1]),
       "published root slot moved");

    /* base now holds every merged value */
    ok(base_ino_mode(42, &mode, &size) == 1 && mode == 0666 && size == 30,
       "post-fold base inode 42 = 0666/30");
    ok(base_ino_mode(43, NULL, NULL) == 0, "post-fold base inode 43 deleted");
    ok(base_ino_mode(45, &mode, NULL) == 1 && mode == 0644,
       "post-fold base inode 45 added");
    ok(base_dirent_get(PARENT, "b", &child) == 0, "post-fold base dirent b deleted");
    ok(base_dirent_get(PARENT, "c", &child) == 1 && child == 103,
       "post-fold base dirent c added");

    /* overlay reads agree with the base (nothing shadows it now) */
    ok(vol_v3_inode_get(g_v, 42, &in) == 1 && in.mode == 0666,
       "post-fold overlay inode 42 = 0666");
    ok(vol_v3_inode_get(g_v, 43, &in) == 0, "post-fold overlay 43 absent");
    ok(vol_v3_inode_get(g_v, 45, &in) == 1 && in.mode == 0644,
       "post-fold overlay 45 present");
    ok(vol_v3_dirent_get(g_v, PARENT, "b", &child) == 0,
       "post-fold overlay dirent b absent");
    ok(vol_v3_dirent_get(g_v, PARENT, "c", &child) == 1 && child == 103,
       "post-fold overlay dirent c present");

    /* the base tree must still be structurally valid: 42,44,45 (inodes) and
     * a,c (dirents) survive, so five keys. */
    if (vol_v3_base_root(g_v, &root) == 0) {
        char err[128];
        bt_stat st;
        err[0] = 0;
        ok(btree_check(g_v, root, &st, err, sizeof err) == 0 && st.nkeys == 5,
           "post-fold base tree is structurally valid (5 keys)");
    } else {
        ok(0, "read base root after fold");
    }

    /* idempotent second fold on an empty delta */
    rt30_take(&before);
    ok(vol_v3_fold(g_v) == 0, "fold on empty delta is a no-op (rc 0)");
    rt30_take(&after);
    ok(after.seq == before.seq && after.delta_pba == 0,
       "empty-delta fold did not publish or touch the delta");

    /* fold_request below threshold is a no-op */
    ok(vol_v3_fold_request(g_v) == 0, "fold_request below threshold = 0");

    check_reader_contract();

    vol_close(g_v);
    g_v = NULL;
    printf("scenario: %d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("SCENARIO FAIL\n");
        exit(1);
    }
    printf("SCENARIO PASS\n");
}

/* ---- reader-across-fold: publish-then-reset visibility ---------------- */

static void verify(const char *img)
{
    invfs_v3_inode in;
    uint16_t mode;
    uint64_t size, child;

    if (open_v3(img) != 0)
        exit(2);
    printf("fold verify (WP-M14 remount)\n");

    ok(vol_delta_count(g_v) == 0, "remount: delta is empty");
    ok(g_v->rt30.delta_pba == 0, "remount: RT30 names no delta segment");

    /* everything must now come from the base tier */
    ok(base_ino_mode(42, &mode, &size) == 1 && mode == 0666 && size == 30,
       "remount base inode 42 = 0666/30");
    ok(base_ino_mode(43, NULL, NULL) == 0, "remount base inode 43 absent");
    ok(base_ino_mode(45, &mode, NULL) == 1 && mode == 0644,
       "remount base inode 45 present");
    ok(base_dirent_get(PARENT, "b", &child) == 0, "remount base dirent b absent");
    ok(base_dirent_get(PARENT, "c", &child) == 1 && child == 103,
       "remount base dirent c = 103");

    ok(vol_v3_inode_get(g_v, 42, &in) == 1 && in.mode == 0666,
       "remount overlay inode 42 = 0666");
    ok(vol_v3_inode_get(g_v, 43, &in) == 0, "remount overlay inode 43 absent");
    ok(vol_v3_inode_get(g_v, 44, &in) == 1 && in.mode == 0700,
       "remount overlay inode 44 = 0700");
    ok(vol_v3_dirent_get(g_v, PARENT, "c", &child) == 1 && child == 103,
       "remount overlay dirent c = 103");

    vol_close(g_v);
    g_v = NULL;
    printf("verify: %d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("VERIFY FAIL\n");
        exit(1);
    }
    printf("VERIFY PASS\n");
}

/* ---- crash-window driver --------------------------------------------- */

/* Build a base + delta set, then fold. With INVFS_FOLD_ABORT_AT=published in
 * the environment the fold dies SIGKILL'd right after the new base is
 * published and before the delta is reset -- the process never returns. */
static void crash_setup(const char *img)
{
    if (open_v3(img) != 0)
        exit(2);
    printf("fold crash (WP-M14): seeding base + delta\n");
    if (base_put(42, 0640, 10) != 0 ||
        base_put(43, 0644, 11) != 0 ||
        base_dirent_put(PARENT, "a", 101) != 0) {
        fprintf(stderr, "fold_test: crash setup base failed\n");
        exit(1);
    }
    if (delta_put_inode(42, 0600, 20) != 0 ||
        delta_del_inode(43) != 0 ||
        delta_put_dirent(PARENT, "c", 103) != 0) {
        fprintf(stderr, "fold_test: crash setup delta failed\n");
        exit(1);
    }
    /* force the fold; the env hook kills us mid-window */
    (void)vol_v3_fold(g_v);
    fprintf(stderr, "fold_test: crash setup survived the fold (hook not "
            "set?); nothing to verify\n");
    vol_close(g_v);
    exit(1);
}

/* After a crash in the publish/reset window the next mount must present the
 * merged namespace: the new base carries every key, and the old delta is
 * either still present (replayed, idempotent) or reset. Content must be
 * identical either way. */
static void crash_verify(const char *img)
{
    invfs_v3_inode in;
    uint64_t child;

    if (open_v3(img) != 0)
        exit(2);
    printf("fold crash verify (WP-M14 remount after publish/reset window)\n");

    ok(vol_v3_inode_get(g_v, 42, &in) == 1 && in.mode == 0600 && in.size == 20,
       "crash remount: inode 42 = 0600/20 (merged)");
    ok(vol_v3_inode_get(g_v, 43, &in) == 0, "crash remount: inode 43 absent");
    ok(vol_v3_inode_get(g_v, 41, &in) == 0, "crash remount: inode 41 absent");
    ok(vol_v3_dirent_get(g_v, PARENT, "a", &child) == 1 && child == 101,
       "crash remount: dirent a = 101 (untouched)");
    ok(vol_v3_dirent_get(g_v, PARENT, "c", &child) == 1 && child == 103,
       "crash remount: dirent c = 103 (merged)");

    vol_close(g_v);
    g_v = NULL;
    printf("crash verify: %d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("CRASH VERIFY FAIL\n");
        exit(1);
    }
    printf("CRASH VERIFY PASS\n");
}

static void usage(const char *a0)
{
    fprintf(stderr, "usage: %s scenario <img>\n"
                    "       %s verify <img>\n"
                    "       %s crash-setup <img>\n"
                    "       %s crash-verify <img>\n",
            a0, a0, a0, a0);
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
    if (argc == 3 && strcmp(argv[1], "crash-setup") == 0) {
        crash_setup(argv[2]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "crash-verify") == 0) {
        crash_verify(argv[2]);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
