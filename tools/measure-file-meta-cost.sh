#!/bin/bash
# measure-file-meta-cost.sh — the METADATA half of the marginal cost, on its
# own. measure-cpack-member-cost.sh measures a whole decomposition (data +
# metadata); this one removes the data term by making the files empty, so
# what is left between N and 2N is what the engine spends per FILE of pure
# bookkeeping: the inode row, the dirent and the delta record. Those are the
# same three objects a container member creates (the fourth, the recipe row,
# is the container's alone and is ~81 B by the code's own accounting).
#
#   measure-file-meta-cost.sh <n>
set -euo pipefail
BIN="${BIN:-/srv/bench/mcost/bin-low}"
WORK="${WORK:-/dev/shm/mcost/meta}"
IMGSIZE="${IMGSIZE:-1}"
N="$1"

D="$WORK/n$N"; rm -rf "$D"; mkdir -p "$D/src"
IMG="$WORK/n$N.img"; rm -f "$IMG"
python3 -c "
import os,sys
d=sys.argv[1]; n=int(sys.argv[2])
for i in range(n): open(os.path.join(d,'f%06d'%i),'wb').write(b'x')
" "$D/src" "$N"

INVFS_META_FRAC=16 "$BIN/invf-mkfs" "$IMG" "$IMGSIZE" >/dev/null
"$BIN/invf-import" "$IMG" "$D/src" >/dev/null
"$BIN/invf-sweep" "$IMG" > "$D/sweep1.log" 2>&1 || true
"$BIN/invf-sweep" "$IMG" > "$D/sweep2.log" 2>&1 || true
F=$("$BIN/invf-fsck" "$IMG" 2>/dev/null | sed -n 's/.*free blocks: *\([0-9]*\).*/\1/p' | head -1)
T=$(stat -c %s "$IMG")
rm -f "$IMG"
echo "RESULT meta-only n=$N used_blocks=$(( (T/4096) - F ))"
