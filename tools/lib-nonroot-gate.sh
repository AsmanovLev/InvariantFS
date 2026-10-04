#!/bin/bash
# lib-nonroot-gate.sh — WP227 regression guard, shared by the boot harnesses.
#
# WHY THIS EXISTS. Every boot harness in this repo asserts as root, over SSH or
# over the serial console. That is why a guest whose root filesystem was mounted
# WITHOUT `allow_other` passed every test in the tree from WP66 until
# 2026-10-04: the FUSE mount belonged to uid 0, so every unprivileged uid got
# EACCES on every file, and a guest that booted perfectly, reached
# graphical.target and served ssh as root was completely unusable to anyone
# else. No root-only assertion can express that failure.
#
# So this gate deliberately does not run as root. It drops to an unprivileged
# uid and asks three questions a root cannot distinguish:
#
#   1. can it READ  a root-owned file on / ?
#   2. can it EXEC  a world-executable file on / ?
#   3. can it LIST  a directory on / ?
#
# plus a direct check that / carries allow_other, so the failure names its cause
# rather than just its symptom.
#
# USAGE
#   . tools/lib-nonroot-gate.sh
#   nonroot_gate <ssh-command-prefix...> -- <remote-shell> <exec-target> <read-target> <list-dir>
#
# For example, from a harness whose SSH helper is `ssh_guest`:
#   nonroot_gate ssh_guest -- /bin/sh /bin/dash /etc/os-release /usr/bin
#
# `nonroot_gate` calls `fail` (harness convention) on a real failure and returns
# non-zero; it prints "GUARD-SKIP" and returns 0 when the guest lacks the users
# or tools needed to run the check at all -- callers are expected to surface that
# as NOT a pass rather than silence.

nonroot_gate() {
    local -a ssh_cmd=()
    while [ "$1" != "--" ]; do ssh_cmd+=("$1"); shift; done
    shift                                   # drop the --
    local sh_bin="${1:-/bin/sh}" exe="${2:-/bin/dash}" rd="${3:-/etc/os-release}" dir="${4:-/usr/bin}"

    local out
    # NOTE ON QUOTING: this whole script is single-quoted because it crosses an
    # ssh boundary, so it cannot contain single quotes. An earlier draft ran
    #   awk "\$2==\"/\"{print \$4}" /proc/mounts
    # through ssh and it arrived as the literal `awk $2==/{print $4} /proc/mounts`.
    # Everything below therefore uses only double quotes, and the mount line is
    # selected on the HOST side where quoting is not in play.
    out=$("${ssh_cmd[@]}" '
        id -u nobody >/dev/null 2>&1 || { echo "GUARD-SKIP no nobody user"; exit 0; }
        if command -v su >/dev/null 2>&1; then
            as_nobody() { su -s /bin/sh nobody -c "$1" 2>&1; }
            DROPPED=su
        elif command -v setpriv >/dev/null 2>&1; then
            as_nobody() { setpriv --reuid=nobody --regid=nogroup --clear-groups /bin/sh -c "$1" 2>&1; }
            DROPPED=setpriv
        else
            echo "GUARD-SKIP neither su nor setpriv"; exit 0
        fi
        echo "    drop mechanism: $DROPPED"
        printf "    read  RD_FILE : "; as_nobody "cat RD_FILE" >/dev/null 2>&1 && echo PASS || echo FAIL
        printf "    exec  EXE_BIN : "; as_nobody "EXE_BIN -c :" >/dev/null 2>&1 && echo PASS || echo FAIL
        printf "    list  LIST_DIR: "; as_nobody "ls LIST_DIR" >/dev/null 2>&1 && echo PASS || echo FAIL
        echo "    mountopts-for-slash: $(cat /proc/mounts | grep " / ")"
    ' 2>&1 | sed -e "s|RD_FILE|$rd|g" -e "s|EXE_BIN|$exe|g" -e "s|LIST_DIR|$dir|g" -e "s|/bin/sh|$sh_bin|g")

    printf '%s\n' "$out"
    if printf '%s' "$out" | grep -q 'GUARD-SKIP'; then
        nonroot_gate_skipped=1
        return 0
    fi
    nonroot_gate_skipped=0
    printf '%s' "$out" | grep -q "read  $rd : PASS" || {
        fail "WP227: an unprivileged user cannot read $rd on / -- the root filesystem is mounted without allow_other, or is otherwise root-only"; }
    printf '%s' "$out" | grep -q "exec  $exe : PASS" || \
        fail "WP227: an unprivileged user cannot exec $exe on /"
    printf '%s' "$out" | grep -q "list  $dir: PASS" || \
        fail "WP227: an unprivileged user cannot list $dir on /"
    printf '%s' "$out" | sed -n 's/^.*mountopts-for-slash: //p' | grep -q 'allow_other' || \
        fail "WP227: / is not mounted with allow_other -- see INCIDENTS.md WP227"
    return 0
}
