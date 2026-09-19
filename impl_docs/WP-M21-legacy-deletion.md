# WP-M21 — legacy deletion (v2 records, mapper, consistent-cut, reten, migrate-v2)

**Branch:** `wp/M21-legacy-deletion`
**Worktree:** `/tmp/invfs-wp-M21`
**Severity:** HIGH (a hard cutover; a missed v2 dependency breaks v3)
**Source:** `impl_docs/design-meta-v3.md` §10, §11, §15.6, §16
**Estimated effort:** large (mostly deletion + build/test cleanup)

---

## Scope

Delete the v2 metadata machinery so the codebase has a single metadata model
(v3-only). No v3 behaviour change beyond removing dead paths. §10 list:

- `src/core/invarifs.h` — `invfs_inode_rec`, `DELT`, inline `INO2`,
  `INVFS_MAX_REC_LEN` growth paths, flat mapper structs
  (`invarifs.h:1041-1057`), `invfs_ckp0` for v3.
- `src/core/vol_records.c` — append/tombstone/retire record paths.
- `src/core/vol_meta_merge.c` — meta mapper merge; `meta_mapper` references.
- `src/core/vol_rollback.c` — `CKP0`/`\x01reten` registry (`volume.h:555`),
  hidden owner-inodes, WP22d consistent-cut (`open_cuts`).
- `src/core/volume.c` / `volume_internal.h` — `nbuck`/`dbuck`/`ibuck`
  mount-scan indexes (`volume_internal.h:331-337`), `vol_inode_compact` /
  WP58-D scheme-E.
- `src/cli/migrate-v2.c` — `migrate-v2` removed (`Makefile` CLI_MAINS line
  75); legacy `format_version=0` replay.
- `tools/` — v2-only format tests handled by WP-M22.

## Why

The programme treats InvariantFS as **unreleased** (design preamble): "no
migration and no v2 reader. Cutover is v3-only". §10 enumerates what must go;
keeping it leaves two metadata models, two rollback schemes and the O(N)
mount scan the programme exists to remove (§1). §11 rejects the alternatives
that would justify keeping any of it.

## Design

**Cutover rule (frozen):** there is **no `migrate-v2`** and no dual reader.
`vol_open` on a non-`VOLF_V3` volume remains only as long as the removal
window requires; after this WP mkfs writes v3 by default (WP-M1's env gate is
removed) and the v2 branch is deleted. The format is explicitly not frozen,
so no compatibility shim is owed.

**Removal method:** delete structures/functions and let the compiler and
`grep` find dangling users; no `#if 0` graveyards. `Makefile`: drop
`migrate-v2` from `CLI_MAINS` and v2-only objects; keep the build green. Reuse
`fsck_v3_walk` (WP-M4) and the v3 write path; do not port v2 behaviours.

**Risk:** the load-bearing risk is deleting a helper the **DATA plane** still
uses (e.g. shared AST/L2P code in `vol_ast.c`, `vol_read.c`). §10 lists
metadata paths only; DATA-plane code stays. Review each `grep` hit rather
than deleting by filename.

## Validation

1. `make test` — all unit binaries pass after deletion (docs build too,
   AGENTS §1.7).
2. `grep` audit: no remaining reference to `invfs_inode_rec`, `DELT`,
   `INVFS_MAX_REC_LEN`, `meta_mapper`, `vol_meta_merge`, `\x01reten`,
   `open_cuts`, `nbuck`/`dbuck`/`ibuck`, `vol_inode_compact`, `migrate-v2`.
3. mkfs (v3 default) → full write/read/fold/rollback/sweep round-trip;
   `invf-fsck` clean.
4. `make e2e` — full suite (with WP-M22's adapted tests).

## Out of scope (do NOT touch)

- DATA-zone format, codecs, containers, parity/seal.
- Any feature work; this WP only deletes.
- The v3 formats frozen by WP-M1..M20.
- Backup/export tooling built on public CLI behaviour.

## Coordination notes

- Subagent ID: `wp-M21-legacy-deletion`;
  `INVFS_E2E_AGENT=wp-M21-legacy-deletion`.
- E2E gates: `test-meta-v3.sh`; full `make e2e` after WP-M22 lands.
- Dependencies: **WP-M1…M20** (the v3 replacement must be complete first).
- Blocks: WP-M22 (v2 tests cannot be finalised until this lands).
- Do not touch `INCIDENTS.md` / `AUDIT.md` status fields without updating
  them (AGENTS §1.7); mark findings closed by deletion as closed.
