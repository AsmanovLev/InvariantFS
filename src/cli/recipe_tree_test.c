/* recipe_tree_test.c -- WP635: a recipe may be a TREE.
 *
 * A top recipe of at most MAX_SEGMENTS_V2 entries where an entry of
 * INVFS_ALGO_CHILD names no data block (pba == 0) and block_id indexes a
 * trailing CHLD table of child-recipe BLAKE3 addresses. The whole reason the
 * form exists is that nothing else can walk from the top to the children's
 * data, so the FREE path must recurse -- that is what this gate proves, on a
 * real volume with real allocated blocks:
 *
 *   1. a flat recipe serializes byte-identical through every entry point
 *      (serialize / serialize_win(NULL) / serialize_tail(NULL)), so no
 *      pre-WP635 volume changes on disk
 *   2. a CHLD blob round-trips: children() finds it, windows() REFUSES it
 *      (a caller that walked reference entries as payloads would free pba 0)
 *   3. vol_free_recipe_blocks on a tree frees the CHILD's blocks -- the leg
 *      the whole change is for: the top names no blocks at all, so a flat
 *      walk frees nothing and reports success
 *   4. a child that does not resolve fails the free (-1) instead of leaving
 *      the space silently allocated
 *   5. a truncated CHLD section is malformed, not "no children"
 *   6. nesting past INVFS_AST_CHILD_DEPTH_MAX is refused, not followed
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "invarifs.h"
#include "volume_internal.h"

static int checks = 0, failures = 0;

static void ok(int cond, const char *msg)
{
    checks++;
    if (!cond) { failures++; fprintf(stderr, "  FAIL: %s\n", msg); }
}

/* A recipe entry that names a child instead of a block. */
static void child_entry(invfs_ast_block_entry *e, uint64_t len, uint32_t id)
{
    memset(e, 0, sizeof *e);
    e->length = (uint32_t)len;
    e->algo = INVFS_ALGO_CHILD;
    e->pba = 0;                       /* a reference owns no data block */
    e->block_id = id;                 /* index into the CHLD table */
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    const size_t seg = INVFS_BLOCK_SIZE, sz = seg * 4;
    uint8_t *data = NULL, *fblob = NULL, *tblob = NULL, *cblob = NULL;
    size_t flen = 0, tlen = 0, cblen = 0;
    uint8_t caddr[INVFS_RECIPE_ADDR_LEN];
    uint8_t caddr2[INVFS_RECIPE_ADDR_LEN], taddr2[INVFS_RECIPE_ADDR_LEN];
    invfs_ast_child_ref cref;
    invfs_ast_block_entry top;
    const invfs_ast_block_entry *cents = NULL;
    size_t n_cents = 0;
    const invfs_ast_block_entry *tents = NULL;
    size_t tn = 0;
    invfs_ast_hdr chdr, th_t;
    invfs_inode ino;
    invfs_volume *v = NULL;
    int err = 0;

    data = (uint8_t *)malloc(sz);
    if (!data) return 2;
    for (size_t i = 0; i < sz; i++)
        data[i] = (uint8_t)((i * 17 + (i >> 7)) & 0xFF);

    /* 1: the flat blob is unchanged by this WP -- all three entry points */
    {
        invfs_ast_block_entry e;
        uint8_t *a = NULL, *b = NULL, *c = NULL;
        size_t al = 0, bl = 0, cl = 0;
        memset(&e, 0, sizeof e);
        e.length = 4096; e.pba = 100;
        ok(vol_ast_recipe_serialize(sz, &e, 1, &a, &al) == 0 &&
           vol_ast_recipe_serialize_win(sz, &e, 1, NULL, 0, &b, &bl) == 0 &&
           vol_ast_recipe_serialize_tail(sz, &e, 1, NULL, 0, NULL, 0,
                                         &c, &cl) == 0 &&
           al == bl && al == cl && memcmp(a, b, al) == 0 &&
           memcmp(a, c, al) == 0,
           "a flat recipe is byte-identical through all three serializers");
        ok(vol_ast_recipe_serialize_tail(sz, &e, 1, NULL, 1, &cref, 1,
                                         &a, &al) == -1,
           "windows and a child table are refused together");
        free(a); free(b); free(c);
    }

    snprintf(img, sizeof img, "%s/invfs-recipe-tree-test.img", dir);
    {
        char cmd[600];
        snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 40 >/dev/null 2>&1", img);
        if (system(cmd) != 0) { fprintf(stderr, "mkfs failed\n"); return 2; }
    }
    v = vol_open(img, &err);
    if (!v) { fprintf(stderr, "vol_open failed (%d)\n", err); return 2; }

    /* A real file, so the child level names real blocks with real refcounts.
     * `fblob`/`cents` stay alive: for a v1/v2 recipe the entries POINT INTO
     * the blob, so freeing the blob first would leave the test reading freed
     * memory and "prove" whatever happened to be there.
     *
     * The control is a SECOND identical file freed FLAT: whatever this build
     * actually returns to the pool for one 16 KiB LZ4 segment (one entry with
     * a multi-block extent, so the count is not simply "one block per entry")
     * is the number the tree has to match. Calibrating against the flat path
     * is the difference between asserting the recursion and asserting a
     * constant that happens to be right today. */
    {
        uint64_t id = vol_write_bulk(v, "child_data.bin", data, sz, NULL);
        ok(id != 0, "wrote the child's data file");
        if (!id) return 1;
        ok(vol_inode_get(v, id, &ino) > 0, "read the file's inode row");
    }
    ok(vol_recipe_load(v, ino.recipe_addr, &fblob, &flen) == 0 && fblob,
       "loaded the file's own recipe");
    ok(vol_ast_recipe_parse(fblob, flen, &chdr, &cents, &n_cents) == 0 &&
       cents && n_cents > 0, "parsed the child-level entries");

    /* the child IS a recipe blob: same header, same 32 B entries, absolute
     * file_offset -- so the reader binary-searches inside it directly. */
    ok(vol_ast_recipe_serialize_tail(sz, cents, (uint32_t)n_cents,
                                     NULL, 0, NULL, 0, &cblob, &cblen) == 0,
       "serialized the child recipe");
    ok(vol_recipe_store(v, cblob, cblen, caddr) == 0, "stored the child recipe");
    memset(&cref, 0, sizeof cref);
    memcpy(cref.addr, caddr, INVFS_RECIPE_ADDR_LEN);
    child_entry(&top, sz, 0);
    ok(vol_ast_recipe_serialize_tree(sz, &top, 1, &cref, 1, &tblob, &tlen) == 0,
       "serialized the reference recipe");

    /* 2: the tail scanner reports a child table where there are no windows */
    {
        const invfs_ast_window_entry *w = NULL;
        const uint8_t *chld = NULL;
        uint32_t nw = 7, nc = 0;
        ok(vol_ast_recipe_parse(tblob, tlen, &th_t, &tents, &tn) == 0 && tn == 1,
           "the reference recipe parses");
        ok(vol_ast_recipe_windows(tblob, tlen, &th_t, &w, &nw) == -1 && w == NULL,
           "a CHLD recipe is refused by the window scanner, not read as "
           "'no windows'");
        ok(vol_ast_recipe_children(tblob, tlen, &th_t, &chld, &nc) == 0 &&
           nc == 1 && chld && memcmp(chld, caddr, INVFS_RECIPE_ADDR_LEN) == 0,
           "the child table round-trips");
        /* 5: truncating the section is malformed, never "zero children" */
        ok(vol_ast_recipe_children(tblob, tlen - 1, &th_t, &chld, &nc) == -1,
           "a truncated CHLD section is refused");
    }

    /* 3: THE LEG -- and its control.
     *
     * The top names no block at all, so a FLAT walk over it frees nothing and
     * reports success: the shape that looks like a reclaim and leaks every
     * byte of the file. The child's blocks are reachable only by recursing.
     *
     * The control is a second file of identical shape freed FLAT, because
     * "how many blocks does one 16 KiB LZ4 segment give back" is a property of
     * this build (one entry, multi-block extent, extent accounting), not a
     * constant the test should invent. Both files are written BEFORE either is
     * freed, so their pbas are distinct -- free one first and the pool hands
     * its address straight back to the next write, which would leave the
     * second file sharing a pba with the first and the refcount at 2. */
    {
        uint64_t id2, fl0, fl1, ft0, ft1;
        uint8_t *f2 = NULL, *c2 = NULL, *t2 = NULL;
        size_t f2len = 0, c2len = 0, t2len = 0;
        uint8_t ca2[INVFS_RECIPE_ADDR_LEN], ta2[INVFS_RECIPE_ADDR_LEN];
        const invfs_ast_block_entry *e2 = NULL;
        size_t n2 = 0;
        invfs_ast_hdr h2;
        invfs_ast_child_ref r2;
        invfs_inode in2;

        for (size_t i = 0; i < sz; i++)
            data[i] = (uint8_t)((i * 29 + 11) & 0xFF);
        id2 = vol_write_bulk(v, "child_data2.bin", data, sz, NULL);
        ok(id2 != 0, "wrote the second child's data file");
        ok(vol_inode_get(v, id2, &in2) > 0, "read its inode row");
        ok(vol_recipe_load(v, in2.recipe_addr, &f2, &f2len) == 0 && f2,
           "loaded the second file's recipe");
        ok(vol_ast_recipe_parse(f2, f2len, &h2, &e2, &n2) == 0 && n2 > 0,
           "parsed the second file's entries");
        ok(e2[0].pba != cents[0].pba,
           "the two files hold distinct blocks (the premise of the control)");
        ok(vol_ast_recipe_serialize_tail(sz, e2, (uint32_t)n2, NULL, 0,
                                         NULL, 0, &c2, &c2len) == 0,
           "serialized the second child recipe");
        ok(vol_recipe_store(v, c2, c2len, ca2) == 0, "stored the second child");
        memset(&r2, 0, sizeof r2);
        memcpy(r2.addr, ca2, INVFS_RECIPE_ADDR_LEN);
        child_entry(&top, sz, 0);
        ok(vol_ast_recipe_serialize_tree(sz, &top, 1, &r2, 1, &t2, &t2len) == 0,
           "serialized the second reference recipe");
        ok(vol_recipe_store(v, t2, t2len, ta2) == 0, "stored the second top");

        /* the ref map is a cache over the live set (volume_internal.h:1530)
         * and two writes have landed since it was built */
        pba_ref_invalidate(v);
        pba_ref_ensure(v);
        ok(pba_ref_count(v, e2[0].pba) == 1,
           "the second file's blocks have exactly one live reference");
        ok(pba_ref_count(v, cents[0].pba) == 1,
           "the first file's blocks have exactly one live reference");

        ft0 = v->free_blocks;
        ok(vol_free_recipe_blocks(v, ta2, 0) == 0,
           "freeing a reference recipe succeeds");
        ft1 = v->free_blocks;
        fl0 = v->free_blocks;
        ok(vol_free_recipe_blocks(v, ino.recipe_addr, 0) == 0,
           "the flat control recipe frees");
        fl1 = v->free_blocks;
        printf("  free blocks: reference tree +%llu, flat control +%llu\n",
               (unsigned long long)(ft1 - ft0),
               (unsigned long long)(fl1 - fl0));
        ok(ft1 > ft0, "the reference recipe returned blocks to the pool");
        ok((ft1 - ft0) == (fl1 - fl0),
           "the tree freed exactly what the flat control freed -- those blocks "
           "are reachable only through the CHLD level, so an equal, non-zero "
           "delta is the recursion happening");
        vol_ast_recipe_parsed_free(&h2, e2);
        free(f2); free(c2); free(t2);
    }

    /* 4: a child address that resolves to nothing is a hard failure */
    {
        uint8_t bad[INVFS_RECIPE_ADDR_LEN], *t3 = NULL;
        size_t t3len = 0;
        uint64_t f0;
        memset(bad, 0x5A, sizeof bad);
        memset(&cref, 0, sizeof cref);
        memcpy(cref.addr, bad, INVFS_RECIPE_ADDR_LEN);
        child_entry(&top, sz, 0);
        ok(vol_ast_recipe_serialize_tree(sz, &top, 1, &cref, 1, &t3, &t3len) == 0,
           "built a reference recipe to a non-existent child");
        ok(vol_recipe_store(v, t3, t3len, taddr2) == 0, "stored it");
        free(t3);
        f0 = v->free_blocks;
        ok(vol_free_recipe_blocks(v, taddr2, 0) == -1,
           "an unresolvable child fails the free instead of reporting success");
        ok(v->free_blocks == f0, "and nothing was freed");
    }

    /* 6: a child that carries a child table of its own is past the one level
     * the format and the lifecycle implement. The depth guard is what stops a
     * hand-built blob from recursing forever, and it fires BEFORE anything is
     * freed -- a partial free reported as a failure is the honest answer. */
    {
        uint8_t *nb = NULL, *mb = NULL, *t4 = NULL;
        size_t nblen = 0, mblen = 0, t4len = 0;
        uint64_t f0;
        invfs_ast_child_ref nref;
        uint8_t maddr[INVFS_RECIPE_ADDR_LEN];
        invfs_ast_block_entry ne;
        /* the file's own flat recipe is the innermost child */
        ok(vol_ast_recipe_serialize_tail(sz, cents, (uint32_t)n_cents,
                                         NULL, 0, NULL, 0, &nb, &nblen) == 0,
           "built the innermost flat child");
        ok(vol_recipe_store(v, nb, nblen, caddr2) == 0, "stored it");
        free(nb);
        /* middle: one reference to the flat child */
        memset(&nref, 0, sizeof nref);
        memcpy(nref.addr, caddr2, INVFS_RECIPE_ADDR_LEN);
        child_entry(&ne, sz, 0);
        ok(vol_ast_recipe_serialize_tree(sz, &ne, 1, &nref, 1, &mb, &mblen) == 0,
           "built the middle reference recipe");
        ok(vol_recipe_store(v, mb, mblen, maddr) == 0, "stored the middle");
        free(mb);
        /* top: one reference to the middle -- that is depth 2 */
        memset(&nref, 0, sizeof nref);
        memcpy(nref.addr, maddr, INVFS_RECIPE_ADDR_LEN);
        child_entry(&ne, sz, 0);
        ok(vol_ast_recipe_serialize_tree(sz, &ne, 1, &nref, 1, &t4, &t4len) == 0,
           "built the two-level reference recipe");
        ok(vol_recipe_store(v, t4, t4len, taddr2) == 0, "stored the two-level top");
        free(t4);
        f0 = v->free_blocks;
        ok(vol_free_recipe_blocks(v, taddr2, 0) == -1,
           "a two-level tree is refused, not followed (depth guard)");
        ok(v->free_blocks == f0,
           "and the refused free left the pool untouched");
    }

    vol_ast_recipe_parsed_free(&chdr, cents);
    vol_ast_recipe_parsed_free(&th_t, tents);
    free(fblob);
    free(cblob);
    free(tblob);
    free(data);
    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
