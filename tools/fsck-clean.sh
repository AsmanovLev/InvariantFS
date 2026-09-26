# tools/fsck-clean.sh — the "this volume is consistent" gate, one definition.
#
#   . "$REPO/tools/fsck-clean.sh"          # once, after REPO is set
#   fsck_clean "$WORK/fsck.log"            # 0 = clean, 1 = not (never exits)
#
# WHY THIS IS NOT JUST `grep orphans: 0`
#
# The v2 report's `orphans:` counter is "blocks marked allocated in the bitmap
# that no live record references". Meta-v3 has no L2P mapper, so the v3 checker
# cannot compute that number at all -- and it deliberately does not fake one.
# What v3 DOES verify, and prints, is:
#   - pages walked / base keys: the whole base tree was read
#   - bad pages / torn slots:   nothing in it is unreadable or ambiguous
#   - cycles/shared:            the tree is not cyclic or over-shared
#   - reachable-but-free pages: allocator cross-check (one direction)
#   - ^OK:                      the scan found nothing at all
# The v3 scan checks allocated-vs-reachable in ONE direction only (a reachable
# page must be allocated). The other direction -- an allocated page nothing
# references, i.e. a leak -- is not something the v3 scan looks for, so
# printing a hardcoded `orphans: 0` would assert something unchecked. Assert
# instead what each format actually verifies.
#
# The v2 branch is unchanged, so a v2 volume is still gated exactly as before.

fsck_clean() {
    local log=$1
    # no [ -f ] here on purpose: callers pass a process substitution
    # (`fsck_clean <(invf-fsck ...)`), which is a pipe, not a regular file.
    # A missing/unreadable log simply fails every grep below, i.e. "not clean",
    # and the caller names the path in its own message.
    if grep -q "format:       v3" "$log"; then
        grep -q "bad pages:    0" "$log" &&
        grep -q "cycles/shared: 0" "$log" &&
        grep -q "^OK$" "$log"
    else
        grep -q "orphans:      0" "$log" &&
        grep -q "missing:      0" "$log"
    fi
}

# Convenience for the common "assert and die" site, so a suite does not have
# to repeat the || { echo FAIL; exit 1; } boilerplate at every call.
fsck_require_clean() {
    local log=$1 label=${2:-fsck}
    fsck_clean "$log" && return 0
    echo "FAIL: $label: fsck does not report a clean volume ($log)"
    [ -f "$log" ] && sed -n '1,40p' "$log"
    return 1
}
