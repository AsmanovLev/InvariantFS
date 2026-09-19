/* helper_exec.h — WP61: shared containment launcher for external helpers.
 *
 * One implementation serves every helper child the engine spawns: the
 * codecpack exec hooks in vol_cpack.c (encode/decode, container commands,
 * estimate) and the builtin transcode lanes (cjxl/djxl, ffmpeg, MAC,
 * packMP3). The controls are additive to the WP12d Landlock layer:
 *
 *   - privilege drop  (root daemons run helpers as INVFS_HELPER_UID/GID,
 *                      nobody by default);
 *   - network namespace (CLONE_NEWNET; a userns fallback when unprivileged);
 *   - rlimits         (CPU, AS, NOFILE, NPROC, FSIZE);
 *   - a wall-clock timeout that SIGKILLs the whole process group;
 *   - an environment scrub (minimal PATH, TMPDIR; LD_*, HOME and tokens
 *     dropped).
 *
 * Landlock stays exactly where WP12d put it: applied between fork and exec
 * when a sandbox spec is supplied. No specification (builtin lanes) means
 * no Landlock, matching the pre-WP61 behaviour.
 */
#ifndef INVFS_HELPER_EXEC_H
#define INVFS_HELPER_EXEC_H

#include <stddef.h>
#include <stdint.h>

#ifndef _WIN32
#include <sys/types.h>
#endif

/* Landlock whitelist for a pack child. NULL for builtin lanes (no Landlock).
 * Field semantics match the WP12d tool_sandbox: pack_dir/ro_path/rw_dir are
 * the pack subtree, the read-only input/recipe, and the RW scratch dir;
 * requires is the manifest's comma-separated grandchild tool list. */
typedef struct invfs_helper_sandbox {
    const char *pack_dir;
    const char *ro_path;
    const char *rw_dir;
    const char *requires;
} invfs_helper_sandbox;

/* Run argv with the WP61 containment applied.
 *
 *   mem_cap       RLIMIT_AS ceiling in bytes (0 = leave unchanged).
 *   sb            Landlock spec, or NULL for the builtin lanes.
 *   work_dir      scratch dir to hand to the dropped uid (may be NULL;
 *                 needed only when running as root).
 *   no_path_search 1 = execv(argv[0]) (strict lanes), 0 = execvp().
 *   out/out_cap   non-NULL captures stdout (NUL-terminated, truncated);
 *                 NULL mutes stdin/stdout/stderr to /dev/null.
 *   timeout_ms    0 = INVFS_HELPER_TIMEOUT_MS / the 120 s default.
 *
 * Returns the child's exit status (0..255), or -1 on fork failure, a
 * non-exit signal death, or the timeout kill. */
int invfs_helper_exec(char *const argv[], uint64_t mem_cap,
                      const invfs_helper_sandbox *sb,
                      const char *work_dir,
                      int no_path_search,
                      char *out, size_t out_cap,
                      uint64_t timeout_ms);

#endif /* INVFS_HELPER_EXEC_H */
