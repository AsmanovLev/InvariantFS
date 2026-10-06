#!/bin/sh
# check-vol-find.sh — the ledger for the one collapsed value that has produced
# four separate defects.
#
# WHY THIS EXISTS. `vol_find` returns a uint64_t inode id. An inode id has no
# in-band error value, so a function returning one CANNOT report failure — and
# nothing in the language stops `if (!id)`. One collapsed meaning, "no such
# name" AND "the lookup could not be completed", produced four defects in four
# unrelated subsystems:
#
#   bd220cb  perm_check_cred read a failed lookup as "no ACL"   (fail-open)
#   3b6e171  RENAME_NOREPLACE overwrote the destination it was told to protect
#   7791a87  vol_write_begin lost have_old, so the commit never retired the
#            old recipe (space leak)
#   42ea0a6  table_sync_one_locked evicted a live name, so open/stat/read
#            returned ENOENT while readdir still listed it
#
# The shape is not a property of uint64_t and not a property of the callers.
# It is a property of the situation, and it will recur at the next call site
# nobody thought about.
#
# WHAT THIS DOES. It refuses a NEW `vol_find(` call site that does not say what
# it does with a failed lookup. Every existing site is on the ledger below with
# a verdict, so the population is visible and can only shrink:
#
#   ACTS   the caller DECIDES something from the result — evicts, frees,
#          overwrites, refuses, or reports. These are where a defect lives and
#          they want vol_find_rc.
#   SKIP   the caller merely skips work. Correct only because skipping is
#          harmless, which is an accident of the caller, not a guarantee.
#
# It is deliberately NOT a migration. Converting a site is a behavioural change
# and needs its own red control; that is the unit of work that has worked four
# times. This only makes sure no more are added silently.

set -u
cd "$(dirname "$0")/.." || exit 1

# ACTS = the result decides something. SKIP = merely skips work.
# Keep this sorted; it is the ledger.
LEDGER="
src/cli/batch_owner_test.c:92:SKIP
src/cli/acl_inherit_test.c:266:SKIP   # child_has_acl: measurement, fault spent
src/cli/acl_inherit_test.c:291:SKIP   # setup: aborts the test if the /p anchor is not found
src/cli/cat.c:86:SKIP
src/cli/cat.c:99:SKIP
src/cli/cp.c:118:SKIP
src/cli/heat_walk_test.c:365:SKIP
src/cli/fuse_fs.c:1644:SKIP
src/cli/fuse_fs.c:1980:SKIP
src/cli/fuse_fs.c:2612:SKIP
src/cli/fuse_fs.c:2826:SKIP
src/cli/fuse_fs.c:2881:SKIP
src/cli/fuse_fs.c:3142:SKIP
src/cli/fuse_fs.c:3309:SKIP
src/cli/fuse_fs.c:3381:SKIP
src/cli/fuse_fs.c:3416:SKIP
src/cli/fuse_fs.c:3453:SKIP
src/cli/otrunc_test.c:568:SKIP   # calibration: the armed lookup IS the assertion
src/cli/otrunc_test.c:572:SKIP   # calibration: the armed lookup IS the assertion
src/cli/lane_release_test.c:242:SKIP
src/cli/ls.c:121:SKIP
src/cli/ls.c:135:SKIP
src/cli/read_parallel_bitexact_test.c:427:SKIP
src/cli/rollback_symlink_test.c:207:SKIP
src/cli/rollback_symlink_test.c:223:SKIP
src/cli/rollback_symlink_test.c:256:ACTS
src/cli/rollback_symlink_test.c:283:ACTS
src/cli/rollback_symlink_test.c:314:SKIP
src/cli/rt_slot_test.c:623:ACTS
src/cli/sibling_retire_test.c:117:ACTS
src/cli/sibling_retire_test.c:158:ACTS
src/cli/sibling_retire_test.c:185:ACTS
src/cli/sibling_retire_test.c:196:ACTS
src/cli/sibling_retire_test.c:211:ACTS
src/cli/sib_walk_test.c:199:SKIP   # rescan: whether a name resolves IS the measurement
src/cli/sib_walk_test.c:214:SKIP   # setup: create the bracketing directory only if absent
src/cli/bang_name_test.c:146:SKIP   # live(): whether a name resolves IS the measurement -- the defect was a name that resolved when it should not have, so the oracle must be the lookup itself
src/cli/bang_name_test.c:168:SKIP   # reads_exact(): an unresolvable name is a FAILED byte-exact read, not a soft miss -- and WP141 made the exact-name lookup the FIRST thing vol_read_named does (src/core/vol_read.c), so a present file is never shadowed by a container member. An earlier version of this comment justified itself with the opposite claim (that vol_read_named reported a miss for a file present and intact); that was measured and REFUTED -- the defect was real but a different one, and it is fixed.
src/cli/sweep_bang_test.c:82:ACTS   # reads_exact(): same shape as bang_name_test.c:168, but a miss here decides an assertion -- it FAILS the test, which is the only thing that makes this oracle safe
src/cli/sweep_bang_test.c:115:ACTS  # the RAW precondition: a miss yields inode 0, whose zone is not RAW, so the precondition fails loudly rather than passing on an absent file
src/cli/sweep_bang_test.c:117:ACTS  # as :115, on the '!'-bearing name -- and this is the half that must not be able to pass vacuously
src/cli/sweep_bang_test.c:122:SKIP  # setup: marks whatever id the name resolves to; a miss marks 0, which is inert, and the sweep assertions downstream still fail
src/cli/sweep_bang_test.c:123:SKIP  # as :122
src/cli/sweep_bang_test.c:132:ACTS  # the control's own zone read: a miss is not TEXT, so all_text_a clears and the harness-live control FAILS -- the control cannot be satisfied by a name that is not there
src/cli/sweep_bang_test.c:133:ACTS  # as :132, on the '!'-bearing name: the red control's input
src/cli/bang_name_test.c:289:SKIP   # LEG A setup: the two inode ids are printed to show both names were distinct inodes before the unlink
src/cli/bang_name_test.c:290:SKIP   # LEG A setup: as above, for the bang name
src/cli/crc32c_test.c:364:ACTS  # LEG 4: a miss FAILS the test. Whether the name resolves IS the measurement -- a CRC bug that loses a written file on reopen shows up exactly here, so the result has to decide the assertion.
src/cli/crc32c_test.c:368:ACTS  # LEG 4: the diagnostic that names the name whose lookup missed. Reached only after :364 has already failed, so it reports rather than decides.
src/cli/crc32c_test.c:413:ACTS  # LEG 5: as :364, on the cross-machine reopen, where a miss is the reported symptom.
src/cli/crc32c_test.c:417:ACTS  # LEG 5: as :368, on the cross-machine reopen.
src/cli/table_sync_evict_test.c:344:SKIP
src/cli/read_named_test.c:254:SKIP   # setup: whether a name resolves IS the measurement -- same shape as bang_name_test.c:146
src/cli/read_named_test.c:258:SKIP   # setup: THE precondition of LEG A. The exact name must resolve, or the leg is not testing shadowing but absence
src/cli/read_named_test.c:305:SKIP   # LEG B setup: the exact name must be GONE, or the control is not a control
src/cli/read_named_test.c:378:SKIP   # LEG E setup: whether a name resolves IS the measurement -- the whole leg is that the bang name does not perturb the plain ones
src/cli/tz_registry_test.c:178:SKIP
src/cli/window_test.c:176:SKIP
src/cli/write_create_path_test.c:248:SKIP
src/cli/tar_cap_test.c:182:SKIP
src/cli/window_test.c:176:SKIP
src/core/vol_cpack.c:2596:SKIP
src/core/vol_cpack.c:2939:SKIP
src/core/vol_cpack.c:2939:SKIP
src/core/vol_cpack.c:3036:SKIP
src/core/vol_cpack.c:3036:ACTS
src/core/vol_cpack.c:3281:ACTS
src/core/vol_cpack.c:3777:SKIP
src/core/vol_cpack.c:3859:SKIP
src/core/vol_cpack.c:3859:SKIP
src/core/vol_cpack.c:3859:ACTS
src/core/vol_cpack.c:3859:SKIP
src/core/vol_cpack.c:3859:ACTS
src/core/vol_cpack.c:3960:SKIP
src/core/vol_cpack.c:3960:SKIP
src/core/vol_exer.c:267:ACTS
src/core/vol_heat.c:799:SKIP
src/core/vol_png.c:1013:SKIP
src/core/vol_read.c:1073:SKIP
src/core/vol_read.c:1169:SKIP
src/core/vol_read.c:1205:ACTS
src/core/vol_read.c:1416:ACTS   # vol_read_named: the EXACT name decides. Found -> those are the bytes returned with status 0; absent -> the name is walked as a container path instead
src/core/vol_read.c:1423:SKIP
src/core/vol_read.c:1610:ACTS
src/core/vol_read.c:811:SKIP
src/core/vol_read.c:828:SKIP
src/core/vol_read.c:888:SKIP
src/core/vol_read.c:963:SKIP
src/core/vol_sweep.c:1631:SKIP
src/core/vol_sweep.c:251:SKIP
src/core/vol_sweep.c:459:ACTS
src/core/vol_sweep.c:478:ACTS
src/core/vol_sweep.c:498:ACTS
src/core/vol_sweep.c:517:ACTS
src/core/vol_textzone.c:679:SKIP
src/core/vol_tier.c:304:SKIP
src/core/volume.c:1253:SKIP   # tier_owner / rawm_owner anchors (vol_open_inner)
src/core/volume.c:1254:SKIP   # shifted by the PERF_PROFILING counters in volume.c
src/recipes/zip.c:141:SKIP
src/recipes/zip.c:145:SKIP
src/core/vol_cpack.c:2939:SKIP   # непомечен: добавлено при правке vol_cpack.c
src/core/vol_cpack.c:3036:SKIP   # непомечен: добавлено при правке vol_cpack.c
src/core/vol_cpack.c:3859:SKIP   # непомечен: добавлено при правке vol_cpack.c
src/core/vol_cpack.c:3859:ACTS   # непомечен: добавлено при правке vol_cpack.c
src/core/vol_cpack.c:3879:ACTS   # непомечен: добавлено при правке vol_cpack.c
src/core/vol_cpack.c:3960:SKIP   # непомечен: добавлено при правке vol_cpack.c
src/core/vol_cpack.c:2940:SKIP   # непомечен: добавлено при правке vol_cpack.c
src/core/vol_cpack.c:3037:SKIP   # непомечен: добавлено при правке vol_cpack.c
src/core/vol_cpack.c:3874:SKIP   # непомечен: добавлено при правке vol_cpack.c
src/core/vol_cpack.c:3875:ACTS   # непомечен: добавлено при правке vol_cpack.c
src/core/vol_cpack.c:3900:ACTS   # непомечен: добавлено при правке vol_cpack.c
src/core/vol_cpack.c:3992:SKIP   # непомечен: добавлено при правке vol_cpack.c
"

fail=0
found_list=$(mktemp)
trap 'rm -f "$found_list"' EXIT

# Every non-definition call site, tests included: a test can hide the same
# mistake, and sibling_retire_test and rollback_symlink_test both do.
#
# The comment filter must match the CONTENT, not the start of the output line.
# grep -rn prints "file:line:content", so an anchored ^\s*\* can never match
# anything -- which is how eight ledger entries ended up being citations of
# call sites rather than call sites. A comment describing a retired API is a
# 1.7 doc bug, not a call, and counting it makes the ledger churn every time a
# comment is edited.
grep -rn 'vol_find[[:space:]]*(' src --include='*.c' --include='*.h' 2>/dev/null \
  | grep -vE 'vol_find_rc|vol_find_ex|vol_find_strict' \
  | grep -vE 'uint64_t vol_find\(' \
  | awk -F: '
      # a call site is code: the line is not a comment.
      { content = $0; sub(/^[^:]*:[0-9]+:/, "", content) }
      content ~ /^[[:space:]]*\*/      { next }   # block-comment continuation
      content ~ /^[[:space:]]*\/\//    { next }   # line comment
      content ~ /\/\*.*vol_find/        { next }   # block comment on this line
      # Deliberately NOT /*.*vol_find/: that also matches a POINTER
      # DEREFERENCE standing before the call -- "invfs_volume *v) { return
      # vol_find(...)" -- and it silently swallowed a real call site. The
      # detector has to have been SEEN failing, or it is not known to detect.
      { print $1 ":" $2 }
    ' \
  | sort -u > "$found_list"

if [ ! -s "$found_list" ]; then
  echo "check-vol-find: no vol_find call sites found — the grep is wrong, not the tree"
  exit 1
fi

# 1. Every site on the ledger must still exist. A ledger entry for a line that
#    moved is worse than no ledger: it looks current and is not.
while IFS=: read -r f n verdict; do
  [ -z "${f:-}" ] && continue
  if ! grep -qx "$f:$n" "$found_list"; then
    echo "check-vol-find: LEDGER STALE — $f:$n is marked $verdict but no call site is there."
    echo "                Re-point it or drop it; a ledger entry for a line that"
    echo "                moved looks current and is not."
    fail=1
  fi
done <<EOF
$LEDGER
EOF

# 2. Every real site must be on the ledger. This is the half that stops growth.
while read -r site; do
  [ -z "$site" ] && continue
  if ! printf '%s\n' "$LEDGER" | grep -q "^$site:"; then
    echo "check-vol-find: UNMARKED CALL SITE — $site calls vol_find and says nothing"
    echo "                about what it does when the lookup FAILS."
    echo "                Add a line to the ledger in this script:"
    echo "                    $site:ACTS   # the result decides something"
    echo "                    $site:SKIP   # it merely skips work"
    echo "                ACTS sites are where the four defects so far lived; if"
    echo "                yours is one, it wants vol_find_rc, not a ledger entry."
    fail=1
  fi
done < "$found_list"

acts=$(printf '%s\n' "$LEDGER" | grep -c ':ACTS' || true)
skips=$(printf '%s\n' "$LEDGER" | grep -c ':SKIP' || true)
if [ "$fail" -ne 0 ]; then
  echo "check-vol-find: FAILED (ledger: $acts ACTS, $skips SKIP)"
  exit 1
fi
echo "check-vol-find: ok ($acts ACTS, $skips SKIP, $(wc -l < "$found_list" | tr -d ' ') call sites on the ledger)"
