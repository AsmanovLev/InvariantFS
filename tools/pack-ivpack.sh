#!/usr/bin/env bash
# tools/pack-ivpack.sh — Package an InvariantFS plugin into an .ivpack archive (ADR-007).
#
# An .ivpack archive is a standard uncompressed ZIP (Store / -0) containing:
#   - manifest (metadata, capabilities, entrypoints)
#   - lib<name>.so (shared library exporting ivpack C ABI)
#   - bin/<name> (fallback CLI binary)
#   - SHA256SUMS (sha256 of all contents)
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <pack_dir> <output.ivpack>"
    echo "Example: $0 tools/codecpacks/qcow2.codecpack /tmp/qcow2.ivpack"
    exit 1
fi

PACK_DIR="$1"
OUT_IVPACK="$2"

if [ ! -d "$PACK_DIR" ]; then
    echo "Error: pack dir $PACK_DIR not found" >&2
    exit 1
fi

WORK_DIR=$(mktemp -d /tmp/ivpack_build.XXXXXX)
trap 'rm -rf "$WORK_DIR"' EXIT

# Copy manifest and assets
mkdir -p "$WORK_DIR/content"
cp -r "$PACK_DIR"/* "$WORK_DIR/content/"

cd "$WORK_DIR/content"

# Ensure SHA256SUMS is computed
rm -f SHA256SUMS
find . -type f | sort | xargs sha256sum > SHA256SUMS

# Create uncompressed zip-0
rm -f "$OUT_IVPACK"
zip -q -0 -r "$OUT_IVPACK" .

echo "Created ivpack: $OUT_IVPACK ($(stat -c%s "$OUT_IVPACK") bytes)"
