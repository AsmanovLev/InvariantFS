#!/bin/bash
# test-pack-sign.sh -- pack signatures v1 (WP203) e2e.
#
# Leg 1 (round trip): keygen -> sign -> install --volume succeeds, and the
#   install prints the `signature OK` gate note; list --volume shows it.
# Leg 2 (refuse-bad): tampered manifest, tampered helper, and tampered
#   sidecar each refuse (rc != 0) and change nothing on the volume.
# Leg 3 (missing-sig matrix): --skip installs; piped `y` installs; piped
#   `n`, closed stdin, and no-flag-non-tty refuse; -y installs; -n reports
#   UNSIGNED, exits 0, and installs nothing.
# Leg 4 (untrusted key): a validly signed pack with an empty keyring
#   refuses (signed-by-unknown is unverifiable, fail closed).
# Leg 5 (clean): fsck OK at the end.
#
# All installs go through --volume (offline image): the gate under test
# runs before the host/volume branch, so the volume path exercises the
# same code without mutating the shared host root. Hermetic: images and
# fixtures under a scratch root (tools/lib-scratch.sh picker); the helper
# is fixed bytes, not a script (tamper legs flip a byte); no mount.
#
# Run standalone, or through the e2e runner:
#   INVFS_E2E_AGENT=wp203 bash tools/run-e2e.sh tools/test-pack-sign.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
. "$REPO/tools/lib-scratch.sh"
B=$REPO/bin
WORK="${INVFS_PACKSIGN_WORK:-$(invfs_scratch_root)/invfs-packsign-$$}"
IMG=ps-sign.img
rm -rf "$WORK" && mkdir -p "$WORK/pack/bin" "$WORK/keys" "$WORK/empty"
cd "$WORK"
rm -f "$IMG"

export INVFS_KEYRING="$WORK/keys"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok:   $*"; }

mkpack() {
    cat > pack/manifest <<'EOF'
name = vxp
algo = 46
caps = external
encode = bin/enc {in} {out}
decode = bin/dec {in} {out}
EOF
    printf 'helper-bytes-001' > pack/bin/enc
    printf 'helper-bytes-002' > pack/bin/dec
    chmod 755 pack/bin/enc pack/bin/dec
    rm -f pack/manifest.sig
}
count_packs() { $B/invfs-pack list --volume "$IMG" 2>/dev/null | grep -c "volume:$IMG" || true; }

echo "== [0] keygen =="
$B/invfs-pack keygen || fail "keygen rc=$?"
[ -f "$WORK/keys/default.sec" ] || fail "no default.sec"
[ -f "$WORK/keys/default.pub" ] || fail "no default.pub"
[ "$(stat -c %a "$WORK/keys/default.sec")" = "600" ] || fail "sec not 0600"
pass "keygen wrote default.{sec,pub}, sec 0600"

echo "== [1] sign + install =="
mkpack
$B/invfs-pack sign --key "$WORK/keys/default.sec" ./pack || fail "sign rc=$?"
[ -f pack/manifest.sig ] || fail "no manifest.sig"
grep -q "^integrity = " pack/manifest || fail "no integrity line"
$B/invf-mkfs "$IMG" 1 >/dev/null
$B/invfs-pack install --volume "$IMG" ./pack > install1.log 2>&1 \
    || fail "signed install rc=$?"
grep -q "signature OK" install1.log || fail "no signature-OK gate note"
$B/invfs-pack list --volume "$IMG" 2>&1 | grep -q "1 pack(s)" \
    || fail "leg1: pack count"
pass "signed install OK"

echo "== [2] refuse-bad =="
printf 'evil = 1\n' >> pack/manifest
$B/invfs-pack install -y --volume "$IMG" ./pack >/dev/null 2>&1 \
    && fail "leg2: tampered manifest installed"
pass "tampered manifest refused (even with -y)"
# restore manifest by re-signing, then tamper a helper instead
$B/invfs-pack sign --key "$WORK/keys/default.sec" ./pack >/dev/null \
    || fail "re-sign rc=$?"
printf 'X' >> pack/bin/enc
$B/invfs-pack install -y --volume "$IMG" ./pack >/dev/null 2>&1 \
    && fail "leg2: tampered helper installed"
pass "tampered helper refused (even with -y)"
# restore helper, tamper the sidecar instead (flip one hex char of the
# key field: envelope stays well-formed, trust does not)
truncate -s -1 pack/bin/enc
$B/invfs-pack sign --key "$WORK/keys/default.sec" ./pack >/dev/null \
    || fail "re-sign 2 rc=$?"
python3 - "$WORK/pack/manifest.sig" <<'EOF'
import sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
b[20] = 0x62 if b[20] == 0x61 else 0x61  # flip a<->b, both hex
open(p, 'wb').write(bytes(b))
EOF
$B/invfs-pack install -y --volume "$IMG" ./pack >/dev/null 2>&1 \
    && fail "leg2: tampered sidecar installed"
pass "tampered sidecar refused (even with -y)"
[ "$(count_packs)" = "1" ] || fail "leg2: refusal changed the volume"
pass "refusals changed nothing"

echo "== [3] missing-sig matrix =="
rm -f pack/manifest.sig
# (manifest still carries a stale integrity line: harmless — unsigned
# packs skip the binding check entirely, which is the point of leg 3.)
# No-flag prompt legs need FRESH pack names (no -y => a duplicate would
# refuse after the gate, which would prove nothing about the prompt).
cp -a pack packB
sed -i 's/name = vxp/name = vxpB/' packB/manifest
cp -a pack packC
sed -i 's/name = vxp/name = vxpC/' packC/manifest
$B/invfs-pack install --skip-signature-verification -y --volume "$IMG" ./pack \
    >/dev/null || fail "leg3: --skip rc=$?"
pass "missing + --skip installs"
printf 'y\n' | $B/invfs-pack install --volume "$IMG" ./packB >/dev/null \
    || fail "leg3: piped y rc=$?"
pass "missing + answer-y installs (prompt really asked)"
printf 'n\n' | $B/invfs-pack install --volume "$IMG" ./packC >/dev/null 2>&1 \
    && fail "leg3: answer-n installed"
pass "missing + answer-n aborts"
$B/invfs-pack install --volume "$IMG" ./packC >/dev/null 2>&1 < /dev/null \
    && fail "leg3: closed-stdin installed"
pass "missing + closed stdin aborts"
$B/invfs-pack install -y --volume "$IMG" ./pack >/dev/null \
    || fail "leg3: -y rc=$?"
pass "missing + -y installs"
rm -rf packC && cp -a pack packC && sed -i 's/name = vxp/name = vxpC/' packC/manifest
before=$(count_packs)
$B/invfs-pack install -n --volume "$IMG" ./packC > dry.log 2>&1 \
    || fail "leg3: -n rc=$?"
grep -q "UNSIGNED" dry.log || fail "leg3: -n names the unsigned state"
[ "$(count_packs)" = "$before" ] || fail "leg3: -n installed something"
pass "missing + -n reports UNSIGNED, installs nothing"

echo "== [4] untrusted key =="
$B/invfs-pack sign --key "$WORK/keys/default.sec" ./pack >/dev/null \
    || fail "re-sign 3 rc=$?"
INVFS_KEYRING="$WORK/empty" $B/invfs-pack install -y --volume "$IMG" ./pack \
    >/dev/null 2>&1 && fail "leg4: unknown key installed"
pass "signed-by-unknown-key refused"

echo "== [5] volume clean =="
$B/invf-fsck "$IMG" | grep -q "^OK$" || fail "leg5: fsck not clean"
[ ! -e /.invariantfs/codecpacks/vxp.codecpack ] \
    || fail "leg5: leaked into the host root"
pass "fsck OK, host untouched"

echo "PASS: pack signatures v1"
