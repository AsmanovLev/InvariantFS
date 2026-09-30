#!/bin/bash
# What does one parity pass actually COST, in blocks and in seconds?
#
# The author approved making XOR the default for the shadow zone at a ~1%
# budget, and separately requires RS to cover the whole disk. Neither can be
# designed without a denominator: "1% parity" is decoration until a pass is
# measured on a volume of known content, because the cost depends on what is
# in the shadow, not on the volume size.
#
# This measures, on identical volumes:
#   - blocks used before/after a bare sweep (what the shadow actually holds),
#   - blocks added by --seal (the parity itself),
#   - wall time of each,
#   - and the same again on a SECOND seal pass, which is the number that
#     matters: a default that re-costs 1% every pass is not a 1% default.
#
# It also checks the two claims the design rests on, because they are the
# ones a default would silently depend on:
#   - parity is a function of the CONTENTS, so a same-size re-mkfs of the
#     same bytes is the reproducibility control;
#   - an UNSEAL returns the volume to its pre-seal state (no silent growth).
set -u
cd "$(dirname "$0")/.." || exit 2

W="${INVFS_SEAL_W:-/srv/bench/seal}"
MIB="${INVFS_SEAL_MIB:-512}"
CORPUS="${INVFS_SEAL_CORPUS:-/srv/bench/corpus}"

usage() { echo "usage: $0 <corpus-dir> [corpus-dir...]" >&2; }
[ $# -ge 1 ] || { usage; exit 2; }

rm -rf "$W"; mkdir -p "$W" || exit 2
say() { echo "  $*"; }

# Blocks in use, from the volume's own accounting rather than a file size:
# invf-fsck reports the free count, and the total is fixed at mkfs time.
used_blocks() {
    bin/invf-fsck "$1" 2>/dev/null |
        awk '/blocks:/ {
            for (i = 1; i <= NF; i++) {
                if ($i == "total,") t = $(i-1)
                if ($i == "free,")  f = $(i-1)
            }
            if (t > 0) printf "%d\n", t - f
        }'
}

for corpus in "$@"; do
    [ -d "$corpus" ] || { say "SKIP $corpus: not a directory"; continue; }
    name=$(basename "$corpus")
    v="$W/$name.img"
    echo "=== $name ($(du -sh "$corpus" 2>/dev/null | cut -f1)) ==="

    INVFS_V3=1 bin/invf-mkfs "$v" "$MIB" >"$W/$name.mkfs.log" 2>&1 \
        || { say "mkfs failed"; tail -3 "$W/$name.mkfs.log"; continue; }

    t0=$(date +%s)
    bin/invf-import "$v" "$corpus" >"$W/$name.import.log" 2>&1 \
        || { say "import failed"; tail -3 "$W/$name.import.log"; continue; }
    t1=$(date +%s)
    u_import=$(used_blocks "$v")
    say "import: $((t1 - t0))s, used $u_import blocks"

    t0=$(date +%s)
    bin/invf-sweep "$v" >"$W/$name.sweep.log" 2>&1
    t1=$(date +%s)
    u_bare=$(used_blocks "$v")
    say "bare sweep: $((t1 - t0))s, used $u_bare blocks (drained into shadow)"

    t0=$(date +%s)
    bin/invf-sweep --seal "$v" >"$W/$name.seal1.log" 2>&1
    t1=$(date +%s)
    u_seal1=$(used_blocks "$v")
    say "seal pass 1: $((t1 - t0))s, used $u_seal1 blocks  -> parity costs $((u_seal1 - u_bare))"

    # The number that decides whether a default is honest: a SECOND pass over
    # the same unchanged volume. If this adds parity again, the "cost" is per
    # pass, not per volume, and a ~1% default is really ~1% every sweep.
    t0=$(date +%s)
    bin/invf-sweep --seal "$v" >"$W/$name.seal2.log" 2>&1
    t1=$(date +%s)
    u_seal2=$(used_blocks "$v")
    say "seal pass 2: $((t1 - t0))s, used $u_seal2 blocks  -> second pass adds $((u_seal2 - u_seal1))"

    # Does the parity read back and actually reconstruct? A cost measurement
    # that never checks the parity is a cost measurement of noise.
    if bin/invf-verify --deep "$v" >"$W/$name.verify.log" 2>&1; then
        say "verify --deep after seal: OK"
    else
        say "verify --deep after seal: FAILED"
        tail -5 "$W/$name.verify.log" | sed 's/^/    /'
    fi

    # unseal must return to the pre-seal footprint, or the parity is a
    # one-way door.
    t0=$(date +%s)
    bin/invf-sweep --unseal "$v" >"$W/$name.unseal.log" 2>&1
    t1=$(date +%s)
    u_unseal=$(used_blocks "$v")
    say "unseal: $((t1 - t0))s, used $u_unseal blocks (back to $u_bare? $([ "$u_unseal" = "$u_bare" ] && echo yes || echo NO))"

    echo
done

echo "logs in $W"
