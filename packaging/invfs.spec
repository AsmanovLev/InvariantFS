# invfs.spec — RPM for InvariantFS (Fedora/RHEL).
#
# Consumes the PUBLISHED RELEASE ARTIFACT (see docs/INSTALL-MATRIX.md),
# same as the Arch PKGBUILD and the Void template: prebuilt bin/invf-*
# plus codecpacks with helpers already built, so %build is empty and
# packaging/install.sh — the one place the FHS layout is defined — does
# the whole %install. No file list to drift out of sync beyond the globs
# below, which mirror install.sh's header.
#
# Build (inside the release workflow; Source0 is the tarball job artifact):
#   rpmbuild -bb packaging/invfs.spec --define "_topdir $PWD/rpmbuild" ...
#Update Version: on every release (release.yml preflight enforces it).

Name:           invfs
Version:        0.6.0
Release:        1%{?dist}
Summary:        Content-addressed filesystem with a bit-exactness invariant
License:        GPL-2.0-only
URL:            https://github.com/AsmanovLev/InvariantFS
Source0:        invfs-v%{version}-x86_64.tar.zst
BuildArch:      x86_64

# Runtime only: the artifact ships compiled.
Requires:       fuse3
Requires:       zlib
Requires:       zstd
# Script-based codecpacks (raw_image) and container helpers.
Recommends:     python3
Recommends:     p7zip

%description
InvariantFS is a FUSE-based, single-host, content-addressed filesystem
built around one guarantee: what you write comes back byte-identical.
Meta-v3 on-disk format (B+ tree base + append-only Delta Log),
content-addressed deduplicated segments, per-content codecs, container
windowing, savepoints with rollback, and native Reed-Solomon seal parity.

%prep
%setup -q -n invfs-v%{version}-x86_64

%build
# Prebuilt artifact: nothing to compile.

%install
# Fedora: systemd units on, dracut on (Gentoo/Fedora hook), mkinitcpio and
# initramfs-tools off (Arch/Debian hooks).
DESTDIR=%{buildroot} PREFIX=/usr \
WITH_SYSTEMD=1 WITH_DRACUT=1 WITH_MKINITCPIO=0 WITH_INITRAMFS_TOOLS=0 \
sh packaging/install.sh

%files
/usr/bin/invf*
/usr/bin/invfs-pack
/usr/sbin/mount.invfs
/usr/sbin/fsck.invfs
/usr/sbin/mkfs.invfs
/usr/lib/invfs/
/usr/lib/systemd/system/invfs-*
/usr/lib/dracut/modules.d/90invfs/
/usr/share/man/man1/invf-ls.1.gz
/usr/share/man/man7/invarifs.7.gz
/usr/share/man/man8/invf-*.8.gz
/usr/share/man/man8/mount.invfs.8.gz
/usr/share/man/man8/fsck.invfs.8.gz
/usr/share/man/man8/mkfs.invfs.8.gz
/usr/share/man/man1/ru/invf-ls.1.gz
/usr/share/man/man7/ru/invarifs.7.gz
/usr/share/man/man8/ru/invf-*.8.gz
/usr/share/man/man8/ru/mount.invfs.8.gz
/usr/share/man/man8/ru/fsck.invfs.8.gz
/usr/share/man/man8/ru/mkfs.invfs.8.gz

%changelog
* Sun Oct 12 2026 InvariantFS <invfs@invariantfs.local> - 0.6.0-1
- Crash-consistency hardening release (Meta-v3, durability contract, SPT0,
  native seal parity). First RPM.
