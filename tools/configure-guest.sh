#!/bin/sh
# Configure a Gentoo rootfs volume for the InvariantFS VM.
# Usage: configure-guest.sh /path/to/fuse/mountpoint
# Run while the volume is FUSE-mounted (host side).
#
# WP54: FUSE-portable provisioning.  The volume is edited through a
# host-side FUSE mount, where two POSIX operations are unavailable:
#   * rename(2) returns EBUSY/EPERM -- so `sed -i` (temp file + rename)
#     silently leaves the target untouched, and `ln -sf` cannot replace a
#     link.  Every edit below therefore rewrites the SAME inode in place
#     (open("w")/`> file`, which keeps the original mode and owner) and
#     symlinks are created with os.symlink (create, never rename).
#   * new files are owned by the mount user (uid 1000), not root -- so the
#     ssh material is chown'd to root:root afterwards, otherwise sshd with
#     StrictModes yes refuses /root/.ssh/authorized_keys (owner 1000).
set -e
M="${1:?mountpoint}"
E="$M/etc"

mkdir -p "$E"

# --- binhost + gpg stub + features ---
mkdir -p "$E/portage"
if grep -q '^# --- InvariantFS VM ---$' "$E/portage/make.conf" 2>/dev/null; then
    : # already provisioned; keep the run idempotent
else
    cat >> "$E/portage/make.conf" <<'EOF'

# --- InvariantFS VM ---
BINHOST="https://distfiles.gentoo.org/releases/amd64/binpackages/23.0/x86-64-v3/"
PORTAGE_BINHOST="${BINHOST}"
EMERGE_DEFAULT_OPTS="${EMERGE_DEFAULT_OPTS} --usepkg --getbinpkg"
FEATURES="${FEATURES} -news -pid-sandbox -userpriv -sandbox"
BINPKG_GPG_VERIFY_BASE_COMMAND="/usr/local/bin/fake-gpg-verify [PORTAGE_CONFIG] [SIGNATURE]"
EOF
fi

# --- fake gpg verifier (real gpg needs mmap; WP4) ---
mkdir -p "$M/usr/local/bin"
cat > "$M/usr/local/bin/fake-gpg-verify" <<'EOF'
#!/bin/sh
# InvariantFS VM stub: real gpg asserts on our FUSE (no mmap yet).
inp=$(cat)
{
  printf '[GNUPG:] GOODSIG 0000000000000000 Gentoo Binhost (stub)\n'
  printf '[GNUPG:] TRUST_ULTIMATE\n'
} >&2
case " $* " in
  *" --output "*)
    printf '%s\n' "$inp" | awk '
      /^-----BEGIN PGP SIGNED MESSAGE-----/ { s=1; next }
      done { next }
      /^-----BEGIN PGP SIGNATURE-----/ { done=1; next }
      s && /^$/ && !c { c=1; next }
      c { print }'
    ;;
esac
exit 0
EOF
chmod 755 "$M/usr/local/bin/fake-gpg-verify"

# --- DNS for chroot networking ---
cp /etc/resolv.conf "$E/resolv.conf" 2>/dev/null || true

# --- fstab: protect root from netmount early-unmount at shutdown ---
printf '/dev/vda\t/\tinvfs\tnoatime\t0 1\ntmpfs\t\t/run\ttmpfs\tmode=0755,nosuid,nodev\t0 0\n' > "$E/fstab"

# --- hostname, root passwordless login ---
echo "invarifs-vm" > "$E/hostname"

# In-place edit helper: FUSE rejects rename(2), so python reads the file,
# transforms it, then truncates and rewrites the same inode.  Mode/owner
# are preserved because no new inode is created.
if [ -f "$E/shadow" ]; then
    python3 - "$E/shadow" <<'PYEOF'
import re, sys
p = sys.argv[1]
s = open(p, encoding="utf-8", errors="surrogateescape").read()
s = re.sub(r"(?m)^root:[^:]*:", "root::", s)
open(p, "w", encoding="utf-8", errors="surrogateescape").write(s)
PYEOF
fi

# --- serial console getty ---
if [ -f "$E/inittab" ]; then
    python3 - "$E/inittab" <<'PYEOF'
import re, sys
p = sys.argv[1]
s = open(p, encoding="utf-8", errors="surrogateescape").read()
if not re.search(r"(?m)^s0:", s):
    if s and not s.endswith("\n"):
        s += "\n"
    s += "s0:12345:respawn:/sbin/agetty -L 115200 ttyS0 vt100\n"
s = re.sub(r"(?m)^c([1-6]):", r"##c\1:", s)
open(p, "w", encoding="utf-8", errors="surrogateescape").write(s)
PYEOF
fi

# --- sshd: root + empty password (test VM only) ---
mkdir -p "$E/ssh/sshd_config.d"
printf 'PermitRootLogin yes\nPermitEmptyPasswords yes\n' > "$E/ssh/sshd_config.d/99-invfs.conf"

# --- sshd host keys (rsa/ecdsa/ed25519) ---
# Generate on the host -- a fresh import has none -- and copy the bytes in.
# ssh-keygen may create its output via temp+rename, which the FUSE mount
# rejects, so generate in a host scratch dir and `cp` across (create).
command -v ssh-keygen >/dev/null 2>&1 || {
    echo "configure-guest: ssh-keygen not found; cannot create host keys" >&2
    exit 1
}
GENTMP=$(mktemp -d)
trap 'rm -rf "$GENTMP"' EXIT
for kt in rsa ecdsa ed25519; do
    key="$E/ssh/ssh_host_${kt}_key"
    [ -s "$key" ] && continue
    rm -f "$GENTMP/ssh_host_${kt}_key" "$GENTMP/ssh_host_${kt}_key.pub"
    ssh-keygen -q -t "$kt" -N '' -f "$GENTMP/ssh_host_${kt}_key" >/dev/null
    cp "$GENTMP/ssh_host_${kt}_key"     "$key"
    cp "$GENTMP/ssh_host_${kt}_key.pub" "$key.pub"
done

# --- ssh ownership / modes ---
# Files written through the mount come back owned by the mount user
# (uid 1000); sshd StrictModes refuses an authorized_keys it does not own.
# The daemon runs as that same user and bypasses its own checks, so a
# host-side chown/chmod succeeds and is persisted in the volume metadata.
chown -R root:root "$E/ssh" 2>/dev/null || true
chmod 700 "$E/ssh" 2>/dev/null || true
chmod 600 "$E"/ssh/ssh_host_*_key 2>/dev/null || true
chmod 644 "$E"/ssh/ssh_host_*_key.pub 2>/dev/null || true
chmod 644 "$E/ssh/sshd_config.d/99-invfs.conf" 2>/dev/null || true
if [ -d "$M/root/.ssh" ]; then
    chown -R root:root "$M/root/.ssh" 2>/dev/null || true
    chmod 700 "$M/root/.ssh" 2>/dev/null || true
    [ -f "$M/root/.ssh/authorized_keys" ] \
        && chmod 600 "$M/root/.ssh/authorized_keys" 2>/dev/null || true
fi

# --- runlevels ---
mkdir -p "$E/runlevels/default" "$E/runlevels/sysinit" "$E/runlevels/boot"
# os.symlink creates the link directly; `ln -sf` would need rename(2).
python3 - "$E/runlevels/default" <<'PYEOF'
import os, sys
d = sys.argv[1]
for name, target in (("sshd", "/etc/init.d/sshd"),
                     ("dhcpcd", "/etc/init.d/dhcpcd")):
    p = os.path.join(d, name)
    if os.path.islink(p):
        if os.readlink(p) == target:
            continue
        os.unlink(p)
    elif os.path.lexists(p):
        print("configure-guest: %s exists and is not a symlink; left alone" % p,
              file=sys.stderr)
        continue
    os.symlink(target, p)
PYEOF

echo "guest configured at $M"

# --- portage locks.py: tolerate dcache ghosts from recycled pids ---
# (InvariantFS: link(2) can EEXIST on a stale positive dentry; retry once)
LOCKS="$M/usr/lib/python3.14/site-packages/portage/locks.py"
if [ -f "$LOCKS" ]; then
python3 - "$LOCKS" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
a_old = """                except OSError as e:
                    func_call = f"link('{lockfilename}', '{myhardlock}')"
                    if e.errno == OperationNotPermitted.errno:
                        raise OperationNotPermitted(func_call)
                    elif e.errno == PermissionDenied.errno:
                        raise PermissionDenied(func_call)
                    elif e.errno in (errno.ESTALE, errno.ENOENT):
                        # another process has removed the file, so we'll have
                        # to create it again
                        continue
                    else:
                        raise"""
a_new = """                except OSError as e:
                    func_call = f"link('{lockfilename}', '{myhardlock}')"
                    if e.errno == OperationNotPermitted.errno:
                        raise OperationNotPermitted(func_call)
                    elif e.errno == PermissionDenied.errno:
                        raise PermissionDenied(func_call)
                    elif e.errno in (errno.ESTALE, errno.ENOENT):
                        # another process has removed the file, so we'll have
                        # to create it again
                        continue
                    elif e.errno == errno.EEXIST:
                        # InvariantFS: positive dcache dentry left by a dead
                        # process whose pid got recycled. Drop the ghost and
                        # retry; a second EEXIST means someone else holds it.
                        try:
                            os.unlink(myhardlock)
                            os.link(lockfilename, myhardlock)
                        except OSError as e2:
                            if e2.errno == errno.EEXIST:
                                continue
                            raise
                    else:
                        raise"""
b_old = """    try:
        try:
            os.link(lock_path, hardlink_path)
        except OSError as e:
            if e.errno not in (errno.ENOENT, errno.ESTALE):
                _raise_exc(e)
            return (True, None)"""
b_new = """    try:
        try:
            os.link(lock_path, hardlink_path)
        except OSError as e:
            if e.errno == errno.EEXIST:
                # InvariantFS: a positive dentry may be cached from a dead
                # process whose pid got recycled. Drop and retry once;
                # if it still exists, treat as removed (caller copes).
                try:
                    os.unlink(hardlink_path)
                except OSError:
                    pass
                try:
                    os.link(lock_path, hardlink_path)
                except OSError:
                    return (True, None)
            elif e.errno not in (errno.ENOENT, errno.ESTALE):
                _raise_exc(e)
            return (True, None)"""
if a_old in s:
    s = s.replace(a_old, a_new)
if b_old in s:
    s = s.replace(b_old, b_new)
open(p, "w").write(s)
print("locks.py patched")
PYEOF
fi

# --- getuto stubs (real getuto runs gpg; our FUSE lacks mmap yet) ---
# NB: copy rather than `mv`: rename(2) is unavailable through the FUSE
# mount, and a missing getuto must not abort provisioning.
for gp in "$M/usr/bin/getuto" "$M/usr/sbin/getuto"; do
    [ -f "$gp" ] || continue
    [ -L "$gp" ] && continue          # never write through a symlinked path
    if [ ! -f "$gp.real" ]; then
        cp "$gp" "$gp.real"
        chmod 755 "$gp.real"
    fi
    printf '#!/bin/sh\nexit 0\n' > "$gp"
    chmod 755 "$gp"
done

# installkernel: dracut backend (initramfs generation for kernel-bin)
mkdir -p "$M/etc/portage/package.use"
echo "sys-kernel/installkernel dracut" > "$M/etc/portage/package.use/kernel"

# manual sweep trigger helper (USR1 -> running daemon; progress on console)
cat > "$M/usr/local/bin/invf-sweep" << "EOS"
#!/bin/sh
PID=$(pidof invf-fuse)
[ -n "$PID" ] || { echo "invf-fuse not running"; exit 1; }
kill -USR1 "$PID" && echo "sweep triggered on pid $PID; watch console"
EOS
chmod 755 "$M/usr/local/bin/invf-sweep"
