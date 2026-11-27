#!/bin/bash
# test-xfs-classify.sh — the xfs containerpack's `classify` command (GCFG table).
#
# `tools/codecpacks/xfs.codecpack` can decompose an XFS image into per-file
# members, and since WP-XFS-CLASSIFY it can also answer the question the core
# cannot answer for itself, because the core never parses the pack's recipe:
# of the bytes that ride verbatim in the recipe, which are STRUCTURE (a rebuild
# needs them) and which are GARBAGE (free space, AGFL blocks, unwritten
# extents, padding -- bytes no member needs and the filesystem does not need to
# rebuild itself)?  `xfs classify <image> <out>` emits that as a GCFG table.
#
# What this suite proves, and against WHAT:
#
#   * the tiling is exact -- off ascending, contiguous, sum(len) == image_size,
#     no zero-length entry, block_size stated in the header;
#   * every range the table calls GARBAGE really is FREE, per an independent
#     oracle: `xfs_db -r -c "freesp -d"`, which on this xfsprogs prints the
#     by-block btree's free extents AND the AGFL blocks (measured: the four
#     single-block extents per AG are exactly the four AGFL bno entries
#     `xfs_db -c "agfl N" -c "p"` prints, and the AGFL is where they come
#     from -- XFS removes them from the btree when it puts them on the list);
#   * the garbage set is also COMPLETE: per-AG garbage blocks == agf_freeblks,
#     so an image whose free space the walk missed fails instead of quietly
#     compressing worse;
#   * every MEMBER range lies in allocated space (member ∩ free == @);
#   * the structure claims hold: AG header blocks are structure, the internal
#     log is structure, anything past sb_dblocks is garbage;
#   * zero_verified is a MEASUREMENT: every flagged range is re-read here and
#     must be all zero, and the flags are then poked -- one byte is written
#     into a garbage block behind the pack's back, and the flag must flip to
#     "not zero" and the run must split. Restoring the byte must restore the
#     exact earlier table, which is what proves the flag tracks the bytes and
#     not the btree's opinion of them;
#   * an image the pack must NOT classify gets a non-zero exit and NO table
#     written (fail closed, never a partial or a guessed table).
#
# Fixtures. mkfs.xfs wants >= 300 MiB (measured: 6.19 refuses less), and a
# *populated* XFS image can only be made by mounting one, so each geometry is:
# truncate -> mkfs.xfs -> `mount -o loop` -> write a varied tree (sizes, a
# symlink, a hardlink pair, a nested directory) -> sync -> delete part of it so
# BOTH used and free ranges exist and the freed blocks keep real bytes -> sync
# -> umount -> classify the image file. The sync-before-delete is load
# bearing: measured here, a file created and removed inside the same dirty
# window never reaches the disk at all, so its "free" blocks read back as zeros
# and the zero-verified leg would pass by measuring nothing.
#
# Geometries (the knobs the brief names, and why some of them are REFUSALS):
#   v5-crc1-bigtime1-sparse1  the main leg: -m crc=1 -m bigtime=1 -i sparse=1
#                             plus -m reflink=0,rmapbt=0 -i nrext64=0,
#                             exchange=0 -n parent=0. Those opt-outs are not
#                             decoration: crc=1 alone still carries reflink and
#                             nrext64 from mkfs 6.19's defaults and the pack
#                             declines them, so they are measured as refusal
#                             legs (below) and the geometry they name is then
#                             exercised with them off.
#   v5-sparse0                -i sparse=0: the other inobt record shape.
#   v5-agcount2               -d agcount=2: different AG geometry/addressing.
#   v4                        -m crc=0 -i size=256: the v4 btree magics
#                             (ABTB/ABTC/IABT, 16 B headers) and 256 B inodes.
#   v5-sect4096               -s size=4096 when the loop device can be given a
#                             4096-byte sector size: the AG headers move from
#                             sector offsets to whole blocks, which is a
#                             different code path in the classifier. Skipped
#                             by name when the kernel/loop refuses.
#   empty                     a fresh mkfs image with no members at all:
#                             classify must tile it (free space + structure)
#                             instead of refusing "no regular files".
#   refuse-<reason>           default mkfs.xfs (reflink+nrext64), -m crc=1,
#                             external log, metadump, not-xfs: non-zero exit,
#                             no table.
#
# Run from the repo root after `make`:  bash tools/test-xfs-classify.sh
# Needs cc, python3, mkfs.xfs, xfs_db and sudo for the loop mounts (a plain
# file plus `xfs_db -r` needs no root, but POPULATING the filesystem does).
# No /dev/mapper device is touched and no other suite's mount is used.
set -e
set -o pipefail
export PATH="$PATH:/usr/sbin:/sbin"

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
export REPO
. "$REPO/tools/lib-scratch.sh"

PACK=$REPO/tools/codecpacks/xfs.codecpack
XFS=$PACK/bin/xfs
WORK=${INVFS_XFSCLS_WORK:-$(invfs_scratch_root)/wpxfscls-$$}
MNT=$WORK/mnt
MNTED=0
LODEV=""

say()  { echo "$*"; }
note() { echo "  $*"; }
bad()  { echo "FAIL: $*"; exit 1; }

cleanup() {
    local rc=$?
    if [ "$MNTED" = 1 ]; then
        sudo umount "$MNT" 2>/dev/null || sudo umount -l "$MNT" 2>/dev/null || true
        MNTED=0
    fi
    if [ -n "$LODEV" ]; then sudo losetup -d "$LODEV" 2>/dev/null || true; fi
    if [ "$rc" -eq 0 ]; then rm -rf "$WORK"; fi
    exit "$rc"
}
trap cleanup EXIT

# ---- capability gate -----------------------------------------------------
# A suite that cannot build its fixtures must SAY SO and exit 0; one that
# always fails has stopped reporting anything (same rule as test-xfs.sh and
# test-fuzz.sh). Every reason below is measured, not assumed.
missing=""
for t in cc python3 mkfs.xfs xfs_db truncate; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
if [ -n "$missing" ]; then
    say "xfs classify: SKIPPED (missing tools:$missing)"
    exit 0
fi
if ! sudo -n true 2>/dev/null; then
    say "xfs classify: SKIPPED (populating an XFS image needs a mount;" \
        "sudo is not available without a password)"
    exit 0
fi
if ! grep -qE '(^|[[:space:]])xfs$' /proc/filesystems 2>/dev/null; then
    say "xfs classify: SKIPPED (this kernel has no xfs filesystem support)"
    exit 0
fi

rm -rf "$WORK"
mkdir -p "$WORK/img" "$WORK/out" "$MNT"
cd "$WORK"

say "== build the pack (cc -std=c11 -O2 -Wall -Wextra -Werror) =="
mkdir -p "$PACK/bin"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$XFS" "$PACK/xfs.c"
note "pack binary built: ${XFS#$REPO/}"

# ---- the independent checker --------------------------------------------
cat > "$WORK/gcfg_check.py" <<'PY'
#!/usr/bin/env python3
"""gcfg_check.py — decide whether a GCFG table is HONEST, against xfs_db.

    gcfg_check.py <image> <table> --label NAME
                  [--expect-nonzero] [--extra-garbage-blocks N] [--dump]

The oracle is `xfs_db -r`, NOT the pack: the free set comes from the
by-block btree (plus the AGFL blocks, which xfs_db's freesp prints because
they are free too), and the classification is checked against it range by
range. Nothing here trusts the table's own claims.

Exit 1 on any mismatch, and every mismatch is PRINTED with its offset --
a count alone would not say which byte range is wrong, and a wrong `garbage`
range is data loss for whoever converts an image with this table.
"""
import os
import re
import struct
import subprocess
import sys

CLS = {0: 'meta', 1: 'garbage', 2: 'member'}
GCFG_CLS_META, GCFG_CLS_GARB, GCFG_CLS_MEMBER = 0, 1, 2
GCFG_F_ZERO = 0x01

fails = []


def fail(msg):
    fails.append(msg)
    print("  FAIL: " + msg)


def sh(*args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("oracle failed: %s\n%s" % (" ".join(args), r.stderr))
    return r.stdout


def xfs_db_fields(img, typ, fields, ag=None):
    cmd = ['xfs_db', '-r', '-c', ('%s %d' % (typ, ag)) if ag is not None else typ,
           '-c', 'p ' + ' '.join(fields), img]
    out = sh(*cmd)
    return {k: int(v) for k, v in re.findall(r'(\w+) = (\d+)', out)}


def load_table(path):
    data = open(path, 'rb').read()
    if len(data) < 16:
        fail("table is %d bytes, shorter than the 16-byte header" % len(data))
        return None
    magic, ver, n, bs = struct.unpack_from('<4sIII', data, 0)
    if magic != b'GCFG':
        fail("magic is %r, not GCFG" % magic)
        return None
    if ver != 1:
        fail("version is %d, not 1" % ver)
    if len(data) != 16 + 24 * n:
        fail("header says %d entries (%d bytes), file is %d bytes"
             % (n, 16 + 24 * n, len(data)))
    ents = []
    for i in range(n):
        o, l, c, f, r = struct.unpack_from('<QQBBH', data, 16 + 24 * i)
        ents.append((o, l, c, f, r))
    return bs, ents


def main():
    img, tab = sys.argv[1], sys.argv[2]
    label = sys.argv[sys.argv.index('--label') + 1] if '--label' in sys.argv else img
    expect_nonzero = '--expect-nonzero' in sys.argv
    dump = '--dump' in sys.argv
    member_bytes = None
    if '--member-bytes' in sys.argv:
        member_bytes = int(sys.argv[sys.argv.index('--member-bytes') + 1])
    extra = 0
    if '--extra-garbage-blocks' in sys.argv:
        extra = int(sys.argv[sys.argv.index('--extra-garbage-blocks') + 1])

    size = os.stat(img).st_size
    sb = xfs_db_fields(img, 'sb 0', ['dblocks', 'agblocks', 'agcount',
                                     'blocksize', 'logstart', 'logblocks',
                                     'agblklog'])
    dblocks, agblocks, agcount = sb['dblocks'], sb['agblocks'], sb['agcount']
    bs, logstart, logblocks, agblklog = (sb['blocksize'], sb['logstart'],
                                         sb['logblocks'], sb['agblklog'])
    print("  image %s: %d B, %d blocks of %d, agcount %d x %d, log %d+%d"
          % (os.path.basename(img), size, dblocks, bs, agcount, agblocks,
             logstart, logblocks))

    loaded = load_table(tab)
    if loaded is None:
        return 1
    hdr_bs, ents = loaded
    n = len(ents)
    if not n:
        fail("table holds no entries")

    # ---- 1. tiling -------------------------------------------------------
    pos = 0
    for i, (o, l, c, f, r) in enumerate(ents):
        if o != pos:
            fail("entry %d starts at %d, the previous entry ended at %d "
                 "(%s)" % (i, o, pos, "gap" if o > pos else "overlap"))
        if l == 0:
            fail("entry %d at %d has zero length" % (i, o))
        if c not in CLS:
            fail("entry %d at %d has class %d" % (i, o, c))
        if r != 0:
            fail("entry %d at %d has rsvd %d (must be zero)" % (i, o, r))
        if f & ~GCFG_F_ZERO:
            fail("entry %d at %d has unknown flags 0x%02x" % (i, o, f))
        pos = o + l
    if pos != size:
        fail("table covers %d of %d image bytes" % (pos, size))
    print("  tiling: %d entries, [0, %d), ascending and contiguous: %s"
          % (n, pos, "yes" if pos == size else "NO"))

    # ---- 2. the oracle: free space per xfs_db ---------------------------
    freesp = sh('xfs_db', '-r', '-c', 'freesp -d', img)
    free = []                       # linear block ranges
    for line in freesp.splitlines():
        m = re.match(r'\s*(\d+)\s+(\d+)\s+(\d+)\s*$', line)
        if m:
            ag, agb, ln = (int(x) for x in m.groups())
            free.append((ag * agblocks + agb, ln))
    if not free:
        fail("the oracle (xfs_db freesp -d) returned no free extents; this "
             "xfsprogs prints them differently than this parser expects")
    # The oracle checks itself against two numbers the classifier never sees:
    # the AGF's freeblks COUNT, and the AGF's AGFL window length. Measured on
    # this xfsprogs: freesp -d lists the by-block btree's free extents AND the
    # AGFL blocks (they are free too -- XFS removes them from the btree when it
    # puts them on the list), while agf_freeblks counts the btree only. The
    # four 1-block extents per AG on a fresh image are exactly the AGFL bno[]
    # entries, so `freesp == freeblks + flcount` is the identity that proves the
    # parse above is reading what it thinks it is reading.
    per_ag_free = [0] * agcount
    for o, l in free:
        for ag in range(agcount):
            lo = ag * agblocks
            hi = min(dblocks, lo + agblocks)
            s, e = max(o, lo), min(o + l, hi)
            if e > s:
                per_ag_free[ag] += e - s
    agf = [xfs_db_fields(img, 'agf', ['freeblks', 'flcount'], ag=ag)
           for ag in range(agcount)]
    agf_free = [a['freeblks'] for a in agf]
    agf_fl = [a['flcount'] for a in agf]
    if per_ag_free != [f + n for f, n in zip(agf_free, agf_fl)]:
        fail("oracle self-check: freesp -d per AG %s, agf_freeblks+flcount %s"
             % (per_ag_free, [f + n for f, n in zip(agf_free, agf_fl)]))
    print("  oracle: %d free extents, %d blocks == agf_freeblks %s + AGFL %s"
          % (len(free), sum(per_ag_free), agf_free, agf_fl))

    starts = [o for o, _ in free]
    import bisect

    def is_free(block):
        i = bisect.bisect_right(starts, block) - 1
        return i >= 0 and block < free[i][0] + free[i][1]

    # ---- 3. garbage ⊆ free, member ∩ free == 0, per-AG completeness -----
    garbage_blocks = [0] * agcount
    nbad = 0
    notfree = []                    # garbage blocks the oracle does not call free
    for o, l, c, f, r in ents:
        b0 = o // bs
        b1 = (o + l - 1) // bs
        for b in range(b0, b1 + 1):
            free_b = is_free(b) if b < dblocks else False
            if c == GCFG_CLS_GARB:
                if b < dblocks:
                    ag = min(b // agblocks, agcount - 1)
                    garbage_blocks[ag] += 1
                if b < dblocks and not free_b:
                    notfree.append(b)
            elif c == GCFG_CLS_MEMBER:
                if free_b:
                    nbad += 1
                    if nbad <= 8:
                        fail("member block %d (AG %d block %d) is FREE"
                             % (b, b // agblocks, b % agblocks))
    if nbad > 8:
        fail("... %d member blocks in total are free space" % nbad)
    # A garbage block the oracle does not call free is only legitimate as an
    # unwritten extent (free by construction: unwritten, no logical content,
    # kept out of the member partition), and the fixture declares how many
    # blocks that is. Anything else is the failure this suite exists to catch.
    if not extra and notfree:
        for b in notfree[:8]:
            fail("garbage block %d (AG %d block %d) is NOT in any free extent "
                 "the by-block btree reports" % (b, b // agblocks, b % agblocks))
        if len(notfree) > 8:
            fail("... %d garbage blocks in total are not free space"
                 % len(notfree))
    if extra and len(notfree) != extra:
        for b in notfree[:8]:
            fail("garbage block %d (AG %d block %d) is neither free space nor "
                 "part of the %d-block unwritten extent"
                 % (b, b // agblocks, b % agblocks, extra))
        fail("garbage blocks outside the free set: %d, expected exactly the "
             "%d-block unwritten extent" % (len(notfree), extra))
    # The per-AG completeness equality only holds when the garbage set IS the
    # free set; with an unwritten extent in the fixture it is free + that run.
    if not extra and garbage_blocks != per_ag_free:
        fail("garbage set is not complete: per-AG garbage blocks %s, the "
             "oracle's free space %s (difference %s)"
             % (garbage_blocks, per_ag_free,
                [a - b for a, b in zip(garbage_blocks, per_ag_free)]))
    if extra:
        if sum(garbage_blocks) != sum(per_ag_free) + extra:
            fail("garbage blocks %d != free %d + %d unwritten"
                 % (sum(garbage_blocks), sum(per_ag_free), extra))
        print("  garbage == free (%d) + the %d-block unwritten extent, and the "
              "%d blocks outside the free set are exactly that extent"
              % (sum(per_ag_free), extra, len(notfree)))
    else:
        print("  garbage set: %d blocks, == the oracle's per-AG free space: %s"
              % (sum(garbage_blocks), garbage_blocks))

    # ---- 4. structure: AG headers, the log, past-dblocks padding --------
    def cls_at(off):
        lo, hi = 0, len(ents) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            o, l, c, f, r = ents[mid]
            if off < o:
                hi = mid - 1
            elif off >= o + l:
                lo = mid + 1
            else:
                return c
        return None

    for ag in range(agcount):
        c = cls_at(ag * agblocks * bs)
        if c != GCFG_CLS_META:
            fail("AG %d block 0 (the superblock/AG header block) is class %s, "
                 "not meta_structure" % (ag, CLS.get(c, c)))
    lagno = logstart >> agblklog
    lagbno = logstart & ((1 << agblklog) - 1)
    for b in range(lagbno, min(lagbno + logblocks, agblocks)):
        c = cls_at((lagno * agblocks + b) * bs)
        if c != GCFG_CLS_META:
            fail("the internal log block AG %d block %d is class %s, not "
                 "meta_structure" % (lagno, b, CLS.get(c, c)))
            break
    if size > dblocks * bs:
        for off in range(dblocks * bs, size, bs):
            if cls_at(off) != GCFG_CLS_GARB:
                fail("padding past sb_dblocks at %d is class %s, not garbage"
                     % (off, CLS.get(cls_at(off), cls_at(off))))
                break
    print("  structure: AG header blocks and the internal log are meta, "
          "past-dblocks padding is garbage")

    # ---- 5. zero_verified is a MEASUREMENT ------------------------------
    fh = open(img, 'rb')
    zflag = zbytes = nzflag = nzbytes = 0
    for o, l, c, f, r in ents:
        if c != GCFG_CLS_GARB:
            continue
        fh.seek(o)
        blob = fh.read(l)
        if len(blob) != l:
            fail("short read at %d" % o)
            break
        allzero = blob.count(0) == l
        if f & GCFG_F_ZERO:
            zflag += 1
            zbytes += l
            if not allzero:
                nz = min(i for i, x in enumerate(blob) if x)
                fail("entry at %d len %d is flagged zero_verified but byte %d "
                     "is 0x%02x" % (o, l, nz, blob[nz]))
        else:
            nzflag += 1
            nzbytes += l
            if allzero:
                fail("entry at %d len %d is NOT flagged zero_verified but "
                     "reads back all zero" % (o, l))
    fh.close()
    print("  zero_verified: %d flagged entries (%d B) re-read all zero, "
          "%d unflagged (%d B) hold non-zero bytes"
          % (zflag, zbytes, nzflag, nzbytes))
    if expect_nonzero and nzbytes == 0:
        fail("this fixture deleted real data, so free space must hold non-zero "
             "bytes; every garbage range reads back as zeros, which means the "
             "zero leg measured nothing")

    # ---- 6. summary -----------------------------------------------------
    by = {0: 0, 1: 0, 2: 0}
    for o, l, c, f, r in ents:
        by[c] += l
    if member_bytes is not None and by[2] != member_bytes:
        fail("member ranges cover %d bytes, the fixture's regular files hold "
             "%d" % (by[2], member_bytes))
    if member_bytes is not None:
        print("  members: %d bytes == the fixture's file sizes (%d)"
              % (by[2], member_bytes))
    print("  table: %d entries, meta %d B, garbage %d B (zero-verified %d B), "
          "member %d B, image %d B"
          % (n, by[0], by[1], zbytes, by[2], size))
    if dump:
        for i, (o, l, c, f, r) in enumerate(ents):
            print("    %3d %-8s off=%-12d len=%-10d blk=%d..%d flags=%d"
                  % (i, CLS[c], o, l, o // bs, (o + l - 1) // bs, f))
    if fails:
        print("  %d FAILURES for %s" % (len(fails), label))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
PY

cat > "$WORK/gcfg_poke.py" <<'PY'
#!/usr/bin/env python3
"""Poke one byte into a garbage block (or restore it) and name the block.

    gcfg_poke.py <image> <table> poke   -> writes 0x5a into the first byte of
                                           the largest zero-verified garbage
                                           block range; prints block index
    gcfg_poke.py <image> <table> restore -> writes the saved byte back
"""
import os
import struct
import sys

img, tab, mode = sys.argv[1], sys.argv[2], sys.argv[3]
savef = img + ".poked"

if mode == 'restore':
    block, byte = [int(x) for x in open(savef).read().split()]
    with open(img, 'r+b') as f:
        f.seek(block * 4096)
        f.write(bytes([byte]))
    os.unlink(savef)
    print(block)
    sys.exit(0)

d = open(tab, 'rb').read()
n = struct.unpack_from('<I', d, 8)[0]
bs = struct.unpack_from('<I', d, 12)[0]
best = None
for i in range(n):
    o, l, c, f, r = struct.unpack_from('<QQBBH', d, 16 + 24 * i)
    if c != 1 or not (f & 1) or l < bs:
        continue
    if best is None or l > best[1]:
        best = (o, l)
if best is None:
    sys.exit("no zero-verified garbage range large enough to poke")
block = best[0] // bs
mid = best[0] + (best[1] // 2 // bs) * bs      # a block in the middle
block = mid // bs
with open(img, 'r+b') as f:
    f.seek(block * bs)
    old = f.read(1)[0]
    f.seek(block * bs)
    f.write(b'\x5a')
open(savef, 'w').write("%d %d\n" % (block, old))
print("%d %d %d" % (block, best[0], best[1]))
PY

# ---- fixture plumbing ----------------------------------------------------
IMGS=()
# an unwritten extent that every populated fixture carries: 512 blocks, so the
# garbage set must exceed the free space by exactly 512 blocks (checked below).
UNWRITTEN_BYTES=$((512 * 4096))
UNWRITTEN_BLOCKS=512

# populate_img <mountpoint>: a varied tree, synced, part of it then deleted.
populate_img() {
    local m=$1
    mkdir -p "$m/d1/d2" "$m/tree"
    local i
    for i in 1 2 3 4 5 6 7 8; do
        head -c $((i * 1300)) /dev/urandom > "$m/d1/f$i.bin"
    done
    head -c 9000 /dev/urandom > "$m/d1/d2/deep.bin"
    dd if=/dev/urandom of="$m/big.bin" bs=1M count=6 2>/dev/null
    ln "$m/big.bin" "$m/big.hard"
    ln -s big.bin "$m/sym.link"
    printf 'classify me\n' > "$m/note.txt"
    for i in $(seq 1 24); do
        head -c $((i * 4096)) /dev/urandom > "$m/tree/t$i.bin"
    done
    # An UNWRITTEN extent (fallocate): the member reads zeros, the disk bytes are
    # undefined, and the pack keeps them in the recipe -- so the classifier must
    # call them garbage without calling them zero-verified unless they are. This
    # is the one thing that makes the garbage set larger than the free space.
    fallocate -l "$UNWRITTEN_BYTES" "$m/unwritten.bin" 2>/dev/null \
        || dd if=/dev/zero of="$m/unwritten.bin" bs=4096 count=0 2>/dev/null
    sync
    # Sync BEFORE the deletes: measured here, a file created and removed inside
    # one dirty window never reaches the disk, so its blocks would read back as
    # zeros and the zero_verified leg would measure nothing.
    rm -f "$m/d1/f2.bin" "$m/d1/f5.bin"
    rm -rf "$m/tree"
    sync
}

# mount_img <image> [extra mount options]
mount_img() {
    local img=$1 opts=${2:-loop}
    sudo mount -o "$opts" "$img" "$MNT" || return 1
    MNTED=1
    # the fixture is written as the invoking user, so the mount root has to
    # belong to it (the filesystem's own root is root-owned after mkfs)
    sudo chown "$(id -u):$(id -g)" "$MNT"
    return 0
}

unmount_img() {
    sudo umount "$MNT" || bad "umount $MNT"
    MNTED=0
}

# mkimg <name> <bytes> <mkfs args...> [-- populate]
# Sets the global IMG (not stdout: a failure inside a command substitution
# would exit a subshell and take the message with it).
IMG=""
mkimg() {
    local name=$1 size=$2
    shift 2
    local populate=0
    if [ "${1:-}" = "--populate" ]; then populate=1; shift; fi
    local img=$WORK/img/$name.img
    IMG=$img
    IMGS+=("$img")
    truncate -s "$size" "$img"
    mkfs.xfs -f "$@" "$img" > "$WORK/out/$name.mkfs" 2>&1 \
        || { cat "$WORK/out/$name.mkfs"; bad "mkfs.xfs $* failed"; }
    if [ "$populate" = 1 ]; then
        mount_img "$img" || bad "mount -o loop $img"
        populate_img "$MNT"
        unmount_img
    fi
}

# geom <image>: the geometry the checker and the report both quote, straight
# from xfs_db (not from the pack).
geom() {
    xfs_db -r -c "sb 0" \
        -c 'p blocksize dblocks agblocks agcount logstart logblocks sectsize inodesize features_ro_compat features_incompat' \
        "$1" 2>&1 | tr '\n' ' ' | sed 's/  */ /g'
}

# classify <image> <tag>  -> writes $WORK/out/<tag>.cls, dies on a non-zero exit
classify_ok() {
    local img=$1 tag=$2
    if ! "$XFS" classify "$img" "$WORK/out/$tag.cls" \
            > "$WORK/out/$tag.log" 2>&1; then
        cat "$WORK/out/$tag.log"
        bad "classify refused $tag"
    fi
}

check() {
    local img=$1 tag=$2
    shift 2
    python3 "$WORK/gcfg_check.py" "$img" "$WORK/out/$tag.cls" --label "$tag" "$@" \
        || bad "table for $tag is not honest (see above)"
}

# refuse <image> <tag> <expected-reason-regex>
refuse() {
    local img=$1 tag=$2 want=$3
    local out=$WORK/out/$tag.cls
    rm -f "$out"
    set +e
    "$XFS" classify "$img" "$out" > "$WORK/out/$tag.log" 2>&1
    local rc=$?
    set -e
    local why
    why=$(tr -d '\r' < "$WORK/out/$tag.log" | tail -1)
    [ "$rc" -ne 0 ] || bad "$tag: classify ACCEPTED an image it must refuse"
    [ ! -s "$out" ] || bad "$tag: classify wrote a table and still exited $rc"
    grep -qiE "$want" "$WORK/out/$tag.log" \
        || bad "$tag: refusal does not name what it refused (want /$want/, got: $why)"
    note "$tag: exit $rc, no table, said: ${why#xfs: }"
}

# ---- 1. the main leg: v5, crc=1, bigtime=1, sparse=1 --------------------
say "== fixture 1: v5 crc=1 bigtime=1 sparse=1 (320 MiB, populated) =="
mkimg v5-crc1-bigtime1-sparse1 320M --populate \
    -m crc=1,bigtime=1,finobt=1,inobtcount=1,reflink=0,rmapbt=0 \
    -i sparse=1,size=512,nrext64=0,exchange=0 -n ftype=1,parent=0
IMG1N="v5-crc1-bigtime1-sparse1"
# fixture 1's image is referenced again by the metadump leg below
MAIN=$IMG
note "mkfs: $(tr '\n' ' ' < "$WORK/out/v5-crc1-bigtime1-sparse1.mkfs" | sed 's/  */ /g' | cut -c1-160)"
note "geometry: $(geom "$IMG")"
classify_ok "$IMG" v5-crc1-bigtime1-sparse1
check "$IMG" v5-crc1-bigtime1-sparse1 --expect-nonzero \n    --extra-garbage-blocks "$UNWRITTEN_BLOCKS"

say "== the zero_verified flag tracks the BYTES, not the btree =="
read -r PBLK POFF PLEN < <(python3 "$WORK/gcfg_poke.py" "$IMG" \
    "$WORK/out/v5-crc1-bigtime1-sparse1.cls" poke)
note "wrote 0x5a into garbage block $PBLK (inside the $PLEN-byte zero run at $POFF)"
"$XFS" classify "$IMG" "$WORK/out/poked.cls" > "$WORK/out/poked.log" 2>&1 \
    || bad "classify failed after the poke"
python3 - "$WORK/out/v5-crc1-bigtime1-sparse1.cls" "$WORK/out/poked.cls" "$PBLK" <<'PY' \
    || bad "the zero_verified flag did not follow the byte that was poked"
import struct, sys
a, b, blk = sys.argv[1], sys.argv[2], int(sys.argv[3])

def load(p):
    d = open(p, 'rb').read()
    n = struct.unpack_from('<I', d, 8)[0]
    return [struct.unpack_from('<QQBBH', d, 16 + 24 * i) for i in range(n)]

def zero_bytes(t):
    return sum(l for o, l, c, f, r in t if c == 1 and (f & 1))

pre, post = load(a), load(b)
off = blk * 4096

def at(t, pos):
    for e in t:
        if e[0] <= pos < e[0] + e[1]:
            return e
    return None

e0, e1 = at(pre, off), at(post, off)
if e1 is None:
    sys.exit("the poked block is no longer covered by any entry")
print("   before: entry off=%d len=%d flags=%d" % (e0[0], e0[1], e0[3]))
print("   after : entry off=%d len=%d flags=%d" % (e1[0], e1[1], e1[3]))
if e1[3] & 1:
    sys.exit("still flagged zero after a non-zero byte was written into it")
if e1[0] != off or e1[1] != 4096:
    sys.exit("the poked block is not its own 4096-byte entry -- the run did "
             "not split around the byte that changed")
if zero_bytes(post) != zero_bytes(pre) - 4096:
    sys.exit("zero-verified bytes went %d -> %d, expected exactly one block "
             "less" % (zero_bytes(pre), zero_bytes(post)))
if len(post) != len(pre) + 2:
    sys.exit("a poked block in the MIDDLE of a run must split it into three "
             "(before|block|after): %d entries before, %d after"
             % (len(pre), len(post)))
neigh = [at(post, off - 4096), at(post, off + 4096)]
for n in neigh:
    if n is None or not (n[3] & 1):
        sys.exit("a neighbour of the poked block stopped being zero-verified")
PY
note "flag flipped and the run split at the poked block; now restore it"
python3 "$WORK/gcfg_poke.py" "$IMG" "$WORK/out/poked.cls" restore >/dev/null
"$XFS" classify "$IMG" "$WORK/out/restored.cls" > "$WORK/out/restored.log" 2>&1 \
    || bad "classify failed after restoring the byte"
cmp -s "$WORK/out/v5-crc1-bigtime1-sparse1.cls" "$WORK/out/restored.cls" \
    || bad "restoring the byte did not restore the table -- the flags follow " \
           "something other than the image content"
note "restoring the byte restored the exact earlier table"

# ---- 2. sparse=0 --------------------------------------------------------
say "== fixture 2: v5 -i sparse=0 (the other inobt record shape) =="
mkimg v5-sparse0 320M --populate \
    -m crc=1,bigtime=1,finobt=1,inobtcount=1,reflink=0,rmapbt=0 \
    -i sparse=0,size=512,nrext64=0,exchange=0 -n ftype=1,parent=0
classify_ok "$IMG" v5-sparse0
check "$IMG" v5-sparse0 --expect-nonzero \n    --extra-garbage-blocks "$UNWRITTEN_BLOCKS"

# ---- 3. agcount=2 -------------------------------------------------------
say "== fixture 3: v5 -d agcount=2 =="
mkimg v5-agcount2 320M --populate \
    -d agcount=2 -m crc=1,bigtime=1,finobt=1,inobtcount=1,reflink=0,rmapbt=0 \
    -i sparse=1,size=512,nrext64=0,exchange=0 -n ftype=1,parent=0
classify_ok "$IMG" v5-agcount2
check "$IMG" v5-agcount2 --expect-nonzero \n    --extra-garbage-blocks "$UNWRITTEN_BLOCKS"

# ---- 4. v4 (crc=0, 256 B inodes) ---------------------------------------
say "== fixture 4: v4 -m crc=0 -i size=256 (v4 btree magics) =="
# v4: no crc, 256-byte inodes, the v4 (16-byte header) btree magics ABTB /
# ABTC / IABT. -n size=2 is refused by mkfs 6.19 ("Invalid value 2 for -n size
# option. Value is too small") and sparse inodes do not exist without crc=1, so
# neither is asked for.
#
# This one is populated WITHOUT a mount, with `mkfs.xfs -p <dir>` (populate
# from directory), because this kernel cannot mount it at all:
#   XFS (loop2): Deprecated V4 format (crc=0) not supported by kernel.
#   /proc/config.gz: # CONFIG_XFS_SUPPORT_V4 is not set
# so `mount -o loop` on a v4 image fails with "wrong fs type, bad option, bad
# superblock". mkfs -p needs no root and lands the file data on disk, which is
# enough for the classifier (members, free space, the v4 btree shapes); what it
# cannot produce is free space holding DELETED data, so this leg does not
# demand non-zero garbage.
rm -rf "$WORK/pdir"
mkdir -p "$WORK/pdir/sub"
for i in 1 2 3; do head -c $((i * 2000)) /dev/urandom > "$WORK/pdir/f$i.bin"; done
head -c 5000 /dev/urandom > "$WORK/pdir/sub/deep.bin"
printf 'hi\n' > "$WORK/pdir/note.txt"
PB=$(find "$WORK/pdir" -type f -printf '%s\n' | awk '{s+=$1} END{print s}')
mkimg v4-crc0-256 320M -m crc=0 -i size=256 -n ftype=1 -p "$WORK/pdir"
note "geometry: $(geom "$IMG")"
note "members must cover exactly $PB bytes ($(find "$WORK/pdir" -type f | wc -l) regular files, no mount: CONFIG_XFS_SUPPORT_V4 is off)"
classify_ok "$IMG" v4-crc0-256
check "$IMG" v4-crc0-256 --member-bytes "$PB"

# ---- 5. 4 KiB sectors (if the loop device will take them) --------------
# The AG headers move from sector offsets to whole blocks when the sector is a
# block, which is a different path in the classifier, so it is worth a leg. A
# plain `mount -o loop` cannot do it -- mount hands the unknown option to XFS
# ("fsconfig() failed: xfs: Unknown parameter 'sector-size'") -- so the loop
# device is set up explicitly and cleaned up below.
say "== fixture 5: v5 -s size=4096 (AG headers on block boundaries) =="
IMG5=$WORK/img/v5-sect4096.img
IMGS+=("$IMG5")
truncate -s 320M "$IMG5"
if mkfs.xfs -q -f -s size=4096 \
        -m crc=1,bigtime=1,finobt=1,inobtcount=1,reflink=0,rmapbt=0 \
        -i sparse=1,size=512,nrext64=0,exchange=0 -n ftype=1,parent=0 \
        "$IMG5" > "$WORK/out/v5-sect4096.mkfs" 2>&1; then
    LODEV=$(sudo losetup --find --show --sector-size 4096 "$IMG5" 2>/dev/null) || LODEV=""
    if [ -n "$LODEV" ] && sudo mount "$LODEV" "$MNT" 2>"$WORK/out/v5-sect4096.mount"; then
        MNTED=1
        sudo chown "$(id -u):$(id -g)" "$MNT"
        populate_img "$MNT"
        unmount_img
        sudo losetup -d "$LODEV"
        LODEV=""
        note "geometry: $(geom "$IMG5")"
        classify_ok "$IMG5" v5-sect4096
        check "$IMG5" v5-sect4096 --expect-nonzero \n            --extra-garbage-blocks "$UNWRITTEN_BLOCKS"
    else
        [ -n "$LODEV" ] && { sudo losetup -d "$LODEV"; LODEV=""; }
        say "  NOTE: skipping the 4096-byte-sector leg -- this kernel refused to"
        say "        mount it: $(tr '\n' ' ' < "$WORK/out/v5-sect4096.mount" 2>/dev/null)"
        say "        The AG-header addressing for that case is therefore NOT"
        say "        measured on this host."
        rm -f "$IMG5"
    fi
else
    say "  NOTE: skipping the 4096-byte-sector leg -- mkfs.xfs refused"
    say "        -s size=4096 on a plain file: $(tr '\n' ' ' < "$WORK/out/v5-sect4096.mkfs")"
    rm -f "$IMG5"
fi

# ---- 6. an empty filesystem (no members at all) ------------------------
say "== fixture 6: a fresh mkfs image with no members =="
mkimg v5-empty 320M \
    -m crc=1,bigtime=1,finobt=1,inobtcount=1,reflink=0,rmapbt=0 \
    -i sparse=1,size=512,nrext64=0,exchange=0 -n ftype=1,parent=0
classify_ok "$IMG" v5-empty
check "$IMG" v5-empty

# ---- 6b. padding past sb_dblocks, and an image SHORTER than the fs ------
# An image file may be larger than the filesystem it holds; those bytes belong
# to nobody, so they are garbage. And an image shorter than sb_dblocks is a
# contradiction: the pack must refuse rather than classify the part it can see.
say "== a truncated and a padded copy of the empty image =="
cp --sparse=always "$IMG" "$WORK/img/padded.img"
IMGS+=("$WORK/img/padded.img")
truncate -s $((320 * 1024 * 1024 + 8192)) "$WORK/img/padded.img"
classify_ok "$WORK/img/padded.img" v5-padded
note "the 8192 bytes past sb_dblocks must be garbage, and they are:"
check "$WORK/img/padded.img" v5-padded
cp --sparse=always "$IMG" "$WORK/img/short.img"
IMGS+=("$WORK/img/short.img")
truncate -s $((320 * 1024 * 1024 - 1024 * 1024)) "$WORK/img/short.img"
refuse "$WORK/img/short.img" short 'exceeds the image size|dblocks'

# ---- 7. refusals -------------------------------------------------------
say "== refusals: non-zero exit, no table =="
# mkfs 6.19 defaults carry reflink (ro-compat) and nrext64 (incompat); the
# pack declines both, by name.
mkimg refuse-default 320M >/dev/null
refuse "$WORK/img/refuse-default.img" refuse-default 'reflink|rmapbt'
mkimg refuse-crc1 320M -m crc=1 >/dev/null
refuse "$WORK/img/refuse-crc1.img" refuse-crc1 'reflink|rmapbt'
# external log: sb_logstart == 0, which the classifier refuses explicitly
truncate -s 64M "$WORK/img/extlog.log"
mkimg refuse-extlog 320M -l logdev="$WORK/img/extlog.log",size=64m \
    -m reflink=0,rmapbt=0 -i nrext64=0,exchange=0 -n parent=0 >/dev/null
refuse "$WORK/img/refuse-extlog.img" refuse-extlog 'external log|logstart'
# a 2048-byte block size: this pack is 4096-only (a v1 restriction, named in
# its header), so classify must say so instead of walking a filesystem whose
# geometry it cannot address.
mkimg refuse-bsize2048 320M -b size=2048 -m reflink=0,rmapbt=0 \
    -i nrext64=0,exchange=0 -n parent=0 >/dev/null
refuse "$IMG" refuse-bsize2048 'blocksize 2048|not supported'
# a realtime section: sb_rblocks != 0, which every tool in this pack refuses
# because neither the walk nor the classifier looks at the realtime device.
truncate -s 64M "$WORK/img/rtdev.img"
RTIMG=$WORK/img/refuse-realtime.img
IMGS+=("$RTIMG")
truncate -s 320M "$RTIMG"
if mkfs.xfs -f -d file,name="$RTIMG",size=320m \
        -r rtdev="$WORK/img/rtdev.img",size=64m \
        > "$WORK/out/refuse-realtime.mkfs" 2>&1; then
    note "realtime: $(xfs_db -r -c 'sb 0' -c 'p rblocks rextents' "$RTIMG" | tr '\n' ' ')"
    refuse "$RTIMG" refuse-realtime 'realtime'
else
    note "realtime: SKIPPED, mkfs.xfs refused to build the fixture: " \
         "$(tr '\n' ' ' < "$WORK/out/refuse-realtime.mkfs" | tail -c 200)"
fi
# not an XFS image at all
head -c 1M /dev/urandom > "$WORK/img/notxfs.img"
IMGS+=("$WORK/img/notxfs.img")
refuse "$WORK/img/notxfs.img" notxfs 'not XFS'
# a metadump: `xfs_metadump` writes a dump, not the filesystem, and it starts
# with the metadump magic "XFSM" (measured: the first four bytes of the dump
# are XFSM), so the pack declines it as "not XFS" instead of classifying a
# byte layout that is not the image's. Non-zero exit, no table.
if command -v xfs_metadump >/dev/null 2>&1; then
    if xfs_metadump "$MAIN" "$WORK/img/dump.md" > "$WORK/out/metadump.log" 2>&1; then
        IMGS+=("$WORK/img/dump.md")
        note "metadump: $(stat -c %s "$WORK/img/dump.md") B, first bytes " \
             "$(head -c 4 "$WORK/img/dump.md")"
        refuse "$WORK/img/dump.md" metadump 'not XFS'
    else
        note "metadump: xfs_metadump refused the fixture " \
             "($(tr -d '\r' < "$WORK/out/metadump.log" | tail -1))"
    fi
else
    note "metadump: SKIPPED, xfs_metadump is not installed on this host"
fi

echo "XFS CLASSIFY E2E: PASS"
