# WP62-bootstrap — host bootstrap installer (`curl -fsSL | sh`) + Gentoo clean re-run

**Branch:** `wp/62-bootstrap`
**Worktree:** `/tmp/invfs-wp62-bootstrap`
**Severity:** MEDIUM (install UX; no engine format change)
**Source:** design thread this session
**Estimated effort:** 2–3 days

---

## Scope

Two deliverables in one agent:

1. **`packaging/bootstrap.sh`** — a POSIX-sh host installer meant to be run as
   `curl -fsSL <url> | sh` (or `sh -s -- <flags>`). It installs the `invf-*`
   tools into the host system by delegating the FHS layout to the existing
   `packaging/install.sh` (single source of truth).
2. **A clean, end-to-end Gentoo rootfs install run** on InvFS proving the
   verified path still works on the current tree (post-WP58a), plus the boot
   bootstrap script used to get there.

This is the **FUSE/host** half of installation. The `root-mode` offline
installer (import a distro tarball onto a volume) is separate.

---

## bootstrap.sh

```
curl -fsSL https://get.invarifs.org | sh -s -- --yes
curl -fsSL .../bootstrap.sh | sh -s -- --version v0.3.0 --prefix /usr/local
```

Flags:
- `--version <tag|latest>` (default `latest`) — pin a release
- `--prefix <dir>` (default `/usr/local`) — avoids fighting the package manager
- `--release` / `--source` (default: release if assets exist, else source)
- `--file <tarball>` — offline / inspect-before-run
- `--no-systemd --no-dracut --no-mkinitcpio --no-initramfs-tools` (hooks opt-in)
- `--dry-run`, `--uninstall`, `--list`, `--yes`

Behaviour:
- **No silent sudo.** If not root, print the `sudo sh` command and exit.
- Download `invfs-<ver>-<arch>.tar.zst` + `SHA256SUMS`; **verify sha256 before
  unpack/exec**; optional minisign/gpg hook (not required).
- Source mode: fetch the source tarball (or `git clone --depth 1`), check build
  deps, `make`, then `packaging/install.sh`.
- Runtime deps matrix by `/etc/os-release` (`ID`/`ID_LIKE`): print the install
  command for `fuse3 zstd zlib` (Debian/Arch/Gentoo/Void/Fedora), never install
  them itself.
- Idempotent; write a manifest for `--uninstall`; refuse to write outside
  `$PREFIX`/`$DESTDIR`.
- Two-step friendly: `bootstrap.sh` downloads+verifies, `--run` executes.

Release artifacts (produced by a `make release` / CI target, tracked here but
built with GitHub Releases):
- `invfs-<ver>-x86_64.tar.zst` (bin + codecpacks + man + systemd/dracut)
- `SHA256SUMS` (+ optional `.minisig`)
- Uses the token/workflow scope available on the account.

`packaging/install.sh` gets a `--manifest <file>` option so bootstrap can
record installed paths for uninstall.

---

## Clean Gentoo rootfs run (validation, not a new installer)

Reproduce `docs/GENTOO-INSTALL.md` "Verified Bootstrap Path" on the current
tree, from scratch:
1. `truncate -s 15G root.img` + `INVFS_META_FRAC=16 bin/invf-mkfs root.img 15`
   (single device first, then the two-device DEVT form).
2. Stage3 extract to a directory, then **`invf-import`** (never tar through
   FUSE).
3. `tools/configure-guest.sh` (WP54) on a host FUSE mount.
4. `tools/mkinitramfs.sh` + direct `-kernel`/`-initrd` QEMU boot.
5. Expect OpenRC runlevel 3, serial login, SSH on host port 2222; verify
   `invf-cat` bit-exactness of a few stage3 files and `invf-fsck` clean.

Write the exact commands and observed output into the WP result section so the
run is reproducible. Add a `tools/test-bootstrap.sh` (non-interactive) that
runs `bootstrap.sh --source --prefix <tmp> --dry-run` and the install into a
`DESTDIR`, then `--uninstall` — a safe regression that needs no VM.

---

## Files

- `packaging/bootstrap.sh` (new)
- `packaging/install.sh` — add `--manifest`
- `Makefile` — `release` target (tar.zst + SHA256SUMS)
- `tools/test-bootstrap.sh` (new)
- `docs/GENTOO-INSTALL.md` / `README.md` — install section update
- `impl_docs/WP62-bootstrap.md` — results

---

## Validation

1. `sh -n packaging/bootstrap.sh`; `shellcheck` if available.
2. `tools/test-bootstrap.sh`: DESTDIR install + manifest + uninstall, sha256
   verification, `--dry-run`, missing-dep message.
3. Clean Gentoo run as above; report console markers + `invf-fsck` output.
4. `make test` unaffected (no engine change).

---

## Out of scope (do NOT touch)

- Root-mode offline installer for other distros (Arch/Void/Debian agents).
- Engine/format changes, codec policy (WP59), registry (WP60).
- Actually hosting `get.invarifs.org` (use GitHub Releases URLs for now).

---

## Coordination notes

- Subagent ID: `wp62-bootstrap`; e2e via `INVFS_E2E_AGENT=wp62-bootstrap`.
- This agent owns **bootstrap + Gentoo clean run**.
- GitHub: account `AsmanovLev`, token with `repo`/`workflow`. "Только main-репо
  релизы" — publish releases to `AsmanovLev/InvariantFS`.
- No engine files in scope.

---

## Results (wp/62-bootstrap, 2026-09-19)

### Files changed

| File | Change |
|---|---|
| `packaging/bootstrap.sh` | **new** — POSIX-sh `curl … \| sh` host installer |
| `packaging/install.sh` | `--manifest <file>`; records every installed path |
| `Makefile` | `release` target → `dist/invfs-<ver>-<arch>.tar.zst` + `SHA256SUMS` |
| `tools/test-bootstrap.sh` | **new** — non-interactive regression (34 checks) |
| `tools/initramfs-init.sh` | boot fix (see bug 1): busybox applets + `fuse.ko` |
| `tools/mkinitramfs.sh` | write NIC modules to `/lib/modules` (init's path) |
| `README.md` | "Install (host bootstrap)" section + test line |
| `docs/GENTOO-INSTALL.md` | single-device re-run + new pitfalls |
| `.gitignore` | ignore `dist/` |

### Bootstrap validation

```
$ sh -n packaging/bootstrap.sh && sh -n packaging/install.sh          # clean
$ bash tools/test-bootstrap.sh
  ... bootstrap test: 34 passed, 0 failed
$ make release
  release: dist/invfs-v0.2.1-x86_64.tar.zst
  <sha256>  invfs-v0.2.1-x86_64.tar.zst
$ DESTDIR=/tmp/dest sh packaging/bootstrap.sh --file dist/invfs-*.tar.zst \
      --prefix /usr/local --yes --no-systemd --no-dracut
  bootstrap.sh: sha256 verified: invfs-v0.2.1-x86_64.tar.zst f999fb0b…
  install.sh: installed under /tmp/dest/usr/local   (manifest: 73 entries)
$ DESTDIR=/tmp/dest sh packaging/bootstrap.sh --list        # lists paths
$ DESTDIR=/tmp/dest sh packaging/bootstrap.sh --uninstall   # removes all, prunes dirs
# corrupt SHA256SUMS → "sha256 MISMATCH"; nothing installed
# non-root + no DESTDIR → prints `sudo sh …`, exits 1 (no silent sudo)
```

### Clean Gentoo run (single device, 3 GB QEMU)

```
$ truncate -s 15G root.img
$ INVFS_META_FRAC=16 bin/invf-mkfs root.img 15
  metadata zone 992 MB, raw 2873 MB, shadow 11494 MB, state CLEAN
$ bin/invf-import root.img stage3-root
  imported: 3076 dirs, 54026 files, 8986 symlinks, 0 skipped in 167.9s
$ bin/invf-fuse root.img mnt && tools/configure-guest.sh mnt   # locks.py patched
$ fusermount3 -u mnt
$ tools/mkinitramfs.sh                 # vm/initramfs.cpio.gz
$ qemu-system-x86_64 -machine q35,accel=kvm -m 3G -cpu host -smp 2 \
    -kernel /boot/vmlinuz-7.2.5-200.fc44.x86_64 \
    -initrd vm/initramfs.cpio.gz -append console=ttyS0,115200 \
    -drive id=vol,file=root.img,format=raw,if=ide \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0 \
    -display none -serial stdio -monitor none -no-reboot
```

Console (observed):

```
fuse: init (API version 7.45)
INVFS probe: [/dev/sda 63caad6a0000400086237b3267458b6b]
INVFS_RAW=/dev/sda DEV1=
InvariantFS mounted: 66103 files
~ # Chrooting to InvFS root...
OpenRC 0.63.3 is starting up Gentoo Linux (x86_64)
* Starting sshd ...        [ ok ]
INIT: Entering runlevel: 3
This is invarifs-vm (Linux x86_64 7.2.5-200.fc44.x86_64)
invarifs-vm login:
```

SSH (`ssh -p 2222 root@localhost`, empty root password):

```
SSH_OK
uid=0(root) gid=0(root) ...
invarifs-vm
invfs[sda] on / type fuse (rw,noatime,user_id=0,group_id=0,max_read=1048576)
rc-status: dhcpcd/netmount/sshd/local [ started ]
ip: enp0s2 UP 10.0.2.15/24
```

Bit-exact (`invf-cat <img> <name>` vs the source stage3 tree):

```
BIT-EXACT  /etc/passwd                        769 B
BIT-EXACT  /usr/bin/init                    49064 B
BIT-EXACT  /etc/ssh/moduli                 673940 B
BIT-EXACT  /usr/bin/openrc-init             22712 B
BIT-EXACT  /usr/lib64/ld-linux-x86-64.so.2  258552 B
BIT-EXACT  /usr/bin/bash                   937200 B
BIT-EXACT  /usr/bin/ssh                    936680 B
bit-exact: 7 ok, 0 mismatch
```

`invf-fsck` after an offline `invf-sweep`:

```
state: CLEAN   live files: 66126   l2p entries: 2449
orphans: 0    missing: 0    bad records: 0
held for checkpoint: 26049     checkpoint: sweep #1 live
OK
```

### Bugs found during the clean run

1. **`tools/initramfs-init.sh` could not boot a distro kernel** (fixed here).
   The WP50 script (a) never installed busybox applet symlinks, (b) never
   loaded `fuse.ko` (`CONFIG_FUSE_FS=m`), and (c) `insmod`ed `/lib/modules/*`
   while `mkinitramfs.sh` wrote `/modules/*`. First boot died at
   `/init: line 23: mount: not found`; after fixing (a) it mounted but the
   guest could not exec `/sbin/init`. Fixed by installing applets
   (`/bin/busybox --install -s /bin`), loading `/fuse.ko`, and aligning the
   module path to `/lib/modules` (init also tries `/modules` as a fallback).
2. **`invf-fsck -f` is destructive on a fresh stage3 import** (engine bug, out
   of WP62 scope). Right after import `invf-fsck` reports `orphans: 6848`;
   running `invf-fsck -f` frees them and the volume loses its live top-level
   usr-merge symlinks (`/bin /sbin /lib /lib64`) plus `/boot` and `/dev`,
   so the guest chroot fails with `can't execute '/sbin/init': No such file
   or directory` (missing ELF loader). Plain read-only fsck is safe; an
   offline `invf-sweep` then yields `orphans: 0` / `OK`.
3. **Guest shutdown does not power off** — OpenRC reaches runlevel 0 and
   unmounts, then `INIT: cannot execute "/sbin/halt.sh"` (usr-merge) leaves
   the VM idle; cosmetic.

### Not done / follow-ups

- Two-device `INVFS_DEV1` form was not re-run (single-device was the deliverable).
- Publishing the GitHub Release (`make release` produces the artifacts) is
  left to the orchestrator; no branch was pushed.
- The import/fsck orphan-accounting bug (2) belongs to the engine agents.

