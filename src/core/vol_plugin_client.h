/* vol_plugin_client.h — Client library for dispatching containerpack and codecpack
 * operations to the invf-plugin-host worker pool daemon over SPSC shm / eventfd.
 */
#ifndef VOL_PLUGIN_CLIENT_H
#define VOL_PLUGIN_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "invf_plugin_ipc.h"
#include "../include/ivpack_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Check if the plugin worker pool daemon is available */
bool invfs_plugin_pool_is_available(void);

/* Connect to the plugin pool (returns 0 on success, negative on error) */
int invfs_plugin_pool_connect(void);

/* Disconnect from the pool */
void invfs_plugin_pool_disconnect(void);

/* Run one containerpack command through the worker pool.
 *
 * The four parameters are the pack CLI's operands and mean exactly what they
 * mean on the command line -- which command consumes which is the caller's
 * business (see invfs_codec_pack_cmd):
 *   ENUMERATE/STRIP  in_path=image  out_path=result
 *   EXTRACT          in_path=image  idx=member  out_path=result
 *   REBUILD          recipe_path + mbr_dir -> out_path
 *   MAP              in_path=image  out_path=result  (the pack recomputes
 *                                       the recipe layout from the image)
 *
 * Returns the pack's exit status (0 ok, 3 decline, 1 error) -- an
 * authoritative answer from the plugin -- or a NEGATIVE code when the pool
 * itself could not carry the request, which is the caller's cue to fall back
 * to the CLI exec path.
 */
int invfs_plugin_pool_container_cmd(const char *pack_name,
                                    const char *pack_so_path,
                                    int cmd,
                                    const char *in_path,
                                    const char *idx,
                                    const char *out_path,
                                    const char *recipe_path,
                                    const char *mbr_dir);

/* Estimate via worker pool. Returns 0 on success, or -1 if unavailable */
int invfs_plugin_pool_container_estimate(const char *pack_name,
                                         const char *pack_so_path,
                                         const char *in_path,
                                         uint64_t *out_mbr_sz);

#ifdef __cplusplus
}
#endif

#endif /* VOL_PLUGIN_CLIENT_H */
