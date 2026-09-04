/*
 * devwbench.c -- what a raw write actually costs on this device.
 *
 *   invf-devwbench \\.\E:        (DESTROYS the volume on that device)
 *
 * The copy path currently issues one synchronous write per 64 KB segment. On
 * flash that measured ~84 ms per write, which is the entire cost of a copy:
 * 2381 writes x 84 ms == the 200 s an album took. Before restructuring the
 * write path around that number, this establishes which lever actually moves
 * it, because there are two and they are independent:
 *
 *   chunk size     -- do larger writes amortise, or is the device just slow?
 *   WRITE_THROUGH  -- NO_BUFFERING already bypasses the OS cache; the extra
 *                     flag also forces a device-level flush per write. If the
 *                     device honours it, dropping it and flushing once per
 *                     commit is a far smaller change than write-combining.
 *
 * Writes land at a fixed offset well inside the device and are the same total
 * volume for every case, so the only variables are the two above.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

#define TOTAL_MB   48
#define BASE_OFF   (256ull * 1024 * 1024)   /* past any metadata we care about */

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int run(const char *dev, size_t chunk, int write_through, double *out_ms)
{
    DWORD flags = FILE_FLAG_NO_BUFFERING;
    HANDLE h;
    uint8_t *buf;
    uint64_t off = BASE_OFF;
    uint64_t total = (uint64_t)TOTAL_MB * 1024 * 1024;
    uint64_t done = 0;
    double t0;

    if (write_through) flags |= FILE_FLAG_WRITE_THROUGH;

    h = CreateFileA(dev, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, flags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "open %s failed: %lu\n", dev, GetLastError());
        return -1;
    }

    buf = (uint8_t *)_aligned_malloc(chunk, 4096);
    if (!buf) { CloseHandle(h); return -1; }
    memset(buf, 0x5A, chunk);

    t0 = now_ms();
    while (done < total) {
        LARGE_INTEGER li;
        DWORD wrote = 0;
        li.QuadPart = (LONGLONG)(off + done);
        if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN) ||
            !WriteFile(h, buf, (DWORD)chunk, &wrote, NULL) ||
            wrote != (DWORD)chunk) {
            fprintf(stderr, "write failed at %llu: %lu\n",
                    (unsigned long long)(off + done), GetLastError());
            _aligned_free(buf); CloseHandle(h); return -1;
        }
        done += chunk;
    }
    /* Without WRITE_THROUGH the data may still sit in the device cache. A
       commit would have to flush, so charge that to this case too -- otherwise
       we would be comparing a durable write against a non-durable one. */
    if (!write_through) FlushFileBuffers(h);
    *out_ms = now_ms() - t0;

    _aligned_free(buf);
    CloseHandle(h);
    return 0;
}

int main(int argc, char **argv)
{
    size_t chunks[] = { 64 * 1024, 256 * 1024, 1024 * 1024, 4 * 1024 * 1024 };
    size_t i;
    int wt;

    if (argc < 2) {
        fprintf(stderr, "usage: invf-devwbench <\\\\.\\E:>\n");
        fprintf(stderr, "WARNING: destroys the volume on that device\n");
        return 2;
    }

    printf("raw write cost, %d MB per case, offset %llu\n",
           TOTAL_MB, (unsigned long long)BASE_OFF);
    printf("%-10s %-14s %10s %10s %12s\n",
           "chunk", "mode", "ms", "MB/s", "ms/write");

    for (wt = 1; wt >= 0; wt--) {
        for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
            double ms = 0;
            size_t c = chunks[i];
            uint64_t n = ((uint64_t)TOTAL_MB * 1024 * 1024) / c;
            if (run(argv[1], c, wt, &ms) != 0) return 1;
            printf("%-10zu %-14s %10.1f %10.2f %12.2f\n",
                   c / 1024, wt ? "WRITE_THROUGH" : "flush-at-end",
                   ms, (double)TOTAL_MB * 1000.0 / ms, ms / (double)n);
            fflush(stdout);
        }
    }
    return 0;
}
