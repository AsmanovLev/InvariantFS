#!/bin/bash
# test-p7z-batch.sh — WP140: the containerpack lane must parse the p7z header
# a CONSTANT number of times, not once per member.
#
# THE BUG THIS GATES. `p7z extract` calls analyze(), which re-reads and
# re-parses the WHOLE 7z header -- and for the default LZMA-compressed
# header (kEncodedHeader) also fork/execs `7zz` to LZMA-decode it. The FS
# lane exec'd the pack once PER MEMBER, so packing an n-member container
# cost n header parses and n 7zz execs: O(n^2) in the header size. Measured
# on a real Copy-mode 7z of host /usr (84,279 members, 3,567,834,051 B):
# one parse took 207 s before WP140 and 0.092 s after, and the lane used to
# pay that n times -- the 13-30 day wall clock.
#
# WHY THE ASSERTION IS A COUNT, NOT A CLOCK. This box is shared and a
# wall-clock threshold is a flake generator: a busy neighbour turns a
# passing sweep red with no code change, which is how time gates get
# deleted. So the unit of assertion here is the pack's own header-parse
# counter (`$P7Z_STATS_FILE`), which is a fact about the code path and not
# about the machine:
#
#   control arm (the RED control, and it is the pre-fix behaviour):
#     n single-member `extract` calls  ->  parses == n        (n > 1)
#   fixed arm:
#     one `extract_batch` call         ->  parses == 1
#   and the FS lane:
#     one decomposition of an m-member container
#                                    ->  extract calls: 0, batch calls: small
#
# All of these are integers. None of them can flake under load.
#
# The pack's bytes are checked too: every member extract_batch writes is
# `cmp`'d against the file the per-member `extract` wrote, so a batch that
# parsed once and then handed back the wrong extents cannot pass.
#
# Run from the repo root after `make`:  bash tools/test-p7z-batch.sh

set -u
cd "$(dirname "$0")/.." || exit 1

B=$PWD/bin
PACK=tools/codecpacks/p7z.codecpack
P7Z=$PACK/bin/p7z
WORK=${INVFS_P7Z_BATCH_WORK:-$(mktemp -d "${TMPDIR:-/tmp}/invfs-p7z-batch.XXXXXX")}
NMEM=${INVFS_P7Z_BATCH_NMEM:-300}

checks=0
failures=0
ok() {
    checks=$((checks + 1))
    if [ "$1" = 0 ]; then
        printf '  OK    %s\n' "$2"
    else
        failures=$((failures + 1))
        printf '  FAIL  %s\n' "$2"
    fi
}

cleanup() {
    [ -n "${INVFS_P7Z_BATCH_KEEP:-}" ] || rm -rf "$WORK"
    # If this run WROTE the shim, take it away again. A bin/7zz left behind
    # is not a harmless leftover: p7z's resolve_7zz() finds it as a sibling,
    # so tools/test-p7z.sh's "no 7zz on this host" leg -- which asserts that
    # an encoded-header archive DECLINES without one -- would pass a host
    # that has one and fail for the wrong reason. Observed exactly that way.
    [ -z "${SHIM_MADE:-}" ] || rm -f "$PWD/$PACK/bin/7zz"
}
trap cleanup EXIT

if [ ! -x "$P7Z" ]; then
    echo "  FAIL  $P7Z is missing; run 'make helpers' first"
    exit 1
fi

# 7zz resolution. THE ENVIRONMENT FIXTURE, spelled out here so it is not a
# mystery later:
#
#   p7z's manifest declares `requires = 7zz`, and `requires` is BOTH a probe
#   gate (every name must resolve -- src/codecs/codec.c
#   pack_probe_impl/pack_tool_resolvable) AND the Landlock exec allowlist
#   for the pack's grandchild (src/core/helper_exec.c). A host that ships
#   7z / 7za / 7zr but not 7zz therefore reports the p7z lane as "tools
#   absent" -- indistinguishable, in a log, from a real size refusal.
#   `tools/codecpacks/*/bin/` is gitignored, so the usual remedy is a
#   sibling shim next to bin/p7z, which is exactly the second step of the
#   pack's own resolve_7zz(). This harness CREATES that shim when it has to,
#   so the gate below runs instead of silently skipping.
#
# The fixture uses the DEFAULT (LZMA-compressed) 7z header on purpose, so it
# is the leg that proves a batch does not fork 7zz once per member.
S7ZZ=""
for c in "$PWD/$PACK/bin/7zz" /usr/bin/7zz; do
    [ -x "$c" ] && { S7ZZ=$c; break; }
done
if [ -z "$S7ZZ" ]; then
    REAL=""
    for c in /usr/bin/7za /usr/bin/7z /usr/bin/7zr /usr/local/bin/7za; do
        [ -x "$c" ] && { REAL=$c; break; }
    done
    if [ -z "$REAL" ]; then
        echo "  SKIP  no 7z-family binary on this host at all: the" \
             "encoded-header fixture cannot be built. That is an" \
             "environment result, not a pass."
        exit 0
    fi
    mkdir -p "$PACK/bin"
    printf '#!/bin/sh\nexec %s "$@"\n' "$REAL" > "$PACK/bin/7zz"
    chmod +x "$PACK/bin/7zz"
    SHIM_MADE=1
    echo "  ....  no 7zz on this host; wrote the fixture shim" \
         "$PACK/bin/7zz -> $REAL"
    echo "        (bin/ is gitignored; the shim is the pack's own" \
         "resolve_7zz() second step, not a repo change)"
    S7ZZ="$PWD/$PACK/bin/7zz"
fi
export P7Z_7ZZ=$S7ZZ
echo "  ....  P7Z_7ZZ=$S7ZZ, $NMEM members, work $WORK"

mkdir -p "$WORK/src" "$WORK/one" "$WORK/batch" "$WORK/scr"

# ---------------------------------------------------------------- fixture
# Copy-method members (`-m0=Copy`), so the pack accepts the archive, with the
# DEFAULT header codec so the header itself is LZMA-compressed and every
# parse costs a 7zz exec too. Deterministic content: a fixed generator, no
# /dev/urandom, so the two arms compare byte-identical bytes.
python3 - "$WORK/src" "$NMEM" <<'PY'
import os, sys
d, n = sys.argv[1], int(sys.argv[2])
for i in range(n):
    sub = os.path.join(d, "d%02d" % (i % 8))
    os.makedirs(sub, exist_ok=True)
    # 4 KiB of deterministic, compressible text plus a unique tail, so every
    # member has distinct content of a realistic small-file size.
    body = ("wp140 member %d\n" % i) * 120
    open(os.path.join(sub, "m%05d.txt" % i), "w").write(body + "end %d\n" % i)
PY

( cd "$WORK" && "$S7ZZ" a -t7z -m0=Copy -bso0 -bsp0 "$WORK/fix.7z" ./src/* ) \
    >"$WORK/7zz.log" 2>&1
if [ ! -s "$WORK/fix.7z" ]; then
    echo "  FAIL  could not build the fixture archive"
    tail -5 "$WORK/7zz.log"
    exit 1
fi

"$P7Z" enumerate "$WORK/fix.7z" "$WORK/table" >/dev/null 2>&1
N=$(wc -l < "$WORK/table")
if [ "$N" -lt 1 ]; then
    echo "  FAIL  the fixture does not enumerate (exit $?); is it a Copy archive?"
    exit 1
fi
printf '  ....  fixture: %s members, %s bytes\n' "$N" "$(stat -c %s "$WORK/fix.7z")"

# The fixture must actually be a shape the pack accepts, or every arm below
# measures "declined", which would make the test pass for the wrong reason.
"$P7Z" extract "$WORK/fix.7z" 0 "$WORK/one/probe" >/dev/null 2>&1
ok $? "the fixture is a shape p7z accepts (single extract works)"

# ------------------------------------------------- leg 1: the RED control
# One `extract` per member, exactly what the lane did before WP140. The
# assertion is that the pack really does re-parse per call -- that is the
# premise of the whole fix, and it is a COUNT, so it holds on a loaded box.
STATS=$WORK/stats.control
: > "$STATS"
i=0
while IFS= read -r line; do
    idx=${line%%$'\t'*}
    P7Z_STATS_FILE=$STATS "$P7Z" extract "$WORK/fix.7z" "$idx" \
        "$WORK/one/$idx" >/dev/null 2>&1
    i=$((i + 1))
done < "$WORK/table"

CTRL_PARSES=$(awk -F'parses=' '/parses=/{s+=$2} END{print s+0}' "$STATS")
CTRL_CALLS=$(wc -l < "$STATS")
ok $([ "$CTRL_CALLS" = "$N" ] && echo 0 || echo 1) \
   "control arm: $N single-member extract calls, $CTRL_CALLS observed"
ok $([ "$CTRL_PARSES" = "$N" ] && echo 0 || echo 1) \
   "control arm: $CTRL_PARSES header parses for $N members (1 per call) --" \
   "this is the quadratic term, counted"

# ------------------------------------------------------ leg 2: extract_batch
STATS=$WORK/stats.batch
: > "$STATS"
P7Z_STATS_FILE=$STATS "$P7Z" extract_batch "$WORK/fix.7z" "$WORK/table" \
    "$WORK/batch" >/dev/null 2>&1
BRC=$?
BATCH_CALLS=$(wc -l < "$STATS")
BATCH_PARSES=$(awk -F'parses=' '/parses=/{s+=$2} END{print s+0}' "$STATS")

ok $BRC "extract_batch runs clean on the fixture"
ok $([ "$BATCH_CALLS" = 1 ] && echo 0 || echo 1) \
   "extract_batch: $BATCH_CALLS pack invocation(s) for $N members (1)"
ok $([ "$BATCH_PARSES" = 1 ] && echo 0 || echo 1) \
   "extract_batch: $BATCH_PARSES header parse(s) for $N members --" \
   "constant, not per member"

# And the ratio that matters, stated as a ratio rather than a deadline:
# one decomposition went from N parses to 1.
ok $([ "$CTRL_PARSES" -gt 1 ] && echo 0 || echo 1) \
   "parse-count ratio ${CTRL_PARSES}:${BATCH_PARSES} ($CTRL_PARSES before," \
   "$BATCH_PARSES after) -- $N members, and the 'after' does not grow with n"

# ------------------------------------------------- leg 3: the BYTES are equal
# A batch that parses once and splices the wrong extents would sail through
# both count assertions above. cmp every member against what the per-member
# path produced.
MISMATCH=0
while IFS= read -r line; do
    idx=${line%%$'\t'*}
    cmp -s "$WORK/one/$idx" "$WORK/batch/$idx" || MISMATCH=$((MISMATCH + 1))
done < "$WORK/table"
ok $([ "$MISMATCH" = 0 ] && echo 0 || echo 1) \
   "every member extract_batch wrote is byte-identical to the per-member" \
   "extract output ($MISMATCH mismatches over $N members)"

# ... and against the ORIGINAL file in the fixture tree, which is the real
# bit-exactness claim: the batch hands back the member, not a plausible one.
MISMATCH=0
while IFS= read -r line; do
    idx=${line%%$'\t'*}
    rest=${line#*$'\t'}
    name=${rest%%$'\t'*}
    for src in "$WORK"/src/*/"$name"; do
        [ -f "$src" ] || continue
        cmp -s "$src" "$WORK/batch/$idx" || MISMATCH=$((MISMATCH + 1))
    done
done < "$WORK/table"
ok $([ "$MISMATCH" = 0 ] && echo 0 || echo 1) \
   "every extracted member is byte-identical to the file it was archived" \
   "from ($MISMATCH mismatches over $N members)"

# ------------------------------------------------------------ leg 4: the lane
# The engine half: a real mkfs -> invf-cp -> invf-sweep must reach the pack's
# extract through ONE batch call and ZERO per-member extract calls. The
# counter the lane prints is an integer count, so this cannot flake; without
# WP140 the line is absent and the arm fails.
IMG=$WORK/lane.img
mkdir -p "$WORK/scr"
export INVFS_TOOL_SCRATCH=$WORK/scr
export INVFS_SCRATCH_ROOTS=$WORK/scr
export INVFS_CODECPACKS=$PWD/tools/codecpacks
export INVFS_CODECPACKS_SYS=0
export INVFS_DEBUG_PACKS=1

( cd "$WORK" && "$B/invf-mkfs" "$IMG" 0.2 ) >/dev/null 2>&1
"$B/invf-cp" "$IMG" "$WORK/fix.7z" fix.7z >/dev/null 2>&1
( cd "$WORK" && "$B/invf-sweep" "$IMG" ) > "$WORK/lane.log" 2>&1
LANE_RC=$?

if grep -q 'extract_batch: [0-9]* call(s), [0-9]* members (extract calls: 0)' \
       "$WORK/lane.log"; then
    BC=$(grep -o 'extract_batch: [0-9]* call(s)' "$WORK/lane.log" | head -1 \
         | grep -o '[0-9]*')
    ok 0 "the FS lane extracted $N members via extract_batch" \
          "$(grep -o 'extract_batch: [0-9]* call(s), [0-9]* members (extract calls: [0-9]*)' "$WORK/lane.log" | head -1)"
    ok 0 "the FS lane made ZERO per-member extract calls ($BC batch calls" \
          "for $N members)"
    # The fixture is one small container, so it fits one slice. The bound is
    # deliberately loose -- a slice is 256 MiB by default and this fixture is
    # well under 1 MiB -- because what it pins is that the count does NOT
    # scale with the member count, which is the whole claim. A regression to
    # per-member execs prints 0 here and fails the line above.
    ok $([ "${BC:-0}" -ge 1 ] && [ "${BC:-0}" -le 8 ] && echo 0 || echo 1) \
       "the FS lane's pack calls for $N members are 1..8 ($BC), not $N"
else
    ok 1 "the FS lane did NOT use extract_batch -- it fell back to one" \
          "extract exec per member, which is the O(n^2) this gates" \
          "(sweep rc=$LANE_RC)"
fi

# The lane must still decompose: a lane that quietly stopped would satisfy
# every count above. The container stamp is what says the members landed.
if "$B/invf-ls" "$IMG" > "$WORK/ls.txt" 2>&1; then
    MBR=$(grep -c '!mbr[0-9]' "$WORK/ls.txt" || true)
    ok $([ "${MBR:-0}" -ge "$N" ] && echo 0 || echo 1) \
       "the lane really decomposed the container ($MBR member siblings for" \
       "$N members) -- the count assertions above are not passing on a" \
       "lane that did nothing"
else
    ok 1 "invf-ls failed on the lane image"
fi

# And the members themselves must still read back bit-exact from the volume.
BADREAD=0
while IFS= read -r line; do
    idx=${line%%$'\t'*}
    rest=${line#*$'\t'}
    name=${rest%%$'\t'*}
    "$B/invf-cat" "$IMG" "/fix.7z!mbr$(printf '%04d' "$idx")-$name" \
        > "$WORK/readback" 2>/dev/null || {
        BADREAD=$((BADREAD + 1)); continue; }
    for src in "$WORK"/src/*/"$name"; do
        [ -f "$src" ] || continue
        cmp -s "$src" "$WORK/readback" || BADREAD=$((BADREAD + 1))
    done
done < "$WORK/table"
ok $([ "$BADREAD" = 0 ] && echo 0 || echo 1) \
   "every member read back from the VOLUME with invf-cat is byte-identical" \
   "to its source file ($BADREAD mismatches over $N members). invfs-verify" \
   "--deep is NOT the oracle here: it checks readability and LENGTH only" \
   "(src/cli/verify.c)"

echo
printf 'p7z batch (header-parse count): %d checks, %d failures\n' \
       "$checks" "$failures"
[ "$failures" -eq 0 ]
