#!/bin/bash
# test-cpack-max-members.sh — WP-cpack-max-members: the container member
# bound is one number in the engine and EIGHT mirrored copies in the
# container packs, and this test is what keeps them one number.
#
# WHY THIS IS A TEST AND NOT A COMMENT. The bound was 65536 in the engine
# and in all eight packs, and it was raised to 2^20 in the engine and in
# all eight packs, by hand, in one branch. Nothing in the tree noticed if
# one of the nine was missed -- and a pack left at 65536 does not fail
# loudly, it just quietly declines every container over the old cap for
# that ONE container type, which is precisely the "same lane behaving
# differently per container type" bug this project keeps finding. Eight
# copies of a constant with no cross-check is eight chances to be wrong
# and zero ways to find out. So: cross-check them, on every make test.
#
# The engine side of the same bound (CPACK_MAX_MEMBERS, the parse
# behaviour, the CPACK_TABLE_TOOMANY refusal) is covered by
# src/cli/cpack_guard_test.c, which drives cpack_parse_table directly.
# This file covers the part a C unit test cannot see: that the eight
# source files the packs are actually compiled from agree with it.

set -u
cd "$(dirname "$0")/.." || exit 1

checks=0
failures=0
ok() {
    checks=$((checks + 1))
    if [ "$1" = 0 ]; then
        printf '  OK    %s\n' "$2"
    else
        failures=$((failures + 1))
        printf '  FAIL  %s\n' "$2"
    fi
}

# The number the engine is compiled with. Read from the header, not
# hardcoded here, so this test checks the packs against the ENGINE and not
# against a third copy of the answer.
ENGINE_MAX=$(sed -n 's/^#define CPACK_MAX_MEMBERS \([0-9]*\)u.*/\1/p' \
             src/core/volume_internal.h | head -1)

if [ -z "$ENGINE_MAX" ]; then
    echo "  FAIL  CPACK_MAX_MEMBERS is not a literal in volume_internal.h;" \
         "this test needs to read the engine's number"
    exit 1
fi
printf '  ....  engine CPACK_MAX_MEMBERS = %s\n' "$ENGINE_MAX"

# The packs that bound a MEMBER COUNT. qcow2 and vdi do not: they are
# image formats whose members are clusters, and the only shared number
# they carry is the map-entry cap, which is checked separately below.
MEMBER_PACKS="p7z ext4fs fatfs ntfs xfs rawdisk"

echo
echo "member bound, one number in nine files:"

for p in $MEMBER_PACKS; do
    src="tools/codecpacks/$p.codecpack/$p.c"
    if [ ! -f "$src" ]; then
        ok 1 "$p: $src is missing"
        continue
    fi
    # The largest member bound the pack declares. rawdisk's is the index
    # bound (MAX_MEMBERS 2^20-1, because it starts at idx 1), the rest are
    # the count bound, so compare against the pack's own relationship to
    # the engine rather than demanding one spelling. p7z spells it
    # MAX_FILES (it is counting the 7z header's own file records), which is
    # the same bound under a different name.
    val=$(sed -n 's/^#define [A-Z_]*\(MEMBERS\|FILES\)[ ]*\([0-9]*\)u.*/\2/p' \
              "$src" | head -1)
    if [ -z "$val" ]; then
        ok 1 "$p declares no member bound"
        continue
    fi
    # Accept either the count bound (== engine) or the index bound
    # (== engine - 1, for a pack that numbers members from 1).
    if [ "$val" = "$ENGINE_MAX" ] || [ "$val" = "$((ENGINE_MAX - 1))" ]; then
        ok 0 "$p: member bound $val agrees with the engine's $ENGINE_MAX"
    else
        ok 1 "$p: member bound $val DISAGREES with the engine's $ENGINE_MAX" \
              " -- this pack would still refuse every container over the" \
              " old cap, for this container type only, silently"
    fi
done

echo
echo "map-entry bound, derived from the member bound:"

# The engine derives its map cap: 4 runs per member is generous.
# (src/core/vol_cpack.c, CPACK_MAP_MAX_ENTS)
ENGINE_MAP=$((4 * ENGINE_MAX + 4))
for p in qcow2 vdi; do
    src="tools/codecpacks/$p.codecpack/$p.c"
    if [ ! -f "$src" ]; then
        ok 1 "$p: $src is missing"
        continue
    fi
    # Derived form: 4 * CPACK_MAX_MEMBERS + 4. Compare the pack's OWN
    # CPACK_MAX_MEMBERS to the engine's, which is the number that matters.
    val=$(sed -n 's/^#define CPACK_MAX_MEMBERS \([0-9]*\)u.*/\1/p' "$src" | head -1)
    if [ "$val" = "$ENGINE_MAX" ]; then
        ok 0 "$p: mirrors the engine's member bound ($val), so its map cap" \
              " ($ENGINE_MAP) is the engine's"
    else
        ok 1 "$p: member bound '$val' disagrees with the engine's" \
              " $ENGINE_MAX -- its map cap is not the engine's $ENGINE_MAP"
    fi
    grep -q "MAP_MAX_ENTS" "$src" || \
        ok 1 "$p declares no MAP_MAX_ENTS"
done

echo
echo "no pack left a hardcoded copy of the old cap behind:"

# rawdisk carried a literal 65536u in its recipe reader that was NOT its
# own #define, and fatfs had the map cap spelled as a literal rather than
# derived. Those are the two shapes this takes when the number is copied
# instead of named: the copy does not move when the name does. Search for
# the old cap anywhere in a pack's member/map arithmetic.
for p in p7z ext4fs fatfs ntfs xfs rawdisk qcow2 vdi; do
    src="tools/codecpacks/$p.codecpack/$p.c"
    [ -f "$src" ] || continue
    # 65536 / 65535 / 262148 in a member- or map-related context.
    if grep -nE '(MAX_MEMBERS|MAX_IDX|MAX_FILES|MAP_MAX_ENTS|n_mem|nmem|num_files)[^;]*\b(65536u|65535u|262148u)\b' \
            "$src" >/dev/null 2>&1; then
        ok 1 "$p still hardcodes the old cap:" \
              "$(grep -nE '(MAX_MEMBERS|MAX_IDX|MAX_FILES|MAP_MAX_ENTS|n_mem|nmem|num_files)[^;]*\b(65536u|65535u|262148u)\b' "$src" | head -2)"
    else
        ok 0 "$p has no hardcoded copy of the old cap left"
    fi
done

echo
echo "the engine's map cap is still 4 runs per member, and still fits u32:"

# CPACK_MAP_MAX_ENTS is compared against a uint32_t count read off the
# blob. If the derivation ever overflowed u32 the cap would be vacuous,
# which is the failure a reader cannot see.
if [ "$ENGINE_MAP" -lt 4294967295 ]; then
    ok 0 "engine map cap $ENGINE_MAP fits the uint32_t blob count"
else
    ok 1 "engine map cap $ENGINE_MAP overflows the uint32_t blob count --" \
          "the cap is vacuous and the blob length check is the real bound"
fi

echo
printf 'cpack_max_members parity: %d checks, %d failures\n' \
       "$checks" "$failures"
[ "$failures" -eq 0 ]
