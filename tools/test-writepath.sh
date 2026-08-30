#!/bin/bash
# test-writepath.sh — WP4a mmap + WP4b streaming/ranged write path e2e
# (persistent regression, suite 21).
#
#   Leg A (streaming): a 300 MB file written through the mount via dd in
#   1 MB chunks. The daemon's RSS must stay BOUNDED (the pre-WP4b path
#   buffered the whole file per handle: +300 MB RSS; the streaming path
#   stages one 64K segment per handle) and the file must read back
#   bit-exact. fsck clean.
#
#   Leg B (ranged): pwrite battery at odd offsets — middle, tail with
#   extension, head spanning a segment boundary, a multi-segment
#   unaligned span, a write beyond EOF (sparse zero gap), ftruncate
#   shrink + extend — each region sha256-checked against a python model,
#   once pre-commit (read-your-writes through the live session) and once
#   post-commit. fsck + verify --deep clean.
#
#   Leg C (mmap read): text (-> PPMd TEXT batch), a tar (-> TARR
#   container), and an incompressible file (-> UNCOMPRESSIBLE verbatim
#   shadow), loaded offline with invf-cp so the daemon's background
#   drain cannot pre-sweep them (classification then happens entirely
#   in the offline sweep and is deterministic), swept, then read via
#   mmap and compared bit-exact against the host originals and against
#   read() through the mount. big.bin from leg A (drain-swept to
#   GENERIC ZSTD by then) is re-hashed through mmap as well.
#
#   Leg D (mmap write): PROT_WRITE/MAP_SHARED patches with msync on a
#   RAW file and on a SWEPT file (forces the materialize-to-RAW path);
#   then the dirty-page crash leg: mmap write, msync only region 1,
#   fsync, kill -9 the daemon with region 2 left dirty, remount — the
#   fsynced region must survive, fsck must be clean.
#
#   Leg E (PB7 interplay): two identical files swept (dedupe shares
#   every segment), a ranged rewrite of ONE of them must leave the other
#   bit-exact (shared-block retire guard) and fsck clean.
#
#   Leg F (gpg): real gpg --detach-sign + gpg --verify with both the
#   payload and the signature read from the mounted volume — gpg mmaps
#   its inputs; this is the WP4a acceptance test.
#
# Run from the repo root after `make`:  bash tools/test-writepath.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere (the mountpoint is absolute).
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
WORK=/dev/shm/wp4writepath
IMG1=wp4wp-a.img     # legs A B C D1 D2 E F
IMG2=wp4wp-b.img     # leg D3 crash leg
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/ref" "$WORK/out"
cd /dev/shm
rm -f "$IMG1" "$IMG2"

command -v gpg >/dev/null || { echo "gpg required for leg F"; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }

mnt_up() {   # <img> — daemonized mount, wait for /proc/mounts
    $B/invf-fuse "$1" "$MNT" 2>"$WORK/fuse.$(basename "$1").log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    fail "mount of $1 never appeared"
}

mnt_down() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
    # the unmounted daemon still holds the image open and finishes its
    # background drain (a 300MB sweep takes seconds) -- offline tools must
    # not run until the process is dead (two writers = corrupt volume)
    for _ in $(seq 1 300); do
        alive=0
        pgrep -f "invf-fuse $IMG1" >/dev/null && alive=1
        pgrep -f "invf-fuse $IMG2" >/dev/null && alive=1
        [ "$alive" = 0 ] && return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

deep_ok() { $B/invf-verify "$1" --deep | tail -1 | grep -q " 0 corrupt," \
    || fail "verify --deep not clean: $1"; }

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse $IMG1" 2>/dev/null || true
    pkill -f "invf-fuse $IMG2" 2>/dev/null || true
}
trap cleanup EXIT

rss_kb() { awk '/VmRSS/{print $2}' "/proc/$1/status"; }

echo "== [A] streaming: 300MB via dd bs=1M, daemon RSS bounded, bit-exact =="
$B/invf-mkfs "$IMG1" 0.5 >/dev/null
mnt_up "$IMG1"
head -c 314572800 /dev/urandom > "$WORK/ref/big.bin"   # 300 MB
PID=$(pgrep -f "invf-fuse $IMG1" | head -1)
R0=$(rss_kb $PID)
dd if="$WORK/ref/big.bin" of="$MNT/big.bin" bs=1M 2>"$WORK/dd.log"
sync
R1=$(rss_kb $PID)
echo "  daemon RSS ${R0}kB -> ${R1}kB while streaming 300MB"
# the pre-WP4b whole-file buffer would have added ~300MB here; anything
# under 64MB of growth proves there is no per-handle file buffer
[ $((R1 - R0)) -lt 65536 ] || fail "daemon RSS grew $(( (R1-R0)/1024 ))MB on a 300MB stream"
cmp "$WORK/ref/big.bin" "$MNT/big.bin" || fail "300MB stream not bit-exact"
echo "  bit-exact: 300MB file lands and reads back identically"
sha256sum "$WORK/ref/big.bin" | awk '{print $1}' > "$WORK/big.sha"
rm -f "$WORK/ref/big.bin"   # free the tmpfs before the later legs

echo
echo "== [B] ranged writes: odd offsets, extends, sparse gap, truncate =="
python3 - "$MNT" "$WORK" <<'PYEOF'
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
PYEOF
[ $? = 0 ] || exit 1
mnt_down
fsck_ok "$IMG1"
deep_ok "$IMG1"

echo
echo "== [C] mmap read of swept forms (TEXT batch, container, ZSTD/UNCOMPRESSIBLE) =="
# RAW is 20% of the volume (~94 MiB here), so the 300 MB leg-A file spilled
# into SHADOW and left RAW full. A file written now would ALSO spill
# (alloc_raw_or_shadow) and a spilled file (BINARY zone, no class stamp) is
# never classified by the sweep. Sweep big.bin/r.bin out of RAW first; a
# bare second pass auto-realizes the sweep checkpoint (WP21) so the freed
# RAW blocks are actually allocable again.
$B/invf-sweep "$IMG1" > "$WORK/sweep0a.log" 2>&1 || { cat "$WORK/sweep0a.log"; exit 1; }
$B/invf-sweep "$IMG1" > "$WORK/sweep0b.log" 2>&1 || { cat "$WORK/sweep0b.log"; exit 1; }
# fixtures land via the OFFLINE cli, never the mounted daemon: the daemon's
# per-second pending drain (vol_sweep_one) would otherwise pre-sweep them
# mid-leg and the classification below would be timing-dependent
python3 - "$WORK" <<'PYEOF'
import os, sys, tarfile, io
WORK = sys.argv[1]
WORDS = (b'the quick brown fox jumps over the lazy dog invariant fs segment '
         b'pack alpha beta gamma delta\n')
text = (WORDS * 40000)[:3000000]
open(WORK + '/ref/swept.txt', 'wb').write(text)
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode='w') as tf:
    for n in ('m1.txt', 'm2.txt', 'm3.txt'):
        d = (WORDS * 900)[:50000]
        ti = tarfile.TarInfo(n); ti.size = len(d)
        tf.addfile(ti, io.BytesIO(d))
open(WORK + '/ref/swept.tar', 'wb').write(buf.getvalue())
rnd = os.urandom(3000000)
open(WORK + '/ref/swept.bin', 'wb').write(rnd)
# PB7 leg fixtures: two identical files for the dedupe share
dup = os.urandom(1000000)
open(WORK + '/ref/dup1.bin', 'wb').write(dup)
open(WORK + '/ref/dup2.bin', 'wb').write(dup)
print("  fixtures staged")
PYEOF
for f in swept.txt swept.tar swept.bin dup1.bin dup2.bin; do
    $B/invf-cp "$IMG1" "$WORK/ref/$f" "$f" >/dev/null || fail "invf-cp $f"
done
$B/invf-sweep "$IMG1" > "$WORK/sweep.log" 2>&1 || { cat "$WORK/sweep.log"; exit 1; }
grep -q "sweep done" "$WORK/sweep.log" || fail "sweep did not complete"
tail -2 "$WORK/sweep.log"
# the forms this leg claims to cover must actually have happened
class_of() { $B/meta_probe "$1" --heat "$2" 2>/dev/null | sed -n 's/^class=\([0-9a-z]*\).*/\1/p'; }
[ "$(class_of "$IMG1" swept.txt)" = "7" ] || fail "swept.txt not TEXT post-sweep"
[ "$(class_of "$IMG1" swept.tar)" = "3" ] || fail "swept.tar not CONTAINER post-sweep"
[ "$(class_of "$IMG1" swept.bin)" = "1" ] || fail "swept.bin not UNCOMPRESSIBLE post-sweep"
echo "  classes: swept.txt=TEXT swept.tar=CONTAINER swept.bin=UNCOMPRESSIBLE"
grep -q "merged" "$WORK/sweep.log" || fail "dedupe pass did not merge dup1/dup2"
fsck_ok "$IMG1"
mnt_up "$IMG1"
python3 - "$MNT" "$WORK" <<'PYEOF'
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
PYEOF
[ $? = 0 ] || exit 1

echo
echo "== [D1] mmap write (RAW file) + msync =="
python3 - "$MNT" <<'PYEOF'
import mmap, os, sys
MNT = sys.argv[1]
fd = os.open(MNT + '/mm.bin', os.O_RDWR | os.O_CREAT)
os.ftruncate(fd, 5 * 1024 * 1024)
mm = mmap.mmap(fd, 5 * 1024 * 1024)     # PROT_WRITE|READ, MAP_SHARED
pat = os.urandom(1000)
mm[1234567:1234567 + 1000] = pat
mm[4 * 1024 * 1024 + 13:4 * 1024 * 1024 + 16] = b'END'
mm.flush()                              # msync MS_SYNC
mm.close(); os.close(fd)
data = open(MNT + '/mm.bin', 'rb').read()
assert data[1234567:1234567 + 1000] == pat, "mmap write lost"
assert data[4 * 1024 * 1024 + 13:4 * 1024 * 1024 + 16] == b'END'
print("  mmap write+msync persisted (RAW file)")
PYEOF

echo
echo "== [D2] mmap write of a SWEPT file (materialize to RAW) =="
python3 - "$MNT" "$WORK" <<'PYEOF'
import mmap, os, sys
MNT, WORK = sys.argv[1], sys.argv[2]
ref = bytearray(open(WORK + '/ref/swept.txt', 'rb').read())
fd = os.open(MNT + '/swept.txt', os.O_RDWR)
st = os.fstat(fd)
mm = mmap.mmap(fd, st.st_size)
pat1 = os.urandom(5000)
mm[77777:77777 + 5000] = pat1
ref[77777:77777 + 5000] = pat1
pat2 = os.urandom(200000)
mm[1500000:1500000 + 200000] = pat2     # spans 64K segments
ref[1500000:1500000 + 200000] = pat2
mm.flush()
mm.close(); os.close(fd)
got = open(MNT + '/swept.txt', 'rb').read()
assert got == bytes(ref), "swept-file mmap write mismatch"
open(WORK + '/ref/swept.txt', 'wb').write(bytes(ref))
print("  mmap write+msync persisted (swept file, materialized)")
PYEOF

echo
echo "== [E] PB7 interplay: rewrite one dedupe-shared file, other stays exact =="
python3 - "$MNT" <<'PYEOF'
import os, sys, hashlib
MNT = sys.argv[1]
fd = os.open(MNT + '/dup2.bin', os.O_RDWR)
os.pwrite(fd, os.urandom(65536), 262144)    # one full segment, odd phase
os.fsync(fd); os.close(fd)
h = hashlib.sha256(open(MNT + '/dup1.bin', 'rb').read()).hexdigest()
print("  dup1 sha256 after dup2 rewrite:", h[:16])
PYEOF
cmp "$WORK/ref/dup1.bin" "$MNT/dup1.bin" \
    || fail "dedupe-shared dup1 corrupted by dup2 rewrite (PB7)"
mnt_down
fsck_ok "$IMG1"
mnt_up "$IMG1"

echo
echo "== [F] gpg acceptance: sign + verify over the mounted volume =="
export GNUPGHOME="$WORK/gnupg"
rm -rf "$GNUPGHOME" && mkdir -m 700 "$GNUPGHOME"
gpg --batch --pinentry-mode loopback --passphrase '' \
    --quick-gen-key 'wp4 writepath <wp4@invfs>' ed25519 sign 2>/dev/null
head -c 3000000 /dev/urandom > "$MNT/payload.bin"
gpg --batch --pinentry-mode loopback --passphrase '' \
    --detach-sign -o "$MNT/payload.bin.sig" "$MNT/payload.bin" 2>/dev/null
gpg --batch --verify "$MNT/payload.bin.sig" "$MNT/payload.bin" \
    2>&1 | tee "$WORK/gpg.log"
grep -q "Good signature" "$WORK/gpg.log" || fail "gpg --verify failed"
echo "  gpg --verify: Good signature (payload + sig both read via the mount)"

mnt_down
fsck_ok "$IMG1"
deep_ok "$IMG1"

echo
echo "== [D3] crash leg: dirty mmap pages, no msync, kill -9, remount =="
$B/invf-mkfs "$IMG2" 0.25 >/dev/null
mnt_up "$IMG2"
python3 - "$MNT" <<'PYEOF'
import mmap, os, sys
MNT = sys.argv[1]
fd = os.open(MNT + '/victim.bin', os.O_RDWR | os.O_CREAT)
os.ftruncate(fd, 2 * 1024 * 1024)
mm = mmap.mmap(fd, 2 * 1024 * 1024)
mm[0:16] = b'FLUSHED-REGION--'
mm.flush()                              # written back + committed below
mm[1048576:1048592] = b'NEVER-MSYNCED-XX'    # stays dirty in page cache
os.fsync(fd)                            # fsync commits the session (+barrier)
print("  armed (region1 msynced+fsynced, region2 dirty only)")
PYEOF
PID=$(pgrep -f "invf-fuse $IMG2" | head -1)
kill -9 $PID
sleep 0.5
mnt_down || true
fusermount3 -u "$MNT" 2>/dev/null || true   # clear the stale mount
fsck_ok "$IMG2"                              # journal replay must reconcile
mnt_up "$IMG2"
python3 - "$MNT" <<'PYEOF'
import os, sys
data = open(sys.argv[1] + '/victim.bin', 'rb').read()
if len(data) != 2 * 1024 * 1024:
    sys.exit("FAIL: victim.bin size %d after crash" % len(data))
if data[0:16] != b'FLUSHED-REGION--':
    sys.exit("FAIL: msynced+fsynced region lost after kill -9")
print("  post-crash: fsynced region intact; unsynced region %s"
      % ("also landed (kernel wrote it back before the kill)"
         if data[1048576:1048592] == b'NEVER-MSYNCED-XX' else "absent (legal)"))
PYEOF
mnt_down
fsck_ok "$IMG2"
deep_ok "$IMG2"

rm -f "$IMG1" "$IMG2"
echo
echo "WRITEPATH E2E: PASS"
