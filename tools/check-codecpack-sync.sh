#!/bin/bash
# check-codecpack-sync.sh — fail when tools/codecpacks/<name>.codecpack/ and
# the registry's codecpacks/<name>/<version>/ have drifted apart in a way that
# changes what the ENGINE does.
#
# WHY (WP112). The two repos developed in parallel with nothing cross-checking
# them. The registry's qcow2 manifest shipped `map = bin/qcow2 map {in} {out}`
# and no `decomp_gen` key; the engine e2e never read the registry's copy, so
# the bug passed every suite and was found by hand. The registry's index.json
# carries no `generation` for any of its 15 packs, so nothing cross-checked
# that either. This is the check that makes the next one loud.
#
# It runs in three layers (per-pack exceptions are recorded, with reasons, in
# tools/codecpacks/registry-sync):
#
#   A. record coverage   every vendored pack has a line in the record and
#                        every line names a pack that exists. No registry
#                        needed, so this layer always runs.
#   B. manifest parity   the keys parse_manifest() actually READS must carry
#                        the same value in both copies, modulo the per-pack
#                        allowlist and two normalisations (argv0 spelling, an
#                        absent `type`). Comments, key order, and the keys
#                        the engine ignores are NOT drift -- they are what
#                        made the byte-diff unreadable.
#   C. source relation   `mirror` packs must be byte-identical; `fork` packs
#                        must contain every registry CODE line (comments
#                        excluded -- the engine copy is a superset, because
#                        the ADR-007 ivpack plugin glue is engine-only).
#
# The parsed-key list below is asserted against the code: every key must still
# be a strcmp in src/codecs/codec.c, so the list cannot rot into silence.
#
# Usage:  bash tools/check-codecpack-sync.sh
# Env:    INVFS_CODECPACK_REGISTRY  registry checkout (default: /tmp/invfs-registry,
#                                    then ../invfs-registry; `none` disables
#                                    discovery). Absent = layers B and C print
#                                    a loud SKIP on stderr and the
#                                    script still passes; set
#                                    INVFS_CODECPACK_STRICT=1 to make that a
#                                    failure (that is what `make
#                                    check-codecpacks` does).
# Exit 0 = in sync, 1 = drift, 2 = this script or the record is broken.

set -u
cd "$(git rev-parse --show-toplevel)" || exit 2

RECORD=tools/codecpacks/registry-sync
PACKROOT=tools/codecpacks
CODEC=src/codecs/codec.c

fail=0
note() { echo "  $*"; }
bad()  { echo "  FAIL: $*"; fail=1; }

[ -f "$RECORD" ] || { echo "check-codecpack-sync: no $RECORD"; exit 2; }
[ -f "$CODEC" ]  || { echo "check-codecpack-sync: no $CODEC"; exit 2; }

# ---- the manifest keys parse_manifest() reads ----------------------------
# src/codecs/codec.c:429 (parse_manifest) and its key chain below it.
ENGINE_KEYS="name algo pack_version caps dec_mem generation requires
             decomp_gen type encode decode estimate
             enumerate extract strip rebuild map
             sniff.offset sniff.magic sniff.ext"

# Self-check: a key the engine no longer parses would silently stop being
# compared, which is the exact rot this script exists to prevent.
for k in $ENGINE_KEYS; do
    if ! grep -q "\"$k\"" "$CODEC"; then
        echo "check-codecpack-sync: FAIL: key '$k' is no longer parsed by $CODEC"
        echo "  (parse_manifest moved or dropped it -- update ENGINE_KEYS)"
        exit 2
    fi
done

# ---- manifest key extraction + normalisation ------------------------------
# Emits "key<TAB>value", comments and blank lines dropped, argv0 spelling and
# an absent `type` normalised.
mkv() {
    awk '
        BEGIN { t = "codec" }
        { line = $0; sub(/\r$/, "", line) }
        line ~ /^[ \t]*#/          { next }
        line !~ /^[ \t]*[A-Za-z_][A-Za-z0-9_.]*[ \t]*=/ { next }
        {
            i = index(line, "=")
            k = substr(line, 1, i - 1); v = substr(line, i + 1)
            gsub(/^[ \t]+|[ \t]+$/, "", k)
            gsub(/^[ \t]+|[ \t]+$/, "", v)
            # argv0 spelling: `bin/rawdisk x` and `rawdisk x` are the same
            # command -- both resolve to <pack>/bin/rawdisk (codec.c:571-590
            # at probe, vol_cpack.c:322-352 at exec).
            if (v ~ /^(bin\/)?[A-Za-z0-9_.-]+[ \t]/) {
                n = split(v, a, /[ \t]+/); sub(/^bin\//, "", a[1])
                v = a[1]
                for (j = 2; j <= n; j++) v = v " " a[j]
            }
            # `type` defaults to codec when absent (codec.c:481 sets
            # is_container only for "container"), so jxl and raw_image, which
            # omit it, are not drift.
            if (k == "type") t = (v == "container") ? "container" : "codec"
            else print k "\t" v
        }
        END { print "type\t" t }
    ' "$1"
}

# ---- layer A2: manifest/source generation agreement -----------------------
# The pack's own contract: the generation it stamps into the map header is a
# constant in the pack's source, and the manifest must agree with it (the
# registry's qcow2 manifest states this at its lines 24-26). Bumping one
# without the other is exactly the drift class that costs a silent missed
# re-decomposition -- and unlike layers B and C, this half needs no registry
# checkout at all.
check_generation() {
    local p="$1" macro="$2" pdir="$3" mgen cval
    [ "$macro" = "-" ] && return 0
    mgen=$(awk -F= '/^[[:space:]]*generation[[:space:]]*=/ { gsub(/[[:space:]]/, "", $2); print $2; exit }' \
           "$pdir/manifest")
    cval=$(sed -n "s/^[[:space:]]*#define[[:space:]]\+$macro[[:space:]]\{1,\}\([0-9][0-9]*\)u\{0,1\}.*/\1/p" \
           "$pdir"/*.c "$pdir"/*.py 2>/dev/null | head -1)
    if [ -z "$cval" ]; then
        bad "$p: the record names gen_macro=$macro, but $macro is not defined in the pack source"
    elif [ "$mgen" != "$cval" ]; then
        bad "$p: the manifest says 'generation = $mgen' but $macro is $cval in the source -- bump both together"
    else
        note "$p: manifest 'generation = $cval' agrees with $macro in the pack source"
    fi
}

# ---- layer A: record coverage + generation agreement (no registry needed) --
echo "== layer A: record coverage and generation agreement (no registry needed)"
vend=""
for d in "$PACKROOT"/*.codecpack; do
    [ -d "$d" ] || continue
    vend="$vend $(basename "$d" .codecpack)"
done
for p in $vend; do
    grep -qE "^$p[ 	]" "$RECORD" || \
        bad "$p is vendored but has no line in $RECORD"
    genm=$(awk -v p="$p" '!/^[ \t]*#/ && $1 == p { print $4 }' "$RECORD")
    [ -n "$genm" ] || genm="-"
    check_generation "$p" "$genm" "$PACKROOT/$p.codecpack"
done
for p in $(awk '!/^[ \t]*#/ && NF { print $1 }' "$RECORD"); do
    case " $vend " in
        *" $p "*) ;;
        *) bad "$RECORD names '$p', which is not vendored under $PACKROOT" ;;
    esac
done
[ "$fail" = 0 ] && note "all $(echo $vend | wc -w) vendored packs are in the record"

# ---- registry discovery ---------------------------------------------------
# INVFS_CODECPACK_REGISTRY=none disables discovery outright, which is how the
# "no registry checkout" path is exercised on a host that happens to have one.
reg=""
case "${INVFS_CODECPACK_REGISTRY:-}" in
    none|off) ;;
    *) for c in "${INVFS_CODECPACK_REGISTRY:-}" /tmp/invfs-registry "$PWD/../invfs-registry"; do
           [ -n "$c" ] && [ -d "$c/codecpacks" ] && { reg="$c"; break; }
       done ;;
esac
if [ -z "$reg" ]; then
    if [ "${INVFS_CODECPACK_STRICT:-0}" = 1 ]; then
        echo "check-codecpack-sync: FAIL: no registry checkout (looked at" \
             "\$INVFS_CODECPACK_REGISTRY, /tmp/invfs-registry, ../invfs-registry)"
        exit 1
    fi
    {
        echo "check-codecpack-sync: SKIPPED layers B and C -- no registry checkout"
        echo "  (looked at \$INVFS_CODECPACK_REGISTRY, /tmp/invfs-registry, ../invfs-registry)"
        echo "  layer A ran and passed. Set INVFS_CODECPACK_STRICT=1 to make this a failure."
    } >&2
    exit "$fail"
fi
echo "== layers B and C against $reg"

tmpd=$(mktemp -d) || exit 2
trap 'rm -rf "$tmpd"' EXIT

# ---- layer B: manifest parity on the keys the engine reads ----------------
# Compares the two key sets in awk; $1 = allowlist (comma list, `-` = none).
cmp_manifest() {
    local p="$1" vman="$2" rman="$3" allow="$4" key out
    for key in $ENGINE_KEYS; do
        out=$(awk -F'\t' -v k="$key" -v allow="$allow" -v pk="$p" '
            FILENAME == ARGV[1] { v[$1] = $2; next }
                             { r[$1] = $2 }
            END {
                split(allow, a, ",")
                for (i in a) if (a[i] == k) excused = 1
                if (excused) exit 0
                vv = (k in v) ? v[k] : "<absent>"
                rv = (k in r) ? r[k] : "<absent>"
                if (vv != rv)
                    printf "  FAIL: %s: manifest key %s differs -- engine=\047%s\047 registry=\047%s\047\n", pk, k, vv, rv
            }' "$vman" "$rman")
        [ -n "$out" ] && { echo "$out"; fail=1; }
    done
}

# registry-side source lines that are CODE (no comments, no blanks).
code_lines() {
    awk '{ line = $0; sub(/\r$/, "", line) }
         line ~ /^[ \t]*$/ { next }
         line ~ /^[ \t]*[*\/]/ { next }
         { print line }' "$1"
}

for p in $vend; do
    rdir=$(ls -d "$reg/codecpacks/$p"/*/ 2>/dev/null | sort -V | tail -1)
    if [ -z "$rdir" ]; then note "$p: not in the registry (nothing to compare)"; continue; fi
    rdir=${rdir%/}
    rver=$(basename "$rdir")
    vend_dir="$PACKROOT/$p.codecpack"

    mode=$(awk -v p="$p" '!/^[ \t]*#/ && $1 == p { print $2 }' "$RECORD")
    allow=$(awk -v p="$p" '!/^[ \t]*#/ && $1 == p { print $3 }' "$RECORD")
    case "$mode" in
        mirror|fork|independent) ;;
        -|"") continue ;;   # layer A already said so; do not pile on
        *) bad "$p: mode '$mode' in $RECORD is not mirror|fork|independent"; continue ;;
    esac
    [ -n "$allow" ] || allow="-"

    # ---- layer B ------------------------------------------------------
    mkv "$vend_dir/manifest" > "$tmpd/v.$p"
    mkv "$rdir/manifest"     > "$tmpd/r.$p"
    before=$fail
    cmp_manifest "$p" "$tmpd/v.$p" "$tmpd/r.$p" "$allow"
    if [ "$before" = "$fail" ]; then
        note "$p ($rver): every engine-parsed manifest key matches$([ "$allow" != "-" ] && printf ' (excepted: %s)' "$allow")"
    fi

    # ---- layer C ------------------------------------------------------
    rsrc=$(ls "$rdir"/*.[cC] "$rdir"/*.py 2>/dev/null | head -1)
    if [ -z "$rsrc" ]; then note "$p ($rver): registry ships no source; layer C skipped"; continue; fi
    rbase=$(basename "$rsrc")
    vsrc="$vend_dir/$rbase"
    if [ ! -f "$vsrc" ]; then
        bad "$p: the registry ships $rbase, the vendored copy has none"; continue
    fi
    if [ "$mode" = independent ]; then
        # Neither copy contains the other: they are two implementations of the
        # same job, so "is the vendored copy a superset" is not a meaningful
        # question and answering it either way would be theatre. The record
        # carries the reason; this prints it on EVERY run so the state is
        # visible in `make test` output rather than buried in a file, and
        # layer A2 above is what still has teeth for this pack.
        note "$p/$rbase: INDEPENDENT fork of the registry copy -- not compared;"
        note "        reconciled by hand (see the qcow2 block in $RECORD)"
        continue
    fi
    if [ "$mode" = mirror ]; then
        if cmp -s "$rsrc" "$vsrc"; then
            note "$p/$rbase: byte-identical to the registry (mirror)"
        else
            bad "$p/$rbase: mode=mirror but the vendored copy is not byte-identical"
            diff -u "$rsrc" "$vsrc" | sed -n '3,12p' | sed 's/^/      /'
        fi
    else
        code_lines "$rsrc" > "$tmpd/rc.$p"
        missing=$(awk 'NR == FNR { a[$0] = 1; next } !($0 in a)' "$vsrc" "$tmpd/rc.$p")
        if [ -z "$missing" ]; then
            note "$p/$rbase: vendored copy holds every registry code line (fork superset)"
        else
            bad "$p/$rbase: the registry has code the vendored copy lacks -- re-sync or reconcile:"
            echo "$missing" | head -5 | sed "s|^|      $rdir/$rbase: |"
        fi
    fi
done

# registry packs this repo does not vendor: informational, never a failure.
extra=""
for d in "$reg"/codecpacks/*/; do
    p=$(basename "$d")
    case " $vend " in *" $p "*) ;; *) extra="$extra $p" ;; esac
done
[ -n "$extra" ] && note "registry packs this repo does not vendor (informational):$extra"

if [ "$fail" != 0 ]; then
    echo "check-codecpack-sync: FAIL -- vendored codecpacks have drifted from $reg"
    exit 1
fi
echo "check-codecpack-sync: OK -- vendored codecpacks agree with $reg"
exit 0
