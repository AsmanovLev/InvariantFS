# Security Considerations

## Tool helper resolution

### POSIX behaviour (WP33 and later)

On POSIX systems, external helper executables are resolved in the following order
for the **builtin lanes** (cjxl, djxl, ffmpeg, MAC, packMP3):

1. `$INVFS_TOOLS/<name>` — if the `INVFS_TOOLS` environment variable is set
2. `/usr/lib/invfs/tools/<name>` — the system-wide helper directory
3. **Refusal or PATH fallback** — controlled by `INVFS_REQUIRE_HELPER_PATH`

By default (`INVFS_REQUIRE_HELPER_PATH` unset):
- **Root (uid 0):** strict enforcement — if the helper is not found in
  `$INVFS_TOOLS` or `/usr/lib/invfs/tools`, the sweep or import aborts with
  an error rather than searching `PATH`
- **Non-root:** permissive — if the helper is not found in the trusted
  locations, a warning is printed and the bare name is passed to `execvp`,
  allowing `PATH` resolution as a fallback

Set `INVFS_REQUIRE_HELPER_PATH=0` to disable strict enforcement (not
recommended for root). Set `INVFS_REQUIRE_HELPER_PATH=1` to force strict
enforcement for any user.

**Threat model:** A local privileged user who can write to
`/usr/lib/invfs/tools` already owns the system — the strict resolution
raises the bar only against `PATH` manipulation, not against filesystem
write access to the trusted helper directories.

Pack children (codecpack helpers invoked via `invfs_codec_pack_exec`,
`invfs_codec_pack_cmd`, and `invfs_codec_pack_estimate`) are **not**
affected by this change. A bare manifest tool name is resolved in the
daemon (INVFS_TOOLS -> `/usr/lib/invfs/tools` -> `$PATH`) before the child
is spawned, so the strict/minimal child environment (WP61) does not change
which helper runs.

### Helper child containment (WP61)

Every helper child — codecpack helpers *and* the builtin transcode lanes
(cjxl/djxl, ffmpeg, MAC, packMP3) — is launched through one shared
implementation, `invfs_helper_exec()` in `src/core/helper_exec.c`.

**WP101 — the two classes do NOT get the same sandbox.** The controls in the
table below are common to both; the Landlock filesystem whitelist is *not*:

| | codecpack children (`sbmode == 2`) | builtin transcode lanes |
|---|---|---|
| WP61 controls (below) | yes | yes |
| **Landlock whitelist** | **yes** | **no** |

A builtin lane is launched by `tool_exec_strict()`
(`src/core/vol_cpack.c:191`), whose body passes `sb = NULL`
(`src/core/vol_cpack.c:193`); `landlock_apply()` is
called only `if (sb && sbmode == 2)` (`src/core/helper_exec.c:614`). So
packMP3/ffmpeg/MAC/cjxl run with the WP61 containment (privilege drop,
`CLONE_NEWNET`, rlimits, deadline, environment scrub) but **without any
filesystem whitelist** — they keep whatever read/write access their uid has.
What constrains *which* binary runs is path discipline, not Landlock:
`execv` (no PATH search) and, with `INVFS_REQUIRE_HELPER_PATH` active, the
requirement that the binary lives in `$INVFS_TOOLS` or `/usr/lib/invfs/tools`.

This is not a change in WP61 behaviour — `src/core/helper_exec.h:17-18` has
always said "no specification (builtin lanes) means no Landlock". It is a
correction to this document, which used to read as if the whitelist covered
every helper child. Extending Landlock to the builtin lanes is future work.

The bit-exactness guard is separate from all of this and applies to both
classes: a helper's output is decoded back and `memcmp`d against the original
before anything is committed, and a refusal is a clean decline (the file stays
RAW / the container is left whole). For codecpacks that guard runs
*after* the child has already executed (`src/core/vol_cpack.c:2277`), so it
protects the data, not the child process.

The WP12d Landlock whitelist is unchanged and additive to the controls below:

| Control | Behaviour | Knob |
|---------|-----------|------|
| **Privilege drop** | When the daemon runs as root the child `setuid`s to a dedicated uid/gid (`nobody` by default; the scratch tree is chowned first so the helper can still work) | `INVFS_HELPER_UID`, `INVFS_HELPER_GID` |
| **Network isolation** | The child creates a `CLONE_NEWNET` namespace; unprivileged launches bootstrap a mapped user namespace first. Netlink/loopback only — no route to the host or the Internet | (none) |
| **Resource caps** | `RLIMIT_CPU` (deadline + slack), `RLIMIT_AS` (the pack's `dec_mem`, as before), `RLIMIT_NOFILE`, `RLIMIT_NPROC`, `RLIMIT_FSIZE` | `INVFS_HELPER_NOFILE`, `INVFS_HELPER_NPROC`, `INVFS_HELPER_FSIZE_MB` |
| **Deadline** | A wall-clock timeout `SIGKILL`s the child's whole process group (grandchildren included) | `INVFS_HELPER_TIMEOUT_MS` (default 120000) |
| **Environment scrub** | The child environment is rebuilt: minimal `PATH` (`/usr/local/bin:/usr/bin:/bin`), sandbox `TMPDIR`, `LANG`/`LC_*`; `LD_*`, `HOME` and tokens are dropped. Named variables can be passed through explicitly | `INVFS_HELPER_PATH`, `INVFS_HELPER_KEEPENV` |

`INVFS_HELPER_KEEPENV` is a comma/space list of variables copied into the
child (never `LD_*`). It exists for legitimate tool overrides such as the
p7z pack's `P7Z_7ZZ`; leave it unset in production unless a pack needs it.

If the namespace operations are unavailable (an old kernel or a locked-down
container), the launcher degrades to the controls that are left. For a
codecpack child that still includes Landlock. For a **builtin** lane child
there is no Landlock to degrade to, so what remains is privilege drop, the
resource caps, the deadline and the environment scrub — a child that is
filesystem-unconfined by design (see the table above). Either way the helper
never runs with the daemon's own uid *and* an unscrubbed environment.
The controls are covered by the `invf-helper_exec_test` unit binary and
`tools/test-helper-isolation.sh`.

### Windows behaviour (future work — NOT YET HARDENED)

The Windows build hardcodes absolute paths to external helpers:

| Helper | Hardcoded path |
|--------|---------------|
| cjxl, djxl | `D:\VFS\tools\jxl\x64-windows-static\bin\cjxl.exe` |
| packMP3 | `D:\VFS\packMP3-v1.0g\packMP3.exe` |
| MAC | `D:\bin\MAC.exe` |

**Risk:** Any user who can write to those paths can inject arbitrary code
that runs in the context of the sweep worker.

**Mitigation (future work):** Replace hardcoded paths with a single
configurable root via the `%INVFS_TOOLS%` environment variable, and require
that directory to be owned by an administrator account. This is tracked as
a deferred fix — the Windows build is not the primary target of WP33.

### Environment variables

| Variable | Purpose | Default |
|----------|---------|---------|
| `INVFS_TOOLS` | Primary helper directory | (unset) |
| `INVFS_REQUIRE_HELPER_PATH` | Strict mode for builtin lanes | 1 for root, 0 for non-root |
| `INVFS_PACK_SANDBOX` | Landlock sandbox for pack children (0=off, unset=auto) | auto |
| `INVFS_HELPER_UID` / `INVFS_HELPER_GID` | uid/gid the helper drops to when the daemon is root | `nobody` |
| `INVFS_HELPER_TIMEOUT_MS` | Wall-clock deadline, then SIGKILL the process group | 120000 |
| `INVFS_HELPER_NPROC` | `RLIMIT_NPROC` for the helper | 256 |
| `INVFS_HELPER_NOFILE` | `RLIMIT_NOFILE` for the helper | 1024 |
| `INVFS_HELPER_FSIZE_MB` | `RLIMIT_FSIZE` (MiB per file) | 8192 |
| `INVFS_HELPER_PATH` | Overrides the child's minimal `PATH` | system path |
| `INVFS_HELPER_KEEPENV` | Comma/space list of parent vars copied into the child (never `LD_*`) | (unset) |
| `INVFS_NO_AUTOPACK` | P0-1 guard: never execute a pack-supplied binary (probe, encode/decode, container commands); files defer instead | (unset) |

## Other security considerations

### Landlock sandbox (pack children only)

Every codecpack exec (encode/decode, container commands, estimate hook) runs
under a per-exec Landlock ruleset on Linux. **Builtin transcode lanes
(cjxl/djxl, ffmpeg, MAC, packMP3) do not** — see the table in
[Helper child containment](#helper-child-containment-wp61) above. The ruleset
is a whitelist:

- **RO + EXEC:** the pack directory, the runtime trees (`/usr/lib`,
  `/usr/lib64`, `/lib`, `/lib64`, `/usr/bin`, `/bin`, `/usr/local/bin`),
  `$INVFS_TOOLS` when set, and the resolved `argv[0]`
- **RO file:** `/etc/ld.so.cache`, `/etc/localtime`, the input file
- **RW subtree:** the scratch directory (temp files created by helpers)
- **Everything else:** denied

If Landlock is unavailable on the build host, the process falls back to
`prctl(PR_SET_NO_NEW_PRIVS)` + `RLIMIT_AS` only.

### Pack trust policy (P0-1: owner decision, not a bug)

Mounting or sweeping a volume EXECUTES third-party code by design: the
manifest probe runs each candidate tool with `--invfs-manifest`
(`src/codecs/codec.c:632`, `tool_self_describes`), and admitted packs run
their encode/decode and container commands as subprocesses
(`src/core/vol_cpack.c:377` `invfs_codec_pack_exec`, `:410`
`invfs_codec_pack_cmd`). The user decides whom to trust -- per-pack
consent prompts were considered and rejected as overkill. Trust roots
are the probe search dirs (`$INVFS_CODECPACKS`, then
`/usr/lib/invfs/codecpacks`, then self-describing tools on `PATH`).

Known exception, documented rather than fixed: the probe child inherits
the FULL unsanitised environment (`HOME`, `LD_*`, everything) and none
of the `helper_child_setup` containment (`impl_docs/AUDIT.md` WP210-NEW-2).
It runs before admission, so a hostile pack is unvetted at that point.
Signatures (verify before first exec) are the planned fix; they are not
implemented yet.

Escape hatch, for mounting an untrusted volume: `-o noautopack`
(`src/cli/fuse_fs.c:3924`) or `INVFS_NO_AUTOPACK=1` (covers the offline
tools, which share the codec TU). When armed, the probe reports every
tool absent without forking, and the pack exec/cmd trampolines decline
before forking (`src/codecs/codec.c:665`, `src/core/vol_cpack.c:386`,
`:427`): pack files stay generic or defer to a later run. What it does
NOT cover: builtin lanes that exec vetted system tools through the
trusted-dirs resolution above (a hostile volume cannot inject its own
`cjxl` there), manifest *file* reads (no exec involved), and the
separately-launched `invf-plugin-host` pool (the operator starts it
deliberately). Proven by `src/cli/noautopack_test.c`: hostile fixtures
that fingerprint on execution, with guard-off controls.

### In-process plugins (WP625): a .so runs as the daemon

Out-of-process execution (subprocess exec, plugin-host IPC) contains a
hostile pack behind a process boundary: the worst it does is lie about
bytes (caught by the decode-back guard) or crash the child. In-process
loading (`dlopen`) removes that boundary on purpose, for the estimate hot
path, and a `.so` then runs with the daemon's address space, privileges,
file descriptors and malloc identity. It can corrupt volumes silently,
exfiltrate key material from memory, and turn a segfault into a dead
daemon. This WP lands ONLY with the trust model below enforced; the
orchestrator reviews the gates line-by-line before merge.

What the loader does (`src/core/vol_plugin_inproc.c`):

- **Pilot allowlist.** Only pack name `jxl` (libjxlest.so's estimate) may
  load in-process. Any other name refuses before any filesystem access.
- **Gate 1 — signature.** `pack_sig_verify_dir` must return PACK_SIG_OK
  over the pack dir at LOAD time. UNSIGNED refuses here even though the
  install gate may have prompted past it: a prompted install runs
  out-of-process, where a tampered helper is contained; in-process there
  is no containment left. (This is the one place load-time re-verify
  exists; the exec/IPC paths keep their install-gate shape.)
- **Gate 2 — local/anchored origin.** The pack dir's realpath must sit
  under `/.invariantfs/codecpacks` (the signature-gated install
  destination), `/.invfs/codecpacks`, `/usr/lib/invfs/codecpacks`, or an
  explicit `$INVFS_CODECPACKS` entry. Realpath on BOTH sides: a symlink
  pointing out of an anchored root refuses. Relative paths, absent dirs,
  tool-scratch staging (`/dev/shm`, `/tmp`, `/var/tmp` trees),
  volume-staged dirs and volume packs (`origin_vol != NULL`) refuse.
- **Gate 3 — explicit opt-in.** `-o allow_inprocess_plugins=<names>`
  (daemon) or `$INVFS_ALLOW_INPROCESS_PLUGINS` (offline tools, same
  syntax; an explicit `-o` wins). Default OFF — default mounts behave
  byte-for-byte as before. The choice is recorded in
  `user.invfs.stats` (`inprocess_allow`, `inprocess_hit`,
  `inprocess_fallback`, `inprocess_route`).
- **Any gate failing falls back to IPC/exec, never to refuse-to-codec.**
  The sweep sees a decline and runs the existing path.
- **Hostile-.so legs fail closed without killing the daemon.** The `.so`
  probe-loads in a forked child first (dlopen + three `dlsym`s +
  descriptor read + ABI-major + name check); a constructor that crashes
  shows up as a dead child, one that hangs trips the 10 s ceiling
  (SIGKILL). The daemon maps the `.so` only after a clean probe, then
  re-checks the descriptor post-load. `dlclose` on registry reset.
- **dlopen hygiene.** `RTLD_NOW|RTLD_LOCAL`, deliberately WITHOUT
  `RTLD_DEEPBIND`: the pilot is libc-only (no vendored static deps to
  collide), so DEEPBIND would only add a second libc identity to one
  address space. `LOCAL` keeps plugin exports out of the global
  namespace. The soname is FIXED (`libjxlest.so`) — never
  manifest-controlled, so a signed manifest cannot smuggle an arbitrary
  `.so` name past review (the name would be self-licensed manifest
  bytes). No `LD_LIBRARY_PATH` dependence.

Why the three gates are EACH load-bearing:

1. Without the signature, an attacker who can write the pack dir (or win
   a TOCTOU on it) gets code execution as the daemon user.
2. Without the anchored origin, a signature alone is not enough: any
   signed pack from any fetched-at-runtime URL would qualify, and the
   fetch path is network-reachable.
3. Without opt-in, every existing mount silently gains daemon-privileged
   third-party code on upgrade — the default must stay byte-identical.

Still explicitly UNTRUSTED (not fixed by this WP):

- Everything not passing all three gates (runs IPC/exec as before).
- Call-time behaviour of a loaded `.so`: the probe guards load
  (constructors, symbols, ABI), not every future call. A signed,
  anchored, opted-in pack that turns hostile is daemon-privileged —
  that is what the signature (who do you trust) and opt-in (say so
  explicitly) are for. (Mitigating detail: the pilot's estimate body
  runs in a forked child via `ivpack_run_child`, so even the
  in-process call keeps a process boundary around the parsing — but
  that fork is WITHOUT exec, inside a multithreaded daemon, so a
  sibling thread holding a libc lock at the fork instant can deadlock
  the child. The loader therefore runs the estimate on a worker thread
  with a 5 s `pthread_timedjoin_np` ceiling (`est_call_bounded`,
  `src/core/vol_plugin_inproc.c`): a wedged child becomes an IPC
  fallback, never a wedged sweep. The abandoned worker is
  self-draining and holds no loader locks.)
- Probe/exec containment gaps inherited from the pack trust policy
  above (full-environment probe child, WP210-NEW-2).
- TOCTOU between probe and load (a swapped file misses the dev+ino
  cache and re-probes next call; worst case one stale mapping — the
  same race any `dlopen` has).
- Windows plugin loading: parked, every entry declines.
- `LD_LIBRARY_PATH`/`LD_PRELOAD` in the daemon's environment affect
  `dlopen` in both the probe child and the daemon itself. The daemon's
  environment is operator-controlled (same privilege as the mount
  options), so this is documented, not gated — but do not run an
  opted-in daemon with an untrusted environment.

Operator surface: `invf-pack inproc <pack>` reports the three gates for
one installed pack (read-only). Trust matrix: `tools/test-dlopen-trust.sh`
plus the `invf-dlopen_trust_test` unit binary (signed/unsigned x
local/remote x opt-in/opt-out x good/bad-ABI/crashing/hanging .so).

### Crash safety

InvariantFS is not crash-safe against arbitrary power loss. Crash recovery
works for normal shutdown. See `test-flakey.sh` for power-loss soak tests.
