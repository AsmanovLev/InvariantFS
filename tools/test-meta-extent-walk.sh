#!/bin/bash
# tools/test-meta-extent-walk.sh
#
# Regression: inode records spanning MULTIPLE dynamic metadata extents must all
# be visible to lookup, ls, and compaction.
#
# Guards the WP30 per-extent-walk bug:
#   - compact_scan_live reported "live set 1 vs index N disagree" and declined;
#   - lookup of a record stored in an OLDER extent returned ENOENT.
#
# Strategy: use invf-cp in a tight loop (offline, no FUSE -- FUSE mounts hang
# in some CI environments). invf-cp uses the same write path as FUSE (same
# meta_get_append_pos, same vol_inode_next lookups), so the mapper iteration
# is fully exercised. Then offline invf-ls + invf-sweep verify the records
# are findable AND compaction agrees on the live set.
#
# Legs:
#   A. write a sentinel dir + sentinel file FIRST (lands in oldest extent),
#      then bulk-import NF FILES to force allocation of additional extents.
#      Use invf-cp to import a directory tree (mkdir + many file cp's) so
#      records land in BOTH older AND newer extents.
#   B. cross-extent lookup: invf-cat each sentinel record by name; if the
#      scanner only reads the active extent, ENOENT.
#   C. compaction: invf-sweep must NOT print "live set ... vs index ...
#      disagree". invf-fsck must be clean.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/tmp/wp30-extwalk
IMG=$WORK/volume.img
SRC=$WORK/src
OUT=$WORK/out
NFILES="${NFILES:-1500}"
PASS=0; FAIL=0

# Best-effort cleanup of stale FUSE mounts in WORK (ignore failures).
[ -d "$WORK/mnt" ] && timeout 2 fusermount3 -uz "$WORK/mnt" 2>/dev/null
rm -rf "$WORK" 2>/dev/null
mkdir -p "$WORK" "$WORK/src" "$WORK/out"
cd /tmp
rm -f volume.img

say(){ echo "[extwalk] $*"; }
ok(){ PASS=$((PASS+1)); say "PASS: $*"; }
bad(){ FAIL=$((FAIL+1)); say "FAIL: $*"; }

fail(){ bad "$@"; echo "RESULT: $PASS passed, $FAIL failed"; exit 1; }

echo "== mkfs =="
dd if=/dev/zero of="$IMG" bs=1M count=1 seek=2048 status=none
"$B/invf-mkfs" "$IMG" >/dev/null || fail mkfs

# ---- Leg A: sentinel FIRST (oldest extent), then bulk to force rotation ----
say "Leg A: sentinel + $NFILES bulk files"
mkdir -p "$SRC/olddir"
echo sentinel > "$SRC/olddir/sentinel"
mkdir -p "$SRC/bulkdir"
for i in $(seq 1 "$NFILES"); do
  printf "data%05d_%s\n" "$i" "$(date +%N%N)" > "$SRC/bulkdir/file_$i"
done

# Import the sentinel FIRST (oldest extent)
"$B/invf-cp" "$IMG" "$SRC/olddir/sentinel" "olddir/sentinel" >/dev/null 2>&1 \
  || fail "cp sentinel failed"

# Now bulk-import to force rotation into a new extent
BULK_OK=0
BULK_FAIL=0
for i in $(seq 1 "$NFILES"); do
  if "$B/invf-cp" "$IMG" "$SRC/bulkdir/file_$i" "bulkdir/file_$i" >/dev/null 2>&1; then
    BULK_OK=$((BULK_OK + 1))
  else
    BULK_FAIL=$((BULK_FAIL + 1))
    [ "$BULK_FAIL" -eq 1 ] && say "first refusal at bulkfile_$i (wrote $BULK_OK so far)"
    [ "$BULK_FAIL" -ge 10 ] && break
  fi
done
echo "  imported $BULK_OK bulk files, $BULK_FAIL refused"

# ---- Leg B: cross-extent lookup ----
say "Leg B: cross-extent lookup"

# Sentinel (oldest extent) must be findable by name
if "$B/invf-cat" "$IMG" "olddir/sentinel" "$OUT/sentinel" >/dev/null 2>&1 \
   && cmp -s "$SRC/olddir/sentinel" "$OUT/sentinel"; then
  ok "sentinel (oldest extent) readable by name"
else
  bad "sentinel in oldest extent NOT findable -> lookup only scanned active extent"
fi

# Bulk files (newer extents) must also be findable by name
N_FIND=0
for i in $(seq 1 "$BULK_OK"); do
  if "$B/invf-cat" "$IMG" "bulkdir/file_$i" "$OUT/find_$i" >/dev/null 2>&1 \
     && cmp -s "$SRC/bulkdir/file_$i" "$OUT/find_$i"; then
    N_FIND=$((N_FIND + 1))
  fi
done
if [ "$N_FIND" -eq "$BULK_OK" ]; then
  ok "all $BULK_OK bulk files (newer extents) readable by name"
else
  bad "only $N_FIND/$BULK_OK bulk files findable -> lookup undercounts"
fi

# Total visible count via invf-ls must match
ALL_FILES=$("$B/invf-ls" "$IMG" | grep -E '^[0-9]+ file' | head -1 \
            | sed -n 's/^\([0-9]\+\) file(s)/\1/p')
[ "$ALL_FILES" = "$((BULK_OK + 1))" ] && ok "invf-ls reports all $((BULK_OK + 1)) files" \
                                     || bad "invf-ls reports $ALL_FILES, expected $((BULK_OK + 1))"

# ---- Leg C: compaction & fsck ----
say "Leg C: compaction & fsck"

SWEEP_OUT=$("$B/invf-sweep" "$IMG" 2>&1)
if echo "$SWEEP_OUT" | grep -Eq 'live set [0-9]+ vs index [0-9]+ disagree'; then
  bad "sweep compaction declined (live set != index)"
  echo "$SWEEP_OUT" | grep -E 'live set|declined' | head
else
  ok "sweep compaction did not decline on live/index mismatch"
fi

FSCK_OUT=$("$B/invf-fsck" "$IMG" 2>&1)
ORPHANS=$(echo "$FSCK_OUT" | grep -E '^  orphans:' | awk '{print $2}')
MISSING=$(echo "$FSCK_OUT" | grep -E '^  missing:' | awk '{print $2}')
BAD=$(echo "$FSCK_OUT" | grep -E '^  bad records:' | awk '{print $3}')
if [ "${ORPHANS:-0}" = "0" ] && [ "${MISSING:-0}" = "0" ] && [ "${BAD:-0}" = "0" ]; then
  ok "fsck clean (orphans=0, missing=0, bad=0)"
else
  bad "fsck not clean (orphans=$ORPHANS, missing=$MISSING, bad=$BAD)"
  echo "$FSCK_OUT" | head -15
fi

# invf-verify must succeed
if "$B/invf-verify" "$IMG" >/dev/null 2>&1; then
  ok "invf-verify bit-exact"
else
  bad "invf-verify failed"
fi

echo "RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]