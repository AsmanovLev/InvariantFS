#!/bin/bash
# test-fedora-install.sh -- Fedora container-base rootfs onto InvariantFS.
#
# Offline only (NO QEMU boot; boot-fedora-qemu.sh is a separate step):
# download the pinned Fedora Container-Base OCI tarball, verify
# its hash, unpack the image layer on an ORDINARY filesystem (never
# extract through FUSE), provision minimally, format a single-device
# volume and a two-device (INVFS_DEV1) volume, import offline with
# invf-import, then verify: object counts, fsck clean, and bit-exact
# reads of regular files.
#
# Run from the repo root after `make`:
#   INVFS_E2E_AGENT=wpxx-fedora bash tools/run-e2e.sh tools/test-fedora-install.sh
#
# Env overrides:
#   INVFS_FEDORA_WORK     work dir    (default /var/tmp/invfs-fedora-test)
#   INVFS_FEDORA_TARBALL  cached OCI tar.xz (else download)
#   INVFS_FEDORA_URL      base URL    (default Fedora 44 Container images)
#   INVFS_FEDORA_ROOTFS   file name   (default below, pinned with hash)
#   INVFS_FEDORA_KEEP=1   keep the work dir instead of cleaning up
set -euo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B="$REPO/bin"
WORK="${INVFS_FEDORA_WORK:-/var/tmp/invfs-fedora-test}"
URL="${INVFS_FEDORA_URL:-https://download.fedoraproject.org/pub/fedora/linux/releases/44/Container/x86_64/images}"
ROOTFS="${INVFS_FEDORA_ROOTFS:-Fedora-Container-Base-Generic-44-1.7.x86_64.oci.tar.xz}"
# Pinned hash (policy: reproducible inputs; bump name+hash together).
SHA256="${INVFS_FEDORA_SHA256:-75200f5752a74a21a616ca9a75e25beb594e2e117a0195c54f87c0b3e3974d1b}"
TARBALL="${INVFS_FEDORA_TARBALL:-$WORK/$ROOTFS}"
STAGE="$WORK/stage"

fail() { echo "FAIL: $*"; exit 1; }
note() { echo "== $* =="; }

for t in invf-mkfs invf-import invf-ls invf-fsck invf-cat; do
    [ -x "$B/$t" ] || fail "missing $B/$t (run make first)"
done
command -v tar >/dev/null || fail "tar not found"
command -v python3 >/dev/null || fail "python3 not found (OCI manifest)"
# The Fedora tree contains root-only files (shadow, sudo); the import runs
# under sudo so they land in the volume instead of being skipped (the
# tool's own contract). Fail here, not mid-run, if sudo would prompt.
command -v sudo >/dev/null || fail "sudo not found"
sudo -n true 2>/dev/null || fail "need passwordless sudo (root-only files)"

mkdir -p "$WORK"
# Fedora trees contain read-only dirs (ca-trust); plain rm -rf fails on
# them and set -e would abort a re-run over a stale stage. Forced removal.
rm -rf "$STAGE" 2>/dev/null || { chmod -R u+w "$STAGE" 2>/dev/null; rm -rf "$STAGE"; }
mkdir -p "$STAGE"

note "stage 1: Fedora OCI tarball"
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
echo "$SHA256  $TARBALL" | sha256sum -c - || fail "tarball hash mismatch"
echo "tarball: $TARBALL ($(stat -c %s "$TARBALL") bytes, hash OK)"

note "stage 2: extract the image layer on an ordinary fs (not FUSE)"
OCI="$WORK/.oci"; rm -rf "$OCI"; mkdir -p "$OCI"
tar -xaf "$TARBALL" -C "$OCI" || fail "OCI outer unpack failed"
# OCI image layout: index.json -> manifest digest -> layers[0].digest.
LAYER=$(python3 - "$OCI/index.json" <<'PY' || fail "no Layers in OCI index"
import json, sys
idx = json.load(open(sys.argv[1]))
mdigest = idx["manifests"][0]["digest"].split(":", 1)[1]
man = json.load(open("%s/blobs/sha256/%s" % (sys.argv[1].rsplit("/", 1)[0], mdigest)))
print(man["layers"][0]["digest"].split(":", 1)[1])
PY
)
[ -n "$LAYER" ] || fail "empty layer list in OCI manifest"
BLAYER="$OCI/blobs/sha256/$LAYER"
[ -s "$BLAYER" ] || fail "layer blob missing: $LAYER"
tar -xaf "$BLAYER" -C "$STAGE" || fail "layer unpack failed"
rm -rf "$OCI"
# OCI whiteouts (.wh.*) would import as junk; a base layer should have
# none, and if it ever does this must fail loudly, not silently carry them.
if find "$STAGE" -name '.wh.*' | grep -q .; then
    find "$STAGE" -name '.wh.*' | head -5
    fail "OCI whiteouts present (unhandled)"
fi
# NOTE: Fedora container images ship systemd unit files but NO systemd
# binary and no init (containers don't run PID 1). Boot needs a dnf
# install of systemd first -- a later step, not this one; see the header.
for f in etc/os-release etc/passwd usr/bin/bash; do
    [ -e "$STAGE/$f" ] || fail "Fedora tree missing $f"
done

note "stage 3: provision minimally (hostname; machine-id stays absent)"
printf 'invfs-fedora\n' > "$STAGE/etc/hostname"

# Regular files used for the bit-exact comparison. --follow resolves the
# merged-usr symlinks (bin/sh) to the file, like the FUSE read does.
# NOTE what is NOT here: /usr/bin/sudo*, /etc/shadow*, /etc/gshadow* are
# root-only (0400/0600 or restricted) in the Fedora tree, so a user-level
# cmp cannot open them. They ARE imported (see sudo below) but cannot be
# verified without root; asserting them would fail on the open, not on
# the bytes. Count stays honest via MATCHED.
BITEXACT="etc/os-release usr/lib/os-release etc/passwd etc/group \
etc/hostname usr/bin/bash bin/sh"
MATCHED=0
for f in $BITEXACT; do [ -f "$STAGE/$f" ] && MATCHED=$((MATCHED + 1)); done
[ "$MATCHED" -ge 3 ] || fail "only $MATCHED bit-exact candidates present"

EXPECT=$(find "$STAGE" -mindepth 1 | wc -l)
echo "staged objects: $EXPECT"

check_volume() { # <label> <img>
    local label="$1" img="$2"
    local objs dirs listed lsout="$WORK/ls-$label.txt"
    # Capture once: `invf-ls | grep -q` under pipefail reports the producer's
    # SIGPIPE as a pipeline failure even when grep matched.
    "$B/invf-ls" "$img" > "$lsout" 2>/dev/null \
        || fail "$label: invf-ls failed"
    # Count ENTRIES, not the tally: the summary counts non-directories
    # only (see tools/test-void-install.sh WP211 for why).
    listed=$(grep -cE '^[[:space:]]*[0-9]+ bytes  inode [0-9]+  ' "$lsout" || true)
    objs=$(tail -1 "$lsout" | awk '{print $1}')
    dirs=$(grep -cE '/$' "$lsout" || true)
    [ -n "$listed" ] || fail "$label: invf-ls produced no entry lines"
    [ "$listed" = "$EXPECT" ] \
        || fail "$label: listed $listed entries != staged $EXPECT \
(dirs=$dirs, invf-ls tally=$objs -- the tally excludes directories by design)"
    echo "$label: invf-ls listed $listed entries (== staged $EXPECT)"
    # bit-exact reads
    local f out ok=1
    for f in $BITEXACT; do
        [ -f "$STAGE/$f" ] || continue
        out="$WORK/out-$label-$(echo "$f" | tr / _)"
        if ! "$B/invf-cat" --follow "$img" "$f" "$out" >/dev/null 2>&1; then
            echo "  cat failed: $f"; ok=0; continue
        fi
        if cmp -s "$STAGE/$f" "$out"; then
            echo "  OK $f"
        else
            echo "  MISMATCH $f"; ok=0
        fi
    done
    [ "$ok" = 1 ] || fail "$label: bit-exact reads"
    # fsck: repair then assert clean (offline). A repairing fsck exits
    # non-zero (3 here, like e2fsck's "corrected"), so the verdict line
    # is the assertion, not the exit code.
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
# Root-only files (shadow, sudo) exist in the Fedora tree; the tool's own
# contract says to re-run as root rather than silently drop them.
sudo "$B/invf-import" "$SINGLE" "$STAGE" 2>&1 | tee "$WORK/import-single.log"
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
sudo env "INVFS_DEV1=$MD_SH" "$B/invf-import" "$MD_RAW" "$STAGE" 2>&1 | tee "$WORK/import-multi.log"
grep -q '0 skipped' "$WORK/import-multi.log" || fail "multi: import skipped files"
check_volume multi "$MD_RAW"
cmp <(head -c $(( (METAHI + 1) * 4096 )) "$MD_RAW") \
    <(head -c $(( (METAHI + 1) * 4096 )) "$MD_SH") \
    || fail "metadata mirror differs between dev0 and dev1"
echo "multi: metadata span ($((METAHI + 1)) blocks) byte-identical on both devices"

if [ "${INVFS_FEDORA_KEEP:-0}" = 1 ]; then
    echo "kept work dir: $WORK (stage + single/multi images)"
else
    rm -rf "$STAGE" "$WORK"/out-* "$WORK"/*.log \
        "$SINGLE" "$MD_RAW" "$MD_SH" 2>/dev/null || true
fi

echo
echo "PASS: Fedora rootfs staged, imported (single + two-device), fsck clean, bit-exact"
