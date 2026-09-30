/*
 * read_parallel_bitexact_test.c — the whole-file read must be bit-exact,
 * N-segment, R times, at INVFS_READ_THREADS>1, on BOTH backing-store paths.
 *
 * WHY THIS TEST EXISTS
 * --------------------
 * vol_decode_ast_entries() has two implementations of the same contract:
 *
 *   - the serial loop, which validates each segment's framing before it
 *     decodes (hdr == length for NONE, hdr < length for the compressed
 *     algos), and
 *   - the WP94 "fast path" (vol_read.c), which fans the same entries across
 *     INVFS_READ_THREADS pthreads and decodes them with NEITHER that framing
 *     validation NOR the ONE heat touch the serial loop owes.
 *
 * A second, weaker implementation of a contract is a defect on its own: it
 * can only ever be right by coincidence. This test pins the *observable*
 * half of that contract — the bytes — because that is the half that matters,
 * and because the framing half is separately unobservable from outside.
 *
 * The two legs differ only in blkio's `aligned` flag:
 *
 *   leg 1  image file, aligned=0  -> blkio_pread is a bare pread(2)
 *   leg 2  INVFS_FORCE_DEV=1, aligned=1 -> every transfer goes through
 *          blkio's SINGLE per-volume bounce buffer (blkio.c), which the
 *          read threads share.
 *
 * Leg 2 is the one that matters for concurrency: `bounce` is one malloc per
 * volume (blkio_open), dev_pread() reads into it and memcpys out of it with
 * no lock, and the fast path runs up to 32 threads over one volume at once.
 * A single whole-file read can hit that window by luck; R reads of an
 * N-segment file cannot miss it.
 *
 * Knobs (all optional): INVFS_BX_SIZE (bytes, default 32 MiB),
 * INVFS_BX_REPS (whole-file reads, default 8), INVFS_BX_THREADS (default 6).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "invarifs.h"
#include "volume_internal.h"

static size_t env_size(const char *k, size_t dflt)
{
    const char *e = getenv(k);
    char *end;
    unsigned long long n;
    if (!e || !*e) return dflt;
    n = strtoull(e, &end, 0);
    if (end == e || n == 0) return dflt;
    return (size_t)n;
}

static uint64_t env_u64(const char *k, uint64_t dflt)
{
    const char *e = getenv(k);
    char *end;
    unsigned long long n;
    if (!e || !*e) return dflt;
    n = strtoull(e, &end, 0);
    if (end == e) return dflt;
    return (uint64_t)n;
}

/* The read threads and the serial loop must agree about a byte; find the
 * first byte where they do not, so a failure names a place, not a count. */
static long first_diff(const uint8_t *got, const uint8_t *want, size_t n)
{

    size_t i;
    for (i = 0; i < n; i++)
        if (got[i] != want[i]) return (long)i;
    return -1;
}

static int run_leg(const char *img, const char *label, int force_dev,
                   const uint8_t *orig, size_t fsize,
                   uint64_t reps, int threads)
{
    invfs_volume *v;
    int err = 0, fails = 0;
    uint64_t id, r;
    char cmd[600];

    if (force_dev) setenv("INVFS_FORCE_DEV", "1", 1);
    else           unsetenv("INVFS_FORCE_DEV");
    if (threads > 0) {
        char t[16];
        snprintf(t, sizeof t, "%d", threads);
        setenv("INVFS_READ_THREADS", t, 1);
    }

    snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 2 >/dev/null 2>&1", img);
    if (system(cmd) != 0) {
        printf("  FAIL  %s: mkfs failed\n", label);
        return 1;
    }
    v = vol_open(img, &err);
    if (!v) { printf("  FAIL  %s: vol_open (%d)\n", label, err); return 1; }

    id = vol_v3_write_bulk(v, "bx.bin", (uint8_t *)orig, fsize, NULL);
    if (!id) { printf("  FAIL  %s: write\n", label); vol_close(v); return 1; }
    vol_flush(v);

    for (r = 0; r < reps; r++) {
        uint8_t *got = NULL;
        size_t glen = 0;
        if (vol_read_inode(v, id, 0, &got, &glen) != 0) {
            printf("  FAIL  %s: read %llu/%llu errored\n", label,
                   (unsigned long long)r, (unsigned long long)reps);
            fails++;
            continue;
        }
        if (glen != fsize) {
            printf("  FAIL  %s: read %llu/%llu length %zu != %zu\n", label,
                   (unsigned long long)r, (unsigned long long)reps, glen, fsize);
            fails++;
            free(got);
            continue;
        }
        if (memcmp(got, orig, fsize) != 0) {
            long d = first_diff(got, orig, fsize);
            printf("  FAIL  %s: read %llu/%llu NOT bit-exact; first diff at "
                   "offset %ld (got 0x%02x want 0x%02x)\n", label,
                   (unsigned long long)r, (unsigned long long)reps, d,
                   got[d] & 0xff, orig[d] & 0xff);
            fails++;
        }
        free(got);
    }

    vol_close(v);
    if (!fails)
        printf("  OK    %s: %llu whole-file reads of %zu bytes "
               "(%zu segments) bit-exact\n", label,
               (unsigned long long)reps, fsize, fsize / SEGMENT_SIZE);
    return fails;
}

/* The frame-vs-recipe disagreement leg.
 *
 * A NONE frame's csize IS its payload length, so an entry that says
 * algo=NONE with a length the frame cannot back is the one disagreement the
 * serial loop refuses and the WP94 fast path used to walk straight past --
 * memcpy'ing `e->length` bytes out of a `hdr`-byte allocation. The tail of
 * that copy is whatever the allocator put next: it returned SUCCESS, so the
 * caller went on to store the garbage under a frame CRC computed over the
 * garbage. A bit-exactness violation with no error anywhere in the system.
 *
 * The recipe needs >= 16 entries to reach the fan-out at all, so this
 * tampers ONE entry of an otherwise ordinary multi-segment file: the algo
 * becomes NONE while the length stays the original one, so the stored LZ4
 * frame (csize well under a segment) no longer matches what the entry claims.
 *
 * The invariant pinned here is not "the read fails" -- it is that the decode
 * contract has ONE implementation. Before the fix the 6-worker fan-out
 * returned success here and the 1-worker control did not, which is the whole
 * defect in one line of output.
 */
static int run_frame_disagreement_leg(const char *img,
                                      const uint8_t *orig, size_t fsize)
{
    invfs_volume *v;
    int err = 0, fails = 0, one_rc, par_rc;
    uint64_t id;
    invfs_v3_inode in;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    invfs_ast_block_entry *tap = NULL;
    uint8_t *blob = NULL, *nblob = NULL;
    size_t blen = 0, nblen = 0;
    uint8_t naddr[INVFS_V3_RECIPE_ADDR_LEN];
    char cmd[600], tb[16];

    unsetenv("INVFS_FORCE_DEV");
    snprintf(cmd, sizeof cmd, "./bin/invf-mkfs %s 2 >/dev/null 2>&1", img);
    if (system(cmd) != 0) { printf("  FAIL  frame leg: mkfs failed\n"); return 1; }
    v = vol_open(img, &err);
    if (!v) { printf("  FAIL  frame leg: vol_open (%d)\n", err); return 1; }

    id = vol_v3_write_bulk(v, "frame.bin", (uint8_t *)orig, fsize, NULL);
    if (!id) { printf("  FAIL  frame leg: write\n"); vol_close(v); return 1; }
    if (vol_v3_inode_get(v, id, &in) != 1) {
        printf("  FAIL  frame leg: inode_get\n"); vol_close(v); return 1;
    }
    if (vol_v3_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob ||
        vol_ast_recipe_parse(blob, blen, &ah, &ents, NULL) != 0) {
        printf("  FAIL  frame leg: recipe load/parse\n"); free(blob); vol_close(v); return 1;
    }
    if (ah.num_blocks < 16) {
        printf("  note  frame leg: recipe has %u entries, below the 16 the "
               "parallel fan-out needs; leg is vacuous\n", ah.num_blocks);
        free(blob); vol_close(v); return 0;
    }

    tap = (invfs_ast_block_entry *)malloc((size_t)ah.num_blocks * sizeof *tap);
    if (!tap) { free(blob); vol_close(v); return 1; }
    memcpy(tap, ents, (size_t)ah.num_blocks * sizeof *tap);
    free(blob); blob = NULL;

    /* Pick an entry whose STORED frame is genuinely shorter than the length
       the recipe claims, and relabel it verbatim. If the write path stored
       every segment uncompressed then no such entry exists and the leg would
       be vacuous -- say so loudly rather than reporting a vacuous OK. */
    {
        size_t pick = (size_t)-1;
        uint32_t pick_csize = 0;
        size_t k;
        for (k = 0; k < ah.num_blocks; k++) {
            uint32_t csize = 0; uint8_t *b = NULL;
            if (tap[k].pba == 0) continue;
            if (seg_read_checked(v, tap[k].pba, 0, 1, &csize, &b) != 0)
                continue;
            free(b);
            if (csize < tap[k].length) { pick = k; pick_csize = csize; break; }
        }
        if (pick == (size_t)-1) {
            printf("  FAIL  frame leg: every frame is >= its entry length, so "
                   "no disagreement exists and this leg proved nothing\n");
            free(tap); vol_close(v); return 1;
        }
        tap[pick].algo = INVFS_ALGO_NONE;
        printf("  ..    frame leg: entry %zu relabelled NONE; frame holds "
               "%u bytes, entry claims %llu\n", pick, pick_csize,
               (unsigned long long)tap[pick].length);
    }

    if (vol_ast_recipe_serialize(ah.file_size, tap, ah.num_blocks,
                                 &nblob, &nblen) != 0 ||
        vol_v3_recipe_store(v, nblob, nblen, naddr) != 0) {
        printf("  FAIL  frame leg: could not republish recipe\n");
        free(nblob); free(tap); vol_close(v); return 1;
    }
    free(nblob); free(tap);
    memcpy(in.recipe_addr, naddr, sizeof naddr);
    /* The DELTA setter, not vol_v3_inode_put(): write_bulk left the row in the
       delta overlay, and vol_v3_inode_get resolves overlay-first -- a
       base-tree write would be shadowed by the overlay entry and the reader
       would never see the tampered recipe at all. */
    if (vol_v3_inode_delta_put(v, id, &in) != 0) {
        printf("  FAIL  frame leg: inode_delta_put\n"); vol_close(v); return 1;
    }
    vol_flush(v);

    /* Baseline: the SAME fast path with a single worker. Note that
       INVFS_READ_THREADS=1 does NOT select the serial loop -- it runs the
       fan-out with one worker, so this is a like-for-like control on worker
       count alone. (The serial loop below is only reached when the fan-out
       declines, e.g. on a codec it does not handle.) */
    setenv("INVFS_READ_THREADS", "1", 1);
    {
        uint8_t *got = NULL; size_t glen = 0;
        one_rc = vol_read_inode(v, id, 0, &got, &glen);
        free(got);
    }
    if (one_rc == 0)
        printf("  note  frame leg: 1-worker read ACCEPTED the disagreement "
               "(leg is inconclusive)\n");

    /* The parallel implementation must reach the same verdict. */
    snprintf(tb, sizeof tb, "%d", 6);
    setenv("INVFS_READ_THREADS", tb, 1);
    {
        uint8_t *got = NULL; size_t glen = 0;
        par_rc = vol_read_inode(v, id, 0, &got, &glen);
        if (par_rc == 0) {
            if (glen == fsize && memcmp(got, orig, fsize) == 0) {
                printf("  OK    frame leg: parallel read bit-exact\n");
            } else {
                long d = -1;
                size_t i;
                for (i = 0; i < glen && i < fsize; i++)
                    if (got[i] != orig[i]) { d = (long)i; break; }
                printf("  FAIL  frame leg: parallel read SUCCEEDED on a NONE "
                       "frame shorter than the entry length; returned %zu "
                       "bytes\n", glen);
                if (d >= 0)
                    printf("        first wrong byte at offset %ld: got "
                           "0x%02x want 0x%02x\n",
                           d, got[d] & 0xff, orig[d] & 0xff);
                fails++;
            }
            free(got);
        } else if (par_rc != one_rc) {
            printf("  FAIL  frame leg: 1-worker read refused (%d) but the "
                   "6-worker read errored differently (%d) -- they must agree\n",
                   one_rc, par_rc);
            fails++;
        } else {
            printf("  OK    frame leg: the 6-worker read refuses exactly as "
                   "the 1-worker one does (rc=%d)\n", par_rc);
        }
    }

    vol_close(v);
    return fails;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[256];
    size_t fsize = env_size("INVFS_BX_SIZE", 32u * 1024 * 1024);
    uint64_t reps = env_u64("INVFS_BX_REPS", 8);
    int threads = (int)env_u64("INVFS_BX_THREADS", 6);
    uint8_t *orig;

    int fails = 0;

    snprintf(img, sizeof img, "%s/invf-read-parallel-bx.img", dir);
    unlink(img);

    printf("read_parallel_bitexact_test: %zu bytes, %zu segments, "
           "%llu reads, INVFS_READ_THREADS=%d\n",
           fsize, fsize / SEGMENT_SIZE, (unsigned long long)reps, threads);

    orig = (uint8_t *)malloc(fsize);
    if (!orig) { fprintf(stderr, "oom\n"); return 2; }
    /* Compressible, but not uniformly so: each 64 KiB segment gets its own
       deterministic pattern, so LZ4 shrinks most segments (which is what
       gives the frame-disagreement leg a frame shorter than its entry) while
       the fan-out still sees a realistic mix. */
    {
        size_t seg;
        for (seg = 0; seg * SEGMENT_SIZE < fsize; seg++) {
            size_t off = seg * SEGMENT_SIZE;
            size_t n = fsize - off < SEGMENT_SIZE ? fsize - off : SEGMENT_SIZE;
            size_t i;
            uint64_t s = 0x9E3779B97F4A7C15ull ^ (uint64_t)seg;
            for (i = 0; i < 64; i++) {
                s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                /* an occasional incompressible island, so not every segment
                   collapses to nothing */
                if ((i % 61) == 7) orig[off + i] = (uint8_t)(s >> 24);
            }
            for (i = 0; i < n; i++)
                orig[off + i] = (uint8_t)(orig[off + (i % 64)] ^ (uint8_t)(i >> 12));
        }
    }

    fails += run_leg(img, "image path (aligned=0)", 0, orig, fsize, reps, threads);
    fails += run_leg(img, "FORCE_DEV path (aligned=1, shared bounce)",
                     1, orig, fsize, reps, threads);
    fails += run_frame_disagreement_leg(img, orig, fsize);

    unlink(img);
    free(orig);

    printf("%s\n", fails ? "read_parallel_bitexact_test: FAILED" :
                          "read_parallel_bitexact_test: PASS");
    return fails ? 1 : 0;
}
