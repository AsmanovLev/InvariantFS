# docs/archive — finished work, kept for the record

These documents described work that is **done**. They are not reference
material: nobody should read them to learn how the engine works today, and they
are not maintained. The code and the ADRs are the live sources.

They are here rather than deleted because each one records *why* a thing is the
way it is — the measurements, the abandoned options, the bugs that forced the
design. That history is cheap to keep (git would keep it anyway) but expensive to
rediscover.

Moved here on 2026-11-25 as part of a documentation pass. Nothing in `src/`,
`tools/` or `packaging/` cited these files, so no code comment points here; if you
find one that does, it is a leftover to fix.

| file | what it was | status |
|---|---|---|
| `FOLD-AND-MIRROR-FINDINGS.md` | measurements behind the fold/mirror design | findings incorporated |
| `ROOTFS-SCALE.md` | rootfs-at-scale experiments (40k+ files) | findings incorporated |
| `HANDOFF-40b-entry-migration.md` | removal of the 32-byte AST wire format | done |
| `HANDOFF-small-file-latency.md` | why small files are expensive | findings incorporated |
| `evidence/` | raw measurement records (25 JSON runs) behind WP224/WP225, plus `allow-other` and disk layouts | evidence for closed findings |

## What is deliberately NOT here: work-package reports

`WP*.md` reports and `tasks/WP-M*-TASK.md` task specs are **deleted, not
archived**. AGENTS.md 1.7 makes a work package a scratchpad whose shipped
rationale belongs in the commit that shipped it: `git log --diff-filter=D
--name-only` still produces every one of them, verbatim, with the code diff
next to it. Copying them here would fork the record and give a reader two
places to check that can disagree -- which is the exact failure this directory
exists to prevent.

The line is: a *finding* (what was measured, what was learned) is kept here; a
*task* (what someone planned to do) is not.

## What is NOT here, on purpose

- `impl_docs/AUDIT.md`, `incidents` (`INCIDENTS.md`), `OPEN-ISSUES.md` — **live**:
  they are the discovery sources, and copying them here would fork the truth.
- `docs/adr/**` — ADRs are decisions that still bind; superseded ones are marked
  as such in place rather than moved.
- the symbol/type index — generated on demand by `tools/gen_impl_docs.sh`
  (`make docs`) and not committed at all: an index is a view of the tree, not a
  document, and a stale view is a wrong answer.
