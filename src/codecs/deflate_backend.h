#ifndef INVFS_DEFLATE_BACKEND_H
#define INVFS_DEFLATE_BACKEND_H

#include "deflate_repro.h"

typedef struct {
    uint8_t engine;
    int (*find)(const uint8_t *, size_t, const uint8_t *, size_t, int,
                invfs_deflate_params *);
    int (*encode)(const uint8_t *, size_t, const invfs_deflate_params *,
                  uint8_t **, size_t *);
    int (*decompress)(const uint8_t *, size_t, int, uint8_t **, size_t *);
} invfs_deflate_backend;

extern const invfs_deflate_backend invfs_deflate_backend_system;
extern const invfs_deflate_backend invfs_deflate_backend_stock;

const invfs_deflate_backend *invfs_deflate_backend_get(uint8_t engine);

#endif
