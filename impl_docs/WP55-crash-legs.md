# WP55 — mapper crash/durability legs (landed)

Branch: wp/55-crash-legs (commit 51b6a6f, squash-merged).

New `tools/test-mapper-crash.sh`: SIGKILL mid-import and mid-sweep, reopen
consistency, mapper duplicate/descending audit, rollback after kill, and
`invf-sweep --realize`. Tools-only; legs self-gate/SKIP on `bad records > 0`
so the suite can land before WP52.

Verified: suite 23 passed / 0 failed / 1 skipped; `test-meta-extent-walk`
6/0. It surfaced the WP56 candidate (full-sweep batch-commit failure,
duplicate pba, `--realize` orphan leak) — see INCIDENTS.md.
