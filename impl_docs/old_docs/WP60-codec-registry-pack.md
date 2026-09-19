# WP60-codec-registry-pack — codec-pack registry + `invfs-pack` manager

**Branch:** `wp/60-registry-pack`
**Worktree:** `/tmp/invfs-wp60`
**Repo:** `git@github.com:AsmanovLev/InvariantFS-registry.git` (created)
**Severity:** MEDIUM (ecosystem; no engine format change)
**Source:** design thread this session
**Estimated effort:** 4–6 days (manager + repo + CI release)

---

## Scope

Two pieces:
1. **`InvariantFS-registry`** — a git repo holding codec packs + a machine
   index (`index.json`) + JSON schema. Layout:
   ```
   codecpacks/<name>/<version>/{manifest, helper..., sha256, sig?}
   codecpacks/index.json          # all packs, all versions
   kernels/index.json             # reserved (kernel-compat profiles)
   schema/codecpack.schema.json
   schema/index.schema.json
   ```
2. **`invfs-pack`** — a host CLI that installs/removes/updates/verifies packs
   from a repo (`file://` or `https://`), and manages **alternatives** for one
   input family (user picks the encoder: FLAC/APE/WavPack, JXL/AVIF, ...).

This is the user-facing half of the codec policy (WP59): `invfs-pack` is what
makes `PCK0`'s required set satisfiable.

---

## `invfs-pack` UX

```
invfs-pack list [family]         # installed + available
invfs-pack search <term>
invfs-pack info <pack>
invfs-pack install <pack>        # + hard depends; conflict prompt (default: keep existing)
invfs-pack install [--without <pack>]
invfs-pack remove <pack>
invfs-pack update [<pack>]
invfs-pack verify [<pack>]       # sha256 (and sig if present)
invfs-pack alternatives <family> # show candidates + current choice
invfs-pack use <family> <pack>   # switch primary encoder (future sweeps only)
invfs-pack where                # resolved pack roots + repo
```

Decisions locked this session:
- **Conflict default: install but do not touch the existing pack.** When the
  new pack covers a family already covered, keep both; never silently replace.
- **No `--family` argument for install.** Alternatives are discovered via
  `provides`/`replaces` metadata; the explicit family form is only for `use` and
  `alternatives`.
- **Version mismatch** is a separate hard refusal (`--ignore-codec-versions` in
  WP59), with soft versioning via `min_read` + `version`.
- **Overwrite/replace of files under an unsatisfied codec is not implemented**
  (WP59 gate refuses; only unlink is allowed).
- **No required pack signatures** (decided: rare vector; optional `sig` field
  supported but not enforced by default).
- **Statics:** C helpers are built **statically** in the registry (musl
  preferred; fall back to `glibc-static` when musl is unavailable). Static is a
  *build* property of the registry, **not** a user-facing requirement. Python /
  external-tool packs keep `depends`/`recommends` (`python3`, `cjxl`, `djxl`,
  `mac`, `wavpack`, ...).
- **Family classification:** `raw_audio`, `raw_image`, `container`, `archive`,
  `code`. `category`: `primary`/`optional`.

### Manifest additions

```
name, algo, codec_id, pack_version, version, min_read, format_id
family, category, provides, replaces, conflicts, priority
caps, type, sniff.*, encode/decode/...
depends, recommends          # external tools: "python3,cjxl,djxl"
```

`provides`/`replaces` drive alternatives:
```
flacr   provides=raw_audio  priority=100
ape     provides=raw_audio  priority=50  requires=(external) mac
wavpack provides=raw_audio  priority=50  requires=(external) wavpack
```
`install ape` → sees flacr already provides raw_audio → **keeps flacr**, reports
that both are present; `use raw_audio ape` makes APE the primary for future
sweeps.

---

## Pack roots and trust (v1 / v2)

Priority (WP59):
1. `$INVFS_CODECPACKS`
2. host `/.invariantfs/codecpacks` — admin-provided, **trusted**
3. on-volume `/.invariantfs/codecpacks` — self-hosting; **v1 declaration only**
   (read `packs.conf`, match ids/versions), **v2** may execute them behind
   `--trust-volume-packs` (no mandatory signatures)

`invfs-pack` installs into the **host** root by default;
`--on-volume <img|mnt>` installs the same pack onto a volume (for root-mode /
sweepboot self-hosting).

Legacy path `/.invfs/codecpacks` (and `/usr/lib/invfs/codecpacks`) stay as
read fallbacks so existing sweepboot keeps working.

---

## Deliverables

- `InvariantFS-registry` seeded with the existing 10 packs
  (`raw_image, splt_test, vdi, ntfs, rawdisk, fatfs, ext4fs, xfs, qcow2, p7z,
  jxl`) migrated to the new manifest fields + `index.json` + schema.
- `invfs-pack` (C11, in the main repo: `src/cli/pack.c` + `tools/invfs-pack`,
  or a standalone tool in the registry repo — decide at implementation).
- CI in the registry repo: build static helpers, hash, emit `index.json`,
  attach release assets.
- Docs: `packaging/man/invf-pack.8`, README section.

---

## Validation

1. `invfs-pack list/info/install/remove/verify` against `file://` repo.
2. Alternatives: install flacr then ape → both present, no clobber; `use` flips
   the primary; `alternatives raw_audio` lists all three.
3. `depends`/`recommends`: `raw_image` without `python3`/`cjxl` installs but
   reports degraded coverage; the engine leaves such files RAW.
4. sha256 verified; a tampered pack is refused.
5. Static C helpers run on a musl host and a glibc host (portability check).
6. Integration with WP59: `mkfs` with `INVFS_CODECPACKS=<repo>` populates PCK0;
   mounting on a host without the packs is refused; installing them via
   `invfs-pack` then satisfies the gate.
7. e2e: `tools/test-policy-gate.sh` (WP59) plus the existing suite green.

---

## Out of scope

- Full dependency solver / transactional rollback (v1: flat alternatives only).
- Automatic re-encode of existing files on `use` (only future sweeps).
- Mandatory signatures / trust chains.
- OCI registry transport (plain HTTP + `file://` only).

---

## Coordination notes

- Subagent ID: `wp60-registry-pack`; e2e via `INVFS_E2E_AGENT=wp60-registry-pack`.
- Depends on WP59 (PCK0 fields) for the end-to-end gate test; the registry
  itself can be seeded in parallel.
- Deploy note: may be run as one of the 4 agents (registry + manager).
