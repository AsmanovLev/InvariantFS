#!/bin/bash
# test-dynzone.sh — WP-DZ dynamic zone sizing e2e (persistent regression).
#
# The mkfs-time RAW=1/5 / Shadow=rest split is ADVISORY POLICY, not a hard
# region split: one shared free-block pool, allocation requests carry a
# zone PREFERENCE, and the RAW -> SHADOW "spill" is gone as a special
# mechanism. Raw-class writes prefer the raw extent; once RAW's fair share
# cannot satisfy a request the SAME allocation continues into shadow-space
# blocks with the content class still RAW (zone=0) -- placement no longer
# decides what a block is. Shadow-class content keeps canonical
# shadow-side placement (the seal stripes are pba-ranges over the shadow
# extent).
#
#   Leg 1 (overflow, offline path): fill a small image past the old RAW
#     share with incompressible data via invf-cp. Writes continue; EVERY
#     segment of every file keeps zone=0 (raw-class -- the spill's BINARY
#     tagging is gone); the late files' entries carry pbas inside the
#     shadow extent; read-back bit-exact; fsck clean.
#   Leg 2 (stats by content class): invf-stats / vol_compute_stats report
#     raw_used_bytes ABOVE the raw extent's byte size (class accounting,
#     not region accounting) and SHADOW class still empty; unclaimed ~0.
#   Leg 3 (mounted write path): with the raw extent full, a session write
#     through the FUSE mount succeeds (into shadow space), reads back
#     bit-exact, fsck clean after unmount.
#   Leg 4 (the sweep sees the overflow -- the pre-WP-DZ bug): a
#     compressible file written while RAW is full sits raw-classed in the
#     shadow extent; invf-sweep transcodes it (zone flips to BINARY),
#     bit-exact, the raw extent drains after the realize pass; fsck +
#     verify --deep clean.
#   Leg 5 (seal interplay, image B): seal stripes stay over the shadow
#     extent by pba range, so raw-class blocks that overflowed there are
#     covered like any occupant: seal WITHOUT a sweep (dzpick), corrupt
#     one overflow block, read self-heals bit-exact via stripe parity.
#   Leg 6 (resize interplay, image B): unseal, grow 512M -> 768M: the
#     advisory RAW share is unchanged, the shadow side absorbs the whole
#     delta; fsck clean, files bit-exact, a post-grow write lands.
#
# Run via the global e2e lock:  bash tools/run-e2e.sh tools/test-dynzone.sh
# Uses /dev/shm like the other soak scripts. NOTE: blkio treats /dev/*
# paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere (the mountpoint is absolute).
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wpdynzone
IMG1=wpdynzone-a.img    # legs 1-4 (fill/stats/mount/sweep)
IMG2=wpdynzone-b.img    # legs 5-6 (seal + resize)
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/ref" "$WORK/out" "$WORK/tools"
cd /dev/shm
rm -f "$IMG1" "$IMG2"

fail() { echo "FAIL: $*" >&2; exit 1; }

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse $IMG1" 2>/dev/null || true
    pkill -f "invf-fuse $IMG2" 2>/dev/null || true
}
trap cleanup EXIT

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

deep_ok() { $B/invf-verify "$1" --deep | tail -1 | grep -q " 0 corrupt," \
    || fail "verify --deep not clean: $1"; }

mnt_up() {   # daemonized mount, wait for /proc/mounts
    $B/invf-fuse "$1" "$MNT" 2>"$WORK/fuse.$1.log"
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
    for _ in $(seq 1 300); do
        pgrep -f "invf-fuse $1" >/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

zone_used() { # <img> raw|shadow -> used blocks in the REGION (pba range)
    $B/meta_probe "$1" --zonefree 2>/dev/null \
      | awk -v w="$2" '{for(i=1;i<NF;i+=2) if ($i==w) {split($(i+1),a,"/"); print a[1]}}'; }
zone_total() {
    $B/meta_probe "$1" --zonefree 2>/dev/null \
      | awk -v w="$2" '{for(i=1;i<NF;i+=2) if ($i==w) {split($(i+1),a,"/"); print a[2]}}'; }
# ast tag histogram of a file: "zone=0:N zone=2:M"
zones_of() { $B/meta_probe "$1" --heat "$2" 2>/dev/null \
    | awk '/^ast /{for(i=1;i<=NF;i++) if ($i ~ /^zone=/){sub("zone=","",$i); z[$i]++}}
           END{for(k in z) printf "zone=%s:%d ", k, z[k]}'; }
# 0 = the file has at least one L2P entry at pba >= $3
has_pba_past() { $B/meta_probe "$1" --heat "$2" 2>/dev/null \
    | awk -v lo="$3" '/^ast /{for(i=1;i<=NF;i++) if ($i ~ /^pba=/)
        {sub("pba=","",$i); if ($i+0 >= lo) found=1}} END{exit !found}'; }

echo "== build the dzpick helper (public API only; sealpick convention) =="
cat > "$WORK/tools/dzpick.c" <<'DZPICK_EOF'
/* dzpick -- WP-DZ test helper (uses only the public volume.h API).
 *
 *   dzpick <img> seal | unseal   -> vol_seal without the sweep walk (so
 *                                  raw-class blocks stay in place under
 *                                  the fresh stripes)
 *   dzpick <img> firstshadow <name> -> first pba of the file's maps that
 *                                  sits inside the shadow extent
 *   dzpick <img> classstats      -> "raw_used shadow_used text_used
 *                                  unclaimed raw_region_blocks" (bytes,
 *                                  exact -- the parseable form of what
 *                                  invf-stats prints rounded)
 */
#include <stdio.h>
#include <stdlib.h>
#include "volume.h"
#include "invarifs.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    if (argc < 3) { fprintf(stderr, "usage: dzpick <img> <cmd> [arg]\n"); return 2; }
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open failed err=%d\n", err); return 1; }
    if (strcmp(argv[2], "seal") == 0 || strcmp(argv[2], "unseal") == 0) {
        invfs_seal_report rep;
        int rc = vol_seal(v, strcmp(argv[2], "unseal") == 0, &rep);
        if (rc == 0 && strcmp(argv[2], "seal") == 0)
            printf("sealed: %llu stripes, %llu parity blocks\n",
                   (unsigned long long)rep.stripes,
                   (unsigned long long)rep.parity_blocks);
        vol_close(v);
        return rc ? 1 : 0;
    }
    if (strcmp(argv[2], "firstshadow") == 0) {
        /* WP27: the file's segments live in its record's AST entries
         * (pbas inline); the WAL is owner-only, so this reads the live
         * record directly (the meta_probe pattern) */
        const invfs_superblock *sb = vol_sb(v);
        uint64_t id = vol_find(v, argv[3]);
        uint64_t pos;
        if (!id) { vol_close(v); return 1; }
        pos = vol_inode_area_start(v);
        while (pos) {
            uint32_t magic, rl;
            uint64_t ino, fsz, np;
            np = vol_inode_next(v, pos, &magic, &ino, &fsz, NULL, 0, &rl);
            if (!np) break;
            pos = np;
            if (magic != INODE_REC_MAGIC || ino != id) continue;
            {
                uint8_t *rb = malloc(rl);
                invfs_ast_hdr ah;
                uint32_t k;
                int done = 0;
                if (rb && vol_read_raw(v, np - rl - 4, rb, rl) == 0) {
                    size_t base = (size_t)(invfs_rec_cbody(
                                       (const invfs_inode_rec *)rb) - rb);
                    if (invfs_ast_hdr_parse(rb + base, rl - base, &ah) == 0) {
                        for (k = 0; k < ah.num_blocks; k++) {
                            const invfs_ast_block_entry *e =
                                (const invfs_ast_block_entry *)
                                (rb + base + ah.hdr_len +
                                 (size_t)k * sizeof(*e));
                            if (e->length &&
                                e->pba >= sb->shadow_zone_start) {
                                printf("%llu\n", (unsigned long long)e->pba);
                                free(rb);
                                vol_close(v);
                                return 0;
                            }
                        }
                    }
                }
                free(rb);
                (void)done;
            }
        }
        vol_close(v);
        return 1;   /* no shadow-extent block */
    }
    if (strcmp(argv[2], "classstats") == 0) {
        invfs_volume_stats st;
        if (vol_compute_stats(v, &st) != 0) { vol_close(v); return 1; }
        printf("raw_used=%llu shadow_used=%llu text_used=%llu "
               "unclaimed=%llu raw_region_blocks=%llu\n",
               (unsigned long long)st.raw_used_bytes,
               (unsigned long long)st.shadow_used_bytes,
               (unsigned long long)st.text_used_bytes,
               (unsigned long long)st.unclaimed_used_bytes,
               (unsigned long long)vol_sb(v)->raw_zone_blocks);
        vol_close(v);
        return 0;
    }
    vol_close(v);
    return 2;
}
DZPICK_EOF
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/tools/dzpick" \
    "$WORK/tools/dzpick.c" \
    $REPO/build/obj/{volume,vol_cpack,vol_png,vol_seal,vol_repair,vol_rollback,vol_resize,vol_fsck,vol_crash,vol_exer,vol_dedupe,vol_textzone,vol_heat,vol_sweep,vol_meta_merge,vol_read,vol_write,vol_records,vol_ast,vol_dirs,vol_tier,arc,crc32c,lz4,blkio,flacx,tarx,pngx,miniz,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,blake3,blake3_dispatch,blake3_portable,rs}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
DZ=$WORK/tools/dzpick

echo "== fixtures =="
python3 - "$WORK/ref" <<'PY'
import os, sys
d = sys.argv[1]
rnd = open('/dev/urandom','rb').read
for k in range(1, 5):
    open('%s/f%d.bin' % (d, k), 'wb').write(rnd(32 * 1024 * 1024))
words = (b'the quick brown fox jumps over the lazy dog invariant fs segment '
         b'pack alpha beta gamma delta\n')
open('%s/words.txt' % d, 'wb').write((words * 100000)[:8 * 1024 * 1024])
open('%s/small.bin' % d, 'wb').write(rnd(1024 * 1024))
print("  4x32MB incompressible + 8MB compressible + 1MB small")
PY

echo
echo "== [1] fill past the old RAW share: writes continue, raw-classed =="
$B/invf-mkfs "$IMG1" 0.5 > "$WORK/mkfs1.log"
RAW_LO=$(sed -n 's/.*raw zone: *blocks \([0-9]*\) \.\. \([0-9]*\).*/\1/p' "$WORK/mkfs1.log")
RAW_HI=$(sed -n 's/.*raw zone: *blocks \([0-9]*\) \.\. \([0-9]*\).*/\2/p' "$WORK/mkfs1.log")
SHADOW_LO=$(sed -n 's/.*shadow zone: *blocks \([0-9]*\) \.\. \([0-9]*\).*/\1/p' "$WORK/mkfs1.log")
RAWB=$(zone_total "$IMG1" raw)
[ -n "$RAW_LO" ] && [ -n "$SHADOW_LO" ] && [ -n "$RAWB" ] \
    || fail "could not parse zone geometry"
echo "  raw extent: $RAW_LO..$RAW_HI ($RAWB blocks), shadow starts at $SHADOW_LO"
for k in 1 2 3 4; do
    $B/invf-cp "$IMG1" "$WORK/ref/f$k.bin" "f$k.bin" >/dev/null \
        || fail "leg1: invf-cp f$k.bin refused (write did not continue)"
    Z=$(zones_of "$IMG1" "f$k.bin")
    echo "  f$k.bin tags: $Z"
    [ "$Z" = "zone=0:512 " ] || fail "leg1: f$k.bin not all raw-class: $Z"
    $B/invf-cat "$IMG1" "f$k.bin" "$WORK/out/f$k.bin" >/dev/null
    cmp "$WORK/ref/f$k.bin" "$WORK/out/f$k.bin" \
        || fail "leg1: f$k.bin not bit-exact"
done
# 4x32MB = 128MB of raw-class content on a ~94MB raw extent: the later
# files MUST have blocks inside the shadow extent (placement proof)
has_pba_past "$IMG1" f4.bin "$SHADOW_LO" \
    || fail "leg1: f4.bin has no blocks past the raw extent"
RU=$(zone_used "$IMG1" raw)
echo "  raw region occupancy after fill: $RU/$RAWB blocks"
[ "$RU" -gt $(( RAWB * 95 / 100 )) ] \
    || fail "leg1: raw extent not actually exhausted ($RU/$RAWB)"
fsck_ok "$IMG1"
echo "  4x32MB written past the 94MB raw share, all zone=0, bit-exact, fsck OK"

echo
echo "== [2] stats: per-zone used by CONTENT CLASS, not by region =="
CS=$($DZ "$IMG1" classstats) || fail "leg2: classstats failed"
echo "  $CS"
RAW_USED=$(echo "$CS" | sed -n 's/.*raw_used=\([0-9]*\).*/\1/p')
SH_USED=$(echo "$CS" | sed -n 's/.*shadow_used=\([0-9]*\).*/\1/p')
UNC=$(echo  "$CS" | sed -n 's/.*unclaimed=\([0-9]*\).*/\1/p')
RREG=$( echo "$CS" | sed -n 's/.*raw_region_blocks=\([0-9]*\).*/\1/p')
# raw-CLASS bytes (4x32MB = 134217728 + framing) exceed the raw REGION
# (RREG*4096): region accounting could never say this
[ "$RAW_USED" -gt $(( RREG * 4096 )) ] \
    || fail "leg2: raw-class bytes $RAW_USED do not exceed the raw region ($RREG blocks)"
[ "$SH_USED" = "0" ] || fail "leg2: shadow class not empty pre-sweep ($SH_USED)"
[ "$UNC" -lt 1048576 ] || fail "leg2: unclaimed bytes too large ($UNC)"
$B/invf-stats "$IMG1" | grep -q "content class" \
    || fail "leg2: invf-stats lacks the content-class header"
echo "  raw class $RAW_USED B > raw region $(( RREG * 4096 )) B; shadow class 0; unclaimed $UNC"

echo
echo "== [3] mounted session write past the share =="
mnt_up "$IMG1"
python3 - "$MNT" "$WORK/ref/small.bin" <<'PY'
import sys
open(sys.argv[1] + '/m.bin', 'wb').write(open(sys.argv[2], 'rb').read())
PY
sync
cmp "$WORK/ref/small.bin" "$MNT/m.bin" || fail "leg3: mounted read-back differs"
mnt_down "$IMG1"
has_pba_past "$IMG1" m.bin "$SHADOW_LO" \
    || fail "leg3: mounted write did not land in shadow space"
$B/invf-cat "$IMG1" m.bin "$WORK/out/m.bin" >/dev/null
cmp "$WORK/ref/small.bin" "$WORK/out/m.bin" || fail "leg3: offline read differs"
fsck_ok "$IMG1"
echo "  mounted write succeeded with the raw extent full; bit-exact; fsck OK"

echo
echo "== [4] the sweep sees the overflow (raw-class blocks in shadow space) =="
# compressible 8MB, written while the raw extent is full: lands
# raw-classed (LZ4) in the shadow extent
$B/invf-cp "$IMG1" "$WORK/ref/words.txt" words.txt >/dev/null || fail "leg4: cp words.txt"
Z=$(zones_of "$IMG1" words.txt)
case "$Z" in
    "zone=0:"*) ;;                       # every segment raw-classed
    *) fail "leg4: words.txt not all raw-class: $Z";;
esac
has_pba_past "$IMG1" words.txt "$SHADOW_LO" \
    || fail "leg4: words.txt did not overflow into shadow space"
RUB=$(zone_used "$IMG1" raw)
$B/invf-sweep "$IMG1" > "$WORK/sweep1.log" 2>&1 \
    || { cat "$WORK/sweep1.log"; fail "leg4: sweep failed"; }
grep -q "sweep done" "$WORK/sweep1.log" || fail "leg4: sweep incomplete"
grep -q "failed=0" "$WORK/sweep1.log" || fail "leg4: sweep reports failures"
Z=$(zones_of "$IMG1" words.txt)
echo "  words.txt tags after sweep: $Z"
case "$Z" in
    *"zone=0"*) fail "leg4: sweep did not transcode the overflow file";; esac
$B/invf-cat "$IMG1" words.txt "$WORK/out/words.txt" >/dev/null
cmp "$WORK/ref/words.txt" "$WORK/out/words.txt" || fail "leg4: words.txt not bit-exact"
# second bare sweep auto-realizes the checkpoint (frees the retained
# blocks); only then does the raw extent show the drain
$B/invf-sweep "$IMG1" > "$WORK/sweep2.log" 2>&1 \
    || { cat "$WORK/sweep2.log"; fail "leg4: realize sweep failed"; }
RUA=$(zone_used "$IMG1" raw)
echo "  raw region occupancy: $RUB before sweep -> $RUA after realize"
[ "$RUA" -lt "$RUB" ] || fail "leg4: sweep did not drain the raw extent"
CS=$($DZ "$IMG1" classstats)
echo "  $CS"
SH_USED=$(echo "$CS" | sed -n 's/.*shadow_used=\([0-9]*\).*/\1/p')
[ "$SH_USED" -gt 100000000 ] || fail "leg4: shadow class too small post-sweep"
fsck_ok "$IMG1"
deep_ok "$IMG1"
echo "  overflow transcoded RAW->SHADOW, raw extent drained, fsck+verify clean"

echo
echo "== [5] seal: stripes over the shadow extent cover overflow blocks =="
$B/invf-mkfs "$IMG2" 0.5 > "$WORK/mkfs2.log"
SHADOW_LO2=$(sed -n 's/.*shadow zone: *blocks \([0-9]*\) \.\. \([0-9]*\).*/\1/p' "$WORK/mkfs2.log")
[ -n "$SHADOW_LO2" ] || fail "leg5: could not parse shadow start"
for k in 1 2 3; do
    $B/invf-cp "$IMG2" "$WORK/ref/f$k.bin" "f$k.bin" >/dev/null || fail "leg5: cp f$k.bin"
done
has_pba_past "$IMG2" f3.bin "$SHADOW_LO2" || fail "leg5: no overflow on image B"
# seal WITHOUT a sweep: the raw-class overflow blocks stay in place and
# land under the stripes like any occupied shadow-extent block
$DZ "$IMG2" seal > "$WORK/seal-b.log" || { cat "$WORK/seal-b.log"; fail "leg5: seal failed"; }
cat "$WORK/seal-b.log"
grep -q "stripes" "$WORK/seal-b.log" || fail "leg5: no stripes reported"
# corrupt one overflow block of f3.bin (raw-class, shadow extent)
VICTIM=$($DZ "$IMG2" firstshadow f3.bin) || fail "leg5: f3.bin has no shadow block"
echo "  corrupting block $VICTIM (a raw-class block in the shadow extent)"
dd if=/dev/urandom of="$IMG2" bs=4096 count=1 seek="$VICTIM" conv=notrunc 2>/dev/null
# the read must self-heal through the stripe parity, bit-exact
$B/invf-cat "$IMG2" f3.bin "$WORK/out/f3.healed" 2> "$WORK/heal.log" \
    || fail "leg5: read of the corrupted file failed"
cmp "$WORK/ref/f3.bin" "$WORK/out/f3.healed" \
    || fail "leg5: healed read not bit-exact"
grep -q "\[seal\] recovered block $VICTIM" "$WORK/heal.log" \
    || { cat "$WORK/heal.log"; fail "leg5: no parity recovery log"; }
fsck_ok "$IMG2"
deep_ok "$IMG2"
echo "  overflow block recovered via stripe parity; fsck+verify clean"

echo
echo "== [6] resize: the advisory RAW share persists, shadow absorbs growth =="
$DZ "$IMG2" unseal >/dev/null || fail "leg6: unseal failed"
RT_B=$(zone_total "$IMG2" raw);  ST_B=$(zone_total "$IMG2" shadow)
$B/invf-resize "$IMG2" 768M | tee "$WORK/resize.log" | grep -q "invf-resize: OK" \
    || fail "leg6: grow did not report OK"
RT_A=$(zone_total "$IMG2" raw);  ST_A=$(zone_total "$IMG2" shadow)
echo "  raw extent blocks: $RT_B -> $RT_A (advisory share must not move)"
echo "  shadow blocks:     $ST_B -> $ST_A (tail growth lands shadow-side)"
[ "$RT_A" = "$RT_B" ] || fail "leg6: advisory raw share moved on grow"
[ "$ST_A" -gt "$ST_B" ] || fail "leg6: shadow side did not absorb the growth"
for k in 1 2 3; do
    $B/invf-cat "$IMG2" "f$k.bin" "$WORK/out/f$k.rs" >/dev/null
    cmp "$WORK/ref/f$k.bin" "$WORK/out/f$k.rs" || fail "leg6: f$k.bin differs post-grow"
done
# post-grow write lands in the fresh tail (shadow side), still raw-class
$B/invf-cp "$IMG2" "$WORK/ref/words.txt" postgrow.txt >/dev/null || fail "leg6: post-grow cp"
Z=$(zones_of "$IMG2" postgrow.txt)
case "$Z" in
    "zone=0:"*) ;;
    *) fail "leg6: post-grow file not raw-class: $Z";;
esac
$B/invf-cat "$IMG2" postgrow.txt "$WORK/out/postgrow.txt" >/dev/null
cmp "$WORK/ref/words.txt" "$WORK/out/postgrow.txt" || fail "leg6: post-grow not bit-exact"
fsck_ok "$IMG2"
deep_ok "$IMG2"
echo "  grow: share unchanged, tail absorbed shadow-side, content bit-exact"

rm -f "$IMG1" "$IMG2"
echo
echo "DYNZONE E2E: PASS"
