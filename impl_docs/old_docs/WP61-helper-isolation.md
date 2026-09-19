# WP61-helper-isolation — harden codecpack/helper child execution

**Branch:** `wp/61-helper-iso`
**Worktree:** `/tmp/invfs-wp61`
**Severity:** MEDIUM (defense-in-depth around executed helpers)
**Source:** design thread this session (helper trust question)
**Estimated effort:** 1–2 days

---

## Scope

The engine executes external helpers for codec packs (`invfs_codec_pack_exec`,
`vol_cpack.c`) and transcode lanes (`cjxl`/`djxl`, `mac`, `ffmpeg`). WP12d
already applies a **Landlock** allow-list to pack children, but Landlock only
restricts filesystem access — it does not contain network, credentials, or
resource abuse. This WP adds the cheap, high-value containment around the
existing Landlock layer.

**Threat model:** an untrusted/compromised helper (or a helper fed a crafted
file) runs with the daemon's privileges. In root-mode the daemon is PID-adjacent
to init and can be uid 0; a helper escape is a full compromise.

---

## Controls to add (in order of value)

1. **Drop privileges** for the child when the daemon runs as root: setuid to a
   dedicated uid/gid (e.g. `invfs-helper`, configurable via
   `INVFS_HELPER_UID/GID`) or `nobody` if unset. Highest value, lowest cost.
2. **Network isolation:** `CLONE_NEWNET` (or an empty net namespace in the
   fork path) so a helper cannot exfiltrate or call out. Needed because Landlock
   has no network rules.
3. **Resource caps:** `setrlimit` — `RLIMIT_CPU`, `RLIMIT_AS` (bounded by the
   pack's `dec_mem`), `RLIMIT_NOFILE`, `RLIMIT_NPROC` (no fork bombs),
   `RLIMIT_FSIZE`. Then `kill` on timeout.
4. **Timeout:** every helper exec gets a deadline; on expiry `SIGKILL` the
   process group (not just the leader).
5. **seccomp-bpf** (optional, v2): allow-list syscalls. Defer unless cheap —
   the above already removes the main vectors.
6. **Environment scrub:** clear the environment to a minimal set (keep `PATH`
   only if required by the manifest; drop `LD_*`, tokens, `HOME`); reopen
   stdio as pipes only.

Keep the existing Landlock layer; this is additive.

---

## Files

- `src/core/vol_cpack.c` — `invfs_codec_pack_exec` child setup (namespaces,
  rlimits, uid drop, env scrub, timeout kill).
- `src/core/codec.c` / transcode lanes — same helper launcher for `cjxl` etc.
  (single implementation, not two).
- `src/doc/` / `docs/SECURITY.md` — document the new controls + knobs.
- `src/cli/codec_test.c` — unit coverage for the launcher's failure modes.

---

## Validation

1. `make test` — pass.
2. A fixture pack whose helper tries to (a) open a socket, (b) fork-bomb,
   (c) read `$HOME`, (d) run forever — each contained: no network, no hang,
   killed at the deadline, no env leak. Assert via a harness pack in
   `test-helper-resolution.sh` style.
3. Existing `test-binbatch.sh` / `test-textzone.sh` / `test-jxl.sh` unchanged
   (helpers still produce bit-exact results).
4. Root daemon: the child runs as the dropped uid (verify in `/proc`).

---

## Out of scope

- Pack **trust/signatures** (decided: not required; on-volume packs are not
  auto-executed — WP60).
- A full sandbox/VM for helpers.
- Landlock changes (keep as is).

---

## Coordination notes

- Subagent ID: `wp61-helper-iso`; e2e via `INVFS_E2E_AGENT=wp61-helper-iso`.
- Independent of WP59/60; land anytime after WP58a.
- If the helper launcher is shared with the transcode lanes, touch it once.
