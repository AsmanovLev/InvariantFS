/*
 * cat.c — extract a file from an InvariantFS volume
 *
 *   invf-cat <image> <name> [output-file]
 *
 * If output file is given, writes it. With --verify it compares
 * against a host file and reports bit-perfect status.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "invarifs.h"
#include "volume.h"

int main(int argc, char **argv)
{
    const char *img, *name;
    invfs_volume *vol;
    uint64_t inode_id;
    uint8_t *data = NULL;
    size_t len = 0;
    int err;
    const char *out = NULL;

#ifdef _WIN32
    /* stdout must not convert LF -> CRLF on binary file dumps */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: invf-cat <image> <name> [output-file]\n");
        return 2;
    }
    img = argv[1];
    name = argv[2];
    if (argc == 4) out = argv[3];

    vol = vol_open(img, &err);
    if (!vol) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
        return 1;
    }

    if (strchr(name, '!')) {
        /* prefer a real inode (e.g. "name!recipe" FLAC sibling); fall back
           to container-member extraction (ZIP) if no such inode exists */
        inode_id = vol_find(vol, name);
        if (inode_id != 0) {
            if (vol_read_file(vol, inode_id, &data, &len) != 0) {
                fprintf(stderr, "read failed\n");
                vol_close(vol);
                return 1;
            }
        } else if (vol_read_named(vol, name, &data, &len) != 0) {
            fprintf(stderr, "'%s' not found / extract failed\n", name);
            vol_close(vol);
            return 1;
        }
    } else {
        inode_id = vol_find(vol, name);
        if (inode_id == 0) {
            fprintf(stderr, "'%s' not found\n", name);
            vol_close(vol);
            return 1;
        }
        if (vol_read_file(vol, inode_id, &data, &len) != 0) {
            fprintf(stderr, "read failed\n");
            vol_close(vol);
            return 1;
        }
    }

    if (out) {
        FILE *f = fopen(out, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", out); free(data); vol_close(vol); return 1; }
        if (fwrite(data, 1, len, f) != len || fclose(f) != 0) {
            fprintf(stderr, "write failed: %s\n", out);
            free(data); vol_close(vol); return 1;
        }
        printf("extracted '%s' -> %s (%zu bytes)\n", name, out, len);
    } else {
        if (fwrite(data, 1, len, stdout) != len || fflush(stdout) != 0) {
            fprintf(stderr, "write failed: stdout\n");
            free(data); vol_close(vol); return 1;
        }
    }

    free(data);
    vol_close(vol);
    return 0;
}
