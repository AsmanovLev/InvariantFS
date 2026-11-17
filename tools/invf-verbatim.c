/* invf-verbatim -- READ-ONLY prototype: measure the VERBATIM (logical) space.
 *
 * Why this exists (ADR-012): today's deduplication keys on the STORED bytes
 * of a segment, so the volume has no idea how much *unique logical content* it
 * actually holds -- the number every content-addressed design is built on.
 * This tool computes it without touching the on-disk format and without
 * writing a single byte:
 *
 *   verbatim space = the set of distinct 4 KiB chunks of DECODED file data
 *
 * and prints it next to the physical footprint and the logical total. That
 * difference is the deduplication potential of the volume measured by CONTENT,
 * which is exactly what the current stored-byte key cannot see (a re-compressed
 * copy, or a copy cut at different segment boundaries, counts twice today).
 *
 * It also prints, per file, how many of its chunks already exist elsewhere (the
 * "unique contribution"), so the biggest redundancy sources are visible.
 *
 * Deliberately NOT a filesystem change: no recipe is read differently, nothing
 * is validated differently, nothing is written. Use it to size the decision
 * before the format moves.
 *
 * Usage: invf-verbatim [-q] [--top N] [--leaf-kb K] [--max-leaves N] <image>
 *
 * exit 0 = report printed, 1 = volume error, 2 = usage.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "volume.h"
#include "invarifs.h"
#include "blake3.h"

#define VV_LEAF_DEFAULT  4096u
#define VV_MAX_LEAVES_DEFAULT (4u * 1024u * 1024u)   /* 4M leaves ~ 16 GiB logical */

/* Open-addressing set of 32-byte chunk hashes. Only membership matters. */
typedef struct {
    uint8_t *keys;        /* cap * 32 bytes */
    uint8_t *used;        /* cap bytes */
    size_t   cap, n;
} hset;

static int hs_init(hset *h, size_t cap)
{
    h->cap = cap;
    h->n = 0;
    h->keys = (uint8_t *)calloc(cap, 32);
    h->used = (uint8_t *)calloc(cap, 1);
    return (h->keys && h->used) ? 0 : -1;
}

static void hs_free(hset *h)
{
    free(h->keys);
    free(h->used);
    h->keys = NULL;
    h->used = NULL;
    h->cap = h->n = 0;
}

static size_t hs_slot(const hset *h, const uint8_t *k)
{
    uint64_t v;
    memcpy(&v, k, sizeof v);
    size_t i = (size_t)(v & (h->cap - 1));
    while (h->used[i] && memcmp(h->keys + i * 32, k, 32) != 0)
        i = (i + 1) & (h->cap - 1);
    return i;
}

/* returns 1 when the key was NEW (inserted), 0 when it already existed,
 * -1 when the table is full (caller stops inserting). */
static int hs_add(hset *h, const uint8_t *k)
{
    size_t i = hs_slot(h, k);
    if (h->used[i])
        return 0;
    if ((h->n + 1) * 10 > h->cap * 7)   /* 70% load: stay ahead of the probe */
        return -1;
    memcpy(h->keys + i * 32, k, 32);
    h->used[i] = 1;
    h->n++;
    return 1;
}

typedef struct {
    invfs_volume *v;
    hset         *set;
    uint32_t      leaf;          /* leaf size in bytes */
    int           quiet;
    size_t        top_n;
    int           capped;         /* the table hit its cap: counts are a bound */

    uint64_t      files;
    uint64_t      logical;         /* sum of file sizes */
    uint64_t      leaves_total;    /* chunks hashed */
    uint64_t      leaves_unique;   /* chunks inserted */
    uint64_t      bytes_unique;    /* leaves_unique * leaf (tail rounded up) */
    uint64_t      bytes_dup;       /* data whose chunk already existed */
} vv_ctx;

typedef struct {
    uint64_t uniq, dup, size;
    char     name[256];
} vv_file;

static vv_file *g_top;
static size_t   g_top_n;

static void top_add(const char *name, uint64_t size, uint64_t uniq, uint64_t dup)
{
    size_t i;
    if (!g_top || g_top_n == 0)
        return;
    /* keep the array sorted by dup descending, small n (--top) */
    for (i = 0; i < g_top_n; i++) {
        if (dup > g_top[i].dup) {
            size_t j = g_top_n - 1;
            while (j > i) { g_top[j] = g_top[j - 1]; j--; }
            g_top[i].dup = dup;
            g_top[i].uniq = uniq;
            g_top[i].size = size;
            snprintf(g_top[i].name, sizeof g_top[i].name, "%s", name ? name : "?");
            return;
        }
    }
    (void)size; (void)uniq;
}

static int vv_cb(invfs_volume *v, uint64_t inode_id, const char *name, void *ctx_)
{
    vv_ctx *c = (vv_ctx *)ctx_;
    invfs_inode in;
    uint8_t *buf;
    uint64_t off = 0, f_uniq = 0, f_dup = 0, f_leaves = 0;
    size_t chunk = 1u << 20;   /* read in 1 MiB windows */

    if (vol_inode_get(v, inode_id, &in) != 1)
        return 0;
    if (in.type != INVFS_ITYP_REG || in.size == 0)
        return 0;
    if (name && (unsigned char)name[0] == 0x01)
        return 0;                      /* engine bookkeeping, not a user file */

    buf = (uint8_t *)malloc(chunk);
    if (!buf)
        return 0;

    c->files++;
    c->logical += in.size;

    while (off < in.size) {
        uint64_t want = in.size - off;
        int got;
        uint64_t p;
        if (want > chunk)
            want = chunk;
        got = vol_read_range(v, inode_id, off, (size_t)want, buf);
        if (got <= 0)
            break;
        for (p = 0; p < (uint64_t)got; p += c->leaf) {
            size_t len = (size_t)((uint64_t)got - p < c->leaf ? (uint64_t)got - p
                                                              : c->leaf);
            uint8_t h[32];
            blake3_hasher hx;
            int rc;
            blake3_hasher_init(&hx);
            blake3_hasher_update(&hx, buf + p, len);
            blake3_hasher_finalize(&hx, h, sizeof h);
            c->leaves_total++;
            f_leaves++;
            rc = c->capped ? 0 : hs_add(c->set, h);
            if (rc == 1) {
                c->leaves_unique++;
                c->bytes_unique += c->leaf;   /* tail is rounded up: an upper bound */
                f_uniq += len;
            } else if (rc == 0) {
                c->bytes_dup += len;
                f_dup += len;
            } else {
                c->capped = 1;                /* table full: stop inserting */
            }
        }
        off += (uint64_t)got;
    }
    free(buf);
    top_add(name, in.size, f_uniq, f_dup);
    (void)f_leaves;
    return 0;
}

int main(int argc, char **argv)
{
    const char *img = NULL;
    uint32_t leaf = VV_LEAF_DEFAULT;
    size_t max_leaves = VV_MAX_LEAVES_DEFAULT;
    vv_ctx c;
    invfs_volume *v;
    invfs_volume_stats st;
    int err = 0, i, quiet = 0;
    size_t cap = 1;

    memset(&c, 0, sizeof c);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) { quiet = 1; continue; }
        if (!strcmp(argv[i], "--top") && i + 1 < argc) {
            c.top_n = (size_t)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(argv[i], "--leaf-kb") && i + 1 < argc) {
            uint64_t kb = strtoull(argv[++i], NULL, 10);
            if (kb < 1 || kb > 4096) { fprintf(stderr, "leaf-kb out of range\n"); return 2; }
            leaf = (uint32_t)(kb * 1024);
            continue;
        }
        if (!strcmp(argv[i], "--max-leaves") && i + 1 < argc) {
            max_leaves = (size_t)strtoull(argv[++i], NULL, 10);
            continue;
        }
        if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        img = argv[i];
    }
    if (!img) {
        fprintf(stderr, "usage: %s [-q] [--top N] [--leaf-kb K] [--max-leaves N] <image>\n",
                argv[0]);
        return 2;
    }

    c.leaf = leaf;
    c.quiet = quiet;
    while (cap < max_leaves * 2) cap <<= 1;   /* <= 50% load: keep probing short */
    c.set = (hset *)calloc(1, sizeof *c.set);
    if (!c.set || hs_init(c.set, cap) != 0) { fprintf(stderr, "out of memory\n"); return 1; }
    if (c.top_n) {
        g_top_n = c.top_n;
        g_top = (vv_file *)calloc(g_top_n, sizeof *g_top);
    }

    v = vol_open(img, &err);
    if (!v) { fprintf(stderr, "open failed err=%d\n", err); return 1; }

    memset(&st, 0, sizeof st);
    if (vol_compute_stats(v, &st) != 0)
        fprintf(stderr, "warning: stats walk failed; physical numbers omitted\n");

    if (vol_iter_live_inodes(v, vv_cb, &c) != 0)
        fprintf(stderr, "warning: live-inode walk reported an error; counts are partial\n");

    printf("\nVERBATIM SPACE (read-only prototype, ADR-012)\n");
    printf("  leaf                     : %u B\n", c.leaf);
    printf("  files                    : %" PRIu64 "\n", c.files);
    printf("  logical total            : %10.2f MiB\n", c.logical / 1048576.0);
    printf("  chunks hashed            : %" PRIu64 "\n", c.leaves_total);
    printf("  distinct chunks          : %" PRIu64 "%s\n", c.leaves_unique,
           c.capped ? "  (table cap reached: lower bound)" : "");
    printf("  VERBATIM SPACE           : %10.2f MiB%s\n", c.bytes_unique / 1048576.0,
           c.capped ? "  (lower bound)" : "");
    printf("  already-seen data        : %10.2f MiB (%.1f%% of logical)\n",
           c.bytes_dup / 1048576.0,
           c.logical ? 100.0 * (double)c.bytes_dup / (double)c.logical : 0.0);
    if (st.raw_used_bytes || st.shadow_used_bytes || st.text_used_bytes) {
        double phys = (double)(st.raw_used_bytes + st.shadow_used_bytes +
                               st.text_used_bytes) / 1048576.0;
        printf("  physical data            : %10.2f MiB\n", phys);
        printf("  verbatim / physical      : %10.2f x  (compression+dedupe in effect)\n",
               phys ? (c.bytes_unique / 1048576.0) / phys : 0.0);
        if (st.unclaimed_used_bytes)
            printf("  unclaimed                : %10.2f MiB\n",
                   st.unclaimed_used_bytes / 1048576.0);
    }
    if (g_top) {
        size_t k, shown = 0;
        printf("\n  top redundant files (already-seen bytes):\n");
        for (k = 0; k < g_top_n; k++) {
            if (!g_top[k].size) continue;
            printf("    %-42.42s %9.2f MiB of %9.2f MiB (%.0f%%)\n",
                   g_top[k].name, g_top[k].dup / 1048576.0,
                   g_top[k].size / 1048576.0,
                   g_top[k].size ? 100.0 * (double)g_top[k].dup / (double)g_top[k].size : 0.0);
            if (++shown >= g_top_n) break;
        }
        if (!shown)
            printf("    (none: every chunk was new)\n");
    }
    printf("\n");

    vol_close(v);
    if (c.set) { hs_free(c.set); free(c.set); }
    free(g_top);
    return 0;
}
