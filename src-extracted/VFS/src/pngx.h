/* pngx.h — PNG Repack (JXL lossless + IVPN recipe) shared definitions. */
#ifndef PNGX_H
#define PNGX_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t width, height;
    uint8_t  bitdepth, colortype, interlace;
    uint8_t  bpp;                /* bytes per pixel (8-bit rows) */
    size_t   row_bytes;          /* filter byte + width*bpp */
    uint8_t *pre;  size_t pre_len;  uint8_t *post; size_t post_len;
    size_t  *idat_split; size_t idat_n;
    uint8_t *filters; size_t nrows;
    uint8_t *filtered; size_t filtered_len;
    uint8_t *rgb;  size_t rgb_len;
    uint8_t *idat; size_t idat_len;
    uint8_t *idat_crc;   /* original IDAT chunk CRCs (4B each) */
    uint8_t  iend_crc[4];/* original IEND CRC (may be broken) */
    uint8_t  enc, level, mem;
} pngx_info;

int pngx_extract(const uint8_t *png, size_t png_len,
                 const unsigned char *idat_extra, size_t idat_extra_len,
                 int (*inflate_fn)(const unsigned char *in, size_t in_len,
                                   unsigned char **out, size_t *out_len),
                 pngx_info *info);
int pngx_build_recipe(const pngx_info *info, uint8_t enc, uint8_t level,
                      uint8_t mem, uint8_t **recipe_out, size_t *rlen_out);
int pngx_parse_recipe(const uint8_t *r, size_t rlen, pngx_info *info);
int pngx_refilter(const uint8_t *rgb, size_t rgb_len, const pngx_info *info,
                  uint8_t **out, size_t *out_len);
int pngx_rebuild(const pngx_info *info, const uint8_t *idat_stream,
                 size_t idat_len, uint8_t **out, size_t *out_len);
void pngx_free(pngx_info *info);
uint32_t wr32v(uint8_t *p, uint32_t v);
int pngx_chunk_append(uint8_t **buf, size_t *len, size_t *cap,
                      const uint8_t *type, const uint8_t *data, size_t dlen);

#endif
