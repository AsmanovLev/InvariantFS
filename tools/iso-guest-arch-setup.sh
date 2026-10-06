#!/bin/bash
# iso-guest-arch-setup.sh -- runs INSIDE a stock Arch live env.
#
# Two front ends, one script (no $0 tricks, no prompts, config from env):
#   archiso hook:  script=http://10.0.2.2:8000/iso-guest-arch-setup.sh
#   by hand / TTY: curl -fsS http://10.0.2.2:8000/iso-guest-arch-setup.sh | bash
#                  (or download whole, verify out-of-band, then run)
# SRV and VOL override the defaults below when exported first.
#
# Realistic install: makepkg the recipe + pacman -U (not loose binaries),
# then image with the INSTALLED tools. All output to serial; the host
# greps for the INVFS-ISO-SETUP marker. Ends by powering the guest off.
exec >/dev/console 2>&1
set -u
SRV="${SRV:-http://10.0.2.2:8000}"
VOL=""   # resolved in phase 2: invfsvol= cmdline, else only writable disk
B=/home/builder/pkg
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
    # Graceful first (flushes the imaged volume), forced fallback: a bare
    # `systemctl poweroff` has hung from this hook, leaving a stale guest
    # holding the volume lock and confusing the next attempt.
    timeout 30 systemctl poweroff || poweroff -f
}
trap 'done_rc=$?; finish' EXIT

# ---- phase 1: build + install the package, like a user would ----
# Watchdog: heartbeat + memory to console every 60 s. If the guest dies
# silently (OOM takes the shell: no trap, no marker), the serial shows
# exactly when the heart stopped and what pressure preceded it.
( while true; do
    echo "iso-watch: mem=$(free -m | awk '/^Mem/{print $3"/"$2"MB"}') load=$(cut -d' ' -f1 /proc/loadavg)"
    sleep 60
  done ) &
id builder >/dev/null 2>&1 || useradd -m builder
mkdir -p "$B"
for f in PKGBUILD invfs-v0.5.0-x86_64.tar.zst SHA256SUMS; do
    curl -fsS -o "$B/$f" "$SRV/$f" || exit 1
done
say "recipe + artifact fetched"
# Provenance FIRST: the artifact must match the served checksums before
# anything trusts it. (The published chain adds a signature on top;
# test staging cannot sign, so the checksum file is the root here.)
( cd "$B" && sha256sum -c SHA256SUMS ) || exit 1
say "artifact matches SHA256SUMS"
# Test-staging pin: the recipe pins the PUBLISHED hash, but this artifact
# was built from HEAD minutes ago. Re-pin this COPY to the verified hash so
# makepkg enforces a real (matching) checksum instead of --skipchecksums.
H=$(cut -d' ' -f1 < "$B/SHA256SUMS")
sed -i "s/'[0-9a-f]\{64\}'/'$H'/" "$B/PKGBUILD"
grep -q "'  " "$B/PKGBUILD" && { say "PKGBUILD sums array has a non-hash entry"; exit 1; }
# Build tooling, minimal: makepkg needs fakeroot (ownership) but, with
# options=('!strip' '!debug') in the recipe, no binutils/base-devel. The live
# ISO root is a RAM overlay -- a full base-devel does not fit next to the
# pacstrap target.
command -v fakeroot >/dev/null 2>&1 || pacman -Sy --needed --noconfirm fakeroot || exit 1
chown -R builder:builder "$B"
su builder -c "cd '$B' && makepkg --noconfirm" || exit 1
PKG=$(ls "$B"/invfs-*.pkg.tar.zst | head -1)
pacman -U --noconfirm "$PKG" || exit 1
pacman -Q invfs || exit 1
pacman -Qk invfs 2>&1 | tail -2
say "installed: $(pacman -Q invfs)"

# ---- phase 2: image with the INSTALLED tools ----
# The volume disk: named on the kernel cmdline (invfsvol=/dev/vda) so the
# recipe never guesses wrong; falls back to the only writable disk (the
# ISO itself is read-only).
VOL="$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^invfsvol=//p' | head -1)"
if [ -z "$VOL" ]; then
    VOL=$(lsblk -dn -o NAME,RO,TYPE | awk '$2=="0" && $3=="disk" {print "/dev/"$1}' | head -1)
fi
[ -b "${VOL:-none}" ] || { say "no volume disk (cmdline: $(tr ' ' '\n' < /proc/cmdline | grep -o 'invfsvol=[^ ]*' || echo none); disks: $(lsblk -dn -o NAME,RO | tr '\n' ' '))"; exit 1; }
say "pacstrap base + linux to /mnt (native Arch install)"
pacstrap /mnt base linux || exit 1
say "pacstrap done: $(du -sh /mnt | cut -f1) in /mnt"
invf-mkfs "$VOL" 8 || exit 1
# Bulk-import profile from docs/benchmarks/commit-policy.md: the default
# commit intervals are conservative (live-root safe); an install may batch
# aggressively because the whole tree is verified right after.
export INVFS_COMMIT_BYTES=16 INVFS_COMMIT_MS=5000 INVFS_COMMIT_IDLE_MS=1000
say "import /mnt -> $VOL (batched)"
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
invf-import "$VOL" /mnt || exit 1
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
# Fully synchronous reads: invf-cat writes a temp FILE and exits (lock
# released) before cmp starts. The old process-substitution form let a
# mismatch-exited cmp orphan a still-running cat, whose held lock failed
# the NEXT try with 'in use by another process'.
# Retries stay (a genuine transient still shouldn't fail the install)
# and no invf-cat stderr is ever discarded.
for f in etc/pacman.conf boot/vmlinuz-linux; do
    ok=0
    for r in 1 2 3; do
        if invf-cat "$VOL" "$f" "$B/out" 2>"$B/cat-$r.err" && cmp -s "/mnt/$f" "$B/out"; then
            ok=$r; break
        fi
        say "try $r FAILED for $f: $(head -c 200 "$B/cat-$r.err" | tr '\n' '|')"
        sleep 2
    done
    if [ "$ok" != 0 ] && [ "$ok" != 1 ]; then say "FLAKY-READ: $f passed on try $ok (read-path finding, install continues)"; fi
    if [ "$ok" = 0 ]; then
        say "MISMATCH: $f (3 tries, first errors above)"
        say "source: $(md5sum < "/mnt/$f")"
        exit 1
    fi
done
say "spot bit-exact OK"
NFILES=$(find /mnt -type f | wc -l)
STATS="pkg=$(pacman -Q invfs) files=$NFILES src=$(du -sb /mnt | cut -f1)B"
done_rc=0
