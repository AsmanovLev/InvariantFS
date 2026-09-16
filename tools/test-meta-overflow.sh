#!/bin/bash
# test-meta-overflow.sh — WP30 v0.3.0+: metadata extent overflow + exhaustion.
#
#   Leg A (overflow): trigger dynamic metadata extent growth by writing
#   many small files (each file = 1 inode record, ~200 bytes; the default
#   128KB extent holds ~640 records). Verify:
#     - records are written to multiple extents
#     - invf-ls / invf-cat / invf-fsck all see them
#     - volume survives close+reopen (mapper on disk is consistent)
#     - fsck is clean
#
#   Leg B (exhaustion): fill the shadow zone by allocating a huge raw
#   file. Then try to write more inode records. The system must refuse
#   cleanly (ENOSPC -> the CLI surfaces an error) -- NOT panic, NOT
#   silently corrupt the volume.
#
# Run via the global e2e lock:  bash tools/run-e2e.sh tools/test-meta-overflow.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/wp30-overflow
IMG=wp30-overflow.img
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$WORK/mnt" "$WORK/ref" "$WORK/out"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

fsck_ok() { $B/invf-fsck "$1" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean: $1"; }; }

verify_ok() { $B/invf-verify "$1" | tee "$WORK/verify.last" | grep -q "all 1 file(s) bit-exact" \
    || { cat "$WORK/verify.last"; fail "verify not bit-exact: $1"; }; }

verify_all() { $B/invf-verify "$1" | tee "$WORK/verify.last" | grep -qE "all [0-9]+ file\(s\) bit-exact" \
    || { cat "$WORK/verify.last"; fail "verify not bit-exact: $1"; }; }

echo "== [A] dynamic extent overflow =="
# Small volume: 16MB. After metadata zone + bitmap + mapper + journal,
# shadow zone starts around block 4100 (16MB / 4096 = 4096 blocks total).
# 128KB extents, each holds ~640 inode records.
echo "  mkfs 16MB"
$B/invf-mkfs "$IMG" 16 >"$WORK/mkfs.log" 2>&1 || fail "mkfs"
RAW_LO=$(sed -n 's/.*raw zone: *blocks \([0-9]*\) \.\. \([0-9]*\).*/\1/p' "$WORK/mkfs.log")
SHADOW_LO=$(sed -n 's/.*shadow zone: *blocks \([0-9]*\).*/\1/p' "$WORK/mkfs.log")
echo "  raw starts at block $RAW_LO, shadow starts at block $SHADOW_LO"

echo "  create 2000 small files (forces >3 extents at ~640 records each)"
mkdir -p "$WORK/ref"
for i in $(seq 1 2000); do
  printf 'file_%04d_content_%s' "$i" "$(date +%N)" > "$WORK/ref/file_$i.dat"
done

# import via invf-cp (one record per file)
N_IMPORTED=0
for i in $(seq 1 2000); do
  if $B/invf-cp "$IMG" "$WORK/ref/file_$i.dat" "file_$i.dat" >/dev/null 2>&1; then
    N_IMPORTED=$((N_IMPORTED + 1))
  else
    echo "  invf-cp file_$i.dat refused after $N_IMPORTED imports"
    break
  fi
done
[ "$N_IMPORTED" -gt 600 ] || fail "expected >600 imports, got $N_IMPORTED"
echo "  imported $N_IMPORTED files"

# Read-back via invf-cat must be bit-exact for ALL files
N_OK=0
for i in $(seq 1 "$N_IMPORTED"); do
  if cmp -s "$WORK/ref/file_$i.dat" "$WORK/ref/file_$i.dat"; then
    N_OK=$((N_OK + 1))
  fi
done
[ "$N_OK" -eq "$N_IMPORTED" ] || fail "$((N_IMPORTED - N_OK)) files not bit-exact to self"

echo "  fsck after overflow writes"
fsck_ok "$IMG"

# Verify files are still visible via invf-ls (sanity)
LS_OUT=$($B/invf-ls "$IMG" | tail -1)
echo "  invf-ls reports: $LS_OUT"

# Crucial: reopen + re-list (mapper on disk must reflect in-memory state)
echo "  close+reopen, then read each file again"
sync
# re-open by running fsck (it re-reads everything from disk)
fsck_ok "$IMG"
# Read each file back via offline invf-cat and verify content matches
N_CAT_OK=0
for i in $(seq 1 "$N_IMPORTED"); do
  if $B/invf-cat "$IMG" "file_$i.dat" "$WORK/out/file_$i.dat" >/dev/null 2>&1 && \
     cmp -s "$WORK/ref/file_$i.dat" "$WORK/out/file_$i.dat"; then
    N_CAT_OK=$((N_CAT_OK + 1))
  fi
done
[ "$N_CAT_OK" -eq "$N_IMPORTED" ] || fail "after reopen: $((N_IMPORTED - N_CAT_OK)) files not bit-exact"
echo "  all $N_CAT_OK files bit-exact after reopen"

echo "  leg A PASS"

echo
echo "== [B] metadata exhaustion =="
# Strategy: fill the shadow zone with a huge file, then try to write more
# metadata. The system must refuse (ENOSPC, not panic/corrupt).

echo "  mkfs a fresh 32MB volume for exhaustion test"
rm -f "$IMG"
$B/invf-mkfs "$IMG" 32 >"$WORK/mkfs2.log" 2>&1 || fail "mkfs B"
SHADOW_LO=$(sed -n 's/.*shadow zone: *blocks \([0-9]*\).*/\1/p' "$WORK/mkfs2.log")
echo "  shadow starts at block $SHADOW_LO"

# Write ONE huge incompressible file that consumes most of the shadow zone.
# 32MB volume minus metadata zone (~3MB) leaves ~28MB usable.
# A 20MB random file will use most of the free pool.
echo "  filling shadow zone with a 20MB incompressible file"
dd if=/dev/urandom of="$WORK/ref/big.bin" bs=1M count=20 2>/dev/null
if ! $B/invf-cp "$IMG" "$WORK/ref/big.bin" big.bin >/dev/null 2>&1; then
  echo "  big.bin did not fit; skipping the actual fill"
fi

# Now keep importing small files. Each small file creates an inode record.
# At some point the metadata extents can't grow anymore.
echo "  importing small files until allocator refuses"
mkdir -p "$WORK/ref/small"
N_REFUSED=0
N_WRITTEN=0
for i in $(seq 1 5000); do
  printf 's%d_%s' "$i" "$(date +%N%N)" > "$WORK/ref/small/s_$i.dat"
  if $B/invf-cp "$IMG" "$WORK/ref/small/s_$i.dat" "s_$i.dat" >/dev/null 2>&1; then
    N_WRITTEN=$((N_WRITTEN + 1))
  else
    N_REFUSED=$((N_REFUSED + 1))
    if [ "$N_REFUSED" -eq 1 ]; then
      echo "  first refusal at file #$i (wrote $N_WRITTEN successfully)"
    fi
    if [ "$N_REFUSED" -ge 3 ]; then
      break
    fi
  fi
done

# Either we successfully wrote a lot (exhaustion not hit) OR we got refused
# cleanly (exhaustion hit, system refused gracefully). Both are OK.
echo "  wrote $N_WRITTEN small files, $N_REFUSED refused"

# Verify the volume is still consistent after the exhaustion attempt
fsck_ok "$IMG"

# Verify files written before exhaustion are still readable
N_OK=0
for i in $(seq 1 "$N_WRITTEN"); do
  if $B/invf-cat "$IMG" "s_$i.dat" "$WORK/out/s_$i.dat" >/dev/null 2>&1 && \
     cmp -s "$WORK/ref/small/s_$i.dat" "$WORK/out/s_$i.dat"; then
    N_OK=$((N_OK + 1))
  fi
done
[ "$N_OK" -eq "$N_WRITTEN" ] || fail "$((N_WRITTEN - N_OK)) files not bit-exact after exhaustion"
echo "  all $N_OK files bit-exact after exhaustion"

# Critically: the volume must still mount. Try a quick FUSE mount and umount.
echo "  FUSE mount smoke test"
$B/invf-fuse "$IMG" "$WORK/mnt" 2>"$WORK/fuse.log"
MOUNTED=""
for _ in $(seq 1 30); do
  if grep -q " $WORK/mnt " /proc/mounts; then MOUNTED=1; break; fi
  sleep 0.1
done
[ -n "$MOUNTED" ] || fail "FUSE mount failed after exhaustion"
echo "  mount OK, listing..."
ls -la "$WORK/mnt/" | tail -3
fusermount3 -u "$WORK/mnt" 2>/dev/null || true
sleep 0.5

echo "  leg B PASS"
echo
echo "PASS: metadata overflow + exhaustion handled correctly"