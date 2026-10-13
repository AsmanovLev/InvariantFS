# WP625 — in-process codec plugins: engine dlopen loader (.so)

## Scope

- `src/core/vol_plugin*.c/h` (new loader: dlopen/dlsym, versioned ABI,
  signature gate; IPC client path stays as fallback)
- `src/cli/pack*.c` (pack-side: trust metadata for in-process eligibility)
- `tools/test-dlopen-trust.sh` (new: trust-matrix gate)
- `docs/SECURITY.md` (new section: in-process plugin threat model)

Do not touch: existing codec lanes, IPC plugin-host path (stays default),
survive_term/`@` mark, sweep ordering, registry index format.

## Why

Orchestrator decision 2026-10-13: codecs ship as `.so` (WP615 libjxlest.so,
WP619 fat-pack payloads) but execute out-of-process via plugin-host IPC.
Per-call IPC roundtrips (socket + marshal + process hop) tax every
codec invocation; hot paths (photo lanes, batch members) pay it
repeatedly. In-process loading removes the hop for trusted packs.

This is a deliberate trust-architecture change, not a speedup patch:
a `.so` runs with the daemon's address space and privileges and can
corrupt volumes silently. The WP lands ONLY with the trust model below
enforced and reviewed.

## Design

- Versioned C ABI (`INVFS_CODEC_ABI` major check at load; mismatch =
  refuse, fall back to IPC). Pilot: `jxlest` only; no other codec moves
  in-process in this WP.
- Trust gate (ALL must hold for in-process, else IPC fallback, never
  silent refuse-to-codec):
  1. pack signature verifies (`pack_sig` ed25519 chain, existing);
  2. pack origin is local/anchored (registry pin or explicit admin
    -installed path — never a fetched-at-runtime URL);
  3. explicit opt-in: mount option `-o allow_inprocess_plugins=...`
     (default OFF — current behavior byte-for-byte until the operator
     opts in; the choice is recorded in `user.invfs.stats`).
- Loader hygiene: `dlopen(RTLD_LOCAL|RTLD_DEEPBIND?)` — decide with
  symbol-collision analysis (libjxlest vs vendored statically-linked
  deps); `dlclose` on pack unload; no `LD_LIBRARY_PATH` dependence;
  hostile-`.so` legs (missing symbols, ABI mismatch, constructor that
  hangs/crashes) must fail closed to IPC without taking the daemon down
  (load in a forked probe child first — a crashing probe must not SIGSEGV
  the daemon).
- Threat section in `docs/SECURITY.md`: what an in-process plugin can do,
  why the three gates are each load-bearing, what is still explicitly
  untrusted (everything not passing all three).

## Validation

- `tools/test-dlopen-trust.sh`: matrix {signed/unsigned} x {local/remote}
  x {opt-in/opt-out} x {good-ABI/bad-ABI/crashing-.so} → asserts
  in-process-vs-IPC routing per cell + daemon survival throughout.
- Perf leg (informational, not a gate): same photo corpus via IPC vs
  in-process, wall + p50/p99 per-file-op latency; numbers into the WP
  report (and `docs/benchmarks/` only if the delta is worth quoting).
- `make test`, registry `make check/test`, `check-repo-hygiene.sh`.
- Expected: default mounts behave identically (prove: before/after sweep
  comparison on a mixed corpus, default options); opt-in jxlest runs
  in-process with signature+origin verified.

## Out of scope

- Moving any codec besides the jxlest pilot in-process.
- Windows plugin loading (documented as parked, per policy).
- Changing the IPC path (frozen as the default + fallback).
- NBLI lane work (WP620 owns the lane; this WP only builds the loader
  it may later use).
- eBPF/security-audit tooling beyond the SECURITY.md section (WP622
  owns observability).

## Coordination notes

- Subagent ID: `wp625-dlopen-loader`. Worktree: `/tmp/invfs-wp625`, branch
  `wp/625-dlopen-loader`, from main.
- E2E gate: `tools/test-writepath.sh` regression (loader touches the read
  path) + the new trust-matrix suite; photo-lane e2e optional.
- Security-sensitive WP: the orchestrator reviews the trust gates
  line-by-line before merge (no exceptions).
- Disk discipline + commit-per-leg. Report format:
  WP / Files changed / Tests run / Result / Remaining TODOs.
