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

mnt_up() {   # <tag>
    setsid "$B/invf-fuse" "$IMG" "$MNT" >"$WORK/fuse.$1.log" 2>&1 < /dev/null &
    DPID=$!
    disown 2>/dev/null || true
    for _ in $(seq 1 80); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.25
    }
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
        kill -0 "$DPID" 2>/dev/null || return 0
        sleep 0.2
    done
    die "invf-fuse daemon did not exit after unmount (it holds the image lock)"
}

"$B/invf-mkfs" "$IMG" 64 >"$WORK/mkfs.log" 2>&1 || die "invf-mkfs failed (see $WORK/mkfs.log)"

# Payloads. Distinct lengths so a swap or a truncation cannot pass cmp.
printf 'PAYLOAD-A-ALPHA\n'            > "$WORK/pay_a"
printf 'PAYLOAD-A-BANG-BETA-LONGER\n' > "$WORK/pay_ab"

# ---- LEG 1: the defect ---------------------------------------------

echo "LEG 1: 'a!b' must survive rm a, and read back byte-exact"

mnt_up l1
cp "$WORK/pay_a" "$MNT/a"      || die "cannot create 'a' through the mount"
cp "$WORK/pay_ab" "$MNT/a!b"    || die "cannot create 'a!b' through the mount"

[ -f "$MNT/a" ]   && pass "'a' created through the mount"   || fail "'a' missing after create"
[ -f "$MNT/a!b" ] && pass "'a!b' created through the mount" || fail "'a!b' missing after create"

rm "$MNT/a"
rc=$?
# Before the fix this was 0. Assert it is 0: a fix that made rm FAIL rather
# than make rm safe would be a different (and unacceptable) answer.
[ $rc -eq 0 ] && pass "rm a exited 0" || fail "rm a exited $rc"

[ -e "$MNT/a!b" ] && pass "'a!b' STILL EXISTS through the mount after rm a" \
                  || fail "'a!b' WAS DESTROYED by rm a -- the defect"
[ -e "$MNT/a" ]   && fail "'a' still exists (rm did nothing)" \
                  || pass "'a' is gone, so the unlink did happen"

# ---- LEG 3: the boundary (while mounted) ---------------------------

echo "LEG 3: a '!' name is now refused at create, and counted at import"

if cp "$WORK/pay_ab" "$MNT/new!name" 2>"$WORK/err.create"; then
    fail "creating 'new!name' through the mount SUCCEEDED -- '!' is not reserved"
else
    pass "creating 'new!name' through the mount is REFUSED"
fi
[ -e "$MNT/new!name" ] && fail "'new!name' exists despite the refusal" \
                        || pass "'new!name' is not on the volume"
grep -q "reserved" "$WORK/err.create" \
    && pass "the refusal explains WHY ('!' is reserved)" \
    || echo "       note: refusal message did not mention 'reserved'"

# ---- LEG 2 fixture: a real decomposed container --------------------

echo "LEG 2 (CONTROL): a real decomposition's siblings must still be purged"

( cd "$WORK/in" && mkdir -p d && for i in 0 1 2 3; do \
    head -c 4096 /dev/urandom > "d/member$i.bin"; done && \
  tar cf "$WORK/in/t.tar" d ) || die "cannot build the TAR fixture"

cp "$WORK/in/t.tar" "$MNT/" || die "cannot copy t.tar through the mount"
"$B/invf-sweep" "$IMG" >"$WORK/sweep1.log" 2>&1 || die "invf-sweep failed (see $WORK/sweep1.log)"

# Ask the volume what it actually holds, offline, by NAME.
"$B/invf-ls" "$IMG" > "$WORK/ls1.txt" 2>"$WORK/ls1.err" || die "invf-ls failed"
n_parts=$(grep -c 't\.tar!part' "$WORK/ls1.txt")
[ "$n_parts" -gt 0 ] && pass "the sweep really minted $n_parts 't.tar!partN' siblings" \
                     || fail "no 't.tar!partN' siblings were minted -- leg 2 proves nothing"

mnt_down
# the daemon is gone; offline tools are the only writer now

"$B/invf-ls" "$IMG" > "$WORK/ls2.txt" 2>&1 || die "invf-ls after unmount failed"

rm -f "$IMG" 2>/dev/null
"$B/invf-mkfs" "$IMG" 64 >"$WORK/mkfs2.log" 2>&1 || die "invf-mkfs (leg 2 volume) failed"
"$B/invf-import" "$IMG" "$WORK/in" >"$WORK/import.log" 2>&1 || die "invf-import failed"
"$B/invf-sweep" "$IMG" >"$WORK/sweep2.log" 2>&1 || die "invf-sweep (leg 2) failed"
"$B/invf-ls" "$IMG" > "$WORK/ls3.txt" 2>&1 || die "invf-ls (leg 2) failed"

n_parts=$(grep -c 't\.tar!part' "$WORK/ls3.txt")
[ "$n_parts" -gt 0 ] && pass "leg 2 volume really holds $n_parts 't.tar!partN' siblings" \
                     || die "no 't.tar!partN' siblings -- the control proves nothing, stop"

# unlink the container through FUSE, exactly as a user would
mnt_up l2
rm "$MNT/t.tar"
rc=$?
[ $rc -eq 0 ] && pass "rm t.tar exited 0" || fail "rm t.tar exited $rc"
mnt_down

"$B/invf-ls" "$IMG" > "$WORK/ls4.txt" 2>&1 || die "invf-ls after leg 2 unlink failed"

grep -q 't\.tar!part' "$WORK/ls4.txt" \
    && { fail "siblings survived rm t.tar:"; grep 't\.tar!part' "$WORK/ls4.txt"; } \
    || pass "EVERY 't.tar!partN' sibling was purged -- sibling purging still works"

"$B/invf-ls" "$IMG" 2>/dev/null | grep -q 'bytes.*t\.tar$' \
    && fail "t.tar itself survived its own unlink" \
    || pass "t.tar itself is gone"

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