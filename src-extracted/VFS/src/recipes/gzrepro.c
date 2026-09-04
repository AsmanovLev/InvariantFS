/* gzrepro.c — can vanilla zlib deflate BIT-EXACTLY reproduce the deflate
 * stream of a real gzip file?
 *
 * Usage: gzrepro <file.gz>
 * Splits gzip: [header][deflate stream][crc32][isize]. Inflates the raw
 * stream (zlib, windowBits=-15), then re-deflates with every
 * (level 1..9) x (memLevel 7..9) and compares with the original stream.
 * Prints the matching parameters — these go into the InvariantFS recipe.
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
    if (argc >= 3 && strcmp(argv[1], "--make-gz") == 0) {
        /* recompress a file as gzip with our zlib; output to argv[3] or
           file.gz. level = argv[4] or 6 */
        const char *src_path = argv[2];
        const char *dst_path = argc >= 4 ? argv[3] : NULL;
        int level = argc >= 5 ? atoi(argv[4]) : 6;
        char tmpbuf[512];
        if (!dst_path) {
            snprintf(tmpbuf, sizeof tmpbuf, "%s.gz", src_path);
            dst_path = tmpbuf;
        }
        size_t n = 0;
        unsigned char *src = read_file(src_path, &n);
        if (!src) { fprintf(stderr, "cannot open %s\n", src_path); return 1; }
        /* raw deflate */
        z_stream s;
        memset(&s, 0, sizeof s);
        if (deflateInit2(&s, level, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY) != Z_OK) return 1;
        size_t bound = deflateBound(&s, n);
        unsigned char *re = (unsigned char *)malloc(bound);
        s.next_in = src; s.avail_in = (uInt)n;
        s.next_out = re; s.avail_out = (uInt)bound;
        if (deflate(&s, Z_FINISH) != Z_STREAM_END) return 1;
        size_t re_len = (size_t)s.total_out;
        deflateEnd(&s);
        FILE *f = fopen(dst_path, "wb");
        if (!f) return 1;
        unsigned char hdr[10] = { 0x1F, 0x8B, 0x08, 0x00, 0,0,0,0, 0x00, 0xFF };
        fwrite(hdr, 1, 10, f);
        fwrite(re, 1, re_len, f);
        uLong c = crc32(0L, Z_NULL, 0);
        c = crc32(c, src, (uInt)n);
        unsigned char tail[8];
        tail[0] = (unsigned char)(c & 0xFF); tail[1] = (unsigned char)((c >> 8) & 0xFF);
        tail[2] = (unsigned char)((c >> 16) & 0xFF); tail[3] = (unsigned char)((c >> 24) & 0xFF);
        tail[4] = (unsigned char)(n & 0xFF); tail[5] = (unsigned char)((n >> 8) & 0xFF);
        tail[6] = (unsigned char)((n >> 16) & 0xFF); tail[7] = (unsigned char)((n >> 24) & 0xFF);
        fwrite(tail, 1, 8, f);
        fclose(f);
        printf("made %s (level %d, %zu bytes)\n", dst_path, level, re_len + 18);
        free(re); free(src);
        return 0;
    }
    size_t gz_len = 0;
    unsigned char *gz = read_file(argv[1], &gz_len);
    if (!gz) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    if (gz_len < 18 || gz[0] != 0x1F || gz[1] != 0x8B) {
        printf("not gzip\n"); return 1;
    }
    unsigned flg = gz[3];
    size_t hlen = 10;
    if (flg & 0x04) { unsigned xlen = gz[hlen] | (gz[hlen + 1] << 8); hlen += 2 + xlen; }
    if (flg & 0x08) { while (gz[hlen]) hlen++; hlen++; }
    if (flg & 0x10) { while (gz[hlen]) hlen++; hlen++; }
    if (flg & 0x02) hlen += 2;
    size_t stream_len = gz_len - hlen - 8;
    unsigned isize = (unsigned)gz[gz_len - 4] | ((unsigned)gz[gz_len - 3] << 8) |
                     ((unsigned)gz[gz_len - 2] << 16) | ((unsigned)gz[gz_len - 1] << 24);
    printf("gzip header: %zu bytes, deflate stream: %zu, isize: %u\n",
           hlen, stream_len, isize);

    /* inflate raw deflate (zlib) */
    z_stream in;
    memset(&in, 0, sizeof in);
    inflateInit2(&in, -15);
    size_t cap = (size_t)isize + (size_t)isize / 2 + 64; /* worst: stored blocks */
    if (cap < 1024) cap = 1024;
    unsigned char *out = (unsigned char *)malloc(cap);
    in.next_in = gz + hlen;
    in.avail_in = (uInt)stream_len;
    in.next_out = out;
    in.avail_out = (uInt)(cap > 0x7FFFFFFF ? 0x7FFFFFFF : cap);
    int r = inflate(&in, Z_FINISH);
    inflateEnd(&in);
    if (r != Z_STREAM_END) { printf("inflate failed (rc=%d)\n", r); return 1; }
    size_t out_len = (size_t)in.total_out;
    printf("inflated: %zu bytes\n", out_len);

    /* try zlib deflate with every (level, memLevel) */
    int found = 0;
    for (int level = 1; level <= 9; level++) {
        for (int mem = 7; mem <= 9; mem++) {
            z_stream s;
            memset(&s, 0, sizeof s);
            if (deflateInit2(&s, level, Z_DEFLATED, -15, mem,
                             Z_DEFAULT_STRATEGY) != Z_OK)
                continue;
            size_t bound = deflateBound(&s, out_len);
            unsigned char *re = (unsigned char *)malloc(bound);
            s.next_in = out;
            s.avail_in = (uInt)out_len;
            s.next_out = re;
            s.avail_out = (uInt)bound;
            int r2 = deflate(&s, Z_FINISH);
            size_t re_len = (size_t)s.total_out;
            deflateEnd(&s);
            if (r2 != Z_STREAM_END) { free(re); continue; }
            if (re_len == stream_len && memcmp(re, gz + hlen, stream_len) == 0) {
                printf("MATCH: level=%d memLevel=%d (zlib %s)\n",
                       level, mem, zlibVersion());
                found = 1;
            }
            free(re);
        }
    }
    if (!found) {
        printf("no match: not vanilla zlib deflate\n");
        printf("(7zip / java / miniz / pigz / another zlib version / padded stream)\n");
    }
    free(out);
    free(gz);
    return found ? 0 : 1;
}
