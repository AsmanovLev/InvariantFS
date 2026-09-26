import os, random, subprocess, sys
mnt = sys.argv[1]
# text files: real busybox .c sources
srcs = []
for root, dirs, files in os.walk('/home/user/InvariantFS/tools/busybox-src'):
    dirs.sort()
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
