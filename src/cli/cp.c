/*
 * cp.c — copy a host file into an InvariantFS volume
 *
 *   invf-cp <image> <host-file> [name-in-volume]
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "invarifs.h"
#include "volume.h"

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    unsigned char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc(sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

int main(int argc, char **argv)
{
    const char *img, *host, *name;
    size_t len = 0;
    unsigned char *data;
    invfs_volume *vol;
    int err;
    uint64_t inode_id;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-cp <image> <host-file> [name]\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: invf-cp <image> <host-file> [name]\n");
        return 2;
    }
    img = argv[1];
    host = argv[2];
    name = (argc == 4) ? argv[3] : host;

    data = read_file(host, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", host);
        return 1;
    }

    vol = vol_open(img, &err);
    if (!vol) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
        free(data);
        return 1;
    }

    /* Copying onto a name that already exists is an overwrite, not a second
       file. Appending a bare INOD record reads back correctly -- the scans keep
       the newest version per name -- but the old inode's blocks and any sweep
       siblings (name!recipe, name!partN) are never reclaimed. vol_replace_file
       writes the new record first, then retires the old one. */
    inode_id = vol_find(vol, name)
             ? vol_replace_file(vol, name, data, len)
             : vol_create_file(vol, name, data, len);
    if (inode_id == 0) {
        fprintf(stderr, "write failed (volume full?)\n");
        vol_close(vol);
        free(data);
        return 1;
    }

    if (vol_flush(vol) != 0) {
        fprintf(stderr, "flush failed\n");
        vol_close(vol);
        free(data);
        return 1;
    }

    printf("stored '%s' as inode %llu: %zu bytes -> RAW zone\n",
           name, (unsigned long long)inode_id, len);
    vol_close(vol);
    free(data);
    return 0;
}
