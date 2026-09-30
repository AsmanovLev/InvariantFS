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

/*
 * 1 if THIS call is the one the armed spec selected (and disarms it), else 0.
 *
 * `static inline` with function-local statics: each translation unit gets its
 * own arming state, so two sites cannot share a countdown. Two threads racing
 * through the init below both compute the same values from the same environ;
 * there is nothing to tear.
 */
static inline int invfs_vol_fault(const char *site)
{
    static const char *seen;      /* the spec string the state was built from */
    static char armed[32];
    static long countdown;
    const char *spec = getenv("INVFS_FAULT");

    if (spec != seen) {
        seen = spec;
        armed[0] = 0;
        countdown = 0;
        if (spec && *spec) {
            const char *colon = strchr(spec, ':');
            if (colon && (size_t)(colon - spec) < sizeof armed) {
                memcpy(armed, spec, (size_t)(colon - spec));
                armed[colon - spec] = 0;
                countdown = strtol(colon + 1, NULL, 10);
                if (countdown < 1)
                    countdown = 0;
            }
        }
    }
    if (!countdown || strcmp(armed, site) != 0)
        return 0;
    if (--countdown > 0)
        return 0;
    countdown = 0;                /* one shot */
    return 1;
}

#endif /* INVFS_VOL_FAULT_H */
