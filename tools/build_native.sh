#!/bin/bash
# Build InvariantFS native tools on Linux.
# Adapted from VFS build_linux.sh + tests_fuse.ps1 build line.
# Usage: tools/build_native.sh [tool ...]   (default: all portable tools)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src"
OUT="$ROOT/bin"
LOG="$ROOT/logs/build_native.log"
mkdir -p "$OUT" "$ROOT/logs"
cd "$SRC"
: > "$LOG"

CORE="src/core/volume.c src/core/vol_cpack.c src/core/vol_png.c src/core/vol_seal.c src/core/vol_repair.c src/core/vol_rollback.c src/core/vol_resize.c src/core/vol_fsck.c src/core/vol_crash.c src/core/vol_exer.c src/core/vol_dedupe.c src/core/vol_textzone.c src/core/vol_heat.c src/core/vol_sweep.c src/core/vol_read.c src/core/vol_write.c src/core/vol_records.c src/core/vol_ast.c src/core/vol_dirs.c src/core/vol_tier.c src/core/arc.c src/core/crc32c.c src/codecs/lz4.c src/recipes/flacx.c src/recipes/tarx.c src/recipes/pngx.c src/core/blkio.c src/codecs/ppmd8.c src/codecs/ppmd8enc.c src/codecs/ppmd8dec.c src/codecs/ppmd_codec.c src/codecs/codec.c src/codecs/bcj_x86.c src/codecs/rs.c"
B3="src/codecs/blake3.c src/codecs/blake3_dispatch.c src/codecs/blake3_portable.c"
COMMON="-std=gnu11 -O2 -DINVFS_EMBED_FLACX -DMINIZ_NO_ZLIB_APIS \
 -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
 -Wl,-l:libzstd.so.1 -lz -I$SRC/src -I$SRC/src/core -I$SRC/src/codecs -I$SRC/src/recipes -I$SRC/src/vendor7z -pthread"

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
    mkfs)       build invf-mkfs       "$SRC/src/cli/mkfs.c"        $CORE "$SRC/src/codecs/miniz.c" $B3 || RC=1 ;;
    verify)     build invf-verify     "$SRC/src/cli/verify.c"      $CORE "$SRC/src/codecs/miniz.c" $B3 || RC=1 ;;
    fsck)       build invf-fsck       "$SRC/src/cli/fsck.c"        $CORE "$SRC/src/codecs/miniz.c" $B3 || RC=1 ;;
    cp)         build invf-cp         "$SRC/src/cli/cp.c"          $CORE "$SRC/src/codecs/miniz.c" $B3 || RC=1 ;;
    cat)        build invf-cat        "$SRC/src/cli/cat.c"         $CORE "$SRC/src/codecs/miniz.c" $B3 || RC=1 ;;
    ls)         build invf-ls         "$SRC/src/cli/ls.c"          $CORE "$SRC/src/codecs/miniz.c" $B3 || RC=1 ;;
    stat)       build invf-stat       "$SRC/src/cli/stat.c"        $CORE "$SRC/src/codecs/miniz.c" $B3 || RC=1 ;;
    zip)        build invf-zip        "$SRC/src/recipes/zip.c"         $CORE $B3 || RC=1 ;;  # zip.c embeds miniz itself
    arctest)    build invf-arctest    "$SRC/src/cli/arctest.c"     $CORE "$SRC/src/codecs/miniz.c" $B3 || RC=1 ;;
    blkio_test) build invf-blkio_test "$SRC/src/cli/blkio_test.c"  "$SRC/src/core/blkio.c" "$SRC/src/core/crc32c.c" || RC=1 ;;
    fuse)
      gcc $COMMON $(pkg-config --cflags fuse3) -o "$OUT/invf-fuse" \
        "$SRC/src/cli/fuse_fs.c" $CORE "$SRC/src/codecs/miniz.c" $B3 \
        $(pkg-config --libs fuse3) >>"$LOG" 2>&1 \
        && echo "OK   invf-fuse" || { echo "FAIL invf-fuse   (logs/build_native.log)"; RC=1; } ;;
    sweep)      echo "SKIP sweep      Windows-only CLI; sweep runs inside invf-fuse daemon" ;;
    treecp)     echo "SKIP treecp     Windows-only CLI; use invf-fuse mount instead" ;;
    *)          echo "SKIP $t unknown target"; RC=1 ;;
  esac
done
exit $RC
