#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
B="$ROOT/bin"
WORK=$(mktemp -d /tmp/invfs-sweep-ui.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

python3 - "$WORK" <<'PY'
import os
import sys
work = sys.argv[1]
block = 65536
common = bytes(((i * 37 + 11) & 0xff) for i in range(block))
unique_a = bytes(((i * 59 + 17) & 0xff) for i in range(block))
unique_b = bytes(((i * 83 + 29) & 0xff) for i in range(block))
with open(os.path.join(work, "a.bin"), "wb") as f:
    f.write(common + common + unique_a)
with open(os.path.join(work, "b.bin"), "wb") as f:
    f.write(common + unique_b)
PY

"$B/invf-mkfs" "$WORK/test.img" 64 >/dev/null 2>&1
"$B/invf-cp" "$WORK/test.img" "$WORK/a.bin" >/dev/null 2>&1
"$B/invf-cp" "$WORK/test.img" "$WORK/b.bin" >/dev/null 2>&1
"$B/invf-sweep" "$WORK/test.img" --log "$WORK/sweep.log" >"$WORK/sweep.out" 2>&1

grep -q '^\[1/7\] prepare' "$WORK/sweep.out"
grep -q '^\[3/7\] transform.*100\.0%' "$WORK/sweep.out"
grep -q '^\[5/7\] dedupe.*cross=1 intra=1 merged=2' "$WORK/sweep.out"
grep -q '^\[7/7\] finalize.*volume durable' "$WORK/sweep.out"
grep -q '^==== invf-sweep ' "$WORK/sweep.log"
grep -q '^\[5/7\] dedupe.*cross=1 intra=1 merged=2' "$WORK/sweep.log"

if [[ "${INVFS_SWEEP_UI_VERBOSE:-0}" == "1" ]]; then
    cat "$WORK/sweep.out"
fi

echo "sweep_ui_test: stage progress, dedupe scope counters, persistent log: PASS"
