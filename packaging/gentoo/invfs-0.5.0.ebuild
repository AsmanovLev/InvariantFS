# Copyright 2026 InvariantFS Developers
# Distributed under the terms of the GNU General Public License v2

EAPI=8

# Consumes the PUBLISHED RELEASE ARTIFACT (see docs/INSTALL-MATRIX.md).
# Gentoo already has the build deps installed by the ebuild; the artifact
# ships prebuilt bin/invf-* and the codecpack helpers, so this ebuild
# compiles nothing and only drives packaging/install.sh -- the one place
# the FHS layout is defined, shared with the Arch/Debian/Void packaging.
#
# AUR-style local install:
#     mkdir -p /var/db/repos/local/invfs
#     cp invfs-0.5.0.ebuild /var/db/repos/local/invfs/
#     echo 'sys-apps/invfs' >> /etc/portage/package.accept_keywords
#     echo 'sys-apps/invfs **' >> /etc/portage/package.mask   # only if needed
#     emerge --ask sys-apps/invfs
#
# Checksum provenance: BLAKE2B digest below is the sha256 line from the
# published dist/SHA256SUMS, re-expressed as blake2b (what portage wants).
# `make release-sums` prints the pair. The published SHA256SUMS is
# detach-signed (ed25519) by `make release SIGNING_KEY=...`; portage cannot
# verify that signature itself, so the chain is:
#     pinned BLAKE2B -> dist/SHA256SUMS -> SHA256SUMS.sig -> author pubkey
# the same chain packaging/bootstrap.sh --check-only --pubkey checks.

MY_PN="invfs-v${PV}-x86_64"
# The tarball unpacks to WORKDIR/${MY_PN}, not WORKDIR/${P}.
S="${WORKDIR}"

# Pinned digest of the release artifact, as portage wants it
# (`make release-sums` prints the pair; re-pin on every release).
BLAKE2B="610ade57e603b4e005948642ab9ec1ce4342603fc983167592284dd295451d44"
SIZE="1383575"

DESCRIPTION="Semantic content-addressed filesystem with a bit-exactness invariant"
HOMEPAGE="https://github.com/AsmanovLev/InvariantFS"
SRC_URI="
	https://github.com/AsmanovLev/InvariantFS/releases/download/v${PV}/${MY_PN}.tar.zst
	https://github.com/AsmanovLev/InvariantFS/releases/download/v${PV}/SHA256SUMS
"

LICENSE="GPL-2"
SLOT="0"
KEYWORDS="~amd64 ~x86"

# The artifact is prebuilt: nothing is compiled here, so there are no
# BUILD_DEPENDS. The RDEPENDs below are what the installed binaries and the
# codecpack helpers actually dlopen/exec at runtime.
RDEPEND="
	app-arch/zstd
	sys-libs/zlib
	sys-fs/fuse:3
"
# jpeg -> lossless JXL is a codecpack; without cjxl/djxl the pack probes
# absent and the content waits RAW (the builtin codecs still work). Weak,
# matching the RPM/Debian `Recommends`/`optdepends` treatment.
# (No x264 dep: nothing in the tree consumes it. The lossless-H.264 lane
# was measured and shelved; if it ever ships, its atom goes here as
# media-video/x264-encoder, not the nonexistent media-video/x264-utils.)
RDEPEND+=" media-libs/libjpeg-turbo media-libs/libjxl"
# 7zip powers the 7z containerpack (app-arch/p7zip was masked 2026-08,
# replaced by app-arch/7zip).
RDEPEND+=" app-arch/7zip"

# Nothing to build: the artifact ships the compiled engine.
src_compile() {
	: # prebuilt
}

src_install() {
	cd "${S}/${MY_PN}" || die "artifact did not unpack to ${MY_PN}"

	local -x DESTDIR="${ED}"
	local -x PREFIX=/usr
	# Gentoo's initramfs generator is dracut; the mkinitcpio hook is Arch's.
	local -x WITH_SYSTEMD=1
	local -x WITH_DRACUT=1
	local -x WITH_MKINITCPIO=0
	local -x WITH_INITRAMFS_TOOLS=0
	sh packaging/install.sh || die "packaging/install.sh failed"

	# Timers stay opt-in: Gentoo's systemd convention is to ship them
	# disabled and let the admin run `systemctl enable --now invfs-sweep.timer`.
	systemd_dounit invfs-sweep.service invfs-sweep.timer \
	               invfs-verify.service invfs-verify.timer
}

pkg_postinst() {
	elog "InvariantFS ${PV} installed. The sweep/verify timers are DISABLED."
	elog "Enable the periodic sweep with:  systemctl enable --now invfs-sweep.timer"
	elog "Verify a volume offline with:     invf-verify --deep <volume.img>"
}

pkg_prerm() {
	systemd_dounit invfs-sweep.service invfs-sweep.timer \
	               invfs-verify.service invfs-verify.timer
}
