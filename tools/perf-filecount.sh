#!/bin/bash
# perf-filecount.sh — does InvariantFS's per-FILE import cost stay constant?
#
# THE QUESTION. The PERF_PROFILING counters gave the first real number:
#
#     50 files, 1 dir, ~20 bytes each
#     vmux writes        152      ~3 writes per file
#     vmux write bytes   819,536  ~16 KB written per file
#     vol_flush calls    2        NOT per-file flush
#
# ~16 KB of writes to store ~1 KB of data. That is either a FIXED per-file
# metadata cost (fine to explain, and flat as the tree grows) or something that
# SCALES with tree size (bad, and probably the reason Arch's 34,209 files did
# not finish in 90 minutes while Debian's 7,509 did). Those two have opposite
# fixes, so this measures which one it is before anyone optimises anything.
#
#   make PERF=1                       # counters required
#   tools/perf-filecount.sh 500 5000  # N files at each size, same flat layout
#
# Corpus is GENERATED, never copied or reused: page cache would otherwise make
# the source reads free and flatter every number here. Files are one 512-byte
# line, so total bytes stay near-constant across sizes and the ONLY variable is
# file count -- which is the point. Directory shape is held flat (all in one
# dir) deliberately; breadth is a separate axis for a separate run.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/bin"
WORK="${PERF_WORK:-/mnt/invfs-scratch/perf-filecount}"
SIZES=("$@")
[ ${#SIZES[@]} -gt 0 ] || SIZES=(50 500 5000)

[ -x "$BIN/invf-import" ] || { echo "FAIL: $BIN/invf-import missing (run make)" >&2; exit 1; }
# The counters are what this script is FOR. Without them it would print
# timings and no explanation, which is the mistake this whole exercise exists
# to avoid -- so refuse rather than produce a number nobody can interpret.
#
# NOTE: `nm ... | grep -q` does NOT work here under `set -o pipefail`. grep -q
# exits at the first match, nm takes SIGPIPE (141), pipefail promotes that to a
# failed pipeline, and the check reports "not instrumented" on a binary that is.
# Count to a variable and test that instead -- the same shape as checking a
# verifier's exit code through `cmd | tail -1`, which hid a hygiene failure
# twice in one session.
_perf_syms=$(nm "$BIN/invf-import" 2>/dev/null | grep -c invfs_perf || true)
if [ "${_perf_syms:-0}" -eq 0 ]; then
    echo "FAIL: invf-import was built WITHOUT -DPERF_PROFILING." >&2
    echo "      Rebuild with: make PERF=1" >&2
    exit 1
fi

mkdir -p "$WORK" || exit 1
printf '%-8s %-8s %10s %10s %12s %10s %8s %10s\n' \
       files secs files/s vmux_writes MB_written flushes write/file

for n in "${SIZES[@]}"; do
    src="$WORK/src-$n"
    rm -rf "$src"; mkdir -p "$src" || exit 1

    # Generate, do not copy. 512 bytes of deterministic content per file so no
    # two runs differ and no run is flattered by cache warmth.
    python3 - "$src" "$n" <<'PY'
import sys, os
d, n = sys.argv[1], int(sys.argv[2])
for i in range(n):
    with open(os.path.join(d, "f%07d.dat" % i), "w") as fh:
        fh.write(("payload %07d " % i) * 7 + "\n")   # ~70 bytes
PY
    made=$(find "$src" -type f | wc -l)

    img="${PERF_VOLUME:-$WORK/vol-$n.img}"
    if [ -n "${PERF_VOLUME:-}" ]; then
        # A RAW DEVICE, not an image file. This matters for the numbers: an
        # image would put ext4 (and possibly a loop layer) between the engine
        # and the device, and the whole point is to compare like with like.
        # The volume is mkfs'd once by the caller and then REUSED across sizes
        # -- invf-import merges, so each run adds to the one volume and the
        # per-file cost is read as a delta rather than from a cold volume.
        if [ ! -b "$img" ]; then
            echo "  PERF_VOLUME=$img is not a block device" >&2
            continue
        fi
    else
        rm -f "$img"
        "$BIN/invf-mkfs" "$img" 1 >/dev/null 2>&1 || { echo "  mkfs failed for $n" >&2; continue; }
    fi

    dump="$WORK/dump-$n.txt"
    start=$(date +%s.%N)
    INVFS_PERF_DUMP=1 INVFS_PERF_DUMP_PATH="$dump" \
        "$BIN/invf-import" "$img" "$src" >"$WORK/import-$n.log" 2>&1
    rc=$?
    end=$(date +%s.%N)

    if [ $rc -ne 0 ] || [ ! -s "$dump" ]; then
        printf '%-8s %-8s %10s\n' "$made" "-" "IMPORT FAILED rc=$rc"
        continue
    fi
    secs=$(awk -v a="$start" -v b="$end" 'BEGIN{printf "%.1f", b-a}')
    # The dump pads every line to a fixed column, so the number is the LAST
    # field, not $2. Taking $2 silently yielded the literal "writes", awk's
    # +0 turned that into 0, and the table printed all zeros while looking
    # perfectly plausible -- which is the exact failure mode this script exists
    # to prevent, committed by the script meant to detect it.
    _w=$(awk '/^[[:space:]]*vmux writes[[:space:]]/      {w=$NF} END{print w+0}' "$dump")
    _wb=$(awk '/^[[:space:]]*vmux write bytes[[:space:]]/ {b=$NF} END{print b+0}' "$dump")
    _f=$(awk '/^[[:space:]]*vol_flush calls[[:space:]]/  {f=$NF} END{print f+0}' "$dump")
    if [ "${_w:-0}" -eq 0 ] && [ "${_wb:-0}" -eq 0 ]; then
        printf '%-8s %-8s %10s   <-- counters empty; is the dump format still this?\n' \
               "$made" "$secs"
        continue
    fi
    python3 - "$made" "$secs" "$_w" "$_wb" "$_f" <<'PY'
import sys
# secs is a float (awk prints e.g. 4.7); the counters are ints. Parsing all
# five as int() was the third bug in this script and aborted every row.
n = int(sys.argv[1]); secs = float(sys.argv[2])
w, wb, fl = (int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
print("%-8d %-8s %10.1f %11d %12.1f %10d %8.1f %10.1f" %
      (n, secs, n/float(secs or 1), w, wb/1048576.0, fl,
       float(w)/n, wb/float(n or 1)))
PY
    [ -n "${PERF_VOLUME:-}" ] || rm -rf "$img"   # sparse, but keep the working set small
done

echo
echo "write/file and MB_written/files are the columns to read: flat means a"
echo "fixed per-file cost; growing means the metadata tree is the bottleneck."
