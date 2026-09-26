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
# WP89: the walk is sorted, dirs included. os.walk yields directories in
# readdir order, i.e. in whatever order the tree was created -- a real
# `git submodule update` checkout and a plain `cp -a` of the same content
# enumerate differently -- so the chosen `text` was a property of the
# FILESYSTEM, not of the commit: this suite picked archival/dpkg.c in one
# worktree and scripts/kconfig/expr.c in another, and the suite's result
# differed with it. dirs.sort() makes the fixture the same everywhere.
text = None
for root, dirs, files in os.walk(os.environ["REPO"] + "/tools/busybox-src"):  # fixture input (submodule)
    dirs.sort()
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
