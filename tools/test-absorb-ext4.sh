#!/bin/bash
# test-absorb-ext4.sh — WP-JOURNAL: an absorbed ext4 image must open, which
# means the pack's `classify` map must call the JOURNAL meta_structure.
#
# Why this suite exists. `invf-mkfs --absorb` adopts an existing filesystem in
# place: it asks the filesystem's pack for the used-byte map (`classify`, GCFG
# v1), reserves everything that is not garbage, and publishes the old image as
# one file (`old.ext4`) inside the new volume. The ext4fs pack walked inodes
# from s_first_ino, so the RESERVED inodes were never read -- fs->meta never
# covered the journal (inode 8) or the resize inode (7) and `classify` called
# both of them GARBAGE. The published image then had a zero journal:
#
#     $ e2fsck -fn old.ext4
#     Superblock has an invalid journal (inode 8).
#     Journal superblock is corrupt.
#     e2fsck: Cannot proceed with file system check
#
# What is asserted, in order, per image:
#   A  every block of inode 8 (journal) and of inode 7 (resize inode, a
#      block-map inode whose blocks are the reserved-GDT backups) lies inside a
#      4096-block the map reserves. THIS is the regression gate: it fails on the
#      pack before the fix with the exact block list, without needing a volume.
#   B  the whole absorb route is exercised for real -- `invf-absorb plan` ->
#      `invf-mkfs --reserve` -> `invf-absorb publish` -> `invf-cat` -- and
#      `e2fsck -fn` on the published file must pass all five passes with no
#      error and no "WARNING: Filesystem still has errors".
#   C  RED LEG: zeroing ONLY inode 8's blocks in a copy of the source must make
#      e2fsck refuse the image, so (B) is a measurement and not a rubber stamp.
#   D  the map's promise, exactly: the published bytes are the source's bytes on
#      every reserved 4096-block and all-zero everywhere else. This is what
#      catches a reserve list that rounds a range's tail away (measured: the
#      orphan file's last 1 KiB block of a 1 KiB-block image).
#
# Geometries: mkfs.ext4 defaults (1 KiB blocks: ranges are 1024-aligned inside
# 4096-byte reserve blocks, so the rounding in (D) matters) and -b 4096 (ranges
# already aligned).
#
# No FUSE, no loop device, no root: images are files, every step is offline.
# blkio treats /dev/* paths as raw devices, so this cd's into /dev/shm and uses
# RELATIVE names, like the other image suites.
#
# Run: bash tools/run-e2e.sh tools/test-absorb-ext4.sh
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
export REPO
B=${INVFS_BIN:-$REPO/bin}
export PATH="$B:$PATH"
SRCPACK=$REPO/tools/codecpacks/ext4fs.codecpack
W=${INVFS_ABSORB_WORK:-/dev/shm/wp-absorb-e4-$$}
trap 'rm -rf "$W"' EXIT
mkdir -p "$W"
cd "$W"

echo "== tools =="
for t in mkfs.ext4 e2fsck debugfs python3; do
    command -v "$t" >/dev/null || { echo "FAIL: $t not installed"; exit 1; }
done
for t in invf-mkfs invf-absorb invf-cat; do
    [ -x "$B/$t" ] || { echo "FAIL: $B/$t missing (run make)"; exit 1; }
done

# ---------------------------------------------------------------------------
# The pack helper under test, built from the tree's SOURCE into $W (never into
# the tree: tools/test-ext4fs.sh rebuilds that path in place and this suite can
# run concurrently with it).
# ---------------------------------------------------------------------------
PACKS=$W/packs
mkdir -p "$PACKS/ext4fs.codecpack/bin"
cp "$SRCPACK/manifest" "$PACKS/ext4fs.codecpack/manifest"
echo "== build pack helper (cc -O2 -Wall -Wextra -Werror) =="
WARN=$(cc -std=c11 -O2 -Wall -Wextra -Werror \
        -o "$PACKS/ext4fs.codecpack/bin/ext4fs" "$SRCPACK/ext4fs.c" 2>&1) \
    || { echo "FAIL: pack build failed"; echo "$WARN"; exit 1; }
[ -z "$WARN" ] || { echo "FAIL: pack build not warning-clean:"; echo "$WARN"; exit 1; }
E4=$PACKS/ext4fs.codecpack/bin/ext4fs
export INVFS_PACKS=$PACKS
echo "helper built clean"

# ---------------------------------------------------------------------------
# GCFG helpers (little-endian): "GCFG", u32 ver, u32 n, u32 block_size, then
# n x 24 B {u64 off; u64 len; u8 cls; u8 flags; u16 rsvd}; cls 0 meta_structure,
# 1 garbage, 2 member.
# ---------------------------------------------------------------------------
cat > $W/gcfg.py <<'PY'
import struct, sys

BS_RESERVE = 4096          # invf-mkfs reserve granularity (INVFS_BLOCK_SIZE)

def table(path):
    d = open(path, "rb").read()
    assert d[:4] == b"GCFG", "no GCFG magic in " + path
    ver, n, bs = struct.unpack_from("<III", d, 4)
    assert ver == 1, "GCFG version %d" % ver
    out = []
    for i in range(n):
        off, ln = struct.unpack_from("<QQ", d, 16 + i * 24)
        cls, flags = d[16 + i * 24 + 16], d[16 + i * 24 + 17]
        out.append((off, ln, cls, flags))
    return out, bs

def reserved(path):
    """The 4096-byte blocks absorb's reserve list covers -- the same rounding
    src/cli/absorb.c writes: start DOWN, count measured from that start."""
    tab, _ = table(path)
    blocks = set()
    for off, ln, cls, _f in tab:
        if cls == 1 or ln == 0:
            continue
        start = off // BS_RESERVE
        n = (off % BS_RESERVE + ln + BS_RESERVE - 1) // BS_RESERVE
        for b in range(start, start + n):
            blocks.add(b)
    return blocks, tab

def table_size_hint(img_path):
    """ext4 block size from the superblock at 1024 (log block size at 0x18)."""
    with open(img_path, "rb") as f:
        f.seek(1024 + 0x18)
        log_bs = struct.unpack("<I", f.read(4))[0]
    return 1024 << log_bs

def tiling(path, image_size):
    """Entries must tile [0, image_size) exactly: no gap, no overlap. A map
    that does not tile cannot answer 'what is this byte' for every byte."""
    tab, _ = table(path)
    pos = 0
    for off, ln, cls, _f in tab:
        if off != pos:
            sys.exit("FAIL: %s: entry at %d leaves a hole/overlap at %d"
                     % (path, off, pos))
        pos = off + ln
    if pos != image_size:
        sys.exit("FAIL: %s: entries end at %d, image is %d bytes"
                 % (path, pos, image_size))

if sys.argv[1] == "tiling":
    tiling(sys.argv[2], int(sys.argv[3]))

elif sys.argv[1] == "reserved-inode":
    # blocks of a reserved inode (filesystem block numbers) -> every one must
    # sit inside a 4096-block the map reserves. Blocks per 4096-byte reserve
    # unit come from the table's own block_size (1 KiB image: 4 per unit).
    gcfg, blocks_file, osize = sys.argv[2], sys.argv[3], int(sys.argv[4])
    res, tab = reserved(gcfg)
    _, bs = table(gcfg)
    tiling(gcfg, osize)
    seen = 0
    miss = []
    for tok in open(blocks_file).read().split():
        if not tok.isdigit():
            continue
        seen += 1
        if (int(tok) * bs) // BS_RESERVE not in res:
            miss.append(int(tok))
    if not seen:
        sys.exit("FAIL: no blocks recorded for the checked inode(s)")
    if miss:
        sys.exit("FAIL: %d of %d inode blocks are GARBAGE in the map "
                 "(first: %s)" % (len(miss), seen, miss[:8]))
    print("    %d inode block(s), every one inside a reserved 4096-block"
          % seen)

elif sys.argv[1] == "reserve-list":
    # The plan's list must cover every 4096-block a non-garbage range touches.
    # Rounding the start down and then measuring the count from the range's own
    # length drops the TAIL block of any range that is not 4096-aligned, which
    # is every range of a 1 KiB or 2 KiB ext4 image; this is the oracle for
    # that arithmetic, so it fails even when the lost block happens to be
    # covered by the next range on a particular fixture.
    gcfg, listfile = sys.argv[2], sys.argv[3]
    res, tab = reserved(gcfg)
    got = set()
    for line in open(listfile):
        f = line.split()
        if len(f) == 2:
            s0, c0 = int(f[0]), int(f[1])
            got.update(range(s0, s0 + c0))
    miss = sorted(res - got)
    if miss:
        sys.exit("FAIL: %s leaves %d of %d needed 4096-block(s) unreserved "
                 "(first: %s)" % (listfile, len(miss), len(res),
                                  [(b, b * BS_RESERVE) for b in miss[:6]]))
    print("    reserve list covers all %d needed 4096-block(s) "
          "(%d listed)" % (len(res), len(got)))

elif sys.argv[1] == "mapcheck":
    # published bytes == source bytes on reserved blocks, zero elsewhere
    gcfg, src, got = sys.argv[2], sys.argv[3], sys.argv[4]
    s = open(src, "rb").read()
    g = open(got, "rb").read()
    if len(s) != len(g):
        sys.exit("FAIL: published %d bytes, source %d" % (len(g), len(s)))
    res, tab = reserved(gcfg)
    tiling(gcfg, len(s))
    nz = rot = 0
    for b in range((len(s) + BS_RESERVE - 1) // BS_RESERVE):
        lo, hi = b * BS_RESERVE, min((b + 1) * BS_RESERVE, len(s))
        if b in res:
            if s[lo:hi] != g[lo:hi]:
                sys.exit("FAIL: reserved 4096-block %d differs (offset %d)"
                         % (b, lo))
        else:
            nz += 1
            if s[lo:hi] != bytes(hi - lo):
                rot += 1
            if g[lo:hi] != bytes(hi - lo):
                sys.exit("FAIL: garbage 4096-block %d is not zero in the "
                         "published image (offset %d)" % (b, lo))
    # A fixture whose free space is entirely zero would read back identical to
    # the source even if the map had called nothing garbage, so say which case
    # this was instead of pretending the comparison proved more than it did.
    print("    %d reserved / %d garbage 4096-byte blocks: reserved bytes are "
          "the source's, garbage reads zero (%d garbage block(s) held non-zero "
          "bytes in the source, dropped as the map says)"
          % (len(res), nz, rot))

elif sys.argv[1] == "zero-inode":
    # red-leg fixture: zero the blocks of ONE inode, exactly what a map that
    # calls that inode garbage produces
    src, blocks_file, out = sys.argv[2], sys.argv[3], sys.argv[4]
    img = bytearray(open(src, "rb").read())
    bs = table_size_hint(src)
    n = 0
    for tok in open(blocks_file).read().split():
        if tok.isdigit():
            b = int(tok) * bs
            img[b:b + bs] = bytes(bs)
            n += 1
    if not n:
        sys.exit("FAIL: zero-inode: no blocks in %s" % blocks_file)
    open(out, "wb").write(img)
    print("    zeroed %d block(s) of %s" % (n, blocks_file))
else:
    sys.exit("FAIL: gcfg.py: unknown command " + sys.argv[1])
PY

# ---------------------------------------------------------------------------
# Fixtures. debugfs writes the tree without a mount; e2fsck -fy makes the image
# clean so that `e2fsck -fn` on the absorbed copy is a real comparison.
# ---------------------------------------------------------------------------
head -c 300000 /dev/urandom > f300k.bin
head -c 1400000 /dev/zero | tr '\0' 'j' > f14m.txt
printf 'journal fixture\n' > small.txt

mk_image() {             # $1 name, $2 size, $3.. mkfs args
    local img=$1 size=$2
    shift 2
    rm -f "$img"
    truncate -s "$size" "$img"
    mkfs.ext4 -q -F "$@" "$img"
    debugfs -w -R "mkdir /dir" "$img" >/dev/null 2>&1 || true
    debugfs -w -R "write f300k.bin /dir/f300k.bin" "$img" >/dev/null 2>&1
    debugfs -w -R "write f14m.txt /f14m.txt" "$img" >/dev/null 2>&1
    debugfs -w -R "write small.txt /small.txt" "$img" >/dev/null 2>&1
    debugfs -w -R "ln /dir/f300k.bin /hardlink.bin" "$img" >/dev/null 2>&1
    e2fsck -fy "$img" >/dev/null 2>&1 || true
    if ! e2fsck -fn "$img" >/dev/null 2>&1; then
        echo "FAIL: fixture $img is not clean before the absorb"
        exit 1
    fi
}

echo "== fixtures =="
mk_image img1k.ext4 64M                    # mkfs defaults: 1 KiB blocks
mk_image img4k.ext4 96M -b 4096            # 4 KiB blocks
ls -l img1k.ext4 img4k.ext4 | awk '{printf "    %-12s %s bytes\n", $9, $5}'

fair() { grep -qE 'invalid journal|Journal superblock is corrupt|WARNING: Filesystem still has errors' "$1"; }

for img in img1k.ext4 img4k.ext4; do
    st=${img%.ext4}
    echo
    echo "===== $img ====="
    "$E4" classify "$img" "$st.gcfg"
    osize=$(stat -c %s "$img")

    # --- A: the journal and the resize inode are STRUCTURE, not garbage ------
    debugfs -R "blocks <8>" "$img" 2>/dev/null | tr ' ' '\n' \
        | grep -E '^[0-9]+$' > "$st.journal.blocks"
    python3 $W/gcfg.py reserved-inode "$st.gcfg" "$st.journal.blocks" "$osize" \
        | sed "s/^/    journal (inode 8):/"
    debugfs -R "blocks <7>" "$img" 2>/dev/null | tr ' ' '\n' \
        | grep -E '^[0-9]+$' > "$st.resize.blocks"
    if [ -s "$st.resize.blocks" ]; then
        python3 $W/gcfg.py reserved-inode "$st.gcfg" "$st.resize.blocks" \
            "$osize" | sed "s/^/    resize inode (7):/"
    else
        echo "    resize inode (7): not in use on this image"
    fi

    # --- B: plan -> mkfs --reserve -> publish -> read back -> e2fsck ----------
    # The volume is 3x the source: `publish` COPIES the source's bytes into
    # framed RAW segments (a range cannot be named in place -- see absorb.h),
    # so it needs free space for them. A 1:1 volume has none left after its
    # own metadata zone: measured "no room for 257 more blocks".
    rm -f "$st.res.list" "$st.vol.img" "$st.got.ext4"
    invf-absorb plan "$img" "$st.res.list" > "$st.plan.log" 2>&1 \
        || { echo "FAIL: invf-absorb plan"; cat "$st.plan.log"; exit 1; }
    sed -n 's/^/    /p' "$st.plan.log"
    python3 $W/gcfg.py reserve-list "$st.gcfg" "$st.res.list" | sed 's/^/    /'
    mkfs_log=$("$B/invf-mkfs" "$st.vol.img" --size-bytes "$((osize * 3))" \
                    --reserve "$st.res.list") \
        || { echo "FAIL: invf-mkfs --reserve"; exit 1; }
    prefix=$(echo "$mkfs_log" |
             sed -n 's/.*absorb prefix: *blocks 0 \.\. \([0-9][0-9]*\).*/\1/p')
    [ -n "$prefix" ] || { echo "FAIL: mkfs printed no absorb prefix"; exit 1; }
    prefix=$((prefix + 1))
    echo "    absorb prefix: $prefix blocks (from mkfs)"
    invf-absorb publish "$st.vol.img" old.ext4 "$img" "$st.res.list" "$prefix" \
        > "$st.publish.log" 2>&1 \
        || { echo "FAIL: invf-absorb publish"; cat "$st.publish.log"; exit 1; }
    grep -q published "$st.publish.log" || {
        echo "FAIL: publish did not report a file"; cat "$st.publish.log"; exit 1; }

    # The published file is read back through the ENGINE. This is the shape
    # absorb publishes (holes for garbage) and the shape OPEN-ISSUES measured;
    # `invf-absorb verify` cannot pass it yet (F6 refuses a recipe with holes),
    # which is a separate finding, not this suite's subject.
    invf-cat "$st.vol.img" old.ext4 > "$st.got.ext4" 2>"$st.cat.log" \
        || { echo "FAIL: invf-cat old.ext4"; cat "$st.cat.log"; exit 1; }
    got=$(stat -c %s "$st.got.ext4")
    [ "$got" = "$osize" ] || { echo "FAIL: read back $got of $osize bytes"; exit 1; }
    e2fsck -fn "$st.got.ext4" > "$st.fsck.log" 2>&1 || true
    sed -n 's/^/    /p' "$st.fsck.log"
    if fair "$st.fsck.log"; then
        echo "FAIL: e2fsck refuses the absorbed image"
        exit 1
    fi
    echo "    PASS: e2fsck -fn opened the absorbed $img"

    # --- C: red leg -- the journal's own bytes are what made that pass ------
    python3 $W/gcfg.py zero-inode "$img" "$st.journal.blocks" "$st.ctl.ext4"
    e2fsck -fn "$st.ctl.ext4" > "$st.ctl.log" 2>&1 || true
    grep -q 'invalid journal' "$st.ctl.log" || {
        echo "FAIL: red leg -- zeroing inode 8's blocks did NOT make e2fsck"
        echo "      complain, so the acceptance above proves nothing"
        sed -n 's/^/    /p' "$st.ctl.log"
        exit 1
    }
    echo "    PASS: red leg -- zeroing the journal makes e2fsck refuse it"

    # --- D: the map's promise, byte for byte --------------------------------
    python3 $W/gcfg.py mapcheck "$st.gcfg" "$img" "$st.got.ext4" | sed 's/^/    /'
done

echo
echo "absorb-ext4: PASS (journal + resize inode are structure; the absorbed"
echo "images open; the published bytes follow the map)"
