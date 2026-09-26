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
