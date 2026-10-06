/*
 * vol_walk.h -- the status of a v3 walk is not optional.
 *
 * A v3 walk is FALLIBLE. vol_inode_get() returns -1 for a quarantined or
 * unreadable base page, walk_dir() stops there, and the walk returns -1
 * having delivered a PREFIX of the namespace. The primitives are all honest
 * about it: vol_walk, vol_iter_live_inodes, vol_dirent_scan and
 * btree_scan return a status, and the doc comment on every one of them says
 * "0 complete, -1 error".
 *
 * What was missing was anything that made a caller CONSUME that status. Five
 * sites took the walk's result and dropped it, and each one turned a partial
 * answer into a whole one on its own terms:
 *
 *   src/core/vol_sweep.c    the sweep collect reported a short live set as
 *                           COMPLETE, so the daemon printed DONE and swept
 *                           a subset of the volume
 *   src/cli/verify.c        `invf-verify --deep` printed "0 corrupt"
 *   src/cli/fuse_fs.c       the file table lost whole subtrees, and the
 *                           user's `ls` got ENOENT for files still on disk
 *   tools/invf-sweep.c      warned, then swept anyway
 *   src/core/vol_btree.c    vol_name_of answered "no such name" instead
 *                           of "the tree could not be read"
 *
 * Five `if` statements would fix five sites and change nothing about the
 * class: a sixth walk added next month has nothing to satisfy, and the five
 * have no shared contract to disagree about. The class stops growing only if
 * the short-walk state TRAVELS WITH THE DATA, so that a count cannot be
 * produced from a walk that stopped.
 *
 * That is the vol_walk_t below. It is a RECEIPT: the count, and whether the
 * walk finished, in one value the caller already has to carry.
 *
 *     vol_walk_t w;
 *     vol_walk_init(&w, g_vol, "vol_walk");
 *     vol_walk_result(&w, vol_walk(g_vol, cb, &c), c.n, c.n);
 *     if (vol_walk_commit(&w) != 0)
 *         return -EIO;            // the walk did not finish
 *     ... now, and only now, is w.n a safe number ...
 *
 * WHY A CALLER CANNOT IGNORE IT
 * ==============================
 *
 * Two mechanisms, because either alone leaves a hole, and which hole is
 * which matters:
 *
 *  1. warn_unused_result on the two DISCHARGE calls. Measured on the
 *     toolchain this builds with (gcc 14, -std=gnu11 -Wall): casting the
 *     result to void does NOT suppress it -- `(void)vol_walk_commit(&w)`
 *     still warns -Wunused-result. That was checked rather than assumed,
 *     because the usual claim is that a void cast silences the attribute,
 *     and if that were true here the four `(void)vol_walk(...)` sites
 *     would be exactly the case this mechanism misses. It is not, on this
 *     compiler. What it DOES miss is a caller that BINDS the result and
 *     never reads it: `int rc = vol_walk_commit(&w);` and then nothing. The
 *     compiler cannot see through that, and neither can a code review.
 *
 *  2. the LATCH. A short walk that is neither committed nor explicitly
 *     abandoned leaves v->walk_unclaimed incremented -- a relaxed-atomic
 *     counter on the volume. The mark is read from places the forgetting
 *     caller does not control: vol_close() names it, and so can the sweep's
 *     DONE line and the FUSE table rebuild. A caller that drops the result
 *     on the floor cannot get silence; it gets a line that says the walk
 *     stopped and nobody claimed it.
 *
 *     the return value is:                       the latch reports at
 *     ---------------------------------------    -----------------------
 *     left as a bare expression                    yes (build warning)
 *     cast to void                                  yes (build warning)
 *     bound to a variable and never read            YES  <-- the hole (1) leaves
 *     stored in a struct and never read             YES  <-- and this
 *
 * So the honest summary: a bare call or a void-cast call is a build
 * warning, and a caller that goes out of its way to swallow the value into
 * a local is caught at vol_close instead. The one that neither reaches is a
 * caller that swallows it AND is the last thing to touch the volume without
 * closing it -- a daemon that is killed. That one is a real residual, it is
 * named here rather than papered over, and closing it needs a
 * __attribute__((error)) build, which this tree does not use.
 *
 * WHAT THIS MUST NOT DO
 * =====================
 *
 * A short walk is not the only shape an answer can take. A volume with no
 * live inodes, and a namespace with no entries, are COMPLETE walks of length
 * ZERO -- the right answer, not a failure, and one of the common ones (a
 * fresh volume, a volume whose files are all gone). vol_walk_n() returns 0
 * for those and vol_walk_commit() returns 0. The sentinel for "this number
 * is not a count" is (size_t)-1, never 0. Confusing the two would trade a
 * silent wrong answer for a loud false alarm, which is a different bug and a
 * worse one for whoever has to read the log at 3am.
 */
#ifndef INVFS_VOL_WALK_H
#define INVFS_VOL_WALK_H

#include <stddef.h>
#include <stdint.h>

typedef struct invfs_volume invfs_volume;

/* The value vol_walk_n() returns for a walk that did not finish. Chosen so
 * it cannot be mistaken for a count in a %zu, in a comparison against a
 * capacity, or in a loop bound -- which is the point of it. */
#define VOL_WALK_NO_COUNT ((size_t)-1)

typedef struct vol_walk {
    invfs_volume *v;        /* the volume latched with an unclaimed short walk */
    int    rc;              /* 0 = reached the end; <0 = STOPPED early         */
    size_t n;               /* entries the caller was handed                   */
    size_t found;           /* entries the walk SAW; > n means it was truncated */
    const char *what;       /* the walk's name, for the diagnostic             */
    int    latched;         /* this receipt is charged to the volume           */
    int    armed;           /* a result has been recorded on this receipt     */
} vol_walk_t;

/* Dropping one of these is a build warning. Note the tree is built -Wall
 * without -Werror, so "warning" is the teeth actually present -- and that is
 * only half of them, because `(void)f()` silences the attribute outright
 * (see the latch, and the table in the comment above).
 *
 * The attribute is on the two DISCHARGE calls, which every caller genuinely
 * has to branch on. It is deliberately NOT on vol_walk_result(): the caller
 * has no use for a return value there, and inventing one would mean putting
 * an ignored expression at my own call sites that says exactly what this
 * whole file exists to stop. So the recording call is covered by the latch
 * alone -- which is the honest answer, and is why the latch is not
 * optional. */
#if defined(__GNUC__)
#  define VOL_WALK_WUR __attribute__((warn_unused_result))
#else
#  define VOL_WALK_WUR
#endif

void vol_walk_init(vol_walk_t *w, invfs_volume *v, const char *what);

/* Record the walk's outcome. `n` is what the caller was handed, `found` is
 * what the walk saw (pass `n` for a walk with no truncation signal, which is
 * every walk that is not vol_collect_sweepables_ex). Either of these makes
 * the walk SHORT:
 *
 *   rc < 0        the walk stopped: n is a strict prefix of a set that could
 *                 not be enumerated (a quarantined base page, an OOM, a
 *                 callback error)
 *   found > n     the walk was truncated by the caller's own cap
 *
 * and latches the volume when the walk is short. */
void vol_walk_result(vol_walk_t *w, int rc, size_t n, size_t found);

/* 1 when the walk is COMPLETE: it reached the end and nothing truncated it.
 * rc == 0 && n == 0 is complete and empty -- a real answer. */
static inline int vol_walk_complete(const vol_walk_t *w)
{
    return w && w->rc == 0 && w->found <= w->n;
}

/* The count -- but only from a complete walk. VOL_WALK_NO_COUNT otherwise,
 * including for a walk that was truncated by the caller's cap. The reason it
 * is a separate function and not a field access is exactly that: a field can
 * be read by anyone, at any time, with no way to say "I have checked". */
static inline size_t vol_walk_n(const vol_walk_t *w)
{
    if (!w)
        return VOL_WALK_NO_COUNT;
    return vol_walk_complete(w) ? w->n : VOL_WALK_NO_COUNT;
}

/* "every entry the walk saw" -- the honest upper form. It is a lower bound
 * on a short walk (the walk stopped at the first failure, so there may be
 * more), which is why vol_walk_commit() is the gate and not this. */
static inline size_t vol_walk_seen(const vol_walk_t *w)
{
    return w ? w->found : 0;
}

/* Discharge the receipt. 0 = the walk finished, -1 = it did not (and this
 * caller now knows it). Clears the latch. */
VOL_WALK_WUR int vol_walk_commit(vol_walk_t *w);

/* Decline to discharge it, having SEEN it: the caller has decided a short
 * walk is worth acting on and is saying so out loud. This is the escape
 * hatch, and it is named and explicit precisely so that "I did not look" and
 * "I looked and accepted this" are not the same call.
 *
 * It RECORDS that decision on stderr before discharging the latch, because
 * the latch cannot: abandon clears it by design, which is what makes it the
 * not-a-mistake exit. Without the line, a caller that proceeds on a partial
 * listing and prints nothing of its own leaves the volume with no way to
 * know it happened. */
VOL_WALK_WUR int vol_walk_abandon(vol_walk_t *w);

/* BOTH discharge calls reject a receipt that never had vol_walk_result()
 * called on it, and return -1. vol_walk_complete() is true on an untouched
 * receipt, so without that check a caller whose control flow skipped the
 * walk -- an early return above the recording, a branch that never reached
 * it -- would hear "the walk was whole" about a walk that never ran. Same
 * mistake as ignoring a short one, and closed the same way. */

/* How many short walks this volume has run and not accounted for. Read and
 * cleared; the caller is expected to say so. Returns 0 when there are none,
 * which is the overwhelmingly common case and costs one relaxed load. */
size_t vol_walk_reap(invfs_volume *v);

/* The volume struct field the latch lives in. Declared here so the core TUs
 * that include volume_internal.h and the one that does not cannot drift. */
#define VOL_WALK_LATCH_FIELD walk_unclaimed

#endif /* INVFS_VOL_WALK_H */
