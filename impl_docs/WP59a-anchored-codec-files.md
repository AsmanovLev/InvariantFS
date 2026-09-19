# WP59a-anchored-codec-files — pin codec-bearing files builtin-readable (`INVFS_CLASS_ANCHORED`)

**Branch:** `wp/59a-anchored`
**Worktree:** `/tmp/invfs-wp59a`
**Severity:** MEDIUM (self-hosting correctness)
**Source:** WP19's unlanded `INVFS_CLASS_ANCHORED`; design thread this session
**Estimated effort:** 2–3 days

---

## Scope

Implement the **anchored class** the WP19 finding reserved
(`impl_docs/AUDIT.md:118`: "NOT LANDED: `INVFS_CLASS_ANCHORED=10` /
`anchor_boot` mount opt / `user.invfs.anchor` xattr — zero refs in the tree").
`anchored` marks an inode whose **stored bytes must never be transcoded into a
form that requires a codec pack to read**. Its purpose is the chicken-and-egg
problem of WP59: the volume's own codec policy/config and the codec packs it
self-hosts must be readable by a bare engine with no packs installed.

---

## Why

WP59 anchors the policy in block 0, but the **config and pack files on the
volume** are still ordinary files. After a sweep they can become PPMd/ZSTD
batch members (fine — builtin) or, worse, be claimed by a pack codec (the exact
file you need before you can load packs). `anchored` closes that hole:
- `/.invariantfs/config/packs.conf` is anchored → always stored with a builtin
  codec, never contended by container/pack codecs.
- `/.invariantfs/codecpacks/**` is anchored → self-hosting `--extract-packs`
  (WP23) can always read them, even on a volume swept with those same packs.
- FUSE/import plants the anchor attribute on these paths at creation.

The WP19 intent also had a boot-trace pinning use ("boot files stay hot"); keep
that as a secondary application: anchored files are exempt from generic
recompression and demotion.

---

## Design

Reuse the existing per-file metadata (INO2 ext, `invfs_meta_ext_hdr`) instead of
a new record field: an **xattr TLV** (WP16b size-capped) named
`user.invfs.anchor` with a 1-byte value, plus a class value in the AST/class
space. WP19 reserved class id **10 = ANCHORED**; the enum currently stops at
`DEFER_ENOSPC=9` (see `vol_sweep.c` classification) — extend it.

```
class enum: ... 9 = DEFER_ENOSPC, 10 = ANCHORED
```

Rules:
- The **sweep classifier** never assigns a pack/container codec to an anchored
  file; it may still apply builtin LZ4/ZSTD/PPMd-via-batch. Anchored skips
  dedupe remap by name and demotion/tier eviction.
- `vol_sweep` transcode lanes check `invfs_inode_is_anchored()` first and
  return "leave builtin" (mirrors the `record_owns_siblings` guard pattern).
- `invf-import` and FUSE set the anchor on `/.invariantfs/config/*` and
  `/.invariantfs/codecpacks/**` (and optionally on a list from
  `INVFS_ANCHOR_PATHS`).
- `--extract-packs` (WP23) asserts every extracted file was readable through
  builtin codecs; add a regression that sweeps a volume holding packs for its
  own codecs and still extracts them.

Mount opt `anchor_boot` (WP19): optional; pins a small set instead of paths.
Out of scope for v1 unless trivial.

---

## Files

- `src/core/invarifs.h` / `volume_internal.h` — class value 10, xattr name,
  `invfs_inode_is_anchored()` helper reading the INO2 ext.
- `src/core/vol_sweep.c` — classifier + transcode lanes skip anchored.
- `src/core/vol_tier.c` — no demotion/eviction of anchored.
- `src/core/vol_dedupe.c` — skip remap (like the TEXT gate).
- `tools/invf-import.c`, `src/cli/fuse_fs.c` — set the anchor on the reserved
  paths.
- `tools/invf-sweep.c` — `--extract-packs` regression hooks.
- `impl_docs/AUDIT.md` — close the WP19 `CLASS_ANCHORED` finding.

---

## Validation

1. `make test` — pass; unit test for classify(anchored) == builtin-only,
   anchor set/read round-trip.
2. e2e `test-sweepboot.sh` — extend with a leg where the volume hosts packs for
   its own codecs; sweep, then `--extract-packs` still reads them bit-exact.
3. `test-binbatch.sh` / `test-textzone.sh` unchanged.
4. A crafted anchored file under `/.invariantfs/config/` survives a sweep as a
   builtin-readable member (inspect the recipe's algo).

---

## Out of scope

- Pack trust/execution (WP60), policy gate (WP59).
- General freeze/immutability semantics beyond "builtin-readable".
- The full WP19 heat/`anchor_boot` boot-trace pinning.

---

## Coordination notes

- Subagent ID: `wp59a-anchored`; e2e via `INVFS_E2E_AGENT=wp59a-anchored`.
- Depends on WP58a. Pairs with WP59 (policy) and WP60 (registry paths).
- Land order: WP58a → WP59a → WP59 (or WP59 first if the gate is needed
  independently; WP59a only needs the paths reserved).
