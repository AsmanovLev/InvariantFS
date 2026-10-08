#!/bin/bash
# bench-rootfs-erofs.sh -- EROFS density leg (mkfs.erofs was apt-installed
# after the main bench started). Density only: the box kernel has no erofs
# support, so the image cannot be mounted/read here.
# Same corpus, same workdir, own results file (merged into the doc later).
set -u
WORK="${1:-/srv/flakey/invfs-bench}"
CORPUS=$WORK/corpus/usr
RES=$WORK/RESULTS-erofs.txt
[ -f "$WORK/corpus.ready" ] || { echo "corpus not ready, run bench-rootfs.sh first"; exit 1; }
echo "== erofs density @ $(date -u +%FT%TZ) ==" | tee -a "$RES"
S=$(date +%s); mkfs.erofs -zzstd,level=15 "$WORK/root.erofs" "$CORPUS" >/dev/null 2>&1; echo "mkfs.erofs rc=$? $(( $(date +%s) - S ))s" | tee -a "$RES"
echo "erofs image: $(du -sb "$WORK/root.erofs" | cut -f1)B" | tee -a "$RES"
