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

/* Try to execute a containerpack command via worker pool.
 * Returns 0 on success, positive error code from worker, or -1 if worker pool
 * is unavailable or does not support the request (signaling fallback to CLI exec).
 */
int invfs_plugin_pool_container_cmd(const char *pack_name,
                                    const char *pack_so_path,
                                    int cmd,
                                    const char *in_path,
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
