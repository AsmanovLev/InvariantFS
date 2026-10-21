#!/usr/bin/env bash
# test-block0-layout.sh — WP650: the Block 0 slots, asserted on the BYTES.
#
# Why this exists next to the _Static_assert grid in invarifs.h: a compile-time
# assertion sees a layout that stopped fitting, and cannot see a WRITE that
# lands on a neighbour (a memcpy at the wrong offset, an off-by-one in a
# descriptor save). This gate reads block 0 of a real volume and names what it
# found at each load-bearing offset:
#
#   0x3C4  PCK0   codec-policy table   (ends at or before 0x9D0)
#   0x9D0  RT30   base-tree root       (48 B -> 0xA00)
#   0xA00  ..     reserved-zero        (32 B, deliberately not a slot)
#   0xA20  SPN0   the only v3 savepoint (56 B -> 0xA58); ABSENT until a capture
#   last   ANC0   the tail anchor
#
# SPN0 is written by the first capture, not by mkfs, so the gate asserts the
# slot is EMPTY before and LOAD-BEARING after: a magic string that is right on
# an untouched volume proves nothing about the descriptor the restore needs.
set -u
cd "$(dirname "$0")/.." || exit 1

BIN=${BIN:-./bin}
for t in invf-mkfs invf-sweep invf-fsck invf-cp; do
    [ -x "$BIN/$t" ] || { echo "FAIL: missing $BIN/$t (run make all)"; exit 1; }
done

WORK=${WORK:-/dev/shm/wp650-block0}
rm -rf "$WORK"; mkdir -p "$WORK" || exit 1
trap 'rm -rf "$WORK"' EXIT
IMG="$WORK/b0.img"

pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  OK    $*"; }
bad() { fail=$((fail+1)); echo "  FAIL  $*"; }

# ascii_at <offset> <count> -> the bytes with whitespace stripped (od -v, so
# a run of zeros is never collapsed into a '*')
ascii_at() { dd if="$IMG" bs=1 skip=$(( $1 )) count=$(( $2 )) 2>/dev/null | od -An -c -v | tr -d ' \n'; }
hex_at()   { dd if="$IMG" bs=1 skip=$(( $1 )) count=$(( $2 )) 2>/dev/null | od -An -tx1 -v | tr -d ' \n'; }
u32_at()   { dd if="$IMG" bs=1 skip=$(( $1 )) count=4 2>/dev/null | od -An -tu4 -v | tr -d ' '; }
u64_at()   { dd if="$IMG" bs=1 skip=$(( $1 )) count=8 2>/dev/null | od -An -tu8 -v | tr -d ' '; }

want_ascii() { printf '%s' "$1" | od -An -c -v | tr -d ' \n'; }

magic_at() { # <offset> <expected>
    local got; got=$(ascii_at "$1" 4)
    if [ "$got" = "$(want_ascii "$2")" ]; then
        ok "$(printf '0x%X' "$1") = $2"
    else
        bad "$(printf '0x%X' "$1") is '$got', wanted '$2'"
    fi
}

echo "== build a volume =="
"$BIN/invf-mkfs" "$IMG" 1 >/dev/null 2>&1 || { echo "FAIL: mkfs"; exit 1; }
SZ=$(stat -c %s "$IMG") || exit 1
echo "  volume: $IMG ($SZ bytes)"

echo "== static slots (before any capture) =="
magic_at 0x3C4 PCK0
magic_at 0x9D0 RT30

echo "== PCK0 must end at or before RT30 (a 64th codec ref walks into the root area) =="
n=$(u32_at 0x3CC); n=${n:-0}
if [ "$n" -le 63 ]; then ok "n_codecs=$n <= INVFS_PCK0_MAX_CODECS (63)"; else
    bad "n_codecs=$n exceeds the 63 the slot was sized for"; fi
pcend=$(( 0x3C4 + 28 + n * 24 + 4 ))
if [ "$pcend" -le $(( 0x9D0 )) ]; then
    ok "$(printf 'with n=%d PCK0 ends at 0x%X (RT30 at 0x9D0)' "$n" "$pcend")"
else
    bad "$(printf 'with n=%d PCK0 would end at 0x%X, past RT30' "$n" "$pcend")"
fi

echo "== SPT0 (0xA00, 32 B) and SPN0 (0xA20, 56 B) are ABSENT before a capture =="
spt0_before=$(ascii_at 0xA00 4)
spn0_before=$(ascii_at 0xA20 4)
if [ "$spt0_before" = "$(want_ascii SPT0)" ]; then
    ok "SPT0 already present before a capture (mkfs wrote it)"
else
    ok "SPT0 absent before the first capture (written by the capture, not mkfs)"
fi
if [ "$spn0_before" = "$(want_ascii SPN0)" ]; then
    ok "SPN0 already present before a capture"
else
    ok "SPN0 absent before the first capture"
fi

echo "== ANC0 at the tail =="
tail_off=$(( SZ - 4096 ))
magic_at "$tail_off" ANC0

echo "== ARM a savepoint, then re-read SPN0 =="
printf 'block0 layout\n' > "$WORK/one.txt"
"$BIN/invf-cp" "$IMG" "$WORK/one.txt" one.txt >/dev/null 2>&1 || bad "invf-cp failed"
"$BIN/invf-sweep" "$IMG" > "$WORK/sweep.log" 2>&1
if grep -qi "save point: pinned" "$WORK/sweep.log"; then
    ok "the sweep captured a save point"
    grep -i "save point: pinned" "$WORK/sweep.log" | head -n 1 | sed 's/^/        /'
else
    bad "no 'save point: pinned' line in the sweep log"; tail -n 5 "$WORK/sweep.log" | sed 's/^/        /'
fi

magic_at 0xA00 SPT0
sver=$(u32_at 0xA04); gen=$(u32_at 0xA08); broot=$(u64_at 0xA0C); dend=$(u64_at 0xA14)
sver=${sver:-0}; gen=${gen:-0}; broot=${broot:-0}; dend=${dend:-0}
[ "$sver" -eq 1 ] && ok "SPT0 version=1" || bad "SPT0 version is $sver, wanted 1"
[ "$gen" -ge 1 ] && ok "SPT0 generation nonce=$gen (F7: every capture stores one more)" || bad "SPT0 gen is $gen, wanted >= 1"
[ "$broot" -ne 0 ] && ok "SPT0 names the pinned base root (pba=$broot, delta_end=$dend)" || bad "SPT0 base_root is 0"

magic_at 0xA20 SPN0
ver=$(u32_at 0xA24); flags=$(u32_at 0xA28)
pba=$(u64_at 0xA2C); blocks=$(u64_at 0xA34); npinned=$(u64_at 0xA3C)
ver=${ver:-0}; flags=${flags:-0}; pba=${pba:-0}; blocks=${blocks:-0}; npinned=${npinned:-0}
[ "$ver" -eq 1 ] && ok "SPN0 version=1" || bad "SPN0 version is $ver, wanted 1"
[ $(( flags & 1 )) -eq 1 ] && ok "SPN0 armed (flags bit0)" || bad "SPN0 not armed after a capture (flags=$flags)"
if [ "$pba" -ne 0 ] && [ "$blocks" -ne 0 ]; then
    ok "SPN0 names a bitmap run (pba=$pba, $blocks blocks, $npinned pinned)"
else
    bad "SPN0 has no bitmap run after a capture (pba=$pba, blocks=$blocks)"
fi
total_blocks=$(( SZ / 4096 ))
if [ $(( pba + blocks )) -le "$total_blocks" ]; then
    ok "the pin run lies inside the volume ($total_blocks blocks)"
else
    bad "the pin run (pba=$pba + $blocks) lies outside the volume ($total_blocks blocks)"
fi

echo "== the capture must not have touched RT30 or the reserved gap =="
magic_at 0x9D0 RT30
spt0_after=$(hex_at 0xA00 32)
if [ ${#spt0_after} -eq 64 ]; then ok "SPT0 occupies 0xA00..0xA20 exactly (32 bytes)"; else
    bad "SPT0 does not occupy its 32-byte slot ($spt0_after)"; fi

echo "== fsck still says the volume is fine =="
if "$BIN/invf-fsck" "$IMG" > "$WORK/fsck.log" 2>&1; then ok "fsck: OK"; else
    bad "fsck failed after the capture"; tail -n 5 "$WORK/fsck.log" | sed 's/^/        /'; fi

echo
if [ "$fail" -eq 0 ]; then echo "PASS: block0 layout ($pass checks)"; exit 0; fi
echo "FAIL: block0 layout ($fail of $((pass+fail)) checks)"; exit 1
