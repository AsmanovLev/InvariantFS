#!/bin/sh
# Configure a Void Linux root tree for the InvariantFS VM.
# Usage: configure-void.sh /path/to/root
#
# WP64: the argument may be either an ordinary staging directory (the
# preferred path: provision before `invf-import`) or a host-side FUSE
# mount of an already-imported volume.  FUSE-portable rules:
#   * rewrite existing files IN PLACE (`> file`, which truncates and keeps
#     the same inode/mode/owner); never `sed -i`, which needs rename(2);
#   * create symlinks with os.symlink (create), never `ln -sf` (rename);
#   * never rely on rename(2) for anything the guest will boot.
#
# Void uses runit: a service is enabled by symlinking /etc/sv/<name> into
# /etc/runit/runsvdir/default/.
set -e
M="${1:?usage: configure-void.sh <root>}"
E="$M/etc"
[ -d "$E" ] || { echo "configure-void: no $E in $M" >&2; exit 1; }
[ -e "$M/sbin/init" ] || { echo "configure-void: $M is not a Void root (no sbin/init)" >&2; exit 1; }

ROOT_PASS="${INVFS_VOID_ROOT_PASS:-root}"

# --- hostname -------------------------------------------------------------
printf 'void-invfs\n' > "$E/hostname"

# --- fstab ----------------------------------------------------------------
# The FUSE root cannot be remounted and is already mounted; only the
# pseudo-filesystems and (optional) tmpfs scratch areas are listed.  On a
# FUSE root runit's 03-filesystems is skipped (see the patch below), so
# /tmp mounts only if something else runs `mount -a`; keeping it here is
# harmless and correct for a real block-device install.
cat > "$E/fstab" <<'EOF'
proc	/proc	proc	defaults	0 0
sysfs	/sys	sysfs	defaults	0 0
tmpfs	/tmp	tmpfs	defaults,noatime,mode=1777,size=1G	0 0
tmpfs	/var/tmp	tmpfs	defaults,noatime,mode=1777,size=512M	0 0
EOF

# --- serial console: autologin root on ttyS0 ------------------------------
# agetty-ttyS0 sources ../agetty-serial/conf; set autologin (-a root).
# `/sbin/agetty` is util-linux in Void (supports -a).
cat > "$E/sv/agetty-serial/conf" <<'EOF'
GETTY_ARGS="-L -8 -a root"
BAUD_RATE=115200
TERM_NAME=vt100
EOF

# --- runit services -------------------------------------------------------
# python os.symlink is used because `ln -sf` replaces via rename(2), which
# older InvFS FUSE builds reject.  Idempotent: an existing correct link is
# left alone, a wrong one is unlinked and recreated.
python3 - "$E/runit/runsvdir/default" <<'PYEOF'
import os, sys
d = sys.argv[1]
os.makedirs(d, exist_ok=True)
for name, target in (("agetty-ttyS0", "/etc/sv/agetty-ttyS0"),
                     ("dhcpcd",      "/etc/sv/dhcpcd"),
                     ("sshd",        "/etc/sv/sshd")):
    p = os.path.join(d, name)
    if os.path.islink(p):
        if os.readlink(p) == target:
            continue
        os.unlink(p)
    elif os.path.lexists(p):
        print("configure-void: %s exists and is not a symlink; left alone" % p,
              file=sys.stderr)
        continue
    os.symlink(target, p)
PYEOF

# --- sshd: root login, password auth --------------------------------------
# Void's sshd_config ends with `Include /etc/ssh/sshd_config.d/*.conf`.
mkdir -p "$E/ssh/sshd_config.d"
cat > "$E/ssh/sshd_config.d/99-invfs.conf" <<'EOF'
PermitRootLogin yes
PasswordAuthentication yes
UseDNS no
EOF
chmod 644 "$E/ssh/sshd_config.d/99-invfs.conf"

# --- sshd host keys (pre-generate; copied in, never renamed) --------------
command -v ssh-keygen >/dev/null 2>&1 || {
    echo "configure-void: ssh-keygen not found; cannot create host keys" >&2
    exit 1
}
KEYTMP=$(mktemp -d)
trap 'rm -rf "$KEYTMP"' EXIT INT TERM
for kt in rsa ecdsa ed25519; do
    key="$E/ssh/ssh_host_${kt}_key"
    [ -s "$key" ] && continue
    rm -f "$KEYTMP/ssh_host_${kt}_key" "$KEYTMP/ssh_host_${kt}_key.pub"
    ssh-keygen -q -t "$kt" -N '' -f "$KEYTMP/ssh_host_${kt}_key" >/dev/null
    cp "$KEYTMP/ssh_host_${kt}_key"     "$key"
    cp "$KEYTMP/ssh_host_${kt}_key.pub" "$key.pub"
done
chmod 600 "$E"/ssh/ssh_host_*_key 2>/dev/null || true
chmod 644 "$E"/ssh/ssh_host_*_key.pub 2>/dev/null || true

# --- root password --------------------------------------------------------
# Rewrite /etc/shadow in place (same inode).  `openssl passwd -6` emits a
# SHA-512 crypt hash accepted by glibc; fall back to python crypt.
if command -v openssl >/dev/null 2>&1; then
    HASH=$(openssl passwd -6 "$ROOT_PASS")
else
    HASH=$(python3 - "$ROOT_PASS" <<'PYEOF'
import sys, crypt, secrets
print(crypt.crypt(sys.argv[1], "$6$" + secrets.token_hex(8)))
PYEOF
)
fi
[ -n "$HASH" ] || { echo "configure-void: could not hash root password" >&2; exit 1; }
# /etc/shadow ships mode 0400; the extracting user owns it but cannot write
# it until u+w is added (chmod is allowed on one's own file).
chmod u+w "$E/shadow" 2>/dev/null || true
python3 - "$E/shadow" "$HASH" <<'PYEOF'
import sys
p, h = sys.argv[1], sys.argv[2]
try:
    s = open(p, encoding="utf-8", errors="surrogateescape").read()
except FileNotFoundError:
    s = ""
lines = s.splitlines(True)
out, done = [], False
for ln in lines:
    if ln.startswith("root:"):
        f = ln.split(":", 2)
        ln = "root:" + h + ":" + (f[2] if len(f) > 2 else "\n")
        done = True
    out.append(ln)
if not done:
    out.append("root:" + h + ":0:0:99999:7:::\n")
open(p, "w", encoding="utf-8", errors="surrogateescape").write("".join(out))
PYEOF
chmod 600 "$E/shadow" 2>/dev/null || true

# --- FUSE-root boot fix (runit core-services) -----------------------------
# 03-filesystems.sh unconditionally does
#     mount -o remount,ro /   || emergency_shell
#     fsck -A ...             (~fails on a FUSE root)
#     mount -a
# On a FUSE root the remount fails (util-linux re-enters /sbin/mount.fuse,
# which treats the bracketed source "invfs[vol]" as a helper program) and
# boot drops to an emergency shell before any service starts.  Detect a
# fuse root and skip the block.  Idempotent: only prepend once.
if [ -f "$E/runit/core-services/03-filesystems.sh" ]; then
    python3 - "$E/runit/core-services/03-filesystems.sh" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8", errors="surrogateescape").read()
marker = "# WP64: FUSE root"
if marker not in s:
    guard = '''# WP64: FUSE root -- mount.fuse cannot remount (it re-execs the
# bracketed source as a helper) and fsck(8) has no FUSE handler, so the
# root fsck/remount/mount -a block below would drop to emergency_shell.
if grep -q ' / fuse ' /proc/mounts 2>/dev/null; then
    msg "FUSE root detected; skipping root fsck/remount and mount -a"
    return 0
fi
'''
    s = s.replace("[ -n \"$VIRTUALIZATION\" ] && return 0\n",
                  "[ -n \"$VIRTUALIZATION\" ] && return 0\n\n" + guard, 1)
    open(p, "w", encoding="utf-8", errors="surrogateescape").write(s)
PYEOF
fi

echo "void root configured at $M (root password: ${ROOT_PASS})"
