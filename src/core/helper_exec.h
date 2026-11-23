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

/* Run fn(user) in a child under the SAME containment, without execve.
 *
 * This exists for ABI v3 host-I/O packs (ADR-015): a pack's container
 * commands are C functions, and the callbacks the host hands them
 * (ivpack_host_io) are function pointers into this process, which cannot
 * survive an execve -- but they DO survive fork(). So a pack that reads the
 * container through those callbacks has to run in a forked child of the
 * process that owns the volume handle, and that child must keep the pack
 * containment the exec path gives (Landlock whitelist, RLIMIT_AS/CPU, netns,
 * privilege drop). InvariantFS' read path reaches its backing store through
 * blkio's already-open descriptor, so the child serves the callbacks over the
 * INHERITED fd: the sandbox is not widened by a single path.
 *
 * Differences from invfs_helper_exec, both deliberate:
 *   - inherited descriptors are NOT closed in the child (the volume fd is the
 *     read path; closing it would leave the callbacks with EBADF). Nothing is
 *     leaked: the child is short-lived.
 *   - argv_hint is the pack's own .so path; it is used only so the Landlock
 *     rules cover the image path and the pack's directory exactly as they do
 *     for an exec'd helper. It is not executed.
 *   - stdin/stdout/stderr are muted to /dev/null like the container-command
 *     path (a pack's chatter belongs to the CLI, not to the daemon's log).
 *   - the return value is the child's exit status (0..255), 0x7f00+ if the
 *     child was killed by a signal (the caller must treat that as a crash),
 *     or -1 on fork failure / timeout kill.
 *
 * The caller is responsible for the fork-safety of everything fn touches: it
 * runs single-threaded in a forked image of the parent, so any mutex another
 * parent thread holds at fork time stays locked forever. InvariantFS' rule is
 * that every core read/write entry point runs under g_io_lock, and this is
 * called from the sweep while it holds it, which is what makes that safe. */
int invfs_helper_run_child(int (*fn)(void *), void *user, uint64_t mem_cap,
                           const invfs_helper_sandbox *sb,
                           const char *work_dir, const char *argv_hint,
                           uint64_t timeout_ms);

#endif /* INVFS_HELPER_EXEC_H */
