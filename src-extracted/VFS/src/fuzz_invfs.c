/*
 * fuzz_invfs.c — deterministic fuzz / property-test harness for the pure
 * and parsing layers (WP-hygiene: zero fuzz coverage -> this closes it for
 * the layers that need no volume image).
 *
 * Standalone, NOT part of `make test`; build+run on demand:
 *   make fuzz
 *   bin/invf-fuzz [iterations] [seed]     (defaults: 10000, fixed seed)
 *
 * Legs:
 *   1. sniff      every registry codec's sniff() over random + magic-mutated
 *                 buffers: scores stay in 0..100, deterministic, and a
 *                 buffer WITH a codec's strong magic scores strictly higher
 *                 than the same buffer without it.
 *   2. classifier invfs_text_family / invfs_binary_family: deterministic,
 *                 pure-printable content (no name) is text, a NUL byte is
 *                 never text, < 4 KiB is never binary-family.
 *   3. manifest   the codecpack manifest parser (via temp files + the real
 *                 pack_scan_dir -> parse_manifest -> pack_register path):
 *                 mutated manifests never crash it, registration is never
 *                 half-accepted, and generated-valid manifests round-trip
 *                 with matching registry fields.
 *   4. ppmd       invfs_ppmd_encode/decode round-trips (1 B .. 2 MiB, random
 *                 + low-entropy structured) are bit-exact; corrupted /
 *                 mutated / truncated streams never crash the decoder.
 *                 NOTE: the PPMd stream itself carries no integrity check
 *                 (the segment layer above it CRCs); only the corruptions
 *                 with a DETECTABLE shape are asserted rc < 0 (bad props
 *                 bytes, < 7 stream bytes, decode past the END marker).
 *                 Arbitrary body mutations may decode to garbage with
 *                 rc == 0 -- those are counted, not failed.
 *   5. bcj        invfs_bcj_x86_enc/dec (the vendored z7_BranchConvSt_X86
 *                 core, see bcj_x86.c) round-trip bit-exactly on random and
 *                 x86-shaped buffers >= 16 B.
 *   6. mrmp       a LOCAL oracle reimplementing the WP16b map rules; the
 *                 rules mirror volume.c's cpack_map_parse/cpack_map_validate
 *                 (read-only reference -- volume.c is NOT linked here):
 *                 [4B "MRMP"][u32 count][count x 29B {u64 orig_off, u64 len,
 *                 u8 kind, u32 idx, u64 src_off}]; blob length exact, count
 *                 in [1, CPACK_MAP_MAX_ENTS], kind <= 1; entries sorted by
 *                 orig_off, partitioning [0, container_size) exactly
 *                 (contiguous, no gaps/overlaps, len > 0); RECIPE entries
 *                 have idx == 0 and fit the recipe blob; MEMBER entries
 *                 reference a known idx and fit its usize. Property: every
 *                 generated-valid map passes; single mutations are rejected
 *                 (the rare crafted-valid mutation is allowed + counted).
 *
 * Everything is driven by one xorshift64* PRNG: a rerun with the same
 * [iterations] [seed] reproduces every buffer byte-for-byte.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "codec.h"
#include "invarifs.h"
#include "ppmd_codec.h"
#include "bcj_x86.h"

/* ---- test-local implementations of the pack exec hooks (WP13) ----
 * The production hooks live in volume.c (not linked here); codec.c's
 * trampolines reference them. The fuzzer never executes a pack (sniff /
 * registration only), so unconditional failure stubs are the contract. */
int invfs_codec_pack_exec(const invfs_codec *c, int is_encode,
                          const char *in_path, const char *out_path)
{
    (void)c; (void)is_encode; (void)in_path; (void)out_path;
    return -1;
}

int invfs_codec_pack_estimate(const invfs_codec *c, const char *in_path,
                              uint64_t *out_bytes)
{
    (void)c; (void)in_path; (void)out_bytes;
    return -1;
}

/* ---------------- deterministic PRNG (xorshift64*) ---------------- */

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t xr_next(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static uint64_t xr_below(uint64_t n)   /* 0 when n == 0 (callers avoid) */
{
    return n ? xr_next() % n : 0;
}

/* ---------------- result tracking ---------------- */

typedef struct {
    const char *name;
    uint64_t    cases;
    uint64_t    fails;
    uint64_t    printed;    /* failure prints capped per leg */
} leg_t;

static void leg_fail(leg_t *l, uint64_t iter, const char *fmt, ...)
{
    va_list ap;

    l->fails++;
    if (l->printed < 20) {
        l->printed++;
        fprintf(stderr, "FAIL[%s] iter=%" PRIu64 ": ", l->name, iter);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fprintf(stderr, "\n");
    }
}

static double leg_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void leg_done(leg_t *l, double t0, const char *info)
{
    printf("leg %-10s: %" PRIu64 " cases, %" PRIu64 " failed, %6.1fs%s%s%s\n",
           l->name, l->cases, l->fails, leg_now() - t0,
           info ? "  (" : "", info ? info : "", info ? ")" : "");
}

/* ---------------- buffer generators ---------------- */

static void gen_random(uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) b[i] = (uint8_t)xr_next();
}

static void gen_printable(uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint64_t r = xr_below(98);
        b[i] = r < 95 ? (uint8_t)(32 + r)
                      : (uint8_t)(r == 95 ? '\t' : r == 96 ? '\n' : '\r');
    }
}

/* low-entropy structured content for the PPMd stress leg */
static void gen_structured(uint8_t *b, size_t n)
{
    size_t i = 0;

    switch (xr_below(4)) {
    case 0:     /* RLE runs */
        while (i < n) {
            uint8_t v = (uint8_t)xr_next();
            size_t run = 1 + (size_t)xr_below(64);
            while (run-- && i < n) b[i++] = v;
        }
        break;
    case 1:     /* tiny alphabet */
        {
            uint8_t alpha[4];
            size_t k;
            for (k = 0; k < 4; k++) alpha[k] = (uint8_t)xr_next();
            for (i = 0; i < n; i++) b[i] = alpha[xr_below(4)];
        }
        break;
    case 2:     /* repeated random block with rare mutations */
        {
            uint8_t blk[256];
            size_t bl = 16 + (size_t)xr_below(241);
            gen_random(blk, bl);
            while (i < n) {
                size_t k;
                for (k = 0; k < bl && i < n; k++)
                    b[i++] = (xr_below(64) == 0) ? (uint8_t)xr_next()
                                                 : blk[k];
            }
        }
        break;
    default:    /* text-ish words with a counter */
        {
            static const char *const words[] = {
                "the ", "quick ", "brown ", "fox ", "compression ",
                "codec ", "sector ", "inode ", "\n"
            };
            unsigned ln = 0;
            while (i < n) {
                const char *w = words[xr_below(9)];
                size_t wl = strlen(w), k;
                for (k = 0; k < wl && i < n; k++) b[i++] = (uint8_t)w[k];
                if (++ln % 40 == 0 && i < n) b[i++] = (uint8_t)('0' + ln % 10);
            }
        }
        break;
    }
}

/* random file name for the classifiers: NULL / no-ext / known ext / junk */
static const char *gen_name(char *buf, size_t cap)
{
    static const char *const exts[] = {
        "c", "py", "json", "txt", "html", "sh", "bin", "zzz", "C", "MD"
    };
    uint64_t kind = xr_below(6);

    if (kind == 0) return NULL;
    if (kind == 1) return "noext";
    if (kind < 4) {
        snprintf(buf, cap, "file.%s", exts[xr_below(10)]);
        return buf;
    }
    /* random junk name, maybe with a dot */
    {
        size_t n = 1 + (size_t)xr_below(20), i;
        if (n + 2 > cap) n = cap - 2;
        for (i = 0; i < n; i++)
            buf[i] = (char)(32 + xr_below(95));
        if (xr_below(2) && n > 2) buf[n / 2] = '.';
        buf[n] = '\0';
        return buf;
    }
}

/* ================= leg 1: sniff fuzz ================= */

/* Mirrors the static sniff magics in codec.c (zip/tarr/gzr/pngr/flacr/pmp
 * ID3/jxl box/ape/wv; exer goes through invfs_binary_family's ELF magic and
 * needs the >= 4096 binary gate). Used to build magic-mutation buffers. */
typedef struct {
    uint32_t algo;
    uint8_t  magic[8];
    size_t   mlen;
    size_t   off;
    int      hit;           /* score the codec gives a full magic hit */
} sniff_magic;

static const sniff_magic sniff_magics[] = {
    { INVFS_ALGO_ZIPR,  { 'P', 'K', 0x03, 0x04 },              4,   0, 100 },
    { INVFS_ALGO_TARR,  { 'u', 's', 't', 'a', 'r' },           5, 257, 100 },
    { INVFS_ALGO_GZR,   { 0x1F, 0x8B },                        2,   0, 100 },
    { INVFS_ALGO_PNGR,  { 0x89, 'P', 'N', 'G',
                          '\r', '\n', 0x1A, '\n' },            8,   0, 100 },
    { INVFS_ALGO_FLACR, { 'f', 'L', 'a', 'C' },                4,   0, 100 },
    { INVFS_ALGO_PMP,   { 'I', 'D', '3' },                     3,   0, 100 },
    { INVFS_ALGO_JXL,   { 0, 0, 0, 0x0C, 'J', 'X', 'L', ' ' }, 8,   0, 100 },
    { INVFS_ALGO_APE,   { 'M', 'A', 'C', ' ' },                4,   0, 100 },
    { INVFS_ALGO_WV,    { 'w', 'v', 'p', 'k' },                4,   0, 100 },
    { INVFS_ALGO_EXER,  { 0x7F, 'E', 'L', 'F' },               4,   0,  40 },
};

static leg_t leg_sniff(uint64_t iters)
{
    leg_t leg = { "sniff", 0, 0, 0 };
    double t0 = leg_now();
    uint8_t buf[8192], work[8192];
    char namebuf[64];
    size_t ncodecs = 0, ci, mi;
    const invfs_codec *all = invfs_codec_all(&ncodecs);
    uint64_t it;

    if (!all || !ncodecs) {
        leg_fail(&leg, 0, "empty registry");
        leg_done(&leg, t0, NULL);
        return leg;
    }

    for (it = 0; it < iters; it++) {
        size_t len = (size_t)xr_below(sizeof buf + 1);
        const char *name = gen_name(namebuf, sizeof namebuf);

        /* random buffer (or NULL head at len 0) through EVERY codec sniff */
        gen_random(buf, len);
        for (ci = 0; ci < ncodecs; ci++) {
            const invfs_codec *c = &all[ci];
            const uint8_t *head = (len == 0 && xr_below(2)) ? NULL : buf;
            int s1, s2;
            if (!c->sniff) continue;
            leg.cases++;
            s1 = c->sniff(head, len, name);
            if (s1 < 0 || s1 > 100)
                leg_fail(&leg, it, "codec %s: score %d out of [0,100]",
                         c->name, s1);
            s2 = c->sniff(head, len, name);
            if (s1 != s2)
                leg_fail(&leg, it, "codec %s: sniff not deterministic "
                         "(%d then %d)", c->name, s1, s2);
        }

        /* magic-mutated buffers, one table entry per iteration */
        mi = (size_t)(it % (sizeof sniff_magics / sizeof sniff_magics[0]));
        {
            const sniff_magic *m = &sniff_magics[mi];
            const invfs_codec *c = invfs_codec_by_algo(m->algo);
            if (c && c->sniff) {
                size_t need = m->off + m->mlen;
                size_t blen = need + (size_t)xr_below(sizeof buf - need + 1);
                int s_with, s_without, s_flip, s_trunc;
                size_t j;

                if (m->algo == INVFS_ALGO_EXER && blen < 4096)
                    blen = 4096 + (size_t)xr_below(sizeof buf - 4096 + 1);

                /* valid magic + random tail */
                gen_random(buf, blen);
                memcpy(buf + m->off, m->magic, m->mlen);
                s_with = c->sniff(buf, blen, "fuzz.bin");
                leg.cases++;
                if (s_with != m->hit)
                    leg_fail(&leg, it, "codec %s: magic hit scores %d, "
                             "want %d", c->name, s_with, m->hit);

                /* same buffer WITHOUT the magic must score strictly lower */
                memcpy(work, buf, blen);
                for (j = 0; j < m->mlen; j++) work[m->off + j] = 0;
                s_without = c->sniff(work, blen, "fuzz.bin");
                leg.cases++;
                if (!(s_without < s_with))
                    leg_fail(&leg, it, "codec %s: magic-less buffer scores "
                             "%d, hit %d (must be strictly lower)",
                             c->name, s_without, s_with);

                /* one bit flipped inside the magic */
                memcpy(work, buf, blen);
                work[m->off + xr_below(m->mlen)] ^=
                    (uint8_t)(1u << xr_below(8));
                s_flip = c->sniff(work, blen, "fuzz.bin");
                leg.cases++;
                if (!(s_flip < s_with))
                    leg_fail(&leg, it, "codec %s: bit-flipped magic scores "
                             "%d, hit %d", c->name, s_flip, s_with);

                /* truncated mid-magic */
                s_trunc = c->sniff(buf, need - 1, "fuzz.bin");
                leg.cases++;
                if (!(s_trunc < s_with))
                    leg_fail(&leg, it, "codec %s: truncated magic scores "
                             "%d, hit %d", c->name, s_trunc, s_with);
            }
        }
    }
    leg_done(&leg, t0, "0..100 bounds, determinism, magic dominance");
    return leg;
}

/* ================= leg 2: classifier fuzz ================= */

static leg_t leg_classifier(uint64_t iters)
{
    leg_t leg = { "classifier", 0, 0, 0 };
    double t0 = leg_now();
    uint8_t buf[8192];
    char namebuf[64];
    uint64_t it;

    for (it = 0; it < iters; it++) {
        size_t len = (size_t)xr_below(sizeof buf + 1);
        unsigned shape = (unsigned)xr_below(5);
        const char *name = gen_name(namebuf, sizeof namebuf);
        int t1, t2, b1, b2;

        switch (shape) {
        case 0:  gen_random(buf, len); break;
        case 1:  gen_printable(buf, len); break;
        case 2:  /* printable + a planted NUL (never text) */
            gen_printable(buf, len);
            if (len) buf[xr_below(len)] = 0;
            break;
        case 3:  /* executable magic at a random (often < 4 KiB) size */
            gen_random(buf, len);
            if (len >= 4) {
                switch (xr_below(3)) {
                case 0: buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L';
                        buf[3] = 'F'; break;
                case 1: buf[0] = 'M'; buf[1] = 'Z'; break;
                default: buf[0] = 0xFE; buf[1] = 0xED; buf[2] = 0xFA;
                         buf[3] = 0xCF; break;
                }
            }
            break;
        default: /* text-ish */
            gen_structured(buf, len);
            break;
        }

        t1 = invfs_text_family(name, buf, len);
        t2 = invfs_text_family(name, buf, len);
        leg.cases++;
        if (t1 != t2)
            leg_fail(&leg, it, "text_family not deterministic (%d then %d)",
                     t1, t2);
        if (t1 < 0 || t1 > INVFS_TEXT_FAMILY_CONTENT)
            leg_fail(&leg, it, "text_family %d outside 0..%d", t1,
                     INVFS_TEXT_FAMILY_CONTENT);

        b1 = invfs_binary_family(buf, len, name);
        b2 = invfs_binary_family(buf, len, name);
        leg.cases++;
        if (b1 != b2)
            leg_fail(&leg, it, "binary_family not deterministic (%d then %d)",
                     b1, b2);
        if (b1 != 0 && (b1 < INVFS_BIN_FAMILY_ELF_X64 ||
                        b1 > INVFS_BIN_FAMILY_MACHO))
            leg_fail(&leg, it, "binary_family %d outside the 20..25 range",
                     b1);

        /* pure-printable content with no name -> text (CONTENT family) */
        if (shape == 1 && len > 0) {
            int t = invfs_text_family(NULL, buf, len);
            leg.cases++;
            if (t == 0)
                leg_fail(&leg, it, "pure-printable %zu-byte buffer is not "
                         "text", len);
        }
        /* a NUL anywhere in the window -> never text (no name) */
        if (shape == 2 && len > 0) {
            int t = invfs_text_family(NULL, buf, len);
            leg.cases++;
            if (t != 0)
                leg_fail(&leg, it, "buffer with NUL classified text (%d)",
                         t);
        }
        /* < 4 KiB -> never binary-family, magic or not */
        if (len < 4096) {
            leg.cases++;
            if (b1 != 0)
                leg_fail(&leg, it, "%zu-byte buffer classified binary (%d)",
                         len, b1);
        }
    }
    leg_done(&leg, t0, "determinism + text/binary invariants");
    return leg;
}

/* ================= leg 3: manifest parser fuzz ================= */

/* Drives the REAL parser through the filesystem: each manifest lands in
 * <root>/packs/fz.codecpack/manifest, INVFS_CODECPACKS points at packs/,
 * and invfs_codec_probe_reset() + invfs_codec_all() force a rescan through
 * pack_scan_dir -> parse_manifest -> pack_register. */

#define MF_MAX_LINES 64
#define MF_LINE_CAP  4200

typedef struct {
    char     line[MF_MAX_LINES][MF_LINE_CAP];
    char     key[MF_MAX_LINES][16];   /* the key a line carries ("" = none) */
    int      nlines;
    /* intended values (round-trip checks) */
    char     name[48];
    long     algo;
    int      is_container;
    int      has_map;
    uint32_t caps;
    uint64_t dec_mem;
    unsigned generation;
    size_t   n_magic;
    struct { uint8_t bytes[16]; size_t len; size_t off; } magic[3];
    char     exts[64];                /* comma list ("" = none) */
} mf_gen;

static void mf_line(mf_gen *g, const char *key, const char *fmt, ...)
{
    va_list ap;
    char *l;
    size_t off = 0;

    if (g->nlines >= MF_MAX_LINES) return;
    l = g->line[g->nlines];
    if (xr_below(2)) l[off++] = xr_below(2) ? ' ' : '\t';   /* leading ws */
    va_start(ap, fmt);
    vsnprintf(l + off, MF_LINE_CAP - off, fmt, ap);
    va_end(ap);
    snprintf(g->key[g->nlines], sizeof g->key[0], "%s", key ? key : "");
    g->nlines++;
}

static void mf_hex(char *dst, const uint8_t *b, size_t n)
{
    static const char hexd[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        dst[2 * i]     = hexd[b[i] >> 4];
        dst[2 * i + 1] = hexd[b[i] & 15];
    }
    dst[2 * n] = '\0';
}

static void mf_generate(mf_gen *g)
{
    static const char *const captok[5] = {
        "seek", "batched", "wholefile", "container", "external"
    };
    static const uint32_t capbit[5] = {
        INVFS_CODEC_CAP_SEEK, INVFS_CODEC_CAP_BATCHED,
        INVFS_CODEC_CAP_WHOLEFILE, INVFS_CODEC_CAP_CONTAINER,
        INVFS_CODEC_CAP_EXTERNAL
    };
    char caps_s[128] = "";
    char hex[40];
    size_t i, j;

    memset(g, 0, sizeof *g);

    /* fields */
    snprintf(g->name, sizeof g->name, "fz");
    {
        size_t want = 1 + (size_t)xr_below(12), l = strlen(g->name);
        for (i = 0; i < want && l + 2 < sizeof g->name; i++)
            g->name[l++] = (char)('a' + xr_below(26));
        g->name[l] = '\0';
    }
    g->algo = 40 + (long)xr_below(24);          /* 40..63: free per WP16d */
    g->is_container = xr_below(4) == 0;         /* 25% container packs */
    g->caps = 0;
    for (i = 0; i < 5; i++)
        if (xr_below(2)) {
            g->caps |= capbit[i];
            if (caps_s[0]) strcat(caps_s, "|");
            strcat(caps_s, captok[i]);
        }
    g->dec_mem = xr_next() & 0xFFFFFFFFull;
    g->generation = (unsigned)xr_below(65536);
    g->n_magic = (size_t)xr_below(4);           /* 0..3 rules */
    for (i = 0; i < g->n_magic; i++) {
        g->magic[i].len = 1 + (size_t)xr_below(16);
        for (j = 0; j < g->magic[i].len; j++)
            g->magic[i].bytes[j] = (uint8_t)xr_next();
        g->magic[i].off = (size_t)xr_below(301);
    }
    g->exts[0] = '\0';
    if (xr_below(2)) {
        size_t nex = 1 + (size_t)xr_below(3), p = 0;
        for (i = 0; i < nex && p + 8 < sizeof g->exts; i++) {
            size_t el = 1 + (size_t)xr_below(5), k;
            if (i) g->exts[p++] = ',';
            for (k = 0; k < el; k++)
                g->exts[p++] = (char)('a' + xr_below(26));
            g->exts[p] = '\0';
        }
    }
    g->has_map = g->is_container && xr_below(2);

    /* lines */
    mf_line(g, "name", "name = %s", g->name);
    mf_line(g, "algo", "algo=%ld", g->algo);
    if (g->is_container) mf_line(g, "type", "type = container");
    mf_line(g, "caps", "caps = %s", caps_s);
    mf_line(g, "dec_mem", "dec_mem = %" PRIu64, g->dec_mem);
    mf_line(g, "generation", "generation=%u", g->generation);
    mf_line(g, "pack_version", "pack_version = 1");
    if (g->exts[0]) mf_line(g, "sniff.ext", "sniff.ext = %s", g->exts);
    for (i = 0; i < g->n_magic; i++) {
        mf_hex(hex, g->magic[i].bytes, g->magic[i].len);
        if (xr_below(2)) {   /* offset BEFORE magic: the pending-off path */
            mf_line(g, "sniff.offset", "sniff.offset = %zu", g->magic[i].off);
            mf_line(g, "sniff.magic", "sniff.magic = %s", hex);
        } else {             /* magic then offset: the most-recent path */
            mf_line(g, "sniff.magic", "sniff.magic=%s", hex);
            mf_line(g, "sniff.offset", "sniff.offset=%zu", g->magic[i].off);
        }
    }
    if (g->is_container) {
        mf_line(g, "enumerate", "enumerate = cp {in} {out}");
        mf_line(g, "extract", "extract = cp {in} {idx} {out}");
        mf_line(g, "strip", "strip = cp {in} {out}");
        mf_line(g, "rebuild", "rebuild = cp {recipe} {dir} {out}");
        if (g->has_map) mf_line(g, "map", "map = cp {in} {out}");
    } else {
        mf_line(g, "encode", "encode = cp {in} {out}");
        mf_line(g, "decode", "decode = cp {in} {out}");
        if (xr_below(4) == 0)
            mf_line(g, "map", "map = cp {in} {out}");  /* codec pack: ignored */
    }
    /* noise the parser must skip */
    if (xr_below(2)) mf_line(g, "", "# a comment line");
    if (xr_below(2)) mf_line(g, "", "bogus line without equals");
    if (xr_below(2)) mf_line(g, "", "unknown.key = whatever");

    /* shuffle (the parser is order-insensitive for these keys). The
     * sniff.offset/sniff.magic pairs move as ONE block: offset semantics
     * are positional (the most recent magic rule, or pending for the next),
     * so splitting a pair would change the rules the manifest describes. */
    if (g->nlines > 1) {
        int span[MF_MAX_LINES][2];   /* start, len of each block */
        int nblocks = 0, bi = 0, a;
        char ol[MF_MAX_LINES][MF_LINE_CAP];
        char ok[MF_MAX_LINES][16];

        while (bi < g->nlines) {
            int bj = bi + 1;
            if (bj < g->nlines &&
                ((!strcmp(g->key[bi], "sniff.offset") &&
                  !strcmp(g->key[bj], "sniff.magic")) ||
                 (!strcmp(g->key[bi], "sniff.magic") &&
                  !strcmp(g->key[bj], "sniff.offset"))))
                bj++;
            span[nblocks][0] = bi;
            span[nblocks][1] = bj - bi;
            nblocks++;
            bi = bj;
        }
        for (a = nblocks - 1; a > 0; a--) {
            int b = (int)xr_below((uint64_t)a + 1);
            int ts[2];
            ts[0] = span[a][0]; ts[1] = span[a][1];
            span[a][0] = span[b][0]; span[a][1] = span[b][1];
            span[b][0] = ts[0]; span[b][1] = ts[1];
        }
        {
            int dst = 0, b2, l2;
            for (b2 = 0; b2 < nblocks; b2++)
                for (l2 = 0; l2 < span[b2][1]; l2++) {
                    memcpy(ol[dst], g->line[span[b2][0] + l2], MF_LINE_CAP);
                    memcpy(ok[dst], g->key[span[b2][0] + l2], 16);
                    dst++;
                }
            memcpy(g->line, ol, sizeof ol);
            memcpy(g->key, ok, sizeof ok);
        }
    }
}

/* render the line table to text */
static size_t mf_render(const mf_gen *g, char *out, size_t cap)
{
    size_t p = 0;
    int i;

    for (i = 0; i < g->nlines; i++) {
        size_t l = strlen(g->line[i]);
        int crlf = xr_below(4) == 0;    /* sometimes \r\n endings */
        if (p + l + 3 > cap) break;
        memcpy(out + p, g->line[i], l);
        p += l;
        if (crlf) out[p++] = '\r';
        out[p++] = '\n';
    }
    out[p] = '\0';
    return p;
}

static int mf_write(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    return fclose(f);
}

/* static registry algos: everything a pack must NOT collide with */
static int mf_algo_is_static(uint32_t a)
{
    static const uint32_t st[] = {
        INVFS_ALGO_NONE, INVFS_ALGO_ZSTD, INVFS_ALGO_PPMD, INVFS_ALGO_APE,
        INVFS_ALGO_JXL, INVFS_ALGO_LZ4, INVFS_ALGO_WV, INVFS_ALGO_FLACR,
        INVFS_ALGO_TARR, INVFS_ALGO_GZR, INVFS_ALGO_PNGR, INVFS_ALGO_PMP,
        INVFS_ALGO_ZIPR, INVFS_ALGO_EXER
    };
    size_t i;
    for (i = 0; i < sizeof st / sizeof st[0]; i++)
        if (st[i] == a) return 1;
    return 0;
}

/* find up to 2 registered entries whose algo is outside the baseline set */
static size_t mf_new_entries(const invfs_codec *all, size_t n,
                             const uint32_t *base_algo, size_t base_n,
                             const invfs_codec **out)
{
    size_t i, found = 0, k;
    for (i = 0; i < n && found < 2; i++) {
        int known = 0;
        for (k = 0; k < base_n; k++)
            if (base_algo[k] == all[i].algo) { known = 1; break; }
        if (!known) out[found++] = &all[i];
    }
    return found;
}

/* the "never half-accepted" invariants: anything that registers must be a
 * fully-formed pack entry per the documented registration rules */
static int mf_entry_consistent(const invfs_codec *c)
{
    const invfs_pack_def *def;

    if (!c->name || !c->name[0]) return 0;
    if (c->algo >= 64 || mf_algo_is_static(c->algo)) return 0;
    if (!c->sniff || !c->probe) return 0;
    def = invfs_codec_pack_def(c);
    if (!def) return 0;
    if (def->is_container) {
        if (c->encode || c->decode) return 0;      /* containers never */
        if (!(c->caps & INVFS_CODEC_CAP_CONTAINER) ||
            !(c->caps & INVFS_CODEC_CAP_EXTERNAL) ||
            !(c->caps & INVFS_CODEC_CAP_WHOLEFILE))
            return 0;                              /* caps forced */
        if (!def->enumerate || !def->extract || !def->strip || !def->rebuild)
            return 0;
        return ((c->caps & INVFS_CODEC_CAP_SEEK) != 0) == (def->map != NULL);
    }
    /* codec pack */
    if (!c->encode || !c->decode) return 0;        /* trampolines wired */
    if (def->map) return 0;                        /* map line ignored */
    return 1;
}

static int mf_required_key(const mf_gen *g, const char *key)
{
    if (!key[0]) return 0;
    if (!strcmp(key, "name") || !strcmp(key, "algo")) return 1;
    if (g->is_container) {
        if (!strcmp(key, "type")) return 1;   /* dropping it flips to codec */
        return !strcmp(key, "enumerate") || !strcmp(key, "extract") ||
               !strcmp(key, "strip") || !strcmp(key, "rebuild");
    }
    return !strcmp(key, "encode") || !strcmp(key, "decode");
}

static leg_t leg_manifest(uint64_t iters)
{
    leg_t leg = { "manifest", 0, 0, 0 };
    double t0 = leg_now();
    char root[128], packs[160], pack[192], mpath[224];
    uint32_t base_algo[64];
    size_t base_n = 0, i;
    const invfs_codec *all;
    uint64_t it;
    mf_gen gen;
    char text[16384];
    size_t text_len;

    snprintf(root, sizeof root, "/tmp/invfs-fuzz-%d", (int)getpid());
    snprintf(packs, sizeof packs, "%s/packs", root);
    snprintf(pack, sizeof pack, "%s/fz.codecpack", packs);
    snprintf(mpath, sizeof mpath, "%s/manifest", pack);
    if (mkdir(root, 0755) != 0 || mkdir(packs, 0755) != 0 ||
        mkdir(pack, 0755) != 0) {
        leg_fail(&leg, 0, "cannot create temp dirs under /tmp");
        leg_done(&leg, t0, NULL);
        return leg;
    }

    setenv("INVFS_CODECPACKS", packs, 1);
    invfs_codec_probe_reset();
    all = invfs_codec_all(&base_n);
    if (!all || base_n == 0 || base_n > 64) {
        leg_fail(&leg, 0, "baseline registry broken");
        goto out;
    }
    for (i = 0; i < base_n; i++) base_algo[i] = all[i].algo;

    for (it = 0; it < iters; it++) {
        const invfs_codec *found[2];
        size_t nfound = 0, nnow = 0;
        uint64_t mode = xr_below(10);
        int dropped_required = 0;

        mf_generate(&gen);
        text_len = mf_render(&gen, text, sizeof text);

        if (mode < 2) {
            /* ---- round-trip: generated-valid manifest ---- */
            leg.cases++;
            if (mf_write(mpath, text, text_len) != 0) {
                leg_fail(&leg, it, "temp manifest write failed");
                continue;
            }
            invfs_codec_probe_reset();
            all = invfs_codec_all(&nnow);
            nfound = mf_new_entries(all, nnow, base_algo, base_n, found);
            if (nnow != base_n + 1 || nfound != 1) {
                leg_fail(&leg, it, "valid manifest did not register "
                         "(n=%zu base=%zu new=%zu)", nnow, base_n, nfound);
            } else {
                const invfs_codec *c = found[0];
                const invfs_pack_def *def = invfs_codec_pack_def(c);
                leg.cases++;
                if (strcmp(c->name, gen.name) != 0 ||
                    c->algo != (uint32_t)gen.algo ||
                    c->dec_mem_bytes != gen.dec_mem ||
                    c->generation != (uint16_t)gen.generation)
                    leg_fail(&leg, it, "field mismatch: name=%s algo=%u "
                             "dec_mem=%" PRIu64 " gen=%u", c->name, c->algo,
                             (uint64_t)c->dec_mem_bytes, c->generation);
                if (!mf_entry_consistent(c))
                    leg_fail(&leg, it, "round-trip entry inconsistent");
                if (!gen.is_container && c->caps != gen.caps)
                    leg_fail(&leg, it, "codec pack caps %u != generated %u",
                             c->caps, gen.caps);
                if (gen.is_container && def &&
                    (def->map != NULL) != (gen.has_map != 0))
                    leg_fail(&leg, it, "container map wiring mismatch");
                /* sniff: each magic rule -> 100; first ext -> 50 */
                for (i = 0; i < gen.n_magic; i++) {
                    uint8_t hb[1024];
                    size_t need = gen.magic[i].off + gen.magic[i].len;
                    size_t hl = need + 8;          /* off cap is 300: fits */
                    int s;
                    memset(hb, 0xAA, hl);
                    memcpy(hb + gen.magic[i].off, gen.magic[i].bytes,
                           gen.magic[i].len);
                    leg.cases++;
                    s = c->sniff(hb, hl, "probe.bin");
                    if (s != 100) {
                        char dbg[256];
                        FILE *df;
                        snprintf(dbg, sizeof dbg,
                                 "/tmp/invf-fuzz-fail-%d-%llu.manifest",
                                 (int)getpid(), (unsigned long long)it);
                        df = fopen(dbg, "wb");
                        if (df) {
                            fwrite(text, 1, text_len, df);
                            fclose(df);
                            fprintf(stderr, "  (manifest dumped to %s; "
                                    "rule bytes:", dbg);
                            for (size_t db = 0; db < gen.magic[i].len; db++)
                                fprintf(stderr, " %02x",
                                        gen.magic[i].bytes[db]);
                            fprintf(stderr, " off=%zu)\n", gen.magic[i].off);
                        }
                        leg_fail(&leg, it, "pack sniff magic@%zu -> %d",
                                 gen.magic[i].off, s);
                    }
                }
                if (gen.exts[0]) {
                    char xn[80];
                    uint8_t zeros[64];
                    const char *comma = strchr(gen.exts, ',');
                    size_t el = comma ? (size_t)(comma - gen.exts)
                                      : strlen(gen.exts);
                    memset(zeros, 0, sizeof zeros);
                    snprintf(xn, sizeof xn, "x.%.*s", (int)el, gen.exts);
                    leg.cases += 2;
                    if (c->sniff(zeros, sizeof zeros, xn) != 50)
                        leg_fail(&leg, it, "pack sniff ext -> != 50");
                    if (c->sniff(zeros, sizeof zeros, "x.zzz") != 0)
                        leg_fail(&leg, it, "pack sniff unknown ext -> != 0");
                }
            }
        } else {
            /* ---- mutated manifest ---- */
            unsigned mut = (unsigned)xr_below(9);
            char mtext[20480];
            size_t mlen = text_len;

            memcpy(mtext, text, text_len + 1);
            switch (mut) {
            case 0:  /* truncate at a random byte */
                if (mlen) mlen = (size_t)xr_below(mlen + 1);
                break;
            case 1:  /* flip 1..8 random bytes (may hit non-UTF8 range) */
                {
                    unsigned f = 1 + (unsigned)xr_below(8);
                    while (f-- && mlen)
                        mtext[xr_below(mlen)] = (char)(uint8_t)xr_next();
                }
                break;
            case 2:  /* drop a random line (targeted must-reject check) */
            case 3:  /* duplicate a random line */
                {
                    mf_gen mcpy = gen;
                    int li = gen.nlines ? (int)xr_below((uint64_t)gen.nlines)
                                        : -1;
                    if (li >= 0) {
                        if (mut == 2) {
                            dropped_required =
                                mf_required_key(&gen, gen.key[li]);
                            memmove(&mcpy.line[li], &mcpy.line[li + 1],
                                    (size_t)(mcpy.nlines - li - 1) *
                                    MF_LINE_CAP);
                            memmove(&mcpy.key[li], &mcpy.key[li + 1],
                                    (size_t)(mcpy.nlines - li - 1) * 16);
                            mcpy.nlines--;
                        } else if (mcpy.nlines < MF_MAX_LINES) {
                            memmove(&mcpy.line[li + 1], &mcpy.line[li],
                                    (size_t)(mcpy.nlines - li) *
                                    MF_LINE_CAP);
                            memmove(&mcpy.key[li + 1], &mcpy.key[li],
                                    (size_t)(mcpy.nlines - li) * 16);
                            mcpy.nlines++;
                        }
                    }
                    mlen = mf_render(&mcpy, mtext, sizeof mtext);
                }
                break;
            case 4:  /* huge value (past the parser's 1024-byte line buf) */
                {
                    size_t p = 0;
                    p += (size_t)snprintf(mtext + p, sizeof(mtext) - p,
                                          "huge.key = ");
                    while (p < 4000) mtext[p++] = (char)('A' + xr_below(26));
                    mtext[p++] = '\n';
                    if (p + mlen < sizeof mtext) {
                        memcpy(mtext + p, text, mlen);
                        mlen += p;
                    } else {
                        mlen = p;
                    }
                }
                break;
            case 5:  /* non-UTF8 bytes */
                {
                    unsigned f = 1 + (unsigned)xr_below(6);
                    while (f-- && mlen)
                        mtext[xr_below(mlen)] = (char)(0x80 + xr_below(0x80));
                }
                break;
            case 6:  /* absurd numbers */
                {
                    mf_gen mcpy = gen;
                    int li;
                    for (li = 0; li < mcpy.nlines; li++)
                        if (!strcmp(mcpy.key[li], "algo"))
                            snprintf(mcpy.line[li], MF_LINE_CAP,
                                     "algo = 99999999999999999999999");
                        else if (!strcmp(mcpy.key[li], "dec_mem"))
                            snprintf(mcpy.line[li], MF_LINE_CAP,
                                     "dec_mem = 18446744073709551615000");
                        else if (!strcmp(mcpy.key[li], "generation"))
                            snprintf(mcpy.line[li], MF_LINE_CAP,
                                     "generation = -7");
                    mlen = mf_render(&mcpy, mtext, sizeof mtext);
                }
                break;
            case 7:  /* whitespace storm */
                {
                    mf_gen mcpy = gen;
                    int li;
                    for (li = 0; li < mcpy.nlines; li++) {
                        char tmp[MF_LINE_CAP];
                        snprintf(tmp, sizeof tmp, " \t %s \t ", mcpy.line[li]);
                        memcpy(mcpy.line[li], tmp, strlen(tmp) + 1);
                    }
                    mlen = mf_render(&mcpy, mtext, sizeof mtext);
                }
                break;
            default: /* pure garbage */
                mlen = (size_t)xr_below(4000);
                for (i = 0; i < mlen; i++)
                    mtext[i] = xr_below(4) ? (char)(32 + xr_below(95))
                                           : (char)(uint8_t)xr_next();
                break;
            }

            leg.cases++;
            if (mf_write(mpath, mtext, mlen) != 0) {
                leg_fail(&leg, it, "temp manifest write failed");
                continue;
            }
            invfs_codec_probe_reset();
            all = invfs_codec_all(&nnow);
            nfound = mf_new_entries(all, nnow, base_algo, base_n, found);

            /* never more than one pack from one manifest; never a loss */
            leg.cases++;
            if (nnow > base_n + 1 || nfound > 1 || nnow < base_n)
                leg_fail(&leg, it, "registry shape broken: n=%zu base=%zu "
                         "new=%zu (mut=%u)", nnow, base_n, nfound, mut);

            /* a required line was dropped -> MUST be fully rejected */
            if (dropped_required) {
                leg.cases++;
                if (nfound != 0)
                    leg_fail(&leg, it, "manifest missing a required key "
                             "registered anyway (half-accepted)");
            }

            /* anything that DID register must be fully consistent */
            if (nfound == 1) {
                leg.cases++;
                if (!mf_entry_consistent(found[0]))
                    leg_fail(&leg, it, "mutated manifest half-accepted "
                             "(mut=%u)", mut);
            }

            /* determinism: a rescan of the same file reproduces the shape
             * (compare against SAVED fields: the rescan overwrites the
             * registry array the first scan's pointers refer to) */
            leg.cases++;
            {
                char nm[64];
                uint32_t alg = 0, cps = 0;
                size_t n2 = 0, f2;
                const invfs_codec *found2[2];

                if (nfound == 1) {
                    snprintf(nm, sizeof nm, "%s", found[0]->name);
                    alg = found[0]->algo;
                    cps = found[0]->caps;
                }
                invfs_codec_probe_reset();
                all = invfs_codec_all(&n2);
                f2 = mf_new_entries(all, n2, base_algo, base_n, found2);
                if (n2 != nnow || f2 != nfound ||
                    (nfound == 1 &&
                     (strcmp(found2[0]->name, nm) != 0 ||
                      found2[0]->algo != alg || found2[0]->caps != cps)))
                    leg_fail(&leg, it, "manifest parse not deterministic");
            }
        }
    }

out:
    unsetenv("INVFS_CODECPACKS");
    invfs_codec_probe_reset();
    unlink(mpath);
    rmdir(pack);
    rmdir(packs);
    rmdir(root);
    leg_done(&leg, t0, "round-trip + mutated manifests via temp files");
    return leg;
}

/* ================= leg 4: PPMd round-trip stress ================= */

static leg_t leg_ppmd(uint64_t iters)
{
    leg_t leg = { "ppmd", 0, 0, 0 };
    double t0 = leg_now();
    uint64_t it;
    uint64_t corrupt_ok = 0, corrupt_fail = 0;

    for (it = 0; it < iters; it++) {
        size_t inlen, cap, clen = 0;
        uint8_t *in, *comp, *back;
        int rc;

        /* 90% small (1 B..8 KiB), 10% log-spread up to 2 MiB */
        if (xr_below(10) < 9)
            inlen = 1 + (size_t)xr_below(8192);
        else {
            unsigned e = 13 + (unsigned)xr_below(9);   /* 8 KiB .. 2 MiB */
            inlen = (size_t)1 << e;
            if (inlen > (2u << 20)) inlen = 2u << 20;
            inlen -= (size_t)xr_below(1024);
        }
        cap = inlen + inlen / 2 + 4096;
        in   = (uint8_t *)malloc(inlen);
        comp = (uint8_t *)malloc(cap);
        back = (uint8_t *)malloc(inlen + 4096);
        if (!in || !comp || !back) {
            leg_fail(&leg, it, "out of memory");
            free(in); free(comp); free(back);
            continue;
        }
        if (xr_below(2)) gen_random(in, inlen);
        else             gen_structured(in, inlen);

        leg.cases++;
        rc = invfs_ppmd_encode(in, inlen, comp, cap, &clen);
        if (rc != 0 || clen == 0) {
            leg_fail(&leg, it, "encode failed rc=%d inlen=%zu", rc, inlen);
            free(in); free(comp); free(back);
            continue;
        }
        leg.cases++;
        if (invfs_ppmd_decode(comp, clen, back, inlen) != 0 ||
            memcmp(back, in, inlen) != 0)
            leg_fail(&leg, it, "round-trip not bit-exact (inlen=%zu "
                     "clen=%zu)", inlen, clen);

        /* deterministic rejection: decode past the END marker */
        leg.cases++;
        if (invfs_ppmd_decode(comp, clen, back,
                              inlen + 1 + xr_below(64)) != -1)
            leg_fail(&leg, it, "decode past the END marker accepted");

        /* deterministic rejection: fewer than 7 stream bytes */
        leg.cases++;
        {
            size_t cut = clen < 6 ? clen : 6;
            if (invfs_ppmd_decode(comp, cut, back, inlen) != -1)
                leg_fail(&leg, it, "%zu-byte stream accepted (min is 7)",
                         cut);
        }

        /* deterministic rejection: corrupted props bytes (params pinned) */
        if (clen >= 2) {
            uint8_t save0 = comp[0];
            leg.cases++;
            comp[0] ^= 0xFF;
            if (invfs_ppmd_decode(comp, clen, back, inlen) != -1)
                leg_fail(&leg, it, "corrupted props accepted");
            comp[0] = save0;
        }

        /* corrupted / mutated / truncated streams: never crash (rc either
         * way -- the stream has no integrity layer; see the header note) */
        if (inlen <= (256u << 10)) {
            unsigned nmut = 1 + (unsigned)xr_below(3);
            while (nmut--) {
                unsigned how = (unsigned)xr_below(3);
                size_t mlen = clen;
                leg.cases++;
                if (how == 0 && clen > 7) {          /* truncate (>= 7) */
                    mlen = 7 + (size_t)xr_below(clen - 7);
                } else if (how == 1 && clen) {       /* bit flips */
                    unsigned f = 1 + (unsigned)xr_below(4);
                    while (f--)
                        comp[xr_below(clen)] ^=
                            (uint8_t)(1u << xr_below(8));
                } else if (clen) {                   /* garbage span */
                    size_t pos = (size_t)xr_below(clen);
                    size_t spn = 1 + (size_t)xr_below(clen - pos);
                    while (spn--) comp[pos++] = (uint8_t)xr_next();
                }
                rc = invfs_ppmd_decode(comp, mlen, back, inlen);
                if (rc == 0) corrupt_ok++; else corrupt_fail++;
                /* restore the stream for the next mutation */
                if (invfs_ppmd_encode(in, inlen, comp, cap, &clen) != 0) {
                    leg_fail(&leg, it, "re-encode after mutation failed");
                    break;
                }
            }
        }
        free(in); free(comp); free(back);
    }
    {
        char info[128];
        snprintf(info, sizeof info, "mutated streams: %" PRIu64
                 " rejected, %" PRIu64 " decoded (no integrity layer)",
                 corrupt_fail, corrupt_ok);
        leg_done(&leg, t0, info);
    }
    return leg;
}

/* ================= leg 5: BCJ x86 round-trip ================= */

static leg_t leg_bcj(uint64_t iters)
{
    leg_t leg = { "bcj", 0, 0, 0 };
    double t0 = leg_now();
    uint8_t buf[65536], work[65536];
    uint64_t it;

    for (it = 0; it < iters; it++) {
        size_t len = 16 + (size_t)xr_below(sizeof buf - 15);
        size_t i;

        switch (xr_below(5)) {
        case 0:  gen_random(buf, len); break;
        case 1:  memset(buf, 0xE8, len); break;                 /* all call */
        case 2:  memset(buf, 0x00, len); break;
        case 3:  for (i = 0; i < len; i++) buf[i] = (i & 1) ? 0xE8 : 0xE9;
                 break;
        default: /* x86-shaped: planted call/jmp sites in random filler */
            gen_random(buf, len);
            for (i = 0; i + 5 <= len; i += 4) {
                if (xr_below(8) == 0) {
                    uint32_t rel = (uint32_t)xr_next();
                    buf[i] = xr_below(2) ? 0xE8 : 0xE9;
                    buf[i + 1] = (uint8_t)rel;
                    buf[i + 2] = (uint8_t)(rel >> 8);
                    buf[i + 3] = (uint8_t)(rel >> 16);
                    buf[i + 4] = (uint8_t)(rel >> 24);
                }
            }
            break;
        }

        memcpy(work, buf, len);
        leg.cases++;
        invfs_bcj_x86_enc(work, len);
        invfs_bcj_x86_dec(work, len);
        if (memcmp(work, buf, len) != 0)
            leg_fail(&leg, it, "BCJ enc->dec not bit-exact (len=%zu)", len);
    }
    leg_done(&leg, t0, "enc/dec bijectivity, 16 B..64 KiB");
    return leg;
}

/* ================= leg 6: MRMP validator oracle ================= */

/* The rules below MIRROR volume.c's cpack_map_parse / cpack_map_validate
 * (WP16b, read-only reference -- volume.c is another workstream's and is
 * NOT linked here): [4B "MRMP"][u32 count][count x 29-byte entries
 * {u64 orig_off, u64 len, u8 kind, u32 idx, u64 src_off}]; blob length
 * exactly 8 + count*29; count in [1, CPACK_MAP_MAX_ENTS]; kind <= 1;
 * entries sorted by orig_off and partitioning [0, container_size) exactly
 * (each orig_off == running pos, len > 0, pos+len <= size, final pos ==
 * size); RECIPE (kind 0): idx == 0 and [src_off, +len) inside the recipe;
 * MEMBER (kind 1): idx in the member table and [src_off, +len) inside its
 * usize. This leg property-tests the ORACLE: generated-valid maps always
 * pass; single-mutation maps are rejected (a rare crafted-valid mutation
 * is allowed and counted). */

#define MRMP_ENT_WIRE  29
#define MRMP_MAX_ENTS  (4u * 65536u + 4u)   /* = CPACK_MAP_MAX_ENTS */
#define MRMP_MAX_MEM   16
#define MRMP_MAX_GEN_ENTS 32

typedef struct {
    uint64_t orig_off;
    uint64_t len;
    uint64_t src_off;
    uint32_t idx;
    uint8_t  kind;
} mrmp_ent;

typedef struct { uint32_t idx; uint64_t usize; } mrmp_member;

static void put_le64(uint8_t *p, uint64_t v)
{ unsigned i; for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_le32(uint8_t *p, uint32_t v)
{ unsigned i; for (i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static uint32_t get_le32(const uint8_t *p)
{
    uint32_t v = 0;
    unsigned i;
    for (i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

/* mirror of volume.c cpack_map_parse */
static int mrmp_parse(const uint8_t *blob, size_t blob_len,
                      mrmp_ent **out, size_t *n_out)
{
    mrmp_ent *ents;
    uint32_t count, i;

    *out = NULL;
    *n_out = 0;
    if (blob_len < 8 || memcmp(blob, "MRMP", 4) != 0) return -1;
    count = get_le32(blob + 4);
    if (!count || count > MRMP_MAX_ENTS) return -1;
    if (blob_len != 8 + (size_t)count * MRMP_ENT_WIRE) return -1;
    ents = (mrmp_ent *)malloc((size_t)count * sizeof *ents);
    if (!ents) return -1;
    for (i = 0; i < count; i++) {
        const uint8_t *p = blob + 8 + (size_t)i * MRMP_ENT_WIRE;
        ents[i].orig_off = get_le64(p);
        ents[i].len      = get_le64(p + 8);
        ents[i].kind     = p[16];
        ents[i].idx      = get_le32(p + 17);
        ents[i].src_off  = get_le64(p + 21);
        if (ents[i].kind > 1) { free(ents); return -1; }
    }
    *out = ents;
    *n_out = count;
    return 0;
}

static const mrmp_member *mrmp_find(const mrmp_member *mem, size_t n,
                                    uint32_t idx)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (mem[i].idx == idx) return &mem[i];
    return NULL;
}

/* mirror of volume.c cpack_map_validate */
static int mrmp_validate(const mrmp_ent *e, size_t n, uint64_t csize,
                         uint64_t recipe_len,
                         const mrmp_member *mem, size_t nmem)
{
    uint64_t pos = 0;
    size_t i;

    if (!n) return -1;
    for (i = 0; i < n; i++) {
        if (e[i].orig_off != pos)
            return -1;                       /* gap/overlap/unsorted */
        if (!e[i].len || e[i].len > csize - pos)
            return -1;                       /* zero-length or past the end */
        if (e[i].kind == 0) {                /* RECIPE */
            if (e[i].idx != 0) return -1;
            if (e[i].src_off > recipe_len ||
                e[i].len > recipe_len - e[i].src_off)
                return -1;
        } else {                             /* MEMBER */
            const mrmp_member *mm = mrmp_find(mem, nmem, e[i].idx);
            if (!mm) return -1;
            if (e[i].src_off > mm->usize ||
                e[i].len > mm->usize - e[i].src_off)
                return -1;
        }
        pos += e[i].len;
    }
    return pos == csize ? 0 : -1;
}

typedef struct {
    mrmp_ent    ents[MRMP_MAX_GEN_ENTS];
    size_t      nents;
    mrmp_member mem[MRMP_MAX_MEM];
    size_t      nmem;
    uint64_t    csize;
    uint64_t    recipe_len;
} mrmp_map;

/* generated-valid: a random exact partition, sources sized to fit (slack
 * kept tiny so a single field mutation almost always breaks the map) */
static void mrmp_generate(mrmp_map *m)
{
    size_t i, j;

    memset(m, 0, sizeof *m);
    m->csize = 1 + xr_below(1u << 20);
    m->nents = 1 + (size_t)xr_below(MRMP_MAX_GEN_ENTS);
    if ((uint64_t)m->nents > m->csize) m->nents = (size_t)m->csize;

    m->nmem = 1 + (size_t)xr_below(8);
    for (i = 0; i < m->nmem; i++) {           /* unique member idxs */
        uint32_t cand;
        int dup;
        do {
            cand = (uint32_t)xr_below(64);
            dup = 0;
            for (j = 0; j < i; j++)
                if (m->mem[j].idx == cand) dup = 1;
        } while (dup);
        m->mem[i].idx = cand;
        m->mem[i].usize = 0;                  /* grown to fit below */
    }

    {
        uint64_t remaining = m->csize, pos = 0;
        uint64_t need_recipe = 0;
        for (i = 0; i < m->nents; i++) {
            uint64_t left = (uint64_t)(m->nents - i);
            uint64_t len = (i == m->nents - 1)
                         ? remaining
                         : 1 + xr_below(remaining - (left - 1));
            m->ents[i].orig_off = pos;
            m->ents[i].len = len;
            m->ents[i].kind = (uint8_t)xr_below(2);
            if (m->ents[i].kind == 0) {
                m->ents[i].idx = 0;
                m->ents[i].src_off = xr_below(65);
                if (m->ents[i].src_off + len > need_recipe)
                    need_recipe = m->ents[i].src_off + len;
            } else {
                mrmp_member *mm = &m->mem[xr_below(m->nmem)];
                m->ents[i].idx = mm->idx;
                m->ents[i].src_off = xr_below(65);
                if (m->ents[i].src_off + len > mm->usize)
                    mm->usize = m->ents[i].src_off + len;
            }
            pos += len;
            remaining -= len;
        }
        m->recipe_len = need_recipe + xr_below(4);       /* tiny slack */
    }
    for (i = 0; i < m->nmem; i++) {
        m->mem[i].usize += xr_below(4);                  /* tiny slack */
        if (!m->mem[i].usize) m->mem[i].usize = xr_below(1024);
    }
}

static size_t mrmp_serialize(const mrmp_map *m, uint8_t *out)
{
    size_t i;
    uint8_t *p = out;

    memcpy(p, "MRMP", 4);
    put_le32(p + 4, (uint32_t)m->nents);
    p += 8;
    for (i = 0; i < m->nents; i++) {
        put_le64(p, m->ents[i].orig_off);
        put_le64(p + 8, m->ents[i].len);
        p[16] = m->ents[i].kind;
        put_le32(p + 17, m->ents[i].idx);
        put_le64(p + 21, m->ents[i].src_off);
        p += MRMP_ENT_WIRE;
    }
    return (size_t)(p - out);
}

static int mrmp_check(const uint8_t *blob, size_t blob_len,
                      uint64_t csize, uint64_t recipe_len,
                      const mrmp_member *mem, size_t nmem)
{
    mrmp_ent *ents = NULL;
    size_t n = 0;
    int rc;

    if (mrmp_parse(blob, blob_len, &ents, &n) != 0)
        return -1;
    rc = mrmp_validate(ents, n, csize, recipe_len, mem, nmem);
    free(ents);
    return rc;
}

static leg_t leg_mrmp(uint64_t iters)
{
    leg_t leg = { "mrmp", 0, 0, 0 };
    double t0 = leg_now();
    uint8_t blob[8 + MRMP_MAX_GEN_ENTS * MRMP_ENT_WIRE];
    uint8_t mut[sizeof blob];
    uint64_t it;
    uint64_t crafted = 0, rejected = 0;

    for (it = 0; it < iters; it++) {
        mrmp_map m;
        size_t blen;
        unsigned mut_kind;

        mrmp_generate(&m);
        blen = mrmp_serialize(&m, blob);

        /* property 1: generated-valid maps ALWAYS pass */
        leg.cases++;
        if (mrmp_check(blob, blen, m.csize, m.recipe_len, m.mem, m.nmem) != 0)
            leg_fail(&leg, it, "generated-valid map rejected (nents=%zu "
                     "csize=%" PRIu64 ")", m.nents, m.csize);

        /* property 2: single mutations are rejected */
        mut_kind = (unsigned)xr_below(14);
        memcpy(mut, blob, blen);
        {
            uint64_t csize = m.csize, rlen = m.recipe_len;
            mrmp_member mmem[MRMP_MAX_MEM];
            size_t mnmem = m.nmem, mlen = blen;
            size_t ent_i = (size_t)xr_below(m.nents);

            memcpy(mmem, m.mem, sizeof mmem);
            switch (mut_kind) {
            case 0:  /* flip a random byte anywhere */
                mut[xr_below(blen)] ^= (uint8_t)(1u << xr_below(8));
                break;
            case 1:  /* truncate */
                mlen = 1 + (size_t)xr_below(blen - 1);
                break;
            case 2:  /* corrupt the magic */
                mut[xr_below(4)] ^= (uint8_t)(1u << xr_below(8));
                break;
            case 3:  /* count field lies */
                {
                    uint64_t bad = (uint64_t)m.nents +
                                   (xr_below(2) ? 1 : (uint64_t)-1) +
                                   (xr_below(8) == 0 ? 1000 : 0);
                    put_le32(mut + 4, (uint32_t)bad);
                }
                break;
            case 4:  /* reorder: swap two entries */
                if (m.nents >= 2) {
                    size_t a = ent_i, b = (size_t)xr_below(m.nents);
                    uint8_t tmp[MRMP_ENT_WIRE];
                    if (a == b) b = (a + 1) % m.nents;
                    memcpy(tmp, mut + 8 + a * MRMP_ENT_WIRE, MRMP_ENT_WIRE);
                    memcpy(mut + 8 + a * MRMP_ENT_WIRE,
                           mut + 8 + b * MRMP_ENT_WIRE, MRMP_ENT_WIRE);
                    memcpy(mut + 8 + b * MRMP_ENT_WIRE, tmp, MRMP_ENT_WIRE);
                } else {
                    mut[xr_below(blen)] ^= (uint8_t)(1u << xr_below(8));
                }
                break;
            case 5:  /* overlap: grow one entry's len */
                put_le64(mut + 8 + ent_i * MRMP_ENT_WIRE + 8,
                         m.ents[ent_i].len + 1 + xr_below(97));
                break;
            case 6:  /* gap: move one entry's orig_off */
                put_le64(mut + 8 + ent_i * MRMP_ENT_WIRE,
                         m.ents[ent_i].orig_off + 1 + xr_below(97));
                break;
            case 7:  /* zero-length entry */
                put_le64(mut + 8 + ent_i * MRMP_ENT_WIRE + 8, 0);
                break;
            case 8:  /* bad kind */
                mut[8 + ent_i * MRMP_ENT_WIRE + 16] =
                    (uint8_t)(2 + xr_below(254));
                break;
            case 9:  /* RECIPE with idx != 0 / MEMBER with unknown idx */
                {
                    uint8_t *e = mut + 8 + ent_i * MRMP_ENT_WIRE;
                    if (m.ents[ent_i].kind == 0)
                        put_le32(e + 17, 1 + (uint32_t)xr_below(9));
                    else
                        put_le32(e + 17, 1000 + (uint32_t)xr_below(9000));
                }
                break;
            case 10: /* src_off past its source */
                {
                    uint8_t *e = mut + 8 + ent_i * MRMP_ENT_WIRE;
                    const mrmp_member *mm =
                        mrmp_find(m.mem, m.nmem, m.ents[ent_i].idx);
                    uint64_t bound = m.ents[ent_i].kind == 0
                        ? m.recipe_len : mm->usize;
                    put_le64(e + 21, bound + 1 + xr_below(1000));
                }
                break;
            case 11: /* last entry len short of / past the end */
                if (xr_below(2))
                    put_le64(mut + 8 + (m.nents - 1) * MRMP_ENT_WIRE + 8,
                             m.ents[m.nents - 1].len + 1 + xr_below(97));
                else
                    put_le64(mut + 8 + (m.nents - 1) * MRMP_ENT_WIRE + 8,
                             m.ents[m.nents - 1].len - 1);
                break;
            case 12: /* container size context mutated */
                csize = m.csize + 1 + xr_below(m.csize);
                break;
            default: /* shrink a used member's usize below the map's need */
                {
                    size_t k;
                    for (k = 0; k < m.nents; k++) {
                        if (m.ents[k].kind == 1) {
                            size_t q;
                            uint64_t need = m.ents[k].src_off +
                                            m.ents[k].len;
                            for (q = 0; q < mnmem; q++)
                                if (mmem[q].idx == m.ents[k].idx)
                                    mmem[q].usize = need - 1;
                            break;
                        }
                    }
                    if (k == m.nents)   /* no MEMBER entries: corrupt magic */
                        mut[xr_below(4)] ^= (uint8_t)(1u << xr_below(8));
                }
                break;
            }

            leg.cases++;
            if (mrmp_check(mut, mlen, csize, rlen, mmem, mnmem) == 0)
                crafted++;       /* rare crafted-valid mutation: allowed */
            else
                rejected++;
        }
    }
    /* sanity: crafted-valid must stay rare (single mutations against tiny
     * slack) -- 10% bound, the observed rate should be far below */
    leg.cases++;
    if (crafted * 10 > rejected + crafted)
        leg_fail(&leg, iters, "crafted-valid rate suspiciously high: "
                 "%" PRIu64 " of %" PRIu64, crafted, crafted + rejected);
    {
        char info[128];
        snprintf(info, sizeof info, "%" PRIu64 " mutated rejected, "
                 "%" PRIu64 " crafted-valid (allowed)", rejected, crafted);
        leg_done(&leg, t0, info);
    }
    return leg;
}

/* ================= main ================= */

int main(int argc, char **argv)
{
    uint64_t iters = 10000;
    uint64_t seed  = 0x1CF51EE5u;          /* fixed default */
    struct timespec t0, t1;
    double elapsed;
    leg_t legs[6];
    uint64_t total_cases = 0, total_fails = 0;
    int i;

    if (argc > 1) {
        iters = strtoull(argv[1], NULL, 0);
        if (!iters) iters = 10000;
    }
    if (argc > 2)
        seed = strtoull(argv[2], NULL, 0);
    if (argc > 3) {
        fprintf(stderr, "usage: %s [iterations] [seed]\n", argv[0]);
        return 2;
    }
    rng_state = seed ? seed : 0x9E3779B97F4A7C15ull;   /* xorshift != 0 */

    /* hermetic registry: no environment packs unless a leg sets its own */
    setenv("INVFS_CODECPACKS", "", 1);
    invfs_codec_probe_reset();

    printf("invf-fuzz: iterations=%" PRIu64 " seed=%" PRIu64 "%s\n",
           iters, rng_state, argc > 2 ? "" : " (default)");

    clock_gettime(CLOCK_MONOTONIC, &t0);
    legs[0] = leg_sniff(iters);
    legs[1] = leg_classifier(iters);
    legs[2] = leg_manifest(iters);
    legs[3] = leg_ppmd(iters);
    legs[4] = leg_bcj(iters);
    legs[5] = leg_mrmp(iters);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    elapsed = (double)(t1.tv_sec - t0.tv_sec) +
              (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    for (i = 0; i < 6; i++) {
        total_cases += legs[i].cases;
        total_fails += legs[i].fails;
    }
    printf("total: %" PRIu64 " cases, %" PRIu64 " failures, %.1fs\n",
           total_cases, total_fails, elapsed);
    printf("%s\n", total_fails ? "FAIL" : "PASS");
    return total_fails ? 1 : 0;
}
