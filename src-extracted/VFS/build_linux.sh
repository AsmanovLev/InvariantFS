#!/bin/bash
# build Linux CLI tools
set -e
cd /mnt/d/VFS
mkdir -p build_linux
B3="src/blake3.c src/blake3_dispatch.c src/blake3_portable.c"
for t in mkfs verify fsck cp cat ls sweep stat zip; do
  case $t in
    mkfs)   src='src/mkfs.c'; EXTRA='src/miniz.c';;
    verify) src='src/verify.c'; EXTRA='src/miniz.c';;
    fsck)    src='src/fsck.c'; EXTRA='src/miniz.c';;
    cp)     src='src/cp.c'; EXTRA='src/miniz.c';;
    cat)    src='src/cat.c'; EXTRA='src/miniz.c';;
    ls)     src='src/ls.c'; EXTRA='src/miniz.c';;
    sweep)  src='src/sweep.c'; EXTRA='src/miniz.c';;
    stat)   src='src/stat.c'; EXTRA='src/miniz.c';;
    zip)    src='src/zip.c'; EXTRA='src/miniz.c';;
  esac
  gcc -O2 -o build_linux/invf-$t $src src/volume.c src/arc.c src/crc32c.c src/lz4.c src/flacx.c src/tarx.c src/pngx.c $EXTRA $B3 \
      -DINVFS_EMBED_FLACX -DMINIZ_NO_ZLIB_APIS -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
      -Wl,-l:libzstd.so.1 -lz -Isrc -pthread || { echo "FAIL $t"; continue; }
  echo "OK invf-$t"
done
