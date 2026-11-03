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
https://dl-cdn.alpinelinux.org/alpine/v3.21/main
https://dl-cdn.alpinelinux.org/alpine/v3.21/community
EOF

# --- serial console: autologin root on ttyS0 ------------------------------
if [ -f "$E/inittab" ]; then
    # Replace default ttyS0 getty with autologin root shell
    python3 - "$E/inittab" <<'PYEOF'
import sys
path = sys.argv[1]
with open(path, "r") as f:
    lines = f.readlines()
new_lines = []
has_ttyS0 = False
for line in lines:
    if line.startswith("ttyS0::"):
        new_lines.append("ttyS0::respawn:/sbin/getty -L 115200 ttyS0 -a root\n")
        has_ttyS0 = True
    else:
        new_lines.append(line)
if not has_ttyS0:
    new_lines.append("ttyS0::respawn:/sbin/getty -L 115200 ttyS0 -a root\n")
with open(path, "w") as f:
    f.writelines(new_lines)
PYEOF
fi

# --- shadow / root password -----------------------------------------------
# Allow root login without password
if [ -f "$E/shadow" ]; then
    python3 - "$E/shadow" <<'PYEOF'
import sys
path = sys.argv[1]
with open(path, "r") as f:
    lines = f.readlines()
new_lines = []
for line in lines:
    if line.startswith("root:"):
        parts = line.split(":")
        parts[1] = "" # empty password
        new_lines.append(":".join(parts))
    else:
        new_lines.append(line)
with open(path, "w") as f:
    f.writelines(new_lines)
PYEOF
fi

# --- openrc runlevels -----------------------------------------------------
mkdir -p "$E/runlevels/sysinit" "$E/runlevels/boot" "$E/runlevels/default"
python3 - "$E" <<'PYEOF'
import os, sys
e = sys.argv[1]

def enable(level, svc):
    src = f"/etc/init.d/{svc}"
    dst = os.path.join(e, f"runlevels/{level}/{svc}")
    if not os.path.exists(dst):
        try:
            os.symlink(src, dst)
        except OSError:
            pass

for s in ("devfs", "dmesg", "mdev"):
    enable("sysinit", s)

for s in ("bootmisc", "hostname", "sysctl", "modules"):
    enable("boot", s)

for s in ("networking", "local"):
    enable("default", s)
PYEOF

# --- FUSE root guard in OpenRC --------------------------------------------
# In OpenRC /etc/init.d/root tries to remount / as rw. On FUSE this may
# return error or warning. Ensure OpenRC doesn't halt if remount is not permitted.
if [ -f "$E/init.d/root" ]; then
    python3 - "$E/init.d/root" <<'PYEOF'
import sys
path = sys.argv[1]
with open(path, "r") as f:
    content = f.read()
if "mount -o remount" in content and "|| true" not in content:
    content = content.replace("mount -o remount,rw /", "mount -o remount,rw / 2>/dev/null || true")
    with open(path, "w") as f:
        f.write(content)
PYEOF
fi

echo "configure-alpine: configured $M for InvariantFS"
