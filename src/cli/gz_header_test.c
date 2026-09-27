/* gz_header_test.c — WP129: the GZR gzip header parse, under ASan.
 *
 * The bug this pins: vol_create_gz_file's header walk formed an index and
 * only validated it afterwards. FEXTRA could advance the cursor by up to
 * 65 537, and FNAME/FCOMMENT were `while (gz[hlen]) hlen++` with no bound
 * at all, over a heap buffer of exactly gz_len bytes. An 18-byte file was
 * enough:
 *
 *   1f 8b 08 6d 00 00 00 00 00 00 ff 00 00 00 00 00 00 00 00
 *
 * FLG=0x6d (FTEXT|FHCRC|FEXTRA|FNAME|FCOMMENT), XLEN=0x00FF, so the walk
 * formed hlen=267 and read at offset 267 of an 18-byte allocation.
 *
 * The test calls the REAL, PUBLIC vol_create_gz_file -- the function the
 * sweep calls at vol_sweep.c:1087 -- with buffers of exactly the file
 * length, which is what makes an over-read an ASan report rather than a
 * read into allocator padding. It is deliberately written against the
 * public API and not against the new internal helper, so the SAME source
 * compiles and runs on main: there it dies under ASan on the first case.
 * A test that only compiles after the fix cannot show the fix changed
 * anything.
 *
 * Both directions are asserted, and the second is the one that costs
 * money if it is wrong:
 *
 *   refuse  malformed headers must be refused, not read past. On main
 *           these ASan-abort; here they return 0.
 *   accept  well-formed gzip must still decompose, bit-exactly, for every
 *           defined FLG combination. A bounds check that quietly starts
 *           refusing valid gzip is worse than the overflow: it silently
 *           costs compression with no error anywhere -- and it would still
 *           pass any test that only asks "does it crash".
 *
 * The accept leg is not a header check. It builds a real tar, deflates it
 * at the parameters the engine's bit-exactness probe searches, and requires
 * the lane to come back non-zero. Non-zero means: header walked, stream
 * inflated, tar split, AND the re-deflate matched the original bytes. So
 * a header refusal and a probe miss are not confused for each other.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <zlib.h>
#include "volume_internal.h"

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; fprintf(stderr, "  FAIL  %s\n", what); }
    else       { printf("  OK    %s\n", what); }
}

/* ---- fixtures -------------------------------------------------------- */

/* A tar with ONE member of ~1 MiB of compressible-but-not-trivial text.
 *
 * The size is not arbitrary. vol_create_gz_file ends with a
 * "transcode only when smaller" guard (vol_cpack.c:1695): if the parts
 * plus the recipe are not smaller than the original .gz, it keeps the
 * original and returns 0. A 4 KiB fixture trips that guard -- 564 bytes of
 * parts against 136 bytes of gzip -- so the lane would return 0 for a
 * reason that has nothing to do with the header walk, and the accept leg
 * would be asserting the wrong thing. 1 MiB of lowercase text is a case
 * where the per-member codecs genuinely beat gzip level 6, which is what
 * makes a non-zero return mean "header walked, probe matched, and the
 * decomposition was worth committing".
 *
 * The lane needs the ustar magic at +257, at least one member, and a
 * correct header checksum (tarx_check_checksum, src/recipes/tarx.c:89). */
#define TAR_DATA 1048576u
static uint8_t *mk_tar(size_t *out_len)
{
    const size_t tlen = 512 + TAR_DATA + 1024;   /* header + data + 2 EOF blocks */
    uint8_t *t = calloc(1, tlen);
    uint64_t s = 0x243F6A8885A308D3ULL;
    size_t i;
    unsigned sum = 0;
    int k;
    if (!t) return NULL;
    /* ustar header field offsets: name 0, mode 100, uid 108, gid 116,
     * size 124, mtime 136, chksum 148, typeflag 156, magic 257 */
    snprintf((char *)t + 0,   100, "big.txt");
    snprintf((char *)t + 100, 8,  "0000644");
    snprintf((char *)t + 116, 8,  "0000000");
    snprintf((char *)t + 124, 12, "4000000");   /* 0o4000000 = 1048576 */
    snprintf((char *)t + 136, 12, "00000001234");
    t[156] = '0';
    memcpy(t + 257, "ustar", 5);
    /* Word-shaped text, not a uniform letter stream. The distribution
     * matters: a uniform 26-letter stream carries ~4.7 bits/char, gzip's
     * Huffman gets essentially all of it, and the "transcode only when
     * smaller" guard then (correctly) keeps the original -- so the accept
     * leg would be asserting nothing. Random-length words over the same
     * alphabet carry the redundancy real prose has, which is what the
     * per-member text codec is there to exploit. Verified against the
     * lane: this payload decomposes and commits. */
    {
        size_t o = 0;
        while (o < TAR_DATA) {
            size_t wlen = 20 + (size_t)(s % 71);
            size_t k;
            for (k = 0; k < wlen && o < TAR_DATA; k++) {
                s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                t[512 + o++] = (uint8_t)('a' + (s % 26));
            }
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            if (o < TAR_DATA) t[512 + o++] = '\n';
        }
    }
    memset(t + 148, ' ', 8);
    for (k = 0; k < 512; k++) sum += t[k];
    snprintf((char *)t + 148, 7, "%06o", sum);
    t[154] = 0;
    t[155] = ' ';
    *out_len = tlen;
    return t;
}

/* A gzip member over `tar` with the given FLG bits, deflated at level 6 /
 * memLevel 8 -- inside the engine probe's search space (levels 1-9 x
 * memLevel 7-9, Z_DEFAULT_STRATEGY), so the probe reproduces it exactly
 * and the lane accepts. Only the HEADER varies between calls, so every
 * variant decompresses to the same stream and reaches the same probe. */
static uint8_t *mk_member(const uint8_t *tar, size_t tlen, unsigned flg,
                          const uint8_t *xdata, size_t xlen,
                          const char *fname, const char *fcomment,
                          int hcrc, size_t truncate_to, size_t *out_len)
{
    z_stream d;
    uint8_t *out, *stream;
    size_t hlen = 10, slen, cap = tlen + tlen / 2 + 4096, i;

    if (cap < 65536) cap = 65536;
    stream = malloc(cap);
    if (!stream) return NULL;
    memset(&d, 0, sizeof d);
    if (deflateInit2(&d, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(stream); return NULL;
    }
    d.next_in = (Bytef *)tar; d.avail_in = (uInt)tlen;
    d.next_out = stream;       d.avail_out = (uInt)cap;
    if (deflate(&d, Z_FINISH) != Z_STREAM_END) { deflateEnd(&d); free(stream); return NULL; }
    slen = (size_t)d.total_out;
    deflateEnd(&d);

    out = calloc(1, cap + hlen + xlen + 64 +
                 (fname ? strlen(fname) + 1 : 0) + (fcomment ? strlen(fcomment) + 1 : 0));
    if (!out) { free(stream); return NULL; }
    out[0] = 0x1F; out[1] = 0x8B; out[2] = 8; out[3] = (uint8_t)flg;
    out[8] = 0x00; out[9] = 0x03;            /* XFL, OS=unknown */
    hlen = 10;
    if (flg & 0x04) {                        /* FEXTRA */
        out[hlen++] = (uint8_t)(xlen & 0xFF);
        out[hlen++] = (uint8_t)(xlen >> 8);
        for (i = 0; i < xlen; i++) out[hlen++] = xdata[i];
    }
    if (fname)    { size_t n = strlen(fname);    memcpy(out + hlen, fname, n);    hlen += n; out[hlen++] = 0; }
    if (fcomment) { size_t n = strlen(fcomment); memcpy(out + hlen, fcomment, n); hlen += n; out[hlen++] = 0; }
    if (hcrc) {                              /* FHCRC: low 16 bits of CRC32 */
        uint32_t c = crc32(0L, out, (uInt)hlen);
        out[hlen++] = (uint8_t)(c & 0xFF);
        out[hlen++] = (uint8_t)((c >> 8) & 0xFF);
    }
    memcpy(out + hlen, stream, slen);
    hlen += slen;
    {
        uint32_t c = crc32(0L, tar, (uInt)tlen);
        for (i = 0; i < 4; i++) out[hlen++] = (uint8_t)((c >> (8 * i)) & 0xFF);
        for (i = 0; i < 4; i++) out[hlen++] = (uint8_t)(((uint32_t)tlen >> (8 * i)) & 0xFF);
    }
    free(stream);
    *out_len = truncate_to ? truncate_to : hlen;
    return out;
}

/* The parser gets a buffer of EXACTLY len bytes, which is what vol_sweep
 * hands it. Without that, an over-read lands in allocator padding and the
 * bug is invisible. */
static uint64_t feed(invfs_volume *v, const uint8_t *data, size_t len, const char *name)
{
    uint8_t *gz = malloc(len ? len : 1);
    uint64_t ino;
    if (!gz) return 0;
    memcpy(gz, data, len);
    ino = vol_create_gz_file(v, name, gz, len);
    free(gz);
    return ino;
}

int main(void)
{
    /* The 18-byte reproducer, spelled out so the test carries its own
     * evidence and does not depend on tools/mk-gzhdr-seeds.py. */
    static const uint8_t poc18[18] = {
        0x1f, 0x8b, 0x08, 0x6d, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t poc_maxxlen[18] = {
        0x1f, 0x8b, 0x08, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t poc_reserved[16] = {
        0x1f, 0x8b, 0x08, 0xe0, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00
    };
    static const char *VARIANTS[] = {
        "FLG=0x00 plain",
        "FLG=0x01 FTEXT",
        "FLG=0x02 FHCRC",
        "FLG=0x04 FEXTRA(8)",
        "FLG=0x08 FNAME",
        "FLG=0x10 FCOMMENT",
        "FLG=0x0E FEXTRA+FNAME+FHCRC",
        "FLG=0x1F every defined bit"
    };
    static const unsigned FVAR[] = {
        0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x0E, 0x1F
    };
    char img[512], cmd[1400];
    const char *dir = getenv("GZHDR_DIR");
    uint8_t *tar, *m, payload[8192];
    size_t tlen = 0, n;
    int err = 0, i;
    invfs_volume *v = NULL;

    if (!dir) dir = "/tmp";
    printf("gz_header_test (WP129): the GZR gzip header walk is bounded\n");
    for (n = 0; n < sizeof payload; n++)
        payload[n] = (uint8_t)("the quick brown fox jumps over the lazy dog "[n % 43]);
    tar = mk_tar(&tlen);
    if (!tar) { fprintf(stderr, "gz_header_test: OOM\n"); return 2; }

    /* ---- 1. malformed headers: refuse, never read past -------------- */
    /* v is NULL here on purpose: every case below is refused by the header
     * walk, which returns before the first use of the volume. If one ever
     * were NOT, that is a finding, not a nuisance. */
    ok(feed(NULL, poc18, sizeof poc18, "poc18.gz") == 0,
       "18-byte PoC (FLG=0x6d XLEN=0x00FF): refused, not read past");
    memcpy(payload, "\x1f\x8b\x08\x08", 4);          /* FNAME, no NUL in tail */
    memset(payload + 4, 'A', 14);
    ok(feed(NULL, payload, 18, "nonul.gz") == 0,
       "FNAME with no NUL anywhere: refused (was an unbounded scan)");
    payload[3] = 0x10;                                /* FCOMMENT, same shape */
    ok(feed(NULL, payload, 18, "nonul2.gz") == 0,
       "FCOMMENT with no NUL anywhere: refused");
    ok(feed(NULL, poc_maxxlen, sizeof poc_maxxlen, "maxxlen.gz") == 0,
       "FEXTRA XLEN=0xFFFF with FNAME: refused (cursor jumps 65k)");
    ok(feed(NULL, poc_reserved, sizeof poc_reserved, "reserved.gz") == 0,
       "FLG reserved bits (0xE0): refused per RFC 1952 s2.1.1");
    /* A real member cut in the middle of its FNAME / FCOMMENT string. */
    m = mk_member(payload, 2048, 0x08, NULL, 0, "a-fairly-long-original-name.tar",
                  NULL, 0, 20, &n);
    ok(m && feed(NULL, m, n, "trunc.gz") == 0, "header truncated inside FNAME: refused");
    free(m);
    m = mk_member(payload, 2048, 0x10, NULL, 0, NULL, "a-fairly-long-comment-here",
                  0, 20, &n);
    ok(m && feed(NULL, m, n, "trunc2.gz") == 0, "header truncated inside FCOMMENT: refused");
    free(m);
    /* FEXTRA declaring more bytes than the whole file holds. */
    m = mk_member(payload, 2048, 0x04, (const uint8_t *)"\0\0\0\0\0\0", 6,
                  NULL, NULL, 0, 0, &n);
    ok(m && feed(NULL, m, 12, "shortxlen.gz") == 0, "FEXTRA longer than the file: refused");
    free(m);
    /* FEXTRA that fits, but leaves no room for the deflate stream + trailer. */
    m = mk_member(payload, 2048, 0x04, (const uint8_t *)"\0\0\0\0\0\0", 6,
                  NULL, NULL, 0, 0, &n);
    ok(m && feed(NULL, m, n - 2, "exact-xlen.gz") == 0,
       "FEXTRA exact-fit with the trailer cut: refused, no underflow");
    free(m);
    /* Just under the 18-byte floor. */
    ok(feed(NULL, poc18, 17, "short.gz") == 0, "17 bytes: below the floor, refused");
    ok(feed(NULL, poc18, 0, "empty.gz") == 0, "0 bytes: refused");

    /* ---- 2. well-formed members must STILL decompose --------------- */
    snprintf(img, sizeof img, "%s/invf-gzhdr-test.img", dir);
    unlink(img);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
             getenv("PWD") ? getenv("PWD") : ".", img);
    if (system(cmd) != 0 || !(v = vol_open(img, &err))) {
        printf("  SKIP  accept leg: cannot build a volume (err=%d)\n", err);
    } else {
        for (i = 0; i < 8; i++) {
            char label[96];
            unsigned flg = FVAR[i];
            m = mk_member(tar, tlen, flg,
                          (const uint8_t *)"\x01\x02\x03\x04\x05\x06\x07\x08", 8,
                          (flg & 0x08) ? "members/one.txt" : NULL,
                          (flg & 0x10) ? "made by something" : NULL,
                          (flg & 0x02) ? 1 : 0, 0, &n);
            snprintf(label, sizeof label,
                     "well-formed %s still decompresses bit-exactly", VARIANTS[i]);
            if (!m) { ok(0, label); continue; }
            /* Non-zero => header walked, stream inflated, tar split, and
             * the re-deflate matched the original bytes. The probe is in
             * the path, so this is the bit-exactness invariant asserted,
             * not just the bounds check. */
            ok(feed(v, m, n, "hello.tar.gz") != 0, label);
            free(m);
        }
        vol_close(v);
        unlink(img);
    }
    free(tar);

    printf("gz_header_test: %d checks, %d FAIL\n", checks, failures);
    return failures ? 1 : 0;
}
