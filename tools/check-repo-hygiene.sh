#!/bin/bash
# check-repo-hygiene.sh — fail if tracked files look like build artifacts:
#
#   1. binary blobs under version control (ELF / MZ / Mach-O / ar magic,
#      or artifact extensions .o/.obj/.a/.so/.pyc/.class)
#   2. tracked files larger than 1 MiB
#
# outside the explicit allowlist below. Not wired into any CI yet — run by
# hand from the repo root:  bash tools/check-repo-hygiene.sh
# Exit 0 = clean, 1 = violations found.

set -u
cd "$(git rev-parse --show-toplevel)" || exit 2

MAX_BYTES=$((1024 * 1024))

# Allowlist (gitignore-style prefix match on the tracked path):
#  - bin/busybox-static: vendored static busybox consumed by
#    tools/mkinitramfs.sh; not a build output of this repo.
#    port history (impl_docs/DOCMAP.md row A5); trace.log exceeds 1 MiB.
ALLOWLIST=(
    "bin/busybox-static"
)

allowlisted() {
    local p="$1" a
    for a in "${ALLOWLIST[@]}"; do
        case "$a" in
            */) [ "${p#"$a"}" != "$p" ] && return 0 ;;   # prefix
            *)  [ "$p" = "$a" ] && return 0 ;;           # exact
        esac
    done
    return 1
}

is_binary() {
    local p="$1" magic
    case "$p" in
        *.o|*.obj|*.a|*.so|*.so.*|*.pyc|*.class) return 0 ;;
    esac
    [ -f "$p" ] || return 1
    magic=$(head -c 4 -- "$p" | od -An -tx1 | tr -d ' \n')
    case "$magic" in
        7f454c46)      return 0 ;;  # ELF
        4d5a*)         return 0 ;;  # MZ (PE/COFF/DOS)
        feedface|feedfacf|cefaedfe|cffaedfe) return 0 ;;  # Mach-O
        213c6172)      return 0 ;;  # !<ar  (static archive)
    esac
    return 1
}

viol=0

# mode-160000 entries are gitlinks (tools/busybox-src) — not files, skip.
# `git ls-files -s` line format: "<mode> <object> <stage>\t<path>"
while IFS= read -r -d '' rec; do
    mode=${rec%% *}
    path=${rec#*$'\t'}
    [ "$mode" = "160000" ] && continue
    allowlisted "$path" && continue
    if [ -f "$path" ]; then
        size=$(stat -c%s -- "$path")
        if [ "$size" -gt "$MAX_BYTES" ]; then
            printf 'LARGE  %s (%d bytes > %d)\n' "$path" "$size" "$MAX_BYTES"
            viol=1
        fi
    fi
    if is_binary "$path"; then
        printf 'BINARY %s\n' "$path"
        viol=1
    fi
done < <(git ls-files -s -z)

if [ "$viol" -eq 0 ]; then
    echo "repo hygiene: OK ($(git ls-files | wc -l) tracked files, no binaries/oversize outside allowlist)"
else
    echo "repo hygiene: FAIL — untrack or allowlist the files above" >&2
fi
exit "$viol"
