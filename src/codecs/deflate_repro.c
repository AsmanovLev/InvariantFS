#include "deflate_backend.h"

#include <stdlib.h>
#include <string.h>

static _Thread_local invfs_deflate_params tls_cached;
static _Thread_local int tls_cached_valid;

const invfs_deflate_backend *invfs_deflate_backend_get(uint8_t engine)
{
    if (engine == INVFS_DEFLATE_ENGINE_ZLIB_STOCK)
        return &invfs_deflate_backend_stock;
    return &invfs_deflate_backend_system;
}

const char *invfs_deflate_engine_name(uint8_t engine)
{
    if (engine == INVFS_DEFLATE_ENGINE_ZLIB_STOCK)
        return "zlib-stock";
    if (engine == INVFS_DEFLATE_ENGINE_ZLIB_SYSTEM)
        return "zlib-system";
    if (engine == INVFS_DEFLATE_ENGINE_MINIZ)
        return "miniz";
    return "unknown";
}

static int try_cached(const uint8_t *raw, size_t raw_len,
                      const uint8_t *target, size_t target_len,
                      int window_bits, invfs_deflate_params *out)
{
    const invfs_deflate_backend *backend;
    uint8_t *encoded = NULL;
    size_t encoded_len = 0;

    if (!tls_cached_valid || tls_cached.window_bits != window_bits)
        return 0;
    backend = invfs_deflate_backend_get(tls_cached.engine);
    if (backend->encode(raw, raw_len, &tls_cached, &encoded, &encoded_len) != 0)
        return 0;
    if (encoded_len == target_len && memcmp(encoded, target, target_len) == 0) {
        *out = tls_cached;
        free(encoded);
        return 1;
    }
    free(encoded);
    return 0;
}

int invfs_deflate_repro_find(const uint8_t *raw, size_t raw_len,
                             const uint8_t *target_stream, size_t target_len,
                             int window_bits_hint,
                             invfs_deflate_params *out_params)
{
    static const invfs_deflate_backend *const order[] = {
        &invfs_deflate_backend_stock,
        &invfs_deflate_backend_system
    };
    int window_bits = window_bits_hint ? window_bits_hint : -15;
    size_t i;

    if (!raw || !target_stream || !out_params)
        return -1;
    if (try_cached(raw, raw_len, target_stream, target_len,
                   window_bits, out_params))
        return 0;
    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (order[i]->find(raw, raw_len, target_stream, target_len,
                           window_bits, out_params) == 0) {
            tls_cached = *out_params;
            tls_cached_valid = 1;
            return 0;
        }
    }
    return -1;
}

int invfs_deflate_repro_encode(const uint8_t *raw, size_t raw_len,
                               const invfs_deflate_params *params,
                               uint8_t **out_stream, size_t *out_len)
{
    const invfs_deflate_backend *backend;
    const invfs_deflate_backend *other;
    int rc;

    if (!raw || !params || !out_stream || !out_len)
        return -1;
    backend = invfs_deflate_backend_get(params->engine);
    rc = backend->encode(raw, raw_len, params, out_stream, out_len);
    if (rc == 0) {
        tls_cached = *params;
        tls_cached_valid = 1;
        return 0;
    }
    other = backend == &invfs_deflate_backend_stock
                ? &invfs_deflate_backend_system
                : &invfs_deflate_backend_stock;
    return other->encode(raw, raw_len, params, out_stream, out_len);
}

int invfs_deflate_decompress(const uint8_t *stream, size_t stream_len,
                             int window_bits,
                             uint8_t **out_raw, size_t *out_len)
{
    if (invfs_deflate_backend_system.decompress(stream, stream_len, window_bits,
                                                out_raw, out_len) == 0)
        return 0;
    return invfs_deflate_backend_stock.decompress(stream, stream_len, window_bits,
                                                  out_raw, out_len);
}
