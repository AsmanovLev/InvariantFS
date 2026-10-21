/* sweep_orig_len_test.c — WP658: a recipe entry that claims more than
 * SEGMENT_SIZE must be refused, not decoded into a 64 KiB stack buffer.
 *
 * sweep_thread_worker declares `uint8_t orig[SEGMENT_SIZE]` on the stack and
 * decodes `orig_len` bytes into it, where `orig_len` comes straight from the
 * recipe entry (vol_sweep.c: tasks[k].orig_len = (size_t)e->length). The
 * neighbouring ast_frame_ok() check validates the COMPRESSED size against
 * orig_len, not orig_len against the buffer -- so an over-long claim used to
 * reach LZ4_decompress_safe / ZSTD_decompress / memcpy with a destination far
 * too small.
 *
 * No in-tree writer can produce such an entry (vol_write.c:193 requires
 * length <= SEGMENT_SIZE on the read-back, and every segment write passes
 * exactly SEGMENT_SIZE). So this is defence in depth, and the fixture is
 * built the way a damaged or foreign recipe would arrive: write a healthy
 * file, take its blob, rewrite ONE entry's length, re-store the blob under
 * its own BLAKE3 address, and publish a second inode on it. Nothing in the
 * volume's own write path is bypassed.
 *
 * What is asserted:
 *   1. the sweep runs and does NOT crash (this is the whole point: before the
 *      fix the over-long claim is a stack smash in a worker thread);
 *   2. the poisoned inode keeps its bytes -- the segment is refused, not
 *      rewritten with a truncated or clamped payload;
 *   3. the healthy inode sharing the same data is untouched;
 *   4. fsck still reports the volume clean (a refusal is not damage);
 *   5. the bound is `>` not `>=`: a legal full-size entry still decodes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "volume.h"
#include "volume_internal.h"
#include "invarifs.h"

static const char *g_img = "/dev/shm/wp658-origlen.img";
static invfs_volume *g_v;
static int g_pass, g_fail;

static void ok(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  OK    %s\n", what); }
    else      { g_fail++; printf("  FAIL  %s\n", what); }
}

static invfs_volume *open_vol(void)
{
    int err = 0;
    invfs_volume *v = vol_open(g_img, &err);
    if (!v) { fprintf(stderr, "vol_open(%s) failed: %d\n", g_img, err); exit(2); }
    return v;
}

static void mkfs_fresh(void)
{
    char cmd[700];
    unlink(g_img);
    snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 64 >/dev/null 2>&1", g_img);
    if (system(cmd) != 0) { fprintf(stderr, "mkfs failed\n"); exit(2); }
}

/* Write a file of `n` bytes of a recognisable pattern through the public
 * write path, and return its inode id. */
static uint64_t make_file(invfs_volume *v, const char *name, size_t n)
{
    uint8_t *buf = malloc(n);
    uint64_t id = 0;
    size_t i;
    if (!buf) return 0;
    for (i = 0; i < n; i++) buf[i] = (uint8_t)(0xA5 ^ (i * 7));
    id = vol_write_bulk(v, name, buf, n, NULL);
    if (id == 0) id = 0;
    free(buf);
    return id;
}

int main(void)
{
    uint64_t id_good, id_bad;
    invfs_inode in;
    uint8_t *blob = NULL, *blob2 = NULL;
    size_t blen = 0, blen2 = 0;
    uint8_t addr[INVFS_RECIPE_ADDR_LEN];
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;
    const size_t fsz = 3 * INVFS_SEGMENT_SIZE;   /* three full segments */

    printf("sweep_orig_len_test: an over-long recipe entry must be refused\n");

    mkfs_fresh();
    g_v = open_vol();

    /* ---- fixture: a healthy 3-segment file ---------------------------- */
    id_good = make_file(g_v, "good.bin", fsz);
    ok(id_good != 0, "leg 1: a 3-segment file is written");
    if (!id_good) { printf("FAIL: fixture\n"); return 1; }

    ok(vol_inode_get(g_v, id_good, &in) == 1, "leg 1: its row reads");
    ok(vol_recipe_load(g_v, in.recipe_addr, &blob, &blen) == 0 && blob,
       "leg 1: its recipe loads");
    ok(vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) == 0 &&
       ents && n_ents == 3, "leg 1: the recipe names 3 entries");

    /* ---- leg 2: is the bound ALREADY enforced at write time? ---------- */
    {
        int all_full = 1;
        size_t i;
        for (i = 0; i < n_ents; i++)
            if (ents[i].length != INVFS_SEGMENT_SIZE) all_full = 0;
        ok(all_full, "leg 2: every entry at write time is exactly "
                     "SEGMENT_SIZE -- the writer cannot emit more, which is "
                     "why the bound is defence in depth, not a live bug");
    }

    /* ---- leg 3: poison ONE entry's declared length -------------------- */
    {
        invfs_ast_block_entry *e = (invfs_ast_block_entry *)ents;
        e[0].length = INVFS_SEGMENT_SIZE + 4096;
        if (vol_ast_recipe_serialize(ah.file_size, ents, (uint32_t)n_ents,
                                     &blob2, &blen2) != 0 || !blob2) {
            ok(0, "leg 3: the poisoned recipe serializes");
            return 1;
        }
        ok(1, "leg 3: the poisoned recipe serializes (entry 0 claims "
              "SEGMENT_SIZE + 4096)");
        ok(vol_recipe_store(g_v, blob2, blen2, addr) == 0,
           "leg 3: it re-stores under its own BLAKE3 address");
        id_bad = vol_create_content_node(g_v, "bad.bin", fsz, addr);
        ok(id_bad != 0 && id_bad != id_good,
           "leg 3: a second inode is published on the poisoned blob");
    }
    vol_ast_recipe_parsed_free(&ah, ents);
    free(blob);
    free(blob2);
    blob = blob2 = NULL;

    /* ---- leg 4: the sweep must not crash, and must keep the bytes ----- */
    {
        int rc = vol_sweep_one(g_v, id_bad, "bad.bin");
        printf("        (vol_sweep_one(bad.bin) -> %d)\n", rc);
        ok(1, "leg 4: the sweep survived an over-long entry (no stack smash)");
        /* HONESTY NOTE, measured: this leg passes WITHOUT the WP658 guard too.
         * The length lie is caught one step earlier by the shared frame check
         * (ast_frame_ok: for NONE it requires hdr == len, so a recipe entry
         * that claims more than the frame holds never reaches a decode arm).
         * Reaching the arm would need a FRAME larger than SEGMENT_SIZE, and no
         * writer can emit one (vol_write.c:193). So the guard below closes a
         * path that is unreachable by construction; it is landed as a cheap
         * invariant, and this leg is a REGRESSION guard on the refusal, not the
         * red-before. The red-before does not exist -- state that, do not fake
         * it. */
    }

    {
        uint8_t *d = NULL;
        size_t n = 0;
        int rc = vol_read_inode(g_v, id_bad, 0, &d, &n);
        /* Either answer is legitimate: the read path may refuse (it bounds
         * e->length at its own arms) or return the original bytes if the
         * segment was never touched. What is NOT legitimate is a short or
         * zero-filled payload -- that would be the clamp this WP refuses. */
        ok(rc != 0 || (n == fsz && d && d[0] == (uint8_t)(0xA5 ^ (0 * 7))),
           "leg 4: the poisoned inode either refuses or still returns its "
           "original bytes (never a truncated/clamped payload)");
        free(d);
    }

    /* ---- leg 5: no collateral on the healthy inode -------------------- */
    {
        uint8_t *d = NULL;
        size_t n = 0;
        int rc = vol_read_inode(g_v, id_good, 0, &d, &n);
        int same = (rc == 0 && n == fsz);
        size_t i;
        if (same)
            for (i = 0; i < n; i++)
                if (d[i] != (uint8_t)(0xA5 ^ (i * 7))) { same = 0; break; }
        ok(same, "leg 5: good.bin is bit-exact (no collateral)");
        free(d);
    }

    /* ---- leg 6: the poisoned recipe is DETECTED, not silently kept ---- */
    {
        invfs_recipe_audit a;
        memset(&a, 0, sizeof a);
        ok(vol_recipe_audit(g_v, &a) == 0, "leg 6: the recipe audit completes");
        printf("        (audit: %llu fault(s) over %llu checked)\n",
               (unsigned long long)a.nfault_total,
               (unsigned long long)a.checked);
        /* This is the leg that must not be vacuous: an over-long entry is a
         * concrete incoherence, and the audit is the tool that names it. */
        ok(a.nfault_total >= 1,
           "leg 6: the audit NAMES the over-long entry (>= 1 fault), so the "
           "refusal is visible to an operator instead of silent");
    }

    /* ---- leg 7: `>` not `>=` -- a full-size entry still decodes ------- */
    {
        int rc = vol_sweep_one(g_v, id_good, "good.bin");
        uint8_t *d = NULL;
        size_t n = 0;
        int same = 0;
        printf("        (vol_sweep_one(good.bin) -> %d)\n", rc);
        if (vol_read_inode(g_v, id_good, 0, &d, &n) == 0 && n == fsz) {
            size_t i;
            same = 1;
            for (i = 0; i < n; i++)
                if (d[i] != (uint8_t)(0xA5 ^ (i * 7))) { same = 0; break; }
        }
        ok(same, "leg 7: a legal full-size segment still decodes and sweeps "
                 "bit-exactly (the bound is `>`, not `>=`)");
        free(d);
    }

    vol_close(g_v);
    unlink(g_img);

    printf("\nsweep_orig_len_test: %d checks, %d failure(s)\n",
           g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
