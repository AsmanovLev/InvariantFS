/*
 * vol_walk.c -- the walk receipt. See vol_walk.h for why it exists.
 *
 * The whole file is the LATCH: the accessors are inline in the header
 * because a caller must reach them without a second thought, and the
 * counter is here because it has to live on the volume.
 */
#include "volume_internal.h"
#include "vol_walk.h"

/* Relaxed atomics, deliberately. The counter is a diagnostic about a
 * *previous* walk -- nothing reads it to make a decision at the moment it
 * is written -- so a lost update under a race costs a missing line in a log
 * and nothing else. Ordered atomics would imply a synchronisation this does
 * not have. __atomic_* builtins rather than <stdatomic.h>: they need no
 * feature-test macro, and they are what the -Werror plugin and helper
 * compile lines already rely on. */
static inline unsigned walk_load(const invfs_volume *v)
{
    return __atomic_load_n(&v->VOL_WALK_LATCH_FIELD, __ATOMIC_RELAXED);
}

static inline void walk_bump(invfs_volume *v, long delta)
{
    __atomic_add_fetch(&v->VOL_WALK_LATCH_FIELD, delta, __ATOMIC_RELAXED);
}

void vol_walk_init(vol_walk_t *w, invfs_volume *v, const char *what)
{
    if (!w)
        return;
    w->v = v;
    w->rc = 0;
    w->n = 0;
    w->found = 0;
    w->what = what ? what : "v3 walk";
    w->latched = 0;
    w->armed = 0;
}

void vol_walk_result(vol_walk_t *w, int rc, size_t n, size_t found)
{
    if (!w)
        return;
    w->rc = rc;
    w->n = n;
    w->found = found;
    w->armed = 1;

    if (vol_walk_complete(w) || !w->v)
        return;
    /* LATCH. A short walk nobody has claimed yet. The counter is what
     * vol_close() and the reporting surfaces read; until it is claimed or
     * abandoned, this walk is an open question about the volume, and the
     * volume is going to say so out loud rather than let a caller that
     * forgot decide that by silence. */
    w->latched = 1;
    walk_bump(w->v, 1);
}

static void walk_unlatch(vol_walk_t *w)
{
    if (!w || !w->latched)
        return;
    w->latched = 0;
    if (w->v)
        walk_bump(w->v, -1);
}

/* A receipt that was never RECORDED cannot be discharged, because nothing
 * is known about it: vol_walk_complete() on an untouched receipt is true,
 * so without this a caller whose control flow skipped the walk -- an early
 * return above the vol_walk_result() call, a branch that never got there --
 * would commit it and hear 0, "the walk was whole", about a walk that never
 * ran. That is the mirror of the case the latch covers and it is worth
 * closing the same way, because both are the same mistake: a walk end that
 * nobody thought was a walk end. */
static int walk_unarmed(const vol_walk_t *w, const char *what)
{
    if (!w || w->armed)
        return 0;
    fprintf(stderr, "vol_walk: %s was %s without ever recording a walk "
            "result. The walk did not run, and treating that as a complete "
            "walk is the same defect as ignoring a short one.\n",
            w && w->what ? w->what : "a receipt", what);
    return 1;
}

int vol_walk_commit(vol_walk_t *w)
{
    if (walk_unarmed(w, "committed"))
        return -1;
    {
        int short_walk = (w && !vol_walk_complete(w)) ? 1 : 0;
        walk_unlatch(w);
        return short_walk ? -1 : 0;
    }
}

int vol_walk_abandon(vol_walk_t *w)
{
    /* "I saw this and I am acting anyway" is a DECISION, and it is the
     * decision sites 1 and 4 of this WP are about -- so it is recorded,
     * here, at the point it is made.
     *
     * The latch cannot do this job: abandon discharges it by design, which
     * is what makes it the "not a mistake" exit. So before the discharge,
     * the shortfall is written down. The watermark sweep pass is a live
     * user of this path (it is documented fail-open, deliberately), and it
     * prints its own INCOMPLETE line -- but a caller that abandons and
     * prints nothing would otherwise leave the volume with no way at all to
     * know that a partial listing was acted on. Better one line at the
     * decision than an absence nobody can measure. */
    if (walk_unarmed(w, "abandoned"))
        return -1;
    if (w && !vol_walk_complete(w)) {
        fprintf(stderr, "vol_walk: %s stopped after %zu of this volume's "
                "entries; its caller is proceeding on the partial result "
                "anyway.\n",
                w->what ? w->what : "a v3 walk", w->found);
    }
    {
        int short_walk = (w && !vol_walk_complete(w)) ? 1 : 0;
        walk_unlatch(w);
        return short_walk ? -1 : 0;
    }
}

size_t vol_walk_reap(invfs_volume *v)
{
    unsigned n;

    if (!v)
        return 0;
    n = __atomic_exchange_n(&v->VOL_WALK_LATCH_FIELD, 0u, __ATOMIC_RELAXED);
    return (size_t)n;
}
