/*
 * treecp.c — copy a host directory tree into a volume in one process.
 *
 *   invf-treecp <image-or-device> <host-dir> <prefix> [limit]
 *
 * Why this exists: invf-bench writes 4 KiB of 'x' and invf-cp reopens the
 * volume for every file, so neither reproduces what the Dokan mount does --
 * one long-lived volume handle fed real files of real sizes, whose CONTENT
 * decides which codec path runs. The mount died silently at ~115 files of a
 * music tree; this walks the same tree in the same order, in-process, so the
 * failure can be caught without Dokan or elevation.
 *
 * Each name is printed and flushed BEFORE the write, so when the process dies
 * from an access violation the last line names the file that killed it.
 * stdout is unbuffered for the same reason.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "invarifs.h"
#include "volume.h"

static invfs_volume *g_v;
static int g_done, g_fail, g_limit;

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* Read a whole host file. Returns NULL on failure. */
static uint8_t *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *b;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    b = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    if (!b) { fclose(f); return NULL; }
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b); fclose(f); return NULL;
    }
    fclose(f);
    *len_out = (size_t)n;
    return b;
}

static void walk(const char *host, const char *vname)
{
    char pat[4096];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof pat, "%s\\*", host);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        char hp[4096], vp[4096];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (g_limit && g_done + g_fail >= g_limit) break;

        snprintf(hp, sizeof hp, "%s\\%s", host, fd.cFileName);
        snprintf(vp, sizeof vp, "%s/%s", vname, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            printf("[dir ] %s\n", vp);
            fflush(stdout);
            vol_ensure_path(g_v, vp);
            vol_flush(g_v);
            walk(hp, vp);
        } else {
            size_t len = 0;
            uint8_t *data;
            uint64_t id;
            double t0;

            /* printed BEFORE the write: a silent death names its own file */
            printf("[%4d] %s\n", g_done + g_fail, vp);
            fflush(stdout);

            data = slurp(hp, &len);
            if (!data) {
                printf("        READ FAILED (host)\n");
                fflush(stdout);
                g_fail++;
                continue;
            }
            t0 = now_ms();
            id = vol_create_file(g_v, vp, data, len);
            if (!id) {
                printf("        create failed (%zu bytes)\n", len);
                fflush(stdout);
                g_fail++;
            } else {
                vol_mark_pending(g_v, id);
                if (vol_flush(g_v) != 0) {
                    printf("        FLUSH FAILED\n");
                    fflush(stdout);
                    g_fail++;
                } else {
                    printf("        ok %zu bytes  %.0f ms\n", len, now_ms() - t0);
                    fflush(stdout);
                    g_done++;
                }
            }
            free(data);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int main(int argc, char **argv)
{
    int err = 0;

    if (argc < 4) {
        fprintf(stderr,
            "usage: invf-treecp <image|device> <host-dir> <prefix> [limit]\n");
        return 1;
    }
    g_limit = argc > 4 ? atoi(argv[4]) : 0;

    setvbuf(stdout, NULL, _IONBF, 0);

    g_v = vol_open(argv[1], &err);
    if (!g_v) { fprintf(stderr, "vol_open failed (%d)\n", err); return 1; }

    vol_ensure_path(g_v, argv[3]);
    vol_flush(g_v);
    walk(argv[2], argv[3]);

    printf("\ndone: %d ok, %d failed\n", g_done, g_fail);
    vol_close(g_v);
    return g_fail ? 2 : 0;
}
