#ifndef PPMD_CODEC_H
#define PPMD_CODEC_H

#include <stddef.h>
#include <stdint.h>

/* PPMd8 stream codec wrapper (WP10).
 * Wire format: [2B LE props][PPMd8 range-coded stream incl. END marker].
 * Params: order=8, mem=64MB, restor=CUT_OFF (props bytes f7 13).
 * decode needs the EXACT decoded size up front (outlen). */
int invfs_ppmd_encode(const uint8_t *in, size_t inlen,
                      uint8_t *out, size_t outcap, size_t *outlen);
int invfs_ppmd_decode(const uint8_t *in, size_t inlen,
                      uint8_t *out, size_t outlen);

#endif /* PPMD_CODEC_H */
