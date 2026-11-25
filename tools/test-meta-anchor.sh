#!/bin/bash
# test-meta-anchor.sh — the ANC0 tail anchor: a redundant LOCATION for the
# block-0 descriptors that decide whether the volume can be opened at all.
#
# WHAT THIS EXISTS FOR. RT30 (block 0, 0x9D0, 48 bytes) is the only copy of the
# root descriptor, and SPT0 (0xA00, 32 bytes) the only copy of the save-point
# descriptor. RT30's "double slot" is not two copies of the descriptor:
# root_slot[2] are two POINTERS, so the double slot survives a STALE ROOT and
# does nothing for a LOST DESCRIPTOR. Parity does not help either -- parity
# protects against INDEPENDENT loss, an XOR over block 0's contents dies with
# block 0, while a copy in a different place protects against REGIONAL loss.
# So the fix is a second LOCATION, at the tail.
#
# WHY THE TAIL: anchor_pba = total_blocks - 1 is computable from the DEVICE
# SIZE alone. A recovery path that cannot find the anchor without reading the
# block that may be dead is not a recovery path. Every damage leg below
# destroys block 0's descriptor and then opens the volume through the ordinary
# public path, so recovery is measured, not asserted.
#
#   leg 0  build: N files through the public write path, folded into the base.
#          Asserts an anchor exists at the tail, its geometry fingerprint
#          matches THIS image, its mirrored RT30 tracks the live one, and the
#          tail block's allocation bit is SET. Without that last one every
#          later leg is a race the allocator could win by handing the block
#          out -- so it is asserted here, not at the end.
#   leg 1  RESERVE under load: a write cycle, an unlink cycle (the real free
#          path) and a full offline sweep, checking the tail bit after EVERY
#          step. The allocator can only hand the block out if the bit clears,
#          so "never cleared" is the proof the reservation holds.
#   leg 2  DAMAGE -> RECOVER, magic: clobber block-0 RT30's magic. The volume
#          must open off the anchor, say LOUDLY which source it used, and
#          every file must read back byte-identical. This leg FAILS on a tree
#          with no anchor: today the open presents an EMPTY namespace (0 files)
#          and fsck says "the base tree is unreachable".
#   leg 3  DAMAGE -> RECOVER, crc: same, with the magic intact and only the
#          crc32c field clobbered -- i.e. a torn write rather than a wipe.
#   leg 4  REFUSE a foreign image: another volume's anchor block copied onto
#          this image's tail. The fingerprint must REFUSE it, and refusal must
#          be DISTINGUISHABLE from "no anchor" (a different reported state),
#          and the volume must come up with today's behaviour -- an empty
#          base -- never somebody else's root.
#   leg 5  REFUSE each fingerprint arm in isolation: patch total_blocks, then
#          block_size, then format_version, then the uuid inside the anchor,
#          one at a time, re-CRCing the descriptor each time so the ONLY thing
#          wrong with it is the arm under test. Every one must be refused.
#   leg 6  REFUSE a damaged anchor: clobber the anchor's own crc32c. An anchor
#          that does not validate is not "absent" -- it is damage, and the two
#          must not be conflated.
#   leg 7  BACKWARD COMPATIBILITY / RED CONTROL: a volume whose tail block has
#          NO anchor -- which is every volume created before this change, and
#          whose tail block is an ordinary block holding real bytes. The full
#          write+sweep cycle must leave those bytes BYTE-IDENTICAL (the anchor
#          code must never adopt or create a tail block it did not write), the
#          volume must open and read byte-identical, and fsck must say OK.
#          This is the leg that keeps the change from being a data-loss bug on
#          every existing image: if the anchor machinery ever started writing
#          to a tail block it did not create, it would overwrite a file.
#   leg 8  the fallback must not fire on a HEALTHY volume, and a healthy open
#          must not print the recovery line -- a fallback that chatters is one
#          nobody reads.
#   leg 10 a LOST bitmap bit must not cost the volume its anchor: the bitmap is
#          a derived cache, so the exclusion has to be structural.
#   leg 11 the third allocator-shaped path: extend_meta_extent() must not GROW
#          a metadata extent over the anchor. It used to decide "the adjacent
#          run is free" from clear bits alone, and a clear bit is not proof of
#          freedom -- leg 10 is the world in which the anchor's bit is clear.
#          Two geometries that reach the anchor must refuse AND leave the
#          block byte-identical; the same call one block further out must
#          SUCCEED, so a blanket refusal cannot pass the leg.
#   leg 12 the FOURTH allocator-shaped path, and the one that actually writes
#          over the anchor: mb_alloc_meta_zone() (vol_metabuf.c:253) hands out
#          a metadata base page from the first clear bit in a scan CLAMPED to
#          total_blocks, so a metadata zone that reaches the tail has the
#          anchor as a candidate. A geometry injected in RAM (one superblock
#          field pair, no flush, every bit and field restored before close)
#          makes the anchor the scan's only candidate: it must be refused with
#          the block byte-identical, the same call with the zone one block
#          short of the tail must SUCCEED, and with VOLF_READONLY the same
#          geometry must move no bit at all.
#
# Run:  bash tools/run-e2e.sh tools/test-meta-anchor.sh
#   or:  bash tools/test-meta-anchor.sh     (from make test)
set -u
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# INVFS_BIN lets a sanitizer build (bin-tsan) be exercised without
# replacing the binaries the normal runs use.
B=${INVFS_BIN:-$REPO/bin}
T=$B/invf-anchor_test
WORK=${INVFS_ANCHOR_WORK:-/dev/shm/v3anchor-$$}
NFILES=${INVFS_ANCHOR_FILES:-24}

rm -rf "$WORK" && mkdir -p "$WORK" || exit 1

fail() { echo "FAIL: $*" >&2; exit 1; }
block_count() { python3 -c "import os,sys;print(os.path.getsize(sys.argv[1])//4096)" "$1"; }
note() { echo "  $*"; }

# One KEY=VALUE out of a probe line, the way the other suites read the tools'
# reports. grepping the whole line would silently miss every field that is not
# the first one, and a suite that asserts nothing is worse than no suite.
field() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }
is()    { [ "$(field "$1" "$2")" = "$3" ]; }

# The driver is a normal `make` target but be tolerant of a bare checkout.
if [ ! -x "$T" ]; then
    make -C "$REPO" bin/invf-anchor_test >/dev/null 2>&1 \
        || fail "cannot build bin/invf-anchor_test"
fi
[ -x "$B/invf-fsck" ] || fail "bin/invf-fsck missing (run make)"

mkvol() {  # mkvol <img> [MB]
    rm -f "$1"
    "$B/invf-mkfs" "$1" "${2:-0.3}" >"$1.mkfs" 2>&1 \
        || { cat "$1.mkfs"; fail "mkfs failed for $1"; }
}

# The anchor block, byte-for-byte, for the backward-compat identity check.
# The driver reads it at (total_blocks-1)*4096 -- the offset the VOLUME uses.
# "The last 4096 bytes of the file" is a different block: invf-mkfs floors
# size/4096, so a 0.3 GB image ends in a PARTIAL block and the anchor sits
# inside it. Comparing the wrong block would make this check vacuous.
tailblk() { "$T" taildump "$1" "$2" >/dev/null; }

echo "=== leg 0: build + anchor present, reserved, fingerprint-matched ==="
mkvol "$WORK/base.img"
grep -q "tail anchor: *ANC0 at block" "$WORK/base.img.mkfs" \
    || { cat "$WORK/base.img.mkfs"; fail "leg 0: mkfs did not report a tail anchor"; }
OUT=$("$T" build "$WORK/base.img" "$NFILES" 2>&1) || { echo "$OUT"; fail "leg 0: build"; }
echo "$OUT" | sed 's/^/  /'
P=$("$T" probe "$WORK/base.img" 2>&1) || { echo "$P"; fail "leg 0: probe"; }
echo "$P" | sed 's/^/  /'
is "$P" ANCHOR_STATE ok \
    || fail "leg 0: no valid anchor at the volume tail after mkfs+write (got '$(field "$P" ANCHOR_STATE)')"
is "$P" TAIL_BIT 1 \
    || fail "leg 0: the anchor block's allocation bit is CLEAR -- the allocator could hand it out"
is "$P" TAIL_FREE 0 \
    || fail "leg 0: the anchor block is reported free"
is "$P" FP_MATCH 1 \
    || fail "leg 0: the anchor's geometry fingerprint does not match its own image"
[ "$(field "$P" MIRROR_SEQ)" -gt 0 ] 2>/dev/null \
    || fail "leg 0: the anchor's mirrored RT30 is still the empty mkfs descriptor"
# Snapshot the fully-built image here. Leg 1 is deliberately destructive
# (it unlinks everything to exercise the real free path), and the damage
# legs need a volume that still HAS files -- a recovery leg that recovers an
# empty base would pass whether or not the anchor works.
cp "$WORK/base.img" "$WORK/full.img" || fail "leg 0: snapshot"
# Red control for the whole feature: an anchor that merely re-held block 0's
# bytes would satisfy every check above, so assert the anchor block is NOT
# byte-identical to block 0. A backup that dies with the thing it backs up is
# not a backup.
blk0=$("$T" block0 "$WORK/full.img" 2>&1) || fail "leg 0: block0 dump"
anchor_hex=$("$T" anchorhex "$WORK/full.img" 2>&1) || fail "leg 0: anchorhex dump"
[ "$blk0" != "$anchor_hex" ] \
    || fail "leg 0: the anchor block is byte-identical to block 0 (red control)"
note "anchor present, reserved, fingerprint-matched, in a different block"

echo "=== leg 1: the reservation survives a real write/unlink/sweep cycle ==="
"$T" reserve "$WORK/base.img" "$NFILES" 2>&1 | sed 's/^/  /' \
    || fail "leg 1: the anchor block's bit went clear during a write/unlink cycle"
"$B/invf-sweep" "$WORK/base.img" >"$WORK/sweep.log" 2>&1 \
    || { tail -20 "$WORK/sweep.log"; fail "leg 1: invf-sweep failed"; }
P=$("$T" probe "$WORK/base.img" 2>&1) || { echo "$P"; fail "leg 1: probe after sweep"; }
echo "$P" | sed 's/^/  /'
is "$P" TAIL_BIT 1 || fail "leg 1: the anchor block was freed by the sweep"
is "$P" ANCHOR_STATE ok || fail "leg 1: the anchor did not survive the sweep"
# The reserve leg ends in an unlink cycle, so re-create the files before
# asking for them: the assertion here is that the sweep did not damage the
# volume, not that unlink failed to unlink.
"$T" build "$WORK/base.img" 8 >/dev/null 2>&1 \
    || fail "leg 1: could not re-create files after the sweep"
"$T" readback "$WORK/base.img" 8 2>&1 | sed 's/^/  /' \
    || fail "leg 1: files did not read back byte-identical after the sweep"
note "the anchor block stayed allocated through write, unlink and a full sweep"

echo "=== leg 2: block-0 RT30 magic destroyed -> open off the anchor ==="
cp "$WORK/full.img" "$WORK/magic.img" || fail "leg 2: cp"
"$T" corrupt-rt30 "$WORK/magic.img" magic || fail "leg 2: corrupt"
# The pre-anchor behaviour, spelled out so this suite has teeth: a tree with
# no anchor opens this image as an EMPTY namespace and cannot repair it.
# The verdict is DAMAGED, and that is the POINT: the volume is readable, but
# it is running on a restored descriptor because block 0 is broken, and a
# fallback that lets a damaged volume come back "OK" is exactly the failure
# mode this change must not have. The data must still walk, and no repair
# may be needed to read it.
FSCK=$("$B/invf-fsck" "$WORK/magic.img" 2>&1) || true
echo "$FSCK" | grep -E 'ANC0|^DAMAGED$|^OK$|CANNOT REPAIR' | sed 's/^/  fsck: /'
echo "$FSCK" | grep -q 'ANC0: the volume opened on its TAIL ANCHOR' \
    || fail "leg 2: fsck did not report that it ran on the tail anchor"
echo "$FSCK" | grep -q 'CANNOT REPAIR' \
    && fail "leg 2: fsck still cannot reach the base tree -- the anchor was not adopted"
echo "$FSCK" | grep -q '^DAMAGED$' \
    || fail "leg 2: a volume recovered off the anchor reported OK -- block 0 is still broken"
# grep for the RECOVERY MARKER, not the word "anchor": the test files are
# called wp_anchor_file_NNNN.txt, so a loose grep would match the filenames
# and pass whether or not the volume ever said anything.
OPENLOG=$("$B/invf-ls" "$WORK/magic.img" 2>&1) || true
echo "$OPENLOG" | grep 'ANC0 TAIL ANCHOR ADOPTED' | sed 's/^/  open: /'
echo "$OPENLOG" | grep -q 'ANC0 TAIL ANCHOR ADOPTED' \
    || fail "leg 2: the open did not NAME the anchor as the descriptor source"
echo "$OPENLOG" | grep -q "$NFILES file(s)" \
    || fail "leg 2: the recovered volume did not present its $NFILES files"
"$T" readback "$WORK/magic.img" "$NFILES" 2>&1 | sed 's/^/  /' \
    || fail "leg 2: files did not read back byte-identical off the anchor"
note "recovered from a destroyed block-0 magic, loudly, byte-identical"

echo "=== leg 3: block-0 RT30 crc32c destroyed (torn write) ==="
cp "$WORK/full.img" "$WORK/crc.img" || fail "leg 3: cp"
"$T" corrupt-rt30 "$WORK/crc.img" crc || fail "leg 3: corrupt"
FSCK=$("$B/invf-fsck" "$WORK/crc.img" 2>&1) || true
echo "$FSCK" | grep -E 'ANC0|^DAMAGED$|^OK$|CANNOT REPAIR' | sed 's/^/  fsck: /'
echo "$FSCK" | grep -q 'ANC0: the volume opened on its TAIL ANCHOR' \
    || fail "leg 3: fsck did not report that it ran on the tail anchor"
echo "$FSCK" | grep -q 'CANNOT REPAIR' \
    && fail "leg 3: a torn block-0 RT30 could not be recovered off the anchor"
echo "$FSCK" | grep -q '^DAMAGED$' \
    || fail "leg 3: a torn block-0 RT30 came back OK -- the fallback hid the damage"
"$T" readback "$WORK/crc.img" "$NFILES" 2>&1 | sed 's/^/  /' \
    || fail "leg 3: files did not read back byte-identical off the anchor"
note "recovered from a torn block-0 RT30 CRC, byte-identical"

echo "=== leg 4: a foreign volume's anchor must be REFUSED ==="
# A DIFFERENT SIZE, on purpose: the realistic "this anchor belongs to some
# other image" case. The same-size case is covered by leg 5's uuid arm, and
# teleport-check below refuses to let this leg run vacuously (invf-mkfs's
# UUID is test-grade, so two same-size images made in the same second can
# collide and the refusal would then be reading as a bug).
mkvol "$WORK/other.img" 0.2
"$T" teleport-check "$WORK/full.img" "$WORK/other.img" || fail "leg 4: the two images are not distinguishable"
"$T" teleport-anchor "$WORK/full.img" "$WORK/other.img" || fail "leg 4: teleport"
P=$("$T" probe "$WORK/other.img" 2>&1) || { echo "$P"; fail "leg 4: probe"; }
echo "$P" | sed 's/^/  /'
is "$P" ANCHOR_STATE refused-geometry \
    || fail "leg 4: a foreign anchor was not refused as a geometry mismatch (got '$(field "$P" ANCHOR_STATE)')"
is "$P" ANCHOR_STATE absent \
    && fail "leg 4: a refusal was reported as 'no anchor' -- the two must be distinguishable"
# And the refusal must leave the volume exactly as it is today: an empty base,
# never somebody else's root.
"$T" readback "$WORK/other.img" 0 2>&1 | sed 's/^/  /' \
    || fail "leg 4: the refused volume did not come up as an empty base"
note "a foreign anchor is refused, and the refusal is not 'absent'"

echo "=== leg 5: every fingerprint arm, one at a time ==="
# The refusal is only load-bearing if it PREVENTS adoption. So each arm is
# patched AND block 0's RT30 is then destroyed: with a matching fingerprint
# that combination is exactly legs 2/3 and it recovers; with a mismatched one
# it must NOT, and the volume must land where it lands today. Refusing while
# still adopting would pass an "ANCHOR_STATE=refused-geometry" assertion.
for arm in total_blocks block_size format_version uuid; do
    cp "$WORK/full.img" "$WORK/arm.img" || fail "leg 5: cp $arm"
    "$T" patch-fingerprint "$WORK/arm.img" "$arm" || fail "leg 5: patch $arm"
    P=$("$T" probe "$WORK/arm.img" 2>&1) || { echo "$P"; fail "leg 5: probe $arm"; }
    is "$P" ANCHOR_STATE refused-geometry \
        || fail "leg 5: a mismatched $arm was NOT refused (got $(field "$P" ANCHOR_STATE))"
    "$T" corrupt-rt30 "$WORK/arm.img" magic >/dev/null || fail "leg 5: corrupt $arm"
    P=$("$T" probe "$WORK/arm.img" 2>&1) || { echo "$P"; fail "leg 5: probe $arm after damage"; }
    is "$P" ANCHOR_STATE refused-geometry \
        || fail "leg 5: $arm no longer reads as refused once block 0 is damaged"
    is "$P" ADOPTED 0 \
        || fail "leg 5: $arm was REFUSED and the mirror was adopted anyway"
    FSCK=$("$B/invf-fsck" "$WORK/arm.img" 2>&1) || true
    echo "$FSCK" | grep -q 'CANNOT REPAIR' \
        || fail "leg 5: with $arm mismatched the damaged volume was still told it had a root"
    note "$arm -> refused, and the foreign mirror was NOT adopted"
done
note "total_blocks / block_size / format_version / uuid each refuse independently"

echo "=== leg 6: a damaged anchor is damage, not absence ==="
cp "$WORK/full.img" "$WORK/badanch.img" || fail "leg 6: cp"
"$T" corrupt-anchor-crc "$WORK/badanch.img" || fail "leg 6: corrupt"
P=$("$T" probe "$WORK/badanch.img" 2>&1) || { echo "$P"; fail "leg 6: probe"; }
echo "$P" | sed 's/^/  /'
is "$P" ANCHOR_STATE refused-damage \
    || fail "leg 6: a corrupt anchor was not reported as damage (got '$(field "$P" ANCHOR_STATE)')"
is "$P" ANCHOR_STATE absent \
    && fail "leg 6: a damaged anchor was reported as 'no anchor'"
note "a damaged anchor reports damage, distinctly from absence"

echo "=== leg 7: BACKWARD COMPATIBILITY -- a volume with no anchor (red control) ==="
mkvol "$WORK/legacy.img"
"$T" build "$WORK/legacy.img" "$NFILES" >/dev/null 2>&1 \
    || fail "leg 7: build the legacy image"
# This is what a pre-anchor volume's tail actually is: a block nobody
# reserved, with real bytes in it. The point of writing a recognisable
# pattern is that the anchor code must NEVER overwrite it.
"$T" fill-tail "$WORK/legacy.img" "LEGACY-TAIL-DATA" || fail "leg 7: fill-tail"
P=$("$T" probe "$WORK/legacy.img" 2>&1) || { echo "$P"; fail "leg 7: probe"; }
echo "$P" | sed 's/^/  /'
is "$P" ANCHOR_STATE absent \
    || fail "leg 7: a volume that never had an anchor reported one (got '$(field "$P" ANCHOR_STATE)')"
tailblk "$WORK/legacy.img" "$WORK/legacy.tail.before"
"$T" writecycle "$WORK/legacy.img" "$NFILES" >/dev/null 2>&1 \
    || fail "leg 7: writecycle"
"$B/invf-sweep" "$WORK/legacy.img" >"$WORK/legacy.sweep" 2>&1 \
    || { tail -20 "$WORK/legacy.sweep"; fail "leg 7: sweep on the legacy image"; }
tailblk "$WORK/legacy.img" "$WORK/legacy.tail.after"
cmp -s "$WORK/legacy.tail.before" "$WORK/legacy.tail.after" \
    || fail "leg 7: RED CONTROL -- a full write+sweep cycle REWROTE the tail block of a volume that never had an anchor. On a real image that block is ordinary file data."
P=$("$T" probe "$WORK/legacy.img" 2>&1) || { echo "$P"; fail "leg 7: probe after cycle"; }
is "$P" ANCHOR_STATE absent \
    || fail "leg 7: the anchor code CREATED an anchor on a volume that never had one"
FSCK=$("$B/invf-fsck" "$WORK/legacy.img" 2>&1) || { echo "$FSCK"; fail "leg 7: fsck"; }
echo "$FSCK" | grep -q '^OK$' || { echo "$FSCK"; fail "leg 7: fsck not OK on the legacy image"; }
"$T" readback-gen "$WORK/legacy.img" "$NFILES" 1 2>&1 | sed 's/^/  /' \
    || fail "leg 7: legacy volume did not read back byte-identical"
note "a pre-anchor volume: tail bytes untouched, no anchor invented, fsck OK"

echo "=== leg 8: quiet on a healthy volume ==="
P=$("$T" probe "$WORK/full.img" 2>&1) || { echo "$P"; fail "leg 8: probe"; }
echo "$P" | sed 's/^/  /'
is "$P" ADOPTED 0 \
    || fail "leg 8: a HEALTHY volume adopted the anchor -- the fallback is firing on a good image"
L=$("$B/invf-ls" "$WORK/full.img" 2>&1) || true
echo "$L" | grep -q 'ANC0 TAIL ANCHOR' \
    && fail "leg 8: a healthy open printed the recovery line -- a fallback that chatters is one nobody reads"
note "healthy volumes never take the fallback and never print it"


echo "=== leg 10: the anchor survives a LOST bitmap bit (structural, not bookkeeping) ==="
# The anchor's block is reserved by a bitmap bit set at mkfs. That is NOT
# sufficient alone: the bitmap is a derived cache (AGENTS.md 2.4) and a rebuild
# from replay leaves the anchor free, because no delta record ever names it.
# Rebuilding the bitmap is precisely what happens while repairing a damaged
# volume -- the situation the anchor exists for. So this leg clears the bit in
# the on-disk bitmap and then writes the volume hard, many times, so the
# allocator runs often enough to reach the tail. If the exclusion is
# structural the block is untouched; if it is a bitmap bit, the last block gets
# handed out and the anchor is gone.
# The volume must be driven NEAR CAPACITY, or the allocator simply never
# considers the tail and the leg proves nothing. A first cut of this leg used
# the default 0.3 MB volume and 40 files; the red control (dropping the
# structural exclusion) still PASSED, which is how the vacuousness was found --
# the allocator had no reason to reach the last block. Filling the volume is
# what makes the assertion mean something.
mkvol "$WORK/bm.img" "${INVFS_ANCHOR_FILL_MB:-48}"
"$T" build "$WORK/bm.img" "$NFILES" >/dev/null 2>&1 \
    || fail "leg 10: could not populate the volume"
# push free space down so the next allocations are forced toward the tail
note "volume is ${INVFS_ANCHOR_FILL_MB:-48} MB; the allocator must now walk the tail"
P0=$("$T" probe "$WORK/bm.img" 2>&1) || { echo "$P0"; fail "leg 10: probe before"; }
SEQ0=$(printf '%s\n' "$P0" | sed -n 's/.*MIRROR_SEQ=\([0-9]*\).*/\1/p')
is "$P0" ANCHOR_STATE ok || { echo "$P0"; fail "leg 10: no anchor to begin with"; }
python3 - "$WORK/bm.img" <<'PYEOF'
import sys, struct
path = sys.argv[1]; bs = 4096
with open(path, 'r+b') as f:
    sb = f.read(bs)
    meta_start = struct.unpack_from('<Q', sb, 0x28)[0]
    total = struct.unpack_from('<Q', sb, 0x20)[0]
    pba = total - 1
    off = meta_start * bs + pba // 8
    f.seek(off)
    b = f.read(1)[0]
    f.seek(off)
    f.write(bytes([b & ~(1 << (pba % 8))]))
    print(f"  cleared the anchor bit (pba {pba}, bitmap byte {pba//8})")
PYEOF
# Fill hard and repeatedly, so the allocator is forced to walk the whole free
# pool -- including the last block -- many times over.
for round in 1 2 3 4 5 6; do
    "$T" build "$WORK/bm.img" 60 >/dev/null 2>&1 \
        || fail "leg 10: could not write round $round after the bit was cleared"
done
note "wrote 6x60 files and swept on a near-full volume"
"$B/invf-sweep" "$WORK/bm.img" >"$WORK/bm-sweep.log" 2>&1 \
    || { tail -20 "$WORK/bm-sweep.log"; fail "leg 10: invf-sweep failed"; }
# The anchor is REFRESHED on every RT30 store, so its bytes legitimately change
# here. Byte-identity would be the wrong assertion and would fail against a
# perfectly healthy volume. The assertion is that the block is still a VALID
# ANC0 descriptor -- user data landing there would destroy the magic and the
# crc, which is the actual failure this leg is about -- and that its seq has
# ADVANCED, which proves the anchor writer still owns the block rather than
# merely that nothing touched it.
P1=$("$T" probe "$WORK/bm.img" 2>&1) || { echo "$P1"; fail "leg 10: probe after"; }
echo "$P1" | sed 's/^/  /'
is "$P1" ANCHOR_STATE ok \
    || fail "leg 10: the anchor block no longer validates after the bitmap bit was cleared and the volume was written hard -- the allocator handed it out, so the exclusion is a bitmap bit rather than a structural rule, and a bitmap rebuild would lose the volume's only recoverable root descriptor"
SEQ1=$(printf '%s\n' "$P1" | sed -n 's/.*MIRROR_SEQ=\([0-9]*\).*/\1/p')
[ -n "$SEQ0" ] && [ -n "$SEQ1" ] && [ "$SEQ1" -gt "$SEQ0" ] \
    || fail "leg 10: the anchor's mirror_seq did not advance (before=$SEQ0 after=$SEQ1), so the anchor writer stopped owning the block"
# HONEST SCOPE, because a test that implies more than it proves is worse than
# none: this leg does NOT prove the allocator guard is load-bearing. With the
# guard removed this leg STILL PASSES, verified by red control -- because
# alloc_blocks is called with zone_end == total_blocks (vol_delta.c:456,
# vol_metabuf.c:323 and others all pass shadow_zone_start +
# shadow_zone_blocks), so the last block IS inside a live scan range, but
# ENOSPC policy keeps ordinary allocation away from the tail: reserved_blocks
# (total/128 + 64) and hard_min_blocks (total/1024 + 16, volume.c:1814-1817).
# Reaching the tail would mean exhausting the reserve. So what this leg
# actually establishes is: clearing the reservation bit does not by itself cost
# the volume its anchor, and the anchor writer keeps owning the block. The
# guard in alloc_blocks is HARDENING on a reachable path, not a fix for an
# observed failure, and is kept for that reason and not for a measured one.
note "leg 10: with its bitmap bit cleared, the anchor still validates and its seq advanced $SEQ0 -> $SEQ1"

echo
echo "=== leg 11: extend_meta_extent must not grow a metadata extent OVER the anchor ==="
# The third allocator-shaped path. extend_meta_extent() (volume.c:2639) grows
# a metadata extent in place when the adjacent run "is free", and it decided
# that from the clear bits in v->bitmap and nothing else. A CLEAR BIT IS NOT
# PROOF OF FREEDOM: the ANC0 anchor lies INSIDE the shadow zone's range, and
# its bit is legitimately clear on a volume whose bitmap was rebuilt from
# replay (no delta record ever names the block). That is leg 10's world, and
# in it a metadata extent ending near the tail can be grown ACROSS the anchor,
# marked allocated as metadata, and overwritten by the next metadata record --
# taking the volume's only recoverable copy of RT30/SPT0 with it.
#
# `invf-anchor_test extend-meta` injects three geometries IN RAM (one mapper
# entry, no flush, no MET0 persist, every bit restored before close) and calls
# the real function, all with the same class bump (1 -> 2, 32 -> 64 blocks):
#
#   edge     the adjacent run STARTS at the anchor   (the guard's first test)
#   runlast  the adjacent run ENDS at the anchor     (the guard's second test)
#   shifted  the SAME CALL with the run one block further out -- free and
#            unprotected -- which MUST succeed, or the two refusing legs above
#            would be satisfied by a function that refuses every extension on
#            the volume.
#
# The driver also performs the write the merge path would perform if the
# extent had grown (records append at the extent's old end, i.e. the first
# newly claimed block), so the byte assertion is measured, not assumed: in
# `edge` that first newly claimed block IS the anchor. With the guard removed
# this leg fails on ANCHOR_BYTES_CHANGED and on the cmp below, not merely on
# the return code.
for mode in edge runlast; do
    cp "$WORK/full.img" "$WORK/xm-$mode.img" || fail "leg 11: cp $mode"
    tailblk "$WORK/xm-$mode.img" "$WORK/xm-$mode.before"
    OUT=$("$T" extend-meta "$WORK/xm-$mode.img" "$mode" 2>&1) \
        || { echo "$OUT"; fail "leg 11: extend-meta $mode (the leg's geometry was not free -- it would have been vacuous)"; }
    echo "$OUT" | sed 's/^/  /'
    is "$OUT" RUN_FREE 1 \
        || fail "leg 11 ($mode): the adjacent run was not free before the call, so the refusal below would prove nothing"
    tailblk "$WORK/xm-$mode.img" "$WORK/xm-$mode.after"
    cmp -s "$WORK/xm-$mode.before" "$WORK/xm-$mode.after" \
        || fail "leg 11 ($mode): RED CONTROL -- extend_meta_extent() grew a metadata extent OVER the ANC0 anchor and the metadata record write that follows destroyed it. The volume's only second copy of RT30/SPT0 is gone."
    is "$OUT" ANCHOR_BYTES_CHANGED 0 \
        || fail "leg 11 ($mode): the anchor block's bytes changed across the call"
    is "$OUT" RC 0 \
        || fail "leg 11 ($mode): extend_meta_extent() returned $(field "$OUT" RC) for an adjacent run that reaches the anchor -- expected 0"
    is "$OUT" ANCHOR_BIT_AFTER 0 \
        || fail "leg 11 ($mode): the anchor block's BITMAP bit was set by the extension -- the block is now metadata-allocated"
    is "$OUT" ANCHOR_METATYPE_AFTER 0 \
        || fail "leg 11 ($mode): the anchor block was typed META in meta_type_bitmap by the extension"
    is "$OUT" CLAIMED_ANCHOR 0 \
        || fail "leg 11 ($mode): the mapper entry now claims the anchor block"
    is "$OUT" RECORD_WRITE 0 \
        || fail "leg 11 ($mode): a metadata record was written into the block the extension claimed"
    P=$("$T" probe "$WORK/xm-$mode.img" 2>&1) || { echo "$P"; fail "leg 11: probe $mode"; }
    is "$P" ANCHOR_STATE ok \
        || fail "leg 11 ($mode): the anchor no longer validates after the refused extension (state '$(field "$P" ANCHOR_STATE)')"
    note "$mode: refused, anchor bit/metatype untouched, block byte-identical"
done
cp "$WORK/full.img" "$WORK/xm-shifted.img" || fail "leg 11: cp shifted"
tailblk "$WORK/xm-shifted.img" "$WORK/xm-shifted.before"
OUT=$("$T" extend-meta "$WORK/xm-shifted.img" shifted 2>&1) \
    || { echo "$OUT"; fail "leg 11: extend-meta shifted"; }
echo "$OUT" | sed 's/^/  /'
is "$OUT" RUN_FREE 1 || fail "leg 11 (shifted): the adjacent run was not free before the call"
is "$OUT" RC 1 \
    || fail "leg 11 (shifted): the SAME call with the run one block further from the anchor returned $(field "$OUT" RC), not 1 -- the guard refuses every extension, and rejected legs would prove nothing"
is "$OUT" MAPPER_CLASS_AFTER 2 \
    || fail "leg 11 (shifted): the extension did not land (mapper class $(field "$OUT" MAPPER_CLASS_AFTER), expected 2)"
is "$OUT" RUN_BITS_SET_AFTER 32 \
    || fail "leg 11 (shifted): the 32 newly claimed blocks were not marked in the bitmap ($(field "$OUT" RUN_BITS_SET_AFTER) set)"
is "$OUT" CLAIMED_ANCHOR 0 \
    || fail "leg 11 (shifted): a successful extension one block away still claims the anchor"
is "$OUT" ANCHOR_BIT_AFTER 0 \
    || fail "leg 11 (shifted): the anchor's bitmap bit was set by an extension that does not claim it"
tailblk "$WORK/xm-shifted.img" "$WORK/xm-shifted.after"
cmp -s "$WORK/xm-shifted.before" "$WORK/xm-shifted.after" \
    || fail "leg 11 (shifted): a successful extension one block away still rewrote the anchor block"
note "shifted: the same call one block away SUCCEEDS (class 2, 32 blocks marked), anchor untouched"
note "the extent grow path is guarded structurally, and it still grows"

echo
leg12() {
    # leg 12: the metadata BASE-PAGE allocator, mb_alloc_meta_zone().
    #
    # The fourth allocator-shaped path to decide "free" from a clear bit, and
    # the first whose own next step overwrites the block: mbuf_alloc() calls
    # mb_write_empty() on whatever this scan returns, so a metadata zone that
    # reaches the tail does not merely mark the anchor allocated -- it erases
    # the volume's only second copy of RT30/SPT0.
    #
    # Unreachable on an image invf-mkfs builds (it marks the whole metadata
    # zone allocated, vol_metabuf.c:245-248), so `invf-anchor_test alloc-meta`
    # CONSTRUCTS the geometry in RAM, the way leg 11 does: one superblock field
    # pair per mode, the anchor's bitmap bit cleared first (the world a bitmap
    # rebuild from replay leaves, and the only world in which the bug is
    # reachable), every bit and field restored before close. No flush, no
    # persist:
    #
    #   tail   the zone IS the tail ([anchor, anchor+2) -- the clamp in the
    #          allocator leaves the anchor as the SINGLE candidate). Must
    #          refuse, with the block byte-identical.
    #   short  the same zone one block short of the tail ([anchor-1, anchor)).
    #          The ordinary free block next to the anchor must be handed out,
    #          or a guard that refused the whole zone would pass `tail` while
    #          breaking every metadata write on the volume.
    #   ro     `short` plus VOLF_READONLY. No bit may move: the latch has to run
    #          before the scan's first bit_set.
    echo "=== leg 12: the metadata base-page allocator must not hand out the anchor ==="
    for mode in tail short ro; do
        cp "$WORK/full.img" "$WORK/am-$mode.img" || fail "leg 12: cp $mode"
        tailblk "$WORK/am-$mode.img" "$WORK/am-$mode.before"
        OUT=$("$T" alloc-meta "$WORK/am-$mode.img" "$mode" 2>&1) \
            || { echo "$OUT"; fail "leg 12: alloc-meta $mode could not build its geometry (see the message above); the leg would have been vacuous"; }
        echo "$OUT" | sed 's/^/  /'
        # Vacuity guards, asserted before anything else: the anchor must be the
        # scan's ONLY candidate in `tail`, or the refusal proves nothing.
        is "$OUT" ZONE_CANDIDATES 1 \
            || fail "leg 12 ($mode): the injected zone has $(field "$OUT" ZONE_CANDIDATES) candidates, so 'the scan did not take the anchor' proves nothing"
        tailblk "$WORK/am-$mode.img" "$WORK/am-$mode.after"
        cmp -s "$WORK/am-$mode.before" "$WORK/am-$mode.after" \
            || fail "leg 12 ($mode): RED CONTROL -- mb_alloc_meta_zone() handed out the ANC0 anchor as a metadata base page and mbuf_alloc's mb_write_empty() erased it. The volume's only second copy of RT30/SPT0 is gone."
        is "$OUT" RETURNED_ANCHOR 0 \
            || fail "leg 12 ($mode): the allocator RETURNED pba $(field "$OUT" ANCHOR_PBA) -- the anchor block -- as a metadata base page"
        is "$OUT" ANCHOR_BIT_AFTER 0 \
            || fail "leg 12 ($mode): the anchor's bitmap bit was SET by the metadata allocator"
        is "$OUT" ANCHOR_METATYPE_AFTER 0 \
            || fail "leg 12 ($mode): the anchor was typed META in meta_type_bitmap"
        is "$OUT" ANCHOR_BYTES_CHANGED 0 \
            || fail "leg 12 ($mode): the anchor block's bytes changed across the call"
        P=$("$T" probe "$WORK/am-$mode.img" 2>&1) || { echo "$P"; fail "leg 12: probe $mode"; }
        is "$P" ANCHOR_STATE ok \
            || fail "leg 12 ($mode): the anchor no longer validates after the call (state '$(field "$P" ANCHOR_STATE)')"
        if [ "$mode" = tail ]; then
            # The refusal must be the SCAN'S verdict, not "the zone was never
            # walked": a completed proven-empty scan is one run.
            is "$OUT" SCAN_RUNS_DELTA 1 \
                || fail "leg 12 (tail): the zone scan did not complete a proven-empty pass (SCAN_RUNS_DELTA=$(field "$OUT" SCAN_RUNS_DELTA)) -- the anchor was not considered, so the refusal proves nothing"
            is "$OUT" RC 0 \
                || fail "leg 12 (tail): the metadata allocator returned $(field "$OUT" RC) with the anchor as its only candidate -- expected 0 (no metadata block available)"
            is "$OUT" BITS_CHANGED 0 \
                || fail "leg 12 (tail): a bit in the allocation bitmap moved during a refused allocation"
            note "tail: the anchor was the scan's only candidate, it was refused, and the block is byte-identical"
        fi
        if [ "$mode" = short ]; then
            CAND=$(field "$OUT" CAND_PBA)
            [ -n "$CAND" ] && [ "$CAND" != 0 ] \
                || fail "leg 12 (short): the driver reported no control pba"
            is "$OUT" CAND_BIT_BEFORE 0 \
                || fail "leg 12 (short): the control block pba $CAND was already allocated, so its allocation would not be the ordinary kind"
            is "$OUT" RC "$CAND" \
                || fail "leg 12 (short): with the zone one block short of the tail the SAME call returned $(field "$OUT" RC), not the free block $CAND -- the guard refuses more than the anchor, and the refusing legs would prove nothing"
            is "$OUT" CAND_BIT_AFTER 1 \
                || fail "leg 12 (short): the returned block was not marked allocated"
            note "short: the same call one block short of the tail SUCCEEDS (pba $CAND), anchor untouched"
        fi
        if [ "$mode" = ro ]; then
            is "$OUT" READONLY 1 \
                || fail "leg 12 (ro): the driver did not latch the volume read-only"
            is "$OUT" RC 0 \
                || fail "leg 12 (ro): a READ-ONLY volume's metadata allocator returned $(field "$OUT" RC)"
            is "$OUT" BITS_CHANGED 0 \
                || fail "leg 12 (ro): RED CONTROL -- the metadata allocator moved a bit in the allocation bitmap on a volume latched VOLF_READONLY. The latch runs after the zone scan, so a read-only volume is being changed by an allocating path."
            is "$OUT" CAND_BIT_AFTER 0 \
                || fail "leg 12 (ro): the block the scan would have taken ($(field "$OUT" CAND_PBA)) was marked allocated on a read-only volume"
            note "ro: the latch runs before the scan -- no bit moved, nothing allocated"
        fi
    done
    note "the metadata base-page allocator is guarded structurally, and it still allocates"
}
leg12

echo
echo "PASS:  metadata anchor (ANC0) -- reserve, refresh, recover, refuse, back-compat, extent-guard, metabuf-alloc-guard"
exit 0
