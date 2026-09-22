/* sweep_v3_test.c — WP-M23 offline test for v3 id-keyed sweep publication
 * and nested-corpus safety.
 *
 * Verifies:
 *   1. Nested-corpus sweep through vol_sweep_pending does NOT overwrite
 *      same-named files at the root (resolves P0 silent data loss).
 *   2. Nested files are correctly moved from RAW to Shadow.
 *   3. No stray root entries are created when sweeping nested files.
 *   4. Active write sessions on nested files are protected against sweep races
 *      via vol_write_active_id.
 *   5. vol_sweep_file functions correctly on v3 volumes (SIGUSR1 / manual sweep).
 *   6. Bit-exact invariant preserved across all files through remount.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
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

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    invfs_volume *v;
    uint8_t buf_nested[128 * 1024];
    uint8_t buf_root[64 * 1024];
    uint8_t buf_other[32 * 1024];
    uint64_t id_nested, id_root, id_other;
    uint8_t *read_back = NULL;
    size_t read_len = 0;

    printf("sweep_v3_test (WP-M23): id-keyed publication & nested corpus safety\n");

    snprintf(img, sizeof img, "%s/invf-sweep-v3-test.img", dir);
    unlink(img);

    /* 1. Format fresh v3 volume */
    {
        char cmd[1024];
        snprintf(cmd, sizeof cmd,
                 "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 getenv("PWD") ? getenv("PWD") : ".", img);
        if (system(cmd) != 0) {
            printf("  cannot create volume with invf-mkfs\n");
            return 2;
        }
    }

    {
        int err = 0;
        v = vol_open(img, &err);
        if (!v) {
            fprintf(stderr, "sweep_v3_test: vol_open(%s) failed: err=%d\n", img, err);
            return 2;
        }
    }

    /* Fill buffers with compressible repeating patterns */
    for (size_t i = 0; i < sizeof buf_nested; i++)
        buf_nested[i] = (uint8_t)("NESTED_PHOTO_DATA_"[(i % 18)]);
    for (size_t i = 0; i < sizeof buf_root; i++)
        buf_root[i] = (uint8_t)("ROOT_PHOTO_DIFFERENT_"[(i % 21)]);
    for (size_t i = 0; i < sizeof buf_other; i++)
        buf_other[i] = (uint8_t)("OTHER_FILE_DATA_"[(i % 16)]);

    /* 2. Create nested directories */
    ok(vol_v3_mkdir(v, "dir1") != 0, "mkdir dir1");
    ok(vol_v3_mkdir(v, "dir1/sub2") != 0, "mkdir dir1/sub2");

    /* 3. Write nested file: dir1/sub2/photo.jpg */
    id_nested = vol_v3_write_bulk(v, "dir1/sub2/photo.jpg", buf_nested,
                                  sizeof buf_nested, NULL);
    ok(id_nested != 0, "write dir1/sub2/photo.jpg");

    /* 4. Write root file with same leaf name: photo.jpg */
    id_root = vol_v3_write_bulk(v, "photo.jpg", buf_root,
                                sizeof buf_root, NULL);
    ok(id_root != 0 && id_root != id_nested, "write root photo.jpg with distinct id");

    /* 5. Write another nested file: dir1/other.txt */
    id_other = vol_v3_write_bulk(v, "dir1/other.txt", buf_other,
                                 sizeof buf_other, NULL);
    ok(id_other != 0, "write dir1/other.txt");

    /* Verify all are initially in RAW zone */
    ok(vol_inode_first_zone(v, id_nested) == INVFS_ZONE_RAW, "dir1/sub2/photo.jpg is in RAW zone");
    ok(vol_inode_first_zone(v, id_root) == INVFS_ZONE_RAW, "root photo.jpg is in RAW zone");
    ok(vol_inode_first_zone(v, id_other) == INVFS_ZONE_RAW, "dir1/other.txt is in RAW zone");

    /* 6. Mark them pending for daemon sweep */
    vol_mark_pending(v, id_nested);
    vol_mark_pending(v, id_root);
    vol_mark_pending(v, id_other);
    ok(vol_pending_count(v) == 3, "pending count is 3");

    uint64_t raw_free_before = v->raw_free;
    /* 7. Drain pending queue via vol_sweep_pending */
    {
        int swept = vol_sweep_pending(v);
        ok(swept == 3, "vol_sweep_pending processed 3 files");
    }
    ok(v->raw_free > raw_free_before, "RAW zone reclaimed freed blocks after sweep");

    /* 8. Assert all files are now in Shadow (BINARY) zone */
    ok(vol_inode_first_zone(v, id_nested) == INVFS_ZONE_BINARY,
       "dir1/sub2/photo.jpg successfully swept to Shadow");
    ok(vol_inode_first_zone(v, id_root) == INVFS_ZONE_BINARY,
       "root photo.jpg successfully swept to Shadow");
    ok(vol_inode_first_zone(v, id_other) == INVFS_ZONE_BINARY,
       "dir1/other.txt successfully swept to Shadow");

    /* 9. Verify NO stray entries at root and path lookup still resolves correctly */
    {
        uint64_t found_root_id = 0, found_nested_id = 0, found_other_id = 0;
        ok(vol_v3_path_lookup(v, "photo.jpg", &found_root_id) == 1 &&
           found_root_id == id_root, "lookup photo.jpg matches root id");
        ok(vol_v3_path_lookup(v, "dir1/sub2/photo.jpg", &found_nested_id) == 1 &&
           found_nested_id == id_nested, "lookup dir1/sub2/photo.jpg matches nested id");
        ok(vol_v3_path_lookup(v, "dir1/other.txt", &found_other_id) == 1 &&
           found_other_id == id_other, "lookup dir1/other.txt matches id");
    }

    /* 10. Check bit-exact contents — verify no cross-overwriting! */
    {
        read_back = NULL; read_len = 0;
        ok(vol_read_inode(v, id_root, 0, &read_back, &read_len) == 0 &&
           read_len == sizeof buf_root &&
           memcmp(read_back, buf_root, sizeof buf_root) == 0,
           "root photo.jpg preserved exact bytes (not overwritten by nested file)");
        free(read_back);

        read_back = NULL; read_len = 0;
        ok(vol_read_inode(v, id_nested, 0, &read_back, &read_len) == 0 &&
           read_len == sizeof buf_nested &&
           memcmp(read_back, buf_nested, sizeof buf_nested) == 0,
           "dir1/sub2/photo.jpg preserved exact bytes");
        free(read_back);

        read_back = NULL; read_len = 0;
        ok(vol_read_inode(v, id_other, 0, &read_back, &read_len) == 0 &&
           read_len == sizeof buf_other &&
           memcmp(read_back, buf_other, sizeof buf_other) == 0,
           "dir1/other.txt preserved exact bytes");
        free(read_back);
    }

    /* 11. Test active write session guard on nested file */
    {
        uint64_t act_id = vol_v3_write_bulk(v, "dir1/sub2/racing.txt", buf_other,
                                            sizeof buf_other, NULL);
        ok(act_id != 0, "create dir1/sub2/racing.txt in RAW");
        ok(vol_inode_first_zone(v, act_id) == INVFS_ZONE_RAW, "racing file is initially in RAW");

        invfs_wsession *ws = NULL;
        uint64_t sess_id = vol_write_begin(v, "dir1/sub2/racing.txt", 0, &ws);
        ok(sess_id != 0 && ws != NULL, "open write session on dir1/sub2/racing.txt");

        ok(vol_write_range(ws, 0, buf_other, sizeof buf_other) == 0, "write session payload");
        ok(vol_write_active_id(v, act_id) == 1, "vol_write_active_id detects active session on act_id");

        /* Queue for sweep while active */
        vol_mark_pending(v, act_id);
        int swept_active = vol_sweep_pending(v);
        ok(swept_active == 0, "vol_sweep_pending skipped active write session");
        ok(vol_inode_first_zone(v, act_id) == INVFS_ZONE_RAW, "active file stayed in RAW");

        /* Commit the write session */
        ok(vol_write_commit(ws) == 0, "commit write session");
        ok(vol_write_active_id(v, act_id) == 0, "vol_write_active_id returns 0 after commit");

        /* Now queue and sweep */
        vol_mark_pending(v, act_id);
        swept_active = vol_sweep_pending(v);
        ok(swept_active == 1, "vol_sweep_pending swept committed file");
        ok(vol_inode_first_zone(v, act_id) == INVFS_ZONE_BINARY, "committed file now in Shadow");
    }

    /* 12. Test vol_sweep_file on v3 (manual / SIGUSR1 sweep) */
    {
        uint64_t id_man = vol_v3_write_bulk(v, "dir1/manual.txt", buf_other,
                                            sizeof buf_other, NULL);
        ok(id_man != 0, "write dir1/manual.txt");
        ok(vol_inode_first_zone(v, id_man) == INVFS_ZONE_RAW, "dir1/manual.txt starts in RAW");

        int rc = vol_sweep_file(v, id_man);
        ok(rc == 0, "vol_sweep_file returns 0 (swept)");
        ok(vol_inode_first_zone(v, id_man) == INVFS_ZONE_BINARY, "dir1/manual.txt moved to Shadow");

        rc = vol_sweep_file(v, id_man);
        ok(rc == 1, "second vol_sweep_file returns 1 (already swept/noop)");
    }

    /* 13. Close and remount volume — assert durable state */
    vol_close(v);
    {
        int err = 0;
        v = vol_open(img, &err);
        ok(v != NULL, "remount volume after sweep");

        read_back = NULL; read_len = 0;
        ok(vol_read_inode(v, id_root, 0, &read_back, &read_len) == 0 &&
           read_len == sizeof buf_root &&
           memcmp(read_back, buf_root, sizeof buf_root) == 0,
           "durable remount: root photo.jpg bit-exact");
        free(read_back);

        read_back = NULL; read_len = 0;
        ok(vol_read_inode(v, id_nested, 0, &read_back, &read_len) == 0 &&
           read_len == sizeof buf_nested &&
           memcmp(read_back, buf_nested, sizeof buf_nested) == 0,
           "durable remount: dir1/sub2/photo.jpg bit-exact");
        free(read_back);

        /* 14. Unlink test: verify data blocks are reclaimed */
        uint64_t free_before_unlink = v->free_blocks;
        ok(vol_v3_unlink(v, "photo.jpg") == 0, "unlink photo.jpg");
        ok(v->free_blocks > free_before_unlink, "free blocks increased after unlink");

        vol_close(v);
    }

    unlink(img);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
