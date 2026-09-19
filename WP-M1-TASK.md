# WP-M1 — v3 format skeleton: superblock, root area, mkfs, open

**Branch:** `wp/m1-format-v3`
**Worktree:** `/tmp/invfs-wp-m1`
**Severity:** HIGH (format-breaking)
**Source:** `impl_docs/design-meta-v3.md` §12 (on-disk format v3)
**Estimated effort:** large (first WP of the metadata-v3 programme)

---

## Scope

Land the **on-disk v3 skeleton** only: the format marker, the root-area
descriptor, and the `mkfs` / `vol_open` handling for a **v3, empty**
volume. No trees, no delta, no metadata operations yet.

Files in scope:
- `src/core/invarifs.h` — v3 constants + descriptor structs (additive).
- `src/cli/mkfs.c` — write a v3 volume (marker + zeroed root area).
- `src/core/volume.c` — `vol_open`/`vol_close` accept v3 and mount an
  empty namespace; `vol_sync`/`vol_flush` stay no-ops on empty v3.
- `src/core/vol_fsck.c` / `src/cli/fsck.c` — minimal v3 accept.
- `tools/test-meta-v3.sh` — new e2e leg 0 (mkfs/open/close/fsck).

Do **not** delete v2 yet (that is WP-M21). v3 lands alongside; the default
`mkfs` may keep writing v2 until WP-M2 wires the v3 engine, but the v3
path must be reachable via `INVFS_V3=1` (env) on mkfs and must round-trip.

---

## Why

Everything in the programme depends on a stable v3 on-disk interface
(design §12). This WP freezes that interface so later WPs (page format,
trees, delta) can proceed independently.

---

## Design

- `INVFS_VERSION` stays 3 for v3 volumes; keep the 8-byte `"InvariFS\0"`
  magic. Add `VOLF_V3 0x00000010` in `sb.vol_flags` (outside the
  superblock checksum, per the existing convention).
- Add a **root-area descriptor** in block 0 at the first free
  16-byte-aligned offset after the existing descriptors (RDP0/RSZ0/CKP0/
  CMP0/DEVT and any later ones — read `invarifs.h` to pick; document the
  chosen offset in a comment). Layout:

```
char     magic[4]        "RT30"
uint32_t version         1
uint32_t page_size       metadata page size (default 4096)
uint64_t root_slot[2]    pba of base root slot A / B (0 = empty)
uint64_t delta_pba       pba of the active delta segment (0 = none)
uint64_t seq             root generation (monotone; higher = newer)
uint32_t crc32c          over the descriptor with this field read as 0
```

- Add **v3 base-page and block-pointer structs** (`invfs_page_hdr`,
  `invfs_blkptr`) to `invarifs.h` as the frozen wire format for WP-M2/M3.
- `mkfs` v3 path (`INVFS_V3=1`): set `format_version=3` and `VOLF_V3`,
  allocate a small root area (e.g. 2 pages) in the metadata zone, zero it,
  write the RT30 descriptor with `seq=0`, empty root slots, no delta.
- `vol_open`: if `VOLF_V3`, skip the v2 record scan; present an empty
  namespace and mount CLEAN. `vol_close` writes the superblock CLEAN.
- Keep the existing v2 path working when `VOLF_V3` is clear.

### Crash / durability

- The RT30 descriptor is written last, after the root area is durable.
- `seq` is the atomicity anchor: a higher `seq` with a valid CRC wins
  (reuse the `l2p_replay` idiom, `volume.c:644-669`).

---

## Validation

1. `make test` — all 4 unit binaries pass.
2. `INVFS_V3=1 invf-mkfs t.img 1` then `invf-fuse` mount: empty root
   lists cleanly; `invf-fsck t.img` exits 0; unmount CLEAN.
3. `bash tools/run-e2e.sh tools/test-meta-v3.sh` (leg 0) PASS.
4. A plain (v2) `invf-mkfs` volume still mounts and behaves as before.

---

## Out of scope (do NOT touch)

- Trees, deltas, fold, save points, page allocator, sweep.
- Deleting v2 code or the mapper.
- CLI tools beyond mkfs/fsck wiring.

---

## Coordination notes

- Subagent ID: `wp-m1-format-v3`; pass `INVFS_E2E_AGENT=wp-m1-format-v3`.
- Dependencies: none (base = `wp/meta-v3-design`).
- Blocks: WP-M2 (page format + allocator), WP-M4 (fsck).
