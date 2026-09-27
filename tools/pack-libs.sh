#!/bin/bash
# pack-libs.sh — print the link libraries a codecpack declares in its own
# manifest, as a single space-separated token list.
#
#   usage: bash tools/pack-libs.sh tools/codecpacks/qcow2.codecpack
#
# A pack declares what it needs to link as `libs = -lz` in its manifest; this
# is the ONE place in this repo that reads it, and both the .so build
# (Makefile's PLUGIN_SO_RULE) and the helper build (HELPER_RULE) go through
# it, as do the e2e harnesses that compile bin/<p> themselves
# (`make print-libs-<pack>`).
#
# The token contract is the registry's, deliberately
# (registry tools/build-helpers.sh:47): only flag-shaped tokens are passed on,
# and anything else is REPORTED on stderr and dropped. A manifest is data
# that a pack author writes; a build that splices its words onto a command
# line unfiltered is a build that can be talked into running something else.
#
# Exit 0 and print nothing when the pack declares no `libs` -- the seven
# packs that link libc only.

set -u
dir="${1:-}"
if [ -z "$dir" ] || [ ! -f "$dir/manifest" ]; then
    echo "pack-libs: no manifest at '$dir'" >&2
    exit 2
fi

v=$(sed -n 's/^[[:space:]]*libs[[:space:]]*=[[:space:]]*//p' "$dir/manifest" |
    grep -v '^[[:space:]]*#' | head -n 1)

out=""
for t in $v; do
    case "$t" in
        -l*|-pthread|-lm|-lstdc++|-lgcc|-lc) out="$out $t" ;;
        *) echo "pack-libs: ignoring non-link token '$t' in $dir/manifest" >&2 ;;
    esac
done
[ -n "$out" ] && echo "${out# }"
exit 0
