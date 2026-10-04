/* perf_counters.c — implementation for the PERF_PROFILING build.
 *
 * Compiled only when -DPERF_PROFILING is set; the Makefile links this object
 * into every invf-* binary under that flag and omits it otherwise.
 */
#include "perf_counters.h"

#ifdef PERF_PROFILING

uint64_t invfs_perf_counters[PERF_MAX];
int       invfs_perf_enabled;

static const char *const names[PERF_MAX] = {
    "vol_write calls",      "vol_write bytes",
    "blkio writes",         "blkio write bytes",
    "vol_read calls",       "vol_read bytes",
    "blkio reads",          "blkio read bytes",
    "metabuf touches",      "metabuf misses",
    "btree splits",         "btree rebalances",
    "checksum ops",         "vol_flush calls",
    "vol_sync calls",       "DEVT writes",
    "superblock writes",    "mark_dirty calls",
    "vmux writes",          "vmux write bytes",
    "vmux reads",           "vmux read bytes",
    "fsync calls",          "fsync errors",
};

/* Ratios are the point of this file. The raw counts are only interesting next
 * to each other: 8.8 files/s means nothing until you know whether there were
 * 8.8 DEVT rewrites per file or 0.08. */
void invfs_perf_dump(const char *why)
{
    static FILE *out;
    int i;

    if (!out) {
        const char *p = getenv("INVFS_PERF_DUMP_PATH");
        out = (p && *p) ? fopen(p, "a") : stderr;
        if (!out) out = stderr;
    }
    if (out != stderr) {
        fprintf(out, "=== invfs perf: %s (pid %d)\n", why, (int)getpid());
    }

    for (i = 0; i < PERF_MAX; i++)
        fprintf(out, "  %-22s %llu\n", names[i],
                (unsigned long long)invfs_perf_counters[i]);

    /* Derived, because these are the ratios worth arguing about. */
    {
        uint64_t f = invfs_perf_counters[PERF_VOL_FLUSH_CALLS];
        uint64_t s = invfs_perf_counters[PERF_VOL_SYNC_CALLS];
        uint64_t w = invfs_perf_counters[PERF_VOL_WRITE_CALLS];
        uint64_t t = invfs_perf_counters[PERF_METABUF_TOUCHES];
        uint64_t m = invfs_perf_counters[PERF_METABUF_MISSES];
        uint64_t bw = invfs_perf_counters[PERF_BLKIO_WRITE_BYTES];
        uint64_t pw = invfs_perf_counters[PERF_VOL_WRITE_BYTES];

        fprintf(out, "  --- derived ---\n");
        if (f) fprintf(out, "  DEVT+metadata flushes per vol_write : %.4f\n",
                       (double)invfs_perf_counters[PERF_DEVT_WRITES] / (double)f);
        if (f) fprintf(out, "  vol_sync calls     per vol_flush    : %.4f\n",
                       (double)s / (double)f);
        if (t) fprintf(out, "  metabuf miss rate                  : %.2f%%\n",
                       100.0 * (double)m / (double)t);
        if (pw) fprintf(out, "  write amplification (issued/asked): %.4f\n",
                       (double)bw / (double)pw);
        {
            uint64_t vw = invfs_perf_counters[PERF_VMUX_WRITE_BYTES];
            uint64_t vr = invfs_perf_counters[PERF_VMUX_READ_BYTES];
            if (vw) fprintf(out, "  vmux write amplification vs vol_write: %.4f\n",
                            (double)vw / (double)(pw ? pw : 1));
            if (vr) fprintf(out, "  read/write ratio at the volume layer: %.4f\n",
                            (double)vr / (double)(vw ? vw : 1));
            if (f && pw) {
                /* fsyncs per vol_flush AND per file are the numbers that decide
                 * whether a --no-sync install mode is worth building. */
                fprintf(out, "  fsyncs per file                  : %.2f\n",
                        (double)invfs_perf_counters[PERF_FSYNC_CALLS] /
                        (double)(invfs_perf_counters[PERF_VOL_WRITE_CALLS] +
                                 invfs_perf_counters[PERF_READ_CALLS] ?: 1));
                fprintf(out, "  fsyncs / vol_flush               : %.2f\n",
                        (double)invfs_perf_counters[PERF_FSYNC_CALLS] / (double)f);
                fprintf(out, "  fsyncs / vmux write              : %.2f\n",
                        (double)invfs_perf_counters[PERF_FSYNC_CALLS] /
                        (double)(vw ? invfs_perf_counters[PERF_VMUX_WRITES] : 1));
            }
        }
        if (w) fprintf(out, "  avg bytes per vol_write             : %.1f\n",
                       (double)pw / (double)w);
    }
    fflush(out);
}

void invfs_perf_sigusr1(int sig)
{
    (void)sig;
    invfs_perf_dump("SIGUSR1");
}

/* atexit handler: prototype-only in the header (a body there is a multiple
 * definition across volume.o and blkio.o), defined here once. */
void invfs_perf_dump_atexit(void)
{
    invfs_perf_dump("exit");
}

#endif /* PERF_PROFILING */
