#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include "tmpstore.h"
#include "invarifs.h"

#define INITIAL_BUCKETS 64
#define LOAD_FACTOR_THRESHOLD 0.75

static tmpstore *g_tmpstore = NULL;
static pthread_mutex_t g_tmpstore_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hash_path(const char *path) {
    uint32_t h = 5381;
    while (*path) {
        h = ((h << 5) + h) ^ (unsigned char)*path;
        path++;
    }
    return h;
}

static tmpstore_entry *entry_create(const char *path, const uint8_t *data, size_t len) {
    tmpstore_entry *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->name = strdup(path);
    if (!e->name) { free(e); return NULL; }
    if (len > 0) {
        e->data = malloc(len);
        if (!e->data) { free(e->name); free(e); return NULL; }
        memcpy(e->data, data, len);
    } else {
        e->data = NULL;
    }
    e->size = len;
    e->capacity = len;
    e->mtime = time(NULL);
    return e;
}

static void entry_destroy(tmpstore_entry *e) {
    if (e) {
        free(e->name);
        free(e->data);
        free(e);
    }
}

static void entry_lru_remove(tmpstore *ts, tmpstore_entry *e) {
    if (e == ts->lru_head) {
        ts->lru_head = e->next;
    } else {
        tmpstore_entry *prev = ts->lru_head;
        while (prev && prev->next != e) prev = prev->next;
        if (prev) prev->next = e->next;
    }
    if (e == ts->lru_tail) ts->lru_tail = e->next;
}

static void entry_lru_add(tmpstore *ts, tmpstore_entry *e) {
    e->next = NULL;
    if (ts->lru_tail) {
        ts->lru_tail->next = e;
        ts->lru_tail = e;
    } else {
        ts->lru_head = ts->lru_tail = e;
    }
}

static void entry_lru_move_to_tail(tmpstore *ts, tmpstore_entry *e) {
    if (e == ts->lru_tail) return;
    entry_lru_remove(ts, e);
    entry_lru_add(ts, e);
}

static int need_resize(tmpstore *ts) {
    return (size_t)ts->entry_count > (size_t)(ts->n_buckets * LOAD_FACTOR_THRESHOLD);
}

static int tmpstore_resize(tmpstore *ts) {
    size_t new_n = ts->n_buckets * 2;
    tmpstore_entry **new_buckets = calloc(new_n, sizeof(*new_buckets));
    if (!new_buckets) return -1;
    for (size_t i = 0; i < ts->n_buckets; i++) {
        tmpstore_entry *e = ts->buckets[i];
        while (e) {
            tmpstore_entry *next = e->next;
            uint32_t h = hash_path(e->name);
            size_t idx = h % new_n;
            e->next = new_buckets[idx];
            new_buckets[idx] = e;
            e = next;
        }
    }
    free(ts->buckets);
    ts->buckets = new_buckets;
    ts->n_buckets = new_n;
    return 0;
}

int tmpstore_init(size_t max_bytes, tmp_area_mode mode) {
    pthread_mutex_lock(&g_tmpstore_lock);
    if (g_tmpstore) {
        pthread_mutex_unlock(&g_tmpstore_lock);
        return 0;
    }
    g_tmpstore = calloc(1, sizeof(*g_tmpstore));
    if (!g_tmpstore) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
    g_tmpstore->magic = TMPSTORE_MAGIC;
    g_tmpstore->mode = mode;
    g_tmpstore->max_bytes = max_bytes ? max_bytes : 128 * 1024 * 1024;
    g_tmpstore->n_buckets = INITIAL_BUCKETS;
    g_tmpstore->buckets = calloc(INITIAL_BUCKETS, sizeof(*g_tmpstore->buckets));
    if (!g_tmpstore->buckets) {
        free(g_tmpstore);
        g_tmpstore = NULL;
        pthread_mutex_unlock(&g_tmpstore_lock);
        return -1;
    }
    pthread_mutex_unlock(&g_tmpstore_lock);
    return 0;
}

void tmpstore_destroy(void) {
    pthread_mutex_lock(&g_tmpstore_lock);
    if (!g_tmpstore) {
        pthread_mutex_unlock(&g_tmpstore_lock);
        return;
    }
    for (size_t i = 0; i < g_tmpstore->n_buckets; i++) {
        tmpstore_entry *e = g_tmpstore->buckets[i];
        while (e) {
            tmpstore_entry *next = e->next;
            entry_destroy(e);
            e = next;
        }
    }
    free(g_tmpstore->buckets);
    free(g_tmpstore);
    g_tmpstore = NULL;
    pthread_mutex_unlock(&g_tmpstore_lock);
}

static tmpstore_entry *tmpstore_lookup_unlocked(tmpstore *ts, const char *path) {
    uint32_t h = hash_path(path);
    size_t idx = h % ts->n_buckets;
    tmpstore_entry *e = ts->buckets[idx];
    while (e) {
        if (strcmp(e->name, path) == 0) return e;
        e = e->next;
    }
    return NULL;
}

static int evict_lru(tmpstore *ts, size_t need) {
    while (ts->lru_head && ts->used_bytes + need > ts->max_bytes) {
        tmpstore_entry *e = ts->lru_head;
        entry_lru_remove(ts, e);
        uint32_t h = hash_path(e->name);
        size_t idx = h % ts->n_buckets;
        tmpstore_entry **prev = &ts->buckets[idx];
        while (*prev && *prev != e) prev = &(*prev)->next;
        if (*prev == e) *prev = e->next;
        ts->used_bytes -= e->size;
        ts->entry_count--;
        entry_destroy(e);
    }
    return (ts->used_bytes + need <= ts->max_bytes) ? 0 : -1;
}

int tmpstore_put(const char *path, const uint8_t *data, size_t len) {
    pthread_mutex_lock(&g_tmpstore_lock);
    if (!g_tmpstore) { fprintf(stderr, "tmpstore_put: g_tmpstore is NULL!\n"); pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
    fprintf(stderr, "tmpstore_put: path=%s data=%p len=%zu\n", path, data, len);
    tmpstore_entry *existing = tmpstore_lookup_unlocked(g_tmpstore, path);
    fprintf(stderr, "tmpstore_put: existing=%p\n", existing);
    if (existing) {
        if (existing->capacity >= len) {
            memcpy(existing->data, data, len);
        } else {
            uint8_t *new_data = realloc(existing->data, len);
            if (!new_data) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
            size_t old_size = existing->size;
            existing->data = new_data;
            existing->capacity = len;
            memcpy(existing->data, data, len);
            g_tmpstore->used_bytes -= old_size;
        }
        existing->size = len;
        existing->mtime = time(NULL);
        g_tmpstore->used_bytes += len;
        entry_lru_move_to_tail(g_tmpstore, existing);
        pthread_mutex_unlock(&g_tmpstore_lock);
        return 0;
    }
    if (evict_lru(g_tmpstore, len) != 0) {
        pthread_mutex_unlock(&g_tmpstore_lock);
        return -1;
    }
    tmpstore_entry *e = entry_create(path, data, len);
    if (!e) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
    uint32_t h = hash_path(path);
    size_t idx = h % g_tmpstore->n_buckets;
    e->next = g_tmpstore->buckets[idx];
    g_tmpstore->buckets[idx] = e;
    g_tmpstore->used_bytes += len;
    g_tmpstore->entry_count++;
    entry_lru_add(g_tmpstore, e);
    if (need_resize(g_tmpstore)) tmpstore_resize(g_tmpstore);
    pthread_mutex_unlock(&g_tmpstore_lock);
    return 0;
}

int tmpstore_get(const char *path, uint8_t **data, size_t *len) {
    pthread_mutex_lock(&g_tmpstore_lock);
    if (!g_tmpstore) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
    tmpstore_entry *e = tmpstore_lookup_unlocked(g_tmpstore, path);
    if (!e) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
    entry_lru_move_to_tail(g_tmpstore, e);
    *data = e->data;
    *len = e->size;
    pthread_mutex_unlock(&g_tmpstore_lock);
    return 0;
}

int tmpstore_delete(const char *path) {
    pthread_mutex_lock(&g_tmpstore_lock);
    if (!g_tmpstore) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
    fprintf(stderr, "tmpstore_delete: path=%s\n", path);
    uint32_t h = hash_path(path);
    size_t idx = h % g_tmpstore->n_buckets;
    tmpstore_entry **prev = &g_tmpstore->buckets[idx];
    tmpstore_entry *e = *prev;
    while (e && strcmp(e->name, path) != 0) {
        prev = &e->next;
        e = e->next;
    }
    if (!e) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
    *prev = e->next;
    entry_lru_remove(g_tmpstore, e);
    g_tmpstore->used_bytes -= e->size;
    g_tmpstore->entry_count--;
    entry_destroy(e);
    pthread_mutex_unlock(&g_tmpstore_lock);
    return 0;
}

bool tmpstore_exists(const char *path) {
    pthread_mutex_lock(&g_tmpstore_lock);
    if (!g_tmpstore) { pthread_mutex_unlock(&g_tmpstore_lock); return false; }
    bool exists = tmpstore_lookup_unlocked(g_tmpstore, path) != NULL;
    pthread_mutex_unlock(&g_tmpstore_lock);
    return exists;
}

size_t tmpstore_used_bytes(void) {
    pthread_mutex_lock(&g_tmpstore_lock);
    size_t used = g_tmpstore ? g_tmpstore->used_bytes : 0;
    pthread_mutex_unlock(&g_tmpstore_lock);
    return used;
}

size_t tmpstore_entry_count(void) {
    pthread_mutex_lock(&g_tmpstore_lock);
    size_t count = g_tmpstore ? g_tmpstore->entry_count : 0;
    pthread_mutex_unlock(&g_tmpstore_lock);
    return count;
}

int tmpstore_truncate(const char *path, size_t len) {
    pthread_mutex_lock(&g_tmpstore_lock);
    if (!g_tmpstore) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
    tmpstore_entry *e = tmpstore_lookup_unlocked(g_tmpstore, path);
    if (!e) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
    if (len < e->size) {
        e->size = len;
    } else if (len > e->size) {
        if (len > e->capacity) {
            uint8_t *new_data = realloc(e->data, len);
            if (!new_data) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
            size_t old_size = e->size;
            e->data = new_data;
            e->capacity = len;
            g_tmpstore->used_bytes -= old_size;
            g_tmpstore->used_bytes += len;
        }
        memset(e->data + e->size, 0, len - e->size);
        e->size = len;
    }
    e->mtime = time(NULL);
    pthread_mutex_unlock(&g_tmpstore_lock);
    return 0;
}

int tmpstore_write(const char *path, const uint8_t *data, size_t len, size_t offset) {
    pthread_mutex_lock(&g_tmpstore_lock);
    if (!g_tmpstore) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
    tmpstore_entry *e = tmpstore_lookup_unlocked(g_tmpstore, path);
    if (!e) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
    size_t new_size = offset + len;
    if (new_size > e->capacity) {
        size_t new_cap = new_size * 2;
        uint8_t *new_data = realloc(e->data, new_cap);
        if (!new_data) { pthread_mutex_unlock(&g_tmpstore_lock); return -1; }
        size_t old_used = e->size;
        e->data = new_data;
        e->capacity = new_cap;
        g_tmpstore->used_bytes -= old_used;
        g_tmpstore->used_bytes += new_size;
    }
    if (new_size > e->size) {
        memset(e->data + e->size, 0, new_size - e->size);
        e->size = new_size;
    }
    memcpy(e->data + offset, data, len);
    e->mtime = time(NULL);
    pthread_mutex_unlock(&g_tmpstore_lock);
    return 0;
}
