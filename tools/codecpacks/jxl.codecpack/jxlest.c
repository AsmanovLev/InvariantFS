/*
 * jxlest.c — JPEG decode-working-set estimator for the jxl codecpack
 * (WP16e). `jxlest estimate <in>` prints the SOF-derived estimate w*h*3 —
 * the pixel buffer djxl materializes on decode-back — as a bare byte
 * count on stdout, or 0 when the geometry is unknown (the FS admits the
 * file and lets cjxl try; the same convention the retired builtin
 * jpeg_raw_estimate had).
 *
 * Exit 0 with the bare count (0 = unknown); nonzero only when the input
 * is unreadable — the WP13 estimate-failure convention stamps
 * GENERIC_GUARD. The marker walk streams through a bounded window: SOF
 * always precedes SOS, but APPn segments (EXIF, embedded thumbnails) can
 * be large, so the file is never held whole.
 *
 * ADR-007 (WP615): this file doubles as a .so plugin (libjxlest.so, built
 * with -fPIC -shared -DIVPACK_SHARED_LIB). The plugin glue below is compiled
 * in BOTH builds -- the CLI build simply never calls it, and main() is what
 * -DIVPACK_SHARED_LIB drops -- so the .so and the CLI can never drift apart.
 * The headers are declarations + macros only: no new dependencies, no -ldl,
 * and the pack still builds with plain `cc -std=c11 -Wall -Wextra -Werror`.
 * jxl is an estimate-only codecpack (no enumerate/extract/strip/rebuild/map),
 * so the container verbs are decline stubs over the shared validation macro
 * while estimate shares the very cmd_estimate() the CLI calls.
 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L   /* fseeko */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if __has_include("ivpack_api.h")
#include "ivpack_api.h"
#elif __has_include("../../../src/include/ivpack_api.h")
#include "../../../src/include/ivpack_api.h"
#endif
#if __has_include("ivpack_impl.h")
#include "ivpack_impl.h"
#elif __has_include("../../../src/include/ivpack_impl.h")
#include "../../../src/include/ivpack_impl.h"
#endif

static FILE *f;
static uint8_t win[192 * 1024];   /* > max segment: 2 + 0xFFFF */
static uint64_t wbase;            /* file offset of win[0] */
static size_t wlen;               /* valid bytes in win */

/* 1 when [p, p+n) sits in the window (repositioning as needed). p only
 * ever moves forward; a segment that runs past EOF reads as absent. */
static int need(uint64_t p, size_t n)
{
    if (p >= wbase && n <= wlen && p - wbase <= wlen - n)
        return 1;
    if (fseeko(f, (off_t)p, SEEK_SET) != 0)
        return 0;
    wbase = p;
    wlen = fread(win, 1, sizeof win, f);
    return n <= wlen;
}

static uint8_t at(uint64_t p) { return win[(size_t)(p - wbase)]; }

/* The SOF marker-walk estimator both the CLI and the plugin estimate export
 * call. Prints w*h*3 (or 0 when the geometry is unknown) and returns the
 * exit status: 0 normally, 1 when the input is unreadable. */
static int cmd_estimate(const char *path)
{
    uint64_t p = 2;   /* past SOI (FF D8) */
    unsigned long long est = 0;

    /* A fork-guarded plugin child inherits whatever window state the worker
     * happens to hold; the walk always restarts at SOI, so reset it. A CLI
     * process calls this once, where the reset is a no-op. */
    wbase = 0;
    wlen = 0;
    f = fopen(path, "rb");
    if (!f) return 1;

    /* marker walk for SOF0/SOF1/SOF2 (FFC0..FFC2: baseline/extended/
     * progressive) — jpeg_raw_estimate's rules, byte for byte */
    if (!need(0, 2) || at(0) != 0xFF || at(1) != 0xD8)
        goto out;                     /* not a JPEG: geometry unknown */
    while (need(p, 4)) {
        uint8_t m;
        unsigned seglen;
        if (at(p) != 0xFF) { p++; continue; }    /* tolerate garbage */
        m = at(p + 1);
        if (m == 0xFF) { p++; continue; }        /* fill byte */
        if (m == 0x00) { p += 2; continue; }     /* stuffed 0xFF */
        if (m == 0xD9) break;                    /* EOI */
        if (m == 0xDA) break;                    /* SOS: entropy data */
        if (m == 0xD8 || m == 0x01 ||
            (m >= 0xD0 && m <= 0xD7)) {          /* SOI/TEM/RSTn */
            p += 2;
            continue;
        }
        seglen = ((unsigned)at(p + 2) << 8) | at(p + 3);
        if (seglen < 2 || !need(p + 2, seglen)) break;
        if (m >= 0xC0 && m <= 0xC2) {
            unsigned h, w;
            if (!need(p, 9)) break;
            h = ((unsigned)at(p + 5) << 8) | at(p + 6);
            w = ((unsigned)at(p + 7) << 8) | at(p + 8);
            if (w && h)
                est = (unsigned long long)w * h * 3;
            break;
        }
        p += 2 + seglen;
    }
out:
    fclose(f);
    printf("%llu\n", est);
    return 0;
}

/* ---- ivpack plugin C ABI export (ADR-007, WP615) ---------------------------
 * Built as libjxlest.so with -DIVPACK_SHARED_LIB -fPIC -shared; the fork
 * guard in ivpack_impl.h keeps the CLI's exit(3)=decline / exit(1)=error
 * contract intact inside a long-lived worker. See ivpack_impl.h.
 *
 * jxl is a codecpack, not a containerpack: the five container verbs have no
 * CLI counterpart (the CLI answers usage-2 for non-estimate argv), so each
 * one-line stub below declines with 3 ("not mine") while the shared
 * IVPACK_DEFINE_CONTAINER_CMD macro keeps the operand validation -- NULL
 * args, unknown verbs, missing paths answer NEGATIVE, the CLI-fallback cue
 * -- identical to every other pack. Estimate shares cmd_estimate() with the
 * CLI, so the number the .so reports is the number the CLI would print,
 * including the codecpack convention the containerpacks do not share: an
 * unknown geometry prints 0 and returns 0 (admit; the decode-back guard is
 * the arbiter), it does NOT decline with 3. */
static int cmd_enumerate(const char *in, const char *out)
{ (void)in; (void)out; return 3; }
static int cmd_extract(const char *in, const char *idx, const char *out)
{ (void)in; (void)idx; (void)out; return 3; }
static int cmd_strip(const char *in, const char *out)
{ (void)in; (void)out; return 3; }
static int cmd_rebuild(const char *recipe, const char *dir, const char *out)
{ (void)recipe; (void)dir; (void)out; return 3; }
static int cmd_map(const char *in, const char *out)
{ (void)in; (void)out; return 3; }

/* pack_class is "codecpack": the jxl manifest carries no `type =` key, so
 * unlike the containerpacks there is no manifest field to agree with -- the
 * literal is pinned by src/cli/ivpack_packs_test.c instead. `name` matches
 * the manifest `name = jxl`. */
static const ivpack_desc s_jxlest_desc = {
    IVPACK_API_VERSION, "jxl", "1.0.0", "codecpack", 0
};

const ivpack_desc *ivpack_get_desc(void) { return &s_jxlest_desc; }

IVPACK_CANON_CALLS(jxlest)

IVPACK_DEFINE_CONTAINER_CMD(jxlest, IVPACK_GLUE_NONE())

IVPACK_DEFINE_CONTAINER_ESTIMATE(jxlest, IVPACK_GLUE_NONE(),
                             rc = (int)cmd_estimate(a);)

#ifndef IVPACK_SHARED_LIB
int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "estimate") != 0) {
        fprintf(stderr, "usage: jxlest estimate <in.jpg>\n");
        return 2;
    }
    return cmd_estimate(argv[2]);
}
#endif /* !IVPACK_SHARED_LIB */
