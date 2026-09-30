#!/bin/bash
# measure-cpack-member-cost.sh — the MEASUREMENT behind the containerpack
# size guard's per-member term (src/core/vol_cpack.c, cpack_project()).
#
# CPACK_MEMBER_COST used to be a flat 4 pages/member, obtained by dividing
# ONE run's whole-image delta residual by its member count. This script
# measures the real marginal cost instead: it reports the VOLUME's own
# free-block count after a real decomposition, and the engine's own
# projection of the same shape, so the two can be compared member for
# member. Difference two member counts that carry the SAME per-member size
# distribution and every fixed per-sweep cost (fold, checkpoint, RT30 slots,
# COW page copies) cancels -- what is left is the marginal cost of one more
# member. See measure-cpack-member-cost-run.sh, which does the differing.
#
#   measure-cpack-member-cost.sh <label> <n> <size-spec> <content-kind>
#
#     label       free-form tag for the run
#     n           member count of the container
#     size-spec   "u:<lo>-<hi>"  uniform, or "log:<lo>-<hi>" log-uniform
#                 (bytes; a member is never empty)
#     content-kind "text" (real source files -- compresses, so the
#                 decomposition is a gain and the lane engages: this is the
#                 regime a rootfs is in) or "rand" (incompressible; the
#                 control that shows the DATA term alone)
#
# The container is the SPLT fixture pack (tools/codecpacks/splt_test), whose
# members are plain payload chunks: a decomposition is exactly "N files
# appear, 1 file disappears".
#
# The build under $BIN must have CPACK_MEMBER_COST small enough for the
# guard to accept, or the lane never runs and there is nothing to measure.
# The constant only gates the guard's verdict; once the lane is running the
# volume allocates what it allocates, so the numbers are the volume's.
#
# Output: one "RESULT <label> ..." line, machine-readable.
set -euo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
BIN="${BIN:-$REPO/bin}"
WORK="${WORK:-/srv/bench/mcost/run}"
IMGSIZE="${IMGSIZE:-4}"          # GB, sparse
SWEEPS="${SWEEPS:-3}"
PACKS="${PACKS:-$REPO/tools/codecpacks}"

LABEL="$1"; N="$2"; SIZESPEC="$3"; KIND="$4"

case "$SIZESPEC" in
  u:*)   MODE=u;  R=${SIZESPEC#u:};  LO=${R%-*}; HI=${R#*-} ;;
  log:*) MODE=log; R=${SIZESPEC#log:}; LO=${R%-*}; HI=${R#*-} ;;
  *) echo "bad size-spec: $SIZESPEC" >&2; exit 2 ;;
esac

D="$WORK/$LABEL"
rm -rf "$D"; mkdir -p "$D/src"
IMG="$WORK/$LABEL.img"
rm -f "$IMG"

# Every member is UNIQUE (a random prefix and a pseudo-random offset into
# the text pool): a corpus that repeats a file verbatim would be collapsed
# by the sweep's dedupe stage and the run would measure nothing.
python3 - "$D/src/cont.splt" "$N" "$MODE" "$LO" "$HI" "$KIND" "$REPO" <<'PY'
import math, os, random, struct, sys
out, n, mode, lo, hi, kind, repo = sys.argv[1:8]
n = int(n)
lo, hi = int(lo), int(hi)
rnd = random.Random(0xC0FFEE)

pool = []
for root, dirs, files in os.walk(os.path.join(repo, "src")):
    dirs.sort()
    for f in sorted(files):
        p = os.path.join(root, f)
        if os.path.getsize(p) > 4096:
            pool.append(open(p, "rb").read())
pool = pool[:400]
assert pool, "no text corpus"

sizes = []
for i in range(n):
    if mode == "u":
        sizes.append(rnd.randrange(lo, hi + 1))
    else:
        sizes.append(int(math.exp(rnd.uniform(math.log(lo), math.log(hi)))))

with open(out, "wb") as f:
    f.write(b"SPLT" + struct.pack("<I", n))
    f.write(b"".join(struct.pack("<Q", s) for s in sizes))
    for i, s in enumerate(sizes):
        if kind == "text":
            src = pool[rnd.randrange(len(pool))]
            off = rnd.randrange(len(src))
            body = (src[off:] + src[:off])
            f.write(rnd.randbytes(16) + (body * (s // len(body) + 1))[:s - 16])
        else:
            f.write(rnd.randbytes(s))
print("%d members, %d bytes container" % (n, os.path.getsize(out)), file=sys.stderr)
PY

INVFS_META_FRAC=16 "$BIN/invf-mkfs" "$IMG" "$IMGSIZE" >/dev/null
"$BIN/invf-import" "$IMG" "$D/src" >/dev/null

free() { "$BIN/invf-fsck" "$IMG" 2>/dev/null | sed -n 's/.*free blocks: *\([0-9]*\).*/\1/p' | head -1; }

F0=$(free)

# cpack_project() runs on every containerpack pass, before the guard is
# consulted, so the very same sweep that commits the decomposition also
# reports the engine's OWN ZSTD-19 projection of it (the measurement
# instrumentation). No separate projection pass is needed -- and running one
# first would stamp the file and make the lane skip it on the next sweep.
i=0
while [ "$i" -lt "$SWEEPS" ]; do
  INVFS_CPACK_MEASURE=1 INVFS_CODECPACKS="$PACKS" \
    "$BIN/invf-sweep" "$IMG" > "$D/sweep$((i+1)).log" 2>&1 || true
  i=$((i+1))
done
FN=$(free)
ENGAGED=$(grep -c "splt_test (codecpack)" "$D/sweep1.log" || true)
PROJ=$(grep -o "\[measure\].*" "$D/sweep1.log" | head -1 || true)
ACCT=$(grep -o "[0-9]* fixed + [0-9]* content + [0-9]* member-cost.*" "$D/sweep1.log" | head -1 || true)

TOTAL=$(stat -c %s "$IMG")
USED=$(( (TOTAL / 4096) - FN ))
printf 'RESULT %s n=%s engaged=%s orig=%s used_blocks=%s bytes=%s\n' \
  "$LABEL" "$N" "$ENGAGED" "$TOTAL" "$USED" "$((USED * 4096))"
[ -n "$PROJ" ] && echo "    $PROJ"
[ -n "$ACCT" ] && echo "    accounting: $ACCT"
rm -f "$IMG"          # the image is 2-4 GiB of address space; the logs stay
exit 0
