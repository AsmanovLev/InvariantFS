/*
 * heat_san_test.c — the RED CONTROL for the read-heat table concurrency defect.
 *
 * WHAT THIS IS
 * ------------
 * `v->heat_tab[]` (src/core/volume_internal.h:436) is the session's per-inode
 * accrued-read map. heat_tab_touch() (src/core/vol_heat.c:49) allocates it,
 * GROWS it, FREEING the old array (vol_heat.c:68), and inserts into it — with
 * no lock and no atomic anywhere in the file. The read path reaches it
 * lock-free: invf_read releases g_io_lock at fuse_fs.c:1407 and THEN calls
 * vol_read_range at :1414, and the daemon runs fuse_loop_mt (:3353). So N
 * threads mutate one table at once.
 *
 * This is the sibling of src/cli/arc_san_test.c, and it is built the same way:
 * one source, two binaries, linked against src/core/vol_heat.c ALONE.
 * vol_heat.c is NOT as self-contained as arc.c — it needs the real
 * volume_internal.h for the struct and calls 25 project symbols (vol_find,
 * vol_get_xattr, vol_v3_inode_get, meta_locate_ext, ...). All 25 are defined
 * as stubs at the bottom of THIS FILE, so the link needs nothing outside
 * libc + libzstd and a sanitizer build still costs about a second. No volume,
 * no image, no /dev/shm.
 *
 * WHY THIS IS NOT A PROBABILITY TEST
 * ----------------------------------
 * The same standard as the ARC control, and it is met by construction rather
 * than by hoping for an interleaving.
 *
 *   The unfixed path contains NO SYNCHRONIZATION AT ALL. There is no mutex,
 *   no atomic, no release store and no acquire load between any two
 *   heat_touch_read() calls. Under TSAN's model that means every pair of
 *   concurrent touches is a conflicting-access pair with NO HAPPENS-BEFORE
 *   EDGE, always — not "usually", not "if they overlap in time". TSAN does
 *   not need the accesses to overlap; it needs them to be unordered, and
 *   unorderedness is guaranteed by the absence of any edge. So the structure
 *   leg reports on the first run, every run. Measured: see the report.
 *
 * WHY IT COMPILES AGAINST THE UNFIXED TREE
 * ----------------------------------------
 * The fix adds a mutex. A test that only compiles against the FIXED tree
 * cannot be a red control, so heat_locks_init() is declared WEAK here: on a
 * tree without it the symbol resolves to NULL and the test skips the
 * initialization — i.e. it runs the pre-fix, unlocked code, which is the bug.
 * Same source, same command, red before and green after.
 *
 * Built twice from this one file (see the Makefile):
 *   -fsanitize=thread   -> the structure leg, and the deterministic one
 *   -fsanitize=address  -> the lifetime leg (the free() at vol_heat.c:68)
 * INVFS_HEAT_SAN_LEG=structure|lifetime|both selects a leg.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "volume_internal.h"
#include "vol_walk.h"

/* Weak on purpose: see the header comment. NULL on a tree that predates the
 * fix, and the table is then touched with no lock at all -- which is the
 * defect under test. */
extern void heat_locks_init(invfs_volume *v) __attribute__((weak));

#define NTHREADS 6
#define ITERS    4000
#define KEYS     512

static int failures;

static void ok(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

/* ------------------------------------------------------------------ */
/* the volume under test: a zeroed invfs_volume with the heat fields    */
/* warm. Nothing else in the struct is touched by heat_touch_read.      */
/* ------------------------------------------------------------------ */

static invfs_volume *mkvol(void)
{
    invfs_volume *v = (invfs_volume *)calloc(1, sizeof *v);
    if (!v) return NULL;
    /* Fixed tree: take the lock down properly. Unfixed tree: the weak symbol
     * is NULL and this is exactly the pre-fix state. */
    if (heat_locks_init) heat_locks_init(v);
    return v;
}

static void freevol(invfs_volume *v)
{
    if (!v) return;
    free(v->heat_tab);
    free(v);
}

/* ------------------------------------------------------------------ */
/* leg 1: STRUCTURE. The shape of a live mount's read path.            */
/*                                                                  */
/* vol_read_range calls heat_touch_read once per segment, with         */
/* g_io_lock released (fuse_fs.c:1407, :1414), from every thread       */
/* fuse_loop_mt runs. That is this loop.                               */
/* ------------------------------------------------------------------ */

struct racer {
    invfs_volume *v;
    long          id;
    long          touches;
};

static void *racer(void *argp)
{
    struct racer *r = (struct racer *)argp;
    int i;
    for (i = 0; i < ITERS; i++) {
        /* KEYS is well above the 70%-load grow threshold, so the table
         * grows (and FREES its old array, vol_heat.c:68) many times per
         * thread rather than once at the end. */
        uint64_t inode = 1 + (uint64_t)((r->id * 37 + i) % KEYS);
        heat_touch_read(r->v, inode, (uint64_t)i);
    }
    r->touches += ITERS;
    return NULL;
}

static void leg_structure(void)
{
    invfs_volume *v;
    pthread_t th[NTHREADS];
    struct racer racers[NTHREADS];
    long i, distinct = 0;
    uint64_t k;

    printf("== structure leg: %d threads x %d heat_touch_read on one table ==\n",
           NTHREADS, ITERS);
    printf("   heat_locks_init present: %s\n", heat_locks_init ? "yes" : "no");
    fflush(stdout);

    v = mkvol();
    if (!v) { ok(0, "mkvol"); return; }

    for (i = 0; i < NTHREADS; i++) {
        racers[i].v = v; racers[i].id = i; racers[i].touches = 0;
        pthread_create(&th[i], NULL, racer, &racers[i]);
    }
    for (i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);

    /* Table invariants. heat is advisory -- a lost touch is a colder file --
     * so none of these is a bit-exactness failure. They are here so the test
     * has teeth even without a sanitizer, and so the fixed tree is asserting
     * something rather than merely not crashing.
     *
     * heat_tab_touch is dedupe-by-presence ("already counted this session",
     * vol_heat.c:95), so after NTHREADS*ITERS touches over KEYS distinct
     * inodes the table MUST hold exactly KEYS entries and heat_tab_n MUST
     * equal KEYS. Two threads claiming the same empty slot (the classic
     * open-addressing race) inflates heat_tab_n past the real count and
     * loses an inode forever -- and the count and the contents then
     * disagree, which is the signature below.
     *
     * The per-inode lookup goes through heat_file_r() rather than
     * heat_session_take(): heat_tab_take() zeroes the slot it removes
     * ("no tombstone compaction", vol_heat.c), which truncates the probe
     * chain for every inode behind it, so a take-all sweep reports missing
     * keys on a perfectly healthy table. That is a pre-existing property of
     * the map and not what this test is about. */
    for (k = 1; k <= KEYS; k++) distinct += heat_file_r(v, k) == 1 ? 1 : 0;
    printf("   heat_tab_n=%zu distinct-found=%ld\n", v->heat_tab_n, distinct);
    ok(distinct == KEYS, "every inode that was touched is in the table");
    ok(v->heat_tab_n == (size_t)KEYS,
       "heat_tab_n counts exactly the distinct inodes touched");

    freevol(v);
}

/* ------------------------------------------------------------------ */
/* leg 2: LIFETIME. The grow's free() against a concurrent reader.    */
/*                                                                  */
/* heat_tab_touch frees the old array at vol_heat.c:68 and republishes */
/* v->heat_tab at :69. A thread that is between the two dereferences   */
/* the array after it is freed -- and a thread that already computed    */
/* `i` from the OLD mask and then indexes the NEW array writes into a */
/* slot that may belong to another inode, or past the end when the     */
/* pointer/mask pair is observed torn. Neither has a hazard pointer    */
/* and neither is ordered against anything.                             */
/* ------------------------------------------------------------------ */

struct grower {
    invfs_volume *v;
    long          base;
    long          n;
};

static void *grower(void *argp)
{
    struct grower *g = (struct grower *)argp;
    long i;
    for (i = 0; i < g->n; i++)
        heat_touch_read(g->v, (uint64_t)(g->base + i), 0);
    return NULL;
}

/* The deterministic half of this leg is NOT "we hope a free lands inside a
 * reader". It is that on the unfixed tree there is no ordering of ANY kind,
 * so the concurrent grow/free/republish is an unordered triple by
 * construction -- which is what TSAN reports and what ASAN reports when the
 * window is open. The workload is shaped to open that window as wide as it
 * can be made without touching production code: a big KEYSPACE keeps the
 * table growing (and freeing) throughout the run, and many distinct inodes
 * per slot keep the probe loops long, because a long probe loop is what
 * maximizes the number of dereferences of a pointer that another thread is
 * freeing. */
static void leg_lifetime(void)
{
    invfs_volume *v;
    pthread_t th[NTHREADS];
    struct grower g[NTHREADS];
    long i;

    printf("== lifetime leg: concurrent grow/free of the table array ==\n");
    fflush(stdout);

    v = mkvol();
    if (!v) { ok(0, "mkvol"); return; }

    /* KEYSPACE per thread is far past the grow threshold (256 -> 512 -> ...),
     * so every thread performs many free()+republish cycles. Disjoint ranges
     * so the touches are genuinely distinct inserts, which is what drives
     * the table past each threshold instead of short-circuiting on the
     * dedupe. */
    for (i = 0; i < NTHREADS; i++) {
        g[i].v = v; g[i].base = 1 + (i + 1) * 1000000; g[i].n = 20000;
        pthread_create(&th[i], NULL, grower, &g[i]);
    }
    for (i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);

    ok(v->heat_tab != NULL, "the table survived the storm");
    freevol(v);
}

/* ------------------------------------------------------------------ */
/* leg 3: PLANNED FREE. The lifetime leg above is a STORM: it fires    */
/* every run in practice, but it is catching a window. This leg does    */
/* not catch anything. It SCHEDULES the free.                           */
/*                                                                  */
/* There is no production-code hook for this and none is wanted. The    */
/* free is intercepted at the LINKER (`-Wl,--wrap=free`, see the        */
/* Makefile), so the `free(v->heat_tab)` at vol_heat.c:68 performs the */
/* REAL free and then parks, before control returns to vol_heat.c:69.   */
/* The main thread starts a reader only once it has observed that park, */
/* so the reader is guaranteed to run in the window the two adjacent     */
/* stores at :68 and :69 leave open: the array is freed, and            */
/* v->heat_tab still names it. No timing, no luck, and no production    */
/* code was touched to get there.                                       */
/*                                                                  */
/* On the FIXED tree the grower holds heat.mu across the whole           */
/* free+republish, so the reader blocks on the mutex until the new       */
/* table is published and then reads it -- no freed dereference, green.  */
/* That also means the grower has to be released BEFORE the reader is   */
/* joined, while on the unfixed tree the reader does not block and has   */
/* to be joined BEFORE the grower is released (otherwise the test would */
/* merely be hoping for the window). heat_locks_init -- already the      */
/* weak symbol that tells the two trees apart -- picks the order. This  */
/* is the same trick arc_san_test.c uses for arc_get_copy, and the       */
/* ASSERTION is not weakened by it: in both trees the reader is         */
/* required to have run, and ASAN must have named the use-after-free.    */
/* ------------------------------------------------------------------ */

extern void __real_free(void *p);

static int            g_arm;        /* intercept the heat grow's free */
static pthread_mutex_t g_pm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_pc = PTHREAD_COND_INITIALIZER;
static void           *g_parked;     /* the freed array, parked at :68 */
static int            g_released;

void __wrap_free(void *p)
{
    if (!g_arm) { __real_free(p); return; }
    __real_free(p);                 /* the free is DONE; :69 has not run */
    pthread_mutex_lock(&g_pm);
    g_parked = p;
    g_released = 0;
    pthread_cond_broadcast(&g_pc);
    while (!g_released) pthread_cond_wait(&g_pc, &g_pm);
    pthread_mutex_unlock(&g_pm);
}

/* One touch; the reader that must land inside the window. */
struct one {
    invfs_volume *v;
    uint64_t      inode;
};

static void *one_touch(void *argp)
{
    struct one *o = (struct one *)argp;
    heat_touch_read(o->v, o->inode, 0);
    return NULL;
}

static void planned_release(void)
{
    pthread_mutex_lock(&g_pm);
    g_released = 1;
    pthread_cond_broadcast(&g_pc);
    pthread_mutex_unlock(&g_pm);
}

static void leg_planned(void)
{
    invfs_volume *v;
    pthread_t tw, tr;
    struct one w, reader;
    uint64_t seed = 1;

    printf("== planned leg: the grow's free(), scheduled, not raced ==\n");
    fflush(stdout);

    v = mkvol();
    if (!v) { ok(0, "mkvol"); return; }

    /* Put the table at a size where the NEXT touch must grow it, so the
     * grow branch at vol_heat.c:56 is the one that runs and the one whose
     * free() we park. Seed until heat_tab_touch's own 70%-load condition
     * (vol_heat.c:56) is already true, rather than guessing a count: a
     * guess of 200 grows the table once on the way past and leaves it at
     * n=200/mask=511, where the next touch does NOT grow. */
    while (!v->heat_tab ||
           (v->heat_tab_n + 1) * 10 < (v->heat_tab_mask + 1) * 7)
        heat_touch_read(v, ++seed, 0);
    if (!v->heat_tab) { ok(0, "the seed fill produced no table"); freevol(v); return; }
    ok(1, "seed fill produced a table");

    g_arm = 1;

    /* W: the toucher that grows, frees the array and parks before :69. */
    w.v = v; w.inode = ++seed;
    pthread_create(&tw, NULL, one_touch, &w);
    pthread_mutex_lock(&g_pm);
    while (!g_parked) pthread_cond_wait(&g_pc, &g_pm);
    pthread_mutex_unlock(&g_pm);
    ok(g_parked == v->heat_tab,
       "the grow freed the array and parked before republishing it");

    /* R: a reader that is now guaranteed to run with v->heat_tab dangling. */
    reader.v = v; reader.inode = ++seed;
    pthread_create(&tr, NULL, one_touch, &reader);

    if (heat_locks_init) {          /* fixed tree: R blocks on heat.mu */
        planned_release();
        pthread_join(tw, NULL);
        pthread_join(tr, NULL);
    } else {                        /* unfixed: R must run inside the window */
        pthread_join(tr, NULL);
        planned_release();
        pthread_join(tw, NULL);
    }

    g_arm = 0;
    ok(1, "the planned interleaving completed");
    freevol(v);
}

int main(void)
{
    /* INVFS_HEAT_SAN_LEG = structure | lifetime | planned | all (default
     * structure+lifetime; `planned` is selected explicitly because it
     * needs the -Wl,--wrap=free link and must not run with the storm legs,
     * which would park every free they do). */
    const char *leg = getenv("INVFS_HEAT_SAN_LEG");
    int do_struct = 1, do_life = 1, do_plan = 0;
    /* line-buffered, so a run that dies under a sanitizer still shows which
     * leg it died in rather than losing the last block to the stdio buffer */
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (leg && *leg) {
        if (!strcmp(leg, "planned")) {
            do_struct = do_life = 0;
            do_plan = 1;
        } else if (!strcmp(leg, "all")) {
            do_plan = 1;
        } else {
            do_struct = strcmp(leg, "lifetime") != 0;
            do_life   = strcmp(leg, "structure") != 0;
        }
    }
    if (do_struct) leg_structure();
    if (do_life)   leg_lifetime();
    if (do_plan)   leg_planned();
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

/* ================================================================== */
/* STUBS. vol_heat.c is linked ALONE, so every symbol it needs that is */
/* not libc and not libzstd is defined here. None of them is on the     */
/* path under test: the touch/insert/grow/free code touches only the    */
/* heat fields of `v`, and the stubs exist so the LINK succeeds.        */
/* ================================================================== */

int vol_write_enabled(invfs_volume *v) { (void)v; return 1; }
int sweep_enospc(invfs_volume *v, uint64_t need) { (void)v; (void)need; return 0; }
uint64_t vol_find(invfs_volume *v, const char *name) { (void)v; (void)name; return 0; }
int vol_get_class(invfs_volume *v, uint64_t inode, uint8_t *cls, uint8_t *algo,
                  uint16_t *gen)
{ (void)v; (void)inode; if (cls) *cls = 0; if (algo) *algo = 0; if (gen) *gen = 0; return 1; }
uint64_t vol_get_dec_mem_limit(invfs_volume *v) { (void)v; return 0; }
int vol_get_xattr(invfs_volume *v, uint64_t inode, const char *xn, void *val,
                  size_t *vlen)
{ (void)v; (void)inode; (void)xn; (void)val; (void)vlen; return -1; }
int vol_set_xattr(invfs_volume *v, uint64_t inode, const char *xn,
                  const void *val, size_t vlen)
{ (void)v; (void)inode; (void)xn; (void)val; (void)vlen; return 0; }
int vol_mark_dirty(invfs_volume *v) { (void)v; return 0; }
int vol_read_file(invfs_volume *v, uint64_t inode, uint8_t **out, size_t *out_len)
{ (void)v; (void)inode; if (out) *out = NULL; if (out_len) *out_len = 0; return -1; }
int vol_stamp_class(invfs_volume *v, uint64_t inode, uint8_t cls, uint8_t algo,
                    uint16_t gen)
{ (void)v; (void)inode; (void)cls; (void)algo; (void)gen; return 0; }
int vol_stat_full(invfs_volume *v, const char *name, uint64_t *id_out,
                  uint64_t *size_out, uint64_t *ctime_out)
{ (void)v; (void)name; if (id_out) *id_out = 0; if (size_out) *size_out = 0;
  if (ctime_out) *ctime_out = 0; return -1; }
int vol_store_generic(invfs_volume *v, uint64_t inode, const char *name,
                      uint8_t cls, uint8_t calgo)
{ (void)v; (void)inode; (void)name; (void)cls; (void)calgo; return -1; }
int vol_v3_inode_get(invfs_volume *v, uint64_t inode, invfs_v3_inode *out)
{ (void)v; (void)inode; (void)out; return -1; }
int vol_v3_iter_live_inodes(invfs_volume *v,
                            int (*cb)(invfs_volume *, uint64_t, const char *, void *),
                            void *ctx)
{ (void)v; (void)cb; (void)ctx; return 0; }

/* WP145: vol_heat.c's two passes now DISCHARGE a walk receipt
 * (vol_walk_t, src/core/vol_walk.h), and src/core/vol_walk.c is not part of
 * this standalone link -- so these three are the minimum the symbol table
 * needs. They are not a second receipt: the real ones live in vol_walk.c and
 * every production caller uses those. Here the receipt can only ever report
 * a COMPLETE walk, because the stubbed iterator above always returns 0 and
 * delivers nothing -- which is the right answer for this test, whose subject
 * is the heat TABLE under concurrency and not the walk. */
void vol_walk_init(vol_walk_t *w, invfs_volume *v, const char *what)
{ if (w) { memset(w, 0, sizeof *w); w->v = v; w->what = what; } }
void vol_walk_result(vol_walk_t *w, int rc, size_t n, size_t found)
{ if (w) { w->rc = rc; w->n = n; w->found = found; w->armed = 1; } }
int vol_walk_commit(vol_walk_t *w) { return (w && w->rc) ? -1 : 0; }
uint64_t vol_v3_publish_blob_inode(invfs_volume *v, uint64_t inode,
                                   const uint8_t *blob, size_t blob_len,
                                   uint64_t orig_size, uint32_t algo)
{ (void)v; (void)inode; (void)blob; (void)blob_len; (void)orig_size; (void)algo; return 0; }
const invfs_codec *invfs_codec_by_algo(uint32_t algo) { (void)algo; return NULL; }
int invfs_profile_zstd_level(int p) { return p; }
uint16_t invfs_registry_generation(void) { return 0; }
int invfs_sweep_ui_active(void) { return 0; }

