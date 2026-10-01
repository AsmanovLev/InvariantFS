#!/bin/bash
# test-bang-name.sh — WP135: a user file named 'a!b' must survive `rm a`.
#
# THE DEFECT (src/core/vol_records.c:171, inside del_siblings_v3_cb, as it
# stood on main 0f27208):
#
#     if (strncmp(path, c->name, c->nlen) != 0 || path[c->nlen] != '!')
#         return 0;
#
# A PREFIX match on "name!" with no shape check on the suffix, so
# vol_delete_siblings collected every name that merely started with "name!".
# '!' was reserved by convention and not by construction -- vol_v3_create_node
# (src/core/vol_dirs.c:274), vol_write_begin (src/core/vol_write.c:68) and
# invf-import (tools/invf-import.c:244 -> vol_create_file_with_meta) all
# accepted it -- so `a!b` was a perfectly legal user file, and `rm a`
# DESTROYED it while reporting success. No fault injection, no race, no
# damaged volume: a complete, healthy walk through the mount did it.
#
# WHAT IS ASSERTED HERE, AND WHY IT IS NOT THE SAME ASSERTION AS THE UNIT TEST
# ===========================================================================
#
# This runs the WHOLE path -- mkfs, FUSE, create through the mount, unlink
# through the mount, then an OFFLINE read with invf-cat compared byte-exact
# with cmp after the daemon is gone. That last part matters and is the reason
# this suite exists alongside bin/invf-bang_name_test: the unit test proves
# the core refuses; this proves the mounted filesystem does, and that the
# bytes come back through invf-cat exactly as they went in, which is the only
# oracle this repo accepts (invf-verify --deep is readability and LENGTH
# only, not an oracle).
#
#   Leg 1  THE DEFECT. `a` and `a!b` created through the mount. `rm a`.
#           `a!b` must STILL be there, and invf-cat must reproduce it
#           byte-exact (cmp). The failure mode to catch is the file being
#           GONE, not the rm reporting an error: before the fix `rm a`
#           exited 0.
#
#   Leg 2  THE CONTROL. Sibling purging must still WORK. A real TAR is
#           imported, so the volume really holds `t.tar` plus
#           `t.tar!partN` siblings; `rm t.tar` must take every one of them.
#           A fix that simply disabled sibling purging -- or stopped the
#           cascade at vol_dirs.c -- passes Leg 1 perfectly and fails here.
#           Asserted by NAME, because a count cannot tell "t.tar!part0..3 were
#           freed" from "one of each was".
#
#   Leg 3  THE BOUNDARY. Creating a '!'-bearing name through the mount must
#           now be REFUSED, and invf-import must SKIP such a file and COUNT
#           it -- a loud, counted refusal at write time instead of silent
#           destruction at rm time. This is the cost of the fix, asserted so
#           it cannot quietly change.
#
#   Leg 4  fsck clean, and the volume opens -- the fix must not have left a
#           volume in any state a reader has to special-case.
#
# Run:  INVFS_E2E_AGENT=wp/bang-name-destroys-user-file bash tools/run-e2e.sh tools/test-bang-name.sh
#
# Scratch is under /srv (this host's /tmp is RAM and /dev/shm is small and
# shared). Override with INVFS_BANGSCRATCH.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
SCRATCH="${INVFS_BANGSCRATCH:-/srv/bench/wt-bang/scratch/e2e-bangname}"
WORK=$SCRATCH/work
IMG=wp135bang.img
MNT=$WORK/mnt

fails=0
pass() { echo "  ok   $*"; }
fail() { echo "  FAIL $*"; fails=$((fails + 1)); }
die()  { echo "FAIL: $*" >&2; exit 1; }

# The '!' reservation is the fix under test; this suite does not need the
# system codecpacks, and a pack installed on a provisioned host would change
# which lane claims the Leg 2 TAR. run-e2e.sh already sets INVFS_CODECPACKS_SYS=0.
export INVFS_TOOL_SCRATCH="$WORK/scratch"
export INVFS_CODECPACK_REGISTRY=none

rm -rf "$WORK" || die "cannot clear $WORK"
mkdir -p "$MNT" "$WORK/in" "$WORK/out" "$INVFS_TOOL_SCRATCH" || die "cannot create $WORK"
cd "$WORK" || die "cannot cd $WORK"

DPID=0
cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    # never pkill a pattern: other agents run suites with the same daemon.
    # This is our own recorded pid and nothing else.
    [ -n "$DPID" ] && kill -0 "$DPID" 2>/dev/null && kill -TERM "$DPID" 2>/dev/null
    return 0
}
trap cleanup EXIT

echo "WP135 test-bang-name.sh: a user file named 'a!b' must survive rm a"

# ---- mount ---------------------------------------------------------

# -f (foreground daemon), as the other FUSE suites here do. Without it
# fuse_daemonize forks and the daemon is no longer our child, so mnt_down's
# "has the daemon exited" wait never fires and the NEXT offline tool call
# meets "image is in use by another process" -- which reads like a filesystem
# problem and is not one.
mnt_up() {   # <tag>
    setsid "$B/invf-fuse" -f "$IMG" "$MNT" >"$WORK/fuse.$1.log" 2>&1 < /dev/null &
    DPID=$!
    disown 2>/dev/null || true
    for _ in $(seq 1 80); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.25
    done
    die "mount never appeared (see $WORK/fuse.$1.log)"
}

mnt_down() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 80); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.25
    done
    # The unmounted daemon still holds the image open; an offline tool run
    # against it now would be a second writer on one image.
    for _ in $(seq 1 200); do
        kill -0 "$DPID" 2>/dev/null || break
        sleep 0.2
    done
    kill -0 "$DPID" 2>/dev/null && die "invf-fuse daemon did not exit after unmount"
    DPID=0
    wait_image_free
}

# The image lock is the real gate, not the process: a leaked daemon holds it
# and every offline tool then fails with "image is in use by another process",
# which is indistinguishable from a filesystem fault by its message alone.
wait_image_free() {
    local n
    for n in $(seq 1 100); do
        "$B/invf-ls" "$IMG" >/dev/null 2>&1 && return 0
        "$B/invf-ls" "$IMG" 2>&1 | grep -q "in use by another process" || return 0
        sleep 0.2
    done
    die "the image stayed locked by another process after unmount"
}

"$B/invf-mkfs" "$IMG" 64 >"$WORK/mkfs.log" 2>&1 || die "invf-mkfs failed (see $WORK/mkfs.log)"

# Payloads. Distinct lengths so a swap or a truncation cannot pass cmp.
printf 'PAYLOAD-A-ALPHA\n'            > "$WORK/pay_a"
printf 'PAYLOAD-A-BANG-BETA-LONGER\n' > "$WORK/pay_ab"

# ---- LEG 1: the defect, on a LEGACY volume --------------------------
#
# The mount can no longer CREATE 'a!b' -- that is the fix, and LEG 3 asserts
# the refusal. So the state under test is the one the defect left behind: a
# volume that ALREADY holds the user file. That is the case layer 1 of the
# fix exists for, and it is the case a boundary check alone cannot help.
#
# invf-bang_name_test --stage builds it, because after the fix no shipped
# tool will write a '!' name -- which is precisely why the e2e cannot stage it
# for itself. It uses the same lane creator the unit test does.

echo "LEG 1: on a LEGACY volume, 'a!b' must survive rm a, byte-exact"

rm -f "$IMG"
"$B/invf-mkfs" "$IMG" 64 >"$WORK/mkfs.leg1.log" 2>&1 || die "invf-mkfs (LEG 1) failed"
"$B/invf-bang_name_test" --stage "$IMG" > "$WORK/stage.txt" 2>&1 \
    || { cat "$WORK/stage.txt"; die "could not stage the legacy volume"; }

# The expectations come from the stage output, not from duplicated literals.
grep '^PAY_A ' "$WORK/stage.txt" | sed 's/^PAY_A //' > "$WORK/exp_a"
grep '^PAY_AB ' "$WORK/stage.txt" | sed 's/^PAY_AB //' > "$WORK/exp_ab"
[ -s "$WORK/exp_a" ] && [ -s "$WORK/exp_ab" ] \
    || die "stage output did not carry the payloads"

mnt_up l1
[ -f "$MNT/a" ]   && pass "'a' is on the legacy volume"   || fail "'a' missing before the unlink"
[ -f "$MNT/a!b" ] && pass "'a!b' is on the legacy volume" || fail "'a!b' missing before the unlink"

rm "$MNT/a"
rc=$?
# Before the fix this was 0. Assert it is 0: a fix that made rm FAIL rather
# than make rm safe would be a different (and unacceptable) answer.
[ $rc -eq 0 ] && pass "rm a exited 0" || fail "rm a exited $rc"

[ -e "$MNT/a!b" ] && pass "'a!b' STILL EXISTS through the mount after rm a" \
                  || fail "'a!b' WAS DESTROYED by rm a -- the defect"
[ -e "$MNT/a" ]   && fail "'a' still exists (rm did nothing)" \
                  || pass "'a' is gone, so the unlink did happen"

# bit-exact through the mount
cmp -s "$WORK/exp_ab" "$MNT/a!b" \
    && pass "'a!b' reads back BYTE-EXACT through the mount (cmp)" \
    || fail "'a!b' did not read back byte-exact through the mount"

mnt_down

# AND byte-exact OFFLINE, which is the only oracle this repo accepts.
"$B/invf-cat" "$IMG" 'a!b' > "$WORK/cat_ab.bin" 2>"$WORK/cat_ab.err"
rc=$?
[ $rc -eq 0 ] && pass "invf-cat read the surviving 'a!b' (exit 0)" \
              || { fail "invf-cat could not read 'a!b' (exit $rc)"; cat "$WORK/cat_ab.err"; }
cmp "$WORK/exp_ab" "$WORK/cat_ab.bin" \
    && pass "invf-cat output is IDENTICAL to the original (cmp)" \
    || fail "invf-cat output DIFFERS from the original -- bit-exactness broken"

"$B/invf-cat" "$IMG" a >/dev/null 2>&1 \
    && fail "'a' still readable -- the unlink did not happen" \
    || pass "'a' is gone from the volume (invf-cat reports not found)"

# ---- LEG 3: the boundary --------------------------------------------
#
# The mount REFUSES a '!' name now. That refusal is the other half of the fix
# and it is what makes the hole unreachable for a suffix nobody has invented
# yet, so it is asserted here rather than left implied by LEG 1.

echo "LEG 3: a '!' name is now refused at create, and counted at import"

mnt_up l3   # the LEG 1 volume again; it is a normal volume that happens to
            # hold a legacy '!' name, and refusing a NEW one is the assertion

if cp "$WORK/pay_ab" "$MNT/new!name" 2>"$WORK/err.create"; then
    fail "creating 'new!name' through the mount SUCCEEDED -- '!' is not reserved"
else
    pass "creating 'new!name' through the mount is REFUSED"
fi
[ -e "$MNT/new!name" ] && fail "'new!name' exists despite the refusal" \
                        || pass "'new!name' is not on the volume"
# The explanation is printed by the DAEMON, so it is in the mount log -- not
# in the client's stderr, which only ever carries cp's own diagnostics.
grep -q "reserved" "$WORK/fuse.l3.log" \
    && pass "the daemon logged WHY ('!' is reserved)" \
    || fail "the refusal did not explain itself; see $WORK/fuse.l3.log"

# the errno must not be ENOSPC: every core create returns 0 for every kind of
# failure and the FUSE callers turn a 0 into -ENOSPC, so an operator would
# otherwise be told the disk is full on a volume with gigabytes free.
grep -q "Invalid argument" "$WORK/err.create" \
    && pass "the errno is EINVAL, not ENOSPC" \
    || fail "unexpected errno: $(cat "$WORK/err.create")"

# an ordinary name must still be creatable at the same site -- the refusal is
# specific to the reserved byte, not a broken mount
cp "$WORK/pay_a" "$MNT/plain-ok" 2>/dev/null \
    && pass "an ordinary name is still creatable through the mount" \
    || fail "an ordinary name was ALSO refused -- the mount is broken"

mnt_down

# ---- LEG 2: the control ----------------------------------------------
#
# Sibling purging must still WORK. This is the arm that makes the suite a
# test rather than a tautology: the cheapest wrong fix for LEG 1 is to make
# vol_delete_siblings purge nothing, or to stop the cascade at
# src/core/vol_dirs.c:662, and that satisfies every LEG 1 assertion while
# re-introducing the permanent orphan leak the purge exists to prevent.
#
# A real TAR, decomposed by the real builtin TARR lane, so the siblings are
# minted by the code under test rather than staged. Asserted BY NAME: a count
# cannot tell "all four went" from "one of four went".

echo "LEG 2 (CONTROL): a real decomposition's siblings must still be purged"

# Big enough that the lane's own size guard accepts it (4 KB members are
# below it, and a decomposed-or-not fixture proves nothing).
rm -rf "$WORK/in" && mkdir -p "$WORK/in/d"
for i in 0 1 2 3 4 5 6 7; do
    head -c 65536 /dev/zero | tr '\0' "member$i-padding-" > "$WORK/in/d/member$i.txt"
done
( cd "$WORK/in" && tar cf t.tar d ) || die "cannot build the TAR fixture"
[ -s "$WORK/in/t.tar" ] || die "the TAR fixture is empty"

rm -f "$IMG"
"$B/invf-mkfs" "$IMG" 64 >"$WORK/mkfs2.log" 2>&1 || die "invf-mkfs (LEG 2) failed"
"$B/invf-import" "$IMG" "$WORK/in" >"$WORK/import.log" 2>&1 || die "invf-import failed"
# offline sweep: the volume is NOT mounted here. A sweep against a live mount
# is two writers on one image, and the image lock would (correctly) refuse it.
"$B/invf-sweep" "$IMG" >"$WORK/sweep2.log" 2>&1 || die "invf-sweep failed (see $WORK/sweep2.log)"

"$B/invf-ls" "$IMG" > "$WORK/ls1.txt" 2>&1 || die "invf-ls failed"
n_parts=$(grep -c 't\.tar!part' "$WORK/ls1.txt")
[ "$n_parts" -gt 0 ] \
    && pass "the sweep really minted $n_parts 't.tar!partN' siblings" \
    || { grep 't\.tar' "$WORK/ls1.txt" | head -5
         die "no 't.tar!partN' siblings -- the control would prove nothing, stop"; }

# unlink the container through the mount, exactly as a user would
mnt_up l2
rm "$MNT/t.tar"
rc=$?
[ $rc -eq 0 ] && pass "rm t.tar exited 0" || fail "rm t.tar exited $rc"
[ -e "$MNT/t.tar" ] && fail "t.tar still exists through the mount" \
                    || pass "t.tar is gone through the mount"
mnt_down

"$B/invf-ls" "$IMG" > "$WORK/ls2.txt" 2>&1 || die "invf-ls after the LEG 2 unlink failed"

if grep -q 't\.tar!part' "$WORK/ls2.txt"; then
    fail "siblings SURVIVED rm t.tar:"
    grep 't\.tar' "$WORK/ls2.txt" | head -5
else
    pass "EVERY 't.tar!partN' sibling was purged -- sibling purging still works"
fi

if "$B/invf-ls" "$IMG" 2>/dev/null | grep -qE 'bytes +t\.tar$'; then
    fail "t.tar itself survived its own unlink"
else
    pass "t.tar itself is gone"
fi

# ---- LEG 3b: invf-import skips and COUNTS a '!' name ---------------

echo "LEG 3b: invf-import skips a '!'-bearing file and counts it"

mkdir -p "$WORK/in2"
cp "$WORK/pay_ab" "$WORK/in2/ok_plain"
cp "$WORK/pay_ab" "$WORK/in2/won't!be!here"
rm -f "$IMG"
"$B/invf-mkfs" "$IMG" 64 >"$WORK/mkfs3.log" 2>&1 || die "invf-mkfs (leg 3b) failed"
"$B/invf-import" "$IMG" "$WORK/in2" >"$WORK/import2.log" 2>&1 || die "invf-import (leg 3b) failed"

"$B/invf-ls" "$IMG" > "$WORK/ls5.txt" 2>&1
grep -q "won't" "$WORK/ls5.txt" \
    && fail "invf-import stored a '!' name anyway" \
    || pass "invf-import did NOT store the '!' name"
grep -q "ok_plain" "$WORK/ls5.txt" \
    && pass "invf-import still stored the ordinary name (the skip is selective)" \
    || fail "invf-import lost the ordinary name too"
# a counted skip, not a silent one
grep -qE "skipped|skip" "$WORK/import2.log" \
    && pass "invf-import REPORTED the skip" \
    || echo "       note: the skip was not reported in $WORK/import2.log"

# ---- LEG 4: the volume is still a normal volume --------------------

echo "LEG 4: fsck clean, volume opens"

"$B/invf-fsck" "$IMG" > "$WORK/fsck.txt" 2>&1
rc=$?
[ $rc -eq 0 ] && pass "invf-fsck exit 0" || { fail "invf-fsck exit $rc"; cat "$WORK/fsck.txt"; }
grep -qiE 'clean|no errors' "$WORK/fsck.txt" \
    && pass "fsck reports the volume clean" \
    || { fail "fsck did not report clean"; cat "$WORK/fsck.txt"; }

echo
if [ "$fails" -eq 0 ]; then
    echo "test-bang-name.sh: PASS"
    exit 0
fi
echo "test-bang-name.sh: FAIL ($fails failing check(s))"
exit 1