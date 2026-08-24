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
