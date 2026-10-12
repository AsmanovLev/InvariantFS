#!/bin/bash
# test-handler-doom.sh — WP610: the six namespace mutators WP606/607 did
# not wrap (mkdir, mknod, symlink, link, rmdir, truncate) fail clean.
#
# THE VERDICT (proven per handler in src/cli/handler_doom_test.c, which
# this suite drives): every delta failure inside vol_delta_append dooms
# inline (WP606), none of the six engines reads after staging (the rename
# GHOST shape WP606 audits as unique to rename), the session commit stages
# delta only at commit time (begin/range/truncate are session RAM + data
# segments, freed by abort), and the sole net-growth-on-failure path
# (hardlink's compensating put-back) stages a byte-identical null record.
# So all six handlers are left UNWRAPPED — a FUSE-level doom would
# endanger vol_ensure_path's legitimate parent creates and innocent
# earlier-but-unfenced group members for zero gain — and this suite is the
# proof that executes: each leg faults its op mid-way (INVFS_FAULT seams,
# the production failure each stands in for) and asserts the WP610
# property — vol_delta_pending() back at baseline (no retained-but-
# unfenced records), the namespace exactly as before, byte-exact
# survivors — with invf-fsck OK over the whole fault sequence.
#
# A leg whose fault never fired cannot pass vacuously: every fault leg
# asserts the op FAILED (unarmed it would succeed), and every phase
# binary exits nonzero on any failed check (set -e below trips on it).
#
# Layout: the phase binary is built by `make` (bin/invf-handler_doom_test,
# also run leg-by-leg under `make test`); this suite sequences the same
# phases on one image and adds the fsck-OK half. Image paths are RELATIVE
# (blkio treats /dev/* as raw devices) under /dev/shm; run e2e only via
# run-e2e.sh with INVFS_E2E_AGENT set (AGENTS.md 1.5).
#
# Run:  INVFS_E2E_AGENT=wp610 bash tools/run-e2e.sh tools/test-handler-doom.sh
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
T=$B/invf-handler_doom_test
WORK=/dev/shm/wp610hd
IMG=hd.img
rm -rf "$WORK" && mkdir -p "$WORK"
cd "$WORK"

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$T" ] || fail "phase binary $T missing (run make first)"
export INVF_MKFS="$B/invf-mkfs"

echo "== phases: hookctl + setup =="
"$T" . hookctl || fail "hookctl"
"$T" . setup || fail "setup"

echo "== fault legs: mkdir/mknod/symlink =="
"$T" . mkdir_put || fail "mkdir_put"
"$T" . mkdir_eexist || fail "mkdir_eexist"
"$T" . mknod_put || fail "mknod_put"
"$T" . symlink_put || fail "symlink_put"
"$T" . symlink_store || fail "symlink_store"
$B/invf-fsck "$IMG" > fsck1.log 2>&1 || fail "fsck after create-faults failed"
grep -q "^OK$" fsck1.log || fail "fsck after create-faults not OK"
echo "  create-fault block green + fsck OK"

echo "== fault legs: link/rmdir =="
"$T" . link_put || fail "link_put"
"$T" . link_ok || fail "link_ok"
"$T" . rmdir_full || fail "rmdir_full"
"$T" . rmdir_missing || fail "rmdir_missing"
"$T" . rmdir_ok || fail "rmdir_ok"
$B/invf-fsck "$IMG" > fsck2.log 2>&1 || fail "fsck after link/rmdir failed"
grep -q "^OK$" fsck2.log || fail "fsck after link/rmdir not OK"
echo "  link/rmdir block green + fsck OK"

echo "== fault legs: truncate/resize =="
"$T" . truncate_put || fail "truncate_put"
"$T" . truncate_store || fail "truncate_store"
"$T" . truncate_ok || fail "truncate_ok"
"$T" . resize_put || fail "resize_put"
"$T" . resize_ok || fail "resize_ok"
$B/invf-fsck "$IMG" > fsck3.log 2>&1 || fail "fsck after truncate-faults failed"
grep -q "^OK$" fsck3.log || fail "fsck after truncate-faults not OK"
echo "  truncate block green + fsck OK"

echo "== verify: fixtures exact, fault targets absent =="
"$T" . verify || fail "verify"
$B/invf-fsck "$IMG" > fsck-final.log 2>&1 || fail "final fsck failed"
grep -q "^OK$" fsck-final.log || fail "final fsck not OK"
# byte-exact survivors through the public read path, not just the engine
$B/invf-cat "$IMG" surv.bin surv.out >/dev/null 2>&1 || fail "cat surv.bin"
$B/invf-cat "$IMG" subdir/child.bin child.out >/dev/null 2>&1 || fail "cat child.bin"
echo "  verify green + final fsck OK"
echo "PASS: handler-doom fault sequence clean"
