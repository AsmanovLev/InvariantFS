import mmap, os, sys, hashlib
MNT, WORK = sys.argv[1], sys.argv[2]
def mhash(p):
    fd = os.open(p, os.O_RDONLY)
    st = os.fstat(fd)
    mm = mmap.mmap(fd, st.st_size, prot=mmap.PROT_READ)
    h = hashlib.sha256(mm).hexdigest()
    mm.close(); os.close(fd)
    return h
def rhash(p):
    with open(p, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()
for f in ('swept.txt', 'swept.tar', 'swept.bin', 'dup1.bin', 'dup2.bin'):
    m = mhash(MNT + '/' + f)
    r = rhash(MNT + '/' + f)
    o = rhash(WORK + '/ref/' + f)
    if not (m == r == o):
        sys.exit("FAIL: mmap read of %s mismatch (mmap=%s read=%s orig=%s)"
                 % (f, m[:12], r[:12], o[:12]))
    print("  mmap read bit-exact:", f)
# big.bin from leg A, re-hashed through mmap: by now it has been through
# the daemon drain and/or the offline sweep (exact form is timing-
# dependent; the bytes must not care)
want = open(WORK + '/big.sha').read().strip()
got = mhash(MNT + '/big.bin')
if got != want:
    sys.exit("FAIL: mmap read of big.bin mismatch post-sweep")
print("  mmap read bit-exact: big.bin (300MB, post-sweep)")
