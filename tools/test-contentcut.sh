#!/bin/bash
# test-contentcut.sh — WP613 torn-segment salvage e2e (persistent regression).
#
#   Leg A  sound volume (multi-segment big1 + single-segment single +
#          control), all incompressible so segments land verbatim: verify
#          --deep clean, fsck OK.
#   Leg B  tear one payload byte inside big1's seg 2 and inside single's
#          only segment (python flips a byte past a unique token placed at
#          a known file offset -- the power-loss shape: disk bytes stop
#          matching the framed CRC): both files read EIO (invf-cat fails),
#          verify --deep names them corrupt, fsck exits 3 and
#          --list-damaged names both segment-torn.
#   Leg C  `invf-fsck --cut --dry-run`: plan cuts big1 only, changes
#          nothing (sizes + EIO unchanged), rc 0.
#   Leg D  `invf-fsck --cut`: big1 rewritten to its 128 KiB good prefix
#          and verified (invf-cat == first 131072 reference bytes);
#          single UNCUTTABLE (first span torn -- truncating to empty is
#          deletion, refused), still EIO at its old size; control
#          bit-exact; ledger names only single afterwards. rc 3 (damage
#          remains: the honest-terminal file).
#
# Hermetic: images under /dev/shm (relative paths); no mount, no daemon.
set -e
set -o pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
B=$REPO/bin
WORK=/dev/shm/wpcontentcut
IMG=wpcontentcut.img

fail() { echo "FAIL: $1" >&2; exit 1; }
rm -rf "$WORK" && mkdir -p "$WORK" && cd "$WORK"

echo "== [A] sound volume verifies =="
# 1 GiB, not 32: the leg-B tear fixture below scans the whole image
# through one mmap, and a 32 GiB sparse image OOM-kills the scanner
# (the payload here is ~360 KiB; nothing needs the room).
$B/invf-mkfs "$IMG" 1 >/dev/null || fail "mkfs"
W="$WORK" python3 - <<'EOF' || fail "fixture gen"
import os
w = os.environ["W"]
big = bytearray(os.urandom(200000))
t2 = os.urandom(16); big[132000:132016] = t2
open(w+"/big1.bin","wb").write(big)
open(w+"/t2.token","wb").write(t2)
sng = bytearray(os.urandom(10000))
ts = os.urandom(16); sng[5000:5016] = ts
open(w+"/single.bin","wb").write(sng)
open(w+"/ts.token","wb").write(ts)
open(w+"/ctl.bin","wb").write(os.urandom(150000))
open(w+"/big1.ref","wb").write(big)
EOF
$B/invf-cp "$IMG" big1.bin big1.bin >/dev/null || fail "cp big1"
$B/invf-cp "$IMG" single.bin single.bin >/dev/null || fail "cp single"
$B/invf-cp "$IMG" ctl.bin ctl.bin >/dev/null || fail "cp ctl"
$B/invf-verify "$IMG" --deep > verify-clean.log 2>&1 || fail "baseline verify rc=$?"
grep -q "0 corrupt" verify-clean.log || fail "baseline not clean"
$B/invf-fsck "$IMG" | grep -q "^OK$" || fail "baseline fsck not OK"
echo "  sound volume: verify clean, fsck OK"

echo "== [B] torn segments fail loudly =="
python3 - <<'EOF' || fail "tear fixture"
import mmap
f = open("wpcontentcut.img","r+b")
mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_WRITE)
def tear(tokenfile, delta):
    tok = open(tokenfile,"rb").read()
    # Every verbatim occurrence: the write path leaves a staging copy
    # behind the committed segment, and either order is possible -- tear
    # all of them (all are file-content bytes, nothing else can contain
    # a 16-byte urandom token). Stale copies are unreferenced, so extra
    # flips are harmless; the live segment is always among them.
    at = mm.find(tok)
    assert at >= 0, tokenfile + " token not found verbatim"
    n = 0
    while at >= 0:
        mm[at + delta] ^= 0xFF
        n += 1
        at = mm.find(tok, at + 1)
    print("  %s: tore %d occurrence(s)" % (tokenfile, n))
tear("t2.token", 100)   # inside big1 seg 2 payload ([131072,196608))
tear("ts.token", 100)   # inside single's only segment payload
mm.close()
f.close()
print("  tore big1 seg 2 + single seg 0")
EOF
if $B/invf-cat "$IMG" big1.bin big1.out >/dev/null 2>&1; then
    fail "torn big1 catted (must be EIO)"
fi
echo "  big1 reads EIO (honest terminal)"
if $B/invf-cat "$IMG" single.bin single.out >/dev/null 2>&1; then
    fail "torn single catted (must be EIO)"
fi
echo "  single reads EIO (honest terminal)"
if $B/invf-verify "$IMG" --deep > verify-torn.log 2>&1; then
    fail "verify passed a torn volume (must report corrupt)"
fi
grep -q "CORRUPT" verify-torn.log || { cat verify-torn.log; fail "verify names no corrupt file"; }
echo "  verify --deep names the torn files"
if $B/invf-fsck "$IMG" >/dev/null 2>&1; then
    fail "fsck passed a torn volume (must exit 3)"
fi
$B/invf-fsck "$IMG" --list-damaged > damaged-before.log 2>&1 || true
grep -q "segment-torn" damaged-before.log || { cat damaged-before.log; fail "ledger names no segment-torn"; }
grep -c "segment-torn" damaged-before.log | grep -q "2" || { cat damaged-before.log; fail "ledger must name 2 segment-torn files"; }
echo "  fsck --list-damaged names both segment-torn"

echo "== [C] dry-run plans, changes nothing =="
$B/invf-fsck "$IMG" --cut --dry-run > cut-plan.log 2>&1 || fail "dry-run rc=$?"
grep -q "WOULD cut" cut-plan.log || { cat cut-plan.log; fail "plan names no cut"; }
grep -q "UNCUTTABLE" cut-plan.log || { cat cut-plan.log; fail "plan names no uncuttable file"; }
if $B/invf-cat "$IMG" big1.bin big1.out >/dev/null 2>&1; then
    fail "dry-run resurrected big1 (must change nothing)"
fi
echo "  plan cuts big1, refuses single, volume untouched"

echo "== [D] explicit cut salvages the prefix =="
if $B/invf-fsck "$IMG" --cut > cut.log 2>&1; then
    rc=0
else
    rc=$?
fi
[ "$rc" = "3" ] || { cat cut.log; fail "cut rc=$rc (want 3: single remains)"; }
grep -q "CUT to 131072" cut.log || { cat cut.log; fail "cut names no 128 KiB salvage"; }
$B/invf-cat "$IMG" big1.bin big1.out >/dev/null || fail "cut big1 unreadable"
head -c 131072 big1.ref > big1.prefix
cmp big1.prefix big1.out || fail "cut big1 != surviving prefix (not bit-exact)"
echo "  big1 reads back EXACTLY its 128 KiB survivors"
if $B/invf-cat "$IMG" single.bin single.out >/dev/null 2>&1; then
    fail "cut emptied single (must refuse truncate-to-empty)"
fi
echo "  single still EIO at its old size (honest terminal kept)"
$B/invf-cat "$IMG" ctl.bin ctl.out >/dev/null || fail "control unreadable"
cmp ctl.bin ctl.out || fail "control not bit-exact (collateral)"
$B/invf-fsck "$IMG" --list-damaged > damaged-after.log 2>&1 || true
grep -q "segment-torn" damaged-after.log || { cat damaged-after.log; fail "ledger lost single"; }
[ "$(grep -c "segment-torn" damaged-after.log)" = "1" ] || { cat damaged-after.log; fail "ledger must name only single now"; }
echo "  ledger names only the uncuttable file; control bit-exact"

rm -f "$IMG" big1.bin single.bin ctl.bin big1.ref big1.prefix big1.out single.out ctl.out t2.token ts.token verify-clean.log verify-torn.log damaged-before.log damaged-after.log cut-plan.log cut.log
echo
echo "CONTENTCUT E2E: PASS"
