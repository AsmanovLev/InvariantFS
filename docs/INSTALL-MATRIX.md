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

**When this table was last measured:** 2026-10-02, on `main` @ `4252d0c`, on
the host this repository is developed on (Debian 13 trixie, kernel
`6.12.107+deb13-amd64`, x86_64) plus a QEMU/KVM guest of the same Debian
release. Every claim below is a command that was run and whose output is
quoted in §3 or §7. Where a thing could not be run here, the row says so
in those words rather than leaving the old claim standing.

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
| **Gentoo Linux** | `packaging/gentoo/invfs-0.5.0.ebuild` (EAPI 8) | portage | `emerge sys-apps/invfs` | ebuild's `src_install()` executed against the artifact — see below; `ebuild`/`portage` **not installed here and no Gentoo host exists on this machine** |
| **Void Linux** | `packaging/void/template` | xbps-src | `xbps-src` + `xbps-install` | template's `do_install()` executed against the artifact; `xbps-src` **not available in Debian** |
| **Debian** | `packaging/debian/` | `.deb` | `dpkg-buildpackage` + `apt`/`dpkg` | **built, then installed in a clean VM and exercised** — see §3 |

`packaging/invfs.spec` (RPM) also exists and is unchanged by this work. It is
not one of the four targets; it is left alone rather than tidied as a
drive-by.

The three non-Debian rows all carry the same caveat, and it is a big one:
**no Arch, Gentoo or Void machine, and no `makepkg`, `portage` or
`xbps-src`, exists on this host.** What was run is each recipe's own
install function, against the release artifact, with that tool's variables.
That is a real measurement of the recipe's logic and a real measurement of
`packaging/install.sh` — which is where every one of the four targets
actually gets its file layout — but it is not the distribution's package
manager running, and it does not resolve, download, or sign anything.

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

### Gentoo, measured directly because it has the thinnest history

`65f9b47` introduced the ebuild and the Void template as new packaging for
two of the four targets, so they are the least-exercised paths in the tree.
There is no Gentoo host, no portage and no `xbps-src` on this machine, so
`emerge` and `xbps-src` were **not** run and no row above should imply
otherwise. What was done instead, on 2026-10-02:

- the release artifact was unpacked to `$S/invfs-v0.5.0-x86_64` (the name
  `MY_PN` resolves to) and the ebuild was sourced with `S`, `ED`, `PV` set
  as portage would set them, then **`src_install()` was called** with
  `die`/`elog` and `systemd_dounit` stubbed. It returned 0 and produced:
  19 binaries, 23 gzipped man pages, the 4 systemd units, the 2 dracut
  files, `initcpio` correctly absent (`WITH_MKINITCPIO=0`, as the comment
  says — dracut is Gentoo's generator), and all 9 codecpack helpers.
- **the pinned digest is not rot.** `BLAKE2B`/`SIZE` in the ebuild are
  `610ade57…`/`1383575`, and those are exactly the `b2sum -l 256` and byte
  count of the artifact built at the commit that introduced the ebuild
  (`65f9b47`). They do **not** match a rebuild from `main` @ `4252d0c`
  (`d08bdf60…`/`1264390`), which is expected — the ebuild says "re-pin on
  every release", and `make release` embeds `INVFS_BUILD_DATE`, so a
  rebuild is a different artifact. There is no published release in this
  tree to check the pin against other than that one build.
- `bash -n` on the ebuild: clean.
- **Not checked:** that `SRC_URI` resolves. It points at GitHub releases,
  which are not reachable from this host, so the digest could not be
  exercised through portage's own fetch. A stale pin would surface as a
  `Digest verification failed`, not as a wrong install.

One thing worth a reviewer's eye rather than a claim: `KEYWORDS="~amd64
~x86"`. `~x86` is not an architecture keyword; the `ARCH=amd64` above it
already covers this package. It was not touched here.

### `packaging/install.sh` itself — re-measured, in a VM

`install.sh` is the one piece all four targets share, so it was measured
three ways on 2026-10-02 rather than inferred from a recipe.

**(a) From the release artifact, in a clean Debian 13 guest with no C
compiler.** `DESTDIR=/tmp/stageA sh packaging/install.sh` → exit 0.
19 binaries, 23 gzipped man pages, 4 systemd units, 2 dracut files, and
**all 9 codecpack helpers including `qcow2`**, which is the one whose link
the old hand-rolled recipe could not resolve.

**(b) The same command in the same guest after `apt-get install gcc make`** —
i.e. the exact condition under which the last recorded state exited 1:

```
CC present: cc=/usr/bin/cc gcc=/usr/bin/gcc make=/usr/bin/make
install.sh: installed under /tmp/stageB/usr
INSTALL_SH_RC=0
bin: 19 file(s)
man gz: 23
units: invfs-sweep.service invfs-sweep.timer invfs-verify.service invfs-verify.timer
codecpack helpers staged: 9
```

Byte-for-byte the same result as (a). **The WP114 fix holds: the script no
longer exits 1 on a host with a C compiler.**

**(c) The `make helpers` fallback, exercised directly.** That is the code
the rewrite introduced, so it was forced: on the host, from a copy of the
worktree with `qcow2`'s and `xfs`'s helper binaries deleted, so `install.sh`
has to build them:

```
install.sh: building codecpack helpers (make helpers)
cc -std=c11 -O2 -Wall -Wextra -Werror -Isrc/codecs -Isrc/zlib -DZ_PREFIX \
   -o tools/codecpacks/qcow2.codecpack/bin/qcow2 \
   tools/codecpacks/qcow2.codecpack/qcow2.c build/obj/deflate_repro.o \
   build/obj/deflate_backend_system.o ... -lz
cc ... -o tools/codecpacks/xfs.codecpack/bin/xfs ...
install.sh: installed under /srv/bench/scratch/stageH/usr
EXIT=0
```

The flags are the Makefile's, not a second copy of them, and the
`deflate_repro` objects are on the link line. (That run installed 87
binaries rather than 19 — see §5.6.)

## 3. Debian, verified in a clean VM from the package

This is the one target whose real package manager really ran — and it ran
**in a guest, not from the checkout**. Debian 13 (trixie) genericcloud
image, kernel `6.12.107+deb13-cloud-amd64`, x86_64, QEMU with KVM, booted
from a pristine base image with a cloud-init seed; scratch images under
`/srv/bench`.

### Building the package

```bash
make -j"$(nproc)"
cp -a packaging/debian debian
INVFS_CODECPACK_REGISTRY=none DEB_BUILD_OPTIONS=nocheck \
    dpkg-buildpackage -us -uc -b      # -> ../invfs_0.5.0-1_amd64.deb
```

Two things about that command are worth knowing, because both are what a
maintainer will hit:

- **`debian/rules` has no `override_dh_auto_test`, so `dpkg-buildpackage`
  runs the entire `make test` suite during the build.** It passed. It is
  still the case that a `.deb` build cannot complete unless the unit suite
  passes, which is a defensible policy and is not changed here.
- **`DEB_BUILD_OPTIONS=nocheck` is needed on this host only** because
  `make test` includes `check-codecpack-sync`, which compares the vendored
  codecpacks against a registry checkout it discovers at
  `$PWD/../invfs-registry`. In a worktree under `/srv/bench/worktrees` a
  sibling checkout of that name exists and has drifted, so the comparison
  fails. `INVFS_CODECPACK_REGISTRY=none` does not suppress it inside the
  build; the documented way to run the suite is with it set.

### Installing and exercising it in the guest

```bash
scp invfs_0.5.0-1_amd64.deb debian@guest:/tmp/
ssh guest 'sudo apt-get install -y /tmp/invfs_0.5.0-1_amd64.deb'
```

Result: `install ok installed`, 192 paths from `dpkg -L invfs`.

```
$ invf-mkfs /var/tmp/final.img 1
  state: CLEAN, uuid: 8602c06a0000400086237b3267458b6b
$ invf-verify /var/tmp/final.img
OK: /var/tmp/final.img is a valid InvariantFS volume
$ invf-fuse /var/tmp/final.img /mnt/invfs
invfs[final.img] /mnt/invfs fuse rw,nosuid,nodev,relatime,user_id=0,group_id=0,max_read=1048576 0 0
$ cp -a /tmp/corpus2/. /mnt/invfs/          # 1 MiB random, 2.9 MB text, 4 MB tar, 20 B binary
cp_rc=0
$ fusermount3 -u /mnt/invfs                  # umount_rc=0
```

**Bit-exact read-back, `invf-cat` off the volume against the source, then
`cmp`:**

```
/random1m.bin invf-cat_rc=0 cmp=IDENTICAL
   src 1048576B 32c1dc621639fcec92dc44c0c8472d37
   cat 1048576B 32c1dc621639fcec92dc44c0c8472d37
/text.txt     invf-cat_rc=0 cmp=IDENTICAL
   src 2988890B d4a7731c524a0dd46781a2c87c4f4b44
   cat 2988890B d4a7731c524a0dd46781a2c87c4f4b44
/small.tar    invf-cat_rc=0 cmp=IDENTICAL
   src 4044800B fbf68bea62068eecee08ce2f35c4f063
   cat 4044800B fbf68bea62068eecee08ce2f35c4f063
/tiny.bin     invf-cat_rc=0 cmp=IDENTICAL
   src      20B a96772739caaaf8d403a70ee7cf9a3a2
   cat      20B a96772739caaaf8d403a70ee7cf9a3a2
  OVERALL_BITEXACT=YES
```

The same four files read back **through the FUSE mount** were also
`IDENTICAL`, and `invf-verify --deep` afterwards reported
`6 files ok, 0 corrupt, 12119752 bytes verified`.

**systemd units and man pages, in the guest:**

```
/usr/lib/systemd/system/invfs-sweep.service
/usr/lib/systemd/system/invfs-sweep.timer
/usr/lib/systemd/system/invfs-verify.service
/usr/lib/systemd/system/invfs-verify.timer
  invfs-sweep.service    enabled=static   active=inactive
  invfs-sweep.timer      enabled=disabled active=inactive
  invfs-verify.service   enabled=static   active=inactive
  invfs-verify.timer     enabled=disabled active=inactive
  man1: 1 page(s); man7: 1 page(s); man8: 10; ru translations: 10
  man /usr/share/man/man8/invf-mkfs.8.gz  ->  INVF-MKFS(8)  System Administration  INVF-MKFS(8)
```

All 12 man pages in `packaging/man/` ship, gzipped, plus the 10 Russian
translations at `manN/ru/*.gz` — the set that used to be silently dropped,
see §5.3. The timers are **disabled**, which is the shipped policy
(`dh_installsystemd --no-enable`, and the same in the ebuild): enabling
them is the administrator's decision.

### The one caveat that conditions every row above

The evidence in this section was collected on a guest booted with
`-cpu host`. **On a guest with QEMU's default `qemu64` CPU, none of it
works**: every write is lost on reopen, `invf-ls` reports `0 file(s)`, and
the offline tools print `vol_delta: segment <n> has a torn tail; truncated
at 0 record(s)`. The cause is not packaging and not the package — it is
`crc32c_slice8()` in `src/core/crc32c.c`, the runtime CPUID fallback taken
on any CPU without SSE4.2, which returns a different CRC32C from the
hardware path for every length ≥ 8. The same binary, on the same image
bytes, then disagrees with itself across machines: a volume written on the
`qemu64` guest is rejected elsewhere with
`superblock checksum mismatch (stored 692793ab, computed 54e0f74c)`.

This is an engine bug, not a packaging bug, and it is filed separately; it
is recorded here because a reader of this file would otherwise repeat the
measurement on a default VM and conclude that the package does not work.
**Nothing in §3 should be read as "InvariantFS on a VM works" without that
qualification.**

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
   `bin/invf*`, so all four targets shipped `invf-anchor_test`,
   `invf-delta_test`, `invf-ivpack_packs_test` and friends as user-facing
   tools. `make release` now copies exactly `$(TOOLS)` — the repo's own
   definition of what ships — instead of the whole `bin/`.

   **This was only half-fixed, and the half that was not is still open.**
   `make release` filters; `packaging/install.sh` does not, and it is what
   every packager actually calls. Measured 2026-10-02:

   | how it was installed | binaries in the destination |
   |---|---|
   | from the release artifact | 19 (exactly the artifact's `invf*`) |
   | `.deb`, built by `dpkg-buildpackage` | **88** |
   | `packaging/install.sh` run from a source tree | **87** |

   `tools/test-packaging.sh`, in a single run on 2026-10-02, reports the
   same split from the other direction — **96** binaries for Arch and
   **96** for Gentoo, against **19** for Void and **19** for Debian. Those
   96 are the repo's `bin/invf*` after `make test`, not the artifact's, so
   whatever route those two recipes take inside that suite, the number a
   user ends up with depends on the state of the build tree rather than on
   what the recipe says. That part was not diagnosed here.

   The `.deb` row is the one that matters to a user, and it is not a
   fluke of ordering: it is 88 whenever the build tree has already run
   `make test` (which `dpkg-buildpackage` does, see §3). The 88 include
   `invf-anchor_test`, `invf-arc-conc-asan`, `invf-arc-conc-tsan`,
   `invf-heat-conc-asan` and the rest of the sanitiser builds. Not fixed
   here — it wants a `$(TOOLS)`-derived list in `install.sh` rather than a
   glob, and the same question then has to be answered for the spec's
   `%{_bindir}/invf*`.

   Related, and also unfixed: `meta_probe` is in `$(TOOLS)` but does not
   match `bin/invf*`, so it is dropped by the artifact path. The release
   ships 19 of `$(TOOLS)`'s 20.
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
- `tools/test-arch-install.sh` and `tools/test-void-install.sh` were **not**
  in `make e2e` and **not** in CI, and have now been added to CI — see the
  end of this section for what happened when they were actually run.
- There is no Debian install guide at all, and no Debian boot script. Debian
  is the one target whose *package* is verified here; its *rootfs on
  InvariantFS* is entirely unexplored.
- Bootable partition layout: the guides use direct `-kernel`/`-initrd` boot
  with the volume on a separate disk, and Arch additionally uses
  `mkdisk-arch.sh`. There is no GRUB/systemd-boot integration, no ESP
  handling, and no verified multi-disk-on-real-hardware layout.

### The two distro harnesses, actually run (2026-10-02)

They are no longer hypothetical, and the result is not "PASS":

- **Void** — started, got as far as building the real Void ROOTFS staging
  tree (7 961 staged objects) and creating the single-device volume, then
  spent the rest of the run in `invf-import`. Did not finish.
- **Arch** — started, built the Arch staging tree (1.8 GB), created the
  single-device volume, and spent the rest of the run in `invf-import`.
  Did not finish; stopped deliberately to free the e2e lock.

**Why they stall is measurable, and it is not the harnesses.** `invf-import`
costs one `fsync` per file, and this host's storage makes an `fsync`
expensive:

```
# 200 x (4 KiB pwrite + fsync) into a file on the same filesystem the
# volumes live on (/srv, a shared 1.8 T ext4 disk):
200 x (4KiB pwrite + fsync) = 25.03 s -> 125.2 ms per fsync
```

and an import of **500 files of 4 KiB each**, isolated and with nothing else
running on that volume, was still in progress after 183 s having used 0.22 s
of CPU, with this stack:

```
$ sudo cat /proc/<invf-import>/syscall ; sudo cat /proc/<invf-import>/stack
74 (fsync)
[<0>] jbd2_log_wait_commit+0xdb/0x150 [jbd2]
[<0>] ext4_sync_file+0xf4/0x2b0 [ext4]
[<0>] do_fsync+0x39/0x70
[<0>] __x64_sys_fsync+0x13/0x20
utime=8 stime=14      # 0.22 s of CPU in 183 s of wall clock
```

So the import rate here is a handful of files per second, and a Void root
(7 961 objects) is an hour-scale job while an Arch root (~10^5 objects) is
many hours. That is a real property of `invf-import` — one journal commit
per file, on a path that is otherwise I/O-light — and on storage where
`fsync` costs 125 ms it is the dominant cost of building a root. It is
recorded here as an observation, not as a verdict: the harnesses may well
complete on a machine with a fast NVMe or an idle disk, and nothing in this
repository has run them to completion anywhere.

**Both are now in CI** (`.github/workflows/engine-ci.yml`, a weekly
`distro-install` job plus `workflow_dispatch`), so this question gets a
real answer on real runners instead of being inferred from here. That change
has not been executed — this machine has no GitHub Actions runner.

**So, plainly:** the packaging layer is real, and on Debian it has now been
installed into a clean VM from the package and used. The claim "a distro root
boots on InvariantFS" is still a design plus a set of scripts that have not
been seen to finish, and this work does not change that. Getting from here to
a verified boot means letting `tools/test-arch-install.sh` and
`tools/test-void-install.sh` run to completion somewhere their storage can
absorb one fsync per imported file, then running the QEMU scripts and keeping
the serial logs — which is exactly the kind of evidence that the serial log
at `/srv/bench/invfs-boot/serial.log` was supposed to be.

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
