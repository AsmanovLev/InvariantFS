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
 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L   /* fseeko */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

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

int main(int argc, char **argv)
{
    uint64_t p = 2;   /* past SOI (FF D8) */
    unsigned long long est = 0;

    if (argc != 3 || strcmp(argv[1], "estimate") != 0) {
        fprintf(stderr, "usage: jxlest estimate <in.jpg>\n");
        return 2;
    }
    f = fopen(argv[2], "rb");
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
