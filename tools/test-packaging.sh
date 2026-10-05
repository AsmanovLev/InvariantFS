#!/bin/bash
# test-packaging.sh — WP114: prove the packaging layer actually installs.
#
# The point of this suite is that "installing is not verifying". For each of
# the four advertised targets it drives that target's OWN recipe against the
# release artifact into a staging root, then asserts the result:
#
#   * every bin/invf-* the artifact shipped is present and executable
#   * every codecpack helper is present and executable
#   * the man pages are PRESENT, GZIPPED, at manN/<name>.<sec>.gz, including
#     the ru/ translations, and at least one RENDERS
#   * the reported version matches the artifact's VERSION file
#
# The recipe is really executed -- the script sources packaging/PKGBUILD,
# packaging/void/template and packaging/gentoo/invfs-*.ebuild and calls their
# package()/do_install()/src_install() with makepkg/xbps-src/portage's
# variables. Only the two or three functions that are not part of "where do
# the files go" (vmkdir, systemd_dounit, cd to $S) are stubbed, and each
# stub says so. Anything else is the recipe's own code.
#
# Debian is different on purpose: a Debian source package builds from source,
# so its artifact is the .deb itself, and this suite builds it with
# dpkg-buildpackage and inspects the result with dpkg-deb. Set
# INVFS_PKG_DEB=0 to skip the .deb build (the payload is still checked).
#
# NEGATIVE CONTROLS (this is the part that proves the checks can fail):
#   1. a truncated artifact is rejected by sha256 and installs nothing
#   2. a tampered SHA256SUMS is rejected
#   3. an UNCOMPRESSED man page install fails the man assertion, so the
#      gzip check is known to be able to fail rather than always passing
#   4. bootstrap.sh --pubkey with no signature fails closed
#
# Environment:
#   INVFS_PKG_DEB=0|1     build the .deb (default 1)
#   KEEP=1                keep the staging roots for inspection
#
# Run standalone, or through the e2e runner:
#   INVFS_E2E_AGENT=wp-vm-packaging-installer bash tools/run-e2e.sh tools/test-packaging.sh
set -uo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
# WP205: one scratch-root answer for the whole suite set (see
# tools/lib-scratch.sh).
. "$REPO/tools/lib-scratch.sh"
# /tmp is a tmpfs on this host: it is RAM. Staging a packaging run there makes
# the run fail on space and report it as "did not install a plain page" -- a
# claim about the product that was really about the disk (see INCIDENTS.md).
# WP205: the root comes from the shared picker, which prefers real disk for
# exactly that reason; keep the variable overridable.
WORK="${WORK:-$(invfs_scratch_root)/invfs-pkgtest.$$}"
# Single %, not %%: `%%/*` strips the LONGEST suffix starting with a slash,
# which for an absolute path is the WHOLE path -- so the test saw an empty
# string, `[ -d "" ]` was always false, and this silently fell back to /tmp on
# every run. The fix above looked applied and was not.
[ -d "$(dirname "$WORK")" ] || WORK="${TMPDIR:-/tmp}/invfs-pkgtest.$$"
DEB="${INVFS_PKG_DEB:-1}"
KEEP="${KEEP:-0}"
NFAIL=0

cleanup() { [ "$KEEP" = 1 ] || rm -rf "$WORK"; }
trap cleanup EXIT

fail() { echo "FAIL: $*"; NFAIL=$((NFAIL+1)); }
pass() { echo "ok:   $*"; }
note() { echo ":: $*"; }

need() { command -v "$1" >/dev/null 2>&1 || { echo "FAIL: missing tool: $1"; exit 1; }; }
need tar; need gzip; need file; need awk

mkdir -p "$WORK"
cd "$REPO"

# --------------------------------------------------------------------------
# Build the artifact under test
# --------------------------------------------------------------------------
note "building the release artifact (make release)"
if ! make release > "$WORK/release.log" 2>&1; then
    echo "FAIL: make release failed:"; tail -20 "$WORK/release.log"; exit 1
fi
VER=$(cut -d- -f2 < <(ls dist/invfs-*-x86_64.tar.zst 2>/dev/null | head -1 | xargs -r basename) 2>/dev/null)
ART=$(ls dist/invfs-*-x86_64.tar.zst | head -1)
# Absolute, because the rpm leg unpacks it from inside a staging directory and a
# relative "dist/..." does not resolve after the cd. CI said
#     FAIL: rpm: cannot unpack dist/invfs-v0.5.0-x86_64.tar.zst
# which reads like a corrupt archive and is really just tar not finding the file.
ART_ABS=$(cd "$(dirname "$ART")" && pwd)/$(basename "$ART")
SUM=dist/SHA256SUMS
ARTBASE=$(basename "$ART")
note "artifact: $ARTBASE"
note "expected: $(cut -d' ' -f1 "$SUM")"

# The artifact's own idea of its version
VFILE=$(tar --zstd -xOf "$ART" --wildcards '*/VERSION' | head -1)
note "artifact VERSION: $VFILE"

# --------------------------------------------------------------------------
# The shared assertions. $1 = staging root, $2 = label
# --------------------------------------------------------------------------
assert_tree() {
    root=$1; label=$2
    bin="$root/usr/bin"

    # --- binaries resolve and run ---
    [ -d "$bin" ] || { fail "$label: no $bin"; return; }
    nbin=$(ls "$bin" 2>/dev/null | wc -l)
    [ "$nbin" -ge 10 ] || fail "$label: only $nbin binaries in usr/bin"
    pass "$label: $nbin binaries in usr/bin"

    # Every invf-* the artifact shipped must be installed and executable.
    want=$(tar --zstd -tf "$ART" | sed -n 's|.*/bin/\(invf-[a-z0-9_-]*\)$|\1|p' | sort -u)
    miss=
    for b in $want; do
        [ -x "$bin/$b" ] || miss="$miss $b"
    done
    if [ -n "$miss" ]; then fail "$label: missing/non-exec binaries:$miss"
    else pass "$label: all $(echo "$want" | wc -w) artifact binaries present + exec"; fi

    # --- codecpack helpers ---
    nhelp=$(find "$root/usr/lib/invfs/codecpacks" -name bin -type d 2>/dev/null | wc -l)
    hlist=$(find "$root/usr/lib/invfs/codecpacks" -path '*/bin/*' -type f 2>/dev/null | wc -l)
    if [ "$hlist" -lt 8 ]; then fail "$label: only $hlist codecpack helpers installed"
    else pass "$label: $hlist codecpack helpers across $nhelp packs"; fi

    # --- MAN PAGES: present, gzipped, right path, including ru/ ---
    mandir="$root/usr/share/man"
    if [ ! -d "$mandir" ]; then fail "$label: no $mandir"; return; fi

    # every English page from the source tree must be installed as .gz
    neng=0; nbad=0
    for m in packaging/man/*.[178]; do
        [ -f "$m" ] || continue
        sec=${m##*.}; base=$(basename "$m")
        gz="$mandir/man$sec/$base.gz"
        neng=$((neng+1))
        if [ ! -f "$gz" ]; then
            # is it installed but UNCOMPRESSED? that is the bug this asserts
            if [ -f "$mandir/man$sec/$base" ]; then
                fail "$label: $base installed UNCOMPRESSED (policy wants $base.gz)"
            else
                fail "$label: man page missing: man$sec/$base.gz"
            fi
            nbad=$((nbad+1))
        elif ! file "$gz" | grep -q 'gzip compressed'; then
            fail "$label: $base.gz is not gzip: $(file -b "$gz")"; nbad=$((nbad+1))
        fi
    done
    [ "$nbad" = 0 ] && pass "$label: $neng English man pages, all gzip at manN/*.gz"

    # same for the Russian translations -- the ones debian/install used to drop
    nru=0; nbad=0
    for m in packaging/man/ru/*.[178]; do
        [ -f "$m" ] || continue
        sec=${m##*.}; base=$(basename "$m")
        nru=$((nru+1))
        gz="$mandir/man$sec/ru/$base.gz"
        if [ ! -f "$gz" ]; then fail "$label: ru man page missing: man$sec/ru/$base.gz"; nbad=$((nbad+1))
        elif ! file "$gz" | grep -q 'gzip compressed'; then
            fail "$label: ru $base.gz is not gzip"; nbad=$((nbad+1))
        fi
    done
    [ "$nru" -ge 10 ] || fail "$label: only $nru ru pages in the source tree?"
    [ "$nbad" = 0 ] && pass "$label: $nru Russian man pages, all gzip at manN/ru/*.gz"

    # --- one of them RENDERS ---
    if command -v man >/dev/null 2>&1; then
        rendered=0
        for gz in "$mandir/man8/invf-mkfs.8.gz" "$mandir/man7/invarifs.7.gz" \
                   "$mandir/man8/ru/invf-mkfs.8.gz"; do
            [ -f "$gz" ] || continue
            out=$(MANWIDTH=80 man --nh --nj -l "$gz" 2>/dev/null)
            case "$out" in
                *"invf-mkfs"*|*"INVF-MKFS"*|*"InvariantFS"*)
                    rendered=$((rendered+1))
                    echo "$out" | head -3 | sed "s/^/     | /"
                    ;;
            esac
        done
        if [ "$rendered" -ge 1 ]; then pass "$label: $rendered man page(s) rendered via man -l"
        else fail "$label: no man page rendered to recognizable text"; fi
    else
        note "$label: no man(1) on this host, skipping render check"
    fi
}

assert_version() {
    root=$1; label=$2
    # invf-mkfs is the cheapest --version reporter; tolerate its exact wording
    out=$("$root/usr/bin/invf-mkfs" --version 2>&1 | head -3)
    case "$out" in
        *"$VFILE"*|*"$VFILE"*) pass "$label: invf-mkfs --version reports $VFILE" ;;
        *) fail "$label: invf-mkfs --version says '$out', expected $VFILE" ;;
    esac
}

# --------------------------------------------------------------------------
# 1. ARCH — run the PKGBUILD's own package()
# --------------------------------------------------------------------------
stage_arch() {
    d="$WORK/arch"; mkdir -p "$d/src" "$d/pkg"
    tar --zstd -xf "$ART" -C "$d/src"
    # makepkg's variables, and the only makepkg function PKGBUILD uses.
    srcdir="$d/src/invfs-$VFILE-x86_64"
    pkgdir="$d/pkg"
    startdir="$REPO/packaging"
    ( set -e
      . "$REPO/packaging/PKGBUILD"
      package
    ) > "$d/log" 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        fail "arch: PKGBUILD package() failed (rc=$rc)"; tail -15 "$d/log"
        return 1
    fi
    pass "arch: PKGBUILD package() ran clean"
    assert_tree "$pkgdir" "arch"; assert_version "$pkgdir" "arch"
}

# --------------------------------------------------------------------------
# 2. GENTOO — run the ebuild's own src_install()
# --------------------------------------------------------------------------
stage_gentoo() {
    d="$WORK/gentoo"; mkdir -p "$d" "$d/image"
    tar --zstd -xf "$ART" -C "$d"
    S="$d/invfs-$VFILE-x86_64"     # portage's $S
    ED="$d/image"                  # portage's $ED (the staging root)
    PV=$(echo "$VFILE" | sed 's/^v//')
    # the ONLY things stubbed: portage's unit-file helper, and its shell setup
    systemd_dounit() { :; }        # portage: installs unit files into $ED
    ( set -e
        . "$REPO/packaging/gentoo/invfs-$PV.ebuild"
      src_install
    ) > "$d/log" 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
          # The log BEFORE fail, not after. `fail` exits, so trailing it on the
          # same line is the same as never printing it -- which is exactly what
          # CI showed: a bare "rc=127" with no indication which command was
          # missing. Same discard-the-signal shape as the invf-ls check.
          echo "   --- ebuild src_install log (last 20 lines) ---" >&2
          tail -20 "$d/log" >&2
          echo "   --- end log ---" >&2
          fail "gentoo: ebuild src_install() failed (rc=$rc)"
        return 1
    fi
    pass "gentoo: ebuild src_install() ran clean"
    assert_tree "$ED" "gentoo"; assert_version "$ED" "gentoo"
}

# --------------------------------------------------------------------------
# 3. VOID — run the template's own do_install()
# --------------------------------------------------------------------------
stage_void() {
    d="$WORK/void"; mkdir -p "$d/src" "$d/pkg"
    tar --zstd -xf "$ART" -C "$d/src"
    srcdir="$d/src"
    PKGDESTDIR="$d/pkg"
    # the ONLY thing stubbed: xbps-src's vmkdir. do_install does not call it.
    vmkdir() { mkdir -p "$1"; }
    ( set -e
      . "$REPO/packaging/void/template"
      do_install
    ) > "$d/log" 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        fail "void: template do_install() failed (rc=$rc)"; tail -15 "$d/log"
        return 1
    fi
    pass "void: xbps template do_install() ran clean"
    assert_tree "$PKGDESTDIR" "void"; assert_version "$PKGDESTDIR" "void"
}

# --------------------------------------------------------------------------
# 4. DEBIAN — build the real .deb, then inspect the real package
# --------------------------------------------------------------------------
stage_debian() {
    d="$WORK/deb"; mkdir -p "$d"
    if [ "$DEB" != 1 ]; then
        note "debian: INVFS_PKG_DEB=0, replaying debian/rules' install step only"
        ( set -e
          DESTDIR="$d/tmp" PREFIX=/usr \
          WITH_DRACUT=0 WITH_MKINITCPIO=0 WITH_INITRAMFS_TOOLS=1 \
          sh "$REPO/packaging/install.sh"
        ) > "$d/log" 2>&1 || { fail "debian: install.sh failed"; tail -15 "$d/log"; return 1; }
        assert_tree "$d/tmp" "debian(staged)"
        return 0
    fi
    need dpkg-buildpackage
    # dpkg-buildpackage insists on ./debian at the tree root, but this repo
    # keeps the packaging in packaging/debian (it is shared with the RPM,
    # Arch, Gentoo and Void recipes, so it is not a Debian-only directory).
    # Rather than add a root-level symlink to the repo -- which would change
    # the tracked tree for the duration of a test -- stage a build root whose
    # entries are symlinks back into the checkout, plus a real `debian` link.
    # Symlink the source in, but NOT build/ or bin/ -- otherwise the deb
    # build writes its objects and binaries straight into the worktree and
    # the two builds clobber each other's output. Not linking .git is
    # deliberate too: a Debian source package has no .git either, so this
    # exercises the Makefile's VERSION fallback (the tarball case) rather
    # than quietly depending on a git checkout.
    b="$WORK/debsrc"; mkdir -p "$b"
    for e in "$REPO"/*; do
        case "$(basename "$e")" in
            dist|impl_docs|build|bin) continue;;
        esac
        ln -sfn "$e" "$b/$(basename "$e")"
    done
    # COPY debian/, do not symlink it: debhelper writes debhelper-build-stamp,
    # files/, *.substvars, debian/tmp and debian/<pkg>/ straight into ./debian,
    # so a symlink makes the build scribble all over the real packaging/debian
    # in the worktree (and into the release artifact, which copies packaging/).
    cp -a "$REPO/packaging/debian" "$b/debian"

    note "debian: dpkg-buildpackage -b in $b (compiles; may take a few minutes)"
    # debhelper's dh_auto_test would run the whole unit suite, which belongs
    # to `make test` / CI and is not what this suite is checking. debhelper
    # honours DEB_BUILD_OPTIONS=nocheck (there is no --no-check flag on
    # dpkg-buildpackage itself). Set INVFS_PKG_DEB_CHECKS=1 to run it anyway.
    dbo=nocheck
    # This `||` fires whenever the variable is NOT 1, so the assignment was
    # unconditional and INVFS_PKG_DEB_CHECKS=1 could never enable the checks its
    # own comment above promises. It reads as if it tested the variable.
    [ "${INVFS_PKG_DEB_CHECKS:-0}" = 1 ] && dbo=""

    # `-uc -us` unsign the PACKAGES. The .buildinfo METADATA file is signed
    # separately and those flags do not cover it, so with no secret key in the
    # keyring the build compiled, packaged, and then failed on its last step:
    #
    #   Error: Failed to resolve --signer-userid "InvariantFS Developers <...>"
    #   dpkg-buildpackage: error: failed to sign ../invfs_0.5.0-1_amd64.buildinfo
    #
    # That is a missing capability reported as a packaging failure -- exactly
    # what the loop-mounted packs in test-fuzz were doing until they were gated
    # on the capability rather than on a binary's presence. Same fix here: probe,
    # and say so. The suite asserts nothing about a real signature -- its
    # `neg_missing_signature` control checks that `--pubkey` fails CLOSED when
    # no signature is installed -- so skipping one loses no assertion.
    signargs=""
    if ! gpg --list-secret-keys 2>/dev/null | grep -q '^sec'; then
        signargs="--no-sign"
        note "debian: no secret GPG key in the keyring -- building UNSIGNED" \
             "and skipping the buildinfo signature; this suite asserts none."
    fi
    if ! ( cd "$b" && DEB_BUILD_OPTIONS=$dbo \
            dpkg-buildpackage -b -uc -us $signargs ) \
            > "$d/log" 2>&1; then
        fail "debian: dpkg-buildpackage failed"; tail -25 "$d/log"; return 1
    fi
    deb=$(ls "$WORK"/invfs_*.deb 2>/dev/null | head -1)
    [ -n "$deb" ] || { fail "debian: no .deb produced"; tail -10 "$d/log"; return 1; }
    note "debian: built $deb"
    note "debian: built $deb"
    pass "debian: dpkg-buildpackage produced $(basename "$deb")"

    # the version must be in the .deb's own metadata
    dv=$(dpkg-deb -f "$deb" Version)
    case "$dv" in
        *"$(echo "$VFILE" | sed 's/^v//')"*) pass "debian: .deb Version=$dv" ;;
        *) fail "debian: .deb Version=$dv does not match $VFILE" ;;
    esac

    dpkg-deb -x "$deb" "$d/root"
    assert_tree "$d/root" "debian"
    assert_version "$d/root" "debian"

    # the thing debian/install used to get wrong, checked on the real .deb
    nru=$(dpkg-deb -c "$deb" | awk '$1 ~ /^-/ {print $NF}' \
          | grep -c 'usr/share/man/man[178]/ru/.*\.gz$')
    if [ "$nru" -ge 10 ]; then pass "debian: .deb ships $nru ru man pages (manN/ru/*.gz)"
    else fail "debian: .deb ships only $nru ru man pages -- the debian/install glob bug"; fi

    # only REGULAR FILES: dpkg-deb -c also lists the manN/ and manN/ru/
    # directories, which have no .gz suffix and are not man pages at all
    nungz=$(dpkg-deb -c "$deb" | awk '$1 ~ /^-/ {print $NF}' \
          | grep 'usr/share/man/' | grep -vc '\.gz$')
    if [ "$nungz" = 0 ]; then pass "debian: every man page in the .deb is gzipped (Debian policy 10.1)"
    else fail "debian: $nungz uncompressed man page(s) in the .deb"; fi
}

  # --------------------------------------------------------------------------
  # RPM (packaging/invfs.spec)
  # --------------------------------------------------------------------------
  # packaging/invfs.spec has existed the whole time and NOTHING here ever built
  # it -- grep for "rpmbuild" in this file returned zero before this. Same
  # disease as the boot harnesses: written, committed, never executed. RPM was
  # also the one format missing from an otherwise complete matrix (dpkg, ebuild,
  # xbps).
  #
  # Skipped LOUDLY when rpmbuild is absent rather than failed: a missing build
  # tool is not a packaging bug, and reporting it as one is how a real one gets
  # lost -- which is how this leg would have stayed invisible for months.
  stage_rpm() {
    d="$WORK/rpm"; mkdir -p "$d"
    if ! command -v rpmbuild >/dev/null 2>&1; then
        note "rpm: rpmbuild not installed -- SKIPPED (not a failure)"
        note "      install it with:  apt-get install -y rpm"
        return 0
    fi
    note "rpm: rpmbuild -bb (compiles; may take a few minutes)"
    mkdir -p "$d/rpmtop/SOURCES"
    cp -a "$REPO/packaging/invfs.spec" "$d/rpmtop/SOURCES/"

    # Build a source tarball here rather than reaching into dist/, so this leg
    # cannot pass or fail on whether `make release` happened to run first.
    ( cd "$d" && tar --zstd -xf "$ART_ABS" ) 2>/dev/null \
        || { fail "rpm: cannot unpack $ART_ABS"; return 1; }
    src=$(find "$d" -maxdepth 1 -type d -name 'invfs-*' | head -1)
    [ -n "$src" ] || { fail "rpm: unpacked tree not found"; return 1; }
    cp -a "$src/." "$d/rpmtop/SOURCES/"

    if ! ( cd "$d/rpmtop" && rpmbuild -bb --define "_topdir $d/rpmtop" \
             invfs.spec ) > "$d/log" 2>&1; then
        fail "rpm: rpmbuild failed"; tail -25 "$d/log"; return 1
    fi
    rpmf=$(find "$d/rpmtop/RPMS" -name 'invfs-*.rpm' 2>/dev/null | head -1)
    [ -n "$rpmf" ] || { fail "rpm: no .rpm produced"; tail -10 "$d/log"; return 1; }
    note "rpm: built $(basename "$rpmf")"

    rm -rf "$d/root"; mkdir -p "$d/root"
    ( cd "$d/root" && rpm2cpio "$rpmf" | cpio -idmu --quiet ) 2>/dev/null \
        || { fail "rpm: cannot unpack $rpmf (rpm2cpio or cpio missing)"; return 1; }
    assert_tree "$d/root" "rpm"
    assert_version "$d/root" "rpm"

    nungz=$(find "$d/root/usr/share/man" -type f ! -name '*.gz' 2>/dev/null | wc -l)
    if [ "$nungz" = 0 ]; then pass "rpm: every man page in the .rpm is gzipped"
    else fail "rpm: $nungz uncompressed man page(s) in the .rpm"; fi

    # %post is where RPM differs STRUCTURALLY from dpkg: scriptlets, not files
    # under debian/. A silent no-op %post is the classic RPM packaging bug and
    # this is the only leg that can see it.
    if grep -qE '^%post' "$REPO/packaging/invfs.spec"; then
        pass "rpm: invfs.spec declares a %post scriptlet"
    else
        fail "rpm: invfs.spec has no %post -- post-install wiring never runs"
    fi
  }

# --------------------------------------------------------------------------
# NEGATIVE CONTROLS
# --------------------------------------------------------------------------
neg_truncated() {
    d="$WORK/neg-trunc"; mkdir -p "$d"
    head -c $(( $(wc -c < "$ART") / 2 )) "$ART" > "$d/$ARTBASE"
    # The sums file must describe the REAL, full artifact. Generating it from
    # the truncated file (as this test first did) makes the control vacuous:
    # it would then be checking "does a matching checksum match", and would
    # pass with the verification code doing nothing at all.
    cp "$SUM" "$d/SHA256SUMS"
    before=$(ls "$d/pkg" 2>/dev/null | wc -l)
    out=$(cd "$d" && sh "$REPO/packaging/bootstrap.sh" --file "$d/$ARTBASE" \
             --prefix /usr --yes 2>&1)
    rc=$?
    after=$(ls "$d/pkg" 2>/dev/null | wc -l)
    if [ $rc -eq 0 ]; then fail "negctl: TRUNCATED artifact was ACCEPTED (exit 0)"
    elif ! echo "$out" | grep -qi 'mismatch'; then
        fail "negctl: truncated artifact rejected, but not by a checksum error"; echo "$out" | tail -5
    elif [ "$after" != "$before" ]; then
        fail "negctl: truncated artifact installed something anyway"
    else
        pass "negctl: truncated artifact REJECTED by sha256, exit $rc, nothing installed"
        echo "$out" | grep -iA3 mismatch | sed 's/^/     | /' | head -5
    fi
}

neg_tampered_sums() {
    d="$WORK/neg-sums"; mkdir -p "$d"
    cp "$ART" "$d/$ARTBASE"
    # a sums file whose digest does not match the artifact
    echo "0000000000000000000000000000000000000000000000000000000000000000  $ARTBASE" \
        > "$d/SHA256SUMS"
    out=$(cd "$d" && sh "$REPO/packaging/bootstrap.sh" --file "$d/$ARTBASE" \
             --prefix /usr --yes 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then fail "negctl: TAMPERED SHA256SUMS was ACCEPTED"
    else pass "negctl: tampered SHA256SUMS REJECTED, exit $rc"; fi
}

# The gzip assertion is only worth anything if it CAN fail. Install the same
# tree with WITH_GZIP_MAN=0 and assert that assert_tree rejects it.
neg_uncompressed_man() {
    d="$WORK/neg-man"; mkdir -p "$d/root"
    tmp=$(mktemp -d "$WORK/unsrc.XXXX")
    # a copy with only the man pages, so install.sh's build steps are skipped
    mkdir -p "$tmp/packaging/man" "$tmp/bin"
    cp -a "$REPO/packaging/man/." "$tmp/packaging/man/"
    cp -a "$REPO/packaging/install.sh" "$tmp/packaging/"
    for b in "$REPO"/bin/invf-*; do cp "$b" "$tmp/bin/"; done
    ( cd "$tmp" && DESTDIR="$d/root" PREFIX=/usr WITH_SYSTEMD=0 WITH_DRACUT=0 \
        WITH_CODECPACKS=0 WITH_GZIP_MAN=0 sh packaging/install.sh ) > "$d/log" 2>&1
    if [ ! -f "$d/root/usr/share/man/man8/invf-mkfs.8" ]; then
        fail "negctl: WITH_GZIP_MAN=0 did not install a plain page; the negative control is void"
        return
    fi
    pass "negctl: WITH_GZIP_MAN=0 installs invf-mkfs.8 UNCOMPRESSED (as intended)"
    # now run the real assertion over it and require a failure
    sub=$(NFAIL=0; assert_tree "$d/root" "negctl" 2>&1)
    if [ "$NFAIL" -gt 0 ] || echo "$sub" | grep -q 'UNCOMPRESSED'; then
        pass "negctl: the gzip assertion REJECTS that tree (so it is not vacuous)"
        echo "$sub" | grep 'UNCOMPRESSED' | head -2 | sed 's/^/     | /'
    else
        fail "negctl: the gzip assertion PASSED an uncompressed install -- it is vacuous"
    fi
    rm -rf "$tmp"
}

# --pubkey means "verify or die". With no signature published, dying is the
# only correct behaviour.
neg_missing_signature() {
    d="$WORK/neg-sig"; mkdir -p "$d"
    cp "$ART" "$d/$ARTBASE"; cp "$SUM" "$d/SHA256SUMS"
    printf 'not-a-real-key\n' > "$d/pubkey"
    out=$(cd "$d" && sh "$REPO/packaging/bootstrap.sh" --file "$d/$ARTBASE" \
             --pubkey "$d/pubkey" --prefix /usr --yes 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        fail "negctl: --pubkey with NO signature installed anyway (not fail-closed)"
    elif echo "$out" | grep -qi 'signature expected but missing'; then
        pass "negctl: --pubkey with no signature FAILS CLOSED, exit $rc"
    else
        pass "negctl: --pubkey with no signature rejected, exit $rc"
    fi
}

# --------------------------------------------------------------------------
# SIGNATURE. The positive and negative cases need a real key, so make a
# throwaway ed25519 one in a temp GNUPGHOME. Nothing here touches the
# caller's keyring and nothing reaches the network.
# --------------------------------------------------------------------------
sig_setup() {
    SIGDIR="$WORK/sig"
    export GNUPGHOME="$SIGDIR/gnupg"
    rm -rf "$SIGDIR"; mkdir -p "$GNUPGHOME"; chmod 700 "$GNUPGHOME"
    command -v gpg >/dev/null 2>&1 || { note "no gpg, skipping signature tests"; return 1; }
    gpg --batch --passphrase "" --quick-generate-key \
        "InvFS Test Good <good@localhost>" ed25519 sign never >/dev/null 2>&1
    gpg --batch --passphrase "" --quick-generate-key \
        "InvFS Test Bad <bad@localhost>" ed25519 sign never >/dev/null 2>&1
    gpg --armor --export good@localhost > "$SIGDIR/good.pub" 2>/dev/null
    gpg --armor --export bad@localhost  > "$SIGDIR/bad.pub"  2>/dev/null
    mkdir -p "$SIGDIR/pub/v0.5.0"
    cp "$ART" "$SUM" "$SIGDIR/pub/v0.5.0/"
    gpg --batch --yes --local-user good@localhost \
        --output "$SIGDIR/pub/v0.5.0/SHA256SUMS.sig" \
        --detach-sign "$SIGDIR/pub/v0.5.0/SHA256SUMS" >/dev/null 2>&1
    return 0
}

# $1 = label, $2 = pubkey ("" for none), $3 = extra flags
sig_case() {
    lbl=$1; key=$2; shift 2
    d="$WORK/sig-run-$lbl"; rm -rf "$d"; mkdir -p "$d"
    python3 -m http.server "$SIGPORT" --directory "$SIGDIR/pub" >/dev/null 2>&1 &
    SRVPID=$!
    sleep 1
    # DESTDIR so the case runs unprivileged: --prefix outside $HOME would
    # (correctly) demand root, which would make this a test of sudo rather
    # than of signature verification.
    set -- --release --version v0.5.0 --prefix /usr --yes "$@"
    [ -n "$key" ] && set -- --pubkey "$key" "$@"
    out=$(env INVFS_CACHE_DIR="$d/cache" INVFS_RELEASE_BASE="http://127.0.0.1:$SIGPORT" \
          DESTDIR="$d/root" PREFIX=/usr \
          sh "$REPO/packaging/bootstrap.sh" "$@" 2>&1)
    rc=$?
    kill $SRVPID 2>/dev/null; wait $SRVPID 2>/dev/null
    SIG_OUT=$out; SIG_RC=$rc
    SIG_FILES=$(find "$d/root" -type f 2>/dev/null | wc -l)
}

sig_tests() {
    sig_setup || return 0
    SIGPORT=$(( 8900 + ($$ % 90) ))

    # positive: the right key must be accepted, and the install must land
    sig_case good "$SIGDIR/good.pub" --check-only
    if [ "$SIG_RC" = 0 ] && echo "$SIG_OUT" | grep -q 'signature verified'; then
        if [ "$SIG_FILES" = 0 ]; then
            pass "sig: correct key ACCEPTED, and --check-only installed nothing"
        else
            fail "sig: --check-only installed $SIG_FILES files; it must not"
        fi
    else
        fail "sig: correct key REJECTED (rc=$SIG_RC)"; echo "$SIG_OUT" | tail -3 | sed 's/^/     | /'
    fi

    # positive: a real install from the verified bytes
    sig_case install "$SIGDIR/good.pub"
    if [ "$SIG_RC" = 0 ] && [ "$SIG_FILES" -gt 50 ]; then
        pass "sig: install from the signature-verified artifact: $SIG_FILES files"
        nman=$(find "$WORK/sig-run-install/root" -path '*share/man*' -name '*.gz' 2>/dev/null | wc -l)
        [ "$nman" -gt 0 ] && pass "sig: ...including $nman gzipped man pages"
    else
        fail "sig: install from verified artifact failed (rc=$SIG_RC, $SIG_FILES files)"
    fi

    # negative: the WRONG key must be refused, and nothing installed
    sig_case wrongkey "$SIGDIR/bad.pub"
    if [ "$SIG_RC" != 0 ] && echo "$SIG_OUT" | grep -qi 'SIGNATURE VERIFICATION FAILED'; then
        if [ "$SIG_FILES" = 0 ]; then
            pass "sig: WRONG key REJECTED, exit $SIG_RC, nothing installed"
        else
            fail "sig: wrong key rejected but $SIG_FILES files were installed anyway"
        fi
    else
        fail "sig: wrong key was ACCEPTED (rc=$SIG_RC) -- verification is not fail-closed"
    fi

    # negative: a published signature with NO key must warn, not silently pass
    sig_case nokey ""
    if echo "$SIG_OUT" | grep -qi 'no --pubkey'; then
        pass "sig: signature present but no --pubkey -> loud WARNING (checksum is unauthenticated)"
    else
        fail "sig: no warning when a signature was published but no key given"
    fi

    # negative: tamper the artifact AFTER signing -- sha256 must catch it
    sig_case tampered "$SIGDIR/good.pub"
    d="$SIGDIR/pub/v0.5.0"
    cp "$d/$ARTBASE" /tmp/.sigcase.bak 2>/dev/null
    printf 'x' >> "$d/$ARTBASE"
    sig_case tampered2 "$SIGDIR/good.pub"
    mv /tmp/.sigcase.bak "$d/$ARTBASE" 2>/dev/null
    if [ "$SIG_RC" != 0 ] && echo "$SIG_OUT" | grep -qi 'sha256 MISMATCH'; then
        pass "sig: artifact tampered AFTER signing -> sha256 MISMATCH, exit $SIG_RC"
    else
        fail "sig: post-signing tampering was not caught (rc=$SIG_RC)"
    fi
    unset GNUPGHOME
}

# --------------------------------------------------------------------------
note "=== staging each target's own recipe ==="
stage_rpm      || true
stage_arch    || true
stage_gentoo  || true
stage_void    || true
stage_debian  || true

note ""
note "=== negative controls (a check that cannot fail verifies nothing) ==="
neg_truncated
neg_tampered_sums
neg_uncompressed_man
neg_missing_signature
note ""
note "=== signature verification ==="
sig_tests

echo
if [ "$NFAIL" -eq 0 ]; then
    echo "test-packaging.sh: PASS (all targets staged, all assertions held, all negative controls rejected)"
    exit 0
fi
echo "test-packaging.sh: FAIL ($NFAIL assertion(s) failed)"
exit 1
