#!/usr/bin/env bash
# WP116 reproducer: 60-file corpus, INVFS_META_FRAC=16, 1 GB volume.
#
# Stages the corpus, imports it, sweeps N times, and reports the exact
# 4 KiB block delta vs an empty volume of identical geometry plus the
# block census at each stage. Every step is bounded well under 60 s.
set -u

BIN="${BIN:-/tmp/invfs-wp116/bin}"
WORK="${WORK:-/home/user/.wp116-work}"
SWEEPS="${SWEEPS:-8}"
CORPUS="${CORPUS:-$WORK/corpus}"

mkdir -p "$WORK"

# ---- corpus: 60 files of compressible text, ~2.8 KB each --------------
if [ ! -d "$CORPUS" ]; then
  mkdir -p "$CORPUS"
  for i in $(seq -w 1 60); do
    {
      echo "/* file $i -- InvariantFS WP116 leak reproducer */"
      for j in $(seq 1 40); do
        echo "line $j: the quick brown fox jumps over the lazy dog $i $j"
      done
    } > "$CORPUS/f$i.txt"
  done
fi
PAYLOAD=$(cat "$CORPUS"/* | wc -c)
echo "corpus: 60 files, $PAYLOAD B payload"

# ---- empty reference volume (identical geometry) ----------------------
mk() { # $1 = image path
  rm -f "$1"
  INVFS_META_FRAC=16 "$BIN/invf-mkfs" "$1" 1024 >/dev/null 2>&1
}

mk "$WORK/empty.img"
mk "$WORK/test.img"

EMPTY_FREE=$("$BIN/wp116_census" "$WORK/empty.img" 2>/dev/null | awk '/^free_blocks/{print $2}')
EMPTY_ALLOC=$("$BIN/wp116_census" "$WORK/empty.img" 2>/dev/null | awk '/^alloc_blocks/{print $2}')
echo "empty volume: alloc=$EMPTY_ALLOC free=$EMPTY_FREE"

# ---- import -----------------------------------------------------------
"$BIN/invf-import" "$WORK/test.img" "$CORPUS" >/dev/null 2>&1 \
  || { echo "import FAILED"; exit 1; }
echo "import done"

report() { # $1 = label
  local out
  out=$("$BIN/wp116_census" "$WORK/test.img" 2>/dev/null)
  local alloc pages live dead
  alloc=$(echo "$out" | awk '/^alloc_blocks/{print $2}')
  pages=$(echo "$out" | awk '/^v3 base pages/{print $4}')
  live=$(echo  "$out" | awk '/^live base pages/{print $4}')
  dead=$(echo "$out" | awk '/^unreachable base pages/{print $4}')
  echo "[$1] alloc=$alloc  base_pages=$pages  live=$live  unreachable=$dead  unreachable_bytes=$((dead*4096))"
  echo "$out" | sed -n '/base pages by generation/,$p'
}

report "after import"

# ---- sweeps -----------------------------------------------------------
for n in $(seq 1 "$SWEEPS"); do
  "$BIN/invf-sweep" "$WORK/test.img" >"$WORK/sweep.$n.log" 2>&1
  rc=$?
  echo "sweep $n rc=$rc  $(grep -iE 'reclaim|fold' "$WORK/sweep.$n.log" | head -3 | tr '\n' ' ')"
  report "after sweep $n"
done

# ---- final arithmetic -------------------------------------------------
OUT=$("$BIN/wp116_census" "$WORK/test.img" 2>/dev/null)
FINAL_ALLOC=$(echo "$OUT" | awk '/^alloc_blocks/{print $2}')
DEAD=$(echo "$OUT" | awk '/^unreachable base pages/{print $4}')
DELTA=$((FINAL_ALLOC - EMPTY_ALLOC))
echo
echo "=== WP116 summary ==="
echo "empty alloc blocks      : $EMPTY_ALLOC"
echo "swept  alloc blocks      : $FINAL_ALLOC"
echo "exact block delta       : $DELTA  ($((DELTA*4096)) B)"
echo "payload                 : $PAYLOAD B"
echo "unreachable base pages  : $DEAD ($((DEAD*4096)) B)"
echo "expansion               : $(echo "scale=6; ($DELTA*4096)/$PAYLOAD" | bc)x"
