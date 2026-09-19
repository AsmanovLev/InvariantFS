# WP59-codec-policy — on-volume codec policy descriptor (`PCK0`) + mount gate

**Branch:** `wp/59-codec-policy`
**Worktree:** `/tmp/invfs-wp59`
**Severity:** HIGH (mount correctness / data safety)
**Source:** design thread this session (codec packs, self-hosting, "chicken-and-egg")
**Estimated effort:** 3–5 days (descriptor + gate + registry interaction)

---

## Scope

Add a **block-0 policy descriptor `PCK0`** that records the codec-pack
configuration a volume was written with, and make the engine **gate mount/read
paths on it**. This is the contract that makes codec packs safe: a volume whose
decoder set is unknown must not silently open and let files be read (or worse,
rewritten) with the wrong codec.

It also solves the **chicken-and-egg** problem by anchoring the policy *outside*
the file tree (block 0, builtin-readable), so it can be read before any pack is
loaded. This is the first implementation of the WP19 `INVFS_CLASS_ANCHORED`
intent (pinning codec-bearing files so they stay builtin-readable) — see
WP59a.

The invariant stays intact: **what each record contains is still self-describing
(algo/entries/pba) and each record is independently decodable.** `PCK0` answers
*"which decoder set is this volume written against"*, not *"how to decode byte
N"*. It is a readiness contract, not a second source of truth about content.

---

## Why

- Codec packs are external executables (`vol_cpack.c`) and on-volume packs
  (self-hosting, `tools/invf-sweep.c:270` `/.invfs/codecpacks`). Without a policy
  descriptor, opening a foreign volume gives no signal that packs are missing —
  reads fail deep in the codec path, or a rewrite re-encodes with a different
  codec.
- The volume can be carried between machines (external disk). The policy must be
  readable **from the volume itself, before mounting**, without any pack.
- Version drift between packs must be detectable (a codec's wire format can
  change), hence `version` + `min_read` in every pack manifest.

---

## `PCK0` wire format (block 0, offset `0x3C4`)

Follows the existing descriptor convention (`magic[4]` + fields + `crc32c`,
zeros = absent, outside the superblock checksum; see RDP0/RSZ0/CKP0/... in
`invarifs.h`). `0x3C4` is free (MET0 ends at `0x3C4`).

```
0x3C4  char     magic[4]        "PCK0"
0x3C8  u32      version         1
0x3CC  u32      n_codecs        0..63
0x3D0  u32      policy_flags    bit0 = BASIC_ONLY (no pack required at all)
0x3D4  u64      conf_hash       BLAKE3 (low 8B) of /.invariantfs/config/packs.conf (0 = none)
0x3DC  u32      conf_len        length of packs.conf (0 = none)
0x3E0  u8       conf_encoding   INVFS_ALGO_* used to store packs.conf
0x3E4  [n_codecs] codec refs (24 B each, sorted by codec_id):
              u32 codec_id      stable id, independent of pack version
              u32 algo          INVFS_ALGO_* registry value
              u16 version       pack version installed at write time
              u16 min_read      minimum reader version that can decode
              u8  pack_id[12]   NUL-padded pack name (e.g. "raw_image")
0x...  u32      crc32c           over the descriptor with this field 0
```

`n_codecs` is capped so the descriptor fits in one block:
`offsetof(crc32c) + 4 <= 4096` → with a 128-byte fixed area, **max 63 codecs**
(63*24 = 1512 B). v1 writes the live set only (families actually present with a
pack codec).

Rules:
- `mkfs` **always writes PCK0** (BASIC_ONLY if no packs configured).
- A volume **without** a valid PCK0 is *legacy/unknown policy* → hard gate (with
  the `--ignore-missing-codecs` escape), never silently decoded.
- `conf_hash`/`conf_len`/`conf_encoding` let the engine detect a stale/foreign
  `packs.conf` without reading it; `conf_encoding` must be a **builtin** codec
  (see WP59a) so the config is readable with no packs.

---

## Gate semantics (the contract)

Evaluated at `vol_open`/`invf-fuse` mount and on the file-data paths. A codec
reference is **satisfied** iff a pack with matching `codec_id` is installed and
`installed.version >= min_read` (and, unless `--ignore-codec-versions`, the
installed version is compatible with `version`).

Resolvers, in priority order (see WP60):
1. `$INVFS_CODECPACKS` (explicit)
2. host `/.invariantfs/codecpacks` (admin-provided, trusted)
3. on-volume `/.invariantfs/codecpacks` (self-hosting; **declaration only in
   v1**, v2 may trust/use them)

Behaviour:

| Situation | Default | Notes |
|---|---|---|
| all codecs satisfied | mount RW | normal |
| missing codec(s) | **refuse mount** | loud list: pack_id, version, why |
| `version` mismatch (installed too old) | **refuse mount** | separate from missing |
| `--ignore-missing-codecs` | mount **RW** | per-file: read→EIO, write/replace/truncate→EIO/EPERM, **unlink/rmdir allowed** |
| `--ignore-codec-versions` | allow version mismatch | independent of missing |
| volume has no PCK0 (legacy) | **refuse mount** | message: "no codec policy; use --ignore-missing-codecs" |

Per-file policy under `--ignore-missing-codecs`:
- read of a file whose recipe uses an unsatisfied codec → `EIO`
- write/truncate/replace on such a file → `EPERM`/`EIO` (**not implemented**
  beyond refusal; no re-encode-with-different-codec path)
- `unlink`/`rmdir` → **allowed** (does not need to decode content)
- metadata paths (`getattr`, `readdir`, `stat`, `invf-stats`, fsck metadata)
  → **not gated**

Gated operations (require satisfied policy): `invf-cat`, `invf-cp`, `verify`,
`--deep` data reads, sweep data passes. Non-gated: `invf-ls`, `invf-stat`,
`invf-stats`, metadata fsck, `vol_open` scan.

The gate is **not** applied to a read-only inspection mode used by repair
tools; `invf-fsck` metadata may open with `--inspect`.

---

## Manifest additions (registry side, WP60)

Each `*.codecpack/manifest` gains:
- `codec_id` — stable u32, independent of `version` (the PCK0 key)
- `version` — pack version
- `min_read` — minimum reader version (your `backwards_compatible` idea:
  a monotone lower bound; `version` may grow while `min_read` stays)
- `format_id` (optional) — stable wire-format id, if codec format ≠ pack version
- `family` / `category` — classification (`raw_audio`, `raw_image`,
  `container`, `archive`, `code`; `primary`/`optional`)
- `replaces` / `provides` (registry metadata; see WP60 alternatives)
- `depends` (hard) / `recommends` (soft) — external tools (`python3`, `cjxl`)

`packs.conf` (builtin-readable, `/.invariantfs/config/packs.conf`) mirrors the
active set: `codec_id version min_read pack_id` lines + selection per family.

---

## Files

- `src/core/invarifs.h` — `INVFS_PCK0_OFF 0x3C4`, `invfs_pck0`, codec-ref
  struct, helpers.
- `src/cli/mkfs.c` — write PCK0 (BASIC_ONLY default; populated from
  `INVFS_CODECPACKS` when present at mkfs).
- `src/core/volume.c` — read/validate PCK0 at open; expose `vol_codec_policy`;
  gate hook.
- `src/core/codec.c` / `vol_cpack.c` — map active packs → codec refs; the
  satisfied/unsatisfied check.
- `src/cli/fuse_fs.c` — mount gate + `-o ignore-missing-codecs`,
  `-o ignore-codec-versions`; per-file EIO/EPERM.
- `src/cli/{cat,verify,cp}.c` + `tools/invf-sweep.c` — gated paths.
- `src/cli/{ls,stat}.c` + `invf-stats` — explicitly not gated (assert in tests).
- `src/doc/02-on-disk-format.md` — block-0 table row for PCK0.
- `impl_docs/AUDIT.md` — note PCK0 replaces the (unlanded) `CLASS_ANCHORED`
  boot-path role for policy readiness.

---

## Validation

1. `make test` — pass; add unit tests for PCK0 encode/parse, gate decisions
   (missing/old/legacy/overrides), and the 63-codec bound.
2. Crafted images: no PCK0; torn PCK0 (bad crc); `n_codecs=64` (refused);
   unknown `codec_id`; `min_read > installed`.
3. e2e:
   - `test-meta-extent-walk.sh` on a volume with PCK0 (byte-identical metadata
     otherwise).
   - a new `test-policy-gate.sh`: mount without packs → refuse; with
     `--ignore-missing-codecs` → RW + read EIO + unlink OK + metadata OK;
     `--ignore-codec-versions` for a version bump; legacy (no PCK0) refused.
   - `test-dedupe.sh` / `test-binbatch.sh` / `test-textzone.sh` unchanged
     (they use builtin codecs → BASIC_ONLY path).
4. `invf-cat`/`verify` return the gate error, not a deep codec crash.
5. Crash: power-cut between superblock and PCK0 write leaves a consistent
   volume (PCK0 is written in the mkfs single block-0 write; runtime RMW must
   barrier like the other descriptors).

---

## Out of scope

- On-volume pack **execution**/trust (v2; WP60).
- `INVFS_CLASS_ANCHORED` file pinning of packs (WP59a).
- Pack signatures (decided: not required; see WP60 risk note).
- Per-file (rather than per-volume) policies.

---

## Coordination notes

- Subagent ID: `wp59-codec-policy`; e2e via `INVFS_E2E_AGENT=wp59-codec-policy`.
- Depends on: WP58a (record layout; landed 48b1366).
- Blocks: WP60 (registry must emit `codec_id`/`min_read`), mount-gate tests.
- Helper isolation hardening (seccomp/netns/rlimits) is WP61, independent.
