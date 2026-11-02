#!/bin/sh
set -eu
TAG=${1:-${INVFS_VERSION:-}}
[ -n "$TAG" ] || { echo "stamp-version: usage: stamp-version.sh <vX.Y.Z>"; exit 1; }
VER="${TAG#v}"

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

# 1. invarifs.h
if [ -f src/core/invarifs.h ]; then
    sed -i "s/#define INVFS_VERSION_STRING .*/#define INVFS_VERSION_STRING \"v$VER\"/" src/core/invarifs.h
fi

# 2. PKGBUILD
if [ -f packaging/PKGBUILD ]; then
    sed -i "s/^pkgver=.*/pkgver=$VER/" packaging/PKGBUILD
fi

# 3. invfs.spec
if [ -f packaging/invfs.spec ]; then
    sed -i "s/^Version:.*/Version:        $VER/" packaging/invfs.spec
fi

# 4. void/template
if [ -f packaging/void/template ]; then
    sed -i "s/^version=.*/version=\${INVFS_TEMPLATE_VERSION:-$VER}/" packaging/void/template
fi

# 5. gentoo ebuild
if [ -d packaging/gentoo ]; then
    target="packaging/gentoo/invfs-$VER.ebuild"
    if [ ! -f "$target" ]; then
        latest_ebuild=$(ls -t packaging/gentoo/invfs-*.ebuild 2>/dev/null | head -1 || true)
        if [ -n "$latest_ebuild" ] && [ -f "$latest_ebuild" ]; then
            cp "$latest_ebuild" "$target"
        fi
    fi
fi

# 6. debian/changelog
if [ -f packaging/debian/changelog ]; then
    if ! head -n 1 packaging/debian/changelog | grep -q "($VER-"; then
        date_str=$(date -R 2>/dev/null || date)
        tmpfile=$(mktemp)
        printf 'invfs (%s-1) unstable; urgency=medium\n\n  * Release %s\n\n -- InvariantFS Developers <invfs@localhost>  %s\n\n' "$VER" "$TAG" "$date_str" > "$tmpfile"
        cat packaging/debian/changelog >> "$tmpfile"
        mv "$tmpfile" packaging/debian/changelog
    fi
fi

echo "stamp-version: stamped version $VER ($TAG) successfully"
