/* symlink_v3_test.c — WP-M24 offline test for v3 symlinks and special files.
 *
 * Verifies:
 *   1. vol_create_symlink works on v3 (root and nested paths).
 *   2. vol_get_meta returns INVFS_ITYP_LNK, correct length, and target string.
 *   3. vol_read_inode reads the target string directly.
 *   4. vol_apply_meta updates symlink target correctly.
 *   5. vol_create_special creates FIFO, SOCK, CHR, BLK with correct type, mode, rdev.
 *   6. vol_get_meta preserves rdev and modes for special files.
 *   7. Reopen volume: all symlinks and special files persist durably.
 *   8. INVFS_V2=1 mkfs refusal: deprecated v2 format cannot be created.
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
    uint64_t id_link_root, id_link_nested;
    uint64_t id_fifo, id_sock, id_chr, id_blk;
    invfs_meta_pub m;
    uint8_t *read_buf = NULL;
    size_t read_len = 0;

    printf("symlink_v3_test (WP-M24): v3 symlinks, special files, and mkfs v2 retirement\n");

    snprintf(img, sizeof img, "%s/invf-symlink-v3-test.img", dir);
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
            fprintf(stderr, "symlink_v3_test: vol_open(%s) failed: err=%d\n", img, err);
            return 2;
        }
    }

    /* 2. Setup directory hierarchy */
    ok(vol_v3_mkdir(v, "dir1") != 0, "mkdir dir1");
    ok(vol_v3_mkdir(v, "dir1/sub2") != 0, "mkdir dir1/sub2");

    /* 3. Test symlink creation */
    id_link_root = vol_create_symlink(v, "link_to_somewhere", "/usr/bin/python3");
    ok(id_link_root != 0, "vol_create_symlink root");

    id_link_nested = vol_create_symlink(v, "dir1/sub2/rel_link", "../../photo.jpg");
    ok(id_link_nested != 0, "vol_create_symlink nested");

    /* 4. Verify vol_get_meta on symlinks */
    memset(&m, 0, sizeof m);
    ok(vol_get_meta(v, id_link_root, &m) == 0, "vol_get_meta link_root");
    ok(m.type == INVFS_ITYP_LNK, "link_root type is INVFS_ITYP_LNK");
    ok(strcmp(m.target, "/usr/bin/python3") == 0, "link_root target matches");
    ok(m.size == strlen("/usr/bin/python3"), "link_root size matches target len");

    memset(&m, 0, sizeof m);
    ok(vol_get_meta(v, id_link_nested, &m) == 0, "vol_get_meta link_nested");
    ok(m.type == INVFS_ITYP_LNK, "link_nested type is INVFS_ITYP_LNK");
    ok(strcmp(m.target, "../../photo.jpg") == 0, "link_nested target matches");

    /* 5. Verify vol_read_inode on symlink */
    ok(vol_read_inode(v, id_link_root, 0, &read_buf, &read_len) == 0, "vol_read_inode link_root");
    ok(read_len == strlen("/usr/bin/python3"), "vol_read_inode link_root len");
    ok(read_buf && memcmp(read_buf, "/usr/bin/python3", read_len) == 0, "vol_read_inode link_root content");
    free(read_buf); read_buf = NULL;

    /* 6. Verify vol_apply_meta updating symlink target */
    snprintf(m.target, sizeof m.target, "%s", "/opt/custom/bin");
    m.type = INVFS_ITYP_LNK;
    m.mode = 0777;
    ok(vol_apply_meta(v, "link_to_somewhere", &m) != 0, "vol_apply_meta link_to_somewhere");

    memset(&m, 0, sizeof m);
    ok(vol_get_meta(v, id_link_root, &m) == 0, "vol_get_meta after target update");
    ok(strcmp(m.target, "/opt/custom/bin") == 0, "updated target matches");

    /* 7. Test special files */
    id_fifo = vol_create_special(v, "dir1/my_fifo", INVFS_ITYP_FIFO, 0664, 0);
    ok(id_fifo != 0, "vol_create_special FIFO");

    id_sock = vol_create_special(v, "my_sock", INVFS_ITYP_SOCK, 0775, 0);
    ok(id_sock != 0, "vol_create_special SOCK");

    /* major 1, minor 3 (null dev) => 0x0103 */
    id_chr = vol_create_special(v, "dev_null", INVFS_ITYP_CHR, 0666, 0x0103);
    ok(id_chr != 0, "vol_create_special CHR");

    /* major 8, minor 0 (sda) => 0x0800 */
    id_blk = vol_create_special(v, "dev_sda", INVFS_ITYP_BLK, 0660, 0x0800);
    ok(id_blk != 0, "vol_create_special BLK");

    /* 8. Verify vol_get_meta on special files */
    memset(&m, 0, sizeof m);
    ok(vol_get_meta(v, id_fifo, &m) == 0, "vol_get_meta FIFO");
    ok(m.type == INVFS_ITYP_FIFO, "FIFO type matches");
    ok((m.mode & 0777) == 0664, "FIFO mode matches");

    memset(&m, 0, sizeof m);
    ok(vol_get_meta(v, id_sock, &m) == 0, "vol_get_meta SOCK");
    ok(m.type == INVFS_ITYP_SOCK, "SOCK type matches");

    memset(&m, 0, sizeof m);
    ok(vol_get_meta(v, id_chr, &m) == 0, "vol_get_meta CHR");
    ok(m.type == INVFS_ITYP_CHR, "CHR type matches");
    ok(m.rdev == 0x0103, "CHR rdev matches (0x0103)");

    memset(&m, 0, sizeof m);
    ok(vol_get_meta(v, id_blk, &m) == 0, "vol_get_meta BLK");
    ok(m.type == INVFS_ITYP_BLK, "BLK type matches");
    ok(m.rdev == 0x0800, "BLK rdev matches (0x0800)");

    /* 9. Commit & close, reopen volume, verify persistence */
    vol_close(v);

    {
        int err = 0;
        v = vol_open(img, &err);
        ok(v != NULL, "vol_open after remount");
    }

    memset(&m, 0, sizeof m);
    ok(vol_get_meta(v, id_link_root, &m) == 0, "remount: vol_get_meta link_root");
    ok(strcmp(m.target, "/opt/custom/bin") == 0, "remount: link_root target intact");

    memset(&m, 0, sizeof m);
    ok(vol_get_meta(v, id_link_nested, &m) == 0, "remount: vol_get_meta link_nested");
    ok(strcmp(m.target, "../../photo.jpg") == 0, "remount: link_nested target intact");

    memset(&m, 0, sizeof m);
    ok(vol_get_meta(v, id_chr, &m) == 0, "remount: vol_get_meta CHR");
    ok(m.type == INVFS_ITYP_CHR && m.rdev == 0x0103, "remount: CHR intact");

    memset(&m, 0, sizeof m);
    ok(vol_get_meta(v, id_blk, &m) == 0, "remount: vol_get_meta BLK");
    ok(m.type == INVFS_ITYP_BLK && m.rdev == 0x0800, "remount: BLK intact");

    vol_close(v);
    unlink(img);

    /* 10. Test INVFS_V2=1 refusal */
    {
        char cmd[1024];
        char dead_img[512];
        snprintf(dead_img, sizeof dead_img, "%s/invf-dead-v2.img", dir);
        unlink(dead_img);
        snprintf(cmd, sizeof cmd,
                 "INVFS_V2=1 %s/bin/invf-mkfs %s 10 2>/dev/null",
                 getenv("PWD") ? getenv("PWD") : ".", dead_img);
        int rc = system(cmd);
        int exit_code = WEXITSTATUS(rc);
        ok(exit_code == 2, "INVFS_V2=1 refused with exit code 2");
        unlink(dead_img);
    }

    printf("\nsymlink_v3_test summary: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
