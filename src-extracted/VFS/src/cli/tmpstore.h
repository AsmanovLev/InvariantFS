#ifndef INVFS_TMPSTORE_H
#define INVFS_TMPSTORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define TMPSTORE_MAGIC 0x544D5053  /* "TMPS" */

typedef enum {
    TMP_AREA_RAM = 0,
    TMP_AREA_VOLUME = 1,
    TMP_AREA_AUTO = 2,
} tmp_area_mode;

typedef struct tmpstore_entry {
    char *name;
    uint8_t *data;
    size_t size;
    size_t capacity;
    int64_t mtime;
    struct tmpstore_entry *next;
} tmpstore_entry;

typedef struct {
    uint32_t magic;
    tmp_area_mode mode;
    size_t max_bytes;
    size_t used_bytes;
    tmpstore_entry **buckets;
    size_t n_buckets;
    tmpstore_entry *lru_head;
    tmpstore_entry *lru_tail;
    int entry_count;
} tmpstore;

int  tmpstore_init(size_t max_bytes, tmp_area_mode mode);
void tmpstore_destroy(void);

int  tmpstore_put(const char *path, const uint8_t *data, size_t len);
int  tmpstore_get(const char *path, uint8_t **data, size_t *len);
int  tmpstore_delete(const char *path);
bool tmpstore_exists(const char *path);

size_t tmpstore_used_bytes(void);
size_t tmpstore_entry_count(void);

int  tmpstore_truncate(const char *path, size_t len);
int  tmpstore_write(const char *path, const uint8_t *data, size_t len, size_t offset);

#endif /* INVFS_TMPSTORE_H */
