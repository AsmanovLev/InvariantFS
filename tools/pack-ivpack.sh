#!/usr/bin/env bash
# tools/pack-ivpack.sh — Package an InvariantFS pack into an .ivpack archive
# (ADR-007 §3: an uncompressed ZIP-0, so the central directory is O(1) readable
# and every payload can be mmap'd straight out of the archive with no decode
# and no temporary extraction).
#
#   bash tools/pack-ivpack.sh <pack_dir> <output.ivpack>
#   bash tools/pack-ivpack.sh tools/codecpacks/qcow2.codecpack dist/ivpack/qcow2.ivpack
#
# Layout produced:
#   <name>.ivpack (zip -0)
#   ├── manifest          # the pack's own manifest, verbatim
#   ├── sha256            # `sha256sum -c`-able list of every other entry
#   ├── lib/<name>.so     # the plugin (ivpack C ABI); required
#   └── bin/<name>        # the CLI helper, the fallback path; optional but
#                         # built by tools/test-ivpacks.sh and the Makefile
#
# `make ivpacks` builds every containerpack .so first and drops the archives in
# dist/ivpack/. Nothing here is committed: .gitignore covers *.so, *.ivpack and
# dist/, so an .ivpack is always a reproducible build artifact of the tree it
# was packed from.
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <pack_dir> <output.ivpack>" >&2
    echo "Example: $0 tools/codecpacks/qcow2.codecpack dist/ivpack/qcow2.ivpack" >&2
    exit 1
fi

PACK_DIR="${1%/}"
OUT_IVPACK="$2"

# absolute, because the zip step runs from a temporary content dir
case "$OUT_IVPACK" in
    /*) ;;
    *)  OUT_IVPACK="$PWD/$OUT_IVPACK" ;;
esac

if [ ! -d "$PACK_DIR" ]; then
    echo "Error: pack dir $PACK_DIR not found" >&2
    exit 1
fi
if [ ! -f "$PACK_DIR/manifest" ]; then
    echo "Error: $PACK_DIR has no manifest" >&2
    exit 1
fi
for t in zip sha256sum; do
    command -v "$t" >/dev/null || { echo "Error: $t not installed" >&2; exit 1; }
done

# The pack name is the manifest's, not the directory's (the directory carries
# the .codecpack suffix).
NAME="$(sed -n 's/^name[[:space:]]*=[[:space:]]*//p' "$PACK_DIR/manifest" | head -1)"
[ -n "$NAME" ] || { echo "Error: manifest has no name = line" >&2; exit 1; }

SO="$PACK_DIR/lib$NAME.so"
if [ ! -f "$SO" ]; then
    echo "Error: $SO not found - build it first (\`make plugin-so\`)" >&2
    exit 1
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ivpack_build.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT
CONTENT="$WORK_DIR/content"
mkdir -p "$CONTENT/lib" "$CONTENT/bin"

cp "$PACK_DIR/manifest" "$CONTENT/manifest"
cp "$SO" "$CONTENT/lib/lib$NAME.so"
chmod 0755 "$CONTENT/lib/lib$NAME.so"

# Optional companions: the CLI fallback and any pack data files the manifest
# references (a helper script, a vendored binary). Directories we never ship:
# the pack's own bin/ build output is copied explicitly below, and nothing else
# in a pack dir is loadable.
if [ -f "$PACK_DIR/bin/$NAME" ]; then
    cp "$PACK_DIR/bin/$NAME" "$CONTENT/bin/$NAME"
    chmod 0755 "$CONTENT/bin/$NAME"
fi
for extra in "$PACK_DIR"/*.py; do
    [ -e "$extra" ] || continue
    cp "$extra" "$CONTENT/bin/$(basename "$extra")"
done

# sha256 covers every entry except itself, with paths relative to the archive
# root so `sha256sum -c sha256` works in an unpacked tree.
( cd "$CONTENT" && find . -type f ! -name sha256 -print0 \
    | sort -z | sed -z 's|^\./||' \
    | xargs -0 sha256sum > sha256 )

rm -f "$OUT_IVPACK"
mkdir -p "$(dirname "$OUT_IVPACK")"
# -X: no extra file attributes, so the same tree packs to the same bytes.
( cd "$CONTENT" && zip -q -0 -X -r "$OUT_IVPACK" . )

echo "Created ivpack: $OUT_IVPACK ($(stat -c%s "$OUT_IVPACK") bytes, pack '$NAME')"
