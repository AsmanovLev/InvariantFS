#!/bin/bash
# test-pack-registry.sh -- invf-pack registry fetch (WP608) e2e.
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
# Leg 8 (release name@ver): `install vxr@1.0.0 --release` finds no .ivpack
#   asset (vxr is tarball-only), prints ONE loud fallback line, then
#   fetches the release tarball (releases/download/v1.0.0/), verifies it
#   against the raw-tree sha256 listing, passes the WP203 gate, and
#   lands on a fresh volume.
# Leg 9 (release latest): bare `install vxr --release` resolves the
#   latest version (2.0.0) from the same index.json — no GitHub API.
# Leg 10 (tampered release refuses): a re-packed tarball with a tampered
#   helper fails the sha256 gate exactly like a raw fetch, and a
#   truncated tarball is unlistable; the volume is unchanged in both.
# Leg 11 (missing version): vxr@9.9.9 fails LOUD in both modes, naming
#   the version and the registry.
# Leg 12 (clean): fsck OK on every image; the host roots are untouched.
# Leg 13 (.ivpack install): `install vxi@1.0.0 --release` prefers the
#   .ivpack bundle (releases/download/v1.1.0/<name>-<version>.ivpack),
#   unzips it with the system unzip (no new deps), verifies the bundle
#   sha256 plus the raw-tree listing, takes the missing-signature path
#   (v1.1.0 ships no manifest.sig: -y proceeds with UNSIGNED, -n
#   reports, prompt answers), and lays out manifest + bin/ live with
#   lib/ beside them (verified, dormant). `list` shows [ivpack].
# Leg 14 (no-.so pack): a bundle with manifest + bin/ but no lib/
#   installs bin-only; `list` still shows [ivpack].
# Leg 15 (tampered .ivpack refuses): a re-packed bundle with a modified
#   helper fails the bundle sha256 gate; the volume is unchanged, and
#   the restored bundle installs again.
# Leg 16 (malicious .ivpack refuses): `..`/absolute entries refuse at
#   name validation before extract; a symlink entry refuses at the
#   extracted-tree walk. Neither writes outside the pack dir.
# Leg 17 (missing unzip): unzip absent from PATH fails with the distro
#   hint, never silently.
# Leg 18 (--release-tag): a bundle served under a custom tag installs
#   with --release-tag; the default tag stays v1.1.0.
# Leg 19 (list markers): one volume carrying .ivpack + raw + local
#   installs lists [ivpack] / [raw] / [local] respectively.
# Leg 20 (clean): fsck OK on the new images; the host root is untouched.
# Legs 13-20 run after leg 12; vxr stays tarball-only so legs 8-12 keep
# proving the loud fallback.
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
WORK="${INVFS_PACKREG_WORK:-$(invfs_scratch_root)/invf-packreg-$$}"
IMG=pr-reg.img
IMG2=pr-reg2.img
IMG3=pr-reg3.img
IMG4=pr-reg4.img
IMG5=pr-reg5.img
IMG6=pr-reg6.img
IMG7=pr-reg7.img
rm -rf "$WORK" && mkdir -p "$WORK/repo/codecpacks" "$WORK/keys" "$WORK/empty"
cd "$WORK"
rm -f "$IMG" "$IMG2" "$IMG3" "$IMG4" "$IMG5" "$IMG6" "$IMG7"
PLAT="$(uname -m)-linux-musl"

export INVFS_KEYRING="$WORK/keys"
FIXREPO="file://$WORK/repo"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok:   $*"; }
count_packs() { $B/invf-pack list --volume "${1:-$IMG}" 2>/dev/null | grep -c "volume:${1:-$IMG}" || true; }

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
$B/invf-pack keygen >/dev/null || fail "keygen rc=$?"
mkpack
$B/invf-pack sign --key "$WORK/keys/default.sec" "$PKDIR" >/dev/null \
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
    $B/invf-pack sign --key "$WORK/keys/default.sec" "$d" >/dev/null \
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
$B/invf-pack list --repo "$FIXREPO" > list.log 2>&1 \
    || fail "list rc=$?"
grep -q "vxp.*\[available\]" list.log || fail "leg1: vxp not [available]"
grep -q "vxpU.*\[available\]" list.log || fail "leg1: vxpU not [available]"
grep -q "pack(s) available in registry" list.log \
    || fail "leg1: no available count"
$B/invf-pack list --installed --repo file:///nonexistent-wp608 \
    > listinst.log 2>&1 || fail "leg1: list --installed rc=$?"
grep -q "warning: registry" listinst.log \
    && fail "leg1: --installed touched the network"
pass "registry packs listed [available]; --installed stays offline"

echo "== [2] fetch + install (signed) =="
$B/invf-pack install --repo "$FIXREPO" --fetch-source --volume "$IMG" vxp > install1.log 2>&1 \
    || fail "fetch install rc=$?"
grep -q "fetching vxp 1.0.0" install1.log || fail "leg2: no fetching line"
grep -q "file(s) verified" install1.log || fail "leg2: no sha256 verify line"
grep -q "signature OK" install1.log || fail "leg2: no signature-OK gate note"
$B/invf-pack list --volume "$IMG" 2>&1 | grep -q "1 pack(s)" \
    || fail "leg2: pack count"
pass "bare install fetched, verified, gate-OK, aboard"

echo "== [3] tampered transport refuses =="
printf 'X' >> "$PKDIR/bin/enc"
$B/invf-pack install -y --repo "$FIXREPO" --fetch-source --volume "$IMG" vxp \
    > tamper.log 2>&1 && fail "leg3: tampered helper installed"
grep -q "sha256 mismatch" tamper.log || fail "leg3: no mismatch diagnostic"
[ "$(count_packs)" = "1" ] || fail "leg3: refusal changed the volume"
truncate -s -1 "$PKDIR/bin/enc"
pass "tampered helper refused at transport, volume unchanged"

echo "== [4] bad signature refuses (even with -y) =="
printf 'evil = 1\n' >> "$PKDIR/manifest"
# re-hash the listing so transport passes and the GATE is what refuses
( cd "$PKDIR" && sha256sum manifest manifest.sig bin/enc bin/dec > sha256 )
$B/invf-pack install -y --repo "$FIXREPO" --fetch-source --volume "$IMG" vxp \
    > badsig.log 2>&1 && fail "leg4: bad-signature pack installed"
[ "$(count_packs)" = "1" ] || fail "leg4: refusal changed the volume"
pass "bad-signature fetched pack refused"
# restore the fixture for the remaining legs (fresh tree, fresh signature)
mkpack
$B/invf-pack sign --key "$WORK/keys/default.sec" "$PKDIR" >/dev/null \
    || fail "re-sign rc=$?"
write_listing

echo "== [5] missing-signature fetched matrix =="
before=$(count_packs)
$B/invf-pack install -y --repo "$FIXREPO" --fetch-source --volume "$IMG" vxpU \
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
$B/invf-pack install -n --repo "$FIXREPO" --fetch-source --volume "$IMG" vxpN \
    > unsigned-n.log 2>&1 || fail "leg5: -n rc=$?"
grep -qi "unsigned" unsigned-n.log || fail "leg5: -n names no unsigned state"
[ "$(count_packs)" = "2" ] || fail "leg5: -n installed something"
pass "missing + -n reports UNSIGNED, installs nothing"

echo "== [6] unreachable repo + unknown pack =="
$B/invf-pack list --repo file:///nonexistent-wp608 > unreach.log 2>&1 \
    || fail "leg6: list rc=$?"
grep -q "warning: registry .*unreachable" unreach.log \
    || fail "leg6: no loud unreachable line"
grep -q "\[available\]" unreach.log \
    && fail "leg6: phantom available packs"
$B/invf-pack install --repo "$FIXREPO" --volume "$IMG" nosuchpack \
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
$B/invf-pack install --repo "$FIXREPO" --volume "$IMG2" \
    "vxr@1.0.0" --release > rel1.log 2>&1 \
    || fail "release install rc=$?"
grep -q "fetching vxr 1.0.0 release v1.0.0/vxr-1.0.0-" rel1.log \
    || fail "leg8: no release fetching line"
# vxr ships no .ivpack: exactly ONE loud fallback line, then the tarball.
grep -q "no .ivpack asset for vxr 1.0.0 under tag v1.1.0" rel1.log \
    || fail "leg8: no loud .ivpack fallback line"
[ "$(grep -c "falling back to the release tarball" rel1.log)" = "1" ] \
    || fail "leg8: fallback line is not exactly one"
grep -q "file(s) verified" rel1.log || fail "leg8: no sha256 verify line"
grep -q "signature OK" rel1.log || fail "leg8: no signature-OK gate note"
[ "$(count_packs "$IMG2")" = "1" ] || fail "leg8: pack count"
pass "name@ver --release fetched the tarball, verified, gate-OK, aboard"

echo "== [9] release latest resolve (no GitHub API) =="
$B/invf-mkfs "$IMG3" 1 >/dev/null
$B/invf-pack install --repo "$FIXREPO" --volume "$IMG3" \
    vxr --release > rel2.log 2>&1 \
    || fail "release latest rc=$?"
grep -q "fetching vxr 2.0.0 release" rel2.log \
    || fail "leg9: latest (2.0.0) not picked"
grep -q "no .ivpack asset for vxr 2.0.0 under tag v1.1.0" rel2.log \
    || fail "leg9: no loud .ivpack fallback line"
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
$B/invf-pack install --repo "$FIXREPO" --volume "$IMG4" \
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
$B/invf-pack install --repo "$FIXREPO" --volume "$IMG4" \
    "vxr@1.0.0" --release > reltrunc.log 2>&1 \
    && fail "leg10b: truncated tarball installed"
grep -Eq "cannot list|cannot unpack" reltrunc.log \
    || fail "leg10b: no corrupt-tarball diagnostic"
[ "$(count_packs "$IMG4")" = "0" ] \
    || fail "leg10b: refusal changed the volume"
mv "$TB.good" "$TB"
# restored tarball installs again (the fixture is whole)
$B/invf-pack install --repo "$FIXREPO" --volume "$IMG4" \
    "vxr@1.0.0" --release > relrestore.log 2>&1 \
    || fail "restored release install rc=$?"
[ "$(count_packs "$IMG4")" = "1" ] || fail "leg10c: pack count"
pass "truncated release refused loud; restored tarball installs"

echo "== [11] missing version errors loudly (both modes) =="
$B/invf-pack install --repo "$FIXREPO" --volume "$IMG4" \
    "vxr@9.9.9" --release > relmiss.log 2>&1 \
    && fail "leg11: missing version installed"
grep -q "version '9.9.9' not found" relmiss.log \
    || fail "leg11: release names no version"
grep -q "not found in registry" relmiss.log \
    || fail "leg11: release names no registry"
$B/invf-pack install --repo "$FIXREPO" --fetch-source --volume "$IMG4" \
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

echo "== [13] .ivpack fixture (vxi with lib, vxn bin-only) =="
command -v zip >/dev/null || fail "leg13: zip required for the fixture"
# vxi 1.0.0: manifest + bin helpers + lib/vxi.so, UNSIGNED (no sig
# sidecar, like the real v1.1.0). The raw-tree listing covers the
# raw-tree files only; the bundle carries its own sha256 over every
# other entry (IVPACK-RELEASES.md layout: zip -0 -X, archive root).
VXDIR="$WORK/repo/codecpacks/vxi/1.0.0"
mkdir -p "$VXDIR/bin" "$VXDIR/lib"
cat > "$VXDIR/manifest" <<'EOF'
name = vxi
algo = 46
caps = external
encode = bin/enc {in} {out}
decode = bin/dec {in} {out}
EOF
printf 'i-helper-001' > "$VXDIR/bin/enc"
printf 'i-helper-002' > "$VXDIR/bin/dec"
chmod 755 "$VXDIR/bin/enc" "$VXDIR/bin/dec"
printf 'fake-so-bytes-vxi' > "$VXDIR/lib/vxi.so"
( cd "$VXDIR" && sha256sum manifest bin/enc bin/dec > sha256 )
# vxn 1.0.0: manifest + one bin helper, NO lib/ (no-.so shape).
VNDIR="$WORK/repo/codecpacks/vxn/1.0.0"
mkdir -p "$VNDIR/bin"
cat > "$VNDIR/manifest" <<'EOF'
name = vxn
algo = 46
caps = external
encode = bin/run {in} {out}
decode = bin/run {in} {out}
EOF
printf 'n-helper-run' > "$VNDIR/bin/run"
chmod 755 "$VNDIR/bin/run"
( cd "$VNDIR" && sha256sum manifest bin/run > sha256 )
python3 - "$WORK/repo/codecpacks/index.json" <<'EOF'
import sys, json
p = sys.argv[1]
d = json.load(open(p))
d["packs"].append({"name": "vxi", "version": "1.0.0", "pack_version": 1,
                   "algo": 46, "family": "test", "priority": 70,
                   "manifest": "codecpacks/vxi/1.0.0/manifest",
                   "sha256": "codecpacks/vxi/1.0.0/sha256"})
d["packs"].append({"name": "vxn", "version": "1.0.0", "pack_version": 1,
                   "algo": 46, "family": "test", "priority": 65,
                   "manifest": "codecpacks/vxn/1.0.0/manifest",
                   "sha256": "codecpacks/vxn/1.0.0/sha256"})
json.dump(d, open(p, "w"), indent=2)
EOF
# pack one .ivpack per IVPACK-RELEASES.md: zip -0 -X, entries at the
# archive root (manifest, sha256, lib/ iff present, bin/).
mkivpack() {
    local n="$1" ver="$2"
    local d="$WORK/repo/codecpacks/$n/$ver" st="$WORK/ivstage"
    local out="$WORK/repo/releases/download/v1.1.0" entries
    rm -rf "$st"
    mkdir -p "$st" "$out"
    cp -a "$d/manifest" "$d/bin" "$st/"
    [ -d "$d/lib" ] && cp -a "$d/lib" "$st/"
    ( cd "$st" && { find manifest bin lib -type f 2>/dev/null || true; } \
        | LC_ALL=C sort | xargs -r sha256sum > sha256 )
    entries="manifest sha256 bin"
    [ -d "$st/lib" ] && entries="$entries lib"
    # shellcheck disable=SC2086: entries is intentionally split
    ( cd "$st" && zip -0 -X -q -r "$out/$n-$ver.ivpack" $entries ) \
        || fail "zip $n $ver"
    rm -rf "$st"
    unzip -l "$out/$n-$ver.ivpack" | grep -q "manifest" \
        || fail "ivpack $n $ver has no manifest"
}
mkivpack vxi 1.0.0
mkivpack vxn 1.0.0
cp "$WORK/repo/releases/download/v1.1.0/vxi-1.0.0.ivpack" \
    "$WORK/vxi-1.0.0.ivpack.good"
pass "ivpack fixture ready (vxi with lib, vxn bin-only)"

echo "== [13] .ivpack install (unsigned, lib dormant, [ivpack]) =="
$B/invf-mkfs "$IMG5" 1 >/dev/null
$B/invf-pack install -y --repo "$FIXREPO" --volume "$IMG5" \
    "vxi@1.0.0" --release > iv1.log 2>&1 \
    || fail ".ivpack install rc=$?"
grep -q "fetching vxi 1.0.0 release v1.1.0/vxi-1.0.0.ivpack" iv1.log \
    || fail "leg13: no .ivpack fetching line"
grep -q "(.ivpack, .*file(s) verified" iv1.log \
    || fail "leg13: no .ivpack verified line"
grep -qi "unsigned" iv1.log \
    || fail "leg13: -y names no unsigned state"
grep -q "falling back" iv1.log \
    && fail "leg13: present .ivpack fell back"
[ "$(count_packs "$IMG5")" = "1" ] || fail "leg13: pack count"
$B/invf-pack list --volume "$IMG5" 2>&1 | grep -q "\[ivpack\]" \
    || fail "leg13: list shows no [ivpack]"
# layout: manifest byte-verbatim + bin/ live + lib/ beside them.
$B/invf-cat "$IMG5" .invariantfs/codecpacks/vxi.codecpack/manifest \
    "$WORK/man.got" >/dev/null 2>&1 || fail "leg13: no manifest aboard"
cmp "$WORK/man.got" "$VXDIR/manifest" \
    || fail "leg13: manifest not byte-verbatim"
$B/invf-cat "$IMG5" .invariantfs/codecpacks/vxi.codecpack/bin/enc \
    "$WORK/enc.got" >/dev/null 2>&1 || fail "leg13: no bin/enc aboard"
cmp "$WORK/enc.got" "$VXDIR/bin/enc" \
    || fail "leg13: bin/enc bytes differ"
$B/invf-cat "$IMG5" .invariantfs/codecpacks/vxi.codecpack/lib/vxi.so \
    "$WORK/so.got" >/dev/null 2>&1 || fail "leg13: no lib/vxi.so aboard"
cmp "$WORK/so.got" "$VXDIR/lib/vxi.so" \
    || fail "leg13: lib/vxi.so bytes differ"
pass ".ivpack installed unsigned (-y), manifest+bin live, lib dormant, [ivpack]"

echo "== [13b] .ivpack missing-signature prompt + dry-run =="
$B/invf-mkfs "$IMG6" 1 >/dev/null
printf 'y\n' | $B/invf-pack install --repo "$FIXREPO" \
    --volume "$IMG6" "vxi@1.0.0" --release > ivprompt-y.log 2>&1 \
    || fail "leg13b: piped-y rc=$?"
[ "$(count_packs "$IMG6")" = "1" ] || fail "leg13b: piped-y installed nothing"
$B/invf-pack install -n --repo "$FIXREPO" --volume "$IMG6" \
    "vxn@1.0.0" --release > ivprompt-n.log 2>&1 \
    || fail "leg13b: -n rc=$?"
grep -qi "unsigned" ivprompt-n.log \
    || fail "leg13b: -n names no unsigned state"
[ "$(count_packs "$IMG6")" = "1" ] \
    || fail "leg13b: -n installed something"
printf 'n\n' | $B/invf-pack install --repo "$FIXREPO" \
    --volume "$IMG6" "vxn@1.0.0" --release > ivprompt-n2.log 2>&1 \
    && fail "leg13b: piped-n installed"
[ "$(count_packs "$IMG6")" = "1" ] \
    || fail "leg13b: piped-n changed the volume"
pass "prompt-y installs, prompt-n and -n install nothing"

echo "== [14] no-.so pack installs bin-only =="
$B/invf-pack install -y --repo "$FIXREPO" --volume "$IMG6" \
    "vxn@1.0.0" --release > ivn.log 2>&1 \
    || fail "leg14: no-.so install rc=$?"
grep -q "(.ivpack, .*file(s) verified" ivn.log \
    || fail "leg14: no .ivpack verified line"
[ "$(count_packs "$IMG6")" = "2" ] || fail "leg14: pack count"
$B/invf-cat "$IMG6" .invariantfs/codecpacks/vxn.codecpack/bin/run \
    "$WORK/run.got" >/dev/null 2>&1 || fail "leg14: no bin/run aboard"
cmp "$WORK/run.got" "$VNDIR/bin/run" \
    || fail "leg14: bin/run bytes differ"
$B/invf-cat "$IMG6" .invariantfs/codecpacks/vxn.codecpack/lib/vxn.so \
    "$WORK/so-no.bin" >/dev/null 2>&1 \
    && fail "leg14: unexpected lib/ in a no-.so pack"
$B/invf-pack list --volume "$IMG6" 2>&1 | grep -q "vxn.*\[ivpack\]" \
    || fail "leg14: list shows no [ivpack] for vxn"
pass "no-.so .ivpack installs bin-only, still [ivpack]"

echo "== [15] tampered .ivpack refuses =="
TAMPI="$WORK/repo/releases/download/v1.1.0/vxi-1.0.0.ivpack"
rm -rf "$WORK/ivtamper"
mkdir -p "$WORK/ivtamper"
cp -a "$VXDIR/manifest" "$VXDIR/bin" "$VXDIR/lib" "$WORK/ivtamper/"
cp "$VXDIR/sha256" "$WORK/ivtamper/bundle-sha256-good"
( cd "$VXDIR" && sha256sum manifest lib/vxi.so bin/enc bin/dec \
    > "$WORK/ivtamper/sha256" )
printf 'X' >> "$WORK/ivtamper/bin/enc"
( cd "$WORK/ivtamper" && zip -0 -X -q -r "$TAMPI" \
    manifest sha256 bin lib ) || fail "repack tampered"
before=$(count_packs "$IMG6")
$B/invf-pack install -y --repo "$FIXREPO" --volume "$IMG6" \
    "vxi@1.0.0" --release > ivtamper.log 2>&1 \
    && fail "leg15: tampered bundle installed"
grep -q "sha256 mismatch" ivtamper.log \
    || fail "leg15: no mismatch diagnostic"
[ "$(count_packs "$IMG6")" = "$before" ] \
    || fail "leg15: refusal changed the volume"
cp "$WORK/vxi-1.0.0.ivpack.good" "$TAMPI"
$B/invf-pack install -y --repo "$FIXREPO" --volume "$IMG5" \
    "vxi@1.0.0" --release > ivrestore.log 2>&1 \
    || fail "leg15: restored bundle install rc=$?"
[ "$(count_packs "$IMG5")" = "1" ] || fail "leg15: pack count"
pass "tampered .ivpack refused at the sha256 gate, restored installs"

echo "== [16] malicious .ivpack refuses =="
python3 - "$TAMPI" <<'EOF'
import sys, zipfile
p = sys.argv[1]
z = zipfile.ZipFile(p, "w", zipfile.ZIP_STORED)
z.writestr("manifest", b"name = vxi\n")
z.writestr("sha256", b"dead" * 16 + b"  manifest\n")
z.writestr("bin/enc", b"x")
z.writestr("../evil-probe.txt", b"evil")
z.writestr("/abs-probe.txt", b"evil")
z.close()
EOF
before=$(count_packs "$IMG6")
$B/invf-pack install -y --repo "$FIXREPO" --volume "$IMG6" \
    "vxi@1.0.0" --release > ivevil.log 2>&1 \
    && fail "leg16: escaping bundle installed"
grep -q "escapes the pack dir" ivevil.log \
    || fail "leg16: no escape diagnostic"
[ "$(count_packs "$IMG6")" = "$before" ] \
    || fail "leg16: refusal changed the volume"
[ ! -e "$WORK/evil-probe.txt" ] \
    || fail "leg16: escape wrote outside the pack dir"
python3 - "$TAMPI" <<'EOF'
import sys, zipfile
p = sys.argv[1]
z = zipfile.ZipFile(p, "w", zipfile.ZIP_STORED)
man = b"name = vxi\nalgo = 46\ncaps = external\n"
z.writestr("manifest", man)
zi = zipfile.ZipInfo("bin/link")
zi.create_system = 3
zi.external_attr = (0o120777) << 16
z.writestr(zi, "manifest")
import hashlib
z.writestr("sha256", hashlib.sha256(man).hexdigest().encode() +
           b"  manifest\n" + hashlib.sha256(b"manifest").hexdigest().encode() +
           b"  bin/link\n")
z.close()
EOF
$B/invf-pack install -y --repo "$FIXREPO" --volume "$IMG6" \
    "vxi@1.0.0" --release > ivlink.log 2>&1 \
    && fail "leg16: symlink bundle installed"
grep -q "REFUSED" ivlink.log || fail "leg16: no symlink refusal"
[ "$(count_packs "$IMG6")" = "$before" ] \
    || fail "leg16: symlink refusal changed the volume"
cp "$WORK/vxi-1.0.0.ivpack.good" "$TAMPI"
pass "escaping entries refused pre-extract, symlink refused, nothing written"

echo "== [17] missing unzip fails with the distro hint =="
PATH="$WORK/empty" $B/invf-pack install -y --repo "$FIXREPO" \
    --volume "$IMG6" "vxi@1.0.0" --release > ivnounzip.log 2>&1 \
    && fail "leg17: install without unzip succeeded"
grep -q "unzip is required" ivnounzip.log \
    || fail "leg17: no unzip-required diagnostic"
grep -q "install unzip" ivnounzip.log \
    || fail "leg17: no distro hint"
[ "$(count_packs "$IMG6")" = "$before" ] \
    || fail "leg17: refusal changed the volume"
pass "no unzip: loud distro hint, nothing installed"

echo "== [18] --release-tag override =="
mkdir -p "$WORK/repo/releases/download/v9-custom"
cp "$WORK/vxi-1.0.0.ivpack.good" \
    "$WORK/repo/releases/download/v9-custom/vxi-1.0.0.ivpack"
$B/invf-pack install -y --repo "$FIXREPO" --volume "$IMG6" \
    "vxi@1.0.0" --release --release-tag v9-custom > ivtag.log 2>&1 \
    || fail "leg18: custom-tag install rc=$?"
grep -q "release v9-custom/vxi-1.0.0.ivpack" ivtag.log \
    || fail "leg18: custom tag not used"
[ "$(count_packs "$IMG6")" = "$before" ] \
    || fail "leg18: re-import changed the pack count"
pass "--release-tag serves the bundle from the custom tag"

echo "== [19] list marks [ivpack] vs [raw] vs [local] =="
mkdir -p "$WORK/localpack/bin"
cat > "$WORK/localpack/manifest" <<'EOF'
name = vxl
algo = 46
caps = external
encode = bin/enc {in} {out}
decode = bin/dec {in} {out}
EOF
printf 'local-helper-001' > "$WORK/localpack/bin/enc"
printf 'local-helper-002' > "$WORK/localpack/bin/dec"
chmod 755 "$WORK/localpack/bin/enc" "$WORK/localpack/bin/dec"
$B/invf-pack install --repo "$FIXREPO" --fetch-source --volume "$IMG5" \
    vxp > ivraw.log 2>&1 || fail "leg19: raw install rc=$?"
$B/invf-pack install -y --volume "$IMG5" \
    ./localpack > ivlocal.log 2>&1 || fail "leg19: local install rc=$?"
[ "$(count_packs "$IMG5")" = "3" ] || fail "leg19: pack count"
$B/invf-pack list --volume "$IMG5" > ivmarks.log 2>&1 \
    || fail "leg19: list rc=$?"
grep -q "vxi.*\[ivpack\]" ivmarks.log \
    || fail "leg19: vxi not [ivpack]"
grep -q "vxp.*\[raw\]" ivmarks.log \
    || fail "leg19: vxp not [raw]"
grep -q "vxl.*\[local\]" ivmarks.log \
    || fail "leg19: vxl not [local]"
pass "one volume lists [ivpack] + [raw] + [local]"

echo "== [19b] default is release (bare install, no --release flag) =="
$B/invf-mkfs "$IMG7" 1 >/dev/null
$B/invf-pack install --repo "$FIXREPO" --volume "$IMG7" \
    vxr > defrel.log 2>&1 || fail "leg19b: default install rc=$?"
grep -q "fetching vxr .* release " defrel.log \
    || fail "leg19b: default did not take the release path"
[ "$(count_packs "$IMG7")" = "1" ] || fail "leg19b: pack count"
$B/invf-fsck "$IMG7" | grep -q "^OK$" || fail "leg19b: fsck"
pass "bare install defaults to the release bundle"

echo "== [20] ivpack volumes clean, host untouched =="
for _img in "$IMG5" "$IMG6"; do
    $B/invf-fsck "$_img" | grep -q "^OK$" || fail "leg20: fsck $_img"
done
[ ! -e /.invariantfs/codecpacks/vxi.codecpack ] \
    || fail "leg20: leaked into the host root"
[ ! -e /.invariantfs/codecpacks/vxn.codecpack ] \
    || fail "leg20: leaked into the host root"
[ ! -e /.invariantfs/codecpacks/vxl.codecpack ] \
    || fail "leg20: leaked into the host root"
pass "ivpack fsck OK, host untouched"

echo "PASS: pack registry fetch"
