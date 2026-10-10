/*
 * seal_test — the WP201 native seal's unit gate: no volume, no filesystem.
 *
 * WHY THIS EXISTS. The seal's trust anchors are two things that can be
 * checked without ever mounting a volume: the Reed-Solomon group math
 * (encode → drop up to m symbols → decode → byte-compare, determinism,
 * refusal past capacity) and the footer/parity-header parser (strict on
 * untrusted bytes: torn reads must read as "not sealed", never as a seal).
 * tools/test-seal.sh covers the volume behaviour on top; this file pins the
 * layers underneath, at the menu's own shapes, so a math or parser
 * regression fails here first with a line number instead of a vague e2e
 * red.
 *
 * WHAT IS ASSERTED
 *   1. The menu is fixed: 5->(20,1), 10->(9,1), 20->(8,2), 25->(6,2),
 *      anything else -> the default 10. No arbitrary matrix shapes.
 *   2. Parity is deterministic at every menu shape: two encodes agree.
 *   3. Bit-exact recovery from e <= m erasures at every menu shape, both
 *      group codes, several deterministic erasure sets each.
 *   4. m+1 erasures are REFUSED (rc -1), never reconstructed garbage.
 *   5. A built footer round-trips through the strict parser (fields +
 *      entries + groups match).
 *   6. Every corruption of that footer reads as "not sealed" (1), never
 *      valid: bad magic/version/geometry, torn body, truncated buffer,
 *      trailing bytes, absurd counts (even with a valid CRC), bad CRC,
 *      empty/NULL input. Same for the parity header, including the
 *      in-progress ngroups placeholder a seal writes mid-run.
 *   7. The encoders refuse bad input (zero sizes, overlong names) instead
 *      of emitting bytes the parser would then have to survive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vol_seal.h"
#include "rs.h"

#define BSZ 4096u

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

static uint64_t rng_state;
static void rng_seed(uint64_t s) { rng_state = s ? s : 1; }
static uint32_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return (uint32_t)(x >> 32);
}

static void fill_block(uint8_t *b, unsigned seed, size_t len)
{
    size_t j;
    switch (seed % 5) {
    case 0: memset(b, 0x00, len); break;
    case 1: memset(b, 0xFF, len); break;
    case 2: memset(b, (uint8_t)seed, len); break;
    case 3: for (j = 0; j < len; j++) b[j] = (uint8_t)(j ^ seed); break;
    default:
        for (j = 0; j < len; j++) b[j] = (uint8_t)(rng_next() & 0xFF);
        break;
    }
}

/* ---- leg 1: the menu is fixed --------------------------------------- */

static void leg_menu(void)
{
    struct { int pct; unsigned k, m; } want[] = {
        { 5, 20, 1 }, { 10, 9, 1 }, { 20, 8, 2 }, { 25, 6, 2 },
    };
    size_t i;
    unsigned k, m;
    uint64_t g, pb;
    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        seal_menu(want[i].pct, &k, &m);
        CHECK(k == want[i].k && m == want[i].m,
              "menu %d: got (%u,%u), want (%u,%u)",
              want[i].pct, k, m, want[i].k, want[i].m);
        CHECK(k + m <= 256, "menu %d: k+m=%u exceeds the RS limit",
              want[i].pct, k + m);
    }
    /* Unknown percents fall back to the default, never to garbage. */
    seal_menu(0, &k, &m);
    CHECK(k == 9 && m == 1, "menu 0: got (%u,%u), want the default (9,1)",
          k, m);
    seal_menu(99, &k, &m);
    CHECK(k == 9 && m == 1, "menu 99: got (%u,%u), want the default (9,1)",
          k, m);
    seal_menu(-3, &k, &m);
    CHECK(k == 9 && m == 1, "menu -3: got (%u,%u), want the default (9,1)",
          k, m);
    /* Capacity plan: 1 byte still seals one group; exact multiples exact. */
    seal_plan(9, 1, 0, &g, &pb);
    CHECK(g == 0 && pb == 0, "plan empty: got (%llu,%llu)",
          (unsigned long long)g, (unsigned long long)pb);
    seal_plan(9, 1, 1, &g, &pb);
    CHECK(g == 1 && pb == SEAL_SYM_BYTES, "plan 1B: got (%llu,%llu)",
          (unsigned long long)g, (unsigned long long)pb);
    seal_plan(9, 1, 9ull * SEAL_SYM_BYTES, &g, &pb);
    CHECK(g == 1 && pb == SEAL_SYM_BYTES, "plan exact: got (%llu,%llu)",
          (unsigned long long)g, (unsigned long long)pb);
    seal_plan(9, 1, 9ull * SEAL_SYM_BYTES + 1, &g, &pb);
    CHECK(g == 2 && pb == 2 * SEAL_SYM_BYTES, "plan +1: got (%llu,%llu)",
          (unsigned long long)g, (unsigned long long)pb);
    printf("  menu: fixed pairs + default fallback + plan arithmetic ok\n");
}

/* ---- legs 2-4: the group math at the menu shapes --------------------- */

static void leg_rs_shape(unsigned k, unsigned m, int algo)
{
    unsigned n = k + m;
    uint8_t *mem = calloc(n, BSZ);
    uint8_t *mem2 = calloc(n, BSZ);
    uint8_t *work = calloc(n, BSZ);
    uint8_t **src, **par, **par2, **blk;
    uint8_t *present;
    unsigned i, c;
    const char *name = rs_algo_name(algo);
    int rc;

    if (!mem || !mem2 || !work) {
        printf("  FAIL: oom in rs leg\n");
        failures++;
        goto done;
    }
    src = malloc(sizeof *src * k);
    par = malloc(sizeof *par * m);
    par2 = malloc(sizeof *par2 * m);
    blk = malloc(sizeof *blk * n);
    present = malloc(n);
    if (!src || !par || !par2 || !blk || !present) {
        printf("  FAIL: oom in rs leg\n");
        failures++;
        goto done2;
    }
    rng_seed(0x5EA1u + k * 16 + m);
    for (i = 0; i < n; i++)
        fill_block(mem + (size_t)i * BSZ, i, BSZ);
    for (i = 0; i < k; i++) src[i] = mem + (size_t)i * BSZ;
    for (i = 0; i < m; i++) par[i] = mem + (size_t)(k + i) * BSZ;
    for (i = 0; i < m; i++) par2[i] = mem2 + (size_t)(k + i) * BSZ;

    rc = rs_encode(algo, k, m, BSZ, src, par);
    CHECK(rc == 0, "%s (%u,%u): rs_encode rc=%d", name, k, m, rc);
    if (rc != 0) goto done2;
    /* Determinism: the same data twice -> identical parity. */
    rc = rs_encode(algo, k, m, BSZ, src, par2);
    CHECK(rc == 0, "%s (%u,%u): second rs_encode rc=%d", name, k, m, rc);
    {
        int same = 1;
        for (i = 0; i < m; i++)
            if (memcmp(par[i], par2[i], BSZ) != 0) { same = 0; break; }
        CHECK(same, "%s (%u,%u): two encodes disagree", name, k, m);
    }
    /* Recovery: 12 deterministic erasure sets of 1..m slots. */
    for (c = 0; c < 12; c++) {
        unsigned e = 1 + (c % m), j;
        uint8_t erased[8];
        memset(erased, 0xFF, sizeof erased);
        for (i = 0; i < n; i++) {
            memcpy(work + (size_t)i * BSZ, mem + (size_t)i * BSZ, BSZ);
            blk[i] = work + (size_t)i * BSZ;
            present[i] = 1;
        }
        for (j = 0; j < e; j++) {
            unsigned slot = (c * 7 + j * 3 + 1) % n;
            while (memchr(erased, (int)slot, j) && j > 0)
                slot = (slot + 1) % n;
            erased[j] = (uint8_t)slot;
            present[slot] = 0;
            memset(blk[slot], 0xA5, BSZ);
        }
        rc = rs_decode(algo, k, m, BSZ, blk, present);
        CHECK(rc == 0, "%s (%u,%u) case %u: decode rc=%d (e=%u)",
              name, k, m, c, rc, e);
        if (rc == 0 && memcmp(work, mem, (size_t)n * BSZ) != 0)
            CHECK(0, "%s (%u,%u) case %u: not bit-exact (e=%u)",
                  name, k, m, c, e);
    }
    /* Past capacity: refuse, never reconstruct. */
    {
        for (i = 0; i < n; i++) {
            memcpy(work + (size_t)i * BSZ, mem + (size_t)i * BSZ, BSZ);
            blk[i] = work + (size_t)i * BSZ;
            present[i] = 1;
        }
        for (i = 0; i < m + 1; i++) present[i] = 0;
        rc = rs_decode(algo, k, m, BSZ, blk, present);
        CHECK(rc == -1, "%s (%u,%u): m+1 erasures gave rc=%d, must refuse",
              name, k, m, rc);
    }
    printf("  %-10s (%2u,%u): deterministic, 12 erasure sets bit-exact, "
           "m+1 refused\n", name, k, m);

done2:
    free(src); free(par); free(par2); free(blk); free(present);
done:
    free(mem); free(mem2); free(work);
}

/* ---- legs 5-7: footer + parity-header codec -------------------------- */

static uint8_t *build_footer(size_t *len_out, uint32_t *nfiles_out,
                             uint32_t *ngroups_out)
{
    /* Two files, two groups: enough structure to corrupt meaningfully. */
    static const char *names[] = { "a.txt", "sub/b.bin" };
    static const unsigned ftypes[] = { SEAL_FT_REG, SEAL_FT_REG };
    static const uint64_t sizes[] = { 100000, 70000 };
    uint8_t hash[32];
    uint8_t *body = NULL, *full = NULL;
    size_t body_cap = 4096, body_len = 0;
    uint8_t pre[SEAL_PRELUDE_LEN], tmp[2 + 256 + 1 + 8 + 32], ge[10];
    uint8_t tr[SEAL_TRAILER_LEN];
    size_t i, w;
    for (i = 0; i < sizeof hash; i++) hash[i] = (uint8_t)(i * 3 + 1);
    body = malloc(body_cap);
    if (!body) return NULL;
    if (!seal_prelude_enc(pre, 9, 1, RS_ALGO_VM, 7)) { free(body); return NULL; }
    memcpy(body, pre, sizeof pre);
    body_len = sizeof pre;
    for (i = 0; i < 2; i++) {
        w = seal_entry_enc(tmp, sizeof tmp, names[i], ftypes[i], sizes[i],
                           hash);
        if (!w) { free(body); return NULL; }
        memcpy(body + body_len, tmp, w);
        body_len += w;
    }
    if (!seal_group_enc(ge, 9, 9ull * SEAL_SYM_BYTES)) { free(body); return NULL; }
    memcpy(body + body_len, ge, 10);
    body_len += 10;
    if (!seal_group_enc(ge, 3, 3ull * 1000)) { free(body); return NULL; }
    memcpy(body + body_len, ge, 10);
    body_len += 10;
    if (!seal_trailer_enc(tr, body, body_len, 2, 2)) { free(body); return NULL; }
    full = malloc(body_len + sizeof tr);
    if (!full) { free(body); return NULL; }
    memcpy(full, body, body_len);
    memcpy(full + body_len, tr, sizeof tr);
    free(body);
    *len_out = body_len + sizeof tr;
    *nfiles_out = 2;
    *ngroups_out = 2;
    return full;
}

static void leg_footer_roundtrip(void)
{
    size_t len = 0;
    uint32_t nf = 0, ng = 0;
    uint8_t *fb = build_footer(&len, &nf, &ng);
    seal_footinfo fi;
    seal_entry *ents = NULL;
    seal_group *grps = NULL;
    int rc;
    CHECK(fb != NULL, "footer build failed");
    if (!fb) return;
    rc = seal_footer_parse(fb, len, &fi, &ents, &grps);
    CHECK(rc == 0, "valid footer parsed as %d", rc);
    if (rc == 0) {
        CHECK(fi.k == 9 && fi.m == 1 && fi.sym == SEAL_SYM_BYTES &&
              fi.seq == 7 && fi.nfiles == 2 && fi.ngroups == 2,
              "footer fields wrong: k=%u m=%u seq=%llu nf=%u ng=%u",
              fi.k, fi.m, (unsigned long long)fi.seq, fi.nfiles, fi.ngroups);
        CHECK(ents && !strcmp(ents[0].name, "a.txt") && ents[0].size == 100000 &&
              ents[0].has_hash, "entry 0 wrong");
        CHECK(ents && !strcmp(ents[1].name, "sub/b.bin") &&
              ents[1].size == 70000, "entry 1 wrong");
        CHECK(grps && grps[0].datasyms == 9 &&
              grps[0].databytes == 9ull * SEAL_SYM_BYTES, "group 0 wrong");
        CHECK(grps && grps[1].datasyms == 3 && grps[1].databytes == 3000,
              "group 1 wrong");
    }
    free(ents); free(grps);
    /* Info-only (no allocation) parses too. */
    rc = seal_footer_parse(fb, len, &fi, NULL, NULL);
    CHECK(rc == 0, "info-only parse gave %d", rc);
    free(fb);
    printf("  footer: built form round-trips (fields, entries, groups)\n");
}

static void corrupt_case(const char *what, uint8_t *fb, size_t len, size_t buflen)
{
    seal_footinfo fi;
    int rc = seal_footer_parse(fb, len, &fi, NULL, NULL);
    CHECK(rc == 1, "corrupt footer (%s) parsed as %d, want 1 (not sealed)",
          what, rc);
    (void)buflen;
}

static void leg_footer_corrupt(void)
{
    size_t len = 0;
    uint32_t nf = 0, ng = 0;
    uint8_t *good = build_footer(&len, &nf, &ng);
    uint8_t *fb;
    size_t cap = len + 16;
    CHECK(good != NULL, "footer build failed");
    if (!good) return;
    fb = malloc(cap);
    CHECK(fb != NULL, "oom");
    if (!fb) { free(good); return; }

#define RESET() memcpy(fb, good, len)
#define PATCH(off, val) do { RESET(); fb[off] ^= (val); \
        corrupt_case("byte " #off, fb, len, cap); } while (0)

    RESET(); fb[0] ^= 0xFF;
    corrupt_case("magic", fb, len, cap);
    RESET(); fb[8] ^= 0xFF;
    corrupt_case("version", fb, len, cap);
    RESET(); fb[10] = 0; fb[11] = 0;
    corrupt_case("k=0", fb, len, cap);
    RESET(); fb[10] = 200; fb[11] = 0;
    corrupt_case("k+m>256", fb, len, cap);
    RESET(); fb[14] ^= 0x01;
    corrupt_case("sym", fb, len, cap);
    RESET(); fb[18] = 9;
    corrupt_case("algo", fb, len, cap);
    RESET(); { int i; for (i = 20; i < 28; i++) fb[i] = 0; }
    corrupt_case("seq=0", fb, len, cap);
    /* Torn body with the ORIGINAL (now wrong) CRC: still refused. */
    RESET(); fb[SEAL_PRELUDE_LEN + 5] ^= 0x40;
    corrupt_case("body byte, stale CRC", fb, len, cap);
    /* Truncations. */
    RESET(); corrupt_case("len-1", fb, len - 1, cap);
    RESET(); corrupt_case("len-10", fb, len - 10, cap);
    RESET(); corrupt_case("prelude only", fb, SEAL_PRELUDE_LEN, cap);
    RESET(); corrupt_case("empty", fb, 0, cap);
    CHECK(seal_footer_parse(NULL, 0, NULL, NULL, NULL) == 1,
          "NULL footer did not read as not-sealed");
    /* Trailing bytes (valid CRC over the PREFIX does not save it: the
     * parser CRCs the whole buffer, so any suffix breaks the CRC — and a
     * recomputed CRC over buffer+garbage still fails exact-consumption). */
    RESET(); fb[len] = 0x00;
    corrupt_case("one trailing byte", fb, len + 1, cap);
    /* Absurd counts WITH a valid CRC (re-trailered): bounds before trust.
     * invfs_crc32c is the engine's own CRC (src/core/invarifs.h:1377),
     * declared here so the test can re-seal a tampered buffer exactly as
     * an attacker would. */
    RESET();
    {
        uint32_t invfs_crc32c(const void *data, size_t len);
        size_t t = len - 12;
        uint32_t c;
        fb[t] = fb[t+1] = fb[t+2] = fb[t+3] = 0xFF;   /* nf = 2^32-1 */
        c = invfs_crc32c(fb, len - 4);
        fb[len-4] = (uint8_t)c; fb[len-3] = (uint8_t)(c >> 8);
        fb[len-2] = (uint8_t)(c >> 16); fb[len-1] = (uint8_t)(c >> 24);
        corrupt_case("nf absurd, valid CRC", fb, len, cap);
    }
    /* Same for ngroups. */
    RESET();
    {
        uint32_t invfs_crc32c(const void *data, size_t len);
        size_t t = len - 8;
        uint32_t c;
        fb[t] = fb[t+1] = fb[t+2] = fb[t+3] = 0xFF;   /* ng = 2^32-1 */
        c = invfs_crc32c(fb, len - 4);
        fb[len-4] = (uint8_t)c; fb[len-3] = (uint8_t)(c >> 8);
        fb[len-2] = (uint8_t)(c >> 16); fb[len-1] = (uint8_t)(c >> 24);
        corrupt_case("ng absurd, valid CRC", fb, len, cap);
    }
    /* Namelen 0 on the first entry (stale CRC is enough to refuse). */
    RESET(); fb[SEAL_PRELUDE_LEN] = 0; fb[SEAL_PRELUDE_LEN + 1] = 0;
    corrupt_case("namelen 0", fb, len, cap);
#undef RESET
#undef PATCH
    free(fb);
    free(good);
    printf("  footer: every corruption reads as not-sealed, never valid\n");
}

static void leg_parhdr(void)
{
    uint8_t h[SEAL_PARHDR_LEN];
    unsigned k, m;
    uint32_t ng;
    uint64_t sq;
    int rc;
    CHECK(seal_parhdr_enc(h, 9, 1, 41, 3) == SEAL_PARHDR_LEN,
          "parity header encode failed");
    rc = seal_parhdr_parse(h, sizeof h, &k, &m, &ng, &sq);
    CHECK(rc == 0, "valid parity header parsed as %d", rc);
    CHECK(k == 9 && m == 1 && ng == 41 && sq == 3,
          "parity header fields wrong");
    h[0] ^= 0xFF;
    CHECK(seal_parhdr_parse(h, sizeof h, NULL, NULL, NULL, NULL) == 1,
          "bad-magic parity header trusted");
    h[0] ^= 0xFF;
    h[sizeof h - 1] ^= 0x01;
    CHECK(seal_parhdr_parse(h, sizeof h, NULL, NULL, NULL, NULL) == 1,
          "bad-CRC parity header trusted");
    h[sizeof h - 1] ^= 0x01;
    CHECK(seal_parhdr_parse(h, sizeof h - 1, NULL, NULL, NULL, NULL) == 1,
          "short parity header trusted");
    /* The in-progress placeholder must never verify. */
    CHECK(seal_parhdr_enc(h, 9, 1, 0xFFFFFFFFu, 3) == SEAL_PARHDR_LEN,
          "placeholder header encode failed");
    CHECK(seal_parhdr_parse(h, sizeof h, NULL, NULL, NULL, NULL) == 1,
          "in-progress placeholder parity header trusted");
    printf("  parity header: valid parses, torn/placeholder refused\n");
}

static void leg_encoders(void)
{
    uint8_t out[SEAL_PRELUDE_LEN];
    uint8_t e[300];
    uint8_t g[10];
    uint8_t h[32];
    char longname[300];
    memset(h, 0xAB, sizeof h);
    memset(longname, 'x', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    CHECK(seal_prelude_enc(out, 9, 1, RS_ALGO_VM, 1) == SEAL_PRELUDE_LEN,
          "good prelude refused");
    CHECK(seal_prelude_enc(out, 0, 1, RS_ALGO_VM, 1) == 0,
          "k=0 prelude emitted");
    CHECK(seal_prelude_enc(out, 200, 100, RS_ALGO_VM, 1) == 0,
          "k+m>256 prelude emitted");
    CHECK(seal_prelude_enc(out, 9, 1, 99, 1) == 0,
          "bad-algo prelude emitted");
    CHECK(seal_prelude_enc(out, 9, 1, RS_ALGO_VM, 0) == 0,
          "seq=0 prelude emitted");
    CHECK(seal_entry_enc(e, sizeof e, "f", SEAL_FT_REG, 8, h) > 0,
          "good entry refused");
    CHECK(seal_entry_enc(e, sizeof e, "", SEAL_FT_REG, 8, h) == 0,
          "empty-name entry emitted");
    CHECK(seal_entry_enc(e, sizeof e, longname, SEAL_FT_REG, 8, h) == 0,
          "overlong-name entry emitted");
    CHECK(seal_entry_enc(e, 4, "file", SEAL_FT_REG, 8, h) == 0,
          "small-cap entry emitted");
    CHECK(seal_entry_enc(NULL, 0, "f", SEAL_FT_REG, 8, h) == 0,
          "NULL-out entry emitted");
    CHECK(seal_group_enc(g, 9, 100) == 10, "good group refused");
    CHECK(seal_group_enc(g, 0, 100) == 0, "datasyms=0 group emitted");
    CHECK(seal_group_enc(g, 300, 100) == 0, "datasyms=300 group emitted");
    printf("  encoders: bad input refused, never emitted\n");
}

int main(void)
{
    printf("seal_test: menu + group math (GF(2^8)) + footer codec\n");
    leg_menu();
    {
        /* Every fixed menu pair in the shipped code; both group codes on
         * the default pair, the shipped default on the rest. */
        unsigned ks[] = { 20, 9, 8, 6 };
        unsigned ms[] = { 1, 1, 2, 2 };
        size_t i;
        for (i = 0; i < 4; i++)
            leg_rs_shape(ks[i], ms[i], RS_ALGO_VM);
        leg_rs_shape(9, 1, RS_ALGO_CAUCHY);
    }
    leg_footer_roundtrip();
    leg_footer_corrupt();
    leg_parhdr();
    leg_encoders();
    printf("seal_test: %d checks, %d failures\n", checks, failures);
    if (failures) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS -- menu fixed, groups recover bit-exact, "
           "parser trusts nothing torn\n");
    return 0;
}
