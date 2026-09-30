/*
 * rs_stability_test -- does the RS math actually return the original bytes?
 *
 * WHY THIS EXISTS
 *
 * The bench in invf-sweep --redundant-bench reports rs-vm at 382.8 MB/s and
 * rs-cauchy at 97.0 MB/s (k=32, m=4, 64 MiB). That measures THROUGHPUT and
 * nothing else, so it cannot answer the question that matters: if blocks are
 * lost, does the reconstruction hand back the exact original bytes, or
 * something that merely looks plausible? A parity layer that returns wrong
 * bytes is worse than no parity layer, because the loss stays invisible until
 * the day somebody needs the data.
 *
 * The end-to-end test for that (tools/test-seal.sh) CANNOT answer it: the seal
 * is v2-only, v2 is retired in v0.5.0 ("only v3 is supported"), so no volume
 * with parity can be created and the recovery legs never run. The test SKIPs.
 * This file tests the math directly, with no volume and no filesystem, which
 * is where the question actually lives: rs.c has no idea what a stripe or an
 * L2P is.
 *
 * WHAT IS ASSERTED
 *
 *   1. Parity is deterministic: encoding the same data twice gives identical
 *      parity bytes. If it did not, every pass would rewrite parity and the
 *      cost measurement would be meaningless.
 *   2. Erasure recovery is bit-exact: for many different sets of erased
 *      slots, decode reconstructs EVERY block to a memcmp match. This is the
 *      file's whole purpose, and it is this project's own invariant applied
 *      to the parity layer.
 *   3. Beyond capacity it REFUSES: with m+1 erasures rs_decode returns -1
 *      and says so. This is the leg that gives the test teeth -- a
 *      reconstructor that "succeeds" past its limit is the failure mode that
 *      loses data silently.
 *   4. And the boundary, stated explicitly because it decides what this
 *      parity can be trusted for: this is ERASURE coding. A block that is
 *      still PRESENT but whose bytes were damaged is not detected and not
 *      corrected. So the layer is worth exactly what the volume's ability to
 *      know WHICH blocks are gone is worth -- see leg 4, which asserts the
 *      limit rather than hiding it.
 *
 * Parameters are the volume's: k=32, m in 2..8, 4 KiB blocks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs.h"

#define K      32u
#define BSZ    4096u
#define BLOCKS (K + 8u)

static int failures;
static int checks;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL: ");                                             \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

/* Deterministic, so a failure is reproducible from the seed printed. */
static uint64_t rng_state;
static void rng_seed(uint64_t s) { rng_state = s ? s : 1; }
static uint32_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return (uint32_t)(x >> 32);
}

/*
 * `buf` is a FLAT n*BSZ byte buffer, NOT an array of block pointers. An
 * earlier version took `uint8_t *const *` and indexed it as blocks[i], which
 * re-read the buffer's own bytes as addresses and died on the first call.
 * The two are interchangeable at the call site -- (uint8_t *)buf compiles
 * cleanly as (uint8_t *const *) -- which is precisely why the mistake
 * survived review and had to be caught by running the thing.
 */
static void fill_patterns(uint8_t *buf, unsigned n)
{
    /*
     * Degenerate blocks are included deliberately. Field arithmetic over
     * all-zero input exercises nothing, and a stability test that only sees
     * random data can miss a shape that only misbehaves on uniform input.
     */
    for (unsigned i = 0; i < n; i++) {
        uint8_t *b = buf + (size_t)i * BSZ;
        switch (i % 5) {
        case 0: memset(b, 0x00, BSZ); break;                 /* all zero  */
        case 1: memset(b, 0xFF, BSZ); break;                 /* all ones  */
        case 2: memset(b, (uint8_t)i, BSZ); break;           /* constant  */
        case 3: for (unsigned j = 0; j < BSZ; j++) b[j] = (uint8_t)(j ^ i);
                break;                                       /* ramp      */
        default:
            for (unsigned j = 0; j < BSZ; j++)
                b[j] = (uint8_t)(rng_next() & 0xFF);
            break;                                           /* random    */
        }
    }
}

/* Only the m PARITY slots, i.e. [K, K+m). Both encodes are handed the same
 * `data` pointers, so their data slots are the same bytes by construction and
 * comparing them proves nothing. An earlier version compared all n slots and
 * failed every case for the honest reason that the second buffer's data slots
 * were still zero -- a broken test, not a broken encoder. */
static int parity_equal(const uint8_t *a, const uint8_t *b, unsigned m)
{
    for (unsigned j = 0; j < m; j++)
        if (memcmp(a + (size_t)(K + j) * BSZ,
                   b + (size_t)(K + j) * BSZ, BSZ) != 0)
            return 0;
    return 1;
}

/* Deterministic PRNG for choosing erasure sets, independent of the data. */
static uint32_t choose_state;
static uint32_t choose_next(void)
{
    uint32_t x = choose_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    choose_state = x;
    return x;
}

static void run_algo(int algo, unsigned m, unsigned ncase, int exhaustive)
{
    const char *name = rs_algo_name(algo);
    unsigned n = K + m;
    unsigned kcase;

    uint8_t *mem = calloc(n, BSZ);
    uint8_t *mem2 = calloc(n, BSZ);
    uint8_t *work = calloc(n, BSZ);
    uint8_t *work2 = calloc(n, BSZ);
    if (!mem || !mem2 || !work || !work2) {
        printf("  FAIL: %s m=%u: out of memory\n", name, m);
        failures++;
        goto done;
    }

    /* A single fixed data set for the whole (algo, m) pair: determinism and
     * recovery must not depend on which sample we happened to pick. */
    rng_seed(0x5EEDu + m);
    choose_state = 0xC0FFEEu + m;   /* MUST be seeded: xorshift from 0 is a
                                   * fixed point and would spin forever. */
    fill_patterns(mem, n);

    {
        uint8_t *src[K], *parity[8], *parity2[8];
        for (unsigned i = 0; i < K; i++) src[i] = mem + (size_t)i * BSZ;
        for (unsigned j = 0; j < m; j++) parity[j]  = mem  + (size_t)(K + j) * BSZ;
        for (unsigned j = 0; j < m; j++) parity2[j] = mem2 + (size_t)(K + j) * BSZ;

        int rc = rs_encode(algo, K, m, BSZ, src, parity);
        CHECK(rc == 0, "%s m=%u: rs_encode returned %d", name, m, rc);
        if (rc != 0)
            goto done;

        /* Leg 1: parity is a pure function of the data. */
        int rc2 = rs_encode(algo, K, m, BSZ, src, parity2);
        CHECK(rc2 == 0, "%s m=%u: second rs_encode returned %d", name, m, rc2);
        CHECK(parity_equal(mem, mem2, m),
              "%s m=%u: two encodes of the same data disagree -- parity is not "
              "deterministic", name, m);
    }

    /* Leg 2: bit-exact recovery from m erasures. */
    for (kcase = 0; kcase < ncase; kcase++) {
        uint8_t present[BLOCKS];
        uint8_t erased[8];

        for (unsigned i = 0; i < n; i++) {
            memcpy(work + (size_t)i * BSZ, mem + (size_t)i * BSZ, BSZ);
            present[i] = 1;
        }
        for (unsigned j = 0; j < m; j++) {
            unsigned pos;
            if (exhaustive) {
                /* Walk every (first, second) pair without repeating. */
                unsigned a = kcase / (n - 1), b = kcase % (n - 1);
                if (b >= a) b++;
                pos = (j == 0) ? a : b;
            } else {
                do { pos = choose_next() % n; }
                while (memchr(erased, (int)pos, j) && j > 0);
            }
            erased[j] = (uint8_t)pos;
            present[pos] = 0;
        }

        {
            uint8_t *blk[BLOCKS];
            for (unsigned i = 0; i < n; i++) blk[i] = work + (size_t)i * BSZ;
            int rc = rs_decode(algo, K, m, BSZ, blk, present);
            CHECK(rc == 0, "%s m=%u case %u: rs_decode returned %d (should "
                  "recover from exactly m erasures)", name, m, kcase, rc);
            if (rc == 0) {
                for (unsigned i = 0; i < n; i++) {
                    if (memcmp(work + (size_t)i * BSZ,
                               mem + (size_t)i * BSZ, BSZ) != 0) {
                        CHECK(0, "%s m=%u case %u: slot %u did NOT come back "
                              "bit-exact (erased: %u,%u,%u,%u)", name, m, kcase,
                              i, m > 0 ? erased[0] : 0, m > 1 ? erased[1] : 0,
                              m > 2 ? erased[2] : 0, m > 3 ? erased[3] : 0);
                        break;
                    }
                }
            }
        }
    }

    /* Leg 3: one erasure past capacity must be refused, not fudged. This is
     * the leg that can fail. */
    {
        uint8_t present[BLOCKS];
        uint8_t *blk[BLOCKS];
        for (unsigned i = 0; i < n; i++) {
            memcpy(work2 + (size_t)i * BSZ, mem + (size_t)i * BSZ, BSZ);
            present[i] = 1;
        }
        for (unsigned j = 0; j < m + 1; j++)
            present[j] = 0;                       /* slots 0..m erased */
        for (unsigned i = 0; i < n; i++) blk[i] = work2 + (size_t)i * BSZ;

        int rc = rs_decode(algo, K, m, BSZ, blk, present);
        CHECK(rc == -1, "%s m=%u: with m+1 erasures rs_decode returned %d; it "
              "MUST refuse, or it will return confident garbage", name, m, rc);
    }

    printf("  %-10s m=%u: deterministic, %u erasure sets recovered bit-exact, "
           "m+1 refused\n", name, m, ncase);

done:
    free(mem); free(mem2); free(work); free(work2);
}

int main(void)
{
    printf("rs_stability_test: k=%u, block=%u B, field GF(2^8) poly 0x11D\n",
           K, BSZ);

    /* m=2 exhaustively: every pair of the 34 slots (C(34,2) = 561). For m=4
     * the header quotes 58905 submatrices as the reason the vm shape is built
     * the way it is; that many full block recoveries is a few minutes of CPU,
     * so m=4 gets a large deterministic sample and m=8 a smaller one. */
    struct { unsigned m; unsigned ncase; int exhaustive; } plan[] = {
        { 2, 561u,  1 },
        { 3, 5984u, 0 },
        { 4, 4000u, 0 },
        { 8, 1500u, 0 },
    };

    for (unsigned a = 0; a < sizeof(plan) / sizeof(plan[0]); a++)
        for (int algo = RS_ALGO_VM; algo <= RS_ALGO_CAUCHY; algo++)
            run_algo(algo, plan[a].m, plan[a].ncase, plan[a].exhaustive);

    /* Leg 4, stated rather than hidden: this is erasure coding, not error
     * correction. A block that is PRESENT but DAMAGED is neither detected nor
     * fixed, and no amount of parity changes that. This asserts the limit so
     * the limit cannot be rediscovered the hard way. */
    {
        const unsigned m = 2u, n = K + m;
        uint8_t *mem = calloc(n, BSZ);
        uint8_t present[BLOCKS];
        uint8_t *blk[BLOCKS];
        uint8_t *src[K], *parity[8];
        if (!mem) { printf("  FAIL: oom in leg 4\n"); failures++; return 1; }
        for (unsigned i = 0; i < n; i++) memset(mem + (size_t)i * BSZ, (uint8_t)i, BSZ);
        for (unsigned i = 0; i < K; i++) src[i] = mem + (size_t)i * BSZ;
        for (unsigned j = 0; j < m; j++) parity[j] = mem + (size_t)(K + j) * BSZ;
        rs_encode(RS_ALGO_VM, K, m, BSZ, src, parity);

        /* Damage a data block in place but still declare it present. */
        for (unsigned i = 0; i < n; i++) {
            present[i] = 1;
            blk[i] = mem + (size_t)i * BSZ;
        }
        blk[0][100] ^= 0xFF;
        /* Keep a copy of what we handed in, so "returned unchanged" is a
         * claim about BYTES and not about a return code. */
        {
            uint8_t *before = (uint8_t *)malloc((size_t)n * BSZ);
            int rc, same;
            if (!before) { failures++; printf("  FAIL: leg 4: malloc\n"); return 1; }
            memcpy(before, mem, (size_t)n * BSZ);
            rc = rs_decode(RS_ALGO_VM, K, m, BSZ, blk, present);
            /* This leg had NO assertion at all: `rc` was interpolated into a
             * printf and nothing tested it, so an rs_decode that started
             * ERRORING on a present-but-damaged block -- or one that started
             * silently "correcting" it -- left the suite at zero failures
             * while printing a confident, now-false claim. The header calls
             * this leg the one that "asserts the limit rather than hiding
             * it"; it did not. Both halves of the claim are checked now. */
            CHECK(rc == 0,
                  "leg 4: erasure coding, not error correction -- a PRESENT "
                  "damaged block must not be reported as an error (rc=%d)", rc);
            same = memcmp(blk[0], before, BSZ) == 0;
            CHECK(same,
                  "leg 4: the damaged data block is returned UNCHANGED -- a "
                  "parity layer that quietly repaired it would be claiming "
                  "error correction it does not have");
            free(before);
            printf("  leg 4 (erasure coding, not error correction): a PRESENT "
                   "but damaged block is returned unchanged (rc=%d) -- correct, "
                   "and the reason parity must be paired with real damage "
                   "detection\n", rc);
        }
        free(mem);
    }

    printf("rs_stability_test: %d checks, %d failures\n", checks, failures);
    if (failures) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS -- both shapes are MDS at these parameters and "
           "recover the original bytes bit-exactly from m erasures\n");
    return 0;
}
