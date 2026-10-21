#!/bin/bash
# test-ntfs-big.sh — WP640 NTFS pack gate 4 GiB -> 1 TiB.
#
# WHY THIS SCRIPT EXISTS. tools/test-ntfs.sh needs mkfs.ntfs + sudo
# loop-mounts, neither of which exists on this box (it exits at its own
# line 90), so the pack's size gate cannot be exercised by the corpus
# suite here. This script carries its own fixture: a minimal synthetic
# NTFS image writer (python, below) that emits byte-exact structures the
# REAL pack parses -- boot sector, fixup'd FILE records, resident
# $FILE_NAME, non-resident $DATA runlists -- so the gate under test is
# the gate that ships, not a mock of it.
#
#   Leg L0 (fixture validity): small image -> `ntfs enumerate` names both
#   user files with exact sizes. Refuses loudly if the writer drifts.
#   Leg L1 (gate moved): the SAME structures truncated sparse to 5 GiB
#   (st_size > old 4 GiB gate) -> enumerate exits 0 with both members.
#   Old code: exit 3 (decline). No volume, no sweep: enumerate reads
#   only the MFT the runlists name.
#   Leg L2 (honest ceiling): sparse 2 TiB image -> enumerate exits 3.
#   Above 1 TiB the AST block_id wall binds (needs WP635/WP639 wide
#   ids); failing closed is the contract.
#   Leg L3 (lifecycle, 128 MiB): mkfs -> invf-cp -> offline sweep
#   (log must name the ntfs codecpack lane) -> invf-fsck CLEAN ->
#   invf-cat bit-exact vs the original -> unlink -> members gone ->
#   fsck CLEAN.
#   Leg L3p (WP641 streaming parity): the SAME 128 MiB container through
#   both admission paths -- whole-file (in.size <= INVFS_SWEEP_MAX_FILE)
#   and streaming (cap forced to 1 KiB, so the head-sniff branch takes
#   vol_containerpack_sweep_stream: ranged pin + mmap, O(1) RAM). Asserts
#   the two paths agree byte for byte: same sibling set, same container
#   readback, same member payloads, fsck CLEAN on both. This is the
#   cross-check WP641's spec lists as unprovable without a big-RAM box --
#   the cap makes both paths reachable on one box.
#   Leg L4 (scale, 16 GiB, 200 x 60 MiB members): same lifecycle on the
#   big scratch (INVFS_BIG_SCRATCH unset => SKIP). Asserts the scale
#   properties unconditionally (bit-exact round-trip through the mounted
#   read path -- invf-cat cannot malloc a 16 GiB file -- plus fsck CLEAN
#   plus unlink reclaim) and records WHICH path won, from the engine's own
#   marker rather than by inference: decomposed by the STREAMING admission
#   (200 members + table + map, "streaming admission: ... bytes (ranged pin
#   + mmap)" in sweep.log or fuse.log -- both processes are instrumented),
#   or generic-ZSTD (floor won), with the lane's own refusal line quoted so
#   a decline says WHY.
#
#   WHY A DECLINE IS STILL LEGAL AFTER WP641: the lane no longer needs the
#   whole file in RAM (it streams the pin and mmaps it), but three other
#   walls remain and none is this gate's subject. (a) The size guard
#   prices the shape by ZSTD-19-ing every member -- hours on 12 GiB of
#   members -- so the leg is expensive, not impossible. (b) The pack stages
#   the container plus every member in tool scratch (peak ~2x the
#   container), so a 16 GiB decomposition wants ~40 GiB of scratch; the
#   leg therefore points BOTH the mount and the offline sweep at the big
#   scratch (INVFS_SCRATCH_ROOTS=$BS/scratch first, the defaults behind
#   it), and a box without that much room gets the lane's printed demand
#   instead of a decomposition. (c) THE RECIPE: the engine slurps it whole
#   and writes it as one blob, so the lane's RAM is O(recipe), and this
#   pack stores the COMPLEMENT OF THE MEMBERS byte-exact in the recipe --
#   4,596,965,280 B for this corpus (16 GiB image, 12 GiB of members),
#   measured. A 4.6 GiB malloc on a 7 GiB box with ~2 GiB available was
#   OOM-killed at exactly that point (2026-10-21T11:05, the sweep died with
#   the recipe file already written); the leg therefore sets
#   INVFS_CPACK_RECIPE_MAX=1024 so the lane REFUSES BY NAME there instead of
#   dying, and quotes that line. On a box with RAM to hold the recipe, drop
#   the knob (it is unset by default = no limit) and the same leg decomposes.
#   What WP641 removed is the OLD wall: a
#   16 GiB whole-file malloc on a 7 GiB box, which failed silently and
#   handed every big container to the generic floor. The generic branch
#   below quotes
#   the lane's reason instead of assuming "declined on RAM".
#   AGENTS.md 2.5's "the tick never transforms" describes an older tick:
#   the drain calls vol_sweep_pending -> vol_sweep_one (full dispatch,
#   lanes + generic floor + stamps). Doc bug, filed separately.
#
# SCOPE NOTE (measured, not assumed): the hermetic legs stop at 128 MiB;
# the scale leg L4 (16 GiB on INVFS_BIG_SCRATCH) proves the lifted gate at
# volume level past the old 4 GiB ceiling. The pack accepts to 1 TiB (legs
# L1/L2), but a multi-GB image's NTRC recipe blob is read whole by the
# sweep transform (vol_read_inode mallocs file_size), which is the
# WP634-A read-streaming wall, not this pack's. Larger volume-level
# images wait for that chain; this WP only lifts the pack gate and
# refuses honestly above it. WP641 then moved the CONTAINER LANE off
# that wall for over-cap containers (streamed pin + mmap, no whole-file
# malloc: src/core/vol_cpack.c, vol_containerpack_sweep_stream) -- leg
# L3p is that change's byte-parity gate.
#
# Run from the repo root after `make`:  bash tools/test-ntfs-big.sh
# Under e2e rules via: bash tools/run-e2e.sh tools/test-ntfs-big.sh
# with INVFS_E2E_AGENT=wp640 (AGENTS.md 1.5). Uses /dev/shm, RELATIVE
# image paths (blkio treats /dev/* as raw devices).
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
PACK=$REPO/tools/codecpacks/ntfs.codecpack
WORK=/dev/shm/wp640ntfsbig
MNT=$WORK/mnt
# A lingering FUSE daemon from a killed run holds $WORK/mnt and makes
# `rm -rf` below fail with EBUSY (leaving a half-deleted tree behind).
# Unmount first; the daemon itself is left alone (await_vol_free in the
# L4 leg deals with it by PID where a lock actually matters).
for _ in $(seq 1 50); do
    grep -q " $WORK/mnt " /proc/mounts || break
    fusermount3 -u "$WORK/mnt" 2>/dev/null || true
    sleep 0.2
done
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/ref"
cd /dev/shm
rm -f wp640-s.img wp640-5g.img wp640-2t.img wp640-512m.img wp640-vol.img

fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "  OK    $1"; }

echo "== build the pack (manifest recipe, engine-visible path) =="
mkdir -p "$PACK/bin"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACK/bin/ntfs" "$PACK/ntfs.c" \
    || fail "pack build"
NTFS=$PACK/bin/ntfs
# the sweep AND the reads resolve helpers through this dir (test-ntfs.sh:74)
export INVFS_CODECPACKS=$REPO/tools/codecpacks

echo "== write synthetic fixtures =="
cat > "$WORK/mkntfs.py" <<'PYEOF'
import os, random, struct, sys
CS = 4096          # cluster: 8 x 512 B sectors
REC = 1024         # MFT record size (shift -10)
NREC = 32          # MFT records 0..31 -> 8 clusters
MFT_LCN = 4
DATA_LCN = 16

def usa_fixup(rec, usn=0x1234):
    # USA at 42: [usn, rep0, rep1]; trailers at 510 and 1022 carry the USN.
    r = bytearray(rec)
    struct.pack_into('<HHH', r, 42, usn, 0xAAAA, 0xBBBB)
    r[510:512] = struct.pack('<H', usn)
    r[1022:1024] = struct.pack('<H', usn)
    return bytes(r)

def attr_pad(a):
    pad = (-len(a)) % 8
    return a + b'\0' * pad

def filename_attr(name):
    un = name.encode('utf-16le')
    content = bytearray(66 + len(un))
    struct.pack_into('<Q', content, 0, 5)          # parent ref (root-ish)
    content[64] = len(name)
    content[65] = 1                                # Win32 namespace
    content[66:66 + len(un)] = un
    clen = len(content)
    total = (24 + clen + 7) & ~7
    a = bytearray()
    a += struct.pack('<IIBBHHH', 0x30, total, 0, 0, 0, 0, 1)
    a += struct.pack('<IHBB', clen, 24, 0, 0)   # len/coff/flags/pad
    a += bytes(content)
    assert len(a) <= total
    a = attr_pad(bytes(a))
    assert len(a) == total
    return a

def data_attr(lcn, ncl, real):
    # one contiguous run; widths are sized, not assumed (a 16 GiB image
    # needs 3-byte LCNs, and olen/llen are capped at 8 by the runlist spec).
    # runlist: [header][len llen][lcn-delta olen][terminator]; length FIRST.
    # The delta is SIGNED (a set top bit decodes as negative), so a
    # non-negative delta whose top bit would be set needs one more byte
    # (real NTFS encoders do the same; without it the pack correctly
    # refuses with "bad runlist"). Lengths are unsigned: no change.
    lb = ncl.to_bytes(max(1, (ncl.bit_length() + 7) // 8), 'little')
    _db = lcn.to_bytes(max(1, (lcn.bit_length() + 7) // 8), 'little')
    db = _db if not (_db[-1] & 0x80) else _db + b'\x00'
    assert len(lb) <= 8 and len(db) <= 8, 'run does not fit u64 runlist'
    run = bytes([(len(db) << 4) | len(lb)]) + lb + db + b'\0'
    total = (64 + len(run) + 7) & ~7
    a = bytearray()
    a += struct.pack('<IIBBHHH', 0x80, total, 1, 0, 0, 0, 1)
    a += struct.pack('<QQH', 0, ncl - 1, 64)       # low/high VCN, run off
    a += b'\0\0\0\0\0\0'                           # comp unit + pad
    a += struct.pack('<QQQ', ncl * CS, real, real) # alloc/real/init
    a += run
    assert len(a) <= total
    a = attr_pad(bytes(a))
    assert len(a) == total
    return a

def record(flags, attrs):
    r = bytearray(REC)
    r[0:4] = b'FILE'
    struct.pack_into('<HH', r, 4, 42, 3)           # usa_off, usa_n (2 sec)
    struct.pack_into('<HHH', r, 16, 1, 1, 48)      # seq, links, aoff
    struct.pack_into('<H', r, 22, flags)
    struct.pack_into('<HH', r, 24, REC, REC)
    struct.pack_into('<H', r, 40, 1)               # next attr id
    off = 48
    for a in attrs:
        r[off:off + len(a)] = a
        off += len(a)
    struct.pack_into('<I', r, off, 0xFFFFFFFF)     # END marker
    return usa_fixup(bytes(r))

def boot():
    b = bytearray(512)
    b[0:3] = bytes([0xEB, 0x52, 0x90])
    b[3:11] = b'NTFS    '
    struct.pack_into('<H', b, 11, 512)
    b[13] = 8                                      # sectors/cluster
    struct.pack_into('<Q', b, 48, MFT_LCN)
    struct.pack_into('<Q', b, 56, MFT_LCN)         # mirror (unchecked)
    b[64] = 256 - 10                               # rec shift -10 -> 1024
    b[65] = 1
    b[510:512] = b'\x55\xAA'
    return bytes(b)

def content(rnd, name, kind, nbytes):
    # One file's bytes. The caller must drop the reference right after
    # the write: a 12 GiB corpus must never sit in RAM at once (7 GiB
    # box, no swap -- the OOM killer proved it twice, anon-rss 4-5 GiB).
    if kind == 'text':
        unit = b'pack lantern %s\n' % name.encode()
        data = (unit * (65536 // len(unit) + 1))[:65536]
        return (data * (nbytes // len(data) + 1))[:nbytes]
    if kind == 'rand':
        return rnd.randbytes(nbytes)
    data = b'\x00\xff\x55\xaa' * 4096
    return (data * (nbytes // len(data) + 1))[:nbytes]

def img(layout, path, size, nrec, seed=0x640):
    # layout: [(name, kind, nbytes, lcn)] -- runs must be disjoint, in-image.
    # Two phases, peak RSS ~= largest single file: (1) MFT + boot from
    # metadata only; (2) generate + write each file, releasing before next.
    rnd = random.Random(seed)
    ncl_mft = (nrec * REC + CS - 1) // CS
    nrecb = bytearray(nrec * REC)
    # record 0: $MFT, unnamed DATA covering the MFT runs
    r0 = record(1, [data_attr(MFT_LCN, ncl_mft, nrec * REC)])
    nrecb[0:REC] = r0
    for i, (name, kind, nbytes, lcn) in enumerate(layout):
        ncl = (nbytes + CS - 1) // CS
        r = record(1, [filename_attr(name),
                       data_attr(lcn, ncl, nbytes)])
        nrecb[(16 + i) * REC:(17 + i) * REC] = r
    with open(path, 'wb') as f:
        f.write(boot())
        # MFT at its LCN
        f.seek(MFT_LCN * CS)
        f.write(bytes(nrecb))
        for (name, kind, nbytes, lcn) in layout:
            f.seek(lcn * CS)
            f.write(content(rnd, name, kind, nbytes))
        f.truncate(size)

def gen(outdir, outname, nrec, spec, size, seed=0x640, start_lcn=16):
    # spec: [(name, kind, nbytes)] -- layout first (sizes/LCNs only, no
    # content), so the 12 GiB corpus never accumulates in RAM (see content()
    # above for why). Whole-image memcmp is the bar, so no per-file
    # reference copies are staged.
    layout = []
    lcn = start_lcn
    for name, kind, nbytes in spec:
        ncl = (nbytes + CS - 1) // CS
        assert lcn * CS + nbytes <= size, 'run past the image end'
        layout.append((name, kind, nbytes, lcn))
        lcn += ncl
    img(layout, os.path.join(outdir, outname), size, nrec, seed)

if __name__ == '__main__':
    # args: outdir outname nrec size seed start_lcn name:kind:bytes ...
    outdir, outname = sys.argv[1], sys.argv[2]
    nrec, size = int(sys.argv[3]), int(sys.argv[4])
    seed, start = int(sys.argv[5], 0), int(sys.argv[6])
    spec = []
    for tok in sys.argv[7:]:
        name, kind, nbytes = tok.split(':')
        spec.append((name, kind, int(nbytes)))
    gen(outdir, outname, nrec, spec, size, seed, start)
    print('fixture written: %s/%s' % (outdir, outname))
PYEOF
# Memory discipline for the fixture writer: it streams one file at a time
# (peak ~= largest member), but a future edit could reintroduce
# accumulation -- and on this 7 GiB swapless box that OOM-kills whatever
# the kernel picks, which has now twice taken the whole session down with
# it. The cap fails the generator fast and loud instead (2 GiB address
# space; the writer needs ~200 MiB). It is a subshell, so the suite's own
# limit is untouched.
mkntfs() { ( ulimit -v 2097152; exec python3 "$WORK/mkntfs.py" "$@" ); }
mkntfs "$WORK/ref" small.img 32 1048576 0x640 16 \
    f1.bin:rand:4096 f2.bin:rand:12288
mkntfs "$WORK/ref" big5g.img 32 5368709120 0x640 16 \
    f1.bin:rand:4096 f2.bin:rand:12288
mkntfs "$WORK/ref" big2t.img 32 2199023255552 0x640 16 \
    f1.bin:rand:4096 f2.bin:rand:12288
mkntfs "$WORK/ref" life128m.img 32 134217728 0x640 16 \
    t1.txt:text:41943040 r1.bin:rand:41943040 p1.bin:pat:20971520

echo "== [L0] fixture validity through the real pack =="
$NTFS enumerate "$WORK/ref/small.img" "$WORK/enum0.txt" \
    || fail "enumerate small (fixture invalid?)"
grep -q "f1.bin" "$WORK/enum0.txt" || fail "f1 missing from enumerate"
grep -q "f2.bin" "$WORK/enum0.txt" || fail "f2 missing from enumerate"
ok "L0: synthetic NTFS parses, both members enumerated"

echo "== [L1] 5 GiB sparse: gate moved (was exit 3) =="
$NTFS enumerate "$WORK/ref/big5g.img" "$WORK/enum5g.txt" \
    || fail "enumerate 5 GiB declined (gate did not move)"
grep -q "f1.bin" "$WORK/enum5g.txt" && grep -q "f2.bin" "$WORK/enum5g.txt" \
    || fail "5 GiB members missing"
ok "L1: 5 GiB image accepted with both members"

echo "== [L2] 2 TiB sparse: honest refuse =="
if $NTFS enumerate "$WORK/ref/big2t.img" "$WORK/enum2t.txt" 2>/dev/null; then
    fail "2 TiB image accepted (above the 1 TiB AST wall)"
fi
ok "L2: 2 TiB image declined (exit 3, fail closed)"

echo "== [L3] 128 MiB lifecycle: sweep, rebuild, fsck, unlink =="
$B/invf-mkfs wp640-vol.img 1 >/dev/null || fail "mkfs"
$B/invf-cp wp640-vol.img "$WORK/ref/life128m.img" disk.ntfs >/dev/null \
    || fail "cp image"
INVFS_DEBUG_PACKS=1 $B/invf-sweep wp640-vol.img > "$WORK/sweep.log" 2>&1 \
    || fail "sweep"
grep -q "ntfs (codecpack)" "$WORK/sweep.log" \
    || fail "sweep log does not name the ntfs lane"
# 128 MiB is under the default 256 MiB cap, so this leg must have taken the
# untouched whole-file path: the streaming marker must NOT appear here (L3p
# asserts the other direction on the same fixture).
if grep -q "streaming admission" "$WORK/sweep.log"; then
    fail "whole-file leg took the streaming admission (cap not honoured)"
fi
$B/invf-fsck wp640-vol.img | tee "$WORK/fsck1" | grep -q "^OK$" \
    || fail "fsck after sweep not OK"
$B/invf-cat wp640-vol.img disk.ntfs > "$WORK/rebuilt.img" \
    || fail "cat rebuilt"
cmp "$WORK/ref/life128m.img" "$WORK/rebuilt.img" \
    || fail "rebuilt image not bit-exact"
ok "L3: sweep decomposes, rebuild bit-exact, fsck OK"

echo "== [L3p] WP641 streaming admission == whole-file path (byte parity) =="
# wp640-vol.img above was swept with the DEFAULT cap (256 MiB), so this
# 128 MiB container took the whole-file path (vol_read_inode ->
# vol_containerpack_sweep with the bytes in hand). The same fixture into a
# second volume with INVFS_SWEEP_MAX_FILE=1024 pushes it over the cap, so
# the sweep's head-sniff branch takes vol_containerpack_sweep_stream.
# One fixture, two admissions, one commit -- any drift shows up here.
$B/invf-mkfs wp640-str.img 1 >/dev/null || fail "parity mkfs"
$B/invf-cp wp640-str.img "$WORK/ref/life128m.img" disk.ntfs >/dev/null \
    || fail "parity cp"
INVFS_DEBUG_PACKS=1 INVFS_SWEEP_MAX_FILE=1024 \
    $B/invf-sweep wp640-str.img > "$WORK/sweep-str.log" 2>&1 \
    || fail "streaming sweep"
grep -q "ntfs (codecpack)" "$WORK/sweep-str.log" \
    || fail "streaming sweep did not name the ntfs lane"
# Which admission ran is not visible in the pack's own output (both paths
# run the same pack commands), so the engine announces it: the streaming
# branch prints this marker, the whole-file branch cannot (it never maps a
# pin). Asserting BOTH directions is the point -- a cap that is silently
# ignored would show the marker on the wrong leg.
grep -q "streaming admission: 134217728 bytes" "$WORK/sweep-str.log" \
    || fail "streaming leg did not take the streaming admission (cap ignored)"
$B/invf-fsck wp640-str.img | grep -q "^OK$" \
    || fail "fsck on the streaming volume not OK"
$B/invf-fsck wp640-vol.img | grep -q "^OK$" \
    || fail "fsck on the whole-file volume not OK"
# Same names AND same sizes on both volumes; the inode id is left out
# because it records allocation history, not the decomposition's shape.
$B/invf-ls wp640-str.img | awk '/ bytes  inode /{print $1, $NF}' \
    | sort > "$WORK/ls-str.txt"
$B/invf-ls wp640-vol.img | awk '/ bytes  inode /{print $1, $NF}' \
    | sort > "$WORK/ls-whole.txt"
cmp "$WORK/ls-str.txt" "$WORK/ls-whole.txt" \
    || fail "streaming and whole-file volumes list different siblings"
awk '{print $NF}' "$WORK/ls-str.txt" | grep '^disk\.ntfs!' \
    > "$WORK/sibs.txt"
grep -q . "$WORK/sibs.txt" || fail "streaming volume has no member siblings"
$B/invf-cat wp640-str.img disk.ntfs > "$WORK/str-container.img" \
    || fail "streaming cat"
cmp "$WORK/ref/life128m.img" "$WORK/str-container.img" \
    || fail "streaming volume container not bit-exact"
# Member payloads, one by one: the sibling names carry the pack's member
# ids, so this compares the stored members, not just the reconstruction.
while read -r sib; do
    $B/invf-cat wp640-str.img "$sib" > "$WORK/m-str.bin" \
        || fail "streaming member cat ($sib)"
    $B/invf-cat wp640-vol.img "$sib" > "$WORK/m-whole.bin" \
        || fail "whole-file member cat ($sib)"
    cmp "$WORK/m-str.bin" "$WORK/m-whole.bin" \
        || fail "member $sib differs between the two paths"
done < "$WORK/sibs.txt"
ok "L3p: streaming admission == whole-file path "\
"($(wc -l < "$WORK/sibs.txt") sibling(s), bytes identical)"

mnt_up() {
    $B/invf-fuse "$1" "$MNT" 2>"$WORK/fuse.log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    fail "mount of $1 never appeared"
}
mnt_up wp640-vol.img
ls "$MNT" | grep -q "disk.ntfs" || fail "container missing through mount"
rm "$MNT/disk.ntfs" || fail "unlink through mount"
fusermount3 -u "$MNT" 2>/dev/null || true
for _ in $(seq 1 50); do
    grep -q " $MNT " /proc/mounts || break
    sleep 0.1
done
$B/invf-fsck wp640-vol.img | grep -q "^OK$" || fail "fsck after unlink not OK"
ok "L3: unlink through mount reclaims, fsck OK"

echo "== [L4] 16 GiB scale lifecycle (needs INVFS_BIG_SCRATCH, else SKIP) =="
# After fusermount -u the mountpoint is gone but the daemon may still be
# flushing a big volume (observed: alive 30 s+ post-unmount, S-state, TERM
# ignored). The sweep/fsck take an flock on the image and fail spuriously
# while it lives, so wait on the LOCK -- never on a name pattern (a
# pkill -f pattern matches the test's own shell; that has killed the
# operator twice, it will not kill the suite).
await_vol_free() {  # <img> [seconds]
    img=$1; budget=${2:-300}; i=0
    while [ $i -lt "$budget" ]; do
        if flock -n "$img" true 2>/dev/null; then return 0; fi
        sleep 1; i=$((i + 1))
    done
    echo "note: $img still locked after ${budget}s; holders:" >&2
    for p in $(pgrep -x invf-fuse 2>/dev/null); do
        if tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | grep -qF "$img"; then
            echo "  pid $p: $(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" >&2
            kill "$p" 2>/dev/null || true
        fi
    done
    sleep 5
    if flock -n "$img" true 2>/dev/null; then
        echo "note: $img freed after TERM" >&2; return 0
    fi
    for p in $(pgrep -x invf-fuse 2>/dev/null); do
        if tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | grep -qF "$img"; then
            echo "note: KILL $p (TERM ignored)" >&2
            kill -9 "$p" 2>/dev/null || true
        fi
    done
    sleep 2
    flock -n "$img" true 2>/dev/null
}
if [ -z "${INVFS_BIG_SCRATCH:-}" ]; then
    echo "  SKIP   L4: set INVFS_BIG_SCRATCH to a dir with ~40 GB free"
else
    BS="$INVFS_BIG_SCRATCH/wp640big16"
    rm -rf "$BS" && mkdir -p "$BS/mnt" "$BS/scratch"
    # Streaming decomposition stages the whole container in scratch;
    # point the scratch roots at the big disk (the default tmpfs roots
    # cannot hold a 16 GiB pin + members + rebuild).
    SPEC=""
    i=0
    while [ $i -lt 200 ]; do
        if [ $((i % 2)) -eq 0 ]; then
            SPEC="$SPEC f$(printf %04d $i).bin:text:62914560"
        else
            SPEC="$SPEC f$(printf %04d $i).bin:rand:62914560"
        fi
        i=$((i + 1))
    done
    # shellcheck disable=SC2086
mkntfs "$BS" big16.img 256 17179869184 0x641 80 \
        $SPEC || fail "L4 fixture (200 x 60 MiB)"
    $B/invf-mkfs "$BS/bigvol.img" 24 >/dev/null || fail "L4 mkfs"
    # invf-cp reads the whole file into RAM; a 16 GiB image goes through
    # the streaming FUSE write path instead (write sessions stage 64 KiB).
    # WP641: the streaming lane stages ~2x the container in tool scratch,
    # so point the scratch roots at the big disk FIRST (the default tmpfs
    # roots cannot hold a 16 GiB pin + members + rebuild), and set the
    # sweep interval so the 1 Hz drain's occupancy guard defers while
    # this dd holds a handle: with the interval unset the drain checks no
    # occupancy and runs a FULL per-file sweep mid-dd, stamping a
    # half-written 16 GiB file GENERIC -- after which the offline sweep
    # correctly skips it and the lane never gets its turn. This changes
    # nothing about WHERE lanes run; it is the mount knob an operator
    # would use for the same reason.
    INVFS_SWEEP_INTERVAL=1 INVFS_SCRATCH_ROOTS="$BS/scratch:/dev/shm:/tmp" \
        INVFS_DEBUG_PACKS=1 \
        $B/invf-fuse "$BS/bigvol.img" "$BS/mnt" 2>"$BS/fuse.log"
    for _ in $(seq 1 50); do
        grep -q " $BS/mnt " /proc/mounts && break
        sleep 0.1
    done
    grep -q " $BS/mnt " /proc/mounts || fail "L4 mount never appeared"
    dd if="$BS/big16.img" of="$BS/mnt/disk.ntfs" bs=4M status=none \
        || fail "L4 dd through mount"
    sync
    fusermount3 -u "$BS/mnt" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $BS/mnt " /proc/mounts || break
        sleep 0.1
    done
    await_vol_free "$BS/bigvol.img" 300 || fail "L4 volume still locked"
    echo "== [L4] pre-sweep state (informational: the live daemon may" 
    echo "    already have swept this file in the background) =="
    # NOTE: no `| head` here -- head closes the pipe early, SIGPIPE kills
    # meta_probe mid-dump, and with `set -o pipefail` + `set -e` that ends
    # the whole script silently (no FAIL line). Dump to a file instead.
    $B/meta_probe "$BS/bigvol.img" --heat disk.ntfs > "$BS/presweep.txt" 2>&1 \
        || fail "L4 presweep probe"
    head -n 8 "$BS/presweep.txt"
    # The same scratch roots the mount above got, for the same reason: the
    # lane stages the container (pin + every member + the rebuild output)
    # in tool scratch, so the OFFLINE sweep is the process that needs the
    # big disk. Without this the lane declines on SCRATCH -- "no root can
    # hold this job's bytes" -- and that is a different wall from the one
    # WP641 lifted (whole-file RAM). Leaving it unset would make this leg
    # assert the scratch policy of whatever /dev/shm and /tmp happen to be
    # on, not the streaming admission under test. The default search order
    # is preserved behind $BS/scratch, so a host without the big scratch
    # behaves exactly as before.
    INVFS_SCRATCH_ROOTS="$BS/scratch:/dev/shm:/tmp:/var/tmp" \
        INVFS_CPACK_RECIPE_MAX=1024 INVFS_DEBUG_PACKS=1 \
        $B/invf-sweep "$BS/bigvol.img" > "$BS/sweep.log" 2>&1 \
        || fail "L4 sweep"
    $B/invf-fsck "$BS/bigvol.img" | grep -q "^OK$" \
        || fail "L4 fsck after sweep not OK"
    # Two legal outcomes, both bit-exact (see header note on the
    # whole-file lane RAM wall): decomposed (lane won: 200 members +
    # table + map) or generic-ZSTD (floor won: file stamped GENERIC).
    # invf-cat cannot serve a 16 GiB file (whole-file malloc), so the
    # rebuild check always goes through the mounted (ranged) read path.
    NM=$($B/invf-ls "$BS/bigvol.img" | grep -c "disk.ntfs!" || true)
    # Both processes that can run the lane are instrumented
    # (INVFS_DEBUG_PACKS=1 above), and the streaming branch is the only
    # one that prints its marker, so "which admission decomposed this"
    # is a fact in a log rather than an inference from the sibling count.
    MARK=""
    for lf in "$BS/sweep.log" "$BS/fuse.log"; do
        if grep -q "streaming admission: 17179869184 bytes" "$lf" 2>/dev/null; then
            MARK="$MARK $(basename "$lf")"
        fi
    done
    if [ "$NM" = 202 ]; then
        ok "L4: decomposed by the container lane: 200 members + table + map"
        [ -n "$MARK" ] \
            || fail "L4: decomposed with no streaming-admission marker in either log -- neither the offline sweep nor the daemon stated which admission ran (see $BS/sweep.log, $BS/fuse.log)"
        ok "L4: admission was STREAMING (ranged pin + mmap):$MARK"
    else
        CLS=$($B/meta_probe "$BS/bigvol.img" --heat disk.ntfs 2>/dev/null \
            | awk '/^class=/ {print $1}');
        [ "$CLS" = "class=4" ] \
            || fail "L4: neither decomposed ($NM siblings) nor GENERIC ($CLS)"
        # A decline must say WHY: quote the lane's own line(s).
        echo "  note: lane decline reason from $BS/sweep.log:"
        # NO `| head` here, for the reason spelled out above the presweep
        # probe: head closes the pipe, grep dies of SIGPIPE, and pipefail
        # + set -e then ends the script with no FAIL line -- which is how
        # this leg once printed four OK lines and then exited 2 on a
        # syntax error nobody could see. Bound the output with sed on a
        # file instead.
        grep -iE '\[cpack\]|scratch|size guard|no lane claimed|not a gain|declin' \
            "$BS/sweep.log" > "$BS/decline.txt" 2>/dev/null || true
        sed -n '1,8p' "$BS/decline.txt" | sed 's/^/    /'
        [ -s "$BS/decline.txt" ] \
            || echo "    (the log names no lane line at all: see $BS/sweep.log)"
        # How far the lane got is a fact, not an inference: the marker is
        # printed by the streaming admission, so its presence means a 16 GiB
        # pin was streamed and the refusal came LATER (guards, scratch,
        # recipe). Its absence means the lane never reached the pin.
        if [ -n "$MARK" ]; then
            ok "L4: lane reached the 16 GiB container (streaming admission:$MARK) and refused after the pin"
        else
            ok "L4: lane never reached the pin (no streaming-admission marker)"
        fi
        ok "L4: generic floor ($CLS); lane declined, reason above"
    fi
    # NOTE: mount explicitly at $BS/mnt (the L3 helper mnt_up mounts at
    # the global $MNT -- using it here reads an empty dir, not the file).
    $B/invf-fuse "$BS/bigvol.img" "$BS/mnt" 2>"$BS/fuse-ro.log"
    for _ in $(seq 1 50); do
        grep -q " $BS/mnt " /proc/mounts && break
        sleep 0.1
    done
    grep -q " $BS/mnt " /proc/mounts || fail "L4 remount for readback"
    cmp "$BS/big16.img" "$BS/mnt/disk.ntfs" \
        || fail "L4 rebuilt image not bit-exact"
    ok "L4: 16 GiB rebuild bit-exact through the mounted read path, fsck OK"
    rm "$BS/mnt/disk.ntfs" || fail "L4 unlink through mount"
    fusermount3 -u "$BS/mnt" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $BS/mnt " /proc/mounts || break
        sleep 0.1
    done
    await_vol_free "$BS/bigvol.img" 120 || fail "L4 volume still locked"
    $B/invf-fsck "$BS/bigvol.img" | grep -q "^OK$" \
        || fail "L4 fsck after unlink not OK"
    ok "L4: unlink through mount reclaims, fsck OK"
fi

echo "PASS: test-ntfs-big (all legs green)"
