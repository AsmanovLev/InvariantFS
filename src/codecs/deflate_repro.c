/* deflate_repro.c — universal deflate reproduction helper for InvariantFS.
 *
 * Implements deterministic parameter search, stream reproduction, and
 * decompression for bit-exact container handling (GZIP, ZIP, QCOW2, PNG).
 */
#include "deflate_repro.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

int invfs_deflate_decompress(const uint8_t *stream, size_t stream_len,
                             int window_bits,
                             uint8_t **out_raw, size_t *out_len)
{
    z_stream strm;
    uint8_t *buf = NULL;
    size_t cap = stream_len < 32768 ? 65536 : stream_len * 3;
    int rc;

    if (!stream || !out_raw || !out_len)
        return -1;
    *out_raw = NULL;
    *out_len = 0;

    buf = (uint8_t *)malloc(cap);
    if (!buf) return -1;

    memset(&strm, 0, sizeof strm);
    rc = inflateInit2(&strm, window_bits);
    if (rc != Z_OK) {
        free(buf);
        return -1;
    }

    strm.next_in = (Bytef *)stream;
    strm.avail_in = (uInt)(stream_len > 0x7FFFFFFF ? 0x7FFFFFFF : stream_len);

    for (;;) {
        strm.next_out = (Bytef *)(buf + strm.total_out);
        strm.avail_out = (uInt)(cap - strm.total_out);

        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc == Z_STREAM_END)
            break;
        if (rc != Z_OK) {
            inflateEnd(&strm);
            free(buf);
            return -1;
        }

        if (strm.avail_out == 0) {
            size_t ncap = cap * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, ncap);
            if (!nb) {
                inflateEnd(&strm);
                free(buf);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
    }

    *out_len = (size_t)strm.total_out;
    inflateEnd(&strm);
    *out_raw = buf;
    return 0;
}

int invfs_deflate_repro_encode(const uint8_t *raw, size_t raw_len,
                              const invfs_deflate_params *params,
                              uint8_t **out_stream, size_t *out_len)
{
    z_stream s;
    uint8_t *stream = NULL;
    size_t bound;
    int r;

    if (!raw || !params || !out_stream || !out_len)
        return -1;
    *out_stream = NULL;
    *out_len = 0;

    memset(&s, 0, sizeof s);
    if (deflateInit2(&s, params->level, Z_DEFLATED, params->window_bits,
                     params->mem_level, params->strategy) != Z_OK)
        return -1;

    bound = deflateBound(&s, (uLong)raw_len);
    stream = (uint8_t *)malloc(bound ? bound : 1);
    if (!stream) {
        deflateEnd(&s);
        return -1;
    }

    s.next_in = (Bytef *)raw;
    s.avail_in = (uInt)(raw_len > 0x7FFFFFFF ? 0x7FFFFFFF : raw_len);
    s.next_out = (Bytef *)stream;
    s.avail_out = (uInt)bound;

    r = deflate(&s, Z_FINISH);
    if (r != Z_STREAM_END) {
        deflateEnd(&s);
        free(stream);
        return -1;
    }

    *out_len = (size_t)s.total_out;
    deflateEnd(&s);
    *out_stream = stream;
    return 0;
}

/* Fast candidate trial helper */
static inline int try_candidate(const uint8_t *raw, size_t raw_len,
                                const uint8_t *target_stream, size_t target_len,
                                uint8_t *scratch_buf, size_t scratch_cap,
                                int level, int mem, int strategy, int window_bits,
                                invfs_deflate_params *out_params)
{
    z_stream s;
    memset(&s, 0, sizeof s);
    if (deflateInit2(&s, level, Z_DEFLATED, window_bits, mem, strategy) != Z_OK)
        return 0;

    s.next_in = (Bytef *)raw;
    s.avail_in = (uInt)(raw_len > 0x7FFFFFFF ? 0x7FFFFFFF : raw_len);
    s.next_out = (Bytef *)scratch_buf;
    s.avail_out = (uInt)scratch_cap;

    int r = deflate(&s, Z_FINISH);
    size_t out_len = (size_t)s.total_out;
    deflateEnd(&s);

    if (r == Z_STREAM_END && out_len == target_len &&
        memcmp(scratch_buf, target_stream, target_len) == 0) {
        out_params->engine = INVFS_DEFLATE_ENGINE_ZLIB;
        out_params->level = (int8_t)level;
        out_params->mem_level = (int8_t)mem;
        out_params->strategy = (int8_t)strategy;
        out_params->window_bits = (int8_t)window_bits;
        return 1;
    }
    return 0;
}

int invfs_deflate_repro_find(const uint8_t *raw, size_t raw_len,
                             const uint8_t *target_stream, size_t target_len,
                             int window_bits_hint,
                             invfs_deflate_params *out_params)
{
    int wbits = window_bits_hint ? window_bits_hint : -15;
    z_stream dummy;
    size_t scratch_cap;
    uint8_t *scratch = NULL;
    int found = 0;

    if (!raw || !target_stream || !out_params)
        return -1;

    memset(&dummy, 0, sizeof dummy);
    if (deflateInit2(&dummy, 6, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return -1;
    scratch_cap = deflateBound(&dummy, (uLong)raw_len);
    deflateEnd(&dummy);

    scratch = (uint8_t *)malloc(scratch_cap ? scratch_cap : 1);
    if (!scratch) return -1;

    /* 1. Fast path: check most common defaults (level 6, 9, 1 with mem=8, strategy=0) */
    static const int common_levels[] = { 6, 9, 1, 7, 5, 8, 4, 3, 2 };
    for (size_t i = 0; i < sizeof(common_levels)/sizeof(common_levels[0]); i++) {
        if (try_candidate(raw, raw_len, target_stream, target_len, scratch, scratch_cap,
                          common_levels[i], 8, Z_DEFAULT_STRATEGY, wbits, out_params)) {
            found = 1;
            goto done;
        }
    }

    /* 2. Check levels 1..9 with memLevel 9 and 7 */
    static const int alt_mems[] = { 9, 7 };
    for (size_t m = 0; m < sizeof(alt_mems)/sizeof(alt_mems[0]); m++) {
        for (int lv = 1; lv <= 9; lv++) {
            if (try_candidate(raw, raw_len, target_stream, target_len, scratch, scratch_cap,
                              lv, alt_mems[m], Z_DEFAULT_STRATEGY, wbits, out_params)) {
                found = 1;
                goto done;
            }
        }
    }

    /* 3. Check remaining memLevels 1..6 with default strategy */
    for (int mm = 6; mm >= 1; mm--) {
        for (int lv = 1; lv <= 9; lv++) {
            if (try_candidate(raw, raw_len, target_stream, target_len, scratch, scratch_cap,
                              lv, mm, Z_DEFAULT_STRATEGY, wbits, out_params)) {
                found = 1;
                goto done;
            }
        }
    }

    /* 4. Check alternative strategies (Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED) */
    static const int alt_strats[] = { Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED };
    for (size_t s = 0; s < sizeof(alt_strats)/sizeof(alt_strats[0]); s++) {
        for (int lv = 1; lv <= 9; lv++) {
            for (int mm = 7; mm <= 9; mm++) {
                if (try_candidate(raw, raw_len, target_stream, target_len, scratch, scratch_cap,
                                  lv, mm, alt_strats[s], wbits, out_params)) {
                    found = 1;
                    goto done;
                }
            }
        }
    }

done:
    free(scratch);
    return found ? 0 : -1;
}
