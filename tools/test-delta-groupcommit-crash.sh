#!/bin/bash
# test-delta-groupcommit-crash.sh — WP602 crash probe: acked==durable under
# kill -9 mid-batch (the leg7 assertion shape, offline).
#
# Contract under test: vol_delta_drain() returning 0 means every record
# staged before it is durable. The writer appends N records, drains (THE
# ack), appends M more WITHOUT draining, and is then kill -9'd with the
# volume still open (no vol_close, no CLEAN mark — the power cut). After
# reopen: every acked record present+exact, every unacked record
# complete-or-absent (replay keeps the CRC-valid prefix; a torn tail is
# truncated, never adopted).
#
# What this does and does not prove: kill -9 preserves the page cache, so
# the acked half passing also needs the drain to have issued a REAL fsync —
# that half is groupcommit_test T8 (the interposed fsync() counts the
# drain's physical flush in-process). Together: the ack fenced (T8) and the
# fenced state replays exact after an abrupt death (this script).
#
# Run from the repo root after `make`, or via
#   INVFS_E2E_AGENT=wp602 bash tools/run-e2e.sh tools/test-delta-groupcommit-crash.sh
# Image paths are RELATIVE (blkio treats /dev/* as raw devices).
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B=$REPO/bin
WORK=/dev/shm/wp602crash
IMG=wp602crash.img
rm -rf "$WORK" && mkdir -p "$WORK"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

if [ ! -x "$B/invf-groupcommit_test" ]; then
    make -C "$REPO" bin/invf-groupcommit_test >/dev/null 2>&1 \
        || fail "cannot build bin/invf-groupcommit_test"
fi
if [ ! -x "$B/invf-mkfs" ]; then
    make -C "$REPO" bin/invf-mkfs >/dev/null 2>&1 \
        || fail "cannot build bin/invf-mkfs"
fi
if [ ! -x "$B/invf-fsck" ]; then
    make -C "$REPO" bin/invf-fsck >/dev/null 2>&1 \
        || fail "cannot build bin/invf-fsck"
fi

N=130
M=20

echo "== mkfs =="
$B/invf-mkfs "$IMG" 1 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "mkfs failed"; }

echo "== writer: $N acked + $M staged, then kill -9 =="
"$B/invf-groupcommit_test" gc-crash-write "$IMG" "$N" "$M" >"$WORK/writer.log" 2>&1 &
WRITER=$!
for i in $(seq 1 150); do
    grep -q "^STAGED" "$WORK/writer.log" 2>/dev/null && break
    sleep 0.2
done
grep -q "^STAGED" "$WORK/writer.log" \
    || { cat "$WORK/writer.log"; fail "writer never staged the unacked tail"; }
cat "$WORK/writer.log"
kill -9 "$WRITER" 2>/dev/null || fail "writer already dead before kill -9"
wait "$WRITER" 2>/dev/null || true
echo "writer killed -9 (no vol_close, no CLEAN mark)"

echo "== the death was abrupt: superblock still DIRTY =="
python3 - "$IMG" <<'PY' || fail "volume closed cleanly -- this is not a power cut"
import struct, sys
state = struct.unpack_from("<I", open(sys.argv[1], "rb").read(0x20), 0x18)[0]
print("  on-disk superblock: 0x%02X%s" % (state,
      " (DIRTY: the volume never closed)" if state == 0xDA else " (UNEXPECTED)"))
sys.exit(0 if state == 0xDA else 1)
PY

echo "== verify after reopen: acked exact, unacked exact-or-absent =="
"$B/invf-groupcommit_test" gc-crash-verify "$IMG" "$N" "$M" 2>&1 \
    | tee "$WORK/verify.log" || fail "crash verify failed"

echo "== fsck clean =="
$B/invf-fsck "$IMG" >"$WORK/fsck.log" 2>&1 \
    || { cat "$WORK/fsck.log"; fail "fsck failed after the cut"; }
tail -2 "$WORK/fsck.log"

echo "PASS: WP602 crash probe (acked==durable, unacked never torn)"
