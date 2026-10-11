#!/bin/bash
# test-pack-registry.sh -- invfs-pack registry fetch (WP608) e2e.
#
# Leg 1 (list): `list --repo file://<fixture>` shows the fixture pack as
#   [available]; `list --installed` shows installed-only and never touches
#   the network (no unreachable warning even with a bogus --repo).
# Leg 2 (fetch+install): bare `install <name>` from the file:// fixture
#   stages through the tool scratch, sha256-verifies, passes the WP203
#   gate (signed -> `signature OK`), and lands on the volume.
# Leg 3 (refuse-tampered-transport): a fixture helper tampered after the
#   sha256 listing was written refuses with a sha256 mismatch and changes
#   nothing on the volume.
# Leg 4 (refuse-bad-signature): a fixture manifest tampered after signing
#   (listing re-hashed so transport passes) is refused by the install gate
#   even with -y, and changes nothing.
# Leg 5 (missing-signature matrix, fetched): unsigned fixture pack: -y
#   proceeds (UNSIGNED warning), -n reports and installs nothing (rc 0).
# Leg 6 (unreachable): bogus --repo degrades list to installed-only with
#   one loud `registry ... unreachable` line; unknown pack names fail
#   naming the registry.
# Leg 8 (release name@ver): `install vxr@1.0.0 --release` fetches the
#   release tarball (releases/download/v1.0.0/), verifies it against the
#   raw-tree sha256 listing, passes the WP203 gate, and lands on a fresh
#   volume.
# Leg 9 (release latest): bare `install vxr --release` resolves the
#   latest version (2.0.0) from the same index.json — no GitHub API.
# Leg 10 (tampered release refuses): a re-packed tarball with a tampered
#   helper fails the sha256 gate exactly like a raw fetch, and a
#   truncated tarball is unlistable; the volume is unchanged in both.
# Leg 11 (missing version): vxr@9.9.9 fails LOUD in both modes, naming
#   the version and the registry.
# Leg 12 (clean): fsck OK on every image; the host roots are untouched.
#
# RELEASE-LAYOUT ASSUMPTION (loud, as required): the registry repo
# (https://github.com/AsmanovLev/InvariantFS-registry) has cut ZERO tags
# as of 2026-10-12 — no release has ever been published — so the fixture
# layout below is DEFINED from the publisher, not observed from a live
# release: tools/package-release.sh emits dist/<name>-<version>-<platform>
# .tar.gz containing <name>/<version>/{manifest,sha256,sources,bin/*}
# with default platform $(uname -m)-linux-musl, and
# .github/workflows/build.yml uploads dist/*.tar.gz as release assets on
# `v*` tags (softprops/action-gh-release). The fixture therefore serves
# vxr-<ver>-<plat>.tar.gz under <repo>/releases/download/v<ver>/, the
# code's default tag for pack version <ver>. If the registry ever ships
# a different tag/tarball convention, THIS FIXTURE (not the refusal
# semantics) is what must change.
#
# Hermetic: the "registry" is a file:// fixture tree under a scratch root
# (no network, no curl needed for file://); all installs go through
# --volume (offline image), so the shared host root is never mutated.
# No mount, no daemon.
#
# Run standalone, or through the e2e runner:
#   INVFS_E2E_AGENT=wp608 bash tools/run-e2e.sh tools/test-pack-registry.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
. "$REPO/tools/lib-scratch.sh"
B=$REPO/bin
WORK="${INVFS_PACKREG_WORK:-$(invfs_scratch_root)/invfs-packreg-$$}"
IMG=pr-reg.img
IMG2=pr-reg2.img
IMG3=pr-reg3.img
IMG4=pr-reg4.img
rm -rf "$WORK" && mkdir -p "$WORK/repo/codecpacks" "$WORK/keys" "$WORK/empty"
cd "$WORK"
rm -f "$IMG" "$IMG2" "$IMG3" "$IMG4"
PLAT="$(uname -m)-linux-musl"

export INVFS_KEYRING="$WORK/keys"
FIXREPO="file://$WORK/repo"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok:   $*"; }
count_packs() { $B/invfs-pack list --volume "${1:-$IMG}" 2>/dev/null | grep -c "volume:${1:-$IMG}" || true; }

# pack source tree: the fixture repo's version dir doubles as the authored
# pack dir (signed BEFORE the sha256 listing is written: the listing is
# transport, deleted from the staged tree before the gate runs, so it must
# not be part of the signed set).
PKDIR="$WORK/repo/codecpacks/vxp/1.0.0"
mkdir -p "$PKDIR/bin"
mkpack() {
    rm -rf "$PKDIR"
    mkdir -p "$PKDIR/bin"
    cat > "$PKDIR/manifest" <<'EOF'
name = vxp
algo = 46
caps = external
encode = bin/enc {in} {out}
decode = bin/dec {in} {out}
EOF
    printf 'helper-bytes-001' > "$PKDIR/bin/enc"
    printf 'helper-bytes-002' > "$PKDIR/bin/dec"
    chmod 755 "$PKDIR/bin/enc" "$PKDIR/bin/dec"
}
write_listing() {
    # sha256 listing over the fetchable set (never over itself).
    ( cd "$PKDIR" && sha256sum manifest manifest.sig bin/enc bin/dec > sha256 )
}
write_index() {
    cat > "$WORK/repo/codecpacks/index.json" <<'EOF'
{
  "schema": 1,
  "tool": "test-pack-registry.sh WP608 fixture",
  "packs": [
    {
      "name": "vxp",
      "version": "1.0.0",
      "pack_version": 1,
      "algo": 46,
      "family": "test",
      "priority": 50,
      "manifest": "codecpacks/vxp/1.0.0/manifest",
      "sha256": "codecpacks/vxp/1.0.0/sha256"
    },
    {
      "name": "vxpU",
      "version": "2.1.0",
      "pack_version": 2,
      "algo": 46,
      "family": "test",
      "priority": 40,
      "manifest": "codecpacks/vxpU/2.1.0/manifest",
      "sha256": "codecpacks/vxpU/2.1.0/sha256"
    },
    {
      "name": "vxr",
      "version": "1.0.0",
      "pack_version": 1,
      "algo": 46,
      "family": "test",
      "priority": 60,
      "manifest": "codecpacks/vxr/1.0.0/manifest",
      "sha256": "codecpacks/vxr/1.0.0/sha256"
    },
    {
      "name": "vxr",
      "version": "2.0.0",
      "pack_version": 2,
      "algo": 46,
      "family": "test",
      "priority": 60,
      "manifest": "codecpacks/vxr/2.0.0/manifest",
      "sha256": "codecpacks/vxr/2.0.0/sha256"
    }
  ]
}
EOF
}
# unsigned second pack (leg 5): no manifest.sig, no integrity line.
PKUDIR="$WORK/repo/codecpacks/vxpU/2.1.0"
mkupack() {
    rm -rf "$PKUDIR"
    mkdir -p "$PKUDIR/bin"
    cat > "$PKUDIR/manifest" <<'EOF'
name = vxpU
algo = 46
caps = external
encode = bin/enc {in} {out}
decode = bin/dec {in} {out}
EOF
    printf 'u-helper-001' > "$PKUDIR/bin/enc"
    printf 'u-helper-002' > "$PKUDIR/bin/dec"
    chmod 755 "$PKUDIR/bin/enc" "$PKUDIR/bin/dec"
    ( cd "$PKUDIR" && sha256sum manifest bin/enc bin/dec > sha256 )
}

echo "== [0] fixture =="
$B/invfs-pack keygen >/dev/null || fail "keygen rc=$?"
mkpack
$B/invfs-pack sign --key "$WORK/keys/default.sec" "$PKDIR" >/dev/null \
    || fail "sign rc=$?"
write_listing
mkupack
write_index
# release-fixture pack vxr, two versions: raw-tree dirs (signed, listed)
# plus release tarballs under releases/download/v<ver>/, packed exactly
# like tools/package-release.sh (`tar -C stage -czf dist/<n>-<v>-<p>.tgz
# <n>/<v>` over the signed tree, sha256 listing included in the tarball
# as the publisher ships it).
mkrelpack() {
    local ver="$1" seed="$2"
    local d="$WORK/repo/codecpacks/vxr/$ver"
    local stage="$WORK/stage-vxr-$ver" out="$WORK/repo/releases/download/v$ver"
    rm -rf "$d"
    mkdir -p "$d/bin"
    cat > "$d/manifest" <<'EOF'
name = vxr
algo = 46
caps = external
encode = bin/enc {in} {out}
decode = bin/dec {in} {out}
EOF
    printf 'r-helper-%s-001' "$seed" > "$d/bin/enc"
    printf 'r-helper-%s-002' "$seed" > "$d/bin/dec"
    chmod 755 "$d/bin/enc" "$d/bin/dec"
    $B/invfs-pack sign --key "$WORK/keys/default.sec" "$d" >/dev/null \
        || fail "sign vxr $ver rc=$?"
    ( cd "$d" && sha256sum manifest manifest.sig bin/enc bin/dec > sha256 )
    rm -rf "$stage"
    mkdir -p "$stage/vxr" "$out"
    cp -a "$d" "$stage/vxr/$ver"
    tar -C "$stage" -czf "$out/vxr-$ver-$PLAT.tar.gz" "vxr/$ver" \
        || fail "tar vxr $ver"
    rm -rf "$stage"
}
mkrelpack 1.0.0 a
mkrelpack 2.0.0 b
$B/invf-mkfs "$IMG" 1 >/dev/null
pass "fixture registry ready"

echo "== [1] list: available vs installed =="
$B/invfs-pack list --repo "$FIXREPO" > list.log 2>&1 \
    || fail "list rc=$?"
grep -q "vxp.*\[available\]" list.log || fail "leg1: vxp not [available]"
grep -q "vxpU.*\[available\]" list.log || fail "leg1: vxpU not [available]"
grep -q "pack(s) available in registry" list.log \
    || fail "leg1: no available count"
$B/invfs-pack list --installed --repo file:///nonexistent-wp608 \
    > listinst.log 2>&1 || fail "leg1: list --installed rc=$?"
grep -q "warning: registry" listinst.log \
    && fail "leg1: --installed touched the network"
pass "registry packs listed [available]; --installed stays offline"

echo "== [2] fetch + install (signed) =="
$B/invfs-pack install --repo "$FIXREPO" --volume "$IMG" vxp > install1.log 2>&1 \
    || fail "fetch install rc=$?"
grep -q "fetching vxp 1.0.0" install1.log || fail "leg2: no fetching line"
grep -q "file(s) verified" install1.log || fail "leg2: no sha256 verify line"
grep -q "signature OK" install1.log || fail "leg2: no signature-OK gate note"
$B/invfs-pack list --volume "$IMG" 2>&1 | grep -q "1 pack(s)" \
    || fail "leg2: pack count"
pass "bare install fetched, verified, gate-OK, aboard"

echo "== [3] tampered transport refuses =="
printf 'X' >> "$PKDIR/bin/enc"
$B/invfs-pack install -y --repo "$FIXREPO" --volume "$IMG" vxp \
    > tamper.log 2>&1 && fail "leg3: tampered helper installed"
grep -q "sha256 mismatch" tamper.log || fail "leg3: no mismatch diagnostic"
[ "$(count_packs)" = "1" ] || fail "leg3: refusal changed the volume"
truncate -s -1 "$PKDIR/bin/enc"
pass "tampered helper refused at transport, volume unchanged"

echo "== [4] bad signature refuses (even with -y) =="
printf 'evil = 1\n' >> "$PKDIR/manifest"
# re-hash the listing so transport passes and the GATE is what refuses
( cd "$PKDIR" && sha256sum manifest manifest.sig bin/enc bin/dec > sha256 )
$B/invfs-pack install -y --repo "$FIXREPO" --volume "$IMG" vxp \
    > badsig.log 2>&1 && fail "leg4: bad-signature pack installed"
[ "$(count_packs)" = "1" ] || fail "leg4: refusal changed the volume"
pass "bad-signature fetched pack refused"
# restore the fixture for the remaining legs (fresh tree, fresh signature)
mkpack
$B/invfs-pack sign --key "$WORK/keys/default.sec" "$PKDIR" >/dev/null \
    || fail "re-sign rc=$?"
write_listing

echo "== [5] missing-signature fetched matrix =="
before=$(count_packs)
$B/invfs-pack install -y --repo "$FIXREPO" --volume "$IMG" vxpU \
    > unsigned-y.log 2>&1 || fail "leg5: -y rc=$?"
grep -qi "unsigned" unsigned-y.log || fail "leg5: -y names no unsigned state"
[ "$(count_packs)" = "2" ] || fail "leg5: -y installed nothing"
pass "missing + -y proceeds"
# -n on a fresh unsigned name: reports, installs nothing, rc 0
mkdir -p "$WORK/repo/codecpacks/vxpN/3.0.0/bin"
sed 's/name = vxpU/name = vxpN/' "$PKUDIR/manifest" \
    > "$WORK/repo/codecpacks/vxpN/3.0.0/manifest"
printf 'n-helper-001' > "$WORK/repo/codecpacks/vxpN/3.0.0/bin/enc"
printf 'n-helper-002' > "$WORK/repo/codecpacks/vxpN/3.0.0/bin/dec"
( cd "$WORK/repo/codecpacks/vxpN/3.0.0" \
    && sha256sum manifest bin/enc bin/dec > sha256 )
python3 - "$WORK/repo/codecpacks/index.json" <<'EOF'
import sys, json
p = sys.argv[1]
d = json.load(open(p))
d["packs"].append({"name": "vxpN", "version": "3.0.0", "pack_version": 3,
                   "algo": 46, "family": "test", "priority": 30,
                   "manifest": "codecpacks/vxpN/3.0.0/manifest",
                   "sha256": "codecpacks/vxpN/3.0.0/sha256"})
json.dump(d, open(p, "w"), indent=2)
EOF
$B/invfs-pack install -n --repo "$FIXREPO" --volume "$IMG" vxpN \
    > unsigned-n.log 2>&1 || fail "leg5: -n rc=$?"
grep -qi "unsigned" unsigned-n.log || fail "leg5: -n names no unsigned state"
[ "$(count_packs)" = "2" ] || fail "leg5: -n installed something"
pass "missing + -n reports UNSIGNED, installs nothing"

echo "== [6] unreachable repo + unknown pack =="
$B/invfs-pack list --repo file:///nonexistent-wp608 > unreach.log 2>&1 \
    || fail "leg6: list rc=$?"
grep -q "warning: registry .*unreachable" unreach.log \
    || fail "leg6: no loud unreachable line"
grep -q "\[available\]" unreach.log \
    && fail "leg6: phantom available packs"
$B/invfs-pack install --repo "$FIXREPO" --volume "$IMG" nosuchpack \
    > unknown.log 2>&1 && fail "leg6: unknown pack installed"
grep -q "not found in registry" unknown.log \
    || fail "leg6: unknown pack names no registry"
pass "unreachable degrades loud; unknown names the registry"

echo "== [7] volume clean, host untouched =="
$B/invf-fsck "$IMG" | grep -q "^OK$" || fail "leg7: fsck not clean"
[ ! -e /.invariantfs/codecpacks/vxp.codecpack ] \
    || fail "leg7: leaked into the host root"
[ ! -e /.invariantfs/codecpacks/vxpU.codecpack ] \
    || fail "leg7: leaked into the host root"
pass "fsck OK, host untouched"

echo "== [8] release tarball install (name@ver) =="
$B/invf-mkfs "$IMG2" 1 >/dev/null
$B/invfs-pack install --repo "$FIXREPO" --volume "$IMG2" \
    "vxr@1.0.0" --release > rel1.log 2>&1 \
    || fail "release install rc=$?"
grep -q "fetching vxr 1.0.0 release v1.0.0/vxr-1.0.0-" rel1.log \
    || fail "leg8: no release fetching line"
grep -q "file(s) verified" rel1.log || fail "leg8: no sha256 verify line"
grep -q "signature OK" rel1.log || fail "leg8: no signature-OK gate note"
[ "$(count_packs "$IMG2")" = "1" ] || fail "leg8: pack count"
pass "name@ver --release fetched the tarball, verified, gate-OK, aboard"

echo "== [9] release latest resolve (no GitHub API) =="
$B/invf-mkfs "$IMG3" 1 >/dev/null
$B/invfs-pack install --repo "$FIXREPO" --volume "$IMG3" \
    vxr --release > rel2.log 2>&1 \
    || fail "release latest rc=$?"
grep -q "fetching vxr 2.0.0 release" rel2.log \
    || fail "leg9: latest (2.0.0) not picked"
grep -q "signature OK" rel2.log || fail "leg9: no signature-OK gate note"
[ "$(count_packs "$IMG3")" = "1" ] || fail "leg9: pack count"
pass "bare --release resolved latest from index.json"

echo "== [10] tampered release tarball refuses =="
# re-pack vxr 1.0.0 with a tampered helper; the raw-tree listing is NOT
# re-hashed, so the extracted tree must fail the sha256 gate exactly
# like a tampered raw fetch.
TAMPD="$WORK/tamper-vxr"
rm -rf "$TAMPD"
mkdir -p "$TAMPD/vxr"
cp -a "$WORK/repo/codecpacks/vxr/1.0.0" "$TAMPD/vxr/1.0.0"
printf 'X' >> "$TAMPD/vxr/1.0.0/bin/enc"
TB="$WORK/repo/releases/download/v1.0.0/vxr-1.0.0-$PLAT.tar.gz"
cp "$TB" "$TB.good"
tar -C "$TAMPD" -czf "$TB" "vxr/1.0.0" || fail "repack tampered"
rm -rf "$TAMPD"
$B/invf-mkfs "$IMG4" 1 >/dev/null
$B/invfs-pack install --repo "$FIXREPO" --volume "$IMG4" \
    "vxr@1.0.0" --release > reltamper.log 2>&1 \
    && fail "leg10: tampered tarball installed"
grep -q "sha256 mismatch" reltamper.log \
    || fail "leg10: no mismatch diagnostic"
[ "$(count_packs "$IMG4")" = "0" ] \
    || fail "leg10: refusal changed the volume"
pass "tampered release refused at the sha256 gate, volume unchanged"
# truncated tarball: not even listable, still a loud refusal
head -c 120 "$TB.good" > "$WORK/trunc.tgz"
mv "$WORK/trunc.tgz" "$TB"
$B/invfs-pack install --repo "$FIXREPO" --volume "$IMG4" \
    "vxr@1.0.0" --release > reltrunc.log 2>&1 \
    && fail "leg10b: truncated tarball installed"
grep -Eq "cannot list|cannot unpack" reltrunc.log \
    || fail "leg10b: no corrupt-tarball diagnostic"
[ "$(count_packs "$IMG4")" = "0" ] \
    || fail "leg10b: refusal changed the volume"
mv "$TB.good" "$TB"
# restored tarball installs again (the fixture is whole)
$B/invfs-pack install --repo "$FIXREPO" --volume "$IMG4" \
    "vxr@1.0.0" --release > relrestore.log 2>&1 \
    || fail "restored release install rc=$?"
[ "$(count_packs "$IMG4")" = "1" ] || fail "leg10c: pack count"
pass "truncated release refused loud; restored tarball installs"

echo "== [11] missing version errors loudly (both modes) =="
$B/invfs-pack install --repo "$FIXREPO" --volume "$IMG4" \
    "vxr@9.9.9" --release > relmiss.log 2>&1 \
    && fail "leg11: missing version installed"
grep -q "version '9.9.9' not found" relmiss.log \
    || fail "leg11: release names no version"
grep -q "not found in registry" relmiss.log \
    || fail "leg11: release names no registry"
$B/invfs-pack install --repo "$FIXREPO" --volume "$IMG4" \
    "vxr@9.9.9" > relmiss-raw.log 2>&1 \
    && fail "leg11: raw missing version installed"
grep -q "version '9.9.9' not found" relmiss-raw.log \
    || fail "leg11: raw names no version"
[ "$(count_packs "$IMG4")" = "1" ] \
    || fail "leg11: refusal changed the volume"
pass "missing version fails loud in release and raw modes"

echo "== [12] release volumes clean, host untouched =="
for _img in "$IMG2" "$IMG3" "$IMG4"; do
    $B/invf-fsck "$_img" | grep -q "^OK$" || fail "leg12: fsck $_img"
done
[ ! -e /.invariantfs/codecpacks/vxr.codecpack ] \
    || fail "leg12: leaked into the host root"
pass "release fsck OK, host untouched"

echo "PASS: pack registry fetch"
