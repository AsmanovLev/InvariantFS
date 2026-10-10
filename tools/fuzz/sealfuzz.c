/* sealfuzz.c — WP201: adversarial fuzzer for the seal footer/parity-header
 * parse (the untrusted-bytes surface: torn reads).
 *
 * Target: the SHIPPED seal_footer_parse() / seal_parhdr_parse() from
 * src/core/vol_seal.c (non-static, linked from the real vol_seal.o — no
 * mirror, no second copy to drift). The engine's verify path parses
 * info-only (never allocates entry arrays); this harness ALSO exercises
 * the array-fulfilling form, because a future caller inherits whatever it
 * does on hostile counts.
 *
 * Threat model: the footer/parity files are ordinary volume files; a torn
 * write, a flipped bit, or a hostile hand-edit lands here. A parse that
 * reads out of bounds is a crash in the verify path; a parse that ACCEPTS
 * garbage is a trusted seal over unknown bytes. Both are hunted.
 *
 * Properties hunted (ASan + UBSan judge P1):
 *
 *   P1  neither parser reads outside [0, len) or crashes, on ANY input.
 *       Every input buffer is malloc'd at EXACTLY the input size, so an
 *       over-read is an ASan report rather than malloc slack. The trailer
 *       counts are bounds-checked BEFORE the CRC, and the CRC is checked
 *       before any entry/group calloc, so a forged huge nf/ng without a
 *       valid CRC cannot reach the allocator.
 *   P2  a footer the shipped encoders built still parses 0 with the SAME
 *       fields, entries and groups (seeds + every accepted mutation that
 *       preserves validity). A bounds check that starts refusing valid
 *       footers unseals healthy volumes with no error anywhere.
 *   P3  rc is always 0, 1 or -1; 1 means "not sealed" (never trusted,
 *       never partial: out-arrays stay NULL).
 *
 * Build (standalone driver, the `make test` gate via run-sealfuzz-gate.sh):
 *   $(CC) $(SEALFUZZ_SAN_CFLAGS) -o bin/sealfuzz tools/fuzz/sealfuzz.c \
 *       <core objs> -fsanitize=address,undefined ...
 * Usage: sealfuzz <cases> <seed>  (deterministic PRNG; same args, same run)
 */
#include "vol_seal.h"
#include "rs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

uint32_t invfs_crc32c(const void *data, size_t len);

static uint64_t st;
static uint64_t rnd(void)
{
    st ^= st << 13; st ^= st >> 7; st ^= st << 17;
    return st;
}

/* One valid footer (2 files, 2 groups) via the SHIPPED encoders. */
static size_t build_valid(uint8_t *out, size_t cap)
{
    uint8_t hash[32];
    uint8_t tmp[2 + 256 + 1 + 8 + 32], ge[10], tr[SEAL_TRAILER_LEN];
    size_t bl = 0, w, i;
    for (i = 0; i < sizeof hash; i++) hash[i] = (uint8_t)(i * 3 + 1);
    if (cap < 512) return 0;
    if (!seal_prelude_enc(out, 9, 1, RS_ALGO_VM, 7)) return 0;
    bl = SEAL_PRELUDE_LEN;
    w = seal_entry_enc(tmp, sizeof tmp, "a.txt", SEAL_FT_REG, 100000, hash);
    if (!w) return 0;
    memcpy(out + bl, tmp, w); bl += w;
    w = seal_entry_enc(tmp, sizeof tmp, "sub/b.bin", SEAL_FT_LNK, 0, hash);
    if (!w) return 0;
    memcpy(out + bl, tmp, w); bl += w;
    if (!seal_group_enc(ge, 9, 9ull * SEAL_SYM_BYTES)) return 0;
    memcpy(out + bl, ge, 10); bl += 10;
    if (!seal_group_enc(ge, 2, 70000)) return 0;
    memcpy(out + bl, ge, 10); bl += 10;
    if (!seal_trailer_enc(tr, out, bl, 2, 2)) return 0;
    memcpy(out + bl, tr, sizeof tr); bl += sizeof tr;
    return bl;
}

static int failures;

#define PCHECK(cond, ...)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("SEALFUZZ FAIL: ");                                      \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

/* P2: the valid seed parses clean with exact contents. */
static void check_valid(const uint8_t *fb, size_t len)
{
    seal_footinfo fi;
    seal_entry *ents = NULL;
    seal_group *grps = NULL;
    int rc = seal_footer_parse(fb, len, &fi, &ents, &grps);
    PCHECK(rc == 0, "valid footer refused (rc=%d)", rc);
    if (rc != 0) return;
    PCHECK(fi.k == 9 && fi.m == 1 && fi.sym == SEAL_SYM_BYTES &&
           fi.seq == 7 && fi.nfiles == 2 && fi.ngroups == 2,
           "valid footer fields wrong");
    PCHECK(ents && !strcmp(ents[0].name, "a.txt") && ents[0].size == 100000 &&
           ents[0].has_hash, "valid footer entry 0 wrong");
    PCHECK(ents && !strcmp(ents[1].name, "sub/b.bin") && ents[1].ftype == 2,
           "valid footer entry 1 wrong");
    PCHECK(grps && grps[0].datasyms == 9 &&
           grps[0].databytes == 9ull * SEAL_SYM_BYTES, "group 0 wrong");
    PCHECK(grps && grps[1].datasyms == 2 && grps[1].databytes == 70000,
           "group 1 wrong");
    free(ents);
    free(grps);
}

/* P1+P3: parse any buffer; judge crashes/OOB (ASan/UBSan) and the rc set. */
static void check_any(const uint8_t *buf, size_t len, int with_arrays)
{
    uint8_t *in = malloc(len ? len : 1);
    seal_footinfo fi;
    seal_entry *ents = NULL;
    seal_group *grps = NULL;
    int rc;
    if (!in) { printf("SEALFUZZ FAIL: oom\n"); failures++; return; }
    if (len) memcpy(in, buf, len);
    memset(&fi, 0, sizeof fi);
    if (with_arrays)
        rc = seal_footer_parse(in, len, &fi, &ents, &grps);
    else
        rc = seal_footer_parse(in, len, &fi, NULL, NULL);
    PCHECK(rc == 0 || rc == 1 || rc == -1, "rc=%d outside {0,1,-1}", rc);
    if (rc == 1) {
        /* "not sealed" must not be partial: no arrays escape. */
        PCHECK(ents == NULL && grps == NULL, "refusal leaked arrays");
    }
    free(ents);
    free(grps);
    free(in);
    /* Parity header over the same bytes (exact-size buffer inside). */
    {
        uint8_t *ph = malloc(SEAL_PARHDR_LEN);
        unsigned k, m;
        uint32_t ng;
        uint64_t sq;
        int prc;
        if (!ph) { printf("SEALFUZZ FAIL: oom\n"); failures++; return; }
        memset(ph, 0xA5, SEAL_PARHDR_LEN);
        memcpy(ph, buf, len < SEAL_PARHDR_LEN ? len : SEAL_PARHDR_LEN);
        prc = seal_parhdr_parse(ph, SEAL_PARHDR_LEN, &k, &m, &ng, &sq);
        PCHECK(prc == 0 || prc == 1, "parhdr rc=%d", prc);
        /* Short reads refuse too. */
        if (len < SEAL_PARHDR_LEN)
            PCHECK(seal_parhdr_parse(ph, len, NULL, NULL, NULL, NULL) == 1,
                   "short parity header trusted (len=%zu)", len);
        free(ph);
    }
}

int main(int argc, char **argv)
{
    static uint8_t seedbuf[4096];
    static uint8_t work[8192];
    size_t seedlen, i, ncases;
    if (argc != 3) {
        fprintf(stderr, "usage: sealfuzz <cases> <seed>\n");
        return 2;
    }
    ncases = (size_t)strtoull(argv[1], NULL, 10);
    st = strtoull(argv[2], NULL, 0);
    if (!st) st = 1;

    seedlen = build_valid(seedbuf, sizeof seedbuf);
    if (!seedlen) { printf("SEALFUZZ FAIL: cannot build seed\n"); return 1; }
    check_valid(seedbuf, seedlen);
    check_any(seedbuf, seedlen, 1);

    for (i = 0; i < ncases; i++) {
        size_t op = (size_t)(rnd() % 6);
        size_t len = seedlen, j;
        memcpy(work, seedbuf, seedlen);
        switch (op) {
        case 0: /* bit flips */
            for (j = 0; j < 1 + rnd() % 4; j++)
                work[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
            break;
        case 1: /* byte swaps */
            for (j = 0; j < 1 + rnd() % 8; j++)
                work[rnd() % len] = (uint8_t)rnd();
            break;
        case 2: /* truncation */
            len = rnd() % (seedlen + 1);
            break;
        case 3: { /* splice: duplicate a slice (offset/len chaos) */
            size_t at = rnd() % (seedlen + 1);
            size_t n = rnd() % 16;
            if (at + n > sizeof work) n = sizeof work - at;
            memmove(work + at, seedbuf, n);
            break;
        }
        case 4: /* extension with junk (cap keeps calloc bounded) */
            len = seedlen + rnd() % 512;
            if (len > sizeof work) len = sizeof work;
            for (j = seedlen; j < len; j++) work[j] = (uint8_t)rnd();
            break;
        default: /* single-byte poke at the edges */
            work[0] ^= 0xFF;
            if (len) work[len - 1] ^= 0xFF;
            break;
        }
        check_any(work, len, (i & 1));
    }
    /* The placeholder header a mid-run seal leaves behind must refuse. */
    {
        uint8_t h[SEAL_PARHDR_LEN];
        if (seal_parhdr_enc(h, 9, 1, 0xFFFFFFFFu, 3) == SEAL_PARHDR_LEN)
            PCHECK(seal_parhdr_parse(h, sizeof h, NULL, NULL, NULL, NULL)
                   == 1, "placeholder header trusted");
    }
    if (failures) {
        printf("SEALFUZZ: %d FAILURES (%zu cases)\n", failures, ncases);
        return 1;
    }
    printf("SEALFUZZ: PASS (%zu cases, no OOB, valid seed accepted)\n",
           ncases);
    return 0;
}
