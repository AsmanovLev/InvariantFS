#!/usr/bin/env bash
set -euo pipefail

ROOT="/home/user/InvariantFS"
SRC="$ROOT/src-extracted/VFS/src"
DOC="$ROOT/src-extracted/VFS/doc"
OUT="$ROOT/impl_docs"

rm -rf "$OUT"
mkdir -p "$OUT/functions" "$OUT/types"

for f in "$SRC"/*.c "$SRC"/*.h; do
  b=$(basename "$f")
  ctags -x --c-kinds=f "$f" > "$OUT/functions/${b}.txt" || true
done

{
  echo "# Function Index (all sources)"
  echo
  echo "| symbol | line | file |"
  echo "|---|---|---|"
  cat "$OUT"/functions/*.txt | awk '{gsub(/:.*/,"",$3); print "| "$1" | "$2" | "$3" |"}'
} > "$OUT/FUNCTIONS.md"

for f in "$SRC"/*.h "$SRC"/volume.c "$SRC"/fuse_fs.c "$SRC"/blkio.c; do
  b=$(basename "$f")
  ctags -x --c-kinds=stgu "$f" > "$OUT/types/${b}.txt" || true
done

{
  echo "# Type Index (structs/unions/enums/typedefs)"
  echo
  echo "| name | kind | line | file |"
  echo "|---|---|---|---|"
  cat "$OUT"/types/*.txt | awk '{print "| "$1" | "$2" | "$3" | "$4" |"}' | sort -t'|' -k2
} > "$OUT/TYPES.md"

{
  echo "# File Map"
  echo
  echo "## Sources (.c) by size"
  wc -l "$SRC"/*.c | sort -rn | head -40
  echo
  echo "## Headers (.h) by size"
  wc -l "$SRC"/*.h | sort -rn
} > "$OUT/FILEMAP.md"

{
  echo "# Doc-to-Code Map (Linux-relevant audit lanes)"
  echo
  echo "| doc | audit lane | primary sources |"
  echo "|---|---|---|"
  echo "| 02-on-disk-format | A1 format | volume.h, volume.c, mkfs.c, fsck.c |"
  echo "| 03-ast-recipe, 04-compression-matrix, 05-data-classification, 06-sweep-worker | A2 semantics/transcode | sweep.c, arc.c/h, flacx.c, pngx.c/h, tarx.c, zip.c, gzrepro.c |"
  echo "| 07-read-write-path, 15-caching | A3 IO/cache | volume.c, blkio.c/h, fuse_fs.c |"
  echo "| 08-crash-recovery, 12-enospc-strategy | A4 recovery | volume.c, fsck.c, enospctest.c |"
  echo "| 13-linux-rootfs + ChangeLog + build_linux.sh + f6*.log + tests_fuse.ps1 | A5 port-critical | fuse_fs.c vs doc recipe |"
  echo "| 10-deduplication, 11-security-and-permissions, 18-test-coverage | A6 dedup/security/tests | volume.c, fuse_fs.c, dokan_fs.c, tests.ps1, devtest*.c |"
  echo
  echo "Skipped per scope: 09-windows-port, 14-windows-io-deep, 16-benchmarks, 17-template-zone"
} > "$OUT/DOCMAP.md"

ls "$OUT"
