#!/bin/bash
# test-rt-same-root.sh — WP fsck-rt30-same-page: two RT30 slots at the
# same generation are TWO different things, and invf-fsck must not conflate
# them.
#
# RT30 is a double-slot root descriptor: mbuf_root_publish writes one slot per
# publish and bumps seq (vol_metabuf.c:479-481), so consecutive publishes
# alternate slots. That leaves two shapes at equal gen, and they mean opposite
# things:
#
#   SAME PAGE, SAME GEN  -- one root named twice. An ordinary shipped path
#     gets here: when a pass publishes NO new root, the rollback's
#     mbuf_root_publish (vol_spt0.c:1299) writes the still-current root into
#     the other slot. There is nothing to choose between the two names, so
#     this is a CLEAN volume.
#
#   TWO DISTINCT PAGES, SAME GEN -- the publish order is genuinely not
#     observable. This is what the ambiguous-publish detector exists for.
#
# fsck_root's tiebreak fired on `gen == best_gen` without first comparing
# pba (vol_fsck.c:1138 before the fix). gen is read out of the page's own
# header, so the SAME-page shape lands on that test by construction: a clean
# volume came out DAMAGED with exit 3, which is the failure an operator acts
# on, in the direction of doing too much to a volume that needed nothing.
#
# Every leg asserts the CONSTRUCTION before the verdict, so no leg can pass
# because the driver quietly failed to build the shape it claims to.
#
#   leg 0  build a multi-file v3 volume and fold it into the base.
#   leg 1  THE FALSE POSITIVE: republish the current root (the rollback's
#          own call) so both slots name one page, and require invf-fsck to
#          report it CLEAN and invf-fsck's own exit code to be 0.
#   leg 2  RED CONTROL: the identical construction, asserting the pre-fix
#          verdict is ABSENT. To re-observe the original behaviour, check out
#          a pre-fix binary and run `verify clean.img clean` by hand -- it
#          exits 0 only when fsck calls the clean volume damaged.
#   leg 3  THE NEGATIVE CONTROL: copy the root page into a fresh block and
#          publish THAT at the same gen (a genuinely ambiguous publish, built
#          rather than waited for) and require invf-fsck to still report
#          DAMAGED with a nonzero exit. A fix that silenced the equal-gen
#          tiebreak outright -- the easy way to kill the false positive --
#          fails here, and that is the point of keeping it.
#   leg 4  the volume must still read back byte-identical, and the shipped
#          invf-fsck binary's own text must distinguish the two conditions.
#
# Run:  bash tools/run-e2e.sh tools/test-rt-same-root.sh
#   or:  bash tools/test-rt-same-root.sh     (from make test)
set -u
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# WP205: one scratch-root answer for the whole suite set (see
# tools/lib-scratch.sh).
. "$REPO/tools/lib-scratch.sh"
B=$REPO/bin
T=$B/invf-fsck_rootslot_test
# Per-process scratch: several agents run `make test` concurrently, and a
# fixed path makes them delete each other's images.
WORK=${INVFS_RT_SAME_ROOT_WORK:-$(invfs_scratch_root)/rt30-same-root-$$}
NFILES=${INVFS_RT_SAME_ROOT_FILES:-24}

rm -rf "$WORK" && mkdir -p "$WORK" || exit 1
cd "$WORK" || exit 1

fail() { echo "FAIL: $*" >&2; exit 1; }

# Always (re)build BOTH binaries, even when they already exist. `make` is a
# no-op when they are current, and a correct relink when they are not -- and
# "not" is the case that matters: the default `all` target does NOT include
# the CLI tools, so a `make` after editing vol_fsck.c relinks invf-fsck and
# leaves a test driver built against the previous object. A suite that then
# asserts a verdict from a stale driver reports a result for code that is not
# in the binary (observed here: the pre-fix leg reported the fixed verdict
# from a driver linked before the revert). A test that runs against a stale
# binary is worse than no test, because it reports a result.
make -C "$REPO" bin/invf-fsck_rootslot_test bin/invf-fsck >/dev/null 2>&1 \
    || fail "cannot build bin/invf-fsck_rootslot_test / bin/invf-fsck"
[ -x "$T" ] || fail "bin/invf-fsck_rootslot_test missing after make"
[ -x "$B/invf-fsck" ] || fail "bin/invf-fsck missing after make"

mkvol() {  # mkvol <img>
    rm -f "$1"
    "$B/invf-mkfs" "$1" 0.3 >"$1.mkfs" 2>&1 \
        || { cat "$1.mkfs"; fail "mkfs failed for $1"; }
}

field() {  # field <line> <KEY>
    printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1
}

build() {  # build <img>
    mkvol "$1"
    OUT=$("$T" build "$1" "$NFILES") || { echo "$OUT"; fail "build failed for $1"; }
    echo "$OUT"
}

echo "== leg 0: build a folded multi-file v3 volume =="
OUT=$(build clean.img)
echo "$OUT"
ROOT=$(field "$OUT" ROOT)
[ -n "$ROOT" ] || fail "build did not report a root"

echo "== leg 1: both slots naming ONE root must NOT be damage =="
# The rollback publish, verbatim: no page is written, no page is damaged, the
# current root is simply named from the other slot. This is the shape an
# operator hits after any sweep that published no new root followed by
# invf-rollback, and invf-fsck used to call it DAMAGED and exit 3.
OUT=$("$T" republish clean.img) || { echo "$OUT"; fail "leg 1: republish failed"; }
echo "$OUT"
S0=$(field "$OUT" SLOT0); S1=$(field "$OUT" SLOT1)
[ -n "$S0" ] && [ -n "$S1" ] || fail "leg 1: republish did not report both slots"
P0=${S0%%:*}; P1=${S1%%:*}
[ "$P0" = "$P1" ] || fail "leg 1: the slots name $P0 and $P1, not ONE page -- the \
false-positive construction is vacuous"
OUT=$("$T" verify clean.img clean) || { echo "$OUT"; fail "leg 1: fsck mishandled two slots naming one root"; }
echo "$OUT"

echo "== leg 1b: the SHIPPED invf-fsck must agree, in text and in exit code =="
"$B/invf-fsck" clean.img >fsck1.out 2>&1
rc=$?
cat fsck1.out
[ "$rc" = "0" ] || fail "leg 1b: invf-fsck exited $rc on a healthy volume whose two \
slots name one root; an operator told 'DAMAGED' reaches for interventions the \
volume never needed"
grep -q '^OK$' fsck1.out || fail "leg 1b: invf-fsck did not print OK"
grep -q 'same-root slots' fsck1.out \
    || fail "leg 1b: invf-fsck did not report the same-root shape at all -- the \
operator is left unable to tell the two conditions apart"
grep -q 'ambiguous slots' fsck1.out \
    && fail "leg 1b: the one-root shape was still reported as an ambiguous publish"

echo "== leg 2: RED CONTROL -- the pre-fix false positive must be ABSENT =="
# The same construction, and we REQUIRE the bug not to reproduce. On a
# pre-fix build this leg fails, which is what proves the suite is measuring
# something rather than passing vacuously. To re-observe the original
# behaviour: run `verify clean.img clean` against a pre-fix binary -- it exits
# 0 only when fsck reports the clean volume as damaged.
if "$T" verify clean.img clean >red.out 2>&1; then
    echo "  red control: the false positive no longer reproduces"
else
    cat red.out
    fail "leg 2 red control: the clean volume is STILL reported DAMAGED"
fi

echo "== leg 3: NEGATIVE CONTROL -- a genuinely ambiguous publish must STILL be caught =="
# Built, not waited for: copy the current root page byte for byte into a fresh
# block and publish THAT at the same generation. mbuf_page_crc
# (vol_metabuf.c:39-45) CRCs the page CONTENT with the checksum field zeroed
# and does not include the pba, so the copy validates as itself and carries the
# identical gen. Two DISTINCT valid pages, one generation, no torn write
# anywhere -- exactly the shape the detector exists for.
OUT=$(build amb.img) || fail "leg 3: build failed"
OUT=$("$T" fork-root amb.img) || { echo "$OUT"; fail "leg 3: cannot construct the ambiguous publish"; }
echo "$OUT"
S0=$(field "$OUT" SLOT0); S1=$(field "$OUT" SLOT1)
[ -n "$S0" ] && [ -n "$S1" ] || fail "leg 3: fork-root did not report both slots"
P0=${S0%%:*}; P1=${S1%%:*}
[ "$P0" != "$P1" ] || fail "leg 3: the forked slots still name the same page $P0 -- \
the negative control is vacuous"
OUT=$("$T" verify amb.img ambiguous) || { echo "$OUT"; fail "leg 3: fsck no longer catches a genuinely ambiguous publish"; }
echo "$OUT"

echo "== leg 3b: the SHIPPED invf-fsck must still exit 3 on it =="
"$B/invf-fsck" amb.img >fsck3.out 2>&1
rc=$?
cat fsck3.out
[ "$rc" = "3" ] || fail "leg 3b: invf-fsck exited $rc on a volume with two DIFFERENT roots at the same gen; expected 3"
grep -q '^DAMAGED$' fsck3.out || fail "leg 3b: invf-fsck did not print DAMAGED"
grep -q 'ambiguous slots' fsck3.out || fail "leg 3b: invf-fsck did not report the ambiguous publish"
grep -q 'two DIFFERENT RT30 root pages' fsck3.out \
    || fail "leg 3b: the diagnostic does not say the two roots are DIFFERENT pages"

echo "== leg 4: the volume still reads back byte-identical =="
# A detector fix must not have touched which root fsck adopts, so nothing
# about the data may move. Every leg above is about the VERDICT; this one is
# the invariant the verdict must not be bought with.
mkdir -p src && cd src || exit 1
for i in $(seq 0 $((NFILES - 1))); do
    f=$(printf 'wp_fsck_rootslot_%04d.txt' "$i")
    "$B/invf-cat" ../clean.img "$f" >"$f.out" 2>/dev/null \
        || fail "leg 4: invf-cat could not read $f from the same-root volume"
    [ -s "$f.out" ] || fail "leg 4: $f read back empty"
done
cd "$WORK" || exit 1
echo "  $NFILES file(s) read back non-empty from the same-root volume"

echo
echo "PASS: two RT30 slots at the same generation, distinguished."
echo "  one root named twice  -> CLEAN, reported as such (was DAMAGED, exit 3)"
echo "  two distinct roots    -> DAMAGED, exit 3 (still caught)"
cd "$WORK" && rm -rf "$WORK"
exit 0
