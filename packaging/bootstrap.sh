#!/bin/sh
# packaging/bootstrap.sh — host bootstrap installer for InvariantFS.
#
# Intended to be run straight off the wire:
#
#     curl -fsSL https://github.com/AsmanovLev/InvariantFS/releases/latest/download/bootstrap.sh | sh -s -- --yes
#     curl -fsSL .../bootstrap.sh | sudo sh -s -- --prefix /usr/local
#
# The FHS layout lives in packaging/install.sh (single source of truth).
# This script only: resolves a release/source, downloads, VERIFIES SHA256,
# unpacks, and invokes install.sh with a manifest for later --uninstall.
#
# POSIX sh only. No silent sudo: if root is needed and we are not root,
# print the sudo invocation and exit.
#
# Flags:
#   --version <tag|latest>   pin a release tag (default: latest)
#   --prefix <dir>           install prefix (default: /usr/local)
#   --release | --source     force an artifact kind
#                            (default: release when release assets exist,
#                             else source)
#   --file <tarball>         use a local artifact (offline / inspect-before-run);
#                            release artifact (*.tar.zst) or source tarball
#   --download-only          fetch + verify + stage only; do not install
#   --run                    execute the staged --file / cached artifact
#   --no-systemd             skip systemd units
#   --no-dracut              skip the dracut module
#   --no-mkinitcpio          skip the mkinitcpio hook (already default off)
#   --no-initramfs-tools     skip the initramfs-tools hook (already default off)
#   --dry-run                print the plan; touch nothing
#   --uninstall              remove everything the manifest records
#   --list                   list the manifest
#   --yes                    assume yes (non-interactive)
#   -h | --help
#
# Environment:
#   PREFIX / DESTDIR          as in install.sh (DESTDIR enables staged install)
#   INVFS_REPO                GitHub owner/repo (default AsmanovLev/InvariantFS)
#   INVFS_SOURCE_DIR          local source checkout for --source
#   INVFS_CACHE_DIR           download cache (default ~/.cache/invfs)
#   INVFS_SKIP_MAKE=1         do not rebuild in source mode
#   INVFS_OS_RELEASE          override /etc/os-release path (testing)
#   INVFS_RELEASE_BASE        override the release download base URL
set -eu

PROG=bootstrap.sh
ORIG_ARGS=$*
REPO=${INVFS_REPO:-AsmanovLev/InvariantFS}
PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}
VERSION=latest
MODE=auto
FILE=
DRY_RUN=0
UNINSTALL=0
LIST=0
ASSUME_YES=0
DOWNLOAD_ONLY=0
RUN=0
WITH_SYSTEMD=1
WITH_DRACUT=1
WITH_MKINITCPIO=0
WITH_INITRAMFS_TOOLS=0
ARCH_OVERRIDE=${INVFS_ARCH:-}

# ---- logging ---------------------------------------------------------------
info() { printf '%s: %s\n' "$PROG" "$*" >&2; }
warn() { printf '%s: WARNING: %s\n' "$PROG" "$*" >&2; }
die()  { printf '%s: ERROR: %s\n' "$PROG" "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

usage() {
    sed -n '2,40p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//' || true
}

# ---- args ------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION=${2:?--version needs an argument}; shift 2;;
        --version=*) VERSION=${1#*=}; shift;;
        --prefix) PREFIX=${2:?--prefix needs an argument}; shift 2;;
        --prefix=*) PREFIX=${1#*=}; shift;;
        --release) MODE=release; shift;;
        --source) MODE=source; shift;;
        --file) FILE=${2:?--file needs an argument}; shift 2;;
        --file=*) FILE=${1#*=}; shift;;
        --download-only) DOWNLOAD_ONLY=1; shift;;
        --run) RUN=1; shift;;
        --no-systemd) WITH_SYSTEMD=0; shift;;
        --no-dracut) WITH_DRACUT=0; shift;;
        --no-mkinitcpio) WITH_MKINITCPIO=0; shift;;
        --no-initramfs-tools) WITH_INITRAMFS_TOOLS=0; shift;;
        --dry-run) DRY_RUN=1; shift;;
        --uninstall) UNINSTALL=1; shift;;
        --list) LIST=1; shift;;
        --yes|-y) ASSUME_YES=1; shift;;
        -h|--help) usage; exit 0;;
        *) die "unknown option: $1 (try --help)";;
    esac
done

# ---- paths -----------------------------------------------------------------
ARCH=${ARCH_OVERRIDE:-$(uname -m)}
case "$ARCH" in
    amd64) ARCH=x86_64;;
    arm64) ARCH=aarch64;;
esac
case "$ARCH" in
    x86_64|aarch64) ;;
    *) warn "unsupported architecture '$ARCH'; release assets may not exist";;
esac

PREFIX=${PREFIX%/}
[ -n "$PREFIX" ] || die "empty --prefix"
MANIFEST=${INVFS_MANIFEST:-$DESTDIR$PREFIX/lib/invfs/installed.manifest}
CACHE=${INVFS_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/invfs}
STAGE=${INVFS_STAGE_DIR:-${TMPDIR:-/tmp}/invfs-bootstrap.$$}

# ---- helpers ---------------------------------------------------------------
sha256_of() {  # $1 file -> hex
    if have sha256sum; then sha256sum "$1" | awk '{print $1}'
    elif have shasum; then shasum -a 256 "$1" | awk '{print $1}'
    elif have openssl; then openssl dgst -sha256 "$1" | awk '{print $NF}'
    else die "no sha256 tool (need sha256sum, shasum or openssl)"
    fi
}

fetch() {  # $1 url $2 dest
    if have curl; then curl -fsSL "$1" -o "$2"
    elif have wget; then wget -qO "$2" "$1"
    else die "need curl or wget to download"
    fi
}

url_exists() {  # $1 url
    if have curl; then curl -fsSLI -o /dev/null "$1" 2>/dev/null
    elif have wget; then wget -q --spider "$1" 2>/dev/null
    else return 1
    fi
}

confirm() {  # $1 prompt
    [ "$ASSUME_YES" = 1 ] && return 0
    if [ -t 2 ] && [ -r /dev/tty ]; then
        printf '%s [y/N] ' "$1" >&2
        read -r ans < /dev/tty || ans=
        case "$ans" in y|Y|yes|YES) return 0;; *) return 1;; esac
    fi
    return 1
}

# refuses to touch anything that is not under $DESTDIR$PREFIX
assert_under_prefix() {  # $1 path
    case "$1" in
        "$DESTDIR$PREFIX"/*) return 0;;
        *) die "refusing to touch path outside $DESTDIR$PREFIX: $1";;
    esac
}

sudo_hint() {
    cat >&2 <<EOF
$PROG: root is required to write under $PREFIX.
        Re-run with sudo, for example:
            curl -fsSL <bootstrap-url> | sudo sh -s -- $ORIG_ARGS
        or, from a saved copy:
            sudo sh $0 $ORIG_ARGS
        (use DESTDIR=/some/dir to install without root)
EOF
}

need_root() {
    [ "$DRY_RUN" = 1 ] && return 0
    [ "$LIST" = 1 ] && return 0
    [ "$UNINSTALL" = 1 ] && [ -z "$DESTDIR" ] && [ "$(id -u)" != 0 ] && { sudo_hint; exit 1; }
    [ -n "$DESTDIR" ] && return 0
    [ "$(id -u)" = 0 ] && return 0
    sudo_hint
    exit 1
}

# ---- release resolution ----------------------------------------------------
resolve_latest() {
    tag=
    if have curl; then
        tag=$(curl -fsSLI -o /dev/null -w '%{url_effective}' \
            "https://github.com/$REPO/releases/latest" 2>/dev/null \
            | sed -n 's#.*/tag/##p')
        [ -n "$tag" ] || tag=$(curl -fsSL \
            "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null \
            | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
            | head -1)
    fi
    [ -n "$tag" ] || die "could not resolve latest release for $REPO"
    printf '%s\n' "$tag"
}

release_base() {  # -> base URL for the resolved tag
    tag=$VERSION
    [ "$tag" = latest ] && tag=$(resolve_latest)
    if [ -n "${INVFS_RELEASE_BASE:-}" ]; then
        printf '%s\n' "${INVFS_RELEASE_BASE%/}/$tag"
    else
        printf '%s\n' "https://github.com/$REPO/releases/download/$tag"
    fi
}

release_assets_exist() {
    base=$(release_base 2>/dev/null) || return 1
    url_exists "$base/invfs-$VERSION-$ARCH.tar.zst" && \
        url_exists "$base/SHA256SUMS"
}

# ---- dependency matrix -----------------------------------------------------
os_id_like() {
    osr=${INVFS_OS_RELEASE:-/etc/os-release}
    [ -r "$osr" ] || return 0
    id=$(sed -n 's/^ID=//p' "$osr" | head -1 | tr -d '"')
    like=$(sed -n 's/^ID_LIKE=//p' "$osr" | head -1 | tr -d '"')
    printf '%s %s\n' "$id" "$like"
}

dep_command() {
    set -- $(os_id_like)
    id=${1:-}; like=${2:-}
    for tok in "$id" $like; do
        case "$tok" in
            debian|ubuntu|linuxmint|pop|kali)
                echo "apt-get update && apt-get install -y fuse3 zstd zlib1g"; return;;
            arch|archlinux|manjaro|endeavouros)
                echo "pacman -S --needed fuse3 zstd zlib"; return;;
            gentoo|funtoo)
                echo "emerge --ask sys-fs/fuse:3 app-arch/zstd sys-libs/zlib"; return;;
            void)      echo "xbps-install -S fuse3 zstd zlib"; return;;
            fedora|rhel|centos|rocky|almalinux|ol)
                echo "dnf install fuse3 zstd zlib"; return;;
            opensuse*|suse|sles)
                echo "zypper install fuse3 libzstd1 zlib"; return;;
            alpine)    echo "apk add fuse3 zstd zlib"; return;;
        esac
    done
    echo ""
}

missing_runtime_deps() {
    missing=
    if have pkg-config; then
        for pc in fuse3 libzstd zlib; do
            pkg-config --exists "$pc" 2>/dev/null || missing="$missing $pc"
        done
    else
        have ldconfig || true
        for lib in libfuse3.so.4 libzstd.so.1 libz.so.1; do
            (ldconfig -p 2>/dev/null | grep -q "$lib") || missing="$missing $lib"
        done
    fi
    printf '%s\n' "$missing"
}

print_dep_matrix() {
    cmd=$(dep_command)
    missing=$(missing_runtime_deps)
    if [ -n "$missing" ]; then
        warn "runtime libraries appear missing:$missing"
    else
        info "runtime libraries present (fuse3, zstd, zlib)"
    fi
    cat >&2 <<EOF
$PROG: runtime dependencies are installed by YOUR package manager.
$PROG: this script never installs them. For this host run:
    ${cmd:-<use your distro's fuse3, zstd and zlib packages>}
EOF
}

print_build_deps() {
    cmd=$(dep_command)
    missing=
    for t in cc make pkg-config; do
        have "$t" || missing="$missing $t"
    done
    if have pkg-config; then
        pkg-config --exists fuse3 2>/dev/null || missing="$missing fuse3-dev"
        pkg-config --exists libzstd 2>/dev/null || missing="$missing libzstd-dev"
        pkg-config --exists zlib 2>/dev/null || missing="$missing zlib-dev"
    fi
    if [ -n "$missing" ]; then
        warn "build dependencies appear missing:$missing"
        cat >&2 <<EOF
$PROG: install build deps with your package manager, e.g.:
    ${cmd:-<use your distro's build tools + fuse3/zstd/zlib headers>}
EOF
        return 1
    fi
    return 0
}

# ---- verification ----------------------------------------------------------
verify_sha256() {  # $1 tarball $2 sumsfile $3 artifact basename
    [ -f "$1" ] || die "artifact not found: $1"
    [ -f "$2" ] || die "SHA256SUMS not found: $2"
    expected=$(awk -v f="$3" '$2==f {print $1}' "$2" | head -1)
    [ -n "$expected" ] || die "no SHA256SUMS entry for $3"
    actual=$(sha256_of "$1")
    [ "$actual" = "$expected" ] || die "sha256 MISMATCH for $3
  expected $expected
  actual   $actual"
    info "sha256 verified: $3 $actual"
}

# ---- source location -------------------------------------------------------
locate_source() {
    if [ -n "${INVFS_SOURCE_DIR:-}" ]; then
        printf '%s\n' "$INVFS_SOURCE_DIR"; return 0
    fi
    d=$(CDPATH= cd -- "$(dirname -- "$0")/.." 2>/dev/null && pwd) || d=
    if [ -n "$d" ] && [ -f "$d/Makefile" ] && [ -d "$d/packaging" ]; then
        printf '%s\n' "$d"; return 0
    fi
    if [ -f ./Makefile ] && [ -d ./packaging ] && [ -d ./src ]; then
        pwd; return 0
    fi
    return 1
}

fetch_source() {
    tag=$VERSION
    if [ "$tag" = latest ]; then tag=$(resolve_latest); fi
    mkdir -p "$STAGE"
    tgz="$CACHE/invfs-source-$tag.tar.gz"
    if [ -f "$tgz" ] && [ "$RUN" = 1 ]; then
        info "using cached source tarball $tgz"
    else
        info "downloading source for $tag"
        fetch "https://github.com/$REPO/archive/refs/tags/$tag.tar.gz" "$tgz"
    fi
    tar -xzf "$tgz" -C "$STAGE"
    ap=$(find "$STAGE" -maxdepth 1 -mindepth 1 -type d | head -1)
    [ -n "$ap" ] || die "source tarball did not contain a top-level directory"
    printf '%s\n' "$ap"
}

# ---- install / uninstall ---------------------------------------------------
run_installer() {  # $1 = app/source root
    app=$1
    [ -f "$app/packaging/install.sh" ] || die "no packaging/install.sh under $app"
    [ -n "$DESTDIR" ] && mkdir -p "$DESTDIR$PREFIX/lib/invfs"
    if [ -n "$DESTDIR" ]; then
        mkdir -p "$(dirname "$MANIFEST")"
    elif [ "$DRY_RUN" != 1 ]; then
        mkdir -p "$(dirname "$MANIFEST")" 2>/dev/null || true
    fi
    [ "$DRY_RUN" = 1 ] || : > "$MANIFEST"

    info "installing into $DESTDIR$PREFIX (manifest: $MANIFEST)"
    if [ "$DRY_RUN" = 1 ]; then
        info "DRY-RUN: would run: PREFIX=$PREFIX DESTDIR=$DESTDIR WITH_SYSTEMD=$WITH_SYSTEMD WITH_DRACUT=$WITH_DRACUT WITH_MKINITCPIO=$WITH_MKINITCPIO WITH_INITRAMFS_TOOLS=$WITH_INITRAMFS_TOOLS sh $app/packaging/install.sh --manifest $MANIFEST"
        return 0
    fi
    PREFIX=$PREFIX DESTDIR=$DESTDIR \
        WITH_SYSTEMD=$WITH_SYSTEMD WITH_DRACUT=$WITH_DRACUT \
        WITH_MKINITCPIO=$WITH_MKINITCPIO WITH_INITRAMFS_TOOLS=$WITH_INITRAMFS_TOOLS \
        sh "$app/packaging/install.sh" --manifest "$MANIFEST"
    if [ -f "$MANIFEST" ]; then
        n=$(wc -l < "$MANIFEST" | tr -d ' ')
        info "install complete: $n files recorded in $MANIFEST"
    fi
}

do_release() {
    base=$(release_base)
    artifact="invfs-$VERSION-$ARCH.tar.zst"
    mkdir -p "$CACHE"

    if [ -n "$FILE" ]; then
        TARBALL=$FILE
        SUMS=$(dirname "$FILE")/SHA256SUMS
        [ -f "$SUMS" ] || SUMS=$FILE.SHA256SUMS
        [ -f "$TARBALL" ] || die "artifact not found: $TARBALL"
        info "using local artifact $TARBALL"
    else
        TARBALL=$CACHE/$artifact
        SUMS=$CACHE/SHA256SUMS
        if [ "$RUN" = 1 ] && [ -f "$TARBALL" ] && [ -f "$SUMS" ]; then
            info "using staged artifact $TARBALL"
        else
            info "downloading $base/$artifact"
            if [ "$DRY_RUN" = 1 ]; then
                info "DRY-RUN: would fetch $base/$artifact and $base/SHA256SUMS"
                return 0
            fi
            fetch "$base/$artifact" "$TARBALL"
            fetch "$base/SHA256SUMS" "$SUMS"
        fi
    fi

    if [ "$DRY_RUN" = 1 ]; then
        info "DRY-RUN: would verify $artifact and unpack into $STAGE"
        info "DRY-RUN: would run packaging/install.sh --manifest $MANIFEST"
        return 0
    fi

    verify_sha256 "$TARBALL" "$SUMS" "$(basename "$TARBALL")"

    if [ "$DOWNLOAD_ONLY" = 1 ]; then
        info "download+verify complete (staged)"
        info "artifact: $TARBALL"
        info "sums:     $SUMS"
        info "now run: ${0##*/} --run --version $VERSION --prefix $PREFIX ..."
        return 0
    fi

    confirm "Install InvariantFS from $artifact into $DESTDIR$PREFIX?" || \
        die "aborted (pass --yes to run non-interactively)"
    need_root

    mkdir -p "$STAGE"
    info "unpacking $TARBALL"
    if tar --zstd -tf "$TARBALL" >/dev/null 2>&1; then
        tar --zstd -xf "$TARBALL" -C "$STAGE"
    else
        have zstd || die "tar lacks --zstd and zstd(1) is not installed"
        zstd -dc "$TARBALL" | tar -xf - -C "$STAGE"
    fi
    app=$(find "$STAGE" -maxdepth 1 -mindepth 1 -type d | head -1)
    [ -n "$app" ] || die "artifact did not contain a top-level directory"
    run_installer "$app"
}

do_source() {
    print_dep_matrix
    src=$(locate_source) || src=
    if [ -z "$src" ]; then
        if [ "$DRY_RUN" = 1 ]; then
            info "DRY-RUN: would download source for $VERSION and build with make"
            return 0
        fi
        src=$(fetch_source)
    else
        info "using source checkout $src"
    fi

    info "checking build dependencies"
    if [ "$DRY_RUN" = 1 ]; then
        print_build_deps || true
        info "DRY-RUN: would run: make -C $src -j<N>"
        run_installer "$src"
        return 0
    fi
    print_build_deps || warn "continuing anyway -- build may fail"

    if [ "${INVFS_SKIP_MAKE:-0}" = 1 ]; then
        info "INVFS_SKIP_MAKE=1: not rebuilding"
    else
        confirm "Build InvariantFS from source at $src?" || die "aborted"
        info "building (make)"
        make -C "$src" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
    fi

    if [ "$DOWNLOAD_ONLY" = 1 ]; then
        info "source staged at $src (not installed)"
        return 0
    fi
    need_root
    run_installer "$src"
}

do_list() {
    if [ ! -f "$MANIFEST" ]; then
        info "no manifest at $MANIFEST"
        return 0
    fi
    cat "$MANIFEST"
}

do_uninstall() {
    [ -f "$MANIFEST" ] || die "no manifest at $MANIFEST (nothing to uninstall)"
    assert_under_prefix "$MANIFEST"
    if [ "$DRY_RUN" = 1 ]; then
        info "DRY-RUN: would remove the following:"
        sed 's/^/  /' "$MANIFEST"
        return 0
    fi
    # snapshot outside the prefix: we remove directories (including the
    # manifest's own) while walking the list
    list=${TMPDIR:-/tmp}/invfs-uninstall.$$
    cp "$MANIFEST" "$list"
    trap 'rm -f "$list"' EXIT HUP INT TERM
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        assert_under_prefix "$p"
        if [ -e "$p" ] || [ -L "$p" ]; then
            rm -f "$p"
            info "removed $p"
        fi
    done < "$list"
    # prune now-empty ancestor directories, deepest first, never above prefix
    rm -f "$MANIFEST"
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        d=$(dirname "$p")
        while :; do
            case "$d" in
                "$DESTDIR$PREFIX") rmdir "$d" 2>/dev/null || true; break;;
                "$DESTDIR$PREFIX"/*) ;;
                *) break;;
            esac
            rmdir "$d" 2>/dev/null || break
            d=$(dirname "$d")
        done
    done < "$list"
    rm -f "$list"
    info "uninstall complete"
}

# ---- main ------------------------------------------------------------------
command -v uname >/dev/null 2>&1 || die "uname not found"

if [ "$LIST" = 1 ]; then do_list; exit 0; fi
if [ "$UNINSTALL" = 1 ]; then do_uninstall; exit 0; fi

if [ "$MODE" = auto ]; then
    if [ -n "$FILE" ]; then
        case "$FILE" in
            *.tar.zst) MODE=release;;
            *)         MODE=source;;
        esac
    elif release_assets_exist; then
        MODE=release
    else
        MODE=source
    fi
fi

info "version=$VERSION mode=$MODE prefix=$PREFIX destdir=${DESTDIR:-<none>} arch=$ARCH"

if [ "$MODE" = release ]; then
    print_dep_matrix
    do_release
else
    do_source
fi

info "done"