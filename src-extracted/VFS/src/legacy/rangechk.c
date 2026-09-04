/* Ranged read vs whole-file read, for any file in a volume.
 *
 * usage: invf-rangechk <img> <name> [chunk]
 *
 * The mount reads through vol_read_range, one Dokan/FUSE callback at a time;
 * the CLI reads through vol_read_file, once. Nothing in the suite compared the
 * two on a transcoded file, so range_test.c hardcoded gost.exe and 13279232 --
 * an LZ4 file, the case that works. This takes any name.
 *
 * Two passes, and the cache counters after them, because the three things that
 * can go wrong here fail differently: wrong bytes show up as a MISMATCH, a
 * missing decode path as a FAIL, and a cache that is present but never
 * consulted as hits=0 while every byte still checks out. Only the counters
 * separate the last one from success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "invarifs.h"
#include "volume.h"

static int scan(invfs_volume *v, uint64_t inode, const uint8_t *whole,
                size_t wlen, size_t chunk, const char *label, int *calls_out)
{
    uint8_t *buf = (uint8_t *)malloc(chunk);
    uint64_t off;
    int calls = 0, bad = 0;
    clock_t t0 = clock();
    if (!buf) return -1;
    for (off = 0; off < wlen; off += chunk) {
        size_t want = (wlen - off < chunk) ? (size_t)(wlen - off) : chunk;
        int got = vol_read_range(v, inode, off, want, buf);
        calls++;
        if (got < 0) {
            printf("FAIL: vol_read_range returned %d at offset %llu\n",
                   got, (unsigned long long)off);
            bad = 1; break;
        }
        if ((size_t)got != want) {
            printf("SHORT: %d of %llu at offset %llu\n", got,
                   (unsigned long long)want, (unsigned long long)off);
            bad = 1; break;
        }
        if (memcmp(buf, whole + off, want) != 0) {
            printf("MISMATCH at offset %llu\n", (unsigned long long)off);
            bad = 1; break;
        }
    }
    printf("%s: %d call(s) of %llu, %.3fs\n", label, calls,
           (unsigned long long)chunk, (double)(clock() - t0) / CLOCKS_PER_SEC);
    free(buf);
    *calls_out = calls;
    return bad;
}

int main(int argc, char **argv)
{
    int err = 0, bad = 0, c1 = 0, c2 = 0;
    invfs_volume *v;
    uint64_t inode;
    uint8_t *whole = NULL;
    size_t wlen = 0;
    size_t chunk = (argc > 3) ? (size_t)strtoul(argv[3], NULL, 10) : 65536;
    invfs_arc_stats st;

    if (argc < 3) { printf("usage: %s <img> <name> [chunk]\n", argv[0]); return 2; }
    v = vol_open(argv[1], &err);
    if (!v) { printf("open fail %d\n", err); return 1; }
    inode = vol_find(v, argv[2]);
    if (!inode) { printf("not found: %s\n", argv[2]); return 1; }

    if (vol_read_file(v, inode, &whole, &wlen) != 0) {
        printf("vol_read_file failed\n"); return 1;
    }
    printf("whole: %llu bytes\n", (unsigned long long)wlen);

    if (scan(v, inode, whole, wlen, chunk, "pass1", &c1)) bad = 1;
    if (!bad && scan(v, inode, whole, wlen, chunk, "pass2", &c2)) bad = 1;

    vol_arc_stats(v, &st);
    printf("arc: hits=%llu misses=%llu ghost_hits=%llu inserts=%llu "
           "evictions=%llu refused=%llu entries=%u bytes=%llu budget=%llu\n",
           (unsigned long long)st.hits, (unsigned long long)st.misses,
           (unsigned long long)st.ghost_hits, (unsigned long long)st.inserts,
           (unsigned long long)st.evictions, (unsigned long long)st.refused,
           st.entries, (unsigned long long)st.bytes,
           (unsigned long long)st.budget);
    printf("result: %s\n", bad ? "FAIL" : "identical to whole-file read");

    free(whole);
    vol_close(v);
    return bad;
}
