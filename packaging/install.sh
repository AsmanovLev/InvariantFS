#!/bin/sh
# packaging/install.sh — DESTDIR-aware local install of InvariantFS.
#
# Unpackaged use:
#     make && sudo sh packaging/install.sh
#     sh packaging/install.sh            # respects PREFIX (default /usr)
#     DESTDIR=/tmp/stage sh packaging/install.sh   # staged install
#
# The rpm/debian/arch packaging calls this same script, so the FHS layout
# lives in exactly one place:
#     bin/invf*                   -> $PREFIX/bin   (NOT invf-* -- that glob
#                                    silently drops invfs-pack, the codecpack
#                                    manager, from every package)
#     tools/codecpacks/*.codecpack-> $PREFIX/lib/invfs/codecpacks
#                                    (C helpers rebuilt from source when
#                                     $CC is available)
#     packaging/man/*.[178]       -> $PREFIX/share/man/manN/<name>.<sec>.gz
#     packaging/man/ru/*.[178]    -> $PREFIX/share/man/manN/ru/<name>.<sec>.gz
#     packaging/systemd/*         -> $PREFIX/lib/systemd/system
#     packaging/dracut/90invfs    -> $PREFIX/lib/dracut/modules.d/90invfs
#     packaging/mkinitcpio/*      -> $PREFIX/lib/initcpio/{install,hooks}/invfs
#     packaging/debian/invfs.initramfs-* -> /usr/share/initramfs-tools/...
#
# Man pages are installed GZIPPED (WITH_GZIP_MAN=1, the default): Debian
# policy 10.1 requires it, and man(1) on Arch/Gentoo/Void resolves
# manN/<name>.<sec>.gz before the plain form. Set WITH_GZIP_MAN=0 to
# install the roff sources verbatim. `gzip -n` is used so a rebuild of the
# same input is byte-identical.
#
# Knobs (env): PREFIX DESTDIR CC WITH_SYSTEMD=1 WITH_DRACUT=1
#              WITH_MKINITCPIO=0 WITH_INITRAMFS_TOOLS=0 WITH_GZIP_MAN=1
#
# Options:
#   --manifest <file>   append every installed path to <file> (one per line);
#                       used by packaging/bootstrap.sh for --uninstall.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
PREFIX=${PREFIX:-/usr}
DESTDIR=${DESTDIR:-}
CC=${CC:-cc}
WITH_SYSTEMD=${WITH_SYSTEMD:-1}
WITH_DRACUT=${WITH_DRACUT:-1}
WITH_MKINITCPIO=${WITH_MKINITCPIO:-0}
WITH_INITRAMFS_TOOLS=${WITH_INITRAMFS_TOOLS:-0}
WITH_CODECPACKS=${WITH_CODECPACKS:-1}
WITH_GZIP_MAN=${WITH_GZIP_MAN:-1}
MANIFEST=

while [ $# -gt 0 ]; do
    case "$1" in
        --manifest) MANIFEST=${2:?--manifest needs an argument}; shift 2;;
        --manifest=*) MANIFEST=${1#*=}; shift;;
        -h|--help)
            echo "usage: install.sh [--manifest <file>]"
            exit 0;;
        *) echo "install.sh: unknown option: $1" >&2; exit 2;;
    esac
done

# record an installed path (with DESTDIR prefix) for bootstrap --uninstall
record() {
    [ -n "$MANIFEST" ] || return 0
    printf '%s\n' "$1" >> "$MANIFEST"
}
record_tree() {
    [ -n "$MANIFEST" ] || return 0
    [ -d "$1" ] || return 0
    find "$1" \( -type f -o -type l \) -print >> "$MANIFEST"
}

BINDIR=$PREFIX/bin
SBINDIR=$PREFIX/sbin
LIBDIR=$PREFIX/lib/invfs
MANDIR=$PREFIX/share/man
SYSTEMDDIR=$PREFIX/lib/systemd/system
DRACUTDIR=$PREFIX/lib/dracut/modules.d/90invfs

have() { command -v "$1" >/dev/null 2>&1; }

if [ -n "$MANIFEST" ]; then
    mkdir -p "$(dirname "$MANIFEST")"
    : > "$MANIFEST"
fi

# ---- CLI tools -------------------------------------------------------------
ls "$ROOT"/bin/invf* >/dev/null 2>&1 || {
    echo "install.sh: no bin/invf* -- run 'make' first" >&2
    exit 1
}
install -dm755 "$DESTDIR$BINDIR"
for b in "$ROOT"/bin/invf*; do
    [ -f "$b" ] || continue
    install -m755 "$b" "$DESTDIR$BINDIR/"
    record "$DESTDIR$BINDIR/${b##*/}"
done

# ---- /sbin front-ends (fstype invfs) --------------------------------------
# mkfs.invfs / fsck.invfs / mount.invfs: the names mkfs(8), fsck(8) and
# mount(8) dispatch on. Thin POSIX-sh shims over the invf-* tools above.
install -dm755 "$DESTDIR$SBINDIR"
for s in "$ROOT"/tools/sbin/*; do
    [ -f "$s" ] || continue
    install -m755 "$s" "$DESTDIR$SBINDIR/"
    record "$DESTDIR$SBINDIR/${s##*/}"
done

# ---- codecpacks ------------------------------------------------------------
# Copy each pack verbatim, then ship the C helper the Makefile already built
# (<pack>/bin/<name>).
#
# WP114: this used to compile each pack's helper here with a hand-rolled
# `"$CC" -std=c11 -O2 <pack>.c` line. That is a SECOND, wrong copy of the
# build recipe: a pack may need extra objects and extra -I paths (qcow2
# needs src/codecs + src/zlib + -DZ_PREFIX and the deflate_repro objects),
# and those live in the Makefile as PLUGIN_EXTRA_<pack>/HELPER_CFLAGS_<pack>.
# The naive line therefore failed to link qcow2, `set -eu` aborted the whole
# install at exit 1 -- BEFORE the man pages, before systemd, before
# anything -- so no package built on any host that has a C compiler. The
# fix is not to re-derive the flags here but to stop duplicating the build:
# `make helpers` is the single source of truth (it reads each pack's own
# manifest for its link libraries, WP113), and this script only ships what
# it produced. If the helpers are missing we try `make helpers` once, and
# if that is not possible we warn loudly rather than silently shipping a
# pack that cannot do its job.
if [ "$WITH_CODECPACKS" != "0" ]; then
    # One build attempt for the whole tree, only if something is missing.
    need_helpers=
    for pack in "$ROOT"/tools/codecpacks/*.codecpack; do
        [ -d "$pack" ] || continue
        for src in "$pack"/*.c; do
            [ -f "$src" ] || continue
            h=${src##*/}; h=${h%.c}
            [ -x "$pack/bin/$h" ] || need_helpers=1
        done
    done
    if [ -n "$need_helpers" ] && have make && [ -f "$ROOT/Makefile" ]; then
        info="install.sh: building codecpack helpers (make helpers)"
        echo "$info" >&2
        make -C "$ROOT" helpers >&2 || \
            echo "install.sh: WARNING: 'make helpers' failed; shipping packs without helpers" >&2
    fi

    install -dm755 "$DESTDIR$LIBDIR/codecpacks"
    for pack in "$ROOT"/tools/codecpacks/*.codecpack; do
        [ -d "$pack" ] || continue
        name=${pack##*/}
        dst=$DESTDIR$LIBDIR/codecpacks/$name
        rm -rf "$dst"
        cp -a "$pack" "$dst"
        for src in "$pack"/*.c; do
            [ -f "$src" ] || continue
            helper=${src##*/}; helper=${helper%.c}
            if [ -x "$pack/bin/$helper" ]; then
                mkdir -p "$dst/bin"
                install -m755 "$pack/bin/$helper" "$dst/bin/$helper"
            else
                # No helper to ship: the pack probes absent at runtime (its
                # content waits RAW; the builtin codecs still work), so this
                # is a degraded package, not a broken one -- but say so.
                echo "install.sh: WARNING: $name: no built helper '$helper' (run 'make helpers') -- pack will probe absent" >&2
                rm -f "$dst/bin/$helper"
            fi
        done
        record_tree "$dst"
    done
fi

# ---- man pages -------------------------------------------------------------
# install_man <src> <dest-without-.gz>
#
# With WITH_GZIP_MAN=1 (default) the page lands as <dest>.gz, compressed
# with `gzip -n` so the bytes are reproducible for a given input. The
# manifest always records the path that was ACTUALLY installed, so
# bootstrap.sh --uninstall does not leave a stale .gz behind (or try to
# remove a file that was never created).
install_man() {
    _src=$1
    _dst=$2
    if [ "$WITH_GZIP_MAN" = "1" ]; then
        if ! have gzip; then
            echo "install.sh: ERROR: WITH_GZIP_MAN=1 but gzip(1) not found" >&2
            exit 1
        fi
        _gz=$DESTDIR$_dst.gz
        install -Dm644 "$_src" "$_gz.tmp"
        gzip -9 -n -f "$_gz.tmp"
        mv -f "$_gz.tmp.gz" "$_gz"
        record "$_gz"
    else
        install -Dm644 "$_src" "$DESTDIR$_dst"
        record "$DESTDIR$_dst"
    fi
}

for sec in 1 7 8; do
    for m in "$ROOT"/packaging/man/*."$sec"; do
        [ -f "$m" ] || continue
        install_man "$m" "$MANDIR/man${sec}/${m##*/}"
    done
done
# Russian translations
if [ -d "$ROOT/packaging/man/ru" ]; then
    for sec in 1 7 8; do
        for m in "$ROOT"/packaging/man/ru/*."$sec"; do
            [ -f "$m" ] || continue
            install_man "$m" "$MANDIR/man${sec}/ru/${m##*/}"
        done
    done
fi

# ---- systemd units (off by default: nothing enables the timers) ------------
if [ "$WITH_SYSTEMD" = "1" ]; then
    install -dm755 "$DESTDIR$SYSTEMDDIR"
    for u in "$ROOT"/packaging/systemd/*; do
        [ -f "$u" ] || continue
        install -m644 "$u" "$DESTDIR$SYSTEMDDIR/"
        record "$DESTDIR$SYSTEMDDIR/${u##*/}"
    done
fi

# ---- initramfs integrations --------------------------------------------------
if [ "$WITH_DRACUT" = "1" ]; then
    install -dm755 "$DESTDIR$DRACUTDIR"
    install -m755 "$ROOT"/packaging/dracut/90invfs/module-setup.sh \
        "$DESTDIR$DRACUTDIR/module-setup.sh"
    record "$DESTDIR$DRACUTDIR/module-setup.sh"
    install -m755 "$ROOT"/packaging/dracut/90invfs/invfs-mount.sh \
        "$DESTDIR$DRACUTDIR/invfs-mount.sh"
    record "$DESTDIR$DRACUTDIR/invfs-mount.sh"
fi
if [ "$WITH_MKINITCPIO" = "1" ]; then
    install -Dm644 "$ROOT"/packaging/mkinitcpio/invfs_install \
        "$DESTDIR$PREFIX/lib/initcpio/install/invfs"
    record "$DESTDIR$PREFIX/lib/initcpio/install/invfs"
    install -Dm644 "$ROOT"/packaging/mkinitcpio/invfs_hook \
        "$DESTDIR$PREFIX/lib/initcpio/hooks/invfs"
    record "$DESTDIR$PREFIX/lib/initcpio/hooks/invfs"
fi
if [ "$WITH_INITRAMFS_TOOLS" = "1" ]; then
    install -Dm755 "$ROOT"/packaging/debian/invfs.initramfs-hook \
        "$DESTDIR$PREFIX/share/initramfs-tools/hooks/invfs"
    record "$DESTDIR$PREFIX/share/initramfs-tools/hooks/invfs"
    install -Dm755 "$ROOT"/packaging/debian/invfs.initramfs-script \
        "$DESTDIR$PREFIX/share/initramfs-tools/scripts/local-top/invfs"
    record "$DESTDIR$PREFIX/share/initramfs-tools/scripts/local-top/invfs"
fi

echo "install.sh: installed under $DESTDIR$PREFIX"
