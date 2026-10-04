#!/bin/bash
# run-unit-isolated.sh — run ONE `make test` command in a private world.
#
#   bash tools/run-unit-isolated.sh <cmd> [args...]
#
# Why this exists
# ---------------
# The unit binaries write to FIXED paths under /tmp (/tmp/mt_test.qcow2,
# /tmp/evil.so, /tmp/fake_test.qcow2, /tmp/ivpack_packs_test_junk.bin, ...)
# and take their scratch directory from argv, which the test recipe hands
# them as /tmp. /tmp is shared by every worktree, by every concurrent
# `make test`, and by every e2e suite -- and on this host it is a tmpfs, so
# a run that is killed part-way through (the orchestrator interrupts
# agents; a command cap fires) leaves residue that the next run trips over:
# a green tree reading red for a reason that has nothing to do with the
# tree. run-e2e.sh already solves this for the e2e tier; it deliberately
# does NOT cover /tmp ("AGENTS worktrees live under /tmp"), which leaves
# the unit tier with no isolation at all.
#
# What it does
# ------------
# unshare -r -m, a fresh tmpfs on /dev/shm and on /tmp, then exec the
# command. Same shape as run-e2e.sh's ISOLATED mode, minus the slot locking
# (a unit command takes seconds and already has a namespace of its own, so
# there is nothing to serialize) and plus the /tmp tmpfs.
#
# The worktree itself usually DOES live under /tmp (AGENTS.md 1.3), so the
# repo is bind-mounted into the namespace BEFORE /tmp is covered, and the
# command runs from the staged copy: relative paths (bin/invf-...) and $PWD
# keep resolving, and the staged directory is the only part of the real /tmp
# a test can still see.
#
# What it does NOT fix (know this before trusting a green run)
# ------------------------------------------------------------
#   * The uid. unshare -r maps the caller to uid 0 inside the namespace, so
#     a test that branches on getuid() sees a different world. That is why
#     invf-helper_exec_test is NOT run through this wrapper (see Makefile):
#     unprivileged it SKIPs the privilege-drop checks, and under a fake root
#     those checks run and fail. That is a change of test semantics, not
#     residue, and it is not something an isolation wrapper should do.
#   * Anything outside /tmp and /dev/shm. $HOME, the worktree, and the
#     system dirs are the real ones; only their /tmp view is private.
#   * Ports, unix sockets in the real /tmp, and /dev/shm paths a test
#     hardcodes are private now, but a test writing to $HOME still leaks.
#
# Knobs
# -----
#   INVFS_TEST_NO_NS=1     run the command directly, no namespace (debugging)
#   INVFS_TEST_FORCE_NS=1  fail loudly instead of falling back when
#                          namespaces are unavailable
#   INVFS_TEST_STAGE=<dir> staging directory for the repo bind mount
#                          (default: first writable of /var/tmp, /dev/shm)
set -u

PROG=${0##*/}

usage() { echo "usage: $PROG <cmd> [args...]" >&2; exit 2; }

# ---- outer -------------------------------------------------------------
# $1 = repo dir to keep visible, rest = command. (Inner mode.)
if [ "${1:-}" = "--inner" ]; then
    shift
    repo=$1; shift
    [ $# -gt 0 ] || exit 2

    mount -t tmpfs tmpfs /dev/shm 2>/dev/null || true

    # Only a repo that lives under a directory we are about to cover needs
    # staging. A repo anywhere else is untouched by the /tmp tmpfs.
    stage=""
    case "$repo" in
    /tmp|/tmp/*|/dev/shm|/dev/shm/*|/run|/run/*|/var/tmp|/var/tmp/*)
        # /dev/shm first: it is already a private tmpfs at this point, so
        # the staging dir dies with the namespace and leaves nothing behind
        # in the real /var/tmp. The fallbacks sit on a real filesystem, so
        # they are rmdir'd on the way out.
        for root in ${INVFS_TEST_STAGE:-} /dev/shm /var/tmp; do
            [ -n "$root" ] || continue
            case "$root/" in "$repo"/*) continue ;; esac   # inside the repo
            case "$repo/" in /dev/shm/*) [ "$root" = /dev/shm ] && continue ;; esac
            cand="$root/$PROG-$$"
            mkdir -p "$cand" 2>/dev/null || continue
            if mount --bind "$repo" "$cand" 2>/dev/null; then
                  # A bind mount does not change mount options, so a noexec
                  # root stays noexec and every exec out of the staged tree
                  # fails with "Permission denied" -- which surfaces as
                  #   run-unit-isolated.sh: line 127: .../invf-<test>: Permission denied
                  #   make: *** [Makefile:988: test] Error 127
                  # i.e. as though the binary did not exist. Ask the kernel
                  # rather than guessing per image: that is how this stayed
                  # invisible while passing everywhere the root is exec-capable.
                  if findmnt -no OPTIONS -T "$cand" 2>/dev/null \
                       | tr ',' '\n' | grep -qx noexec; then
                      rmdir "$cand" 2>/dev/null || true
                      continue
                  fi
                stage=$cand
                  # Name the choice when the exec below fails. A bind mount
                  # that did not land, or a root that is not exec-capable, both
                  # surface as a two-word exec error naming the ORIGINAL repo
                  # path -- which tells the reader nothing about which of the
                  # three candidate roots was actually used.
                case "$root" in
                /dev/shm) ;;                                    # tmpfs
                *) trap 'rmdir "$cand" 2>/dev/null' EXIT ;;
                esac
                break
            fi
            rmdir "$cand" 2>/dev/null || true
        done
        if [ -z "$stage" ]; then
            echo "$PROG: could not stage $repo out of the way of the /tmp" \
                 "tmpfs; set INVFS_TEST_STAGE to a writable dir outside" \
                 "/tmp and /dev/shm" >&2
            exit 3
        fi
        ;;
    esac

    if ! mount -t tmpfs tmpfs /tmp; then
        echo "$PROG: could not mount a private tmpfs on /tmp" >&2
        exit 3
    fi

    if [ -n "$stage" ]; then
        cd "$stage" || exit 3
        PWD=$stage
        export PWD
        # An absolute argument under the old repo root means the staged
        # path now; relative paths and $PWD need no rewriting.
        n=$#
        i=0
        set -- "$@"
        while [ $i -lt $n ]; do
            arg=$1
            shift
            case "$arg" in
            "$repo"/*) set -- "$@" "$stage/${arg#"$repo"/}" ;;
            *)         set -- "$@" "$arg" ;;
            esac
            i=$((i + 1))
        done
    fi

  if [ -n "$stage" ] && [ "${INVFS_TEST_DEBUG:-0}" = 1 ]; then
      echo "$PROG: staged root=$stage (bind of $repo)" >&2
      for a in "$@"; do
          printf '%s: %s\n' "$PROG" "  arg $a exists=$([ -e "$a" ] && echo yes || echo NO)" >&2
      done
  fi
    exec "$@"
fi

[ $# -gt 0 ] || usage

ns_usable() {
    [ "${INVFS_TEST_NO_NS:-0}" = 1 ] && return 1
    command -v unshare >/dev/null 2>&1 || return 1
    # Cheap probe: user + mount namespace plus a tmpfs we can actually
    # mount. Unprivileged user namespaces are disabled on some hosts and
    # inside some containers; `make test` must not require root.
    unshare -r -m --propagation private \
        sh -c 'mount -t tmpfs tmpfs /tmp' >/dev/null 2>&1
}

if ! ns_usable; then
    if [ "${INVFS_TEST_FORCE_NS:-0}" = 1 ]; then
        echo "$PROG: user+mount namespace unavailable and" \
             "INVFS_TEST_FORCE_NS=1 (needs unprivileged user namespaces)" >&2
        exit 3
    fi
    echo "$PROG: no user+mount namespace; running UNISOLATED (set" \
         "INVFS_TEST_NO_NS=1 to make this explicit)" >&2
    exec "$@"
fi

exec unshare -r -m --propagation private \
     bash "$0" --inner "$(pwd -P)" "$@"
