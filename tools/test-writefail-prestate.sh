#!/bin/bash
# test-writefail-prestate.sh — a mutation that reports FAILURE must leave the
# PRE-CALL state (ADR-009). leg5's oplogs say this is what breaks: in every
# failing artifact the file the gate named had, as its LAST operation, one that
# failed loudly (EIO on a write, ENOENT on a delete), with no successful write
# afterwards -- and the file was then unreadable.
#
# This suite makes that deterministic: no dm-flakey, no seeds, no timing. The
# failure is injected at a named seam (src/core/vol_fault.h) so the engine sees
# exactly the errno it would see from a device that gave up mid-write.
#
# Legs
#   1  baseline: write A (2 MiB, incompressible), read back byte-identical.
#   2  the RECIPE STORE fails during a rewrite -> the rewrite fails, and the
#      file still reads back as A, byte for byte. (The store happens at commit,
#      i.e. after every segment of B has already been written: this is the
#      "failure in the middle of a file" shape, not all-or-nothing.)
#   3  the ROW PUBLISH fails during a rewrite -> same requirement.
#   4  the SAME seam, armed past the end (never fires) -> the rewrite succeeds
#      and the file reads back as B. The control that proves legs 2-3 are
#      testing the failure and not a broken fixture.
#   5  fsck clean and verify --deep 0 corrupt after every leg.
#
# Run from the repo root after `make`:  bash tools/test-writefail-prestate.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=${INVFS_BIN:-$REPO/bin}
WORK=/dev/shm/wfail
IMG=wfail.img
rm -rf "$WORK" && mkdir -p "$WORK"
cd /dev/shm
rm -f "$IMG"

. "$REPO/tools/fsck-clean.sh"
fail() { echo "FAIL: $*" >&2; exit 1; }
sha() { sha256sum < "$1" | cut -d' ' -f1; }

echo "== fixture: A (2 MiB) and B (2 MiB), incompressible =="
python3 - <<'PY'
import random
random.seed(90210)
for name, seed in (("A", 1), ("B", 2)):
    r = random.Random(seed)
    open("/dev/shm/wfail/%s.bin" % name, "wb").write(r.randbytes(2 * 1024 * 1024))
PY
HA=$(sha "$WORK/A.bin"); HB=$(sha "$WORK/B.bin")
[ "$HA" != "$HB" ] || fail "fixture: A and B are identical"

$B/invf-mkfs "$IMG" 0.4 >/dev/null
$B/invf-cp "$IMG" "$WORK/A.bin" f.bin >/dev/null 2>&1 || fail "baseline write failed"
[ "$($B/invf-cat "$IMG" f.bin | sha256sum | cut -d' ' -f1)" = "$HA" ] || fail "leg1: A does not read back"

expect_old() {   # <label>
    local got
    got=$($B/invf-cat "$IMG" f.bin 2>/dev/null | sha256sum | cut -d' ' -f1)
    [ "$got" = "$HA" ] || fail "$1: the failed mutation changed the file"
    $B/invf-fsck "$IMG" | tee "$WORK/fsck.log" >/dev/null
    fsck_clean "$WORK/fsck.log" || fail "$1: fsck not clean"
    $B/invf-verify "$IMG" --deep >"$WORK/verify.log" 2>&1 || {
        tail -5 "$WORK/verify.log"; fail "$1: verify --deep failed"; }
    grep -qiE '0 corrupt' "$WORK/verify.log" || { grep -i corrupt "$WORK/verify.log" | head -3;
        fail "$1: verify --deep reported corruption"; }
    echo "  $1: pre-call state intact (A), fsck clean, verify 0 corrupt"
}

echo "== leg 2: recipe store fails during the rewrite =="
INVFS_FAULT=recipe_store:1 $B/invf-cp "$IMG" "$WORK/B.bin" f.bin >"$WORK/l2.log" 2>&1 \
    && fail "leg2: the injected failure did not fail the rewrite"
echo "  rewrite failed loudly (as injected)"
expect_old leg2

echo "== leg 3: row publish fails during the rewrite =="
INVFS_FAULT=inode_delta_put:1 $B/invf-cp "$IMG" "$WORK/B.bin" f.bin >"$WORK/l3.log" 2>&1 \
    && fail "leg3: the injected failure did not fail the rewrite"
echo "  rewrite failed loudly (as injected)"
expect_old leg3

echo "== leg 4: same seam armed past the end -> the rewrite must SUCCEED =="
INVFS_FAULT=recipe_store:9999 $B/invf-cp "$IMG" "$WORK/B.bin" f.bin >"$WORK/l4.log" 2>&1 \
    || { tail -5 "$WORK/l4.log"; fail "leg4: the rewrite failed with the seam unarmed"; }
[ "$($B/invf-cat "$IMG" f.bin | sha256sum | cut -d' ' -f1)" = "$HB" ] \
    || fail "leg4: the successful rewrite did not store B"
$B/invf-fsck "$IMG" >"$WORK/fsck4.log" 2>&1
fsck_clean "$WORK/fsck4.log" || fail "leg4: fsck not clean after the control rewrite"
echo "  control: rewrite succeeded, B stored, fsck clean"

echo "PASS: a failed mutation leaves the pre-call state — on the recipe-store and"
echo "      the row-publish paths — and the unarmed control still rewrites"
