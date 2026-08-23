/*
 * bench.c — phase timings for the write path, no Dokan in the way.
 *
 *   invf-bench <image> <count>
 *
 * Creates <count> 4 KiB files on an already-formatted image, reporting
 * where the time goes per phase. Used to find the terms that scale with
 * the number of files already on the volume.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "invarifs.h"
#include "volume.h"

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0, n, i;
    uint8_t *payload;
    double t_create = 0, t_pending = 0, t_flush = 0, t_read = 0, t0;

    if (argc < 3) { fprintf(stderr, "usage: invf-bench <image> <count>\n"); return 1; }
    n = atoi(argv[2]);
    if (n <= 0) return 1;

    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "vol_open failed (%d)\n", err); return 1; }

    payload = (uint8_t *)malloc(4096);
    memset(payload, 'x', 4096);

    for (i = 0; i < n; i++) {
        char name[64];
        uint64_t id;
        snprintf(name, sizeof name, "f%d.bin", i);

        t0 = now_ms();
        id = vol_create_file(v, name, payload, 4096);
        t_create += now_ms() - t0;
        if (!id) { fprintf(stderr, "create failed at %d\n", i); break; }

        t0 = now_ms();
        vol_mark_pending(v, id);
        t_pending += now_ms() - t0;

        t0 = now_ms();
        vol_flush(v);
        t_flush += now_ms() - t0;
    }

    for (i = 0; i < n; i++) {
        char name[64];
        uint64_t id, size, ctime;
        uint8_t *data = NULL;
        size_t len = 0;
        snprintf(name, sizeof name, "f%d.bin", i);
        t0 = now_ms();
        if (vol_stat_full(v, name, &id, &size, &ctime) == 0 &&
            vol_read_file(v, id, &data, &len) == 0)
            free(data);
        t_read += now_ms() - t0;
    }

    printf("%5d files: create %6.3f  pending %6.3f  flush %6.3f  read %6.3f  (ms/file)\n",
           n, t_create / n, t_pending / n, t_flush / n, t_read / n);

    free(payload);
    vol_close(v);
    return 0;
}
