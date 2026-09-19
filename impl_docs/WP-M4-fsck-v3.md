# WP-M4 — fsck v3 skeleton: page and root validation

**Branch:** `wp/M4-fsck-v3`
**Worktree:** `/tmp/invfs-wp-M4`
**Severity:** HIGH (a false-clean fsck silently accepts a corrupt base)
**Source:** `impl_docs/design-meta-v3.md` §7 (recovery), §12 (format), §15.1, §16 (invariants)
**Estimated effort:** medium

---

## Scope

Add the **v3 validation path** to fsck: root-slot verification (double
slot, seq + CRC), and a full page-tree walk that validates every reachable
base page (`magic`, `crc32c`, `gen`, `level`, pointer CRC matches the
child page). v2 record scanning is untouched.

Files:
- `src/core/vol_fsck.c` — v3 branch in `vol_fsck_scan` (`vol_fsck.c:434`);
  new `fsck_v3_walk` helpers alongside the existing `fsck_*` helpers.
- `src/cli/fsck.c` — v3 reporting / `-f` flag plumbing (`fsck.c:25`).
- `src/core/volume.h` — report struct fields additive if v3 counts are
  reported (no signature removal).

No repair beyond quarantine/refuse is in scope; no delta replay (no delta
exists yet).

---

## Why

§7 makes recovery read the base root from the double-slot area and fall
back on a torn slot. If fsck cannot independently verify that root and the
pages it reaches, a corrupt v3 volume reports clean. §16 lists "root
double-slot" and "fuzz the record/page parsers" as required invariants.

---

## Design

The design doc is explicit on the invariants but **silent on v3 fsck's
repair policy**; this WP therefore only **detects and reports**, reusing
the existing v2 disposition (quarantine / refuse / `-f` rebuild is a
follow-up WP — do not invent repair behavior here).

### v3 scan entry

```
vol_fsck_scan(v, rep, fix):
    if (v->sb.vol_flags & VOLF_V3):
        return fsck_v3_scan(v, rep, fix);   /* new */
    ... existing v2 path ...
```

### Checks (each failure named in the report)

- **RT30**: magic `"RT30"`, version, `page_size` in {4096,16384}
  (per WP-M1/M2 decision), `crc32c` over the descriptor with the CRC field
  zeroed (`invarifs.h` crate convention, e.g. `invfs_met0`).
- **Root slots**: `root_slot[0]`, `root_slot[1]`. Both zero = empty volume
  (clean). Both valid = higher `seq` wins; equal = ambiguous → report.
  A slot whose `blkptr.checksum` does not match the page at `pba` is torn.
- **Tree walk** from the winning root: DFS/BFS over `blkptr`s. For each
  page: `magic == "MPG1"`, `crc32c` over page bytes with CRC zeroed,
  `level` monotone decreasing from root to leaf, `nentries` consistent
  with the packed-entry area bounded by the page size, every child pointer
  within the metadata zone and its stored `checksum` equal to the child
  page's computed CRC.
- **Cycle / reachability**: a visited-pba set detects cycles and duplicate
  child references (a valid B+-tree never shares a child between parents;
  a shared page means a COW bug).
- **Allocator cross-check**: every reachable page is marked allocated in
  the metadata bitmap; a reachable-but-free page is reported (the v2
  "bitmap divergence" analogue, `volume.c:1838`).

### Output

Existing `invfs_fsck_report` fields for counts; v3 adds
`pages_walked`, `slots_torn`, `bad_pages`, `cycles`. `invf-fsck` exits
non-zero on any v3 failure, as today.

---

## Validation

1. `make test` — must pass (no regressions; fsck is exercised by e2e).
2. `INVFS_V3=1 invf-mkfs t.img 1` (via WP-M1/M2) → `invf-fsck t.img`
   exits 0 for an empty v3 volume.
3. Crafted corruption: flip one byte in a page body and in the RT30
   descriptor; `invf-fsck` must name the bad page/slot, not crash and not
   report clean.
4. `make fuzz` — the existing fuzz harness; WP-M2/M3 page parsers must be
   reached if the harness wires them (if not, add a page-parser fuzz leg
   inside `tools/test-fuzz.sh`; the design lists this in §16).
5. `bash tools/run-e2e.sh tools/test-meta-v3.sh`.

---

## Out of scope (do NOT touch)

- v3 **repair** (rebuild a damaged tree, re-derive root): follow-up WP.
- Delta replay / overlay / fold / save-point consistency (later WPs).
- v2 record fsck behavior and `fsck_rebuild_one` (`vol_fsck.c:57`).
- Changing WP-M2 page format or WP-M3 entry encoding.

---

## Coordination notes

- Subagent ID: `wp-M4-fsck-v3`; run e2e with `INVFS_E2E_AGENT=wp-M4-fsck-v3`.
- E2E gate: `bash tools/run-e2e.sh tools/test-meta-v3.sh`.
- Dependencies: **WP-M2** (page CRC/`blkptr`), **WP-M3** (tree shape for
  the walk). WP-M1's minimal v3 accept is replaced/extended here.
- Blocks: nothing; later WPs reuse `fsck_v3_walk` for reclaim checks.
