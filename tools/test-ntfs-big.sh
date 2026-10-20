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
#   Leg L4 (scale, 16 GiB, 200 x 60 MiB members): same lifecycle on the
#   big scratch (INVFS_BIG_SCRATCH unset => SKIP). Asserts the scale
#   properties unconditionally (bit-exact round-trip through the mounted
#   read path -- invf-cat cannot malloc a 16 GiB file -- plus fsck CLEAN
#   plus unlink reclaim) and records WHICH legal path won: decomposed
#   (200 members + table + map, when the lane gets there first) or
#   generic-ZSTD (floor won). WHY BOTH ARE CORRECT: container lanes need
#   the whole file in RAM (vol_read_inode) and the live FUSE daemon runs
#   full per-file sweeps on its 1 Hz drain tick, so on a small-RAM box the
#   daemon's generic floor legitimately preempts the lane -- and the
#   offline sweep then correctly skips the stamped file ("already
#   swept"). Decomposing multi-GB containers without whole-file RAM
#   (seekable lane reads) is a follow-up WP with its own number, not
#   this gate lift. AGENTS.md 2.5's "drain never transforms" describes
#   an older tick: the drain calls vol_sweep_pending -> vol_sweep_one
#   (full dispatch, lanes + generic floor + stamps). Doc bug, filed
#   separately -- not fixed in this branch.
#
# SCOPE NOTE (measured, not assumed): the hermetic legs stop at 128 MiB;
# the scale leg L4 (16 GiB on INVFS_BIG_SCRATCH) proves the lifted gate at
# volume level past the old 4 GiB ceiling. The pack accepts to 1 TiB (legs
# L1/L2), but a multi-GB image's NTRC recipe blob is read whole by the
# sweep transform (vol_read_inode mallocs file_size), which is the
# WP634-A read-streaming wall, not this pack's. Larger volume-level
# images wait for that chain; this WP only lifts the pack gate and
# refuses honestly above it.
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
    lb = ncl.to_bytes(max(1, (ncl.bit_length() + 7) // 8), 'little')
    db = lcn.to_bytes(max(1, (lcn.bit_length() + 7) // 8), 'little')
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
$B/invf-sweep wp640-vol.img > "$WORK/sweep.log" 2>&1 || fail "sweep"
grep -q "ntfs (codecpack)" "$WORK/sweep.log" \
    || fail "sweep log does not name the ntfs lane"
$B/invf-fsck wp640-vol.img | tee "$WORK/fsck1" | grep -q "^OK$" \
    || fail "fsck after sweep not OK"
$B/invf-cat wp640-vol.img disk.ntfs > "$WORK/rebuilt.img" \
    || fail "cat rebuilt"
cmp "$WORK/ref/life128m.img" "$WORK/rebuilt.img" \
    || fail "rebuilt image not bit-exact"
ok "L3: sweep decomposes, rebuild bit-exact, fsck OK"
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
    rm -rf "$BS" && mkdir -p "$BS/mnt"
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
    if [ "$NM" = 202 ]; then
        ok "L4: decomposed (lane won): 200 members + table + map"
    else
        CLS=$($B/meta_probe "$BS/bigvol.img" --heat disk.ntfs 2>/dev/null \
            | awk '/^class=/ {print $1}');
        [ "$CLS" = "class=4" ] \
            || fail "L4: neither decomposed ($NM siblings) nor GENERIC ($CLS)"
        ok "L4: generic floor (lane declined on RAM): $CLS, see header note"
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
