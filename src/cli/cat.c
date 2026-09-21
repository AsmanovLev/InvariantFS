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
    int ignore_missing_codecs = 0;

#ifdef _WIN32
    /* stdout must not convert LF -> CRLF on binary file dumps */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-cat [--ignore-missing-codecs] <image> <name> [output-file]\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
        if (strcmp(argv[i], "--ignore-missing-codecs") == 0) {
            ignore_missing_codecs = 1;
            continue;
        }
    }

    if (argc < 3 || argc > 5) {
        fprintf(stderr, "usage: invf-cat [--ignore-missing-codecs] <image> <name> [output-file]\n");
        return 2;
    }
    /* skip past --ignore-missing-codecs to find positional args */
    {
        int ai = 1;
        while (ai < argc && strcmp(argv[ai], "--ignore-missing-codecs") == 0) ai++;
        img = argv[ai++];
        name = argv[ai++];
        if (ai < argc) out = argv[ai];
    }

    vol = vol_open(img, &err);
    if (!vol) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
        return 1;
    }

    /* WP59: codec-policy gate. Refuse if no PCK0 and not --ignore-missing-codecs */
    if (!vol_pck0_present(vol) && !ignore_missing_codecs) {
        fprintf(stderr, "invf-cat: no codec policy (PCK0); "
                "use --ignore-missing-codecs to proceed\n");
        vol_close(vol);
        return 1;
    }

    if (strchr(name, '!')) {
        /* container members stay on the whole-member extraction path
           (bounded by the member's own size; vol_read_named splices the
           window) */
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

    /* WP71h: regular files stream through vol_read_range in bounded
     * chunks. The old whole-file slurp (vol_read_file) needed RAM >=
     * file size on a filesystem whose format allows 1 TB files: on a
     * small-RAM host a healthy 4 GiB file "failed to read" (malloc),
     * and invf-verify --deep even mislabelled that OOM as CORRUPT. The
     * engine's windowed read is the sanctioned big-file path (the
     * test-astv2 verify-big harness has always used it). */
    {
        uint64_t size = 0;
        const uint64_t CHUNK = 4ull << 20;
        uint8_t *cbuf;
        uint64_t off = 0;
        FILE *f = NULL;
        int rc = 0;

        inode_id = vol_find_ex(vol, name, &size, NULL);
        if (inode_id == 0) {
            fprintf(stderr, "'%s' not found\n", name);
            vol_close(vol);
            return 1;
        }
        if (out) {
            f = fopen(out, "wb");
            if (!f) {
                fprintf(stderr, "cannot write %s\n", out);
                vol_close(vol);
                return 1;
            }
        }
        cbuf = (uint8_t *)malloc((size_t)CHUNK);
        if (!cbuf) {
            fprintf(stderr, "invf-cat: out of memory for the %llu-byte "
                    "read chunk (NOT a volume error)\n",
                    (unsigned long long)CHUNK);
            if (f) fclose(f);
            vol_close(vol);
            return 1;
        }
        while (off < size) {
            size_t n = (size_t)((size - off > CHUNK) ? CHUNK : (size - off));
            int rd = vol_read_range(vol, inode_id, off, n, cbuf);
            if (rd < 0 || (size_t)rd != n) {
                fprintf(stderr, "read failed at offset %llu (segment CRC "
                        "mismatch, short read or io error -- the volume "
                        "reports details above)\n",
                        (unsigned long long)off);
                rc = 1;
                break;
            }
            if (fwrite(cbuf, 1, n, f ? f : stdout) != n) {
                fprintf(stderr, "write failed: %s\n", out ? out : "stdout");
                rc = 1;
                break;
            }
            off += (uint64_t)rd;
        }
        free(cbuf);
        if (f) {
            if (fclose(f) != 0 && rc == 0) {
                fprintf(stderr, "write failed: %s (close)\n", out);
                rc = 1;
            }
        } else if (fflush(stdout) != 0 && rc == 0) {
            fprintf(stderr, "write failed: stdout\n");
            rc = 1;
        }
        if (rc == 0 && out)
            printf("extracted '%s' -> %s (%llu bytes)\n", name, out,
                   (unsigned long long)size);
        vol_close(vol);
        return rc;
    }
}
