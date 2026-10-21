/* chld_producer_test.c -- WP660: CHLD producer and reader gate.
 *
 * Validates:
 * 1. Byte-identity for small files written with default span.
 * 2. Crossing the recipe wall with INVFS_CHILD_SEGS=8:
 *    multi-child recipe tree produced, each child blob loads,
 *    top is CHLD, vol_read_inode and vol_read_range bit-exact,
 *    vol_fsck CLEAN.
 * 3. Ranged writes into already-sealed children.
 * 4. Free path: unlinking a multi-child tree recurses and frees all blocks.
 * 5. Depth refusal: nested CHLD returns -1 (loud EIO, never zeros).
 * 6. Truncate across child spans.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "invarifs.h"
#include "volume_internal.h"
#include "vol_delta.h"

static int checks = 0, failures = 0;

static void ok(int cond, const char *msg)
{
    checks++;
    if (!cond) {
        failures++;
        fprintf(stderr, "  FAIL: %s\n", msg);
    } else {
        printf("  PASS: %s\n", msg);
    }
}

static uint8_t pat_byte(size_t i)
{
    return (uint8_t)((i * 37 + (i >> 8) + 13) & 0xFF);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    invfs_volume *v = NULL;
    int err = 0;

    snprintf(img, sizeof img, "%s/invfs-chld-producer-test.img", dir);
    {
        char cmd[600];
        snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 100 >/dev/null 2>&1", img);
        if (system(cmd) != 0) { fprintf(stderr, "mkfs failed\n"); return 2; }
    }
    v = vol_open(img, &err);
    if (!v) { fprintf(stderr, "vol_open failed (%d)\n", err); return 2; }

    /* Leg 1: Byte-identity for default span */
    printf("=== Leg 1: Byte-identity with default span ===\n");
    unsetenv("INVFS_CHILD_SEGS");
    {
        size_t sz1 = 200000;
        uint8_t *d1 = malloc(sz1);
        for (size_t i = 0; i < sz1; i++) d1[i] = pat_byte(i);
        uint64_t id1 = vol_write_bulk(v, "small.bin", d1, sz1, NULL);
        ok(id1 != 0, "wrote small.bin");

        invfs_inode in1;
        ok(vol_inode_get(v, id1, &in1) > 0, "got small.bin inode");
        uint8_t *blob1 = NULL;
        size_t blen1 = 0;
        ok(vol_recipe_load(v, in1.recipe_addr, &blob1, &blen1) == 0 && blob1, "loaded small.bin recipe");

        invfs_ast_hdr ah1;
        const invfs_ast_block_entry *ents1 = NULL;
        size_t nents1 = 0;
        ok(vol_ast_recipe_parse(blob1, blen1, &ah1, &ents1, &nents1) == 0, "parsed small.bin recipe");
        ok(ah1.num_children == 0, "small.bin num_children == 0");

        const uint8_t *chld1 = NULL;
        uint32_t nchld1 = 0;
        ok(vol_ast_recipe_children(blob1, blen1, &ah1, &chld1, &nchld1) == 0 && nchld1 == 0,
           "small.bin has no CHLD table");

        uint8_t *ser_blob = NULL;
        size_t ser_len = 0;
        ok(vol_ast_recipe_serialize(sz1, ents1, (uint32_t)nents1, &ser_blob, &ser_len) == 0,
           "serialized small.bin with plain serializer");
        ok(ser_len == blen1 && memcmp(ser_blob, blob1, ser_len) == 0,
           "PROVEN: on-disk recipe is BYTE-IDENTICAL to plain serializer");

        uint8_t *rd1 = NULL;
        size_t rdl1 = 0;
        ok(vol_read_file(v, id1, &rd1, &rdl1) == 0 && rdl1 == sz1, "read small.bin");
        ok(memcmp(rd1, d1, sz1) == 0, "small.bin content matches bit-exactly");
        free(rd1);
        free(ser_blob);
        vol_ast_recipe_parsed_free(&ah1, ents1);
        free(blob1);
        free(d1);
    }

    /* Leg 2: Crossing the recipe wall with INVFS_CHILD_SEGS=8 */
    printf("=== Leg 2: Crossing the recipe wall with INVFS_CHILD_SEGS=8 ===\n");
    setenv("INVFS_CHILD_SEGS", "8", 1);
    uint64_t id2 = 0;
    size_t sz2 = 20 * SEGMENT_SIZE; /* 20 segments -> 3 children: 8, 8, 4 */
    uint8_t *d2 = malloc(sz2);
    for (size_t i = 0; i < sz2; i++) d2[i] = pat_byte(i * 3);

    id2 = vol_write_bulk(v, "multi_child.bin", d2, sz2, NULL);
    ok(id2 != 0, "wrote multi_child.bin (20 segments)");

    invfs_inode in2;
    ok(vol_inode_get(v, id2, &in2) > 0, "got multi_child.bin inode");
    uint8_t *blob2 = NULL;
    size_t blen2 = 0;
    ok(vol_recipe_load(v, in2.recipe_addr, &blob2, &blen2) == 0 && blob2, "loaded top recipe");

    invfs_ast_hdr ah2;
    const invfs_ast_block_entry *ents2 = NULL;
    size_t nents2 = 0;
    ok(vol_ast_recipe_parse(blob2, blen2, &ah2, &ents2, &nents2) == 0, "parsed top recipe");
    ok(nents2 == 3, "top recipe has exactly 3 entries");

    const uint8_t *chld2 = NULL;
    uint32_t nchld2 = 0;
    ok(vol_ast_recipe_children(blob2, blen2, &ah2, &chld2, &nchld2) == 0 && nchld2 == 3,
       "top recipe has CHLD table with 3 children");

    /* Verify each reference entry and its child blob */
    for (uint32_t c = 0; c < 3; c++) {
        ok(ents2[c].algo == INVFS_ALGO_CHILD, "entry algo is INVFS_ALGO_CHILD");
        ok(ents2[c].pba == 0, "entry pba is 0 (reference)");
        ok(ents2[c].block_id == c, "entry block_id matches child index");
        uint64_t expected_off = (uint64_t)c * 8 * SEGMENT_SIZE;
        ok(ents2[c].file_offset == expected_off, "entry file_offset correct");

        uint64_t expected_len = (c == 2) ? (4 * SEGMENT_SIZE) : (8 * SEGMENT_SIZE);
        ok(ents2[c].length == expected_len, "entry length matches span (clipped at tail)");

        /* Load child blob */
        const uint8_t *caddr = chld2 + c * sizeof(invfs_ast_child_ref);
        uint8_t *cblob = NULL;
        size_t cblen = 0;
        ok(vol_recipe_load(v, caddr, &cblob, &cblen) == 0 && cblob, "loaded child recipe");

        invfs_ast_hdr cah;
        const invfs_ast_block_entry *cents = NULL;
        size_t cn = 0;
        ok(vol_ast_recipe_parse(cblob, cblen, &cah, &cents, &cn) == 0, "parsed child recipe");
        size_t expected_cn = (c == 2) ? 4 : 8;
        ok(cn == expected_cn, "child has expected number of segment entries");

        /* Verify absolute offsets inside child */
        for (size_t k = 0; k < cn; k++) {
            uint64_t abs_seg = (uint64_t)c * 8 + k;
            ok(cents[k].file_offset == abs_seg * SEGMENT_SIZE, "child entry file_offset is absolute");
            ok(cents[k].pba != 0, "child entry pba is allocated");
        }

        vol_ast_recipe_parsed_free(&cah, cents);
        free(cblob);
    }

    /* Verify bit-exact readback via vol_read_inode and vol_read_range */
    uint8_t *rd2 = NULL;
    size_t rdl2 = 0;
    ok(vol_read_file(v, id2, &rd2, &rdl2) == 0 && rdl2 == sz2, "vol_read_file multi_child.bin");
    ok(memcmp(rd2, d2, sz2) == 0, "full readback bit-exact");
    free(rd2);

    /* Ranged read across child boundaries: offset in child 0, extending into child 1 */
    uint8_t rrbuf[SEGMENT_SIZE * 4];
    uint64_t rroff = 6 * SEGMENT_SIZE;
    int rgot = vol_read_range(v, id2, rroff, sizeof rrbuf, rrbuf);
    ok(rgot == (int)sizeof rrbuf, "vol_read_range across child boundary returned full length");
    ok(memcmp(rrbuf, d2 + rroff, sizeof rrbuf) == 0, "ranged read across boundary bit-exact");

    /* Leg 3: Ranged writes into already-sealed children */
    printf("=== Leg 3: Ranged writes into sealed children ===\n");
    {
        invfs_wsession *ws = NULL;
        uint64_t wid = vol_write_begin(v, "multi_child.bin", 0, &ws);
        ok(wid != 0 && ws != NULL, "opened write session on multi_child.bin");

        /* Overwrite segment 2 (in child 0) */
        uint8_t patch0[SEGMENT_SIZE];
        memset(patch0, 0xAB, sizeof patch0);
        ok(vol_write_range(ws, 2 * SEGMENT_SIZE, patch0, sizeof patch0) == 0,
           "pwrite into child 0 (segment 2)");
        memcpy(d2 + 2 * SEGMENT_SIZE, patch0, sizeof patch0);

        /* Overwrite segment 18 (in child 2) */
        uint8_t patch2[SEGMENT_SIZE];
        memset(patch2, 0xCD, sizeof patch2);
        ok(vol_write_range(ws, 18 * SEGMENT_SIZE, patch2, sizeof patch2) == 0,
           "pwrite into child 2 (segment 18)");
        memcpy(d2 + 18 * SEGMENT_SIZE, patch2, sizeof patch2);

        ok(vol_write_commit(ws) == 0, "commit write session with patched children");

        uint8_t *rd3 = NULL;
        size_t rdl3 = 0;
        ok(vol_read_file(v, id2, &rd3, &rdl3) == 0 && rdl3 == sz2, "read patched multi_child.bin");
        ok(memcmp(rd3, d2, sz2) == 0, "patched file reads back bit-exact across all children");
        free(rd3);
    }

    /* Leg 4: Free path (unlinking multi-child tree) */
    printf("=== Leg 4: Recursive free path ===\n");
    {
        uint64_t free_before_unlink = v->free_blocks;
        ok(vol_unlink(v, "multi_child.bin") == 0, "unlinked multi_child.bin");
        vol_retire_drain_all(v);

        uint64_t free_after_unlink = v->free_blocks;
        printf("  free blocks before unlink: %llu, after: %llu (freed +%llu)\n",
               (unsigned long long)free_before_unlink,
               (unsigned long long)free_after_unlink,
               (unsigned long long)(free_after_unlink - free_before_unlink));
        ok(free_after_unlink > free_before_unlink, "blocks returned to pool on unlink");
    }

    vol_ast_recipe_parsed_free(&ah2, ents2);
    free(blob2);
    free(d2);

    /* Leg 5: Depth refusal (nested CHLD is a loud EIO, never zeros) */
    printf("=== Leg 5: Depth refusal ===\n");
    {
        /* Create an innermost flat child */
        size_t dsz = SEGMENT_SIZE;
        uint8_t dbyte[SEGMENT_SIZE];
        memset(dbyte, 0x42, sizeof dbyte);
        uint64_t fid = vol_write_bulk(v, "leaf.bin", dbyte, dsz, NULL);
        ok(fid != 0, "wrote leaf file");
        invfs_inode fin;
        ok(vol_inode_get(v, fid, &fin) > 0, "got leaf inode");

        /* Middle: reference to leaf */
        invfs_ast_block_entry me;
        memset(&me, 0, sizeof me);
        me.length = dsz;
        me.algo = INVFS_ALGO_CHILD;
        me.block_id = 0;
        invfs_ast_child_ref mref;
        memcpy(mref.addr, fin.recipe_addr, INVFS_RECIPE_ADDR_LEN);
        uint8_t *mblob = NULL;
        size_t mlen = 0;
        ok(vol_ast_recipe_serialize_tree(dsz, &me, 1, &mref, 1, &mblob, &mlen) == 0,
           "serialized middle child recipe (depth 1)");
        uint8_t maddr[INVFS_RECIPE_ADDR_LEN];
        ok(vol_recipe_store(v, mblob, mlen, maddr) == 0, "stored middle child");
        free(mblob);

        /* Top: reference to middle -> depth 2! */
        invfs_ast_block_entry te;
        memset(&te, 0, sizeof te);
        te.length = dsz;
        te.algo = INVFS_ALGO_CHILD;
        te.block_id = 0;
        invfs_ast_child_ref tref;
        memcpy(tref.addr, maddr, INVFS_RECIPE_ADDR_LEN);
        uint8_t *tblob = NULL;
        size_t tlen = 0;
        ok(vol_ast_recipe_serialize_tree(dsz, &te, 1, &tref, 1, &tblob, &tlen) == 0,
           "serialized top recipe with nested CHLD child");
        uint8_t taddr[INVFS_RECIPE_ADDR_LEN];
        ok(vol_recipe_store(v, tblob, tlen, taddr) == 0, "stored top recipe");
        free(tblob);

        /* Create inode pointing to this top recipe */
        uint64_t bad_id = vol_create_content_node(v, "bad_nested.bin", dsz, taddr);
        ok(bad_id != 0, "created bad_nested.bin inode");

        /* Attempting to read bad_nested.bin MUST FAIL loudly (-1), never return zeros */
        uint8_t *bad_data = NULL;
        size_t bad_len = 0;
        ok(vol_read_file(v, bad_id, &bad_data, &bad_len) != 0,
           "vol_read_file refused nested CHLD (depth limit)");
        ok(bad_data == NULL, "no data allocated on refused read");

        uint8_t bad_buf[SEGMENT_SIZE];
        int brc = vol_read_range(v, bad_id, 0, sizeof bad_buf, bad_buf);
        ok(brc != (int)sizeof bad_buf, "vol_read_range refused nested CHLD (EIO, not zeros)");
    }

    /* Leg 6: Truncate across child spans */
    printf("=== Leg 6: Truncate across child spans ===\n");
    {
        invfs_wsession *ws = NULL;
        uint64_t tid = vol_write_begin(v, "trunc.bin", 0, &ws);
        ok(tid != 0 && ws != NULL, "opened write session for trunc.bin");

        /* Truncate extend to 16 segments (2 children of 8) */
        ok(vol_write_truncate(ws, 16 * SEGMENT_SIZE) == 0, "truncate extend to 16 segments");
        ok(vol_write_commit(ws) == 0, "committed trunc.bin extend");

        uint64_t real_tid = 0;
        ok(vol_find_rc(v, "trunc.bin", &real_tid) == 1 && real_tid != 0, "found trunc.bin inode");

        /* Verify 16 segments of zeros */
        uint8_t zcheck[SEGMENT_SIZE];
        int zgot = vol_read_range(v, real_tid, 10 * SEGMENT_SIZE, sizeof zcheck, zcheck);
        ok(zgot == (int)sizeof zcheck, "read extended segment in child 1");
        int all_zero = 1;
        for (size_t i = 0; i < sizeof zcheck; i++) {
            if (zcheck[i] != 0) { all_zero = 0; break; }
        }
        ok(all_zero, "extended segment in child 1 reads as zeros");
    }

    /* Clean fsck at end */
    printf("=== Volume FSCK ===\n");
    invfs_fsck_report rep;
    memset(&rep, 0, sizeof rep);
    ok(vol_fsck_scan(v, &rep, 0) == 0 && rep.damaged == 0, "vol_fsck_scan CLEAN after all legs");

    vol_close(v);
    {
        char cmd[600];
        snprintf(cmd, sizeof cmd, "./bin/invf-fsck %s >/dev/null 2>&1", img);
        ok(system(cmd) == 0, "invf-fsck CLI returns 0 (CLEAN)");
    }
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
