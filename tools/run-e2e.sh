#!/bin/bash
# run-e2e.sh — parallel-safe e2e runner.
#
#   bash tools/run-e2e.sh <suite.sh>       run one suite (foreground)
#   bash tools/run-e2e.sh --bg <suite.sh>  background; returns immediately
#   bash tools/run-e2e.sh --wait           block until all bg suites finish
#
# Two execution modes, chosen per suite:
#
#   * ISOLATED (default for suites that do not need root/sudo/loop and do
#     not touch shared /tmp paths): the suite runs inside a private
#     user+mount namespace with its own tmpfs on /dev/shm. Two invocations
#     of the SAME suite in different worktrees (e.g. parallel subagents)
#     therefore cannot collide on image names, and run concurrently.
#
#   * LOCKED (suites matching sudo|losetup|id -u|EUID|mount -o loop|/tmp/,
#     or when namespaces are unavailable/disabled): serialized on the legacy
#     global lock, run as the invoking user. /tmp is NOT isolated because
#     AGENTS worktrees live under /tmp.
#
# Concurrency is bounded by INVFS_E2E_SLOTS (default 4) so parallel suites
# do not exhaust RAM/CPU. Every holder publishes attribution:
#   /tmp/invfs-e2e-slots/slot<N>.info   (isolated)
#   /tmp/invfs-e2e.lock.info            (locked)
#
# Knobs: INVFS_E2E_SLOTS=N  INVFS_E2E_NO_NS=1  INVFS_E2E_FORCE_NS=1
#        INVFS_E2E_FORCE_LOCK=1
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
LOCK="${INVFS_E2E_LOCK:-/tmp/invfs-e2e.lock}"
INFO="${INVFS_E2E_INFO:-/tmp/invfs-e2e.lock.info}"
SLOTDIR="${INVFS_E2E_SLOTDIR:-/tmp/invfs-e2e-slots}"
BGDIR="${INVFS_E2E_BGDIR:-/tmp/invfs-e2e-bg}"
BGDONE="$BGDIR/done"
NSLOTS="${INVFS_E2E_SLOTS:-4}"

# --- admin tool resolution (WP655) ----------------------------------------
#
# mkfs.*, losetup, blockdev and mount.* are installed under /sbin or
# /usr/sbin. A non-root invoking user's PATH omits both BY DESIGN (Debian),
# so a bare `command -v mkfs.ntfs` fails for an INSTALLED ntfs-3g: tools/
# test-ntfs.sh:90 read that as "not installed" and exited 1 without running a
# leg, and tools/ebpf/setup-mounts.sh died at 127 until it resolved through
# `command -v X || echo /sbin/X` (2cbf86b). A refusal that reads as a result
# is the shape AGENTS 1.6 forbids for reports, so the rule lives here, in the
# one place every suite passes through:
#
#   * the admin roots join the suite's PATH. No privilege is granted -- a
#     LOCKED suite already runs as the invoking user and still needs sudo to
#     loop-mount anything;
#   * e2e_admin_path / e2e_need_admin resolve a tool through PATH *and* the
#     admin roots, and on failure name the PACKAGE. "absent" (install it) and
#     "off PATH" (resolved, here is where) are different remedies and the
#     message says which one it is;
#   * the same rule is reachable as `run-e2e.sh --need <tool>...` and
#     `run-e2e.sh --path <tool>`, so a suite invoked directly instead of
#     through the runner uses it rather than growing a second copy.
#
# The roots are overridable so the negative control (WP655 validation 5) can
# mask them: INVFS_E2E_ADMIN_ROOTS=/nonexistent bash tools/run-e2e.sh --need losetup
ADMIN_ROOTS="${INVFS_E2E_ADMIN_ROOTS:-/usr/sbin:/sbin}"
export ADMIN_ROOTS

e2e_admin_roots_add() {
    local d add=""
    for d in ${ADMIN_ROOTS//:/ }; do
        [ -n "$d" ] || continue
        case ":$PATH:" in *":$d:"*) continue ;; esac
        add="$add:$d"
    done
    [ -z "$add" ] || PATH="${add#:}:$PATH"
    export PATH
    return 0
}

# e2e_admin_path <tool> -- print its path. Searches PATH, then the admin
# roots explicitly (a direct caller may have left them off). 1 = not found.
e2e_admin_path() {
    local t=$1 p d
    p=$(command -v -- "$t" 2>/dev/null) && [ -n "$p" ] && { printf '%s\n' "$p"; return 0; }
    for d in ${ADMIN_ROOTS//:/ }; do
        [ -n "$d" ] || continue
        if [ -x "$d/$t" ]; then printf '%s\n' "$d/$t"; return 0; fi
    done
    return 1
}

# The package that SHIPS an admin tool, so a refusal names a remedy instead
# of claiming an installed package is missing. Unknown stays unknown: a wrong
# package name is worse than "package unknown".
e2e_admin_package() {
    case "$1" in
        mkfs.ntfs|mkntfs|ntfs-3g|ntfsfix|mount.ntfs*|ntfsclone|ntfsinfo)
            echo ntfs-3g ;;
        mkfs.ext[234]|mkfs.ext4dev|mke2fs|debugfs|e2fsck|dumpe2fs|tune2fs)
            echo e2fsprogs ;;
        mkfs.vfat|mkfs.fat|fsck.vfat|mkdosfs) echo dosfstools ;;
        mkfs.exfat|fsck.exfat)                echo exfatprogs ;;
        mkfs.xfs|xfs_*)                       echo xfsprogs ;;
        mkfs.btrfs|btrfs)                     echo btrfs-progs ;;
        losetup|blockdev|mount|umount|findmnt|lsblk|blkid|setsid|flock)
            echo util-linux ;;
        dmsetup|dmsetup*)                     echo lvm2 ;;
        parted|partprobe)                     echo parted ;;
        *)                                    echo "unknown (not an admin tool this runner maps)" ;;
    esac
}

# e2e_need_admin <tool>... -- the precondition. Refuses (1) unless every
# named tool resolves, and says which diagnosis applies to each failure.
e2e_need_admin() {
    local t p pkg onpath offpath=0 bad=0
    for t in "$@"; do
        if p=$(e2e_admin_path "$t"); then
            onpath=$(command -v -- "$t" 2>/dev/null || true)
            if [ -z "$onpath" ]; then
                # Present, merely off PATH: resolved, not refused.
                offpath=1
                printf '  note: %s is not on PATH; resolved at %s (package: %s)\n' \
                    "$t" "$p" "$(e2e_admin_package "$t")" >&2
            fi
            continue
        fi
        bad=1
        pkg=$(e2e_admin_package "$t")
        printf 'FAIL: %s: ABSENT, not merely off PATH -- searched PATH (%s) and %s\n' \
            "$t" "$PATH" "$ADMIN_ROOTS" >&2
        printf '      package: %s  (install it: apt-get install %s)\n' "$pkg" "$pkg" >&2
    done
    [ "$bad" = 0 ] || printf '      a suite that cannot run its legs must refuse like this, not report a result\n' >&2
    return $bad
}

# --need / --path: the same rule for a suite invoked outside the runner.
e2e_admin_query() {        # $1 = need|path, rest = tools
    local mode=$1 t rc=0
    shift
    [ $# -gt 0 ] || { echo "usage: run-e2e.sh --$mode <tool>..." >&2; return 2; }
    for t in "$@"; do
        if [ "$mode" = path ]; then
            e2e_admin_path "$t" || { echo "$t: absent (package: $(e2e_admin_package "$t"))" >&2; rc=1; }
        else
            e2e_need_admin "$t" || rc=1
        fi
    done
    return $rc
}

e2e_admin_roots_add
export -f e2e_admin_roots_add e2e_admin_path e2e_admin_package e2e_need_admin e2e_admin_query 2>/dev/null || true

ns_usable() {
    [ "${INVFS_E2E_NO_NS:-0}" = 1 ] && return 1
    command -v unshare >/dev/null 2>&1 || return 1
    unshare -rm sh -c 'mount -t tmpfs tmpfs /dev/shm 2>/dev/null' >/dev/null 2>&1
}
NS_OK=0
ns_usable && NS_OK=1

# Suites that must run LOCKED: they need the real uid/root, or they touch
# shared /tmp paths (not isolated, since worktrees live under /tmp).
suite_needs_root() {
    grep -qE '(^|[^[:alnum:]_])(sudo|losetup)([^[:alnum:]_]|$)|id -u|EUID|mount -o loop|/tmp/' \
        "$1" 2>/dev/null
}

mode_for() {
    if [ "${INVFS_E2E_FORCE_LOCK:-0}" = 1 ]; then echo locked; return; fi
    if [ "${INVFS_E2E_FORCE_NS:-0}" = 1 ]; then echo ns; return; fi
    if [ "$NS_OK" = 1 ] && ! suite_needs_root "$1"; then echo ns; else echo locked; fi
}

who_running() {
    local f
    for f in "$SLOTDIR"/slot*.info; do
        [ -e "$f" ] || continue
        echo "  $(basename "$f" .info): $(cat "$f" 2>/dev/null)"
    done
    [ -f "$INFO" ] && echo "  global-lock: $(cat "$INFO" 2>/dev/null)"
    true
}

slot_acquire() {           # sets SLOT; holds fd 8 for the run
    local i
    mkdir -p "$SLOTDIR"
    while :; do
        for i in $(seq 0 $((NSLOTS - 1))); do
            exec 8>"$SLOTDIR/slot$i.lock"
            if flock -n 8; then SLOT=$i; return 0; fi
            exec 8>&-
        done
        sleep 2
    done
}
slot_release() { rm -f "$SLOTDIR/slot$SLOT.info"; exec 8>&- 2>/dev/null; }
slot_mark() {              # $1 suite
    local branch
    branch=$(git -C "$REPO" branch --show-current 2>/dev/null)
    echo "suite=$(basename "$1") pid=$$ branch=${branch:-?} agent=${INVFS_E2E_AGENT:-?} started=$(date -Is) mode=isolated" \
        > "$SLOTDIR/slot$SLOT.info"
}

run_suite() {              # $1 suite, $2 mode
    # WP655: the admin roots are on this PATH already (e2e_admin_roots_add at
    # the top, exported), and the resolvers ride along as exported functions
    # so a suite never re-derives the rule. Nothing about privilege changes.
    e2e_admin_roots_add

    # The system codecpack directory is OFF, exactly as `make test` runs the
    # unit suite (Makefile, "run the unit suite with the system codecpack
    # directory off"). A pack installed under /usr/lib/invfs/codecpacks
    # claims content a builtin lane would otherwise classify -- measured on
    # this host: a 3 MB incompressible file is stamped GENERIC_GUARD (6) by
    # the installed `rawdisk` pack where the builtin floor stamps
    # UNCOMPRESSIBLE (1) -- so a suite that asserts a classification goes red
    # on a provisioned host and green on a bare one, for a reason that has
    # nothing to do with the tree under test. The suites assert the BUILTIN
    # lanes, which is what running with the system packs off means.
    if [ "$2" = ns ]; then
        # private mount ns: fresh tmpfs on /dev/shm; mounts vanish when the
        # namespace exits (even a crashed FUSE mount). /tmp is NOT touched.
        unshare -rm --propagation private bash -c '
            mount -t tmpfs tmpfs /dev/shm 2>/dev/null || true
            export INVFS_CODECPACKS_SYS=0
            exec bash "$1"
        ' bash "$1"
    else
        INVFS_CODECPACKS_SYS=0 bash "$1"
    fi
}

run_locked() {             # $1 suite; holds fd 9
    exec 9>"$LOCK"
    if ! flock -w 10 9; then
        who_running
        echo "waiting for the e2e global lock..."
        flock -w 7200 9 || { echo "e2e lock timeout"; return 124; }
    fi
    echo "suite=$(basename "$1") pid=$$ branch=$(git -C "$REPO" branch --show-current 2>/dev/null) agent=${INVFS_E2E_AGENT:-?} started=$(date -Is) mode=locked" > "$INFO"
    run_suite "$1" locked
    local rc=$?
    rm -f "$INFO"
    return $rc
}

run_one() {                # $1 suite
    local suite="$1" mode rc
    mode=$(mode_for "$suite")
    if [ "$mode" = ns ]; then
        slot_acquire
        slot_mark "$suite"
        run_suite "$suite" ns
        rc=$?
        slot_release
        return $rc
    fi
    run_locked "$suite"
}

case "${1:-}" in
--need|--path)
    # The precondition rule of this file, for a suite invoked directly.
    # See the "admin tool resolution" block above.
    e2e_admin_query "${1#--}" "${@:2}"
    exit $?
    ;;
--bg)
    shift
    suite="${1:-}"
    [ -n "$suite" ] || { echo "usage: run-e2e.sh --bg <suite.sh>"; exit 2; }
    mkdir -p "$BGDIR"
    name=$(basename "$suite" .sh)
    log="$BGDIR/$name.$$.log"
    (
        # BASHPID, not $$: in a subshell $$ is the (already-exited) parent,
        # which would make --wait treat the job as dead immediately.
        pid=$BASHPID
        echo "$pid" > "$BGDIR/$name.$pid.pid"
        run_one "$suite" > "$log" 2>&1
        rc=$?
        echo "$name rc=$rc log=$log" >> "$BGDONE"
        rm -f "$BGDIR/$name.$pid.pid"
        exit $rc
    ) &
    echo "bg started: $name (log: $log; wait: bash tools/run-e2e.sh --wait)"
    ;;
--wait)
    mkdir -p "$BGDIR"
    touch "$BGDONE"
    sleep 1   # let just-started --bg jobs register their pidfiles
    while :; do
        alive=0
        for pf in "$BGDIR"/*.pid; do
            [ -e "$pf" ] || continue
            kill -0 "$(cat "$pf")" 2>/dev/null && { alive=1; break; }
        done
        [ "$alive" = 0 ] && break
        sleep 3
    done
    echo "== background results =="
    cat "$BGDONE"
    ;;
"")
    echo "usage: run-e2e.sh [--bg|--wait] <suite.sh>"; exit 2
    ;;
*)
    run_one "$1"
    ;;
esac
