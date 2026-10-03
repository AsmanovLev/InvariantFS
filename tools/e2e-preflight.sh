#!/bin/bash
# e2e-preflight.sh -- report every external tool the e2e suites mention,
# BEFORE any suite runs, so a missing dependency costs one second instead of
# one sweep.
#
# WHY. Found by running `make e2e` on a host that is not the author's, which
# died on a missing external tool over and over -- cjxl, then numpy, then
# unzip -- each after several suites had passed and several minutes spent. Four
# sweeps, six capability gaps, every one reported as
#
#     FAIL: cjxl not installed
#
# which reads as a product defect and is not one. tools/test-ivpacks.sh gates
# its OPTIONAL tools behind HAVE_* flags and skips the fixture with a note, so
# the pattern is already known in this repo; it was just being applied one suite
# at a time, and only when a sweep happened to reach it.
#
# WHAT IT IS, precisely: ADVISORY. It reports what it finds and names the apt
# line. It does not decide that anything is required -- that judgement belongs
# to each suite, where the context is known, and duplicating it here is how the
# two drift apart. So it exits 0 unless --strict is given.
#
# An earlier version of this script tried to be clever: it classified each
# `for t in ...` loop as all-required or any-of by reading the loop body, and
# got that wrong three separate ways (a fixture-name list, a PYTHON loop inside
# a heredoc, and an alternatives list where one of four tools was present). It
# then sent the reader to apt for things the host already had. The lesson is
# recorded rather than the cleverness: report, do not judge.
#
# Usage:
#   bash tools/e2e-preflight.sh            # report, always exits 0
#   bash tools/e2e-preflight.sh --quiet    # only the missing ones
#   bash tools/e2e-preflight.sh --strict   # exit 1 if anything is missing
set -u
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
QUIET=0; STRICT=0
for a in "$@"; do
    case "$a" in
        --quiet)  QUIET=1 ;;
        --strict) STRICT=1 ;;
    esac
done

# mkfs.ext4, e2fsck, debugfs, mkfs.xfs, losetup and friends live in /sbin: on
# root PATH and on nobody elses. TESTENV appends /sbin for the suites
# themselves, so this has to as well or it reports as missing what the suites
# can actually find.
export PATH="$PATH:/sbin:/usr/sbin"

tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT

# Emit "tool<TAB>suite" for every name a suite checks. The list is read from the
# suites themselves so it cannot drift from them.
for s in "$REPO"/tools/test-*.sh; do
    [ -f "$s" ] || continue
    b=$(basename "$s")
    sed -nE 's/^[[:space:]]*for[[:space:]]+t[[:space:]]+in[[:space:]]+([^;]+);.*/\1/p' "$s" \
        | tr ' "' '\n\n' | grep -E '^[A-Za-z0-9._-]+$' | while read -r t; do
            echo "$t	$b"
        done
    sed -nE 's/.*command -v[[:space:]]+([A-Za-z0-9._-]+).*/\1/p' "$s" \
        | while read -r t; do echo "$t	$b"; done
done | sort -u > "$tmp"

# Drop this repo OWN outputs: the invf-* CLIs, and the pack/helper programs the
# suites build for themselves (rawdisk, jxlest, mac, cbrm, classof, rngread).
# Those are build products, not host capabilities, and listing them as missing
# would point the reader at apt for something this repository compiles.
while IFS=$'\t' read -r tool suite; do
    case "$tool" in
        *.sh|invf-*) continue ;;
        -*) continue ;;                 # a flag, not a command
        [0-9]*) continue ;;              # a loop counter, not a command
        rawdisk|jxlest|mac|cbrm|classof|rngread|cjxl|djxl|jxl) continue ;;
    esac
    [ -e "$REPO/bin/$tool" ] && continue
    ls "$REPO"/tools/codecpacks/*/bin/"$tool" >/dev/null 2>&1 && continue
    printf '%s\t%s\n' "$tool" "$suite"
done < "$tmp" > "${tmp}.2" && mv "${tmp}.2" "$tmp"

n_have=0; miss=0; : > "${tmp}.miss"
while IFS=$'\t' read -r tool suite; do
    [ -n "$tool" ] || continue
    if command -v "$tool" >/dev/null 2>&1; then
        n_have=$((n_have + 1))
        [ "$QUIET" = 1 ] || printf '  ok      %-16s (%s)\n' "$tool" "$suite"
    else
        miss=$((miss + 1))
        printf '%s\t%s\n' "$tool" "$suite" >> "${tmp}.miss"
        [ "$QUIET" = 1 ] || printf '  MISSING %-16s (%s)\n' "$tool" "$suite"
    fi
done < "$tmp"

total=$((n_have + miss))
if [ "$miss" -eq 0 ]; then
    echo "e2e-preflight: all $total tool name(s) the suites mention resolve"
    exit 0
fi

echo
echo "e2e-preflight: $miss of $total mentioned name(s) do not resolve."
echo "NOT ALL OF THESE ARE NEEDED -- some are alternatives (a suite trying 7zz,"
echo "7za and 7z in turn), some are pack helpers a suite builds for itself, and"
echo "some names are package names rather than command names. Check the suite"
echo "before installing anything. If one IS needed, the usual sources are:"
echo "  cjxl/djxl: libjxl-tools   7z/7za/7zz: p7zip-full   mkfs.*/debugfs/e2fsck:"
echo "  e2fsprogs/xfsprogs/dosfstools/exfatprogs (/sbin)   unzip/zip: unzip/zip"
echo
cut -f1 "${tmp}.miss" | sort -u | tr '\n' ' ' | sed 's/^/  names: /'
echo
[ "$STRICT" = 1 ] && exit 1
exit 0
