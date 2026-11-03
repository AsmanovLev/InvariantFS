/*
 * invfs_preflate.h — Universal bit-exact DEFLATE reversibility C-ABI
 *
 * Backed by libinvf_preflate.a (Rust implementation based on preflate-rs).
 *
 * Acknowledgements:
 * - Dirk Steinke (Dropbox): Original preflate C++ implementation
 *   https://github.com/dropbox/preflate
 * - Microsoft & Dirk Steinke: preflate-rs implementation
 *   https://github.com/microsoft/preflate-rs
 * - Jiang JQ: png2jxl reversible PNG architecture
 *   https://github.com/JiangJQ2000/png2jxl
 */

#ifndef INVFS_PREFLATE_H
#define INVFS_PREFLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encodes raw DEFLATE or RFC 1950 zlib stream into plaintext and corrections delta (`pfd`).
 *
 * @param deflate         Pointer to compressed stream.
 * @param deflate_len     Byte length of compressed stream.
 * @param out_plain       Receives pointer to allocated plaintext (free via invfs_preflate_free or free).
 * @param out_plain_len   Receives byte length of plaintext.
 * @param out_pfd         Receives pointer to allocated corrections (free via invfs_preflate_free or free).
 * @param out_pfd_len     Receives byte length of corrections.
 * @return 0 on success, negative error code on failure.
 */
int invfs_preflate_encode(const uint8_t *deflate, size_t deflate_len,
                          uint8_t **out_plain, size_t *out_plain_len,
                          uint8_t **out_pfd, size_t *out_pfd_len);

/**
 * Decodes plaintext and preflate delta (`pfd`) back into the exact original bitstream.
 *
 * @param plain           Pointer to uncompressed plaintext bytes.
 * @param plain_len       Byte length of plaintext.
 * @param pfd             Pointer to preflate corrections delta.
 * @param pfd_len         Byte length of corrections delta.
 * @param out_deflate     Receives pointer to allocated DEFLATE stream (free via invfs_preflate_free or free).
 * @param out_deflate_len Receives byte length of reconstructed stream.
 * @return 0 on success (bit-exact reconstruction), negative error code on failure.
 */
int invfs_preflate_decode(const uint8_t *plain, size_t plain_len,
                          const uint8_t *pfd, size_t pfd_len,
                          uint8_t **out_deflate, size_t *out_deflate_len);

/**
 * Frees buffer allocated by invfs_preflate_encode or invfs_preflate_decode.
 */
void invfs_preflate_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* INVFS_PREFLATE_H */
