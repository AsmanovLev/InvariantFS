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
implementation, `invfs_helper_exec()` in `src/core/helper_exec.c`. The
WP12d Landlock whitelist is unchanged and additive to the controls below:

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
container), the launcher degrades to the other controls and Landlock; it
never falls back to an unrestricted child. The controls are covered by the
`invf-helper_exec_test` unit binary and `tools/test-helper-isolation.sh`.

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

## Other security considerations

### Landlock sandbox (pack children)

Every codecpack exec (encode/decode, container commands, estimate hook) runs
under a per-exec Landlock ruleset on Linux. The ruleset is a whitelist:

- **RO + EXEC:** the pack directory, the runtime trees (`/usr/lib`,
  `/usr/lib64`, `/lib`, `/lib64`, `/usr/bin`, `/bin`, `/usr/local/bin`),
  `$INVFS_TOOLS` when set, and the resolved `argv[0]`
- **RO file:** `/etc/ld.so.cache`, `/etc/localtime`, the input file
- **RW subtree:** the scratch directory (temp files created by helpers)
- **Everything else:** denied

If Landlock is unavailable on the build host, the process falls back to
`prctl(PR_SET_NO_NEW_PRIVS)` + `RLIMIT_AS` only.

### Crash safety

InvariantFS is not crash-safe against arbitrary power loss. Crash recovery
works for normal shutdown. See `test-flakey.sh` for power-loss soak tests.
