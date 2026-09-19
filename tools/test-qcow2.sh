#!/bin/bash
# test-qcow2.sh — the qcow2 containerpack (tools/codecpacks/qcow2.codecpack,
# algo 22) end-to-end (persistent regression). WP16a containerpack ABI +
# WP16b seekable (map) flow over QEMU qcow2 disk images.
#
#   fixtures (inline python3 for the HAND-BUILT ones — spec-correct qcow2
#   v2/v3 incl. refcounts, cross-checked against qemu-img check/info; plus
#   qemu-img/qemu-io-generated ones as external ground truth):
#     diska.qcow2 — hand-built v3, 64K clusters, 8 of 256 guest clusters
#       allocated (text/ELF/zeros/random), slot-scrambled, junk-filled free
#       clusters inside the file, trailing junk. Guest cluster 0 is text so
#       the member stream batches (the member-batching leg).
#     diskb.qcow2 — hand-built v3: the virtual disk's first 4 MiB are a
#       coherent raw disk image (GPT + one 2 MiB Linux partition; no 55AA
#       protective MBR on purpose — see the fixture comment), clusters 0-63
#       allocated, so the member stream IS that disk (the composition leg).
#     disks.qcow2 — hand-built v3 with 512-BYTE clusters (cluster_bits 9),
#       clusters spread over many L2 tables (multi-L1-entry coverage).
#     diskv2.qcow2 — hand-built v2 (72-byte header, 4K clusters).
#     qgen.qcow2  — qemu-img/qemu-io generated, contiguous writes + marker:
#       external ground truth; member == raw prefix (qemu-img convert
#       cross-checks the virtual content).
#     qzero.qcow2 — qemu-img/qemu-io generated with ZERO clusters
#       (write -z): a zero cluster WITH a kept extent (stale bytes stay
#       recipe) and a zero cluster with no extent.
#     decline set — badmagic/backed/snap/compc/extl2/enc/cbits/dupcl/
#       pasteof/empty: all must be DECLINED (exit 3 at the pack, generic
#       fall-through in the FS, no siblings, no stamp).
#   pack-level round-trip (enumerate/extract/strip/map/rebuild/estimate +
#   memcmp + an MRMP partition re-verification in python) runs on every
#   fixture BEFORE the FS legs (the pack is its own first arbiter), plus a
#   30-layout fuzz-lite. FS legs: mkfs -> cp -> invf-sweep with
#   INVFS_CODECPACKS=$WORK/packs (a symlink dir holding qcow2 + rawdisk —
#   the repo's shared pack dir is full at INVFS_PACK_MAX=8, so the shared
#   dir is NOT used here; deterministic registration) -> CONTAINER{22,1}
#   stamps -> verify --deep -> sha256 bit-exact (containers AND direct
#   member reads) -> RANGED reads through the MRMP splice -> pack-ABSENT
#   reads still bit-exact -> sweep 2 stores the members + decomposes
#   diskb's member (a GPT disk) via the rawdisk pack when its tool
#   resolves -> sweep 3 idle -> delete cascade -> fsck clean -> survivors
#   bit-exact. Admission leg: fresh image, INVFS_ARC_BYTES=1M ->
#   GENERIC_MEMLIMIT{22,1} (cls=5), no decomposition, bit-exact.
#
# Run from the repo root after `make`:  bash tools/test-qcow2.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
export REPO
B=$REPO/bin
PACK=$REPO/tools/codecpacks/qcow2.codecpack
WORK=/dev/shm/wp16qcow2
trap 'rm -rf "$WORK" /dev/shm/wp16qcow2*.img' EXIT
IMG=wp16qcow2.img
IMGMEM=wp16qcow2-mem.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/nopacks" "$WORK/bin" "$WORK/packs"
cd /dev/shm
rm -f "$IMG" "$IMGMEM"

echo "== tools =="
command -v cc >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
command -v qemu-img >/dev/null || { echo "SKIP: qemu-img not installed"; exit 0; }
command -v qemu-io >/dev/null || { echo "FAIL: qemu-io not installed"; exit 1; }
echo "  qemu-img: $(qemu-img --version | head -1)"

echo "== build the pack (the -Wall -Wextra -Werror gate) =="
mkdir -p "$PACK/bin"
WARN=$(cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACK/bin/qcow2" "$PACK/qcow2.c" 2>&1) \
    || { echo "FAIL: pack build failed"; echo "$WARN"; exit 1; }
[ -z "$WARN" ] || { echo "FAIL: pack build not warning-clean:"; echo "$WARN"; exit 1; }
echo "  bin/qcow2 built, -Wall -Wextra -Werror clean"

echo "== pack dir: symlinks (the shared dir is full at INVFS_PACK_MAX=8) =="
ln -s "$PACK" "$WORK/packs/qcow2.codecpack"
RAWDISK=$REPO/tools/codecpacks/rawdisk.codecpack
if [ -f "$RAWDISK/manifest" ]; then
    ln -s "$RAWDISK" "$WORK/packs/rawdisk.codecpack"
    echo "  qcow2 + rawdisk linked into \$WORK/packs"
else
    echo "  qcow2 linked into \$WORK/packs (no rawdisk pack — nested leg will skip)"
fi
export INVFS_CODECPACKS=$WORK/packs   # the sweep AND the reads

echo "== composition target: resolve the rawdisk pack's tool =="
# The nested-decomposition leg needs the rawdisk pack's helper. Its manifest
# resolves the bare name "rawdisk" from <pack>/bin/ first, then PATH — so a
# build into $WORK/bin + a PATH prepend exercises the REAL composition
# without touching the rawdisk pack directory. Best effort: if the pack is
# absent or its source does not build here, the leg reports and skips.
if [ ! -x "$RAWDISK/bin/rawdisk" ] && ! command -v rawdisk >/dev/null 2>&1 \
        && [ -f "$RAWDISK/rawdisk.c" ]; then
    if cc -std=c11 -O2 -Wall -Wextra -o "$WORK/bin/rawdisk" \
            "$RAWDISK/rawdisk.c" 2> "$WORK/rawdisk-build.log"; then
        export PATH="$WORK/bin:$PATH"
        echo "  rawdisk.c built into \$WORK/bin (PATH-resolved, pack dir untouched)"
    else
        echo "  NOTE: rawdisk.c present but does not build here (parallel work"
        echo "        in flight?) — nested leg will report and skip:"
        sed 's/^/    /' "$WORK/rawdisk-build.log" | head -5
    fi
elif [ -x "$RAWDISK/bin/rawdisk" ]; then
    echo "  rawdisk pack tool present at $RAWDISK/bin/rawdisk"
elif command -v rawdisk >/dev/null 2>&1; then
    echo "  rawdisk resolved on PATH: $(command -v rawdisk)"
else
    echo "  NOTE: no rawdisk pack/tool — nested leg will report and skip"
fi

echo "== generate fixtures =="
python3 - "$WORK/orig" <<'PY'
import os
import random
import struct
import sys
import zlib

d = sys.argv[1]
rnd = random.Random(22)
MB = 1048576


def build_qcow2(cluster_bits, virtual_size, clusters, version=3,
                seed=1, extra_junk_slots=0, tail=b""):
    """Hand-built, spec-correct qcow2 (refcounts included).
    clusters: {guest_cluster: bytes(cluster_size)}.
    Returns (image_bytes, member_stream, info)."""
    cs = 1 << cluster_bits
    l2_per = cs // 8
    r = random.Random(seed)
    n_guest = (virtual_size + cs - 1) // cs
    l1_size = max(1, (n_guest + l2_per - 1) // l2_per)
    alloc = sorted(clusters)
    l1_used = sorted(set(g // l2_per for g in alloc))

    n_items = len(l1_used) + len(alloc) + extra_junk_slots
    nrb = 1
    while True:                              # refcount blocks cover the file
        first_slot = 2 + nrb + 1
        need = (first_slot + n_items + cs // 2 - 1) // (cs // 2)
        if need == nrb:
            break
        nrb = need
    first_slot = 2 + nrb + 1

    pool = [('l2', x) for x in l1_used] + [('data', g) for g in alloc] + \
           [('junk', i) for i in range(extra_junk_slots)]
    r.shuffle(pool)
    slot_of = {}
    used_slots = set(range(first_slot))
    for i, item in enumerate(pool):
        slot_of[item] = first_slot + i
        if item[0] != 'junk':
            used_slots.add(first_slot + i)   # junk slots stay rc=0 (free
                                             # clusters with stale garbage)
    n_clusters_file = first_slot + len(pool)

    l1_off = (2 + nrb) * cs
    rt_off = cs
    rt = [0] * (cs // 8)
    for b in range(nrb):
        rt[b] = (2 + b) * cs
    rb = [bytearray(cs) for _ in range(nrb)]
    for s in used_slots:
        struct.pack_into(">H", rb[s // (cs // 2)], (s % (cs // 2)) * 2, 1)

    l1 = [0] * l1_size
    l2tabs = {}
    for x in l1_used:
        l2 = [0] * l2_per
        for g in alloc:
            if g // l2_per == x:
                l2[g % l2_per] = 0x8000000000000000 | slot_of[('data', g)] * cs
        l2tabs[x] = l2
        l1[x] = 0x8000000000000000 | slot_of[('l2', x)] * cs

    hdr = bytearray(cs)
    hdr[0:4] = b"QFI\xfb"
    struct.pack_into(">I", hdr, 4, version)
    struct.pack_into(">I", hdr, 20, cluster_bits)
    struct.pack_into(">Q", hdr, 24, virtual_size)
    struct.pack_into(">I", hdr, 36, l1_size)
    struct.pack_into(">Q", hdr, 40, l1_off)
    struct.pack_into(">Q", hdr, 48, rt_off)
    struct.pack_into(">I", hdr, 56, 1)       # refcount_table_clusters
    if version == 3:
        struct.pack_into(">I", hdr, 96, 4)   # refcount_order (16 bits)
        struct.pack_into(">I", hdr, 100, 104)  # header_length

    img = bytearray(n_clusters_file * cs)
    img[0:cs] = hdr
    struct.pack_into(">%dQ" % len(rt), img, rt_off, *rt)
    for b in range(nrb):
        img[(2 + b) * cs:(3 + b) * cs] = rb[b]
    struct.pack_into(">%dQ" % l1_size, img, l1_off, *l1)
    for x, l2 in l2tabs.items():
        struct.pack_into(">%dQ" % l2_per, img, slot_of[('l2', x)] * cs, *l2)
    for g in alloc:
        s = slot_of[('data', g)]
        img[s * cs:(s + 1) * cs] = clusters[g]
    for i in range(extra_junk_slots):
        s = slot_of[('junk', i)]
        img[s * cs:(s + 1) * cs] = r.randbytes(cs)
    out = bytes(img) + tail
    member = b"".join(clusters[g] for g in alloc)
    info = dict(cs=cs, n_clusters_file=n_clusters_file, slot_of=slot_of,
                alloc=alloc)
    return out, member, info


def tile(blob, n):
    return (blob * (n // len(blob) + 1))[:n]


# content sources: a real busybox C source + a real x86-64 ELF
text = None
for root, _dirs, files in os.walk(os.environ["REPO"] + "/tools/busybox-src"):  # fixture input (submodule)
    for n in sorted(files):
        if n.endswith(".c"):
            p = os.path.join(root, n)
            if os.path.getsize(p) > 20000:
                text = open(p, "rb").read()
                break
    if text:
        break
assert text, "no busybox .c fixture found"
elf = None
for p in ("/usr/bin/passwd", "/usr/bin/gpg", "/bin/ls", "/usr/bin/ls",
          "/bin/bash", "/usr/bin/bash"):
    if os.path.isfile(p) and not os.path.islink(p):
        with open(p, "rb") as f:
            h = f.read(20)
        if h[:4] == b"\x7fELF" and h[18] == 62 and os.path.getsize(p) > 65536:
            elf = open(p, "rb").read()
            break
assert elf, "no x86-64 ELF fixture found"

zeros = bytes(MB)

# --- diska.qcow2: v3, 64K clusters, 8 of 256 guest clusters allocated,
#     slot-scrambled, junk-filled free clusters, trailing junk. Guest
#     cluster 0 is TEXT so the member stream head-sniffs as text (the
#     member-batching leg). 6 junk slots push the file past 1 MiB (the
#     admission leg needs that). ------------------------------------------
CS = 65536
aclu = {0: tile(text, CS), 2: tile(elf, CS), 5: rnd.randbytes(CS),
        7: bytes(CS), 9: tile(text, CS), 13: rnd.randbytes(CS),
        15: tile(elf, CS), 17: bytes(CS)}
img, member, info = build_qcow2(16, 16 * MB, aclu, version=3, seed=22,
                                extra_junk_slots=6, tail=rnd.randbytes(1000))
open(os.path.join(d, "diska.qcow2"), "wb").write(img)
open(os.path.join(d, "member-a.ref"), "wb").write(member)
slot_of = info["slot_of"]
datas = sorted((v, k[1]) for k, v in slot_of.items() if k[0] == 'data')
junks = sorted(v for k, v in slot_of.items() if k[0] == 'junk')
with open(os.path.join(d, "diska.layout"), "w") as f:
    f.write("size=%d firstext=%d hole=%d midext=%d tail=%d\n" %
            (len(img), datas[0][0] * CS, junks[0] * CS,
             datas[4][0] * CS, len(img) - 1000))

# --- diskb.qcow2: the virtual disk's first 4 MiB are a coherent raw disk
#     image (GPT + one 2 MiB Linux-data partition); guest clusters 0-63
#     allocated, so the compacted member stream IS that disk. The GPT
#     carries NO protective-MBR 55AA on purpose: the fatfs pack sniffs
#     55AA@510 like rawdisk does, and a pack-claimed member waits on ITS
#     probe — the GPT-only signature keeps the composition channel
#     deterministically on rawdisk ("EFI PART" @ LBA1). ------------------
gpt = bytearray(4 * MB)
NLBA = 4 * MB // 512
ent = bytearray(128 * 128)
ent[0:16] = bytes.fromhex("af3dc60f83847247798e793dd69d8477de")  # Linux fs data
ent[16:32] = bytes.fromhex("00112233445566778899aabbccddeeff")
struct.pack_into('<Q', ent, 32, 2048)           # first_lba
struct.pack_into('<Q', ent, 40, 6143)           # last_lba (2 MiB)
nm = "qcow2compose".encode('utf-16-le')
ent[56:56 + len(nm)] = nm
gpt[2 * 512:2 * 512 + len(ent)] = ent
ecrc = zlib.crc32(ent) & 0xFFFFFFFF


def gpt_header(my_lba, alt_lba, ent_lba, ecrc):
    h = bytearray(512)
    h[0:8] = b"EFI PART"
    struct.pack_into('<I', h, 8, 0x00010000)
    struct.pack_into('<I', h, 12, 92)
    struct.pack_into('<Q', h, 24, my_lba)
    struct.pack_into('<Q', h, 32, alt_lba)
    struct.pack_into('<Q', h, 40, 34)
    struct.pack_into('<Q', h, 48, 8158)
    h[56:72] = bytes.fromhex("aabbccddeeff00112233445566778899")
    struct.pack_into('<Q', h, 72, ent_lba)
    struct.pack_into('<I', h, 80, 128)
    struct.pack_into('<I', h, 84, 128)
    struct.pack_into('<I', h, 88, ecrc)
    struct.pack_into('<I', h, 16, zlib.crc32(h[:92]) & 0xFFFFFFFF)
    return h


gpt[512:1024] = gpt_header(1, NLBA - 1, 2, ecrc)
gpt[8159 * 512:8159 * 512 + len(ent)] = ent     # backup entries
gpt[8191 * 512:] = gpt_header(8191, 1, 8159, ecrc)  # backup header
# deterministic content in the partition body (reads as data, not zeros)
r2 = random.Random(23)
for off in range(2048 * 512, 6144 * 512, CS):
    gpt[off:off + CS] = tile(r2.randbytes(997), CS)
bclu = {i: bytes(gpt[i * CS:(i + 1) * CS]) for i in range(64)}
img, member, info = build_qcow2(16, 16 * MB, bclu, version=3, seed=23)
open(os.path.join(d, "diskb.qcow2"), "wb").write(img)
open(os.path.join(d, "member-b.ref"), "wb").write(member)

# --- disks.qcow2: v3, 512-byte clusters (cluster_bits 9, the minimum),
#     clusters spread over many L2 tables (multi-L1 coverage) ------------
CS9 = 512
sclu = {g: rnd.randbytes(CS9) for g in
        [0, 1, 66, 67, 500, 1000, 2048, 2049, 2050, 3000, 4000]}
img, member, info = build_qcow2(9, 2 * MB, sclu, version=3, seed=24,
                                extra_junk_slots=2)
open(os.path.join(d, "disks.qcow2"), "wb").write(img)
open(os.path.join(d, "member-s.ref"), "wb").write(member)

# --- diskv2.qcow2: v2 (72-byte header), 4K clusters ----------------------
CSV2 = 4096
v2clu = {g: rnd.randbytes(CSV2) for g in [0, 3, 10, 100, 700]}
img, member, info = build_qcow2(12, 3 * MB, v2clu, version=2, seed=25,
                                extra_junk_slots=3, tail=rnd.randbytes(123))
open(os.path.join(d, "diskv2.qcow2"), "wb").write(img)
open(os.path.join(d, "member-v2.ref"), "wb").write(member)

for f in sorted(os.listdir(d)):
    if f.endswith(".qcow2"):
        print("  %-14s %d bytes" % (f, os.path.getsize(os.path.join(d, f))))
PY

echo "== qemu-img/qemu-io generated fixtures (external ground truth) =="
# qgen.qcow2: qemu-io deterministic pattern writes, interleaved
# allocated/free guest clusters (external ground truth; the member
# reference below is cross-checked against `qemu-img map`).
qemu-img create -f qcow2 "$WORK/orig/qgen.qcow2" 4M >/dev/null
qemu-io -c "write -P 0x51 0 64k" -c "write -P 0x52 128k 192k" \
    -c "write -P 0x53 1M 64k" -c "write -P 0x54 1536k 256k" \
    "$WORK/orig/qgen.qcow2" >/dev/null 2>&1
# qzero.qcow2: ZERO clusters — one with a kept extent (stale bytes stay
# recipe), one without; plus two real data clusters.
qemu-img create -f qcow2 "$WORK/orig/qzero.qcow2" 8M >/dev/null
qemu-io -c "write -P 0x61 0 64k" -c "write -P 0x62 1M 64k" \
    -c "write -z 1M 64k" -c "write -z 2M 64k" -c "write -P 0x63 3M 64k" \
    "$WORK/orig/qzero.qcow2" >/dev/null 2>&1
# independent member references for the qemu-generated fixtures: a tiny
# second implementation of the qcow2 walk (cross-checks the C parser).
python3 - "$WORK/orig" <<'PY'
import struct
import sys


def member_ref(path):
    d = open(path, "rb").read()
    be32 = lambda o: struct.unpack(">I", d[o:o + 4])[0]
    be64 = lambda o: struct.unpack(">Q", d[o:o + 8])[0]
    assert d[:4] == b"QFI\xfb"
    cs = 1 << be32(20)
    l1_size, l1_off = be32(36), be64(40)
    l2_per = cs // 8
    out = []
    for i in range(l1_size):
        e = be64(l1_off + 8 * i)
        if not e:
            continue
        l2o = e & 0x3fffffffffffffff
        for j in range(l2_per):
            le = be64(l2o + 8 * j)
            if not le or (le & (1 << 62)) or (le & 1):
                continue
            off = le & 0x3ffffffffffffffe
            out.append(d[off:off + cs])
    return b"".join(out)


for name in ("qgen", "qzero"):
    ref = member_ref(sys.argv[1] + "/%s.qcow2" % name)
    open(sys.argv[1] + "/member-%s.ref" % name, "wb").write(ref)
    print("  %s.qcow2: member reference %d bytes" % (name, len(ref)))
PY
# qemu's view of qgen's virtual disk must match the same clusters: use
# `qemu-img map` as the external allocation ground truth and rebuild the
# member from the raw virtual view, then compare against the python walk.
qemu-img convert -f qcow2 -O raw "$WORK/orig/qgen.qcow2" "$WORK/out/qgen-vdisk.raw"
python3 - "$WORK" <<'PY'
import json
import subprocess
import sys
w = sys.argv[1]
raw = open(w + "/out/qgen-vdisk.raw", "rb").read()
ref = open(w + "/orig/member-qgen.ref", "rb").read()
mp = json.loads(subprocess.run(
    ["qemu-img", "map", "--output=json", w + "/orig/qgen.qcow2"],
    capture_output=True, check=True).stdout)
expect = b"".join(raw[int(e["start"]):int(e["start"]) + int(e["length"])]
                  for e in mp if e["data"])
assert expect == ref, "python walk disagrees with qemu-img map's allocation"
print("  qemu-img map/convert cross-check: python walk == qemu's allocation")
PY
ls -la "$WORK/orig"/qgen.qcow2 "$WORK/orig"/qzero.qcow2 | awk '{print "  "$9" "$5" bytes"}'

echo "== qemu cross-validation of the hand-built fixtures =="
for f in diska.qcow2 diskb.qcow2 disks.qcow2 diskv2.qcow2; do
    qemu-img info "$WORK/orig/$f" >/dev/null \
        || { echo "FAIL: qemu-img info refuses fixture $f"; exit 1; }
    qemu-img check "$WORK/orig/$f" > "$WORK/out/chk.$f" 2>&1 \
        || { echo "FAIL: qemu-img check errors on $f"; cat "$WORK/out/chk.$f"; exit 1; }
    echo "  qemu-img info+check accept $f"
done
# the virtual disk content qemu serves must match the generator's intent
qemu-img convert -f qcow2 -O raw "$WORK/orig/diskb.qcow2" "$WORK/out/diskb.raw"
python3 - "$WORK" <<'PY'
import sys
w = sys.argv[1]
raw = open(w + "/out/diskb.raw", "rb").read()
ref = open(w + "/orig/member-b.ref", "rb").read()
MB = 1048576
assert len(raw) == 16 * MB, len(raw)
assert raw[0:4 * MB] == ref, "virtual disk prefix != the GPT disk image"
assert raw[4 * MB:] == bytes(12 * MB), "unallocated tail not zeros"
assert raw[512:520] == b"EFI PART", "the member stream is a GPT disk"
print("  qemu-img convert: diskb virtual disk = GPT image + zero tail")
PY

echo "== decline fixtures =="
python3 - "$WORK/orig" <<'PY'
import os
import struct
import subprocess
import sys

d = sys.argv[1]
base = open(os.path.join(d, "diska.qcow2"), "rb").read()


def w(name, mut):
    b = bytearray(base)
    mut(b)
    open(os.path.join(d, name), "wb").write(bytes(b))


w("badmagic.qcow2", lambda b: b.__setitem__(0, 0x51 ^ 0xFF))
w("enc.qcow2", lambda b: struct.pack_into(">I", b, 32, 1))      # crypt AES
w("cbits.qcow2", lambda b: struct.pack_into(">I", b, 20, 8))    # 256B clusters
w("incompat.qcow2", lambda b: struct.pack_into(">Q", b, 72, 2))  # corrupt bit

# duplicate cluster reference: point a second L2 entry at the first data
# cluster's offset
b = bytearray(base)
l1 = struct.unpack(">Q", b[40:48])[0]
l2 = struct.unpack(">Q", b[l1:l1 + 8])[0] & 0x3FFFFFFFFFFFFFFF
e0 = struct.unpack(">Q", b[l2:l2 + 8])[0]
struct.pack_into(">Q", b, l2 + 8, e0)    # guest cluster 1 -> same extent as 0
open(os.path.join(d, "dupcl.qcow2"), "wb").write(bytes(b))

# data cluster past EOF
b = bytearray(base)
struct.pack_into(">Q", b, l2 + 8, 0x8000000000400000)
open(os.path.join(d, "pasteof.qcow2"), "wb").write(bytes(b))

# qemu-generated: backing file / internal snapshot / compressed / extended
# L2 / refcount_bits=32 / empty (no writes)
subprocess.run(["qemu-img", "create", "-f", "qcow2", d + "/bb.qcow2", "4M"],
               capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", "-b", d + "/bb.qcow2",
                "-F", "qcow2", d + "/backed.qcow2"], capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", d + "/snap.qcow2", "4M"],
               capture_output=True)
subprocess.run(["qemu-io", "-c", "write -P 1 0 64k", d + "/snap.qcow2"],
               capture_output=True)
subprocess.run(["qemu-img", "snapshot", "-c", "s1", d + "/snap.qcow2"],
               capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", d + "/comp.qcow2", "4M"],
               capture_output=True)
subprocess.run(["qemu-io", "-c", "write -P 0x7f 0 64k", d + "/comp.qcow2"],
               capture_output=True)
subprocess.run(["qemu-img", "convert", "-f", "qcow2", "-O", "qcow2", "-c",
                d + "/comp.qcow2", d + "/compc.qcow2"], capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", "-o", "extended_l2=on",
                d + "/extl2.qcow2", "4M"], capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", "-o", "refcount_bits=32",
                d + "/rb32.qcow2", "4M"], capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", d + "/empty.qcow2", "1M"],
               capture_output=True)
os.unlink(d + "/bb.qcow2")
os.unlink(d + "/comp.qcow2")
print("  decline fixtures written")
PY

echo "== pack-level round-trip (the pack is its own first arbiter) =="
Q=$PACK/bin/qcow2
ok=1
for f in diska diskb disks diskv2 qgen qzero; do
    rm -rf "$WORK/pk" && mkdir -p "$WORK/pk/mbr"
    "$Q" enumerate "$WORK/orig/$f.qcow2" "$WORK/pk/table" \
        || { echo "FAIL: enumerate $f"; ok=0; continue; }
    [ "$(cat "$WORK/pk/table")" = "1	diskimg	$(stat -c%s "$WORK/orig/member-${f:4}.ref" 2>/dev/null || stat -c%s "$WORK/orig/member-$f.ref")" ] \
        || { echo "FAIL: $f table: $(cat "$WORK/pk/table")"; ok=0; }
    "$Q" extract "$WORK/orig/$f.qcow2" 1 "$WORK/pk/mbr/1" \
        || { echo "FAIL: extract $f"; ok=0; }
    REF="$WORK/orig/member-${f:4}.ref"; [ -f "$REF" ] || REF="$WORK/orig/member-$f.ref"
    cmp -s "$REF" "$WORK/pk/mbr/1" \
        || { echo "FAIL: $f member stream mismatch"; ok=0; }
    "$Q" strip "$WORK/orig/$f.qcow2" "$WORK/pk/recipe" || { echo "FAIL: strip $f"; ok=0; }
    "$Q" map "$WORK/orig/$f.qcow2" "$WORK/pk/map" || { echo "FAIL: map $f"; ok=0; }
    "$Q" rebuild "$WORK/pk/recipe" "$WORK/pk/mbr" "$WORK/pk/out" \
        || { echo "FAIL: rebuild $f"; ok=0; }
    cmp -s "$WORK/orig/$f.qcow2" "$WORK/pk/out" \
        || { echo "FAIL: $f rebuild not bit-exact"; ok=0; }
    "$Q" estimate "$WORK/orig/$f.qcow2" >/dev/null || { echo "FAIL: estimate $f"; ok=0; }
    # MRMP partition re-verification against the recipe + member
    python3 - "$WORK/orig/$f.qcow2" "$WORK/pk/map" "$WORK/pk/recipe" "$WORK/pk/mbr/1" <<'PY' \
        || { echo "FAIL: $f map does not partition/verify"; ok=0; }
import struct
import sys
img = open(sys.argv[1], "rb").read()
mp = open(sys.argv[2], "rb").read()
recipe = open(sys.argv[3], "rb").read()
member = open(sys.argv[4], "rb").read()
assert mp[:4] == b"MRMP"
n = struct.unpack("<I", mp[4:8])[0]
assert len(mp) == 8 + n * 29
pos = 0
for i in range(n):
    off, ln = struct.unpack("<QQ", mp[8 + i * 29:8 + i * 29 + 16])
    kind = mp[8 + i * 29 + 16]
    idx = struct.unpack("<I", mp[8 + i * 29 + 17:8 + i * 29 + 21])[0]
    src = struct.unpack("<Q", mp[8 + i * 29 + 21:8 + i * 29 + 29])[0]
    assert off == pos and ln > 0
    if kind == 0:
        assert idx == 0 and src + ln <= len(recipe)
        assert recipe[src:src + ln] == img[off:off + ln]
    else:
        assert idx == 1 and src + ln <= len(member)
        assert member[src:src + ln] == img[off:off + ln]
    pos += ln
assert pos == len(img)
sys.exit(0)
PY
    echo "  $f.qcow2: enumerate/extract/strip/map/rebuild/estimate OK, rebuild + map verified"
done
for f in badmagic backed snap compc extl2 enc cbits dupcl pasteof incompat rb32 empty; do
    if "$Q" enumerate "$WORK/orig/$f.qcow2" "$WORK/pk.table" 2>/dev/null; then
        echo "FAIL: $f.qcow2 was NOT declined"; ok=0
    fi
done
echo "  all 12 decline fixtures refused"
[ "$ok" = 1 ] || exit 1

echo "== fuzz-lite: 30 random hand-built layouts, bit-exact =="
python3 - "$WORK" <<'PY'
import os
import random
import struct
import sys

work = sys.argv[1]
fz = os.path.join(work, "fuzz")
os.makedirs(fz, exist_ok=True)


def build_qcow2(cluster_bits, virtual_size, clusters, version=3,
                seed=1, extra_junk_slots=0, tail=b""):
    cs = 1 << cluster_bits
    l2_per = cs // 8
    r = random.Random(seed)
    n_guest = (virtual_size + cs - 1) // cs
    l1_size = max(1, (n_guest + l2_per - 1) // l2_per)
    alloc = sorted(clusters)
    l1_used = sorted(set(g // l2_per for g in alloc))
    n_items = len(l1_used) + len(alloc) + extra_junk_slots
    nrb = 1
    while True:
        first_slot = 2 + nrb + 1
        need = (first_slot + n_items + cs // 2 - 1) // (cs // 2)
        if need == nrb:
            break
        nrb = need
    first_slot = 2 + nrb + 1
    pool = [('l2', x) for x in l1_used] + [('data', g) for g in alloc] + \
           [('junk', i) for i in range(extra_junk_slots)]
    r.shuffle(pool)
    slot_of = {}
    used_slots = set(range(first_slot))
    for i, item in enumerate(pool):
        slot_of[item] = first_slot + i
        if item[0] != 'junk':
            used_slots.add(first_slot + i)
    n_clusters_file = first_slot + len(pool)
    l1_off = (2 + nrb) * cs
    rt_off = cs
    rt = [0] * (cs // 8)
    for b in range(nrb):
        rt[b] = (2 + b) * cs
    rb = [bytearray(cs) for _ in range(nrb)]
    for s in used_slots:
        struct.pack_into(">H", rb[s // (cs // 2)], (s % (cs // 2)) * 2, 1)
    l1 = [0] * l1_size
    l2tabs = {}
    for x in l1_used:
        l2 = [0] * l2_per
        for g in alloc:
            if g // l2_per == x:
                l2[g % l2_per] = 0x8000000000000000 | slot_of[('data', g)] * cs
        l2tabs[x] = l2
        l1[x] = 0x8000000000000000 | slot_of[('l2', x)] * cs
    hdr = bytearray(cs)
    hdr[0:4] = b"QFI\xfb"
    struct.pack_into(">I", hdr, 4, version)
    struct.pack_into(">I", hdr, 20, cluster_bits)
    struct.pack_into(">Q", hdr, 24, virtual_size)
    struct.pack_into(">I", hdr, 36, l1_size)
    struct.pack_into(">Q", hdr, 40, l1_off)
    struct.pack_into(">Q", hdr, 48, rt_off)
    struct.pack_into(">I", hdr, 56, 1)
    if version == 3:
        struct.pack_into(">I", hdr, 96, 4)
        struct.pack_into(">I", hdr, 100, 104)
    img = bytearray(n_clusters_file * cs)
    img[0:cs] = hdr
    struct.pack_into(">%dQ" % len(rt), img, rt_off, *rt)
    for b in range(nrb):
        img[(2 + b) * cs:(3 + b) * cs] = rb[b]
    struct.pack_into(">%dQ" % l1_size, img, l1_off, *l1)
    for x, l2 in l2tabs.items():
        struct.pack_into(">%dQ" % l2_per, img, slot_of[('l2', x)] * cs, *l2)
    for g in alloc:
        s = slot_of[('data', g)]
        img[s * cs:(s + 1) * cs] = clusters[g]
    for i in range(extra_junk_slots):
        s = slot_of[('junk', i)]
        img[s * cs:(s + 1) * cs] = r.randbytes(cs)
    return bytes(img) + tail, b"".join(clusters[g] for g in alloc)


for t in range(30):
    r = random.Random(3000 + t)
    cbits = r.choice([9, 12, 16])
    cs = 1 << cbits
    n_guest = r.randrange(4, 400)
    vsize = n_guest * cs - r.choice([0, 0, 137])
    n_alloc = r.randrange(1, max(2, n_guest))
    gset = sorted(r.sample(range(n_guest), min(n_alloc, n_guest)))
    clusters = {g: r.randbytes(cs) for g in gset}
    ver = r.choice([2, 3])
    junk = r.randrange(0, 4)
    tail = r.randbytes(r.randrange(0, 700)) if r.random() < 0.7 else b""
    img, member = build_qcow2(cbits, vsize, clusters, version=ver,
                              seed=4000 + t, extra_junk_slots=junk, tail=tail)
    open(os.path.join(fz, "fz%02d.qcow2" % t), "wb").write(img)
    open(os.path.join(fz, "fz%02d.member" % t), "wb").write(member)
print("  30 fuzz fixtures generated")
PY
ok=1
for t in $(seq -w 0 29); do
    f="$WORK/fuzz/fz$t.qcow2"
    rm -rf "$WORK/pk" && mkdir -p "$WORK/pk/mbr"
    "$Q" enumerate "$f" "$WORK/pk/table" >/dev/null 2>&1 \
        && "$Q" extract "$f" 1 "$WORK/pk/mbr/1" \
        && cmp -s "$WORK/fuzz/fz$t.member" "$WORK/pk/mbr/1" \
        && "$Q" strip "$f" "$WORK/pk/recipe" \
        && "$Q" map "$f" "$WORK/pk/map" \
        && "$Q" rebuild "$WORK/pk/recipe" "$WORK/pk/mbr" "$WORK/pk/out" \
        && cmp -s "$f" "$WORK/pk/out" \
        || { echo "FAIL: fuzz layout fz$t"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "  30/30 bit-exact (enumerate/extract/strip/map/rebuild)"
fail=0
for t in $(seq -w 0 29); do
    qemu-img check "$WORK/fuzz/fz$t.qcow2" 2>&1 | grep -q "No errors" \
        || { echo "FAIL: qemu-img check complains on fz$t"; fail=1; }
done
[ "$fail" = 0 ] || exit 1
echo "  qemu-img check: 30/30 hand-built fuzz layouts accepted"

# class-stamp reader + ranged-read harness (the test-containerpack.sh pattern)
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
CORE_O="$REPO/build/obj/volume.o $REPO/build/obj/vol_cpack.o $REPO/build/obj/helper_exec.o $REPO/build/obj/vol_png.o $REPO/build/obj/vol_seal.o $REPO/build/obj/vol_repair.o $REPO/build/obj/vol_rollback.o $REPO/build/obj/vol_resize.o $REPO/build/obj/vol_fsck.o $REPO/build/obj/vol_crash.o $REPO/build/obj/vol_exer.o $REPO/build/obj/vol_dedupe.o $REPO/build/obj/vol_textzone.o $REPO/build/obj/vol_heat.o $REPO/build/obj/vol_sweep.o $REPO/build/obj/vol_meta_merge.o $REPO/build/obj/vol_read.o $REPO/build/obj/vol_write.o $REPO/build/obj/vol_records.o $REPO/build/obj/vol_ast.o $REPO/build/obj/vol_dirs.o $REPO/build/obj/arc.o $REPO/build/obj/crc32c.o $REPO/build/obj/lz4.o $REPO/build/obj/blkio.o $REPO/build/obj/flacx.o $REPO/build/obj/tarx.o $REPO/build/obj/pngx.o $REPO/build/obj/miniz.o $REPO/build/obj/ppmd8.o $REPO/build/obj/ppmd8enc.o $REPO/build/obj/ppmd8dec.o $REPO/build/obj/ppmd_codec.o $REPO/build/obj/codec.o $REPO/build/obj/bcj_x86.o $REPO/build/obj/blake3.o $REPO/build/obj/blake3_dispatch.o $REPO/build/obj/blake3_portable.o $REPO/build/obj/rs.o $REPO/build/obj/vol_tier.o"
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/classof" "$WORK/classof.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/rngread" "$WORK/rngread.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread

. "$WORK/orig/diska.layout"   # size firstext hole midext tail
QS="diska.qcow2 diskb.qcow2 disks.qcow2 diskv2.qcow2 qgen.qcow2 qzero.qcow2"
DECLINES="badmagic.qcow2 backed.qcow2 snap.qcow2 compc.qcow2 extl2.qcow2 enc.qcow2 cbits.qcow2 dupcl.qcow2 pasteof.qcow2 incompat.qcow2 rb32.qcow2 empty.qcow2"

echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null
for f in $QS $DECLINES; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== sweep #1 (qcow2 containerpack) =="
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
for f in $QS; do
    grep -q "$f: qcow2 (codecpack)" "$WORK/sweep1.log" \
        || { echo "FAIL: $f not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
done
for f in $DECLINES; do
    if grep -q "$f: qcow2 (codecpack)" "$WORK/sweep1.log"; then
        echo "FAIL: $f was decomposed (must be declined)"; cat "$WORK/sweep1.log"; exit 1
    fi
done
grep -E "codecpack" "$WORK/sweep1.log"

echo "== member batching (members flow the normal pipeline in the same run) =="
grep -E "qcow2!\*.*parts -> " "$WORK/sweep1.log" || true
grep -qE "diska\.qcow2!\*: 1 parts -> (PPMd|ZSTD) batch" "$WORK/sweep1.log" \
    || { echo "FAIL: diska member not batched in sweep 1"; cat "$WORK/sweep1.log"; exit 1; }
echo "member batching present"

echo "== sibling set (member + table + map) =="
for f in $QS; do
    N=$($B/invf-ls "$IMG" | grep -c "$f!" || true)
    echo "  $f!* names: $N"
    [ "$N" -eq 3 ] || { echo "FAIL: $f: want 3 (member + table + map)"; $B/invf-ls "$IMG"; exit 1; }
    $B/invf-ls "$IMG" | grep -q "$f!mbr0001-diskimg" \
        || { echo "FAIL: $f: member sibling missing"; exit 1; }
    $B/invf-ls "$IMG" | grep -q "$f!mbrt" || { echo "FAIL: $f: member table missing"; exit 1; }
    $B/invf-ls "$IMG" | grep -q "$f!mbrmap" || { echo "FAIL: $f: member map missing"; exit 1; }
done
$B/invf-ls "$IMG" | grep "diska\.qcow2!mbr0001-diskimg" | grep -q "524288 bytes" \
    || { echo "FAIL: diska member not 8*64K"; $B/invf-ls "$IMG"; exit 1; }
$B/invf-ls "$IMG" | grep "diskb\.qcow2!mbr0001-diskimg" | grep -q "4194304 bytes" \
    || { echo "FAIL: diskb member not 4 MiB"; $B/invf-ls "$IMG"; exit 1; }
for f in $DECLINES; do
    if $B/invf-ls "$IMG" | grep -q "$f!"; then
        echo "FAIL: declined $f gained siblings"; $B/invf-ls "$IMG"; exit 1
    fi
done
echo "member/table/map siblings present; declines clean"

echo "== class stamps =="
for f in $QS; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=3 algo=22 gen=1" ] || { echo "FAIL: $f: want CONTAINER{QCOW2=22,1}"; exit 1; }
done
for f in $DECLINES; do
    C=$("$WORK/classof" "$IMG" "$f")
    case "$C" in *algo=22*) echo "FAIL: declined $f carries a pack stamp"; exit 1;; esac
done
echo "  declines carry no pack stamp"

echo "== verify --deep =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== sha256 bit-exact (containers + direct member reads) =="
ok=1
for f in $QS $DECLINES; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
# the members are real inodes: read them directly
for f in diska diskb disks diskv2 qgen qzero; do
    $B/invf-cat "$IMG" "$f.qcow2!mbr0001-diskimg" "$WORK/out/m.$f" >/dev/null
    REF="$WORK/orig/member-${f:4}.ref"; [ -f "$REF" ] || REF="$WORK/orig/member-$f.ref"
    cmp -s "$REF" "$WORK/out/m.$f" || { echo "MISMATCH $f member"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "containers and members bit-exact"
# the member table rides verbatim as !mbrt
$B/invf-cat "$IMG" "diska.qcow2!mbrt" "$WORK/out/mbrt-a" >/dev/null
[ "$(cat "$WORK/out/mbrt-a")" = "1	diskimg	524288" ] \
    || { echo "FAIL: mbrt content drifted: $(cat "$WORK/out/mbrt-a")"; exit 1; }
echo "member table sibling verbatim"

echo "== ranged reads (the WP16b local splice over diska.qcow2) =="
rng() {  # rng <off> <len> <tag>
    dd if="$WORK/orig/diska.qcow2" of="$WORK/out/ref.$3" bs=1 skip="$1" count="$2" 2>/dev/null
    "$WORK/rngread" "$IMG" diska.qcow2 "$1" "$2" "$WORK/out/got.$3" >/dev/null
    cmp -s "$WORK/out/ref.$3" "$WORK/out/got.$3" \
        || { echo "FAIL: range $3 (off=$1 len=$2) mismatch"; exit 1; }
}
rng 0 64 head                          # the recipe head (qcow2 header)
rng 0 4 sig                            # the magic itself (recipe)
rng $((firstext - 32)) 64 rec2mbr      # recipe -> first extent boundary
rng $((hole - 32)) 64 mbr2hole         # extent -> junk-hole boundary
rng $hole 2048 hole                    # inside the junk hole (recipe)
rng $((midext + 4096)) 61440 midmbr    # strictly inside a member extent
rng $((tail - 16)) 512 tailgap         # last extent -> trailing junk
rng $((size - 1000)) 1000 tail         # the trailing junk itself (recipe)
"$WORK/rngread" "$IMG" diska.qcow2 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp -s "$WORK/orig/diska.qcow2" "$WORK/out/whole.rng" \
    || { echo "FAIL: whole file via 64K ranged windows mismatch"; exit 1; }
echo "head / boundaries / hole / mid-extent / tail / whole-by-windows all exact"

echo "== pack-ABSENT reads still work (the map is self-describing) =="
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" diska.qcow2 "$WORK/out/absent" >/dev/null
cmp -s "$WORK/orig/diska.qcow2" "$WORK/out/absent" \
    || { echo "FAIL: pack-absent whole read not bit-exact"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" diska.qcow2 \
    $((midext + 4096)) 61440 "$WORK/out/absent.rng" >/dev/null
cmp -s "$WORK/out/ref.midmbr" "$WORK/out/absent.rng" \
    || { echo "FAIL: pack-absent ranged read mismatch"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" diska.qcow2 0 -1 \
    "$WORK/out/absent.wrng" >/dev/null
cmp -s "$WORK/orig/diska.qcow2" "$WORK/out/absent.wrng" \
    || { echo "FAIL: pack-absent ranged whole read mismatch"; exit 1; }
echo "pack-absent: whole read + ranged reads bit-exact, zero pack exec"

echo "== sweep #2: members store through the normal pipeline, no re-fire =="
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
if grep -q ": qcow2 (codecpack)" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-decomposed a container"; cat "$WORK/sweep2.log"; exit 1
fi
grep -q " 0 corrupt," <($B/invf-verify "$IMG" --deep) \
    || { echo "FAIL: corrupt after sweep 2"; exit 1; }
for f in $QS; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/s2.$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/s2.$f" || { echo "FAIL: $f drifted"; exit 1; }
    C=$("$WORK/classof" "$IMG" "$f")
    [ "$C" = "cls=3 algo=22 gen=1" ] || { echo "FAIL: $f stamp drifted: $C"; exit 1; }
done
echo "containers stable and bit-exact after sweep 2"

echo "== composition leg: the member stream is a raw disk image =="
MAC=$("$WORK/classof" "$IMG" "diska.qcow2!mbr0001-diskimg")
MBC=$("$WORK/classof" "$IMG" "diskb.qcow2!mbr0001-diskimg")
echo "  diska member stream: $MAC (after sweep 2)"
echo "  diskb member stream: $MBC (after sweep 2)"
# The nested-decomposition contract: if the rawdisk pack's tool RESOLVES
# (the FS probe convention: <pack>/bin/rawdisk executable or rawdisk on
# PATH), sweep 2 — the first sweep the member stream was eligible for —
# must have decomposed it further (nested containers), and the container
# read-back goes through two map splices. If the pack directory is absent
# or its helper unbuilt, the member honestly waits RAW (sniff-claimed by
# rawdisk, probe-failed) or stores generic — report which, skip the leg.
if [ -x "$RAWDISK/bin/rawdisk" ] || command -v rawdisk >/dev/null 2>&1; then
    if ! grep -q "diskb.qcow2!mbr0001-diskimg: rawdisk (codecpack)" "$WORK/sweep2.log"; then
        # not on its first eligible sweep — one more chance before failing
        $B/invf-sweep "$IMG" > "$WORK/sweep2b.log" 2>&1 || { cat "$WORK/sweep2b.log"; exit 1; }
        grep -q "diskb.qcow2!mbr0001-diskimg: rawdisk (codecpack)" "$WORK/sweep2b.log" \
            || { echo "FAIL: rawdisk resolves but the member stream was never decomposed"; \
                 cat "$WORK/sweep2.log"; cat "$WORK/sweep2b.log"; exit 1; }
    fi
    grep -q " 0 corrupt," <($B/invf-verify "$IMG" --deep) \
        || { echo "FAIL: corrupt after the nested decomposition"; exit 1; }
    $B/invf-cat "$IMG" diskb.qcow2 "$WORK/out/nested.qcow2" >/dev/null
    cmp -s "$WORK/orig/diskb.qcow2" "$WORK/out/nested.qcow2" \
        || { echo "FAIL: nested container read-back not bit-exact"; exit 1; }
    INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" diskb.qcow2 \
        "$WORK/out/nested2.qcow2" >/dev/null
    cmp -s "$WORK/orig/diskb.qcow2" "$WORK/out/nested2.qcow2" \
        || { echo "FAIL: pack-absent NESTED read not bit-exact"; exit 1; }
    echo "  rawdisk present: member decomposed nested (a map splice through a"
    echo "  map splice), read-back bit-exact with the packs present AND absent"
else
    echo "  NOTE: rawdisk tool not resolvable ($RAWDISK/bin/rawdisk not built,"
    echo "        no rawdisk on PATH) — nested-decomposition leg SKIPPED."
    echo "        The member stream took the normal pipeline (stamps above:"
    echo "        RAW-waiting under a sniff-claiming-but-unbuilt rawdisk, or"
    echo "        generic ZSTD with the pack absent); bit-exactness was"
    echo "        verified by the direct member reads above."
fi

echo "== sweep #3: idle =="
$B/invf-sweep "$IMG" > "$WORK/sweep3.log" 2>&1 || { cat "$WORK/sweep3.log"; exit 1; }
if grep -q ": qcow2 (codecpack)" "$WORK/sweep3.log"; then
    echo "FAIL: sweep 3 re-fired the qcow2 pack"; cat "$WORK/sweep3.log"; exit 1
fi
grep -q " 0 corrupt" <($B/invf-verify "$IMG" --deep) || { echo "FAIL: corrupt"; exit 1; }
echo "no re-decomposition"

echo "== delete cascade (vol_unlink, the FUSE path) =="
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
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/cbrm" "$WORK/cbrm.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
"$WORK/cbrm" "$IMG" $QS
if $B/invf-ls "$IMG" | grep -q "qcow2!"; then
    echo "FAIL: !mbr/!mbrt/!mbrmap siblings survived the container delete"; $B/invf-ls "$IMG"; exit 1
fi
echo "containers + all members + tables + maps deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "orphans:      0" "$WORK/fsck.log" || { echo "FAIL: fsck reports orphans"; exit 1; }
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivors bit-exact =="
ok=1
for f in $DECLINES; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/surv.$f" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/surv.$f" || { echo "MISMATCH $f"; ok=0; }
done
[ "$ok" = 1 ] || exit 1
echo "declined files bit-exact after the deletes"

echo "== admission leg: INVFS_ARC_BYTES=1M =="
$B/invf-mkfs "$IMGMEM" 0.2 >/dev/null
$B/invf-cp "$IMGMEM" "$WORK/orig/diska.qcow2" diska.qcow2 >/dev/null
# the container ( > 1 MiB ) exceeds the ARC budget -> policy refusal before
# any strip/extract; GENERIC_MEMLIMIT{22,1}
INVFS_ARC_BYTES=1M $B/invf-sweep "$IMGMEM" > "$WORK/sweep-mem.log" 2>&1 \
    || { cat "$WORK/sweep-mem.log"; exit 1; }
if grep -q ": qcow2 (codecpack)" "$WORK/sweep-mem.log"; then
    echo "FAIL: decomposition ran under a 1M ARC budget"; exit 1
fi
C=$("$WORK/classof" "$IMGMEM" diska.qcow2)
echo "  diska.qcow2 (arc limit): $C"
[ "$C" = "cls=5 algo=22 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{QCOW2=22,1}"; exit 1; }
$B/invf-cat "$IMGMEM" diska.qcow2 "$WORK/out/diskamem.qcow2" >/dev/null
cmp -s "$WORK/orig/diska.qcow2" "$WORK/out/diskamem.qcow2" \
    || { echo "FAIL: arc-limited read not bit-exact"; exit 1; }
echo "policy refusal stored generic, bit-exact"

echo "QCOW2 CONTAINERPACK E2E: PASS"
