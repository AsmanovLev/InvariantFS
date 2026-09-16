#!/bin/bash
# test-rocp.sh — WP24-lite: read-only time-travel mount at the live sweep
# checkpoint (vol_open_at / -o at_checkpoint). Persistent regression.
#
#   A: fresh image, v1 corpus imported through a RW FUSE mount (texts, a
#      fabricated ELF binary, an incompressible file, an empty dir).
#   B: invf-sweep -> the CKP0 checkpoint arms, the retention registry holds
#      the retired pre-sweep blocks.
#   C: a second RW mount makes the v2 (PRESENT) changes: overwrite one
#      file, delete another, add a third.
#   D: mount -o at_checkpoint -> the view is exactly the v1 sweep-start
#      state (sha256 per file, the deleted file is back, the added file is
#      absent); EVERY write op fails EROFS; a parallel RW mount of the same
#      image is REFUSED (flock -- simultaneous RW+RO time-travel on one
#      image is out of scope for v1 by design); -o at_checkpoint=<seq>
#      pins the live sequence, a wrong sequence refuses loudly.
#   E: after unmount the PRESENT is intact (v2 state) and fsck is clean.
#   F: invf-sweep --realize (point of no return) -> at_checkpoint now
#      REFUSES with a clear message; fsck stays clean.
#
# Run from the repo root after `make`:  bash tools/test-rocp.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere (the mountpoints are absolute).
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B=$REPO/bin
WORK=/dev/shm/wp24rocp
IMG=wp24rocp.img
MNT=$WORK/mnt        # the time-travel / RW mount under test
MNT2=$WORK/mnt2      # the parallel-mount refusal target
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/mnt2" "$WORK/v1" "$WORK/v2" "$WORK/out"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

mnt_up() {   # <img> [extra -o opts...] — daemonized mount, wait for /proc/mounts
    local img=$1; shift
    $B/invf-fuse "$@" "$img" "$MNT" 2>"$WORK/fuse.last.log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    cat "$WORK/fuse.last.log" >&2
    fail "mount of $img never appeared"
}

mnt_down() { # unmount MNT and wait for the daemon to exit (it holds the flock)
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
    for _ in $(seq 1 300); do
        pgrep -f "invf-fuse $IMG" >/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    fusermount3 -u "$MNT2" 2>/dev/null || true
    pkill -f "invf-fuse $IMG" 2>/dev/null || true
}
trap cleanup EXIT

expect_erofs() { # <label> <cmd...>: the command must fail with EROFS
    local label=$1; shift
    set +e
    "$@" 2>"$WORK/erofs.txt"
    local rc=$?
    set -e
    [ "$rc" != 0 ] || fail "$label: write SUCCEEDED on a checkpoint view"
    grep -qi "read-only" "$WORK/erofs.txt" \
        || { cat "$WORK/erofs.txt"; fail "$label: expected EROFS"; }
    echo "  $label: EROFS"
}

echo "== build the probe helper (public API only; rbpick convention) =="
mkdir -p "$WORK/tools"
cat > "$WORK/tools/rocpick.c" <<'ROCPICK_EOF'
/* rocpick — WP24-lite test helper (uses only the public volume.h API).
 *
 *   rocpick <img> cat <name> <out>  open at the live checkpoint, write the
 *                                   file's checkpoint-time bytes to <out>
 *   rocpick <img> refuse            open at the live checkpoint; every write
 *                                   primitive must refuse
 *   rocpick <img> seq <n>           open at checkpoint #n (0 = the live one)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"

int main(int argc, char **argv)
{
    int err = 0;
    invfs_volume *v;

    if (argc < 3) return 2;

    if (!strcmp(argv[2], "seq") && argc == 4) {
        v = vol_open_at(argv[1], strtoull(argv[3], NULL, 10), &err);
        if (!v) { printf("open err %d\n", err); return 1; }
        printf("open ok tt=%d\n", vol_time_travel(v));
        vol_close(v);
        return 0;
    }

    v = vol_open_at(argv[1], 0, &err);
    if (!v) { printf("open err %d\n", err); return 1; }

    if (!strcmp(argv[2], "cat") && argc == 5) {
        uint8_t *buf = NULL;
        size_t len = 0;
        FILE *f;
        int rc = vol_read_named(v, argv[3], &buf, &len);
        if (rc != 0) { printf("read failed\n"); vol_close(v); return 1; }
        f = fopen(argv[4], "wb");
        if (!f || (len && fwrite(buf, 1, len, f) != len) ||
            (f && fclose(f) != 0)) { vol_close(v); return 1; }
        free(buf);
        vol_close(v);
        printf("read %zu bytes\n", len);
        return 0;
    }
    if (!strcmp(argv[2], "refuse")) {
        static const uint8_t payload[16] = { 0 };
        int bad = 0;
        printf("tt=%d write_enabled=%d\n", vol_time_travel(v),
               vol_write_enabled(v));
        if (!vol_time_travel(v)) bad = 1;
        if (vol_write_enabled(v)) bad = 1;
        if (vol_create_file(v, "zz_probe", payload, sizeof payload) != 0) bad = 1;
        if (vol_mkdir(v, "zz_dir") != 0) bad = 1;
        if (vol_delete_file(v, "a.c") == 0) bad = 1;   /* present in the cut */
        if (vol_replace_file(v, "a.c", payload, sizeof payload) != 0) bad = 1;
        /* flush/sync are no-ops on the view: they must SUCCEED without
         * writing (fsync(2) on a read-only file is a success) */
        if (vol_flush(v) != 0) bad = 1;
        if (vol_sync(v) != 0) bad = 1;
        vol_close(v);
        printf("refuse: %s\n", bad ? "LEAKED" : "all refused");
        return bad;
    }
    vol_close(v);
    return 2;
}
ROCPICK_EOF
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/tools/rocpick" \
    "$WORK/tools/rocpick.c" \
    $REPO/build/obj/{volume,vol_cpack,vol_png,vol_seal,vol_repair,vol_rollback,vol_resize,vol_fsck,vol_crash,vol_exer,vol_dedupe,vol_textzone,vol_heat,vol_sweep,vol_meta_merge,vol_read,vol_write,vol_records,vol_ast,vol_dirs,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable,rs,vol_tier}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
RP="$WORK/tools/rocpick"

echo
echo "== [A] mkfs + v1 corpus (through a RW FUSE mount) =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null
python3 - <<'PY'
import os, random
random.seed(24)
d = "/dev/shm/wp24rocp/v1"
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()
def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])
text_file("a.c", 120_000)
text_file("h.py", 45_000)
payload = bytearray()
pat = bytes(range(64)) * 4 + b"\x00" * 128 + os.urandom(64)
while len(payload) < 300_000:
    payload += pat
    payload += bytes([random.randrange(256)]) * 32
h = bytearray(64)
h[0:4] = b"\x7fELF"; h[18] = 62   # EM_X86_64
open(os.path.join(d, "bin_x64"), "wb").write(bytes(h) + bytes(payload[:300_000]))
open(os.path.join(d, "rand.bin"), "wb").write(os.urandom(150_000))
print("v1 corpus:", *sorted(os.listdir(d)))
PY

mnt_up "$IMG"
for f in a.c h.py bin_x64 rand.bin; do
    cp "$WORK/v1/$f" "$MNT/$f"
done
mkdir "$MNT/emptydir"
sync
mnt_down
fsck_ok "$IMG"

echo
echo "== [B] sweep: the checkpoint arms, retention holds the old blocks =="
$B/invf-sweep "$IMG" > "$WORK/sweep.log" 2>&1 || { cat "$WORK/sweep.log"; exit 1; }
grep -q "checkpoint: #1 armed" "$WORK/sweep.log" || fail "no checkpoint armed"
grep -q "retained blocks held for rollback" "$WORK/sweep.log" \
    || fail "no retention registry written"
grep "checkpoint:" "$WORK/sweep.log"
fsck_ok "$IMG"

echo
echo "== [C] the present moves on (v2): overwrite a.c, delete h.py, add new.txt =="
python3 -c "open('$WORK/v2/a.c','w').write('post-checkpoint edition of a.c\n' * 4000)"
python3 -c "open('$WORK/v2/new.txt','w').write('born after the checkpoint\n' * 500)"
mnt_up "$IMG"
cp "$WORK/v2/a.c" "$MNT/a.c"
rm "$MNT/h.py"
cp "$WORK/v2/new.txt" "$MNT/new.txt"
sync
sha256sum "$MNT/a.c" | awk '{print $1}' > "$WORK/v2/a.c.sha"
mnt_down
fsck_ok "$IMG"
$B/invf-ls "$IMG" | grep -q "new.txt" || fail "C: new.txt missing in the present"
if $B/invf-ls "$IMG" | grep -q "h.py"; then fail "C: h.py still present"; fi
echo "  present state: a.c=v2, h.py deleted, new.txt added"

echo
echo "== [D] mount -o at_checkpoint: the view is the v1 sweep-start state =="
mnt_up "$IMG" -o at_checkpoint
grep -q "time-travel mount" "$WORK/fuse.last.log" \
    || { cat "$WORK/fuse.last.log"; fail "no time-travel mount line"; }
grep -q "checkpoint view" "$WORK/fuse.last.log" || fail "view not announced"
# every v1 file reads back at its v1 bytes
for f in a.c h.py bin_x64 rand.bin; do
    cmp -s "$WORK/v1/$f" "$MNT/$f" || fail "D: $f not at v1 bytes"
    sha256sum "$MNT/$f" | awk '{print $1}' > "$WORK/out/$f.sha"
    cmp -s <(sha256sum "$WORK/v1/$f" | awk '{print $1}') "$WORK/out/$f.sha" \
        || fail "D: $f sha256 mismatch"
done
echo "  a.c h.py bin_x64 rand.bin: v1 sha256 exact (a.c shows v1, h.py resurrected)"
# the present's changes are invisible
if ls "$MNT" | grep -q "new.txt"; then fail "D: post-checkpoint file visible"; fi
[ ! -e "$MNT/new.txt" ] || fail "D: new.txt stat-able in the view"
echo "  new.txt absent from the view"
# every write op refuses EROFS
expect_erofs "append a.c"   bash -c "echo x >> '$MNT/a.c'"
expect_erofs "create"       cp "$WORK/v1/h.py" "$MNT/cp.bin"
expect_erofs "unlink"       rm "$MNT/a.c"
expect_erofs "rename"       mv "$MNT/a.c" "$MNT/a.moved"
expect_erofs "mkdir"        mkdir "$MNT/dir1"
expect_erofs "rmdir"        rmdir "$MNT/emptydir"
expect_erofs "truncate"     truncate -s 5 "$MNT/a.c"
expect_erofs "touch new"    touch "$MNT/touched"
# reads keep working after the refused writes (the view is stable)
cmp -s "$WORK/v1/a.c" "$MNT/a.c" || fail "D: a.c changed after refused writes"

echo
echo "== [D2] parallel opens of the same image are REFUSED (flock) =="
# RW mount while the checkpoint view holds the image
set +e
$B/invf-fuse "$IMG" "$MNT2" >"$WORK/fuse.rw2.log" 2>&1
RC=$?
set -e
[ "$RC" != 0 ] || fail "D2: parallel RW mount was allowed"
grep -q "image is in use by another process" "$WORK/fuse.rw2.log" \
    || { cat "$WORK/fuse.rw2.log"; fail "D2: no flock refusal message"; }
if grep -q " $MNT2 " /proc/mounts; then fail "D2: RW mount appeared"; fi
echo "  parallel RW mount refused (flock)"
# a second time-travel view is refused too (one opener per image, v1)
set +e
$B/invf-fuse -o at_checkpoint "$IMG" "$MNT2" >"$WORK/fuse.tt2.log" 2>&1
RC=$?
set -e
[ "$RC" != 0 ] || fail "D2: parallel second view was allowed"
grep -q "image is in use by another process" "$WORK/fuse.tt2.log" \
    || fail "D2: second view not flock-refused"
echo "  parallel second view refused (flock)"
mnt_down
fsck_ok "$IMG"

echo
echo "== [D3] at_checkpoint=<seq>: the live one opens, a wrong one refuses =="
set +e
$B/invf-fuse -o at_checkpoint=99 "$IMG" "$MNT" >"$WORK/fuse.seq.log" 2>&1
RC=$?
set -e
[ "$RC" != 0 ] || fail "D3: at_checkpoint=99 mounted"
grep -q "checkpoint #99 requested, but the live checkpoint is #1" \
    "$WORK/fuse.seq.log" || { cat "$WORK/fuse.seq.log"; fail "D3: no seq message"; }
echo "  wrong sequence refused loudly"
mnt_up "$IMG" -o at_checkpoint=1
cmp -s "$WORK/v1/rand.bin" "$MNT/rand.bin" || fail "D3: seq=1 view wrong"
mnt_down
echo "  at_checkpoint=1 (the live one) mounts the same view"

echo
echo "== [D4] engine level: vol_open_at refuses every write primitive =="
$RP "$IMG" cat a.c "$WORK/out/a.c.eng" >/dev/null
cmp -s "$WORK/v1/a.c" "$WORK/out/a.c.eng" || fail "D4: engine read not v1"
$RP "$IMG" refuse | tee "$WORK/refuse.log"
grep -q "tt=1 write_enabled=0" "$WORK/refuse.log" || fail "D4: view not RO"
grep -q "all refused" "$WORK/refuse.log" || fail "D4: a write leaked"
fsck_ok "$IMG"

echo
echo "== [E] the present is intact (and was never touched by the views) =="
mnt_up "$IMG"
if grep -q "not closed cleanly" "$WORK/fuse.last.log"; then
    fail "E: a time-travel mount left the volume dirty"
fi
sha256sum "$MNT/a.c" | awk '{print $1}' > "$WORK/out/a.c.v2.sha"
cmp -s "$WORK/v2/a.c.sha" "$WORK/out/a.c.v2.sha" \
    || fail "E: a.c not at v2 bytes in the present"
[ ! -e "$MNT/h.py" ] || fail "E: h.py resurrected in the present"
cmp -s "$WORK/v2/new.txt" "$MNT/new.txt" || fail "E: new.txt lost from the present"
# a RW mount still writes fine after all the views
cp "$WORK/v1/h.py" "$MNT/after.txt"
sync
cmp -s "$WORK/v1/h.py" "$MNT/after.txt" || fail "E: post-view write broken"
rm "$MNT/after.txt"
sync
mnt_down
fsck_ok "$IMG"
echo "  present intact: a.c=v2, h.py deleted, new.txt present, RW writes work"

echo
echo "== [F] --realize is the point of no return for the view too =="
# settle the present first: --realize's own re-sweep must be a no-op,
# otherwise it arms the NEXT checkpoint (which would be a valid live view)
$B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 || { cat "$WORK/sweep2.log"; exit 1; }
grep -q "checkpoint: #2 armed" "$WORK/sweep2.log" \
    || { cat "$WORK/sweep2.log"; fail "F: checkpoint #2 not armed"; }
# the view follows the live checkpoint: at #2 the cut is the v2 present
mnt_up "$IMG" -o at_checkpoint
grep -q "checkpoint #2" "$WORK/fuse.last.log" || fail "F: view not at #2"
cmp -s "$WORK/v2/new.txt" "$MNT/new.txt" || fail "F: #2 view new.txt wrong"
[ ! -e "$MNT/h.py" ] || fail "F: #2 view shows the deleted h.py"
mnt_down
echo "  the view follows the live checkpoint (#2 shows the swept v2 state)"
fsck_ok "$IMG"
$B/invf-sweep "$IMG" --realize > "$WORK/realize.log" 2>&1 \
    || { cat "$WORK/realize.log"; exit 1; }
grep -q "checkpoint: previous run realized" "$WORK/realize.log" \
    || fail "F: realize did not free the retention"
set +e
$B/invf-fuse -o at_checkpoint "$IMG" "$MNT" >"$WORK/fuse.gone.log" 2>&1
RC=$?
set -e
[ "$RC" != 0 ] || fail "F: at_checkpoint mounted past --realize"
grep -q "cannot mount at_checkpoint: no live sweep checkpoint" \
    "$WORK/fuse.gone.log" || { cat "$WORK/fuse.gone.log"; fail "F: unclear refusal"; }
if grep -q " $MNT " /proc/mounts; then fail "F: mount appeared past realize"; fi
echo "  post-realize: at_checkpoint refuses with a clear message"
set +e
$RP "$IMG" seq 0 >"$WORK/seq.gone.log" 2>&1
RC=$?
set -e
[ "$RC" != 0 ] || fail "F: engine opened the view past --realize"
grep -q "no live sweep checkpoint" "$WORK/seq.gone.log" \
    || fail "F: engine refusal unclear"
# the orphans are the documented fallout of leg E's create+delete under the
# live checkpoint from a non-arming process (held but never registered in
# \x01reten); with the checkpoint realized, fsck -f reclaims them
set +e
$B/invf-fsck "$IMG" -f > "$WORK/fsck-f.log" 2>&1
RC=$?
set -e
grep -q "REPAIRED" "$WORK/fsck-f.log" \
    || { cat "$WORK/fsck-f.log"; fail "F: fsck -f did not reclaim (rc=$RC)"; }
fsck_ok "$IMG"
# and the volume is still the healthy present
$B/invf-verify "$IMG" --deep | tail -1 | grep -q " 0 corrupt," \
    || fail "F: verify --deep not clean"

echo
echo "ROCP E2E: PASS"
