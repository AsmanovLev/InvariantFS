# WP-e2e-parallel — parallel-safe e2e runner (mount-namespace isolation)

**Branch:** `wp/e2e-parallel`
**Worktree:** `/tmp/invfs-wp-e2e`
**Severity:** MEDIUM (developer infrastructure; unblocks parallel subagents)
**Source:** workflow observation — the single global `flock` serialised
every e2e suite, so parallel subagents (each in its own worktree) could
not test concurrently.
**Estimated effort:** small (one script + AGENTS §1.5).

---

## Scope

Rewrite `tools/run-e2e.sh` so each suite runs in a **private user+mount
namespace** with its own tmpfs on `/dev/shm`, making e2e runs parallel-safe
across worktrees, and update `AGENTS.md` §1.5 to describe the new model.

No test scripts change; no engine/data-path code changes.

---

## Why

E2E suites share `/dev/shm` image names. Since AGENTS §1.3 places every
worktree under `/tmp/invfs-wpN`, two subagents running the *same* suite
(e.g. both on `wp/m1-format-v3`) collide unless serialised — and the old
runner serialised **all** suites on one global lock, throttling unrelated
work.

`/dev/shm` is the real shared resource. `/tmp` cannot be isolated because
the worktrees themselves live under `/tmp`.

---

## Design

Two execution modes, chosen per suite by `mode_for`:

- **ISOLATED** (default): `unshare -rm --propagation private` → mount a
  fresh `tmpfs` on `/dev/shm` → `exec bash <suite>`. A crashed FUSE mount
  is torn down with the namespace. Concurrency is bounded by a slot
  semaphore (`/tmp/invfs-e2e-slots/slot<N>.lock`, default
  `INVFS_E2E_SLOTS=4`) so parallel suites cannot exhaust RAM/CPU.
- **LOCKED**: suites matching
  `sudo|losetup|id -u|EUID|mount -o loop|/tmp/` run as the invoking user
  under the legacy `/tmp/invfs-e2e.lock`, because a userns makes them see
  uid 0 and cannot service loop devices; and `/tmp` is shared.

Capability probe (`ns_usable`) falls back to LOCKED if `unshare -rm` +
tmpfs is unavailable. Knobs: `INVFS_E2E_NO_NS`, `INVFS_E2E_FORCE_NS`,
`INVFS_E2E_FORCE_LOCK`, `INVFS_E2E_SLOTS`.

Attribution: `INVFS_E2E_AGENT` + suite/pid/branch/mode are written to the
holder-info file (per slot, or the global-lock info).

### Fixed bug found during bring-up

`--bg` wrote its pidfile with `$$`, which inside `( ) &` is the
already-exited parent shell — `--wait` then treated every job as dead and
returned immediately. Switched to `$BASHPID`, plus a 1 s registration
settle in `--wait`.

---

## Validation

1. **Isolation:** a synthetic suite (`tools/e2e-parallel-selfcheck.sh`)
   writing to `/dev/shm` and reading it back, run 3× in parallel via
   `--bg`; all `ISOLATED_OK`, elapsed ~4 s vs ~11 s serial.
2. **Real suite, locked:** `bash tools/run-e2e.sh tools/test-mkstemp.sh`
   → `MKSTEMP E2E: PASS`.
3. **Real suite, isolated:** `INVFS_E2E_FORCE_NS=1 bash tools/run-e2e.sh
   tools/test-mkstemp.sh` → `MKSTEMP E2E: PASS` (FUSE mounts inside the
   namespace).
4. **Non-regression:** a suite that fails on this build
   (`test-watermark.sh`) fails identically in LOCKED and ISOLATED modes,
   confirming the runner does not change suite semantics.

---

## Out of scope (do NOT touch)

- Test scripts, `Makefile`, engine/data-path code.
- Isolating `/tmp` (breaks worktrees under `/tmp`).
- Per-suite locks for LOCKED suites beyond the existing global lock.

---

## Deliverables

- `tools/run-e2e.sh` rewritten (modes, slots, attribution, `--wait` fix).
- `AGENTS.md` §1.5 updated to the two-mode model + knobs.

---

## Coordination notes

- Subagent ID: `wp-e2e-parallel`; set `INVFS_E2E_AGENT=wp-e2e-parallel`.
- Run gates: `test-mkstemp.sh` (locked) + a self-check parallel run.
