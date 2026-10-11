/*
 * groupcommit_test.c -- the regression test for delta-barrier group commit.
 *
 * WHAT IS BEING TESTED
 *
 *   vol_delta_append barriers after every record (vol_delta.c:572), so each
 *   metadata mutation costs one physical fsync. Measured on a 500-file /
 *   12-byte import that was 3509 fsyncs -- 7.02 per file, 387.7 s of a
 *   390.0 s wall, 99.4% of the time blocked in fsync -- which scales to
 *   18.2 hours of metadata flush for an 84,279-file rootfs. The fix keeps
 *   the per-CALL guarantee and collapses the per-RECORD flush: many
 *   appends share one fsync, and a barrier that finds nothing new to make
 *   durable costs no flush at all.
 *
 * HOW THE FLUSHES ARE COUNTED
 *
 *   This file DEFINES fsync(). blkio.o's call to it is an undefined symbol
 *   resolved inside this same executable, and a definition in the executable
 *   wins over libc.so -- the same mechanism that lets a program interpose on
 *   its own malloc. So the count is exact, in-process, and needs no
 *   LD_PRELOAD, no ptrace, and no timing. (The wall-clock benchmark needs
 *   LD_PRELOAD because the importer is a separate process; a property test
 *   should not be at the mercy of the machine's load.)
 *
 * THE RED
 *
 *   T1 is the regression: N barriers over one write must cost ONE fsync.
 *   Before the fix the count is exactly N, and the assertion fails for every
 *   N > 1. It is a property test -- the bound is a constant, not a function
 *   of N -- so it stays meaningful as N grows, and T1 sweeps N over two
 *   orders of magnitude to say so out loud.
 *
 * THE OTHER HALF
 *
 *   T2-T4 are the durability side, and they are the reason the skip in T1 is
 *   allowed to exist at all. A collapsed flush is only legitimate if the
 *   guarantee it inherits is real, so these pin: a skipped barrier still
 *   leaves the earlier write on stable storage; a barrier that covers two
 *   writes makes BOTH durable (the ordering rules in vol_crash.c depend on
 *   it); and a FAILED flush advances nothing, so the next barrier retries
 *   instead of reporting a durability it never achieved.
 *
 * WP602 -- THE DELTA LAYER (T8-T10 + the gc-crash-write/verify modes).
 *
 * T1-T7 collapse flushes that have nothing new between them, but every
 * vol_delta_append used to fence its own record, so a sweep batch member
 * (recipe publish + row put + stamp) still paid ~3 fsyncs. WP602 stages a
 * bounded group of appends behind one barrier instead: T8 is the red (N
 * appends cost ceil(N/max) + the one segment rollover, not N), T9 proves
 * the no-op flag (INVFS_DELTA_GROUP_MAX=1 restores the per-record fence
 * exactly), T10 proves the byte cap binds independently of the count cap,
 * and every leg reopens the volume to prove the ack is still honest -- a
 * drained group replays present+exact. The kill -9 half (a staged tail is
 * complete-or-absent, never torn) is tools/test-delta-groupcommit-crash.sh
 * driving the gc-crash-write / gc-crash-verify modes below, the leg7
 * assertion shape.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <dlfcn.h>

#include "blkio.h"
#include "volume_internal.h"   /* pulls in volume.h + invarifs.h */
#include "vol_delta.h"

/* ---- the interposed fsync ------------------------------------------ */

static long   g_fsync_calls;
static long   g_fsync_fail_at = -1;   /* fail every call past this index */

int fsync(int fd)
{
    static int (*real_fsync)(int);

    if (!real_fsync)
        real_fsync = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
    g_fsync_calls++;
    if (g_fsync_fail_at >= 0 && g_fsync_calls > g_fsync_fail_at)
        return -1;                      /* EINVAL: the device said no */
    return real_fsync(fd);
}

static void fsync_reset(void) { g_fsync_calls = 0; g_fsync_fail_at = -1; }
static long fsync_count(void) { return g_fsync_calls; }

/* ---- harness -------------------------------------------------------- */

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static void note(const char *what) { printf("  ..    %s\n", what); }

/* A scratch path under the caller-supplied scratch dir. */
static char g_path[512];

static void scratch_path(const char *dir, const char *name)
{
    snprintf(g_path, sizeof g_path, "%s/gc-%s", dir, name);
}

static int scratch_unlink(void)
{
    unlink(g_path);
    return 0;
}

/* Read the first `len` bytes back through a completely independent handle.
 * Anything this sees survived the closing of the writer's handle, so it is
 * on stable storage. The file may legitimately be longer than `len` (T3
 * writes a second record past the first), so compare the prefix. */
static int read_back(const unsigned char *want, size_t len, const char *what)
{
    unsigned char got[256];
    int fd, n;

    if (len > sizeof got) return 0;
    fd = open(g_path, O_RDONLY);
    if (fd < 0) {
        printf("  FAIL  %s: reopen failed\n", what);
        failures++;
        return 0;
    }
    n = (int)read(fd, got, sizeof got);
    close(fd);
    if (n < (int)len || memcmp(got, want, len) != 0) {
        printf("  FAIL  %s: bytes on disk differ from what was written\n",
               what);
        failures++;
        return 0;
    }
    return 1;
}

static void fill(unsigned char *b, size_t n, unsigned char seed)
{
    size_t i;
    for (i = 0; i < n; i++)
        b[i] = (unsigned char)(seed + i * 7u);
}

/* ================================================================== */
/* T1 -- THE REGRESSION: N barriers over one write cost ONE fsync     */
/* ================================================================== */

/* Before the fix every blkio_flush() is an fsync, so this loop costs N.
 * After it, the first barrier flushes and flushed_gen is already at
 * write_gen, so barriers 2..N are provably free. The bound is a constant
 * (1), which is what makes this a property test rather than a benchmark. */
static void t1_repeated_barriers_are_one_flush(const char *dir)
{
    static const int Ns[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256 };
    unsigned char buf[64];
    size_t i;

    scratch_path(dir, "t1");
    scratch_unlink();
    fill(buf, sizeof buf, 0xA5);

    for (i = 0; i < sizeof Ns / sizeof Ns[0]; i++) {
        blkio io;
        int n = Ns[i], j;
        char what[128];

        if (blkio_open(&io, g_path, BLKIO_CREATE) != 0) {
            printf("  FAIL  t1: blkio_open failed\n");
            failures++;
            return;
        }
        fsync_reset();
        blkio_pwrite(&io, 0, buf, sizeof buf);   /* exactly one write */
        for (j = 0; j < n; j++)
            blkio_flush(&io);
        blkio_close(&io);

        snprintf(what, sizeof what,
                 "t1: %d barriers over 1 write cost 1 fsync (got %ld)",
                 n, fsync_count());
        ok(fsync_count() <= 1, what);
        read_back(buf, sizeof buf, "t1: the single write is still durable");
    }
    note("t1: 1..256 barriers over one write, every count must be 1");
}

/* ================================================================== */
/* T2 -- a SKIPPED barrier does not cost the earlier write its cover   */
/* ================================================================== */

/* The skip in T1 is only sound because flushed_gen is a real promise. This
 * writes, barriers, barriers again (the second is skipped), and reads the
 * bytes back from an independent handle. If skipping could ever drop a
 * write, this is where it would show. */
static void t2_skipped_barrier_keeps_the_cover(const char *dir)
{
    blkio io;
    unsigned char buf[64];

    scratch_path(dir, "t2");
    scratch_unlink();
    fill(buf, sizeof buf, 0x3C);

    if (blkio_open(&io, g_path, BLKIO_CREATE) != 0) {
        printf("  FAIL  t2: blkio_open failed\n");
        failures++;
        return;
    }
    blkio_pwrite(&io, 0, buf, sizeof buf);
    blkio_flush(&io);
    blkio_flush(&io);      /* skipped: nothing new since the flush above */
    blkio_flush(&io);      /* skipped again */
    blkio_close(&io);

    read_back(buf, sizeof buf, "t2: skipped barriers did not drop the write");
    note("t2: 1 write + 3 barriers -> data still on disk after the skips");
}

/* ================================================================== */
/* T3 -- one barrier covers EVERY write issued before it               */
/* ================================================================== */

/* The ordering barriers in vol_crash.c rely on this: a caller writes a
 * structure, then writes the thing that names it, then barriers ONCE. Both
 * must be durable, and the relative order they were written in is what makes
 * the recovery rules work. */
static void t3_one_barrier_covers_every_prior_write(const char *dir)
{
    blkio io;
    unsigned char a[64], b[64];

    scratch_path(dir, "t3");
    scratch_unlink();
    fill(a, sizeof a, 0x11);
    fill(b, sizeof b, 0x77);

    if (blkio_open(&io, g_path, BLKIO_CREATE) != 0) {
        printf("  FAIL  t3: blkio_open failed\n");
        failures++;
        return;
    }
    blkio_pwrite(&io, 0, a, sizeof a);
    blkio_pwrite(&io, 64, b, sizeof b);
    fsync_reset();
    ok(blkio_flush(&io) == 0, "t3: one barrier after two writes succeeds");
    blkio_close(&io);

    ok(fsync_count() == 1, "t3: two writes then one barrier cost 1 fsync");
    read_back(a, sizeof a, "t3: first write durable");
    /* the second write lives at offset 64; read the whole file back */
    {
        unsigned char got[128];
        int fd = open(g_path, O_RDONLY);
        int n = fd < 0 ? -1 : (int)read(fd, got, sizeof got);
        if (fd >= 0) close(fd);
        ok(n == 128 && memcmp(got, a, 64) == 0 && memcmp(got + 64, b, 64) == 0,
           "t3: BOTH writes are on disk after the single barrier");
    }
    note("t3: 2 writes + 1 barrier -> both records present, 1 fsync");
}

/* ================================================================== */
/* T4 -- a FAILED flush advances nothing                               */
/* ================================================================== */

/* If a failed fsync still moved flushed_gen, the next barrier would skip
 * the retry and report a durability that was never achieved -- the exact
 * bug that would make group commit unsafe. So: fail the flush, insist it is
 * reported, then let the next barrier through and insist it really did
 * re-issue an fsync. */
static void t4_failed_flush_is_not_a_flush(const char *dir)
{
    blkio io;
    unsigned char buf[64];

    scratch_path(dir, "t4");
    scratch_unlink();
    fill(buf, sizeof buf, 0x5E);

    if (blkio_open(&io, g_path, BLKIO_CREATE) != 0) {
        printf("  FAIL  t4: blkio_open failed\n");
        failures++;
        return;
    }
    blkio_pwrite(&io, 0, buf, sizeof buf);
    fsync_reset();
    g_fsync_fail_at = 0;                      /* the next fsync fails */
    ok(blkio_flush(&io) != 0, "t4: a failed flush is reported to the caller");

    fsync_reset();
    ok(blkio_flush(&io) == 0, "t4: the next barrier succeeds");
    ok(fsync_count() == 1,
       "t4: the retry after a failure really re-issues an fsync");
    blkio_close(&io);

    read_back(buf, sizeof buf, "t4: the retried barrier made the write durable");
    note("t4: failed flush -> error returned, no advance, retry really fsyncs");
}

/* ================================================================== */
/* T6 -- a RESIZE is not skippable                                      */
/* ================================================================== */

/* ftruncate changes the inode, and it is fsync -- not the write path --
 * that makes the new length survive a crash. Before group commit every
 * barrier flushed, so "resize then barrier" was persisted as a matter of
 * course. A watermark that counted only blkio_pwrite would let that barrier
 * be skipped and a resized image could come back shorter than it was left,
 * which is silent corruption of the volume's geometry. */
static void t6_resize_is_counted(const char *dir)
{
    blkio io;
    unsigned char buf[64];

    scratch_path(dir, "t6");
    scratch_unlink();
    fill(buf, sizeof buf, 0x2B);

    if (blkio_open(&io, g_path, BLKIO_CREATE) != 0) {
        printf("  FAIL  t6: blkio_open failed\n");
        failures++;
        return;
    }
    blkio_pwrite(&io, 0, buf, sizeof buf);
    blkio_flush(&io);
    fsync_reset();

    /* grow the image with no write at all, then ask for durability */
    if (blkio_chsize(&io, 8192) != 0) {
        printf("  FAIL  t6: blkio_chsize failed\n");
        failures++;
        blkio_close(&io);
        return;
    }
    blkio_flush(&io);
    blkio_close(&io);

    ok(fsync_count() == 1,
       "t6: a barrier after a resize really issues an fsync");
    {
        struct stat st;
        ok(stat(g_path, &st) == 0 && st.st_size == 8192,
           "t6: the resized length is on disk");
    }
    note("t6: chsize + barrier -> 1 fsync, length persisted");
}

/* ================================================================== */
/* T7 -- a skip is never allowed to hide a broken descriptor            */
/* ================================================================== */

/* vol_close and vol_sync keep the volume DIRTY instead of writing CLEAN
 * precisely because blkio_flush reports a failing barrier (blkio_test.c
 * pins that). Group commit must not turn "there is nothing new to flush"
 * into a blanket success: with the descriptor gone there is no completed
 * flush behind the call for anyone to rely on, and returning 0 would turn
 * a broken handle into a silent durability lie. */
static void t7_broken_fd_still_reports_failure(const char *dir)
{
    blkio io;
    unsigned char buf[64];

    scratch_path(dir, "t7");
    scratch_unlink();
    fill(buf, sizeof buf, 0x6D);

    if (blkio_open(&io, g_path, BLKIO_CREATE) != 0) {
        printf("  FAIL  t7: blkio_open failed\n");
        failures++;
        return;
    }
    blkio_pwrite(&io, 0, buf, sizeof buf);
    ok(blkio_flush(&io) == 0, "t7: barrier after a write succeeds");
    fsync_reset();

    /* nothing new to flush, and the descriptor is gone */
    close(io.fd);
    io.fd = -1;
    ok(blkio_flush(&io) != 0,
       "t7: a barrier on a broken fd still reports failure");
    blkio_close(&io);
    note("t7: skip path refused for a handle with no descriptor");
}

/* ================================================================== */
/* WP602 T8-T10 -- the delta layer stages bounded groups behind one fence */
/* ================================================================== */

/* mkfs a scratch v3 image through the built tool (the spn_walk_tear_test
 * shape) and open it. Prints a FAIL and returns NULL on setup failure. */
static invfs_volume *t8_mkfs_open(const char *dir, const char *imgname)
{
    static char img[1024];
    char cmd[2048];
    const char *root;
    invfs_volume *v;
    int err = 0;

    snprintf(img, sizeof img, "%s/%s", dir, imgname);
    unlink(img);
    root = getenv("PWD") ? getenv("PWD") : ".";
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 1 2>/dev/null",
             root, img);
    if (system(cmd) != 0) {
        printf("  FAIL  t8-setup: cannot create volume with invf-mkfs\n");
        failures++;
        return NULL;
    }
    v = vol_open(img, &err);
    if (!v) {
        printf("  FAIL  t8-setup: vol_open failed: err=%d\n", err);
        failures++;
        return NULL;
    }
    return v;
}

static void t8_key(int i, char *out, size_t cap)
{
    snprintf(out, cap, "gc%05d", i);
}
static void t8_val(int i, char *out, size_t cap)
{
    snprintf(out, cap, "val-%05d-0123456789", i);
}

/* Append n records; returns -1 on the first failing append. */
static int t8_append_n(invfs_volume *v, int from, int n)
{
    char key[32], val[48];
    int i;

    for (i = 0; i < n; i++) {
        t8_key(from + i, key, sizeof key);
        t8_val(from + i, val, sizeof val);
        if (vol_delta_append(v, (const uint8_t *)key,
                             (uint16_t)strlen(key),
                             (const uint8_t *)val,
                             (uint16_t)strlen(val), 0) != 0)
            return -1;
    }
    return 0;
}

/* Every record in [from, from+n) reads back exact through a fresh lookup. */
static int t8_all_exact(invfs_volume *v, int from, int n, const char *what)
{
    char key[32], val[48];
    uint8_t got[80];
    uint16_t flags = 0, vlen = 0;
    int i;

    for (i = 0; i < n; i++) {
        t8_key(from + i, key, sizeof key);
        t8_val(from + i, val, sizeof val);
        memset(got, 0, sizeof got);
        flags = 0;
        vlen = 0;
        if (vol_delta_lookup_value(v, (const uint8_t *)key,
                                   (uint16_t)strlen(key),
                                   got, sizeof got, &flags, &vlen) != 1 ||
            (flags & INVFS_DELTA_FLAG_DELETE) ||
            vlen != strlen(val) || memcmp(got, val, vlen) != 0) {
            printf("  FAIL  %s: key %s not present+exact\n", what, key);
            failures++;
            return 0;
        }
    }
    return 1;
}

/* T8 -- THE WP602 RED: N delta appends cost groups, not records.
 *
 * With the default count cap (64) and 200 small records on a fresh volume:
 * the first append rolls a segment (header + publish: 2 fsyncs), then the
 * groups close at 64/128/192 (3 fsyncs) and 8 records stay staged. The
 * bound is exact (5) because the run is single-threaded and nothing else
 * writes. Before the fix the count was exactly 202. */
static void t8_delta_appends_share_one_fence(const char *dir)
{
    invfs_volume *v;
    char what[160];

    unsetenv("INVFS_DELTA_GROUP_MAX");
    unsetenv("INVFS_DELTA_GROUP_BYTES");
    v = t8_mkfs_open(dir, "gc-t8.img");
    if (!v)
        return;
    fsync_reset();
    ok(t8_append_n(v, 0, 200) == 0, "t8: 200 appends succeed");
    snprintf(what, sizeof what,
             "t8: 200 appends cost 5 fsyncs (2 rollover + 3 groups), got %ld",
             fsync_count());
    ok(fsync_count() == 5, what);
    snprintf(what, sizeof what,
             "t8: 8 records staged after 192 fenced (pending=%llu)",
             (unsigned long long)vol_delta_pending(v));
    ok(vol_delta_pending(v) == 8, what);
    ok(t8_all_exact(v, 0, 200, "t8: staged reads"),
       "t8: the session reads its own staged writes");
    fsync_reset();
    ok(vol_delta_drain(v) == 0, "t8: drain succeeds");
    snprintf(what, sizeof what,
             "t8: draining 8 staged records costs 1 fsync (got %ld)",
             fsync_count());
    ok(fsync_count() == 1, what);
    ok(vol_delta_pending(v) == 0, "t8: the group is closed after drain");
    fsync_reset();
    ok(vol_delta_drain(v) == 0, "t8: draining an empty group succeeds");
    ok(fsync_count() == 0, "t8: draining an empty group costs no fsync");
    vol_close(v);
    /* Reopen: the drained group replays present+exact -- the ack was
     * honest. vol_close/vol_open are outside the counted region. */
    {
        char img[1024];
        int err = 0;
        snprintf(img, sizeof img, "%s/gc-t8.img", dir);
        v = vol_open(img, &err);
        ok(v != NULL, "t8: reopen succeeds");
        if (v) {
            ok(t8_all_exact(v, 0, 200, "t8: reopen"),
               "t8: the drained group replays present+exact");
            vol_close(v);
        }
    }
    printf("  ..    t8: 200 records -> 6 fsyncs total (0.03/record, legacy 1.0)\n");
}

/* T9 -- the no-op flag: GROUP_MAX=1 restores the per-record fence exactly.
 *
 * Same 40 records on a fresh volume: 2 rollover + 40 record fences = 42
 * physical fsyncs, pending 0 after every append, and the reopened content
 * identical to a batched run's -- the batching is the only delta. */
static void t9_group_size_1_is_legacy(const char *dir)
{
    invfs_volume *v;
    char what[160];
    int i;
    char key[32], val[48];

    setenv("INVFS_DELTA_GROUP_MAX", "1", 1);
    unsetenv("INVFS_DELTA_GROUP_BYTES");
    v = t8_mkfs_open(dir, "gc-t9.img");
    if (!v) {
        unsetenv("INVFS_DELTA_GROUP_MAX");
        return;
    }
    fsync_reset();
    for (i = 0; i < 40; i++) {
        t8_key(i, key, sizeof key);
        t8_val(i, val, sizeof val);
        if (vol_delta_append(v, (const uint8_t *)key,
                             (uint16_t)strlen(key),
                             (const uint8_t *)val,
                             (uint16_t)strlen(val), 0) != 0) {
            printf("  FAIL  t9: append %d failed\n", i);
            failures++;
            break;
        }
        if (vol_delta_pending(v) != 0) {
            printf("  FAIL  t9: pending != 0 after fenced append %d\n", i);
            failures++;
            break;
        }
    }
    snprintf(what, sizeof what,
             "t9: 40 fenced appends cost 42 fsyncs (2 rollover + 40), got %ld",
             fsync_count());
    ok(fsync_count() == 42, what);
    fsync_reset();
    ok(vol_delta_drain(v) == 0, "t9: drain succeeds");
    ok(fsync_count() == 0, "t9: drain after fenced appends costs no fsync");
    vol_close(v);
    {
        char img[1024];
        int err = 0;
        snprintf(img, sizeof img, "%s/gc-t9.img", dir);
        v = vol_open(img, &err);
        ok(v != NULL, "t9: reopen succeeds");
        if (v) {
            ok(t8_all_exact(v, 0, 40, "t9: reopen"),
               "t9: group-size-1 content replays present+exact");
            vol_close(v);
        }
    }
    unsetenv("INVFS_DELTA_GROUP_MAX");
    note("t9: GROUP_MAX=1 -- every append fenced, counts match legacy");
}

/* T10 -- the byte cap binds independently of the count cap.
 *
 * Count cap huge (never fires for 200 records), byte cap 1024: each record
 * is ~31 bytes, so the group closes roughly every 33 records -- several
 * fences, well under the 200 a per-record fence would cost and well over
 * the 1 a pure count cap of this size would. Range, not exact: the byte
 * count is record framing, not a test constant. */
static void t10_byte_cap_binds_first(const char *dir)
{
    invfs_volume *v;
    char what[160];

    setenv("INVFS_DELTA_GROUP_MAX", "1000000", 1);
    setenv("INVFS_DELTA_GROUP_BYTES", "1024", 1);
    v = t8_mkfs_open(dir, "gc-t10.img");
    if (!v) {
        unsetenv("INVFS_DELTA_GROUP_MAX");
        unsetenv("INVFS_DELTA_GROUP_BYTES");
        return;
    }
    fsync_reset();
    ok(t8_append_n(v, 0, 200) == 0, "t10: 200 appends succeed");
    snprintf(what, sizeof what,
             "t10: byte-capped run costs 3..40 fsyncs (got %ld)",
             fsync_count());
    ok(fsync_count() >= 3 && fsync_count() <= 40, what);
    ok(t8_all_exact(v, 0, 200, "t10: staged reads"),
       "t10: the session reads its own staged writes");
    ok(vol_delta_drain(v) == 0, "t10: drain succeeds");
    vol_close(v);
    {
        char img[1024];
        int err = 0;
        snprintf(img, sizeof img, "%s/gc-t10.img", dir);
        v = vol_open(img, &err);
        ok(v != NULL, "t10: reopen succeeds");
        if (v) {
            ok(t8_all_exact(v, 0, 200, "t10: reopen"),
               "t10: the byte-capped groups replay present+exact");
            vol_close(v);
        }
    }
    unsetenv("INVFS_DELTA_GROUP_MAX");
    unsetenv("INVFS_DELTA_GROUP_BYTES");
    note("t10: GROUP_BYTES=1024 with a huge count cap -- bytes close groups");
}

/* ================================================================== */
/* WP602 crash-probe modes (driven by tools/test-delta-groupcommit-crash.sh) */
/* ================================================================== */

/* gc-crash-write <img> <n_acked> <m_unacked>: append n, drain (the group
 * ack), append m more, print STAGED and sleep until killed. Never closes:
 * the kill -9 is the power cut, and the superblock on disk stays DIRTY. */
static int gc_crash_write(const char *img, int n, int m)
{
    invfs_volume *v;
    int err = 0;

    unsetenv("INVFS_DELTA_GROUP_MAX");
    unsetenv("INVFS_DELTA_GROUP_BYTES");
    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "gc-crash-write: vol_open failed: err=%d\n", err);
        return 2;
    }
    if (t8_append_n(v, 0, n) != 0) {
        fprintf(stderr, "gc-crash-write: acked appends failed\n");
        return 1;
    }
    if (vol_delta_drain(v) != 0) {
        fprintf(stderr, "gc-crash-write: drain (the ack) failed\n");
        return 1;
    }
    printf("DRAINED %d (pending=%llu)\n", n,
           (unsigned long long)vol_delta_pending(v));
    fflush(stdout);
    if (t8_append_n(v, n, m) != 0) {
        fprintf(stderr, "gc-crash-write: unacked appends failed\n");
        return 1;
    }
    printf("STAGED %d (pending=%llu)\n", m,
           (unsigned long long)vol_delta_pending(v));
    fflush(stdout);
    for (;;)
        pause();
    return 0;
}

/* gc-crash-verify <img> <n> <m>: reopen after the kill. The n acked records
 * must be present+exact; each of the m unacked records must be exact or
 * absent -- present-but-wrong is a torn tail and fails. */
static int gc_crash_verify(const char *img, int n, int m)
{
    invfs_volume *v;
    int err = 0, i, bad = 0, exact = 0, absent = 0;
    char key[32], val[48];
    uint8_t got[80];
    uint16_t flags, vlen;

    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "gc-crash-verify: vol_open failed: err=%d\n", err);
        return 2;
    }
    for (i = 0; i < n; i++) {
        t8_key(i, key, sizeof key);
        t8_val(i, val, sizeof val);
        memset(got, 0, sizeof got);
        flags = 0;
        vlen = 0;
        if (vol_delta_lookup_value(v, (const uint8_t *)key,
                                   (uint16_t)strlen(key),
                                   got, sizeof got, &flags, &vlen) != 1 ||
            (flags & INVFS_DELTA_FLAG_DELETE) ||
            vlen != strlen(val) || memcmp(got, val, vlen) != 0) {
            printf("BAD: acked key %s not present+exact\n", key);
            bad++;
        }
    }
    for (; i < n + m; i++) {
        int rc;
        t8_key(i, key, sizeof key);
        t8_val(i, val, sizeof val);
        memset(got, 0, sizeof got);
        flags = 0;
        vlen = 0;
        rc = vol_delta_lookup_value(v, (const uint8_t *)key,
                                    (uint16_t)strlen(key),
                                    got, sizeof got, &flags, &vlen);
        if (rc == 0) {
            absent++;
        } else if (rc == 1 && !(flags & INVFS_DELTA_FLAG_DELETE) &&
                   vlen == strlen(val) && memcmp(got, val, vlen) == 0) {
            exact++;
        } else {
            printf("BAD: unacked key %s torn (present but wrong)\n", key);
            bad++;
        }
    }
    printf("gc-crash-verify: acked %d/%d exact; unacked: %d exact %d absent %d bad\n",
           n - bad < 0 ? 0 : n - bad, n, exact, absent, bad);
    vol_close(v);
    return bad ? 1 : 0;
}

/* ================================================================== */
/* T5 -- concurrent writers SHARE one flush                           */
/* ================================================================== */

/* The case group commit exists for. T threads each write once and barrier;
 * they all then ask for durability at the same moment. One of them does the
 * physical flush, and the rest ride on it -- each still getting the
 * guarantee, none of them paying for a flush of its own.
 *
 * The bound is T, which is what a build WITHOUT group commit also costs
 * here, so this case is a guard rather than the red: it fails if group
 * commit ever becomes worse than one flush per writer (a lost wakeup, a
 * deadlock-free-but-serialising loop). The measured number is printed, and
 * on a multi-core host it lands in the single digits. */
#define T5_THREADS 8
#define T5_ROUNDS  64

static pthread_barrier_t g_t5_gate;
static pthread_mutex_t   g_t5_lk = PTHREAD_MUTEX_INITIALIZER;
static blkio            *g_t5_io;
static unsigned char     g_t5_buf[T5_THREADS][64];
static long              g_t5_round_flushes[T5_ROUNDS];

static void *t5_worker(void *arg)
{
    long id = (long)arg;
    int r;

    for (r = 0; r < T5_ROUNDS; r++) {
        long before;
        blkio_pwrite(g_t5_io, (uint64_t)id * 64u, g_t5_buf[id],
                     sizeof g_t5_buf[id]);
        pthread_barrier_wait(&g_t5_gate);      /* everyone has written */
        before = fsync_count();
        blkio_flush(g_t5_io);
        pthread_mutex_lock(&g_t5_lk);
        g_t5_round_flushes[r] = fsync_count() - before;
        pthread_mutex_unlock(&g_t5_lk);
        pthread_barrier_wait(&g_t5_gate);      /* flush done; next round */
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    pthread_t th[T5_THREADS];
    blkio t5io;
    long total, i, worst = 0;
    int t;

    /* Crash-probe driver modes (no dir needed, no other legs run). */
    if (argc > 1 && strcmp(argv[1], "gc-crash-write") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: %s gc-crash-write <img> <n_acked> <m_unacked>\n",
                    argv[0]);
            return 2;
        }
        return gc_crash_write(argv[2], atoi(argv[3]), atoi(argv[4]));
    }
    if (argc > 1 && strcmp(argv[1], "gc-crash-verify") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: %s gc-crash-verify <img> <n> <m>\n",
                    argv[0]);
            return 2;
        }
        return gc_crash_verify(argv[2], atoi(argv[3]), atoi(argv[4]));
    }

    printf("groupcommit_test: group-commit durability + flush-count tests\n");

    t1_repeated_barriers_are_one_flush(dir);
    t2_skipped_barrier_keeps_the_cover(dir);
    t3_one_barrier_covers_every_prior_write(dir);
    t4_failed_flush_is_not_a_flush(dir);
    t6_resize_is_counted(dir);
    t7_broken_fd_still_reports_failure(dir);
    t8_delta_appends_share_one_fence(dir);
    t9_group_size_1_is_legacy(dir);
    t10_byte_cap_binds_first(dir);

    /* T5 */
    scratch_path(dir, "t5");
    scratch_unlink();
    for (i = 0; i < T5_THREADS; i++)
        fill(g_t5_buf[i], sizeof g_t5_buf[i], (unsigned char)(0x10 * i));
    {
        if (blkio_open(&t5io, g_path, BLKIO_CREATE) != 0) {
            printf("  FAIL  t5: blkio_open failed\n");
            failures++;
            goto done;
        }
        g_t5_io = &t5io;
    }
    pthread_barrier_init(&g_t5_gate, NULL, T5_THREADS);
    fsync_reset();
    for (t = 0; t < T5_THREADS; t++)
        pthread_create(&th[t], NULL, t5_worker, (void *)(long)t);
    for (t = 0; t < T5_THREADS; t++)
        pthread_join(th[t], NULL);
    total = fsync_count();
    for (i = 0; i < T5_ROUNDS; i++)
        if (g_t5_round_flushes[i] > worst)
            worst = g_t5_round_flushes[i];

    printf("  ..    t5: %d threads x %d rounds = %d write+barrier pairs "
           "-> %ld fsyncs (worst round: %ld)\n",
           T5_THREADS, T5_ROUNDS, T5_THREADS * T5_ROUNDS, total, worst);
    ok(total <= (long)T5_THREADS * T5_ROUNDS,
       "t5: never more flushes than one per write+barrier pair");
    /* The load-bearing one: the writers that raced into the same flush
     * window did not each pay for it. On a single-core host every round
     * degenerates to T5_THREADS, so only assert the strict form when the
     * host actually gave the threads cores to run on. */
    {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        if (cores > 1)
            ok(total < (long)T5_THREADS * T5_ROUNDS / 2,
               "t5: concurrent barriers share flushes (well under one each)");
        else
            note("t5: single CPU, strict sharing bound not asserted");
    }
    blkio_close(g_t5_io);
    pthread_barrier_destroy(&g_t5_gate);

done:
    printf("groupcommit_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
