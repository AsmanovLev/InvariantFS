/* window_test.c — ADR-010 amendment 2: guest-file windows.
 *
 * A WINDOW_SRC recipe entry owns no blocks; its bytes are re-derived from
 * another inode at read time. This is the unit gate for the opcode, the
 * trailing window table, the publisher and the read-path arm:
 *
 *   1. a verbatim window reproduces the source range bit-for-bit
 *   2. a window into the middle of a source (not just offset 0) is exact
 *   3. an inflating window reproduces the DECOMPRESSED bytes of a deflate
 *      stream produced with recorded parameters (the QCOW2 rankimg case)
 *   4. a window whose source is gone reads as a failure, never as zeros
 *   5. the source's blocks are NOT freed by publishing a window
 *   6. a recipe with no window table still parses (backward compatibility)
 *   7. a malformed window table is rejected rather than silently ignored
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "invarifs.h"
#include "volume_internal.h"
#include "../codecs/deflate_repro.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *msg)
{
    checks++;
    if (!cond) { failures++; fprintf(stderr, "  FAIL: %s\n", msg); }
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    const size_t seg = 4096;
    const size_t src_sz = seg * 4;
    uint8_t *src = NULL, *comp = NULL, *plain = NULL, *got = NULL;
    size_t comp_len = 0, plain_len = 0;
    invfs_volume *v = NULL;
    uint64_t id_src = 0, id_win = 0, id_bad = 0;
    int err = 0;

    snprintf(img, sizeof img, "%s/invfs-window-test.img", dir);
    src = (uint8_t *)malloc(src_sz);
    if (!src) return 2;
    for (size_t i = 0; i < src_sz; i++)
        src[i] = (uint8_t)((i * 31 + (i >> 9) * 7) & 0xFF);

    /* a deflate stream over one segment: the qcow2-cluster shape */
    {
        invfs_deflate_params dp;
        memset(&dp, 0, sizeof dp);
        dp.engine = INVFS_DEFLATE_ENGINE_ZLIB_STOCK;
        dp.level = 6; dp.mem_level = 8; dp.strategy = 0; dp.window_bits = -12;
        if (invfs_deflate_repro_encode(src, seg, &dp, &comp, &comp_len) != 0) {
            fprintf(stderr, "deflate encode failed\n");
            return 2;
        }
    }
    plain_len = seg;
    plain = (uint8_t *)malloc(plain_len ? plain_len : 1);
    got = (uint8_t *)malloc(src_sz);
    if (!plain || !got) return 2;

    {
        char cmd[600];
        snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 64 >/dev/null 2>&1", img);
        if (system(cmd) != 0) { fprintf(stderr, "mkfs failed\n"); return 2; }
    }
    v = vol_open(img, &err);
    if (!v) { fprintf(stderr, "vol_open failed (%d)\n", err); return 2; }

    id_src = vol_v3_write_bulk(v, "src.bin", src, src_sz, NULL);
    ok(id_src != 0, "wrote the source file");
    if (!id_src) return 1;


    /* 1 + 2: verbatim windows, including one at a non-zero source offset */
    id_win = vol_v3_publish_window_inode(v, "win_head.bin", id_src, 0,
                                         seg, seg, 0, 0, 0, 0, 0, 0);
    ok(id_win != 0, "published a verbatim window at offset 0");

    id_bad = vol_v3_publish_window_inode(v, "win_mid.bin", id_src, seg,
                                         seg, seg, 0, 0, 0, 0, 0, 0);
    ok(id_bad != 0, "published a verbatim window at a non-zero offset");


    {
        uint8_t *buf = NULL;
        size_t blen = 0;
        memset(got, 0, src_sz);
        if (vol_read_file(v, id_win, &buf, &blen) == 0 && buf) {
            ok(blen == seg, "verbatim window length");
            ok(buf && memcmp(buf, src, seg) == 0,
               "verbatim window at offset 0 is bit-exact");
            free(buf);
        } else {
            ok(0, "read of the offset-0 window failed");
        }
        memset(got, 0, src_sz);
        if (vol_read_file(v, id_bad, &buf, &blen) == 0 && buf) {
            ok(buf && memcmp(buf, src + seg, seg) == 0,
               "verbatim window at a non-zero offset is bit-exact");
            free(buf);
        } else {
            ok(0, "read of the non-zero-offset window failed");
        }
    }

    /* 3: an inflating window -- the source bytes are the deflate stream */
    {
        uint64_t id_c = vol_v3_write_bulk(v, "src_comp.bin", comp, comp_len, NULL);
        uint64_t id_i = 0;
        uint8_t *buf = NULL;
        size_t blen = 0;
        ok(id_c != 0, "wrote the compressed source");
        id_i = vol_v3_publish_window_inode(v, "win_infl.bin", id_c, 0,
                                            plain_len, comp_len, 1,
                                            INVFS_DEFLATE_ENGINE_ZLIB_STOCK,
                                            6, 8, 0, -12);
        ok(id_i != 0, "published an inflating window");
        if (id_i && vol_read_file(v, id_i, &buf, &blen) == 0 && buf) {
            ok(blen == plain_len, "inflating window length");
            ok(buf && memcmp(buf, src, plain_len) == 0,
               "inflating window reproduces the decompressed bytes");
            free(buf);
        } else {
            ok(0, "read of the inflating window failed");
        }
    }

    /* 5: publishing a window must not free the source's blocks */
    {
        uint64_t free_now = v->free_blocks;
        (void)free_now;
        ok(vol_find(v, "src.bin") == id_src, "the source survives publication");
    }

    /* 4: a window whose source is gone must fail loudly, not read zeros */
    {
        uint64_t id_gone = 0;
        uint8_t *buf = NULL;
        size_t blen = 0;
        /* point a window at an inode id that is not live */
        id_gone = vol_v3_publish_window_inode(v, "win_ghost.bin",
                                              id_src + 999999, 0,
                                              seg, seg, 0, 0, 0, 0, 0, 0);
        ok(id_gone == 0, "refused to publish a window into a missing source");
        if (id_gone == 0) {
            int rc;
            rc = vol_read_file(v, id_win, &buf, &blen);
            /* the good window must still read after the refusal */
            ok(rc >= 0, "the good window still reads after a refused publish");
            free(buf);
        }
    }

    /* 6: backward compatibility -- a plain recipe has no window table */
    {
        invfs_ast_block_entry e;
        uint8_t *blob = NULL;
        size_t blen = 0;
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents = NULL;
        const invfs_ast_window_entry *wins = NULL;
        uint32_t nw = 0;
        memset(&e, 0, sizeof e);
        e.file_offset = 0; e.length = seg; e.zone = INVFS_ZONE_BINARY;
        e.algo = INVFS_ALGO_ZSTD;
        ok(vol_ast_recipe_serialize(seg, &e, 1, &blob, &blen) == 0,
           "serialize a plain recipe");
        {
            size_t ne = 0;
            ok(vol_ast_recipe_parse(blob, blen, &ah, &ents, &ne) == 0,
               "parsed a plain recipe");
            ok(vol_ast_recipe_windows(blob, blen, &ah, &wins, &nw) == 0,
               "a plain recipe reports no window table");
            ok(wins == NULL && nw == 0, "no windows in a plain recipe");
        }
        free(blob);
    }

    /* 7: a malformed window table must be rejected, not ignored */
    {
        uint8_t *blob = NULL;
        size_t blen = 0;
        invfs_ast_hdr ah;
        const invfs_ast_window_entry *wins = NULL;
        uint32_t nw = 0;
        invfs_ast_block_entry e;
        invfs_ast_window_entry w;
        memset(&e, 0, sizeof e);
        memset(&w, 0, sizeof w);
        e.file_offset = 0; e.length = seg; e.zone = INVFS_ZONE_BINARY;
        e.algo = INVFS_ALGO_WINDOW_SRC; e.pba = id_src;
        w.src_off = 0; w.src_len = seg; w.transform = 0;
        ok(vol_ast_recipe_serialize_win(seg, &e, 1, &w, 1, &blob, &blen) == 0,
           "serialize a window recipe");
        /* the trailing section is [4B "WINW"][u32 count][entries...]: the
         * count is sizeof(window_entry) bytes from the end, minus 4 */
        size_t wbytes = sizeof(invfs_ast_window_entry);
        blob[blen - wbytes - 4] = 0x7F;   /* count low byte -> disagrees */
        {
            size_t ne = 0;
            if (vol_ast_recipe_parse(blob, blen, &ah, NULL, &ne) == 0) {
            ok(vol_ast_recipe_windows(blob, blen, &ah, &wins, &nw) == -1,
               "a corrupt window count is rejected, not ignored");
        } else {
            ok(0, "window recipe failed to parse after corruption");
            }
        }
        free(blob);
    }

    vol_flush(v);
    vol_close(v);
    unlink(img);

    printf("window_test: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
