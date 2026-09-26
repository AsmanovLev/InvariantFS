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
