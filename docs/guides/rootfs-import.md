# Installing a root filesystem into an InvariantFS volume

How to turn a directory tree — a distribution rootfs, a stage tarball, a
staging area — into an InvariantFS volume that boots.

## The short version

```sh
INVFS_META_FRAC=16 invf-mkfs rootfs.img 2048
sudo invf-import --fail-on-skip rootfs.img /path/to/rootfs
invf-import --fail-on-skip rootfs.img /path/to/rootfs   # reports anything it dropped
```

**Run the import as root.** Not because the volume needs privilege — it
does not — but because the *source tree* contains files an ordinary user
cannot read, and `invf-import` cannot invent what it was not allowed to
read.

## Why `sudo`, concretely

A root filesystem is full of root-only files: `/etc/shadow`,
`/etc/shadow-`, `/etc/gshadow`, `/etc/gshadow-`, `/etc/security/opasswd`,
`/etc/.pwd.lock`, `/var/log/btmp` (mode 0660), and everything under `/root`
(mode 0700). An ordinary user cannot `open()` the first group, and cannot
`opendir()` the last one.

`invf-import` walks the tree with `lstat()`, which needs no read permission
— only permission to traverse the parent directories. So it reaches every
one of those paths, sees them, and then cannot read them. The right thing
to do at that point is to say so, and it does: each one is named on stderr
with its reason (`unreadable source`), and `--fail-on-skip` turns the whole
class into a non-zero exit.

Import the same tree as an ordinary user and you will instead get a volume
with no `/etc/shadow` that reports a clean import. **That volume will not
boot usefully, and `invf-fsck` will not tell you**: `invf-fsck` checks what
the volume references, and a file that was never written is referenced by
nothing. There is no damage to detect — only an absence, and an absence is
invisible to a checker.

The report is the defence, not the exit status. Read it.

## What the report looks like

```
$ invf-import rootfs.img /srv/rootfs
imported: 4733 dirs, 3617 files, 0 symlinks, 0 specials, 0 skipped in 61.2s
```

A clean run says `0 skipped` and prints nothing else. Anything else looks
like this — on **stderr**, one line per path:

```
invf-import: skipped /srv/rootfs/etc/shadow: unreadable source (DATA LOSS -- this
  path is NOT in the volume; re-run as root to read it) (Permission denied)
invf-import: skipped /srv/rootfs/root: unreadable source (...) (Permission denied;
  every entry under it was skipped too, and NONE of them are in the volume)
```

and a restatement on **stdout**, where it survives `2>/dev/null`:

```
WARNING: 8 path(s) were NOT imported; each one is named with its reason on stderr.
WARNING: 8 of them were UNREADABLE SOURCE -- data loss. This volume does not contain
  those paths and invf-fsck cannot report them as missing.
```

**The count is not a file count.** An unreadable *directory* costs one
increment and loses every entry beneath it, so `8 skipped` above meant
**9 missing files**. The per-path lines are the record; the count is only a
summary of them.

## `--fail-on-skip`

Without it, `invf-import` exits `0` even when it dropped paths — the same
exit status it has always had, so nothing that wraps it breaks. With it,
any skip is exit `3`, after the report is printed. Use it in any pipeline
that produces a bootable image:

```sh
invf-import --fail-on-skip rootfs.img /srv/rootfs || exit 1
```

A tree with nothing unreadable is unaffected: no skip lines, no warning,
exit 0.

## Why not let the tool elevate itself

`invf-import` does not re-exec itself under `sudo` and is not installed
setuid. It is a recursive tree walker that takes a caller-supplied root
path, descends into it, and writes a raw device image — which is exactly
the shape of a setuid-root privilege-escalation surface. The operator's
answer is one `sudo` on the command line, which is also the answer that
shows up in the shell history and in the build log.

## Sizing the volume first

Rootfs trees with tens of thousands of small files need a metadata zone
larger than the default. See AGENTS.md §2.7:

```sh
INVFS_META_FRAC=16 invf-mkfs rootfs.img 2048
```

The symptom of getting this wrong is `ENOSPC` on writes while `df` still
reports free space.

## Importing into a subdirectory

`INVFS_IMPORT_PREFIX=var/db/repos/gentoo` imports *under* an existing
volume directory rather than at the root; the target must already exist.
Skipped paths are reported by their **source** path in both modes, because
that is the path whose permissions you have to fix.

## Bit-exactness check

```sh
invf-cat rootfs.img etc/hostname | cmp - /srv/rootfs/etc/hostname
```

`invf-verify --deep` is **not** an oracle for this: it checks readability
and length only, because a block entry carries a physical block address and
no content hash. `cmp` against `invf-cat` output is the check that proves
bytes.

## See also

- `tools/test-import-skip-report.sh` — the regression suite; it builds the
  same failure shape as an ordinary user and needs no privileges.
- AGENTS.md §2.7 (metadata sizing), §2.9 (the bit-exactness contract).
- `docs/GENTOO-INSTALL.md`, `docs/ARCH-INSTALL.md` — full install guides.