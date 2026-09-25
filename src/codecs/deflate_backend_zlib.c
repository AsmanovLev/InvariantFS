#include "deflate_backend.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifndef INVFS_BACKEND_ENGINE
#define INVFS_BACKEND_ENGINE INVFS_DEFLATE_ENGINE_ZLIB_SYSTEM
#endif
#ifndef INVFS_BACKEND_SYM
#define INVFS_BACKEND_SYM invfs_deflate_backend_system
#endif

static int b_try(const uint8_t *raw, size_t raw_len,
                 const uint8_t *target, size_t target_len,
                 uint8_t *scratch, size_t scratch_cap,
                 int level, int mem, int strategy, int window_bits,
                 invfs_deflate_params *out)
{
    z_stream s;
    int r;
    size_t out_len;

    memset(&s, 0, sizeof s);
    if (deflateInit2(&s, level, Z_DEFLATED, window_bits, mem, strategy) != Z_OK)
        return 0;
    s.next_in = (Bytef *)raw;
    s.avail_in = (uInt)(raw_len > 0x7FFFFFFF ? 0x7FFFFFFF : raw_len);
    s.next_out = scratch;
    s.avail_out = (uInt)scratch_cap;
    r = deflate(&s, Z_FINISH);
    out_len = (size_t)s.total_out;
    deflateEnd(&s);
    if (r == Z_STREAM_END && out_len == target_len &&
        memcmp(scratch, target, target_len) == 0) {
        out->engine = INVFS_BACKEND_ENGINE;
        out->level = (int8_t)level;
        out->mem_level = (int8_t)mem;
        out->strategy = (int8_t)strategy;
        out->window_bits = (int8_t)window_bits;
        return 1;
    }
    return 0;
}

static int b_find(const uint8_t *raw, size_t raw_len,
                  const uint8_t *target, size_t target_len,
                  int window_bits_hint, invfs_deflate_params *out)
{
    static const int levels[] = {6, 9, 1, 7, 5, 8, 4, 3, 2};
    static const int alt_mems[] = {9, 7};
    static const int strategies[] = {
        Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED
    };
    int window_bits = window_bits_hint ? window_bits_hint : -15;
    z_stream probe;
    uint8_t *scratch;
    size_t scratch_cap;
    size_t i;

    if (!raw || !target || !out)
        return -1;
    memset(&probe, 0, sizeof probe);
    if (deflateInit2(&probe, 6, Z_DEFLATED, window_bits, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return -1;
    scratch_cap = deflateBound(&probe, (uLong)raw_len);
    deflateEnd(&probe);
    scratch = malloc(scratch_cap ? scratch_cap : 1);
    if (!scratch)
        return -1;

    for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (b_try(raw, raw_len, target, target_len, scratch, scratch_cap,
                  levels[i], 8, Z_DEFAULT_STRATEGY, window_bits, out))
            goto found;
    }
    for (i = 0; i < sizeof(alt_mems) / sizeof(alt_mems[0]); i++) {
        size_t j;
        for (j = 0; j < sizeof(levels) / sizeof(levels[0]); j++) {
            if (b_try(raw, raw_len, target, target_len, scratch, scratch_cap,
                      levels[j], alt_mems[i], Z_DEFAULT_STRATEGY,
                      window_bits, out))
                goto found;
        }
    }
    if (window_bits == -12)
        goto miss;

    for (int mem = 6; mem >= 1; mem--) {
        for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
            if (b_try(raw, raw_len, target, target_len, scratch, scratch_cap,
                      levels[i], mem, Z_DEFAULT_STRATEGY, window_bits, out))
                goto found;
        }
    }
    for (i = 0; i < sizeof(strategies) / sizeof(strategies[0]); i++) {
        int level;
        for (level = 1; level <= 9; level++) {
            int mem;
            for (mem = 7; mem <= 9; mem++) {
                if (b_try(raw, raw_len, target, target_len, scratch, scratch_cap,
                          level, mem, strategies[i], window_bits, out))
                    goto found;
            }
        }
    }

miss:
    free(scratch);
    return -1;
found:
    free(scratch);
    return 0;
}

static int b_encode(const uint8_t *raw, size_t raw_len,
                    const invfs_deflate_params *params,
                    uint8_t **out_stream, size_t *out_len)
{
    z_stream s;
    uint8_t *stream;
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
    stream = malloc(bound ? bound : 1);
    if (!stream) {
        deflateEnd(&s);
        return -1;
    }
    s.next_in = (Bytef *)raw;
    s.avail_in = (uInt)(raw_len > 0x7FFFFFFF ? 0x7FFFFFFF : raw_len);
    s.next_out = stream;
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

static int b_decompress(const uint8_t *stream, size_t stream_len,
                        int window_bits, uint8_t **out_raw, size_t *out_len)
{
    z_stream s;
    uint8_t *buf;
    size_t cap;
    int rc;

    if (!stream || !out_raw || !out_len)
        return -1;
    *out_raw = NULL;
    *out_len = 0;
    cap = stream_len < 32768 ? 65536 : stream_len * 3;
    buf = malloc(cap);
    if (!buf)
        return -1;
    memset(&s, 0, sizeof s);
    if (inflateInit2(&s, window_bits) != Z_OK) {
        free(buf);
        return -1;
    }
    s.next_in = (Bytef *)stream;
    s.avail_in = (uInt)(stream_len > 0x7FFFFFFF ? 0x7FFFFFFF : stream_len);
    for (;;) {
        s.next_out = (Bytef *)(buf + s.total_out);
        s.avail_out = (uInt)(cap - s.total_out);
        rc = inflate(&s, Z_NO_FLUSH);
        if (rc == Z_STREAM_END)
            break;
        if (rc != Z_OK) {
            inflateEnd(&s);
            free(buf);
            return -1;
        }
        if (s.avail_out == 0) {
            size_t next = cap * 2;
            uint8_t *grown = realloc(buf, next);
            if (!grown) {
                inflateEnd(&s);
                free(buf);
                return -1;
            }
            buf = grown;
            cap = next;
        }
    }
    *out_len = (size_t)s.total_out;
    inflateEnd(&s);
    *out_raw = buf;
    return 0;
}

const invfs_deflate_backend INVFS_BACKEND_SYM = {
    INVFS_BACKEND_ENGINE,
    b_find,
    b_encode,
    b_decompress
};
