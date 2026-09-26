import os, sys, hashlib
MNT, WORK = sys.argv[1], sys.argv[2]
SEG = 65536
seed = open(os.devnull, 'rb')  # deterministic base: zeros won't do, use urandom
base = os.urandom(10 * SEG + 12345)     # 655KB+, non-aligned tail
exp = bytearray(base)
fd = os.open(MNT + '/r.bin', os.O_RDWR | os.O_CREAT)
def pw(buf, off):
    os.pwrite(fd, buf, off)
    need = off + len(buf)
    if need > len(exp):
        exp.extend(bytearray(need - len(exp)))      # sparse gap zero-fill
    exp[off:off + len(buf)] = buf
def tr(n):
    os.ftruncate(fd, n)
    if n < len(exp): del exp[n:]
    else: exp.extend(bytearray(n - len(exp)))
pw(base, 0)
pw(b'PATCHED-AT-ODD-OFFSET', 12345)                 # mid, sub-segment
pw(os.urandom(100000), len(exp) - 50000)            # tail, extends file
pw(os.urandom(70000), 0)                            # head, spans seg 0->1
pw(os.urandom(3 * SEG + 7), 2 * SEG - 13)           # multi-seg unaligned
pw(b'BEYOND-EOF-MARK', len(exp) + 9999)             # sparse gap
tr(400000)                                          # shrink mid-segment
pw(os.urandom(5000), 300000)                        # middle again
tr(len(exp) + 300000)                               # extend: zeros
pw(b'FINAL', len(exp))                              # append
os.fsync(fd)
# pre-commit read-your-writes through the live session (dup fd)
with os.fdopen(os.dup(fd), 'rb') as r:
    got = r.read()
if got != bytes(exp):
    for i,(a,b) in enumerate(zip(got, bytes(exp))):
        if a != b: print("first diff at", i); break
    print("len got", len(got), "exp", len(exp))
    sys.exit("FAIL: pre-commit readback mismatch")
os.close(fd)
got = open(MNT + '/r.bin', 'rb').read()
assert got == bytes(exp), "post-commit readback mismatch"
open(WORK + '/ref/r.bin', 'wb').write(bytes(exp))
print("  ranged battery bit-exact: %d bytes, sha256=%s"
      % (len(exp), hashlib.sha256(got).hexdigest()[:16]))
