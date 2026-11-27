#!/bin/bash
# bench-opt.sh -- is a different -O level actually faster in THIS tree?
#
# Written because -O3 is the reflex, and in this engine it is ~28% SLOWER on the
# path that matters. The measurement is reproducible in about five minutes; the
# numbers and the reasoning are in impl_docs/BENCH-OPTIMISATION.md.
#
# Usage:  tools/bench-opt.sh [-O2 -O3]      (default: -O2 -O3)
# Env:    REPS=3                           (medians of REPS runs)
#
# Discipline this script keeps, on purpose:
#   * the corpus is REAL and mixed (text, shared objects, binaries, urandom),
#     because a syscall-bound corpus makes every build look identical: 11 MiB of
#     small files measured as a tie, and the difference only appeared at 79 MiB;
#   * the CPU-heavy stage (invf-sweep: codecs, batches, dedupe) is timed
#     separately from import, because import does NOT compress -- it writes and
#     folds. Timing import alone answers a question nobody asked;
#   * both builds MUST produce the same deep-verify result, or the comparison is
#     meaningless and the script says so and fails.
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$PWD
REPS=${REPS:-3}
OPTS=${*:--O2 -O3}
[ $# -eq 0 ] && OPTS="-O2 -O3"

echo "== building the variants"
for opt in $OPTS; do
    tag=$(printf '%s' "$opt" | tr -d '-')
    make -j"$(nproc)" OPT="$opt" OUT="bin-$tag" OBJ="build/obj-$tag" \
        "bin-$tag/invf-mkfs" "bin-$tag/invf-import" "bin-$tag/invf-sweep" \
        "bin-$tag/invf-verify" "bin-$tag/invf-ls" "bin-$tag/invf-fsck" \
        >"/tmp/bench-opt-build-$tag.log" 2>&1 \
        && echo "   $opt ok" \
        || { echo "   $opt FAILED (see /tmp/bench-opt-build-$tag.log)"; exit 1; }
done

echo "== staging a real corpus"
W=${BENCH_OPT_WORK:-/dev/shm/invfs-bench-opt}
rm -rf "$W"; mkdir -p "$W/corpus"; cd "$W" || exit 1
cp -a "$REPO/src" corpus/src 2>/dev/null
mkdir -p corpus/lib corpus/bin
for f in /usr/lib/x86_64-linux-gnu/lib{crypto,ssl,zstd,lz4,stdc++}.so*; do
    [ -f "$f" ] && cp -a "$f" corpus/lib/ 2>/dev/null
done
for b in /usr/bin/{bash,git,gcc,ld,objdump}; do
    [ -f "$b" ] && cp -a "$b" corpus/bin/ 2>/dev/null
done
head -c 4000000 /dev/urandom > corpus/random.bin
echo "   $(find corpus -type f | wc -l) files, $(du -sh corpus | cut -f1)"

echo "== measuring ($REPS rep(s) each; median reported)"
for opt in $OPTS; do
    tag=$(printf '%s' "$opt" | tr -d '-')
    B=$REPO/bin-$tag
    imps=""; sweeps=""; deeps=""; results=""
    i=1
    while [ "$i" -le "$REPS" ]; do
        rm -f "$tag.img"
        "$B/invf-mkfs" "$tag.img" 0.5 >/dev/null 2>&1 || { echo "mkfs failed"; exit 1; }
        t0=$(date +%s%N); "$B/invf-import" "$tag.img" corpus >/dev/null 2>&1; t1=$(date +%s%N)
        t2=$(date +%s%N); "$B/invf-sweep" "$tag.img" >/dev/null 2>&1; t3=$(date +%s%N)
        t4=$(date +%s%N); "$B/invf-verify" "$tag.img" --deep >/tmp/bench-opt-deep-$tag.log 2>&1; t5=$(date +%s%N)
        imps="$imps $(( (t1-t0)/1000000 ))"
        sweeps="$sweeps $(( (t3-t2)/1000000 ))"
        deeps="$deeps $(( (t5-t4)/1000000 ))"
        results="$results $(sed -n 's/^deep: \(.*\)$/\1/p' /tmp/bench-opt-deep-$tag.log | tail -1)"
        i=$((i+1))
    done
    med() { printf '%s\n' $1 | sort -n | sed -n "$(( ($2 + 1) / 2 ))p"; }
    echo "   $opt: import=$(med "$imps" "$REPS")ms[$imps ]  sweep=$(med "$sweeps" "$REPS")ms[$sweeps ]  deep=$(med "$deeps" "$REPS")ms[$deeps ]"
    echo "   $opt: text=$(size $B/* 2>/dev/null | awk 'NR>1 && $1+0==$1 {t+=$1} END {print t}')"
done

echo "== equality check (a speed difference on different results is not a result)"
first=""; bad=0
for opt in $OPTS; do
    tag=$(printf '%s' "$opt" | tr -d '-')
    res=$(sed -n 's/^deep: \(.* live inode(s): .* ok, .* corrupt\).*/\1/p' "/tmp/bench-opt-deep-$tag.log" | tail -1)
    [ -z "$first" ] && first="$res"
    [ "$res" = "$first" ] || { echo "   $opt produced a DIFFERENT result: [$res] vs [$first]"; bad=1; }
done
[ "$bad" = 0 ] || { echo "== FAIL: results differ, the timings above compare different work"; exit 1; }
echo "   identical on every variant: $first"
echo "== done; record what you found in impl_docs/BENCH-OPTIMISATION.md if it changed"
