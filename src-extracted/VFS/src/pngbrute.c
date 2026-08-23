/* pngbrute.c — probe: can vanilla zlib deflate reproduce the IDAT stream
 * of a real PNG? Usage: pngbrute <file.png>
 * Parses PNG chunks, inflates the IDAT stream (zlib, windowBits=-15),
 * re-deflates with levels 1..9 (zlib 1.3.1) and compares byte-for-byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc(n ? (size_t)n : 1);
    if (!d) { fclose(f); return NULL; }
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return d;
}

int main(int argc, char **argv)
{
    size_t n = 0;
    unsigned char *png = read_file(argv[1], &n);
    if (!png || n < 33 || memcmp(png, "\x89PNG\r\n\x1a\n", 8) != 0) {
        fprintf(stderr, "not a PNG\n"); return 1;
    }
    /* walk chunks: collect IDAT stream (may be multiple chunks) */
    size_t pos = 8, idat_len = 0, idat_cap = 1 << 20;
    unsigned char *idat = (unsigned char *)malloc(idat_cap);
    int width = 0, height = 0, bitdepth = 0, coltype = 0, interlace = 0;
    int nchunks = 0;
    while (pos + 12 <= n) {
        unsigned clen = (unsigned)png[pos] << 24 | (unsigned)png[pos + 1] << 16 |
                        (unsigned)png[pos + 2] << 8 | (unsigned)png[pos + 3];
        const char *type = (const char *)(png + pos + 4);
        if (memcmp(type, "IHDR", 4) == 0 && clen >= 13) {
            width  = (int)png[pos + 8] << 24 | (int)png[pos + 9] << 16 |
                     (int)png[pos + 10] << 8 | (int)png[pos + 11];
            height = (int)png[pos + 12] << 24 | (int)png[pos + 13] << 16 |
                     (int)png[pos + 14] << 8 | (int)png[pos + 15];
            bitdepth = png[pos + 16];
            coltype  = png[pos + 17];
            interlace = png[pos + 19];
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_len + clen > idat_cap) {
                idat_cap = idat_len + clen + (1 << 20);
                idat = (unsigned char *)realloc(idat, idat_cap);
            }
            memcpy(idat + idat_len, png + pos + 8, clen);
            idat_len += clen;
        }
        pos += 12 + clen;
        nchunks++;
        if (memcmp(type, "IEND", 4) == 0) break;
    }
    printf("PNG %dx%d depth=%d coltype=%d interlace=%d, %d chunks, IDAT %zu bytes\n",
           width, height, bitdepth, coltype, interlace, nchunks, idat_len);

    /* inflate raw deflate (zlib) */
    z_stream in;
    memset(&in, 0, sizeof in);
    inflateInit2(&in, 15);
    size_t cap = (size_t)width * (size_t)(height + 1) * 8 + 1024;
    unsigned char *raw = (unsigned char *)malloc(cap);
    in.next_in = idat;
    in.avail_in = (uInt)idat_len;
    in.next_out = raw;
    in.avail_out = (uInt)(cap > 0x7FFFFFFF ? 0x7FFFFFFF : cap);
    int r = inflate(&in, Z_FINISH);
    inflateEnd(&in);
    if (r != Z_STREAM_END) { printf("inflate failed (rc=%d)\n", r); return 1; }
    size_t raw_len = (size_t)in.total_out;
    printf("inflated: %zu bytes (row bytes %d, raw est %zu)\n",
           raw_len, width * 4 + 1, (size_t)(width * 4 + 1) * height);

    /* try re-deflate levels 1..9 x memLevel 7..9 x strategies */
    int found = 0;
    for (int level = 1; level <= 9; level++) {
        for (int mem = 7; mem <= 9; mem++) {
        for (int strat = 0; strat <= 3; strat++) {
        z_stream s;
        memset(&s, 0, sizeof s);
        if (deflateInit2(&s, level, Z_DEFLATED, 15, mem, strat) != Z_OK)
            continue;
        size_t bound = deflateBound(&s, raw_len);
        unsigned char *re = (unsigned char *)malloc(bound);
        s.next_in = raw; s.avail_in = (uInt)raw_len;
        s.next_out = re; s.avail_out = (uInt)bound;
        int r2 = deflate(&s, Z_FINISH);
        size_t re_len = (size_t)s.total_out;
        deflateEnd(&s);
        if (r2 == Z_STREAM_END && re_len == idat_len && memcmp(re, idat, idat_len) == 0) {
            printf("MATCH: zlib level=%d memLevel=%d strat=%d (zlib %s)\n",
                   level, mem, strat, zlibVersion());
            found = 1;
        }
        if (level == 6 && mem == 8 && strat == 0 && r2 == Z_STREAM_END) {
            printf("[diag] level6/mem8/strat0: re_len=%zu vs idat %zu\n", re_len, idat_len);
            if (re_len == idat_len) {
                size_t d = 0;
                while (d < idat_len && re[d] == idat[d]) d++;
                printf("[diag] first diff at %zu of %zu\n", d, idat_len);
            }
        }
        free(re);
        }}
    }
    if (!found)
        printf("no match: IDAT not vanilla zlib (libdeflate/pngcrush/zopfli?)\n");
    free(raw); free(idat); free(png);
    return found ? 0 : 1;
}
