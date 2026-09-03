#!/bin/bash
# test-xfs.sh — the xfs containerpack (tools/codecpacks/xfs.codecpack)
# end-to-end (persistent regression).
#
#   fixtures (mkfs.xfs + sudo loop-mount populate):
#     fs-a.xfs — 300 MiB XFS v5 (crc=1, finobt, no rmap/reflink/nrext64,
#       512B inodes, agcount=2) populated with: busybox .c text files, a
#       real x86-64 ELF, a sparse file, a hardlink pair, a deep tree
#       (a/b/c/d/e{1..5}), an 8 MiB multi-extent file (bmap BTREE, depth
#       1), an empty file, a shortform symlink, a fifo, an unwritten-
#       extent file (fallocate). Every regular file's inode is forced
#       into AG0 (< 65536, the FS idx cap) by a create-and-check loop --
#       XFS rotates new directories across AGs and AG1 inode numbers
#       exceed the cap (that case is its own refusal fixture, fs-d.xfs).
#     fs-b.xfs — 300 MiB XFS v4 (crc=0, 256B inodes): block-format dir,
#       leaf-format dir (300 files), BTREE-format file, sparse, hardlink.
#     fs-c.xfs — mkfs.xfs DEFAULTS (rmapbt+reflink+nrext64): declined.
#     fs-d.xfs — supported features but a regular file in AG1
#       (ino > 65535): declined.
#     junk.xfs — a text file carrying the .xfs extension: declined.
#
#   hand self-test first (enumerate/extract/strip/rebuild bit-exact, MRMP
#   map validated by an inline-python emulation of the FS map guard),
#   then FS e2e:
#     admission: default ARC 256M -> GENERIC_MEMLIMIT{19}; raised
#       INVFS_ARC_BYTES=1G -> decompose. Second admission leg:
#       INVFS_DEC_MEM_LIMIT=64K (the pack's estimate is sum+64MiB) ->
#       GENERIC_MEMLIMIT{19} even with ARC raised.
#     main: sweep -> CONTAINER{19,1} stamps -> member classes (text ->
#       TEXT{PPMD,1}, ELF -> BATCHED_BIN{ZSTD_BCJ,1}) -> verify --deep ->
#       sha256 bit-exact (containers + direct member reads) -> ranged
#       reads -> pack-ABSENT reads via !mbrmap -> idempotent re-sweep ->
#       delete cascade -> fsck clean -> refusals stay generic+bit-exact.
#
# Run from the repo root after `make`:  bash tools/test-xfs.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
PACK=$REPO/tools/codecpacks/xfs.codecpack
# scratch lives on /tmp (a separate tmpfs): only the blkio-opened VOLUME
# images must sit under /dev/shm with relative paths (the header note);
# /dev/shm fills up fast when several pack waves soak at once
WORK=/tmp/wpxfs
IMG=wp16xfs.img
IMGMEM=wp16xfs-mem.img
IMGMEM2=wp16xfs-mem2.img
IMGNEG=wp16xfs-neg.img
export INVFS_CODECPACKS=$REPO/tools/codecpacks   # the sweep AND the reads
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/ref" "$WORK/nopacks"
cd /dev/shm
rm -f "$IMG" "$IMGMEM" "$IMGMEM2" "$IMGNEG"

echo "== tools =="
command -v cc >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
command -v mkfs.xfs >/dev/null || { echo "FAIL: mkfs.xfs not installed"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
sudo -n true 2>/dev/null || { echo "FAIL: need passwordless sudo (loop mounts)"; exit 1; }

echo "== build the pack (cc -O2 -Wall -Wextra; warnings are errors) =="
mkdir -p "$PACK/bin"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACK/bin/xfs" "$PACK/xfs.c"
echo "pack binary built clean"

echo "== test helpers (classof / rngread / cbrm from the repo objects) =="
cat > "$WORK/classof.c" <<'C'
/* classof.c — print the WP10 storage-class stamp of one file. */
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
 * read path for a seekable container). */
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
CORE_O="$REPO/build/obj/volume.o $REPO/build/obj/vol_cpack.o $REPO/build/obj/vol_png.o $REPO/build/obj/vol_seal.o $REPO/build/obj/vol_repair.o $REPO/build/obj/vol_rollback.o $REPO/build/obj/vol_resize.o $REPO/build/obj/vol_fsck.o $REPO/build/obj/vol_crash.o $REPO/build/obj/vol_exer.o $REPO/build/obj/vol_dedupe.o $REPO/build/obj/vol_textzone.o $REPO/build/obj/vol_heat.o $REPO/build/obj/vol_sweep.o $REPO/build/obj/vol_read.o $REPO/build/obj/vol_write.o $REPO/build/obj/vol_records.o $REPO/build/obj/vol_ast.o $REPO/build/obj/vol_dirs.o $REPO/build/obj/arc.o $REPO/build/obj/crc32c.o $REPO/build/obj/lz4.o $REPO/build/obj/blkio.o $REPO/build/obj/flacx.o $REPO/build/obj/tarx.o $REPO/build/obj/pngx.o $REPO/build/obj/miniz.o $REPO/build/obj/ppmd8.o $REPO/build/obj/ppmd8enc.o $REPO/build/obj/ppmd8dec.o $REPO/build/obj/ppmd_codec.o $REPO/build/obj/codec.o $REPO/build/obj/bcj_x86.o $REPO/build/obj/blake3.o $REPO/build/obj/blake3_dispatch.o $REPO/build/obj/blake3_portable.o $REPO/build/obj/rs.o $REPO/build/obj/vol_tier.o"
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/classof" "$WORK/classof.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/rngread" "$WORK/rngread.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/cbrm" "$WORK/cbrm.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== fixtures: mkfs.xfs + loop-mount populate =="
MNT=$WORK/mnt
mkdir -p "$MNT"
UMOUNTED=0
cleanup() { if [ "$UMOUNTED" = 0 ]; then sudo umount "$MNT" 2>/dev/null || true; fi; \
    rm -rf "$WORK" /dev/shm/wp16xfs*.img 2>/dev/null || true; }
trap cleanup EXIT

# mkdir, retrying until the inode lands in AG0 (ino < 65536): XFS rotates
# new directories across allocation groups, and AG1 inode numbers exceed
# the FS member-idx cap (see fs-d.xfs for the refusal leg).
mkag0() {
    local d="$1" i tries=0
    while :; do
        mkdir -p "$d" 2>/dev/null || true
        i=$(stat -c %i "$d")
        [ "$i" -lt 65536 ] && return 0
        rmdir "$d" 2>/dev/null || true
        tries=$((tries + 1))
        [ "$tries" -gt 80 ] && { echo "FAIL: AG rotor stuck on $d"; exit 1; }
    done
}

mkxfs() {  # mkxfs <img> <mkfs args...>
    dd if=/dev/zero of="$1" bs=1M count=300 status=none
    mkfs.xfs -f "${@:2}" "$1" >/dev/null
}

echo "  fs-a.xfs (v5)"
mkxfs "$WORK/orig/fs-a.xfs" -m crc=1,finobt=1,rmapbt=0,reflink=0,inobtcount=0,bigtime=1 \
    -i size=512,sparse=0,nrext64=0 -n ftype=1
sudo mount -o loop "$WORK/orig/fs-a.xfs" "$MNT"
sudo chown user:user "$MNT"
for d in a a/b a/b/c a/b/c/d a/b/c/d/e1 a/b/c/d/e2 a/b/c/d/e3 a/b/c/d/e4 a/b/c/d/e5 sub; do
    mkag0 "$MNT/$d"
done
python3 - "$MNT" <<'PY'
import os, random, subprocess, sys
mnt = sys.argv[1]
# text files: real busybox .c sources
srcs = []
for root, _d, files in os.walk('/home/user/InvariantFS/tools/busybox-src'):
    for n in sorted(files):
        p = os.path.join(root, n)
        if n.endswith('.c') and os.path.getsize(p) > 20000:
            srcs.append(p)
    if len(srcs) >= 3:
        break
assert len(srcs) >= 3, "busybox .c fixtures missing"
for i, s in enumerate(srcs[:3]):
    open(f'{mnt}/text{i}.c', 'wb').write(open(s, 'rb').read())
# a real x86-64 ELF
elf = None
for p in ('/usr/bin/passwd', '/usr/bin/gpg', '/bin/ls', '/usr/bin/ls',
          '/bin/bash', '/usr/bin/bash'):
    if os.path.isfile(p) and not os.path.islink(p):
        h = open(p, 'rb').read(20)
        if h[:4] == b'\x7fELF' and h[18] == 62 and os.path.getsize(p) > 65536:
            elf = p
            break
assert elf, "no x86-64 ELF fixture found"
open(f'{mnt}/prog.elf', 'wb').write(open(elf, 'rb').read())
# sparse file (holes read as zeros)
with open(f'{mnt}/sparse.bin', 'wb') as f:
    f.write(b'HEAD')
    f.seek(1 << 20)
    f.write(b'MID')
    f.seek(3 << 20)
    f.write(b'END')
# hardlink pair (one member)
open(f'{mnt}/hardA.txt', 'w').write('hardlinked content\n' * 100)
os.link(f'{mnt}/hardA.txt', f'{mnt}/hardB.txt')
# empty file (zero-length member)
open(f'{mnt}/empty.bin', 'wb').close()
# shortform symlink + fifo: recipe bytes, never members
os.symlink('text0.c', f'{mnt}/slink')
os.mkfifo(f'{mnt}/fifo')
# unwritten extents (fallocate): physical bytes stay in the recipe
subprocess.run(['xfs_io', '-f', '-c', 'resvsp 0 1m', '-c', 'pwrite 0 4k',
                '-c', 'pwrite 900000 4k', f'{mnt}/unw.bin'], check=True,
               stdout=subprocess.DEVNULL)
# 8 MiB multi-extent file: 4 KiB writes interleaved with a decoy so the
# extents scatter past the inline-fork capacity (bmap BTREE, depth 1)
rnd = random.Random(7)
blk = rnd.randbytes(4096)
f1 = open(f'{mnt}/big8m.bin', 'wb')
f2 = open(f'{mnt}/decoy.bin', 'wb')
for i in range(2048):
    f1.write(blk); f1.flush(); os.fsync(f1.fileno())
    f2.write(blk); f2.flush(); os.fsync(f2.fileno())
f1.close(); f2.close()
assert os.path.getsize(f'{mnt}/big8m.bin') == 8 << 20
# deep tree + subdir content
for i in range(1, 6):
    open(f'{mnt}/a/b/c/d/e{i}/leaf.txt', 'w').write(f'leaf {i}\n' * 50)
for i in range(3):
    open(f'{mnt}/sub/sfile{i}.txt', 'w').write(f'sub file {i}\n' * 20)
PY
sync
find "$MNT" -xdev -type f -printf "%i %P\n" | sort -n > "$WORK/ref/fs-a.inos"
mkdir -p "$WORK/ref/fs-a.tree" && cp -a "$MNT/." "$WORK/ref/fs-a.tree/"
awk '$1 >= 65536 {bad++} END {exit (bad+0) > 0}' "$WORK/ref/fs-a.inos" \
    || { echo "FAIL: fs-a has a regular file outside AG0"; cat "$WORK/ref/fs-a.inos"; exit 1; }
sudo umount "$MNT"
echo "    $(wc -l < "$WORK/ref/fs-a.inos") files (incl. hardlink), all in AG0"

echo "  fs-b.xfs (v4)"
mkxfs "$WORK/orig/fs-b.xfs" -m crc=0 -i size=256 -n ftype=1
sudo mount -o loop "$WORK/orig/fs-b.xfs" "$MNT"
sudo chown user:user "$MNT"
mkag0 "$MNT/blkdir"
mkag0 "$MNT/bigdir"
python3 - "$MNT" <<'PY'
import os, sys
mnt = sys.argv[1]
src = None
for root, _d, files in os.walk('/home/user/InvariantFS/tools/busybox-src'):
    for n in sorted(files):
        p = os.path.join(root, n)
        if n.endswith('.c') and os.path.getsize(p) > 30000:
            src = p
            break
    if src:
        break
open(f'{mnt}/hello.c', 'wb').write(open(src, 'rb').read())
elf = None
for p in ('/usr/bin/passwd', '/bin/ls', '/usr/bin/ls', '/bin/bash'):
    if os.path.isfile(p) and not os.path.islink(p):
        h = open(p, 'rb').read(20)
        if h[:4] == b'\x7fELF' and h[18] == 62 and os.path.getsize(p) > 65536:
            elf = p
            break
open(f'{mnt}/tool.elf', 'wb').write(open(elf, 'rb').read())
with open(f'{mnt}/holes.bin', 'wb') as f:
    f.write(b'A' * 4096)
    f.seek(4 << 20)
    f.write(b'B' * 4096)
open(f'{mnt}/link1.txt', 'w').write('v4 hardlink\n' * 40)
os.link(f'{mnt}/link1.txt', f'{mnt}/link2.txt')
os.symlink('hello.c', f'{mnt}/slink')
for i in range(35):                       # block-format directory
    open(f'{mnt}/blkdir/bf{i:02d}.txt', 'w').write(f'blk {i}\n')
for i in range(300):                      # leaf-format directory
    open(f'{mnt}/bigdir/file{i:03d}.txt', 'w').write(f'leaf {i}\n')
# a fragmented file: bmap BTREE on v4 too (inline capacity is 6)
fs = [open(f'{mnt}/frag{i}.bin', 'wb') for i in range(8)]
blk = bytes(4096)
for r in range(40):
    for f in fs:
        f.write(blk); f.flush(); os.fsync(f.fileno())
for f in fs:
    f.close()
PY
sync
find "$MNT" -xdev -type f -printf "%i %P\n" | sort -n > "$WORK/ref/fs-b.inos"
mkdir -p "$WORK/ref/fs-b.tree" && cp -a "$MNT/." "$WORK/ref/fs-b.tree/"
awk '$1 >= 65536 {bad++} END {exit (bad+0) > 0}' "$WORK/ref/fs-b.inos" \
    || { echo "FAIL: fs-b has a regular file outside AG0"; exit 1; }
sudo umount "$MNT"
echo "    $(wc -l < "$WORK/ref/fs-b.inos") files (incl. hardlink), all in AG0"

echo "  fs-c.xfs (default mkfs: rmapbt+reflink+nrext64 -> declined)"
mkxfs "$WORK/orig/fs-c.xfs"
sudo mount -o loop "$WORK/orig/fs-c.xfs" "$MNT"
sudo chown user:user "$MNT"
echo "default features file" > "$MNT/file.txt"
sync
sudo umount "$MNT"

echo "  fs-d.xfs (regular file in AG1 -> ino > 65535 -> declined)"
mkxfs "$WORK/orig/fs-d.xfs" -m crc=1,finobt=1,rmapbt=0,reflink=0,inobtcount=0,bigtime=1 \
    -i size=512,sparse=0,nrext64=0 -n ftype=1
sudo mount -o loop "$WORK/orig/fs-d.xfs" "$MNT"
sudo chown user:user "$MNT"
echo "ag0 file" > "$MNT/ag0.txt"
i=0
while :; do
    mkdir "$MNT/d$i"
    ino=$(stat -c %i "$MNT/d$i")
    [ "$ino" -ge 65536 ] && break
    i=$((i + 1)); [ "$i" -gt 40 ] && { echo "FAIL: no AG1 dir after 40 tries"; exit 1; }
done
echo "ag1 file (ino ${ino}000-ish)" > "$MNT/d$i/ag1.txt"
sync
sudo umount "$MNT"
echo "    ag1.txt lives under dir ino $ino"

echo "  junk.xfs (a text file carrying the extension)"
python3 -c "open('$WORK/orig/junk.xfs','w').write('this is not a filesystem image, just text\n' * 40)"

UMOUNTED=1
trap - EXIT

echo "== hand self-test: strip/rebuild bit-exact, map guard emulation =="
X=$PACK/bin/xfs
python3 - "$WORK" <<'PY'
import os, struct, subprocess, sys

WORK = sys.argv[1]
X = os.path.join(os.environ.get('PACK', '/home/user/InvariantFS/tools/codecpacks/xfs.codecpack'), 'bin', 'xfs')

def run(*args, want=0):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != want:
        print(f'FAIL: {args} -> rc {r.returncode} (want {want})',
              r.stderr.decode()[:400])
        sys.exit(1)
    return r

def roundtrip(name):
    img = f'{WORK}/orig/{name}'
    tab = f'{WORK}/out/{name}.tab'
    rec = f'{WORK}/out/{name}.recipe'
    mp = f'{WORK}/out/{name}.map'
    out = f'{WORK}/out/{name}.rebuilt'
    mdir = f'{WORK}/out/{name}.mbr'
    os.makedirs(mdir, exist_ok=True)
    run(X, 'enumerate', img, tab)
    mem = {}
    for line in open(tab):
        idx, sname, usize = line.rstrip('\n').split('\t')
        mem[int(idx)] = (sname, int(usize))
    assert mem, 'no members'
    run(X, 'strip', img, rec)
    run(X, 'map', img, mp)
    with open(f'{WORK}/out/{name}.sizes', 'w') as sf:
        sf.write(f'{os.path.getsize(rec)} {sum(u for _s, u in mem.values())} '
                 f'{os.path.getsize(mp)} {os.path.getsize(tab)}\n')
    for idx, (sname, usize) in mem.items():
        run(X, 'extract', img, str(idx), f'{mdir}/{idx}')
        assert os.path.getsize(f'{mdir}/{idx}') == usize, \
            f'extract size mismatch for {idx}'
    run(X, 'rebuild', rec, mdir, out)
    a = open(img, 'rb').read()
    b = open(out, 'rb').read()
    assert a == b, f'{name}: rebuild not bit-exact'
    # MRMP check: the FS-side validation + map guard, emulated
    data = open(mp, 'rb').read()
    assert data[:4] == b'MRMP'
    (n,) = struct.unpack_from('<I', data, 4)
    assert len(data) == 8 + n * 29
    rh = open(rec, 'rb').read(32)
    assert rh[:8] == b'XFSRCP01'
    image_size, nmem, nran = struct.unpack_from('<QII', rh, 8)
    assert image_size == len(a) and nmem == len(mem) and nran == n
    payload = 32 + nmem * 16 + nran * 40
    rf = open(rec, 'rb')
    imf = open(img, 'rb')
    pos = 0
    run_off = payload
    nmbr = 0
    for i in range(n):
        orig, ln, kind, idx, src = struct.unpack_from('<QQBIQ', data, 8 + i * 29)
        assert orig == pos and ln > 0, f'{name}: map does not partition'
        if kind == 0:
            assert idx == 0
            rf.seek(run_off)
            assert rf.read(ln) == a[orig:orig + ln], 'recipe bytes differ'
            run_off += ln
        else:
            assert idx in mem and src + ln <= mem[idx][1]
            with open(f'{mdir}/{idx}', 'rb') as mf:
                mf.seek(src)
                assert mf.read(ln) == a[orig:orig + ln], 'member bytes differ'
            nmbr += 1
        pos += ln
    assert pos == image_size
    print(f'  {name}: {len(mem)} members, {n} map ranges ({nmbr} member), '
          f'recipe {os.path.getsize(rec)} B, rebuild bit-exact')
    # keep only .tab/.map for the later legs; the big blobs go
    os.unlink(rec)
    os.unlink(out)
    import shutil
    shutil.rmtree(mdir)

roundtrip('fs-a.xfs')
roundtrip('fs-b.xfs')
PY

echo "== hand self-test: refusals (exit 3) =="
for f in fs-c.xfs fs-d.xfs junk.xfs; do
    rc=0
    $X enumerate "$WORK/orig/$f" /dev/null 2>"$WORK/out/$f.err" || rc=$?
    [ "$rc" = 3 ] || { echo "FAIL: $f: want exit 3, got $rc"; exit 1; }
    echo "  $f declined: $(head -1 "$WORK/out/$f.err")"
done

echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 2 >/dev/null
for f in fs-a.xfs fs-b.xfs junk.xfs; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-mkfs "$IMGNEG" 2 >/dev/null
for f in fs-c.xfs fs-d.xfs; do
    $B/invf-cp "$IMGNEG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== admission leg 1: default ARC (256M) -> GENERIC_MEMLIMIT{19} =="
$B/invf-mkfs "$IMGMEM" 2 >/dev/null
$B/invf-cp "$IMGMEM" "$WORK/orig/fs-a.xfs" fs-a.xfs >/dev/null
$B/invf-sweep "$IMGMEM" > "$WORK/sweep-mem.log" 2>&1 || { cat "$WORK/sweep-mem.log"; exit 1; }
if grep -q "xfs (codecpack)" "$WORK/sweep-mem.log"; then
    echo "FAIL: decomposition ran under the default ARC budget"; exit 1
fi
C=$("$WORK/classof" "$IMGMEM" fs-a.xfs)
echo "  fs-a.xfs (default ARC): $C"
[ "$C" = "cls=5 algo=19 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{XFS=19,1}"; exit 1; }
$B/invf-cat "$IMGMEM" fs-a.xfs "$WORK/out/mem1.xfs" >/dev/null
cmp -s "$WORK/orig/fs-a.xfs" "$WORK/out/mem1.xfs" \
    || { echo "FAIL: memlimit read not bit-exact"; exit 1; }
rm -f "$WORK/out/mem1.xfs"
echo "  300M container waits RAW, stamped, bit-exact"

echo "== admission leg 2: INVFS_DEC_MEM_LIMIT=64K (estimate gate) =="
$B/invf-mkfs "$IMGMEM2" 2 >/dev/null
$B/invf-cp "$IMGMEM2" "$WORK/orig/fs-a.xfs" fs-a.xfs >/dev/null
INVFS_ARC_BYTES=1G INVFS_DEC_MEM_LIMIT=64K $B/invf-sweep "$IMGMEM2" \
    > "$WORK/sweep-mem2.log" 2>&1 || { cat "$WORK/sweep-mem2.log"; exit 1; }
if grep -q "xfs (codecpack)" "$WORK/sweep-mem2.log"; then
    echo "FAIL: decomposition ran under a 64K decode-memory limit"; exit 1
fi
C=$("$WORK/classof" "$IMGMEM2" fs-a.xfs)
echo "  fs-a.xfs (dec_mem 64K): $C"
[ "$C" = "cls=5 algo=19 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{XFS=19,1}"; exit 1; }
$B/invf-cat "$IMGMEM2" fs-a.xfs "$WORK/out/mem2.xfs" >/dev/null
cmp -s "$WORK/orig/fs-a.xfs" "$WORK/out/mem2.xfs" \
    || { echo "FAIL: memlimit read not bit-exact"; exit 1; }
rm -f "$WORK/out/mem2.xfs"
echo "  estimate sum+64MiB is the admission input; policy refusal bit-exact"

echo "== sweep (INVFS_ARC_BYTES=1G): decomposition =="
INVFS_ARC_BYTES=1G $B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 \
    || { cat "$WORK/sweep1.log"; exit 1; }
grep -q "fs-a.xfs: xfs (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: fs-a.xfs not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
grep -q "fs-b.xfs: xfs (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: fs-b.xfs not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
if grep -q "junk.xfs: xfs" "$WORK/sweep1.log"; then
    echo "FAIL: junk.xfs was decomposed"; exit 1
fi
grep -E "fs-a\.xfs!\*: [0-9]+ parts -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: text members not PPMd-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -E "fs-a\.xfs!\*: [0-9]+ parts -> ZSTD batch" "$WORK/sweep1.log" \
    || { echo "FAIL: binary members not ZSTD-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -E "codecpack|parts -> " "$WORK/sweep1.log"

echo "== sibling sets (members + table + map) =="
NA=$(wc -l < "$WORK/out/fs-a.xfs.tab")
NB=$(wc -l < "$WORK/out/fs-b.xfs.tab")
LS=$($B/invf-ls "$IMG")
CA=$(grep -c "fs-a\.xfs!" <<<"$LS" || true)
CB=$(grep -c "fs-b\.xfs!" <<<"$LS" || true)
echo "  fs-a.xfs: $NA members -> $CA siblings (want $((NA + 2)))"
echo "  fs-b.xfs: $NB members -> $CB siblings (want $((NB + 2)))"
[ "$CA" = "$((NA + 2))" ] || { echo "FAIL: fs-a sibling count"; $B/invf-ls "$IMG"; exit 1; }
[ "$CB" = "$((NB + 2))" ] || { echo "FAIL: fs-b sibling count"; $B/invf-ls "$IMG"; exit 1; }
grep -q "fs-a\.xfs!mbrt" <<<"$LS" || { echo "FAIL: no member table"; exit 1; }
grep -q "fs-a\.xfs!mbrmap" <<<"$LS" || { echo "FAIL: no member map"; exit 1; }
grep "fs-a\.xfs!mbr.*empty" <<<"$LS" | grep -q "0 bytes" \
    || { echo "FAIL: empty member missing/not 0 bytes"; $B/invf-ls "$IMG"; exit 1; }
if grep -q "junk\.xfs!" <<<"$LS"; then
    echo "FAIL: refused junk.xfs left siblings"; exit 1
fi
# the hardlink pair is ONE member
HC=$(grep -c "fs-a\.xfs!mbr.*hard" <<<"$LS" || true)
[ "$HC" = 1 ] || { echo "FAIL: hardlink pair produced $HC members (want 1)"; exit 1; }
echo "  table + map present; empty member 0 bytes; hardlink pair is one member"

echo "== class stamps =="
C=$("$WORK/classof" "$IMG" fs-a.xfs); echo "  fs-a.xfs: $C"
[ "$C" = "cls=3 algo=19 gen=1" ] || { echo "FAIL: want CONTAINER{XFS=19,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" fs-b.xfs); echo "  fs-b.xfs: $C"
[ "$C" = "cls=3 algo=19 gen=1" ] || { echo "FAIL: want CONTAINER{XFS=19,1}"; exit 1; }
sib() {  # sib <container> <idx> <sname> -> the member sibling name
    printf "%s!mbr%04u-%s" "$1" "$2" "$3"
}
TXTINO=$(awk -F'\t' '$2 == "text0.c" {print $1}' "$WORK/out/fs-a.xfs.tab")
ELFINO=$(awk -F'\t' '$2 == "prog.elf" {print $1}' "$WORK/out/fs-a.xfs.tab")
[ -n "$TXTINO" ] && [ -n "$ELFINO" ] || { echo "FAIL: members missing from table"; exit 1; }
C=$("$WORK/classof" "$IMG" "$(sib fs-a.xfs "$TXTINO" text0.c)")
echo "  text0.c member: $C"
[ "$C" = "cls=7 algo=2 gen=1" ] || { echo "FAIL: want TEXT{PPMD,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" "$(sib fs-a.xfs "$ELFINO" prog.elf)")
echo "  prog.elf member: $C"
[ "$C" = "cls=8 algo=14 gen=1" ] || { echo "FAIL: want BATCHED_BIN{ZSTD_BCJ,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" junk.xfs)
echo "  junk.xfs: $C"
case "$C" in *algo=19*|*cls=3*) echo "FAIL: junk.xfs carries a pack stamp"; exit 1;; esac

echo "== verify --deep (every container through its map) =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== sha256 bit-exact (containers + direct member reads) =="
ok=1
for f in fs-a.xfs fs-b.xfs junk.xfs; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f.got" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f.got" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
[ "$ok" = 1 ] || exit 1
rm -f "$WORK/out/fs-a.xfs.got" "$WORK/out/fs-b.xfs.got" "$WORK/out/junk.xfs.got"
echo "  containers bit-exact"
# members are real inodes: read every single one through its stored form
# (PPMd batch slice / ZSTD-BCJ batch slice / RAW) and compare against the
# bytes on the original filesystem
python3 - "$WORK" "$IMG" "$B/invf-cat" <<'PY'
import subprocess, sys

WORK, IMG, CAT = sys.argv[1:4]
bad = 0
for img, tag in (('fs-a.xfs', 'fs-a'), ('fs-b.xfs', 'fs-b')):
    host = {}
    for line in open(f'{WORK}/ref/{tag}.inos'):
        ino, rel = line.split(' ', 1)
        host.setdefault(int(ino), rel.rstrip('\n'))
    mnt_src = {}
    for line in open(f'{WORK}/out/{img}.tab'):
        idx, sname, usize = line.rstrip('\n').split('\t')
        mnt_src[int(idx)] = (sname, int(usize))
    for idx, (sname, usize) in mnt_src.items():
        rel = host.get(idx)
        if rel is None:
            print(f'  MISMATCH: member {idx} not in host map'); bad += 1
            continue
        ref = open(f'{WORK}/ref/{tag}.tree/{rel}', 'rb').read()
        sib = f'{img}!mbr{idx:04d}-{sname}'
        r = subprocess.run([CAT, IMG, sib, f'{WORK}/out/mbr.got'],
                           stderr=subprocess.DEVNULL)
        got = open(f'{WORK}/out/mbr.got', 'rb').read() \
            if r.returncode == 0 else b''
        if got != ref or len(got) != usize:
            print(f'  MISMATCH member {idx} ({rel})'); bad += 1
    print(f'  {img}: {len(mnt_src)} member direct reads bit-exact'
          + ('' if not bad else ' -- FAILURES'))
sys.exit(1 if bad else 0)
PY

echo "== ranged reads (the WP16b local splice) =="
# pick ranges straight out of the pack's map: mid-ELF-member, a
# recipe->member boundary, the container tail, the superblock head
python3 - "$WORK" <<'PY'
import struct, sys
WORK = sys.argv[1]
data = open(f'{WORK}/out/fs-a.xfs.map', 'rb').read()
(n,) = struct.unpack_from('<I', data, 4)
ents = [struct.unpack_from('<QQBIQ', data, 8 + i * 29) for i in range(n)]
size = sum(e[1] for e in ents)
# member ranges of the ELF member
tab = {}
for line in open(f'{WORK}/out/fs-a.xfs.tab'):
    idx, sname, usize = line.rstrip('\n').split('\t')
    tab[sname] = (int(idx), int(usize))
elf = tab['prog.elf'][0]
m = max((e for e in ents if e[2] == 1 and e[3] == elf), key=lambda e: e[1])
# strictly inside the ELF member's longest extent
o1 = m[0] + m[1] // 4
l1 = min(65536, m[1] - m[1] // 4)
# a recipe->member boundary: first member range with orig > 0
b = next(e for e in ents if e[2] == 1)
with open(f'{WORK}/out/ranges.txt', 'w') as f:
    f.write(f'{o1} {l1}\n')
    f.write(f'{b[0] - 64} 128\n')
    f.write(f'{size - 1000} 1000\n')
    f.write('0 64\n')
print(f'  ranges: mid-elf +{o1}/{l1}, boundary +{b[0]-64}/128, tail, head')
PY
i=0
while read -r off len; do
    i=$((i + 1))
    dd if="$WORK/orig/fs-a.xfs" of="$WORK/out/ref.r$i" bs=1 skip="$off" count="$len" 2>/dev/null
    "$WORK/rngread" "$IMG" fs-a.xfs "$off" "$len" "$WORK/out/got.r$i" >/dev/null
    cmp -s "$WORK/out/ref.r$i" "$WORK/out/got.r$i" \
        || { echo "FAIL: range $i (off=$off len=$len) mismatch"; exit 1; }
done < "$WORK/out/ranges.txt"
"$WORK/rngread" "$IMG" fs-a.xfs 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp -s "$WORK/orig/fs-a.xfs" "$WORK/out/whole.rng" \
    || { echo "FAIL: whole file via 64K ranged windows mismatch"; exit 1; }
rm -f "$WORK/out/whole.rng"
echo "  mid-member / boundary / tail / head / whole-by-windows all exact"

echo "== pack-ABSENT reads still work (the map is self-describing) =="
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" fs-a.xfs "$WORK/out/absent" >/dev/null
cmp -s "$WORK/orig/fs-a.xfs" "$WORK/out/absent" \
    || { echo "FAIL: pack-absent whole read not bit-exact"; exit 1; }
rm -f "$WORK/out/absent"
read -r off len < "$WORK/out/ranges.txt"
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" fs-a.xfs "$off" "$len" \
    "$WORK/out/absent.rng" >/dev/null
cmp -s "$WORK/out/ref.r1" "$WORK/out/absent.rng" \
    || { echo "FAIL: pack-absent ranged read mismatch"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" fs-b.xfs "$WORK/out/absentb" >/dev/null
cmp -s "$WORK/orig/fs-b.xfs" "$WORK/out/absentb" \
    || { echo "FAIL: pack-absent fs-b read not bit-exact"; exit 1; }
rm -f "$WORK/out/absentb"
echo "  pack-absent: whole + ranged reads bit-exact, zero pack exec"

echo "== sweep #2: idempotent =="
INVFS_ARC_BYTES=1G $B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 \
    || { cat "$WORK/sweep2.log"; exit 1; }
if grep -q "codecpack" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-fired the containerpack"; cat "$WORK/sweep2.log"; exit 1
fi
grep -q " 0 corrupt," <($B/invf-verify "$IMG" --deep) || { echo "FAIL: corrupt"; exit 1; }
echo "  no re-decomposition"

echo "== ratio demo (informational) =="
python3 - "$WORK" <<'PY'
import os, sys
WORK = sys.argv[1]
for img in ('fs-a.xfs', 'fs-b.xfs'):
    orig = os.path.getsize(f'{WORK}/orig/{img}')
    rec, musz, msz, tsz = (int(x) for x in
                           open(f'{WORK}/out/{img}.sizes').read().split())
    nm = sum(1 for _ in open(f'{WORK}/out/{img}.tab'))
    print(f'  {img}: {orig} B ({orig >> 20} MiB) decomposes into:')
    print(f'    recipe blob  {rec:>12} B  (every non-member byte verbatim:'
          f' superblocks, AG headers, B+trees, inode chunks, dir blocks,')
    print(f'                 {"":>12}    the log, free space -- mostly zeros'
          f' for a fresh image)')
    print(f'    {nm} members    {musz:>12} B  (the file contents, now flowing'
          f' through PPMd/ZSTD batching + dedupe as first-class files)')
    print(f'    table + map  {tsz + msz:>12} B')
    print(f'    => the pack itself stores {rec + musz + tsz + msz} B'
          f' ({(rec + musz + tsz + msz) / orig:.3f}x) before the pipeline;'
          f' the win is member-level compression/dedupe, not the recipe')
PY

echo "== delete cascade =="
"$WORK/cbrm" "$IMG" fs-a.xfs fs-b.xfs
LS=$($B/invf-ls "$IMG")
if grep -q "fs-a\.xfs!\|fs-b\.xfs!" <<<"$LS"; then
    echo "FAIL: !mbr/!mbrt/!mbrmap siblings survived the delete"; $B/invf-ls "$IMG"; exit 1
fi
echo "  containers + all members + tables + maps deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivor bit-exact =="
$B/invf-cat "$IMG" junk.xfs "$WORK/out/junk2.xfs" >/dev/null
cmp -s "$WORK/orig/junk.xfs" "$WORK/out/junk2.xfs" \
    || { echo "FAIL: junk.xfs drifted after deletes"; exit 1; }

echo "== refusal legs (rmapbt/reflink/nrext64; AG1 member) =="
INVFS_ARC_BYTES=1G $B/invf-sweep "$IMGNEG" > "$WORK/sweep-neg.log" 2>&1 \
    || { cat "$WORK/sweep-neg.log"; exit 1; }
if grep -q "xfs (codecpack)" "$WORK/sweep-neg.log"; then
    echo "FAIL: a refused image was decomposed"; cat "$WORK/sweep-neg.log"; exit 1
fi
for f in fs-c.xfs fs-d.xfs; do
    C=$("$WORK/classof" "$IMGNEG" "$f")
    echo "  $f: $C"
    case "$C" in *algo=19*|*cls=3*) echo "FAIL: $f carries a pack stamp"; exit 1;; esac
    LS=$($B/invf-ls "$IMGNEG")
    if grep -q "$f!" <<<"$LS"; then
        echo "FAIL: $f left siblings"; exit 1
    fi
    $B/invf-cat "$IMGNEG" "$f" "$WORK/out/$f.got" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.got" \
        || { echo "FAIL: $f not bit-exact"; exit 1; }
    rm -f "$WORK/out/$f.got"
done
grep -q " 0 corrupt," <($B/invf-verify "$IMGNEG" --deep) \
    || { echo "FAIL: verify on refusal image not clean"; exit 1; }
grep -q "orphans:      0" <($B/invf-fsck "$IMGNEG") \
    || { echo "FAIL: fsck on refusal image reports orphans"; exit 1; }
echo "  declined images store generic, bit-exact, fsck clean"

echo "XFS CONTAINERPACK E2E: PASS"
