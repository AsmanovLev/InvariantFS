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
affected by this change. They continue to use `execvp` with `PATH` fallback
and are protected by the Landlock sandbox whitelist that restricts which
directories their children can access.

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
