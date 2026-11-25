/*
 * vol_fault.h — TEST-ONLY fault injection for the engine.
 *
 * Some core paths can only be reached by an allocation failure or a metadata
 * read error, and neither is reproducible from a test: you cannot exhaust the
 * machine's memory on purpose, and you cannot corrupt a B+ tree without
 * destroying the thing under test. This is the seam that makes them
 * reachable. One environment variable arms one named site to fail its nth
 * call:
 *
 *     INVFS_FAULT="<site>:<n>"    the n-th call (1-based) to <site> fails
 *
 * It is ONE-SHOT: the arming does not outlive the failure it caused. Changing
 * the variable re-arms it, which is how a test runs several cases in one
 * process.
 *
 * PRODUCTION BEHAVIOUR WITH THE VARIABLE UNSET IS UNCHANGED. Nothing is
 * compiled differently and no code path is added or removed. Every call is
 * one getenv() (a scan of environ) and one pointer compare against the
 * remembered value; the result is a hard 0. A daemon started without the
 * variable never leaves the unarmed state.
 *
 * Two rules for call sites:
 *
 *   1. Put the check where the caller already has a real errno to report. The
 *      injected failure has to be indistinguishable, to the caller, from the
 *      failure it stands in for — so the site returns that errno, not a
 *      generic -1.
 *   2. Arm the site, do not guard a behaviour with it. A test-only branch
 *      that production can never take is dead code the compiler cannot see
 *      past; a test-only *failure* of a production path is not.
 */
#ifndef INVFS_VOL_FAULT_H
#define INVFS_VOL_FAULT_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

/*
 * 1 if THIS call is the one the armed spec selected (and disarms it), else 0.
 *
 * `static inline` with function-local statics: each translation unit gets its
 * own arming state, so two sites cannot share a countdown. Two threads racing
 * through the init below both compute the same values from the same environ;
 * there is nothing to tear.
 */
/* Arming state at file scope, so invfs_vol_fault_reload() can reach it.
 * Still exactly ONE copy per translation unit, which is the property that
 * matters: two sites cannot share a countdown.
 *
 * THREADS (2026-11-25): this header used to claim "two threads racing through
 * the init both compute the same values from the same environ; there is
 * nothing to tear". ThreadSanitizer disagreed, with six reports from one run of
 * test-put-failure: the countdown is decremented by whichever thread reaches the
 * site, and two threads can decrement it at once (a lost update means the
 * injected failure fires twice or not at all -- the suite then proves something
 * other than what it armed). The armed string was also read while another thread
 * rewrote it during a reload.
 *
 * The state is now either atomic or held under a lock, and the PRODUCTION path
 * is still one getenv() plus one relaxed load: when INVFS_FAULT is unset the
 * function returns before touching the lock, so a daemon started without the
 * variable pays nothing new. */
static _Atomic(const char *) invfs_fault_seen;
static pthread_mutex_t invfs_fault_lock = PTHREAD_MUTEX_INITIALIZER;
static char        invfs_fault_armed[32];
static long        invfs_fault_countdown;

/* Re-read the environment on the next call EVEN IF the string is unchanged.
 *
 * Without this a test arms a site, does its setup writes, then arms the same
 * value again -- and because the spec is compared by POINTER, setting an
 * identical string is not a change, so the countdown stays spent and the
 * intended call never fires. That failure is invisible: the leg goes GREEN,
 * which is how a red control ends up proving nothing.
 */
static inline void invfs_vol_fault_reload(void)
{
    atomic_store_explicit(&invfs_fault_seen, (const char *)(intptr_t)-1,
                          memory_order_release);    /* never a getenv value */
}

/* The reload above reaches only the copy in the CALLING translation unit, and
 * the state is per-TU static on purpose (two sites must not share a
 * countdown). So a test that arms a site in one core file from its own
 * translation unit cannot reload it by hand, and the obvious workaround --
 * unsetenv() then setenv() the same value -- does NOT work: unsetenv frees
 * the old string and setenv very often gets the same address back, so the
 * pointer compare sees no change and the countdown stays spent. The leg then
 * runs against the real, healthy path and goes green proving nothing.
 *
 * This is the door for that case: a definition living in the translation
 * unit that OWNS the site, reachable from a test in another one. It is
 * defined once, in src/core/vol_btree.c, beside the xattr row-read site, and
 * reaches nothing but that one TU's arming state. Like every other door
 * here it is inert in production: nothing calls it unless a test does. */
void invfs_vol_btree_fault_reload(void);

/* WP135: the same door for src/core/vol_dirs.c, which owns the  walk's
 * fault sites. Two files have needed it now, which is what a per-TU static
 * costs; a third would want the state hoisted into a tiny shared TU. */
void invfs_vol_dirs_fault_reload(void);

/* WP-IO-LOCK: the same door for src/core/volume.c (the pba_ref_rebuild site). */
void invfs_vol_volume_fault_reload(void);

/* P0-2: the same door for src/core/vol_textzone.c (registry load seam). */
void invfs_vol_textzone_fault_reload(void);

static inline int invfs_vol_fault(const char *site)
{
    const char *spec = getenv("INVFS_FAULT");

    /* Production fast path: unset or empty means unarmed, and that is decided
     * without taking a lock. If the variable was armed earlier in this process
     * (a test disarming between cases) the state is cleared once, under the
     * lock, so a later arm starts from zero. */
    if (!spec || !*spec) {
        if (atomic_load_explicit(&invfs_fault_seen, memory_order_relaxed)) {
            pthread_mutex_lock(&invfs_fault_lock);
            invfs_fault_armed[0] = 0;
            invfs_fault_countdown = 0;
            atomic_store_explicit(&invfs_fault_seen, NULL, memory_order_relaxed);
            pthread_mutex_unlock(&invfs_fault_lock);
        }
        return 0;
    }

    int fire = 0;
    pthread_mutex_lock(&invfs_fault_lock);
    if (spec != atomic_load_explicit(&invfs_fault_seen, memory_order_relaxed)) {
        invfs_fault_armed[0] = 0;
        invfs_fault_countdown = 0;
        const char *colon = strchr(spec, ':');
        if (colon && (size_t)(colon - spec) < sizeof invfs_fault_armed) {
            memcpy(invfs_fault_armed, spec, (size_t)(colon - spec));
            invfs_fault_armed[colon - spec] = 0;
            invfs_fault_countdown = strtol(colon + 1, NULL, 10);
            if (invfs_fault_countdown < 1)
                invfs_fault_countdown = 0;
        }
        atomic_store_explicit(&invfs_fault_seen, spec, memory_order_relaxed);
    }
    if (invfs_fault_countdown > 0 && strcmp(invfs_fault_armed, site) == 0) {
        if (--invfs_fault_countdown == 0) {   /* one shot, exactly once */
            fire = 1;
        }
    }
    pthread_mutex_unlock(&invfs_fault_lock);
    return fire;
}

#endif /* INVFS_VOL_FAULT_H */
