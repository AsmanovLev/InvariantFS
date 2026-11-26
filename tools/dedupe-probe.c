/* tools/dedupe-probe.c — how much duplicate data is really there, by model.
 *
 * Measures the SAME bytes under three dedupe models plus one misalignment
 * experiment, so that "would CDC find more?" is a number rather than an opinion:
 *
 *   whole     identical files
 *   fixed-4K  fixed 4 KiB blocks aligned to the file start (ZFS/btrfs-style
 *             dedupe, VMware 8K, the thing people mean by "block dedupe")
 *   cdc-8K    content-defined chunking, Gear rolling hash, 2 KiB min / 8 KiB
 *             average / 64 KiB max (Windows Data Deduplication, restic, borg and
 *             every "decomposer")
 *
 * plus: take a file, insert ONE byte at the front, and count how many
 * blocks/chunks each model still recognises -- the misalignment question.
 *
 * MEASURED 2026-11-25 on 1.49 GiB sampled from a 607 GB NTFS data volume (1500
 * files, stride across the tree, 64 KiB .. 32 MiB):
 *
 *   model        unique    duplicate    saved
 *   whole-file   1.48 GiB   0.01 GiB    0.75%
 *   fixed-4K     1.37 GiB   0.13 GiB    8.57%
 *   cdc-8K       1.32 GiB   0.18 GiB   11.80%     (+38% over fixed-4K)
 *
 *   misalignment: a 33 MB DLL, one byte inserted at the front
 *     fixed-4K: 0 of 8100 blocks still matched
 *     cdc-8K  : 3661 of 3663 chunks still matched
 *
 *   compression of the UNIQUE cdc chunks: zstd-3 51.7%, zstd-19 54.0%
 *
 * End to end, dedupe + zstd-3 keeps 0.66 GiB of 1.49 with fixed-4K and 0.64 GiB
 * with CDC: the 38% dedupe advantage collapses to 1.6 points once a codec sees
 * what dedupe missed. That is what ADR-012's "fixed 4 KiB is equivalent to CDC at
 * 8 KiB" is about -- true on VM images, not on mixed user data. See
 * impl_docs/OPEN-ISSUES.md for what to do about it.
 *
 * Build: gcc -O2 -o /tmp/dedupe-probe tools/dedupe-probe.c -lzstd
 * Run:   /tmp/dedupe-probe /path/to/tree 4000 8000000000
 *        (@listfile may be given instead of a directory: "<path>\t<size>" lines,
 *         which avoids an lstat per entry over 9p)
 *
 * 64-bit FNV-1a for chunk identity: collisions are irrelevant at this scale for an
 * estimate, and it is ~1 GB/s instead of ~500 MB/s for a cryptographic hash.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zstd.h>

#define MAXPATH      4096
#define MIN_FILE     (64 * 1024)
#define MAX_PER_FILE (32u * 1024 * 1024)
#define BLOCK        4096u
#define CDC_MIN      2048u
#define CDC_MAX      65536u
#define CDC_MASK     ((1u << 13) - 1)      /* avg 8 KiB */
#define MAXFILES     200000

static uint64_t gear[256];

/* ---- open-addressing hash set of 64-bit keys --------------------------- */
typedef struct { uint64_t *key; uint32_t *len; size_t cap, n; } hset;

static void hs_init(hset *s, size_t cap)
{
    s->cap = cap; s->n = 0;
    s->key = calloc(cap, sizeof *s->key);
    s->len = calloc(cap, sizeof *s->len);
    if (!s->key || !s->len) { fprintf(stderr, "oom\n"); exit(1); }
}
static void hs_grow(hset *s);

static int hs_add(hset *s, uint64_t k, uint32_t len)
{
    size_t i = (size_t)k & (s->cap - 1);
    while (s->len[i]) {
        if (s->key[i] == k) return 0;          /* duplicate */
        i = (i + 1) & (s->cap - 1);
    }
    s->key[i] = k; s->len[i] = len ? len : 1;  /* len 0 would look empty */
    if (++s->n * 10 >= s->cap * 7) hs_grow(s);
    return 1;
}
static void hs_grow(hset *s)
{
    hset t;
    size_t i;
    hs_init(&t, s->cap * 2);
    for (i = 0; i < s->cap; i++)
        if (s->len[i]) hs_add(&t, s->key[i], s->len[i]);
    free(s->key); free(s->len);
    *s = t;
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* ---- file list -------------------------------------------------------- */
typedef struct { char path[MAXPATH]; unsigned long long size; } fent;
static fent *files;
static size_t nfiles;

static void walk(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char p[MAXPATH];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!strcmp(e->d_name, "$RECYCLE.BIN") ||
            !strcmp(e->d_name, "System Volume Information")) continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { walk(p); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        if ((unsigned long long)st.st_size < MIN_FILE) continue;
        if (nfiles >= MAXFILES) continue;
        snprintf(files[nfiles].path, MAXPATH, "%s", p);
        files[nfiles].size = (unsigned long long)st.st_size;
        nfiles++;
    }
    closedir(d);
}

/* ---- the three models ------------------------------------------------- */
/* Compression axis: dedupe removes copies, a codec shrinks what is left, and a
 * decomposer does both. Compressing only the UNIQUE chunks measures exactly the
 * residual a codec would see. */
static ZSTD_CCtx *g_cc3, *g_cc19;
static uint64_t zsto3, zsto19;

static uint64_t uniq_whole, dup_whole, n_whole;
static uint64_t uniq_fixed, dup_fixed, n_fixed;
static uint64_t uniq_cdc,   dup_cdc,   n_cdc;

static void cdc_scan(const uint8_t *p, size_t n, hset *s,
                     uint64_t *uniq, uint64_t *dup, uint64_t *cnt)
{
    size_t i = 0;
    while (i < n) {
        uint64_t h = 0;
        size_t start = i, end = i + CDC_MAX < n ? i + CDC_MAX : n;
        while (i < end) {
            h = (h << 1) + gear[p[i]];
            i++;
            if (i - start >= CDC_MIN && (h & CDC_MASK) == 0) break;
        }
        size_t len = i - start;
        if (hs_add(s, fnv(p + start, len), (uint32_t)len)) {
            *uniq += len;
            if (g_cc3) {
                static uint8_t out[CDC_MAX + 128];
                size_t r = ZSTD_compressCCtx(g_cc3, out, sizeof out, p + start, len, 3);
                if (!ZSTD_isError(r)) zsto3 += r;
                r = ZSTD_compressCCtx(g_cc19, out, sizeof out, p + start, len, 19);
                if (!ZSTD_isError(r)) zsto19 += r;
            }
        } else { *dup += len; }
        (*cnt)++;
    }
}

static void fixed_scan(const uint8_t *p, size_t n, hset *s,
                       uint64_t *uniq, uint64_t *dup, uint64_t *cnt)
{
    size_t off;
    for (off = 0; off < n; off += BLOCK) {
        size_t len = (off + BLOCK <= n) ? BLOCK : n - off;
        if (hs_add(s, fnv(p + off, len), (uint32_t)len)) *uniq += len;
        else *dup += len;
        (*cnt)++;
    }
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "/mnt/d";
    size_t maxfiles = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 4000;
    unsigned long long budget = argc > 3 ? strtoull(argv[3], NULL, 10) : (1ull << 32);
    hset sw, sf, sc;
    uint64_t rng = 0x5EED, read_bytes = 0, seen = 0;
    unsigned long long total_bytes = 0;
    size_t i, step;
    uint8_t *buf = malloc(MAX_PER_FILE);
    char biggest[4096] = {0};
    unsigned long long biggest_sz = 0;

    if (!buf) return 1;
    if (getenv("CHUNK_ZSTD")) {
        g_cc3 = ZSTD_createCCtx();
        g_cc19 = ZSTD_createCCtx();
        if (!g_cc3 || !g_cc19) { g_cc3 = g_cc19 = NULL; }
    }
    for (i = 0; i < 256; i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        gear[i] = rng;
    }
    files = calloc(MAXFILES, sizeof *files);
    if (!files) return 1;

    if (root[0] == '@') {
        /* Paths listed by a native walk (see _list_sample.py): walking /mnt/d
         * from WSL costs one lstat per entry over 9p and takes minutes for
         * nothing. Format: "<path>	<size>" per line. */
        FILE *lf = fopen(root + 1, "r");
        char line[MAXPATH + 32];
        if (!lf) { fprintf(stderr, "cannot open list %s", root + 1); return 1; }
        while (fgets(line, sizeof line, lf) && nfiles < MAXFILES) {
            char *tab = strchr(line, '	');
            if (!tab) continue;
            *tab = 0;
            snprintf(files[nfiles].path, MAXPATH, "%s", line);
            files[nfiles].size = strtoull(tab + 1, NULL, 10);
            nfiles++;
        }
        fclose(lf);
    } else {
        walk(root);
    }
    for (i = 0; i < nfiles; i++) total_bytes += files[i].size;
    printf("volume: %s\n", root);
    printf("files >= %d KiB: %zu (%.1f GiB)\n", MIN_FILE / 1024, nfiles, total_bytes / 1073741824.0);

    /* deterministic stride so the sample spans the whole volume */
    step = nfiles > maxfiles ? nfiles / maxfiles : 1;
    hs_init(&sw, 1024); hs_init(&sf, 1u << 20); hs_init(&sc, 1u << 19);

    for (i = 0; i < nfiles && read_bytes < budget; i += step) {
        FILE *f;
        size_t want, got;
        f = fopen(files[i].path, "rb");
        if (!f) continue;
        want = files[i].size < MAX_PER_FILE ? (size_t)files[i].size : MAX_PER_FILE;
        got = fread(buf, 1, want, f);
        fclose(f);
        if (!got) continue;
        read_bytes += got;
        seen++;

        if (hs_add(&sw, fnv(buf, got), (uint32_t)got)) { uniq_whole += got; }
        else { dup_whole += got; }
        n_whole++;

        fixed_scan(buf, got, &sf, &uniq_fixed, &dup_fixed, &n_fixed);
        cdc_scan(buf, got, &sc, &uniq_cdc, &dup_cdc, &n_cdc);

        if (got > biggest_sz) { biggest_sz = got; snprintf(biggest, sizeof biggest, "%s", files[i].path); }
    }

    printf("sample: %llu files, %.2f GiB read (every %zu-th)\n",
           (unsigned long long)seen, read_bytes / 1073741824.0, step);
    printf("\n%-10s %13s %15s %8s %10s\n", "model", "unique", "duplicate", "saved", "objects");
    printf("%-10s %10.2f GiB %12.2f GiB %7.2f%% %10llu\n", "whole-file",
           uniq_whole / 1073741824.0, dup_whole / 1073741824.0,
           100.0 * dup_whole / (double)(uniq_whole + dup_whole ? uniq_whole + dup_whole : 1),
           (unsigned long long)n_whole);
    printf("%-10s %10.2f GiB %12.2f GiB %7.2f%% %10llu\n", "fixed-4K",
           uniq_fixed / 1073741824.0, dup_fixed / 1073741824.0,
           100.0 * dup_fixed / (double)(uniq_fixed + dup_fixed ? uniq_fixed + dup_fixed : 1),
           (unsigned long long)n_fixed);
    printf("%-10s %10.2f GiB %12.2f GiB %7.2f%% %10llu\n", "cdc-8K",
           uniq_cdc / 1073741824.0, dup_cdc / 1073741824.0,
           100.0 * dup_cdc / (double)(uniq_cdc + dup_cdc ? uniq_cdc + dup_cdc : 1),
           (unsigned long long)n_cdc);

    if (zsto3) {
        double u = (double)uniq_cdc;
        printf("\ncompression of the UNIQUE cdc chunks: %.2f GiB -> "
               "zstd-3 %.2f GiB (%.1f%%), zstd-19 %.2f GiB (%.1f%%)\n",
               uniq_cdc / 1073741824.0, zsto3 / 1073741824.0,
               100.0 * (1.0 - (double)zsto3 / u),
               zsto19 / 1073741824.0,
               100.0 * (1.0 - (double)zsto19 / u));
    }

    /* ---- misalignment: same file, one byte inserted at the front -------- */
    if (biggest_sz) {
        FILE *f = fopen(biggest, "rb");
        if (f) {
            uint8_t *a = calloc(1, biggest_sz + 1);
            uint8_t *b = calloc(1, biggest_sz + 16);
            size_t n = fread(a, 1, biggest_sz, f);
            fclose(f);
            if (n) {
                hset fa, fb, ca, cb;
                uint64_t u, d, c, buf2[8];
                size_t off;
                memcpy(b + 1, a, n);          /* b = X + a */
                b[0] = 'X';
                hs_init(&fa, 1u << 16); hs_init(&fb, 1u << 16);
                hs_init(&ca, 1u << 16); hs_init(&cb, 1u << 16);
                u = d = c = 0;
                fixed_scan(a, n, &fa, &u, &d, &c);
                buf2[0] = c;                       /* blocks in a */
                u = d = c = 0;
                fixed_scan(b, n + 1, &fb, &u, &d, &c);
                buf2[1] = c;                       /* blocks in b */
                u = d = c = 0;
                cdc_scan(a, n, &ca, &u, &d, &c);
                buf2[2] = c;                       /* chunks in a */
                u = d = c = 0;
                cdc_scan(b, n + 1, &cb, &u, &d, &c);
                buf2[3] = c;                       /* chunks in b */

                /* how many of a's objects does b also contain? */
                uint64_t shared_f = 0, shared_c = 0;
                for (off = 0; off < fa.cap; off++)
                    if (fa.len[off]) {
                        size_t j = (size_t)fa.key[off] & (fb.cap - 1);
                        while (fb.len[j] && fb.key[j] != fa.key[off]) j = (j + 1) & (fb.cap - 1);
                        if (fb.len[j] && fb.key[j] == fa.key[off]) shared_f++;
                    }
                for (off = 0; off < ca.cap; off++)
                    if (ca.len[off]) {
                        size_t j = (size_t)ca.key[off] & (cb.cap - 1);
                        while (cb.len[j] && cb.key[j] != ca.key[off]) j = (j + 1) & (cb.cap - 1);
                        if (cb.len[j] && cb.key[j] == ca.key[off]) shared_c++;
                    }
                printf("\nmisalignment: %s (%zu B), one byte inserted at the front\n", biggest, n);
                printf("  fixed-4K: %llu blocks, %llu still match after the insert (%.1f%%)\n",
                       (unsigned long long)buf2[0], (unsigned long long)shared_f,
                       buf2[0] ? 100.0 * shared_f / buf2[0] : 0);
                printf("  cdc-8K  : %llu chunks, %llu still match after the insert (%.1f%%)\n",
                       (unsigned long long)buf2[2], (unsigned long long)shared_c,
                       buf2[2] ? 100.0 * shared_c / buf2[2] : 0);
            }
        }
    }
    return 0;
}
