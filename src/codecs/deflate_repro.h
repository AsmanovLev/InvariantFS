/* deflate_repro.h — universal deflate reproduction helper for InvariantFS.
 *
 * Provides bit-exact reproduction parameter discovery, re-encoding, and
 * decompression across QCOW2 compressed clusters, GZIP, ZIP, and PNG IDAT.
 */
#ifndef INVFS_DEFLATE_REPRO_H
#define INVFS_DEFLATE_REPRO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Engine identifiers */
#define INVFS_DEFLATE_ENGINE_ZLIB   0
#define INVFS_DEFLATE_ENGINE_MINIZ  1

/* Parameter descriptor for exact deflate reproduction */
typedef struct {
    uint8_t engine;      /* INVFS_DEFLATE_ENGINE_* */
    int8_t  level;       /* 1..9 */
    int8_t  mem_level;   /* 1..9 */
    int8_t  strategy;    /* 0=DEFAULT, 1=FILTERED, 2=HUFFMAN, 3=RLE, 4=FIXED */
    int8_t  window_bits; /* e.g. -12 for QCOW2, -15 for raw deflate, 15 for zlib, 31 for gzip */
} invfs_deflate_params;

/*
 * Search for exact matching parameters that reproduce target_stream bit-for-bit
 * from uncompressed raw data.
 *
 * window_bits_hint: e.g. -12 for QCOW2, -15 for raw deflate / GZIP / ZIP, 15 for zlib header.
 * If window_bits_hint == 0, defaults to -15.
 *
 * Returns 0 on exact bit-for-bit match (*out_params populated).
 * Returns -1 if no standard parameter combination matches bit-exact.
 */
int invfs_deflate_repro_find(const uint8_t *raw, size_t raw_len,
                             const uint8_t *target_stream, size_t target_len,
                             int window_bits_hint,
                             invfs_deflate_params *out_params);

/*
 * Re-encode raw uncompressed data using the specified params into *out_stream (*out_len).
 * On success, *out_stream is allocated with malloc() and must be freed by caller.
 * Returns 0 on success, -1 on error.
 */
int invfs_deflate_repro_encode(const uint8_t *raw, size_t raw_len,
                              const invfs_deflate_params *params,
                              uint8_t **out_stream, size_t *out_len);

/*
 * Decompress a deflate stream into a newly allocated buffer *out_raw (*out_len).
 * window_bits: -15 (raw deflate), -12 (qcow2), 15 (zlib), 31 (gzip).
 * Returns 0 on success, -1 on corrupt stream or error.
 */
int invfs_deflate_decompress(const uint8_t *stream, size_t stream_len,
                             int window_bits,
                             uint8_t **out_raw, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* INVFS_DEFLATE_REPRO_H */
