/* perf_counters.h — compile-time-optional engine counters.
 *
 * WHY THIS EXISTS, and why it is a build flag rather than a runtime one.
 *
 * The open question is where invf-import's time actually goes. Measured:
 * Debian 7,509 files in ~850s = 8.8 files/s; Arch 34,209 files did not finish
 * in 90 minutes = <6.3 files/s. Until now every explanation was a guess:
 *
 *   - "7 fsync barriers per file" -- WRONG. blkio.h:27 says writes go through
 *     the page cache with no O_DIRECT/O_SYNC, and invf-import.c:452 calls
 *     vol_flush ONCE at the end. Import is not barrier-bound.
 *   - "the metadata tree is expensive" -- plausible, unmeasured.
 *   - "write amplification" -- plausible, unmeasured.
 *
 * So the counters are always compiled in and always incremented, and the dump
 * is what costs. A getenv() in the hot path would be a measurable tax; a
 * relaxed counter add is not. The alternative -- an env-gated build -- means
 * the binary you measure is not the binary you ship, which is exactly the trap
 * this whole exercise exists to avoid.
 *
 * NOT called INVFS_PROFILE: that name is taken, in volume.c:1298, by the WP16b
 * codec profile (fast|balanced|dense|archive) which selects the sweep's ZSTD
 * level. Reusing it would have silently changed compression behaviour.
 *
 * Build:   make EXTRA_CFLAGS=-DPERF_PROFILING     (or CFLAGS += -DPERF_PROFILING)
 * Use:     INVFS_PERF_DUMP=1 <cmd>   dump at exit
 *          kill -USR1 <pid>          dump while running
 * Always:  INVFS_PERF_DUMP_PATH=<f>  append to a file instead of stderr
 *
 * Cost when compiled out: the macros expand to nothing, and the counters are
 * not in the binary at all.
 */
#ifndef INVFS_PERF_COUNTERS_H
#define INVFS_PERF_COUNTERS_H

#include <stdint.h>

#ifdef PERF_PROFILING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>   /* getpid(), used by invfs_perf_dump */

/* Declared here because the SIGUSR1 install macro below names it before
 * perf_counters.c defines it; without this, C11 rejects the use. */
void invfs_perf_sigusr1(int sig);

/* Named so a dump is readable without cross-referencing this file. Order is
 * the print order. */
enum {
    PERF_VOL_WRITE_CALLS,      /* vol_write() entries                    */
    PERF_VOL_WRITE_BYTES,      /* payload bytes handed to vol_write()    */
    PERF_BLKIO_WRITES,         /* blkio write() calls reaching storage   */
    PERF_BLKIO_WRITE_BYTES,    /* bytes actually issued to the device    */
    PERF_READ_CALLS,           /* vol_read() entries                     */
    PERF_READ_BYTES,           /* payload bytes returned                 */
    PERF_BLKIO_READS,
    PERF_BLKIO_READ_BYTES,
    PERF_METABUF_TOUCHES,      /* metadata page reads (cache or disk)    */
    PERF_METABUF_MISSES,       /* ...of those, a miss                     */
    PERF_BTREE_SPLITS,         /* B+ tree node splits                    */
    PERF_BTREE_REBALANCES,
    PERF_CHECKSUM_OPS,
    PERF_VOL_FLUSH_CALLS,      /* vol_flush() entries                    */
    PERF_VOL_SYNC_CALLS,       /* vol_sync() entries                     */
    PERF_DEVT_WRITES,          /* vol_write_devt() -- descriptor rewrite */
    PERF_SB_WRITES,            /* vol_write_sb()                         */
    PERF_MARK_DIRTY_CALLS,
    PERF_VMUX_WRITES,        /* io_write(): the real storage layer         */
    PERF_VMUX_WRITE_BYTES,
    PERF_VMUX_READS,
    PERF_VMUX_READ_BYTES,
    PERF_FSYNC_CALLS,        /* fsync(2) -- THE one that matters            */
    PERF_FSYNC_ERR,
    PERF_FSYNC_DEFERRED,   /* flushes skipped by the commit policy */
    /* ---- WP-IO-LOCK: what the background writers ask of g_io_lock ------ */
    /* The sweep's per-candidate name lookup was one whole reverse walk each
     * until it was answered from a snapshot; NAME_OF_CALLS is the proof that
     * the fallback is not being taken in production (the fast path resolves
     * every candidate without one). */
    PERF_NAME_OF_CALLS,      /* reverse name walks (vol_name_of)            */
    PERF_NAME_SNAPSHOTS,     /* id -> path snapshots built                  */
    /* A recipe publish stales the reference map; the next free gate used to
     * rebuild it in place (a full walk) inside the caller's g_io_lock. These
     * three are the measurement that decides the fix: how often a publish
     * does it, and how long one rebuild costs. */
    PERF_RECIPE_PUBS,        /* row publishes with a recipe_addr change     */
    PERF_PBAREF_REBUILDS,    /* full reference-map rebuilds                 */
    PERF_PBAREF_REBUILD_NS,  /* time spent walking for them                 */
    /* The sweep's own hold: time inside vol_sweep_one per candidate, and the
     * worst single file (the hold the foreground pays). */
    PERF_SWEEP_FILE_CALLS,
    PERF_SWEEP_FILE_NS,
    PERF_SWEEP_FILE_MAX_NS,
    PERF_MAX
};

extern uint64_t invfs_perf_counters[PERF_MAX];
extern int       invfs_perf_enabled;   /* set when dumping is wanted */

void invfs_perf_dump(const char *why);

/* Relaxed ordering: a counter that is off by a few under concurrency is still
 * a measurement; a fence on every file write is not. */
#define INVFS_PERF_ADD(idx, n)                                            \
    __atomic_fetch_add(&invfs_perf_counters[(idx)], (uint64_t)(n),        \
                       __ATOMIC_RELAXED)

/* High-water mark. Same relaxed ordering; the compare-exchange loop keeps the
 * largest value ever offered, so "worst single hold" needs no lock. */
#define INVFS_PERF_MAX(idx, n)                                            \
    do {                                                                  \
        uint64_t _pn = (uint64_t)(n);                                     \
        uint64_t _pc = __atomic_load_n(&invfs_perf_counters[(idx)],        \
                                       __ATOMIC_RELAXED);                 \
        while (_pn > _pc &&                                               \
               !__atomic_compare_exchange_n(&invfs_perf_counters[(idx)],   \
                                            &_pc, _pn, 0,                  \
                                            __ATOMIC_RELAXED,              \
                                            __ATOMIC_RELAXED)) { }        \
    } while (0)

/* Monotonic nanoseconds, for the duration counters. Compiles to 0 when the
 * build has no counters -- and every consumer multiplies it away. */
static inline uint64_t invfs_perf_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#define INVFS_PERF_NOW_NS() invfs_perf_now_ns()

/* atexit, not just the signal: a tool that runs to completion and exits is the
 * common case, and asking for SIGUSR1 to catch a process that has already
 * finished is not an option. Registered once so vol_open being called more than
 * once per process cannot stack handlers.
 *
 * atexit takes void(*)(void), so the wrapper carries no argument and names its
 * own reason -- passing invfs_perf_dump_once(const char*) straight through is an
 * incompatible-pointer-type error, which is what it was on the first attempt.
 *
 * PROTOTYPE ONLY. Defining it here is a link error: volume.c and blkio.c both
 * include this header, so a body in the header gives every translation unit its
 * own copy and the link fails with a multiple definition. The body lives in
 * perf_counters.c. Same reason invfs_perf_dump and invfs_perf_sigusr1 are only
 * declared here. */
void invfs_perf_dump_atexit(void);

#define INVFS_PERF_ENABLE_WHEN_REQUESTED()                                \
    do {                                                                  \
        static int _invfs_perf_installed;                                 \
        if (!_invfs_perf_installed && getenv("INVFS_PERF_DUMP")) {         \
            _invfs_perf_installed = 1;                                     \
            invfs_perf_enabled = 1;                                        \
            signal(SIGUSR1, invfs_perf_sigusr1);                          \
            atexit(invfs_perf_dump_atexit);                                \
        }                                                                 \
    } while (0)

#else /* !PERF_PROFILING -- the shipped default */

/* The arguments are still EVALUATED (and folded away), so a call site can time
 * a span and pass the result in without a set-but-unused warning in the
 * shipped build. Nothing they name has a side effect. */
#define INVFS_PERF_ADD(idx, n) ((void)(n))
#define INVFS_PERF_MAX(idx, n) ((void)(n))
#define INVFS_PERF_NOW_NS() ((uint64_t)0)
#define INVFS_PERF_ENABLE_WHEN_REQUESTED() ((void)0)

#endif /* PERF_PROFILING */

#endif /* INVFS_PERF_COUNTERS_H */
