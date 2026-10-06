#!/bin/bash
# iso-guest-void-setup.sh -- runs INSIDE a stock Void live env, chrooted
# into the live root from the dracut break shell (see iso-void-install.sh).
#
# Two front ends, one script (no prompts, config from env):
#   dracut break: chroot /sysroot /bin/bash -c 'curl -fsS \
#                   http://10.0.2.2:8001/iso-guest-void-setup.sh | bash'
#   by hand / TTY: curl -fsS http://10.0.2.2:8001/iso-guest-void-setup.sh | bash
# SRV and VOL override the defaults below when exported first.
#
# Realistic install per the Handbook's XBPS method (docs.voidlinux.org,
# "Installation via chroot"): xbps-install -r the base system, then image
# with the release artifact's install.sh. No void-installer TUI is driven
# anywhere -- the installer is not what's under test, the resulting root
# on InvFS is. All output to console; the host greps for the
# INVFS-ISO-SETUP marker. Ends by powering the guest off.
exec >/dev/console 2>&1
set -u
SRV="${SRV:-http://10.0.2.2:8001}"
VOL=""   # resolved below: invfsvol= cmdline, else only writable disk
STAGE=/stage-invfs
B=/root/pkg
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

# Fetch tool: curl preferred, then wget, then xbps-fetch (ships in the
# Void live env, which has neither curl nor wget nor python3). xbps-fetch
# saves under the remote basename in the cwd, so download beside the
# destination and rename.
fetch() { # fetch <url> <dest>
    if command -v curl >/dev/null 2>&1; then
        curl -fsS -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    elif command -v xbps-fetch >/dev/null 2>&1; then
        ( cd "$(dirname "$2")" && xbps-fetch "$1" ) || return 1
        [ "$(dirname "$2")/$(basename "$1")" = "$2" ] \
            || mv "$(dirname "$2")/$(basename "$1")" "$2"
    else
        python3 -c 'import sys,urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$1" "$2"
    fi
}

# ---- phase 0: network (dracut ip=dhcp should have it; make sure) ----
if ! fetch "$SRV/iso-guest-void-setup.sh" /dev/null; then
    say "no route to serve dir; trying dhcpcd"
    dhcpcd -w 30 2>/dev/null || udhcpc -i eth0 2>/dev/null || true
    fetch "$SRV/iso-guest-void-setup.sh" /dev/null || exit 1
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
# zstd is NOT in the base live root; tar --zstd needs it. Install from
# the Void repos (also proves guest package management works this early).
xbps-install -Sy zstd || exit 1
mkdir -p "$B/x" && tar --zstd -xf "$B/invfs-v0.5.0-x86_64.tar.zst" -C "$B/x" || exit 1
( cd "$B/x/invfs-v0.5.0-x86_64" && DESTDIR=/ PREFIX=/usr sh packaging/install.sh ) || exit 1
command -v invf-mkfs >/dev/null 2>&1 || exit 1
say "installed: $(invf-mkfs --version 2>&1 | head -1)"

# ---- phase 2: install Void per the Handbook XBPS method ----
# The volume disk: named on the kernel cmdline (invfsvol=/dev/vda) so the
# script never guesses wrong; falls back to the only writable disk (the
# ISO itself is read-only).
VOL="$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^invfsvol=//p' | head -1)"
if [ -z "$VOL" ]; then
    VOL=$(lsblk -dn -o NAME,RO,TYPE | awk '$2=="0" && $3=="disk" {print "/dev/"$1}' | head -1)
fi
[ -b "${VOL:-none}" ] || { say "no volume disk (cmdline: $(tr ' ' '\n' < /proc/cmdline | grep -o 'invfsvol=[^ ]*' || echo none); disks: $(lsblk -dn -o NAME,RO | tr '\n' ' '))"; exit 1; }
mkdir -p "$STAGE"
say "xbps-install -r $STAGE base-system (Handbook XBPS method)"
xbps-install -r "$STAGE" -Sy base-system || exit 1
say "staged: $(du -sh "$STAGE" | cut -f1) in $STAGE"
invf-mkfs "$VOL" 8 || exit 1
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
# --follow: merged-usr symlinks (etc/os-release -> ../usr/lib/os-release)
# must resolve to the file, like the FUSE read does.
# Fully synchronous reads: invf-cat writes a temp FILE and exits (lock
# released) before cmp starts. Retries stay (a genuine transient still
# shouldn't fail the install) and no invf-cat stderr is ever discarded.
mkdir -p "$B"
for f in etc/passwd etc/os-release usr/lib/os-release bin/sh usr/bin/xbps-install; do
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
