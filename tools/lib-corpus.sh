#!/bin/bash
# lib-corpus.sh -- where a suite takes its C sources from.
#
#   . "$REPO/tools/lib-corpus.sh"
#   root=$(invfs_c_corpus_root) || { note "no C sources"; skip; }
#   big=$(invfs_c_biggest 100000 "$root")
#
# WHY THIS EXISTS. Six gate suites wanted a C-source corpus, and five of them
# asked for it by hardcoding a path into tools/busybox-src -- a git SUBMODULE.
# `actions/checkout@v7` does not recurse submodules, so on CI and on every
# fresh clone that directory is EMPTY, and the suites died on their own
# fixtures:
#
#   * three of them walked it and built an empty archive
#     ("tar: Cowardly refusing to create an empty archive")
#   * two asserted on a >100 KB busybox .c ("no big busybox .c found")
#   * one demanded six named busybox paths and failed on the first missing one
#
# So none of them could pass anywhere but the one machine whose checkout path
# is hardcoded -- including this project's own CI.
#
# The submodule is now a PREFERENCE, not a requirement: the tree's own src/ is
# the fallback, and it is the same kind of input (hundreds of .c files, plenty
# of them large). If neither yields a source, a suite says so and skips
# rather than building a fixture out of nothing and reporting the failure as
# if the product were at fault.
#
# POSIX sh, no side effects beyond defining functions. Callers must have REPO.

# Echo a directory that contains .c files, preferring the submodule.
invfs_c_corpus_root() {
    local d
    for d in "$REPO/tools/busybox-src" "$REPO/src"; do
        [ -d "$d" ] || continue
        find "$d" -name '*.c' -print -quit 2>/dev/null | grep -q . || continue
        printf '%s\n' "$d"
        return 0
    done
    return 1
}

# Echo the largest .c file of at least $1 bytes under $2, or nothing.
# Sorted by size so the choice does not depend on directory order -- which is
# what made "pick a busybox path" brittle in the first place.
invfs_c_biggest() {
    find "$2" -type f -name '*.c' -size "+${1}c" -printf '%s\t%p\n' 2>/dev/null \
        | sort -rn | head -1 | cut -f2-
}

# Echo the N largest .c files of at least $1 bytes under $2, largest first.
invfs_c_biggest_n() {
    find "$2" -type f -name '*.c' -size "+${1}c" -printf '%s\t%p\n' 2>/dev/null \
        | sort -rn | head -"${3:-1}" | cut -f2-
}
