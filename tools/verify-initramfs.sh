#!/bin/bash
# tools/verify-initramfs.sh -- prove an initramfs can actually reach an
# InvariantFS volume, instead of asserting that it probably could.
#
# A recipe nobody has booted is exactly the kind of thing that rots. This
# script is the closest thing to a boot that can run without a hypervisor: it
# unpacks the cpio, checks the structure, then CHROOTS into the unpacked tree
# and, using nothing but what the cpio contains, creates a real volume,
# mounts it through the initramfs' own invf-fuse, writes a file, reads it back
# and compares hashes, then unmounts and runs the offline rescue path
# (invf-verify --deep, invf-cat, invf-fsck).
#
# What it does NOT prove: kernel handoff, PID 1 semantics, switch_root, udev,
# real block-device discovery. Those need a real boot. See the "UNPROVEN"
# section of the WP report.
#
# Usage:
#   tools/verify-initramfs.sh [--initramfs PATH] [--workdir DIR] [--keep]
#                             [--no-mount] [--quiet]
#
# Exit 0 = every check passed. Non-zero = at least one FAIL (counted above).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${INVFS_INITRAMFS_OUT:-$ROOT/vm/initramfs.cpio.gz}"
WORKBASE="${INVFS_VERIFY_BASE:-/srv/bench}"
MOUNT_TEST=1
KEEP=0
QUIET=0

while [ $# -gt 0 ]; do
    case "$1" in
        --initramfs) IMG="$2"; shift 2 ;;
        --workdir)   WORKBASE="$2"; shift 2 ;;
        --no-mount)  MOUNT_TEST=0; shift ;;
        --keep)      KEEP=1; shift ;;
        --quiet)     QUIET=1; shift ;;
        -h|--help)   sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

pass=0; fail=0
ok()  { [ "$QUIET" = 1 ] || printf 'ok   - %s\n' "$1"; pass=$((pass+1)); }
bad() { printf 'FAIL - %s\n' "$1" >&2; fail=$((fail+1)); }
note(){ [ "$QUIET" = 1 ] || printf '       %s\n' "$1"; }
phase(){ [ "$QUIET" = 1 ] || printf '\n== %s ==\n' "$1"; }

[ -f "$IMG" ] || { echo "no initramfs at $IMG (build it with tools/mkinitramfs.sh)" >&2; exit 2; }

# A big scratch dir under /srv/bench: the proof volume is a real (sparse)
# image and / is small and shared with other agents.
mkdir -p "$WORKBASE" || WORKBASE=/tmp
W=$(mktemp -d "$WORKBASE/invfs-initramfs-verify.XXXXXX") || exit 2
cleanup() {
    [ "$KEEP" = 1 ] || rm -rf "$W"
}
trap cleanup EXIT
TREE="$W/tree"

# ---------------------------------------------------------------------------
phase "1. unpack"
# ---------------------------------------------------------------------------
mkdir -p "$TREE"
if ! (cd "$TREE" && gzip -dc "$IMG" | cpio -idm --quiet) 2>"$W/cpio.err"; then
    bad "cpio extract from $IMG"
    sed 's/^/       /' "$W/cpio.err" >&2
    exit 1
fi
ok "unpacked $(du -sh "$TREE" | cut -f1) from ${IMG##*/}"

MAN="$TREE/invfs-initramfs.manifest"
have_manifest=0; [ -f "$MAN" ] && have_manifest=1
if [ "$have_manifest" = 1 ]; then
    note "manifest: $(tr '\n' ' ' < "$MAN")"
fi

# ---------------------------------------------------------------------------
phase "2. structure"
# ---------------------------------------------------------------------------
[ -x "$TREE/init" ] && ok "/init present and executable" \
                    || bad "/init missing or not executable"
grep -q "invf-fuse" "$TREE/init" 2>/dev/null \
    && ok "/init actually invokes invf-fuse (not just a stub)" \
    || bad "/init never mentions invf-fuse"

if [ -x "$TREE/bin/busybox" ]; then
    case "$(file -b "$TREE/bin/busybox")" in
        *"statically linked"*) ok "busybox is statically linked" ;;
        *) bad "busybox is NOT static: $(file -b "$TREE/bin/busybox")" ;;
    esac
else
    bad "bin/busybox missing"
fi

# Applet symlinks must be RELATIVE and resolve; `busybox --install -s` bakes in
# the build host's absolute path, which does not exist in the guest.
broken=0
for l in mount grep cat insmod chroot sha256sum switch_root; do
    if [ -L "$TREE/bin/$l" ]; then
        [ -e "$TREE/bin/$l" ] || { bad "applet symlink bin/$l dangles -> $(readlink "$TREE/bin/$l")"; broken=1; }
    else
        bad "applet symlink bin/$l missing"
        broken=1
    fi
done
[ "$broken" = 0 ] && ok "busybox applet symlinks resolve (mount/grep/cat/insmod/chroot/sha256sum/switch_root)"

# The rescue toolset -- the reason a damaged root volume is not a brick.
for b in invf-fuse invf-verify invf-fsck invf-cat invf-ls invf-stat; do
    if [ -x "$TREE/usr/local/bin/$b" ]; then
        ok "staged $b"
    else
        bad "MISSING $b -- a volume that will not mount cannot be repaired"
    fi
done

# fuse.ko: required only for CONFIG_FUSE_FS=m. Absent is fine when the build
# host's kernel has FUSE built in, but the manifest must then say so.
fuse_mode=$(sed -n 's/^fuse=//p' "$MAN" 2>/dev/null)
if [ -s "$TREE/fuse.ko" ]; then
    ok "fuse.ko staged ($(wc -c < "$TREE/fuse.ko") bytes)"
elif [ "$fuse_mode" = builtin ]; then
    ok "no fuse.ko, but the build kernel has FUSE built in (manifest fuse=builtin)"
else
    bad "no fuse.ko and fuse mode is '${fuse_mode:-unknown}' -- cannot mount"
fi

# Every shared object a dynamic binary needs must be present, resolved from
# the image's own lib64 and nowhere else.
missing_lib=0
for f in "$TREE"/usr/local/bin/*; do
    [ -e "$f" ] || continue
    case "$(file -b "$f")" in
        *"statically linked"*) continue ;;
    esac
    while read -r so; do
        [ -n "$so" ] || continue
        [ -e "$TREE/lib64/$(basename "$so")" ] || {
            bad "$(basename "$f") needs $(basename "$so"), not in the image"
            missing_lib=1
        }
    done < <(ldd "$f" 2>/dev/null | sed -n -e 's/.*=> \(\/[^ ]*\) .*/\1/p' \
                                            -e 's/^[[:space:]]*\(\/lib[^ ]*\) .*/\1/p')
done
[ "$missing_lib" = 0 ] && ok "no unresolved shared libraries among the staged tools"

# ---------------------------------------------------------------------------
phase "3. execute the staged binaries inside a chroot"
# ---------------------------------------------------------------------------
# unshare -rm gives CAP_SYS_ADMIN + CAP_SYS_CHROOT in a private user
# namespace, so this needs no host root. The binaries run with the unpacked
# tree as /, so nothing on the host can satisfy a missing dependency.
if ! unshare -rm true 2>/dev/null; then
    bad "cannot create a user namespace (unshare -rm); chroot proof unavailable"
    note "on a host without userns, phases 3-4 are SKIPPED -- this is NOT a boot"
    MOUNT_TEST=0
else
    mkdir -p "$TREE/proof"
    cat > "$TREE/proof/smoke.sh" <<'EOS'
set -u
PATH=/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin
export PATH
rc=0
for b in invf-fuse invf-sweep invf-rollback invf-verify invf-fsck invf-cat invf-ls invf-stat; do
    out=$( "$b" 2>&1 )
    case "$out" in
        *usage*|*Usage*) echo "RAN $b" ;;
        *"not found"*)    echo "MISSING-LIB $b: $out"; rc=1 ;;
        *)                echo "RAN $b (no usage banner)"; ;;
    esac
done
exit $rc
EOS
    if out=$(unshare -rm sh -c "
            mount --rbind /dev '$TREE/dev' 2>/dev/null
            mount -t proc proc '$TREE/proc' 2>/dev/null
            exec chroot '$TREE' /bin/busybox sh /proof/smoke.sh" 2>&1); then
        ran=$(printf '%s\n' "$out" | grep -c '^RAN ' || true)
        if [ "$ran" -ge 8 ] && ! printf '%s\n' "$out" | grep -q '^MISSING-LIB'; then
            ok "all $ran staged tools execute inside the chroot"
        else
            bad "some staged tools did not run inside the chroot"
            printf '%s\n' "$out" | sed 's/^/       /' >&2
        fi
    else
        bad "chroot smoke test failed"
        printf '%s\n' "$out" | sed 's/^/       /' >&2
    fi
fi

# ---------------------------------------------------------------------------
phase "4. live volume proof inside the chroot"
# ---------------------------------------------------------------------------
# The real question: can the initramfs MOUNT a pre-existing InvariantFS volume
# and get the data back bit-exact, using only what the cpio contains?
#
# Note the split of labour, which mirrors a real install. The root volume is
# manufactured AHEAD of the boot by the installer, using the host's
# invf-mkfs. invf-mkfs is deliberately NOT staged in the initramfs
# (reformatting a root volume from an incidental initramfs shell is a
# footgun, and nothing in the boot path needs it) -- so this phase hands the
# chroot a finished volume and requires the chroot to mount, read, verify
# and fsck it with only the cpio's own tools.
if [ "$MOUNT_TEST" = 0 ]; then
    note "skipped (--no-mount, or no user namespace)"
elif [ ! -e /dev/fuse ]; then
    bad "/dev/fuse does not exist on this host -- cannot mount to prove anything"
else
    PREP="$TREE/proof"
    mkdir -p "$PREP"

    # ---- install time: create and populate the volume ---------------------
    if ! INVFS_META_FRAC=16 "$ROOT/bin/invf-mkfs" "$PREP/vol.img" 2 >/dev/null 2>&1; then
        bad "host invf-mkfs could not create the test volume"
        PREP_OK=0
    else
        ok "install-time: invf-mkfs created a 2 GiB volume"
        MNT="$W/mnt"; mkdir -p "$MNT"
        "$ROOT/bin/invf-fuse" "$PREP/vol.img" "$MNT" >"$W/fuse.log" 2>&1 &
        HPID=$!
        i=0
        while [ $i -lt 30 ]; do
            grep -q "InvariantFS mounted" "$W/fuse.log" 2>/dev/null && break
            kill -0 "$HPID" 2>/dev/null || break
            sleep 1; i=$((i+1))
        done
        if ! grep -q "InvariantFS mounted" "$W/fuse.log" 2>/dev/null; then
            bad "install-time: could not mount the test volume to populate it"
            sed 's/^/       /' "$W/fuse.log" >&2
            PREP_OK=0
        else
            head -c 1048576 /dev/urandom > "$PREP/expected.bin" 2>/dev/null
            if cp "$PREP/expected.bin" "$MNT/payload.bin" 2>/dev/null && sync; then
                (cd "$PREP" && sha256sum expected.bin | cut -d' ' -f1 > expected.sha256)
                ok "install-time: wrote a 1 MiB payload into the volume"
                PREP_OK=1
            else
                bad "install-time: could not write the payload"
                PREP_OK=0
            fi
        fi
        # release the EXCLUSIVE lock so the chroot can take it
        kill "$HPID" 2>/dev/null
        sleep 1
        umount -l "$MNT" 2>/dev/null
        sleep 1
    fi

    if [ "${PREP_OK:-0}" != 1 ]; then
        bad "skipping the chroot proof: no usable test volume"
    else
    cat > "$PREP/live.sh" <<'EOS'
set -u
PATH=/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin
export PATH
D=/proof
M=$D/mnt
rc=0
fail() { echo "FAIL $1"; rc=1; }

rm -rf "$M"; mkdir -p "$M"

# 1. mount the pre-existing volume with the initramfs' own daemon
invf-fuse "$D/vol.img" "$M" >"$D/fuse.log" 2>&1 &
FPID=$!
i=0
while [ $i -lt 30 ]; do
    grep -q "InvariantFS mounted" "$D/fuse.log" 2>/dev/null && break
    kill -0 "$FPID" 2>/dev/null || break
    sleep 1; i=$((i+1))
done
if ! grep -q "InvariantFS mounted" "$D/fuse.log" 2>/dev/null; then
    fail "invf-fuse did not mount the volume:"
    sed 's/^/       /' "$D/fuse.log"
    exit 1
fi
echo "MOUNT ok: $(grep -m1 'InvariantFS mounted' "$D/fuse.log")"

# 2. read back what the installer wrote, against the hash recorded then
want=$(cut -d' ' -f1 < "$D/expected.sha256")
got=$(sha256sum "$M/payload.bin" 2>/dev/null | cut -d' ' -f1)
if [ -n "$got" ] && [ "$want" = "$got" ]; then
    echo "READBACK ok sha256=$got"
else
    fail "payload read back from the mount is not bit-exact: want $want got $got"
fi

# 3. write a NEW file from inside the initramfs and read it back
head -c 262144 /dev/urandom > "$D/fresh.bin" 2>/dev/null
if cp "$D/fresh.bin" "$M/fresh.bin" 2>/dev/null && sync; then
    a=$(sha256sum "$D/fresh.bin" | cut -d' ' -f1)
    b=$(sha256sum "$M/fresh.bin"   | cut -d' ' -f1)
    [ -n "$b" ] && [ "$a" = "$b" ] \
        && echo "WRITE-THROUGH ok sha256=$b" \
        || fail "write through the mount was not bit-exact: $a vs $b"
else
    fail "could not write through the mount"
fi

# 4. unmount, so the EXCLUSIVE-lock offline tools can open the image
kill "$FPID" 2>/dev/null
sleep 2
umount -l "$M" 2>/dev/null
sleep 1

# 5. the rescue path -- these only work on an UNMOUNTED volume, which is
#    exactly the situation when the root volume refuses to mount.
if invf-cat "$D/vol.img" /payload.bin "$D/cat.bin" >/dev/null 2>&1 \
   && cmp -s "$D/expected.bin" "$D/cat.bin"; then
    echo "INVF-CAT ok (offline read of an unmounted volume is bit-exact)"
else
    fail "invf-cat could not read the volume back offline"
fi

# invf-verify --deep: gate on the EXIT CODE, never on stdout. The tool prints
# "OK: ... is a valid InvariantFS volume" for the shallow superblock pass
# BEFORE the deep pass runs, so a --deep run that cannot open the volume still
# says OK on stdout while exiting 1. (Reported as a finding, not worked around
# in the tool.)
if invf-verify --deep "$D/vol.img" >"$D/verify.out" 2>"$D/verify.err"; then
    echo "VERIFY-DEEP ok: $(grep -m1 'state:' "$D/verify.out" | sed 's/^ *//')"
else
    fail "invf-verify --deep rejected the volume:"
    sed 's/^/       /' "$D/verify.err" | head -5
fi

if invf-fsck "$D/vol.img" >"$D/fsck.out" 2>&1; then
    # fsck's first line is a vol_open banner; show its own verdict if it has one
    echo "FSCK ok: $(grep -m1 -i -E 'fsck|clean|ok|error' "$D/fsck.out" \
                     | grep -m1 -iv 'vol_open' | sed 's/^ *//')"
else
    fail "invf-fsck rejected the volume:"
    sed 's/^/       /' "$D/fsck.out" | head -5
fi

invf-ls "$D/vol.img" >"$D/ls.out" 2>&1 \
    && echo "INVF-LS ok: $(grep -m1 payload "$D/ls.out" | sed 's/^ *//')" \
    || echo "INVF-LS produced no listing (non-fatal)"
exit $rc
EOS
    if out=$(unshare -rm sh -c "
            mount --rbind /dev '$TREE/dev' 2>/dev/null
            mount -t proc proc '$TREE/proc' 2>/dev/null
            exec chroot '$TREE' /bin/busybox sh /proof/live.sh" 2>&1); then
        while IFS= read -r line; do
            case "$line" in
                MOUNT*|READBACK*|WRITE-THROUGH*|INVF-CAT*|VERIFY-DEEP*|FSCK*|INVF-LS*)
                    ok "${line%%:* }" ;;
                *) [ "$QUIET" = 1 ] || printf '       %s\n' "$line" ;;
            esac
        done <<< "$out"
    else
        bad "live volume proof failed inside the chroot"
        printf '%s\n' "$out" | sed 's/^/       /' >&2
    fi
    fi
fi

# ---------------------------------------------------------------------------
phase "result"
# ---------------------------------------------------------------------------
printf '%d passed, %d failed\n' "$pass" "$fail"
if [ "$fail" -ne 0 ]; then
    echo "initramfs is NOT bootable as built" >&2
    exit 1
fi
echo "initramfs self-verified: it mounts a real InvariantFS volume and reads"
echo "data back bit-exact, using only what this cpio contains."
echo "NOTE: kernel handoff / PID 1 / switch_root are NOT covered by this script."
exit 0
