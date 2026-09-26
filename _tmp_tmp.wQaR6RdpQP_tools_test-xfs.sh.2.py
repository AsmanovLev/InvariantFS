import os, sys
mnt = sys.argv[1]
src = None
for root, dirs, files in os.walk('/home/user/InvariantFS/tools/busybox-src'):
    dirs.sort()
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
