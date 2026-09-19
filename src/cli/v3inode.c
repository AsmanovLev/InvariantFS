/* v3inode.c — WP-M5 e2e driver for the metadata-v3 inode tree.
 *
 * Until WP-M6 (dirents) lands there is no name -> inode resolution on a v3
 * volume, so the inode tree is exercised directly by id. This tool is the
 * offline counterpart of a FUSE create/stat: it opens the volume (a mount
 * at the library level), mutates/reads one inode row, and closes cleanly.
 *
 * usage:
 *   invf-v3inode <img> put <id> <type> <mode-octal> <uid> <gid> <size>
 *   invf-v3inode <img> get <id>
 *   invf-v3inode <img> del <id>
 *
 * get prints one line and exits 0 when the row is present, 1 when absent,
 * 2 on a usage/open error. */
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
            "       %s <img> del <id>\n",
            argv0, argv0, argv0);
}

static int open_vol(const char *path, invfs_volume **out)
{
    int err = 0;
    invfs_volume *v = vol_open(path, &err);
    if (!v) {
        fprintf(stderr, "vol_open(%s) failed: err=%d\n", path, err);
        return -1;
    }
    if (!(vol_sb(v)->vol_flags & VOLF_V3)) {
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
    invfs_v3_inode in;
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
    if (vol_v3_inode_put(v, id, &in) != 0) {
        fprintf(stderr, "vol_v3_inode_put(%llu) failed\n",
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
    invfs_v3_inode in;
    invfs_meta_pub m;
    uint64_t id;
    int rc;

    if (argc != 4) { usage(argv[0]); return 2; }
    id = strtoull(argv[3], NULL, 0);
    if (open_vol(img, &v) != 0)
        return 2;

    rc = vol_v3_inode_get(v, id, &in);
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
    if (vol_v3_inode_delete(v, id) != 0) {
        fprintf(stderr, "vol_v3_inode_delete(%llu) failed\n",
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
    usage(argv[0]);
    return 2;
}
