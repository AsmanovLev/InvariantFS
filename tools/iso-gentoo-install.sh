#!/bin/bash
# iso-gentoo-install.sh -- stock-Gentoo-minimal-ISO install onto InvariantFS.
#
# Same shape as tools/iso-arch-install.sh: the ISO boots PRISTINE
# (checksum-verified against the SHA512 upstream publishes for the build it
# currently points at) and everything
# InvFS-specific comes from the local server. The automation hook is the
# Handbook's own unattended-install pair: dosshd + passwd= on the kernel
# cmdline. The driver then SSHes in (sshpass, throwaway CI-only password)
# and runs one guest command -- no serial input driving, no installer
# (Gentoo has no installer TUI; the Handbook install IS the CLI flow).
#
#   WORK=/path/to/work bash tools/iso-gentoo-install.sh
#
# Needs: qemu-system-x86_64 (KVM, else TCG fallback), sudo (ISO loop-mount),
# sshpass, ~16 GB scratch, network. Prints INVFS-ISO-SETUP: PASS/FAIL from
# the guest serial and exits 0/1, so CI can gate on it directly.
set -u
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
WORK="${WORK:?set WORK to a scratch dir}"
# Inputs are RESOLVED, not pinned. Gentoo republishes current-install-* and
# current-stage3-* every week, and a pinned DATED filename 404s the moment the
# build rotates -- measured: 20261011T170049Z disappeared and this job died on
# a download 404 with no code change behind it. Upstream publishes latest-*.txt
# for exactly this, and the SHA512 comes from the .DIGESTS that names the same
# artefact (there is no .iso.sha512; measured 404).
GENTOO_BASE="https://distfiles.gentoo.org/releases/amd64/autobuilds"

# resolve_latest <latest-txt-url> <ere>: the artefact name upstream currently
# points at. Prints nothing (rc 1) when the listing cannot be read.
resolve_latest() {
    curl -fsSL "$1" 2>/dev/null | tr -d '\r' | grep -Eo "$2" | head -1
}

# digest_sha512 <digests-url> <exact-name>: the SHA512 line for that artefact.
# A .DIGESTS file repeats "# SHA512 HASH" once per artefact (the .CONTENTS.gz
# has its own), so the name must match exactly -- and the line is passed
# through with its original two-space separator, which sha512sum -c requires.
digest_sha512() {
    curl -fsSL "$1" 2>/dev/null | tr -d '\r' \
        | awk -v want="$2" 'length($1)==128 && $1 ~ /^[0-9a-f]+$/ && $2==want {print; exit}'
}

# Kernel+initramfs paths INSIDE the ISO. Stable across builds; the copy below
# fails loudly if they are not there.
ISO_KERNEL="boot/gentoo"
ISO_INITRD="boot/gentoo.igz"
# dracut live root lookup (confirmed: grub.cfg root=live:CDLABEL=...). READ
# from the downloaded ISO (see below), never derived from its name: it tracks
# the build date, and a wrong label means the guest kernel never finds the
# live root -- which reads as a boot hang, not as a wrong label.
ISO_LABEL=""
# Throwaway root password for the dosshd CI guest. Not a credential to
# anything real; the guest is NATed, short-lived, and destroyed after.
GUEST_PW="invfs-live-install"
PORT="${INVFS_ISO_PORT:-8002}"
SSHPORT="${INVFS_ISO_SSHPORT:-2223}"
MEM="${INVFS_ISO_MEM:-8G}"
TIMEOUT_MIN="${INVFS_ISO_TIMEOUT_MIN:-100}"
ACCEL="${INVFS_ACCEL:-auto}"

VOL="$WORK/vol.img"
STAGEDISK="$WORK/stage.img"
SER="$WORK/serial.log"
SPIPE="$WORK/serial.pipe"
SLOG="$WORK/serve.log"
SSHLOG="$WORK/ssh.log"
SERVEDIR="$WORK/serve"
MNT="$WORK/iso-mnt"
fail() { echo "iso-gentoo-install: FAIL: $*" >&2; exit 1; }
note() { echo "iso-gentoo-install: $*"; }

# Resolve the current upstream artefacts. Fatal, not best-effort: a harness
# that silently tested a stale artefact would be worse than one that fails.
ISO_NAME=$(resolve_latest \
    "$GENTOO_BASE/current-install-amd64-minimal/latest-install-amd64-minimal.txt" \
    'install-amd64-minimal-[0-9]{8}T[0-9]{6}Z\.iso') || true
[ -n "${ISO_NAME:-}" ] || fail "could not resolve the current Gentoo minimal ISO name"
ISO_VER="${ISO_NAME#install-amd64-minimal-}"; ISO_VER="${ISO_VER%.iso}"
ISO_URL="$GENTOO_BASE/current-install-amd64-minimal/$ISO_NAME"
ISO_SHA512=$(digest_sha512 "$ISO_URL.DIGESTS" "$ISO_NAME") || true
[ -n "${ISO_SHA512:-}" ] || fail "no SHA512 for $ISO_NAME in its .DIGESTS"
ISO="$WORK/$ISO_NAME"
note "upstream ISO: $ISO_NAME"

mkdir -p "$WORK" "$SERVEDIR"
[ -x "$REPO/bin/invf-mkfs" ] || fail "run make first (no $REPO/bin/invf-mkfs)"
command -v sshpass >/dev/null 2>&1 || fail "sshpass not installed"

# Strays from a killed run hold the volume lock, the port, or the pipes.
P=$(ps -eo pid,args | grep "[g]entooisosearchuuid" | awk '{print $1}' || true)
[ -n "$P" ] && { kill -KILL $P 2>/dev/null; note "killed stray qemu: $P"; }
Q=$(ps -eo pid,args | grep "[i]nvfs-serve.sh" | awk '{print $1}' || true)
[ -n "$Q" ] && { kill -KILL $Q 2>/dev/null; note "killed stray server: $Q"; }
sleep 1

# fetch_verify <url> <dest> <sha512-line>: a completed-but-corrupt
# download (seen live: right size, wrong hash) must retry, not fail the
# whole job on runner-network luck. Verifies after every attempt.
fetch_verify() {
    _url=$1; _dest=$2; _sum=$3
    _try=1
    while [ $_try -le 3 ]; do
        rm -f "$_dest"
        if curl -fsSL -o "$_dest" "$_url"; then
            if ( cd "$(dirname "$_dest")" && echo "$_sum" | sha512sum -c - ); then
                return 0
            fi
            note "attempt $_try: checksum mismatch, redownloading"
        else
            note "attempt $_try: download failed, retrying"
        fi
        _try=$((_try + 1))
        sleep 10
    done
    return 1
}
if [ ! -f "$ISO" ]; then
    note "downloading $ISO_URL"
    fetch_verify "$ISO_URL" "$ISO" "$ISO_SHA512" || fail "ISO download failed"
fi
( cd "$WORK" && echo "$ISO_SHA512" | sha512sum -c - ) || fail "ISO checksum mismatch"
# The label is read from the artefact, not derived from its name: it is what
# the guest kernel searches for (CDLABEL=...), so a guess turns a label change
# into "the live root never appeared".
ISO_LABEL=$(sudo blkid -s LABEL -o value "$ISO" 2>/dev/null || true)
[ -n "$ISO_LABEL" ] || fail "could not read the ISO volume label (blkid)"
note "ISO label: $ISO_LABEL"
note "ISO verified: $(basename "$ISO")"

note "extracting kernel+initramfs from the ISO"
mkdir -p "$MNT"
sudo mount -o loop,ro "$ISO" "$MNT" || fail "loop-mount failed"
# The ISO's files are root-owned and not user-readable; copy as root,
# then take ownership (qemu runs as the user and must read them).
sudo cp "$MNT/$ISO_KERNEL" "$WORK/vmlinuz-gentoo" || { sudo umount "$MNT"; fail "no $ISO_KERNEL in ISO"; }
sudo cp "$MNT/$ISO_INITRD" "$WORK/initramfs-gentoo.img" || { sudo umount "$MNT"; fail "no $ISO_INITRD in ISO"; }
sudo umount "$MNT"
sudo chown "$(id -u):$(id -g)" "$WORK/vmlinuz-gentoo" "$WORK/initramfs-gentoo.img"

note "building the release artifact the guest will install"
( cd "$REPO" && make release >/dev/null 2>&1 ) || fail "make release failed"
ART=$(ls -t "$REPO"/dist/invfs-*-x86_64.tar.zst | head -1)
cp "$ART" "$REPO/dist/SHA256SUMS" "$REPO/tools/iso-guest-gentoo-setup.sh" "$SERVEDIR/"
ABASE=$(basename "$ART" .tar.zst)
# The guest script must never pin a version: point its artifact
# references at the tree under test (served copy only).
sed -i "s/invfs-v[0-9.][0-9.]*-x86_64/$ABASE/g" "$SERVEDIR/iso-guest-gentoo-setup.sh"
note "serving: $(ls "$SERVEDIR" | tr '\n' ' ')"

# Stage3 comes from the host server, not the guest's internet: a guest-side
# fetch failure is silent-or-cryptic (observed: curl rc=0 with no file,
# then a bare sha512sum miss), while the host path is loud and shares the
# already-proven NAT route. Resolved from the same upstream listing as the
# ISO, and the served guest script is re-pointed at the same artefact below
# -- otherwise the guest's own authoritative re-verify fails against a hash
# that rotated under it.
STAGE3_NAME=$(resolve_latest \
    "$GENTOO_BASE/current-stage3-amd64-openrc/latest-stage3-amd64-openrc.txt" \
    'stage3-amd64-openrc-[0-9]{8}T[0-9]{6}Z\.tar\.xz') || true
[ -n "${STAGE3_NAME:-}" ] || fail "could not resolve the current stage3 name"
STAGE3_VER="${STAGE3_NAME#stage3-amd64-openrc-}"; STAGE3_VER="${STAGE3_VER%.tar.xz}"
STAGE3_URL="$GENTOO_BASE/current-stage3-amd64-openrc/$STAGE3_NAME"
STAGE3_DIGEST=$(digest_sha512 "$STAGE3_URL.DIGESTS" "$STAGE3_NAME") || true
[ -n "${STAGE3_DIGEST:-}" ] || fail "no SHA512 for $STAGE3_NAME in its .DIGESTS"
# The host saves it as stage3.tar.xz, so the digest's name half must name the
# LOCAL file; the guest writes the upstream name and keeps the raw line.
STAGE3_SHA512="${STAGE3_DIGEST%%  *}  stage3.tar.xz"
note "upstream stage3: $STAGE3_NAME"
sed -i \
    -e "s|^STAGE3_VER=.*|STAGE3_VER=\"$STAGE3_VER\"|" \
    -e "s|^STAGE3_URL=.*|STAGE3_URL=\"$STAGE3_URL\"|" \
    -e "s|^STAGE3_SHA512=.*|STAGE3_SHA512=\"$STAGE3_DIGEST\"|" \
    "$SERVEDIR/iso-guest-gentoo-setup.sh"
if [ ! -f "$SERVEDIR/stage3.tar.xz" ]; then
    note "downloading stage3 $STAGE3_VER"
    fetch_verify "$STAGE3_URL" "$SERVEDIR/stage3.tar.xz" "$STAGE3_SHA512" || fail "stage3 download failed"
fi
( cd "$SERVEDIR" && echo "$STAGE3_SHA512" | sha512sum -c - ) || fail "stage3 checksum mismatch"
note "stage3 staged: $(du -h "$SERVEDIR/stage3.tar.xz" | cut -f1)"

if [ "$ACCEL" = auto ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL=kvm; CPU=host
    else ACCEL=tcg; CPU=max; note "/dev/kvm unusable; TCG fallback (slower)"; fi
fi
[ "$ACCEL" = kvm ] && CPU=${INVFS_CPU:-host} || CPU=${INVFS_CPU:-max}

truncate -s 12G "$VOL"
# Staging disk: ext4, LABEL=stage (same RAM-overlay reasoning as the
# void harness: tmpfs caps at half of RAM, a stage3 + cache overflows
# it; disk-backed staging removes RAM sizing from the equation).
truncate -s 8G "$STAGEDISK"
# mkfs lives in sbin/, which is not on every PATH -- resolve it.
if command -v mkfs.ext4 >/dev/null 2>&1; then MKFS_EXT4=mkfs.ext4
elif [ -x /sbin/mkfs.ext4 ]; then MKFS_EXT4=/sbin/mkfs.ext4
elif [ -x /usr/sbin/mkfs.ext4 ]; then MKFS_EXT4=/usr/sbin/mkfs.ext4
else fail "mkfs.ext4 not found (needs e2fsprogs)"; fi
"$MKFS_EXT4" -F -q -L stage "$STAGEDISK" || fail "mkfs.ext4 stage failed"
# Serial doubles as evidence AND as the fallback eyes: if SSH never comes
# up, the serial log is the only diagnosis. Pipe form keeps .out readable
# while running (file: only flushes at qemu exit).
rm -f "$SPIPE.in" "$SPIPE.out"
mkfifo "$SPIPE.in" "$SPIPE.out" 2>/dev/null || true
: > "$SER"
cat "$SPIPE.out" >>"$SER" &
# NOTE: no dummy writer here. Opening .in O_WRONLY blocks until a reader
# appears, and the only reader is qemu -- which starts below. Opening it
# here deadlocks the driver forever (nothing ever launches qemu). The
# writer opens after launch, mirroring iso-void-install.sh.
note "starting server on :$PORT"
bash "$REPO/tools/invfs-serve.sh" "$SERVEDIR" "$PORT" >"$SLOG" 2>&1 &
SRVPID=$!
cleanup() { kill $SRVPID 2>/dev/null; }
trap cleanup EXIT
sleep 1

note "starting qemu (accel=$ACCEL, dosshd, passwd set)"
# qemu's own stdout goes to a file, NOT the driver's: it inherits the
# driver's stdout pipe, and a live qemu would hold it open forever,
# hanging any downstream `| tail` (or CI log capture) past the run.
QLOG="$WORK/qemu.log"
qemu-system-x86_64 -machine q35,accel=$ACCEL -cpu "$CPU" -m "$MEM" -smp 2 \
  -kernel "$WORK/vmlinuz-gentoo" -initrd "$WORK/initramfs-gentoo.img" \
  -append "root=live:CDLABEL=$ISO_LABEL rd.live.dir=/ rd.live.squashimg=image.squashfs cdroot dosshd passwd=$GUEST_PW console=ttyS0,115200 ip=dhcp gentooisosearchuuid=$ISO_LABEL invfsvol=/dev/vda" \
  -cdrom "$ISO" \
  -drive "file=$VOL,format=raw,if=virtio" \
  -drive "file=$STAGEDISK,format=raw,if=virtio" \
  -netdev "user,id=net0,hostfwd=tcp::$SSHPORT-:22" -device virtio-net-pci,netdev=net0 \
  -vga none -serial "pipe:$SPIPE" -display none -no-reboot >"$QLOG" 2>&1 &
QPIDE=$!
# Dummy writer, only now that qemu has both pipe ends open (same order as
# iso-void-install.sh): qemu opens .in O_RDONLY and .out O_WRONLY, the cat
# above reads .out; this fd unblocks .in. Nothing is ever sent -- SSH is the
# control channel, serial is evidence. Opening it before launch deadlocks.
exec 9>"$SPIPE.in"
# The trap must kill qemu too: on FAIL the driver exits while the guest
# (a live shell) would otherwise idle forever, holding locks.
cleanup() { kill $QPIDE $SRVPID 2>/dev/null; exec 9>&- 2>/dev/null; }
note "qemu pid $QPIDE; waiting for SSH (up to 30 min)"
ssh_up() {
    sshpass -p "$GUEST_PW" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=10 -o BatchMode=no -p "$SSHPORT" root@localhost "echo SSHUP" 2>/dev/null | grep -q SSHUP
}
N=$(( 30 * 6 ))
up=0
for _ in $(seq 1 "$N"); do
    sleep 10
    kill -0 $QPIDE 2>/dev/null || { note "qemu exited early"; break; }
    if ssh_up; then up=1; break; fi
done
[ "$up" = 1 ] || fail "SSH never came up (see $SER)"
note "SSH up; running guest (up to $TIMEOUT_MIN min)"
sshpass -p "$GUEST_PW" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ServerAliveInterval=60 -p "$SSHPORT" root@localhost \
    "curl -fsS http://10.0.2.2:$PORT/iso-guest-gentoo-setup.sh | bash" >"$SSHLOG" 2>&1 &
SSHPID=$!
M=$(( TIMEOUT_MIN * 6 ))
for _ in $(seq 1 "$M"); do
    sleep 10
    grep -aq "INVFS-ISO-SETUP:" "$SER" 2>/dev/null && break
    kill -0 $QPIDE 2>/dev/null || { note "qemu exited early"; break; }
done
wait $SSHPID 2>/dev/null || true
grep -a "INVFS-ISO-SETUP:" "$SER" 2>/dev/null | tail -2
if grep -aq "INVFS-ISO-SETUP: PASS" "$SER" 2>/dev/null; then
    note "volume left at $VOL"
    exit 0
else
    fail "no PASS marker (see $SER $SSHLOG)"
fi
