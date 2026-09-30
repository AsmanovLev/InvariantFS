# Install matrix: what is actually installable, and what was actually run

This file is the map the packaging layer has to answer to. It exists because
the four install guides made very different claims — some "verified", some
implied — and a reader could not tell which was which. Per AGENTS.md §1.7 the
**code is the spec**: where a guide and the code disagree, the code wins and
the guide is the bug. Where a *test* has never been run, that is stated here
plainly rather than left to the reader's optimism.

Two things are deliberately kept apart, because conflating them is how this
project shipped an unrun doc before:

- **installable** — a real, versioned artifact that the target's own package
  tooling can install, built and checked here.
- **booted** — a full distro userspace actually reached a shell on an
  InvariantFS root on this host.

Nothing in the packaging layer implies a boot. Real rootfs boot on real
hardware is deferred; see the last section for what is missing.

## 1. The artifact

One artifact, four packagers. `make release` produces:

| file | what it is |
|---|---|
| `dist/invfs-<ver>-<arch>.tar.zst` | prebuilt `bin/invf-*`, the codecpacks with their C helpers already built, and the `packaging/` tree |
| `dist/SHA256SUMS` | the artifact's sha256 |
| `SHA256SUMS.sig` (in `dist/`) | detached ed25519 signature **of the sums file** (only with `SIGNING_KEY=`) |

The signature covers `SHA256SUMS` rather than the tarball directly, so one
signature covers every file published under a release tag. It is opt-in and
never silently skipped: without `SIGNING_KEY` the build prints
`release: NOTE: UNSIGNED ... do not publish this`.

The tar is built with `tar --sort=name --owner=0 --group=0 --mtime=@0`, so
**two builds of the same tree are byte-identical** (verified in
`tools/test-packaging.sh` and by rebuilding and comparing `dist/SHA256SUMS`).
The one thing that legitimately changes the bytes is `INVFS_BUILD_DATE`,
which the engine compiles in; a rebuild on a different day is a different
artifact and will say so in its sha256.

After each release, run `make release-sums` and paste the printed lines into
the three recipes. It prints rather than writes on purpose: a digest of the
tarball cannot live inside the tree the tarball is built from, because
writing it there would change the tarball on the next run. For the same
reason the target-native recipes themselves (`PKGBUILD`, `invfs.spec`,
`gentoo/`, `void/`, and the `debian/` recipe files) are **stripped from the
artifact** — they are inputs to building a package, not payload, and
`packaging/install.sh` never reads them. Without that, pinning a digest would
change the digest. With it, the pin is a genuine fixed point: editing a
recipe does not change the artifact, which is checked below.

## 2. Per-target status

| target | install recipe | format | native tooling | built/run here? |
|---|---|---|---|---|
| **Arch Linux** | `packaging/PKGBUILD` | makepkg | `makepkg -si` | recipe's `package()` executed against the artifact; `makepkg` itself **not installed on this Debian host** |
| **Gentoo Linux** | `packaging/gentoo/invfs-0.5.0.ebuild` (EAPI 8) | portage | `emerge sys-apps/invfs` | ebuild's `src_install()` executed against the artifact; `ebuild`/`portage` **not installed here** |
| **Void Linux** | `packaging/void/template` | xbps-src | `xbps-src` + `xbps-install` | template's `do_install()` executed against the artifact; `xbps-src` **not available in Debian** |
| **Debian** | `packaging/debian/` | `.deb` | `dpkg-buildpackage` + `dpkg -i` | **built and installed natively** — see §3 |

`packaging/invfs.spec` (RPM) also exists and is unchanged by this work. It is
not one of the four targets; it is left alone rather than tidied as a
drive-by.

### What "executed against the artifact" means

`tools/test-packaging.sh` sources each recipe and calls its own
`package()` / `src_install()` / `do_install()` with that tool's variables
(`$pkgdir`/`$srcdir`, `$ED`/`$S`, `$PKGDESTDIR`). Only the functions that are
not about *where files go* are stubbed, and each stub is labelled in the
script:

- `vmkdir` (xbps-src) — `do_install` does not call it
- `systemd_dounit` (portage) — installs unit files into `$ED`
- `$S` for the ebuild, which is a real extraction, not a stub

This is meaningfully stronger than "the script looks right", and it is
**not** the same as running `makepkg`. It does not exercise makepkg's
dependency resolution, its own dependency scanner, or package signing. Do not
read the Arch/Gentoo/Void rows as "tested on Arch/Gentoo/Void".

## 3. Debian, verified natively

This is the one target whose real package manager ran on this host
(Debian 13 trixie, kernel `6.12.107+deb13-amd64`, x86_64).

```bash
make release-sums                 # prints the digest to pin
cd <worktree> && dpkg-buildpackage -b -uc -us
sudo dpkg -i ../invfs_0.5.0-1_amd64.deb
```

What the check asserts, on the real `.deb`:

- the binaries are present and executable, and `invf-mkfs --version` reports
  the version the artifact claims
- every man page in the package is gzipped (Debian policy 10.1 §10.1.1)
- the Russian translations are present at `manN/ru/*.gz` — **they were
  silently missing before**, see §5
- at least one page renders under `man -l`

## 4. The man pages

They ship **gzipped**, which is the opposite of what the tree did before this
work — see §5. `packaging/install.sh` installs
`packaging/man/<name>.<sec>` as `$PREFIX/share/man/manN/<name>.<sec>.gz`,
and `packaging/man/ru/*` as `manN/ru/<name>.<sec>.gz`, using `gzip -n` so a
rebuild is byte-identical. `WITH_GZIP_MAN=0` restores plain pages.

They are **plain ASCII roff in the source tree**; the `.gz` is created at
install time. (Any note that the repository's man pages are themselves
gzipped is a doc bug.)

To reproduce the render check:

```bash
man -l /usr/share/man/man8/invf-mkfs.8.gz
man -l /usr/share/man/man8/ru/invf-mkfs.8.gz    # Russian translation
```

## 5. Defects found and fixed while building this

Recorded because each was a packaging bug that made a target *not*
installable, and none of them was visible from the install guides.

1. **`packaging/install.sh` aborted the entire install on any host with a C compiler.**
   It rebuilt each codecpack helper with a hand-rolled
   `cc -std=c11 -O2 <pack>.c`. That is a second, wrong copy of the build
   recipe: a pack may need extra objects and include paths (qcow2 needs
   `src/codecs` + `src/zlib` + `-DZ_PREFIX` and the `deflate_repro` objects,
   which live in the Makefile as `PLUGIN_EXTRA_qcow2` / `HELPER_CFLAGS_qcow2`).
   The naive line failed to link qcow2, `set -eu` aborted at exit 1, and it
   did so **before the man pages, before the systemd units, before anything**.
   So no package built on any host with a compiler — the `.deb`, the
   `PKGBUILD`, the spec and any ebuild all failed the same way. The fix is to
   stop duplicating the build: `make helpers` is the single source of truth
   and `packaging/install.sh` ships what it produced.
2. **`jxlest` had no build rule at all.** The jxl pack is a codecpack, not a
   containerpack, so it is in neither `CPACKS` nor any `*_EXTRA_*` list.
   `packaging/install.sh` had been building it by accident. With fix (1) it would have
   silently stopped being installed, so it now has an explicit rule.
3. **The Debian binary dropped all 12 Russian man pages.**
   `packaging/debian/install` matched `usr/share/man/*/man1/invf-*.1`, but
   `packaging/install.sh` writes translations to `manN/ru/*.1.gz` — a path that glob
   cannot match, and `dh_install` does not complain about a `usr/share/`
   entry that matches nothing. The man directories are now listed as
   directories, which is also compression-agnostic.
4. **Man pages were installed uncompressed**, against Debian policy and
   against what `man` resolves first on Arch/Gentoo/Void.
5. **`packaging/debian/control` and `packaging/debian/copyright` named the
   wrong upstream** (`github.com/anomalyco/InvariantFS`) while `PKGBUILD`
   and `invfs.spec` said `github.com/AsmanovLev/InvariantFS`.
6. **Every package shipped the unit-test harnesses.** `all` builds ~23
   `*_test` binaries into `bin/`, and `packaging/install.sh` installs
   `bin/invf-*`, so all four targets shipped `invf-anchor_test`,
   `invf-delta_test`, `invf-ivpack_packs_test` and friends as user-facing
   tools. The release now copies exactly `$(TOOLS)` — the repo's own
   definition of what ships — instead of the whole `bin/`.
7. **`invfs-pack` was missing from every package.** It ships in the
   artifact, but the `invf-*` glob does not match `invfs-pack` (that is
   `invfs`, not `invf-`), so the codecpack manager was silently dropped by
   `packaging/install.sh`, by `packaging/debian/install` and by
   `%{_bindir}/invf-*` in the spec. The globs are now `invf*`.
8. **A build from a source tarball reported no version at all.** `VERSION`
   came from `git describe`, whose old `|| echo unknown` could never fire
   (the pipeline's exit status is `sed`'s). A tarball build — which is what
   Debian, Arch, Gentoo and Void all do — therefore passed
   `-DINVFS_VERSION_STRING=""`, which *defeated* the `#ifndef` fallback in
   `src/core/invarifs.h` and produced binaries printing
   `version  (build ...)`. `VERSION` now falls back to that header.
9. **`$(OUT)` had no rule, so a clean build always failed.** `build/obj`
   had a `mkdir -p` rule; `bin/` did not, and nothing creates it. Every
   developer's tree already had a `bin/`, which hid it — but
   `dpkg-buildpackage` builds from a pristine source package and died with
   `ld: cannot open output file bin/invf-mkfs`.

## 6. The `curl | sh` installer

Two forms, and the author should get the choice, not be handed a blob.

Inspect first, then run — nothing is installed until you say so:

```bash
curl -fsSLO https://github.com/AsmanovLev/InvariantFS/releases/latest/download/bootstrap.sh
curl -fsSLO https://github.com/AsmanovLev/InvariantFS/releases/latest/download/invfs.pub
less bootstrap.sh
sh bootstrap.sh --check-only --pubkey invfs.pub     # downloads, verifies, installs NOTHING
sudo sh bootstrap.sh --yes                          # then install
```

`--check-only` resolves the release, downloads the artifact and
`SHA256SUMS`, verifies the sha256 **and** the ed25519 signature, prints the
exact bytes it verified, and stops. The follow-up command it prints is
`--file <the exact path it just verified>`, so the install is of bytes you
have already seen, not of whatever is on the server a minute later.

Piped, if you prefer:

```bash
curl -fsSL .../bootstrap.sh | sudo sh -s -- --yes --pubkey /dev/stdin < invfs.pub
```

Properties:

- **Fail-closed on provenance.** With `--pubkey`, a missing or bad signature
  aborts; it does not install anyway. Without `--pubkey`, the run still warns
  loudly that a checksum proves only that the download was intact, not that
  it came from InvariantFS.
- **Idempotent.** Re-running reinstalls the same files; `--uninstall` uses the
  manifest written at install time (`$PREFIX/lib/invfs/installed.manifest`)
  and refuses to touch any path outside `$PREFIX`.
- **Non-destructive.** It never formats, mounts or writes a volume. It also
  does not install your distribution's `fuse3`/`zstd`/`zlib` — it prints the
  command for your package manager and lets you run it.
- **Fails before it half-installs.** Prerequisite checks (curl/wget, a
  sha256 tool, root for the prefix, `zstd` if `tar` lacks `--zstd`) all run
  before anything is unpacked.

`--check-only` and the negative controls are covered by
`tools/test-packaging.sh`, which asserts that a truncated artifact is
rejected by sha256 and installs nothing, that a tampered `SHA256SUMS` is
rejected, and that `--pubkey` with no signature fails closed.

## 7. Booting a distro root: not verified here

This is the honest gap, and it is larger than it looks.

**What exists.** `docs/ARCH-INSTALL.md`, `docs/GENTOO-INSTALL.md` and
`docs/VOID-INSTALL.md` describe full installs. `tools/test-arch-install.sh`
and `tools/test-void-install.sh` are offline harnesses (no boot, no FUSE)
that build a root, `invf-mkfs` + `invf-import` it, then `invf-fsck`,
`invf-verify --deep` and bit-exact `invf-cat` spot checks.
`tools/boot-{arch,gentoo,void}-qemu.sh`, `tools/mkdisk-arch.sh` and
`tools/configure-void.sh` are the boot-side scripts.

**What is not true today.**

- The guides' headers say "verified 2026-09-19" for Arch and Void. **That
  verification did not happen on this machine.** The only QEMU boot artifact
  under `/srv/bench` is `invfs-boot/` (2026-09-28), and it is not a distro
  root at all: it is a 4-file self-test initramfs, and it **failed** —
  `ERROR: no InvariantFS volume found on any block device`, dropping to a
  rescue shell. Nothing on this host shows a successful Arch, Gentoo or Void
  boot.
- `tools/test-arch-install.sh` and `tools/test-void-install.sh` are **not** in `make e2e`
  and **not** in CI. They need network, passwordless sudo and ~2 GB, so they
  have never run in any automated loop here. Their existence is not evidence
  of execution.
- There is no Debian install guide at all, and no Debian boot script. Debian
  is the one target whose *package* is verified here; its *rootfs on
  InvariantFS* is entirely unexplored.
- Bootable partition layout: the guides use direct `-kernel`/`-initrd` boot
  with the volume on a separate disk, and Arch additionally uses
  `mkdisk-arch.sh`. There is no GRUB/systemd-boot integration, no ESP
  handling, and no verified multi-disk-on-real-hardware layout.

**So, plainly:** the packaging layer is real and checked. The claim "a
distro root boots on InvariantFS" is currently a design plus a set of
unrun scripts, and this work does not change that. Getting from here to a
verified boot means running `tools/test-arch-install.sh` (and its void and
gentoo equivalents) on a machine with KVM, then running the QEMU scripts and
keeping the serial logs — which is exactly the kind of evidence that
the serial log at `/srv/bench/invfs-boot/serial.log` was supposed to be.

## 8. Reproducing everything in this file

```bash
make                                        # build the engine
make helpers                                # build the codecpack helpers
make release                                # dist/invfs-<ver>-<arch>.tar.zst + SHA256SUMS
make release-sums                           # the digest lines to pin in the recipes
bash tools/test-packaging.sh                # all four targets + negative controls

# Debian only, natively:
dpkg-buildpackage -b -uc -us
sudo dpkg -i ../invfs_0.5.0-1_amd64.deb
man -l /usr/share/man/man8/invf-mkfs.8.gz
```

Via the e2e runner, as the project requires:

```bash
INVFS_E2E_AGENT=wp-vm-packaging-installer INVFS_E2E_SLOTS=1 \
    bash tools/run-e2e.sh tools/test-packaging.sh
```
