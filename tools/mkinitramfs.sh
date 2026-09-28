#!/bin/bash
# Build the InvariantFS self-contained initramfs: a busybox userland plus
# the invf-* tools needed to mount an InvariantFS volume as the ROOT
# filesystem, and to repair one that will not mount.
#
# This is the "no distro package manager required" path. For a distro-native
# root-on-InvFS boot see packaging/dracut/90invfs (dracut) and
# packaging/mkinitcpio/invfs_install + invfs_hook (mkinitcpio); those stage
# the same toolset and hand the mounted root to their own switch_root.
#
# Output: $INVFS_INITRAMFS_OUT (default vm/initramfs.cpio.gz, gitignored).
#
# Knobs:
#   INVFS_INITRAMFS_OUT=path   where to write the cpio.gz
#   INVFS_INITRAMFS_VERIFY=1   run tools/verify-initramfs.sh on the result
#   INVFS_INITRAMFS_STATIC=0   force dynamic binaries (skip the static relink)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${INVFS_INITRAMFS_OUT:-$ROOT/vm/initramfs.cpio.gz}"
KVER="$(uname -r)"

# ---------------------------------------------------------------------------
# The toolset
#
# Two groups, and the second one is the reason this file exists at all:
#
#   - the daemon that mounts the volume (invf-fuse), plus the offline
#     maintenance tools (sweep, rollback);
#   - the RESCUE set: invf-verify, invf-fsck, invf-cat, invf-ls, invf-stat.
#     A root filesystem that fails to mount must be recoverable from the
#     initramfs itself. Without these, a damaged root volume is a bricked
#     machine whose only recourse is rescue media the user may not have.
#     invf-cat/invf-ls/invf-stat read an UNMOUNTED volume directly, which is
#     the only way to inspect a volume whose FUSE mount refuses to come up.
#
# invf-mkfs is deliberately NOT staged: reformatting a root volume from an
# incidental initramfs shell is a footgun, and nothing in the boot path needs
# it. (The dracut module stages the same set.)
#
# Ordering note: invf-cat / invf-verify / invf-fsck all take an EXCLUSIVE lock
# on the image ("image is in use by another process" otherwise), so they only
# work on a volume that is NOT currently mounted.
# ---------------------------------------------------------------------------
INVF_TOOLS="invf-fuse invf-sweep invf-rollback invf-verify invf-fsck invf-cat invf-ls invf-stat"

# Per-tool extra objects, mirroring the Makefile link rules. invf-fuse pulls
# tmpstore.o; invf-sweep and invf-rollback are built from tools/*.c and so
# have differently named objects.
extra_objs() {
# NOTE: these must be absolute. The staging step below cd's into $IR, and both
# build/core_objs.txt and these names are repo-relative.
    case "$1" in
        invf-fuse)     echo "$ROOT/build/obj/fuse_fs.o $ROOT/build/obj/tmpstore.o" ;;
        invf-sweep)    echo "$ROOT/build/obj/invf-sweep.o" ;;
        invf-rollback) echo "$ROOT/build/obj/invf-rollback.o" ;;
        # the cli tools are invf-<name> but compile from src/cli/<name>.c
        invf-verify)   echo "$ROOT/build/obj/verify.o" ;;
        invf-fsck)     echo "$ROOT/build/obj/fsck.o" ;;
        invf-cat)      echo "$ROOT/build/obj/cat.o" ;;
        invf-ls)       echo "$ROOT/build/obj/ls.o" ;;
        invf-stat)     echo "$ROOT/build/obj/stat.o" ;;
        *)             echo "$ROOT/build/obj/$1.o" ;;
    esac
}

# libfuse3 is needed by the daemon only; every other tool links zstd + z.
# Both ship .a on a normal distro, and the repo carries its own stock zlib
# (src/zlib, zlib_stock_*.o) for the deflate backend, so -lz is only there for
# vol_cpack's inflateInit2_ use.
static_libs() {
    case "$1" in
        invf-fuse) echo "-Wl,-l:libfuse3.a" ;;
        *)         echo "" ;;
    esac
}

# ---------------------------------------------------------------------------
# Static relink
#
# Prefer a fully static initramfs: with no dynamic loader and no shared
# objects there is no ld.so search path to get wrong, no libc/loader version
# skew, and the whole "every .so resolves" class of bootstrap failure simply
# does not exist. All nine tools static-link cleanly here (none of them use
# dlopen -- the only dlopen in the tree is in a test binary), so this normally
# succeeds for everything.
#
#   -Wl,--allow-multiple-definition is required and benign: the bundled stock
#   zlib (zlib_stock_zutil.o) and libz.a both define z_errmsg. The stock copy
#   wins; the symbol is only an error-message string.
#
# If a static link is impossible (no .a for some library, a future tool that
# genuinely needs dlopen) we fall back to the dynamic binary and copy its
# shared libraries, discovered with ldd -- never a hardcoded list. The earlier
# recipe copied a fixed set out of /lib64, a path that only exists on
# Fedora/RHEL-style layouts: on a Debian multiarch host every one of those
# paths is missing, so the first `cp` aborted the whole build under `set -e`.
# ldd is the authority on what a binary actually needs, so a tool that grows
# a dependency is picked up automatically.
# ---------------------------------------------------------------------------
ldd_paths() {  # <elf> -> one absolute path per resolved object
    ldd "$1" 2>/dev/null | sed -n \
        -e 's/.*=> \(\/[^ ]*\) .*/\1/p' \
        -e 's/^[[:space:]]*\(\/lib[^ ]*\) .*/\1/p'
}

mkdir -p "$(dirname "$OUT")"
IR="$ROOT/vm/initramfs"
rm -rf "$IR"
mkdir -p "$IR"
cd "$IR"

# vm/ is gitignored, so a fresh checkout has no vm/initramfs tree at all.
mkdir -p bin sbin usr/local/bin usr/local/lib lib64 modules \
         proc sys dev tmp mnt/invfs run

# ---- busybox --------------------------------------------------------------
# Already a tracked static binary (bin/busybox-static, vendored, not built by
# this repo). The userland this initramfs has is exactly this.
cp "$ROOT/bin/busybox-static" bin/busybox
chmod 755 bin/busybox
ln -sf busybox bin/sh
# Applet symlinks: /init calls mount/grep/cat/insmod/chroot/... by name, so
# install the full applet set (the tracked tree has no skeleton symlinks).
# `busybox --install -s` records the build-time absolute path as the link
# target, which does not exist in the guest; create relative links instead.
for a in $(./bin/busybox --list); do ln -sf busybox "bin/$a"; done

# ---- invf tools -----------------------------------------------------------
CORE_OBJS=""
if [ -f "$ROOT/build/core_objs.txt" ]; then
    # absolutise: the staging loop runs from inside $IR
    CORE_OBJS="$(sed "s#^#$ROOT/#" "$ROOT/build/core_objs.txt" | tr '\n' ' ')"
fi

DYNAMIC_TOOLS=""
for b in $INVF_TOOLS; do
    src="$ROOT/bin/$b"
    [ -x "$src" ] || { echo "ERROR: $src missing -- run 'make' first" >&2; exit 1; }

    out="usr/local/bin/$b"
    linked=dynamic
    first_obj="$(extra_objs "$b" | cut -d' ' -f1)"
    if [ "${INVFS_INITRAMFS_STATIC:-1}" = 1 ] && [ -n "$CORE_OBJS" ] && [ -f "$first_obj" ]; then
        if cc -O2 -o "$out" $(extra_objs "$b") $CORE_OBJS \
               -Wl,-l:libzstd.a $(static_libs "$b") -lz -lpthread -static \
               -Wl,--allow-multiple-definition 2>/dev/null; then
            linked=static
        fi
    fi

    if [ "$linked" = dynamic ]; then
        cp "$src" "$out"
        DYNAMIC_TOOLS="$DYNAMIC_TOOLS $b"
    fi
    chmod 755 "$out"

    # Never ship a binary whose libraries do not resolve on the build host:
    # that is precisely the "initramfs that cannot start" failure.
    if ldd "$out" 2>/dev/null | grep -q 'not found'; then
        echo "ERROR: $b has unresolved shared libraries:" >&2
        ldd "$out" | grep 'not found' >&2
        exit 1
    fi
done

# ---- shared libraries (dynamic tools only) --------------------------------
: > lib64/libs.manifest
for b in $DYNAMIC_TOOLS; do
    for so in $(ldd_paths "usr/local/bin/$b"); do
        cp -L "$so" "lib64/$(basename "$so")"
        echo "$b $(basename "$so")" >> lib64/libs.manifest
    done
done
# The loader itself is never a "=> /path" line on some glibc builds; make sure
# it is present whenever anything is dynamic.
if [ -n "$DYNAMIC_TOOLS" ]; then
    for ldso in /lib64/ld-linux-x86-64.so.2 /lib/ld-linux-x86-64.so.2; do
        if [ -e "$ldso" ]; then cp -L "$ldso" lib64/ 2>/dev/null; break; fi
    done
fi

# ---- fuse kernel module ---------------------------------------------------
# OPTIONAL, and its absence is not an error. It is only needed when the
# target kernel builds FUSE as a module (CONFIG_FUSE_FS=m). A kernel with FUSE
# built in (CONFIG_FUSE_FS=y -- the usual case for a distro kernel) has no
# fuse.ko on disk at all: there is nothing to stage and nothing to load,
# because /dev/fuse already works.
#
# The previous recipe piped `find ... | head -1 | xargs -I{} cp {}` straight
# into a `file -b` on a possibly-empty temp file, so on such a host it either
# aborted under `set -e` or shipped a zero-length fuse.ko that /init would
# happily try to insmod. Record which case this is and let /init decide at run
# time by probing /dev/fuse.
FUSE_MODE=missing
fuse_src=$(find "/lib/modules/$KVER/kernel/fs/fuse" -name 'fuse.ko.*' 2>/dev/null | head -1 || true)
if [ -n "$fuse_src" ]; then
    FUSE_MODE=module
    case "$(file -b "$fuse_src")" in
        *XZ*)   xz  -dc "$fuse_src" > fuse.ko ;;
        *Zstd*) zstd -dc "$fuse_src" > fuse.ko ;;
        *)      cp "$fuse_src" fuse.ko ;;
    esac
    [ -s fuse.ko ] || { echo "ERROR: fuse.ko decompressed to zero bytes" >&2; exit 1; }
elif [ -d /sys/module/fuse ]; then
    FUSE_MODE=builtin
else
    echo "WARNING: no fuse.ko for $KVER and this kernel exposes no /sys/module/fuse." >&2
    echo "         The initramfs will not be able to mount a volume." >&2
fi

# virtio_blk (SEE THE VOLUME) + virtio_net / net_failover / failover (the
# guest NIC, so a root-on-InvFS box is reachable for remote repair).
#
# virtio_blk was missing from this list, which is why a volume attached as a
# virtio disk was invisible to the initramfs: on a distro kernel
# CONFIG_VIRTIO_BLK=m (checked on 6.12.107+deb13-amd64), so the disk does not
# exist as a block device until the module loads, and the init's probe then
# reports "no InvariantFS volume found on any block device" -- indistinguishable
# from a lost volume. virtio, virtio_ring and virtio_pci are builtin on that
# kernel (no .ko in the tree, and virtio_pci is =y), so virtio_blk is the only
# one to stage. Same as fuse: absent on a host with no module dir.
#
# Order is the load order: insmod resolves nothing, so a module has to appear
# after everything it depends on.
mkdir -p lib/modules
for mod in virtio_blk failover net_failover virtio_net; do
    src=$(find "/lib/modules/$KVER/kernel" -name "$mod.ko.*" 2>/dev/null | head -1 || true)
    [ -n "$src" ] || continue
    case "$(file -b "$src")" in
        *XZ*)   xz  -dc "$src" > "lib/modules/$mod.ko" ;;
        *Zstd*) zstd -dc "$src" > "lib/modules/$mod.ko" ;;
        *)      cp "$src" "lib/modules/$mod.ko" ;;
    esac
done

# ---- init -----------------------------------------------------------------
cp "$ROOT/tools/initramfs-init.sh" init
chmod 755 init

# Optional self-test data. INVFS_SELFTEST_SUM is the expected md5 of the file
# the volume under test carries, and the `invfs.selftest` cmdline flag makes
# the init read the file back through the mount and compare. Unset in every
# normal build, so the shipped image carries no test data at all.
if [ -n "${INVFS_SELFTEST_SUM:-}" ]; then
    printf '%s\n' "$INVFS_SELFTEST_SUM" > selftest.sha
    echo "self-test: expected md5 $INVFS_SELFTEST_SUM baked into selftest.sha"
else
    rm -f selftest.sha
fi

# ---- manifest -------------------------------------------------------------
# Read by tools/verify-initramfs.sh and by anyone debugging a bad image: it
# answers "what was actually staged, and was it static?" without unpacking
# and guessing.
STATIC_LIST=$(for b in $INVF_TOOLS; do
                  case "$(file -b "usr/local/bin/$b")" in
                      *"statically linked"*) printf '%s ' "$b" ;;
                  esac
              done)
{
    echo "kver=$KVER"
    echo "fuse=$FUSE_MODE"
    echo "static=$STATIC_LIST"
    echo "dynamic=${DYNAMIC_TOOLS# }"
    echo "tools=$INVF_TOOLS"
} > invfs-initramfs.manifest

find . | cpio -o -H newc --quiet | gzip -1 > "$OUT"
echo "wrote $OUT"
ls -l "$OUT"
echo "fuse=$FUSE_MODE  static=[${STATIC_LIST% }]  dynamic=[${DYNAMIC_TOOLS# }]"

if [ "${INVFS_INITRAMFS_VERIFY:-0}" = 1 ]; then
    echo
    echo "== verifying =="
    "$ROOT/tools/verify-initramfs.sh" --initramfs "$OUT"
fi
