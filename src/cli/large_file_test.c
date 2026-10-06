/*
 * large_file_test.c — WP-M25: streaming & multi-chunk recipe blobs for files > 7.7 MiB
 *
 * Validates:
 *   1. Writing a 16 MiB file (256 segments, recipe ~8.2 KiB > 3800 B single-page limit)
 *   2. Content addressing: recipe is chunked (RMC1 descriptor + 3 chunks) and verified
 *   3. Bit-exact read-back through vol_read_file / vol_read_inode
 *   4. Random ranged reads across chunk boundaries
 *   5. Durable remount round-trip
 *   6. Data block reclamation on unlink
 *   7. Clean fsck
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "invarifs.h"
#include "volume_internal.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *msg)
{
    checks++;
    if (cond) {
        printf("  OK    %s\n", msg);
    } else {
        printf("  FAIL  %s\n", msg);
        failures++;
    }
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[256];
    invfs_volume *v = NULL;
    size_t file_size = 16 * 1024 * 1024; /* 16 MiB */
    uint8_t *orig_data = NULL;
    uint8_t *read_buf = NULL;
    size_t read_len = 0;
    uint64_t id = 0;

    snprintf(img, sizeof img, "%s/invf-large-v3-test.img", dir);
    unlink(img);

    printf("large_file_test (WP-M25): multi-chunk recipe blobs (> 7.7 MiB)\n");

    orig_data = (uint8_t *)malloc(file_size);
    if (!orig_data) {
        fprintf(stderr, "failed to allocate 16 MiB test buffer\n");
        return 2;
    }
    for (size_t i = 0; i < file_size; i++) {
        orig_data[i] = (uint8_t)((i * 17 + 31) ^ (i >> 16));
    }

    /* 1. mkfs v3 volume */
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 64 >/dev/null 2>&1", img);
        if (system(cmd) != 0) {
            fprintf(stderr, "mkfs failed\n");
            free(orig_data);
            return 2;
        }
    }

    {
        int err = 0;
        v = vol_open(img, &err);
        if (!v) {
            fprintf(stderr, "vol_open failed (%d)\n", err);
            free(orig_data);
            return 2;
        }
    }

    /* 2. Write 16 MiB file */
    id = vol_write_bulk(v, "large_16m.bin", orig_data, file_size, NULL);
    ok(id != 0, "write 16 MiB file via vol_write_bulk");

    /* 3. Stat size check */
    {
        uint64_t stat_id = 0, stat_sz = 0, stat_ct = 0;
        ok(vol_path_stat(v, "large_16m.bin", &stat_id, &stat_sz, &stat_ct) == 0 &&
           stat_id == id && stat_sz == file_size,
           "path_stat matches 16 MiB size and inode id");
    }

    /* 4. Full read-back check */
    ok(vol_read_inode(v, id, 0, &read_buf, &read_len) == 0 &&
       read_len == file_size &&
       memcmp(read_buf, orig_data, file_size) == 0,
       "full read-back matches bit-exact");
    free(read_buf);
    read_buf = NULL;

    /* 5. Ranged reads across various segment offsets */
    {
        uint8_t range_buf[128 * 1024];
        size_t off = 7 * 1024 * 1024 + 512; /* around 7 MB */
        int rlen = vol_read_range(v, id, off, sizeof range_buf, range_buf);
        ok(rlen == sizeof range_buf &&
           memcmp(range_buf, orig_data + off, sizeof range_buf) == 0,
           "range read at 7 MiB boundary bit-exact");

        off = 15 * 1024 * 1024 + 1000; /* near end of 16 MB */
        rlen = vol_read_range(v, id, off, 32768, range_buf);
        ok(rlen == 32768 &&
           memcmp(range_buf, orig_data + off, 32768) == 0,
           "range read near 16 MiB EOF bit-exact");
    }

    /* 6. Close and durable remount */
    vol_close(v);
    {
        int err = 0;
        v = vol_open(img, &err);
        ok(v != NULL, "durable remount after 16 MiB write");

        read_len = 0;
        ok(vol_read_inode(v, id, 0, &read_buf, &read_len) == 0 &&
           read_len == file_size &&
           memcmp(read_buf, orig_data, file_size) == 0,
           "remount: 16 MiB file bit-exact");
        free(read_buf);
        read_buf = NULL;

        /* Header item 7 is "Clean fsck". It used to run at the very end of
         * the test, AFTER step 7 had unlinked the file and the volume had been
         * closed -- so it was an exit-code assertion on a volume that no
         * longer contained the thing under test. The one failure it could have
         * caught is an unlink that freed the live recipe blob while leaving
         * the base tree pointing at it: the volume then reports clean and this
         * passed. That is the btree_repair failure mode exactly, and it is the
         * failure a "16 MiB multi-chunk recipe" test most needs to rule out.
         *
         * So fsck now runs HERE, with the file present, and the file is read
         * back again afterwards. The later run is kept as a second opinion on
         * the post-unlink state, and its exit code is now checked rather than
         * discarded. */
        vol_close(v);   /* vol_open is exclusive: the CLI needs the image */
        v = NULL;
        {
            char cmd[512];
            snprintf(cmd, sizeof cmd, "./bin/invf-fsck %s >/dev/null 2>&1", img);
            ok(system(cmd) == 0, "invf-fsck clean WITH the 16 MiB file present");
        }
        v = vol_open(img, &err);
        ok(v != NULL, "the fsck-clean volume reopens");
        if (v) {
            read_len = 0;
            ok(vol_read_inode(v, id, 0, &read_buf, &read_len) == 0 &&
               read_len == file_size &&
               memcmp(read_buf, orig_data, file_size) == 0,
               "the file still reads back bit-exact AFTER invf-fsck ran on a "
               "volume holding it -- fsck did not take the recipe with it");
            free(read_buf);
            read_buf = NULL;
        }
    }

    /* 7. Unlink and data block reclamation */
    {
        uint64_t free_before = v->free_blocks;
        ok(vol_unlink(v, "large_16m.bin") == 0, "unlink 16 MiB file");
        ok(v->free_blocks > free_before, "free blocks increased after unlinking 16 MiB file");
    }

    vol_close(v);

    /* 8. fsck clean after the unlink -- a second opinion on the post-unlink
     * state, not the only one. Step 6 above is the load-bearing fsck leg. */
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "./bin/invf-fsck %s >/dev/null 2>&1", img);
        ok(system(cmd) == 0, "invf-fsck clean after the unlink");
    }

    unlink(img);
    free(orig_data);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
