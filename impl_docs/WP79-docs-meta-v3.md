# WP79 — align docs with Meta-v3

**Branch:** `wp/79-docs-meta-v3`
**Worktree:** `/home/user/invfs-wp79`
**Severity:** MEDIUM (docs describe a filesystem that no longer exists)
**Source:** user report — "AGENTS.md/README must match reality; RAW is no longer append-only"
**Estimated effort:** S

---

## Scope

Docs-only. Bring the user-facing documentation in line with the Meta-v3
(v0.5.0) architecture: `AGENTS.md` §2, `README.md`,
`docs/architecture/OVERVIEW.md`. No code changes.

---

## Why

The docs still described the v2 model: an `L2P journal`, an append-only
`inode area`, a `Mapper` table, tombstone kills, "inline data < 2 KiB", a
serialized e2e runner, and an "append-only RAW zone". None of that is true on
the default (v3) format.

Evidence gathered from code:

- `src/core/invarifs.h:206-211` — the four zone fields are **advisory policy**
  over one shared free-block pool; raw-class allocation overflows into
  shadow-space blocks with the class tag unchanged (`zone=0`).
- `src/core/volume.c:2679` — `alloc_raw_or_shadow` **always** sets
  `*zone_out = INVFS_ZONE_RAW`: placement never decides the class.
- `src/core/vol_btree.c` / `vol_delta.c` / `vol_fold.c` / `vol_metabuf.c` —
  no L2P usage; v3 metadata is the `RT30` descriptor + COW B+ tree base +
  append-only Delta Log, merged by the fold worker.
- `src/cli/mkfs.c:166-171` — v3 is the default format.
- `src/cli/fsck.c` v3 branch — reports base-tree health, not v2
  orphan/missing counters.
- `tools/run-e2e.sh:2` — the runner is **parallel-safe** (private mount
  namespaces), not globally serialized.

---

## Changes

### AGENTS.md §2

- **§2.3 zone table** — replaced `L2P journal` / `Inode area` / `Mapper` with
  `Meta-v3 area` (RT30 + COW B+ tree + Delta Log) and the advisory-zone note;
  added the seal-parity row.
- **§2.4 write path** — write-once segments + Delta Log mutation + fold;
  `unlink` = delta delete (tombstones gone); bitmap flushed on flush/close.
- **§2.5 sweep** — drain → re-cluster → dedupe/GC → transcode → id-keyed
  publication.
- **§2.6 recovery** — Delta Log replay; v3 rollback on **SPT0 savepoints**;
  the v2 `CKP0` + `\x01reten` model is retired.
- **§2.7 / §2.10** — metadata-zone symptom reworded (base-page fallback to the
  shadow pool); "tombstones accumulating" → dead segments/delta entries.

### README.md

- "Append-only zones" → write-once ingestion + advisory zones.
- "Recovery tooling — append-only WAL" → Delta Log.
- Removed the false "Inline data < 2 KiB in the inode record" bullet →
  replaced with the real O(1) mount property.
- `make test` "4722" → "~4700"; e2e described as parallel-safe (not
  "serialized via /tmp/invfs-e2e.lock").
- Non-features + comparison rows reworded (write-once; savepoints).

### docs/architecture/OVERVIEW.md

- §2 zone table: dropped the `L2P Journal` row, marked RAW advisory, added the
  Meta-v3 note.

---

## Validation

- Docs-only; `make` / `make test` unaffected.
- Each changed claim was checked against the file:line evidence above.
- `bash tools/check-repo-hygiene.sh` stays OK.

---

## Out of scope / follow-ups (still v2-era, need their own WP)

- `impl_docs/FILEMAP.md` — does not know
  `vol_btree/vol_delta/vol_metabuf/vol_fold/vol_reclaim/vol_spt0/vol_meta_merge/vol_plugin_client`.
- `impl_docs/FUNCTIONS.md`, `impl_docs/TYPES.md` — generated indexes, stale.
- `impl_docs/old_docs/AUDIT.md`, `INCIDENTS.md` — v2-epoch; statuses stale.
- `src/doc/*` on-disk-format chapters.
- Stale source comments, e.g. `src/cli/mkfs.c:9,307` still say "L2P journal +
  indexes" for the metadata zone.

---

## Coordination notes

- Subagent ID: `wp79-docs`
- No e2e gates (docs-only).
- Dependencies: describes the post-WP77/78 target state for savepoints and
  sweep batching; those WPs must land for the claims to be fully true.
