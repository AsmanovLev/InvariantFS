#!/bin/bash
# measure-cpack-member-cost-run.sh — the matrix behind
# CPACK_MEMBER_COST, and the differencing that turns it into a marginal
# cost. Drives measure-cpack-member-cost.sh once per (members, size) cell
# and prints, for each pair, the MEASURED marginal cost of one more member
# and its two parts:
#
#   data  = the block-rounded ZSTD-19 projection of the member payloads,
#           which is the only size-dependent term (a 100 B member pays a
#           whole 4 KiB block, a 100 KiB member pays only its own blocks)
#   meta  = measured_marginal - data, i.e. everything else the volume
#           spends per member: the recipe row, the inode row, the dirent and
#           the delta record, all of which pack into shared COW B+ tree
#           pages rather than taking a page each.
#
# Differencing N against 2N at a fixed size distribution cancels every
# fixed per-sweep cost, so what is left is the true marginal.
set -euo pipefail
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
BIN="${BIN:-/srv/bench/mcost/bin-low}"
M="$REPO/tools/measure-cpack-member-cost.sh"
OUT="${OUT:-/srv/bench/mcost/measurements.txt}"

: > "$OUT"
run() {   # run <label> <n> <sizespec> <kind>
  local line
  line=$(BIN="$BIN" "$M" "$@" 2>/dev/null | grep '^RESULT')
  echo "    $line" | tee -a "$OUT" >/dev/null
  echo "$line" >> "$OUT"
}

# marginal <label> <n1> <sizespec> <kind> <n2>
marginal() {
  local tag=$1 n1=$2 spec=$3 kind=$4 n2=$5
  local r1 r2 b1 b2 d1 d2 m1 m2
  r1=$(BIN="$BIN" "$M" "$tag-n1" "$n1" "$spec" "$kind" 2>/dev/null)
  r2=$(BIN="$BIN" "$M" "$tag-n2" "$n2" "$spec" "$kind" 2>/dev/null)
  echo "$r1" >> "$OUT"; echo "$r2" >> "$OUT"
  b1=$(echo "$r1" | sed -n 's/.*used_blocks=\([0-9]*\).*/\1/p')
  b2=$(echo "$r2" | sed -n 's/.*used_blocks=\([0-9]*\).*/\1/p')
  d1=$(echo "$r1" | sed -n 's/.*data_blocks=\([0-9]*\).*/\1/p'); d1=${d1:-0}
  d2=$(echo "$r2" | sed -n 's/.*data_blocks=\([0-9]*\).*/\1/p'); d2=${d2:-0}
  m1=$(echo "$r1" | sed -n 's/.*content=\([0-9]*\).*/\1/p'); m1=${m1:-0}
  m2=$(echo "$r2" | sed -n 's/.*content=\([0-9]*\).*/\1/p'); m2=${m2:-0}
  if [ -z "$b1" ] || [ -z "$b2" ]; then
    echo "  $tag: NO RESULT"; return 1
  fi
  awk -v tag="$tag" -v n1="$n1" -v n2="$n2" -v b1="$b1" -v b2="$b2" \
         -v d1="$d1" -v d2="$d2" -v m1="$m1" -v m2="$m2" 'BEGIN {
    dn = n2 - n1
    meas = (b2 - b1) * 4096 / dn
    data = (d2 - d1) * 4096 / dn
    meta = meas - data
    raw  = (m2 - m1) / dn
    printf "  %-22s n=%-6s used=%s->%s  MARGINAL %8.0f B/member  = data %7.0f (%5.1f%%) + meta %7.0f (%5.1f%%)   [raw %.0f B, unrounded data %.0f B]\n",
        tag, dn, b1, b2, meas, data, 100*data/meas, meta, 100*meta/meas, raw, (m2-m1)/dn
  }' | tee -a "$OUT"
}

echo "== marginal cost of ONE more member (N vs 2N, fixed size mix) =="
echo "== regime A: archive-shaped (1 KiB - 64 KiB, ~201 members) =="
marginal regA-archive 201 log:1024-65536 text 402
echo "== regime B: rootfs-shaped (1 KiB - 8 KiB, tens of thousands) =="
marginal regB-rootfs 12500 u:1024-8192 text 25000
echo "== member-size sweep at fixed member count: isolates the size term =="
marginal size-1k   2000 u:1024-1024   text 4000
marginal size-4k   2000 u:4096-4096   text 4000
marginal size-16k  2000 u:16384-16384 text 4000
marginal size-64k  2000 u:65536-65536 text 4000
echo
echo "== absolute scale: a real 50,000-member rootfs container =="
run abs-50k 50000 u:1024-8192 text
echo "(measurements in $OUT)"
