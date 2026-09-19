#!/bin/bash
# test-registry.sh — basic smoke test for invfs-pack (WP60)
#
# Tests: list, info, verify, install/remove round-trip, alternatives, use, where
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$REPO_DIR/bin/invfs-pack"
PACKS_DIR="$REPO_DIR/tools/codecpacks"

if [ ! -x "$BIN" ]; then
    echo "FAIL: $BIN not found; run 'make' first" >&2
    exit 1
fi

# use a temp dir as the "host root" to avoid needing /.invariantfs
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
HOST_ROOT="$TMPDIR/codecpacks"
mkdir -p "$HOST_ROOT"

# redirect invfs-pack's host root search by setting a fake env
# We'll symlink packs there for testing
export INVFS_CODECPACKS="$PACKS_DIR"

PASS=0
FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1" >&2; }

echo "=== invfs-pack test suite ==="

# --- where ---
echo "[where]"
OUT=$("$BIN" where 2>&1)
echo "$OUT" | grep -q "Pack roots" && pass "where prints header" || fail "where header"
echo "$OUT" | grep -q "\$INVFS_CODECPACKS" && pass "where shows env" || fail "where env"

# --- list ---
echo "[list]"
OUT=$("$BIN" list 2>&1)
COUNT=$(echo "$OUT" | grep -c "pack(s)" || true)
echo "$OUT" | grep -q "11 pack(s)" && pass "list found 11 packs" || fail "list: $(echo "$OUT" | tail -1)"

# list with family filter
OUT=$("$BIN" list container 2>&1)
echo "$OUT" | grep -q "pack(s)" && pass "list container family" || fail "list container"

# --- info ---
echo "[info]"
OUT=$("$BIN" info jxl 2>&1)
echo "$OUT" | grep -q "codec_id.*4" && pass "info codec_id" || fail "info codec_id"
echo "$OUT" | grep -q "family.*raw_image" && pass "info family" || fail "info family"
echo "$OUT" | grep -q "priority.*100" && pass "info priority" || fail "info priority"

# --- verify ---
echo "[verify]"
OUT=$("$BIN" verify jxl 2>&1)
echo "$OUT" | grep -q "blake3:" && pass "verify computes blake3" || fail "verify blake3"

# verify all packs
OUT=$("$BIN" verify 2>&1)
COUNT=$(echo "$OUT" | grep -c "blake3:" || true)
[ "$COUNT" -eq 11 ] && pass "verify all 11 packs" || fail "verify all: $COUNT"

# --- alternatives ---
echo "[alternatives]"
OUT=$("$BIN" alternatives raw_image 2>&1)
echo "$OUT" | grep -q "raw_image" && echo "$OUT" | grep -q "jxl" && pass "alternatives raw_image" || fail "alternatives raw_image"

OUT=$("$BIN" alternatives container 2>&1)
COUNT=$(echo "$OUT" | grep -c "priority=" || true)
[ "$COUNT" -ge 8 ] && pass "alternatives container finds packs" || fail "alternatives container: $COUNT"

# --- install/remove round-trip (using temp dir) ---
echo "[install/remove]"
# Create a fake host root and override PACK_CONF
FAKE_HOST="$TMPDIR/fake_host"
mkdir -p "$FAKE_HOST"
# We need to override the host root path. Since invfs-pack hardcodes
# /.invariantfs/codecpacks, we create it via sudo or skip if not root.
if [ "$(id -u)" = "0" ]; then
    mkdir -p /.invariantfs/codecpacks
    "$BIN" install jxl 2>&1
    if [ -d /.invariantfs/codecpacks/jxl.codecpack ]; then
        pass "install jxl"
        # verify installed pack
        OUT=$("$BIN" verify jxl 2>&1)
        echo "$OUT" | grep -q "blake3:" && pass "verify installed jxl" || fail "verify installed jxl"
        "$BIN" remove jxl 2>&1
        if [ ! -d /.invariantfs/codecpacks/jxl.codecpack ]; then
            pass "remove jxl"
        else
            fail "remove jxl (dir still exists)"
        fi
    else
        fail "install jxl (dir not created)"
    fi
else
    echo "  SKIP: install/remove (not root)"
fi

# --- use ---
echo "[use]"
if [ "$(id -u)" = "0" ]; then
    "$BIN" use raw_image jxl 2>&1
    if grep -q "jxl" /.invariantfs/codecpacks/packs.conf 2>/dev/null; then
        pass "use raw_image jxl"
    else
        fail "use raw_image jxl"
    fi
else
    echo "  SKIP: use (not root)"
fi

# --- where ---
echo "[where]"
OUT=$("$BIN" where 2>&1)
echo "$OUT" | grep -q "host, trusted" && pass "where shows trusted" || fail "where trusted"
echo "$OUT" | grep -q "system" && pass "where shows system" || fail "where system"

# --- summary ---
echo ""
echo "=== results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
