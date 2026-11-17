/* invf-reconsolidate -- READ-ONLY planner for `invf-sweep --reconsolidate`.
 *
 * Why a planner first: the session's chunk size is not a knob. It is
 * INVFS_SEGMENT_SIZE in 64 places in vol_write.c, five of them stack arrays
 * (`uint8_t plain[SEGMENT_SIZE]`), so making it dynamic is a refactor of the
 * write hot path, not a flag. Before that refactor pays for itself we need the
 * number it would buy, measured on the real volume -- and that is exactly what
 * this tool produces.
 *
 * For every candidate file it reports:
 *   current  = the footprint the file occupies NOW (sum of its extents'
 *              physical blocks, i.e. rounding included);
 *   repacked = the footprint the same bytes would take with a target chunk
 *              size, computed by ACTUALLY compressing the data in memory at
 *              the same level (so the estimate is not a model);
 *   saving   = current - repacked.
 *
 * It also labels the content family (text / binary / elfish / container-ish) so
 * the grouping question ("sort blocks by type, like the PPMd batches do") can be
 * answered with numbers instead of taste.
 *
 * Nothing is written, no format is touched, no state is read differently: it
 * uses vol_read_range for the bytes and the recipe entries for the footprint.
 *
 * Usage: invf-reconsolidate [--target-kb K] [--min-kib M] [--top N] [--json] <img>
 */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include <zstd.h>

#include "volume.h"
#include "volume_internal.h"
#include "invarifs.h"
#include "codec.h"

#define RC_MAX_FILE   (256ull << 20)   /* do not plan files bigger than this */

typedef struct {
    uint64_t ino, size, cur, rep;
    int      fam;       /* 0 raw/unknown, 1 text, 2 binary-family, 3 container */
} rc_cand;

static rc_cand *g_c;
static size_t   g_n, g_cap;
static uint32_t g_target;
static uint64_t g_min;
static int      g_json;

/* A file is re-packable here when every entry is a self-contained segment of
 * its OWN bytes: no TEXT-zone batch slices, no ADR-010 windows. Those are
 * already shared with something else, so "repack this file" would either
 * duplicate data or break the sharing. */
static int recipe_is_repackable(invfs_volume *v, uint64_t ino)
{
    invfs_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *e = NULL;
    size_t n = 0, i;
    int ok = 1;

    if (vol_inode_get(v, ino, &in) != 1)
        return 0;
    if (vol_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob) {
        free(blob);
        return 0;
    }
    if (vol_ast_recipe_parse(blob, blen, &ah, &e, &n) != 0 || !e)
        ok = 0;
    else
        for (i = 0; i < n; i++) {
            if (e[i].zone == INVFS_ZONE_TEXT ||
                e[i].algo == INVFS_ALGO_WINDOW_SRC) { ok = 0; break; }
        }
    vol_ast_recipe_parsed_free(&ah, e);
    free(blob);
    return ok;
}

static int fam_of(const uint8_t *d, size_t n, const char *name)
{
    if (n >= 2 && d[0] == 0x1F && d[1] == 0x8B) return 3;         /* gzip */
    if (n >= 262 && !memcmp(d + 257, "ustar", 5)) return 3;        /* tar */
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return 3;     /* png */
    if (n >= 4 && d[0] == 'P' && d[1] == 'K') return 3;            /* zip */
    if (n >= 4 && (d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F'))
        return 2;
    if (invfs_text_family(name, d, n) > 0) return 1;
    return 0;
}

static const char *fam_name(int f)
{
    switch (f) {
    case 1: return "text";
    case 2: return "binary";
    case 3: return "container";
    default: return "raw";
    }
}

/* physical footprint of one file's current extents (rounding included) */
static uint64_t cur_footprint(invfs_volume *v, uint64_t ino)
{
    invfs_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *e = NULL;
    size_t n = 0, i;
    uint64_t sum = 0;

    if (vol_inode_get(v, ino, &in) != 1)
        return 0;
    if (vol_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob) {
        free(blob);
        return 0;
    }
    if (vol_ast_recipe_parse(blob, blen, &ah, &e, &n) == 0 && e) {
        for (i = 0; i < n; i++) {
            uint64_t plen = 0;
            if (!e[i].pba)
                continue;
            if (seg_extent_checked(v, e[i].pba, &plen) != 0 || !plen)
                continue;      /* resident in an unknown shape: skip, not guess */
            sum += plen * INVFS_BLOCK_SIZE;
        }
    }
    vol_ast_recipe_parsed_free(&ah, e);
    free(blob);
    return sum;
}

/* footprint the same bytes would take when every chunk is compressed on its own
 * at the target size -- the real cost of the target, not a formula */
static uint64_t repack_footprint(const uint8_t *d, size_t n)
{
    size_t off = 0, bound = ZSTD_compressBound(g_target);
    void *cb = malloc(bound);
    uint64_t sum = 0;

    if (!cb)
        return 0;
    while (off < n) {
        size_t take = n - off < g_target ? n - off : g_target;
        size_t c = ZSTD_compress(cb, bound, d + off, take, 19);
        if (ZSTD_isError(c)) { sum = 0; break; }
        sum += (uint64_t)((8 + c + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE) *
               INVFS_BLOCK_SIZE;
        off += take;
    }
    free(cb);
    return sum;
}

typedef struct { invfs_volume *v; } rc_ctx;

static int rc_cb(invfs_volume *v, uint64_t ino, const char *name, void *ctx)
{
    rc_ctx *c = (rc_ctx *)ctx;
    invfs_inode in;
    uint8_t *data;
    uint64_t cur, rep;

    (void)c;
    if (vol_inode_get(v, ino, &in) != 1)
        return 0;
    if (in.type != INVFS_ITYP_REG || in.size < g_min || in.size > RC_MAX_FILE)
        return 0;
    if (name && (unsigned char)name[0] == 0x01)
        return 0;
    /* Container siblings (a decomposed payload, its table, its leaf list) are
     * NOT re-packable by this planner: their stored form is already a stream
     * the container lane cut with its own policy, and re-compressing their
     * DECODED bytes at a different chunk size measures something else. They
     * need the extent ladder (ADR-012), not a re-pack. Measured: including them
     * made the plan look 67% WORSE than the status quo on a container corpus. */
    if (name && strchr(name, '!'))
        return 0;
    if (recipe_is_repackable(v, ino) != 1)
        return 0;

    cur = cur_footprint(v, ino);
    if (!cur)
        return 0;                     /* no resolvable extents (e.g. batched) */
    data = (uint8_t *)malloc((size_t)in.size);
    if (!data)
        return 0;
    if (vol_read_range(v, ino, 0, (size_t)in.size, data) != (int)in.size) {
        free(data);
        return 0;
    }
    rep = repack_footprint(data, (size_t)in.size);
    if (g_n == g_cap) {
        size_t nc = g_cap ? g_cap * 2 : 64;
        rc_cand *nn = (rc_cand *)realloc(g_c, nc * sizeof *nn);
        if (!nn) { free(data); return 0; }
        g_c = nn; g_cap = nc;
    }
    g_c[g_n].ino = ino;
    g_c[g_n].size = in.size;
    g_c[g_n].cur = cur;
    g_c[g_n].rep = rep;
    g_c[g_n].fam = fam_of(data, (size_t)(in.size < 4096 ? in.size : 4096), name);
    g_n++;
    free(data);
    (void)ctx;
    return 0;
}

static int cmp_saving(const void *a, const void *b)
{
    const rc_cand *x = (const rc_cand *)a, *y = (const rc_cand *)b;
    int64_t sx = (int64_t)x->cur - (int64_t)x->rep;
    int64_t sy = (int64_t)y->cur - (int64_t)y->rep;
    if (sx != sy) return sx < sy ? 1 : -1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *img = NULL;
    rc_ctx c;
    invfs_volume *v;
    int err = 0, i, top = 10;
    uint64_t sum_cur = 0, sum_rep = 0;
    uint64_t perfam_cur[4] = {0, 0, 0, 0}, perfam_rep[4] = {0, 0, 0, 0};

    g_target = 1024u * 1024u;
    g_min = 1024ull * 1024ull;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) { g_json = 1; continue; }
        if (!strcmp(argv[i], "--target-kb") && i + 1 < argc) {
            uint64_t kb = strtoull(argv[++i], NULL, 10);
            if (kb < 4 || kb > 65536) { fprintf(stderr, "target-kb out of range\n"); return 2; }
            g_target = (uint32_t)(kb * 1024);
            continue;
        }
        if (!strcmp(argv[i], "--min-kib") && i + 1 < argc) {
            g_min = strtoull(argv[++i], NULL, 10) * 1024ull;
            continue;
        }
        if (!strcmp(argv[i], "--top") && i + 1 < argc) {
            top = atoi(argv[++i]);
            continue;
        }
        if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        img = argv[i];
    }
    if (!img) {
        fprintf(stderr, "usage: %s [--target-kb K] [--min-kib M] [--top N] [--json] <image>\n",
                argv[0]);
        return 2;
    }

    v = vol_open(img, &err);
    if (!v) { fprintf(stderr, "open failed err=%d\n", err); return 1; }

    memset(&c, 0, sizeof c);
    c.v = v;
    if (vol_iter_live_inodes(v, rc_cb, &c) != 0)
        fprintf(stderr, "warning: walk reported an error; numbers are partial\n");

    qsort(g_c, g_n, sizeof *g_c, cmp_saving);
    for (i = 0; i < (int)g_n; i++) {
        sum_cur += g_c[i].cur;
        sum_rep += g_c[i].rep;
        perfam_cur[g_c[i].fam] += g_c[i].cur;
        perfam_rep[g_c[i].fam] += g_c[i].rep;
    }

    if (g_json) {
        printf("{\"target_bytes\":%u,\"files\":%zu,\"current_bytes\":%" PRIu64
               ",\"repacked_bytes\":%" PRIu64 "}\n",
               g_target, g_n, sum_cur, sum_rep);
    } else {
        printf("\nRECONSOLIDATION PLAN (read-only; nothing is written)\n");
        printf("  target chunk     : %u KiB\n", g_target / 1024);
        printf("  candidate files  : %zu (>= %" PRIu64 " KiB)\n", g_n, g_min / 1024);
        printf("  current footprint: %10.2f MiB\n", sum_cur / 1048576.0);
        printf("  repacked         : %10.2f MiB\n", sum_rep / 1048576.0);
        printf("  SAVING           : %10.2f MiB (%.1f%%)\n",
               (double)((int64_t)sum_cur - (int64_t)sum_rep) / 1048576.0,
               sum_cur ? 100.0 * ((double)sum_cur - (double)sum_rep) / (double)sum_cur : 0.0);
        printf("  by family        : ");
        for (i = 0; i < 4; i++) {
            if (!perfam_cur[i]) continue;
            printf("%s %.1f->%.1f  ", fam_name(i),
                   perfam_cur[i] / 1048576.0, perfam_rep[i] / 1048576.0);
        }
        printf("\n\n  top files by saving:\n");
        for (i = 0; i < (int)g_n && i < top; i++) {
            int64_t sv = (int64_t)g_c[i].cur - (int64_t)g_c[i].rep;
            if (sv <= 0) break;
            printf("    ino %-8" PRIu64 " %-9s %8.2f -> %8.2f MiB  save %7.2f (%.0f%%)\n",
                   g_c[i].ino, fam_name(g_c[i].fam),
                   g_c[i].cur / 1048576.0, g_c[i].rep / 1048576.0,
                   sv / 1048576.0,
                   g_c[i].cur ? 100.0 * (double)sv / (double)g_c[i].cur : 0.0);
        }
        printf("\n");
    }

    vol_close(v);
    free(g_c);
    return 0;
}
