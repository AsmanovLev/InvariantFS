/* src/cli/dedupe-sim.c — what a dedupe policy would ACTUALLY free, in blocks.
 *
 * Every number the dedupe debate has produced so far is a LOGICAL ratio: how many
 * bytes are identical under model X. That is not what the allocator gets back. A
 * block comes back only when its last owner stops naming it, so the interesting
 * quantity is `used` -- the union of reachable physical blocks -- before and after
 * a policy is applied. This tool measures that on a real volume, from real
 * recipes, and reports it in blocks.
 *
 * What it does, per inode with content:
 *   1. vol_inode_get -> recipe_addr
 *   2. vol_recipe_load + vol_ast_recipe_parse -> the entries (file_offset, length,
 *      algo, pba) that describe the file's physical mapping
 *   3. seg_extent_checked(pba) -> how many 4 KiB blocks that extent really holds
 *   4. the hash of the DECODED stream (vol_read_range), which is the identity a
 *      whole-file policy needs and which no recipe field gives: recipe_addr
 *      addresses the serialized recipe, and two identical files written separately
 *      have different recipes because their PBAs differ
 *
 * Then it reports, for one snapshot:
 *
 *   baseline        distinct PBAs and their blocks, i.e. what is used now
 *   whole-file      identical (size, decoded hash) groups; for every loser, the
 *                   blocks of extents whose ONLY owner is that loser -- the ones
 *                   that would really come back
 *   owners          the owner histogram, which is what decides whether any
 *                   range-level scheme can free anything at all
 *
 * and it prints the logical duplicate bytes next to the freed blocks, because the
 * gap between those two numbers IS the granularity question.
 *
 * Usage: invf-dedupe-sim <image> [--max-files N] [--no-hash]
 *        --no-hash skips the decoded-stream hashing (baseline + owners only).
 *
 * MEASURED, first run (2026-11-26): the repository imported as a corpus (2.2 GB of
 * source tree, 1270 files with content, 0.12 GiB logical):
 *
 *   before the existing sweep   2872 PBAs, 21806 blocks, owners 1:2872
 *   398 files duplicate an earlier one, 53.2 MiB of logical duplicates
 *   invf-sweep                  "merged 1148 segments, freed 8091 blocks (31.6 MiB)"
 *   after                       546 PBAs, 13697 blocks, owners 1:532 2:3 4+:11,
 *                               1799 recipe entries naming those 546 extents
 *
 * The two numbers side by side are the whole argument of this file: 53.2 MiB of
 * identical bytes existed, and 31.6 MiB came back -- the gap is granularity (an
 * extent returns only when its last owner lets go) plus what the sweep's repack
 * costs. Every ratio quoted before this was logical.
 *
 * Policy W, on the SAME volume AFTER the existing sweep ran (so it is the gain
 * beyond today s dedupe, which is the baseline that matters):
 *
 *   used 13697 blocks -> 11158, B = 18.54% of used blocks returned
 *   E = 9 of 546 extents fully freed (1.6%), 11 extents still shared
 *
 * and that pair is the warning Astra gave, measured: 18.5% of the blocks came back
 * from 1.6% of the extents, because the sweep had repacked small files into large
 * batch extents. B and E cannot stand in for each other -- a high E of tiny
 * one-block extents can be worth less than a low E of megabytes.
 *
 * Not modelled yet, deliberately: extent splitting and remainder re-packing (the
 * `S` column), donor pinning (V vs P), and CDC candidate ranges. Those need the
 * candidate set and the packer's rounding rules; this tool establishes the
 * denominator they would be measured against.
 */
#include "volume_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- tiny open-addressing map: pba -> owners, and hash -> first inode ---- */
typedef struct { uint64_t pba; uint32_t owners; uint64_t blocks; uint8_t touched; } own_ent;
typedef struct { own_ent *v; size_t cap, n; } own_map;

static void own_init(own_map *m, size_t cap)
{
    m->cap = cap; m->n = 0;
    m->v = calloc(cap, sizeof *m->v);
    if (!m->v) { fprintf(stderr, "oom\n"); exit(1); }
}

static void own_grow(own_map *m)
{
    own_map t;
    size_t i;
    own_init(&t, m->cap * 2);
    for (i = 0; i < m->cap; i++) {
        if (!m->v[i].pba) continue;
        size_t j = (size_t)m->v[i].pba & (t.cap - 1);
        while (t.v[j].pba) j = (j + 1) & (t.cap - 1);
        t.v[j] = m->v[i];
        t.n++;
    }
    free(m->v);
    *m = t;
}

static own_ent *own_find(own_map *m, uint64_t pba)
{
    size_t i = (size_t)pba & (m->cap - 1);
    while (m->v[i].pba) {
        if (m->v[i].pba == pba) return &m->v[i];
        i = (i + 1) & (m->cap - 1);
    }
    return NULL;
}

static own_ent *own_add(own_map *m, uint64_t pba, uint64_t blocks)
{
    own_ent *e = own_find(m, pba);
    if (e) { e->owners++; return e; }
    size_t i = (size_t)pba & (m->cap - 1);
    while (m->v[i].pba) i = (i + 1) & (m->cap - 1);
    m->v[i].pba = pba;
    m->v[i].owners = 1;
    m->v[i].blocks = blocks;
    if (++m->n * 10 >= m->cap * 7) own_grow(m);
    return own_find(m, pba);
}

/* ---- one inode's content summary ---------------------------------------- */
typedef struct {
    uint64_t ino, size, log_bytes, blocks;
    uint64_t hash;             /* decoded-stream hash (0 = not computed) */
    uint64_t own_blocks;       /* blocks of extents this inode owns alone */
    uint32_t text_entries;     /* entries pointing into a TEXT batch */
    uint64_t *pbas;            /* the extents this inode names ... */
    uint32_t *blks;            /* ... and how many blocks each holds */
    size_t npba;
} finfo;

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

typedef struct {
    invfs_volume *v;
    finfo *files;
    size_t n, cap;
    own_map own;
    int do_hash;
    uint64_t log_bytes, entries, windows, ref_entries, text_entries;
} sim_ctx;

static int walk_cb(void *ctx_, const char *path, uint64_t ino, uint32_t type,
                   uint64_t size, int64_t mtime)
{
    sim_ctx *c = (sim_ctx *)ctx_;
    invfs_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr hdr;
    const invfs_ast_block_entry *ents = NULL;
    size_t nents = 0;
    finfo f;
    size_t i;

    (void)path; (void)mtime;
    if (type != INVFS_ITYP_REG) return 0;
    if (size == 0) return 0;
    if (vol_inode_get(c->v, ino, &in) != 1) return 0;

    memset(&f, 0, sizeof f);
    f.ino = ino;
    f.size = size;

    if (in.recipe_addr[0] != 0 &&
        vol_recipe_load(c->v, in.recipe_addr, &blob, &blen) == 0 &&
        vol_ast_recipe_parse(blob, blen, &hdr, &ents, &nents) == 0) {
        for (i = 0; i < nents; i++) {
            uint64_t plen = 0;
            if (!ents[i].pba) continue;
            /* TEXT-zone entries name a block of a BATCH, which the batch's
             * owner holds on behalf of every member -- the inode does not own
             * it and the engine never retires it from an inode's recipe
             * (pba_ref skips the zone for exactly this reason). Counting it as
             * the inode's own block is how the first W number came out at
             * 18.54% while the implemented leg retired zero blocks on a swept
             * corpus: the model was charging the loser for blocks it never
             * held. Reported separately, never folded into the owner map. */
            if (ents[i].zone == INVFS_ZONE_TEXT) {
                c->text_entries++;
                f.text_entries++;
                f.log_bytes += ents[i].length;
                continue;
            }
            if (ents[i].algo == INVFS_ALGO_WINDOW_SRC) {
                /* a window owns no blocks itself; it names another inode */
                c->windows++;
                continue;
            }
            if (seg_extent_checked(c->v, ents[i].pba, &plen) != 0) continue;
            own_add(&c->own, ents[i].pba, plen);
            f.pbas = realloc(f.pbas, (f.npba + 1) * sizeof *f.pbas);
            f.blks = realloc(f.blks, (f.npba + 1) * sizeof *f.blks);
            if (!f.pbas || !f.blks) { fprintf(stderr, "oom\n"); exit(1); }
            f.pbas[f.npba] = ents[i].pba;
            f.blks[f.npba] = (uint32_t)plen;
            f.npba++;
            f.blocks += plen;
            f.log_bytes += ents[i].length;
            c->entries++;
        }
        vol_ast_recipe_parsed_free(&hdr, ents);
    }
    free(blob);

    if (c->do_hash && f.log_bytes) {
        /* the decoded stream, in 1 MiB pieces, so a big file does not need to fit
         * in memory just to be hashed */
        uint8_t *buf = malloc(1u << 20);
        if (buf) {
            uint64_t off = 0, h = 1469598103934665603ull;
            while (off < f.size) {
                uint64_t want = f.size - off;
                if (want > (1u << 20)) want = 1u << 20;
                uint64_t got = (uint64_t)vol_read_range(c->v, ino, off, want, buf);
                if (got != want) { h = 0; break; }
                for (uint64_t k = 0; k < got; k++) { h ^= buf[k]; h *= 1099511628211ull; }
                off += got;
            }
            f.hash = h;
            free(buf);
        }
    }
    (void)fnv1a;

    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 1024;
        c->files = realloc(c->files, c->cap * sizeof *c->files);
        if (!c->files) { fprintf(stderr, "oom\n"); exit(1); }
    }
    c->files[c->n++] = f;
    c->log_bytes += f.log_bytes;
    return 0;
}

int main(int argc, char **argv)
{
    sim_ctx c;
    invfs_volume *v;
    int err = 0;
    int i;
    size_t max_files = 0;
    uint64_t base_blocks = 0, own_once_blocks = 0, dup_logic = 0, loser_blocks = 0;
    uint64_t dup_files = 0, blocks_freed = 0;
    size_t k;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <image> [--max-files N] [--no-hash]\n", argv[0]);
        return 2;
    }
    memset(&c, 0, sizeof c);
    c.do_hash = 1;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--no-hash")) c.do_hash = 0;
        else if (!strcmp(argv[i], "--max-files") && i + 1 < argc)
            max_files = (size_t)strtoull(argv[++i], NULL, 10);
    }

    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "cannot open %s (err %d)\n", argv[1], err); return 1; }
    c.v = v;
    own_init(&c.own, 1u << 16);

    if (vol_walk_strict(v, walk_cb, &c) != 0)
        fprintf(stderr, "warning: the walk did not complete; numbers are a lower bound\n");

    /* baseline: distinct PBAs and their blocks */
    for (k = 0; k < c.own.cap; k++) {
        if (!c.own.v[k].pba) continue;
        base_blocks += c.own.v[k].blocks;
        if (c.own.v[k].owners == 1) own_once_blocks += c.own.v[k].blocks;
    }

    /* whole-file identity: group by (size, hash); losers' singly-owned extent
     * blocks are what a whole-file policy would actually return */
    if (c.do_hash && c.n > 1 && (!max_files || c.n <= max_files)) {
        uint64_t *key = malloc(c.n * sizeof *key);
        int *mark = calloc(c.n, sizeof *mark);
        if (key && mark) {
            for (k = 0; k < c.n; k++)
                key[k] = c.files[k].hash ? (c.files[k].hash ^ (c.files[k].size * 0x9E3779B97F4A7C15ull)) : 0;
            for (size_t a = 0; a < c.n; a++) {
                if (mark[a] || !key[a]) continue;
                uint64_t group_logic = 0, group_losers = 0;
                for (size_t b = a + 1; b < c.n; b++) {
                    if (mark[b] || key[b] != key[a]) continue;
                    mark[b] = 1;
                    group_logic += c.files[b].log_bytes;
                    group_losers++;
                }
                if (group_losers) {
                    dup_files += group_losers;
                    dup_logic += group_logic;
                }
            }
            free(key); free(mark);
        }
    }

    /* an extent owned by exactly one loser: countable only per extent, so report
     * the singly-owned total and the duplicate bytes; the delta is the point */
    (void)loser_blocks; (void)blocks_freed;

    printf("%s\n", argv[1]);
    printf("  files with content      : %zu\n", c.n);
    printf("  logical bytes           : %.2f GiB\n", c.log_bytes / 1073741824.0);
    printf("  recipe entries          : %llu (windows skipped: %llu)\n",
           (unsigned long long)c.entries, (unsigned long long)c.windows);
    printf("  TEXT-zone entries       : %llu (owned by their batch, never retired "
           "from an inode)\n", (unsigned long long)c.text_entries);
    printf("  distinct physical PBAs  : %zu\n", c.own.n);
    printf("  used blocks (4 KiB)     : %llu  = %.2f GiB\n",
           (unsigned long long)base_blocks, base_blocks * 4096.0 / 1073741824.0);
    printf("  blocks owned by ONE inode: %llu = %.2f GiB\n",
           (unsigned long long)own_once_blocks, own_once_blocks * 4096.0 / 1073741824.0);
    printf("  extent sharing ratio    : %.2f logical bytes per used byte\n",
           base_blocks ? (double)c.log_bytes / (base_blocks * 4096.0) : 0.0);
    if (c.do_hash) {
        printf("  whole-file duplicate groups contribute:\n");
        printf("    files that duplicate an earlier one : %llu\n", (unsigned long long)dup_files);
        printf("    their LOGICAL bytes                 : %.1f MiB\n", dup_logic / 1048576.0);
        printf("    (freed blocks are a lower number: an extent comes back only when\n"
               "     its last owner stops naming it -- see the owner histogram below)\n");
    }
    /* ---- policy `W`: every duplicate loses its own copy --------------------
     * A whole-file duplicate is the only case with no partial coverage, so the V/P
     * distinction collapses here: the loser's whole recipe is replaced by the
     * donor's. What it would FREE is the real question, and an extent comes back
     * only when its last owner lets go -- so model exactly that: drop the loser's
     * reference from every extent it names and see which extents fall to zero. */
    if (c.do_hash && c.n > 1) {
        uint64_t *key = malloc(c.n * sizeof *key);
        int *loser = calloc(c.n, sizeof *loser);
        if (key && loser) {
            uint64_t used_after = base_blocks, freed_blocks = 0;
            uint64_t freed_ext = 0, stuck_ext = 0, stuck_blocks = 0;
            uint64_t meta_saved = 0;
            size_t a, b, e;

            for (k = 0; k < c.n; k++)
                key[k] = c.files[k].hash ? (c.files[k].hash ^ (c.files[k].size * 0x9E3779B97F4A7C15ull)) : 0;
            for (a = 0; a < c.n; a++) {
                if (loser[a] || !key[a]) continue;
                for (b = a + 1; b < c.n; b++)
                    if (!loser[b] && key[b] == key[a]) loser[b] = 1;
            }
            for (b = 0; b < c.n; b++) {
                if (!loser[b]) continue;
                for (e = 0; e < c.files[b].npba; e++) {
                    own_ent *o = own_find(&c.own, c.files[b].pbas[e]);
                    if (!o || !o->owners) continue;
                    o->owners--;
                    o->touched = 1;
                    if (o->owners == 0) { freed_blocks += o->blocks; freed_ext++; }
                }
            }
            /* a shared recipe blob becomes garbage once its last inode names another
             * one; the inode row already stores recipe_addr, so pointing it at the
             * donor's costs nothing */
            for (b = 0; b < c.n; b++) if (loser[b]) meta_saved++;

            /* an extent that keeps at least one owner after the policy did NOT
             * come back; count it once, not once per loser that named it */
            for (e = 0; e < c.own.cap; e++)
                if (c.own.v[e].pba && c.own.v[e].touched && c.own.v[e].owners > 0) {
                    stuck_ext++;
                    stuck_blocks += c.own.v[e].blocks;
                }
            used_after = base_blocks - freed_blocks;
            printf("\n\n  policy W (whole-file dedupe: each duplicate points at its donor)\n");
            printf("    used blocks before / after : %llu / %llu  (%.2f -> %.2f GiB)\n",
                   (unsigned long long)base_blocks, (unsigned long long)used_after,
                   base_blocks * 4096.0 / 1073741824.0, used_after * 4096.0 / 1073741824.0);
            printf("    B  = (used0-usedW)/used0   : %.2f%%\n",
                   base_blocks ? 100.0 * freed_blocks / base_blocks : 0.0);
            printf("    E  = extents fully freed   : %llu of %zu (%.1f%%)\n",
                   (unsigned long long)freed_ext, c.own.n,
                   c.own.n ? 100.0 * freed_ext / c.own.n : 0.0);
            printf("    extents still shared      : %llu (%llu blocks held by another owner)\n",
                   (unsigned long long)stuck_ext, (unsigned long long)stuck_blocks);
            printf("    recipe rows that stop naming their own recipe blob: %llu\n",
                   (unsigned long long)meta_saved);
            free(key); free(loser);
        } else { free(key); free(loser); }
    }

    /* Extents with MORE THAN ONE owner, named. Without a dedupe pass having
     * run, an extent that two live inodes both name is an allocator handing
     * out a block somebody still references -- the shape a data block takes
     * when it is overwritten under a live recipe. Printing the pbas and their
     * owners is what turns "an extent stopped decoding" into "these two\n     * inodes claim the same block". */
    if (c.own.n) {
        uint64_t shown = 0;
        size_t e;
        printf("\n  multi-owner extents (no dedupe ran unless policy W below says so):\n");
        for (e = 0; e < c.own.cap && shown < 12; e++) {
            size_t f, listed = 0;
            if (!c.own.v[e].pba || c.own.v[e].owners < 2) continue;
            printf("    pba %llu  %llu blocks  owners=%u  bitmap=%s  named by:",
                   (unsigned long long)c.own.v[e].pba,
                   (unsigned long long)c.own.v[e].blocks,
                   c.own.v[e].owners,
                   (c.v->bitmap && bit_get(c.v->bitmap, c.own.v[e].pba)) ?
                       "allocated" : "FREE");
            for (f = 0; f < c.n && listed < 4; f++) {
                size_t q;
                for (q = 0; q < c.files[f].npba; q++) {
                    if (c.files[f].pbas[q] == c.own.v[e].pba) {
                        printf(" ino%llu", (unsigned long long)c.files[f].ino);
                        listed++;
                        break;
                    }
                }
            }
            if (c.own.v[e].owners > listed) printf(" (+%u more)",
                                                  c.own.v[e].owners - (unsigned)listed);
            printf("\n");
            shown++;
        }
        if (!shown) printf("    none\n");
    }

    /* The one question a bitmap/row disagreement shows up in: a pba a LIVE row
     * names but whose bitmap bit says FREE. The next allocation then hands it
     * out and overwrites a live segment -- leg5's whole class. */
    if (c.v->bitmap) {
        uint64_t named_free = 0, shown = 0;
        size_t e;
        for (e = 0; e < c.own.cap; e++) {
            if (!c.own.v[e].pba || c.own.v[e].owners == 0) continue;
            if (bit_get(c.v->bitmap, c.own.v[e].pba)) continue;
            named_free++;
            if (shown < 6) {
                printf("    NAMED-BUT-FREE pba=%llu blocks=%llu owners=%u\n",
                       (unsigned long long)c.own.v[e].pba,
                       (unsigned long long)c.own.v[e].blocks,
                       c.own.v[e].owners);
                shown++;
            }
        }
        printf("  named by a live row but marked FREE: %llu extent(s)%s\n",
               (unsigned long long)named_free,
               named_free ? "  <-- THIS is the double-allocation source" : "");
    }

    {
        uint64_t hist[5] = {0, 0, 0, 0, 0};
        for (k = 0; k < c.own.cap; k++) {
            if (!c.own.v[k].pba) continue;
            uint32_t o = c.own.v[k].owners;
            if (o > 4) o = 4;
            hist[o]++;
        }
        printf("  owner histogram         : 1:%llu 2:%llu 3:%llu 4+:%llu\n",
               (unsigned long long)hist[1], (unsigned long long)hist[2],
               (unsigned long long)hist[3], (unsigned long long)hist[4]);
    }
    vol_close(v);
    return 0;
}
