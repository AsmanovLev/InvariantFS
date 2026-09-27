/* recipefuzz.c — WP110: hostile codecpack-recipe fuzzer for the REAL engine.
 *
 * A codecpack is a separately-compiled trust boundary: the engine consumes
 * the recipe/map blob the pack hands back (vol_cpack.c:2668 cpack_map_parse
 * at sweep time, vol_cpack.c:2384-2401 again at read time). This harness
 * drives those PRODUCTION functions, not a mirror of them:
 *
 *     #include "vol_cpack.c"      <- pulls in the real static
 *                                    cpack_map_parse / cpack_map_validate /
 *                                    cpack_map_serve
 *
 * and links against the real engine objects (build/core_objs.txt minus
 * vol_cpack.o). src/legacy/fuzz_invfs.c's "leg 6" MIRRORS the parser; a
 * mirror can agree with the code while the code is wrong, and it cannot
 * find an ASan-visible overflow at all. This one calls the code.
 *
 * Threat model: the pack (or a buggy one, or a corrupted !mbrmap sibling
 * on disk) supplies the bytes. Every field of every entry is attacker
 * chosen: counts, offsets, lengths that overflow, negative-looking
 * u64s, entries pointing outside the container, truncation at each field
 * boundary.
 *
 * The property hunted -- the one that matters:
 *
 *   P1  no crash / no OOB on ANY blob (ASan+UBSan).
 *   P2  if cpack_map_parse accepts, the entries are inside the blob.
 *   P3  if cpack_map_validate ACCEPTS a map, then cpack_map_serve over
 *       the whole container stays inside the recipe buffer and the
 *       destination buffer. A validate that accepts an unservable map is
 *       a real memory-safety bug, not a cosmetic one.
 *
 * Build: see tools/fuzz/README-recipefuzz or the Makefile recipe.
 */
#include "vol_cpack.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- deterministic PRNG (xorshift64*) so every case is reproducible ---- */
static uint64_t g_s;
static uint64_t rnd(void)
{
    uint64_t x = g_s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_s = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static uint32_t rnd32(void) { return (uint32_t)(rnd() >> 32); }
static uint64_t rnd_pow2(void)   /* values with a strong bias to extremes */
{
    switch (rnd32() & 7) {
    case 0: return 0;
    case 1: return 1;
    case 2: return UINT64_MAX;
    case 3: return UINT64_MAX - (rnd32() & 0xFFFF);
    case 4: return 0x8000000000000000ULL;          /* "negative" i64 */
    case 5: return (uint64_t)(int64_t)(int32_t)rnd32();
    default: return rnd();
    }
}

static unsigned long g_cases, g_parsed, g_validated, g_served;

/* The interesting u64 magnitudes a pack could plausibly emit. */
static const uint64_t SIZES[] = { 0, 1, 512, 4096, 65536, 1u << 20, 1u << 30,
                                  0x7FFFFFFFULL, 0x80000000ULL, 1ULL << 40,
                                  1ULL << 47, 0xFFFFFFFFFFFFULL, UINT64_MAX };
#define NSIZES (sizeof SIZES / sizeof SIZES[0])

static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* Build one hostile MRMP/MRM2 blob. mode selects the corruption family so
 * every entry field gets attacked systematically, not just by chance.
 *
 * mode 9 ("partition") is the important one: it emits a map that really
 * DOES partition [0, container_size) -- so cpack_map_validate accepts it
 * and cpack_map_serve actually runs -- while every per-entry field stays
 * adversarial (src_off right at / past recipe_len, len 0, huge len, kind
 * 0/1/2, idx out of range, raw_len 0, engine 3, level 0/10, window_bits
 * negative). That is the only way to reach the memcpy at vol_cpack.c:2197
 * with a map the engine believed. */
static size_t gen_blob(uint8_t *buf, size_t cap, int *v2_out, int mode,
                       uint64_t container_size, uint64_t recipe_len)
{
    int v2 = (mode % 3) != 0;
    size_t ent_wire = v2 ? CPACK_MAP_ENT_WIRE2 : CPACK_MAP_ENT_WIRE;
    size_t base = v2 ? 12 : 8;
    uint32_t count;
    size_t i, need;

    if (mode == 9) {                       /* a real partition of the range */
        uint64_t pos = 0, left = container_size, budget = recipe_len;
        size_t n = 1 + rnd32() % 6, used = 0;
        if (!container_size || container_size > (1u << 20)) return 0;
        if (!recipe_len || recipe_len > (1u << 20)) return 0;
        need = base + n * ent_wire;
        if (need > cap) return 0;
        memcpy(buf, CPACK_MAP_MAGIC1, 4);   /* v1: kind 2 is not legal there */
        put32(buf + 4, (uint32_t)n);
        for (i = 0; i < n && left; i++) {
            uint8_t *p = buf + base + i * ent_wire;
            uint64_t len = (i + 1 == n) ? left : 1 + rnd() % left;
            uint64_t soff;
            /* A RECIPE range is servable only if src_off+len <= recipe_len
             * -- that is exactly the check at vol_cpack.c:2032-2034. So to
             * reach the memcpy at 2197 at all, the map must be legal; the
             * question is whether serve's OWN test (2196) then agrees on
             * every boundary. Hug it: soff is drawn flush against the end. */
            if (len > budget) len = budget;
            if (!len) break;
            soff = (rnd32() & 1) ? (budget - len)
                                 : (budget - len ? rnd() % (budget - len + 1) : 0);
            put64(p + 0, pos);
            put64(p + 8, len);
            p[16] = 0;                                   /* RECIPE */
            put32(p + 17, 0);                            /* idx must be 0 */
            put64(p + 21, soff);
            pos += len; left -= len; budget -= len; used++;
        }
        if (!used) return 0;
        /* the count must be `used`, not `n`: a zero-length entry is
         * rejected by cpack_map_validate (vol_cpack.c:2028), so padding
         * the tail out would make every case a trivial reject. */
        put32(buf + 4, (uint32_t)used);
        *v2_out = 0;
        return base + used * ent_wire;
    }

    if (mode == 10) {
        /* A map that correctly partitions [0, container_size) -- so every
         * container-side check in cpack_map_validate passes -- but whose
         * RECIPE src_off points PAST recipe_len. Against the real engine
         * this must be rejected at vol_cpack.c:2032-2034. It is the case
         * that makes the negative test meaningful: with that check deleted,
         * cpack_map_serve's memcpy at 2197 walks off the recipe buffer and
         * ASan fires. Without such cases a fuzzer "passes" no matter what. */
        uint64_t pos = 0, left = container_size;
        size_t n = 1 + rnd32() % 4;
        if (!container_size || container_size > (1u << 18)) return 0;
        need = base + n * ent_wire;
        if (need > cap) return 0;
        memcpy(buf, CPACK_MAP_MAGIC1, 4);
        put32(buf + 4, (uint32_t)n);
        for (i = 0; i < n && left; i++) {
            uint8_t *p = buf + base + i * ent_wire;
            uint64_t len = (i + 1 == n) ? left : 1 + rnd() % left;
            if (len > left) len = left;
            put64(p + 0, pos);
            put64(p + 8, len);
            p[16] = 0;                        /* RECIPE */
            put32(p + 17, 0);
            put64(p + 21, recipe_len + 1 + (rnd() % 65536));  /* OUT OF RANGE */
            pos += len; left -= len;
        }
        *v2_out = 0;
        return need;
    }

    switch (mode % 9) {
    case 0: count = 0; break;                      /* empty map */
    case 1: count = 1; break;
    case 2: count = CPACK_MAP_MAX_ENTS; break;     /* at the cap */
    case 3: count = CPACK_MAP_MAX_ENTS + 1; break; /* one past the cap */
    case 4: count = 0xFFFFFFFFu; break;            /* absurd */
    default: count = 1 + rnd32() % 64; break;
    }
    need = base + (size_t)count * ent_wire;
    if (need > cap) return 0;                       /* caller resizes */

    memcpy(buf, v2 ? CPACK_MAP_MAGIC2 : CPACK_MAP_MAGIC1, 4);
    put32(buf + 4, count);
    if (v2) put32(buf + 8, rnd());                 /* decomp_gen */
    for (i = 0; i < count; i++) {
        uint8_t *p = buf + base + i * ent_wire;
        uint32_t m = mode % 9;
        put64(p + 0,  (m == 5) ? rnd_pow2() : i * 4096ULL); /* orig_off */
        put64(p + 8,  (m == 6) ? rnd_pow2() : 4096);         /* len */
        p[16] = (uint8_t)((m == 7) ? rnd() : (i ? 1 : 0));   /* kind */
        put32(p + 17, (m == 8) ? rnd32() : i);              /* idx */
        put64(p + 21, (m == 8) ? rnd_pow2() : i * 512ULL);  /* src_off */
        if (v2) {
            put32(p + 29, (m == 4) ? rnd32() : 65536);      /* raw_len */
            p[33] = (uint8_t)(rnd() % 4);   /* engine: 0..3, 2/3 invalid */
            p[34] = (uint8_t)(rnd() % 12);  /* level: 0..11 */
            p[35] = (uint8_t)(rnd() % 12);  /* mem_level */
            p[36] = (uint8_t)(rnd() % 8);   /* strategy */
            p[37] = (uint8_t)(int8_t)(rnd() % 256); /* window_bits: signed */
        }
    }
    *v2_out = v2;
    return need;
}

/* One fuzz case: gen -> (maybe truncate) -> parse -> validate -> serve. */
static void one_case(int mode, uint64_t container_size, uint64_t recipe_len)
{
    size_t cap = 1 << 20;
    uint8_t *blob = malloc(cap);
    cpack_map_ent *ents = NULL;
    size_t nents = 0, i;
    int v2 = 0;
    if (!blob) return;

    size_t len = gen_blob(blob, cap, &v2, mode, container_size, recipe_len);
    if (!len) { free(blob); return; }

    /* truncation at every byte boundary in the first few entries -- the
     * classic place a parser reads a field before checking the length */
    if (mode != 9 && mode != 10 && (rnd32() & 3) == 0)
        len = rnd() % (len + 1);
    if (len < 4) { free(blob); return; }

    g_cases++;
    if (cpack_map_parse(blob, len, &ents, &nents, NULL) != 0) { free(blob); return; }
    g_parsed++;

    /* P2: anything the parser accepted must be readable from the blob */
    for (i = 0; i < nents; i++) {
        size_t off = (v2 ? 12 : 8) + i * (v2 ? CPACK_MAP_ENT_WIRE2 : CPACK_MAP_ENT_WIRE);
        if (off + (v2 ? CPACK_MAP_ENT_WIRE2 : CPACK_MAP_ENT_WIRE) > len) {
            fprintf(stderr, "P2 VIOLATION: accepted %zu entries but entry %zu "
                            "starts at %zu, blob is %zu bytes\n",
                    nents, i, off, len);
            exit(3);
        }
        if (ents[i].kind > (v2 ? CPACK_MAP_KIND_REPRO : 1)) {
            fprintf(stderr, "P2 VIOLATION: accepted kind %u in v%d\n",
                    ents[i].kind, v2);
            exit(3);
        }
    }

    /* the member table the map is validated against */
    {
        cpack_member mem[4];
        size_t nmem = 1 + rnd32() % 3;
        for (i = 0; i < nmem; i++) {
            mem[i].idx = (uint32_t)(rnd32() % 4);
            mem[i].usize = SIZES[rnd() % NSIZES];
            memset(mem[i].sname, 'a', sizeof mem[i].sname - 1);
            mem[i].sname[sizeof mem[i].sname - 1] = 0;
        }
        qsort(mem, nmem, sizeof *mem, cpack_member_idx_cmp);

        if (cpack_map_validate(ents, nents, container_size, recipe_len,
                               mem, nmem) != 0) { free(ents); free(blob); return; }
        g_validated++;

        /* P3: validate said YES. Now actually serve the whole container out
         * of a real recipe buffer. Only kind 0 (RECIPE) entries are servable
         * without a volume; a validated map made of them must not read past
         * recipe_len nor write past dst. ASan is the judge. */
        {
            int only_recipe = 1;
            uint8_t *recipe, *dst;
            for (i = 0; i < nents; i++)
                if (ents[i].kind != 0) { only_recipe = 0; break; }
            if (only_recipe && container_size && container_size < (16u << 20)) {
                recipe = malloc((size_t)recipe_len + 1);
                dst = malloc((size_t)container_size + 1);
                if (recipe && dst) {
                    /* sweep a request window across the container: the
                     * binary search at 2181-2185 and the rel/avail
                     * arithmetic at 2191-2194 depend on where the request
                     * starts, so one whole-container call is not enough. */
                    unsigned t;
                    for (t = 0; t < 4; t++) {
                        uint64_t roff = (t == 0) ? 0 : rnd() % container_size;
                        uint64_t rlen = 1 + rnd() % (container_size - roff);
                        memset(recipe, 0xA5, (size_t)recipe_len);
                        memset(dst, 0x5A, (size_t)container_size);
                        /* v is NULL on purpose: kind 0 never calls
                         * cpack_member_read, so no engine deref happens. */
                        if (cpack_map_serve(NULL, "f", ents, nents, mem, nmem,
                                            recipe, (size_t)recipe_len,
                                            roff, dst, (size_t)rlen) == 0)
                            g_served++;
                    }
                }
                free(recipe);
                free(dst);
            }
        }
    }
    free(ents);
    free(blob);
}

int main(int argc, char **argv)
{
    unsigned long iters = (argc > 1) ? strtoul(argv[1], NULL, 0) : 200000;
    unsigned long i;
    g_s = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0x1234567;
    if (!g_s) g_s = 1;
    for (i = 0; i < iters; i++) {
        int mode = (int)(i % 11);
        uint64_t csize = SIZES[rnd() % NSIZES];
        uint64_t rlen  = SIZES[rnd() % NSIZES];
        if (mode == 9) {
            /* the partition generator needs both sizes real and small, or
             * every case is a trivial reject and the serve path starves */
            csize = 1 + rnd() % (1u << 18);
            rlen  = 1 + rnd() % (1u << 18);
        } else if (mode == 10) {
            csize = 1 + rnd() % (1u << 18);
            rlen  = 1 + rnd() % 4096;
        } else if (rlen > (16u << 20)) {
            rlen = 16u << 20;
        }
        one_case(mode, csize, rlen);
    }
    printf("recipefuzz: %lu cases | parsed %lu | validated %lu | served %lu\n",
           g_cases, g_parsed, g_validated, g_served);
    return 0;
}
