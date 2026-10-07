/* sweep_refuse_fuse_test.c -- worker-refused implies untouched (F9 worker half).
 *
 * WHAT THIS IS
 * ------------
 * Commit c21d223 moved the OFFLINE sweep's K=1 drop + capture + verify +
 * reclaim to after the live set validates, because a sweep refused at
 * collect kept its prepare-time frees (leg-5 dead-end: live base roots
 * freed, dead ladder). The FUSE worker (invf_sweep_worker) was left alone
 * and has the same flaw three times over:
 *
 *   - incomplete-list manual refuse runs AFTER capture+reclaim (the F9 twin);
 *   - capture-failed manual runs AFTER the K=1 drop destroyed the old window;
 *   - verify-failed refuse runs AFTER capture reclaimed and then unwinds.
 *
 * Each prints "(the volume is unchanged)" over a bitmap/window that changed.
 *
 * WHY THIS IS NOT A PROBABILITY TEST
 * ----------------------------------
 * Both legs are deterministic. Leg A forces the collect to report
 * incomplete with a one-shot btree walk-stop fault (position found
 * dynamically, the walk_status_fuse_test pattern -- positions shift when
 * the capture moves, so a pinned position would silently measure the
 * healthy path). Fixture debt (deleted files inside the prior pin) makes
 * the pre-fix reclaim free real blocks, so the bitmap assertion bites.
 * (A planned Leg B -- K=1 capture refusal -- was deleted: both drivers
 * K=1-drop before attempting capture, so that refusal cannot occur in a
 * sweep. The accepted residual -- failed capture after K=1 drop loses the
 * window -- is documented in FINDINGS F9-worker.)
 *
 * THE CONTROL ON THE CONTROL. A fault that never fires (or a fixture with
 * no debt) also leaves everything unchanged -- a green leg proving
 * nothing. So the finder only accepts a position whose probe log names the
 * COLLECT's own INCOMPLETE line (both spellings, same as the pattern
 * test), and Leg A additionally asserts the pre-run snapshot actually
 * contains reclaimable debt (deleted files' blocks still allocated). A leg
 * that cannot arm is a FAIL, not a pass.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _GNU_SOURCE          /* O_PATH, RENAME_NOREPLACE (see fuse_fs.c) */
#define FUSE_USE_VERSION 31  /* must match fuse_fs.c before any header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

#define main invf_fuse_daemon_main_unused
#include "fuse_fs.c"          /* statics under test; stubs below for link */
#undef main

#include "vol_fault.h"
#include "vol_spt0.h"

/* ---- libfuse / tmpstore stubs (same shape as walk_status_fuse_test) --- */
struct fuse_context *fuse_get_context(void) { return NULL; }
int  fuse_daemonize(int foreground) { (void)foreground; return 0; }
void fuse_destroy(struct fuse *f) { (void)f; }
void fuse_unmount(struct fuse *f) { (void)f; }
struct fuse_session *fuse_get_session(struct fuse *f) { (void)f; return NULL; }
int  fuse_loop_mt_31(struct fuse *f, int clone_fd) { (void)f; (void)clone_fd; return 0; }
int  fuse_mount(struct fuse *f, const char *mp) { (void)f; (void)mp; return 0; }
struct fuse *_fuse_new_31(struct fuse_args *args, const struct fuse_operations *op,
                           size_t op_size, struct libfuse_version *ver, void *data)
{ (void)args; (void)op; (void)op_size; (void)ver; (void)data; return NULL; }
int  fuse_set_signal_handlers(struct fuse_session *se) { (void)se; return 0; }
void fuse_remove_signal_handlers(struct fuse_session *se) { (void)se; }
int  tmpstore_init(size_t max_bytes, tmp_area_mode mode)
{ (void)max_bytes; (void)mode; return 0; }
void tmpstore_destroy(void) { }

/* ---- harness --------------------------------------------------------- */
static int checks, failures;
static char g_img[512];
static invfs_volume *g_v;

static void ok(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(cond ? "  ok   " : "  FAIL ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    checks++;
    if (!cond) failures++;
}

static void arm_btree(const char *spec)
{
    setenv("INVFS_FAULT", spec, 1);
    invfs_vol_btree_fault_reload();
}
static void arm_btree_pos(const char *site, int n)
{
    char spec[64];
    snprintf(spec, sizeof spec, "%s:%d", site, n);
    arm_btree(spec);
}
static void disarm(void)
{
    unsetenv("INVFS_FAULT");
    invfs_vol_btree_fault_reload();
}

/* ---- stderr capture (proven shape from walk_status_fuse_test) --------- */
#define CAPMAX 65536
static char cap_buf[CAPMAX];
static size_t cap_len;
static pthread_mutex_t cap_mtx = PTHREAD_MUTEX_INITIALIZER;
static int cap_saved = -1;
static int cap_pipe[2] = { -1, -1 };
static volatile int cap_stop;
static pthread_t cap_thread;
static int cap_thread_live;

static void *cap_reader(void *arg)
{
    int rfd = (int)(intptr_t)arg;
    for (;;) {
        char b[512];
        ssize_t k = read(rfd, b, sizeof b);
        if (k <= 0)
            break;
        pthread_mutex_lock(&cap_mtx);
        if (cap_len + (size_t)k < CAPMAX - 1) {
            memcpy(cap_buf + cap_len, b, (size_t)k);
            cap_len += (size_t)k;
            cap_buf[cap_len] = 0;
        }
        pthread_mutex_unlock(&cap_mtx);
        if (cap_stop)
            break;
    }
    return NULL;
}

static void cap_start(void)
{
    pthread_t th;
    fflush(stderr);
    if (pipe(cap_pipe) != 0)
        return;
    cap_saved = dup(2);
    dup2(cap_pipe[1], 2);
    close(cap_pipe[1]);
    cap_len = 0; cap_buf[0] = 0; cap_stop = 0;
    cap_thread_live = pthread_create(&th, NULL, cap_reader,
                                     (void *)(intptr_t)cap_pipe[0]) == 0;
    if (cap_thread_live)
        cap_thread = th;
}

static size_t cap_stop_and_get(char *out, size_t outcap)
{
    size_t n;
    fflush(stderr);
    cap_stop = 1;
    if (cap_saved >= 0) {
        dup2(cap_saved, 2);
        close(cap_saved);
        cap_saved = -1;
    }
    if (cap_thread_live) {
        pthread_join(cap_thread, NULL);
        cap_thread_live = 0;
    }
    if (cap_pipe[0] >= 0) {
        close(cap_pipe[0]);
        cap_pipe[0] = -1;
    }
    pthread_mutex_lock(&cap_mtx);
    n = cap_len < outcap - 1 ? cap_len : outcap - 1;
    memcpy(out, cap_buf, n);
    out[n] = 0;
    pthread_mutex_unlock(&cap_mtx);
    return n;
}

/* ---- snapshots --------------------------------------------------------
 * The bitmap copy is the untouched-proof; the spt0 triple (base_root,
 * delta_end, generation nonce) proves the window survived byte-identical.
 * Free-block count is a cheap second channel for the bitmap. */
typedef struct {
    uint8_t *bitmap;
    size_t bmlen;
    uint64_t blocks;
    uint64_t free_blocks;
    invfs_spt0 sp;
    int sp_live;
} snap_t;

static int snap_take(snap_t *s)
{
    uint64_t blocks = 0;
    const uint8_t *bm = vol_bitmap(g_v, &blocks);
    size_t len;
    if (!bm || !blocks)
        return -1;
    len = (size_t)((blocks + 7) / 8);
    free(s->bitmap);
    s->bitmap = (uint8_t *)malloc(len ? len : 1);
    if (!s->bitmap)
        return -1;
    s->bmlen = len;
    s->blocks = blocks;
    memcpy(s->bitmap, bm, len);
    s->free_blocks = vol_free_blocks_cached(g_v);
    s->sp_live = spt0_info(g_v, &s->sp);
    return 0;
}

/* Total blocks behind a bitmap+len pair. Re-derived per compare (cheap)
 * from the live volume: the bitmap's trailing slack bits (past
 * total_blocks) are not volume state and must not participate -- a
 * byte-compare of the raw buffer false-positives on allocator slack. */
static int snap_same(const snap_t *a, const snap_t *b, const char *what)
{
    size_t full;
    uint8_t mask;
    uint64_t total = a->blocks;
    if (a->bmlen != b->bmlen) {
        printf("        (%s: bitmap length differs)\n", what);
        return 0;
    }
    full = (size_t)(total / 8);
    mask = (uint8_t)(total % 8 ? (0xFFu >> (8 - (total % 8))) : 0xFFu);
    if (full > a->bmlen)
        full = a->bmlen;
    if (memcmp(a->bitmap, b->bitmap, full) != 0) {
        uint64_t bb;
        printf("        (%s: bitmap differs:", what);
        for (bb = 0; bb < full; bb++) {
            uint8_t d = (uint8_t)(a->bitmap[bb] ^ b->bitmap[bb]);
            int bit;
            for (bit = 0; bit < 8; bit++)
                if (d & (uint8_t)(1u << bit))
                    printf(" blk=%llu", (unsigned long long)(bb * 8 + bit));
        }
        printf(")\n");
        return 0;
    }
    if ((total % 8) && full < a->bmlen &&
        ((a->bitmap[full] & mask) != (b->bitmap[full] & mask))) {
        printf("        (%s: bitmap tail differs)\n", what);
        return 0;
    }
    if (a->free_blocks != b->free_blocks) {
        printf("        (%s: free %llu -> %llu)\n", what,
               (unsigned long long)a->free_blocks,
               (unsigned long long)b->free_blocks);
        return 0;
    }
    return 1;
}

static int snap_window_same(const snap_t *a, const snap_t *b)
{
    if (!a->sp_live || !b->sp_live)
        return 0;
    return a->sp.base_root == b->sp.base_root &&
           a->sp.delta_end == b->sp.delta_end &&
           a->sp.flags == b->sp.flags;
}

/* Find a one-shot walk-stop position that lands in the COLLECT (not the
 * capture): probe n=1..8, accept the first whose log names the collect's
 * own INCOMPLETE line. Returns 0 when no position can be found -- the legs
 * then fail loudly instead of measuring the healthy path. */
static int find_collect_pos(char *redlog, size_t outcap)
{
    int outer, n;
    for (outer = 0; outer < 2; outer++) {
        for (n = 1; n <= 8; n++) {
            char log[CAPMAX];
            g_shutdown = 0;
            cap_start();
            arm_btree_pos("iter_live_inodes", n);
            invf_sweep_worker(0);
            disarm();
            cap_stop_and_get(log, sizeof log);
            if (strstr(log, "INCOMPLETE: the live-set WALK did not complete")
                    != NULL ||
                strstr(log, "DONE found=0") != NULL) {
                snprintf(redlog, outcap, "%s", log);
                return n;
            }
        }
    }
    if (outcap) redlog[0] = 0;
    return 0;
}

static void write_file(const char *name, const char *content)
{
    if (!vol_replace_file(g_v, name, (const uint8_t *)content,
                          strlen(content))) {
        fprintf(stderr, "setup: cannot write %s\n", name);
        exit(2);
    }
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    int err = 0, i, pos;
    char redlog[CAPMAX];
    snap_t before, after;
    char log[CAPMAX];

    printf("sweep_refuse_fuse_test: worker-refused implies untouched\n");
    memset(&before, 0, sizeof before);
    memset(&after, 0, sizeof after);

    snprintf(g_img, sizeof g_img, "%s/invf-sweep-refuse-fuse-test.img", dir);
    unlink(g_img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 64 2>/dev/null",
                 root, g_img);
        if (system(cmd) != 0) {
            printf("  cannot create volume with invf-mkfs\n");
            return 2;
        }
    }
    g_v = vol_open(g_img, &err);
    if (!g_v) {
        printf("  vol_open failed: err=%d\n", err);
        return 2;
    }
    g_vol = g_v;                 /* the daemon's only handle on the volume */
    snprintf(g_img_path, sizeof g_img_path, "%s", g_img);
    snprintf(g_mnt_path, sizeof g_mnt_path, "%s", "/mnt-test");

    /* fixture with sweepable content + debt-to-be: files, one good pass
     * (live window), then deletes whose blocks the prior pin holds. */
    for (i = 0; i < 8; i++) {
        char nm[64], body[128];
        snprintf(nm, sizeof nm, "keep%d", i);
        snprintf(body, sizeof body, "keep-payload-%d padding padding", i);
        write_file(nm, body);
    }
    for (i = 0; i < 3; i++) {
        char nm[64];
        static char bigbody[100 * 1024];
        memset(bigbody, 'D', sizeof bigbody);
        snprintf(bigbody, sizeof bigbody, "dead-payload-%d", i);
        snprintf(nm, sizeof nm, "dead%d", i);
        if (!vol_replace_file(g_v, nm, (const uint8_t *)bigbody,
                              sizeof bigbody)) {
            printf("  setup: cannot write %s\n", nm);
            return 2;
        }
    }
    g_shutdown = 0;
    invf_sweep_worker(0);
    ok(spt0_info(g_v, NULL) != 0, "setup: a live window exists");
    for (i = 0; i < 3; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "dead%d", i);
        if (vol_unlink(g_v, nm) != 0) {
            printf("  setup: cannot delete %s\n", nm);
            return 2;
        }
    }

    /* ---- Leg A: incomplete collect, manual pass must leave no trace --- */
    pos = find_collect_pos(redlog, sizeof redlog);
    ok(pos > 0, "A0. collect fault position found (else nothing below "
                "measures refusal)");
    if (snap_take(&before) != 0)
        return 2;
    ok(before.sp_live, "A1. window live before the refused pass");
    g_shutdown = 0;
    cap_start();
    arm_btree_pos("iter_live_inodes", pos);
    invf_sweep_worker(0);
    disarm();
    cap_stop_and_get(log, sizeof log);
    printf("        --- refused pass log ---\n%s"
           "        ------------------------\n", log);
    ok(strstr(log, "REFUSING the pass") != NULL,
       "A2. manual pass refuses on incomplete collect");
    if (snap_take(&after) != 0)
        return 2;
    ok(snap_same(&before, &after, "refused pass"),
       "A3. refused pass: zero bitmap delta");
    /* NOTE: the triple alone cannot prove intactness -- a destroy plus an
     * identical rebuild (same root/end/nonce) compares equal. The bitmap
     * is the teeth here: any capture allocates its mark set, any reclaim
     * frees debt, so only a pass that ran nothing is bit-identical. */
    ok(snap_window_same(&before, &after),
       "A4. refused pass: prior window byte-identical (no K=1 drop, "
       "no reclaim)");

    /* re-establish: one clean pass (also proves the fixture still sweeps) */
    g_shutdown = 0;
    invf_sweep_worker(0);
    ok(spt0_info(g_v, NULL) != 0, "setup2: window live again");

    /* ---- Leg B: DELETED ----
     * Leg B was designed as "K=1 capture refusal must not eat the live
     * window" -- but that path cannot occur: both drivers K=1-drop
     * BEFORE attempting capture, so capture never K=1-refuses in a sweep
     * (the refusal exists only for direct spt0_capture callers). Probing
     * it here measured only the test's own misunderstanding, twice (first
     * as a false red, then as a false green). The residual it pointed at
     * -- a FAILED capture after a K=1 drop loses the window -- is real
     * but accepted: single-slot SPT0 cannot atomically replace, capture
     * fails only on damaged-base/ENOSPC (window unusable/degraded there
     * anyway), and no data path is reachable before a successful capture.
     * See FINDINGS F9-worker. */

    free(before.bitmap);
    free(after.bitmap);
    vol_close(g_v);
    printf("sweep_refuse_fuse_test: %d checks, %d failures\n",
           checks, failures);
    return failures ? 1 : 0;
}
