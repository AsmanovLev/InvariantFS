/* vol_plugin_inproc.h — WP625 in-process codec plugin loader.
 *
 * A `.so` runs with the daemon's address space and privileges and can
 * corrupt volumes silently, so in-process loading is gated three ways and
 * every gate is load-bearing (see docs/SECURITY.md "In-process plugins"):
 *
 *   1. pack signature verifies (pack_sig ed25519 chain, PACK_SIG_OK only;
 *      UNSIGNED and BAD both refuse — load-time re-verify exists ONLY for
 *      the in-process path, the exec/IPC paths keep their install-gate
 *      shape per pack_sig.h);
 *   2. pack origin is local/anchored (an admin install/scan root:
 *      /.invariantfs/codecpacks, /.invfs/codecpacks,
 *      /usr/lib/invfs/codecpacks, or an explicit $INVFS_CODECPACKS
 *      entry — never a fetched-at-runtime scratch dir, never a
 *      volume-staged dir);
 *   3. explicit opt-in: mount option -o allow_inprocess_plugins=<names>
 *      (default OFF — default mounts behave byte-for-byte as before).
 *
 * Pilot: pack name "jxl" only (libjxlest.so's estimate path). No other
 * codec moves in-process in this WP. Anything failing any gate falls back
 * to the IPC/exec path — never a silent refuse-to-codec.
 *
 * Hostile-.so legs (missing symbols, ABI mismatch, a constructor that
 * hangs or crashes) fail closed WITHOUT killing the daemon: the .so is
 * probe-loaded in a forked child first, and the daemon dlopens it only
 * after the probe reports a clean descriptor. A crashing probe shows up
 * as a dead child, not a dead daemon.
 *
 * dlopen hygiene: RTLD_NOW|RTLD_LOCAL, deliberately WITHOUT RTLD_DEEPBIND.
 * libjxlest is libc-only (no vendored static deps to collide with the
 * daemon's), so DEEPBIND buys nothing and would prefer the plugin's libc
 * symbols over the daemon's (a second malloc identity in one address
 * space). LOCAL keeps the plugin's exports out of the global namespace.
 * No LD_LIBRARY_PATH dependence: the .so path is resolved to a file
 * under the anchored pack dir before dlopen. dlclose on unload/reset.
 *
 * POSIX only. On _WIN32 every entry declines (Windows plugin loading is
 * parked, per policy).
 */
#ifndef VOL_PLUGIN_INPROC_H
#define VOL_PLUGIN_INPROC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Versioned C ABI. The major MUST equal IVPACK_API_VERSION: a plugin
 * whose desc->api_version major differs is refused (fallback to IPC).
 * Minor drift within a major is accepted — v1/v2 field-append history in
 * ivpack_api.h is backward compatible by construction (the host fills v2
 * fields only when desc->api_version >= 2). */
#define INVFS_CODEC_ABI_MAJOR 2
#define INVFS_CODEC_ABI_MINOR 0

/* Pilot allowlist: the only pack names this WP ever loads in-process.
 * The opt-in spec names entries from this list; anything else in the
 * spec is recorded and ignored. */
#define INVFS_INPROC_PILOT_JXL "jxl"

/* Opt-in mount flag. NULL/empty spec = OFF (the default). Comma-separated
 * pack names, e.g. "jxl". Whitespace tolerated. */
void invfs_inproc_set_allow(const char *spec);
const char *invfs_inproc_allow_spec(void);
/* 1 iff name is both opted in and a pilot pack. */
int invfs_inproc_allowed(const char *name);

/* Origin gate. 1 iff the realpath of packdir sits under an anchored
 * admin root (host roots below or an $INVFS_CODECPACKS entry). Volume
 * packs and scratch-staged trees are never anchored — callers pass
 * is_volume!=0 for those and skip straight to 0. */
int invfs_inproc_origin_anchored(const char *packdir, int is_volume);

/* Full trust gate. ALL must hold: pilot name, opt-in, anchored origin,
 * PACK_SIG_OK over packdir. 1 = may load in-process; 0 = IPC fallback.
 * reason (when non-NULL) names the first failing gate for the log. */
int invfs_inproc_eligible(const char *packname, const char *packdir,
                          int is_volume, char *reason, size_t reasoncap);

/* Probe + load. Forks a probe child that dlopens soname, resolves the
 * three ivpack exports and checks desc (major + name); only then dlopens
 * in this process and re-checks. Returns the handle, or NULL with err
 * set (caller falls back to IPC). The handle is cached per soname
 * (dev+ino identity); unload_all dlcloses everything. */
void *invfs_inproc_load(const char *packname, const char *soname,
                        char *err, size_t errcap);
void invfs_inproc_unload_all(void);

/* Pilot estimate dispatch. Runs the FULL gate (packdir + is_volume) and,
 * when it passes, the pack's estimate through the loaded .so
 * (soname = "<packdir>/libjxlest.so" for the pilot). 0 = served
 * in-process (*out_bytes set, route counter hit++); -1 = caller runs the
 * existing pool/exec path (route counter fallback++). Never fails a
 * sweep: -1 is a decline, not an error. */
int invfs_inproc_estimate(const char *packname, const char *packdir,
                          int is_volume, const char *in_path,
                          uint64_t *out_bytes);

/* Routing observability (for user.invfs.stats + the trust matrix). */
uint64_t invfs_inproc_count_hit(void);
uint64_t invfs_inproc_count_fallback(void);
const char *invfs_inproc_last_route(void); /* "inprocess" | "ipc" + reason */

#ifdef __cplusplus
}
#endif

#endif /* VOL_PLUGIN_INPROC_H */
