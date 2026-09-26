import struct, sys, os
d = sys.argv[1]
arch = open(os.path.join(d, "stored.7z"), "rb").read()
m = open(os.path.join(d, "stored.map"), "rb").read()
count, = struct.unpack("<I", m[4:8])
ents = []
for i in range(count):
    o = 8 + i * 29
    orig_off, ln = struct.unpack("<QQ", m[o:o + 16])
    kind = m[o + 16]
    idx, = struct.unpack("<I", m[o + 17:o + 21])
    ents.append((orig_off, ln, kind, idx))
members = [e for e in ents if e[2] == 1]
recipes = [e for e in ents if e[2] == 0]
rand = max(members, key=lambda e: e[1])     # the 9 MiB member
with open(os.path.join(d, "stored.layout"), "w") as f:
    f.write("size=%d mbr0_off=%d bnd_off=%d midrand_off=%d hdr_off=%d\n" % (
        len(arch),
        members[0][0],
        members[1][0],                      # member|member (or mbr|recipe) edge
        rand[0] + 4 * 1024 * 1024,          # 4 MiB into rand
        recipes[-1][0]))                    # the 7z header area (recipe)
print("  layout: size=%d, %d member extents, %d recipe ranges"
      % (len(arch), len(members), len(recipes)))
