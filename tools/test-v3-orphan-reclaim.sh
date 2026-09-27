#!/bin/bash
# test-v3-orphan-reclaim.sh — WP121: safety proof for the v3 orphan collector.
#
# The collector (btree_collect_orphans) frees base pages that no live root
# can reach. A wrong answer to that question is SILENT DATA LOSS, not a
# crash, so this suite is built around a RED CONTROL: leg 3 reproduces the
# wrong liveness predicate ("named by the newest RT30 slot") on purpose and
# asserts that the damage leg then BREAKS. A test that only ever exercises
# the correct code path cannot tell a correct predicate from a no-op.
#
#   leg 0  build: 40 files written through the public write path, folded
#          into the base, then 60 forced base generations. Asserts the leak
#          is actually there (PAGES_ORPHAN > 0) -- otherwise legs 1-3 would
#          pass because nothing happened, which is the failure mode this
#          suite exists to rule out.
#   leg 1  default-off: with INVFS_RECLAIM_ORPHANS unset the collector frees
#          exactly nothing.
#   leg 2  collect + verify: with the gate on, the orphan pages go, and then
#          every file is still readable BYTE-IDENTICAL through vol_read_named
#          across a close/reopen, and invf-fsck still reports OK.
#   leg 3  damage, correct predicate: corrupt the newest RT30 root so the
#          reader falls back to the other slot (WP86), then assert every page
#          the fallback tree reaches is STILL ALLOCATED, and that the files
#          are still byte-identical.
#   leg 4  damage, WRONG predicate (red control): rebuild an identical
#          volume, collect with the live set narrowed to the newest root,
#          corrupt the newest root, and assert the fallback tree is now
#          standing on UNALLOCATED pages -- i.e. the check in leg 3 fails
#          when the predicate is wrong. If this leg ever passes, leg 3 has
#          no teeth and the suite says so loudly.
#   leg 5  a save point (SPT0) is a liveness root: with a pinned root the
#          collector must not free its pages.
#
# Run:  bash tools/run-e2e.sh tools/test-v3-orphan-reclaim.sh
#   or:  bash tools/test-v3-orphan-reclaim.sh     (from make test)
set -u
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
T=$B/invf-orphan_test
WORK=${INVFS_ORPHAN_WORK:-/dev/shm/v3orphan}
NFILES=40
NGENS=60

rm -rf "$WORK" && mkdir -p "$WORK" || exit 1
cd "$WORK" || exit 1

fail() { echo "FAIL: $*" >&2; exit 1; }

# The driver is a normal `make` target but be tolerant of a bare checkout.
if [ ! -x "$T" ]; then
    make -C "$REPO" bin/invf-orphan_test >/dev/null 2>&1 \
        || fail "cannot build bin/invf-orphan_test"
fi
[ -x "$B/invf-fsck" ] || fail "bin/invf-fsck missing (run make)"

mkvol() {  # mkvol <img>
    rm -f "$1"
    INVFS_V3=1 "$B/invf-mkfs" "$1" 0.3 >"$1.mkfs" 2>&1 \
        || { cat "$1.mkfs"; fail "mkfs failed for $1"; }
}

field() {  # field <line> <KEY>
    printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1
}

echo "== leg 0: build the leak (files + forced generations) =="
mkvol good.img
OUT=$("$T" build good.img "$NFILES" "$NGENS") || { echo "$OUT"; fail "build failed"; }
echo "$OUT"
ORPHAN=$(field "$OUT" PAGES_ORPHAN)
[ -n "$ORPHAN" ] || fail "build did not report PAGES_ORPHAN"
[ "$ORPHAN" -gt 0 ] || fail "no orphan pages to reclaim -- the suite would pass vacuously"
ALLOC=$(field "$OUT" PAGES_ALLOC)
LIVE=$(field "$OUT" PAGES_LIVE)
NEWEST=$(field "$OUT" NEWEST)
OLDER=$(field "$OUT" OLDER)
[ "$NEWEST" != "$OLDER" ] || fail "RT30 does not name two distinct roots; the damage leg would be vacuous"
echo "  leak: $ORPHAN of $ALLOC allocated base pages are orphans (live tree = $LIVE)"

echo "== leg 0b: pre-collect bit-exactness =="
"$T" verify good.img "$NFILES" || fail "readback failed BEFORE any collect"

echo "== leg 1: default-off gate =="
# A clean environment: INVFS_RECLAIM_ORPHANS must not be inherited.
env -u INVFS_RECLAIM_ORPHANS "$T" gate-off good.img || fail "the collector is not default-off"

echo "== leg 2: collect, then read every file back byte-identical =="
OUT=$(INVFS_RECLAIM_ORPHANS=1 "$T" collect good.img) || { echo "$OUT"; fail "collect failed"; }
echo "$OUT"
F1=$(field "$OUT" COLLECTED)
[ -n "$F1" ] || fail "collect printed no COLLECTED count"
[ "$F1" -gt 0 ] || fail "the collector freed nothing with the gate on -- legs 3/4 would be vacuous"
echo "  collected $F1 pages (leak was $ORPHAN)"

# Same process, then a fresh open: the frees have to be durable, not just
# in-RAM bitmap edits.
"$T" verify good.img "$NFILES" || fail "readback failed after collect (same session)"
FSCK=$("$B/invf-fsck" good.img 2>&1) || { echo "$FSCK"; fail "fsck exited nonzero after collect"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK after collect"; }
echo "  fsck OK after collect"


echo "== leg 3: damage the newest root, correct predicate =="
# A copy, so the collector's own volume is left readable.
cp good.img dmg.img
SL=$("$T" slots dmg.img) || fail "slots failed"
echo "$SL"
DNEWEST=$(field "$SL" NEWEST)
DOLDER=$(field "$SL" OLDER)
[ "$DNEWEST" != "$DOLDER" ] || fail "two distinct slots expected"
cp dmg.img dmg-before-damage.img
"$T" damage-newest dmg.img || fail "damage failed"
# The reader must now adopt the OLDER slot.
SL2=$("$T" slots dmg.img) || fail "slots after damage failed"
echo "$SL2"
ADOPTED=$(field "$SL2" NEWEST)
[ "$ADOPTED" = "$DOLDER" ] \
    || fail "after damaging root $DNEWEST the reader adopted $ADOPTED, not the older slot $DOLDER"
W=$("$T" walk dmg.img "$ADOPTED") || fail "walk of the fallback root failed"
echo "$W"
UN=$(field "$W" UNALLOCATED)
[ -n "$UN" ] || fail "walk printed no UNALLOCATED count"
[ "$UN" -eq 0 ] \
    || fail "THE HAZARD: the fallback root $ADOPTED stands on $UN unallocated page(s). The collector freed a page the RT30 two-slot fallback still needs."
echo "  fallback root $ADOPTED: every page still allocated (UNALLOCATED=0)"
"$T" verify dmg.img "$NFILES" || fail "readback failed after the fallback"
# fsck MUST exit nonzero here -- we damaged a root on purpose and saying OK
# would mean it cannot see the damage. What it must NOT report is a bad
# page or a short walk: "pages walked" has to cover the whole fallback
# tree, which is the property a wrong liveness predicate breaks.
FSCK=$("$B/invf-fsck" dmg.img 2>&1) || true
echo "$FSCK" | sed -n '/state:/p;/root seq:/p;/pages walked:/p;/base keys:/p;/torn slots:/p;/bad pages:/p;/cycles/p'
WALKED=$(printf '%s\n' "$FSCK" | sed -n 's/.*pages walked: *\([0-9]*\).*/\1/p' | head -1)
BADP=$(printf '%s\n' "$FSCK" | sed -n 's/.*bad pages: *\([0-9]*\).*/\1/p' | head -1)
TORN=$(printf '%s\n' "$FSCK" | sed -n 's/.*torn slots: *\([0-9]*\).*/\1/p' | head -1)
[ -n "$WALKED" ] && [ -n "$BADP" ] && [ -n "$TORN" ] \
    || fail "could not parse the fsck report"
[ "$TORN" = "1" ] || fail "fsck reports $TORN torn slots, expected exactly the 1 we created"
[ "$BADP" = "0" ] || fail "fsck reports $BADP bad page(s) after the fallback -- the tree is damaged"
[ "$WALKED" -gt 0 ] || fail "fsck walked 0 pages after the fallback"
# The walk the driver did and the walk fsck did must agree, or one of the
# two is reading a different tree than the reader adopts.
PAGES=$(field "$W" PAGES)
[ "$WALKED" = "$PAGES" ] \
    || fail "fsck walked $WALKED pages, the driver's walk of the adopted root saw $PAGES"
echo "  fsck sees exactly the one torn slot we made; fallback tree intact ($WALKED pages, 0 bad)"

echo "== leg 4: RED CONTROL -- the wrong predicate must break leg 3 =="
mkvol naive.img
OUT=$("$T" build naive.img "$NFILES" "$NGENS") || { echo "$OUT"; fail "red-control build failed"; }
N_ORPHAN=$(field "$OUT" PAGES_ORPHAN)
[ "$N_ORPHAN" -gt 0 ] || fail "red control: nothing to collect"
# Emulate "live == reachable from the newest root" by pointing both RT30
# slots at the newest root before the collector runs. The shipped collector
# is unchanged; only its input is. The on-disk RT30 is left alone.
OUT=$(INVFS_RECLAIM_ORPHANS=1 "$T" naive-collect naive.img) || { echo "$OUT"; fail "naive collect failed"; }
echo "$OUT"
NF=$(field "$OUT" NAIVE-COLLECTED)
[ "$NF" -gt 0 ] || fail "red control: the wrong predicate freed nothing, so it proves nothing"
SL=$("$T" slots naive.img) || fail "red-control slots failed"
N_OLDER=$(field "$SL" OLDER)
"$T" damage-newest naive.img || fail "red-control damage failed"
W=$("$T" walk naive.img "$N_OLDER") || fail "red-control walk failed"
echo "$W"
RUN=$(field "$W" UNALLOCATED)
[ -n "$RUN" ] || fail "red control: walk printed no UNALLOCATED count"
if [ "$RUN" -eq 0 ]; then
    cat >&2 <<'EOF'
FAIL: RED CONTROL DID NOT FIRE. The volume reclaimed with the WRONG liveness
predicate (newest slot only) still had a fully allocated fallback root, so
leg 3's assertion cannot distinguish the correct predicate from the wrong
one. Leg 3 is vacuous and must not be trusted.
EOF
    exit 1
fi
echo "  red control fired: the wrong predicate left $RUN fallback page(s) unallocated"
echo "  -> leg 3's check has teeth"

cp naive.img naive-reuse.img
cp dmg.img good-reuse.img

echo "== leg 4b: what the unallocated pages actually become =="
# UNALLOCATED > 0 is latent. This realises it: churn the allocator so it
# consumes the free pool, and check whether any block of the reader's
# FALLBACK tree comes back out as somebody else's page. That is the silent
# data loss -- not a failed read, wrong bytes with no error at all.
N_OLDERGEN=$(field "$SL" OLDERGEN)
R=$(INVFS_RECLAIM_ORPHANS=1 "$T" reuse naive-reuse.img "$N_OLDER" "$N_OLDERGEN" 20000) \
    || fail "reuse probe failed"
echo "  wrong predicate : $R"
RUSED=$(field "$R" REUSED)
[ "$RUSED" -gt 0 ] \
    || fail "RED CONTROL DID NOT FIRE at the reuse stage: the wrong predicate's freed fallback pages were never handed back out, so the corruption stayed latent. Either prove it here or the hazard argument is incomplete."
echo "  -> the allocator handed $RUSED of the fallback tree's own page(s) back out"

# The identical probe on the correctly-collected, damaged volume must find
# none. Same image geometry, same churn, same damaged root: the ONLY
# difference is the liveness predicate.
DGEN=$(field "$SL2" NEWESTGEN)
R=$(INVFS_RECLAIM_ORPHANS=1 "$T" reuse good-reuse.img "$ADOPTED" "$DGEN" 20000) \
    || fail "reuse probe failed on the correctly-collected volume"
echo "  correct predicate: $R"
RUSED2=$(field "$R" REUSED)
[ "$RUSED2" -eq 0 ] \
    || fail "the shipped predicate let $RUSED2 page(s) of the fallback tree be reallocated -- the liveness predicate is wrong"
echo "  -> zero: no page of the fallback tree was reallocated"

echo "== leg 5: a live save point (v->pinned_root) is a liveness source =="
# Same emulation as leg 4 -- both RT30 slots narrowed to the newest root --
# but with the older root pinned as the save point's base. If pinned_root
# were not a liveness source, the older root's pages would be freed exactly
# as they were in leg 4. Leg 4 freed them; leg 5 must not. That is the
# differential, and it is the one place a save point is load-bearing.
mkvol pinned.img
OUT=$("$T" build pinned.img "$NFILES" "$NGENS") || { echo "$OUT"; fail "leg-5 build failed"; }
P_ORPHAN=$(field "$OUT" PAGES_ORPHAN)
[ "$P_ORPHAN" -gt 0 ] || fail "leg 5: nothing to collect"
OUT=$(INVFS_RECLAIM_ORPHANS=1 "$T" pinned-collect pinned.img) || { echo "$OUT"; fail "pinned collect failed"; }
echo "$OUT"
SL=$("$T" slots pinned.img) || fail "leg-5 slots failed"
P_OLDER=$(field "$SL" OLDER)
W=$("$T" walk pinned.img "$P_OLDER") || fail "leg-5 walk failed"
echo "$W"
PUN=$(field "$W" UNALLOCATED)
[ -n "$PUN" ] || fail "leg 5: walk printed no UNALLOCATED count"
[ "$PUN" -eq 0 ] \
    || fail "leg 5: the save point's pinned root stands on $PUN unallocated page(s) -- pinned_root is not being treated as live"
echo "  pinned root survives the same narrowing that destroyed it in leg 4"

echo "== leg 6: collection is idempotent and the volume stays clean =="
OUT=$(INVFS_RECLAIM_ORPHANS=1 "$T" collect good.img) || { echo "$OUT"; fail "second collect failed"; }
echo "$OUT"
F2=$(field "$OUT" COLLECTED)
[ "$F2" -eq 0 ] || fail "second collect freed $F2 more pages -- collection is not idempotent"
"$T" verify good.img "$NFILES" || fail "readback failed after the second collect"
FSCK=$("$B/invf-fsck" good.img 2>&1) || { echo "$FSCK"; fail "fsck nonzero after the second collect"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK after the second collect"; }

echo "== leg 7: the production fold path, not a direct collector call =="
# Legs 1-5 call vol_reclaim_orphans directly. This leg goes through
# vol_v3_fold -> fold_reclaim_hook, which is what the FUSE drain
# (vol_sweep.c:2361-2364) actually reaches, including the RT30-slot guard
# that stops fold_reclaim_hook's one-generation diff from freeing the root
# the other slot still names.
mkvol fold.img
OUT=$(INVFS_RECLAIM_ORPHANS=1 "$T" fold fold.img "$NFILES" "$NGENS" 2>"$WORK/fold.err") \
    || { echo "$OUT"; cat "$WORK/fold.err"; fail "fold leg failed"; }
echo "$OUT"
grep -q "collected .* orphaned v3 base page" "$WORK/fold.err" \
    || { cat "$WORK/fold.err"; fail "the fold path never reached the collector -- the production chain is not wired"; }
sed 's/^/  | /' "$WORK/fold.err" | grep collected
F_ORPHAN=$(field "$OUT" PAGES_ORPHAN)
[ -n "$F_ORPHAN" ] || fail "fold leg printed no PAGES_ORPHAN"
echo "  orphans left after the fold path: $F_ORPHAN (was $NGENS before it ran)"
"$T" verify fold.img "$NFILES" || fail "readback failed after the fold path"
FSCK=$("$B/invf-fsck" fold.img 2>&1) || { echo "$FSCK"; fail "fsck nonzero after the fold path"; }
echo "$FSCK" | grep -q "^OK$" || { echo "$FSCK"; fail "fsck not OK after the fold path"; }
# And the same leg with the gate off must collect nothing, on the fold path
# too -- the guard is not the collector, and must not be mistaken for one.
mkvol fold-off.img
OUT=$(env -u INVFS_RECLAIM_ORPHANS "$T" fold fold-off.img "$NFILES" "$NGENS" 2>"$WORK/foldoff.err") \
    || { echo "$OUT"; fail "gate-off fold leg failed"; }
if grep -q "collected .* orphaned" "$WORK/foldoff.err"; then
    cat "$WORK/foldoff.err"
    fail "the fold path collected orphans with INVFS_RECLAIM_ORPHANS unset"
fi
GO=$(field "$OUT" PAGES_ORPHAN)
[ "$GO" -gt 0 ] || fail "gate-off fold leg left no orphans, so the assertion above proved nothing"
echo "  gate off on the fold path: $GO orphans retained, nothing collected"
"$T" verify fold-off.img "$NFILES" || fail "readback failed on the gate-off fold volume"

echo "ALL V3 ORPHAN RECLAIM LEGS PASS"
