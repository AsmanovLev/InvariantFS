#!/bin/bash
# test-void-install.sh — WP64: Void Linux rootfs install onto InvariantFS.
#
# Stages 2-3 only (NO QEMU boot): download/cache a Void ROOTFS tarball,
# unpack it on an ORDINARY filesystem (never extract through FUSE), provision
# it with tools/configure-void.sh, format a single-device volume and a
# two-device (INVFS_DEV1) volume, import offline with invf-import, then
# verify: object counts, fsck clean, provisioned symlinks, and bit-exact
# reads of regular files.
#
# Run from the repo root after `make`:
#   INVFS_E2E_AGENT=wp64-void bash tools/run-e2e.sh tools/test-void-install.sh
#
# Env overrides:
#   INVFS_VOID_WORK     work dir    (default /var/tmp/invfs-wp64-test)
#   INVFS_VOID_TARBALL  cached rootfs tar.xz (else download)
#   INVFS_VOID_URL      base URL    (default repo-default.voidlinux.org/live/current)
#   INVFS_VOID_ROOTFS   file name   (default void-x86_64-ROOTFS-20250202.tar.xz)
#   INVFS_VOID_KEEP=1   keep the work dir instead of cleaning up
#
# NOTE (engine caveat, see docs/VOID-INSTALL.md "Known issue: fsck -f"):
# `invf-fsck -f` produces a volume that is clean for offline reads but whose
# runtime FUSE directory lookups break at boot.  This suite therefore runs
# fsck -f ONLY to assert the on-disk clean state; the boot path re-imports a
# fresh volume and does not fsck -f first.
set -euo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B="$REPO/bin"
WORK="${INVFS_VOID_WORK:-/var/tmp/invfs-wp64-test}"
URL="${INVFS_VOID_URL:-https://repo-default.voidlinux.org/live/current}"
ROOTFS="${INVFS_VOID_ROOTFS:-void-x86_64-ROOTFS-20250202.tar.xz}"
TARBALL="${INVFS_VOID_TARBALL:-$WORK/$ROOTFS}"
STAGE="$WORK/stage"

fail() { echo "FAIL: $*"; exit 1; }
note() { echo "== $* =="; }

for t in invf-mkfs invf-import invf-ls invf-fsck invf-cat; do
    [ -x "$B/$t" ] || fail "missing $B/$t (run make first)"
done
command -v tar >/dev/null || fail "tar not found"

mkdir -p "$WORK"
rm -rf "$STAGE"; mkdir -p "$STAGE"

note "stage 1: Void rootfs tarball"
if [ ! -s "$TARBALL" ]; then
    echo "downloading $URL/$ROOTFS"
    if command -v curl >/dev/null; then
        curl -fL --retry 3 -o "$TARBALL.part" "$URL/$ROOTFS" \
            || fail "download failed (offline?)"
    elif command -v wget >/dev/null; then
        wget -O "$TARBALL.part" "$URL/$ROOTFS" || fail "download failed (offline?)"
    else
        fail "no curl/wget and no cached tarball at $TARBALL"
    fi
    mv "$TARBALL.part" "$TARBALL"
fi
echo "tarball: $TARBALL ($(stat -c %s "$TARBALL") bytes)"

note "stage 2: extract on an ordinary fs (not FUSE)"
tar xJpf "$TARBALL" -C "$STAGE" 2>/dev/null || tar xJf "$TARBALL" -C "$STAGE"
for f in sbin/init etc/runit/1 etc/runit/2 etc/sv/sshd etc/sv/dhcpcd; do
    [ -e "$STAGE/$f" ] || fail "Void tree missing $f"
done
# The tracked /sbin is a symlink into usr/ (usr-merge); sbin/init resolves
# to usr/bin/init -> runit-init.  Accept the real file too.
[ -e "$STAGE/usr/bin/init" ] || [ -e "$STAGE/sbin/init" ] \
    || fail "no runit init under /sbin or /usr/bin"

note "stage 3: provision (runit services, autologin, sshd)"
"$REPO/tools/configure-void.sh" "$STAGE" || fail "configure-void.sh"
[ -L "$STAGE/etc/runit/runsvdir/default/sshd" ] || fail "sshd not enabled"
[ -L "$STAGE/etc/runit/runsvdir/default/dhcpcd" ] || fail "dhcpcd not enabled"
[ -L "$STAGE/etc/runit/runsvdir/default/agetty-ttyS0" ] || fail "ttyS0 getty not enabled"
grep -q -- '-a root' "$STAGE/etc/sv/agetty-serial/conf" || fail "ttyS0 not autologin"
grep -q 'FUSE root' "$STAGE/etc/runit/core-services/03-filesystems.sh" \
    || fail "FUSE-root core-service guard missing"
grep -q '^PermitRootLogin yes' "$STAGE/etc/ssh/sshd_config.d/99-invfs.conf" \
    || fail "sshd PermitRootLogin not set"

EXPECT=$(find "$STAGE" -mindepth 1 | wc -l)
echo "staged objects: $EXPECT"

# Regular files used for the bit-exact comparison.  Never pick a symlink:
# invf-cat emits the link target as an empty extraction.
BITEXACT="usr/lib/os-release usr/bin/dash usr/bin/runit usr/bin/ip \
usr/bin/mkdir etc/ssh/sshd_config etc/runit/core-services/03-filesystems.sh \
etc/fstab etc/hostname etc/shadow usr/lib/libc.so.6"

check_volume() { # <label> <img> [extra env already exported]
    local label="$1" img="$2"
    local objs lsout="$WORK/ls-$label.txt"
    # Capture once: `invf-ls | grep -q` under pipefail reports the producer's
    # SIGPIPE as a pipeline failure even when grep matched.
    "$B/invf-ls" "$img" > "$lsout" 2>/dev/null \
        || fail "$label: invf-ls failed"
    objs=$(tail -1 "$lsout" | awk '{print $1}')
    [ -n "$objs" ] || fail "$label: invf-ls produced no count"
    [ "$objs" = "$EXPECT" ] || fail "$label: invf-ls $objs != staged $EXPECT"
    echo "$label: invf-ls reports $objs objects (== staged)"
    # provisioned symlinks are visible in the volume tree
    grep -q 'runit/runsvdir/default/sshd' "$lsout" \
        || fail "$label: sshd service symlink missing from volume"
    grep -q 'usr/bin/init' "$lsout" \
        || fail "$label: usr/bin/init (runit-init) missing from volume"
    # bit-exact reads
    local f out ok=1
    for f in $BITEXACT; do
        [ -f "$STAGE/$f" ] || continue
        out="$WORK/out-$(echo "$f" | tr / _)"
        if ! "$B/invf-cat" "$img" "$f" "$out" >/dev/null 2>&1; then
            echo "  cat failed: $f"; ok=0; continue
        fi
        if cmp -s "$STAGE/$f" "$out"; then
            echo "  OK $f"
        else
            echo "  MISMATCH $f"; ok=0
        fi
    done
    [ "$ok" = 1 ] || fail "$label: bit-exact reads"
    # fsck: repair then assert clean (offline; see the caveat at the top).
    # A repairing fsck exits non-zero (3 here, like e2fsck's "corrected"),
    # so the verdict line is the assertion, not the exit code.
    "$B/invf-fsck" -f "$img" > "$WORK/fsck-$label-f.log" 2>&1 || true
    grep -qE '^(REPAIRED|OK)$' "$WORK/fsck-$label-f.log" \
        || { cat "$WORK/fsck-$label-f.log"; fail "$label: fsck -f no verdict"; }
    "$B/invf-fsck" "$img" > "$WORK/fsck-$label.log" 2>&1 \
        || { cat "$WORK/fsck-$label.log"; fail "$label: fsck report"; }
    grep -q '^OK$' "$WORK/fsck-$label.log" \
        || { cat "$WORK/fsck-$label.log"; fail "$label: fsck not clean"; }
    echo "$label: fsck -f + report -> OK"
}

note "stage 4: single-device volume"
SINGLE="$WORK/root-single.img"
rm -f "$SINGLE"
INVFS_META_FRAC=16 "$B/invf-mkfs" "$SINGLE" 15 \
    | tee "$WORK/mkfs-single.log"
grep -q 'devices:.*2' "$WORK/mkfs-single.log" \
    && fail "single-device mkfs unexpectedly reported 2 devices" || true
"$B/invf-import" "$SINGLE" "$STAGE" 2>&1 | tee "$WORK/import-single.log"
grep -q '0 skipped' "$WORK/import-single.log" || fail "single: import skipped files"
check_volume single "$SINGLE"

note "stage 5: two-device volume (INVFS_DEV1)"
MD_RAW="$WORK/root-multi.img"
MD_SH="$WORK/shadow.img"
rm -f "$MD_RAW" "$MD_SH"
INVFS_META_FRAC=16 "$B/invf-mkfs" "$MD_RAW" 15 "$MD_SH" 20 \
    | tee "$WORK/mkfs-multi.log"
grep -q 'devices:.*2' "$WORK/mkfs-multi.log" \
    || fail "two-device mkfs did not report 2 devices"
# metadata mirror: the whole span up to metadata_end is byte-identical
METAHI=$(sed -n 's/.*metadata zone: *blocks [0-9]* \.\. \([0-9]*\).*/\1/p' \
         "$WORK/mkfs-multi.log")
[ -n "$METAHI" ] || fail "could not parse metadata geometry"
export INVFS_DEV1="$MD_SH"
"$B/invf-import" "$MD_RAW" "$STAGE" 2>&1 | tee "$WORK/import-multi.log"
grep -q '0 skipped' "$WORK/import-multi.log" || fail "multi: import skipped files"
check_volume multi "$MD_RAW"
cmp <(head -c $(( (METAHI + 1) * 4096 )) "$MD_RAW") \
    <(head -c $(( (METAHI + 1) * 4096 )) "$MD_SH") \
    || fail "metadata mirror differs between dev0 and dev1"
echo "multi: metadata span ($((METAHI + 1)) blocks) byte-identical on both devices"

if [ "${INVFS_VOID_KEEP:-0}" = 1 ]; then
    echo "kept work dir: $WORK (stage + single/multi images)"
else
    rm -rf "$STAGE" "$WORK"/out-* "$WORK"/*.log \
        "$SINGLE" "$MD_RAW" "$MD_SH" 2>/dev/null || true
fi

echo
echo "PASS: Void rootfs staged, provisioned, imported (single + two-device), fsck clean, bit-exact"
