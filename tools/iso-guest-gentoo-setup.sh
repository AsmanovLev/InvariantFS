#!/bin/bash
# iso-guest-gentoo-setup.sh -- runs INSIDE a stock Gentoo minimal live env
# over SSH (driver boots dosshd + passwd= and runs this one command).
#
# Two front ends, one script (no prompts, config from env):
#   ssh: curl -fsS http://10.0.2.2:8002/iso-guest-gentoo-setup.sh | bash
#   by hand / TTY: same curl pipe on the live console
# SRV and VOL override the defaults below when exported first.
#
# Realistic install per the Handbook (which IS the CLI flow on Gentoo --
# there is no installer TUI): unpack a pinned stage3, minimal config,
# image with the release artifact's install.sh. All output to console;
# the host greps the serial for the INVFS-ISO-SETUP marker. Ends by
# powering the guest off.
exec >/dev/console 2>&1
set -u
SRV="${SRV:-http://10.0.2.2:8002}"
VOL=""   # resolved below: invfsvol= cmdline, else only writable disk
B=/root/pkg
# Pinned stage3 (policy: reproducible inputs; bump with checksum together).
STAGE3_VER="20261004T164559Z"
STAGE3_URL="https://distfiles.gentoo.org/releases/amd64/autobuilds/current-stage3-amd64-openrc/stage3-amd64-openrc-$STAGE3_VER.tar.xz"
STAGE3_SHA512="2cf0030d683481ef9a5aac737b636abb3532c88a15972709811423564945ef8694685003dccb5c78e453633b792af2af27c504f0a0e69101bd472b26d892aea8  stage3-amd64-openrc-$STAGE3_VER.tar.xz"
say() { echo "iso-setup: $*"; }
done_rc=1
STATS=""
finish() {
    if [ "$done_rc" = 0 ]; then
        echo "INVFS-ISO-SETUP: PASS $STATS"
    else
        echo "INVFS-ISO-SETUP: FAIL rc=$done_rc"
    fi
    sleep 2
    # Graceful first (flushes the imaged volume), forced fallback.
    timeout 60 poweroff || poweroff -f
}
trap 'done_rc=$?; finish' EXIT

# ---- phase 0: network (dracut ip=dhcp should have it; make sure) ----
fetch() { # fetch <url> <dest>
    if command -v curl >/dev/null 2>&1; then
        curl -fsS -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    else
        python3 -c 'import sys,urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$1" "$2"
    fi
}
if ! fetch "$SRV/iso-guest-gentoo-setup.sh" /dev/null; then
    say "no route to serve dir; trying dhcpcd"
    dhcpcd -w 30 2>/dev/null || udhcpc -i eth0 2>/dev/null || true
    fetch "$SRV/iso-guest-gentoo-setup.sh" /dev/null || exit 1
fi
say "serve dir reachable"

# ---- phase 1: install the InvFS tools from the release artifact ----
# Watchdog: heartbeat + memory to console every 60 s. If the guest dies
# silently (OOM takes the shell: no trap, no marker), the serial shows
# exactly when the heart stopped and what pressure preceded it.
( while true; do
    echo "iso-watch: mem=$(free -m | awk '/^Mem/{print $3"/"$2"MB"}') load=$(cut -d' ' -f1 /proc/loadavg)"
    sleep 60
  done ) &
mkdir -p "$B"
for f in invfs-v0.5.0-x86_64.tar.zst SHA256SUMS; do
    fetch "$SRV/$f" "$B/$f" || exit 1
done
say "artifact fetched"
# Provenance FIRST: the artifact must match the served checksums before
# anything trusts it.
( cd "$B" && sha256sum -c SHA256SUMS ) || exit 1
say "artifact matches SHA256SUMS"
mkdir -p "$B/x" && tar --zstd -xf "$B/invfs-v0.5.0-x86_64.tar.zst" -C "$B/x" || exit 1
( cd "$B/x/invfs-v0.5.0-x86_64" && DESTDIR=/ PREFIX=/usr sh packaging/install.sh ) || exit 1
command -v invf-mkfs >/dev/null 2>&1 || exit 1
say "installed: $(invf-mkfs --version 2>&1 | head -1)"

# ---- phase 2: install Gentoo per the Handbook (stage3, no kernel) ----
# The volume disk is named on the kernel cmdline (invfsvol=/dev/vda).
# Staging is disk-backed (LABEL=stage), not the RAM overlay: a stage3
# unpack overflows tmpfs (capped at half of RAM).
VOL="$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^invfsvol=//p' | head -1)"
[ -b "${VOL:-none}" ] || { say "no volume disk (cmdline has no invfsvol=)"; exit 1; }
STAGE=/stage
mkdir -p "$STAGE"
mount -L stage "$STAGE" || mount /dev/disk/by-label/stage "$STAGE" || exit 1
say "staged on disk-backed $STAGE (not the RAM overlay)"
say "stage3 $STAGE3_VER -> $STAGE"
fetch "$STAGE3_URL" "$B/stage3.tar.xz" || exit 1
( cd "$B" && echo "$STAGE3_SHA512" | sha512sum -c - ) || exit 1
say "stage3 checksum OK"
tar -xpf "$B/stage3.tar.xz" -C "$STAGE" || exit 1
# Minimal config for an offline-verified root: hostname only. No kernel,
# no bootloader, no users -- this job proves installation onto InvFS,
# not bootability (the bootstrap-qemu jobs own boot).
printf 'invfs-gentoo\n' > "$STAGE/etc/hostname"
say "staged: $(du -sh "$STAGE" | cut -f1) in $STAGE"
invf-mkfs "$VOL" 12 || exit 1
# Bulk-import profile from docs/benchmarks/commit-policy.md: the default
# commit intervals are conservative (live-root safe); an install may batch
# aggressively because the whole tree is verified right after.
export INVFS_COMMIT_BYTES=16 INVFS_COMMIT_MS=5000 INVFS_COMMIT_IDLE_MS=1000
say "import $STAGE -> $VOL (batched)"
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
invf-import "$VOL" "$STAGE" || exit 1
unset INVFS_COMMIT_BYTES INVFS_COMMIT_MS INVFS_COMMIT_IDLE_MS
# Lock-holder snapshot: if fsck below reports 'in use by another process',
# this names the holder.
say "holders of $VOL:"
ls -l /proc/[0-9]*/fd 2>/dev/null | grep -a "$(basename "$VOL")$" || say "(none visible)"
invf-fsck "$VOL" || exit 1

# ---- phase 3: asserts ----
# Paths are slashless by repo convention (all harnesses call invf-cat with
# etc/passwd, not /etc/passwd); a leading slash reads nothing and cmp then
# reports a MISMATCH that is really a lookup miss.
# --follow resolves symlinks to the file, like the FUSE read does.
# Fully synchronous reads: invf-cat writes a temp FILE and exits (lock
# released) before cmp starts. Retries stay (a genuine transient still
# shouldn't fail the install) and no invf-cat stderr is ever discarded.
mkdir -p "$B"
for f in etc/os-release etc/gentoo-release etc/passwd bin/bash usr/bin/emerge; do
    [ -f "$STAGE/$f" ] || continue
    ok=0
    for r in 1 2 3; do
        if invf-cat --follow "$VOL" "$f" "$B/out" 2>"$B/cat-$r.err" && cmp -s "$STAGE/$f" "$B/out"; then
            ok=$r; break
        fi
        say "try $r FAILED for $f: $(head -c 200 "$B/cat-$r.err" | tr '\n' '|')"
        sleep 2
    done
    if [ "$ok" != 0 ] && [ "$ok" != 1 ]; then say "FLAKY-READ: $f passed on try $ok (read-path finding, install continues)"; fi
    if [ "$ok" = 0 ]; then
        say "MISMATCH: $f (3 tries, first errors above)"
        say "source: $(md5sum < "$STAGE/$f")"
        exit 1
    fi
done
say "spot bit-exact OK"
NFILES=$(find "$STAGE" -type f | wc -l)
STATS="files=$NFILES src=$(du -sb "$STAGE" | cut -f1)B"
done_rc=0
