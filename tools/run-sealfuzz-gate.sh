#!/usr/bin/env bash
# run-sealfuzz-gate.sh — WP201: the `make test` gate for the seal footer
# and parity-header parse (the untrusted-bytes surface).
#
# Runs bin/sealfuzz (which drives the SHIPPED seal_footer_parse() /
# seal_parhdr_parse()) over a deterministic PRNG sweep, under ASan+UBSan.
# The gate fails on any out-of-bounds read (ASan), any refusal of a valid
# footer (P2), or any rc outside {0,1,-1} (P3). Same args, same run.
set -u
cd "$(dirname "$0")/.."

ITERS=${SEALFUZZ_ITERS:-50000}
SEED=${SEALFUZZ_SEED:-0x51A1F005}

if [ ! -x bin/sealfuzz ]; then
  echo "run-sealfuzz-gate: bin/sealfuzz missing (build it first)" >&2
  exit 1
fi
exec ./bin/sealfuzz "$ITERS" "$SEED"
