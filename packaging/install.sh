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
#     bin/invf-*                  -> $PREFIX/bin
#     tools/codecpacks/*.codecpack-> $PREFIX/lib/invfs/codecpacks
#                                    (C helpers rebuilt from source when
#                                     $CC is available)
#     packaging/man/*.[78]        -> $PREFIX/share/man/manN
#     packaging/systemd/*         -> $PREFIX/lib/systemd/system
#     packaging/dracut/90invfs    -> $PREFIX/lib/dracut/modules.d/90invfs
#     packaging/mkinitcpio/*      -> $PREFIX/lib/initcpio/{install,hooks}/invfs
#     packaging/debian/invfs.initramfs-* -> /usr/share/initramfs-tools/...
#
# Knobs (env): PREFIX DESTDIR CC WITH_SYSTEMD=1 WITH_DRACUT=1
#              WITH_MKINITCPIO=0 WITH_INITRAMFS_TOOLS=0
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

BINDIR=$PREFIX/bin
LIBDIR=$PREFIX/lib/invfs
MANDIR=$PREFIX/share/man
SYSTEMDDIR=$PREFIX/lib/systemd/system
DRACUTDIR=$PREFIX/lib/dracut/modules.d/90invfs

have() { command -v "$1" >/dev/null 2>&1; }

# ---- CLI tools -------------------------------------------------------------
ls "$ROOT"/bin/invf-* >/dev/null 2>&1 || {
    echo "install.sh: no bin/invf-* -- run 'make' first" >&2
    exit 1
}
install -dm755 "$DESTDIR$BINDIR"
for b in "$ROOT"/bin/invf-*; do
    [ -f "$b" ] || continue
    install -m755 "$b" "$DESTDIR$BINDIR/"
done

# ---- codecpacks ------------------------------------------------------------
# Copy each pack verbatim, then rebuild every C helper (<pack>/<name>.c ->
# <pack>/bin/<name>) from source when a compiler is available. Without a
# compiler the committed prebuilt helper (if any) is shipped as-is.
if [ "$WITH_CODECPACKS" != "0" ]; then
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
            if have "$CC"; then
                mkdir -p "$dst/bin"
                "$CC" -std=c11 -O2 -Wall -Wextra -o "$dst/bin/$helper" "$src"
                chmod 755 "$dst/bin/$helper"
            elif [ ! -x "$dst/bin/$helper" ]; then
                # no compiler and no committed prebuilt helper: the pack
                # ships without its tool and will probe absent at runtime
                # (its content waits RAW; the builtin codecs still work)
                echo "install.sh: WARNING: $name: cannot build helper '$helper' (no $CC) and no prebuilt bin/$helper -- pack will probe absent" >&2
            fi
        done
    done
fi

# ---- man pages -------------------------------------------------------------
for sec in 1 7 8; do
    for m in "$ROOT"/packaging/man/*."$sec"; do
        [ -f "$m" ] || continue
        install -Dm644 "$m" "$DESTDIR$MANDIR/man${sec}/${m##*/}"
    done
done
# Russian translations
if [ -d "$ROOT/packaging/man/ru" ]; then
    for sec in 1 7 8; do
        for m in "$ROOT"/packaging/man/ru/*."$sec"; do
            [ -f "$m" ] || continue
            install -Dm644 "$m" "$DESTDIR$MANDIR/man${sec}/ru/${m##*/}"
        done
    done
fi

# ---- systemd units (off by default: nothing enables the timers) ------------
if [ "$WITH_SYSTEMD" = "1" ]; then
    install -dm755 "$DESTDIR$SYSTEMDDIR"
    for u in "$ROOT"/packaging/systemd/*; do
        [ -f "$u" ] || continue
        install -m644 "$u" "$DESTDIR$SYSTEMDDIR/"
    done
fi

# ---- initramfs integrations --------------------------------------------------
if [ "$WITH_DRACUT" = "1" ]; then
    install -dm755 "$DESTDIR$DRACUTDIR"
    install -m755 "$ROOT"/packaging/dracut/90invfs/module-setup.sh \
        "$DESTDIR$DRACUTDIR/module-setup.sh"
    install -m755 "$ROOT"/packaging/dracut/90invfs/invfs-mount.sh \
        "$DESTDIR$DRACUTDIR/invfs-mount.sh"
fi
if [ "$WITH_MKINITCPIO" = "1" ]; then
    install -Dm644 "$ROOT"/packaging/mkinitcpio/invfs_install \
        "$DESTDIR$PREFIX/lib/initcpio/install/invfs"
    install -Dm644 "$ROOT"/packaging/mkinitcpio/invfs_hook \
        "$DESTDIR$PREFIX/lib/initcpio/hooks/invfs"
fi
if [ "$WITH_INITRAMFS_TOOLS" = "1" ]; then
    install -Dm755 "$ROOT"/packaging/debian/invfs.initramfs-hook \
        "$DESTDIR$PREFIX/share/initramfs-tools/hooks/invfs"
    install -Dm755 "$ROOT"/packaging/debian/invfs.initramfs-script \
        "$DESTDIR$PREFIX/share/initramfs-tools/scripts/local-top/invfs"
fi

echo "install.sh: installed under $DESTDIR$PREFIX"
