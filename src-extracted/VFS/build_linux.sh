#!/bin/bash
# build Linux CLI tools
set -e
cd /mnt/d/VFS
mkdir -p build_linux
B3="src/codecs/blake3.c src/codecs/blake3_dispatch.c src/codecs/blake3_portable.c"
for t in mkfs verify fsck cp cat ls sweep stat zip; do
  case $t in
    mkfs)   src='src/cli/mkfs.c'; EXTRA='src/codecs/miniz.c';;
    verify) src='src/cli/verify.c'; EXTRA='src/codecs/miniz.c';;
    fsck)    src='src/cli/fsck.c'; EXTRA='src/codecs/miniz.c';;
    cp)     src='src/cli/cp.c'; EXTRA='src/codecs/miniz.c';;
    cat)    src='src/cli/cat.c'; EXTRA='src/codecs/miniz.c';;
    ls)     src='src/cli/ls.c'; EXTRA='src/codecs/miniz.c';;
    sweep)  src='src/cli/sweep.c'; EXTRA='src/codecs/miniz.c';;
    stat)   src='src/cli/stat.c'; EXTRA='src/codecs/miniz.c';;
    zip)    src='src/recipes/zip.c'; EXTRA='src/codecs/miniz.c';;
  esac
  gcc -O2 -o build_linux/invf-$t $src src/core/volume.c src/core/arc.c src/core/crc32c.c src/codecs/lz4.c src/recipes/flacx.c src/recipes/tarx.c src/recipes/pngx.c $EXTRA $B3 \
      -DINVFS_EMBED_FLACX -DMINIZ_NO_ZLIB_APIS -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
      -Wl,-l:libzstd.so.1 -lz -Isrc -Isrc/core -Isrc/codecs -Isrc/recipes -Isrc/vendor7z -pthread || { echo "FAIL $t"; continue; }
  echo "OK invf-$t"
done
