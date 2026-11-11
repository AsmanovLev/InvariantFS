#!/bin/sh
# Configure an Alpine Linux root tree for the InvariantFS VM.
# Usage: configure-alpine.sh /path/to/root
#
# May be run on an ordinary staging directory or a host-side FUSE mount.
# FUSE-portable rules:
#   * rewrite existing files in place (`> file`), never `sed -i` (needs rename);
#   * create symlinks without assuming `ln -sf` will rename an existing file;
#   * never rely on rename(2) for anything the guest will boot.
#
# Alpine uses OpenRC: services are enabled by symlinking into /etc/runlevels/<level>/.
set -e
M="${1:?usage: configure-alpine.sh <root>}"
E="$M/etc"
[ -d "$E" ] || { echo "configure-alpine: no $E in $M" >&2; exit 1; }

# --- hostname -------------------------------------------------------------
printf 'alpine-invfs\n' > "$E/hostname"

# --- fstab ----------------------------------------------------------------
cat > "$E/fstab" <<'EOF'
proc		/proc		proc	defaults	0 0
sysfs		/sys		sysfs	defaults	0 0
tmpfs		/tmp		tmpfs	defaults,noatime,mode=1777,size=512M	0 0
tmpfs		/run		tmpfs	mode=0755,nosuid,nodev	0 0
EOF

# --- network interfaces ---------------------------------------------------
mkdir -p "$E/network"
cat > "$E/network/interfaces" <<'EOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
EOF

# --- dns resolver ---------------------------------------------------------
printf 'nameserver 10.0.2.3\nnameserver 1.1.1.1\n' > "$E/resolv.conf"

# --- apk repositories -----------------------------------------------------
mkdir -p "$E/apk"
cat > "$E/apk/repositories" <<'EOF'
https://dl-cdn.alpinelinux.org/alpine/.24/main
https://dl-cdn.alpinelinux.org/alpine/.24/community
EOF

# --- serial console: autologin root on ttyS0 ------------------------------
if [ -f "$E/inittab" ]; then
    # Replace default ttyS0 getty with autologin root shell
    awk '
        BEGIN { found = 0 }
        /^ttyS0::/ {
            print "ttyS0::respawn:/sbin/getty -L 115200 ttyS0 -a root"
            found = 1
            next
        }
        { print }
        END {
            if (!found) {
                print "ttyS0::respawn:/sbin/getty -L 115200 ttyS0 -a root"
            }
        }
    ' "$E/inittab" > "$E/inittab.tmp" && mv "$E/inittab.tmp" "$E/inittab"
fi

# --- shadow / root password -----------------------------------------------
# Allow root login without password
if [ -f "$E/shadow" ]; then
    awk -F: -v OFS=: '
        /^root:/ { $2 = ""; print; next }
        { print }
    ' "$E/shadow" > "$E/shadow.tmp" && mv "$E/shadow.tmp" "$E/shadow"
fi

# --- openrc runlevels -----------------------------------------------------
mkdir -p "$E/runlevels/sysinit" "$E/runlevels/boot" "$E/runlevels/default"
for s in devfs dmesg mdev; do
    [ -e "$E/runlevels/sysinit/$s" ] || ln -sf "/etc/init.d/$s" "$E/runlevels/sysinit/$s" 2>/dev/null || true
done
for s in bootmisc hostname sysctl modules; do
    [ -e "$E/runlevels/boot/$s" ] || ln -sf "/etc/init.d/$s" "$E/runlevels/boot/$s" 2>/dev/null || true
done
for s in networking local; do
    [ -e "$E/runlevels/default/$s" ] || ln -sf "/etc/init.d/$s" "$E/runlevels/default/$s" 2>/dev/null || true
done

# --- FUSE root guard in OpenRC --------------------------------------------
# In OpenRC /etc/init.d/root tries to remount / as rw. On FUSE this may
# return error or warning. Ensure OpenRC doesn't halt if remount is not permitted.
if [ -f "$E/init.d/root" ]; then
    awk '
        /mount -o remount,rw \// && !/\|\| true/ {
            sub(/mount -o remount,rw \//, "mount -o remount,rw / 2>/dev/null || true")
        }
        { print }
    ' "$E/init.d/root" > "$E/init.d/root.tmp" && mv "$E/init.d/root.tmp" "$E/init.d/root"
fi

echo "configure-alpine: configured $M for InvariantFS"
