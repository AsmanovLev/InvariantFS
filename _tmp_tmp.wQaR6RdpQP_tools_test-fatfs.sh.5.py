import struct, sys
tablef, mapf, work = sys.argv[1:4]
elf = frag = None
for line in open(tablef):
    idx, sname, usize = line.rstrip('\n').split('\t')
    if sname == 'ELF.BIN': elf = int(idx)
    if sname == 'FRAG.BIN': frag = int(idx)
assert elf is not None and frag is not None
blob = open(mapf, 'rb').read()
(count,) = struct.unpack_from('<I', blob, 4)
offs = {}
for i in range(count):
    off, ln, kind, idx, src = struct.unpack_from('<QQBIQ', blob, 8 + i*29)
    if kind == 1 and idx in (elf, frag) and idx not in offs:
        offs[idx] = off
with open(work + "/orig/fat32.layout", "w") as f:
    f.write("elf_idx=%d elf_off=%d frag_idx=%d frag_off=%d\n"
            % (elf, offs[elf], frag, offs[frag]))
