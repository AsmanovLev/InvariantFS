/* vol_cpack_hostio.h — ABI v3 host I/O for containerpacks (ADR-015).
 *
 * A containerpack used to be invoked with a path, so the sweep had to
 * materialize the whole container into a scratch directory before the pack
 * could read a byte (src/core/vol_cpack.c, the "pin"), the pack read it three
 * times, and a member larger than RAM could not be committed at all. ABI v3
 * appends `ivpack_host_io` to ivpack_container_args: `pread`/`pwrite`
 * callbacks over the container's own bytes, served by the core's existing
 * range path (vol_read_range), gated on desc->api_version >= 3 and
 * IVPACK_F_HOST_IO.
 *
 * The callbacks are function pointers, so they cannot cross an execve: a pack
 * that wants them has to run in this process. It does NOT run in the daemon,
 * though -- `cpack_hostio_cmd` forks a child first and loads the .so THERE,
 * under the same containment the exec path gives a helper (Landlock
 * whitelist, RLIMIT_AS/CPU, netns, privilege drop), and the child serves the
 * callbacks over the volume descriptor it inherited. So the pack containment
 * model is unchanged: pack code never runs in the daemon's address space, and
 * no new path becomes reachable.
 *
 * Nothing here changes the CLI (`bin/<pack> <cmd> <in> <out>`) or the
 * out-of-process daemon: both hand a pack a path, io == NULL, and a v1/v2/v3
 * pack keeps working exactly as before. That is the gate: `io` is filled only
 * for a pack that says it can use it.
 */
#ifndef VOL_CPACK_HOSTIO_H
#define VOL_CPACK_HOSTIO_H

#include <stdint.h>
#include <stddef.h>

#include "core/volume.h"
#include "codecs/codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 iff this pack can take its container READS through ABI v3 host I/O:
 * the pack's host dir holds lib<name>.so, and its descriptor reports
 * api_version >= 3 with IVPACK_F_HOST_IO set. A directory pack that predates
 * v3, a pack with no .so (CLI-only), an .ivpack archive (its .so would have
 * to be extracted first) and a runtime without dlopen all answer 0, and the
 * caller keeps the path flow it has today. Probed in a forked child once and
 * cached by file identity. */
int cpack_hostio_pack_supported(const invfs_codec *c);

/* Open the read handle for `inode` (which spans `size` bytes) and make it the
 * handle the container commands of THIS thread are dispatched with. Returns 0
 * on success, -1 when host I/O is unavailable (INVFS_CPACK_HOSTIO=0, no
 * support, size 0) -- in which case cpack_hostio_active() stays 0 and the
 * caller must fall back to the path flow (pin). */
int cpack_hostio_begin(invfs_volume *v, uint64_t inode, uint64_t size);
void cpack_hostio_end(void);
int cpack_hostio_active(void);

/* Dispatch one container command (INVFS_PACK_CMD_*, src/codecs/codec.h) to
 * the pack's .so in a contained forked child, with `io` filled and in_path
 * NULL. `idx`/`dir`/`recipe`/`out` have the same meaning they have for
 * invfs_codec_pack_cmd. Returns the pack's own status (0 ok, 1 error,
 * 3 decline, 5 decline-solid), or -1 if the call could not be made at all
 * (fork failed, dlopen failed, the .so does not declare ABI v3 host I/O, the
 * child crashed or timed out). A caller that has already skipped its pin must
 * treat -1 as "abandon this decomposition": there is no file to retry with. */
int cpack_hostio_cmd(const invfs_codec *c, int cmd, const char *idx,
                     const char *dir, const char *recipe, const char *out);

/* Diagnostics: how many reads the handle has served and how many bytes,
 * summed over every command of the current decomposition. Printed by the
 * sweep under INVFS_DEBUG_PACKS so a gate can assert that a pin-free run
 * really read the container through the callbacks (0 reads = the pack fell
 * back to a path that should not exist). */
void cpack_hostio_stats(uint64_t *reads, uint64_t *bytes);

#ifdef __cplusplus
}
#endif

#endif /* VOL_CPACK_HOSTIO_H */
