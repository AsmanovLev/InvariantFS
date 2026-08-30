#!/bin/bash
# Build InvariantFS native tools on Linux.
# Adapted from VFS build_linux.sh + tests_fuse.ps1 build line.
# Usage: tools/build_native.sh [tool ...]   (default: all portable tools)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src-extracted/VFS"
OUT="$ROOT/bin"
LOG="$ROOT/logs/build_native.log"
mkdir -p "$OUT" "$ROOT/logs"
cd "$SRC"
: > "$LOG"

CORE="src/volume.c src/vol_cpack.c src/vol_png.c src/vol_seal.c src/vol_repair.c src/vol_rollback.c src/vol_resize.c src/vol_fsck.c src/vol_crash.c src/vol_exer.c src/vol_dedupe.c src/vol_textzone.c src/vol_heat.c src/vol_sweep.c src/vol_read.c src/vol_write.c src/vol_records.c src/vol_ast.c src/vol_dirs.c src/arc.c src/crc32c.c src/lz4.c src/flacx.c src/tarx.c src/pngx.c src/blkio.c"
B3="src/blake3.c src/blake3_dispatch.c src/blake3_portable.c"
COMMON="-std=gnu11 -O2 -DINVFS_EMBED_FLACX -DMINIZ_NO_ZLIB_APIS \
 -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
 -Wl,-l:libzstd.so.1 -lz -I$SRC/src -pthread"

build() { # <name> <srcs...>
  local name="$1"; shift
  if gcc $COMMON -o "$OUT/$name" "$@" >>"$LOG" 2>&1; then
    echo "OK   $name"
  else
    echo "FAIL $name   (logs/build_native.log)"
    return 1
  fi
}

TOOLS="${*:-mkfs verify fsck cp cat ls stat zip arctest blkio_test fuse}"
RC=0
for t in $TOOLS; do
  case $t in
    mkfs)       build invf-mkfs       "$SRC/src/mkfs.c"        $CORE "$SRC/src/miniz.c" $B3 || RC=1 ;;
    verify)     build invf-verify     "$SRC/src/verify.c"      $CORE "$SRC/src/miniz.c" $B3 || RC=1 ;;
    fsck)       build invf-fsck       "$SRC/src/fsck.c"        $CORE "$SRC/src/miniz.c" $B3 || RC=1 ;;
    cp)         build invf-cp         "$SRC/src/cp.c"          $CORE "$SRC/src/miniz.c" $B3 || RC=1 ;;
    cat)        build invf-cat        "$SRC/src/cat.c"         $CORE "$SRC/src/miniz.c" $B3 || RC=1 ;;
    ls)         build invf-ls         "$SRC/src/ls.c"          $CORE "$SRC/src/miniz.c" $B3 || RC=1 ;;
    stat)       build invf-stat       "$SRC/src/stat.c"        $CORE "$SRC/src/miniz.c" $B3 || RC=1 ;;
    zip)        build invf-zip        "$SRC/src/zip.c"         $CORE $B3 || RC=1 ;;  # zip.c embeds miniz itself
    arctest)    build invf-arctest    "$SRC/src/arctest.c"     $CORE "$SRC/src/miniz.c" $B3 || RC=1 ;;
    blkio_test) build invf-blkio_test "$SRC/src/blkio_test.c"  "$SRC/src/blkio.c" "$SRC/src/crc32c.c" || RC=1 ;;
    fuse)
      gcc $COMMON $(pkg-config --cflags fuse3) -o "$OUT/invf-fuse" \
        "$SRC/src/fuse_fs.c" $CORE "$SRC/src/miniz.c" $B3 \
        $(pkg-config --libs fuse3) >>"$LOG" 2>&1 \
        && echo "OK   invf-fuse" || { echo "FAIL invf-fuse   (logs/build_native.log)"; RC=1; } ;;
    sweep)      echo "SKIP sweep      Windows-only CLI; sweep runs inside invf-fuse daemon" ;;
    treecp)     echo "SKIP treecp     Windows-only CLI; use invf-fuse mount instead" ;;
    *)          echo "SKIP $t unknown target"; RC=1 ;;
  esac
done
exit $RC
