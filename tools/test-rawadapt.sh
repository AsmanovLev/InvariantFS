#!/bin/bash
# test-rawadapt.sh — WP23 adaptive RAW-zone compression effort e2e
# (persistent regression).
#
# The write path picks the per-64K-segment codec by RAW-zone fill
# pressure (vol_write.c):  <80% -> LZ4,  >=80% -> ZSTD-3,
# >=95% -> ZSTD-6; INVFS_PROFILE=turbo forces verbatim NONE,
# INVFS_RAW_ADAPT=0 pins legacy LZ4-only (explicit knobs beat pressure).
# The AST algo field carries the choice and the read path dispatches
# per segment, so mixed-algo files are legal by construction.
#
#   Leg 1: fill ~50%  -> mounted write lands LZ4  (algo=5), bit-exact
#   Leg 2: fill ~84%  -> mounted write lands ZSTD (algo=1), bit-exact
#   Leg 3: fill ~96%  -> mounted write lands ZSTD (algo=1), bit-exact
#   Leg 4: fill ~84%, INVFS_RAW_ADAPT=0   -> stays LZ4 (knob wins)
#   Leg 5: fill ~84%, INVFS_PROFILE=turbo -> verbatim NONE (profile wins)
#   Leg 6: oscillation around 80%: a 1MB write crossing the boundary
#          mid-file (MIXED algos in one AST), a delete dip back to LZ4,
#          a refill back to ZSTD -- every byte bit-exact at every step
#   Leg 7: a maintenance sweep over ZSTD-in-RAW segments transcodes them
#          (the per-segment dispatch in the sweep's RAW reader), then
#          fsck + verify --deep clean and everything still bit-exact
#
# Fill is STEERED EXACTLY: filler goes in offline (invf-cp is the
# non-adaptive create path) and every file's physical block count is
# read back with meta_probe --heat, so the band edges are measured, not
# estimated. Two filler granularities (256KB ~41 blocks, 16KB ~3 blocks)
# let the 80% line be approached to within a couple of blocks. The
# session files under test always go through the MOUNT (the WP4b
# session path is the adaptive one).
#
# Run via the global e2e lock:  bash tools/run-e2e.sh tools/test-rawadapt.sh
# Uses /dev/shm like the other soak scripts. NOTE: blkio treats /dev/*
# paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere (the mountpoint is absolute).
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp23rawadapt
IMG1=wp23rawadapt-a.img    # legs 1-3, 7
IMG2=wp23rawadapt-b.img    # legs 4-6 (precedence + oscillation)
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/ref" "$WORK/out"
cd /dev/shm
rm -f "$IMG1" "$IMG2"

fail() { echo "FAIL: $*" >&2; exit 1; }

# physical blocks a file occupies (L2P entry lens, summed) and the list
# of per-segment AST algos -- both read-only probes of a closed volume
blocks_of() { $B/meta_probe "$1" --heat "$2" 2>/dev/null \
    | awk '/^entry /{for(i=1;i<=NF;i++) if ($i ~ /^len=/) {sub("len=","",$i); s+=$i}} END {print s+0}'; }
algos_of() { $B/meta_probe "$1" --heat "$2" 2>/dev/null \
    | awk '/^ast /{for(i=1;i<=NF;i++) if ($i ~ /^algo=/) {sub("algo=","",$i); printf "%s ", $i}}'; }

mnt_up() {   # <img> [ENV=VAL ...] — daemonized mount, wait for /proc/mounts
    local img=$1; shift
    env "$@" $B/invf-fuse "$img" "$MNT" 2>"$WORK/fuse.$img.log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    fail "mount of $img never appeared"
}

mnt_down() { # <img>
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
    # the unmounted daemon still holds the image open; offline probes
    # must wait for the process to die (two writers = corrupt volume)
    for _ in $(seq 1 300); do
        pgrep -f "invf-fuse $1" >/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

deep_ok() { $B/invf-verify "$1" --deep | tail -1 | grep -q " 0 corrupt," \
    || fail "verify --deep not clean: $1"; }

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse $IMG1" 2>/dev/null || true
    pkill -f "invf-fuse $IMG2" 2>/dev/null || true
}
trap cleanup EXIT

# ---- fixtures ---------------------------------------------------------
# fat filler: random-word text (~1.6:1 under LZ4: 256KB -> ~41 blocks,
# 16KB -> ~3 blocks) so the RAW fill climbs in measured steps; probe
# payloads: highly compressible words corpus (exactly 256KB)
python3 - "$WORK/ref" <<'PY'
import random, sys
d = sys.argv[1]
rnd = random.Random(23)
vocab = [('w%05d' % i).encode() for i in range(12000)]
for k in range(64):
    with open('%s/fat%02d.txt' % (d, k), 'wb') as f:
        n = 0
        while n < 262144:
            w = rnd.choice(vocab); f.write(w); f.write(b' '); n += len(w) + 1
for k in range(40):
    with open('%s/small%02d.txt' % (d, k), 'wb') as f:
        n = 0
        while n < 16384:
            w = rnd.choice(vocab); f.write(w); f.write(b' '); n += len(w) + 1
words = (b'the quick brown fox jumps over the lazy dog invariant fs segment '
         b'pack alpha beta gamma delta\n')
probe = (words * 4000)[:262144]                      # exactly 256KB
for t in ('low', 'mid', 'high', 'koff', 'turbo', 'oscB', 'oscC'):
    open('%s/probe-%s.txt' % (d, t), 'wb').write(probe)
fat = bytearray()
while len(fat) < 1048576:
    w = rnd.choice(vocab); fat += w + b' '
open('%s/fat1m.bin' % d, 'wb').write(bytes(fat))     # the leg-6a crossing write
print("  fixtures: 64 fat + 40 small fillers, 7 probe payloads, 1MB fat")
PY

# ---- geometry ---------------------------------------------------------
$B/invf-mkfs "$IMG1" 0.0625 > "$WORK/mkfs1.log"
RAWBLOCKS=$(sed -n 's/.*raw zone:.*(\([0-9]*\) blocks,.*/\1/p' "$WORK/mkfs1.log")
[ -n "$RAWBLOCKS" ] || fail "could not parse raw zone size"
T80=$(( RAWBLOCKS * 80 / 100 ))
T95=$(( RAWBLOCKS * 95 / 100 ))
echo "  RAW zone: $RAWBLOCKS blocks (80% = $T80, 95% = $T95)"

FILL=0      # running RAW-block total: every add measured via meta_probe
FATN=0
SMALLN=0
LIVE=()     # volume paths the script created and did not delete
fill_to() { # <img> <target-blocks> — offline fillers until FILL >= target
    # fat steps (~41 blocks) only while they cannot overshoot the target;
    # the last ~50 blocks come in small ~3-block steps (worst-case
    # overshoot one small file)
    while [ "$FILL" -lt "$2" ]; do
        local f
        if [ $(( $2 - FILL )) -gt 50 ]; then
            f=$(printf 'fat%02d.txt' "$FATN"); FATN=$((FATN+1))
        else
            f=$(printf 'small%02d.txt' "$SMALLN"); SMALLN=$((SMALLN+1))
        fi
        [ -f "$WORK/ref/$f" ] || fail "filler pool exhausted at $FILL blocks"
        $B/invf-cp "$1" "$WORK/ref/$f" "$f" >/dev/null
        FILL=$((FILL + $(blocks_of "$1" "$f")))
        LIVE+=("$f")
    done
    echo "  fill: $FILL/$RAWBLOCKS blocks ($(( FILL * 100 / RAWBLOCKS ))%)"
}
refill_note() { # recompute FILL from the live set (post-delete honesty)
    local f
    FILL=0
    for f in "${LIVE[@]}"; do
        FILL=$((FILL + $(blocks_of "$1" "$f")))
    done
    echo "  fill (re-derived): $FILL/$RAWBLOCKS ($(( FILL * 100 / RAWBLOCKS ))%)"
}

# write one payload through the mount, then read it back mounted
mwrite() { # <vname> <refname>
    python3 - "$MNT/$1" "$WORK/ref/$2" <<'PY'
import sys
open(sys.argv[1], 'wb').write(open(sys.argv[2], 'rb').read())
PY
    sync
    cmp "$WORK/ref/$2" "$MNT/$1" || fail "mounted read-back of $1 not bit-exact"
}

assert_algos() { # <img> <vname> <want-egrep> <what>
    local got; got=$(algos_of "$1" "$2")
    echo "  $2 algos: $got($3)"
    echo "$got" | grep -qE "$3" || fail "$2: want algo pattern '$3', got '$got'"
}

bitexact_off() { # <img> <vname> <refname>
    $B/invf-cat "$1" "$2" "$WORK/out/$2" >/dev/null
    cmp "$WORK/ref/$3" "$WORK/out/$2" || fail "$2: offline read not bit-exact"
}

echo "== [1] fill ~50%: session write stays LZ4 =="
fill_to "$IMG1" $(( RAWBLOCKS / 2 ))
mnt_up "$IMG1"
mwrite s_low.txt probe-low.txt
mnt_down "$IMG1"
assert_algos "$IMG1" s_low.txt '^5 5 5 5 $' "all LZ4 below 80%"
bitexact_off "$IMG1" s_low.txt probe-low.txt
FILL=$((FILL + $(blocks_of "$IMG1" s_low.txt))); LIVE+=("s_low.txt")
fsck_ok "$IMG1"

echo
echo "== [2] fill ~84%: session write lands ZSTD (the >=80% rung) =="
fill_to "$IMG1" $(( T80 + 61 ))
mnt_up "$IMG1"
mwrite s_mid.txt probe-mid.txt
mnt_down "$IMG1"
assert_algos "$IMG1" s_mid.txt '^1 1 1 1 $' "all ZSTD at >=80%"
bitexact_off "$IMG1" s_mid.txt probe-mid.txt
FILL=$((FILL + $(blocks_of "$IMG1" s_mid.txt))); LIVE+=("s_mid.txt")
deep_ok "$IMG1"

echo
echo "== [3] fill ~96%: session write lands ZSTD (the >=95% rung) =="
fill_to "$IMG1" $(( T95 + 10 ))
mnt_up "$IMG1"
mwrite s_high.txt probe-high.txt
mnt_down "$IMG1"
assert_algos "$IMG1" s_high.txt '^1 1 1 1 $' "all ZSTD at >=95%"
bitexact_off "$IMG1" s_high.txt probe-high.txt
FILL=$((FILL + $(blocks_of "$IMG1" s_high.txt))); LIVE+=("s_high.txt")
fsck_ok "$IMG1"
deep_ok "$IMG1"

echo
echo "== [4] knob off: INVFS_RAW_ADAPT=0 keeps LZ4 at ~84% =="
$B/invf-mkfs "$IMG2" 0.0625 >/dev/null
FILL=0; FATN=0; SMALLN=0; LIVE=()
fill_to "$IMG2" $(( T80 + 61 ))
mnt_up "$IMG2" INVFS_RAW_ADAPT=0
mwrite s_koff.txt probe-koff.txt
mnt_down "$IMG2"
assert_algos "$IMG2" s_koff.txt '^5 5 5 5 $' "knob off -> LZ4 despite pressure"
bitexact_off "$IMG2" s_koff.txt probe-koff.txt

echo
echo "== [5] profile precedence: INVFS_PROFILE=turbo -> verbatim NONE at ~84% =="
mnt_up "$IMG2" INVFS_PROFILE=turbo
mwrite s_turbo.txt probe-turbo.txt
mnt_down "$IMG2"
assert_algos "$IMG2" s_turbo.txt '^0 0 0 0 $' "turbo -> verbatim despite pressure"
bitexact_off "$IMG2" s_turbo.txt probe-turbo.txt
fsck_ok "$IMG2"
deep_ok "$IMG2"

echo
echo "== [6] pressure oscillation around the 80% line =="
rm -f "$IMG2"
$B/invf-mkfs "$IMG2" 0.0625 >/dev/null    # fresh (an image file is not erased by re-mkfs)
FILL=0; FATN=0; SMALLN=0; LIVE=()
echo "  [6a] 1MB write crossing 80% mid-file -> LZ4 head, ZSTD tail"
# NOTE: the kernel may dispatch FUSE writes out of order; the session
# then zero-fills the not-yet-written gap first (each zero segment is
# decided at the CURRENT pressure) and re-touches it with real content
# later. Starting far enough below the line keeps even the pathological
# order (gap-fill of the whole file, then content) from tipping seg 0
# over 80%: assertions below are written order-agnostic on purpose
# (LZ4 head present + ZSTD present + never a ZSTD->LZ4 regression --
# pressure only climbs within one write).
fill_to "$IMG2" $(( T80 - 30 ))
mnt_up "$IMG2"
mwrite oscA.txt fat1m.bin
mnt_down "$IMG2"
ALG=$(algos_of "$IMG2" oscA.txt)
echo "  oscA algos: $ALG"
case "$ALG" in
    "5 "*) ;;                             # head below the line: LZ4
    *) fail "oscA: head segments not LZ4: $ALG";;
esac
echo "$ALG" | grep -qw 1 || fail "oscA: no ZSTD tail (80% never crossed mid-write)"
# the LZ4 prefix must be a PREFIX: once the line is crossed no later
# segment may drop back (pressure is monotone inside one write)
case "$ALG" in *"1 5 "*) fail "oscA: algo regressed after crossing: $ALG";; esac
bitexact_off "$IMG2" oscA.txt fat1m.bin
FILL=$((FILL + $(blocks_of "$IMG2" oscA.txt))); LIVE+=("oscA.txt")

echo "  [6b] deletes dip under 80% -> LZ4 again"
mnt_up "$IMG2"
DEL=0
for f in "${LIVE[@]}"; do
    case "$f" in fat*)
        rm -f "$MNT/$f"
        DEL=$((DEL+1))
        [ "$DEL" -ge 6 ] && break;;
    esac
done
[ "$DEL" -ge 6 ] || fail "leg 6b: not enough fillers to delete"
LIVE=("${LIVE[@]:6}")    # the fillers are the LIVE head
sync
mwrite oscB.txt probe-oscB.txt
mnt_down "$IMG2"
assert_algos "$IMG2" oscB.txt '^5 5 5 5 $' "after deletes -> LZ4 again"
bitexact_off "$IMG2" oscB.txt probe-oscB.txt
LIVE+=("oscB.txt")

echo "  [6c] refill over 80% -> ZSTD again"
refill_note "$IMG2"
fill_to "$IMG2" $(( T80 + 61 ))
mnt_up "$IMG2"
mwrite oscC.txt probe-oscC.txt
mnt_down "$IMG2"
assert_algos "$IMG2" oscC.txt '^1 1 1 1 $' "refilled -> ZSTD again"
bitexact_off "$IMG2" oscC.txt probe-oscC.txt
fsck_ok "$IMG2"
deep_ok "$IMG2"
echo "  oscillation: LZ4 -> (cross mid-file) -> ZSTD -> LZ4 -> ZSTD, all bit-exact"

echo
echo "== [7] maintenance sweep over ZSTD-in-RAW segments =="
$B/invf-sweep "$IMG1" > "$WORK/sweep1.log" 2>&1 \
    || { cat "$WORK/sweep1.log"; fail "sweep of pressure-written volume failed"; }
grep -q "sweep done" "$WORK/sweep1.log" || fail "sweep did not complete"
grep -q "failed=0" "$WORK/sweep1.log" || fail "sweep reports failures"
# the pressure-written files transcode OUT of RAW...
$B/meta_probe "$IMG1" --heat s_mid.txt 2>/dev/null | grep -q "zone=[12]" \
    || fail "s_mid.txt still RAW after sweep"
# ...and every byte survives (ZSTD-in-RAW decode fed the transcode)
bitexact_off "$IMG1" s_low.txt probe-low.txt
bitexact_off "$IMG1" s_mid.txt probe-mid.txt
bitexact_off "$IMG1" s_high.txt probe-high.txt
# second bare sweep auto-realizes the WP21 checkpoint, settling the volume
$B/invf-sweep "$IMG1" > "$WORK/sweep2.log" 2>&1 \
    || { cat "$WORK/sweep2.log"; fail "realize sweep failed"; }
fsck_ok "$IMG1"
deep_ok "$IMG1"

rm -f "$IMG1" "$IMG2"
echo
echo "RAW-ADAPT E2E: PASS"
