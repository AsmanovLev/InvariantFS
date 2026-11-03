#!/bin/bash
# test_png.sh — PNG lossless recompression codecpack end-to-end test
# Verifies png.codecpack CLI helper and InvariantFS integration
# (invf-mkfs, invf-cp, invf-sweep, invf-verify, invf-cat).
#
# Guarantees 100% byte-for-byte fidelity (sha256(orig) == sha256(rebuilt)).

set -e
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -d "$SCRIPT_DIR/bin" ]; then
    REPO="${REPO:-$(cd "$SCRIPT_DIR/../../.." && pwd)}"
    PACK_DIR="$SCRIPT_DIR"
else
    REPO="${REPO:-$(cd "$SCRIPT_DIR/.." && pwd)}"
    PACK_DIR="$REPO/tools/codecpacks/png.codecpack"
fi
BIN_DIR="$REPO/bin"

WORK="/dev/shm/wp_png"
IMG="wp_png.img"

trap 'rm -rf "$WORK" "/dev/shm/$IMG"' EXIT

echo "=== 1. Environment & Preflight Checks ==="
command -v cjxl >/dev/null || { echo "FAIL: cjxl not found"; exit 1; }
command -v djxl >/dev/null || { echo "FAIL: djxl not found"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 not found"; exit 1; }

# Locate InvariantFS binaries
for cmd in invf-mkfs invf-cp invf-sweep invf-cat invf-verify; do
    if [ ! -x "$BIN_DIR/$cmd" ]; then
        echo "FAIL: $BIN_DIR/$cmd not found or not executable. Run 'make' first."
        exit 1
    fi
done

# Prepare clean workspace in /dev/shm (tmpfs avoids Landlock issues with WSL/drvfs mounts)
rm -rf "$WORK" "/dev/shm/$IMG"
mkdir -p "$WORK/orig" "$WORK/out" "$WORK/bin" "$WORK/packs"

# Copy pack into /dev/shm to ensure native POSIX fs
cp -r "$PACK_DIR" "$WORK/packs/png.codecpack"
chmod +x "$WORK/packs/png.codecpack/bin/png2jxl"

# Put png2jxl on PATH
export PATH="$WORK/packs/png.codecpack/bin:$BIN_DIR:$PATH"
export INVFS_CODECPACKS="$WORK/packs"

# Find sample PNG files
SAMPLE_DIR="/mnt/d/Users/User/invfs_deploy"
SAMPLES=(
    "item1.png"
    "bios_menu.png"
    "menu_box.png"
    "host_desktop.png"
    "vm_crop.png"
)

for s in "${SAMPLES[@]}"; do
    if [ -f "$SAMPLE_DIR/$s" ]; then
        cp "$SAMPLE_DIR/$s" "$WORK/orig/$s"
    fi
done

NUM_SAMPLES=$(ls -1 "$WORK/orig"/*.png 2>/dev/null | wc -l)
if [ "$NUM_SAMPLES" -eq 0 ]; then
    echo "Creating synthetic PNG test sample..."
    python3 -c "
from PIL import Image
im = Image.new('RGBA', (200, 100), (255, 0, 0, 255))
im.save('$WORK/orig/synthetic.png', 'PNG')
"
fi

echo "Staged $(ls -1 "$WORK/orig"/*.png | wc -l) test PNG files."

echo ""
echo "=== 2. CLI Helper Standalone Tests (encode, decode, check, estimate) ==="
TEST_PNG=$(ls -1 "$WORK/orig"/*.png | head -n 1)
TEST_BASE=$(basename "$TEST_PNG")

echo "--- Testing: png2jxl estimate ---"
EST_BYTES=$(png2jxl estimate "$TEST_PNG")
echo "Estimate for $TEST_BASE: $EST_BYTES bytes decode working set"
if [ -z "$EST_BYTES" ] || [ "$EST_BYTES" -le 0 ]; then
    echo "FAIL: png2jxl estimate failed or returned non-positive value: '$EST_BYTES'"
    exit 1
fi

echo "--- Testing: png2jxl encode & decode roundtrip ---"
png2jxl encode "$TEST_PNG" "$WORK/out/$TEST_BASE.jxl" -f
png2jxl decode "$WORK/out/$TEST_BASE.jxl" "$WORK/out/$TEST_BASE.rebuilt.png" -f

ORIG_SHA=$(sha256sum "$TEST_PNG" | awk '{print $1}')
REBUILT_SHA=$(sha256sum "$WORK/out/$TEST_BASE.rebuilt.png" | awk '{print $1}')
echo "Original SHA256: $ORIG_SHA"
echo "Rebuilt  SHA256: $REBUILT_SHA"

if [ "$ORIG_SHA" != "$REBUILT_SHA" ]; then
    echo "FAIL: Standalone roundtrip SHA256 mismatch!"
    exit 1
fi
echo "Standalone CLI roundtrip: 100% bit-exact PASS."

echo "--- Testing: png2jxl check ---"
# Check 1: Reconstructable JXL should exit 0
if png2jxl check "$WORK/out/$TEST_BASE.jxl" >/dev/null; then
    echo "png2jxl check on reconstructable JXL: PASS (exit 0)"
else
    echo "FAIL: png2jxl check failed on valid png2jxl file"
    exit 1
fi

# Check 2: Non-reconstructable standard JXL should exit 1 (or >0)
cjxl "$TEST_PNG" "$WORK/out/plain.jxl" -q 100 --lossless_jpeg=0 2>/dev/null || \
cjxl "$TEST_PNG" "$WORK/out/plain.jxl" -d 0 2>/dev/null
if png2jxl check "$WORK/out/plain.jxl" >/dev/null 2>&1; then
    echo "FAIL: png2jxl check returned 0 on plain non-reconstructable JXL!"
    exit 1
else
    echo "png2jxl check on plain JXL: PASS (rejected non-envelope JXL)"
fi

echo ""
echo "=== 3. InvariantFS End-to-End Integration Test ==="
cd /dev/shm
echo "Creating InvariantFS image: $IMG (64M)..."
"$BIN_DIR/invf-mkfs" "$IMG" 64M >/dev/null

echo "Copying PNG files into InvariantFS volume..."
for f in "$WORK/orig"/*.png; do
    bn=$(basename "$f")
    "$BIN_DIR/invf-cp" "$IMG" "$f" "/$bn"
done

echo "Running invf-sweep (with png.codecpack)..."
SWEEP_LOG="$WORK/sweep.log"
"$BIN_DIR/invf-sweep" "$IMG" 2>&1 | tee "$SWEEP_LOG"

echo "Verifying sweep claimed files via png (codecpack)..."
for f in "$WORK/orig"/*.png; do
    bn=$(basename "$f")
    if grep -q "$bn: png (codecpack)" "$SWEEP_LOG"; then
        echo "  [OK] $bn transcoded by png (codecpack)"
    else
        echo "FAIL: $bn was not transcoded by png (codecpack)"
        exit 1
    fi
done

echo "Running invf-verify --deep..."
"$BIN_DIR/invf-verify" --deep "$IMG"

echo ""
echo "=== 4. Roundtrip Extraction & SHA256 Verification ==="
printf "%-20s %10s %10s %8s %8s\n" "Filename" "Orig (B)" "JXL (B)" "Ratio" "Status"
printf "%-20s %10s %10s %8s %8s\n" "--------------------" "----------" "----------" "-------" "--------"

TOTAL_ORIG=0
TOTAL_JXL=0

for f in "$WORK/orig"/*.png; do
    bn=$(basename "$f")
    EXTRACTED="$WORK/out/$bn.extracted"
    JXL_FILE="$WORK/out/$bn.jxl"
    if [ ! -f "$JXL_FILE" ]; then
        png2jxl encode "$f" "$JXL_FILE" -f >/dev/null 2>&1 || true
    fi
    "$BIN_DIR/invf-cat" "$IMG" "/$bn" > "$EXTRACTED" 2>/dev/null
    
    ORIG_HASH=$(sha256sum "$f" | awk '{print $1}')
    EXTR_HASH=$(sha256sum "$EXTRACTED" | awk '{print $1}')
    
    ORIG_SZ=$(stat -c%s "$f")
    JXL_SZ=$(stat -c%s "$JXL_FILE" 2>/dev/null || stat -c%s "$EXTRACTED")
    
    TOTAL_ORIG=$((TOTAL_ORIG + ORIG_SZ))
    TOTAL_JXL=$((TOTAL_JXL + JXL_SZ))
    
    RATIO=$(python3 -c "print(f'{$JXL_SZ / $ORIG_SZ * 100:.1f}%')")
    
    if [ "$ORIG_HASH" != "$EXTR_HASH" ]; then
        printf "%-20s %10d %10d %8s %8s\n" "$bn" "$ORIG_SZ" "$JXL_SZ" "$RATIO" "MISMATCH"
        echo "FAIL: Bit-exact verification failed for $bn"
        exit 1
    else
        printf "%-20s %10d %10d %8s %8s\n" "$bn" "$ORIG_SZ" "$JXL_SZ" "$RATIO" "EXACT"
    fi
done

OVERALL_RATIO=$(python3 -c "print(f'{$TOTAL_JXL / $TOTAL_ORIG * 100:.1f}%')")
printf "%-20s %10s %10s %8s %8s\n" "--------------------" "----------" "----------" "-------" "--------"
printf "%-20s %10d %10d %8s %8s\n" "TOTAL" "$TOTAL_ORIG" "$TOTAL_JXL" "$OVERALL_RATIO" "ALL PASS"

echo ""
echo "ALL TESTS PASSED: png codecpack is 100% byte-exact and fully integrated into InvariantFS."
