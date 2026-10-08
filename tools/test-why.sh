#!/bin/bash
# test-why.sh -- invf-why (read-only per-file storage inspector) e2e.
#
# Leg 1 (fresh): tiny text file, no sweep -- why reports zone=RAW,
#   algo=none, one entry, the inode/dirent/recipe record bytes, and
#   "class: none stamped".
# Leg 2 (swept): repetitive text swept offline -- why reports
#   zone=TEXT algo=ppmd with a TEXT class stamp, stored << size.
# Leg 3 (errors): missing name -> rc 1 "no such name"; a directory ->
#   rc 1 "no inode row" (namespace IS the dirent tree: dirs carry no
#   recipe, and the message must say so, not "unreadable").
# Leg 4 (read-only proof): fsck clean after all of the above -- why
#   never mutates the volume.
#
# Hermetic: no external helpers (text only, builtin lanes), no mount,
# no daemon. Uses /dev/shm (tmpfs) like the other suites; blkio treats
# /dev/* paths as raw devices, so RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/why
IMG=why-inspect.img
rm -rf "$WORK" && mkdir -p "$WORK/src"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }
fsck_ok() { $B/invf-fsck "$IMG" | grep -q "^OK$" || fail "fsck not clean"; }

echo "== [1] fresh file, no sweep =="
echo "hello invariantfs" > "$WORK/src/note.txt"
$B/invf-mkfs "$IMG" 64 >/dev/null
$B/invf-import "$IMG" "$WORK/src" >/dev/null
$B/invf-why "$IMG" /note.txt > "$WORK/why1.txt" || fail "why rc on fresh file"
grep -q "type: reg" "$WORK/why1.txt" || fail "leg1: no type line"
grep -q "zone=RAW algo=none(0)" "$WORK/why1.txt" || fail "leg1: expected RAW/none entry"
grep -q "class: none stamped" "$WORK/why1.txt" || fail "leg1: expected unstamped class"
grep -q "records: total ~" "$WORK/why1.txt" || fail "leg1: no record accounting"

echo "== [2] swept text -> TEXT batch =="
python3 -c "print('lorem ipsum dolor sit amet, consectetur adipiscing elit. ' * 200)" \
  > "$WORK/src/big.txt"
$B/invf-import "$IMG" "$WORK/src" >/dev/null
$B/invf-sweep "$IMG" >/dev/null
$B/invf-why "$IMG" /big.txt > "$WORK/why2.txt" || fail "why rc on swept file"
grep -q "zone=TEXT algo=ppmd(2)" "$WORK/why2.txt" || fail "leg2: expected TEXT/ppmd entry"
grep -q "class: TEXT{algo=ppmd" "$WORK/why2.txt" || fail "leg2: expected TEXT class stamp"
grep -q "stored=.* B in 1 blocks" "$WORK/why2.txt" || fail "leg2: no data extent line"

echo "== [3] error paths =="
$B/invf-why "$IMG" /nope > "$WORK/why3.txt" 2>&1 && fail "leg3: missing name rc=0"
grep -q "no such name" "$WORK/why3.txt" || fail "leg3: missing name message"
$B/invf-why "$IMG" / > "$WORK/why4.txt" 2>&1 && fail "leg3: directory rc=0"
grep -q "no inode row" "$WORK/why4.txt" || fail "leg3: directory message"

echo "== [4] why is read-only =="
fsck_ok

rm -f "$IMG"
echo
echo "WHY E2E: PASS"
