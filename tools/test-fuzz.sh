#!/bin/bash
# test-fuzz.sh — REDUCED fuzz pass (the make-e2e-suitable slice of the
# tools/fuzz/ wave; the full passes live in the fuzz tools themselves).
#
#   1. bitflip.py    40 image-mutation iterations (data/journal/inode/
#                    superblock/descriptor regions; flip/zero4K/truncate/
#                    crafted-descriptor; fsck+verify --deep+bit-exact cats)
#   2. opseq.py      2 images x 120 random ops vs a shadow tree
#   3. packfuzz.py   60 mutants per available containerpack helper
#   4. fuzz_manifest 200 malformed-manifest cases through the real
#                    pack registration path
#
# Everything is deterministic (fixed default seeds); failures print their
# repro lines. Packs whose fixture tooling is missing are skipped, never
# failed. Total runtime target: well under 5 minutes.
#
# Run from the repo root after `make`:  bash tools/test-fuzz.sh
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
FZ=$REPO/tools/fuzz
OBJ=$REPO/build/obj
SHM=/dev/shm

[ -x "$B/invf-mkfs" ] || { echo "FAIL: run make first"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3"; exit 1; }
command -v cc >/dev/null || { echo "FAIL: cc"; exit 1; }

echo "== build fuzz harness helpers =="
cc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$FZ/ophelper" \
    "$FZ/ophelper.c" \
    $OBJ/{volume,vol_cpack,vol_png,vol_seal,vol_repair,vol_rollback,vol_resize,vol_fsck,vol_crash,vol_exer,vol_dedupe,vol_textzone,vol_heat,vol_sweep,vol_read,vol_write,vol_records,vol_ast,vol_dirs,arc,crc32c,lz4,flacx,tarx,pngx,blkio,miniz,blake3,blake3_dispatch,blake3_portable,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,codec,bcj_x86,rs,vol_tier}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
cc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$FZ/fuzz_manifest" \
    "$FZ/fuzz_manifest.c" \
    $OBJ/{codec,ppmd8,ppmd8enc,ppmd8dec,ppmd_codec,lz4,bcj_x86}.o \
    -Wl,-l:libzstd.so.1 -lz -lpthread
echo "helpers built"

echo "== fuzz 1/4: bitflip (60 iterations) =="
python3 "$FZ/bitflip.py" --iterations 60 --seed 0x1A2B3C4D \
    --size-gb 0.12 --seal-every 8 --workdir $SHM/invf-fuzz-bitflip-e2e

echo "== fuzz 2/4: opseq (2 images x 150 ops) =="
python3 "$FZ/opseq.py" --images 2 --ops 150 --seed 0x05E9 \
    --workdir $SHM/invf-fuzz-opseq-e2e

echo "== fuzz 3/4: packfuzz (100 mutants per available pack) =="
PACKS="rawdisk vdi"     # fixture needs only python3 + cc
command -v mkfs.ext4 >/dev/null && command -v debugfs >/dev/null \
    && PACKS="$PACKS ext4fs"
command -v mkfs.vfat >/dev/null && command -v mcopy >/dev/null \
    && PACKS="$PACKS fatfs"
if command -v mkfs.xfs >/dev/null && sudo -n true 2>/dev/null; then
    PACKS="$PACKS xfs"
fi
if command -v mkfs.ntfs >/dev/null && sudo -n true 2>/dev/null; then
    PACKS="$PACKS ntfs"
fi
command -v qemu-img >/dev/null && command -v qemu-io >/dev/null \
    && PACKS="$PACKS qcow2"
echo "packs: $(echo $PACKS | tr ' ' ',')"
python3 "$FZ/packfuzz.py" --mutants 100 --seed 0x9AC4 \
    --packs "$(echo $PACKS | tr ' ' ',')" \
    --workdir $SHM/invf-fuzz-packs-e2e

echo "== fuzz 4/4: manifest (400 malformed cases) =="
INVFS_FUZZ_CASES=$SHM/invf-fuzz-manifest-e2e "$FZ/fuzz_manifest" 400 0xCAFE \
    | tail -2

rm -rf $SHM/invf-fuzz-bitflip-e2e $SHM/invf-fuzz-opseq-e2e \
    $SHM/invf-fuzz-packs-e2e $SHM/invf-fuzz-manifest-e2e
echo "PASS: fuzz reduced pass clean"
