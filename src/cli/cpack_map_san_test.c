/*
 * cpack_map_san_test.c — the RED CONTROL for the parsed-!mbrmap-cache
 * concurrency defect in src/core/vol_cpack.c.
 *
 * WHAT THIS IS
 * ------------
 * `v->maps` / `v->maps_n` / `v->maps_cap` (src/core/volume_internal.h:478-479)
 * are the session's parsed-map cache: one entry per seekable containerpack the
 * read path has touched. On the UNFIXED tree there is NO synchronization
 * anywhere in vol_cpack.c -- no mutex, no atomic, no release store and no
 * acquire load. `cpack_map_get` (src/core/vol_cpack.c:2722) walks the array,
 * `realloc`s it (:2775) and returns `&v->maps[i]`; `cpack_map_read` (:2839)
 * dereferences that borrowed pointer at :2841-2842 and `cpack_map_serve`
 * (:2536) reads the container's bytes out of it and RETURNS SUCCESS.
 *
 * The read path reaches all of this LOCK-FREE: `invf_read` releases
 * g_io_lock at src/cli/fuse_fs.c:1407 and THEN calls vol_read_range at :1423,
 * and the daemon runs fuse_loop_mt (:3362). The three cpack_map_read call
 * sites -- src/core/vol_read.c:1259 (vol_decode_ast_entries), :1729
 * (v3_read_range) and :1972 (vol_read_range) -- are all under it.
 *
 * This is the sibling of src/cli/arc_san_test.c and src/cli/heat_san_test.c
 * and it is built their way: one source, several binaries, linked against
 * src/core/vol_cpack.c ALONE. vol_cpack.c needs the real volume_internal.h for
 * the struct and calls ~45 project symbols (vol_find, vol_read_file,
 * vol_read_range, seg_read_checked, cpack_parse_table is REAL, ...), all
 * defined as stubs at the bottom of THIS FILE, so the link needs nothing
 * outside libc + libzstd + libz. A sanitizer build costs about a second and
 * needs no volume, no image, no /dev/shm.
 *
 * WHAT IS ACTUALLY BEING PROVEN, IN TWO SEPARATE PIECES
 * -----------------------------------------------------
 * 1. A GROW IS REACHABLE ON THE READ PATH, AND IT IS THE ONLY WAY THE ARRAY
 *    EVER GROWS. `cpack_map_get` has exactly one caller -- `cpack_map_read`
 *    (:2839) -- and `cpack_map_read`'s only three callers are inside
 *    vol_read_range. There is no write-path population, no sweep population
 *    and no open-time population: the write to `v->maps_n++` (:2791) and the
 *    `realloc` (:2772-2778) are reached ONLY by a read of a container that is
 *    not cached yet. So two threads reading two DIFFERENT not-yet-cached
 *    containers realloc the array under each other, and a realloc invalidates
 *    every pointer into it, free or no free. Leg 2 below forces that realloc
 *    to land at the worst possible instant.
 *
 * 2. THE BORROWED ENTRY POINTER IS UNBOUNDED. `cpack_map_cache_invalidate`
 *    (:2698) -- reached from vol_retire_inode (src/core/vol_records.c:660) via
 *    vol_delete_inode, i.e. the unlink / supersede path, which DOES hold
 *    g_io_lock -- frees the entry's `ents`, `mem` and `recipe` (:2676-2680)
 *    and then COMPACTS with `v->maps[w] = v->maps[i]` (:2711). A reader that
 *    is between the get and the deref at :2841 is holding a pointer into a
 *    slot that is either freed or silently overwritten with a DIFFERENT
 *    container's entry. Leg 3 forces that.
 *
 * WHY THIS IS NOT A PROBABILITY TEST
 * ----------------------------------
 * The same standard as the ARC and heat controls, met by construction.
 *
 *   Leg 1 (TSAN) is deterministic BY CONSTRUCTION: the unfixed path contains
 *   no synchronization at all, so under TSAN's model every pair of concurrent
 *   accesses to v->maps / v->maps_n is a conflicting-access pair with NO
 *   HAPPENS-BEFORE EDGE, always -- not "usually", not "if they overlap in
 *   time". TSAN needs accesses to be UNORDERED, not overlapping, and with no
 *   edge anywhere the unorderedness is guaranteed.
 *
 *   Legs 2 and 3 (ASAN) SCHEDULE the free rather than racing for it, with no
 *   production hook, using the linker. -Wl,--wrap=realloc performs the grow's
 *   REAL realloc -- which frees the old array -- and then parks BEFORE
 *   `v->maps = nm` at vol_cpack.c:2777 runs, so a reader started after the
 *   park resolves against a freed, still-published array. -Wl,--wrap=free
 *   performs a retire's real free of an entry and parks, with the slot still
 *   naming a live container. The main thread starts the reader only after it
 *   has OBSERVED the park, so both are guaranteed to run inside a window the
 *   production code itself leaves open. No timing, no luck, no production
 *   hook.
 *
 * WHY IT COMPILES AGAINST THE UNFIXED TREE
 * -----------------------------------------
 * The fix adds a mutex. A test that only compiles against the FIXED tree
 * cannot be a red control, so `cpack_locks_init` is declared WEAK here: on a
 * tree without it the symbol resolves to NULL and the test skips the
 * initialization -- i.e. it runs the pre-fix, unlocked code, which is the bug.
 * Same source, same command, red before and green after.
 *
 *   The public entry point `cpack_map_read` and the public
 *   `cpack_map_cache_invalidate` / `cpack_map_cache_reset` have the SAME
 *   signatures on both trees, and so do the volume fields the test drives
 *   (v->maps / v->maps_n / v->maps_cap). `struct cpack_map_cache` is defined
 *   INSIDE vol_cpack.c and is deliberately not touched by the test -- the
 *   test observes the cache only through the shipped accessors, which is the
 *   point: it is a control, not a mirror.
 *
 * Built several ways from this one file (see the Makefile):
 *   -fsanitize=thread   -> leg 1, the structure
 *   -fsanitize=address  -> legs 2 and 3, the lifetime
 *   no sanitizer        -> all legs; the plain build asserts the BYTES
 * INVFS_CPACK_SAN_LEG=structure|planned_realloc|planned_free|all
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "volume_internal.h"
/* only for the TYPES of the stubs below -- none of these headers is needed
 * for anything on the path under test */
#include "helper_exec.h"
#include "tool_scratch.h"
#include "deflate_repro.h"

/* Weak on purpose: see the header comment. NULL on a tree that predates the
 * fix, and the cache is then used with no lock at all -- which is the defect
 * under test. */
extern void cpack_locks_init(invfs_volume *v) __attribute__((weak));

/* the public surface under test -- identical on both trees */
int64_t cpack_map_read(invfs_volume *v, const char *name, uint64_t ino,
                       uint64_t container_size, uint64_t off,
                       uint8_t *dst, size_t len);
void cpack_map_cache_invalidate(invfs_volume *v, const char *name);
void cpack_map_cache_reset(invfs_volume *v);

/* ------------------------------------------------------------------ */
/* the synthetic volume                                               */
/*                                                                     */
/* Container cNNN is 4096 bytes, tiled by a two-entry MRM2 map:        */
/*   [0,2048)      kind 1 (MEMBER), member idx 0 -- served through    */
/*                 cpack_member_read -> vol_find + vol_read_range       */
/*   [2048,4096)   kind 0 (RECIPE), src_off 0 -- served by a memcpy   */
/*                 straight out of the entry's `recipe` blob           */
/*                                                                     */
/* The two halves come out of DIFFERENT fields of the SAME entry, and  */
/* the two halves carry DIFFERENT per-container byte patterns. So a   */
/* reader whose entry pointer has been shifted onto a neighbouring      */
/* container's slot reads that container's map and its recipe and      */
/* returns SUCCESS with the wrong bytes -- which is what the plain     */
/* build checks for, byte for byte.                                    */
/* ------------------------------------------------------------------ */

#define CCONT_SIZE   4096u
#define CHALF        2048u
#define CPACK_REcipe 4096u          /* the recipe blob's length */
#define NCONT        40             /* distinct container names */
#define MEMBER_PAT(c)  ((uint8_t)(0x50 + (c)))
#define RECIPE_PAT(c)  ((uint8_t)(0xA0 + (c)))

static int failures;

static void ok(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

static int cont_of(const char *name)
{
    /* "cNNN", optionally followed by "!..." */
    if (!name || name[0] != 'c') return -1;
    if (name[1] < '0' || name[1] > '9') return -1;
    if (name[2] < '0' || name[2] > '9') return -1;
    if (name[3] < '0' || name[3] > '9') return -1;
    return (name[1] - '0') * 100 + (name[2] - '0') * 10 + (name[3] - '0');
}

static void cont_name(char *buf, size_t cap, int c)
{
    snprintf(buf, cap, "c%03d", c);
}

/* inode ids the stubs hand out. A container index is embedded in the low
 * 16 bits of every id, so a stub that only has the id still knows which
 * container it is serving. */
#define SID_BASE   0x10000000u
static uint64_t sid(int cont, unsigned sub) { return SID_BASE | ((uint64_t)cont << 8) | sub; }
static int sid_cont(uint64_t id) { return (int)((id >> 8) & 0xffff); }

/* ---- the bytes each container must read back as -------------------- */

static int check_bytes(int c, const uint8_t *got, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        uint8_t want = i < CHALF ? MEMBER_PAT(c) : RECIPE_PAT(c);
        if (got[i] != want) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* the volume under test                                               */
/* ------------------------------------------------------------------ */

static invfs_volume *mkvol(void)
{
    invfs_volume *v = (invfs_volume *)calloc(1, sizeof *v);
    if (!v) return NULL;
    v->sb.vol_flags |= VOLF_V3;      /* take cpack_recipe_seg's v3 branch */
    v->sb.total_blocks = 100000;
    /* Fixed tree: take the lock down properly. Unfixed tree: the weak symbol
     * is NULL and this is exactly the pre-fix state. */
    if (cpack_locks_init) cpack_locks_init(v);
    return v;
}

static void freevol(invfs_volume *v)
{
    if (!v) return;
    cpack_map_cache_reset(v);
    free(v);
}

/* Populate the cache with containers 0..n-1, one at a time, so the array is
 * at exactly maps_n == maps_cap == CPACK_MAPS_CAP0 (8) when n reaches 8. The
 * grow below is the production grow, not a manufactured one. */
static int seed(invfs_volume *v, int n)
{
    int c;
    for (c = 0; c < n; c++) {
        char nm[32];
        uint8_t buf[CCONT_SIZE];
        cont_name(nm, sizeof nm, c);
        if (cpack_map_read(v, nm, sid(c, 0), CCONT_SIZE, 0, buf, CCONT_SIZE)
            != (int64_t)CCONT_SIZE) {
            printf("FAIL seed: %s did not read back\n", nm);
            failures++;
            return 0;
        }
        if (!check_bytes(c, buf, CCONT_SIZE)) {
            printf("FAIL seed: %s read back the WRONG bytes\n", nm);
            failures++;
            return 0;
        }
    }
    return 1;
}

/* ================================================================== */
/* LINKER INTERPOSERS. Both schedule a free instead of racing for one. */
/* ================================================================== */

extern void __real_free(void *p);
extern void *__real_realloc(void *p, size_t n);

/* --- the grow's realloc ------------------------------------------- */
/* cpack_map_get's grow is                                            */
/*     nm = realloc(v->maps, nc * sizeof *nm);   (:2775)              */
/*     v->maps = nm;                               (:2777)             */
/* The real realloc has already FREED the old array when we are called  */
/* back, and v->maps still names it: the array is dangling and          */
/* published. Arm only on the array itself, so the loader's own         */
/* allocations cannot trip it.                                         */

static int            g_realloc_armed;
static void          *g_realloc_target;
static void          *g_realloc_done;
static pthread_mutex_t g_rpm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_rpc = PTHREAD_COND_INITIALIZER;
static int            g_realloc_released;

void *__wrap_realloc(void *p, size_t n)
{
    void *r;
    if (!g_realloc_armed || p != g_realloc_target) return __real_realloc(p, n);
    r = __real_realloc(p, n);        /* the free IS DONE; :2777 has not run */
    pthread_mutex_lock(&g_rpm);
    g_realloc_done = r;
    g_realloc_released = 0;
    pthread_cond_broadcast(&g_rpc);
    while (!g_realloc_released) pthread_cond_wait(&g_rpc, &g_rpm);
    pthread_mutex_unlock(&g_rpm);
    return r;
}

/* --- an entry's free ---------------------------------------------- */
/* cpack_map_cache_free_ent frees name, ents, mem, recipe IN THAT ORDER  */
/* (:2676-2679). COUNTING the frees rather than matching a remembered     */
/* pointer keeps the interposer honest: the loader reallocates the entry   */
/* between a probe and the arm, so an address captured earlier is stale.    */
/* The SECOND free of a retire is the `ents` -- the field cpack_map_read  */
/* dereferences at :2841, and the field the still-live slot names.         */

static int            g_free_armed;
static int            g_free_seen;
static int            g_free_hit;
static int            g_free_released;
static pthread_mutex_t g_fpm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_fpc = PTHREAD_COND_INITIALIZER;

void __wrap_free(void *p)
{
    if (!g_free_armed) { __real_free(p); return; }
    __real_free(p);                 /* the free IS DONE */
    if (g_free_hit) return;         /* park ONCE: the rest of the retire's
                                     * frees must run through to the unlock */
    if (++g_free_seen < 2) return;  /* that was the name; ents is next */
    pthread_mutex_lock(&g_fpm);
    g_free_hit = 1;
    g_free_released = 0;
    pthread_cond_broadcast(&g_fpc);
    while (!g_free_released) pthread_cond_wait(&g_fpc, &g_fpm);
    pthread_mutex_unlock(&g_fpm);
}

static void planned_release(void)
{
    pthread_mutex_lock(&g_rpm);
    g_realloc_released = 1;
    pthread_cond_broadcast(&g_rpc);
    pthread_mutex_unlock(&g_rpm);
    pthread_mutex_lock(&g_fpm);
    g_free_released = 1;
    pthread_cond_broadcast(&g_fpc);
    pthread_mutex_unlock(&g_fpm);
}

/* ================================================================== */
/* leg 1: STRUCTURE. The shape of a live mount's read path.            */
/* ================================================================== */

#define NTHREADS 6
#define ITERS    3000

struct racer {
    invfs_volume *v;
    long          id;
    long          reads;
    long          wrong;
    long          failed;
};

static void *racer(void *argp)
{
    struct racer *r = (struct racer *)argp;
    int i;
    for (i = 0; i < ITERS; i++) {
        int c = (int)((r->id * 7 + i) % NCONT);
        char nm[32];
        uint8_t buf[CCONT_SIZE];
        int64_t got;
        cont_name(nm, sizeof nm, c);
        got = cpack_map_read(r->v, nm, sid(c, 0), CCONT_SIZE, 0, buf,
                             CCONT_SIZE);
        if (got != (int64_t)CCONT_SIZE)      r->failed++;
        else if (!check_bytes(c, buf, CCONT_SIZE)) r->wrong++;
        r->reads++;
    }
    return NULL;
}

static void leg_structure(void)
{
    invfs_volume *v;
    pthread_t th[NTHREADS];
    struct racer racers[NTHREADS];
    long i, wrong = 0, failed = 0, reads = 0;

    printf("== structure leg: %d threads x %d cpack_map_read over %d "
           "containers ==\n", NTHREADS, ITERS, NCONT);
    printf("   cpack_locks_init present: %s\n", cpack_locks_init ? "yes" : "no");
    fflush(stdout);

    v = mkvol();
    if (!v) { ok(0, "mkvol"); return; }

    for (i = 0; i < NTHREADS; i++) {
        racers[i].v = v; racers[i].id = i;
        racers[i].reads = racers[i].wrong = racers[i].failed = 0;
        pthread_create(&th[i], NULL, racer, &racers[i]);
    }
    for (i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);

    for (i = 0; i < NTHREADS; i++) {
        reads += racers[i].reads;
        wrong += racers[i].wrong;
        failed += racers[i].failed;
    }
    printf("   %ld reads, %ld short/failed, %ld WRONG BYTES\n",
           reads, failed, wrong);
    ok(reads == (long)NTHREADS * ITERS, "every read completed");
    ok(failed == 0, "no read failed (a failed map read is a loud -1)");
    ok(wrong == 0, "every read returned the container's OWN bytes");

    freevol(v);
}

/* ================================================================== */
/* leg 2: PLANNED REALLOC. The grow, scheduled.                       */
/* ================================================================== */

struct one {
    invfs_volume *v;
    int           c;
    long          got;
};

static void *one_read(void *argp)
{
    struct one *o = (struct one *)argp;
    char nm[32];
    uint8_t buf[CCONT_SIZE];
    cont_name(nm, sizeof nm, o->c);
    o->got = cpack_map_read(o->v, nm, sid(o->c, 0), CCONT_SIZE, 0, buf,
                            CCONT_SIZE);
    if (o->got == (int64_t)CCONT_SIZE && !check_bytes(o->c, buf, CCONT_SIZE))
        o->got = -2;                    /* success with the wrong bytes */
    return NULL;
}

static void leg_planned_realloc(void)
{
    invfs_volume *v;
    pthread_t tw, tr;
    struct one w, reader;
    long parked_read;

    printf("== planned leg: the grow's realloc(), scheduled, not raced ==\n");
    fflush(stdout);

    v = mkvol();
    if (!v) { ok(0, "mkvol"); return; }

    /* 8 entries -> maps_n == maps_cap == 8, so the NEXT insert is the
     * production grow at vol_cpack.c:2772-2778. */
    if (!seed(v, 8)) { freevol(v); return; }
    ok(v->maps != NULL && v->maps_n == 8 && v->maps_cap == 8,
       "the seed fill left the array at maps_n == maps_cap == 8");
    if (v->maps_n != v->maps_cap) { freevol(v); return; }

    g_realloc_armed = 1;
    g_realloc_target = v->maps;

    /* W: the reader that grows the array. Container 8 is not cached, so
     * this is a MISS and the insert is a grow -- the real production grow,
     * reached the way the read path reaches it. */
    w.v = v; w.c = 8; w.got = 0;
    pthread_create(&tw, NULL, one_read, &w);
    pthread_mutex_lock(&g_rpm);
    while (!g_realloc_done) pthread_cond_wait(&g_rpc, &g_rpm);
    pthread_mutex_unlock(&g_rpm);
    ok(1, "the grow realloc'd the array and parked before republishing it");
    ok(v->maps == g_realloc_target,
       "v->maps still names the array the realloc just freed");

    /* R: a reader that now resolves a container against a FREED, still
     * published array. Its very first dereference -- v->maps[i].name in
     * cpack_map_get's scan loop -- is the use-after-free. */
    reader.v = v; reader.c = 0; reader.got = 0;
    pthread_create(&tr, NULL, one_read, &reader);

    if (cpack_locks_init) {
        /* fixed tree: R blocks on cpacks_mu while W holds it across the
         * grow, so W has to be released first. R's borrow is a reference
         * to the ENTRY, not into the array, so running it after the
         * republish is exactly the case the fix is for. */
        planned_release();
        pthread_join(tw, NULL);
        pthread_join(tr, NULL);
    } else {
        /* unfixed: R must run inside the window. */
        pthread_join(tr, NULL);
        planned_release();
        pthread_join(tw, NULL);
    }
    g_realloc_armed = 0;

    parked_read = reader.got;
    ok(parked_read == (long)CCONT_SIZE || parked_read == -2,
       "the reader resolved and read the container");
    ok(parked_read != -2, "the reader did not get another container's bytes");
    freevol(v);
}

/* ================================================================== */
/* leg 3: PLANNED FREE. The retire, scheduled.                        */
/* ================================================================== */

static void *one_invalidate(void *argp)
{
    struct one *o = (struct one *)argp;
    char nm[32];
    cont_name(nm, sizeof nm, o->c);
    cpack_map_cache_invalidate(o->v, nm);
    return NULL;
}

static void leg_planned_free(void)
{
    invfs_volume *v;
    pthread_t tw, tr;
    struct one w, reader;
    printf("== planned leg: a retire's free(), scheduled, not raced ==\n");
    fflush(stdout);

    v = mkvol();
    if (!v) { ok(0, "mkvol"); return; }

    /* Four containers cached, so the retire below has something to take
     * down and a reader has something to resolve. */
    if (!seed(v, 4)) { freevol(v); return; }
    ok(v->maps_n == 4, "the seed fill cached four containers");

    g_free_armed = 1; g_free_seen = 0; g_free_hit = 0;

    /* W: the retire. It frees the entry's name, then its ents -- and the
     * interposer parks on the ents free, with the name already freed and
     * the slot still naming a live container. */
    w.v = v; w.c = 3; w.got = 0;
    pthread_create(&tw, NULL, one_invalidate, &w);
    pthread_mutex_lock(&g_fpm);
    while (!g_free_hit) pthread_cond_wait(&g_fpc, &g_fpm);
    pthread_mutex_unlock(&g_fpm);
    ok(1, "the retire freed the entry and parked with the slot still live");

    /* R: a reader that resolves container 3 -- the one being retired --
     * against a slot whose fields are freed. */
    reader.v = v; reader.c = 3; reader.got = 0;
    pthread_create(&tr, NULL, one_read, &reader);

    if (cpack_locks_init) {
        /* fixed tree: the reader holds a reference to the entry, so the
         * retire only drops the array's reference and the entry outlives
         * it. Nothing here blocks, so either order is safe; release the
         * writer first for symmetry with leg 2. */
        planned_release();
        pthread_join(tw, NULL);
        pthread_join(tr, NULL);
    } else {
        pthread_join(tr, NULL);
        planned_release();
        pthread_join(tw, NULL);
    }
    g_free_armed = 0;

    ok(reader.got == (long)CCONT_SIZE || reader.got == -2,
       "the in-flight reader completed its read");
    ok(reader.got != -2, "the in-flight reader got the right bytes");
    freevol(v);
}

int main(void)
{
    const char *leg = getenv("INVFS_CPACK_SAN_LEG");
    int do_struct = 1, do_realloc = 0, do_free = 0;
    /* line-buffered, so a run that dies under a sanitizer still shows which
     * leg it died in rather than losing the last block to the stdio buffer */
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (leg && *leg) {
        if (!strcmp(leg, "planned_realloc")) {
            do_struct = 0; do_realloc = 1;
        } else if (!strcmp(leg, "planned_free")) {
            do_struct = 0; do_free = 1;
        } else if (!strcmp(leg, "all")) {
            do_realloc = do_free = 1;
        } else if (!strcmp(leg, "structure")) {
            do_struct = 1;
        }
    }
    if (do_struct)  leg_structure();
    if (do_realloc) leg_planned_realloc();
    if (do_free)    leg_planned_free();
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

/* ================================================================== */
/* STUBS. vol_cpack.c is linked ALONE, so every project symbol it needs */
/* that is not libc / libz / libzstd is defined here. The ones on the    */
/* path under test are NOT stubs of behaviour: vol_find, vol_read_file,  */
/* vol_read_range, seg_read_checked and vol_ast_recipe_parse serve the  */
/* synthetic containers above, so the code under test runs its REAL      */
/* load, validate and serve paths.                                      */
/* ================================================================== */

int vol_read_range(invfs_volume *v, uint64_t inode_id, uint64_t offset,
                   size_t len, void *buf)
{
    int c = sid_cont(inode_id);
    (void)v;
    if (c < 0 || c >= NCONT || offset + len > CCONT_SIZE) return -1;
    memset(buf, MEMBER_PAT(c), len);
    return (int)len;
}

/* "!mbrt", "!mbrmap" and "!mbr<idx>" get distinct ids, so vol_read_file
 * below can tell the member TABLE from the MAP blob without a name. */
#define SUB_RECIPE 0
#define SUB_TABLE  1
#define SUB_MAP    2
#define SUB_MEMBER 3

static uint64_t find_id(const char *name)
{
    int c = cont_of(name);
    size_t n;
    if (c < 0 || c >= NCONT || !name) return 0;
    n = strlen(name);
    if (n == 4)                                  return sid(c, SUB_RECIPE);
    if (n > 4 && !strcmp(name + 4, "!mbrt"))     return sid(c, SUB_TABLE);
    if (n > 4 && !strcmp(name + 4, "!mbrmap"))   return sid(c, SUB_MAP);
    if (n > 4 && !strcmp(name + 4, "!mbr0000"))  return sid(c, SUB_MEMBER);
    return 0;
}

uint64_t vol_find(invfs_volume *v, const char *name)
{
    (void)v;
    return find_id(name);
}

int vol_read_file(invfs_volume *v, uint64_t inode, uint8_t **out, size_t *out_len)
{
    int c = sid_cont(inode);
    unsigned sub = (unsigned)(inode & 0xff);
    (void)v;
    if (c < 0 || c >= NCONT || !out || !out_len) return -1;
    if (sub == SUB_TABLE) {
        /* "idx<TAB>empty sname<TAB>usize" -- an empty sname makes the
         * member sibling exactly "cNNN!mbr0000" (cpack_mbr_name:1839). */
        static const char row[] = "0\t\t4096\n";
        uint8_t *b = (uint8_t *)malloc(sizeof row - 1);
        if (!b) return -1;
        memcpy(b, row, sizeof row - 1);
        *out = b; *out_len = sizeof row - 1;
        return 0;
    }
    if (sub == SUB_MAP) {
        /* a v2 map (MRM2): 12-byte header + two 40-byte entries. */
        uint32_t count = 2, gen = 1, idx;
        uint64_t z, l, o, so;
        uint8_t *b = (uint8_t *)malloc(12 + 2 * 40), *e;
        if (!b) return -1;
        memcpy(b, "MRM2", 4);
        memcpy(b + 4, &count, 4);
        memcpy(b + 8, &gen, 4);
        e = b + 12;
        /* e0: [0,2048) MEMBER, member idx 0, src_off 0 */
        memset(e, 0, 40);
        z = 0;   memcpy(e, &z, 8);
        l = CHALF; memcpy(e + 8, &l, 8);
        e[16] = 1;
        idx = 0; memcpy(e + 17, &idx, 4);
        so = 0;  memcpy(e + 21, &so, 8);
        /* e1: [2048,4096) RECIPE, src_off 0 */
        memset(e + 40, 0, 40);
        o = CHALF; memcpy(e + 40, &o, 8);
        l = CCONT_SIZE - CHALF; memcpy(e + 48, &l, 8);
        e[40 + 16] = 0;
        idx = 0; memcpy(e + 40 + 17, &idx, 4);
        so = 0;  memcpy(e + 40 + 21, &so, 8);
        (void)c;
        *out = b; *out_len = 12 + 2 * 40;
        return 0;
    }
    return -1;
}

/* cpack_recipe_seg's v3 branch: vol_v3_inode_get -> vol_v3_recipe_load ->
 * vol_ast_recipe_parse -> seg_read_checked. */
int vol_v3_inode_get(invfs_volume *v, uint64_t inode_id, invfs_v3_inode *out)
{
    int c = sid_cont(inode_id);
    (void)v;
    if (c < 0 || c >= NCONT || !out) return -1;
    memset(out, 0, sizeof *out);
    out->recipe_addr[0] = (uint8_t)c;
    return 1;
}

int vol_v3_recipe_load(invfs_volume *v,
                       const uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN],
                       uint8_t **blob_out, size_t *blen_out)
{
    (void)v;
    if (!addr || !blob_out || !blen_out) return -1;
    *blob_out = (uint8_t *)malloc(4);
    if (!*blob_out) return -1;
    (*blob_out)[0] = addr[0];
    *blen_out = 4;
    return 0;
}

/* One AST entry, whose pba NAMES the container, so seg_read_checked below
 * can tell the containers apart (it has nothing else to go on). */
int vol_ast_recipe_parse(const uint8_t *blob, size_t blen,
                         invfs_ast_hdr *hdr_out,
                         const invfs_ast_block_entry **ents_out,
                         size_t *nents_out)
{
    static __thread invfs_ast_block_entry e;   /* per-thread: cpack_recipe_seg
                                                * reads ents[0] before returning, and a
                                                * plain static would be a race
                                                * in the STUB, not in the code
                                                * under test */
    (void)blen;
    if (!blob || !hdr_out || !ents_out || !nents_out) return -1;
    memset(hdr_out, 0, sizeof *hdr_out);
    e = (invfs_ast_block_entry){0};
    e.pba = 1000u + blob[0];
    *ents_out = &e;
    *nents_out = 1;
    return 0;
}

int seg_read_checked(invfs_volume *v, uint64_t pba, uint64_t plen,
                     uint32_t min_csize, uint32_t *csize_out,
                     uint8_t **blob_out)
{
    uint8_t *b;
    int c;
    (void)v; (void)plen; (void)min_csize;
    if (pba < 1000u) return -1;
    c = (int)(pba - 1000u);
    if (c < 0 || c >= NCONT || !csize_out || !blob_out) return -1;
    b = (uint8_t *)malloc(CPACK_REcipe);
    if (!b) return -1;
    memset(b, RECIPE_PAT(c), CPACK_REcipe);
    *blob_out = b;
    *csize_out = CPACK_REcipe;
    return 0;
}

/* (the map blob and the member table are built by vol_read_file above) */

/* ---- everything else vol_cpack.c needs, none of it on the path
 *      under test. These exist so the LINK succeeds; each is off the
 *      map read/serve path and returns a benign "no". -------------- */

int bz_defer(invfs_volume *v, uint64_t inode_id, const char *name, uint64_t size, uint32_t family)
{ (void)v; (void)inode_id; (void)name; (void)size; (void)family; return 0; }
int tz_defer(invfs_volume *v, uint64_t inode_id, const char *name, uint64_t size, uint32_t family)
{ (void)v; (void)inode_id; (void)name; (void)size; (void)family; return 0; }
int flacx_extract(const uint8_t *d, size_t n, uint8_t **recipe, size_t *rlen, flacx_cover **covers, uint32_t *ncovers)
{ (void)d; (void)n; (void)recipe; (void)rlen; (void)covers; (void)ncovers; return -1; }
int flacx_rebuild(const uint8_t *wav, size_t wlen, const uint8_t *r, size_t rn, const flacx_cover *covers, uint32_t ncovers, uint8_t **out, size_t *olen)
{ (void)wav; (void)wlen; (void)r; (void)rn; (void)covers; (void)ncovers; (void)out; (void)olen; return -1; }
const name_index_entry *idx_get(invfs_volume *v, const char *name, size_t nlen)
{ (void)v; (void)name; (void)nlen; return NULL; }
uint64_t idx_get_id(invfs_volume *v, uint64_t id) { (void)v; return id; }
void idx_put(invfs_volume *v, const char *name, size_t nlen, uint64_t id, uint64_t pos, uint64_t size, uint64_t ctime)
{ (void)v; (void)name; (void)nlen; (void)id; (void)pos; (void)size; (void)ctime; }
int invfs_binary_family(const uint8_t *head, size_t head_len, const char *name)
{ (void)head; (void)head_len; (void)name; return 0; }
const invfs_codec *invfs_codec_by_algo(uint32_t algo) { (void)algo; return NULL; }
const invfs_pack_def *invfs_codec_pack_def(const invfs_codec *c) { (void)c; return NULL; }
int invfs_deflate_repro_encode(const uint8_t *raw, size_t raw_len, const invfs_deflate_params *params, uint8_t **out_stream, size_t *out_len)
{ (void)raw; (void)raw_len; (void)params; (void)out_stream; (void)out_len; return -1; }
int invfs_helper_exec(char *const argv[], uint64_t mem_cap, const invfs_helper_sandbox *sb, const char *work_dir, int no_path_search, char *out, size_t out_cap, uint64_t timeout_ms)
{ (void)argv; (void)mem_cap; (void)sb; (void)work_dir; (void)no_path_search; (void)out; (void)out_cap; (void)timeout_ms; return -1; }
int invfs_plugin_pool_container_cmd(const char *pack_name, const char *pack_so_path, int cmd, const char *in_path, const char *idx, const char *out_path, const char *recipe_path, const char *mbr_dir)
{ (void)pack_name; (void)pack_so_path; (void)cmd; (void)in_path; (void)idx; (void)out_path; (void)recipe_path; (void)mbr_dir; return -1; }
int invfs_plugin_pool_container_estimate(const char *pack_name, const char *pack_so_path, const char *in_path, uint64_t *out_mbr_sz)
{ (void)pack_name; (void)pack_so_path; (void)in_path; (void)out_mbr_sz; return -1; }
bool invfs_plugin_pool_is_available(void) { return false; }
int invfs_text_family(const char *name, const uint8_t *head, size_t head_len)
{ (void)name; (void)head; (void)head_len; return 0; }
int meta_read_record_by_id(invfs_volume *v, uint64_t inode_id, uint8_t **buf_out, uint32_t *rl_out, char *name_out, size_t name_cap, uint64_t *pos_out)
{ (void)v; (void)inode_id; (void)buf_out; (void)rl_out; (void)name_out; (void)name_cap; (void)pos_out; return -1; }
int sweep_enospc(invfs_volume *v, uint64_t need_bytes) { (void)v; (void)need_bytes; return 0; }
int tarx_build_recipe(const tarx_member *m, size_t n, const uint8_t *trailer, size_t trailer_len, uint8_t **recipe_out, size_t *rlen_out)
{ (void)m; (void)n; (void)trailer; (void)trailer_len; (void)recipe_out; (void)rlen_out; return -1; }
int tarx_extract(const uint8_t *tar, size_t tar_len, tarx_member **members_out, size_t *n_out, uint8_t **trailer_out, size_t *trailer_len_out)
{ (void)tar; (void)tar_len; (void)members_out; (void)n_out; (void)trailer_out; (void)trailer_len_out; return -1; }
int tool_scratch_grow(char *dir, size_t cap, uint64_t need, const scratch_file *files, int nfiles)
{ (void)dir; (void)cap; (void)need; (void)files; (void)nfiles; return -1; }
int tool_tmpdir(char *dir, size_t cap, uint64_t need) { (void)dir; (void)cap; (void)need; return -1; }
uint64_t vol_apply_meta(invfs_volume *v, const char *name, const invfs_meta_pub *meta)
{ (void)v; (void)name; (void)meta; return 0; }
uint64_t vol_create_blob_file(invfs_volume *v, const char *name, const uint8_t *blob, size_t blob_len, uint64_t orig_size, uint32_t algo)
{ (void)v; (void)name; (void)blob; (void)blob_len; (void)orig_size; (void)algo; return 0; }
uint64_t vol_create_file(invfs_volume *v, const char *name, const uint8_t *data, size_t len)
{ (void)v; (void)name; (void)data; (void)len; return 0; }
int vol_delete_inode(invfs_volume *v, uint64_t inode_id, const char *name)
{ (void)v; (void)inode_id; (void)name; return 0; }
int vol_delete_siblings(invfs_volume *v, const char *name) { (void)v; (void)name; return 0; }
uint64_t vol_get_dec_mem_limit(invfs_volume *v) { (void)v; return 0; }
int vol_get_meta(invfs_volume *v, uint64_t inode_id, invfs_meta_pub *out)
{ (void)v; (void)inode_id; (void)out; return -1; }
int vol_stamp_class(invfs_volume *v, uint64_t inode_id, uint8_t cls, uint8_t algo, uint16_t gen)
{ (void)v; (void)inode_id; (void)cls; (void)algo; (void)gen; return 0; }
int vol_stat_full(invfs_volume *v, const char *name, uint64_t *id_out, uint64_t *size_out, uint64_t *ctime_out)
{ (void)v; (void)name; if (id_out) *id_out = 0; if (size_out) *size_out = 0;
  if (ctime_out) *ctime_out = 0;
  return -1; }
uint64_t vol_transcode_abort(invfs_volume *v, const char *name) { (void)v; (void)name; return 0; }
int vol_v3_free_recipe_blocks(invfs_volume *v, const uint8_t recipe_addr[INVFS_V3_RECIPE_ADDR_LEN], uint64_t keep_pba)
{ (void)v; (void)recipe_addr; (void)keep_pba; return 0; }
/* WP202: cpack_release_superseded is now a one-line forward to the shared
 * vol_v3_release_superseded_blob (src/core/vol_ast.c), so this file -- which
 * links vol_cpack.c ALONE, per its own header -- needs its stub, the same way
 * it already stubs vol_v3_free_recipe_blocks just above. It is off the
 * map-cache path under test. */
void vol_v3_release_superseded_blob(invfs_volume *v, uint64_t inode_id,
                                    const uint8_t old_addr[INVFS_V3_RECIPE_ADDR_LEN])
{ (void)v; (void)inode_id; (void)old_addr; }
int vol_v3_path_lookup(invfs_volume *v, const char *name, uint64_t *ino_out)
{ (void)v; (void)name; if (ino_out) *ino_out = 0; return -1; }
