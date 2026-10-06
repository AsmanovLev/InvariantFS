# Debian on InvariantFS — Install Guide

> **Read this first: the boot is not yet reliable.** Of 15 observed boots on one
> volume, **8 reached `multi-user.target` and 2 stopped at `getty.target`**
> (`INCIDENTS.md` WP224). systemd itself works — it comes up as PID 1 on the
> FUSE root and, when it completes, reaches `graphical.target` with the FUSE
> control filesystem mounted in-guest. The failure is a boot *transaction* that
> sometimes does not finish, not a broken filesystem and not a hung unit. The
> guest is still usable when it happens. Expect to retry.

This is the fourth install guide, for the fourth boot script
(`tools/boot-debian-qemu.sh`). Debian is here for a specific reason: it is the
one readily available distro whose init is **systemd as PID 1**, which is what
`INCIDENTS.md` H1 and README "Known issues" #3 are about. Arch, Gentoo and Void
all cover runit/OpenRC. Nothing here tests those.

## 1. Get a Debian rootfs

Either debootstrap, or a container base:

```bash
# debootstrap (what CI uses -- .github/workflows/engine-ci.yml).
# The --include list is load-bearing, see the warning below.
debootstrap --variant=minbase \
  --include=systemd,dbus,udev,openssh-server \
  trixie /var/tmp/debroot

# or a container base, if you would rather not use debootstrap
docker pull debian:trixie
docker create --name invfs-deb debian:trixie
docker export invfs-deb | tar -C /var/tmp/debroot -xf -
docker rm invfs-deb
```

⚠️ A **minbase** has no `systemd` and no `procps`. `tools/configure-debian.sh`
does NOT install them -- it only provisions files (see §3) -- so the packages
have to arrive here, via `--include` (or a manual install into the root).
Without them the boot stalls early with zero udev lines in the serial log,
which looks like a different failure entirely. What each one is for:

| package | why |
|---|---|
| `systemd`, `dbus` | PID 1 must be systemd; that is the entire point |
| `udev` | **ships separately from `systemd`** — `systemd-udevd` is in `udev` |
| `openssh-server` | the harness asserts over SSH (best-effort; see §8) |

The `udev` one costs an afternoon if you miss it: without it the boot stalls
early with **zero udev lines in the serial log**, which looks like a different
failure entirely.

## 2. Stage on an ordinary filesystem

Never build a root on InvariantFS and then import it — see "Import, do not tar
through FUSE" in `VOID-INSTALL.md`.

```bash
sudo mkdir -p /var/tmp/debroot
sudo chown "$USER" /var/tmp/debroot
```

## 3. Provision

```bash
bash tools/configure-debian.sh /var/tmp/debroot sshd
```

This is the script that makes a minbase bootable, and it does four things that
are each easy to miss:

- **autologin on `ttyS0`**, so the serial console gives you a root shell without
  a password — this is what lets a harness assert from the console at all
- **`/etc/fstab`** matching the volume layout
- **`systemd-networkd`, enabled** — a `20-wired.network` file alone does
  *nothing* without the `sys-subsystem-networking.target.wants` symlink.
  debootstrap enables neither, so the first boots reached `multi-user.target`
  and then had **no network**: a guest that boots perfectly and is unreachable.
- **`openssh-server`**, if you pass `sshd`

It runs no package manager and touches no chroot -- it only writes files,
so it needs neither network nor scratch beyond the root itself. (An older
revision of this guide claimed it runs `apt` inside a chroot; it never
did, and following that claim produced roots with no systemd in them.)

## 4. Format the volume

```bash
# single device
./bin/invf-mkfs /var/tmp/invfs-deb/deb.img 4

# two devices
./bin/invf-mkfs /var/tmp/invfs-deb/deb.img 15 /var/tmp/invfs-deb/deb-shadow.img 20
```

## 5. Import offline

```bash
# single device
./bin/invf-import /var/tmp/invfs-deb/deb.img /var/tmp/debroot

# two devices
INVFS_DEV1=/var/tmp/invfs-deb/deb-shadow.img \
  ./bin/invf-import /var/tmp/invfs-deb/deb.img /var/tmp/debroot
```

⏱️ **Budget ~850 s.** The importer is fsync-bound — 7 barriers per file, and a
Debian root is ~7,500 files. This is not slow because anything is wrong.

It **merges** into an existing volume, so iterating on a single file in the
root is `invf-import <img> <stagedir>` again (under a second), not a rebuild.
The harness exposes this as `INVFS_DEB_VOLUME=<img>`, which skips mkfs+import
entirely.

> ⚠️ Do not `chroot` with `/proc`, `/dev` and `/sys` bind-mounted into the
> staging tree and then leave them there. The importer will walk live kernel
> state: a 264 MB root becomes an 829 MB image, and the boot then spends its
> time somewhere nobody is looking.

## 6. Build the initramfs

```bash
bash tools/mkinitramfs.sh
```

The initramfs was **already written for systemd** — `tools/initramfs-init.sh:295-300`
mounts `/run` as tmpfs and cgroup2 *before* handing off to PID 1 (WP66 H1), and
`:320` takes the init binary from the kernel cmdline via `get_opt invfs.init`.
That accommodation is what makes §7 work at all.

## 7. Boot under QEMU

```bash
INVFS_DEB_STAGE=/var/tmp/debroot \
  bash tools/boot-debian-qemu.sh
```

Or, once the volume is already imported:

```bash
INVFS_DEB_VOLUME=/var/tmp/invfs-deb/deb.img \
  bash tools/boot-debian-qemu.sh
```

The init is named **explicitly** on the cmdline rather than relying on
`/sbin/init` resolution inside the root, so the assertion is about systemd
starting rather than about a symlink happening to exist:

```
console=ttyS0,115200 invfs.init=/lib/systemd/systemd
```

Useful variables:

| variable | default | notes |
|---|---|---|
| `INVFS_DEB_BOOT_TIMEOUT` | `900` | was 420, and it fired on guests that were visibly fine |
| `INVFS_DEB_MODE` | `single` | `multi` for the two-device shape |
| `INVFS_DEB_ACCEL` | auto | KVM when `/dev/kvm` is r/w, else TCG |
| `INVFS_DEB_PORT` | `2426` | the harness pre-checks it; a leaked QEMU holding it used to be reported as a filesystem failure |
| `INVFS_DEB_LOGDIR` | `$WORK-logs` | serial log, QEMU stderr, import log |

**The harness does not print `PASS`.** Boot assertions pass; the in-guest SSH
leg does not, and a minbase Debian has no `procps`, so `ps -p 1` has nothing to
run (suspected, not verified). SSH is treated as best-effort depth and the
authoritative assertions are made from the serial console, which needs no DHCP
lease and therefore has one fewer way to fail.

## 8. Log in

Autologin gives you a root shell on `console=ttyS0`. From another terminal:

```bash
ssh -p 2426 root@127.0.0.1
```

```bash
# PID 1 must be systemd -- NOT runit
readlink -f /proc/1/exe

# the root really is InvariantFS
grep ' / ' /proc/mounts

# is the transaction finished? (this is the WP224 question, asked in-guest)
systemctl is-system-running
systemctl list-jobs

# leave
systemctl poweroff
```

`systemctl list-jobs` is the one to reach for when a boot stalls. The serial
console can only tell you a target was never *reached*; `list-jobs` names the
job that is holding it. `tools/ki3b-jobs.py` automates exactly that — it puts
the serial console on a socket so it can type, and asks the guest on every boot.

## Known issues and FUSE-portability notes

### The boot transaction sometimes does not complete (WP224, OPEN)

The most important thing on this page. systemd works; the transaction does not
always finish.

Across boots on one volume and one root:

| outcome | boots |
|---|---|
| reached `multi-user.target` and `graphical.target` | 5, 7, 9, 11, 12, 13, 14 |
| stopped at `getty.target`, guest still usable | 8, 15 |

What has been **ruled out**, by measuring rather than assuming:

- it is **not** a hung unit — every unit that prints `Starting` also prints a
  terminal line, and the *set* of such units is identical between good and bad
  boots
- it is **not** the missing-`udev` stall — `udev` is installed, and failing
  boots had udevd running (13 udev lines)
- it is **not** early in boot — failing boots reach the login prompt, start
  `ssh.service`, and give you a shell
- it is **not** "runtime lookup corruption" as README #3 words it; nothing here
  shows corrupted lookups, and README #3 needs revisiting

What has **not** been established: which job blocks `multi-user.target`. That
is what `tools/ki3b-jobs.py` is for.

### Two devices

Both shapes are exercised (`INVFS_DEB_MODE=multi`), and both can stall the same
way. The two-device symlink-directory lookup hazard documented in README "Known
issues" applies here too, and is a separate issue from WP224.

### Import, do not tar through FUSE

Same as Void/Arch/Gentoo: stage on an ordinary filesystem, import offline.
Tarring *through* a FUSE mount produces ordering-dependent archives.

### `/run` must be tmpfs before PID 1

If you write your own initramfs rather than using `tools/mkinitramfs.sh`, this
is the single thing you must not skip. systemd expects `/run` as tmpfs and
cgroup2 before it starts; without them early-mount units fail and daemon startup
hangs. That is INCIDENTS.md H1, and `tools/initramfs-init.sh:295-300` is the
accommodation for it.

### `dbus.service` fails on every boot, good or bad

Expected on this root and **not** the WP224 cause — it fails identically on the
boots that reach `multi-user.target`. Do not chase it as the stall.

## See also

- `docs/INSTALL-MATRIX.md` — what is verified for which distro, and how
- `docs/VOID-INSTALL.md`, `ARCH-INSTALL.md`, `GENTOO-INSTALL.md` — the runit/OpenRC shapes
- `INCIDENTS.md` WP224-OPEN — the nondeterministic boot, in full
