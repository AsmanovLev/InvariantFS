#!/bin/bash
# lib-scratch.sh — one answer to "where does a suite put its work".
#
#   . tools/lib-scratch.sh
#   W=$(invfs_scratch_root)/my-suite-$$
#
# Why this exists
# ---------------
# Twelve suites resolve their scratch root with a chain like
#
#     WORK=${INVFS_FOO_WORK:-/srv/bench/foo-$$}
#
# and /srv/bench is a directory on the author's bench disk. It is not on a CI
# runner, not in a container, and not on a fresh checkout, so on those hosts
# the suite dies at `mkdir: cannot create directory '/srv/bench':
# Permission denied` -- or, for test-packaging.sh, at
# `${INVFS_E2E_SCRATCH:-/srv/bench}` -- before running a single leg, and
# `make test` returns non-zero for a reason that has nothing to do with the
# tree. Three of these run from `make test` on every push.
#
# And the answer must NOT simply be /tmp. `make test` is honest about that
# one: /tmp is a 3.8 GiB tmpfs on this box, so a suite that needs real space
# (or whose whole point is that drop_caches can evict its pages) is
# measuring the wrong filesystem. test-flakey.sh learned this the hard way
# and grew its own `disk_work_root()` for it; this is that picker, shared,
# so the twelve chains stop disagreeing with each other and with it.
#
# The chain, in order:
#
#   1. $INVFS_E2E_SCRATCH           an explicit root (CI sets this)
#   2. $INVFS_UNIT_SCRATCH          an explicit root (WP204's knob)
#   3. the first WRITABLE non-tmpfs of /srv/bench /srv /var/tmp /opt /var/lib
#   4. /tmp                         nothing disk-backed exists; say so once
#
# Every suite keeps its own per-suite override variable (INVFS_FOO_WORK and
# friends) in front of this, so nothing that is pinned today moves.
#
# Sourcing is cheap and idempotent; it defines one function and no state.
# POSIX sh, no `set -e` side effects (a suite that sources this must keep its
# own shell options).

# Is $1 a directory we can write to?
_scratch_writable() {
    [ -d "$1" ] || return 1
    [ -w "$1" ] || return 1
    return 0
}

# Is $1 a tmpfs? A scratch that lives on one is charged to RAM, which is the
# whole reason this picker exists; answer "no" when we cannot tell, because a
# wrong "no" (using tmpfs) is recoverable and a wrong "yes" (refusing every
# candidate and landing on /tmp anyway) is not.
_scratch_is_tmpfs() {
    case "$(stat -f -c %T "$1" 2>/dev/null)" in
        tmpfs) return 0 ;;
        *)      return 1 ;;
    esac
}

# Echo the root. Memoised in a variable so a suite that calls this in a loop
# does not stat the world each time; the value cannot change within a run.
invfs_scratch_root() {
    if [ -n "${_INVFS_SCRATCH_ROOT:-}" ]; then
        printf '%s\n' "$_INVFS_SCRATCH_ROOT"
        return 0
    fi
    if [ -n "${INVFS_E2E_SCRATCH:-}" ]; then
        _INVFS_SCRATCH_ROOT="$INVFS_E2E_SCRATCH"
    elif [ -n "${INVFS_UNIT_SCRATCH:-}" ]; then
        _INVFS_SCRATCH_ROOT="$INVFS_UNIT_SCRATCH"
    else
        _INVFS_SCRATCH_ROOT=""
        for c in /srv/bench /srv /var/tmp /opt /var/lib; do
            _scratch_writable "$c" || continue
            _scratch_is_tmpfs "$c" && continue
            _INVFS_SCRATCH_ROOT="$c"
            break
        done
        if [ -z "$_INVFS_SCRATCH_ROOT" ]; then
            # Nothing disk-backed and writable. /tmp is then the only place
            # left, and a suite that needs more than it has will say so in
            # its own terms -- this only names the choice once.
            _INVFS_SCRATCH_ROOT=/tmp
            if [ -z "${_INVFS_SCRATCH_QUIET:-}" ]; then
                echo "note: no writable non-tmpfs scratch root found" >&2
                echo "      (/srv/bench /srv /var/tmp /opt /var/lib); using" >&2
                echo "      /tmp, which is $(df -h /tmp 2>/dev/null | awk 'NR==2{print $2}')" >&2
                echo "      and is RAM. Set INVFS_E2E_SCRATCH=<dir> to choose." >&2
            fi
        fi
    fi
    printf '%s\n' "$_INVFS_SCRATCH_ROOT"
}
