import os
import random
import struct
import sys

work = sys.argv[1]
fz = os.path.join(work, "fuzz")
os.makedirs(fz, exist_ok=True)


def build_qcow2(cluster_bits, virtual_size, clusters, version=3,
                seed=1, extra_junk_slots=0, tail=b""):
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
