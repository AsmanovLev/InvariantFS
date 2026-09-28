#!/usr/bin/env bash
# run-gzhdr-gate.sh — WP129: the `make test` gate for the gzip header parse.
#
# Regenerates the seed corpus (it is generated, not checked in — see
# tools/mk-gzhdr-seeds.py for why) and runs the harness over it plus a PRNG
# sweep, under ASan+UBSan.
#
# The harness is bin/gzhdrfuzz, which #includes src/core/vol_cpack.c and
# therefore runs the SHIPPED gz_header_len(). The gate fails on:
#   * any read outside the input buffer (ASan),
#   * any P2 disagreement — a well-formed member the pre-WP129 walk
#     accepted, with a different hlen, that the bounded walk now refuses.
#     This is the direction that hurts in production: a bounds check that
#     quietly starts rejecting valid gzip costs compression silently.
#   * any P3 disagreement — the engine accepting a header the parser
#     refused.
set -u
cd "$(dirname "$0")/.."

SEEDS=tools/fuzz/seeds/gzhdr
ITERS=${GZHDR_ITERS:-200000}
SEED=${GZHDR_SEED:-0x9E3779B97F4A7C15}

if [ ! -x bin/gzhdrfuzz ]; then
  echo "run-gzhdr-gate: bin/gzhdrfuzz missing (build it first)" >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "run-gzhdr-gate: python3 required to build the seed corpus" >&2
  exit 1
fi

python3 tools/mk-gzhdr-seeds.py "$SEEDS" >/dev/null || exit 1

# shellcheck disable=SC2046
# detect_leaks=0 matches the Makefile rule. It was originally here because
# vol_open's v->meta_type_bitmap leaked on every open; WP133 fixed that, so
# the reason is gone. It stays 0 only because re-verifying this gate at its
# full iteration count under LSan is WP129's lane, not this WP's.
ASAN_OPTIONS=hard_rss_limit_mb=4096:detect_leaks=0 \
  bin/gzhdrfuzz "$ITERS" "$SEED" $(ls -1 "$SEEDS"/*) || exit 1

echo "gzhdr gate: PASS ($ITERS PRNG cases + $(ls -1 "$SEEDS"/* | wc -l) seeds)"
