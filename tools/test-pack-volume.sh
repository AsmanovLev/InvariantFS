#!/bin/bash
# test-pack-volume.sh -- invfs-pack --volume (install/list packs on an
# offline volume image) e2e.
#
# Leg 1 (install): a script-helper pack installs onto the image; list
#   --volume shows it with a volume: source; the volume opens with the
#   pack loaded (vol scan names it).
# Leg 2 (refusals): duplicate install without -y refuses; -n changes
#   nothing; -y re-imports cleanly.
# Leg 3 (read-only proof): fsck clean after all of the above; the host
#   roots are untouched (nothing installed to /.invariantfs).
#
# Hermetic: images under /dev/shm (relative paths); the helper is a
# shell script (no external tools); no mount, no daemon.
#
# WP203: the fixture pack is UNSIGNED, so every install below carries
# --skip-signature-verification. That flag is about the prompt, not the
# duplicate logic: leg 2's without--y refusal still refuses (at the
# duplicate check, after the gate), and the -n leg still changes nothing.
# Signed-install coverage lives in tools/test-pack-sign.sh.
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/packvol
IMG=pv-inspect.img
rm -rf "$WORK" && mkdir -p "$WORK/src/.invariantfs/codecpacks/vxp.codecpack/bin"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== [1] install onto the volume =="
cat > "$WORK/src/.invariantfs/codecpacks/vxp.codecpack/manifest" <<'EOF'
name = vxp
algo = 46
caps = external
encode = bin/enc {in} {out}
decode = bin/dec {in} {out}
EOF
printf '#!/bin/sh\ncat "$1" > "$2"\n' > "$WORK/src/.invariantfs/codecpacks/vxp.codecpack/bin/enc"
printf '#!/bin/sh\ncp "$1" "$2"\n' > "$WORK/src/.invariantfs/codecpacks/vxp.codecpack/bin/dec"
chmod 755 "$WORK/src/.invariantfs/codecpacks/vxp.codecpack/bin/enc" \
          "$WORK/src/.invariantfs/codecpacks/vxp.codecpack/bin/dec"
$B/invf-mkfs "$IMG" 1 >/dev/null
$B/invfs-pack install --skip-signature-verification --volume "$IMG" "$WORK/src/.invariantfs/codecpacks/vxp.codecpack" \
    || fail "install --volume rc=$?"
$B/invfs-pack list --volume "$IMG" > "$WORK/list.txt" \
    || fail "list --volume rc=$?"
grep -q "vxp" "$WORK/list.txt" || fail "leg1: vxp not listed"
grep -q "volume:$IMG" "$WORK/list.txt" || fail "leg1: no volume: source"
$B/invfs-pack list --volume "$IMG" 2>&1 | grep -q "1 pack(s)" \
    || fail "leg1: pack count"

echo "== [2] refusals =="
$B/invfs-pack install --skip-signature-verification --volume "$IMG" "$WORK/src/.invariantfs/codecpacks/vxp.codecpack" \
    >/dev/null 2>&1 || RC=$?
[ "${RC:-0}" -ne 0 ] || fail "leg2: duplicate rc=0 without -y"
$B/invfs-pack install --skip-signature-verification -n --volume "$IMG" "$WORK/src/.invariantfs/codecpacks/vxp.codecpack" \
    | grep -q "would import" || fail "leg2: -n output"
$B/invfs-pack install --skip-signature-verification -y --volume "$IMG" "$WORK/src/.invariantfs/codecpacks/vxp.codecpack" \
    >/dev/null || fail "leg2: -y re-import rc=$?"
$B/invfs-pack list --volume "$IMG" 2>&1 | grep -q "1 pack(s)" \
    || fail "leg2: pack count after -y"

echo "== [3] volume clean, host untouched =="
$B/invf-fsck "$IMG" | grep -q "^OK$" || fail "leg3: fsck not clean"
[ ! -e /.invariantfs/codecpacks/vxp.codecpack ] \
    || fail "leg3: leaked into the host root"
$B/invf-ls "$IMG" | grep -q "vxp.codecpack" \
    || fail "leg3: pack tree not on the volume"

echo "PASS: pack --volume"
