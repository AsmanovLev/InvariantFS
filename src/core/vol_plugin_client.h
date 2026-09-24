/* vol_plugin_client.h — Client library for dispatching containerpack and codecpack
 * operations to the invf-plugin-host worker pool daemon over SPSC shm / eventfd.
 *
 * Supports multi-threaded concurrent execution, direct in-memory buffers,
 * and automatic spill-to-disk on oversized payloads.
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

/* Connect to the plugin pool (thread-safe, initializes pool mappings) */
int invfs_plugin_pool_connect(void);

/* Disconnect from the pool */
void invfs_plugin_pool_disconnect(void);

/* Multi-threaded command dispatch through the worker pool.
 *
 * Operands match the pack CLI:
 *   ENUMERATE/STRIP  in_path=image  out_path=result
 *   EXTRACT          in_path=image  idx=member  out_path=result
 *   REBUILD          recipe_path + mbr_dir -> out_path
 *   MAP              in_path=image  out_path=result
 *
 * Returns pack status (0 ok, 3 decline, 1 error) or negative if pool unavailable.
 */
int invfs_plugin_pool_container_cmd(const char *pack_name,
                                    const char *pack_so_path,
                                    int cmd,
                                    const char *in_path,
                                    const char *idx,
                                    const char *out_path,
                                    const char *recipe_path,
                                    const char *mbr_dir);

/* Multi-threaded direct in-memory buffer execution with spill-to-disk fallback.
 * If in_len or out_cap exceeds slot capacity (64MB), automatically writes to a temporary
 * disk spill file, runs the command, and reads the result back.
 */
int invfs_plugin_pool_container_cmd_mem(const char *pack_name,
                                        const char *pack_so_path,
                                        int cmd,
                                        const char *recipe_path,
                                        const uint8_t *in_buf,
                                        size_t in_len,
                                        uint8_t *out_buf,
                                        size_t out_cap,
                                        size_t *out_len);

/* Estimate via worker pool */
int invfs_plugin_pool_container_estimate(const char *pack_name,
                                         const char *pack_so_path,
                                         const char *in_path,
                                         uint64_t *out_mbr_sz);

#ifdef __cplusplus
}
#endif

#endif /* VOL_PLUGIN_CLIENT_H */
