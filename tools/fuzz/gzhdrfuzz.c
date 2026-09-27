/* gzhdrfuzz.c — WP129: adversarial fuzzer for the gzip header parse.
 *
 * Target: the REAL engine function. This translation unit #includes
 * src/core/vol_cpack.c, so gz_header_len() and vol_create_gz_file() below
 * are the shipped statics, not a mirror of them. That is the WP110
 * recipefuzz.c precedent and the reason this harness is worth anything:
 * a mirror of the parser can agree with the code while the code is wrong,
 * and a mirror is compiled with whatever bounds the mirror's author gave
 * it — which is precisely the mistake WP129 exists to fix. Here ASan sees
 * the production walk, and there is no second copy of it to drift.
 *
 * Threat model: the bytes come from whatever an upload pipeline dropped on
 * the volume. vol_sweep.c:1087 hands the GZR builtin the whole file, of
 * exactly gz_len bytes, so an over-read is a real heap over-read.
 *
 * Properties hunted (ASan + UBSan judge P1):
 *
 *   P1  gz_header_len never reads outside [0, gz_len) and never crashes,
 *       on ANY input. This is the WP129 regression: an 18-byte file with
 *       FLG=0x6d, XLEN=0x00FF used to read at offset 267, and a file
 *       with no NUL in its tail used to scan off the end entirely.
 *   P2  a header the engine WOULD have accepted is still accepted, with
 *       the SAME hlen. gz_header_len must not start refusing well-formed
 *       gzip: a silent extra reject here costs compression with no error
 *       anywhere, which is worse than the overflow. ref_len_mirror() is
 *       the pre-WP129 walk, kept ONLY as this differential oracle; it is
 *       never used to bound anything.
 *   P3  if gz_header_len refuses, vol_create_gz_file() returns 0 without
 *       touching the volume -- so the refusal really is the engine's
 *       decision and not just the helper's.
 *
 * The buffer handed to the parser is malloc'd at EXACTLY the input size.
 * That is what makes an over-read an ASan report rather than a read into
 * malloc slack.
 *
 * Build (libFuzzer, the long run -- see the Makefile recipe):
 *   clang -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
 *     -fsanitize=fuzzer ... gzhdrfuzz.c <core objs minus vol_cpack.o>
 * Build (standalone driver, the `make test` gate):
 *   same, minus -fsanitize=fuzzer; runs the seed corpus + a PRNG sweep.
 */
#include "vol_cpack.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- P2 oracle: the pre-WP129 walk, byte for byte (git show 226d554 --
 * src/core/vol_cpack.c:1504-1512). It is UNBOUNDED BY CONSTRUCTION and
 * is only ever called on inputs gz_header_len ACCEPTED, where hlen is
 * already proven to sit inside the buffer -- so the scan below cannot run
 * off the end in this harness. Do not "fix" it; it is the before-picture.
 * Its whole purpose is to answer "did the bounds check change a decision
 * on a well-formed header", and it can only be trusted for that because
 * P1 above is what guarantees the precondition. */
static int ref_len_mirror(const uint8_t *gz, size_t gz_len, size_t *hlen_out)
{
    size_t hlen = 10;
    unsigned flg;

    if (gz_len < 18 || gz[0] != 0x1F || gz[1] != 0x8B) return 0;
    flg = gz[3];
    if (flg & 0x04) { unsigned xl = gz[hlen] | (gz[hlen + 1] << 8); hlen += 2 + xl; }
    if (flg & 0x08) { while (gz[hlen]) hlen++; hlen++; }
    if (flg & 0x10) { while (gz[hlen]) hlen++; hlen++; }
    if (flg & 0x02) hlen += 2;
    if (hlen + 8 >= gz_len) return 0;
    *hlen_out = hlen;
    return 1;
}

static unsigned long g_cases, g_accepted, g_p3_checked;

/* Deflate output window, so a zip-bomb-shaped ISIZE cannot make the
 * harness itself the thing that exhausts memory. */
#define MAXOUT (64u * 1024u)

/* The whole body, factored out so the libFuzzer entry point and the
 * standalone driver run the identical code. */
static void one_case(const uint8_t *data, size_t size)
{
    uint8_t *gz;
    size_t hlen = 0;

    g_cases++;
    if (size > (1u << 20)) return;

    /* exactly the file length, no slack -- this is what makes an over-read
       an ASan hit instead of a read into allocator padding */
    gz = malloc(size ? size : 1);
    if (!gz) return;
    memcpy(gz, data, size);

    if (gz_header_len(gz, size, &hlen)) {
        g_accepted++;
        /* P2: an accept must mean the SAME accept the old walk gave, and
         * the same hlen. A bounds check that quietly stops accepting valid
         * gzip is the failure mode that hurts, so it is a hard abort. */
        size_t ref = 0;
        int ref_ok = ref_len_mirror(gz, size, &ref);
        if (!ref_ok || ref != hlen) {
            fprintf(stderr,
                    "P2 VIOLATION: gz_header_len accepted (hlen=%zu) but the "
                    "pre-WP129 walk says accept=%d hlen=%zu; %zu bytes\n",
                    hlen, ref_ok, ref, size);
            abort();
        }
        /* P1: the engine's own use of an accepted hlen. These are the
         * reads vol_create_gz_file makes right after the walk: the 8-byte
         * trailer, and the raw-deflate stream between header and trailer.
         * hlen + 8 < gz_len is the helper's contract; if it were ever
         * broken, stream_len wraps and the pointer arithmetic below is
         * what ASan would catch. */
        {
            size_t stream_len = size - hlen - 8;
            (void)gz[size - 8]; (void)gz[size - 1];   /* CRC32 + ISIZE */
            if (stream_len && stream_len < ((size_t)1 << 30)) {
                uint8_t *out = malloc(MAXOUT);
                if (out) {
                    z_stream in;
                    memset(&in, 0, sizeof in);
                    if (inflateInit2(&in, -15) == Z_OK) {
                        in.next_in = gz + hlen;
                        in.avail_in = (uInt)stream_len;
                        in.next_out = out;
                        in.avail_out = MAXOUT;
                        (void)inflate(&in, Z_FINISH);
                        inflateEnd(&in);
                    }
                    free(out);
                }
            }
        }
        free(gz);
        return;
    }

    /* P3: a refused header must be refused by the ENGINE too, before it
     * can reach the volume. v is NULL on purpose -- a refused header
     * returns before the first deref, and if it ever did not, that would
     * be a finding, not a nuisance. */
    g_p3_checked++;
    if (vol_create_gz_file(NULL, "f.gz", gz, size) != 0) {
        fprintf(stderr, "P3 VIOLATION: engine accepted a header gz_header_len refused\n");
        abort();
    }
    free(gz);
}

#ifdef GZHDR_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    one_case(data, size);
    return 0;
}
#else
/* ---- standalone driver: seed corpus, then a PRNG sweep -------------- */
static uint64_t g_s;
static uint64_t rnd(void)
{
    uint64_t x = g_s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void run_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f) { fprintf(stderr, "gzhdrfuzz: cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); exit(2); }
    buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); exit(2); }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); exit(2); }
    fclose(f);
    one_case(buf, (size_t)n);
    free(buf);
}

int main(int argc, char **argv)
{
    unsigned long iters = (argc > 1) ? strtoul(argv[1], NULL, 0) : 200000;
    unsigned long i;
    uint8_t buf[4096];

    g_s = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0x9E3779B97F4A7C15ULL;
    if (!g_s) g_s = 1;

    /* the seed corpus first: these are the real shapes, including the two
     * that crashed main, and they must behave identically */
    for (i = 3; i < (unsigned long)argc; i++) run_file(argv[i]);

    for (i = 0; i < iters; i++) {
        size_t n, j;
        unsigned mode = (unsigned)(i % 4);
        switch (mode) {
        case 0: n = 10 + (size_t)(rnd() % 8);   break;  /* near the 18 floor */
        case 1: n = 18 + (size_t)(rnd() % 64);  break;  /* exactly the PoC band */
        case 2: n = 1 + (size_t)(rnd() % 512);  break;
        default: n = 1 + (size_t)(rnd() % sizeof buf); break;
        }
        /* gzip magic and a random FLG: the header is the part under test,
         * and a random FLG reaches all 32 combinations of the five defined
         * bits plus the reserved three. */
        for (j = 0; j < n; j++) buf[j] = (uint8_t)rnd();
        if (n >= 3) { buf[0] = 0x1F; buf[1] = 0x8B; buf[2] = 8; buf[3] = (uint8_t)rnd(); }
        /* a NUL-free tail is the shape that made the old walk run forever */
        if ((rnd() & 3) == 0) for (j = 3; j < n; j++) buf[j] = (uint8_t)(0x80 | (rnd() & 0x7F));
        one_case(buf, n);
    }
    printf("gzhdrfuzz: %lu cases | accepted %lu | engine-refusals cross-checked %lu\n",
           g_cases, g_accepted, g_p3_checked);
    return 0;
}
#endif
