#!/bin/sh
# Configure a Gentoo rootfs volume for the InvariantFS VM.
# Usage: configure-guest.sh /path/to/fuse/mountpoint
# Run while the volume is FUSE-mounted (host side).
set -e
M="${1:?mountpoint}"
E="$M/etc"

# --- binhost + gpg stub + features ---
cat >> "$E/portage/make.conf" <<'EOF'

# --- InvariantFS VM ---
BINHOST="https://distfiles.gentoo.org/releases/amd64/binpackages/23.0/x86-64-v3/"
PORTAGE_BINHOST="${BINHOST}"
EMERGE_DEFAULT_OPTS="${EMERGE_DEFAULT_OPTS} --usepkg --getbinpkg"
FEATURES="${FEATURES} -news -pid-sandbox -userpriv -sandbox"
BINPKG_GPG_VERIFY_BASE_COMMAND="/usr/local/bin/fake-gpg-verify [PORTAGE_CONFIG] [SIGNATURE]"
EOF

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
sed -i 's/^root:[^:]*:/root::/' "$E/shadow" || true

# --- serial console getty ---
grep -q "^s0:" "$E/inittab" || printf '%s\n' 's0:12345:respawn:/sbin/agetty -L 115200 ttyS0 vt100' >> "$E/inittab"
sed -i 's/^c[1-6]:/##c&/' "$E/inittab"

# --- sshd: root + empty password (test VM only) ---
mkdir -p "$E/ssh/sshd_config.d"
printf 'PermitRootLogin yes\nPermitEmptyPasswords yes\n' > "$E/ssh/sshd_config.d/99-invfs.conf"

# --- runlevels ---
mkdir -p "$E/runlevels/default" "$E/runlevels/sysinit" "$E/runlevels/boot"
ln -sf /etc/init.d/sshd   "$E/runlevels/default/sshd"   2>/dev/null || true
ln -sf /etc/init.d/dhcpcd "$E/runlevels/default/dhcpcd" 2>/dev/null || true

echo "guest configured at $M"

# --- portage locks.py: tolerate dcache ghosts from recycled pids ---
# (InvariantFS: link(2) can EEXIST on a stale positive dentry; retry once)
python3 - "$E/../usr/lib/python3.14/site-packages/portage/locks.py" <<'PYEOF'
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

# --- getuto stubs (real getuto runs gpg; our FUSE lacks mmap yet) ---
for gp in "$M/usr/bin/getuto" "$M/usr/sbin/getuto"; do
    [ -f "$gp.real" ] || mv "$gp" "$gp.real"
    printf '#!/bin/sh\nexit 0\n' > "$gp"
    chmod 755 "$gp"
done

# installkernel: dracut backend (initramfs generation for kernel-bin)
mkdir -p "$M/etc/portage/package.use"
echo "sys-kernel/installkernel dracut" > "$M/etc/portage/package.use/kernel"

# manual sweep trigger helper (USR1 -> running daemon; progress on console)
cat > "$MNT/usr/local/bin/invf-sweep" << "EOS"
#!/bin/sh
PID=$(pidof invf-fuse)
[ -n "$PID" ] || { echo "invf-fuse not running"; exit 1; }
kill -USR1 "$PID" && echo "sweep triggered on pid $PID; watch console"
EOS
chmod 755 "$MNT/usr/local/bin/invf-sweep"
