/* vnode.c — WP-M5 e2e driver for the metadata-v3 inode tree.
 *
 * Until WP-M6 (dirents) lands there is no name -> inode resolution on a v3
 * volume, so the inode tree is exercised directly by id. This tool is the
 * offline counterpart of a FUSE create/stat: it opens the volume (a mount
 * at the library level), mutates/reads one inode row, and closes cleanly.
 *
 * usage:
 *   invf-vnode <img> put <id> <type> <mode-octal> <uid> <gid> <size>
 *   invf-vnode <img> get <id>
 *   invf-vnode <img> del <id>
 *   invf-vnode <img> dirent get <parent-id> <name>
 *   invf-vnode <img> dirent put <parent-id> <name> <child-id>
 *   invf-vnode <img> dirent del <parent-id> <name>
 *   invf-vnode <img> nlink put <id> <nlink>
 *   invf-vnode <img> nlink audit
 *
 * get prints one line and exits 0 when the row is present, 1 when absent,
 * 2 on a usage/open error. `nlink audit` exits 3 on a fan-in mismatch, like
 * invf-fsck does. */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "volume.h"
#include "invarifs.h"

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s <img> put <id> <type> <mode-octal> <uid> <gid> <size>\n"
            "       %s <img> get <id>\n"
            "       %s <img> del <id>\n"
            "       %s <img> dirent get <parent-id> <name>\n"
            "       %s <img> dirent put <parent-id> <name> <child-id>\n"
            "       %s <img> dirent del <parent-id> <name>\n"
            "       %s <img> nlink put <id> <nlink>\n"
            "       %s <img> nlink audit\n",
            argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static int open_vol(const char *path, invfs_volume **out);

/* WP118: the raw dirent surface (WP-M6's primitive, which the FUSE link
 * path drives through vol_hardlink). Exposed here so an e2e leg can build
 * the exact on-disk shape of the WP111b aliasing -- a second NAME on an
 * inode whose nlink was never bumped -- which the fixed write path can no
 * longer produce on its own (that is what 1771a0d fixed). `dirent put`
 * deliberately does NOT touch nlink: doing the accounting is the caller's
 * job, and the failure being reproduced is precisely a caller that forgot. */
static int cmd_dirent(const char *img, int argc, char **argv)
{
    invfs_volume *v;
    const char *op;
    uint64_t parent, child = 0, got = 0;
    int rc;

    if (argc < 6) { usage(argv[0]); return 2; }
    op = argv[3];
    parent = strtoull(argv[4], NULL, 0);
    if (open_vol(img, &v) != 0)
        return 2;
    if (!strcmp(op, "put")) {
        if (argc != 7) { usage(argv[0]); vol_close(v); return 2; }
        child = strtoull(argv[6], NULL, 0);
        /* the DELTA tier, like every live namespace write: a base put on a
         * volume whose entries have not been folded yet would be shadowed
         * by the delta and read back as "not there" */
        if (vol_dirent_delta_put(v, parent, argv[5], child) != 0) {
            fprintf(stderr, "vol_dirent_delta_put(%llu, %s, %llu) failed\n",
                    (unsigned long long)parent, argv[5],
                    (unsigned long long)child);
            vol_close(v);
            return 2;
        }
        printf("dirent put parent=%llu name=%s child=%llu\n",
               (unsigned long long)parent, argv[5],
               (unsigned long long)child);
        vol_close(v);
        return 0;
    }
    if (!strcmp(op, "del")) {
        if (argc != 6) { usage(argv[0]); vol_close(v); return 2; }
        if (vol_dirent_delta_del(v, parent, argv[5]) != 0) {
            fprintf(stderr, "vol_dirent_delta_del(%llu, %s) failed\n",
                    (unsigned long long)parent, argv[5]);
            vol_close(v);
            return 2;
        }
        printf("dirent del parent=%llu name=%s\n",
               (unsigned long long)parent, argv[5]);
        vol_close(v);
        return 0;
    }
    if (strcmp(op, "get") != 0) { usage(argv[0]); vol_close(v); return 2; }
    if (argc != 6) { usage(argv[0]); vol_close(v); return 2; }
    rc = vol_dirent_get(v, parent, argv[5], &got);
    if (rc != 1) {
        printf("absent parent=%llu name=%s (rc=%d)\n",
               (unsigned long long)parent, argv[5], rc);
        vol_close(v);
        return 1;
    }
    printf("found parent=%llu name=%s child=%llu\n",
           (unsigned long long)parent, argv[5], (unsigned long long)got);
    vol_close(v);
    return 0;
}

/* WP118: the nlink field on its own, and the fan-in audit over the whole
 * namespace. `nlink put` is the second half of the corruption injector (the
 * "missing name" direction: a row that claims more links than any dirent
 * accounts for); `nlink audit` prints the same accounting invf-fsck and
 * invf-verify --deep run, so a leg can assert on it directly. */
static int cmd_nlink(const char *img, int argc, char **argv)
{
    invfs_volume *v;
    const char *op;
    invfs_inode in;
    invfs_nlink_audit a;
    uint64_t id, i;
    int rc;

    if (argc < 4) { usage(argv[0]); return 2; }
    op = argv[3];
    if (open_vol(img, &v) != 0)
        return 2;
    if (!strcmp(op, "put")) {
        if (argc != 6) { usage(argv[0]); vol_close(v); return 2; }
        id = strtoull(argv[4], NULL, 0);
        if (vol_inode_get(v, id, &in) != 1) {
            fprintf(stderr, "inode %llu is absent\n", (unsigned long long)id);
            vol_close(v);
            return 1;
        }
        in.nlink = (uint32_t)strtoul(argv[5], NULL, 0);
        /* delta tier, like vol_v3_hardlink: a base put is shadowed by the
         * delta's copy of the same row */
        if (vol_inode_delta_put(v, id, &in) != 0) {
            fprintf(stderr, "vol_inode_delta_put(%llu) failed\n",
                    (unsigned long long)id);
            vol_close(v);
            return 2;
        }
        printf("nlink put id=%llu nlink=%u\n", (unsigned long long)id,
               in.nlink);
        vol_close(v);
        return 0;
    }
    if (strcmp(op, "audit") != 0) { usage(argv[0]); vol_close(v); return 2; }
    if (argc != 4) { usage(argv[0]); vol_close(v); return 2; }
    rc = vol_nlink_audit(v, &a);
    if (rc != 0) {
        fprintf(stderr, "vol_nlink_audit failed (rc=%d)\n", rc);
        vol_close(v);
        return 2;
    }
    printf("names=%llu inodes=%llu missing=%llu stale=%llu dead=%llu "
           "faults=%llu verdict=%s\n",
           (unsigned long long)a.names, (unsigned long long)a.inodes,
           (unsigned long long)a.missing_names,
           (unsigned long long)a.stale_dirents,
           (unsigned long long)a.dead_names,
           (unsigned long long)a.nfault_total,
           a.mismatch ? "MISMATCH" : "OK");
    for (i = 0; i < a.nfault; i++)
        printf("  fault %s: id=%llu nlink=%u fanin=%u name=%s\n",
               a.fault[i].reason, (unsigned long long)a.fault[i].id,
               a.fault[i].nlink, a.fault[i].fanin, a.fault[i].name);
    vol_close(v);
    return a.mismatch ? 3 : 0;
}

static int open_vol(const char *path, invfs_volume **out)
{
    int err = 0;
    invfs_volume *v = vol_open(path, &err);
    if (!v) {
        fprintf(stderr, "vol_open(%s) failed: err=%d\n", path, err);
        return -1;
    }
    if (!(vol_sb(v)->vol_flags & VOLF_META)) {
        fprintf(stderr, "%s: not a v3 volume (vol_flags=0x%08x)\n",
                path, (unsigned)vol_sb(v)->vol_flags);
        vol_close(v);
        return -1;
    }
    *out = v;
    return 0;
}

static int cmd_put(const char *img, int argc, char **argv)
{
    invfs_volume *v;
    invfs_inode in;
    uint64_t id;
    time_t now;

    if (argc != 9) { usage(argv[0]); return 2; }
    id = strtoull(argv[3], NULL, 0);
    memset(&in, 0, sizeof in);
    in.type  = (uint32_t)strtoul(argv[4], NULL, 0);
    in.mode  = (uint16_t)strtoul(argv[5], NULL, 8);
    in.uid   = (uint32_t)strtoul(argv[6], NULL, 0);
    in.gid   = (uint32_t)strtoul(argv[7], NULL, 0);
    in.size  = (uint64_t)strtoull(argv[8], NULL, 0);
    now = time(NULL);
    in.mtime = (int64_t)now;
    in.atime = (int64_t)now;
    in.nlink = 1;
    in.rdev  = 0;
    memset(&in.recipe, 0, sizeof in.recipe);

    if (open_vol(img, &v) != 0)
        return 2;
    if (vol_inode_put(v, id, &in) != 0) {
        fprintf(stderr, "vol_inode_put(%llu) failed\n",
                (unsigned long long)id);
        vol_close(v);
        return 2;
    }
    printf("put id=%llu type=%u mode=%04o uid=%u gid=%u size=%llu\n",
           (unsigned long long)id, in.type, in.mode, in.uid, in.gid,
           (unsigned long long)in.size);
    vol_close(v);
    return 0;
}

static int cmd_get(const char *img, int argc, char **argv)
{
    invfs_volume *v;
    invfs_inode in;
    invfs_meta_pub m;
    uint64_t id;
    int rc;

    if (argc != 4) { usage(argv[0]); return 2; }
    id = strtoull(argv[3], NULL, 0);
    if (open_vol(img, &v) != 0)
        return 2;

    rc = vol_inode_get(v, id, &in);
    if (rc != 1) {
        printf("absent id=%llu (rc=%d)\n", (unsigned long long)id, rc);
        vol_close(v);
        return 1;
    }
    printf("found id=%llu type=%u mode=%04o uid=%u gid=%u nlink=%u "
           "size=%llu rdev=%llu recipe_pba=%llu mtime=%lld\n",
           (unsigned long long)id, in.type, in.mode, in.uid, in.gid,
           in.nlink, (unsigned long long)in.size,
           (unsigned long long)in.rdev,
           (unsigned long long)in.recipe.pba, (long long)in.mtime);

    /* exercise the public stat surface too (vol_get_meta v3 branch) */
    if (vol_get_meta(v, id, &m) != 0) {
        fprintf(stderr, "vol_get_meta(%llu) failed after a get\n",
                (unsigned long long)id);
        vol_close(v);
        return 2;
    }
    printf("stat id=%llu type=%u mode=%04o uid=%u gid=%u nlink=%u size=%llu\n",
           (unsigned long long)id, m.type, m.mode, m.uid, m.gid, m.nlink,
           (unsigned long long)m.size);
    vol_close(v);
    return 0;
}

static int cmd_del(const char *img, int argc, char **argv)
{
    invfs_volume *v;
    uint64_t id;

    if (argc != 4) { usage(argv[0]); return 2; }
    id = strtoull(argv[3], NULL, 0);
    if (open_vol(img, &v) != 0)
        return 2;
    if (vol_inode_delete(v, id) != 0) {
        fprintf(stderr, "vol_inode_delete(%llu) failed\n",
                (unsigned long long)id);
        vol_close(v);
        return 2;
    }
    printf("deleted id=%llu\n", (unsigned long long)id);
    vol_close(v);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE);
            return 0;
        }
    }
    if (argc < 3) { usage(argv[0]); return 2; }
    cmd = argv[2];
    if (strcmp(cmd, "put") == 0)
        return cmd_put(argv[1], argc, argv);
    if (strcmp(cmd, "get") == 0)
        return cmd_get(argv[1], argc, argv);
    if (strcmp(cmd, "del") == 0)
        return cmd_del(argv[1], argc, argv);
    if (strcmp(cmd, "dirent") == 0)
        return cmd_dirent(argv[1], argc, argv);
    if (strcmp(cmd, "nlink") == 0)
        return cmd_nlink(argv[1], argc, argv);
    usage(argv[0]);
    return 2;
}
