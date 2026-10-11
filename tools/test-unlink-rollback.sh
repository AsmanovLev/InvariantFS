#!/bin/bash
# test-unlink-rollback.sh — WP604: a failing vol_unlink must not mutate.
#
# THE DEFECT THIS IS THE RED CONTROL FOR
#
# vol_unlink returned -1 while still dropping the dirent when deleting a
# file whose recipe would not load (seen on a stale-sealed volume: sealed,
# post-seal write, delete -> rc=-1, name gone, fsck OK). A failing unlink
# that mutates is the worst combination: callers that check rc believe
# nothing happened while the namespace changed under them. The contract is
# rc==0 iff the namespace changed; the fix rolls the partial unlink back
# (row first, dirent second -- the create ordering) and keeps the -1 loud
# with the namespace unchanged (src/core/vol_dirs.c: vol_unlink).
#
# WHAT THIS ASSERTS (fixture: sealed volume, one post-seal write = stale)
#
#   leg A: fault-armed recipe load failure at unlink -> rc!=0 AND the
#      dirent still present (CONSISTENT-FAIL), stderr says "rolled back",
#      the file still reads back bit-exact, fsck OK. Pre-fix this leg is
#      INCONSISTENT (rc=-1, dirent gone) and the script fails.
#   leg B: fault-armed inode row write at hardlink-unlink (nlink>1 path)
#      -> rc!=0 AND both names present on the same inode, fsck nlink ok.
#   leg C: the unarmed paths are untouched -- plain unlink, hardlink
#      link/unlink cycle, and delete on the stale volume all succeed and
#      fsck stays OK (rename-over is covered by invf-pbaref_test's rename
#      legs in test-shard-3, run by make test).
#
# The fault seams (INVFS_FAULT=recipe_load / inode_delta_put) stand in for
# the production states they were built for -- an unloadable recipe and a
# failed delta append -- so each leg also asserts the seam FIRED (the
# rollback diagnostic on A, the nonzero rc on B); a leg the fault never
# reached cannot pass.
#
# Run from the repo root after `make`:  bash tools/test-unlink-rollback.sh
# Uses /dev/shm (tmpfs). Under make test it runs inside TESTISO, which
# gives it a private /dev/shm.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp604rb
IMG=wp604rb.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== build the helper (public API only) =="
cat > "$WORK/unlinkrb.c" <<'RB_EOF'
/* unlinkrb — WP604 helper: unlinkrb <img> <cmd> ...
 *   unlink <name>      delete one file; report rc + presence before/after.
 *                      exit 0 = rc==0 and gone (consistent-ok),
 *                           1 = rc!=0 and present (consistent-fail),
 *                           3 = mismatch either way (INCONSISTENT).
 *   hardlink <from> <to>
 *   ino <name>         print "<name>: present=<0|1> ino=<id>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"

static int cmd_unlink(invfs_volume *v, const char *name)
{
    uint64_t ino = 0;
    int before, after, rc;
    before = (vol_find_rc(v, name, &ino) == 1 && ino);
    printf("before: present=%d\n", before);
    fflush(stdout);
    rc = vol_delete_file(v, name);
    printf("vol_delete_file rc=%d\n", rc);
    fflush(stdout);
    if (rc == 0 && vol_flush(v) != 0) { printf("flush FAILED\n"); return 2; }
    ino = 0;
    after = (vol_find_rc(v, name, &ino) == 1 && ino);
    printf("after: present=%d\n", after);
    if (rc == 0 && !after) { printf("CONSISTENT-OK\n"); return 0; }
    if (rc != 0 && after) { printf("CONSISTENT-FAIL\n"); return 1; }
    printf("INCONSISTENT: rc=%d present=%d\n", rc, after);
    return 3;
}

int main(int argc, char **argv)
{
    int err = 0, rc;
    invfs_volume *v;
    if (argc < 3) { fprintf(stderr, "usage: unlinkrb <img> <cmd> ...\n"); return 2; }
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 2; }
    if (strcmp(argv[2], "unlink") == 0 && argc == 4) {
        rc = cmd_unlink(v, argv[3]);
    } else if (strcmp(argv[2], "hardlink") == 0 && argc == 5) {
        rc = vol_hardlink(v, argv[3], argv[4]);
        printf("vol_hardlink rc=%d\n", rc);
        if (rc == 0 && vol_flush(v) != 0) { printf("flush FAILED\n"); rc = 2; }
        else rc = rc ? 1 : 0;
    } else if (strcmp(argv[2], "ino") == 0 && argc == 4) {
        uint64_t ino = 0;
        int f = (vol_find_rc(v, argv[3], &ino) == 1 && ino);
        printf("%s: present=%d ino=%llu\n", argv[3], f,
               (unsigned long long)ino);
        rc = 0;
    } else {
        fprintf(stderr, "bad cmd\n");
        vol_close(v);
        return 2;
    }
    vol_close(v);
    return rc;
}
RB_EOF
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -o "$WORK/unlinkrb" \
    "$WORK/unlinkrb.c" \
    $(for t in volume vol_cpack helper_exec tool_scratch vol_plugin_client vol_png vol_seal vol_repair vol_resize vol_fsck vol_crash vol_exer vol_dedupe vol_textzone vol_heat vol_sweep vol_read vol_write vol_records vol_ast vol_dirs vol_tier vol_meta_merge vol_metabuf vol_btree vol_delta vol_fold vol_reclaim vol_spt0 vol_anchor vol_walk arc crc32c lz4 flacx tarx pngx blkio miniz blake3 blake3_dispatch blake3_portable ppmd8 ppmd8enc ppmd8dec ppmd_codec codec bcj_x86 rs deflate_repro deflate_backend_system deflate_backend_stock; do printf "$REPO/build/obj/$t.o "; done) \
    $REPO/build/obj/zlib_stock_*.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
RB="$WORK/unlinkrb"

echo "== fixture: sealed volume, then one post-seal write (stale) =="
$B/invf-mkfs "$IMG" 0.2 >/dev/null
python3 - <<'PY'
import random
random.seed(604)
d = "/dev/shm/wp604rb/orig"
WORDS = "the quick brown fox jumps over lazy dogs int static return if while for struct char void NULL size_t uint64_t\n".split()
def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(d + "/" + name, "w").write(" ".join(out)[:size])
text_file("victim.txt", 90000)
text_file("keeper.txt", 60000)
open(d + "/new.txt", "w").write("post-seal bytes " * 4000)
print("corpus ready")
PY
$B/invf-cp "$IMG" "$WORK/orig/victim.txt" victim.txt >/dev/null
$B/invf-cp "$IMG" "$WORK/orig/keeper.txt" keeper.txt >/dev/null
$B/invf-sweep "$IMG" >/dev/null 2>&1
$B/invf-sweep "$IMG" --seal > "$WORK/seal.log" 2>&1
grep -q "verified clean" "$WORK/seal.log" || fail "seal did not verify clean"
$B/invf-cp "$IMG" "$WORK/orig/new.txt" new.txt >/dev/null
set +e
$B/invf-verify "$IMG" --deep > "$WORK/verify-stale.log" 2>&1
VRC=$?
set -e
[ "$VRC" != 0 ] || fail "verify --deep passed over a post-seal write (not stale?)"
echo "  stale as required (verify rc=$VRC)"

echo "== leg A: recipe will not load at unlink -> fail WITHOUT mutating =="
set +e
INVFS_FAULT="recipe_load:4" $RB "$IMG" unlink victim.txt > "$WORK/legA.log" 2>&1
ARC=$?
set -e
cat "$WORK/legA.log"
[ "$ARC" = 1 ] || fail "leg A: expected CONSISTENT-FAIL (exit 1), got exit $ARC"
grep -q "CONSISTENT-FAIL" "$WORK/legA.log" || fail "leg A: no CONSISTENT-FAIL line"
grep -q "rolled back" "$WORK/legA.log" \
    || fail "leg A: no 'rolled back' diagnostic (fault may not have landed)"
# the rolled-back file is fully functional, not a zombie
$B/invf-cat "$IMG" victim.txt "$WORK/out/victim.bin" >/dev/null 2>&1 \
    || fail "leg A: rolled-back victim unreadable"
cmp -s "$WORK/orig/victim.txt" "$WORK/out/victim.bin" \
    || fail "leg A: rolled-back victim not bit-exact"
$B/invf-fsck "$IMG" > "$WORK/fsckA.log" 2>&1 || fail "leg A: fsck failed"
grep -q "^OK$" "$WORK/fsckA.log" || fail "leg A: fsck not OK"
echo "  leg A green (rc!=0, present, bit-exact, fsck OK)"

echo "== leg B: hardlink unlink with failing row write -> fail WITHOUT mutating =="
$RB "$IMG" hardlink keeper.txt keeper2.txt >/dev/null 2>&1 \
    || fail "leg B setup: hardlink failed"
set +e
INVFS_FAULT="inode_delta_put:1" $RB "$IMG" unlink keeper2.txt > "$WORK/legB.log" 2>&1
BRC=$?
set -e
cat "$WORK/legB.log"
[ "$BRC" = 1 ] || fail "leg B: expected CONSISTENT-FAIL (exit 1), got exit $BRC"
grep -q "CONSISTENT-FAIL" "$WORK/legB.log" || fail "leg B: no CONSISTENT-FAIL line"
$RB "$IMG" ino keeper.txt > "$WORK/ino1.log" 2>&1
$RB "$IMG" ino keeper2.txt > "$WORK/ino2.log" 2>&1
cat "$WORK/ino1.log" "$WORK/ino2.log"
I1=$(sed 's/.*ino=//' "$WORK/ino1.log")
I2=$(sed 's/.*ino=//' "$WORK/ino2.log")
grep -q "present=1" "$WORK/ino1.log" || fail "leg B: keeper.txt gone"
grep -q "present=1" "$WORK/ino2.log" || fail "leg B: keeper2.txt gone"
[ "$I1" = "$I2" ] || fail "leg B: survivors not the same inode ($I1 vs $I2)"
$B/invf-fsck "$IMG" > "$WORK/fsckB.log" 2>&1 || fail "leg B: fsck failed"
grep -q "^OK$" "$WORK/fsckB.log" || fail "leg B: fsck not OK"
grep -q "nlink/fan-in:.*ok" "$WORK/fsckB.log" || fail "leg B: nlink check not ok"
echo "  leg B green (rc!=0, both names on ino $I1, fsck OK)"

echo "== leg C: unarmed paths unchanged (plain + hardlink + stale delete) =="
$RB "$IMG" unlink victim.txt > "$WORK/legC1.log" 2>&1 \
    || fail "leg C: plain unlink of the rolled-back victim failed"
grep -q "CONSISTENT-OK" "$WORK/legC1.log" || fail "leg C: plain unlink not clean"
$RB "$IMG" unlink keeper2.txt > "$WORK/legC2.log" 2>&1 \
    || fail "leg C: hardlink-name unlink failed"
grep -q "CONSISTENT-OK" "$WORK/legC2.log" || fail "leg C: hardlink unlink not clean"
$RB "$IMG" unlink keeper.txt > "$WORK/legC3.log" 2>&1 \
    || fail "leg C: stale-volume delete failed"
grep -q "CONSISTENT-OK" "$WORK/legC3.log" || fail "leg C: stale delete not clean"
$B/invf-cat "$IMG" new.txt "$WORK/out/new.bin" >/dev/null 2>&1 \
    || fail "leg C: survivor unreadable"
cmp -s "$WORK/orig/new.txt" "$WORK/out/new.bin" \
    || fail "leg C: survivor not bit-exact"
$B/invf-fsck "$IMG" > "$WORK/fsckC.log" 2>&1 || fail "leg C: fsck failed"
grep -q "^OK$" "$WORK/fsckC.log" || fail "leg C: fsck not OK"
echo "  leg C green (all unarmed unlinks succeed, fsck OK)"

echo
echo "PASS: unlink rollback (WP604)"
