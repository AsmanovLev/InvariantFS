#!/bin/bash
# test-debian-cmdline.sh — the kernel cmdline tools/boot-debian-qemu.sh boots.
#
# WHY THIS EXISTS. The Debian boot gate is the one leg in this project that
# boots a real distro's systemd on a FUSE root, and it costs a mkfs + import
# (~850 s of fsync-bound work) plus a 3-16 min QEMU boot per run. Every
# parameter it boots with -- the masks that keep a TCG runner from losing the
# multi-user race, and the systemd-shutdown debug trio -- used to be assembled
# only inside the boot path, so the only way to check them was to pay for the
# boot. WP629 hoisted the assembly above every preflight check and added
# INVFS_DEB_PRINT_CMDLINE, which makes it a pure function of the environment.
# This gate is what keeps it that way: it runs in about a second, needs no
# root, no QEMU, no kernel, no initramfs, no volume, and it FAILS if the
# assembly creeps back behind the preflight.
#
# WHAT IT ASSERTS (each row is a claim, not a snapshot):
#   1. exact default cmdline -- byte equality, so an accidental reorder or a
#      dropped mask cannot pass as "still contains systemd.mask="
#   2. INVFS_DEB_BOOT_MASK= means NO mask (the `${VAR-default}` vs `${VAR:-default}`
#      distinction: an explicitly empty value must not fall back to the list)
#   3. a custom mask REPLACES the default list (not appends to it)
#   4. INVFS_DEB_SHUTDOWN_DEBUG=1 appends all three tokens, AFTER the masks,
#      in order -- `ignore_loglevel` alone is not enough and `log_level=debug`
#      alone is invisible, so a partial trio is a defect, not a variant
#   5. the off spellings are off, and they do not print a "debug ON" note
#   6. print mode keeps stdout clean: exactly one line, no `note:` diagnostics
#      (stderr carries those), so the line can be fed back into `-append`
#   7. HERMETIC: with REPO pointing at an empty directory, no INVFS_DEB_STAGE,
#      no INVFS_DEB_VOLUME and a bogus initramfs, print mode still exits 0 and
#      creates nothing -- i.e. it does not need `make` first
#   8. NEGATIVE CONTROL: without print mode, that same bogus environment fails
#      with exit 2 and empty stdout. 7 alone would also pass if the preflight
#      checks had simply been deleted; 7+8 together say the print path runs
#      BEFORE the preflight rather than instead of it.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
SCRIPT="$REPO/tools/boot-debian-qemu.sh"
[ -f "$SCRIPT" ] || { echo "FAIL: no $SCRIPT"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
ERRF="$TMP/stderr"
EMPTY="$TMP/empty-repo"
mkdir -p "$EMPTY"

FAILS=0
ok()   { echo "  OK    $*"; }
bad()  { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1"; echo "        want: $3"; echo "        got:  $2"; fi; }

PREFIX="console=ttyS0,115200 invfs.init=/lib/systemd/systemd"
DEF_MASKS="systemd-journal-catalog-update.service apt-daily.timer apt-daily-upgrade.timer dpkg-db-backup.timer fstrim.timer systemd-tmpfiles-clean.timer"
DEBUG_TRIO="systemd.log_level=debug ignore_loglevel systemd.log_target=kmsg"

# Baseline: strip every INVFS_DEB_* knob this gate is about, so an operator's
# exported environment cannot silently change what is being asserted.
# $3 (optional) is the value for INVFS_DEB_PRINT_CMDLINE; the rest arrive as
# extra KEY=VALUE words.
OUT=""; RC=0
run() {
    local pcm="$1"; shift
    OUT=""; RC=0
    OUT=$(env -u INVFS_DEB_STAGE -u INVFS_DEB_VOLUME -u INVFS_DEB_BOOT_MASK \
             -u INVFS_DEB_SHUTDOWN_DEBUG -u INVFS_DEB_PRINT_CMDLINE \
             INVFS_DEB_PRINT_CMDLINE="$pcm" "$@" bash "$SCRIPT" 2>"$ERRF") || RC=$?
}
count_tokens() { printf '%s\n' "$1" | tr ' ' '\n' | grep -c "^$2" || true; }
has_token()    { printf '%s\n' "$1" | tr ' ' '\n' | grep -qx -- "$2"; }

want_default="$PREFIX"
for u in $DEF_MASKS; do want_default="$want_default systemd.mask=$u"; done

echo "== default cmdline is exactly the documented one"
run 1
check "exit 0"                        "$RC" 0
check "byte-exact cmdline"            "$OUT" "$want_default"
check "six systemd.mask= tokens"      "$(count_tokens "$OUT" 'systemd\.mask=')" 6
check "no shutdown-debug trio"        "$(has_token "$OUT" ignore_loglevel && echo yes || echo no)" no

echo "== INVFS_DEB_BOOT_MASK= means no mask at all"
run 1 INVFS_DEB_BOOT_MASK=
check "exit 0"                        "$RC" 0
check "cmdline is the bare prefix"    "$OUT" "$PREFIX"
check "no systemd.mask= token"        "$(count_tokens "$OUT" 'systemd\.mask=')" 0
check "no masking note"               "$(grep -c 'masking boot-noise' "$ERRF" || true)" 0

echo "== a custom mask REPLACES the default list"
run 1 INVFS_DEB_BOOT_MASK=foo.service
check "exit 0"                        "$RC" 0
check "exactly one mask"              "$(count_tokens "$OUT" 'systemd\.mask=')" 1
check "it is the custom one"          "$OUT" "$PREFIX systemd.mask=foo.service"
check "apt-daily.timer is gone"       "$(has_token "$OUT" systemd.mask=apt-daily.timer && echo yes || echo no)" no

echo "== INVFS_DEB_SHUTDOWN_DEBUG=1 appends the full trio, after the masks"
run 1 INVFS_DEB_SHUTDOWN_DEBUG=1 INVFS_DEB_BOOT_MASK=bar.service
check "exit 0"                        "$RC" 0
check "trio follows the mask"         "$OUT" "$PREFIX systemd.mask=bar.service $DEBUG_TRIO"
run 1 INVFS_DEB_SHUTDOWN_DEBUG=1
check "mask list then trio (default)" "$OUT" "$want_default $DEBUG_TRIO"
case "$OUT" in
    *"systemd.log_level=debug ignore_loglevel systemd.log_target=kmsg") ok "trio is the exact tail, in order" ;;
    *) bad "trio is not the exact tail of: $OUT" ;;
esac

echo "== every off spelling is off"
for v in 0 no false off ""; do
    run 1 INVFS_DEB_SHUTDOWN_DEBUG="$v"
    check "debug='$v' -> no trio"     "$(has_token "$OUT" ignore_loglevel && echo yes || echo no)" no
    check "debug='$v' -> no ON note"  "$(grep -c 'shutdown debug ON' "$ERRF" || true)" 0
done

echo "== print mode keeps stdout clean (one line, no diagnostics)"
run 1 INVFS_DEB_SHUTDOWN_DEBUG=1
check "stdout is one line"            "$(printf '%s\n' "$OUT" | wc -l | tr -d ' ')" 1
check "stdout has no 'note:'"         "$(printf '%s' "$OUT" | grep -c 'note:' || true)" 0
check "stderr carries the ON note"    "$(grep -c 'shutdown debug ON' "$ERRF" || true)" 1

echo "== HERMETIC: prints with no build, no stage, no initramfs, no volume"
run 1 REPO="$EMPTY" INVFS_DEB_INITRD="$EMPTY/nope.cpio.gz" \
      INVFS_DEB_WORK="$EMPTY/work" INVFS_DEB_LOGDIR="$EMPTY/logs"
check "exit 0"                        "$RC" 0
check "same default cmdline"          "$OUT" "$want_default"
check "created nothing on disk"       "$(ls -A "$EMPTY" | wc -l | tr -d ' ')" 0

echo "== NEGATIVE CONTROL: without print mode the preflight still refuses"
run 0 REPO="$EMPTY" INVFS_DEB_INITRD="$EMPTY/nope.cpio.gz" \
      INVFS_DEB_WORK="$EMPTY/work" INVFS_DEB_LOGDIR="$EMPTY/logs"
check "exit 2 (preflight, not a boot attempt)" "$RC" 2
check "stdout is empty"               "$OUT" ""
check "stderr names the missing root" "$(grep -c 'INVFS_DEB_STAGE\|run make first\|no initramfs' "$ERRF" || true)" 1

echo
if [ "$FAILS" -eq 0 ]; then echo "PASS: debian boot cmdline assembly (9 assertion groups)"; exit 0; fi
echo "FAIL: $FAILS check(s) failed"; exit 1
