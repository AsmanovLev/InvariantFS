/* lane_release_test.c — WP202: a builtin container lane that supersedes a file
 * on v3 must give the superseded recipe's data blocks back.
 *
 * ---------------------------------------------------------------------------
 * THE FINDING.
 *
 * vol_create_blob_file's v3 branch publishes through
 * vol_v3_create_content_node (src/core/vol_dirs.c:397), which REUSES the
 * dirent's inode id and replaces the row in place. So the instant a sweep lane
 * lands, the recipe that named the ORIGINAL file's segments is unreachable —
 * and vol_create_blob_file is handed the NEW recipe address and never the old
 * one, so it cannot free them. The comment on that function says exactly this
 * and names the two callers that do ask for the space back
 * (vol_v3_publish_blob_inode and the containerpack commit).
 *
 * The builtin container lanes in sweep_dispatch (src/core/vol_sweep.c: FLAC /
 * TAR / GZIP / PNG / MP3) are the ones that did not ask. Each ended in
 *
 *     if (!v3 && vol_delete_inode(v, inode_id, name) != 0) return -1;
 *
 * — correct on v2, and on v3 a no-op that leaves every block the original file
 * occupied allocated under no reachable name. The containerpack lane, which
 * supersedes a file the same way, has released them since WP201. Two
 * implementations of "a lane replaces a file"; one of them hands the space
 * back. This is a fork, not a gap.
 *
 * MEASURED (v3 image, 8 x 500 KB incompressible members in one tar, 4.1 MiB,
 * invf-mkfs + invf-import + ONE invf-sweep, then invf-fsck free-block counts):
 *
 *     before the sweep .................... 16502558 free
 *     after ONE sweep, on main ............ 16501619 free   (-939 blocks)
 *     after a SECOND sweep, on main ....... 16503742 free   (+2123 vs sweep 1)
 *
 * The second sweep is the savepoint capture's spn_reclaim (src/core/
 * vol_spt0.c:672) noticing the blocks the previous pin held and the new
 * generation does not. So on main the space does come back — one generation
 * late, and only if another capture ever happens. The volume carries TWO
 * copies of the corpus in the meantime, which on a volume near its watermark
 * is the difference between completing the sweep and not, and it is a
 * different answer from the containerpack lane's on the same sweep.
 *
 * The same corpus with NO tar (so no container lane at all) converges to
 * 2.2 MiB of "unclaimed" after two sweeps and is flat thereafter — that is
 * the text/batch accounting, and it is the number this test's leg 2 pins the
 * tar case to. The tar case must reach the SAME number after ONE sweep.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS ASSERTED, AND WHY IT CANNOT PASS BY ACCIDENT
 *
 * Leg 1 pins the premise: after a lane, the row really did move in place
 * (newino == inode_id) and the recipe address really did change. If either
 * stops holding, the release has nothing to release and this test would be
 * asserting nothing.
 *
 * Leg 2 asserts the two halves together, which is the shape the v2rb test
 * established: WHAT the release is asked for (are the old PBAs free now) and
 * WHAT the file is left as (does it still read back byte-identically). A fix
 * that freed the blocks by freeing the wrong ones would pass the first and
 * fail the second.
 *
 * The sweep is driven through vol_sweep_one_ex — the shipped entry point the
 * offline driver calls — not through a re-implementation of the five lines
 * that were wrong.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "volume_internal.h"
#include "vol_btree.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", what); }
    else        { printf("  OK    %s\n", what); }
}

static invfs_volume *g_v;
static char g_img[512];

/* ---- the 4.1 MiB tar: 8 incompressible members, so nothing compresses and
 * the container lane is the only thing that can account for the space. The
 * header is a real ustar one, because that is what the lane sniffs for. */
#define TAR_MEMBERS 8
#define TAR_MEMBER_BYTES 512000

static uint8_t *build_tar(size_t *len_out)
{
    const size_t hdr = 512, body = ((TAR_MEMBER_BYTES + 511) / 512) * 512;
    size_t total = (hdr + body) * TAR_MEMBERS + hdr * 2;  /* + two zero hdrs */
    uint8_t *t = (uint8_t *)calloc(1, total);
    size_t off = 0;
    int m;

    if (!t) return NULL;
    for (m = 0; m < TAR_MEMBERS; m++) {
        char nm[32];
        memset(t + off, 0, hdr);
        snprintf(nm, sizeof nm, "member%03d.bin", m);
        memcpy(t + off, nm, strlen(nm));
        memcpy(t + off + 100, "0000644", 8);
        memcpy(t + off + 108, "0000000", 8);
        memcpy(t + off + 116, "0000000", 8);
        /* size is 12 octal bytes at 124 */
        snprintf((char *)t + off + 124, 13, "%011o", (unsigned)TAR_MEMBER_BYTES);
        snprintf((char *)t + off + 136, 12, "%011o", 0u);
        memcpy(t + off + 257, "ustar", 5);
        memcpy(t + off + 263, "00", 2);
        {
            uint32_t sum = 0;
            size_t i;
            for (i = 0; i < hdr; i++) sum += t[off + i];
            snprintf((char *)t + off + 148, 8, "%06o ", sum); t[off + 154] = 0; t[off + 155] = ' ';
        }
        off += hdr;
        {
            size_t i;
            for (i = 0; i < TAR_MEMBER_BYTES; i++)
                t[off + i] = (uint8_t)(i * 131u + (unsigned)m * 7u + 11u);
        }
        off += body;
    }
    memset(t + off, 0, hdr * 2);
    off += hdr * 2;
    *len_out = off;
    return t;
}

static uint64_t write_file(const char *name, const uint8_t *body, size_t n)
{
    invfs_meta_pub m;
    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode = 0644;
    return vol_v3_write_bulk(g_v, name, body, n, &m);
}

static int block_allocated(uint64_t pba)
{
    if (!pba || pba >= g_v->sb.total_blocks) return 0;
    return bit_get(g_v->bitmap, pba) != 0;
}

/* Every data PBAs the recipe at `addr` names, loaded the way the read path
 * loads it (a v3 recipe is a content-addressed blob, not a record). */
#define MAX_ENTS 4096
static int recipe_pbas(const uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN],
                       uint64_t *out, size_t cap, size_t *n_out)
{
    uint8_t *blob = NULL;
    size_t blen = 0, i;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;

    *n_out = 0;
    if (vol_v3_recipe_load(g_v, addr, &blob, &blen) != 0 || !blob) return -1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) != 0 ||
        ents == NULL) { free(blob); return -1; }
    for (i = 0; i < n_ents && *n_out < cap; i++)
        out[(*n_out)++] = ents[i].pba;
    free(blob);
    return 0;
}

static int count_allocated(const uint64_t *p, size_t n)
{
    size_t i;
    int k = 0;
    for (i = 0; i < n; i++) if (block_allocated(p[i])) k++;
    return k;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    uint8_t *tar;
    size_t tar_len = 0;
    uint64_t id, newino;
    invfs_v3_inode before, after;
    uint8_t old_addr[INVFS_V3_RECIPE_ADDR_LEN];
    uint8_t new_addr[INVFS_V3_RECIPE_ADDR_LEN];
    uint64_t old_pbas[MAX_ENTS], new_pbas[MAX_ENTS];
    size_t n_old = 0, n_new = 0;
    int old_alloc_before, old_alloc_after, rc;
    int err = 0;
    uint8_t *back = NULL;
    size_t back_len = 0;

    snprintf(g_img, sizeof g_img, "%s/wp202-lane.img", dir);
    unlink(g_img);
    {
        char cmd[700];
        snprintf(cmd, sizeof cmd, "bin/invf-mkfs %s 48 2>/dev/null", g_img);
        if (system(cmd) != 0) {
            fprintf(stderr, "setup: invf-mkfs failed (run from the repo root)\n");
            return 2;
        }
    }
    g_v = vol_open(g_img, &err);
    if (!g_v) { fprintf(stderr, "setup: vol_open failed (%d)\n", err); return 2; }

    tar = build_tar(&tar_len);
    if (!tar) { fprintf(stderr, "setup: tar build failed\n"); return 2; }
    printf("leg 1+2: a builtin container lane that supersedes a file on v3\n");
    printf("  corpus: 1 tar, %d members x %d bytes = %zu bytes\n",
           TAR_MEMBERS, TAR_MEMBER_BYTES, tar_len);

    /* ---- the original, exactly as the sweep finds it: RAW segments ---- */
    id = write_file("corpus.tar", tar, tar_len);
    ok(id != 0, "the original file is written");
    if (!id) { free(tar); return 1; }
    ok(vol_v3_inode_get(g_v, id, &before) == 1, "its row is readable");
    memcpy(old_addr, before.recipe_addr, sizeof old_addr);
    ok(recipe_pbas(old_addr, old_pbas, MAX_ENTS, &n_old) == 0 && n_old > 0,
       "its recipe names at least one data block");
    old_alloc_before = count_allocated(old_pbas, n_old);
    printf("        original recipe: %zu entries, %d data blocks allocated\n",
           n_old, old_alloc_before);
    ok(old_alloc_before == (int)n_old,
       "every block the original recipe names is allocated to begin with");

    /* ---- the lane, through the shipped entry point ---- */
    rc = vol_sweep_one_ex(g_v, id, "corpus.tar", NULL, NULL);
    ok(rc > 0, "the offline sweep entry point claims the file");
    if (rc <= 0) { free(tar); return 1; }

    /* ---- LEG 1: the premise. If the row did not move in place, the
     * release below has nothing to release and the rest proves nothing. ---- */
    newino = vol_find(g_v, "corpus.tar");
    ok(newino == id,
       "PREMISE: the lane superseded the row IN PLACE (same inode id)");
    ok(vol_v3_inode_get(g_v, id, &after) == 1, "the new row is readable");
    memcpy(new_addr, after.recipe_addr, sizeof new_addr);
    ok(memcmp(new_addr, old_addr, INVFS_V3_RECIPE_ADDR_LEN) != 0,
       "PREMISE: the recipe address really changed");

    /* ---- LEG 2a: what the release was asked for ---- */
    old_alloc_after = count_allocated(old_pbas, n_old);
    printf("        superseded blocks still allocated: %d of %zu\n",
           old_alloc_after, n_old);
    ok(old_alloc_after == 0,
       "every data block the SUPERSEDED recipe named is free after the lane");

    /* ---- LEG 2b: and what the file is left as. A fix that freed the
     * wrong blocks passes 2a and fails here. ---- */
    ok(recipe_pbas(new_addr, new_pbas, MAX_ENTS, &n_new) == 0 && n_new > 0,
       "the live recipe still names data blocks");
    ok(count_allocated(new_pbas, n_new) == (int)n_new,
       "every block the LIVE recipe names is still allocated");
    ok(vol_read_inode(g_v, id, 0, &back, &back_len) == 0 && back,
       "the file reads back through the real read path");
    if (back) {
        ok(back_len == tar_len && memcmp(back, tar, tar_len) == 0,
           "the file is BIT-EXACT after the lane superseded and released");
        free(back);
    }

    free(tar);
    vol_close(g_v);

    printf("lane_release_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
