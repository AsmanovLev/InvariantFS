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
