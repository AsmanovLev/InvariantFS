#!/bin/bash
# test-ext4fs.sh — ext4fs containerpack (algo 17, WP16a/WP16b) end-to-end
# (persistent regression).
#
#   fixtures (mkfs.ext4 + debugfs -w; htree legs via e2fsck -D):
#     fsA.ext4 — 64 MB, 4K blocks: busybox .c files at several paths
#       (/src/*, /deep/a/b/c/d/e/f/leaf.c), one ELF (/bin/busybox.elf),
#       a sparse file (8 MB logical / 1 MB written), a hardlink pair
#       (one inode, two dir entries), an empty file, a fallocate'd file
#       (unwritten extent inside i_size), and a 6 MB multi-extent file
#       (~800 extents over a depth-1 extent tree, via free-space
#       fragmentation). Plus the always-present orphan_file inode.
#     fsB.ext4 — 48 MB, 1K blocks (-b 1024 variant): .c files, ELF,
#       hardlink pair, sparse file (4 MB logical / 512 KB written),
#       empty file, deep tree, 2 MB multi-extent file.
#     fs2k.ext4 — 16 MB, 2K blocks (block-size matrix, hand legs only).
#     ht0.ext4 / ht1.ext4 — htree directories (e2fsck -D), indirect
#       levels 0 and 1 (hand legs only: index blocks are metadata).
#     plain.ext4 — a TEXT file named .ext4: the pack must decline (exit 3)
#       and the file flows to text/generic, never decomposed.
#     refuse battery: encrypt / bigalloc / inline_data / casefold / verity
#       feature images, an ext2 image (no extents), garbage, truncated —
#       all must decline with exit 3.
#
#   The hand self-test (enumerate -> extract xN -> strip -> rebuild -> cmp
#   bit-exact -> map validate + map-splice simulation) runs on fsA/fsB/
#   fs2k/ht0/ht1 BEFORE any InvariantFS e2e.
#
#   FS e2e: mkfs -> invf-import -> INVFS_CODECPACKS=$REPO/tools/codecpacks
#   invf-sweep -> "ext4fs (codecpack)" lines + same-run member batching
#   (PPMd / ZSTD) -> class stamps CONTAINER{17,1} / TEXT{PPMD,1} /
#   BATCHED_BIN{ZSTD_BCJ,1} -> verify --deep -> invf-cat sha256 bit-exact
#   -> direct member reads bit-exact -> ranged reads (mid-member /
#   cross-boundary / tail / whole-by-windows) -> pack-ABSENT reads via the
#   self-describing map -> idempotent re-sweep -> delete cascade -> fsck
#   clean. Admission leg: INVFS_ARC_BYTES=1M -> GENERIC_MEMLIMIT{17,1},
#   bit-exact. Ratio demo: pack volume vs generic-only control volume.
#
# Run from the repo root after `make`:  bash tools/test-ext4fs.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
PACKDIR=$REPO/tools/codecpacks/ext4fs.codecpack
E4=$PACKDIR/bin/ext4fs
WORK=/dev/shm/wp16ext4
trap 'rm -rf "$WORK" /dev/shm/wp16ext4*.img' EXIT
IMG=wp16ext4.img
IMGCTL=wp16ext4-ctl.img
IMGMEM=wp16ext4-mem.img
export INVFS_CODECPACKS=$REPO/tools/codecpacks   # the sweep AND the reads
rm -rf "$WORK" && mkdir -p "$WORK/stage" "$WORK/out" "$WORK/nopacks" \
    "$WORK/incoming" "$WORK/expect"
cd /dev/shm
rm -f "$IMG" "$IMGCTL" "$IMGMEM"

echo "== tools =="
command -v python3   >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
command -v mkfs.ext4 >/dev/null || { echo "FAIL: mkfs.ext4 not installed"; exit 1; }
command -v debugfs   >/dev/null || { echo "FAIL: debugfs not installed"; exit 1; }
command -v e2fsck    >/dev/null || { echo "FAIL: e2fsck not installed"; exit 1; }

echo "== build pack helper (cc -O2 -Wall -Wextra -Werror) =="
mkdir -p "$PACKDIR/bin"
WARN=$(cc -std=c11 -O2 -Wall -Wextra -Werror -o "$E4" "$PACKDIR/ext4fs.c" 2>&1) \
    || { echo "FAIL: pack build failed"; echo "$WARN"; exit 1; }
[ -z "$WARN" ] || { echo "FAIL: pack build not warning-clean:"; echo "$WARN"; exit 1; }
echo "helper built clean (-Wall -Wextra -Werror)"

echo "== build FS-side helpers (classof / rngread / cbrm) =="
cat > "$WORK/classof.c" <<'C'
/* classof.c — print the WP10 storage-class stamp of one file.
 * usage: classof <image> <name>  ->  "cls=<n> algo=<n> gen=<n>" or "none" */
#include <stdio.h>
#include "volume.h"
#include "invarifs.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    uint64_t id;
    uint8_t cls, algo;
    uint16_t gen;

    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) return 1;
    id = vol_find(v, argv[2]);
    if (!id) { vol_close(v); return 1; }
    if (vol_get_class(v, id, &cls, &algo, &gen) != 0)
        printf("none\n");
    else
        printf("cls=%u algo=%u gen=%u\n", cls, algo, gen);
    vol_close(v);
    return 0;
}
C
cat > "$WORK/rngread.c" <<'C'
/* rngread.c — ranged reads through vol_read_range (the WP16b local-splice
 * read path for a seekable container).
 * usage: rngread <image> <name> <off> <len> <out> [chunk]
 *   len < 0 reads the whole file in <chunk>-byte windows (default 65536). */
#include <stdio.h>
#include <stdlib.h>
#include "volume.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    uint64_t id, off, fsz = 0;
    long long len;
    uint64_t chunk;
    FILE *o;
    uint8_t *buf;
    int rc = 1;

    if (argc < 6) return 2;
    v = vol_open(argv[1], &err);
    if (!v) return 1;
    id = vol_find(v, argv[2]);
    if (!id) { fprintf(stderr, "not found: %s\n", argv[2]); goto out; }
    off = strtoull(argv[3], NULL, 0);
    len = strtoll(argv[4], NULL, 0);
    chunk = (argc > 6) ? strtoull(argv[6], NULL, 0) : 65536;
    if (!chunk) chunk = 65536;
    o = fopen(argv[5], "wb");
    if (!o) goto out;
    buf = (uint8_t *)malloc(chunk);
    if (!buf) { fclose(o); goto out; }
    if (vol_stat_full(v, argv[2], NULL, &fsz, NULL) != 0) goto free_out;
    if (len >= 0) {
        if (off + (uint64_t)len > fsz) len = (long long)(fsz > off ? fsz - off : 0);
        fsz = off + (uint64_t)len;
    }
    while (off < fsz) {
        uint64_t want = fsz - off < chunk ? fsz - off : chunk;
        int got = vol_read_range(v, id, off, (size_t)want, buf);
        if (got <= 0) { fprintf(stderr, "read failed at %llu\n",
                                (unsigned long long)off); goto free_out; }
        if (fwrite(buf, 1, (size_t)got, o) != (size_t)got) goto free_out;
        off += (uint64_t)got;
    }
    rc = 0;
free_out:
    free(buf);
    fclose(o);
out:
    vol_close(v);
    return rc;
}
C
cat > "$WORK/cbrm.c" <<'C'
#include <stdio.h>
#include "volume.h"
int main(int argc, char **argv)
{
    int err = 0, rc = 0;
    invfs_volume *v;
    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    for (int i = 2; i < argc; i++)
        if (vol_unlink(v, argv[i]) != 0) { rc = 1; fprintf(stderr, "rm %s failed\n", argv[i]); }
    vol_flush(v);
    vol_close(v);
    return rc;
}
C
CORE_O="$REPO/build/obj/volume.o $REPO/build/obj/vol_cpack.o $REPO/build/obj/vol_png.o $REPO/build/obj/vol_seal.o $REPO/build/obj/vol_repair.o $REPO/build/obj/vol_rollback.o $REPO/build/obj/vol_resize.o $REPO/build/obj/vol_fsck.o $REPO/build/obj/vol_crash.o $REPO/build/obj/vol_exer.o $REPO/build/obj/vol_dedupe.o $REPO/build/obj/vol_textzone.o $REPO/build/obj/vol_heat.o $REPO/build/obj/vol_sweep.o $REPO/build/obj/vol_meta_merge.o $REPO/build/obj/vol_read.o $REPO/build/obj/vol_write.o $REPO/build/obj/vol_records.o $REPO/build/obj/vol_ast.o $REPO/build/obj/vol_dirs.o $REPO/build/obj/arc.o $REPO/build/obj/crc32c.o $REPO/build/obj/lz4.o $REPO/build/obj/blkio.o $REPO/build/obj/flacx.o $REPO/build/obj/tarx.o $REPO/build/obj/pngx.o $REPO/build/obj/miniz.o $REPO/build/obj/ppmd8.o $REPO/build/obj/ppmd8enc.o $REPO/build/obj/ppmd8dec.o $REPO/build/obj/ppmd_codec.o $REPO/build/obj/codec.o $REPO/build/obj/bcj_x86.o $REPO/build/obj/blake3.o $REPO/build/obj/blake3_dispatch.o $REPO/build/obj/blake3_portable.o $REPO/build/obj/rs.o $REPO/build/obj/vol_tier.o"
for h in classof rngread cbrm; do
    gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/$h" "$WORK/$h.c" \
        $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
done

echo "== generate staging content =="
BB=$REPO/tools/busybox-src
for f in coreutils/cat.c coreutils/cp.c coreutils/ls.c archival/tar.c \
         editors/vi.c networking/ping.c; do
    [ -f "$BB/$f" ] || { echo "FAIL: missing busybox source $f"; exit 1; }
    cp "$BB/$f" "$WORK/stage/"
done
cp "$REPO/bin/busybox-static" "$WORK/stage/busybox.elf"
[ "$(head -c4 "$WORK/stage/busybox.elf")" = $'\x7fELF' ] \
    || { echo "FAIL: busybox-static is not an ELF"; exit 1; }
python3 - "$WORK/stage" <<'PY'
import os, random, sys
d = sys.argv[1]
rnd = random.Random(7)
open(os.path.join(d, "big.bin"), "wb").write(rnd.randbytes(6 * 1024 * 1024))
open(os.path.join(d, "big2.bin"), "wb").write(rnd.randbytes(2 * 1024 * 1024))
open(os.path.join(d, "head1m.bin"), "wb").write(rnd.randbytes(1024 * 1024))
open(os.path.join(d, "head512k.bin"), "wb").write(rnd.randbytes(512 * 1024))
open(os.path.join(d, "head256k.bin"), "wb").write(rnd.randbytes(256 * 1024))
open(os.path.join(d, "hl.txt"), "wb").write(b"hardlinked content\n" * 100)
open(os.path.join(d, "frag1k.bin"), "wb").write(rnd.randbytes(1024))
open(os.path.join(d, "frag4k.bin"), "wb").write(rnd.randbytes(4096))
open(os.path.join(d, "empty.bin"), "wb").write(b"")
# the refused text file that merely carries an .ext4 name
open(os.path.join(d, "plain.ext4"), "wb").write(
    b"/* not a filesystem, just text with an .ext4 name */\n" * 1000)
PY
ls -la "$WORK/stage" | tail -8

# ---- fixture builders -------------------------------------------------
# populate <image> <cmdfile>; every write target's parent must exist first.
dbg() { debugfs -w -f "$2" "$1" >/dev/null 2>&1 || { echo "FAIL: debugfs $1"; exit 1; }; }

# record_ino <image> <path> -> prints the inode number
record_ino() {
    debugfs -R "stat $2" "$1" 2>/dev/null | sed -n 's/^Inode: \([0-9]*\).*/\1/p' | head -1
}

# build_manifest <tag> <image> <path...>: appends "tag path ino" lines and
# dumps each member's expected content via debugfs (independent extractor)
build_manifest() {
    local tag=$1 img=$2; shift 2
    local p ino
    for p in "$@"; do
        ino=$(record_ino "$img" "$p")
        [ -n "$ino" ] || { echo "FAIL: no inode for $p in $img"; exit 1; }
        echo "$tag $p $ino" >> "$WORK/manifest"
        debugfs -R "dump $p $WORK/expect/$tag-$ino" "$img" >/dev/null 2>&1 \
            || { echo "FAIL: debugfs dump $p"; exit 1; }
    done
}

echo "== fixture fsA.ext4 (64 MB, 4K blocks) =="
F=$WORK/incoming/fsA.ext4
dd if=/dev/zero of="$F" bs=1M count=64 status=none
mkfs.ext4 -q -F -b 4096 "$F"
# FIRST fragment free space into alternating single-block holes (write
# 3000 one-block files, remove the even ones), so the 6 MB file written
# next (a low freed inode -> allocation goal inside the fragmented region)
# lands as ~1500 extents over a deep extent tree; content files follow.
python3 -c "
print('mkdir /frag')
for i in range(3000): print('write $WORK/stage/frag4k.bin /frag/g%04d' % i)
for i in range(0, 3000, 2): print('rm /frag/g%04d' % i)
" > "$WORK/frag.cmds"
dbg "$F" "$WORK/frag.cmds"
debugfs -w -R "write $WORK/stage/big.bin /big.bin" "$F" >/dev/null 2>&1 \
    || { echo "FAIL: write big.bin"; exit 1; }
cat > "$WORK/fsA.cmds" <<EOF
mkdir /src
write $WORK/stage/cat.c /src/cat.c
write $WORK/stage/cp.c /src/cp.c
write $WORK/stage/ls.c /src/ls.c
write $WORK/stage/tar.c /src/tar.c
write $WORK/stage/vi.c /src/vi.c
write $WORK/stage/ping.c /src/ping.c
mkdir /bin
write $WORK/stage/busybox.elf /bin/busybox.elf
mkdir /deep
mkdir /deep/a
mkdir /deep/a/b
mkdir /deep/a/b/c
mkdir /deep/a/b/c/d
mkdir /deep/a/b/c/d/e
write $WORK/stage/cat.c /deep/a/b/c/d/e/leaf.c
mkdir /hl
write $WORK/stage/hl.txt /hl/alpha.txt
link /hl/alpha.txt /hl/beta.txt
write $WORK/stage/empty.bin /empty.bin
write $WORK/stage/head512k.bin /falloc.bin
fallocate /falloc.bin 128 512
sif /falloc.bin size 2097152
write $WORK/stage/head1m.bin /sparse8m.bin
sif /sparse8m.bin size 8388608
EOF
dbg "$F" "$WORK/fsA.cmds"
python3 -c "
for i in range(1, 3000, 2): print('rm /frag/g%04d' % i)
" > "$WORK/fragrm.cmds"
dbg "$F" "$WORK/fragrm.cmds"
# debugfs's link(1) does not maintain i_links_count; repair the image so
# the fixture is a clean, fsck-consistent filesystem
rc=0
e2fsck -fy "$F" > "$WORK/fsA.fix" 2>&1 || rc=$?
[ $rc -le 1 ] || { echo "FAIL: e2fsck -fy fsA rc=$rc"; cat "$WORK/fsA.fix"; exit 1; }
NEXT=$(debugfs -R "dump_extents /big.bin" "$F" 2>/dev/null | grep -cE "^ *[0-9]+/ *[0-9]+" || true)
echo "  big.bin: $NEXT raw extents (multi-extent fixture)"
[ "$NEXT" -ge 8 ] || { echo "FAIL: big.bin not multi-extent ($NEXT)"; exit 1; }
e2fsck -fn "$F" > "$WORK/fsA.fsck" 2>&1 || { echo "FAIL: fsA not clean"; cat "$WORK/fsA.fsck"; exit 1; }
dumpe2fs -h "$F" 2>/dev/null | grep -q "state: *clean" \
    || { echo "FAIL: fsA state not clean"; exit 1; }
build_manifest fsA "$F" /src/cat.c /src/cp.c /src/ls.c /src/tar.c \
    /src/vi.c /src/ping.c /bin/busybox.elf /deep/a/b/c/d/e/leaf.c \
    /hl/alpha.txt /empty.bin /falloc.bin /sparse8m.bin /big.bin
# the orphan_file inode: a regular file too; dump via <ino>
OINO=$(dumpe2fs -h "$F" 2>/dev/null | sed -n 's/.*Orphan file inode: *\([0-9]*\).*/\1/p' | head -1)
if [ -n "$OINO" ]; then
    echo "fsA <orphan> $OINO" >> "$WORK/manifest"
    debugfs -R "dump <$OINO> $WORK/expect/fsA-$OINO" "$F" >/dev/null 2>&1
    echo "  orphan_file inode rides as member $OINO"
fi
echo "  fsA members: $(grep -c '^fsA ' "$WORK/manifest")"

echo "== fixture fsB.ext4 (48 MB, 1K blocks) =="
F=$WORK/incoming/fsB.ext4
dd if=/dev/zero of="$F" bs=1M count=48 status=none
mkfs.ext4 -q -F -b 1024 "$F"
python3 -c "
print('mkdir /frag')
for i in range(2400): print('write $WORK/stage/frag1k.bin /frag/h%04d' % i)
for i in range(0, 2400, 2): print('rm /frag/h%04d' % i)
" > "$WORK/fragB.cmds"
dbg "$F" "$WORK/fragB.cmds"
debugfs -w -R "write $WORK/stage/big2.bin /big.bin" "$F" >/dev/null 2>&1 \
    || { echo "FAIL: write fsB big.bin"; exit 1; }
cat > "$WORK/fsB.cmds" <<EOF
mkdir /src
write $WORK/stage/vi.c /src/vi.c
write $WORK/stage/ping.c /src/ping.c
mkdir /bin
write $WORK/stage/busybox.elf /bin/busybox.elf
mkdir /deep
mkdir /deep/a
mkdir /deep/a/b
mkdir /deep/a/b/c
mkdir /deep/a/b/c/d
write $WORK/stage/cp.c /deep/a/b/c/d/leaf.c
mkdir /hl
write $WORK/stage/hl.txt /hl/alpha.txt
link /hl/alpha.txt /hl/beta.txt
write $WORK/stage/empty.bin /empty.bin
write $WORK/stage/head256k.bin /sparse4m.bin
sif /sparse4m.bin size 4194304
EOF
dbg "$F" "$WORK/fsB.cmds"
python3 -c "
for i in range(1, 2400, 2): print('rm /frag/h%04d' % i)
" > "$WORK/fragrmB.cmds"
dbg "$F" "$WORK/fragrmB.cmds"
rc=0
e2fsck -fy "$F" > "$WORK/fsB.fix" 2>&1 || rc=$?
[ $rc -le 1 ] || { echo "FAIL: e2fsck -fy fsB rc=$rc"; cat "$WORK/fsB.fix"; exit 1; }
NEXTB=$(debugfs -R "dump_extents /big.bin" "$F" 2>/dev/null | grep -cE "^ *[0-9]+/ *[0-9]+" || true)
echo "  big.bin (fsB): $NEXTB raw extents"
[ "$NEXTB" -ge 8 ] || { echo "FAIL: fsB big.bin not multi-extent ($NEXTB)"; exit 1; }
e2fsck -fn "$F" > "$WORK/fsB.fsck" 2>&1 || { echo "FAIL: fsB not clean"; cat "$WORK/fsB.fsck"; exit 1; }
build_manifest fsB "$F" /src/vi.c /src/ping.c /bin/busybox.elf \
    /deep/a/b/c/d/leaf.c /hl/alpha.txt /empty.bin /sparse4m.bin /big.bin
OINO=$(dumpe2fs -h "$F" 2>/dev/null | sed -n 's/.*Orphan file inode: *\([0-9]*\).*/\1/p' | head -1)
if [ -n "$OINO" ]; then
    echo "fsB <orphan> $OINO" >> "$WORK/manifest"
    debugfs -R "dump <$OINO> $WORK/expect/fsB-$OINO" "$F" >/dev/null 2>&1
fi
echo "  fsB members: $(grep -c '^fsB ' "$WORK/manifest")"

echo "== fixture fs2k.ext4 (16 MB, 2K blocks, hand legs only) =="
F=$WORK/fs2k.ext4
dd if=/dev/zero of="$F" bs=1M count=16 status=none
mkfs.ext4 -q -F -b 2048 "$F"
cat > "$WORK/fs2k.cmds" <<EOF
mkdir /src
write $WORK/stage/tar.c /src/tar.c
write $WORK/stage/head256k.bin /sparse2m.bin
sif /sparse2m.bin size 2097152
EOF
dbg "$F" "$WORK/fs2k.cmds"
e2fsck -fn "$F" >/dev/null 2>&1 || { echo "FAIL: fs2k not clean"; exit 1; }

echo "== fixtures ht0/ht1 (htree dirs via e2fsck -D, hand legs only) =="
F=$WORK/ht0.ext4
dd if=/dev/zero of="$F" bs=1M count=32 status=none
mkfs.ext4 -q -F "$F"
python3 -c "
print('mkdir /many')
for i in range(3000): print('write $WORK/stage/frag1k.bin /many/f%05d' % i)
" > "$WORK/ht0.cmds"
dbg "$F" "$WORK/ht0.cmds"
rc=0
e2fsck -f -D -y "$F" >/dev/null 2>&1 || rc=$?
[ $rc -le 1 ] || { echo "FAIL: e2fsck -D ht0 rc=$rc"; exit 1; }
LV=$(debugfs -R "htree_dump /many" "$F" 2>/dev/null | sed -n 's/.*Indirect levels: \([0-9]*\).*/\1/p')
[ "$LV" = "0" ] || { echo "FAIL: ht0 indirect levels = '$LV', want 0"; exit 1; }
echo "  ht0: 3000-entry htree, indirect levels 0"

F=$WORK/ht1.ext4
dd if=/dev/zero of="$F" bs=1M count=48 status=none
mkfs.ext4 -q -F "$F"
python3 -c "
print('mkdir /deep')
for i in range(3200): print('write $WORK/stage/frag1k.bin /deep/longerfilename%06d.dat' % i)
" > "$WORK/ht1.cmds"
dbg "$F" "$WORK/ht1.cmds"
rc=0
e2fsck -f -D -y "$F" >/dev/null 2>&1 || rc=$?
[ $rc -le 1 ] || { echo "FAIL: e2fsck -D ht1 rc=$rc"; exit 1; }
LV=$(debugfs -R "htree_dump /deep" "$F" 2>/dev/null | sed -n 's/.*Indirect levels: \([0-9]*\).*/\1/p')
[ "$LV" = "1" ] || { echo "FAIL: ht1 indirect levels = '$LV', want 1"; exit 1; }
echo "  ht1: 3200-entry htree, indirect levels 1 (interior nodes present)"

echo "== refuse-battery fixtures =="
cp "$WORK/stage/plain.ext4" "$WORK/incoming/plain.ext4"
for feat in encrypt bigalloc inline_data casefold verity; do
    F=$WORK/refuse-$feat.ext4
    rm -f "$F"
    if mkfs.ext4 -q -F -O $feat "$F" 16384 2>/dev/null; then
        debugfs -w -R "write $WORK/stage/hl.txt /f" "$F" >/dev/null 2>&1
    else
        echo "  (mkfs cannot create -O $feat here; dropping that battery leg)"
        rm -f "$F"
    fi
done
F=$WORK/refuse-ext2.ext4
dd if=/dev/zero of="$F" bs=1M count=16 status=none
mkfs.ext2 -q -F "$F"
debugfs -w -R "write $WORK/stage/hl.txt /f" "$F" >/dev/null 2>&1
head -c 2097152 /dev/urandom > "$WORK/refuse-garbage.ext4"
head -c 1500 "$WORK/incoming/fsA.ext4" > "$WORK/refuse-trunc.ext4"

# -----------------------------------------------------------------------
echo "== HAND SELF-TEST: enumerate/extract/strip/rebuild/map (pre-FS) =="

# validate_map <image> <mapfile> <recipefile> <memberdir>:
# MRMP shape + exact partition + member bounds + a full splice simulation
# (what the FS-side sweep guard will do through the real read path)
validate_map() {
    python3 - "$1" "$2" "$3" "$4" <<'PY'
import os, struct, sys
img, mapf, rcpt, mdir = sys.argv[1:5]
orig = open(img, "rb").read()
recipe = open(rcpt, "rb").read()
d = open(mapf, "rb").read()
assert d[:4] == b"MRMP", "map magic"
(n,) = struct.unpack_from("<I", d, 4)
assert len(d) == 8 + n * 29, "map blob length"
out = bytearray(len(orig))
pos = 0
nmem = 0
for i in range(n):
    off, ln, kind, idx, so = struct.unpack_from("<QQBIQ", d, 8 + i * 29)
    assert off == pos and ln > 0, "map must partition contiguously"
    if kind == 0:
        assert idx == 0 and so + ln <= len(recipe), "RECIPE bounds"
        out[off:off+ln] = recipe[so:so+ln]
    else:
        mp = os.path.join(mdir, "%d" % idx)
        m = open(mp, "rb").read()
        assert so + ln <= len(m), "MEMBER bounds"
        out[off:off+ln] = m[so:so+ln]
        nmem += 1
    pos += ln
assert pos == len(orig), "map must cover the whole image"
assert bytes(out) == orig, "map-splice mismatch"
print("    map: %d entries (%d member runs), splice bit-exact" % (n, nmem))
PY
}

hand_selftest() {
    local img=$1 tag=$2
    local tab=$WORK/$tag.tab
    rm -rf "$WORK/$tag.mbr"
    mkdir -p "$WORK/$tag.mbr"
    $E4 enumerate "$img" "$tab" \
        || { echo "FAIL: $tag enumerate"; exit 1; }
    local n=$(wc -l < "$tab")
    [ "$n" -gt 0 ] || { echo "FAIL: $tag zero members"; exit 1; }
    # extract every member and compare against the manifest's debugfs dumps
    local ok=1 idx name usize
    while IFS=$'\t' read -r idx name usize; do
        $E4 extract "$img" "$idx" "$WORK/$tag.mbr/$idx" \
            || { echo "FAIL: $tag extract idx=$idx"; exit 1; }
        [ "$(stat -c%s "$WORK/$tag.mbr/$idx")" = "$usize" ] \
            || { echo "FAIL: $tag idx=$idx size != usize"; exit 1; }
        if [ -f "$WORK/expect/$tag-$idx" ]; then
            cmp -s "$WORK/expect/$tag-$idx" "$WORK/$tag.mbr/$idx" \
                || { echo "FAIL: $tag member $idx ($name) content mismatch"; ok=0; }
        fi
    done < "$tab"
    [ "$ok" = 1 ] || exit 1
    # strip -> rebuild -> bit-exact
    $E4 strip "$img" "$WORK/$tag.recipe" \
        || { echo "FAIL: $tag strip"; exit 1; }
    $E4 rebuild "$WORK/$tag.recipe" "$WORK/$tag.mbr" "$WORK/$tag.rebuilt" \
        || { echo "FAIL: $tag rebuild"; exit 1; }
    cmp -s "$img" "$WORK/$tag.rebuilt" \
        || { echo "FAIL: $tag rebuild not bit-exact"; exit 1; }
    # map -> validate + splice simulation
    $E4 map "$img" "$WORK/$tag.map" || { echo "FAIL: $tag map"; exit 1; }
    validate_map "$img" "$WORK/$tag.map" "$WORK/$tag.recipe" "$WORK/$tag.mbr"
    # estimate: bare number == sum(usize) + 64 MiB
    local est sum
    est=$($E4 estimate "$img") || { echo "FAIL: $tag estimate"; exit 1; }
    sum=$(awk -F'\t' '{s += $3} END {print s}' "$tab")
    [ "$est" = "$((sum + 64 * 1024 * 1024))" ] \
        || { echo "FAIL: $tag estimate $est != $sum + 64MiB"; exit 1; }
    echo "  $tag: $n members, enumerate/extract/strip/rebuild/map/estimate OK"
}

hand_selftest "$WORK/incoming/fsA.ext4" fsA
hand_selftest "$WORK/incoming/fsB.ext4" fsB
hand_selftest "$WORK/fs2k.ext4" fs2k
# htree fixtures: member content is uniform; exercise extract + rebuild
for t in ht0 ht1; do
    F=$WORK/$t.ext4
    rm -rf "$WORK/$t.mbr" && mkdir -p "$WORK/$t.mbr"
    $E4 enumerate "$F" "$WORK/$t.tab" || { echo "FAIL: $t enumerate"; exit 1; }
    N=$(wc -l < "$WORK/$t.tab")
    while IFS=$'\t' read -r idx name usize; do
        $E4 extract "$F" "$idx" "$WORK/$t.mbr/$idx" || exit 1
    done < "$WORK/$t.tab"
    $E4 strip "$F" "$WORK/$t.recipe" || { echo "FAIL: $t strip"; exit 1; }
    $E4 rebuild "$WORK/$t.recipe" "$WORK/$t.mbr" "$WORK/$t.rebuilt" \
        || { echo "FAIL: $t rebuild"; exit 1; }
    cmp -s "$F" "$WORK/$t.rebuilt" \
        || { echo "FAIL: $t rebuild not bit-exact"; exit 1; }
    $E4 map "$F" "$WORK/$t.map" || { echo "FAIL: $t map"; exit 1; }
    validate_map "$F" "$WORK/$t.map" "$WORK/$t.recipe" "$WORK/$t.mbr"
    echo "  $t: $N members (htree), rebuild + map bit-exact"
done

echo "== HAND SELF-TEST: decline battery (exit 3) =="
for f in "$WORK/incoming/plain.ext4" "$WORK/refuse-garbage.ext4" \
         "$WORK/refuse-trunc.ext4" "$WORK/refuse-ext2.ext4" \
         "$WORK"/refuse-encrypt.ext4 "$WORK"/refuse-bigalloc.ext4 \
         "$WORK"/refuse-inline_data.ext4 "$WORK"/refuse-casefold.ext4 \
         "$WORK"/refuse-verity.ext4; do
    [ -f "$f" ] || continue
    rc=0
    $E4 enumerate "$f" "$WORK/decline.tab" 2>/dev/null || rc=$?
    [ $rc -eq 3 ] || { echo "FAIL: $f rc=$rc, want 3"; exit 1; }
    echo "  declined (rc=3): $(basename "$f")"
done
# an ext4 image without orphan_file and with no regular files at all has
# zero members -> decline (a default empty image has exactly ONE member:
# the orphan_file inode, a zero-filled regular file -- harmless to
# decompose, and the guard proves it bit-exact like everything else)
F=$WORK/emptyfs.ext4
dd if=/dev/zero of="$F" bs=1M count=8 status=none
mkfs.ext4 -q -F -O ^orphan_file "$F"
rc=0
$E4 enumerate "$F" "$WORK/decline.tab" 2>/dev/null || rc=$?
[ $rc -eq 3 ] || { echo "FAIL: empty fs rc=$rc, want 3 (zero members)"; exit 1; }
echo "  declined (rc=3): zero-member filesystem (no orphan_file feature)"
# and the default empty image: exactly one member (the orphan file)
F=$WORK/onlyorphan.ext4
dd if=/dev/zero of="$F" bs=1M count=8 status=none
mkfs.ext4 -q -F "$F"
$E4 enumerate "$F" "$WORK/orphan.tab" || { echo "FAIL: orphan-only image declined"; exit 1; }
N=$(wc -l < "$WORK/orphan.tab")
[ "$N" -eq 1 ] || { echo "FAIL: orphan-only image has $N members, want 1"; exit 1; }
$E4 strip "$F" "$WORK/orphan.recipe" && rm -rf "$WORK/orphan.mbr" && mkdir "$WORK/orphan.mbr"
while IFS=$'\t' read -r idx name usize; do
    $E4 extract "$F" "$idx" "$WORK/orphan.mbr/$idx" || exit 1
done < "$WORK/orphan.tab"
$E4 rebuild "$WORK/orphan.recipe" "$WORK/orphan.mbr" "$WORK/orphan.rebuilt" \
    || { echo "FAIL: orphan-only rebuild"; exit 1; }
cmp -s "$F" "$WORK/orphan.rebuilt" || { echo "FAIL: orphan-only not bit-exact"; exit 1; }
$E4 map "$F" "$WORK/orphan.map" && validate_map "$F" "$WORK/orphan.map" "$WORK/orphan.recipe" "$WORK/orphan.mbr"
echo "  orphan-only image: 1 member, rebuild + map bit-exact"

echo "== HAND SELF-TEST: junk rebuild legs (exit 1) =="
T=fsA
head -c 30000 "$WORK/$T.recipe" > "$WORK/$T.trunc.recipe"
rc=0
$E4 rebuild "$WORK/$T.trunc.recipe" "$WORK/$T.mbr" "$WORK/junk.out" 2>/dev/null || rc=$?
[ $rc -eq 1 ] || { echo "FAIL: truncated recipe rc=$rc, want 1"; exit 1; }
cp "$WORK/$T.recipe" "$WORK/$T.corrupt.recipe"
printf 'ZZZZ' | dd of="$WORK/$T.corrupt.recipe" bs=1 seek=8 conv=notrunc status=none
rc=0
$E4 rebuild "$WORK/$T.corrupt.recipe" "$WORK/$T.mbr" "$WORK/junk.out" 2>/dev/null || rc=$?
[ $rc -eq 1 ] || { echo "FAIL: corrupt-size recipe rc=$rc, want 1"; exit 1; }
# wrong-size member
MIDX=$(sed -n '1p' "$WORK/$T.tab" | cut -f1)
cp "$WORK/$T.mbr/$MIDX" "$WORK/$T.mbr/$MIDX.bak"
truncate -s 17 "$WORK/$T.mbr/$MIDX"
rc=0
$E4 rebuild "$WORK/$T.recipe" "$WORK/$T.mbr" "$WORK/junk.out" 2>/dev/null || rc=$?
[ $rc -eq 1 ] || { echo "FAIL: wrong-size member rc=$rc, want 1"; exit 1; }
mv "$WORK/$T.mbr/$MIDX.bak" "$WORK/$T.mbr/$MIDX"
# missing member
mv "$WORK/$T.mbr/$MIDX" "$WORK/$T.mbr/$MIDX.gone"
rc=0
$E4 rebuild "$WORK/$T.recipe" "$WORK/$T.mbr" "$WORK/junk.out" 2>/dev/null || rc=$?
[ $rc -eq 1 ] || { echo "FAIL: missing member rc=$rc, want 1"; exit 1; }
mv "$WORK/$T.mbr/$MIDX.gone" "$WORK/$T.mbr/$MIDX"
# extract with an unknown idx
rc=0
$E4 extract "$WORK/incoming/fsA.ext4" 65000 "$WORK/junk.out" 2>/dev/null || rc=$?
[ $rc -eq 1 ] || { echo "FAIL: bad extract idx rc=$rc, want 1"; exit 1; }
echo "  junk recipes/members refused with exit 1"

# -----------------------------------------------------------------------
echo "== InvariantFS e2e: mkfs + import =="
$B/invf-mkfs "$IMG" 0.35 >/dev/null
$B/invf-import "$IMG" "$WORK/incoming" >/dev/null
$B/invf-ls "$IMG" | grep -E "fsA.ext4|fsB.ext4|plain.ext4"

echo "== sweep #1 (ext4fs containerpack) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
grep -q "fsA.ext4: ext4fs (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: fsA.ext4 not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "fsB.ext4: ext4fs (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: fsB.ext4 not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
if grep -q "plain.ext4: ext4fs (codecpack)" "$WORK/sweep1.log"; then
    echo "FAIL: plain.ext4 (a text file) was decomposed"; exit 1
fi
# members batch in the SAME run their container decomposed (WP14b pattern)
grep -qE "fsA\.ext4!\*: [0-9]+ parts -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: fsA text members not PPMd-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -qE "fsA\.ext4!\*: [0-9]+ parts -> ZSTD batch" "$WORK/sweep1.log" \
    || { echo "FAIL: fsA ELF member not ZSTD-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -E "codecpack|parts -> " "$WORK/sweep1.log"

echo "== sibling set (members + !mbrt + !mbrmap) =="
NA=$(grep -c '^fsA ' "$WORK/manifest")
NB=$(grep -c '^fsB ' "$WORK/manifest")
SA=$($B/invf-ls "$IMG" | grep -c "fsA\.ext4!" || true)
SB=$($B/invf-ls "$IMG" | grep -c "fsB\.ext4!" || true)
echo "  fsA: $SA siblings ($NA members + table + map); fsB: $SB"
[ "$SA" -eq $((NA + 2)) ] || { echo "FAIL: fsA sibling count"; $B/invf-ls "$IMG"; exit 1; }
[ "$SB" -eq $((NB + 2)) ] || { echo "FAIL: fsB sibling count"; $B/invf-ls "$IMG"; exit 1; }
$B/invf-ls "$IMG" | grep -q "fsA\.ext4!mbrt" \
    || { echo "FAIL: fsA member table missing"; exit 1; }
$B/invf-ls "$IMG" | grep -q "fsA\.ext4!mbrmap" \
    || { echo "FAIL: fsA member map missing"; exit 1; }
# the empty member is a real 0-byte inode
EINO=$(awk '$2 == "/empty.bin" && $1 == "fsA" {print $3}' "$WORK/manifest")
ESIB=$(printf "fsA.ext4!mbr%04d-empty.bin" "$EINO")
$B/invf-ls "$IMG" | grep "$ESIB" | grep -q "0 bytes" \
    || { echo "FAIL: empty member missing/not 0 bytes"; $B/invf-ls "$IMG" | grep empty; exit 1; }
if $B/invf-ls "$IMG" | grep -q "plain\.ext4!"; then
    echo "FAIL: declined plain.ext4 left siblings"; exit 1
fi
echo "siblings exact; empty member is 0 bytes; plain.ext4 clean"

echo "== class stamps =="
C=$("$WORK/classof" "$IMG" fsA.ext4)
echo "  fsA.ext4: $C"
[ "$C" = "cls=3 algo=17 gen=1" ] || { echo "FAIL: want CONTAINER{17,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" fsB.ext4)
echo "  fsB.ext4: $C"
[ "$C" = "cls=3 algo=17 gen=1" ] || { echo "FAIL: want CONTAINER{17,1}"; exit 1; }
# a text member (leaf.c) -> TEXT{PPMD,1}; the ELF -> BATCHED_BIN{ZSTD_BCJ,1}
TINO=$(awk '$2 == "/deep/a/b/c/d/e/leaf.c" && $1 == "fsA" {print $3}' "$WORK/manifest")
TSIB=$(printf "fsA.ext4!mbr%04d-leaf.c" "$TINO")
C=$("$WORK/classof" "$IMG" "$TSIB")
echo "  leaf.c member: $C"
[ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: want TEXT{PPMD,1} for $TSIB"; exit 1; }
XINO=$(awk '$2 == "/bin/busybox.elf" && $1 == "fsA" {print $3}' "$WORK/manifest")
XSIB=$(printf "fsA.ext4!mbr%04d-busybox.elf" "$XINO")
C=$("$WORK/classof" "$IMG" "$XSIB")
echo "  ELF member: $C"
[ "$C" = "cls=8 algo=14 gen=1" ] || { echo "FAIL: want BATCHED_BIN{ZSTD_BCJ,1} for $XSIB"; exit 1; }
C=$("$WORK/classof" "$IMG" plain.ext4)
echo "  plain.ext4: $C"
case "$C" in *algo=17*) echo "FAIL: plain.ext4 carries the pack stamp"; exit 1;; esac
[ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: plain.ext4 should be TEXT{PPMD,1}"; exit 1; }

echo "== verify --deep (reads every container through the map) =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== sha256 bit-exact (containers through the map splice) =="
for f in fsA.ext4 fsB.ext4 plain.ext4; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/incoming/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    [ "$a" = "$b" ] || { echo "FAIL: sha256 mismatch $f"; exit 1; }
    echo "  $f: $b"
done

echo "== direct member reads bit-exact =="
ok=1
while read -r tag path ino; do
    [ "$tag" = fsA ] || [ "$tag" = fsB ] || continue
    base=${path##*/}
    [ "$path" = "<orphan>" ] && base="ino$ino"
    sib=$(printf "%s.ext4!mbr%04d-%s" "$tag" "$ino" "$base")
    $B/invf-cat "$IMG" "$sib" "$WORK/out/m-$tag-$ino" >/dev/null \
        || { echo "FAIL: read $sib"; ok=0; continue; }
    cmp -s "$WORK/expect/$tag-$ino" "$WORK/out/m-$tag-$ino" \
        || { echo "MISMATCH member $sib"; ok=0; }
done < "$WORK/manifest"
[ "$ok" = 1 ] || exit 1
echo "all $(grep -cE '^(fsA|fsB) ' "$WORK/manifest") members read back bit-exact"

echo "== ranged reads (the WP16b local splice, incl. multi-extent member) =="
# big.bin's inode + a slice inside its multi-extent body
BINO=$(awk '$2 == "/big.bin" && $1 == "fsA" {print $3}' "$WORK/manifest")
# find big.bin's first extent image offset from the pack's own map: walk the
# member table -> use extract offsets indirectly: pick offsets by probing the
# original image for known content is fragile; instead slice the image at the
# member-content ranges listed in the pack map.
python3 - "$WORK/incoming/fsA.ext4" "$WORK/fsA.map" "$BINO" > "$WORK/ranges.txt" <<'PY'
import struct, sys
d = open(sys.argv[2], "rb").read()
bino = int(sys.argv[3])
(n,) = struct.unpack_from("<I", d, 4)
runs = []          # big.bin member runs (off, len)
boundary = None    # first RECIPE -> MEMBER transition
prev_kind = None
size = 64 * 1024 * 1024
last_end_kind = None
for i in range(n):
    off, ln, kind, idx, so = struct.unpack_from("<QQBIQ", d, 8 + i * 29)
    if kind == 1 and idx == bino:
        runs.append((off, ln))
    if prev_kind == 0 and kind == 1 and boundary is None:
        boundary = (off - 16, 64)
    prev_kind = kind
runs.sort()
big = max(runs, key=lambda r: r[1])        # the merged contiguous head run
mid = (big[0] + 4096, min(65536, big[1] - 4096))
print("mid %d %d" % mid)
print("boundary %d %d" % boundary)
print("tail %d %d" % (size - 1000, 1000))
print("head %d %d" % (0, 8192))
PY
while read -r tag off len; do
    dd if="$WORK/incoming/fsA.ext4" of="$WORK/out/ref.$tag" bs=1 skip="$off" count="$len" 2>/dev/null
    "$WORK/rngread" "$IMG" fsA.ext4 "$off" "$len" "$WORK/out/got.$tag" >/dev/null
    cmp -s "$WORK/out/ref.$tag" "$WORK/out/got.$tag" \
        || { echo "FAIL: range $tag (off=$off len=$len) mismatch"; exit 1; }
done < "$WORK/ranges.txt"
"$WORK/rngread" "$IMG" fsA.ext4 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp -s "$WORK/incoming/fsA.ext4" "$WORK/out/whole.rng" \
    || { echo "FAIL: whole file via 64K ranged windows mismatch"; exit 1; }
echo "mid-member / recipe-boundary / tail / head / whole-by-windows exact"

echo "== pack-ABSENT reads still work (the map is self-describing) =="
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" fsA.ext4 "$WORK/out/absentA" >/dev/null
cmp -s "$WORK/incoming/fsA.ext4" "$WORK/out/absentA" \
    || { echo "FAIL: pack-absent fsA read not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" fsB.ext4 "$WORK/out/absentB" >/dev/null
cmp -s "$WORK/incoming/fsB.ext4" "$WORK/out/absentB" \
    || { echo "FAIL: pack-absent fsB read not bit-exact"; exit 1; }
line=$(grep '^mid ' "$WORK/ranges.txt")
set -- $line
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" fsA.ext4 "$2" "$3" \
    "$WORK/out/absent.rng" >/dev/null
cmp -s "$WORK/out/ref.mid" "$WORK/out/absent.rng" \
    || { echo "FAIL: pack-absent ranged read mismatch"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" fsA.ext4 0 -1 \
    "$WORK/out/absent.wrng" >/dev/null
cmp -s "$WORK/incoming/fsA.ext4" "$WORK/out/absent.wrng" \
    || { echo "FAIL: pack-absent whole ranged read mismatch"; exit 1; }
echo "pack-absent: whole + ranged reads bit-exact, zero pack exec"

echo "== sweep #2: idempotent (no re-decomposition) =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
if grep -q "ext4fs (codecpack)" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-decomposed a container"; cat "$WORK/sweep2.log"; exit 1
fi
C=$("$WORK/classof" "$IMG" fsA.ext4)
[ "$C" = "cls=3 algo=17 gen=1" ] || { echo "FAIL: fsA stamp drifted: $C"; exit 1; }
grep -q " 0 corrupt," <($B/invf-verify "$IMG" --deep) \
    || { echo "FAIL: corrupt after re-sweep"; exit 1; }
echo "no re-decomposition; stamps stable"

echo "== delete cascade (vol_unlink, the FUSE path) =="
"$WORK/cbrm" "$IMG" fsA.ext4 fsB.ext4 plain.ext4
if $B/invf-ls "$IMG" | grep -q "ext4!"; then
    echo "FAIL: !mbr/!mbrt/!mbrmap siblings survived the delete"; $B/invf-ls "$IMG"; exit 1
fi
echo "containers + all members + maps deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== admission leg: INVFS_ARC_BYTES=1M (GENERIC_MEMLIMIT{17,1}) =="
$B/invf-mkfs "$IMGMEM" 0.2 >/dev/null
$B/invf-import "$IMGMEM" "$WORK/incoming" >/dev/null
INVFS_ARC_BYTES=1M $B/invf-sweep "$IMGMEM" > "$WORK/sweep-mem.log" 2>&1 \
    || { cat "$WORK/sweep-mem.log"; exit 1; }
if grep -q "ext4fs (codecpack)" "$WORK/sweep-mem.log"; then
    echo "FAIL: decomposition ran under a 1M ARC budget"; exit 1
fi
C=$("$WORK/classof" "$IMGMEM" fsA.ext4)
echo "  fsA.ext4 (arc-limited): $C"
[ "$C" = "cls=5 algo=17 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{17,1}"; exit 1; }
C=$("$WORK/classof" "$IMGMEM" fsB.ext4)
echo "  fsB.ext4 (arc-limited): $C"
[ "$C" = "cls=5 algo=17 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{17,1}"; exit 1; }
$B/invf-cat "$IMGMEM" fsA.ext4 "$WORK/out/memA" >/dev/null
cmp -s "$WORK/incoming/fsA.ext4" "$WORK/out/memA" \
    || { echo "FAIL: arc-limited read not bit-exact"; exit 1; }
$B/invf-cat "$IMGMEM" fsB.ext4 "$WORK/out/memB" >/dev/null
cmp -s "$WORK/incoming/fsB.ext4" "$WORK/out/memB" \
    || { echo "FAIL: arc-limited fsB read not bit-exact"; exit 1; }
echo "policy refusal stored generic, bit-exact"

echo "== ratio demo (pack decomposition vs generic-only control) =="
$B/invf-mkfs "$IMGCTL" 0.35 >/dev/null
$B/invf-import "$IMGCTL" "$WORK/incoming" >/dev/null
INVFS_CODECPACKS=$WORK/nopacks $B/invf-sweep "$IMGCTL" > "$WORK/sweep-ctl.log" 2>&1 \
    || { cat "$WORK/sweep-ctl.log"; exit 1; }
if grep -q "codecpack" "$WORK/sweep-ctl.log"; then
    echo "FAIL: control volume saw a codecpack"; exit 1
fi
$B/invf-verify "$IMGCTL" --deep > "$WORK/verify-ctl.log" 2>&1
grep -q " 0 corrupt," "$WORK/verify-ctl.log" \
    || { echo "FAIL: control volume corrupt"; cat "$WORK/verify-ctl.log"; exit 1; }
for f in fsA.ext4 fsB.ext4; do
    $B/invf-cat "$IMGCTL" "$f" "$WORK/out/ctl-$f" >/dev/null
    cmp -s "$WORK/incoming/$f" "$WORK/out/ctl-$f" \
        || { echo "FAIL: control $f not bit-exact"; exit 1; }
done
# re-do the pack volume fresh for a like-for-like size comparison
IMG2=wp16ext4-pack.img
rm -f "$IMG2"
$B/invf-mkfs "$IMG2" 0.35 >/dev/null
$B/invf-import "$IMG2" "$WORK/incoming" >/dev/null
$B/invf-sweep "$IMG2" > "$WORK/sweep-pack.log" 2>&1 || { cat "$WORK/sweep-pack.log"; exit 1; }
grep -q "fsA.ext4: ext4fs (codecpack)" "$WORK/sweep-pack.log" \
    || { echo "FAIL: pack volume did not decompose"; exit 1; }
$B/invf-sweep "$IMG2" > "$WORK/sweep-pack2.log" 2>&1   # settle member generics
$E4 strip "$WORK/incoming/fsA.ext4" "$WORK/demo.recipe"
RRECIPE=$(stat -c%s "$WORK/demo.recipe")
# the volume image files are pre-sized at mkfs; the meaningful number is
# the ALLOCATED block count (invf-stats "used")
PKUSED=$($B/invf-stats "$IMG2" | sed -n 's/^  used[^:]*: \([0-9.]*\) MiB.*/\1/p')
CTUSED=$($B/invf-stats "$IMGCTL" | sed -n 's/^  used[^:]*: \([0-9.]*\) MiB.*/\1/p')
PKLOG=$($B/invf-stats "$IMG2" | sed -n 's/^  logical bytes[^:]*: \([0-9.]*\) MiB/\1/p')
CTLOG=$($B/invf-stats "$IMGCTL" | sed -n 's/^  logical bytes[^:]*: \([0-9.]*\) MiB/\1/p')
PKRATIO=$($B/invf-stats "$IMG2" | sed -n 's/^  est. ratio[^:]*: \([0-9.]*\)x.*/\1/p')
CTRATIO=$($B/invf-stats "$IMGCTL" | sed -n 's/^  est. ratio[^:]*: \([0-9.]*\)x.*/\1/p')
echo "  control (whole images, generic ZSTD-19): used $CTUSED MiB / logical $CTLOG MiB / ratio ${CTRATIO}x"
echo "  pack (recipe raw + members batched):     used $PKUSED MiB / logical $PKLOG MiB / ratio ${PKRATIO}x"
echo "  fsA recipe blob: $RRECIPE bytes (all non-member bytes verbatim)"
echo "  member pipeline: 8 text members -> PPMd batch, ELF -> ZSTD_BCJ batch"
[ -n "$PKUSED" ] && [ -n "$CTUSED" ] || { echo "FAIL: stats parse"; exit 1; }
echo "ratio demo recorded: pack_used=${PKUSED}MiB(${PKRATIO}x) control_used=${CTUSED}MiB(${CTRATIO}x) recipe=$RRECIPE"

echo "EXT4FS CONTAINERPACK E2E: PASS"
