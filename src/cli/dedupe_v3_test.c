/*
 * dedupe_v3_test.c — Meta-v3 segment deduplication unit test
 *
 * Tests:
 *   1. Intra-file duplicate segments (identical 64 KB blocks within the same file)
 *   2. Inter-file duplicate segments (identical 64 KB blocks across different files)
 *   3. vol_sweep_dedupe merges duplicate segment entries in recipes
 *   4. Physical duplicate blocks are freed and returned to bitmap
 *   5. Bit-exact data preservation across all files
 *   6. Durable remount round-trip
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
static int progress_calls = 0;
static int saw_hash_progress = 0;
static int saw_merge_progress = 0;

static void dedupe_progress(void *user, const invfs_dedupe_progress *p)
{
    (void)user;
    progress_calls++;
    if (p && p->phase && (strcmp(p->phase, "hash") == 0 ||
                          strcmp(p->phase, "hash_done") == 0))
        saw_hash_progress = 1;
    if (p && p->phase && strcmp(p->phase, "merge") == 0) saw_merge_progress = 1;
}

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
    size_t seg_sz = SEGMENT_SIZE; /* 64 KiB */
    size_t file_sz = 8 * seg_sz;  /* 512 KiB */
    uint8_t *pat_common = NULL;
    uint8_t *pat_unique = NULL;
    uint8_t *file_a_data = NULL;
    uint8_t *file_b_data = NULL;
    uint8_t *read_buf = NULL;
    size_t read_len = 0;
    uint64_t id_a = 0, id_b = 0;

    snprintf(img, sizeof img, "%s/invf-dedupe-v3-test.img", dir);
    unlink(img);

    printf("dedupe_v3_test: Meta-v3 segment deduplication (intra- and inter-file)\n");

    pat_common = (uint8_t *)malloc(seg_sz);
    pat_unique = (uint8_t *)malloc(seg_sz);
    file_a_data = (uint8_t *)malloc(file_sz);
    file_b_data = (uint8_t *)malloc(file_sz);
    assert(pat_common && pat_unique && file_a_data && file_b_data);

    for (size_t i = 0; i < seg_sz; i++) {
        pat_common[i] = (uint8_t)((i * 37 + 11) & 0xFF);
        pat_unique[i] = (uint8_t)((i * 59 + 73) & 0xFF);
    }

    /* Build File A:
     * seg 0, 1: common pattern (intra-file duplicate!)
     * seg 2..7: unique patterns
     */
    memcpy(file_a_data + 0 * seg_sz, pat_common, seg_sz);
    memcpy(file_a_data + 1 * seg_sz, pat_common, seg_sz);
    for (int s = 2; s < 8; s++) {
        for (size_t i = 0; i < seg_sz; i++)
            file_a_data[s * seg_sz + i] = (uint8_t)((s * 101 + i) & 0xFF);
    }

    /* Build File B:
     * seg 0: common pattern (inter-file duplicate with File A seg 0 & 1!)
     * seg 1: unique pattern
     * seg 2: common pattern (intra-file duplicate with seg 0 & inter-file with File A!)
     * seg 3..7: unique patterns
     */
    memcpy(file_b_data + 0 * seg_sz, pat_common, seg_sz);
    memcpy(file_b_data + 1 * seg_sz, pat_unique, seg_sz);
    memcpy(file_b_data + 2 * seg_sz, pat_common, seg_sz);
    for (int s = 3; s < 8; s++) {
        for (size_t i = 0; i < seg_sz; i++)
            file_b_data[s * seg_sz + i] = (uint8_t)((s * 137 + i) & 0xFF);
    }

    /* 1. Format v3 volume */
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 64 >/dev/null 2>&1", img);
        if (system(cmd) != 0) {
            fprintf(stderr, "mkfs failed\n");
            return 2;
        }
    }

    {
        int err = 0;
        v = vol_open(img, &err);
        if (!v) {
            fprintf(stderr, "vol_open failed (%d)\n", err);
            return 2;
        }
    }

    /* 2. Write File A and File B */
    id_a = vol_v3_write_bulk(v, "file_a.bin", file_a_data, file_sz, NULL);
    ok(id_a != 0, "write file_a.bin");
    id_b = vol_v3_write_bulk(v, "file_b.bin", file_b_data, file_sz, NULL);
    ok(id_b != 0, "write file_b.bin");

    uint64_t free_before = v->free_blocks;

    /* 3. Run vol_sweep_dedupe */
    invfs_dedupe_stats ds;
    int merged = vol_sweep_dedupe_ex(v, &ds, dedupe_progress, NULL);
    ok(merged > 0, "vol_sweep_dedupe merged duplicate segments");
    ok(ds.segments_hashed == 16, "dedupe hashed all 16 live segments");
    ok(ds.duplicate_candidates == 3 &&
       ds.intra_candidates == 1 && ds.cross_candidates == 2,
       "dedupe classified intra-file and cross-file candidates");
    ok(ds.segments_merged == 3 && ds.intra_merged == 1 &&
       ds.cross_merged == 2,
       "dedupe classified intra-file and cross-file merges");
    ok(ds.blocks_freed > 0, "dedupe reports reclaimed block count");
    ok(progress_calls > 0 && saw_hash_progress && saw_merge_progress,
       "dedupe progress callback covers hash and merge phases");
    ok(v->free_blocks > free_before, "free blocks increased after deduplication");

    /* 4. Flush and check bit-exact content */
    vol_flush(v);

    ok(vol_read_inode(v, id_a, 0, &read_buf, &read_len) == 0 &&
       read_len == file_sz &&
       memcmp(read_buf, file_a_data, file_sz) == 0,
       "file_a.bin bit-exact after dedupe");
    free(read_buf); read_buf = NULL;

    ok(vol_read_inode(v, id_b, 0, &read_buf, &read_len) == 0 &&
       read_len == file_sz &&
       memcmp(read_buf, file_b_data, file_sz) == 0,
       "file_b.bin bit-exact after dedupe");
    free(read_buf); read_buf = NULL;

    /* 5. Remount and re-verify */
    vol_close(v);
    {
        int err = 0;
        v = vol_open(img, &err);
        ok(v != NULL, "durable remount after dedupe");

        ok(vol_read_inode(v, id_a, 0, &read_buf, &read_len) == 0 &&
           read_len == file_sz &&
           memcmp(read_buf, file_a_data, file_sz) == 0,
           "remount: file_a.bin bit-exact");
        free(read_buf); read_buf = NULL;

        ok(vol_read_inode(v, id_b, 0, &read_buf, &read_len) == 0 &&
           read_len == file_sz &&
           memcmp(read_buf, file_b_data, file_sz) == 0,
           "remount: file_b.bin bit-exact");
        free(read_buf); read_buf = NULL;

        vol_close(v);
    }

    /* 6. Run fsck */
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "./bin/invf-fsck %s >/dev/null 2>&1", img);
        ok(system(cmd) == 0, "invf-fsck clean on image after dedupe");
    }

    unlink(img);
    free(pat_common);
    free(pat_unique);
    free(file_a_data);
    free(file_b_data);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
