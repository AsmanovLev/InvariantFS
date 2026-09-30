#!/bin/bash
# check-repo-hygiene.sh — fail if tracked files look like build artifacts:
#
#   1. binary blobs under version control (ELF / MZ / Mach-O / ar magic,
#      or artifact extensions .o/.obj/.a/.so/.pyc/.class)
#   2. tracked files larger than 1 MiB
#   3. tracked documentation citing a repo path that does not exist
#      (the src/doc/ rot class — see AGENTS.md 1.7)
#
# outside the explicit allowlist below. Not wired into any CI yet — run by
# hand from the repo root:  bash tools/check-repo-hygiene.sh
# Exit 0 = clean, 1 = violations found.

set -u
cd "$(git rev-parse --show-toplevel)" || exit 2

MAX_BYTES=$((1024 * 1024))
viol=0
tmpd=$(mktemp -d) || exit 2
trap 'rm -rf "$tmpd"' EXIT

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

# 3. doc rot: markdown that cites a repo path which no longer exists.
#    Scoped to every tracked markdown, with the exceptions noted at the file
#    list below. Historical trackers (INCIDENTS.md, CHANGELOG.md) ARE checked:
#    a deleted path there is not rot per se, but a pointer that cannot be
#    followed is rot -- the fix is to reword the pointer (name the commit, or
#    say the file was deleted in N), not to delete the incident.
#    Backticked `path/like/this` tokens and bare md links are checked; this is
#    what stops a deleted subsystem from leaving behind a doc that reads like
#    a live contract.
while IFS= read -r -d '' md; do
    # every repo-ish path mentioned in the doc, backticked or in an md link.
    # The char class includes ':' so a `file.c:LINE` / `dir/file.sh:12-30`
    # citation is captured whole and the :NNN strip below can drop the
    # anchor. Without ':' the token never matches (the class stops at the
    # dot, and the closing backtick is not there), so every file:line
    # citation silently escaped the check.
    grep -oE '`[A-Za-z0-9_./:-]+(/[A-Za-z0-9_.*:-]+)*`' "$md" 2>/dev/null \
    | tr -d '`' \
    | grep -E '/' \
    | grep -vE '^(/|https?|ftp)://|^/|^[a-z]+:[0-9]|/$|^(bin|src/zstd|src/lz4|src/zlib|var|fs|portage|archival|scripts/kconfig)/' \
    | sort -u \
    | while IFS= read -r ref; do
        # strip a trailing :NNN or :NNN-MM line anchor
        ref=${ref%%:*}
        case "$ref" in
            *'*'*|*.md.bak) continue ;;    # globs, backups: not literal
        esac
        # build outputs: a doc may legitimately name an artifact that is
        # produced by a build, not tracked in git. Skipping the artifact
        # EXTENSIONS covers the class without a per-file allowlist that
        # rots the moment someone builds a new one.
        case "$ref" in
            *.gz|*.bz2|*.xz|*.zst|*.img|*.cpio|*.o|*.a|*.so|*.pyc|*.ko|*.iso)
                continue ;;
        esac
        # final segment must look like a file (has an extension); this skips
        # identifier chains such as `vol_get/set/remove_xattr`, which are not
        # paths. A missing extensionless dir is a known blind spot.
        case "${ref##*/}" in *.*) ;; *) continue ;; esac
        [ -e "$ref" ] && continue
        printf 'DOCROT %s cites missing %s\n' "$md" "$ref"
        echo "$md" >> "$tmpd/rot"
    done
#    Scan EVERY tracked markdown, not just docs/ and impl_docs/ top level:
#    Benchmark.md, CHANGELOG.md, INCIDENTS.md and README-RU.md sit at the
#    repo root and were previously never checked -- nor, because the old
#    `[^/]+\.md$` tail also excluded subdirs, were docs/adr/*, docs/guides/*,
#    docs/architecture/* or impl_docs/tasks/*.
#    Excluded, because their paths are relative to their own root (or to a
#    different repo) and every hit in them is a false positive by
#    construction:
#      src/{zstd,lz4,zlib,busybox-src}*/  vendored third-party trees
#      tools/busybox-src/                 submodule (gitlink; never scanned)
#      docs/benchmarks/corpus.md          cites megapolos-installer/.../x.qcow2,
#                                         a path in the *corpus* repo, not here
#    docs/benchmarks/QCOW2-COMPRESSION-BENCHMARK.md is NOT excluded and is
#    clean; the exclusion is per-file precisely so that stays true.
done < <(git ls-files -z '*.md' \
    | grep -zvE '^(src/(zstd|lz4|zlib|busybox-src)[^/]*/|tools/busybox-src/|docs/benchmarks/corpus\.md$)')
if [ -s "$tmpd/rot" ]; then
    viol=1
fi

# 5. RETIRED-SURFACE TRIPWIRE. The mapping journal, the name index and
#    the record stream were deleted (0c82a7a, 16d9ffc). This is NOT the
#    proof that they are gone -- no_v2_surface_test is, because it looks
#    at the reserved gap on a real v3 volume after a real write cycle and
#    finds it byte-for-byte zero, which any re-grown caller breaks
#    whatever it is named. This is the narrower, complementary thing a
#    disk scan cannot see: a retired NAME coming back into a header, a
#    struct field, or a comment, with no behaviour change yet. The format
#    gets re-grown that way -- a change that "just adds a field" -- and
#    by the time the behaviour follows, the deletion is invisible again.
#
#    Deliberately a NAME list, not a pattern: these are the identifiers
#    the deleted surface was called, and renaming one to get past this
#    check is itself the signal a reviewer should catch.
#
#    Allowlist: none in src/, tools/, packaging/. impl_docs/ and
#    INCIDENTS.md are HISTORY -- they are where a retired name is
#    supposed to appear, and that is what makes them worth reading.
if [ "$viol" -eq 0 ]; then
    hits=$(python3 tools/check-retired-surface.py) || viol=1
    if [ -n "$hits" ]; then
        echo "retired-surface tripwire: a deleted identifier is back in the CODE." >&2
        echo "If this is a real re-introduction, that is a format decision and" >&2
        echo "needs a WP, not a lint waiver. See AGENTS.md 1.7." >&2
        echo "$hits" | head -40 >&2
        viol=1
    fi
fi

# 4. citation drift: AGENTS.md 1.7 also requires a cited LINE NUMBER to
#    still point at something. Section 3 above strips the :NNN anchor and
#    checks only the path, so a citation whose file still exists but whose
#    line has drifted past the end of the file passes unnoticed. Citations
#    rot silently as files grow.
if [ "$viol" -eq 0 ]; then
    python3 tools/check-citations.py || viol=1
fi

if [ "$viol" -eq 0 ]; then
    echo "repo hygiene: OK ($(git ls-files | wc -l) tracked files, no binaries/oversize outside allowlist)"
else
    echo "repo hygiene: FAIL — untrack or allowlist the files above" >&2
fi
exit "$viol"
