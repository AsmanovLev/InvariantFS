# the engine Makefile builds -O2 without -g; there is no debuginfo to split
%global debug_package %{nil}

Name:           invfs
Version:        0.2.0
Release:        1%{?dist}
Summary:        Semantic content-addressed filesystem with a bit-exactness invariant

License:        GPLv2
URL:            https://github.com/AsmanovLev/InvariantFS
Source0:        %{name}-%{version}.tar.gz

# the engine is C11 + bundled zstd/lz4/miniz/blake3/flacx; the FUSE daemon
# needs libfuse3; the Makefile links the zstd/z shared libraries directly
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconf-pkg-config
BuildRequires:  fuse3-devel
BuildRequires:  zlib-devel
BuildRequires:  libzstd
BuildRequires:  systemd-rpm-macros

Requires:       fuse3
# cjxl/djxl power the jxl + raw_image codecpacks; absent tools simply
# defer JPEG/RAW-camera content to generic storage (weak dep)
Recommends:     libjxl-utils

%description
InvariantFS is a FUSE filesystem with a bit-exactness invariant: every
compression is verified by decompress-and-compare before it is trusted;
containers (ZIP/TAR, disk images) are decomposed into member inodes that
flow through the whole codec pipeline recursively. Ships the CLI tools,
the FUSE daemon, the system codecpacks, systemd sweep/verify timers
(disabled by default) and a dracut module for booting from an
InvariantFS root (rootfstype=invfs).

%prep
%autosetup

%build
mkdir -p bin
%make_build

%install
# packaging/install.sh is the single source of truth for the FHS layout
# (shared with the debian/arch packaging and unpackaged installs)
DESTDIR=%{buildroot} PREFIX=%{_prefix} \
    WITH_SYSTEMD=1 WITH_DRACUT=1 WITH_MKINITCPIO=0 WITH_INITRAMFS_TOOLS=0 \
    sh packaging/install.sh
install -Dm644 README.md %{buildroot}%{_docdir}/%{name}/README.md
install -Dm644 README-RU.md %{buildroot}%{_docdir}/%{name}/README-RU.md

%post
%systemd_post invfs-sweep.timer invfs-verify.timer

%preun
%systemd_preun invfs-sweep.timer invfs-verify.timer

%postun
%systemd_postun_with_reload invfs-sweep.timer invfs-verify.timer

%files
%license src-extracted/VFS/LICENSE
%doc README.md README-RU.md
%{_bindir}/invf-*
/usr/lib/invfs/codecpacks/
%{_mandir}/man7/invarifs.7*
%{_mandir}/man1/invf-*.1*
%{_mandir}/man8/invf-*.8*
%{_mandir}/man*/ru/invf-*.8*
%{_mandir}/man*/ru/invf-*.1*
%{_mandir}/man*/ru/invarifs.7*
%{_unitdir}/invfs-sweep.service
%{_unitdir}/invfs-sweep.timer
%{_unitdir}/invfs-verify.service
%{_unitdir}/invfs-verify.timer
/usr/lib/dracut/modules.d/90invfs/

%changelog
* Sun Sep 13 2026 InvariantFS Developers <invfs@localhost> - 0.2.0-1
- Legacy cleanup: removed 36 dead Windows-only files from legacy/
- New man pages: invf-stats, invf-import, invf-migrate-v2, invf-ls (read-side)
- Russian man page translations
- Gentoo install guide (docs/GENTOO-INSTALL.md)
- Packaging polish: debian/copyright, Homepage URLs, Architecture: any
- configure-guest.sh bug fix ($MNT -> $M)

* Fri Sep 04 2026 InvariantFS Developers <invfs@localhost> - 0.1.0-1
- Initial packaging (WP-PKG): CLI tools, FUSE daemon, system codecpacks,
  man pages, systemd sweep/verify timers (disabled by default), and a
  dracut module (90invfs) for root-on-InvariantFS boots.
