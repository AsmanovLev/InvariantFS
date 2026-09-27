#!/bin/bash
# tools/test-initramfs-bootstrap.sh — WP125
#
# The InvariantFS initramfs bootstrap: build it, then prove it can reach a
# real volume. This is the test that stands in for a VM boot, because a
# recipe nobody has booted is exactly the kind of thing that rots.
#
#   1. build: tools/mkinitramfs.sh produces a cpio
#   2. structure: the verifier's static checks pass
#   3. live: the verifier mounts a real pre-existing InvariantFS volume from
#      inside a chroot containing NOTHING but the cpio, reads the installer's
#      payload back bit-exact, writes a new file bit-exact, and then runs the
#      offline rescue path (invf-cat, invf-verify --deep, invf-fsck)
#   4. NEGATIVE cases. A verifier that cannot fail is worthless, so:
#      4a. dropping invf-fsck from the image must be caught
#      4b. neutering /init must be caught
#      4c. a dynamic tool whose .so is missing must be caught
#
# Needs: /dev/fuse and user namespaces (unshare -rm). Skips the live phases
# with a clear "skipped", never a silent pass, when they are unavailable.
#
# Run: bash tools/test-initramfs-bootstrap.sh
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="${INVFS_TEST_BASE:-/srv/bench}"
[ -d "$BASE" ] || BASE=/tmp
mkdir -p "$BASE" || BASE=/tmp
W=$(mktemp -d "$BASE/invfs-wp125-e2e.XXXXXX") || exit 2
IMG="$W/initramfs.cpio.gz"
trap 'rm -rf "$W"' EXIT

pass=0; fail=0
ok()  { printf 'ok   - %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL - %s\n' "$1" >&2; fail=$((fail + 1)); }
skip(){ printf 'skip - %s\n' "$1"; }

echo "== 1. build =="
if out=$(cd "$ROOT" && INVFS_INITRAMFS_OUT="$IMG" bash tools/mkinitramfs.sh 2>&1); then
    ok "tools/mkinitramfs.sh built $(basename "$IMG") ($(wc -c < "$IMG") bytes)"
    printf '       %s\n' "$(printf '%s\n' "$out" | tail -1)"
else
    bad "tools/mkinitramfs.sh failed"
    printf '%s\n' "$out" | tail -20 >&2
    printf '\n%d passed, %d failed\n' "$pass" "$fail"
    exit 1
fi

# Repack a variant of the image with a mutation applied, to prove the
# verifier actually detects breakage -- and that it is the EXPECTED check
# which fires. Two guards against a vacuous test:
#   * the mutation must be caught by the named check, so we grep the log for
#     the specific message rather than accepting any non-zero exit;
#   * --no-mount isolates the structure phase, so a mutation cannot "pass"
#     because the unrelated live-volume proof happened to fail.
mutate() {  # <desc> <expected-log-substring> <shell-snippet-run-in-$tree>
    local desc="$1" want="$2" snippet="$3" t="$W/mut" out="$W/mut.cpio.gz"
    rm -rf "$t"; mkdir -p "$t"
    (cd "$t" && gzip -dc "$IMG" | cpio -idm --quiet) || { bad "$desc: unpack"; return 1; }
    (cd "$t" && eval "$snippet") || { bad "$desc: mutation"; return 1; }
    (cd "$t" && find . | cpio -o -H newc --quiet | gzip -1) > "$out"
    if (cd "$ROOT" && bash tools/verify-initramfs.sh --initramfs "$out" \
            --workdir "$BASE" --no-mount >"$W/mut.log" 2>&1); then
        bad "$desc: the verifier PASSED a broken image"
        return 1
    fi
    if ! grep -qF "$want" "$W/mut.log"; then
        bad "$desc: verifier failed, but not on the expected check"
        printf '       wanted log to contain: %s\n' "$want" >&2
        tail -5 "$W/mut.log" >&2
        return 1
    fi
    ok "$desc is caught by the expected check"
    return 0
}

echo
echo "== 2+3. self-verification (structure + live volume) =="
# The live half needs FUSE and a user namespace. If the environment cannot
# provide them, say so loudly and run the structure half only -- never report
# a pass for a proof that did not happen.
CAN_LIVE=1
[ -e /dev/fuse ] || CAN_LIVE=0
unshare -rm true 2>/dev/null || CAN_LIVE=0
if [ "$CAN_LIVE" = 0 ]; then
    skip "live volume proof: needs /dev/fuse + user namespaces (NOT PROVEN HERE)"
    LIVE_FLAG=--no-mount
else
    LIVE_FLAG=
fi
if out=$(cd "$ROOT" && bash tools/verify-initramfs.sh --initramfs "$IMG" \
        --workdir "$BASE" $LIVE_FLAG 2>&1); then
    ok "verify-initramfs.sh: self-verification passed${LIVE_FLAG:+ (structure only)}"
    printf '%s\n' "$out" | sed -n '/== 2. structure/,$p' | sed 's/^/       /'
else
    bad "verify-initramfs.sh reported a failure"
    printf '%s\n' "$out" | sed 's/^/       /' >&2
fi

echo
echo "== 4. negative cases (the verifier must have teeth) =="
# These all target phase-2 structure checks, so they run with --no-mount and
# do not need /dev/fuse. A host without user namespaces can still run them.
mutate "a missing rescue tool (invf-fsck)" \
       "MISSING invf-fsck" \
       'rm -f usr/local/bin/invf-fsck'
mutate "a neutered /init" \
       "/init never mentions invf-fuse" \
       'printf "#!/bin/sh\nexit 0\n" > init'
# Ship a dynamic binary with no libraries beside it: the exact "every .so must
# resolve" failure this bootstrap exists to prevent.
mutate "a dynamic tool with its libraries missing" \
       "not in the image" \
       'cp /usr/bin/env usr/local/bin/invf-verify; rm -f lib64/*'
# A zero-length fuse.ko is what the old recipe shipped on a host whose kernel
# has FUSE built in; insmod would choke on it.
mutate "a truncated fuse.ko" \
       "cannot mount" \
       'printf "kver=x\nfuse=module\n" > invfs-initramfs.manifest; : > fuse.ko'

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
echo "NOTE: this proves the initramfs can mount and read a real volume from a"
echo "      chroot. It does NOT prove a VM boot: kernel handoff, PID 1"
echo "      semantics and switch_root remain unexercised without a hypervisor."
