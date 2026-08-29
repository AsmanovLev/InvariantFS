#!/bin/bash
# test-rollback.sh — WP21 sweep-checkpoint + retention + rollback end-to-end
# (persistent regression).
#
#   image A (core): mkfs -> mixed corpus (texts -> PPMd batches, fabricated
#   ELF/PE binaries -> ZSTD/BCJ batches, a tar -> TARR + part batching, an
#   incompressible file) -> sweep (checkpoint armed, retention registry
#   holds the retired blocks; classes stamped; fsck OK + checkpoint line)
#   -> rollback -> fsck OK, verify --deep clean, every file bit-exact vs
#   the PRE-SWEEP originals, class stamps gone (records resurrected),
#   second rollback = "no checkpoint".
#
#   image B (post-checkpoint writes discarded): sweep -> write a NEW file
#   + overwrite a swept one -> rollback -> the new file is GONE, the
#   overwritten file is back to its pre-sweep bytes, fsck OK.
#
#   image C (realize): sweep -> --realize (retained blocks freed, CKP0
#   cleared -- the point of no return) -> rollback = "no checkpoint"
#   (rc=1), the volume is intact and fully swept.
#
#   image D (retention fidelity): sweep -> rm a swept file (frees its NEW
#   blocks; the OLD ones stay retained) -> rollback -> the delete's
#   tombstone is post-checkpoint, so the file RESURRECTS with pre-sweep
#   bytes; fsck clean.
#
#   image E (crash legs): INVFS_ROLLBACK_ABORT_AT=restored / =rebuilt ->
#   kill -9 mid-rollback -> the next invf-rollback re-enters and finishes
#   (fsck OK, bit-exact). INVFS_SWEEP_ABORT_AFTER kills a sweep mid-walk
#   (volume DIRTY, checkpoint live, registry never written): fsck -f is
#   REFUSED while CKP0 is live, rollback recovers the pre-sweep state.
#
#   image F (refusals): rollback on a fresh volume = "no checkpoint"
#   (rc=1); sweep --seal -> rollback REFUSED under the live seal (rc=2)
#   -> --free-redundant -> rollback works (bit-exact); a bare sweep on
#   the sealed volume declines to checkpoint.
#
# Run from the repo root after `make`:  bash tools/test-rollback.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
WORK=/dev/shm/wp21rollback
IMGA=wp21rb-a.img    # core: sweep -> rollback -> pre-sweep bytes
IMGB=wp21rb-b.img    # post-checkpoint writes discarded
IMGC=wp21rb-c.img    # realize = point of no return
IMGD=wp21rb-d.img    # retention fidelity (resurrection)
IMGE=wp21rb-e.img    # crash legs
IMGF=wp21rb-f.img    # refusals (fresh / sealed / double)
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMGA" "$IMGB" "$IMGC" "$IMGD" "$IMGE" "$IMGF"

fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== build the probe helper (public API only; sealpick convention) =="
mkdir -p "$WORK/tools"
cat > "$WORK/tools/rbpick.c" <<'RBPICK_EOF'
/* rbpick — WP21 test helper (uses only the public volume.h API).
 *
 *   rbpick <img> ckp        -> "present=<0|1> seq=<n>" (the CKP0 state)
 *   rbpick <img> rm <name>  -> delete a file (no CLI rm exists)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"
#include "invarifs.h"

int main(int argc, char **argv)
{
    int err = 0;
    invfs_volume *v;

    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }

    if (!strcmp(argv[2], "ckp")) {
        invfs_ckp0 ck;
        int p = vol_ckp_info(v, &ck);
        printf("present=%d seq=%llu\n", p,
               p ? (unsigned long long)ck.sweep_seq : 0ull);
        vol_close(v);
        return 0;
    }
    if (!strcmp(argv[2], "rm") && argc == 4) {
        int rc = vol_delete_file(v, argv[3]);
        if (rc == 0) rc = vol_flush(v);
        vol_close(v);
        return rc ? 1 : 0;
    }
    vol_close(v);
    return 2;
}
RBPICK_EOF
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/tools/rbpick" \
    "$WORK/tools/rbpick.c" \
    $REPO/build/obj/{volume,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable,rs}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
RP="$WORK/tools/rbpick"

echo "== mkfs + corpus =="
$B/invf-mkfs "$IMGA" 0.5 >/dev/null
python3 - <<'PY'
import os, random, tarfile, io
random.seed(21)
d = "/dev/shm/wp21rollback/orig"
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()

def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])

def fake_bin(name, emachine, size):
    payload = bytearray()
    pat = bytes(range(64)) * 4 + b"\x00" * 128 + os.urandom(64)
    while len(payload) < size:
        payload += pat
        payload += bytes([random.randrange(256)]) * 32
    h = bytearray(64)
    h[0:4] = b"\x7fELF"; h[18] = emachine & 0xFF; h[19] = emachine >> 8
    open(os.path.join(d, name), "wb").write(bytes(h) + bytes(payload[:size]))

text_file("a.c", 120_000)
text_file("h.py", 45_000)
text_file("r.log", 500_000)
fake_bin("bin_x64", 62, 300_000)     # EM_X86_64  -> BCJ batch
fake_bin("bin_a64", 183, 200_000)    # EM_AARCH64 -> non-BCJ batch
# incompressible file: crosses the zone line (RAW -> shadow verbatim)
open(os.path.join(d, "rand.bin"), "wb").write(os.urandom(150_000))
# a tar of some texts (TARR decomposition + part batching)
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w") as tf:
    for name in ["a.c", "h.py"]:
        data = open(os.path.join(d, name), "rb").read()
        ti = tarfile.TarInfo("t/" + name)
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data))
open(os.path.join(d, "t.tar"), "wb").write(buf.getvalue())
print("corpus:", *sorted(os.listdir(d)))
PY

FILES=$(cd "$WORK/orig" && ls)
for f in $FILES; do
    $B/invf-cp "$IMGA" "$WORK/orig/$f" "$f" >/dev/null
done

# bit-exact check of every corpus file against the host original
check_all() { # <label> <img>
    local ok=1 f
    for f in $(cd "$WORK/orig" && ls); do
        $B/invf-cat "$2" "$f" "$WORK/out/$f" >/dev/null 2>"$WORK/out/$f.err" \
            || { echo "  cat failed: $f ($1)"; ok=0; continue; }
        cmp -s "$WORK/orig/$f" "$WORK/out/$f" \
            || { echo "  MISMATCH: $f ($1)"; ok=0; }
    done
    [ "$ok" = 1 ] || fail "bit-exact check: $1"
    echo "  all files bit-exact ($1)"
}

class_of() { $B/meta_probe "$1" --heat "$2" 2>/dev/null | sed -n 's/^class=\([0-9a-z]*\).*/\1/p'; }

echo
echo "== [A1] sweep: checkpoint armed, retention held, classes stamped =="
$B/invf-sweep "$IMGA" > "$WORK/sweep-a.log" 2>&1 || { cat "$WORK/sweep-a.log"; exit 1; }
grep -q "checkpoint: #1 armed" "$WORK/sweep-a.log" || fail "no checkpoint armed"
grep -q "retained blocks held for rollback" "$WORK/sweep-a.log" \
    || fail "no retention registry written"
grep "checkpoint:" "$WORK/sweep-a.log"
# the transformations really happened (records replaced, classes stamped)
[ "$(class_of "$IMGA" a.c)" = "7" ]     || fail "a.c not TEXT post-sweep"
[ "$(class_of "$IMGA" t.tar)" = "3" ]   || fail "t.tar not CONTAINER post-sweep"
[ "$(class_of "$IMGA" rand.bin)" = "1" ] || fail "rand.bin not UNCOMPRESSIBLE post-sweep"
echo "  classes stamped: a.c=TEXT t.tar=CONTAINER rand.bin=UNCOMPRESSIBLE"
check_all "post-sweep" "$IMGA"
# fsck during the checkpoint window: registry-owned blocks are live
$B/invf-fsck "$IMGA" | tee "$WORK/fsck-a1.log"
grep -q "^OK$" "$WORK/fsck-a1.log" || fail "fsck not clean post-sweep"
grep -q "checkpoint:   sweep #1 live" "$WORK/fsck-a1.log" \
    || fail "fsck does not report the live checkpoint"
$RP "$IMGA" ckp | tee "$WORK/ckp-a1.log"
grep -q "present=1 seq=1" "$WORK/ckp-a1.log" || fail "CKP0 not live"
# verify --deep during the window (skips the internal registry owner)
$B/invf-verify "$IMGA" --deep | tail -1 | grep -q " 0 corrupt," \
    || fail "verify not clean post-sweep"

echo
echo "== [A2] rollback -> pre-sweep state, bit-exact, classes gone =="
$B/invf-rollback "$IMGA" | tee "$WORK/rb-a.log" || fail "rollback failed"
grep -q "rolled back to checkpoint #1" "$WORK/rb-a.log" || fail "no rollback line"
grep -q "checkpoint cleared" "$WORK/rb-a.log" || fail "CKP0 not cleared"
$B/invf-fsck "$IMGA" | tee "$WORK/fsck-a2.log"
grep -q "^OK$" "$WORK/fsck-a2.log" || fail "fsck not clean post-rollback"
if grep -q "checkpoint:" "$WORK/fsck-a2.log"; then
    fail "checkpoint still reported post-rollback"
fi
$B/invf-verify "$IMGA" --deep | tail -1 | grep -q " 0 corrupt," \
    || fail "verify not clean post-rollback"
check_all "post-rollback" "$IMGA"
# the resurrected records are the pre-sweep ones: no class stamps
for f in $FILES; do
    [ "$(class_of "$IMGA" "$f")" = "absent" ] \
        || fail "$f still carries a class stamp post-rollback"
done
echo "  class stamps gone (records resurrected)"
$RP "$IMGA" ckp | grep -q "present=0" || fail "CKP0 still live"
# internal owners never leak into listings
if $B/invf-ls "$IMGA" | grep -q "reten\|tzb\|parity"; then
    fail "internal owner leaked into directory listing"
fi

echo
echo "== [A3] double rollback = no checkpoint =="
set +e
$B/invf-rollback "$IMGA" > "$WORK/rb-a2.log" 2>&1
RC=$?
set -e
[ "$RC" = "1" ] || { cat "$WORK/rb-a2.log"; fail "second rollback rc=$RC, want 1"; }
grep -q "no checkpoint" "$WORK/rb-a2.log" || fail "no 'no checkpoint' message"
check_all "post double-rollback" "$IMGA"

echo
echo "== [B] post-checkpoint writes are discarded by the rollback =="
$B/invf-mkfs "$IMGB" 0.5 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGB" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMGB" > "$WORK/sweep-b.log" 2>&1 || { cat "$WORK/sweep-b.log"; exit 1; }
grep -q "checkpoint: #1 armed" "$WORK/sweep-b.log" || fail "B: no checkpoint"
# post-sweep: a brand-new file, and an overwrite of a swept file (the
# edit tree is scratch: $WORK/orig stays the pre-sweep truth)
mkdir -p "$WORK/edit"
python3 -c "open('$WORK/edit/new.txt','w').write('written after the checkpoint\n' * 500)"
$B/invf-cp "$IMGB" "$WORK/edit/new.txt" new.txt >/dev/null
python3 -c "open('$WORK/edit/r.log','w').write('post-sweep edition of r.log\n' * 2000)"
$B/invf-cp "$IMGB" "$WORK/edit/r.log" r.log >/dev/null
$B/invf-ls "$IMGB" | grep -q "new.txt" || fail "B: new.txt not visible pre-rollback"
$B/invf-cat "$IMGB" r.log "$WORK/out/r.log.pre" >/dev/null
cmp -s "$WORK/edit/r.log" "$WORK/out/r.log.pre" \
    || fail "B: r.log not at its post-sweep bytes pre-rollback"
$B/invf-rollback "$IMGB" | tee "$WORK/rb-b.log" || fail "B: rollback failed"
if $B/invf-ls "$IMGB" | grep -q "new.txt"; then
    fail "B: post-checkpoint file survived the rollback"
fi
echo "  new.txt is gone (post-checkpoint RAW changes discarded)"
$B/invf-cat "$IMGB" r.log "$WORK/out/r.log" >/dev/null
cmp -s "$WORK/orig/r.log" "$WORK/out/r.log" \
    || fail "B: r.log not at pre-sweep bytes"
echo "  r.log back to its pre-sweep bytes"
$B/invf-fsck "$IMGB" | grep -q "^OK$" || fail "B: fsck not clean"
check_all "B post-rollback" "$IMGB"

echo
echo "== [C] --realize is the point of no return =="
$B/invf-mkfs "$IMGC" 0.5 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGC" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMGC" > "$WORK/sweep-c.log" 2>&1 || { cat "$WORK/sweep-c.log"; exit 1; }
$RP "$IMGC" ckp | grep -q "present=1" || fail "C: no checkpoint post-sweep"
$B/invf-sweep "$IMGC" --realize > "$WORK/realize-c.log" 2>&1 \
    || { cat "$WORK/realize-c.log"; exit 1; }
grep -q "checkpoint: previous run realized" "$WORK/realize-c.log" \
    || fail "C: realize did not free the retention"
grep "checkpoint:" "$WORK/realize-c.log"
# the realize's own re-sweep is a no-op and leaves NO new checkpoint
$RP "$IMGC" ckp | grep -q "present=0" || fail "C: checkpoint survived --realize"
set +e
$B/invf-rollback "$IMGC" > "$WORK/rb-c.log" 2>&1
RC=$?
set -e
[ "$RC" = "1" ] || { cat "$WORK/rb-c.log"; fail "C: rollback rc=$RC, want 1"; }
grep -q "no checkpoint" "$WORK/rb-c.log" || fail "C: no 'no checkpoint' message"
$B/invf-fsck "$IMGC" | grep -q "^OK$" || fail "C: fsck not clean post-realize"
check_all "C post-realize (volume intact, swept form)" "$IMGC"

echo
echo "== [D] retention fidelity: a post-sweep delete resurrects =="
$B/invf-mkfs "$IMGD" 0.5 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGD" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMGD" > "$WORK/sweep-d.log" 2>&1 || { cat "$WORK/sweep-d.log"; exit 1; }
[ "$(class_of "$IMGD" a.c)" = "7" ] || fail "D: a.c not TEXT post-sweep"
# delete a swept file: its post-sweep form's blocks free immediately, the
# pre-sweep blocks stay retained (the tombstone is post-checkpoint)
$RP "$IMGD" rm a.c || fail "D: rm a.c failed"
if $B/invf-ls "$IMGD" | grep -q "a.c"; then fail "D: a.c still listed"; fi
$B/invf-rollback "$IMGD" > "$WORK/rb-d.log" 2>&1 || fail "D: rollback failed"
$B/invf-cat "$IMGD" a.c "$WORK/out/a.c" >/dev/null \
    || fail "D: a.c did not resurrect"
cmp -s "$WORK/orig/a.c" "$WORK/out/a.c" \
    || fail "D: a.c resurrected with wrong bytes"
echo "  a.c resurrected bit-exact (pre-sweep content, class stamp gone)"
[ "$(class_of "$IMGD" a.c)" = "absent" ] || fail "D: a.c still stamped"
$B/invf-fsck "$IMGD" | grep -q "^OK$" || fail "D: fsck not clean"
check_all "D post-rollback" "$IMGD"

echo
echo "== [E1] crash after the journal restore + truncate: re-run finishes =="
$B/invf-mkfs "$IMGE" 0.5 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGE" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMGE" >/dev/null 2>&1 || fail "E: sweep failed"
set +e
INVFS_ROLLBACK_ABORT_AT=restored $B/invf-rollback "$IMGE" >/dev/null 2>&1
RC=$?
set -e
[ "$RC" = "137" ] || fail "E1: expected SIGKILL (137), got $RC"
# killed post-commit: the pre-sweep view is already in place, CKP0 live
$RP "$IMGE" ckp | grep -q "present=1" || fail "E1: CKP0 lost mid-rollback"
$B/invf-rollback "$IMGE" > "$WORK/rb-e1.log" 2>&1 || fail "E1: re-run failed"
$B/invf-fsck "$IMGE" | grep -q "^OK$" || fail "E1: fsck not clean"
check_all "E1" "$IMGE"
echo "  killed post-restore: re-entered and finished, bit-exact"

echo
echo "== [E2] crash after the rebuild, before the CKP0 clear =="
for f in $FILES; do
    $B/invf-cp "$IMGE" "$WORK/orig/$f" "$f.v2" >/dev/null
done
$B/invf-sweep "$IMGE" >/dev/null 2>&1 || fail "E2: sweep failed"
$RP "$IMGE" ckp | grep -q "present=1" || fail "E2: no checkpoint armed"
set +e
INVFS_ROLLBACK_ABORT_AT=rebuilt $B/invf-rollback "$IMGE" >/dev/null 2>&1
RC=$?
set -e
[ "$RC" = "137" ] || fail "E2: expected SIGKILL (137), got $RC"
# the rebuild ran; CKP0 still live -> a re-run only clears the checkpoint
$RP "$IMGE" ckp | grep -q "present=1" || fail "E2: CKP0 lost mid-rollback"
$B/invf-rollback "$IMGE" > "$WORK/rb-e2.log" 2>&1 || fail "E2: re-run failed"
$B/invf-fsck "$IMGE" | grep -q "^OK$" || fail "E2: fsck not clean"
for f in $FILES; do
    $B/invf-cat "$IMGE" "$f.v2" "$WORK/out/$f.v2" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.v2" || fail "E2: MISMATCH $f.v2"
done
check_all "E2" "$IMGE"
echo "  killed post-rebuild: re-entered and finished, bit-exact"

echo
echo "== [E3] crash mid-sweep (checkpoint live, registry never written) =="
$B/invf-mkfs "$IMGE.x" 0.5 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGE.x" "$WORK/orig/$f" "$f" >/dev/null
done
set +e
INVFS_SWEEP_ABORT_AFTER=4 $B/invf-sweep "$IMGE.x" > "$WORK/sweep-e3.log" 2>&1
RC=$?
set -e
[ "$RC" = "137" ] || fail "E3: expected SIGKILL (137), got $RC"
$RP "$IMGE.x" ckp | grep -q "present=1" || fail "E3: no live checkpoint after the kill"
# the volume mid-sweep: fsck report works; -f is REFUSED while CKP0 is live
set +e
$B/invf-fsck "$IMGE.x" -f > "$WORK/fsck-e3.log" 2>&1
RC=$?
set -e
[ "$RC" != "0" ] || fail "E3: fsck -f ran despite the live checkpoint"
grep -q "a sweep checkpoint is live" "$WORK/fsck-e3.log" \
    || { cat "$WORK/fsck-e3.log"; fail "E3: no refusal message"; }
echo "  fsck -f refused under the live checkpoint"
$B/invf-rollback "$IMGE.x" > "$WORK/rb-e3.log" 2>&1 || fail "E3: rollback failed"
$B/invf-fsck "$IMGE.x" | grep -q "^OK$" || fail "E3: fsck not clean"
check_all "E3" "$IMGE.x"
echo "  crashed mid-sweep: rollback recovered the pre-sweep state"
rm -f "$IMGE.x"

echo
echo "== [F] refusals: fresh volume, live seal, then rollback after unseal =="
$B/invf-mkfs "$IMGF" 0.5 >/dev/null
set +e
$B/invf-rollback "$IMGF" > "$WORK/rb-f0.log" 2>&1
RC=$?
set -e
[ "$RC" = "1" ] || fail "F: fresh-volume rollback rc=$RC, want 1"
grep -q "no checkpoint" "$WORK/rb-f0.log" || fail "F: no 'no checkpoint' message"
echo "  fresh volume: no checkpoint (rc=1)"
for f in $FILES; do
    $B/invf-cp "$IMGF" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMGF" --seal > "$WORK/sweep-f.log" 2>&1 \
    || { cat "$WORK/sweep-f.log"; exit 1; }
grep -q "checkpoint: #1 armed" "$WORK/sweep-f.log" || fail "F: no checkpoint"
grep -q "\[seal\]" "$WORK/sweep-f.log" || fail "F: no seal"
set +e
$B/invf-rollback "$IMGF" > "$WORK/rb-f1.log" 2>&1
RC=$?
set -e
[ "$RC" = "2" ] || { cat "$WORK/rb-f1.log"; fail "F: sealed rollback rc=$RC, want 2"; }
grep -q "free-redundant" "$WORK/rb-f1.log" || fail "F: no unseal guidance"
echo "  sealed volume: rollback refused (rc=2)"
# a bare sweep on the sealed volume declines a fresh checkpoint
$B/invf-sweep "$IMGF" > "$WORK/sweep-f2.log" 2>&1 || { cat "$WORK/sweep-f2.log"; exit 1; }
grep -q "declined (a redundancy seal is live" "$WORK/sweep-f2.log" \
    || fail "F: sweep under seal did not decline the checkpoint"
echo "  sweep under seal declines to checkpoint"
# with the seal freed the checkpoint is gone too (the bare sweep realized
# it) -- import fresh state for the unseal->rollback proof instead:
$B/invf-mkfs "$IMGF.2" 0.5 >/dev/null
for f in $FILES; do
    $B/invf-cp "$IMGF.2" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-sweep "$IMGF.2" --seal >/dev/null 2>&1 || fail "F2: sweep --seal failed"
$B/invf-sweep "$IMGF.2" --free-redundant >/dev/null 2>&1 || fail "F2: unseal failed"
$B/invf-rollback "$IMGF.2" > "$WORK/rb-f2.log" 2>&1 || fail "F2: rollback failed"
check_all "F2 post-unseal rollback" "$IMGF.2"
$B/invf-fsck "$IMGF.2" | grep -q "^OK$" || fail "F2: fsck not clean"
echo "  unsealed volume: rollback works, bit-exact"
rm -f "$IMGF.2"

echo
echo "ROLLBACK E2E: PASS"
