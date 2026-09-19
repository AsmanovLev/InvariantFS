#!/bin/bash
# tools/test-bootstrap.sh — non-interactive regression for packaging/bootstrap.sh.
#
# Needs no network and no root: uses a locally built `make release` artifact
# and a DESTDIR staging tree. Covers:
#   1. POSIX syntax (sh -n) of bootstrap.sh + install.sh
#   2. --source --dry-run touches nothing
#   3. make release -> invfs-<ver>-x86_64.tar.zst + SHA256SUMS
#   4. --file <artifact> install into DESTDIR, manifest written
#   5. --list shows the manifest
#   6. --uninstall removes every recorded path
#   7. sha256 mismatch is rejected before unpack
#   8. --dry-run leaves the filesystem untouched
#   9. runtime dependency matrix uses /etc/os-release ID/ID_LIKE
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BOOT="$ROOT/packaging/bootstrap.sh"
INSTALL="$ROOT/packaging/install.sh"

TMP=$(mktemp -d /tmp/invfs-test-bootstrap.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
pass=0 fail=0

ok()   { printf 'ok   - %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf 'FAIL - %s\n' "$1"; fail=$((fail + 1)); }
check() { # <desc> <file> <substr>
    if grep -q -- "$3" "$2" 2>/dev/null; then ok "$1"; else
        bad "$1"; sed 's/^/       /' "$2" 2>/dev/null | tail -20
    fi
}

echo "== 1. syntax =="
if sh -n "$BOOT" 2>"$TMP/err"; then ok "sh -n bootstrap.sh"; else bad "sh -n bootstrap.sh"; cat "$TMP/err"; fi
if sh -n "$INSTALL" 2>"$TMP/err"; then ok "sh -n install.sh"; else bad "sh -n install.sh"; cat "$TMP/err"; fi

echo "== 2. --source --dry-run =="
INVFS_SOURCE_DIR="$ROOT" sh "$BOOT" --source --prefix /usr/local --dry-run \
    >"$TMP/dryrun.out" 2>&1 && ok "source dry-run exits 0" \
    || { bad "source dry-run exit"; cat "$TMP/dryrun.out"; }
check "dry-run mentions source checkout" "$TMP/dryrun.out" "source checkout"
check "dry-run mentions make" "$TMP/dryrun.out" "make"
check "dry-run does not install" "$TMP/dryrun.out" "DRY-RUN"

echo "== 3. make release =="
make -C "$ROOT" release >"$TMP/release.out" 2>&1 \
    && ok "make release" || { bad "make release"; tail -30 "$TMP/release.out"; }
ART=$(ls "$ROOT"/dist/invfs-*-x86_64.tar.zst 2>/dev/null | head -1 || true)
if [ -n "$ART" ] && [ -f "$ART" ]; then ok "artifact: ${ART##*/}"; else bad "no artifact produced"; fi
[ -f "$ROOT/dist/SHA256SUMS" ] && ok "SHA256SUMS present" || bad "SHA256SUMS missing"
check "sha256sums has artifact" "$ROOT/dist/SHA256SUMS" "$(basename "${ART:-x}")"

echo "== 4. install into DESTDIR =="
DEST="$TMP/dest"
DESTDIR="$DEST" sh "$BOOT" --file "$ART" --prefix /usr/local --yes \
    --no-systemd --no-dracut >"$TMP/install.out" 2>&1 \
    && ok "bootstrap --file install exits 0" \
    || { bad "bootstrap --file install"; cat "$TMP/install.out"; }
[ -x "$DEST/usr/local/bin/invf-mkfs" ] && ok "invf-mkfs installed" || bad "invf-mkfs missing"
[ -x "$DEST/usr/local/bin/invf-fuse" ] && ok "invf-fuse installed" || bad "invf-fuse missing"
[ -f "$DEST/usr/local/lib/invfs/installed.manifest" ] && ok "manifest written" || bad "manifest missing"
[ -f "$DEST/usr/local/share/man/man8/invf-mkfs.8" ] && ok "man page installed" || bad "man page missing"
[ ! -d "$DEST/usr/local/lib/systemd" ] && ok "systemd skipped via --no-systemd" || bad "systemd not skipped"
[ ! -d "$DEST/usr/local/lib/dracut" ] && ok "dracut skipped via --no-dracut" || bad "dracut not skipped"
if [ -f "$DEST/usr/local/lib/invfs/installed.manifest" ]; then
    n=$(wc -l < "$DEST/usr/local/lib/invfs/installed.manifest")
    [ "$n" -gt 10 ] && ok "manifest has $n entries" || bad "manifest too small ($n)"
fi

echo "== 5. --list =="
DESTDIR="$DEST" sh "$BOOT" --list --prefix /usr/local >"$TMP/list.out" 2>&1 \
    && ok "--list exits 0" || bad "--list exit"
check "--list shows invf-mkfs" "$TMP/list.out" "/usr/local/bin/invf-mkfs"

echo "== 6. --uninstall =="
DESTDIR="$DEST" sh "$BOOT" --uninstall --prefix /usr/local >"$TMP/uninstall.out" 2>&1 \
    && ok "--uninstall exits 0" || { bad "--uninstall exit"; cat "$TMP/uninstall.out"; }
[ ! -e "$DEST/usr/local/bin/invf-mkfs" ] && ok "invf-mkfs removed" || bad "invf-mkfs survived"
[ ! -e "$DEST/usr/local/lib/invfs/installed.manifest" ] && ok "manifest removed" || bad "manifest survived"
[ ! -e "$DEST/usr/local/lib/invfs" ] && ok "manifest dir pruned" || bad "manifest dir survived"

echo "== 7. sha256 mismatch rejected =="
BAD="$TMP/bad"; mkdir -p "$BAD"
cp "$ART" "$BAD/$(basename "$ART")"
printf '%064d  %s\n' 0 "$(basename "$ART")" > "$BAD/SHA256SUMS"
if DESTDIR="$TMP/badest" sh "$BOOT" --file "$BAD/$(basename "$ART")" \
        --prefix /usr/local --yes >"$TMP/bad.out" 2>&1; then
    bad "corrupt checksum accepted"
else
    ok "corrupt checksum rejected"
fi
check "mismatch message names sha256" "$TMP/bad.out" "sha256 MISMATCH"
[ ! -e "$TMP/badest/usr/local/bin/invf-mkfs" ] && ok "nothing installed on mismatch" \
    || bad "installed despite mismatch"

echo "== 8. --dry-run install no side effects =="
DRY="$TMP/drydest"
DESTDIR="$DRY" sh "$BOOT" --file "$ART" --prefix /usr/local --dry-run \
    >"$TMP/dryinstall.out" 2>&1 && ok "dry-run install exits 0" || bad "dry-run install exit"
[ ! -e "$DRY/usr/local/bin/invf-mkfs" ] && ok "dry-run installed nothing" \
    || bad "dry-run installed files"

echo "== 9. dependency matrix =="
for distro in "debian apt-get" "arch pacman" "gentoo emerge" "void xbps-install" "fedora dnf"; do
    set -- $distro
    id=$1; tool=$2
    printf 'ID=%s\n' "$id" > "$TMP/os-release-$id"
    INVFS_OS_RELEASE="$TMP/os-release-$id" INVFS_SOURCE_DIR="$ROOT" \
        sh "$BOOT" --source --dry-run >"$TMP/os-$id.out" 2>&1 || true
    check "$id -> $tool" "$TMP/os-$id.out" "$tool"
done

echo
echo "bootstrap test: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
